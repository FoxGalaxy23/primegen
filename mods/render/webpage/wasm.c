/*
 * WebAssembly Web API adapter for MLBrowser.
 * Execution is delegated to the platform Node/V8 WebAssembly runtime through a
 * private, line-oriented RPC pipe. WebAssembly modules never receive Node's
 * ambient APIs: only explicitly supplied imports are bridged into a module.
 * This is a compatibility bridge, not a security boundary against host imports.
 */
#include "quickjs.h"
#include "wasm_node_host.h"
#include "wasm_binary.h"
#include "wasm_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#endif

#define MLB_WASM_MAX_RPC_BYTES (96u * 1024u * 1024u)
#define MLB_WASM_MAX_MEMORY_BYTES (64u * 1024u * 1024u)
#ifndef MLB_WASM_RPC_TIMEOUT_MS
#define MLB_WASM_RPC_TIMEOUT_MS 10000
#endif

typedef struct SBuf { char *s; size_t n, cap; } SBuf;
static void sb_need(SBuf *b, size_t add) { if (b->n + add + 1 > b->cap) { size_t c = b->cap ? b->cap : 256; while (c < b->n + add + 1) { if (c > SIZE_MAX / 2) { c = b->n + add + 1; break; } c *= 2; } char *p = realloc(b->s, c); if (!p) return; b->s = p; b->cap = c; } }
static void sb_mem(SBuf *b, const char *s, size_t n) { sb_need(b, n); if (!b->s || b->n + n + 1 > b->cap) return; memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0; }
static void sb_s(SBuf *b, const char *s) { if (s) sb_mem(b, s, strlen(s)); }
static void sb_ch(SBuf *b, char c) { sb_mem(b, &c, 1); }
static void sb_i64(SBuf *b, int64_t v) { char t[48]; snprintf(t, sizeof t, "%lld", (long long)v); sb_s(b, t); }
static void sb_u64(SBuf *b, uint64_t v) { char t[48]; snprintf(t, sizeof t, "%llu", (unsigned long long)v); sb_s(b, t); }
static void sb_json_string(SBuf *b, const char *s, size_t n) {
    sb_ch(b, '"');
    for (size_t i=0;i<n;i++) { unsigned char c=(unsigned char)s[i];
        if (c=='"' || c=='\\') { sb_ch(b,'\\'); sb_ch(b,(char)c); }
        else if (c=='\b') sb_s(b,"\\b"); else if (c=='\f') sb_s(b,"\\f");
        else if (c=='\n') sb_s(b,"\\n"); else if (c=='\r') sb_s(b,"\\r"); else if (c=='\t') sb_s(b,"\\t");
        else if (c<0x20) { char q[7]; snprintf(q,sizeof q,"\\u%04x",c); sb_s(b,q); }
        else sb_ch(b,(char)c);
    }
    sb_ch(b, '"');
}
static char *sb_take(SBuf *b) { if (!b->s) return strdup(""); char *s=b->s; b->s=NULL;b->n=b->cap=0;return s; }

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64_encode(const uint8_t *src, size_t n) {
    if (n > (SIZE_MAX - 4) / 4 * 3) return NULL;
    size_t outn=((n+2)/3)*4; char *out=malloc(outn+1); if(!out)return NULL;
    size_t i=0,j=0;
    while(i<n) { uint32_t v=(uint32_t)src[i++]<<16; int have2=0,have3=0; if(i<n){v|=(uint32_t)src[i++]<<8;have2=1;} if(i<n){v|=src[i++];have3=1;}
        out[j++]=b64chars[(v>>18)&63]; out[j++]=b64chars[(v>>12)&63]; out[j++]=have2?b64chars[(v>>6)&63]:'='; out[j++]=have3?b64chars[v&63]:'=';
    }
    out[j]=0; return out;
}
static int b64_value(unsigned char c) { if(c>='A'&&c<='Z')return c-'A'; if(c>='a'&&c<='z')return c-'a'+26; if(c>='0'&&c<='9')return c-'0'+52; if(c=='+')return 62; if(c=='/')return 63; return -1; }
static uint8_t *b64_decode(const char *s, size_t n, size_t *outn) {
    *outn=0; if(!s || (n%4)!=0) return NULL; size_t cap=(n/4)*3; uint8_t *out=malloc(cap?cap:1); if(!out)return NULL;
    for(size_t i=0;i<n;i+=4) { int a=b64_value(s[i]), b=b64_value(s[i+1]); int c=s[i+2]=='='?0:b64_value(s[i+2]); int d=s[i+3]=='='?0:b64_value(s[i+3]); if(a<0||b<0||c<0||d<0){free(out);return NULL;}
        uint32_t v=((uint32_t)a<<18)|((uint32_t)b<<12)|((uint32_t)c<<6)|(uint32_t)d; out[(*outn)++]=(uint8_t)(v>>16); if(s[i+2]!='=')out[(*outn)++]=(uint8_t)(v>>8); if(s[i+3]!='=')out[(*outn)++]=(uint8_t)v;
    } return out;
}

typedef struct WasmHost WasmHost;
typedef struct WasmRef WasmRef;
typedef struct WasmCallback { int id; JSValue fn; struct WasmCallback *next; } WasmCallback;
typedef struct WasmObjectRef { int id; JSValue value; struct WasmObjectRef *next; } WasmObjectRef;
/* Native externref IDs are weakly indexed here. Strong references live in the
 * GC-marked fields of the reachable native Global/Table/Instance wrappers. */
typedef struct WasmNativeObjectRef {
    uint64_t id;
    JSValue weak_or_primitive;
    JSValue temporary_value;
    int is_weak;
    int has_temporary;
    struct WasmNativeObjectRef *next;
} WasmNativeObjectRef;
typedef struct WasmPendingRelease { char cmd[24]; int id; struct WasmPendingRelease *next; } WasmPendingRelease;
struct WasmHost {
    JSContext *ctx; JSRuntime *rt;
#ifndef _WIN32
    pid_t pid; int to_child, from_child;
#endif
    int started, failed, next_req, next_module, next_instance, next_ref, next_callback, next_object;
    unsigned native_call_depth;
    JSValue weakref_ctor, weakref_deref;
    char *rx; size_t rx_len, rx_cap;
    WasmCallback *callbacks;
    WasmObjectRef *objects;
    WasmNativeObjectRef *native_objects;
    WasmPendingRelease *pending_release_head, *pending_release_tail;
    WasmHost *next;
};
static WasmHost *g_wasm_hosts;
static JSClassID wasm_module_cid, wasm_instance_cid, wasm_memory_cid, wasm_global_cid, wasm_table_cid;
static int wasm_class_ids_ready;

typedef struct WasmModule { int id; JSValue imports, exports; MlbWasmModule *native; } WasmModule;
typedef struct WasmInstance {
    int id; JSValue module_obj, imports_obj; int *callback_ids; int callback_count, callback_cap;
    MlbWasmInstance *native; JSContext *ctx; JSRuntime *runtime;
    JSValue *native_import_values; int native_import_count; JSValue *native_functions;
    uint32_t native_function_capacity; JSValue native_exception, exports_obj;
    uint64_t *native_externref_handles; JSValue *native_externref_values; uint32_t native_externref_count;
    struct WasmInstance *registry_next; int registry_linked;
} WasmInstance;
typedef struct WasmTableRoots { MlbWasmTable *table; JSRuntime *rt; uint32_t refs; JSValue *values; uint32_t length, capacity; struct WasmTableRoots *next; } WasmTableRoots;
static WasmTableRoots *g_wasm_table_roots;
struct WasmRef {
    int standalone; int id; int owner_instance_id; char *name;
    JSValue owner_obj; JSValue buffer; JSValue buffer_baseline; size_t buffer_len; uint64_t memory_epoch;
    int value_type; int mutable_value; int remote_imported; int local_table_mode;
    JSValue *table_values; uint32_t table_length, table_capacity, table_maximum; int table_has_maximum;
    MlbWasmMemory *native_memory; MlbWasmGlobal *native_global; MlbWasmTable *native_table;
    WasmTableRoots *table_root_store; JSRuntime *runtime;
    WasmRef *memory_registry_next; int memory_registry_linked;
    WasmRef *resource_registry_next; int resource_registry_linked;
    uint64_t *externref_handles; JSValue *externref_values; uint32_t externref_length;
    WasmRef *unused_next;
};
static WasmRef *g_wasm_memory_refs;
static WasmRef *g_wasm_resource_refs;
static WasmInstance *g_wasm_instances;
static uint64_t g_wasm_next_native_externref_id = 1;

static JSValue wasm_export_function_call(JSContext*,JSValueConst,int,JSValueConst*,int,JSValue*);
static JSValue make_native_table_function(JSContext*,JSValueConst,uint32_t);
static void wasm_memory_detach_resource_buffers(JSContext*,MlbWasmMemory*);
static void wasm_memory_detach_stale_buffers(JSContext*);
JSValue wasm_remote_function_call(JSContext*,JSValueConst,int,JSValueConst*,int,JSValue*);
static void mlb_wasm_encode_value(JSContext*,WasmHost*,JSValueConst,SBuf*);
static int wasm_native_host_call_multi(void*,uint32_t,const MlbWasmValue*,size_t,MlbWasmValue*,size_t,size_t*,char*,size_t);
static void native_instance_sync_memory_exports(JSContext*,WasmInstance*,JSValueConst,int);
static int memory_sync(JSContext*,WasmHost*,WasmRef*,int);
static WasmTableRoots *wasm_table_roots_attach(JSContext*,MlbWasmTable*);
static void wasm_table_roots_release_rt(JSRuntime*,WasmTableRoots*);
static int wasm_table_roots_ensure(JSContext*,WasmRef*,uint32_t);
static int wasm_table_root_set(JSContext*,WasmRef*,uint32_t,JSValueConst,int);
static void wasm_table_root_new_instance_functions(JSContext*,WasmRef*,JSValueConst,MlbWasmInstance*);
static int wasm_native_externref_sync_all(JSContext*);
static void wasm_native_externref_cleanup(JSContext*,WasmHost*);
static void wasm_native_externref_clear_temporaries(WasmHost*);
static void wasm_resource_ref_register(JSContext*,WasmRef*);
static void wasm_resource_ref_unregister(WasmRef*);
static void wasm_instance_registry_register(JSContext*,WasmInstance*);
static void wasm_instance_registry_unregister(WasmInstance*);

static WasmHost *host_for_ctx(JSContext *ctx, int create);
#ifndef _WIN32
static int host_write_all(int fd, const char *data, size_t n);
#endif
static WasmHost *host_for_rt(JSRuntime *rt) { for(WasmHost*h=g_wasm_hosts;h;h=h->next)if(h->rt==rt)return h;return NULL; }
static void host_remove_callback_rt(JSRuntime *rt, int id) { WasmHost*h=host_for_rt(rt); if(!h)return; WasmCallback **p=&h->callbacks; while(*p){if((*p)->id==id){WasmCallback*x=*p;*p=x->next;JS_FreeValueRT(rt,x->fn);free(x);return;}p=&(*p)->next;} }
static int wasm_rpc_timeout_ms(void) {
    const char *e = getenv("MLB_WASM_RPC_TIMEOUT_MS");
    if (e && *e) {
        char *end = NULL;
        long v = strtol(e, &end, 10);
        if (end != e && *end == 0 && v >= 250 && v <= 120000) return (int)v;
    }
    return MLB_WASM_RPC_TIMEOUT_MS;
}
static void wasm_queue_release_rt(JSRuntime *rt, const char *cmd, int id) {
    WasmHost *h = host_for_rt(rt);
    if (!h || !h->started || h->failed || id <= 0) return;
    WasmPendingRelease *r = calloc(1, sizeof(*r));
    if (!r) return;
    snprintf(r->cmd, sizeof(r->cmd), "%s", cmd);
    r->id = id;
    if (h->pending_release_tail) h->pending_release_tail->next = r;
    else h->pending_release_head = r;
    h->pending_release_tail = r;
}
#ifndef _WIN32
static void wasm_host_stop(WasmHost *h, int sig) {
    if (!h) return;
    if (h->to_child >= 0) { close(h->to_child); h->to_child = -1; }
    if (h->from_child >= 0) { close(h->from_child); h->from_child = -1; }
    if (h->pid > 0) {
        kill(h->pid, sig);
        int st; pid_t w;
        do { w = waitpid(h->pid, &st, 0); } while (w < 0 && errno == EINTR);
        h->pid = 0;
    }
    h->failed = 1;
}
static int wasm_flush_pending_releases(WasmHost *h) {
    while (h && h->pending_release_head) {
        WasmPendingRelease *r = h->pending_release_head;
        char line[128];
        int n = snprintf(line, sizeof line, "{\"cmd\":\"%s\",\"id\":%d,\"notify\":true}\n", r->cmd, r->id);
        if (n <= 0 || (size_t)n >= sizeof line || !host_write_all(h->to_child, line, (size_t)n)) {
            wasm_host_stop(h, SIGKILL);
            return 0;
        }
        h->pending_release_head = r->next;
        if (!h->pending_release_head) h->pending_release_tail = NULL;
        free(r);
    }
    return 1;
}
#endif
static void ref_make_remote(WasmRef *r, SBuf *b) { if(r->standalone){sb_s(b,"{\"kind\":\"standalone\",\"id\":");sb_i64(b,r->id);sb_ch(b,'}');}else{sb_s(b,"{\"kind\":\"instance\",\"id\":");sb_i64(b,r->owner_instance_id);sb_s(b,",\"name\":");sb_json_string(b,r->name?r->name:"",strlen(r->name?r->name:""));sb_ch(b,'}');} }

#ifndef _WIN32
static int host_write_all(int fd, const char *data, size_t n) { size_t off=0; while(off<n){ssize_t w=write(fd,data+off,n-off);if(w<0){if(errno==EINTR)continue;return 0;}if(w==0)return 0;off+=(size_t)w;}return 1; }
static int host_read_line(WasmHost *h, char **out) {
    *out=NULL;
    for (;;) {
        if (h->rx_len) { char *nl=memchr(h->rx,'\n',h->rx_len); if(nl){size_t n=(size_t)(nl-h->rx);char *line=malloc(n+1);if(!line)return 0;memcpy(line,h->rx,n);line[n]=0;size_t remain=h->rx_len-n-1;memmove(h->rx,nl+1,remain);h->rx_len=remain;*out=line;return 1;} }
        struct pollfd pfd={h->from_child,POLLIN|POLLHUP|POLLERR,0}; int pr=poll(&pfd,1,wasm_rpc_timeout_ms()); if(pr==0){h->failed=1;return 0;} if(pr<0){if(errno==EINTR)continue;h->failed=1;return 0;}
        char tmp[65536];ssize_t n=read(h->from_child,tmp,sizeof tmp);if(n<=0){h->failed=1;return 0;}
        if(h->rx_len+(size_t)n>MLB_WASM_MAX_RPC_BYTES){h->failed=1;return 0;}
        if(h->rx_len+(size_t)n+1>h->rx_cap){size_t cap=h->rx_cap?h->rx_cap:65536;while(cap<h->rx_len+(size_t)n+1)cap*=2;char*q=realloc(h->rx,cap);if(!q){h->failed=1;return 0;}h->rx=q;h->rx_cap=cap;}
        memcpy(h->rx+h->rx_len,tmp,(size_t)n);h->rx_len+=(size_t)n;h->rx[h->rx_len]=0;
    }
}
static int host_start(WasmHost *h) {
    if(h->started)return !h->failed;
    if(h->failed)return 0;
    h->started=1; signal(SIGPIPE,SIG_IGN);
    int to[2],from[2]; if(pipe(to)){h->failed=1;return 0;} if(pipe(from)){close(to[0]);close(to[1]);h->failed=1;return 0;}
    pid_t pid=fork(); if(pid<0){close(to[0]);close(to[1]);close(from[0]);close(from[1]);h->failed=1;return 0;}
    if(pid==0){dup2(to[0],STDIN_FILENO);dup2(from[1],STDOUT_FILENO);close(to[0]);close(to[1]);close(from[0]);close(from[1]);execlp("node","node","--max-old-space-size=256","--wasm-max-mem-pages=1024","-e",mlb_wasm_node_host_script,(char*)NULL);_exit(127);}
    close(to[0]);close(from[1]);h->pid=pid;h->to_child=to[1];h->from_child=from[0];
    /* Smoke handshake prevents a missing Node executable from looking like a malformed module. */
    const char *ping="{\"cmd\":\"ping\",\"req\":0}\n"; if(!host_write_all(h->to_child,ping,strlen(ping))){wasm_host_stop(h,SIGKILL);return 0;}
    char *line=NULL;if(!host_read_line(h,&line)){wasm_host_stop(h,SIGKILL);return 0;}
    JSValue r=JS_ParseJSON(h->ctx,line,strlen(line),"<wasm-host-handshake>");free(line);if(JS_IsException(r)){JS_FreeValue(h->ctx,JS_GetException(h->ctx));wasm_host_stop(h,SIGKILL);return 0;}
    JSValue ok=JS_GetPropertyStr(h->ctx,r,"ok");int yes=JS_ToBool(h->ctx,ok);JS_FreeValue(h->ctx,ok);JS_FreeValue(h->ctx,r);if(!yes){wasm_host_stop(h,SIGKILL);return 0;}return 1;
}
#else
static int host_start(WasmHost *h) { h->started=1;h->failed=1;return 0; }
#endif

static JSValue rpc_decode_value(JSContext *ctx, WasmHost *h, JSValueConst v);
static void rpc_handle_callback(JSContext *ctx, WasmHost *h, JSValueConst message) {
    JSValue cbv=JS_GetPropertyStr(ctx,message,"callback"), callv=JS_GetPropertyStr(ctx,message,"call");int32_t cbid=-1,callid=0;JS_ToInt32(ctx,&cbid,cbv);JS_ToInt32(ctx,&callid,callv);JS_FreeValue(ctx,cbv);JS_FreeValue(ctx,callv);
    WasmCallback *cb=h->callbacks;while(cb&&cb->id!=cbid)cb=cb->next;
    SBuf reply={0};sb_s(&reply,"{\"type\":\"callbackResult\",\"call\":");sb_i64(&reply,callid);
    if(!cb){sb_s(&reply,",\"error\":\"Unknown host callback\"}");}
    else {
        JSValue arr=JS_GetPropertyStr(ctx,message,"args");JSValue lv=JS_GetPropertyStr(ctx,arr,"length");uint32_t n=0;JS_ToUint32(ctx,&n,lv);JS_FreeValue(ctx,lv);if(n>256)n=256;
        JSValue *argv=calloc(n?n:1,sizeof(JSValue));for(uint32_t i=0;i<n;i++){JSValue j=JS_GetPropertyUint32(ctx,arr,i);argv[i]=rpc_decode_value(ctx,h,j);JS_FreeValue(ctx,j);}
        JSValue ret=JS_Call(ctx,cb->fn,JS_UNDEFINED,(int)n,(JSValueConst*)argv);for(uint32_t i=0;i<n;i++)JS_FreeValue(ctx,argv[i]);free(argv);JS_FreeValue(ctx,arr);
        if(JS_IsException(ret)){JSValue ex=JS_GetException(ctx);JSValue str=JS_ToString(ctx,ex);size_t z=0;const char*e=JS_ToCStringLen(ctx,&z,str);sb_s(&reply,",\"error\":");sb_json_string(&reply,e?e:"WebAssembly host callback threw",e?z:31);JSValue nv=JS_IsObject(ex)?JS_GetPropertyStr(ctx,ex,"name"):JS_UNDEFINED;const char*ename=JS_ToCString(ctx,nv);sb_s(&reply,",\"errorName\":");sb_json_string(&reply,(ename&&*ename)?ename:"Error",strlen((ename&&*ename)?ename:"Error"));if(ename)JS_FreeCString(ctx,ename);JS_FreeValue(ctx,nv);if(e)JS_FreeCString(ctx,e);JS_FreeValue(ctx,str);JS_FreeValue(ctx,ex);}
        else {SBuf enc={0};mlb_wasm_encode_value(ctx,h,ret,&enc);sb_s(&reply,",\"value\":");sb_s(&reply,enc.s?enc.s:"null");free(enc.s);JS_FreeValue(ctx,ret);}
        if(JS_IsException(ret)) JS_FreeValue(ctx,ret);
    }
    sb_s(&reply,"}\n");
#ifndef _WIN32
    if(h->to_child>=0&&!h->failed&&!host_write_all(h->to_child,reply.s,reply.n))h->failed=1;
#endif
    free(reply.s);
}

