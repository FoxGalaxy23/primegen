#include "worker.h"
#ifdef _WIN32
#include <string.h>
int worker_start(const char* script_url, WorkerConn* out) { (void)script_url; if (out) memset(out, 0, sizeof *out); return 0; }
int worker_poll(WorkerConn* c, char* type, unsigned char** data, size_t* len) { (void)c; if (type) *type = 'X'; if (data) *data = NULL; if (len) *len = 0; return 0; }
int worker_post(WorkerConn* c, const unsigned char* data, size_t len) { (void)c; (void)data; (void)len; return 0; }
void worker_terminate(WorkerConn* c) { if (c) memset(c, 0, sizeof *c); }
#else
#include "webpage.h"
#include "quickjs.h"
#include "nal.h"
#include "gal.h"
#include "security_sandbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/select.h>
#ifdef HAVE_OPENSSL_VAULT
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#endif
#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#define WORKER_HAS_SQLITE 1
#endif

/* Протокол по внутренним пайпам — как в ws.c: 1 байт type + 4 байта BE length + payload. */
#define WORKER_MAX_MESSAGE (64u * 1024u * 1024u)
static int write_all(int fd, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    while (len) { ssize_t n = write(fd, p, len); if (n < 0 && errno == EINTR) continue; if (n <= 0) return 0; p += (size_t)n; len -= (size_t)n; }
    return 1;
}
static int read_all(int fd, void* data, size_t len) {
    unsigned char* p = (unsigned char*)data;
    while (len) { ssize_t n = read(fd, p, len); if (n < 0 && errno == EINTR) continue; if (n <= 0) return 0; p += (size_t)n; len -= (size_t)n; }
    return 1;
}
static int emit(int fd, char type, const void* payload, uint32_t len) {
    if (len > WORKER_MAX_MESSAGE) return 0;
    unsigned char h[5]; h[0] = (unsigned char)type; h[1] = (unsigned char)(len >> 24); h[2] = (unsigned char)(len >> 16); h[3] = (unsigned char)(len >> 8); h[4] = (unsigned char)len;
    return write_all(fd, h, sizeof h) && (!len || (payload && write_all(fd, payload, len)));
}

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6; }

static int g_out_fd;                                                /* дочерний -> родитель, видно нативным функциям воркера */

