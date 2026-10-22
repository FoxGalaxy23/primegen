#include "worker.h"
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

bool webpage_mixed_blocked(const char* url);

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

static const char* WORKER_BOOTSTRAP =
"var self = globalThis; self.self = self; self.__listeners = Object.create(null);\n"
"self.addEventListener = function(t,f){ (self.__listeners[t] = self.__listeners[t] || []).push(f); };\n"
"self.removeEventListener = function(t,f){ var a = self.__listeners[t]; if (!a) return; var i = a.indexOf(f); if (i >= 0) a.splice(i, 1); };\n"
"self.__dispatch = function(t,ev){ if (typeof self['on'+t] === 'function') { try { self['on'+t](ev); } catch (e) { __nativeError(String(e && e.stack || e)); } }\n"
"  var a = self.__listeners[t]; if (a) for (var i = 0; i < a.length; i++) { try { a[i](ev); } catch (e) { __nativeError(String(e && e.stack || e)); } } };\n"
"self.postMessage = function(d){ __nativePost(JSON.stringify(d === undefined ? null : d)); };\n"
"self.close = function(){ __nativeClose(); };\n"
"self.importScripts = function(){ };\n"                                 /* честной загрузки доп. скриптов пока нет — не падаем */
"self.fetch = function(url){ return new Promise(function(res, rej){ var t = __nativeFetchSync(String(url)); if (t === null) rej(new TypeError('Failed to fetch')); else res({ ok: true, status: 200, text: function(){ return Promise.resolve(t); }, json: function(){ return Promise.resolve(JSON.parse(t)); } }); }); };\n"
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
    JSValue g = JS_GetGlobalObject(ctx); JSValue self = JS_GetPropertyStr(ctx, g, "self");
    JSValue ev = JS_NewObject(ctx); JS_SetPropertyStr(ctx, ev, "data", val);
    JSValue fn = JS_GetPropertyStr(ctx, self, "__dispatch");
    JSValue args[2] = { JS_NewString(ctx, "message"), ev };
    JSValue r = JS_Call(ctx, fn, self, 2, args);
    if (JS_IsException(r)) { JSValue exc = JS_GetException(ctx); const char* s = JS_ToCString(ctx, exc); if (s) { emit(g_out_fd, 'E', s, (uint32_t)strlen(s)); JS_FreeCString(ctx, s); } JS_FreeValue(ctx, exc); }
    JS_FreeValue(ctx, r); JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, ev); JS_FreeValue(ctx, fn); JS_FreeValue(ctx, self); JS_FreeValue(ctx, g);
}

static void child_main(const char* script_url, int out_fd, int in_fd) {
    g_out_fd = out_fd;
    char* src = webpage_mixed_blocked(script_url) ? NULL : nal_http_get(script_url);
    if (!src) { emit(out_fd, 'E', "worker: failed to load script", 30); _exit(0); }

    JSRuntime* rt = JS_NewRuntime(); JSContext* ctx = JS_NewContext(rt);
    JSValue g = JS_GetGlobalObject(ctx);
#define WFN(name, fn, n) JS_SetPropertyStr(ctx, g, name, JS_NewCFunction(ctx, fn, name, n))
    WFN("__nativePost", w_post, 1); WFN("__nativeLog", w_log, 1); WFN("__nativeError", w_error, 1); WFN("__nativeNow", w_now, 0); WFN("__nativeClose", w_close, 0); WFN("__nativeFetchSync", w_fetch_sync, 1);
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