static JSValue rpc_do(JSContext *ctx, WasmHost *h, const char *body, char *err, size_t errcap, int *pok) {
    if(pok)*pok=0;if(err&&errcap)err[0]=0;
#ifndef _WIN32
    if(!host_start(h)){snprintf(err,errcap,"Node.js WebAssembly runtime unavailable (install Node.js to enable WebAssembly)");return JS_UNDEFINED;}
    if(!wasm_flush_pending_releases(h)){snprintf(err,errcap,"WebAssembly runtime stopped while releasing objects");return JS_UNDEFINED;}
    int req=++h->next_req;size_t n=strlen(body);char *full=malloc(n+64);if(!full){snprintf(err,errcap,"Out of memory preparing WebAssembly request");return JS_UNDEFINED;}
    if(n&&body[n-1]=='}')snprintf(full,n+64,"%.*s,\"req\":%d}\n",(int)(n-1),body,req);else{free(full);snprintf(err,errcap,"Malformed internal WebAssembly command");return JS_UNDEFINED;}
    if(!host_write_all(h->to_child,full,strlen(full))){free(full);wasm_host_stop(h,SIGKILL);snprintf(err,errcap,"WebAssembly runtime process closed its input");return JS_UNDEFINED;}free(full);
    for(;;){char *line=NULL;if(!host_read_line(h,&line)){snprintf(err,errcap,"WebAssembly runtime timed out or exited");wasm_host_stop(h,SIGKILL);return JS_UNDEFINED;}
        JSValue msg=JS_ParseJSON(ctx,line,strlen(line),"<wasm-host-rpc>");free(line);if(JS_IsException(msg)){JS_FreeValue(ctx,JS_GetException(ctx));snprintf(err,errcap,"Invalid JSON response from WebAssembly runtime");wasm_host_stop(h,SIGKILL);return JS_UNDEFINED;}
        JSValue ty=JS_GetPropertyStr(ctx,msg,"type");const char *t=JS_ToCString(ctx,ty);int iscb=t&&strcmp(t,"callback")==0;if(t)JS_FreeCString(ctx,t);JS_FreeValue(ctx,ty);
        if(iscb){rpc_handle_callback(ctx,h,msg);JS_FreeValue(ctx,msg);if(h->failed){snprintf(err,errcap,"WebAssembly callback pipe failed");wasm_host_stop(h,SIGKILL);return JS_UNDEFINED;}continue;}
        JSValue ridv=JS_GetPropertyStr(ctx,msg,"req");int32_t rid=-1;JS_ToInt32(ctx,&rid,ridv);JS_FreeValue(ctx,ridv);
        if(rid!=req){JS_FreeValue(ctx,msg);snprintf(err,errcap,"Mismatched WebAssembly runtime response");wasm_host_stop(h,SIGKILL);return JS_UNDEFINED;}
        JSValue okv=JS_GetPropertyStr(ctx,msg,"ok");int ok=JS_ToBool(ctx,okv);JS_FreeValue(ctx,okv);
        if(!ok){JSValue ev=JS_GetPropertyStr(ctx,msg,"error");const char *es=JS_ToCString(ctx,ev);snprintf(err,errcap,"%s",es?es:"WebAssembly operation failed");if(es)JS_FreeCString(ctx,es);JS_FreeValue(ctx,ev);JSValue namev=JS_GetPropertyStr(ctx,msg,"name");const char*ns=JS_ToCString(ctx,namev);if(ns&&errcap>0){/* preserve class prefix in a stable textual form */ char tmp[256];snprintf(tmp,sizeof tmp,"%s",err);snprintf(err,errcap,"%s: %s",ns,tmp);}if(ns)JS_FreeCString(ctx,ns);JS_FreeValue(ctx,namev);JS_FreeValue(ctx,msg);return JS_UNDEFINED;}
        JSValue val=JS_GetPropertyStr(ctx,msg,"value");JS_FreeValue(ctx,msg);if(pok)*pok=1;return val;
    }
#else
    (void)ctx;(void)h;(void)body;snprintf(err,errcap,"WebAssembly bridge currently requires the POSIX process backend");return JS_UNDEFINED;
#endif
}

static WasmHost *host_for_ctx(JSContext *ctx, int create) {
    for(WasmHost*h=g_wasm_hosts;h;h=h->next)if(h->ctx==ctx)return h;
    if(!create)return NULL;WasmHost*h=calloc(1,sizeof *h);if(!h)return NULL;
#ifndef _WIN32
    h->to_child=-1;h->from_child=-1;
#endif
    h->ctx=ctx;h->rt=JS_GetRuntime(ctx);h->next_req=0;h->next_module=100;h->next_instance=100;h->next_ref=100;h->next_callback=100;
    h->weakref_ctor=JS_UNDEFINED;h->weakref_deref=JS_UNDEFINED;
    /* WeakRef is an optimization for avoiding host-side retention of otherwise
       unreachable externref objects. The strong-root fallback is still correct
       for runtimes without WeakRef, although cycles can live longer. This flag
       exists so the fallback stays covered by the native-only regression suite. */
    const char *force_strong_refs=getenv("MLB_WASM_FORCE_STRONG_EXTERNREF_ROOTS");
    if(!force_strong_refs||strcmp(force_strong_refs,"1")!=0){
        JSValue global=JS_GetGlobalObject(ctx);
        JSValue weak_ctor=JS_GetPropertyStr(ctx,global,"WeakRef");
        JSValue weak_deref=JS_UNDEFINED;
        if(JS_IsException(weak_ctor)){JS_FreeValue(ctx,JS_GetException(ctx));weak_ctor=JS_UNDEFINED;}
        if(JS_IsFunction(ctx,weak_ctor)){
            JSValue weak_proto=JS_GetPropertyStr(ctx,weak_ctor,"prototype");
            if(JS_IsException(weak_proto))JS_FreeValue(ctx,JS_GetException(ctx));
            else{
                weak_deref=JS_GetPropertyStr(ctx,weak_proto,"deref");
                if(JS_IsException(weak_deref)){JS_FreeValue(ctx,JS_GetException(ctx));weak_deref=JS_UNDEFINED;}
            }
            JS_FreeValue(ctx,weak_proto);
        }
        JS_FreeValue(ctx,global);
        if(JS_IsFunction(ctx,weak_ctor)&&JS_IsFunction(ctx,weak_deref)){h->weakref_ctor=weak_ctor;h->weakref_deref=weak_deref;}else{JS_FreeValue(ctx,weak_ctor);JS_FreeValue(ctx,weak_deref);}
    }
    h->next=g_wasm_hosts;g_wasm_hosts=h;return h;
}
static int host_add_callback(JSContext *ctx, WasmHost *h, JSValueConst fn) { WasmCallback*c=calloc(1,sizeof *c);if(!c)return -1;c->id=++h->next_callback;c->fn=JS_DupValue(ctx,fn);c->next=h->callbacks;h->callbacks=c;return c->id; }
static void host_remove_callback(JSContext *ctx, WasmHost*h,int id){if(!h)return;WasmCallback**p=&h->callbacks;while(*p){if((*p)->id==id){WasmCallback*c=*p;*p=c->next;JS_FreeValue(ctx,c->fn);free(c);return;}p=&(*p)->next;}}

static JSValue wasm_named_error(JSContext *ctx,const char *name,const char *message) {
    const char *msg=message?message:"WebAssembly operation failed";
    /* Built-in ECMAScript errors must retain their real prototypes, not merely
       carry a renamed Error.name property (which breaks instanceof checks). */
    if(name&&!strcmp(name,"TypeError"))return JS_ThrowTypeError(ctx,"%s",msg);
    if(name&&!strcmp(name,"RangeError"))return JS_ThrowRangeError(ctx,"%s",msg);
    JSValue e=JS_NewError(ctx);JS_SetPropertyStr(ctx,e,"name",JS_NewString(ctx,name?name:"WebAssembly.RuntimeError"));JS_SetPropertyStr(ctx,e,"message",JS_NewString(ctx,msg));return JS_Throw(ctx,e);
}
static JSValue wasm_error_ctor(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv,int magic){(void)this_val;const char*names[]={"CompileError","LinkError","RuntimeError"};JSValue e=JS_NewError(ctx);JS_SetPropertyStr(ctx,e,"name",JS_NewString(ctx,names[(unsigned)magic%3]));if(argc>0)JS_SetPropertyStr(ctx,e,"message",JS_ToString(ctx,argv[0]));return e;}
static const char *strip_error_class(char *msg, const char **kind) { *kind="RuntimeError"; if(!msg)return "WebAssembly operation failed"; const char *p=strchr(msg,':'); if(p&&p-msg<40){static char name[48];size_t n=(size_t)(p-msg);memcpy(name,msg,n);name[n]=0;if(!strcmp(name,"CompileError")||!strcmp(name,"LinkError")||!strcmp(name,"RuntimeError")||!strcmp(name,"TypeError")||!strcmp(name,"RangeError"))*kind=name;return p+2;}return msg; }

static int get_string_field(JSContext *ctx, JSValueConst obj,const char*prop,char*out,size_t cap){JSValue v=JS_GetPropertyStr(ctx,obj,prop);if(JS_IsException(v)){JS_FreeValue(ctx,JS_GetException(ctx));if(cap)out[0]=0;return 0;}const char*s=JS_ToCString(ctx,v);int ok=s!=NULL;if(cap)snprintf(out,cap,"%s",s?s:"");if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,v);return ok;}
static int get_int_field(JSContext *ctx,JSValueConst obj,const char*prop,int *out){JSValue v=JS_GetPropertyStr(ctx,obj,prop);if(JS_IsException(v)){JS_FreeValue(ctx,JS_GetException(ctx));return 0;}int32_t x=0;int ok=JS_ToInt32(ctx,&x,v)==0;JS_FreeValue(ctx,v);*out=x;return ok;}
static int array_len(JSContext *ctx,JSValueConst arr){JSValue l=JS_GetPropertyStr(ctx,arr,"length");uint32_t n=0;JS_ToUint32(ctx,&n,l);JS_FreeValue(ctx,l);return n>100000?100000:(int)n;}
static JSValue array_item(JSContext *ctx,JSValueConst arr,int i){return JS_GetPropertyUint32(ctx,arr,(uint32_t)i);}

static int bytesource(JSContext *ctx, JSValueConst val, const uint8_t **data, size_t *len, JSValue *keep) {
    *data=NULL;*len=0;*keep=JS_UNDEFINED;size_t n=0;uint8_t*p=JS_GetArrayBuffer(ctx,&n,val);if(p){*data=p;*len=n;return 1;}
    /* JS_GetTypedArrayBuffer also handles DataView; its returned buffer must stay alive during parsing. */
    size_t off=0,bpl=0;JSValue ab=JS_GetTypedArrayBuffer(ctx,val,&off,&n,&bpl);if(JS_IsException(ab)){JS_FreeValue(ctx,JS_GetException(ctx));return 0;}
    size_t total=0;uint8_t*q=JS_GetArrayBuffer(ctx,&total,ab);if(!q||off>total||n>total-off){JS_FreeValue(ctx,ab);return 0;}*data=q+off;*len=n;*keep=ab;return 1;
}
static int host_object_handle(JSContext *ctx, WasmHost *h, JSValueConst value) {
    if(!h)return -1;
    for(WasmObjectRef *o=h->objects;o;o=o->next)if(JS_StrictEq(ctx,o->value,value))return o->id;
    if(h->next_object>=INT_MAX-1)return -1;
    WasmObjectRef *o=calloc(1,sizeof *o);if(!o)return -1;
    o->id=++h->next_object;o->value=JS_DupValue(ctx,value);o->next=h->objects;h->objects=o;return o->id;
}
static JSValue host_object_value(JSContext *ctx, WasmHost *h, int id) {
    if(id==0)return JS_NULL;
    if(h)for(WasmObjectRef *o=h->objects;o;o=o->next)if(o->id==id)return JS_DupValue(ctx,o->value);
    return JS_UNDEFINED;
}
static JSValue native_weakref_deref(JSContext *ctx, WasmHost *h, JSValueConst weakref) {
    if (!h || !JS_IsFunction(ctx,h->weakref_deref)) return JS_UNDEFINED;
    return JS_Call(ctx,h->weakref_deref,weakref,0,NULL);
}
static JSValue native_object_value(JSContext *ctx, WasmHost *h, WasmNativeObjectRef *o) {
    if (!o) return JS_UNDEFINED;
    if (o->has_temporary) return JS_DupValue(ctx,o->temporary_value);
    if (!o->is_weak) return JS_DupValue(ctx,o->weak_or_primitive);
    JSValue value=native_weakref_deref(ctx,h,o->weak_or_primitive);
    if (JS_IsException(value)) { JS_FreeValue(ctx,JS_GetException(ctx)); return JS_UNDEFINED; }
    return value;
}
static int native_object_handle(JSContext *ctx, WasmHost *h, JSValueConst value, uint64_t *out) {
    if (!h || !out) return 0;
    for (WasmNativeObjectRef *o=h->native_objects;o;o=o->next) {
        JSValue current=native_object_value(ctx,h,o);
        /* A cleared WeakRef is not the same value as the JS primitive undefined. */
        if (o->is_weak && JS_IsUndefined(current)) { JS_FreeValue(ctx,current); continue; }
        int same=JS_StrictEq(ctx,current,value);
        JS_FreeValue(ctx,current);
        if (same) {
            if (h->native_call_depth && o->is_weak && !o->has_temporary) {
                o->temporary_value=JS_DupValue(ctx,value);o->has_temporary=1;
            }
            *out=o->id;return 1;
        }
    }
    if (!g_wasm_next_native_externref_id || g_wasm_next_native_externref_id==UINT64_MAX) return 0;
    WasmNativeObjectRef *o=calloc(1,sizeof(*o));if(!o)return 0;
    o->id=g_wasm_next_native_externref_id++;
    o->weak_or_primitive=JS_UNDEFINED;o->temporary_value=JS_UNDEFINED;
    o->is_weak=JS_IsObject(value)||JS_IsFunction(ctx,value);
    if(o->is_weak&&JS_IsFunction(ctx,h->weakref_ctor)&&JS_IsFunction(ctx,h->weakref_deref)){
        JSValue weak=JS_CallConstructor(ctx,h->weakref_ctor,1,&value);
        if(!JS_IsException(weak)){
            /* A page can replace globalThis.WeakRef before the host is created.
               Verify the candidate with the captured builtin deref; a fake
               constructor must not poison the registry with a non-WeakRef. */
            JSValue probe=JS_Call(ctx,h->weakref_deref,weak,0,NULL);
            int valid=!JS_IsException(probe)&&JS_StrictEq(ctx,probe,value);
            if(JS_IsException(probe))JS_FreeValue(ctx,JS_GetException(ctx));
            JS_FreeValue(ctx,probe);
            if(valid){
                o->weak_or_primitive=weak;
                if(h->native_call_depth){o->temporary_value=JS_DupValue(ctx,value);o->has_temporary=1;}
            }else{
                JS_FreeValue(ctx,weak);o->is_weak=0;
            }
        }else{
            JS_FreeValue(ctx,JS_GetException(ctx));o->is_weak=0;
        }
    }else if(o->is_weak){
        o->is_weak=0;
    }
    if(!o->is_weak){
        /* Strong fallback for objects/functions when WeakRef is unavailable or
           untrusted, and direct storage for primitives. Cleanup removes this
           root once no native Global/Table/Instance contains the handle. */
        o->weak_or_primitive=JS_DupValue(ctx,value);
    }
    o->next=h->native_objects;h->native_objects=o;*out=o->id;return 1;
}
static int native_externref_from_js(JSContext *ctx, JSValueConst value, uint64_t *out) {
    if(!out)return 0;
    if(JS_IsNull(value)){*out=0;return 1;}
    WasmHost*h=host_for_ctx(ctx,1);return native_object_handle(ctx,h,value,out);
}
static JSValue native_externref_to_js(JSContext *ctx, uint64_t handle) {
    if(handle==0)return JS_NULL;
    WasmHost*h=host_for_ctx(ctx,0);
    if(h)for(WasmNativeObjectRef*o=h->native_objects;o;o=o->next)if(o->id==handle)return native_object_value(ctx,h,o);
    /* Resource handles are runtime-wide; accept another context in this runtime. */
    JSRuntime*rt=JS_GetRuntime(ctx);
    for(WasmHost*x=g_wasm_hosts;x;x=x->next)if(x->rt==rt&&x!=h)for(WasmNativeObjectRef*o=x->native_objects;o;o=o->next)if(o->id==handle)return native_object_value(ctx,x,o);
    return JS_UNDEFINED;
}
static void native_externref_cleanup_temporaries(WasmHost*h){
    if(!h)return;
    for(WasmNativeObjectRef*o=h->native_objects;o;o=o->next)if(o->has_temporary){JS_FreeValueRT(h->rt,o->temporary_value);o->temporary_value=JS_UNDEFINED;o->has_temporary=0;}
}
static void wasm_native_externref_cleanup(JSContext*ctx,WasmHost*h){
    if(!h||h->native_call_depth)return;
    WasmNativeObjectRef**p=&h->native_objects;
    while(*p){WasmNativeObjectRef*o=*p;if(mlb_wasm_rt_externref_is_live(o->id)){p=&o->next;continue;}*p=o->next;
        JS_FreeValue(ctx,o->weak_or_primitive);if(o->has_temporary)JS_FreeValue(ctx,o->temporary_value);free(o);
    }
}

static JSValue rpc_decode_value(JSContext *ctx, WasmHost *h, JSValueConst v) {
    if(!JS_IsObject(v))return JS_DupValue(ctx,v);
    JSValue t=JS_GetPropertyStr(ctx,v,"$undefined");if(!JS_IsUndefined(t)){int b=JS_ToBool(ctx,t);JS_FreeValue(ctx,t);if(b)return JS_UNDEFINED;}else JS_FreeValue(ctx,t);
    t=JS_GetPropertyStr(ctx,v,"$bigint");if(!JS_IsUndefined(t)){int64_t n=0;JS_ToBigInt64(ctx,&n,t);JSValue out=JS_NewBigInt64(ctx,n);JS_FreeValue(ctx,t);return out;}JS_FreeValue(ctx,t);
    t=JS_GetPropertyStr(ctx,v,"$number");if(!JS_IsUndefined(t)){const char*s=JS_ToCString(ctx,t);JSValue out=JS_NAN;if(s){if(!strcmp(s,"Infinity"))out=JS_NewFloat64(ctx,INFINITY);else if(!strcmp(s,"-Infinity"))out=JS_NewFloat64(ctx,-INFINITY);else if(!strcmp(s,"NaN"))out=JS_NewFloat64(ctx,NAN);JS_FreeCString(ctx,s);}JS_FreeValue(ctx,t);return out;}JS_FreeValue(ctx,t);
    t=JS_GetPropertyStr(ctx,v,"$jsObject");if(!JS_IsUndefined(t)){int32_t id=0;JS_ToInt32(ctx,&id,t);JS_FreeValue(ctx,t);return host_object_value(ctx,h,id);}JS_FreeValue(ctx,t);
    t=JS_GetPropertyStr(ctx,v,"$function");if(!JS_IsUndefined(t)){int32_t id=0;JS_ToInt32(ctx,&id,t);JS_FreeValue(ctx,t);JSValue d[1]={JS_NewInt32(ctx,id)};JSValue f=JS_NewCFunctionData(ctx,wasm_remote_function_call,0,0,1,(JSValueConst*)d);JS_FreeValue(ctx,d[0]);JS_DefinePropertyValueStr(ctx,f,"__mlbWasmRemoteFunctionId",JS_NewInt32(ctx,id),JS_PROP_CONFIGURABLE);return f;}JS_FreeValue(ctx,t);
    t=JS_GetPropertyStr(ctx,v,"$callback");if(!JS_IsUndefined(t)){int32_t id=0;JS_ToInt32(ctx,&id,t);JS_FreeValue(ctx,t);for(WasmCallback*c=h?h->callbacks:NULL;c;c=c->next)if(c->id==id)return JS_DupValue(ctx,c->fn);return JS_UNDEFINED;}JS_FreeValue(ctx,t);
    t=JS_GetPropertyStr(ctx,v,"$unsupported");if(!JS_IsUndefined(t)){JS_FreeValue(ctx,t);return JS_NULL;}JS_FreeValue(ctx,t);
    /* For ordinary JSON-compatible externref values, preserve the returned object shape. */
    return JS_DupValue(ctx,v);
}
static int mlb_wasm_encode_object_ref(JSContext *ctx, WasmHost *h, JSValueConst v, SBuf *b) {
    if(!JS_IsObject(v) && !JS_IsFunction(ctx,v))return 0;
    int id=host_object_handle(ctx,h,v);if(id<0){sb_s(b,"null");return 1;}
    sb_s(b,"{\"$jsObject\":");sb_i64(b,id);sb_ch(b,'}');return 1;
}
static void mlb_wasm_encode_externref(JSContext *ctx, WasmHost *h, JSValueConst v, SBuf *b) {
    if(!mlb_wasm_encode_object_ref(ctx,h,v,b))mlb_wasm_encode_value(ctx,h,v,b);
}