typedef struct { JSValue* items; int count; } WorkerTransferList;
static void worker_transfer_free(JSContext* c, WorkerTransferList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) JS_FreeValue(c, list->items[i]);
    free(list->items); list->items = NULL; list->count = 0;
}
static int worker_transfer_collect(JSContext* c, JSValueConst arg, WorkerTransferList* out) {
    memset(out, 0, sizeof *out);
    if (JS_IsUndefined(arg) || JS_IsNull(arg)) return 1;
    if (!JS_IsArray(c, arg)) { JS_ThrowTypeError(c, "DataCloneError: transfer must be an array"); return 0; }
    JSValue lv = JS_GetPropertyStr(c, arg, "length"); uint32_t count = 0;
    if (JS_IsException(lv) || JS_ToUint32(c, &count, lv) < 0) { JS_FreeValue(c, lv); return 0; }
    JS_FreeValue(c, lv);
    if (count > 1024) { JS_ThrowRangeError(c, "DataCloneError: too many transferable objects"); return 0; }
    if (!count) return 1;
    out->items = calloc(count, sizeof(JSValue));
    if (!out->items) { JS_ThrowOutOfMemory(c); return 0; }
    for (uint32_t i = 0; i < count; i++) {
        JSValue item = JS_GetPropertyUint32(c, arg, i);
        if (JS_IsException(item)) { worker_transfer_free(c, out); return 0; }
        size_t byte_len = 0; uint8_t* data = JS_GetArrayBuffer(c, &byte_len, item);
        if (!data) {
            JSValue exc = JS_GetException(c); JS_FreeValue(c, exc); JS_FreeValue(c, item);
            worker_transfer_free(c, out);
            JS_ThrowTypeError(c, "DataCloneError: only attached ArrayBuffer objects are transferable"); return 0;
        }
        for (int j = 0; j < out->count; j++) {
            if (JS_StrictEq(c, out->items[j], item)) {
                JS_FreeValue(c, item); worker_transfer_free(c, out);
                JS_ThrowTypeError(c, "DataCloneError: duplicate transferable object"); return 0;
            }
        }
        out->items[out->count++] = item;
    }
    return 1;
}
static JSValue w_post(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst transfer_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    WorkerTransferList transfer;
    if (!worker_transfer_collect(c, transfer_arg, &transfer)) return JS_EXCEPTION;
    size_t len = 0; uint8_t* bytes = JS_WriteObject(c, &len, value, JS_WRITE_OBJ_REFERENCE);
    if (!bytes) { worker_transfer_free(c, &transfer); return JS_EXCEPTION; }
    if (len > WORKER_MAX_MESSAGE) {
        js_free(c, bytes); worker_transfer_free(c, &transfer);
        return JS_ThrowRangeError(c, "DataCloneError: worker message exceeds 64 MiB");
    }
    int ok = emit(g_out_fd, 'M', bytes, (uint32_t)len);
    js_free(c, bytes);
    if (!ok) { worker_transfer_free(c, &transfer); return JS_ThrowTypeError(c, "Worker message transport failed"); }
    for (int i = 0; i < transfer.count; i++) JS_DetachArrayBuffer(c, transfer.items[i]);
    worker_transfer_free(c, &transfer);
    return JS_UNDEFINED;
}
static JSValue w_log(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t; if (argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(c, argv[0]); if (s) { gal_log("INFO", "[worker] %s", s); JS_FreeCString(c, s); }
    return JS_UNDEFINED;
}
static JSValue w_error(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t; if (argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(c, argv[0]); if (s) { emit(g_out_fd, 'E', s, (uint32_t)strlen(s)); JS_FreeCString(c, s); }
    return JS_UNDEFINED;
}
static JSValue w_now(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) { (void)t; (void)argc; (void)argv; return JS_NewFloat64(c, now_ms()); }
static JSValue w_crypto_random(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t;
#ifdef HAVE_OPENSSL_VAULT
    if (argc < 3) return JS_ThrowTypeError(c, "crypto random expects an ArrayBuffer, offset and length");
    size_t total = 0; uint8_t* data = JS_GetArrayBuffer(c, &total, argv[0]);
    if (!data) return JS_EXCEPTION;
    uint64_t off = 0, len = 0;
    if (JS_ToIndex(c, &off, argv[1]) < 0 || JS_ToIndex(c, &len, argv[2]) < 0) return JS_EXCEPTION;
    if (off > total || len > (uint64_t)total - off) return JS_ThrowRangeError(c, "crypto random byte range is outside the buffer");
    if (len > 65536) return JS_ThrowRangeError(c, "getRandomValues may request at most 65536 bytes");
    if (len && RAND_bytes(data + (size_t)off, (int)len) != 1) return JS_ThrowTypeError(c, "system cryptographic random generator failed");
    return JS_UNDEFINED;
#else
    (void)argc; (void)argv; return JS_ThrowTypeError(c, "Web Crypto requires an OpenSSL-enabled build");
#endif
}
static JSValue w_crypto_digest(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t;
#ifdef HAVE_OPENSSL_VAULT
    if (argc < 4) return JS_ThrowTypeError(c, "crypto digest expects algorithm, ArrayBuffer, offset and length");
    const char* raw = JS_ToCString(c, argv[0]); if (!raw) return JS_EXCEPTION;
    char name[32]; size_t nl = strlen(raw);
    if (!nl || nl >= sizeof name) { JS_FreeCString(c, raw); return JS_ThrowRangeError(c, "unsupported digest algorithm name"); }
    for (size_t i = 0; i <= nl; i++) name[i] = (char)toupper((unsigned char)raw[i]);
    JS_FreeCString(c, raw);
    if (strcmp(name, "SHA-1") && strcmp(name, "SHA-256") && strcmp(name, "SHA-384") && strcmp(name, "SHA-512"))
        return JS_ThrowTypeError(c, "unsupported Web Crypto digest algorithm");
    const EVP_MD* md = EVP_get_digestbyname(name); if (!md) return JS_ThrowTypeError(c, "requested digest unavailable");
    size_t total = 0; uint8_t* data = JS_GetArrayBuffer(c, &total, argv[1]); if (!data) return JS_EXCEPTION;
    uint64_t off = 0, len = 0;
    if (JS_ToIndex(c, &off, argv[2]) < 0 || JS_ToIndex(c, &len, argv[3]) < 0) return JS_EXCEPTION;
    if (off > total || len > (uint64_t)total - off) return JS_ThrowRangeError(c, "digest byte range is outside the buffer");
    if (len > 64u * 1024u * 1024u) return JS_ThrowRangeError(c, "digest input exceeds the 64 MiB implementation limit");
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int digest_len = 0; static const unsigned char empty = 0;
    const void* input = len ? (const void*)(data + (size_t)off) : (const void*)&empty;
    if (EVP_Digest(input, (size_t)len, digest, &digest_len, md, NULL) != 1) return JS_ThrowTypeError(c, "digest calculation failed");
    JSValue out = JS_NewArrayBufferCopy(c, digest, digest_len); OPENSSL_cleanse(digest, sizeof digest); return out;
#else
    (void)argc; (void)argv; return JS_ThrowTypeError(c, "crypto.subtle.digest requires an OpenSSL-enabled build");
#endif
}
static JSValue w_close(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) { (void)c; (void)t; (void)argc; (void)argv; emit(g_out_fd, 'X', NULL, 0); _exit(0); }
/* Синхронный fetch внутри воркера — блокировать можно, это отдельный процесс, на интерфейс браузера это не влияет. */
static JSValue w_fetch_sync(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t; if (argc < 1) return JS_NULL;
    const char* url = JS_ToCString(c, argv[0]); if (!url) return JS_NULL;
    char* body = (webpage_mixed_blocked(url) || !nal_security_allows(url, NAL_RESOURCE_CONNECT)) ? NULL : nal_http_get_kind(url, NAL_RESOURCE_CONNECT); JS_FreeCString(c, url);
    if (!body) return JS_NULL;
    JSValue r = JS_NewString(c, body); free(body); return r;
}

/* CacheStorage is backed by the same SQLite file as profile/localStorage data.
 * Workers are separate processes, so they use their own short-lived connection. */
#ifdef WORKER_HAS_SQLITE
static sqlite3* worker_cache_db = NULL;
static sqlite3* worker_cache_open_db(void) {
    if (worker_cache_db) return worker_cache_db;
    if (sqlite3_open("browser.db", &worker_cache_db) != SQLITE_OK) { if (worker_cache_db) sqlite3_close(worker_cache_db); worker_cache_db = NULL; return NULL; }
    sqlite3_busy_timeout(worker_cache_db, 3000);
    sqlite3_exec(worker_cache_db, "CREATE TABLE IF NOT EXISTS service_worker_caches (profile_id INTEGER, origin TEXT, scope TEXT, cache_name TEXT, created_at INTEGER DEFAULT 0, UNIQUE(profile_id, origin, scope, cache_name));", NULL, NULL, NULL);
    sqlite3_exec(worker_cache_db, "CREATE TABLE IF NOT EXISTS service_worker_cache_entries (profile_id INTEGER, origin TEXT, scope TEXT, cache_name TEXT, request_key TEXT, response_json TEXT NOT NULL, UNIQUE(profile_id, origin, scope, cache_name, request_key));", NULL, NULL, NULL);
    return worker_cache_db;
}
static void worker_cache_origin(const char* scope, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!scope) return;
    const char* sep = strstr(scope, "://"); if (!sep) return;
    const char* end = strchr(sep + 3, '/'); if (!end) end = scope + strlen(scope);
    size_t n = (size_t)(end - scope); if (n >= cap) n = cap - 1; memcpy(out, scope, n); out[n] = 0;
}
static const char* worker_cache_arg(JSContext* c, int argc, JSValueConst* argv, int idx, const char** tmp) {
    if (idx >= argc) { *tmp = NULL; return ""; }
    *tmp = JS_ToCString(c, argv[idx]); return *tmp ? *tmp : "";
}
static int worker_cache_bind(sqlite3_stmt* st, int profile, const char* origin, const char* scope, const char* name) {
    sqlite3_bind_int(st, 1, profile); sqlite3_bind_text(st, 2, origin, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, scope, -1, SQLITE_TRANSIENT); sqlite3_bind_text(st, 4, name, -1, SQLITE_TRANSIENT); return 1;
}
static JSValue w_cache_open(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t; const char *s=NULL,*n=NULL,*p=NULL; const char* scope=worker_cache_arg(c,argc,argv,0,&s); const char* name=worker_cache_arg(c,argc,argv,1,&n); const char* ps=worker_cache_arg(c,argc,argv,2,&p); int profile=atoi(ps); char origin[1024]; worker_cache_origin(scope,origin,sizeof origin); sqlite3* db=worker_cache_open_db();
    if(db){sqlite3_stmt* st=NULL; if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO service_worker_caches(profile_id,origin,scope,cache_name,created_at) VALUES(?,?,?,?,strftime('%s','now'));",-1,&st,NULL)==SQLITE_OK){worker_cache_bind(st,profile,origin,scope,name);sqlite3_step(st);sqlite3_finalize(st);}}
    if(s) JS_FreeCString(c,s);
    if(n) JS_FreeCString(c,n);
    if(p) JS_FreeCString(c,p);
    return JS_NewBool(c,db!=NULL);
}
static JSValue w_cache_get(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t; const char *s=NULL,*n=NULL,*k=NULL,*p=NULL; const char* scope=worker_cache_arg(c,argc,argv,0,&s); const char* name=worker_cache_arg(c,argc,argv,1,&n); const char* key=worker_cache_arg(c,argc,argv,2,&k); const char* ps=worker_cache_arg(c,argc,argv,3,&p); int profile=atoi(ps); char origin[1024];worker_cache_origin(scope,origin,sizeof origin);sqlite3* db=worker_cache_open_db();JSValue out=JS_NULL;sqlite3_stmt* st=NULL;
    if(db&&sqlite3_prepare_v2(db,"SELECT response_json FROM service_worker_cache_entries WHERE profile_id=? AND origin=? AND scope=? AND cache_name=? AND request_key=?;",-1,&st,NULL)==SQLITE_OK){worker_cache_bind(st,profile,origin,scope,name);sqlite3_bind_text(st,5,key,-1,SQLITE_TRANSIENT);if(sqlite3_step(st)==SQLITE_ROW){const char* v=(const char*)sqlite3_column_text(st,0);if(v)out=JS_NewString(c,v);}sqlite3_finalize(st);} if(s)JS_FreeCString(c,s);if(n)JS_FreeCString(c,n);if(k)JS_FreeCString(c,k);if(p)JS_FreeCString(c,p);return out;
}
static JSValue w_cache_set(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t; const char *s=NULL,*n=NULL,*k=NULL,*v=NULL,*p=NULL;const char* scope=worker_cache_arg(c,argc,argv,0,&s);const char* name=worker_cache_arg(c,argc,argv,1,&n);const char* key=worker_cache_arg(c,argc,argv,2,&k);const char* value=worker_cache_arg(c,argc,argv,3,&v);const char* ps=worker_cache_arg(c,argc,argv,4,&p);int profile=atoi(ps);char origin[1024];worker_cache_origin(scope,origin,sizeof origin);sqlite3* db=worker_cache_open_db();int ok=0;
    if(db){sqlite3_exec(db,"BEGIN IMMEDIATE;",NULL,NULL,NULL);sqlite3_stmt* st=NULL;if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO service_worker_caches(profile_id,origin,scope,cache_name,created_at) VALUES(?,?,?,?,strftime('%s','now'));",-1,&st,NULL)==SQLITE_OK){worker_cache_bind(st,profile,origin,scope,name);sqlite3_step(st);sqlite3_finalize(st);}if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO service_worker_cache_entries(profile_id,origin,scope,cache_name,request_key,response_json) VALUES(?,?,?,?,?,?);",-1,&st,NULL)==SQLITE_OK){worker_cache_bind(st,profile,origin,scope,name);sqlite3_bind_text(st,5,key,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,6,value,-1,SQLITE_TRANSIENT);ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);}sqlite3_exec(db,ok?"COMMIT;":"ROLLBACK;",NULL,NULL,NULL);}
    if(s) JS_FreeCString(c,s);
    if(n) JS_FreeCString(c,n);
    if(k) JS_FreeCString(c,k);
    if(v) JS_FreeCString(c,v);
    if(p) JS_FreeCString(c,p);
    return JS_NewBool(c,ok);
}
static JSValue w_cache_remove(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t;const char *s=NULL,*n=NULL,*k=NULL,*p=NULL;const char* scope=worker_cache_arg(c,argc,argv,0,&s);const char* name=worker_cache_arg(c,argc,argv,1,&n);const char* key=worker_cache_arg(c,argc,argv,2,&k);const char* ps=worker_cache_arg(c,argc,argv,3,&p);int profile=atoi(ps);char origin[1024];worker_cache_origin(scope,origin,sizeof origin);sqlite3* db=worker_cache_open_db();int ok=0;sqlite3_stmt* st=NULL;if(db&&sqlite3_prepare_v2(db,"DELETE FROM service_worker_cache_entries WHERE profile_id=? AND origin=? AND scope=? AND cache_name=? AND request_key=?;",-1,&st,NULL)==SQLITE_OK){worker_cache_bind(st,profile,origin,scope,name);sqlite3_bind_text(st,5,key,-1,SQLITE_TRANSIENT);ok=sqlite3_step(st)==SQLITE_DONE&&sqlite3_changes(db)>0;sqlite3_finalize(st);}if(s)JS_FreeCString(c,s);if(n)JS_FreeCString(c,n);if(k)JS_FreeCString(c,k);if(p)JS_FreeCString(c,p);return JS_NewBool(c,ok);
}
static JSValue w_cache_delete(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t;const char *s=NULL,*n=NULL,*p=NULL;const char* scope=worker_cache_arg(c,argc,argv,0,&s);const char* name=worker_cache_arg(c,argc,argv,1,&n);const char* ps=worker_cache_arg(c,argc,argv,2,&p);int profile=atoi(ps);char origin[1024];worker_cache_origin(scope,origin,sizeof origin);sqlite3* db=worker_cache_open_db();int ok=0;sqlite3_stmt* st=NULL;if(db){sqlite3_exec(db,"BEGIN IMMEDIATE;",NULL,NULL,NULL);if(sqlite3_prepare_v2(db,"DELETE FROM service_worker_cache_entries WHERE profile_id=? AND origin=? AND scope=? AND cache_name=?;",-1,&st,NULL)==SQLITE_OK){worker_cache_bind(st,profile,origin,scope,name);sqlite3_step(st);sqlite3_finalize(st);}if(sqlite3_prepare_v2(db,"DELETE FROM service_worker_caches WHERE profile_id=? AND origin=? AND scope=? AND cache_name=?;",-1,&st,NULL)==SQLITE_OK){worker_cache_bind(st,profile,origin,scope,name);ok=sqlite3_step(st)==SQLITE_DONE&&sqlite3_changes(db)>0;sqlite3_finalize(st);}sqlite3_exec(db,"COMMIT;",NULL,NULL,NULL);}if(s)JS_FreeCString(c,s);if(n)JS_FreeCString(c,n);if(p)JS_FreeCString(c,p);return JS_NewBool(c,ok);
}
static char* worker_json_quote(const char* s) {
    size_t n = 2;
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) n += (*p == '"' || *p == 92) ? 2 : (*p < 0x20 ? 6 : 1);
    char* out = malloc(n + 1); if (!out) return NULL;
    size_t i = 0; out[i++] = '"';
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == 92) { out[i++] = 92; out[i++] = (char)*p; }
        else if (*p < 0x20) { static const char h[] = "0123456789abcdef"; out[i++] = 92; out[i++] = 'u'; out[i++] = '0'; out[i++] = '0'; out[i++] = h[*p >> 4]; out[i++] = h[*p & 15]; }
        else out[i++] = (char)*p;
    }
    out[i++] = '"'; out[i] = 0; return out;
}
static JSValue w_cache_names(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t;const char *s=NULL,*p=NULL;const char* scope=worker_cache_arg(c,argc,argv,0,&s);const char* ps=worker_cache_arg(c,argc,argv,1,&p);int profile=atoi(ps);char origin[1024];worker_cache_origin(scope,origin,sizeof origin);sqlite3* db=worker_cache_open_db();size_t cap=128,len=0;char*out=malloc(cap);if(out){out[len++]='[';out[len]=0;}sqlite3_stmt*st=NULL;if(db&&out&&sqlite3_prepare_v2(db,"SELECT cache_name FROM service_worker_caches WHERE profile_id=? AND origin=? AND scope=? ORDER BY cache_name;",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_int(st,1,profile);sqlite3_bind_text(st,2,origin,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,3,scope,-1,SQLITE_TRANSIENT);int first=1;while(sqlite3_step(st)==SQLITE_ROW){const char*n=(const char*)sqlite3_column_text(st,0);char*q=worker_json_quote(n?n:"");if(!q)continue;size_t qn=strlen(q),need=len+qn+2;if(need>cap){size_t nc=cap;while(nc<need)nc*=2;char*z=realloc(out,nc);if(!z){free(q);break;}out=z;cap=nc;}if(!first)out[len++]=',';memcpy(out+len,q,qn);len+=qn;out[len]=0;first=0;free(q);}sqlite3_finalize(st);}if(out){if(len+2>cap){char*z=realloc(out,len+2);if(z)out=z;}out[len++]=']';out[len]=0;}if(s)JS_FreeCString(c,s);if(p)JS_FreeCString(c,p);JSValue ret=JS_NewString(c,out?out:"[]");free(out);return ret;
}
static JSValue w_cache_keys(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t;const char *s=NULL,*n=NULL,*p=NULL;const char* scope=worker_cache_arg(c,argc,argv,0,&s);const char* name=worker_cache_arg(c,argc,argv,1,&n);const char* ps=worker_cache_arg(c,argc,argv,2,&p);int profile=atoi(ps);char origin[1024];worker_cache_origin(scope,origin,sizeof origin);sqlite3* db=worker_cache_open_db();size_t cap=128,len=0;char*out=malloc(cap);if(out){out[len++]='[';out[len]=0;}sqlite3_stmt*st=NULL;if(db&&out&&sqlite3_prepare_v2(db,"SELECT request_key FROM service_worker_cache_entries WHERE profile_id=? AND origin=? AND scope=? AND cache_name=? ORDER BY request_key;",-1,&st,NULL)==SQLITE_OK){worker_cache_bind(st,profile,origin,scope,name);int first=1;while(sqlite3_step(st)==SQLITE_ROW){const char*k=(const char*)sqlite3_column_text(st,0);char*q=worker_json_quote(k?k:"");if(!q)continue;size_t qn=strlen(q),need=len+qn+2;if(need>cap){size_t nc=cap;while(nc<need)nc*=2;char*z=realloc(out,nc);if(!z){free(q);break;}out=z;cap=nc;}if(!first)out[len++]=',';memcpy(out+len,q,qn);len+=qn;out[len]=0;first=0;free(q);}sqlite3_finalize(st);}if(out){if(len+2>cap){char*z=realloc(out,len+2);if(z)out=z;}out[len++]=']';out[len]=0;}if(s)JS_FreeCString(c,s);if(n)JS_FreeCString(c,n);if(p)JS_FreeCString(c,p);JSValue ret=JS_NewString(c,out?out:"[]");free(out);return ret;
}
#else
static JSValue w_cache_open(JSContext* c, JSValueConst t,int a,JSValueConst*v){(void)t;(void)a;(void)v;return JS_NewBool(c,0);}
static JSValue w_cache_get(JSContext* c, JSValueConst t,int a,JSValueConst*v){(void)t;(void)a;(void)v;return JS_NULL;}
static JSValue w_cache_set(JSContext* c, JSValueConst t,int a,JSValueConst*v){(void)t;(void)a;(void)v;return JS_NewBool(c,0);}
static JSValue w_cache_remove(JSContext* c, JSValueConst t,int a,JSValueConst*v){(void)t;(void)a;(void)v;return JS_NewBool(c,0);}
static JSValue w_cache_delete(JSContext* c, JSValueConst t,int a,JSValueConst*v){(void)t;(void)a;(void)v;return JS_NewBool(c,0);}
static JSValue w_cache_names(JSContext* c, JSValueConst t,int a,JSValueConst*v){(void)t;(void)a;(void)v;return JS_NewString(c,"[]");}
static JSValue w_cache_keys(JSContext* c, JSValueConst t,int a,JSValueConst*v){(void)t;(void)a;(void)v;return JS_NewString(c,"[]");}
#endif

static const char* WORKER_BOOTSTRAP =
"var self = globalThis; self.self = self; self.__listeners = Object.create(null);\n"
"self.addEventListener = function(t,f){ (self.__listeners[t] = self.__listeners[t] || []).push(f); };\n"
"self.removeEventListener = function(t,f){ var a = self.__listeners[t]; if (!a) return; var i = a.indexOf(f); if (i >= 0) a.splice(i, 1); };\n"
"self.__dispatch = function(t,ev){ if (typeof self['on'+t] === 'function') { try { self['on'+t](ev); } catch (e) { __nativeError(String(e && e.stack || e)); } }\n"
"  var a = self.__listeners[t]; if (a) for (var i = 0; i < a.length; i++) { try { a[i](ev); } catch (e) { __nativeError(String(e && e.stack || e)); } } };\n"
"self.postMessage = function(d,o){var tr=Array.isArray(o)?o:(o&&o.transfer)||[];__nativePost(d,tr);};\n"
"self.close = function(){ __nativeClose(); };\n"
"self.importScripts = function(){ for(var i=0;i<arguments.length;i++){var u=__swResolve(arguments[i]);var src=__nativeFetchSync(u);if(src===null)throw new Error('Failed to import script: '+u);(0,eval)(src);} };\n"
"self.location = { href: __workerScriptURL, toString: function(){ return this.href; } }; self.location.origin = (function(){ var m = /^([a-z]+:\\/\\/[^/]+)/i.exec(self.location.href); return m ? m[1] : 'null'; })();\n"
"self.isSecureContext = /^https:\\/\\//i.test(self.location.href) || /^file:/i.test(self.location.href) || /^http:\\/\\/(localhost|127(?:\\.\\d+){3})(:\\d+)?(?:\\/|$)/i.test(self.location.href);\n"
"self.crypto = { getRandomValues: function(a){if(!ArrayBuffer.isView(a)||a instanceof DataView||a instanceof Float32Array||a instanceof Float64Array)throw new TypeError('getRandomValues requires an integer TypedArray');if(a.byteLength>65536)throw new RangeError('getRandomValues exceeds 65536 bytes');__nativeCryptoRandom(a.buffer,a.byteOffset,a.byteLength);return a;}, randomUUID: function(){var b=new Uint8Array(16);__nativeCryptoRandom(b.buffer,0,16);b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;var h='';for(var i=0;i<16;i++){if(i===4||i===6||i===8||i===10)h+='-';h+=('0'+b[i].toString(16)).slice(-2);}return h;}, subtle: { digest: function(algorithm,data){return Promise.resolve().then(function(){if(!self.isSecureContext)throw new TypeError('crypto.subtle requires a secure context');var name=typeof algorithm==='string'?algorithm:algorithm&&algorithm.name;var buffer,offset=0,length;if(data instanceof ArrayBuffer){buffer=data;length=data.byteLength;}else if(ArrayBuffer.isView(data)){buffer=data.buffer;offset=data.byteOffset;length=data.byteLength;}else throw new TypeError('digest data must be an ArrayBuffer or view');if(!name)throw new TypeError('digest algorithm name is required');return __nativeCryptoDigest(String(name),buffer,offset,length);});} } };\n"
"function __swResolve(u){u=String(u);if(/^[a-z][a-z0-9+.-]*:/i.test(u))return u;if(u.slice(0,2)==='//')return self.location.href.slice(0,self.location.href.indexOf(':')+1)+u;var path;if(u.charAt(0)==='/')path=u;else{var base=self.location.href.split(/[?#]/)[0];base=base.slice(0,base.lastIndexOf('/')+1);path=base.slice(self.location.origin.length)+u;}var suffix='',q=path.search(/[?#]/);if(q>=0){suffix=path.slice(q);path=path.slice(0,q);}var parts=path.split('/'),out=[];for(var j=0;j<parts.length;j++){if(!parts[j]||parts[j]==='.')continue;if(parts[j]==='..')out.pop();else out.push(parts[j]);}return self.location.origin+'/'+out.join('/')+suffix;}\n"
"self.Headers = function(init){ this.__h = Object.create(null); if (init && typeof init === 'object') { if(init instanceof self.Headers)init=init.__h; var ks = Object.keys(init); for (var i=0;i<ks.length;i++) this.set(ks[i],init[ks[i]]); } }; self.Headers.prototype.set=function(k,v){this.__h[String(k).toLowerCase()]=String(v);}; self.Headers.prototype.get=function(k){var v=this.__h[String(k).toLowerCase()];return v===undefined?null:v;}; self.Headers.prototype.has=function(k){return this.get(k)!==null;}; self.Headers.prototype.delete=function(k){delete this.__h[String(k).toLowerCase()];}; self.Headers.prototype.entries=function(){return Object.keys(this.__h).map(function(k){return [k,this.__h[k]];},this);}; self.Headers.prototype.forEach=function(cb,thisArg){var h=this;Object.keys(this.__h).forEach(function(k){cb.call(thisArg,h.__h[k],k,h);});};\n"
"self.Response = function(body,init){init=init||{};this.__body=body==null?'':String(body);this.status=init.status===undefined?200:+init.status;this.statusText=init.statusText||'OK';this.ok=this.status>=200&&this.status<300;this.url=init.url||'';this.headers=init.headers instanceof self.Headers?init.headers:new self.Headers(init.headers);this.bodyUsed=false;}; self.Response.prototype.text=function(){if(this.bodyUsed)return Promise.reject(new TypeError('Body is unusable'));this.bodyUsed=true;return Promise.resolve(this.__body);}; self.Response.prototype.json=function(){return this.text().then(JSON.parse);}; self.Response.prototype.clone=function(){if(this.bodyUsed)throw new TypeError('Body is unusable');return new self.Response(this.__body,{status:this.status,statusText:this.statusText,url:this.url,headers:this.headers.__h});}; self.Response.error=function(){return new self.Response('',{status:0,statusText:'error'});}; self.Response.redirect=function(u,s){return new self.Response('',{status:s||302,headers:{location:String(u)}});};\n"
"self.Request = function(input,init){init=init||{};this.url=__swResolve(typeof input==='string'?input:(input&&input.url)||String(input));this.method=String(init.method||(input&&input.method)||'GET').toUpperCase();this.headers=new self.Headers(init.headers||(input&&input.headers)||{});this.mode=init.mode||(input&&input.mode)||'cors';this.credentials=init.credentials||(input&&input.credentials)||'same-origin';};\n"
"self.fetch = function(input,init){ return new Promise(function(res, rej){ var req = input instanceof self.Request ? input : new self.Request(input,init); var t = __nativeFetchSync(req.url); if (t === null) rej(new TypeError('Failed to fetch')); else res(new self.Response(t,{url:req.url,status:200,headers:{}})); }); };\n"
"function __swKey(r){return __swResolve(typeof r==='string'?r:(r&&r.url)||String(r));} function __swResponse(o){return new self.Response(o.body,{status:o.status||200,statusText:o.statusText||'OK',url:o.url||'',headers:o.headers||{}});} function __swScope(){return self.registration.scope;} function __swProfile(){return String(self.__mlProfileId||0);} function __swParse(s,fallback){try{return JSON.parse(s);}catch(e){return fallback;}}\n"
"function __swCache(name){name=String(name);var scope=__swScope(),profile=__swProfile();return {put:function(req,res){var k=__swKey(req);return Promise.resolve(res&&res.clone?res.clone():res).then(function(r){if(!r||typeof r.text!=='function')throw new TypeError('Cache.put requires a Response');return r.text().then(function(body){var hs={};if(r.headers&&r.headers.entries)r.headers.entries().forEach(function(p){hs[p[0]]=p[1];});var entry={body:body,status:r.status||200,statusText:r.statusText||'OK',headers:hs,url:r.url||k};if(!__nativeCacheSet(scope,name,k,JSON.stringify(entry),profile))throw new Error('CacheStorage write failed');});});},match:function(req){var x=__nativeCacheGet(scope,name,__swKey(req),profile);if(x===null)return Promise.resolve(undefined);var o=__swParse(x,null);return Promise.resolve(o?__swResponse(o):undefined);},delete:function(req){return Promise.resolve(!!__nativeCacheRemove(scope,name,__swKey(req),profile));},keys:function(req){var list=__swParse(__nativeCacheKeys(scope,name,profile),[]);if(req===undefined)return Promise.resolve(list.map(function(k){return new self.Request(k);}));var key=__swKey(req);return Promise.resolve(list.indexOf(key)>=0?[new self.Request(key)]:[]);},add:function(req){var k=__swKey(req);return self.fetch(k).then(function(r){if(!r.ok)throw new TypeError('Request failed');return this.put(k,r);}.bind(this));},addAll:function(reqs){var cache=this;return Promise.all(Array.prototype.map.call(reqs,function(r){return cache.add(r);})).then(function(){return undefined;});}};}\n"
"self.caches = {open:function(n){var name=String(n);if(!__nativeCacheOpen(self.registration.scope,name,__swProfile()))return Promise.reject(new Error('CacheStorage unavailable'));return Promise.resolve(__swCache(name));},has:function(n){return Promise.resolve(__swParse(__nativeCacheNames(self.registration.scope,__swProfile()),[]).indexOf(String(n))>=0);},delete:function(n){return Promise.resolve(!!__nativeCacheDelete(self.registration.scope,String(n),__swProfile()));},keys:function(){return Promise.resolve(__swParse(__nativeCacheNames(self.registration.scope,__swProfile()),[]));},match:function(req){var ks=__swParse(__nativeCacheNames(self.registration.scope,__swProfile()),[]);return ks.reduce(function(p,k){return p.then(function(v){return v!==undefined?v:__swCache(k).match(req);});},Promise.resolve(undefined));}}; self.skipWaiting=function(){return Promise.resolve();}; self.clients={claim:function(){return Promise.resolve();},matchAll:function(){return Promise.resolve([]);},get:function(){return Promise.resolve(undefined);},openWindow:function(){return Promise.resolve(null);}}; self.registration={scope:self.location.origin+'/',active:self,waiting:null,installing:null,update:function(){return Promise.resolve(self.registration);},unregister:function(){return Promise.resolve(false);}};\n"
"self.__dispatchSW = function(d){var type=String(d.__ml_sw_event||''),waits=[],hasResponse=false,responsePromise=null;var ev={type:type,waitUntil:function(p){waits.push(Promise.resolve(p));},respondWith:function(p){if(hasResponse)throw new TypeError('respondWith called more than once');hasResponse=true;responsePromise=Promise.resolve(p);}};if(type==='fetch'){ev.request=new self.Request(d.request||d.url||'',d.request||{});ev.clientId=d.clientId||'';ev.resultingClientId=d.resultingClientId||'';ev.preloadResponse=Promise.resolve(undefined);}if(d.__ml_sw_scope)self.registration.scope=String(d.__ml_sw_scope);if(d.__ml_sw_profile!==undefined)self.__mlProfileId=d.__ml_sw_profile;self.__dispatch(type,ev);if(type==='fetch'){if(!hasResponse){__nativePost({__ml_sw_reply:d.id,fallback:true});return;}responsePromise.then(function(r){if(typeof r==='string')r=new self.Response(r);if(!r||typeof r.text!=='function')throw new TypeError('respondWith must receive a Response or promise');return r.clone?r.clone().text().then(function(body){var hs={};if(r.headers&&r.headers.entries)r.headers.entries().forEach(function(p){hs[p[0]]=p[1];});return {body:body,status:r.status||200,statusText:r.statusText||'OK',headers:hs,url:r.url||''};}):r.text().then(function(body){return {body:body,status:r.status||200,statusText:r.statusText||'OK',headers:{},url:r.url||''};});}).then(function(r){__nativePost({__ml_sw_reply:d.id,fallback:false,response:r});},function(e){__nativePost({__ml_sw_reply:d.id,fallback:false,error:String(e&&e.message||e)});});}else{Promise.all(waits).then(function(){__nativePost({__ml_sw_lifecycle:type,ok:true});},function(e){__nativePost({__ml_sw_lifecycle:type,ok:false,error:String(e&&e.message||e)});});}};\n"
"self.console = { log: function(){ __nativeLog(Array.prototype.slice.call(arguments).map(String).join(' ')); }, warn: function(){ self.console.log.apply(null, arguments); }, error: function(){ self.console.log.apply(null, arguments); }, info: function(){ self.console.log.apply(null, arguments); } };\n"
"var __timers = [], __nextTimerId = 1;\n"
"self.setTimeout = function(f, ms){ var id = __nextTimerId++; __timers.push({ id: id, fn: f, due: __nativeNow() + (ms || 0), interval: 0 }); return id; };\n"
"self.setInterval = function(f, ms){ var id = __nextTimerId++; __timers.push({ id: id, fn: f, due: __nativeNow() + (ms || 0), interval: ms || 0 }); return id; };\n"
"self.clearTimeout = self.clearInterval = function(id){ __timers = __timers.filter(function(t){ return t.id !== id; }); };\n"
"function __nextDue(){ var m = null; for (var i = 0; i < __timers.length; i++) if (m === null || __timers[i].due < m) m = __timers[i].due; return m; }\n"
"function __tick(){ var now = __nativeNow(); var due = []; var keep = [];\n"
"  for (var i = 0; i < __timers.length; i++) { var t = __timers[i]; if (t.due <= now) { due.push(t); if (t.interval) { t.due = now + t.interval; keep.push(t); } } else keep.push(t); }\n"
"  __timers = keep; for (var j = 0; j < due.length; j++) { try { due[j].fn(); } catch (e) { __nativeError(String(e && e.stack || e)); } } }\n";

/* Возвращает время (мс) до следующего таймера, или -1 если таймеров нет (аргумент передаётся в select()). */
static double worker_next_due(JSContext* ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, g, "__nextDue");
    JSValue r = JS_Call(ctx, fn, g, 0, NULL);
    double d = -1; if (JS_IsNumber(r)) { double v; JS_ToFloat64(ctx, &v, r); d = v; }
    JS_FreeValue(ctx, r); JS_FreeValue(ctx, fn); JS_FreeValue(ctx, g);
    return d;
}
static void worker_run_tick(JSContext* ctx) {
    JSValue g = JS_GetGlobalObject(ctx); JSValue fn = JS_GetPropertyStr(ctx, g, "__tick");
    JSValue r = JS_Call(ctx, fn, g, 0, NULL);
    if (JS_IsException(r)) { JSValue exc = JS_GetException(ctx); const char* s = JS_ToCString(ctx, exc); if (s) { emit(g_out_fd, 'E', s, (uint32_t)strlen(s)); JS_FreeCString(ctx, s); } JS_FreeValue(ctx, exc); }
    JS_FreeValue(ctx, r); JS_FreeValue(ctx, fn); JS_FreeValue(ctx, g);
}
static void worker_dispatch_message(JSContext* ctx, const uint8_t* bytes, size_t len) {
    JSValue val = JS_ReadObject(ctx, bytes, len, JS_READ_OBJ_REFERENCE);
    if (JS_IsException(val)) { JSValue exc = JS_GetException(ctx); const char* s = JS_ToCString(ctx, exc); if (s) { emit(g_out_fd, 'E', s, (uint32_t)strlen(s)); JS_FreeCString(ctx, s); } JS_FreeValue(ctx, exc); return; }
    JSValue sw_event = JS_IsObject(val) ? JS_GetPropertyStr(ctx, val, "__ml_sw_event") : JS_UNDEFINED;
    JSValue g = JS_GetGlobalObject(ctx); JSValue fn; JSValue r;
    if (!JS_IsUndefined(sw_event) && !JS_IsNull(sw_event)) {
        fn = JS_GetPropertyStr(ctx, g, "__dispatchSW"); JSValue args[1] = { JS_DupValue(ctx, val) };
        r = JS_Call(ctx, fn, g, 1, args); JS_FreeValue(ctx, args[0]);
    } else {
        JSValue self = JS_GetPropertyStr(ctx, g, "self");
        JSValue ev = JS_NewObject(ctx); JS_SetPropertyStr(ctx, ev, "data", JS_DupValue(ctx, val));
        fn = JS_GetPropertyStr(ctx, self, "__dispatch"); JSValue args[2] = { JS_NewString(ctx, "message"), ev };
        r = JS_Call(ctx, fn, self, 2, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, ev); JS_FreeValue(ctx, self);
    }
    if (JS_IsException(r)) { JSValue exc = JS_GetException(ctx); const char* s = JS_ToCString(ctx, exc); if (s) { emit(g_out_fd, 'E', s, (uint32_t)strlen(s)); JS_FreeCString(ctx, s); } JS_FreeValue(ctx, exc); }
    JS_FreeValue(ctx, r); JS_FreeValue(ctx, fn); JS_FreeValue(ctx, sw_event); JS_FreeValue(ctx, val); JS_FreeValue(ctx, g);
}
static void child_main(const char* script_url, int out_fd, int in_fd) {
    int sandbox = mlb_sandbox_apply();
    if ((sandbox & MLB_SANDBOX_SECCOMP) == 0 || (sandbox & MLB_SANDBOX_ERROR)) {
        const char msg[] = "Linux worker sandbox could not be established";
        emit(out_fd, 'E', msg, (uint32_t)(sizeof msg - 1)); _exit(126);
    }
    g_out_fd = out_fd;
    char* src = (webpage_mixed_blocked(script_url) || !nal_security_allows(script_url, NAL_RESOURCE_WORKER)) ? NULL : nal_http_get_kind(script_url, NAL_RESOURCE_WORKER);
    if (!src) { emit(out_fd, 'E', "worker: failed to load script", 30); _exit(0); }

    JSRuntime* rt = JS_NewRuntime(); JSContext* ctx = JS_NewContext(rt);
    JSValue g = JS_GetGlobalObject(ctx);
#define WFN(name, fn, n) JS_SetPropertyStr(ctx, g, name, JS_NewCFunction(ctx, fn, name, n))
    WFN("__nativePost", w_post, 1); WFN("__nativeLog", w_log, 1); WFN("__nativeError", w_error, 1); WFN("__nativeNow", w_now, 0); WFN("__nativeCryptoRandom", w_crypto_random, 3); WFN("__nativeCryptoDigest", w_crypto_digest, 4); WFN("__nativeClose", w_close, 0); WFN("__nativeFetchSync", w_fetch_sync, 1); WFN("__nativeCacheOpen", w_cache_open, 3); WFN("__nativeCacheGet", w_cache_get, 4); WFN("__nativeCacheSet", w_cache_set, 5); WFN("__nativeCacheRemove", w_cache_remove, 4); WFN("__nativeCacheDelete", w_cache_delete, 3); WFN("__nativeCacheNames", w_cache_names, 2); WFN("__nativeCacheKeys", w_cache_keys, 3);
    JS_SetPropertyStr(ctx, g, "__workerScriptURL", JS_NewString(ctx, script_url));
#undef WFN
    JS_FreeValue(ctx, g);

    JSValue boot = JS_Eval(ctx, WORKER_BOOTSTRAP, strlen(WORKER_BOOTSTRAP), "<worker-bootstrap>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(boot)) { JSValue exc = JS_GetException(ctx); const char* s = JS_ToCString(ctx, exc); emit(out_fd, 'E', s ? s : "bootstrap failed", s ? (uint32_t)strlen(s) : 17); _exit(0); }
    JS_FreeValue(ctx, boot);

    JSValue res = JS_Eval(ctx, src, strlen(src), script_url, JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(res)) { JSValue exc = JS_GetException(ctx); const char* s = JS_ToCString(ctx, exc); if (s) { emit(out_fd, 'E', s, (uint32_t)strlen(s)); JS_FreeCString(ctx, s); } JS_FreeValue(ctx, exc); JS_FreeValue(ctx, res); _exit(0); }
    JS_FreeValue(ctx, res);

    for (;;) {
        double due = worker_next_due(ctx);
        struct timeval tv; struct timeval* tvp = NULL;
        if (due >= 0) { double ms = due - now_ms(); if (ms < 0) ms = 0; tv.tv_sec = (long)(ms / 1000); tv.tv_usec = (long)(((long)ms % 1000) * 1000); tvp = &tv; }
        else { tv.tv_sec = 1; tv.tv_usec = 0; tvp = &tv; }
        fd_set rf; FD_ZERO(&rf); FD_SET(in_fd, &rf);
        int sel = select(in_fd + 1, &rf, NULL, NULL, tvp);
        if (sel < 0 && errno != EINTR) break;
        if (sel > 0 && FD_ISSET(in_fd, &rf)) {
            unsigned char h[5];
            if (!read_all(in_fd, h, sizeof h)) break;
            uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
            if (len > WORKER_MAX_MESSAGE) { const char msg[] = "Worker message exceeds 64 MiB"; emit(out_fd, 'E', msg, (uint32_t)(sizeof msg - 1)); break; }
            unsigned char* buf = len ? malloc(len) : NULL;
            if (len && (!buf || !read_all(in_fd, buf, len))) { free(buf); break; }
            if (h[0] == 'M' && buf) worker_dispatch_message(ctx, buf, len);
            else if (h[0] == 'X') { free(buf); break; }
            free(buf);
        }
        worker_run_tick(ctx);
        JSContext* c1; while (JS_ExecutePendingJob(rt, &c1) > 0) {}   /* микрозадачи (Promise) */
    }
    JS_FreeContext(ctx); JS_FreeRuntime(rt); _exit(0);
}

int worker_start(const char* script_url, WorkerConn* out) {
    memset(out, 0, sizeof *out);
    int p2c[2], c2p[2];
    if (pipe(p2c) < 0) return 0;
    if (pipe(c2p) < 0) { close(p2c[0]); close(p2c[1]); return 0; }
    pid_t pid = fork();
    if (pid < 0) { close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]); return 0; }
    if (pid == 0) { close(p2c[1]); close(c2p[0]); child_main(script_url, c2p[1], p2c[0]); _exit(0); }
    close(p2c[0]); close(c2p[1]);
    fcntl(c2p[0], F_SETFL, fcntl(c2p[0], F_GETFL, 0) | O_NONBLOCK);
    out->used = 1; out->pid = pid; out->rfd = c2p[0]; out->wfd = p2c[1];
    return 1;
}

