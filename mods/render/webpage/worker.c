#include "worker.h"
#include "webpage.h"
#include "quickjs.h"
#include "nal.h"
#include "gal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/select.h>
#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#define WORKER_HAS_SQLITE 1
#endif

/* Протокол по внутренним пайпам — как в ws.c: 1 байт type + 4 байта BE length + payload. */
static void emit(int fd, char type, const void* payload, uint32_t len) {
    unsigned char h[5]; h[0] = (unsigned char)type; h[1] = (unsigned char)(len >> 24); h[2] = (unsigned char)(len >> 16); h[3] = (unsigned char)(len >> 8); h[4] = (unsigned char)len;
    write(fd, h, 5); if (len && payload) write(fd, payload, len);
}

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6; }

static int g_out_fd;                                                /* дочерний -> родитель, видно нативным функциям воркера */

static JSValue w_post(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t; if (argc < 1) return JS_UNDEFINED;
    const char* s = JS_ToCString(c, argv[0]); if (s) { emit(g_out_fd, 'M', s, (uint32_t)strlen(s)); JS_FreeCString(c, s); }
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
static JSValue w_close(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) { (void)c; (void)t; (void)argc; (void)argv; emit(g_out_fd, 'X', NULL, 0); _exit(0); }
/* Синхронный fetch внутри воркера — блокировать можно, это отдельный процесс, на интерфейс браузера это не влияет. */
static JSValue w_fetch_sync(JSContext* c, JSValueConst t, int argc, JSValueConst* argv) {
    (void)t; if (argc < 1) return JS_NULL;
    const char* url = JS_ToCString(c, argv[0]); if (!url) return JS_NULL;
    char* body = webpage_mixed_blocked(url) ? NULL : nal_http_get(url); JS_FreeCString(c, url);
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
"self.postMessage = function(d){ __nativePost(JSON.stringify(d === undefined ? null : d)); };\n"
"self.close = function(){ __nativeClose(); };\n"
"self.importScripts = function(){ for(var i=0;i<arguments.length;i++){var u=__swResolve(arguments[i]);var src=__nativeFetchSync(u);if(src===null)throw new Error('Failed to import script: '+u);(0,eval)(src);} };\n"
"self.location = { href: __workerScriptURL, toString: function(){ return this.href; } }; self.location.origin = (function(){ var m = /^([a-z]+:\\/\\/[^/]+)/i.exec(self.location.href); return m ? m[1] : 'null'; })();\n"
"function __swResolve(u){u=String(u);if(/^[a-z][a-z0-9+.-]*:/i.test(u))return u;if(u.slice(0,2)==='//')return self.location.href.slice(0,self.location.href.indexOf(':')+1)+u;var path;if(u.charAt(0)==='/')path=u;else{var base=self.location.href.split(/[?#]/)[0];base=base.slice(0,base.lastIndexOf('/')+1);path=base.slice(self.location.origin.length)+u;}var suffix='',q=path.search(/[?#]/);if(q>=0){suffix=path.slice(q);path=path.slice(0,q);}var parts=path.split('/'),out=[];for(var j=0;j<parts.length;j++){if(!parts[j]||parts[j]==='.')continue;if(parts[j]==='..')out.pop();else out.push(parts[j]);}return self.location.origin+'/'+out.join('/')+suffix;}\n"
"self.Headers = function(init){ this.__h = Object.create(null); if (init && typeof init === 'object') { if(init instanceof self.Headers)init=init.__h; var ks = Object.keys(init); for (var i=0;i<ks.length;i++) this.set(ks[i],init[ks[i]]); } }; self.Headers.prototype.set=function(k,v){this.__h[String(k).toLowerCase()]=String(v);}; self.Headers.prototype.get=function(k){var v=this.__h[String(k).toLowerCase()];return v===undefined?null:v;}; self.Headers.prototype.has=function(k){return this.get(k)!==null;}; self.Headers.prototype.delete=function(k){delete this.__h[String(k).toLowerCase()];}; self.Headers.prototype.entries=function(){return Object.keys(this.__h).map(function(k){return [k,this.__h[k]];},this);}; self.Headers.prototype.forEach=function(cb,thisArg){var h=this;Object.keys(this.__h).forEach(function(k){cb.call(thisArg,h.__h[k],k,h);});};\n"
"self.Response = function(body,init){init=init||{};this.__body=body==null?'':String(body);this.status=init.status===undefined?200:+init.status;this.statusText=init.statusText||'OK';this.ok=this.status>=200&&this.status<300;this.url=init.url||'';this.headers=init.headers instanceof self.Headers?init.headers:new self.Headers(init.headers);this.bodyUsed=false;}; self.Response.prototype.text=function(){if(this.bodyUsed)return Promise.reject(new TypeError('Body is unusable'));this.bodyUsed=true;return Promise.resolve(this.__body);}; self.Response.prototype.json=function(){return this.text().then(JSON.parse);}; self.Response.prototype.clone=function(){if(this.bodyUsed)throw new TypeError('Body is unusable');return new self.Response(this.__body,{status:this.status,statusText:this.statusText,url:this.url,headers:this.headers.__h});}; self.Response.error=function(){return new self.Response('',{status:0,statusText:'error'});}; self.Response.redirect=function(u,s){return new self.Response('',{status:s||302,headers:{location:String(u)}});};\n"
"self.Request = function(input,init){init=init||{};this.url=__swResolve(typeof input==='string'?input:(input&&input.url)||String(input));this.method=String(init.method||(input&&input.method)||'GET').toUpperCase();this.headers=new self.Headers(init.headers||(input&&input.headers)||{});this.mode=init.mode||(input&&input.mode)||'cors';this.credentials=init.credentials||(input&&input.credentials)||'same-origin';};\n"
"self.fetch = function(input,init){ return new Promise(function(res, rej){ var req = input instanceof self.Request ? input : new self.Request(input,init); var t = __nativeFetchSync(req.url); if (t === null) rej(new TypeError('Failed to fetch')); else res(new self.Response(t,{url:req.url,status:200,headers:{}})); }); };\n"
"function __swKey(r){return __swResolve(typeof r==='string'?r:(r&&r.url)||String(r));} function __swResponse(o){return new self.Response(o.body,{status:o.status||200,statusText:o.statusText||'OK',url:o.url||'',headers:o.headers||{}});} function __swScope(){return self.registration.scope;} function __swProfile(){return String(self.__mlProfileId||0);} function __swParse(s,fallback){try{return JSON.parse(s);}catch(e){return fallback;}}\n"
"function __swCache(name){name=String(name);var scope=__swScope(),profile=__swProfile();return {put:function(req,res){var k=__swKey(req);return Promise.resolve(res&&res.clone?res.clone():res).then(function(r){if(!r||typeof r.text!=='function')throw new TypeError('Cache.put requires a Response');return r.text().then(function(body){var hs={};if(r.headers&&r.headers.entries)r.headers.entries().forEach(function(p){hs[p[0]]=p[1];});var entry={body:body,status:r.status||200,statusText:r.statusText||'OK',headers:hs,url:r.url||k};if(!__nativeCacheSet(scope,name,k,JSON.stringify(entry),profile))throw new Error('CacheStorage write failed');});});},match:function(req){var x=__nativeCacheGet(scope,name,__swKey(req),profile);if(x===null)return Promise.resolve(undefined);var o=__swParse(x,null);return Promise.resolve(o?__swResponse(o):undefined);},delete:function(req){return Promise.resolve(!!__nativeCacheRemove(scope,name,__swKey(req),profile));},keys:function(req){var list=__swParse(__nativeCacheKeys(scope,name,profile),[]);if(req===undefined)return Promise.resolve(list.map(function(k){return new self.Request(k);}));var key=__swKey(req);return Promise.resolve(list.indexOf(key)>=0?[new self.Request(key)]:[]);},add:function(req){var k=__swKey(req);return self.fetch(k).then(function(r){if(!r.ok)throw new TypeError('Request failed');return this.put(k,r);}.bind(this));},addAll:function(reqs){var cache=this;return Promise.all(Array.prototype.map.call(reqs,function(r){return cache.add(r);})).then(function(){return undefined;});}};}\n"
"self.caches = {open:function(n){var name=String(n);if(!__nativeCacheOpen(self.registration.scope,name,__swProfile()))return Promise.reject(new Error('CacheStorage unavailable'));return Promise.resolve(__swCache(name));},has:function(n){return Promise.resolve(__swParse(__nativeCacheNames(self.registration.scope,__swProfile()),[]).indexOf(String(n))>=0);},delete:function(n){return Promise.resolve(!!__nativeCacheDelete(self.registration.scope,String(n),__swProfile()));},keys:function(){return Promise.resolve(__swParse(__nativeCacheNames(self.registration.scope,__swProfile()),[]));},match:function(req){var ks=__swParse(__nativeCacheNames(self.registration.scope,__swProfile()),[]);return ks.reduce(function(p,k){return p.then(function(v){return v!==undefined?v:__swCache(k).match(req);});},Promise.resolve(undefined));}}; self.skipWaiting=function(){return Promise.resolve();}; self.clients={claim:function(){return Promise.resolve();},matchAll:function(){return Promise.resolve([]);},get:function(){return Promise.resolve(undefined);},openWindow:function(){return Promise.resolve(null);}}; self.registration={scope:self.location.origin+'/',active:self,waiting:null,installing:null,update:function(){return Promise.resolve(self.registration);},unregister:function(){return Promise.resolve(false);}};\n"
"self.__dispatchSW = function(d){var type=String(d.__ml_sw_event||''),waits=[],hasResponse=false,responsePromise=null;var ev={type:type,waitUntil:function(p){waits.push(Promise.resolve(p));},respondWith:function(p){if(hasResponse)throw new TypeError('respondWith called more than once');hasResponse=true;responsePromise=Promise.resolve(p);}};if(type==='fetch'){ev.request=new self.Request(d.request||d.url||'',d.request||{});ev.clientId=d.clientId||'';ev.resultingClientId=d.resultingClientId||'';ev.preloadResponse=Promise.resolve(undefined);}if(d.__ml_sw_scope)self.registration.scope=String(d.__ml_sw_scope);if(d.__ml_sw_profile!==undefined)self.__mlProfileId=d.__ml_sw_profile;self.__dispatch(type,ev);if(type==='fetch'){if(!hasResponse){__nativePost(JSON.stringify({__ml_sw_reply:d.id,fallback:true}));return;}responsePromise.then(function(r){if(typeof r==='string')r=new self.Response(r);if(!r||typeof r.text!=='function')throw new TypeError('respondWith must receive a Response or promise');return r.clone?r.clone().text().then(function(body){var hs={};if(r.headers&&r.headers.entries)r.headers.entries().forEach(function(p){hs[p[0]]=p[1];});return {body:body,status:r.status||200,statusText:r.statusText||'OK',headers:hs,url:r.url||''};}):r.text().then(function(body){return {body:body,status:r.status||200,statusText:r.statusText||'OK',headers:{},url:r.url||''};});}).then(function(r){__nativePost(JSON.stringify({__ml_sw_reply:d.id,fallback:false,response:r}));},function(e){__nativePost(JSON.stringify({__ml_sw_reply:d.id,fallback:false,error:String(e&&e.message||e)}));});}else{Promise.all(waits).then(function(){__nativePost(JSON.stringify({__ml_sw_lifecycle:type,ok:true}));},function(e){__nativePost(JSON.stringify({__ml_sw_lifecycle:type,ok:false,error:String(e&&e.message||e)}));});}};\n"
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
static void worker_dispatch_message(JSContext* ctx, const char* json) {
    JSValue val = JS_ParseJSON(ctx, json, strlen(json), "<message>");
    if (JS_IsException(val)) { JS_FreeValue(ctx, val); val = JS_NULL; }
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
    g_out_fd = out_fd;
    char* src = webpage_mixed_blocked(script_url) ? NULL : nal_http_get(script_url);
    if (!src) { emit(out_fd, 'E', "worker: failed to load script", 30); _exit(0); }

    JSRuntime* rt = JS_NewRuntime(); JSContext* ctx = JS_NewContext(rt);
    JSValue g = JS_GetGlobalObject(ctx);
#define WFN(name, fn, n) JS_SetPropertyStr(ctx, g, name, JS_NewCFunction(ctx, fn, name, n))
    WFN("__nativePost", w_post, 1); WFN("__nativeLog", w_log, 1); WFN("__nativeError", w_error, 1); WFN("__nativeNow", w_now, 0); WFN("__nativeClose", w_close, 0); WFN("__nativeFetchSync", w_fetch_sync, 1); WFN("__nativeCacheOpen", w_cache_open, 3); WFN("__nativeCacheGet", w_cache_get, 4); WFN("__nativeCacheSet", w_cache_set, 5); WFN("__nativeCacheRemove", w_cache_remove, 4); WFN("__nativeCacheDelete", w_cache_delete, 3); WFN("__nativeCacheNames", w_cache_names, 2); WFN("__nativeCacheKeys", w_cache_keys, 3);
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
            unsigned char h[5]; ssize_t r = read(in_fd, h, 5);
            if (r <= 0) break;                                     /* родитель закрыл пайп — завершаемся */
            uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
            char* buf = malloc(len + 1); size_t got = 0;
            while (buf && got < len) { ssize_t rr = read(in_fd, buf + got, len - got); if (rr <= 0) break; got += (size_t)rr; }
            if (buf) { buf[len] = 0; if (h[0] == 'M') worker_dispatch_message(ctx, buf); else if (h[0] == 'X') { free(buf); break; } free(buf); }
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

int worker_poll(WorkerConn* c, char* type, char** text) {
    if (!c->used) return 0;
    unsigned char h[5]; ssize_t r = read(c->rfd, h, 5);
    if (r <= 0) return 0;
    uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
    char* buf = len ? malloc(len + 1) : NULL; size_t got = 0;
    while (buf && got < len) { ssize_t rr = read(c->rfd, buf + got, len - got); if (rr <= 0) { if (rr < 0 && errno == EAGAIN) continue; break; } got += (size_t)rr; }
    if (buf) buf[len] = 0;
    *type = (char)h[0]; *text = buf;
    if (*type == 'X') { close(c->rfd); close(c->wfd); int st; waitpid(c->pid, &st, WNOHANG); c->used = 0; }
    return 1;
}
void worker_post(WorkerConn* c, const char* json) { if (c->used) emit(c->wfd, 'M', json, (uint32_t)strlen(json)); }
void worker_terminate(WorkerConn* c) {
    if (!c->used) return;
    kill(c->pid, SIGKILL); int st; waitpid(c->pid, &st, WNOHANG);
    close(c->rfd); close(c->wfd); c->used = 0;
}