static void mlb_wasm_encode_value(JSContext *ctx, WasmHost *h, JSValueConst v, SBuf *b) {
    if(JS_IsUndefined(v)){sb_s(b,"{\"$undefined\":true}");return;}
    if(JS_IsNull(v)){sb_s(b,"null");return;}
    if(JS_IsBool(v)){sb_s(b,JS_ToBool(ctx,v)?"true":"false");return;}
    if(JS_IsBigInt(ctx,v)){size_t n=0;const char*s=JS_ToCStringLen(ctx,&n,v);sb_s(b,"{\"$bigint\":");sb_json_string(b,s?s:"0",s?n:1);sb_ch(b,'}');if(s)JS_FreeCString(ctx,s);return;}
    if(JS_IsNumber(v)){double d=0;JS_ToFloat64(ctx,&d,v);if(!isfinite(d)){sb_s(b,"{\"$number\":");const char*s=isnan(d)?"NaN":(d<0?"-Infinity":"Infinity");sb_json_string(b,s,strlen(s));sb_ch(b,'}');}else{char q[64];snprintf(q,sizeof q,"%.17g",d);sb_s(b,q);}return;}
    if(JS_IsString(v)){size_t n=0;const char*s=JS_ToCStringLen(ctx,&n,v);sb_json_string(b,s?s:"",s?n:0);if(s)JS_FreeCString(ctx,s);return;}
    if(JS_IsFunction(ctx,v)){
        JSValue mark=JS_GetPropertyStr(ctx,v,"__mlbWasmRemoteFunctionId");if(!JS_IsUndefined(mark)){int32_t id=0;JS_ToInt32(ctx,&id,mark);JS_FreeValue(ctx,mark);sb_s(b,"{\"$function\":");sb_i64(b,id);sb_ch(b,'}');return;}JS_FreeValue(ctx,mark);
        mark=JS_GetPropertyStr(ctx,v,"__mlbWasmExportRef");if(!JS_IsUndefined(mark)&&JS_IsObject(mark)){int iid=0;char name[512];get_int_field(ctx,mark,"instance",&iid);get_string_field(ctx,mark,"name",name,sizeof name);sb_s(b,"{\"$export\":{\"instance\":");sb_i64(b,iid);sb_s(b,",\"name\":");sb_json_string(b,name,strlen(name));sb_s(b,"}}");JS_FreeValue(ctx,mark);return;}JS_FreeValue(ctx,mark);
        int cb=host_add_callback(ctx,h,v);if(cb>0){sb_s(b,"{\"$callback\":");sb_i64(b,cb);sb_ch(b,'}');}else sb_s(b,"null");return;
    }
    if(JS_IsObject(v)){
        int id=host_object_handle(ctx,h,v);
        if(id<0){sb_s(b,"null");return;}
        sb_s(b,"{\"$jsObject\":");sb_i64(b,id);sb_ch(b,'}');return;
    }
    JSValue str=JS_JSONStringify(ctx,v,JS_UNDEFINED,JS_UNDEFINED);if(JS_IsException(str)){JS_FreeValue(ctx,JS_GetException(ctx));sb_s(b,"null");return;}const char*s=JS_ToCString(ctx,str);sb_s(b,s?s:"null");if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,str);
}

static JSClassDef wasm_module_class={"WebAssembly.Module",NULL,NULL,NULL,NULL};
static JSClassDef wasm_instance_class={"WebAssembly.Instance",NULL,NULL,NULL,NULL};
static void wasm_module_gc_mark(JSRuntime*rt,JSValueConst v,JS_MarkFunc*mark){WasmModule*m=JS_GetOpaque(v,wasm_module_cid);if(m){JS_MarkValue(rt,m->imports,mark);JS_MarkValue(rt,m->exports,mark);}}
static void wasm_module_finalizer(JSRuntime*rt,JSValue v){WasmModule*m=JS_GetOpaque(v,wasm_module_cid);if(m){if(m->native)mlb_wasm_rt_module_free(m->native);else wasm_queue_release_rt(rt,"releaseModule",m->id);JS_FreeValueRT(rt,m->imports);JS_FreeValueRT(rt,m->exports);free(m);}}
static void wasm_instance_gc_mark(JSRuntime*rt,JSValueConst v,JS_MarkFunc*mark){WasmInstance*i=JS_GetOpaque(v,wasm_instance_cid);if(i){JS_MarkValue(rt,i->module_obj,mark);JS_MarkValue(rt,i->imports_obj,mark);JS_MarkValue(rt,i->native_exception,mark);JS_MarkValue(rt,i->exports_obj,mark);for(int n=0;n<i->native_import_count;n++)JS_MarkValue(rt,i->native_import_values[n],mark);for(uint32_t n=0;n<i->native_function_capacity;n++)JS_MarkValue(rt,i->native_functions[n],mark);for(uint32_t n=0;n<i->native_externref_count;n++)JS_MarkValue(rt,i->native_externref_values[n],mark);}}
static void wasm_instance_registry_register(JSContext*ctx,WasmInstance*i){if(!i||i->registry_linked)return;i->runtime=JS_GetRuntime(ctx);i->registry_next=g_wasm_instances;g_wasm_instances=i;i->registry_linked=1;}
static void wasm_instance_registry_unregister(WasmInstance*i){if(!i||!i->registry_linked)return;WasmInstance**p=&g_wasm_instances;while(*p){if(*p==i){*p=i->registry_next;break;}p=&(*p)->registry_next;}i->registry_next=NULL;i->registry_linked=0;}
static void wasm_instance_finalizer(JSRuntime*rt,JSValue v){WasmInstance*i=JS_GetOpaque(v,wasm_instance_cid);if(i){wasm_instance_registry_unregister(i);if(i->native)mlb_wasm_rt_instance_free(i->native);else wasm_queue_release_rt(rt,"releaseInstance",i->id);for(int k=0;k<i->callback_count;k++)host_remove_callback_rt(rt,i->callback_ids[k]);JS_FreeValueRT(rt,i->module_obj);JS_FreeValueRT(rt,i->imports_obj);JS_FreeValueRT(rt,i->native_exception);JS_FreeValueRT(rt,i->exports_obj);for(int n=0;n<i->native_import_count;n++)JS_FreeValueRT(rt,i->native_import_values[n]);for(uint32_t n=0;n<i->native_function_capacity;n++)JS_FreeValueRT(rt,i->native_functions[n]);for(uint32_t n=0;n<i->native_externref_count;n++)JS_FreeValueRT(rt,i->native_externref_values[n]);free(i->native_externref_values);free(i->native_externref_handles);free(i->native_functions);free(i->native_import_values);free(i->callback_ids);free(i);WasmHost*h=host_for_rt(rt);if(h)wasm_native_externref_cleanup(h->ctx,h);}}
static void wasm_ref_gc_mark(JSRuntime*rt,JSValueConst v,JS_MarkFunc*mark){WasmRef*r=NULL;JSClassID cid=JS_GetClassID((JSValue)v);if(cid==wasm_memory_cid||cid==wasm_global_cid||cid==wasm_table_cid)r=JS_GetOpaque(v,cid);if(r){JS_MarkValue(rt,r->owner_obj,mark);JS_MarkValue(rt,r->buffer,mark);JS_MarkValue(rt,r->buffer_baseline,mark);for(uint32_t n=0;n<r->table_length&&r->table_values;n++)JS_MarkValue(rt,r->table_values[n],mark);if(r->table_root_store)for(uint32_t n=0;n<r->table_root_store->length&&r->table_root_store->values;n++)JS_MarkValue(rt,r->table_root_store->values[n],mark);for(uint32_t n=0;n<r->externref_length;n++)JS_MarkValue(rt,r->externref_values[n],mark);}}
static void wasm_resource_ref_register(JSContext*ctx,WasmRef*r){if(!r||r->resource_registry_linked)return;r->runtime=JS_GetRuntime(ctx);r->resource_registry_next=g_wasm_resource_refs;g_wasm_resource_refs=r;r->resource_registry_linked=1;}
static void wasm_resource_ref_unregister(WasmRef*r){if(!r||!r->resource_registry_linked)return;WasmRef**p=&g_wasm_resource_refs;while(*p){if(*p==r){*p=r->resource_registry_next;break;}p=&(*p)->resource_registry_next;}r->resource_registry_next=NULL;r->resource_registry_linked=0;}
static void wasm_memory_ref_register(JSContext*ctx,WasmRef*r){if(!r||!r->native_memory||r->memory_registry_linked)return;r->runtime=JS_GetRuntime(ctx);r->memory_registry_next=g_wasm_memory_refs;g_wasm_memory_refs=r;r->memory_registry_linked=1;}
static void wasm_memory_ref_unregister(WasmRef*r){if(!r||!r->memory_registry_linked)return;WasmRef**p=&g_wasm_memory_refs;while(*p){if(*p==r){*p=r->memory_registry_next;break;}p=&(*p)->memory_registry_next;}r->memory_registry_next=NULL;r->memory_registry_linked=0;}
static void wasm_memory_arraybuffer_free(JSRuntime*rt,void*opaque,void*ptr){(void)rt;/* The QuickJS detach path releases data once; finalization later calls again with ptr=NULL. */if(ptr)mlb_wasm_rt_memory_release((MlbWasmMemory*)opaque);}
static void wasm_ref_finalizer(JSRuntime*rt,JSValue v){WasmRef*r=NULL;JSClassID cid=JS_GetClassID(v);if(cid==wasm_memory_cid||cid==wasm_global_cid||cid==wasm_table_cid)r=JS_GetOpaque(v,cid);if(r){wasm_memory_ref_unregister(r);wasm_resource_ref_unregister(r);if(r->standalone&&!r->native_memory&&!r->native_global&&!r->native_table){JSClassID qcid=JS_GetClassID(v);wasm_queue_release_rt(rt,qcid==wasm_memory_cid?"releaseMemory":qcid==wasm_global_cid?"releaseGlobal":"releaseTable",r->id);}if(r->table_root_store)wasm_table_roots_release_rt(rt,r->table_root_store);if(r->native_memory)mlb_wasm_rt_memory_release(r->native_memory);if(r->native_global)mlb_wasm_rt_global_release(r->native_global);if(r->native_table)mlb_wasm_rt_table_release(r->native_table);for(uint32_t n=0;n<r->table_length&&r->table_values;n++)JS_FreeValueRT(rt,r->table_values[n]);free(r->table_values);for(uint32_t n=0;n<r->externref_length;n++)JS_FreeValueRT(rt,r->externref_values[n]);free(r->externref_values);free(r->externref_handles);JS_FreeValueRT(rt,r->owner_obj);JS_FreeValueRT(rt,r->buffer);JS_FreeValueRT(rt,r->buffer_baseline);free(r->name);free(r);WasmHost*h=host_for_rt(rt);if(h)wasm_native_externref_cleanup(h->ctx,h);}}
static JSClassDef wasm_memory_class={"WebAssembly.Memory",wasm_ref_finalizer,wasm_ref_gc_mark,NULL,NULL};
static JSClassDef wasm_global_class={"WebAssembly.Global",wasm_ref_finalizer,wasm_ref_gc_mark,NULL,NULL};
static JSClassDef wasm_table_class={"WebAssembly.Table",wasm_ref_finalizer,wasm_ref_gc_mark,NULL,NULL};

static WasmModule *get_module(JSContext*ctx,JSValueConst v){(void)ctx;return JS_GetOpaque(v,wasm_module_cid);}
static WasmInstance *get_instance(JSContext*ctx,JSValueConst v){return JS_GetOpaque(v,wasm_instance_cid);}
static WasmRef *get_ref(JSContext*ctx,JSValueConst v,JSClassID id){return JS_GetOpaque(v,id);}
static JSValue make_module_object(JSContext*ctx,WasmHost*h,int id,JSValue imports,JSValue exports){(void)h;JSValue o=JS_NewObjectClass(ctx,wasm_module_cid);WasmModule*m=calloc(1,sizeof *m);if(!m){JS_FreeValue(ctx,o);JS_FreeValue(ctx,imports);JS_FreeValue(ctx,exports);return JS_ThrowOutOfMemory(ctx);}m->id=id;m->imports=imports;m->exports=exports;m->native=NULL;JS_SetOpaque(o,m);return o;}
static const char *native_export_kind_name(int kind){switch(kind){case MLB_WASM_FUNC:return "function";case MLB_WASM_TABLE:return "table";case MLB_WASM_MEMORY:return "memory";case MLB_WASM_GLOBAL:return "global";default:return "unknown";}}
static JSValue native_export_descriptors(JSContext*ctx,const MlbWasmModule*nm){JSValue out=JS_NewArray(ctx);size_t n=mlb_wasm_rt_export_count(nm);for(size_t j=0;j<n;j++){const char*name=NULL;int kind=0;uint32_t index=0;if(!mlb_wasm_rt_export_info(nm,j,&name,&kind,&index)){JS_FreeValue(ctx,out);return JS_ThrowInternalError(ctx,"Invalid native WebAssembly export table");}JSValue d=JS_NewObject(ctx);JS_SetPropertyStr(ctx,d,"name",JS_NewString(ctx,name?name:""));JS_SetPropertyStr(ctx,d,"kind",JS_NewString(ctx,native_export_kind_name(kind)));JS_SetPropertyUint32(ctx,out,(uint32_t)j,d);}return out;}
static JSValue native_import_descriptors(JSContext*ctx,const MlbWasmModule*nm){JSValue out=JS_NewArray(ctx);size_t n=mlb_wasm_rt_import_count(nm);for(size_t j=0;j<n;j++){const char*mod=NULL,*name=NULL;int kind=0;uint32_t ty=0;if(!mlb_wasm_rt_import_info(nm,j,&mod,&name,&kind,&ty)){JS_FreeValue(ctx,out);return JS_ThrowInternalError(ctx,"Invalid native WebAssembly import table");}JSValue d=JS_NewObject(ctx);JS_SetPropertyStr(ctx,d,"module",JS_NewString(ctx,mod?mod:""));JS_SetPropertyStr(ctx,d,"name",JS_NewString(ctx,name?name:""));JS_SetPropertyStr(ctx,d,"kind",JS_NewString(ctx,native_export_kind_name(kind)));JS_SetPropertyUint32(ctx,out,(uint32_t)j,d);}return out;}
static JSValue make_instance_object(JSContext*ctx,int id,JSValue module,JSValue imports,int *cbids,int cbcount){JSValue o=JS_NewObjectClass(ctx,wasm_instance_cid);WasmInstance*i=calloc(1,sizeof *i);if(!i){JS_FreeValue(ctx,o);JS_FreeValue(ctx,module);JS_FreeValue(ctx,imports);free(cbids);return JS_ThrowOutOfMemory(ctx);}i->id=id;i->module_obj=module;i->imports_obj=imports;i->callback_ids=cbids;i->callback_count=cbcount;i->ctx=ctx;i->native_exception=JS_UNDEFINED;i->exports_obj=JS_UNDEFINED;JS_SetOpaque(o,i);wasm_instance_registry_register(ctx,i);return o;}
static JSValue make_ref_object(JSContext*ctx,JSClassID cid,int standalone,int id,JSValueConst owner,int owner_iid,const char*name){WasmRef*r=calloc(1,sizeof *r);if(!r)return JS_ThrowOutOfMemory(ctx);r->standalone=standalone;r->id=id;r->owner_instance_id=owner_iid;r->name=name?strdup(name):NULL;r->owner_obj=JS_DupValue(ctx,owner);r->buffer=JS_UNDEFINED;r->buffer_baseline=JS_UNDEFINED;JSValue o=JS_NewObjectClass(ctx,cid);if(JS_IsException(o)){JS_FreeValue(ctx,r->owner_obj);free(r->name);free(r);return o;}JS_SetOpaque(o,r);if(cid==wasm_global_cid||cid==wasm_table_cid)wasm_resource_ref_register(ctx,r);return o;}

static int wasm_native_sync_values(JSContext *ctx, uint64_t *old_handles, JSValue *old_values,
                                   uint32_t old_count, const uint64_t *handles, uint32_t count,
                                   uint64_t **new_handles_out, JSValue **new_values_out) {
    uint64_t *new_handles = count ? malloc((size_t)count * sizeof(*new_handles)) : NULL;
    JSValue *new_values = count ? malloc((size_t)count * sizeof(*new_values)) : NULL;
    if (count && (!new_handles || !new_values)) { free(new_handles); free(new_values); return 0; }
    for (uint32_t n = 0; n < count; ++n) new_values[n] = JS_UNDEFINED;
    for (uint32_t n = 0; n < count; ++n) {
        new_handles[n] = handles[n];
        if (!handles[n]) { new_values[n] = JS_NULL; continue; }
        /* Reuse a current strong root when this slot already refers to the same
         * opaque handle. Multiple slots deliberately receive duplicated roots. */
        int found = 0;
        for (uint32_t k = 0; k < old_count; ++k) if (old_handles && old_values && old_handles[k] == handles[n]) {
            new_values[n] = JS_DupValue(ctx, old_values[k]); found = 1; break;
        }
        if (!found) {
            new_values[n] = native_externref_to_js(ctx, handles[n]);
            if (JS_IsException(new_values[n])) {
                for (uint32_t k = 0; k <= n; ++k) JS_FreeValue(ctx, new_values[k]);
                free(new_handles); free(new_values); return 0;
            }
        }
    }
    *new_handles_out = new_handles; *new_values_out = new_values;
    return 1;
}
static int wasm_native_instance_sync_externrefs(JSContext *ctx, WasmInstance *instance) {
    if (!instance || !instance->native) return 1;
    size_t count = mlb_wasm_rt_instance_externref_count(instance->native);
    if (count > UINT32_MAX) return 0;
    uint32_t ncount = (uint32_t)count;
    uint64_t *handles = ncount ? malloc((size_t)ncount * sizeof(*handles)) : NULL;
    if (ncount && !handles) return 0;
    for (uint32_t n = 0; n < ncount; ++n) if (!mlb_wasm_rt_instance_externref_at(instance->native, n, &handles[n])) { free(handles); return 0; }
    uint64_t *new_handles = NULL; JSValue *new_values = NULL;
    int ok = wasm_native_sync_values(ctx, instance->native_externref_handles, instance->native_externref_values,
                                     instance->native_externref_count, handles, ncount, &new_handles, &new_values);
    free(handles); if (!ok) return 0;
    for (uint32_t n = 0; n < instance->native_externref_count; ++n) JS_FreeValue(ctx, instance->native_externref_values[n]);
    free(instance->native_externref_handles); free(instance->native_externref_values);
    instance->native_externref_handles = new_handles; instance->native_externref_values = new_values;
    instance->native_externref_count = ncount;
    return 1;
}
static int wasm_native_resource_ref_sync_externrefs(JSContext *ctx, WasmRef *ref) {
    if (!ref) return 1;
    uint64_t one = 0; uint32_t count = 0;
    if (ref->native_global && ref->value_type == MLB_WASM_EXTERNREF) {
        MlbWasmValue value;
        if (!mlb_wasm_rt_global_resource_get(ref->native_global, &value) || value.type != MLB_WASM_EXTERNREF) return 0;
        one = value.bits; count = 1;
    } else if (ref->native_table && ref->value_type == 1) {
        if (!mlb_wasm_rt_table_resource_size(ref->native_table, &count)) return 0;
    } else return 1;
    uint64_t *handles = count ? malloc((size_t)count * sizeof(*handles)) : NULL;
    if (count && !handles) return 0;
    if (ref->native_global) handles[0] = one;
    else for (uint32_t n = 0; n < count; ++n) if (!mlb_wasm_rt_table_resource_get_externref(ref->native_table, n, &handles[n])) { free(handles); return 0; }
    uint64_t *new_handles = NULL; JSValue *new_values = NULL;
    int ok = wasm_native_sync_values(ctx, ref->externref_handles, ref->externref_values,
                                     ref->externref_length, handles, count, &new_handles, &new_values);
    free(handles); if (!ok) return 0;
    for (uint32_t n = 0; n < ref->externref_length; ++n) JS_FreeValue(ctx, ref->externref_values[n]);
    free(ref->externref_handles); free(ref->externref_values);
    ref->externref_handles = new_handles; ref->externref_values = new_values; ref->externref_length = count;
    return 1;
}
static int wasm_native_externref_sync_all(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    for (WasmInstance *i = g_wasm_instances; i; i = i->registry_next)
        if (i->runtime == rt && i->native && !wasm_native_instance_sync_externrefs(ctx, i)) return 0;
    for (WasmRef *r = g_wasm_resource_refs; r; r = r->resource_registry_next)
        if (r->runtime == rt && !wasm_native_resource_ref_sync_externrefs(ctx, r)) return 0;
    return 1;
}
static void wasm_native_externref_clear_temporaries(WasmHost *h) { native_externref_cleanup_temporaries(h); }