int worker_poll(WorkerConn* c, char* type, unsigned char** data, size_t* len) {
    if (data) *data = NULL; if (len) *len = 0;
    if (!c->used) return 0;
    unsigned char h[5]; ssize_t r = read(c->rfd, h, sizeof h);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    if (r <= 0) return 0;
    /* A <= PIPE_BUF header is written atomically by emit(). */
    if ((size_t)r != sizeof h) { if (!read_all(c->rfd, h + r, sizeof h - (size_t)r)) return 0; }
    uint32_t n = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
    if (n > WORKER_MAX_MESSAGE) { *type = 'E'; const char msg[] = "Worker message exceeds 64 MiB"; unsigned char* out = malloc(sizeof msg); if (out) memcpy(out, msg, sizeof msg); if (data) *data = out; if (len) *len = sizeof msg - 1; return 1; }
    unsigned char* buf = n ? malloc((size_t)n + (h[0] == 'E' ? 1u : 0u)) : NULL;
    if (n && !buf) return 0;
    /* Once an atomic header is consumed, read the whole frame. The child writes the payload
       with a blocking write loop, so temporarily blocking here avoids losing partial payloads. */
    int old_flags = fcntl(c->rfd, F_GETFL, 0);
    if (old_flags >= 0) fcntl(c->rfd, F_SETFL, old_flags & ~O_NONBLOCK);
    int read_ok = !n || read_all(c->rfd, buf, n);
    if (old_flags >= 0) fcntl(c->rfd, F_SETFL, old_flags);
    if (!read_ok) { free(buf); return 0; }
    if (h[0] == 'E' && buf) buf[n] = 0;
    *type = (char)h[0]; if (data) *data = buf; else free(buf); if (len) *len = n;
    if (*type == 'X') { close(c->rfd); close(c->wfd); int st; waitpid(c->pid, &st, WNOHANG); c->used = 0; }
    return 1;
}
int worker_post(WorkerConn* c, const unsigned char* data, size_t len) {
    if (!c->used || len > WORKER_MAX_MESSAGE || (len && !data)) return 0;
    return emit(c->wfd, 'M', data, (uint32_t)len);
}
void worker_terminate(WorkerConn* c) {
    if (!c->used) return;
    kill(c->pid, SIGKILL); int st; waitpid(c->pid, &st, WNOHANG);
    close(c->rfd); close(c->wfd); c->used = 0;
}

#endif /* _WIN32 */