static int wasm_native_externref_commit(JSContext *ctx) {
    int ok = wasm_native_externref_sync_all(ctx);
    WasmHost *h = host_for_ctx(ctx, 0);
    if (h && !h->native_call_depth) {
        wasm_native_externref_clear_temporaries(h);
        wasm_native_externref_cleanup(ctx, h);
    }
    return ok;
}

static JSValue make_exports(JSContext*ctx,JSValueConst instance_obj,JSValueConst descs,int iid){JSValue ex=JS_NewObject(ctx);WasmInstance*owner=get_instance(ctx,instance_obj);int is_native=owner&&owner->native;int n=array_len(ctx,descs);for(int j=0;j<n;j++){JSValue d=array_item(ctx,descs,j);char name[512],kind[32];get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);JSValue val=JS_UNDEFINED;
        if(!strcmp(kind,"function")){if(is_native){WasmModule*wm=get_module(ctx,owner->module_obj);int fk=0;uint32_t fi=0;if(wm&&mlb_wasm_rt_find_export(wm->native,name,&fk,&fi)&&fk==MLB_WASM_FUNC)val=make_native_table_function(ctx,instance_obj,fi);else val=JS_ThrowInternalError(ctx,"Native WebAssembly function export is unavailable");}else{JSValue data[2]={JS_DupValue(ctx,instance_obj),JS_NewString(ctx,name)};val=JS_NewCFunctionData(ctx,wasm_export_function_call,0,0,2,(JSValueConst*)data);JS_FreeValue(ctx,data[0]);JS_FreeValue(ctx,data[1]);JSValue marker=JS_NewObject(ctx);JS_SetPropertyStr(ctx,marker,"instance",JS_NewInt32(ctx,iid));JS_SetPropertyStr(ctx,marker,"name",JS_NewString(ctx,name));JS_DefinePropertyValueStr(ctx,val,"__mlbWasmExportRef",marker,JS_PROP_CONFIGURABLE);}}
        else if(!strcmp(kind,"memory")){val=make_ref_object(ctx,wasm_memory_cid,0,0,instance_obj,iid,name);if(is_native&&!JS_IsException(val)){WasmRef*mr=JS_GetOpaque(val,wasm_memory_cid);mr->native_memory=mlb_wasm_rt_memory_handle(owner->native);wasm_memory_ref_register(ctx,mr);}}
        else if(!strcmp(kind,"global")){val=make_ref_object(ctx,wasm_global_cid,0,0,instance_obj,iid,name);if(is_native&&!JS_IsException(val)){WasmModule*wm=get_module(ctx,owner->module_obj);int ek=0,mut=0;uint32_t gi=0;uint8_t gt=0;if(wm&&mlb_wasm_rt_find_export(wm->native,name,&ek,&gi)&&ek==MLB_WASM_GLOBAL&&mlb_wasm_rt_global_info(wm->native,gi,&gt,&mut)){WasmRef*gr=JS_GetOpaque(val,wasm_global_cid);gr->value_type=gt;gr->mutable_value=mut;gr->native_global=mlb_wasm_rt_global_handle(owner->native,gi);}}}
        else if(!strcmp(kind,"table")){val=make_ref_object(ctx,wasm_table_cid,0,0,instance_obj,iid,name);if(is_native&&!JS_IsException(val)){WasmRef*tr=JS_GetOpaque(val,wasm_table_cid);tr->native_table=mlb_wasm_rt_table_handle(owner->native);uint8_t elem_type=MLB_WASM_FUNCREF;if(tr->native_table)mlb_wasm_rt_table_element_type(tr->native_table,&elem_type);tr->value_type=elem_type==MLB_WASM_EXTERNREF?1:0;if(tr->native_table&&tr->value_type==0){tr->table_root_store=wasm_table_roots_attach(ctx,tr->native_table);if(!tr->table_root_store){JS_FreeValue(ctx,val);val=JS_ThrowOutOfMemory(ctx);}else wasm_table_root_new_instance_functions(ctx,tr,instance_obj,owner->native);}}}
        else val=JS_UNDEFINED;
        if(!JS_IsException(val))JS_SetPropertyStr(ctx,ex,name,val);else JS_FreeValue(ctx,val);JS_FreeValue(ctx,d);
    }return ex;}

static JSValue wasm_module_ctor(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){
    (void)this_val;
    if(argc<1)return JS_ThrowTypeError(ctx,"WebAssembly.Module requires a BufferSource");
    const uint8_t*data=NULL;size_t len=0;JSValue keep=JS_UNDEFINED;
    if(!bytesource(ctx,argv[0],&data,&len,&keep))return wasm_named_error(ctx,"TypeError","WebAssembly.Module argument must be an ArrayBuffer or typed array");
    if(len>MLB_WASM_MAX_RPC_BYTES/2){JS_FreeValue(ctx,keep);return wasm_named_error(ctx,"RangeError","WebAssembly module is too large for the configured runtime limit");}
    char preflight_error[192];
    if(!mlb_wasm_binary_preflight(data,len,preflight_error,sizeof preflight_error)){JS_FreeValue(ctx,keep);return wasm_named_error(ctx,"CompileError",preflight_error);}
    /* Prefer the in-process C interpreter when the binary validates against its
       supported instruction/import subset; unsupported modules use the compatibility fallback. */
    char native_error[192];MlbWasmModule*native=mlb_wasm_rt_module_new(data,len,native_error,sizeof native_error);
    /* Native-supported resource imports are connected through retained handles.
       Memory/Global and funcref Table imports can share native resources; modules
       that use unsupported instructions or reference-table element types still
       fall back to the compatibility helper when available. */
    if(native){
        WasmHost*h=host_for_ctx(ctx,1);if(!h){mlb_wasm_rt_module_free(native);JS_FreeValue(ctx,keep);return JS_ThrowOutOfMemory(ctx);}
        JSValue imports=native_import_descriptors(ctx,native),exports=native_export_descriptors(ctx,native);JS_FreeValue(ctx,keep);
        if(JS_IsException(imports)||JS_IsException(exports)){mlb_wasm_rt_module_free(native);JS_FreeValue(ctx,imports);JS_FreeValue(ctx,exports);return JS_EXCEPTION;}
        JSValue obj=make_module_object(ctx,h,++h->next_module,imports,exports);
        if(JS_IsException(obj)){mlb_wasm_rt_module_free(native);return obj;}
        WasmModule*wm=JS_GetOpaque(obj,wasm_module_cid);wm->native=native;return obj;
    }
    WasmHost*h=host_for_ctx(ctx,1);if(!h){JS_FreeValue(ctx,keep);return JS_ThrowOutOfMemory(ctx);}
    char*b64=b64_encode(data,len);JS_FreeValue(ctx,keep);if(!b64)return JS_ThrowOutOfMemory(ctx);
    int id=++h->next_module;SBuf cmd={0};sb_s(&cmd,"{\"cmd\":\"compile\",\"id\":");sb_i64(&cmd,id);sb_s(&cmd,",\"bytes\":");sb_json_string(&cmd,b64,strlen(b64));sb_ch(&cmd,'}');free(b64);
    char err[768];int ok=0;JSValue result=rpc_do(ctx,h,cmd.s,err,sizeof err,&ok);free(cmd.s);
    if(!ok){const char*kind="CompileError";const char*msg=strip_error_class(err,&kind);return wasm_named_error(ctx,kind,msg);}
    JSValue imports=JS_GetPropertyStr(ctx,result,"imports"),exports=JS_GetPropertyStr(ctx,result,"exports");JS_FreeValue(ctx,result);return make_module_object(ctx,h,id,imports,exports);
}
static JSValue wasm_validate(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){(void)this_val;if(argc<1)return JS_NewBool(ctx,0);WasmHost*h=host_for_ctx(ctx,1);if(!h)return JS_NewBool(ctx,0);const uint8_t*data=NULL;size_t len=0;JSValue keep=JS_UNDEFINED;if(!bytesource(ctx,argv[0],&data,&len,&keep))return JS_NewBool(ctx,0);if(len>MLB_WASM_MAX_RPC_BYTES/2||!mlb_wasm_binary_preflight(data,len,NULL,0)){JS_FreeValue(ctx,keep);return JS_NewBool(ctx,0);}MlbWasmModule*nm=mlb_wasm_rt_module_new(data,len,NULL,0);if(nm){mlb_wasm_rt_module_free(nm);JS_FreeValue(ctx,keep);return JS_NewBool(ctx,1);}char*b64=b64_encode(data,len);JS_FreeValue(ctx,keep);if(!b64)return JS_NewBool(ctx,0);int id=++h->next_module;SBuf cmd={0};sb_s(&cmd,"{\"cmd\":\"compile\",\"id\":");sb_i64(&cmd,id);sb_s(&cmd,",\"bytes\":");sb_json_string(&cmd,b64,strlen(b64));sb_ch(&cmd,'}');free(b64);char err[768];int ok=0;JSValue r=rpc_do(ctx,h,cmd.s,err,sizeof err,&ok);free(cmd.s);JS_FreeValue(ctx,r);return JS_NewBool(ctx,ok);}

static int add_inst_callback(WasmInstance *in,int cb){if(in->callback_count==in->callback_cap){int cap=in->callback_cap?in->callback_cap*2:8;int*p=realloc(in->callback_ids,(size_t)cap*sizeof(int));if(!p)return 0;in->callback_ids=p;in->callback_cap=cap;}in->callback_ids[in->callback_count++]=cb;return 1;}
static int add_json_ref_import(JSContext*ctx,SBuf*cmd,const char*module,const char*name,const char*kind,JSValueConst v,JSValueConst owner_obj,int owner_iid) {
    JSClassID cid=!strcmp(kind,"memory")?wasm_memory_cid:!strcmp(kind,"global")?wasm_global_cid:wasm_table_cid;WasmRef*r=JS_GetOpaque(v,cid);if(!r){return 0;}
    sb_s(cmd,"{\"module\":");sb_json_string(cmd,module,strlen(module));sb_s(cmd,",\"name\":");sb_json_string(cmd,name,strlen(name));sb_s(cmd,",\"kind\":");sb_json_string(cmd,kind,strlen(kind));sb_s(cmd,",\"ref\":");
    if(!r->standalone && r->owner_instance_id==owner_iid && owner_iid>0){/* legal self-reference still resolves through the owner instance */}
    ref_make_remote(r,cmd);sb_ch(cmd,'}');return 1;
}
static JSValue wasm_instance_from_module(JSContext*ctx,JSValueConst module_obj,JSValueConst imports_obj){
    WasmModule*m=get_module(ctx,module_obj);if(!m)return wasm_named_error(ctx,"TypeError","WebAssembly.Instance requires a WebAssembly.Module");
    if(m->native){
        WasmHost*nh=host_for_ctx(ctx,1);if(!nh)return JS_ThrowOutOfMemory(ctx);
        int nin=array_len(ctx,m->imports),nfunc=0;
        for(int j=0;j<nin;j++){JSValue d=array_item(ctx,m->imports,(uint32_t)j);char kind[32];get_string_field(ctx,d,"kind",kind,sizeof kind);if(!strcmp(kind,"function"))nfunc++;JS_FreeValue(ctx,d);}
        JSValue*iv=calloc((size_t)(nfunc?nfunc:1),sizeof(JSValue));
        MlbWasmImportValue*rb=calloc((size_t)(nin?nin:1),sizeof(*rb));
        if(!iv||!rb){free(iv);free(rb);return JS_ThrowOutOfMemory(ctx);}
        int fi=0,nrb=0;char link_error[256]="Missing or incompatible WebAssembly import";
        for(int j=0;j<nin;j++){
            JSValue d=array_item(ctx,m->imports,(uint32_t)j);char mod[512],name[512],kind[32];
            get_string_field(ctx,d,"module",mod,sizeof mod);get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);
            JSValue ns=JS_IsObject(imports_obj)?JS_GetPropertyStr(ctx,imports_obj,mod):JS_UNDEFINED;
            JSValue val=JS_IsObject(ns)?JS_GetPropertyStr(ctx,ns,name):JS_UNDEFINED;
            if(JS_IsException(ns)){JS_FreeValue(ctx,JS_GetException(ctx));ns=JS_UNDEFINED;}
            if(JS_IsException(val)){JS_FreeValue(ctx,JS_GetException(ctx));val=JS_UNDEFINED;}
            int good=0;const char*imod=NULL,*iname=NULL;int ikind=0;
            mlb_wasm_rt_import_info(m->native,(size_t)j,&imod,&iname,&ikind,NULL);
            if(!strcmp(kind,"function")){
                if(!JS_IsUndefined(val)&&JS_IsFunction(ctx,val)){iv[fi++]=JS_DupValue(ctx,val);good=1;}
                else snprintf(link_error,sizeof link_error,"Missing or incompatible WebAssembly function import");
            }else if(!strcmp(kind,"memory")){
                WasmRef*r=JS_GetOpaque(val,wasm_memory_cid);
                if(r&&r->native_memory){rb[nrb++]=(MlbWasmImportValue){imod,iname,MLB_WASM_MEMORY,{.memory=r->native_memory}};good=1;}
                else snprintf(link_error,sizeof link_error,"WebAssembly.Memory import is not backed by a native resource");
            }else if(!strcmp(kind,"global")){
                WasmRef*r=JS_GetOpaque(val,wasm_global_cid);
                if(r&&r->native_global){rb[nrb++]=(MlbWasmImportValue){imod,iname,MLB_WASM_GLOBAL,{.global=r->native_global}};good=1;}
                else snprintf(link_error,sizeof link_error,"WebAssembly.Global import is not backed by a native resource");
            }else if(!strcmp(kind,"table")){
                WasmRef*r=JS_GetOpaque(val,wasm_table_cid);
                if(r&&r->native_table){rb[nrb++]=(MlbWasmImportValue){imod,iname,MLB_WASM_TABLE,{.table=r->native_table}};good=1;}
                else snprintf(link_error,sizeof link_error,"WebAssembly.Table import is not backed by a native resource");
            }
            if(!good){int missing=JS_IsUndefined(val);for(int k=0;k<fi;k++)JS_FreeValue(ctx,iv[k]);free(iv);free(rb);JS_FreeValue(ctx,d);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,val);return wasm_named_error(ctx,"LinkError",missing?"Missing WebAssembly import":link_error);}
            JS_FreeValue(ctx,d);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,val);
        }
        /* Native imported memories are direct views; remote fallback memories still flush JS writes before instantiation/start. */
        for(int j=0;j<nin;j++){
            JSValue d=array_item(ctx,m->imports,(uint32_t)j);char mod[512],name[512],kind[32];
            get_string_field(ctx,d,"module",mod,sizeof mod);get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);
            if(!strcmp(kind,"memory")){JSValue ns=JS_IsObject(imports_obj)?JS_GetPropertyStr(ctx,imports_obj,mod):JS_UNDEFINED;JSValue val=JS_IsObject(ns)?JS_GetPropertyStr(ctx,ns,name):JS_UNDEFINED;WasmRef*r=JS_GetOpaque(val,wasm_memory_cid);if(r&&!memory_sync(ctx,NULL,r,0)){JS_FreeValue(ctx,val);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,d);for(int k=0;k<fi;k++)JS_FreeValue(ctx,iv[k]);free(iv);free(rb);return wasm_named_error(ctx,"RuntimeError","Unable to synchronize imported native memory");}JS_FreeValue(ctx,val);JS_FreeValue(ctx,ns);}
            JS_FreeValue(ctx,d);
        }
        JSValue modref=JS_DupValue(ctx,module_obj);JSValue imports=JS_IsUndefined(imports_obj)?JS_NewObject(ctx):JS_DupValue(ctx,imports_obj);
        JSValue instobj=make_instance_object(ctx,++nh->next_instance,modref,imports,NULL,0);
        if(JS_IsException(instobj)){for(int j=0;j<fi;j++)JS_FreeValue(ctx,iv[j]);free(iv);free(rb);return instobj;}
        WasmInstance*niw=JS_GetOpaque(instobj,wasm_instance_cid);niw->native_import_values=iv;niw->native_import_count=fi;
        nh->native_call_depth++;
        char err[256];MlbWasmInstance*ni=mlb_wasm_rt_instance_new_with_imports_multi(m->native,NULL,wasm_native_host_call_multi,niw,rb,(size_t)nrb,err,sizeof err);free(rb);
        wasm_memory_detach_stale_buffers(ctx);
        if(!ni){JSValue failure=JS_UNDEFINED;int had_exception=!JS_IsUndefined(niw->native_exception);if(had_exception){failure=niw->native_exception;niw->native_exception=JS_UNDEFINED;}JS_FreeValue(ctx,instobj);if(nh->native_call_depth)nh->native_call_depth--;if(!nh->native_call_depth){wasm_native_externref_clear_temporaries(nh);wasm_native_externref_cleanup(ctx,nh);}if(had_exception)return JS_Throw(ctx,failure);if(!strncmp(err,"start function trapped:",23)){const char*detail=err+23;while(*detail==' ')detail++;return wasm_named_error(ctx,"RuntimeError",*detail?detail:"start function trapped");}return wasm_named_error(ctx,"LinkError",err[0]?err:"Unable to instantiate native WebAssembly module");}
        niw->native=ni;
        /* Active element segments can insert functions from this instance into an imported shared table.
           Keep a JS root for those functions so their owner instance survives as long as the table slot. */
        for(int j=0;j<nin;j++){JSValue d=array_item(ctx,m->imports,(uint32_t)j);char kind[32],mod[512],name[512];get_string_field(ctx,d,"kind",kind,sizeof kind);if(!strcmp(kind,"table")){get_string_field(ctx,d,"module",mod,sizeof mod);get_string_field(ctx,d,"name",name,sizeof name);JSValue ns=JS_IsObject(imports_obj)?JS_GetPropertyStr(ctx,imports_obj,mod):JS_UNDEFINED;JSValue val=JS_IsObject(ns)?JS_GetPropertyStr(ctx,ns,name):JS_UNDEFINED;WasmRef*tr=JS_GetOpaque(val,wasm_table_cid);if(tr)wasm_table_root_new_instance_functions(ctx,tr,instobj,ni);JS_FreeValue(ctx,val);JS_FreeValue(ctx,ns);}JS_FreeValue(ctx,d);}
        JSValue descs=JS_DupValue(ctx,m->exports);JSValue exp=make_exports(ctx,instobj,descs,niw->id);JS_FreeValue(ctx,descs);niw->exports_obj=JS_DupValue(ctx,exp);JS_SetPropertyStr(ctx,instobj,"exports",exp);
        /* Handle buffer invalidation if a start function grew imported memory. */
        for(int j=0;j<nin;j++){
            JSValue d=array_item(ctx,m->imports,(uint32_t)j);char mod[512],name[512],kind[32];
            get_string_field(ctx,d,"module",mod,sizeof mod);get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);
            if(!strcmp(kind,"memory")){JSValue ns=JS_IsObject(imports_obj)?JS_GetPropertyStr(ctx,imports_obj,mod):JS_UNDEFINED;JSValue val=JS_IsObject(ns)?JS_GetPropertyStr(ctx,ns,name):JS_UNDEFINED;WasmRef*r=JS_GetOpaque(val,wasm_memory_cid);if(r)memory_sync(ctx,NULL,r,1);JS_FreeValue(ctx,val);JS_FreeValue(ctx,ns);}
            JS_FreeValue(ctx,d);
        }
        int refs_ok=wasm_native_externref_sync_all(ctx);
        if(nh->native_call_depth)nh->native_call_depth--;
        if(!nh->native_call_depth){wasm_native_externref_clear_temporaries(nh);wasm_native_externref_cleanup(ctx,nh);}
        if(!refs_ok){JS_FreeValue(ctx,instobj);return JS_ThrowOutOfMemory(ctx);}
        return instobj;
    }
    WasmHost*h=host_for_ctx(ctx,1);if(!h)return JS_ThrowOutOfMemory(ctx);int iid=++h->next_instance;SBuf cmd={0};sb_s(&cmd,"{\"cmd\":\"instantiate\",\"id\":");sb_i64(&cmd,iid);sb_s(&cmd,",\"module\":");sb_i64(&cmd,m->id);sb_s(&cmd,",\"imports\":[");int n=array_len(ctx,m->imports),count=0;int *cbids=NULL;int cbcap=0;
    for(int j=0;j<n;j++){JSValue d=array_item(ctx,m->imports,j);char mod[512],name[512],kind[32];get_string_field(ctx,d,"module",mod,sizeof mod);get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);JSValue ns=JS_IsObject(imports_obj)?JS_GetPropertyStr(ctx,imports_obj,mod):JS_UNDEFINED;if(JS_IsException(ns)){JS_FreeValue(ctx,JS_GetException(ctx));ns=JS_UNDEFINED;}JSValue v=JS_IsObject(ns)?JS_GetPropertyStr(ctx,ns,name):JS_UNDEFINED;if(JS_IsException(v)){JS_FreeValue(ctx,JS_GetException(ctx));v=JS_UNDEFINED;}if(JS_IsUndefined(v)||JS_IsException(v)){JS_FreeValue(ctx,d);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,v);for(int z=0;z<count;z++)host_remove_callback(ctx,h,cbids[z]);free(cbids);free(cmd.s);return wasm_named_error(ctx,"LinkError","Missing WebAssembly import");}
        if(count)sb_ch(&cmd,',');int added=0;
        if(!strcmp(kind,"function")){
            if(!JS_IsFunction(ctx,v)){JS_FreeValue(ctx,d);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,v);for(int z=0;z<count;z++)host_remove_callback(ctx,h,cbids[z]);free(cbids);free(cmd.s);return wasm_named_error(ctx,"LinkError","WebAssembly function import is not callable");}
            int cb=host_add_callback(ctx,h,v);if(cb<0){JS_FreeValue(ctx,d);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,v);free(cbids);free(cmd.s);return JS_ThrowOutOfMemory(ctx);}if(count==cbcap){int nc=cbcap?cbcap*2:8;int*nb=realloc(cbids,(size_t)nc*sizeof(int));if(!nb){host_remove_callback(ctx,h,cb);JS_FreeValue(ctx,d);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,v);free(cbids);free(cmd.s);return JS_ThrowOutOfMemory(ctx);}cbids=nb;cbcap=nc;}cbids[count++]=cb;
            sb_s(&cmd,"{\"module\":");sb_json_string(&cmd,mod,strlen(mod));sb_s(&cmd,",\"name\":");sb_json_string(&cmd,name,strlen(name));sb_s(&cmd,",\"kind\":\"function\",\"callback\":");sb_i64(&cmd,cb);sb_ch(&cmd,'}');added=1;
        }else if(!strcmp(kind,"memory")||!strcmp(kind,"global")||!strcmp(kind,"table")){if(!strcmp(kind,"table")){WasmRef*tr=JS_GetOpaque(v,wasm_table_cid);if(tr)tr->remote_imported=1;}added=add_json_ref_import(ctx,&cmd,mod,name,kind,v,JS_UNDEFINED,0);}
        if(!added){JS_FreeValue(ctx,d);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,v);for(int z=0;z<count;z++)host_remove_callback(ctx,h,cbids[z]);free(cbids);free(cmd.s);return wasm_named_error(ctx,"LinkError","Unsupported or incompatible WebAssembly import value");}
        JS_FreeValue(ctx,d);JS_FreeValue(ctx,ns);JS_FreeValue(ctx,v);
    }
    sb_s(&cmd,"]}");char err[768];int ok=0;JSValue response=rpc_do(ctx,h,cmd.s,err,sizeof err,&ok);free(cmd.s);if(!ok){for(int z=0;z<count;z++)host_remove_callback(ctx,h,cbids[z]);free(cbids);const char*kind="LinkError";const char*msg=strip_error_class(err,&kind);return wasm_named_error(ctx,kind,msg);}JS_FreeValue(ctx,response);
    JSValue imports=JS_DupValue(ctx,imports_obj);JSValue modref=JS_DupValue(ctx,module_obj);JSValue instobj=make_instance_object(ctx,iid,modref,imports,cbids,count);if(JS_IsException(instobj))return instobj;WasmInstance*in=JS_GetOpaque(instobj,wasm_instance_cid);JSValue descs=JS_DupValue(ctx,m->exports);JSValue exports=make_exports(ctx,instobj,descs,iid);JS_FreeValue(ctx,descs);JS_SetPropertyStr(ctx,instobj,"exports",exports);return instobj;
}
static JSValue wasm_instance_ctor(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){(void)this_val;if(argc<1)return JS_ThrowTypeError(ctx,"WebAssembly.Instance requires a module");return wasm_instance_from_module(ctx,argv[0],argc>1?argv[1]:JS_UNDEFINED);}

static JSValue promise_with_result(JSContext*ctx,JSValue result,int reject){JSValue funcs[2];JSValue promise=JS_NewPromiseCapability(ctx,funcs);if(JS_IsException(promise)){JS_FreeValue(ctx,result);return promise;}JSValue r=JS_Call(ctx,funcs[reject?1:0],JS_UNDEFINED,1,(JSValueConst*)&result);JS_FreeValue(ctx,r);JS_FreeValue(ctx,result);JS_FreeValue(ctx,funcs[0]);JS_FreeValue(ctx,funcs[1]);return promise;}
static JSValue wasm_compile(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){(void)this_val;JSValue m=wasm_module_ctor(ctx,JS_UNDEFINED,argc,argv);if(JS_IsException(m)){JSValue e=JS_GetException(ctx);return promise_with_result(ctx,e,1);}return promise_with_result(ctx,m,0);}
static JSValue wasm_instantiate(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){(void)this_val;if(argc<1){JSValue e=wasm_named_error(ctx,"TypeError","WebAssembly.instantiate requires a module or BufferSource");e=JS_GetException(ctx);return promise_with_result(ctx,e,1);}JSValue module=JS_UNDEFINED;int bytes=0;if(get_module(ctx,argv[0]))module=JS_DupValue(ctx,argv[0]);else{module=wasm_module_ctor(ctx,JS_UNDEFINED,1,argv);bytes=1;}if(JS_IsException(module)){JSValue e=JS_GetException(ctx);return promise_with_result(ctx,e,1);}JSValue inst=wasm_instance_from_module(ctx,module,argc>1?argv[1]:JS_UNDEFINED);if(JS_IsException(inst)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,module);return promise_with_result(ctx,e,1);}JSValue out=inst;if(bytes){out=JS_NewObject(ctx);JS_SetPropertyStr(ctx,out,"module",JS_DupValue(ctx,module));JS_SetPropertyStr(ctx,out,"instance",JS_DupValue(ctx,inst));JS_FreeValue(ctx,inst);}JS_FreeValue(ctx,module);return promise_with_result(ctx,out,0);}
static JSValue clone_wasm_descriptors(JSContext*ctx,JSValueConst source,int imports){
    JSValue out=JS_NewArray(ctx);int n=array_len(ctx,source);
    for(int i=0;i<n;i++){
        JSValue src=array_item(ctx,source,i),dst=JS_NewObject(ctx);
        if(imports){JSValue v=JS_GetPropertyStr(ctx,src,"module");JS_SetPropertyStr(ctx,dst,"module",v);}
        JSValue name=JS_GetPropertyStr(ctx,src,"name"),kind=JS_GetPropertyStr(ctx,src,"kind");
        JS_SetPropertyStr(ctx,dst,"name",name);JS_SetPropertyStr(ctx,dst,"kind",kind);
        JS_SetPropertyUint32(ctx,out,(uint32_t)i,dst);JS_FreeValue(ctx,src);
    }
    return out;
}
static JSValue wasm_module_imports(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){(void)this_val;if(argc<1)return JS_ThrowTypeError(ctx,"Module.imports requires a Module");WasmModule*m=get_module(ctx,argv[0]);if(!m)return JS_ThrowTypeError(ctx,"argument is not a WebAssembly.Module");return clone_wasm_descriptors(ctx,m->imports,1);}
static JSValue wasm_module_exports(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){(void)this_val;if(argc<1)return JS_ThrowTypeError(ctx,"Module.exports requires a Module");WasmModule*m=get_module(ctx,argv[0]);if(!m)return JS_ThrowTypeError(ctx,"argument is not a WebAssembly.Module");return clone_wasm_descriptors(ctx,m->exports,0);}
static JSValue wasm_module_custom_sections(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){
    (void)this_val;if(argc<2)return JS_ThrowTypeError(ctx,"Module.customSections requires a Module and section name");WasmModule*m=get_module(ctx,argv[0]);if(!m)return JS_ThrowTypeError(ctx,"argument is not a WebAssembly.Module");size_t nn=0;const char*name=JS_ToCStringLen(ctx,&nn,argv[1]);if(!name)return JS_EXCEPTION;
    if(m->native){JSValue out=JS_NewArray(ctx);size_t count=mlb_wasm_rt_custom_section_count(m->native,name);for(size_t i=0;i<count;i++){uint8_t*buf=NULL;size_t len=0;if(!mlb_wasm_rt_custom_section_copy(m->native,name,i,&buf,&len)){JS_FreeCString(ctx,name);JS_FreeValue(ctx,out);return JS_ThrowOutOfMemory(ctx);}JS_SetPropertyUint32(ctx,out,(uint32_t)i,JS_NewArrayBufferCopy(ctx,buf,len));free(buf);}JS_FreeCString(ctx,name);return out;}
    WasmHost*h=host_for_ctx(ctx,1);SBuf b={0};sb_s(&b,"{\"cmd\":\"customSections\",\"module\":");sb_i64(&b,m->id);sb_s(&b,",\"name\":");sb_json_string(&b,name,nn);sb_ch(&b,'}');JS_FreeCString(ctx,name);char err[512];int ok=0;JSValue response=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);if(!ok){JS_FreeValue(ctx,response);return wasm_named_error(ctx,"RuntimeError",err);}JSValue sections=JS_GetPropertyStr(ctx,response,"sections");JS_FreeValue(ctx,response);int n=array_len(ctx,sections);JSValue out=JS_NewArray(ctx);for(int i=0;i<n;i++){JSValue bv=array_item(ctx,sections,i);const char*s=JS_ToCString(ctx,bv);size_t len=0;uint8_t*bytes=s?b64_decode(s,strlen(s),&len):NULL;if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,bv);if(!bytes){JS_FreeValue(ctx,sections);JS_FreeValue(ctx,out);return JS_ThrowOutOfMemory(ctx);}JS_SetPropertyUint32(ctx,out,(uint32_t)i,JS_NewArrayBufferCopy(ctx,bytes,len));free(bytes);}JS_FreeValue(ctx,sections);return out;
}

static JSValue rpc_ref(JSContext*ctx,WasmHost*h,const char*cmd,const WasmRef*r,char*err,size_t errcap,int*ok){SBuf b={0};sb_s(&b,"{\"cmd\":");sb_json_string(&b,cmd,strlen(cmd));sb_s(&b,",\"ref\":");ref_make_remote((WasmRef*)r,&b);sb_ch(&b,'}');JSValue v=rpc_do(ctx,h,b.s,err,errcap,ok);free(b.s);return v;}
static int memory_load_remote(JSContext*ctx,WasmHost*h,WasmRef*r,uint8_t**out,size_t*n,char*err,size_t cap){JSValue res=rpc_ref(ctx,h,"memoryRead",r,err,cap,NULL);if(JS_IsUndefined(res))return 0;JSValue d=JS_GetPropertyStr(ctx,res,"data");const char*s=JS_ToCString(ctx,d);size_t z=s?strlen(s):0;uint8_t*buf=s?b64_decode(s,z,n):NULL;if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,d);JS_FreeValue(ctx,res);if(!buf){snprintf(err,cap,"Invalid or oversized WebAssembly memory response");return 0;}if(*n>MLB_WASM_MAX_MEMORY_BYTES){free(buf);snprintf(err,cap,"WebAssembly memory exceeds the bridge buffer limit");return 0;}*out=buf;return 1;}
static void memory_forget_buffer(JSContext*ctx,WasmRef*r){if(!r)return;if(!JS_IsUndefined(r->buffer)&&!JS_IsNull(r->buffer)){JS_DetachArrayBuffer(ctx,r->buffer);JS_FreeValue(ctx,r->buffer);}if(!JS_IsUndefined(r->buffer_baseline)&&!JS_IsNull(r->buffer_baseline)){JS_DetachArrayBuffer(ctx,r->buffer_baseline);JS_FreeValue(ctx,r->buffer_baseline);}r->buffer=JS_UNDEFINED;r->buffer_baseline=JS_UNDEFINED;r->buffer_len=0;r->memory_epoch=0;}
static int memory_sync(JSContext*ctx,WasmHost*h,WasmRef*r,int refresh){
    (void)refresh;
    if(!r||JS_IsUndefined(r->buffer)||JS_IsNull(r->buffer))return 1;
    if(r->native_memory){
        size_t n=0,buffer_n=0;uint64_t epoch=0;
        uint8_t*data=mlb_wasm_rt_memory_resource_data(r->native_memory,&n,NULL,&epoch);
        if(!data)return 0;
        /* Native buffers are live external ArrayBuffers. No snapshot merge or
           copy is required; stale epochs must be detached before they are used. */
        if(n!=r->buffer_len||epoch!=r->memory_epoch){memory_forget_buffer(ctx,r);return 1;}
        uint8_t*buffer_data=JS_GetArrayBuffer(ctx,&buffer_n,r->buffer);
        if(!buffer_data){JSValue ex=JS_GetException(ctx);JS_FreeValue(ctx,ex);memory_forget_buffer(ctx,r);return 1;}
        if(buffer_n!=n||buffer_data!=data){memory_forget_buffer(ctx,r);return 1;}
        return 1;
    }
    size_t oldn=0;uint8_t*old=JS_GetArrayBuffer(ctx,&oldn,r->buffer);if(!old)return 1;
    if(!h)return 0;
    if(!refresh){char*b64=b64_encode(old,oldn);if(!b64)return 0;SBuf b={0};sb_s(&b,"{\"cmd\":\"memoryWrite\",\"ref\":");ref_make_remote(r,&b);sb_s(&b,",\"offset\":0,\"data\":");sb_json_string(&b,b64,strlen(b64));sb_ch(&b,'}');free(b64);char err[256];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);JS_FreeValue(ctx,res);return ok;}
    uint8_t*data=NULL;size_t n=0;char err[256];if(!memory_load_remote(ctx,h,r,&data,&n,err,sizeof err))return 0;if(n==oldn)memcpy(old,data,n);else memory_forget_buffer(ctx,r);free(data);return 1;
}
static void wasm_memory_detach_resource_buffers(JSContext*ctx,MlbWasmMemory*memory){
    if(!ctx||!memory)return;JSRuntime*rt=JS_GetRuntime(ctx);
    for(WasmRef*r=g_wasm_memory_refs;r;r=r->memory_registry_next){
        if(r->runtime==rt&&r->native_memory==memory&&!JS_IsUndefined(r->buffer)&&!JS_IsNull(r->buffer))memory_forget_buffer(ctx,r);
    }
}
static void wasm_memory_detach_stale_buffers(JSContext*ctx){
    if(!ctx)return;JSRuntime*rt=JS_GetRuntime(ctx);
    for(WasmRef*r=g_wasm_memory_refs;r;r=r->memory_registry_next){
        if(r->runtime!=rt||!r->native_memory||JS_IsUndefined(r->buffer)||JS_IsNull(r->buffer))continue;
        size_t n=0;uint64_t epoch=0;
        if(!mlb_wasm_rt_memory_resource_data(r->native_memory,&n,NULL,&epoch)||n!=r->buffer_len||epoch!=r->memory_epoch)memory_forget_buffer(ctx,r);
    }
}
static void native_instance_sync_memory_exports(JSContext*ctx,WasmInstance*in,JSValueConst exports,int refresh){
    if(!in||!in->native||JS_IsUndefined(exports))return;
    WasmModule*m=get_module(ctx,in->module_obj);if(!m)return;WasmHost*h=host_for_ctx(ctx,0);
    int ne=array_len(ctx,m->exports);
    for(int j=0;j<ne;j++){
        JSValue d=array_item(ctx,m->exports,(uint32_t)j);char name[512],kind[32];
        get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);
        if(!strcmp(kind,"memory")){JSValue v=JS_GetPropertyStr(ctx,exports,name);WasmRef*r=JS_GetOpaque(v,wasm_memory_cid);if(r)memory_sync(ctx,h,r,refresh);JS_FreeValue(ctx,v);}
        JS_FreeValue(ctx,d);
    }
    /* Native resources are live views; synchronization is a validation/no-op for
       them. Remote fallback memories still need explicit write-back/read-back. */
    int ni=array_len(ctx,m->imports);
    for(int j=0;j<ni;j++){
        JSValue d=array_item(ctx,m->imports,(uint32_t)j);char mod[512],name[512],kind[32];
        get_string_field(ctx,d,"module",mod,sizeof mod);get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);
        if(!strcmp(kind,"memory")){
            JSValue ns=JS_GetPropertyStr(ctx,in->imports_obj,mod);JSValue v=JS_IsObject(ns)?JS_GetPropertyStr(ctx,ns,name):JS_UNDEFINED;
            WasmRef*r=JS_GetOpaque(v,wasm_memory_cid);if(r)memory_sync(ctx,h,r,refresh);
            JS_FreeValue(ctx,v);JS_FreeValue(ctx,ns);
        }
        JS_FreeValue(ctx,d);
    }
}
static void instance_sync_memories(JSContext*ctx,JSValueConst instance_obj,WasmInstance*in,int refresh){
    if(!in)return;WasmModule*m=get_module(ctx,in->module_obj);if(!m)return;JSValue ex=JS_GetPropertyStr(ctx,instance_obj,"exports");
    if(in->native){native_instance_sync_memory_exports(ctx,in,ex,refresh);JS_FreeValue(ctx,ex);return;}
    WasmHost*h=host_for_ctx(ctx,0);if(!h){JS_FreeValue(ctx,ex);return;}int ne=array_len(ctx,m->exports);for(int j=0;j<ne;j++){JSValue d=array_item(ctx,m->exports,j);char name[512],kind[32];get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);if(!strcmp(kind,"memory")){JSValue v=JS_GetPropertyStr(ctx,ex,name);WasmRef*r=JS_GetOpaque(v,wasm_memory_cid);if(r)memory_sync(ctx,h,r,refresh);JS_FreeValue(ctx,v);}JS_FreeValue(ctx,d);}JS_FreeValue(ctx,ex);
    int ni=array_len(ctx,m->imports);for(int j=0;j<ni;j++){JSValue d=array_item(ctx,m->imports,j);char mod[512],name[512],kind[32];get_string_field(ctx,d,"module",mod,sizeof mod);get_string_field(ctx,d,"name",name,sizeof name);get_string_field(ctx,d,"kind",kind,sizeof kind);if(!strcmp(kind,"memory")){JSValue ns=JS_GetPropertyStr(ctx,in->imports_obj,mod);JSValue v=JS_GetPropertyStr(ctx,ns,name);WasmRef*r=JS_GetOpaque(v,wasm_memory_cid);if(r)memory_sync(ctx,h,r,refresh);JS_FreeValue(ctx,v);JS_FreeValue(ctx,ns);}JS_FreeValue(ctx,d);}
}

static JSValue native_wasm_value_to_js(JSContext*ctx,const MlbWasmValue*v){if(v->type==MLB_WASM_I32)return JS_NewInt32(ctx,(int32_t)v->bits);if(v->type==MLB_WASM_I64)return JS_NewBigInt64(ctx,(int64_t)v->bits);if(v->type==MLB_WASM_F32){uint32_t b=(uint32_t)v->bits;float f;memcpy(&f,&b,4);return JS_NewFloat64(ctx,(double)f);}if(v->type==MLB_WASM_F64){double d;memcpy(&d,&v->bits,8);return JS_NewFloat64(ctx,d);}if(v->type==MLB_WASM_EXTERNREF)return native_externref_to_js(ctx,v->bits);return JS_UNDEFINED;}
static int native_wasm_value_from_js(JSContext*ctx,JSValueConst value,uint8_t type,MlbWasmValue*out){out->type=type;if(type==MLB_WASM_I32){int32_t x;if(JS_ToInt32(ctx,&x,value)<0)return 0;out->bits=(uint32_t)x;return 1;}if(type==MLB_WASM_I64){int64_t x;if(JS_ToBigInt64(ctx,&x,value)<0)return 0;out->bits=(uint64_t)x;return 1;}if(type==MLB_WASM_F32){double x;if(JS_ToFloat64(ctx,&x,value)<0)return 0;float f=(float)x;uint32_t bits;memcpy(&bits,&f,4);out->bits=bits;return 1;}if(type==MLB_WASM_F64){double x;if(JS_ToFloat64(ctx,&x,value)<0)return 0;memcpy(&out->bits,&x,8);return 1;}if(type==MLB_WASM_EXTERNREF)return native_externref_from_js(ctx,value,&out->bits);return 0;}
static void wasm_native_capture_exception(JSContext *ctx, WasmInstance *in) {
    JSValue ex = JS_GetException(ctx);
    if (!JS_IsUndefined(in->native_exception)) JS_FreeValue(ctx, in->native_exception);
    in->native_exception = ex;
}

static int wasm_native_host_call_multi(void*opaque,uint32_t import_index,const MlbWasmValue*args,size_t argc,MlbWasmValue*results,size_t results_capacity,size_t*results_count,char*error,size_t ec){
    WasmInstance*in=opaque;
    if(results_count)*results_count=0;
    if(!in||!in->ctx||import_index>=(uint32_t)in->native_import_count){if(error&&ec)snprintf(error,ec,"Invalid imported WebAssembly function");return 0;}
    JSContext*ctx=in->ctx;WasmModule*m=get_module(ctx,in->module_obj);
    const uint8_t*pt=NULL,*rt=NULL;size_t np=0,nr=0;
    if(!m||!mlb_wasm_rt_func_signature_multi(m->native,import_index,&pt,&np,&rt,&nr)||np!=argc||np>256||nr>results_capacity||(nr&&!results)){
        if(error&&ec)snprintf(error,ec,"WebAssembly import signature mismatch");
        return 0;
    }
    if(in->native&&!JS_IsUndefined(in->exports_obj))native_instance_sync_memory_exports(ctx,in,in->exports_obj,1);
    JSValue*argv=calloc(np?np:1,sizeof(JSValue));
    if(!argv){if(error&&ec)snprintf(error,ec,"Out of memory preparing JavaScript import arguments");return 0;}
    for(size_t j=0;j<np;j++)argv[j]=native_wasm_value_to_js(ctx,&args[j]);
    JSValue ret=JS_Call(ctx,in->native_import_values[import_index],JS_UNDEFINED,(int)np,(JSValueConst*)argv);
    for(size_t j=0;j<np;j++)JS_FreeValue(ctx,argv[j]);free(argv);
    if(JS_IsException(ret)){
        wasm_native_capture_exception(ctx,in);
        if(error&&ec)snprintf(error,ec,"JavaScript WebAssembly import threw an exception");
        return 0;
    }
    if(in->native&&!JS_IsUndefined(in->exports_obj))native_instance_sync_memory_exports(ctx,in,in->exports_obj,0);
    if(nr==1){
        if(!native_wasm_value_from_js(ctx,ret,rt[0],&results[0])){
            wasm_native_capture_exception(ctx,in);
            JS_FreeValue(ctx,ret);
            if(error&&ec)snprintf(error,ec,"JavaScript import returned an incompatible value");
            return 0;
        }
        if(results_count)*results_count=1;
    }else if(nr>1){
        if(!JS_IsArray(ctx,ret)){JS_FreeValue(ctx,ret);if(error&&ec)snprintf(error,ec,"JavaScript multi-value import must return an array");return 0;}
        JSValue lv=JS_GetPropertyStr(ctx,ret,"length");uint32_t len=0;int len_ok=JS_ToUint32(ctx,&len,lv)==0;JS_FreeValue(ctx,lv);
        if(!len_ok||len!=nr){JS_FreeValue(ctx,ret);if(error&&ec)snprintf(error,ec,"JavaScript multi-value import returned the wrong number of results");return 0;}
        for(size_t j=0;j<nr;j++){
            JSValue v=JS_GetPropertyUint32(ctx,ret,(uint32_t)j);
            if(JS_IsException(v)){
                wasm_native_capture_exception(ctx,in);
                JS_FreeValue(ctx,ret);
                if(error&&ec)snprintf(error,ec,"JavaScript multi-value import result access threw");
                return 0;
            }
            if(!native_wasm_value_from_js(ctx,v,rt[j],&results[j])){
                wasm_native_capture_exception(ctx,in);
                JS_FreeValue(ctx,v);
                JS_FreeValue(ctx,ret);
                if(error&&ec)snprintf(error,ec,"JavaScript multi-value import returned an incompatible result");
                return 0;
            }
            JS_FreeValue(ctx,v);
        }
        if(results_count)*results_count=nr;
    }
    JS_FreeValue(ctx,ret);if(error&&ec)error[0]=0;return 1;
}

static void wasm_detach_grown_memories(JSContext*ctx,JSValueConst instance_obj,WasmInstance*in,JSValueConst response){
    JSValue grown=JS_GetPropertyStr(ctx,response,"grownMemories");
    JSValue exports=JS_GetPropertyStr(ctx,instance_obj,"exports");
    int n=array_len(ctx,grown);
    for(int i=0;i<n;i++){
        JSValue d=array_item(ctx,grown,i);char kind[32],name[512],module[512];
        get_string_field(ctx,d,"kind",kind,sizeof kind);get_string_field(ctx,d,"name",name,sizeof name);
        JSValue target=JS_UNDEFINED;
        if(!strcmp(kind,"export"))target=JS_GetPropertyStr(ctx,exports,name);
        else if(!strcmp(kind,"import")){
            get_string_field(ctx,d,"module",module,sizeof module);
            JSValue ns=JS_GetPropertyStr(ctx,in->imports_obj,module);
            target=JS_GetPropertyStr(ctx,ns,name);JS_FreeValue(ctx,ns);
        }
        WasmRef*r=JS_GetOpaque(target,wasm_memory_cid);
        if(r)memory_forget_buffer(ctx,r);
        JS_FreeValue(ctx,target);JS_FreeValue(ctx,d);
    }
    JS_FreeValue(ctx,exports);JS_FreeValue(ctx,grown);
}

static JSValue wasm_export_function_call_impl(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv,int magic,JSValue*fd){(void)this_val;(void)magic;if(argc>256)return JS_ThrowRangeError(ctx,"Too many WebAssembly function arguments");WasmInstance*in=get_instance(ctx,fd[0]);if(!in)return wasm_named_error(ctx,"RuntimeError","WebAssembly instance has been released");
    if(in->native){
        char name[512],err[256];WasmModule*mod=get_module(ctx,in->module_obj);int kind=0;uint32_t fi=0;
        if(JS_IsNumber(fd[1])){uint32_t direct=0;if(JS_ToUint32(ctx,&direct,fd[1])<0)return JS_EXCEPTION;if(!mod||direct>=4096)return wasm_named_error(ctx,"TypeError","Invalid native WebAssembly function reference");fi=direct;}
        else{const char*ns=JS_ToCString(ctx,fd[1]);snprintf(name,sizeof name,"%s",ns?ns:"");if(ns)JS_FreeCString(ctx,ns);if(!mod||!mlb_wasm_rt_find_export(mod->native,name,&kind,&fi)||kind!=MLB_WASM_FUNC)return wasm_named_error(ctx,"TypeError","Export is not a WebAssembly function");}
        const uint8_t*pt=NULL,*rt=NULL;size_t np=0,nr=0;
        if(!mlb_wasm_rt_func_signature_multi(mod->native,fi,&pt,&np,&rt,&nr)||np!=(size_t)argc||nr>32)return wasm_named_error(ctx,"TypeError","Incorrect WebAssembly function signature");
        MlbWasmValue args[256],out[32];
        for(int a=0;a<argc;a++)if(!native_wasm_value_from_js(ctx,argv[a],pt[a],&args[a]))return JS_EXCEPTION;
        instance_sync_memories(ctx,fd[0],in,0);
        if(!JS_IsUndefined(in->native_exception)){JS_FreeValue(ctx,in->native_exception);in->native_exception=JS_UNDEFINED;}
        size_t got=0;
        if(!mlb_wasm_rt_call_multi(in->native,fi,args,(size_t)argc,out,32,&got,err,sizeof err)){
            wasm_memory_detach_stale_buffers(ctx);instance_sync_memories(ctx,fd[0],in,1);
            if(!JS_IsUndefined(in->native_exception)){JSValue ex=in->native_exception;in->native_exception=JS_UNDEFINED;return JS_Throw(ctx,ex);}
            return wasm_named_error(ctx,"RuntimeError",err);
        }
        wasm_memory_detach_stale_buffers(ctx);instance_sync_memories(ctx,fd[0],in,1);
        if(got==0)return JS_UNDEFINED;
        if(got==1)return native_wasm_value_to_js(ctx,&out[0]);
        JSValue values=JS_NewArray(ctx);
        if(JS_IsException(values))return values;
        for(size_t k=0;k<got;k++){
            JSValue v=native_wasm_value_to_js(ctx,&out[k]);
            if(JS_IsException(v)||JS_SetPropertyUint32(ctx,values,(uint32_t)k,v)<0){JS_FreeValue(ctx,values);return JS_EXCEPTION;}
        }
        return values;
    }
    WasmHost*h=host_for_ctx(ctx,1);if(!h)return JS_ThrowOutOfMemory(ctx);char name[512];const char*s=JS_ToCString(ctx,fd[1]);snprintf(name,sizeof name,"%s",s?s:"");if(s)JS_FreeCString(ctx,s);instance_sync_memories(ctx,fd[0],in,0);SBuf b={0};sb_s(&b,"{\"cmd\":\"call\",\"instance\":");sb_i64(&b,in->id);sb_s(&b,",\"name\":");sb_json_string(&b,name,strlen(name));sb_s(&b,",\"args\":[");for(int i=0;i<argc;i++){if(i)sb_ch(&b,',');mlb_wasm_encode_value(ctx,h,argv[i],&b);}sb_s(&b,"]}");char err[1024];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);if(ok)wasm_detach_grown_memories(ctx,fd[0],in,res);instance_sync_memories(ctx,fd[0],in,1);if(!ok){const char*kind="RuntimeError";const char*msg=strip_error_class(err,&kind);return wasm_named_error(ctx,kind,msg);}JSValue val=JS_GetPropertyStr(ctx,res,"value");JSValue out=rpc_decode_value(ctx,h,val);JS_FreeValue(ctx,val);JS_FreeValue(ctx,res);return out;}
static JSValue wasm_export_function_call(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv,int magic,JSValue*fd){
    WasmInstance *in = (fd && JS_IsObject(fd[0])) ? get_instance(ctx, fd[0]) : NULL;
    WasmHost *h = (in && in->native) ? host_for_ctx(ctx, 1) : NULL;
    if (in && in->native && !h) return JS_ThrowOutOfMemory(ctx);
    if (h) h->native_call_depth++;
    JSValue result = wasm_export_function_call_impl(ctx,this_val,argc,argv,magic,fd);
    if (h) {
        if (h->native_call_depth) h->native_call_depth--;
        if (!h->native_call_depth) {
            int refs_ok = wasm_native_externref_sync_all(ctx);
            wasm_native_externref_clear_temporaries(h);
            wasm_native_externref_cleanup(ctx,h);
            if (!refs_ok && !JS_IsException(result)) { JS_FreeValue(ctx,result); result=JS_ThrowOutOfMemory(ctx); }
        }
    }
    return result;
}

JSValue wasm_remote_function_call(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv,int magic,JSValue*fd){(void)this_val;(void)magic;WasmHost*h=host_for_ctx(ctx,1);if(!h)return JS_ThrowOutOfMemory(ctx);int id=0;JS_ToInt32(ctx,&id,fd[0]);SBuf b={0};sb_s(&b,"{\"cmd\":\"functionRefCall\",\"function\":");sb_i64(&b,id);sb_s(&b,",\"args\":[");for(int i=0;i<argc;i++){if(i)sb_ch(&b,',');mlb_wasm_encode_value(ctx,h,argv[i],&b);}sb_s(&b,"]}");char err[768];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);if(!ok){const char*kind="RuntimeError";const char*msg=strip_error_class(err,&kind);return wasm_named_error(ctx,kind,msg);}JSValue val=JS_GetPropertyStr(ctx,res,"value");JSValue out=rpc_decode_value(ctx,h,val);JS_FreeValue(ctx,val);JS_FreeValue(ctx,res);return out;}

static JSValue wasm_memory_buffer_get(JSContext*ctx,JSValueConst this_val){
    WasmRef*r=get_ref(ctx,this_val,wasm_memory_cid);if(!r)return JS_ThrowTypeError(ctx,"Illegal WebAssembly.Memory receiver");
    if(r->native_memory){size_t n=0;uint64_t epoch=0;uint8_t*data=mlb_wasm_rt_memory_resource_data(r->native_memory,&n,NULL,&epoch);if(!data)return wasm_named_error(ctx,"RuntimeError","Native WebAssembly memory resource is unavailable");if(!JS_IsUndefined(r->buffer)&&r->buffer_len==n&&r->memory_epoch==epoch){if(memory_sync(ctx,NULL,r,1)&&!JS_IsUndefined(r->buffer))return JS_DupValue(ctx,r->buffer);}memory_forget_buffer(ctx,r);mlb_wasm_rt_memory_retain(r->native_memory);r->buffer=JS_NewArrayBuffer(ctx,data,n,wasm_memory_arraybuffer_free,r->native_memory,0);if(JS_IsException(r->buffer)){mlb_wasm_rt_memory_release(r->native_memory);return r->buffer;}r->buffer_len=n;r->memory_epoch=epoch;return JS_DupValue(ctx,r->buffer);}
    if(!JS_IsUndefined(r->buffer))return JS_DupValue(ctx,r->buffer);WasmHost*h=host_for_ctx(ctx,1);if(!h)return JS_ThrowOutOfMemory(ctx);uint8_t*data=NULL;size_t n=0;char err[768];if(!memory_load_remote(ctx,h,r,&data,&n,err,sizeof err)){const char*kind="RuntimeError";const char*msg=strip_error_class(err,&kind);return wasm_named_error(ctx,kind,msg);}r->buffer=JS_NewArrayBufferCopy(ctx,data,n);free(data);if(JS_IsException(r->buffer))return r->buffer;r->buffer_len=n;return JS_DupValue(ctx,r->buffer);
}
static JSValue wasm_memory_grow(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){
    WasmRef*r=get_ref(ctx,this_val,wasm_memory_cid);
    if(!r)return JS_ThrowTypeError(ctx,"Illegal WebAssembly.Memory receiver");
    uint64_t d=0;
    if(argc&&JS_ToIndex(ctx,&d,argv[0])<0)return JS_EXCEPTION;
    if(d>65536)return JS_ThrowRangeError(ctx,"WebAssembly memory growth is too large");
    if(r->native_memory){if(!memory_sync(ctx,NULL,r,0))return wasm_named_error(ctx,"RuntimeError","Unable to access native WebAssembly memory before grow");int64_t prev=mlb_wasm_rt_memory_resource_grow(r->native_memory,(uint32_t)d);if(prev<0)return wasm_named_error(ctx,"RangeError","WebAssembly memory maximum exceeded");wasm_memory_detach_resource_buffers(ctx,r->native_memory);return JS_NewInt32(ctx,(int32_t)prev);}
    if(!r->standalone){WasmInstance*in=get_instance(ctx,r->owner_obj);if(in&&in->native){instance_sync_memories(ctx,r->owner_obj,in,0);int64_t prev=mlb_wasm_rt_memory_grow(in->native,(uint32_t)d);if(prev<0)return wasm_named_error(ctx,"RangeError","WebAssembly memory maximum exceeded");memory_forget_buffer(ctx,r);return JS_NewInt32(ctx,(int32_t)prev);}}
    WasmHost*h=host_for_ctx(ctx,1);
    if(!h)return JS_ThrowOutOfMemory(ctx);
    /* Flush mutations through the current ArrayBuffer before V8 grows memory. */
    if(!memory_sync(ctx,h,r,0))return wasm_named_error(ctx,"RuntimeError","Unable to synchronize WebAssembly memory before grow");
    SBuf b={0};sb_s(&b,"{\"cmd\":\"memoryGrow\",\"ref\":");ref_make_remote(r,&b);sb_s(&b,",\"delta\":");sb_u64(&b,d);sb_ch(&b,'}');
    char err[512];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);
    if(!ok){const char*kind="RuntimeError";const char*msg=strip_error_class(err,&kind);JS_FreeValue(ctx,res);return wasm_named_error(ctx,kind,msg);}
    int prev=-1;get_int_field(ctx,res,"previous",&prev);JS_FreeValue(ctx,res);
    /* Per WebAssembly.Memory.grow, even grow(0) replaces and detaches the old buffer. */
    memory_forget_buffer(ctx,r);
    return JS_NewInt32(ctx,prev);
}
static JSValue wasm_memory_ctor(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){
    (void)this_val;
    if(argc<1||!JS_IsObject(argv[0]))return JS_ThrowTypeError(ctx,"WebAssembly.Memory requires a descriptor");
    int initial=0,maximum=0,hasmax=0,shared=0;
    get_int_field(ctx,argv[0],"initial",&initial);
    JSValue mv=JS_GetPropertyStr(ctx,argv[0],"maximum");
    if(!JS_IsUndefined(mv)){JS_ToInt32(ctx,&maximum,mv);hasmax=1;}
    JS_FreeValue(ctx,mv);
    JSValue sv=JS_GetPropertyStr(ctx,argv[0],"shared");shared=JS_ToBool(ctx,sv);JS_FreeValue(ctx,sv);
    if(initial<0||initial>1024||maximum>1024||maximum<0||(hasmax&&maximum<initial))return JS_ThrowRangeError(ctx,"Invalid WebAssembly.Memory limits");
    if(shared&&!hasmax)return JS_ThrowTypeError(ctx,"Shared WebAssembly.Memory requires a maximum");
    /* The Node/V8 bridge copies Memory.buffer across a process boundary. Allowing
       shared:true here would advertise SharedArrayBuffer semantics that it cannot
       provide, so fail explicitly rather than silently returning a non-shared copy. */
    if(shared)return JS_ThrowTypeError(ctx,"Shared WebAssembly.Memory is not supported by the process bridge");
    char err[256];MlbWasmMemory*resource=mlb_wasm_rt_memory_new((uint32_t)initial,hasmax,hasmax?(uint32_t)maximum:0,err,sizeof err);
    if(!resource)return wasm_named_error(ctx,"RangeError",err[0]?err:"Unable to allocate native WebAssembly memory");
    JSValue o=make_ref_object(ctx,wasm_memory_cid,1,0,JS_UNDEFINED,0,NULL);if(JS_IsException(o)){mlb_wasm_rt_memory_release(resource);return o;}WasmRef*r=JS_GetOpaque(o,wasm_memory_cid);r->native_memory=resource;wasm_memory_ref_register(ctx,r);return o;
}

static const char *wasm_value_type_name(JSContext*ctx,JSValueConst opt){char t[32];if(get_string_field(ctx,opt,"value",t,sizeof t)){if(!strcmp(t,"i32")||!strcmp(t,"i64")||!strcmp(t,"f32")||!strcmp(t,"f64")||!strcmp(t,"externref")||!strcmp(t,"anyfunc")||!strcmp(t,"funcref"))return strdup(!strcmp(t,"funcref")?"anyfunc":t);}return NULL;}
static void free_type_name(const char*t){free((void*)t);}
static JSValue wasm_global_get(JSContext*ctx,JSValueConst this_val){WasmRef*r=get_ref(ctx,this_val,wasm_global_cid);if(!r)return JS_ThrowTypeError(ctx,"Illegal WebAssembly.Global receiver");if(r->native_global){MlbWasmValue value;if(!mlb_wasm_rt_global_resource_get(r->native_global,&value))return wasm_named_error(ctx,"RuntimeError","Native WebAssembly.Global resource is unavailable");return native_wasm_value_to_js(ctx,&value);}if(!r->standalone){WasmInstance*in=get_instance(ctx,r->owner_obj);if(in&&in->native){WasmModule*m=get_module(ctx,in->module_obj);int kind=0;uint32_t gi=0;MlbWasmValue v;if(!m||!r->name||!mlb_wasm_rt_find_export(m->native,r->name,&kind,&gi)||kind!=MLB_WASM_GLOBAL||!mlb_wasm_rt_global_get(in->native,gi,&v))return wasm_named_error(ctx,"RuntimeError","Native WebAssembly.Global export is unavailable");return native_wasm_value_to_js(ctx,&v);}}WasmHost*h=host_for_ctx(ctx,1);char err[512];int ok=0;JSValue res=rpc_ref(ctx,h,"globalRead",r,err,sizeof err,&ok);if(!ok){const char*kind="RuntimeError";const char*msg=strip_error_class(err,&kind);JS_FreeValue(ctx,res);return wasm_named_error(ctx,kind,msg);}JSValue v=JS_GetPropertyStr(ctx,res,"value");JSValue out=rpc_decode_value(ctx,h,v);JS_FreeValue(ctx,v);JS_FreeValue(ctx,res);return out;}
static JSValue wasm_global_set(JSContext*ctx,JSValueConst this_val,JSValueConst value){WasmRef*r=get_ref(ctx,this_val,wasm_global_cid);if(!r)return JS_ThrowTypeError(ctx,"Illegal WebAssembly.Global receiver");if(r->native_global){if(!r->mutable_value)return JS_ThrowTypeError(ctx,"Cannot set an immutable WebAssembly.Global");MlbWasmValue nv;if(!native_wasm_value_from_js(ctx,value,(uint8_t)r->value_type,&nv))return JS_EXCEPTION;if(!mlb_wasm_rt_global_resource_set(r->native_global,nv))return JS_ThrowTypeError(ctx,"Invalid WebAssembly.Global value");if(nv.type==MLB_WASM_EXTERNREF&&!wasm_native_externref_commit(ctx))return JS_ThrowOutOfMemory(ctx);return JS_UNDEFINED;}if(!r->standalone){WasmInstance*in=get_instance(ctx,r->owner_obj);if(in&&in->native){WasmModule*m=get_module(ctx,in->module_obj);int kind=0,mut=0;uint32_t gi=0;uint8_t type=0;MlbWasmValue v;if(!m||!r->name||!mlb_wasm_rt_find_export(m->native,r->name,&kind,&gi)||kind!=MLB_WASM_GLOBAL||!mlb_wasm_rt_global_info(m->native,gi,&type,&mut))return wasm_named_error(ctx,"RuntimeError","Native WebAssembly.Global export is unavailable");if(!mut)return wasm_named_error(ctx,"TypeError","Cannot set an immutable WebAssembly.Global");if(!native_wasm_value_from_js(ctx,value,type,&v))return JS_EXCEPTION;if(!mlb_wasm_rt_global_set(in->native,gi,v))return wasm_named_error(ctx,"TypeError","Invalid WebAssembly.Global value");if(v.type==MLB_WASM_EXTERNREF&&!wasm_native_externref_commit(ctx))return JS_ThrowOutOfMemory(ctx);return JS_UNDEFINED;}}WasmHost*h=host_for_ctx(ctx,1);SBuf b={0};sb_s(&b,"{\"cmd\":\"globalWrite\",\"ref\":");ref_make_remote(r,&b);sb_s(&b,",\"value\":");if(r->value_type==1)mlb_wasm_encode_externref(ctx,h,value,&b);else mlb_wasm_encode_value(ctx,h,value,&b);sb_ch(&b,'}');char err[512];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);JS_FreeValue(ctx,res);if(!ok){const char*kind="RuntimeError";const char*msg=strip_error_class(err,&kind);return wasm_named_error(ctx,kind,msg);}return JS_UNDEFINED;}
static JSValue wasm_global_ctor(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){
    (void)this_val;
    if(argc<2||!JS_IsObject(argv[0]))return JS_ThrowTypeError(ctx,"WebAssembly.Global requires a descriptor and initial value");
    const char*type=wasm_value_type_name(ctx,argv[0]);if(!type)return JS_ThrowTypeError(ctx,"Invalid WebAssembly.Global value type");
    JSValue mut=JS_GetPropertyStr(ctx,argv[0],"mutable");int mutable_value=JS_ToBool(ctx,mut);JS_FreeValue(ctx,mut);
    uint8_t native_type=0;if(!strcmp(type,"i32"))native_type=MLB_WASM_I32;else if(!strcmp(type,"i64"))native_type=MLB_WASM_I64;else if(!strcmp(type,"f32"))native_type=MLB_WASM_F32;else if(!strcmp(type,"f64"))native_type=MLB_WASM_F64;else if(!strcmp(type,"externref"))native_type=MLB_WASM_EXTERNREF;
    if(native_type){
        MlbWasmValue initial;if(!native_wasm_value_from_js(ctx,argv[1],native_type,&initial)){free_type_name(type);return JS_EXCEPTION;}
        char err[256];MlbWasmGlobal*g=mlb_wasm_rt_global_new(native_type,mutable_value,initial,err,sizeof err);free_type_name(type);
        if(!g)return wasm_named_error(ctx,"TypeError",err[0]?err:"Unable to allocate native WebAssembly.Global");
        JSValue o=make_ref_object(ctx,wasm_global_cid,1,0,JS_UNDEFINED,0,NULL);if(JS_IsException(o)){mlb_wasm_rt_global_release(g);return o;}
        WasmRef*r=JS_GetOpaque(o,wasm_global_cid);r->native_global=g;r->mutable_value=mutable_value;r->value_type=native_type;if(native_type==MLB_WASM_EXTERNREF&&!wasm_native_externref_commit(ctx)){JS_FreeValue(ctx,o);return JS_ThrowOutOfMemory(ctx);}return o;
    }
    WasmHost*h=host_for_ctx(ctx,1);if(!h){free_type_name(type);return JS_ThrowOutOfMemory(ctx);}
    int id=++h->next_ref;SBuf b={0};sb_s(&b,"{\"cmd\":\"globalCreate\",\"id\":");sb_i64(&b,id);sb_s(&b,",\"valueType\":");sb_json_string(&b,type,strlen(type));sb_s(&b,",\"mutable\":");sb_s(&b,mutable_value?"true":"false");sb_s(&b,",\"value\":");
    int is_externref=!strcmp(type,"externref");if(is_externref)mlb_wasm_encode_externref(ctx,h,argv[1],&b);else mlb_wasm_encode_value(ctx,h,argv[1],&b);sb_ch(&b,'}');free_type_name(type);
    char err[768];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);JS_FreeValue(ctx,res);if(!ok){const char*kind="TypeError";const char*msg=strip_error_class(err,&kind);return wasm_named_error(ctx,kind,msg);}
    JSValue o=make_ref_object(ctx,wasm_global_cid,1,id,JS_UNDEFINED,0,NULL);WasmRef*r=JS_GetOpaque(o,wasm_global_cid);r->mutable_value=mutable_value;r->value_type=is_externref?1:0;return o;
}

static int wasm_table_native_function(JSContext*ctx,JSValueConst value){if(!JS_IsFunction(ctx,value))return 0;const char*props[]={"__mlbWasmNativeExportRef","__mlbWasmExportRef","__mlbWasmRemoteFunctionId"};for(size_t j=0;j<sizeof(props)/sizeof(props[0]);j++){JSValue marker=JS_GetPropertyStr(ctx,value,props[j]);int yes=j==2?!JS_IsUndefined(marker):JS_IsObject(marker);JS_FreeValue(ctx,marker);if(yes)return 1;}return 0;}
static int wasm_table_enable_local(JSContext*ctx,WasmRef*r){if(r->local_table_mode)return 1;if(r->remote_imported)return 0;WasmHost*h=host_for_ctx(ctx,1);if(!h)return 0;char err[512];int ok=0;JSValue lr=rpc_ref(ctx,h,"tableLength",r,err,sizeof err,&ok);if(!ok){JS_FreeValue(ctx,lr);return 0;}int32_t length=0;if(get_int_field(ctx,lr,"length",&length)<0||length<0||length>1000000){JS_FreeValue(ctx,lr);return 0;}JS_FreeValue(ctx,lr);JSValue*values=calloc((size_t)(length?length:1),sizeof(JSValue));if(!values)return 0;for(int32_t i=0;i<length;i++){SBuf b={0};sb_s(&b,"{\"cmd\":\"tableGet\",\"ref\":");ref_make_remote(r,&b);sb_s(&b,",\"index\":");sb_i64(&b,i);sb_ch(&b,'}');JSValue response=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);if(!ok){JS_FreeValue(ctx,response);for(int32_t j=0;j<i;j++)JS_FreeValue(ctx,values[j]);free(values);return 0;}JSValue encoded=JS_GetPropertyStr(ctx,response,"value");values[i]=rpc_decode_value(ctx,h,encoded);JS_FreeValue(ctx,encoded);JS_FreeValue(ctx,response);if(JS_IsException(values[i])){for(int32_t j=0;j<=i;j++)JS_FreeValue(ctx,values[j]);free(values);return 0;}}r->table_values=values;r->table_length=(uint32_t)length;r->table_capacity=(uint32_t)length;r->local_table_mode=1;return 1;}
static JSValue make_native_table_function(JSContext*ctx,JSValueConst instance_obj,uint32_t function_index){
    WasmInstance*in=get_instance(ctx,instance_obj);
    if(in){
        if(function_index>=in->native_function_capacity){
            uint32_t cap=in->native_function_capacity?in->native_function_capacity:8;
            while(cap<=function_index){if(cap>UINT32_MAX/2){cap=function_index+1;break;}cap*=2;}
            if((size_t)cap>SIZE_MAX/sizeof(JSValue))return JS_ThrowOutOfMemory(ctx);
            JSValue*q=realloc(in->native_functions,(size_t)cap*sizeof(JSValue));
            if(!q)return JS_ThrowOutOfMemory(ctx);
            for(uint32_t z=in->native_function_capacity;z<cap;z++)q[z]=JS_UNDEFINED;
            in->native_functions=q;in->native_function_capacity=cap;
        }
        if(!JS_IsUndefined(in->native_functions[function_index]))return JS_DupValue(ctx,in->native_functions[function_index]);
    }
    JSValue data[2]={JS_DupValue(ctx,instance_obj),JS_NewUint32(ctx,function_index)};
    JSValue fn=JS_NewCFunctionData(ctx,wasm_export_function_call,0,0,2,(JSValueConst*)data);
    JS_FreeValue(ctx,data[0]);JS_FreeValue(ctx,data[1]);
    if(JS_IsException(fn))return fn;
    JSValue marker=JS_NewObject(ctx);
    if(JS_IsException(marker)){JS_FreeValue(ctx,fn);return marker;}
    JS_SetPropertyStr(ctx,marker,"instance",JS_DupValue(ctx,instance_obj));
    JS_SetPropertyStr(ctx,marker,"index",JS_NewUint32(ctx,function_index));
    JS_SetPropertyStr(ctx,marker,"native",JS_NewBool(ctx,1));
    JS_DefinePropertyValueStr(ctx,fn,"__mlbWasmNativeExportRef",marker,JS_PROP_CONFIGURABLE);
    if(in)in->native_functions[function_index]=JS_DupValue(ctx,fn);
    return fn;
}
static int native_table_ref(JSContext*ctx,WasmRef*r,WasmInstance**owner,uint32_t*table_index){
    if(!r||r->standalone)return 0;WasmInstance*in=get_instance(ctx,r->owner_obj);if(!in||!in->native)return 0;
    WasmModule*m=get_module(ctx,in->module_obj);int kind=0;uint32_t idx=0;if(!m||!r->name||!mlb_wasm_rt_find_export(m->native,r->name,&kind,&idx)||kind!=MLB_WASM_TABLE)return 0;
    if(owner)*owner=in;if(table_index)*table_index=idx;return 1;
}
static WasmTableRoots *wasm_table_roots_attach(JSContext*ctx,MlbWasmTable*table){
    if(!table)return NULL;JSRuntime*rt=JS_GetRuntime(ctx);
    for(WasmTableRoots*s=g_wasm_table_roots;s;s=s->next)if(s->table==table&&s->rt==rt){if(s->refs!=UINT32_MAX)s->refs++;return s;}
    WasmTableRoots*s=calloc(1,sizeof(*s));if(!s)return NULL;s->table=table;s->rt=rt;s->refs=1;
    uint32_t n=0;if(!mlb_wasm_rt_table_resource_size(table,&n)){free(s);return NULL;}
    uint32_t cap=n?n:4;s->values=malloc((size_t)cap*sizeof(JSValue));if(!s->values){free(s);return NULL;}
    for(uint32_t z=0;z<cap;z++)s->values[z]=JS_UNDEFINED;s->length=n;s->capacity=cap;s->next=g_wasm_table_roots;g_wasm_table_roots=s;return s;
}
static void wasm_table_roots_release_rt(JSRuntime*rt,WasmTableRoots*s){
    if(!s)return;if(s->refs>1){s->refs--;return;}WasmTableRoots**p=&g_wasm_table_roots;while(*p&&*p!=s)p=&(*p)->next;if(*p==s)*p=s->next;
    for(uint32_t n=0;n<s->length&&s->values;n++)JS_FreeValueRT(rt,s->values[n]);free(s->values);free(s);
}
static int wasm_table_roots_ensure(JSContext*ctx,WasmRef*r,uint32_t required){
    if(!r->table_root_store&&r->native_table)r->table_root_store=wasm_table_roots_attach(ctx,r->native_table);
    WasmTableRoots*s=r->table_root_store;if(!s)return 0;
    if(required<=s->capacity){if(s->length<required)s->length=required;return 1;}
    uint32_t cap=s->capacity?s->capacity:4;while(cap<required){if(cap>UINT32_MAX/2){cap=required;break;}cap*=2;}
    JSValue*q=realloc(s->values,(size_t)cap*sizeof(JSValue));if(!q)return 0;for(uint32_t n=s->capacity;n<cap;n++)q[n]=JS_UNDEFINED;s->values=q;s->capacity=cap;if(s->length<required)s->length=required;return 1;
}
static int wasm_table_root_set(JSContext*ctx,WasmRef*r,uint32_t index,JSValueConst value,int keep){
    if(!wasm_table_roots_ensure(ctx,r,index+1))return 0;WasmTableRoots*s=r->table_root_store;
    JS_FreeValue(ctx,s->values[index]);s->values[index]=keep?JS_DupValue(ctx,value):JS_UNDEFINED;return 1;
}
static int wasm_table_function_ref(JSContext*ctx,JSValueConst value,MlbWasmInstance**target,uint32_t*fi,JSValue*target_obj){
    if(JS_IsNull(value)){if(target)*target=NULL;if(fi)*fi=UINT32_MAX;if(target_obj)*target_obj=JS_UNDEFINED;return 1;}
    if(!JS_IsFunction(ctx,value))return 0;
    JSValue marker=JS_GetPropertyStr(ctx,value,"__mlbWasmNativeExportRef");if(!JS_IsObject(marker)){JS_FreeValue(ctx,marker);return 0;}
    JSValue inst=JS_GetPropertyStr(ctx,marker,"instance");WasmInstance*in=get_instance(ctx,inst);if(!in||!in->native){JS_FreeValue(ctx,inst);JS_FreeValue(ctx,marker);return 0;}
    uint32_t idx=0;int ok=0;JSValue iv=JS_GetPropertyStr(ctx,marker,"index");
    if(!JS_IsUndefined(iv)&&JS_ToUint32(ctx,&idx,iv)==0)ok=1;
    JS_FreeValue(ctx,iv);
    if(!ok){JSValue nm=JS_GetPropertyStr(ctx,marker,"name");const char*name=JS_ToCString(ctx,nm);int k=0;WasmModule*wm=get_module(ctx,in->module_obj);if(name&&wm&&mlb_wasm_rt_find_export(wm->native,name,&k,&idx)&&k==MLB_WASM_FUNC)ok=1;if(name)JS_FreeCString(ctx,name);JS_FreeValue(ctx,nm);}
    if(ok){const uint8_t*pt=NULL;size_t np=0;int hr=0;uint8_t rt=0;WasmModule*wm=get_module(ctx,in->module_obj);ok=wm&&mlb_wasm_rt_func_signature(wm->native,idx,&pt,&np,&hr,&rt);}
    JS_FreeValue(ctx,marker);if(!ok){JS_FreeValue(ctx,inst);return 0;}if(target)*target=in->native;if(fi)*fi=idx;if(target_obj)*target_obj=inst;else JS_FreeValue(ctx,inst);return 1;
}
static void wasm_table_root_new_instance_functions(JSContext*ctx,WasmRef*r,JSValueConst instance_obj,MlbWasmInstance*native){
    if(!r||!r->native_table||!native)return;uint32_t n=0;if(!mlb_wasm_rt_table_resource_size(r->native_table,&n)||!wasm_table_roots_ensure(ctx,r,n))return;
    for(uint32_t z=0;z<n;z++){
        MlbWasmInstance*target=NULL;uint32_t fi=UINT32_MAX;
        if(mlb_wasm_rt_table_resource_get_ref(r->native_table,z,&target,&fi)&&target==native){
            JSValue fn=make_native_table_function(ctx,instance_obj,fi);
            if(!JS_IsException(fn)){wasm_table_root_set(ctx,r,z,fn,1);JS_FreeValue(ctx,fn);}
        }
    }
}
static JSValue wasm_table_length_get(JSContext*ctx,JSValueConst this_val){WasmRef*r=get_ref(ctx,this_val,wasm_table_cid);if(!r)return JS_ThrowTypeError(ctx,"Illegal WebAssembly.Table receiver");if(r->local_table_mode)return JS_NewUint32(ctx,r->table_length);if(r->native_table){uint32_t n=0;if(!mlb_wasm_rt_table_resource_size(r->native_table,&n))return wasm_named_error(ctx,"RuntimeError","Native WebAssembly.Table resource is unavailable");return JS_NewUint32(ctx,n);}WasmInstance*owner=NULL;uint32_t ti=0;if(native_table_ref(ctx,r,&owner,&ti)){uint32_t n=0;if(!mlb_wasm_rt_table_size(owner->native,&n))return wasm_named_error(ctx,"RuntimeError","Native WebAssembly.Table export is unavailable");return JS_NewUint32(ctx,n);}WasmHost*h=host_for_ctx(ctx,1);char err[512];int ok=0;JSValue res=rpc_ref(ctx,h,"tableLength",r,err,sizeof err,&ok);if(!ok){const char*k="RuntimeError";const char*m=strip_error_class(err,&k);JS_FreeValue(ctx,res);return wasm_named_error(ctx,k,m);}int n=0;get_int_field(ctx,res,"length",&n);JS_FreeValue(ctx,res);return JS_NewInt32(ctx,n);}
static JSValue wasm_table_get(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){WasmRef*r=get_ref(ctx,this_val,wasm_table_cid);if(!r)return JS_ThrowTypeError(ctx,"Illegal WebAssembly.Table receiver");uint64_t idx=0;if(argc&&JS_ToIndex(ctx,&idx,argv[0])<0)return JS_EXCEPTION;if(idx>UINT32_MAX)return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds");if(r->local_table_mode){if(idx>=r->table_length)return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds");return JS_DupValue(ctx,r->table_values[idx]);}if(r->native_table){
    if(r->value_type==1){uint64_t handle=0;if(!mlb_wasm_rt_table_resource_get_externref(r->native_table,(uint32_t)idx,&handle))return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds");return native_externref_to_js(ctx,handle);}
    MlbWasmInstance*target=NULL;uint32_t fi=UINT32_MAX;
    if(!mlb_wasm_rt_table_resource_get_ref(r->native_table,(uint32_t)idx,&target,&fi))return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds");
    /* WASM bytecode may mutate slots without going through Table.set(). Never
       return a stale rooted JS function just because that slot was cached. */
    if(r->table_root_store&&(uint32_t)idx<r->table_root_store->length&&!JS_IsUndefined(r->table_root_store->values[idx])){
        JSValueConst cached=r->table_root_store->values[idx];MlbWasmInstance*cached_target=NULL;uint32_t cached_fi=UINT32_MAX;JSValue cached_owner=JS_UNDEFINED;
        int matches=target&&wasm_table_function_ref(ctx,cached,&cached_target,&cached_fi,&cached_owner)&&cached_target==target&&cached_fi==fi;
        JS_FreeValue(ctx,cached_owner);
        if(matches)return JS_DupValue(ctx,cached);
        JS_FreeValue(ctx,r->table_root_store->values[idx]);r->table_root_store->values[idx]=JS_UNDEFINED;
    }
    if(!target)return JS_NULL;
    WasmInstance*owner=get_instance(ctx,r->owner_obj);
    if(owner&&owner->native==target){
        JSValue fn=make_native_table_function(ctx,r->owner_obj,fi);if(JS_IsException(fn))return fn;
        if(!wasm_table_root_set(ctx,r,(uint32_t)idx,fn,1)){JS_FreeValue(ctx,fn);return JS_ThrowOutOfMemory(ctx);}
        return fn;
    }
    return wasm_named_error(ctx,"RuntimeError","Native function reference has no live JavaScript owner");
}WasmInstance*owner=NULL;uint32_t ti=0;if(native_table_ref(ctx,r,&owner,&ti)){MlbWasmInstance*target=NULL;uint32_t fi=UINT32_MAX;if(!mlb_wasm_rt_table_get_ref(owner->native,(uint32_t)idx,&target,&fi))return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds");if(!target)return JS_NULL;if(target==owner->native)return make_native_table_function(ctx,r->owner_obj,fi);return wasm_named_error(ctx,"RuntimeError","Native function reference has no live JavaScript owner");}WasmHost*h=host_for_ctx(ctx,1);SBuf b={0};sb_s(&b,"{\"cmd\":\"tableGet\",\"ref\":");ref_make_remote(r,&b);sb_s(&b,",\"index\":");sb_u64(&b,idx);sb_ch(&b,'}');char err[512];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);if(!ok){const char*k="RangeError";const char*m=strip_error_class(err,&k);JS_FreeValue(ctx,res);return wasm_named_error(ctx,k,m);}JSValue val=JS_GetPropertyStr(ctx,res,"value");JSValue out=rpc_decode_value(ctx,h,val);JS_FreeValue(ctx,val);JS_FreeValue(ctx,res);return out;}
static JSValue wasm_table_set(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){WasmRef*r=get_ref(ctx,this_val,wasm_table_cid);if(!r)return JS_ThrowTypeError(ctx,"Illegal WebAssembly.Table receiver");if(argc<2)return JS_ThrowTypeError(ctx,"Table.set requires index and value");uint64_t idx=0;if(JS_ToIndex(ctx,&idx,argv[0])<0)return JS_EXCEPTION;if(idx>UINT32_MAX)return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds");if(r->native_table){if(r->value_type==1){uint64_t handle=0;if(!native_externref_from_js(ctx,argv[1],&handle))return JS_ThrowOutOfMemory(ctx);if(!mlb_wasm_rt_table_resource_set_externref(r->native_table,(uint32_t)idx,handle)){WasmHost*h=host_for_ctx(ctx,0);if(h&&!h->native_call_depth)wasm_native_externref_cleanup(ctx,h);return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds");}if(!wasm_native_externref_commit(ctx))return JS_ThrowOutOfMemory(ctx);return JS_UNDEFINED;}MlbWasmInstance*target=NULL;uint32_t fi=UINT32_MAX;JSValue owner_obj=JS_UNDEFINED;if(!wasm_table_function_ref(ctx,argv[1],&target,&fi,&owner_obj))return wasm_named_error(ctx,"TypeError","Native WebAssembly.Table accepts null or a function from a native instance");int ok=mlb_wasm_rt_table_resource_set_ref(r->native_table,(uint32_t)idx,target,fi);if(ok){WasmInstance*owner=get_instance(ctx,r->owner_obj);int keep=target&&(!owner||owner->native!=target);if(!wasm_table_root_set(ctx,r,(uint32_t)idx,argv[1],keep))ok=0;}JS_FreeValue(ctx,owner_obj);if(!ok)return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds or invalid function reference");return JS_UNDEFINED;}if(!r->standalone){WasmInstance*owner=NULL;uint32_t ti=0;if(native_table_ref(ctx,r,&owner,&ti)){MlbWasmInstance*target=NULL;uint32_t fi=UINT32_MAX;JSValue target_obj=JS_UNDEFINED;if(!wasm_table_function_ref(ctx,argv[1],&target,&fi,&target_obj))return wasm_named_error(ctx,"TypeError","Native WebAssembly.Table accepts null or a function from a native instance");int ok=mlb_wasm_rt_table_set_ref(owner->native,(uint32_t)idx,target,fi);int keep=target&&target!=owner->native;if(ok&&!wasm_table_root_set(ctx,r,(uint32_t)idx,argv[1],keep))ok=0;JS_FreeValue(ctx,target_obj);if(!ok)return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds or invalid function reference");return JS_UNDEFINED;}}if(r->value_type==0&&!r->local_table_mode&&wasm_table_native_function(ctx,argv[1])){if(!wasm_table_enable_local(ctx,r))return wasm_named_error(ctx,"TypeError","Native WebAssembly function cannot be placed in a table already imported by the compatibility runtime");}if(r->local_table_mode){if(idx>=r->table_length)return wasm_named_error(ctx,"RangeError","WebAssembly.Table index out of bounds");if(!JS_IsNull(argv[1])&&!wasm_table_native_function(ctx,argv[1])){JSValue m=JS_GetPropertyStr(ctx,argv[1],"__mlbWasmExportRef");int old=JS_IsObject(m);JS_FreeValue(ctx,m);if(!old)return wasm_named_error(ctx,"TypeError","Table value must be null or a WebAssembly function");}JSValue nv=JS_DupValue(ctx,argv[1]);JS_FreeValue(ctx,r->table_values[idx]);r->table_values[idx]=nv;return JS_UNDEFINED;}WasmHost*h=host_for_ctx(ctx,1);SBuf b={0};sb_s(&b,"{\"cmd\":\"tableSet\",\"ref\":");ref_make_remote(r,&b);sb_s(&b,",\"index\":");sb_u64(&b,idx);sb_s(&b,",\"value\":");if(r->value_type==1)mlb_wasm_encode_externref(ctx,h,argv[1],&b);else mlb_wasm_encode_value(ctx,h,argv[1],&b);sb_ch(&b,'}');char err[512];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);JS_FreeValue(ctx,res);if(!ok){const char*k="TypeError";const char*m=strip_error_class(err,&k);return wasm_named_error(ctx,k,m);}return JS_UNDEFINED;}
static JSValue wasm_table_grow(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){WasmRef*r=get_ref(ctx,this_val,wasm_table_cid);if(!r)return JS_ThrowTypeError(ctx,"Illegal WebAssembly.Table receiver");uint64_t delta=0;if(argc&&JS_ToIndex(ctx,&delta,argv[0])<0)return JS_EXCEPTION;if(r->native_table){if(delta>UINT32_MAX)return wasm_named_error(ctx,"RangeError","WebAssembly.Table maximum exceeded");if(r->value_type==1){uint64_t handle=0;if(argc>1&&!native_externref_from_js(ctx,argv[1],&handle))return JS_ThrowOutOfMemory(ctx);int64_t old=mlb_wasm_rt_table_resource_grow_externref(r->native_table,(uint32_t)delta,handle);if(old<0){if(!host_for_ctx(ctx,0)||!host_for_ctx(ctx,0)->native_call_depth)wasm_native_externref_cleanup(ctx,host_for_ctx(ctx,0));return wasm_named_error(ctx,"RangeError","WebAssembly.Table maximum exceeded");}if(!wasm_native_externref_commit(ctx))return JS_ThrowOutOfMemory(ctx);return JS_NewInt64(ctx,old);}MlbWasmInstance*target=NULL;uint32_t fi=UINT32_MAX;JSValue target_obj=JS_UNDEFINED;if(!wasm_table_function_ref(ctx,argc>1?argv[1]:JS_NULL,&target,&fi,&target_obj))return wasm_named_error(ctx,"TypeError","Native WebAssembly.Table initializer must be null or a function from a native instance");int64_t old=mlb_wasm_rt_table_resource_grow_ref(r->native_table,(uint32_t)delta,target,fi);if(old<0){JS_FreeValue(ctx,target_obj);return wasm_named_error(ctx,"RangeError","WebAssembly.Table maximum exceeded");}if(delta&&!wasm_table_roots_ensure(ctx,r,(uint32_t)old+(uint32_t)delta)){JS_FreeValue(ctx,target_obj);return JS_ThrowOutOfMemory(ctx);}WasmInstance*owner=get_instance(ctx,r->owner_obj);int keep=target&&(!owner||owner->native!=target);if(keep)for(uint32_t z=(uint32_t)old;z<(uint32_t)old+(uint32_t)delta;z++)wasm_table_root_set(ctx,r,z,argc>1?argv[1]:JS_NULL,1);JS_FreeValue(ctx,target_obj);return JS_NewInt64(ctx,old);}if(r->local_table_mode){if(delta>1000000||delta>UINT32_MAX-r->table_length||(r->table_has_maximum&&delta>r->table_maximum-r->table_length))return wasm_named_error(ctx,"RangeError","WebAssembly.Table maximum exceeded");JSValue init=argc>1?argv[1]:JS_NULL;if(!JS_IsNull(init)&&!wasm_table_native_function(ctx,init))return wasm_named_error(ctx,"TypeError","Table initializer must be null or a WebAssembly function");uint32_t old=r->table_length,newlen=old+(uint32_t)delta;if(newlen>r->table_capacity){JSValue*q=realloc(r->table_values,(size_t)newlen*sizeof(JSValue));if(!q)return JS_ThrowOutOfMemory(ctx);r->table_values=q;r->table_capacity=newlen;}for(uint32_t n=old;n<newlen;n++)r->table_values[n]=JS_DupValue(ctx,init);r->table_length=newlen;return JS_NewUint32(ctx,old);}WasmHost*h=host_for_ctx(ctx,1);SBuf b={0};sb_s(&b,"{\"cmd\":\"tableGrow\",\"ref\":");ref_make_remote(r,&b);sb_s(&b,",\"delta\":");sb_u64(&b,delta);sb_s(&b,",\"value\":");if(argc>1){if(r->value_type==1)mlb_wasm_encode_externref(ctx,h,argv[1],&b);else mlb_wasm_encode_value(ctx,h,argv[1],&b);}else sb_s(&b,"null");sb_ch(&b,'}');char err[512];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);if(!ok){const char*k="RangeError";const char*m=strip_error_class(err,&k);JS_FreeValue(ctx,res);return wasm_named_error(ctx,k,m);}int prev=0;get_int_field(ctx,res,"previous",&prev);JS_FreeValue(ctx,res);return JS_NewInt32(ctx,prev);}
static JSValue wasm_table_ctor(JSContext*ctx,JSValueConst this_val,int argc,JSValueConst*argv){(void)this_val;if(argc<1||!JS_IsObject(argv[0]))return JS_ThrowTypeError(ctx,"WebAssembly.Table requires a descriptor");char element[32];if(!get_string_field(ctx,argv[0],"element",element,sizeof element))return JS_ThrowTypeError(ctx,"Table descriptor requires element");int native_funcref=!strcmp(element,"funcref")||!strcmp(element,"anyfunc");if(!native_funcref&&strcmp(element,"externref"))return JS_ThrowTypeError(ctx,"Invalid WebAssembly.Table element type");int initial=0,maximum=0,hasmax=0;get_int_field(ctx,argv[0],"initial",&initial);JSValue mv=JS_GetPropertyStr(ctx,argv[0],"maximum");if(!JS_IsUndefined(mv)){if(JS_ToInt32(ctx,&maximum,mv)<0){JS_FreeValue(ctx,mv);return JS_EXCEPTION;}hasmax=1;}JS_FreeValue(ctx,mv);if(initial<0||initial>1000000||maximum<0||maximum>1000000||(hasmax&&maximum<initial))return JS_ThrowRangeError(ctx,"Invalid WebAssembly.Table limits");if(native_funcref||!strcmp(element,"externref")){uint8_t et=native_funcref?MLB_WASM_FUNCREF:MLB_WASM_EXTERNREF;char err[256];MlbWasmTable*t=mlb_wasm_rt_table_new_typed((uint32_t)initial,hasmax,(uint32_t)maximum,et,err,sizeof err);if(!t)return wasm_named_error(ctx,"RangeError",err[0]?err:"Unable to allocate native WebAssembly.Table");JSValue o=make_ref_object(ctx,wasm_table_cid,1,0,JS_UNDEFINED,0,NULL);if(JS_IsException(o)){mlb_wasm_rt_table_release(t);return o;}WasmRef*r=JS_GetOpaque(o,wasm_table_cid);r->native_table=t;r->value_type=et==MLB_WASM_EXTERNREF?1:0;r->table_has_maximum=hasmax;r->table_maximum=(uint32_t)maximum;if(et==MLB_WASM_EXTERNREF){uint64_t handle=0;if(argc>1&&!native_externref_from_js(ctx,argv[1],&handle)){JS_FreeValue(ctx,o);return JS_ThrowOutOfMemory(ctx);}if(handle){for(uint32_t n=0;n<(uint32_t)initial;n++)if(!mlb_wasm_rt_table_resource_set_externref(t,n,handle)){JS_FreeValue(ctx,o);return JS_ThrowInternalError(ctx,"Unable to initialize native externref table");}}if(!wasm_native_externref_commit(ctx)){JS_FreeValue(ctx,o);return JS_ThrowOutOfMemory(ctx);}}else{if(!wasm_table_roots_ensure(ctx,r,(uint32_t)initial)){JS_FreeValue(ctx,o);return JS_ThrowOutOfMemory(ctx);}if(argc>1&&!JS_IsNull(argv[1])){MlbWasmInstance*target=NULL;uint32_t fi=UINT32_MAX;JSValue owner_obj=JS_UNDEFINED;if(!wasm_table_function_ref(ctx,argv[1],&target,&fi,&owner_obj)){JS_FreeValue(ctx,o);return JS_ThrowTypeError(ctx,"funcref table initializer must be null or a native WebAssembly function");}for(uint32_t n=0;n<(uint32_t)initial;n++){if(!mlb_wasm_rt_table_resource_set_ref(t,n,target,fi)||!wasm_table_root_set(ctx,r,n,argv[1],1)){JS_FreeValue(ctx,owner_obj);JS_FreeValue(ctx,o);return JS_ThrowOutOfMemory(ctx);}}JS_FreeValue(ctx,owner_obj);}}return o;}WasmHost*h=host_for_ctx(ctx,1);if(!h)return JS_ThrowOutOfMemory(ctx);int id=++h->next_ref;SBuf b={0};sb_s(&b,"{\"cmd\":\"tableCreate\",\"id\":");sb_i64(&b,id);sb_s(&b,",\"element\":");sb_json_string(&b,element,strlen(element));sb_s(&b,",\"initial\":");sb_i64(&b,initial);sb_s(&b,",\"hasMaximum\":");sb_s(&b,hasmax?"true":"false");sb_s(&b,",\"maximum\":");sb_i64(&b,maximum);sb_ch(&b,'}');char err[512];int ok=0;JSValue res=rpc_do(ctx,h,b.s,err,sizeof err,&ok);free(b.s);JS_FreeValue(ctx,res);if(!ok){const char*k="RangeError";const char*m=strip_error_class(err,&k);return wasm_named_error(ctx,k,m);}JSValue o=make_ref_object(ctx,wasm_table_cid,1,id,JS_UNDEFINED,0,NULL);WasmRef*r=JS_GetOpaque(o,wasm_table_cid);r->value_type=1;r->table_has_maximum=hasmax;r->table_maximum=(uint32_t)maximum;return o;}

void webpage_wasm_init(JSContext *ctx) {
    if(!ctx)return;
    if(!wasm_class_ids_ready){JS_NewClassID(&wasm_module_cid);JS_NewClassID(&wasm_instance_cid);JS_NewClassID(&wasm_memory_cid);JS_NewClassID(&wasm_global_cid);JS_NewClassID(&wasm_table_cid);wasm_class_ids_ready=1;}
    JSRuntime*rt=JS_GetRuntime(ctx);
    if(!JS_IsRegisteredClass(rt,wasm_module_cid)){wasm_module_class.finalizer=wasm_module_finalizer;wasm_module_class.gc_mark=wasm_module_gc_mark;JS_NewClass(rt,wasm_module_cid,&wasm_module_class);}
    if(!JS_IsRegisteredClass(rt,wasm_instance_cid)){wasm_instance_class.finalizer=wasm_instance_finalizer;wasm_instance_class.gc_mark=wasm_instance_gc_mark;JS_NewClass(rt,wasm_instance_cid,&wasm_instance_class);}
    if(!JS_IsRegisteredClass(rt,wasm_memory_cid))JS_NewClass(rt,wasm_memory_cid,&wasm_memory_class);
    if(!JS_IsRegisteredClass(rt,wasm_global_cid))JS_NewClass(rt,wasm_global_cid,&wasm_global_class);
    if(!JS_IsRegisteredClass(rt,wasm_table_cid))JS_NewClass(rt,wasm_table_cid,&wasm_table_class);

    JSValue wasm=JS_NewObject(ctx);
    JSValue mproto=JS_NewObject(ctx);JS_SetClassProto(ctx,wasm_module_cid,JS_DupValue(ctx,mproto));
    JSValue mctor=JS_NewCFunction2(ctx,wasm_module_ctor,"Module",1,JS_CFUNC_constructor,0);JS_SetConstructor(ctx,mctor,mproto);
    JS_SetPropertyStr(ctx,mctor,"imports",JS_NewCFunction(ctx,wasm_module_imports,"imports",1));JS_SetPropertyStr(ctx,mctor,"exports",JS_NewCFunction(ctx,wasm_module_exports,"exports",1));JS_SetPropertyStr(ctx,mctor,"customSections",JS_NewCFunction(ctx,wasm_module_custom_sections,"customSections",2));JS_SetPropertyStr(ctx,wasm,"Module",mctor);
    JSValue iproto=JS_NewObject(ctx);JS_SetClassProto(ctx,wasm_instance_cid,JS_DupValue(ctx,iproto));JSValue ictor=JS_NewCFunction2(ctx,wasm_instance_ctor,"Instance",2,JS_CFUNC_constructor,0);JS_SetConstructor(ctx,ictor,iproto);JS_SetPropertyStr(ctx,wasm,"Instance",ictor);
    JSValue memproto=JS_NewObject(ctx);JS_SetClassProto(ctx,wasm_memory_cid,JS_DupValue(ctx,memproto));JS_DefinePropertyGetSet(ctx,memproto,JS_NewAtom(ctx,"buffer"),JS_NewCFunction2(ctx,(JSCFunction*)wasm_memory_buffer_get,"get buffer",0,JS_CFUNC_getter,0),JS_UNDEFINED,JS_PROP_CONFIGURABLE);JS_SetPropertyStr(ctx,memproto,"grow",JS_NewCFunction(ctx,wasm_memory_grow,"grow",1));JSValue memctor=JS_NewCFunction2(ctx,wasm_memory_ctor,"Memory",1,JS_CFUNC_constructor,0);JS_SetConstructor(ctx,memctor,memproto);JS_SetPropertyStr(ctx,wasm,"Memory",memctor);
    JSValue gproto=JS_NewObject(ctx);JS_SetClassProto(ctx,wasm_global_cid,JS_DupValue(ctx,gproto));JS_DefinePropertyGetSet(ctx,gproto,JS_NewAtom(ctx,"value"),JS_NewCFunction2(ctx,(JSCFunction*)wasm_global_get,"get value",0,JS_CFUNC_getter,0),JS_NewCFunction2(ctx,(JSCFunction*)wasm_global_set,"set value",1,JS_CFUNC_setter,0),JS_PROP_CONFIGURABLE);JSValue gctor=JS_NewCFunction2(ctx,wasm_global_ctor,"Global",2,JS_CFUNC_constructor,0);JS_SetConstructor(ctx,gctor,gproto);JS_SetPropertyStr(ctx,wasm,"Global",gctor);
    JSValue tproto=JS_NewObject(ctx);JS_SetClassProto(ctx,wasm_table_cid,JS_DupValue(ctx,tproto));JS_DefinePropertyGetSet(ctx,tproto,JS_NewAtom(ctx,"length"),JS_NewCFunction2(ctx,(JSCFunction*)wasm_table_length_get,"get length",0,JS_CFUNC_getter,0),JS_UNDEFINED,JS_PROP_CONFIGURABLE);JS_SetPropertyStr(ctx,tproto,"get",JS_NewCFunction(ctx,wasm_table_get,"get",1));JS_SetPropertyStr(ctx,tproto,"set",JS_NewCFunction(ctx,wasm_table_set,"set",2));JS_SetPropertyStr(ctx,tproto,"grow",JS_NewCFunction(ctx,wasm_table_grow,"grow",2));JSValue tctor=JS_NewCFunction2(ctx,wasm_table_ctor,"Table",1,JS_CFUNC_constructor,0);JS_SetConstructor(ctx,tctor,tproto);JS_SetPropertyStr(ctx,wasm,"Table",tctor);
    JS_SetPropertyStr(ctx,wasm,"compile",JS_NewCFunction(ctx,wasm_compile,"compile",1));JS_SetPropertyStr(ctx,wasm,"instantiate",JS_NewCFunction(ctx,wasm_instantiate,"instantiate",2));JS_SetPropertyStr(ctx,wasm,"validate",JS_NewCFunction(ctx,wasm_validate,"validate",1));
    JS_SetPropertyStr(ctx,wasm,"CompileError",JS_NewCFunctionMagic(ctx,wasm_error_ctor,"CompileError",1,JS_CFUNC_constructor_magic,0));JS_SetPropertyStr(ctx,wasm,"LinkError",JS_NewCFunctionMagic(ctx,wasm_error_ctor,"LinkError",1,JS_CFUNC_constructor_magic,1));JS_SetPropertyStr(ctx,wasm,"RuntimeError",JS_NewCFunctionMagic(ctx,wasm_error_ctor,"RuntimeError",1,JS_CFUNC_constructor_magic,2));
    JSValue global=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,global,"WebAssembly",wasm);JS_FreeValue(ctx,global);
}

void webpage_wasm_free(JSContext *ctx) {
    WasmHost **p=&g_wasm_hosts;while(*p){if((*p)->ctx==ctx){WasmHost*h=*p;*p=h->next;
#ifndef _WIN32
        if(h->started&&h->pid>0){if(h->to_child>=0)close(h->to_child);if(h->from_child>=0)close(h->from_child);kill(h->pid,SIGTERM);int st;waitpid(h->pid,&st,0);}
#endif
        for(WasmCallback*c=h->callbacks;c;){WasmCallback*n=c->next;JS_FreeValue(ctx,c->fn);free(c);c=n;}
        for(WasmObjectRef*o=h->objects;o;){WasmObjectRef*n=o->next;JS_FreeValue(ctx,o->value);free(o);o=n;}
        for(WasmNativeObjectRef*o=h->native_objects;o;){WasmNativeObjectRef*n=o->next;JS_FreeValue(ctx,o->weak_or_primitive);if(o->has_temporary)JS_FreeValue(ctx,o->temporary_value);free(o);o=n;}
        JS_FreeValue(ctx,h->weakref_ctor);JS_FreeValue(ctx,h->weakref_deref);
        free(h->rx);WasmPendingRelease*pr=h->pending_release_head;while(pr){WasmPendingRelease*pn=pr->next;free(pr);pr=pn;}free(h);return;}p=&(*p)->next;}
}
