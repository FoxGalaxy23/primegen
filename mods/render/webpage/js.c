/* JavaScript для страниц: QuickJS + нативные DOM-примитивы (`__dom`).
   Весь пользовательский API (Element, Event, classList, style, fetch ...) живёт в js_prelude.js. */
#include "webpage.h"
#include "l2.h"
#include "fx.h"
#include "gal.h"
#include "nal.h"
#include "db.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef HAVE_OPENSSL_VAULT
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/file.h>
#else
#include <direct.h>
#endif

void* webpage_js_attach(DOMNode* head, const char* url, PageBundle* bundle);
void  webpage_js_free(void* page);
int   webpage_js_eval(void* page, const char* code, char* out, size_t cap);
int   webpage_js_tick(void* page);
int   webpage_js_tick_async(void* page);
int   webpage_js_poll(void* page, int allow_debugger_ui);
int   webpage_js_has_pending(void* page);
int   webpage_js_dom_commit_pending(void* page);
int   webpage_js_dispatch(void* page, DOMNode* target, const char* type, const char* key, int mods);
int   webpage_js_dispatch_mouse(void* page, DOMNode* target, const char* type, int client_x, int client_y, int button, int buttons, int mods);
int   webpage_js_dispatch_file_drop(void* page, DOMNode* target, const char* path, int client_x, int client_y);
int   webpage_js_before_submit(void* page, DOMNode* control);
const char* webpage_js_take_nav(void* page);
int   webpage_js_take_scroll(void* page, int* x, int* y);
int   webpage_js_take_focus(void* page, DOMNode** node);
void  webpage_js_set_focus(void* page, DOMNode* node);
const char* webpage_js_take_title(void* page);

#ifndef HAVE_QUICKJS
void* webpage_js_attach(DOMNode* h, const char* u, PageBundle* b) { (void)h; (void)u; webpage_bundle_free(b); return NULL; }
void  webpage_js_free(void* p) { (void)p; }
int   webpage_js_tick(void* p) { (void)p; return 0; }
int   webpage_js_tick_async(void* p) { (void)p; return 0; }
int   webpage_js_poll(void* p, int allow_debugger_ui) { (void)p; (void)allow_debugger_ui; return 0; }
int   webpage_js_has_pending(void* p) { (void)p; return 0; }
int   webpage_js_dom_commit_pending(void* p) { (void)p; return 0; }
int   webpage_js_dispatch(void* p, DOMNode* t, const char* ty, const char* k, int m) { (void)p; (void)t; (void)ty; (void)k; (void)m; return 0; }
int   webpage_js_dispatch_mouse(void* p, DOMNode* t, const char* ty, int x, int y, int b, int bs, int m) { (void)p; (void)t; (void)ty; (void)x; (void)y; (void)b; (void)bs; (void)m; return 0; }
int   webpage_js_dispatch_file_drop(void* p, DOMNode* t, const char* path, int x, int y) { (void)p; (void)t; (void)path; (void)x; (void)y; return 0; }
int   webpage_js_before_submit(void* p, DOMNode* c) { (void)p; (void)c; return 1; }
const char* webpage_js_take_nav(void* p) { (void)p; return NULL; }
int webpage_js_take_nav_request(void* page, char* method, size_t method_cap, char* headers, size_t headers_cap, unsigned char** body, size_t* body_len) { (void)page; (void)method; (void)method_cap; (void)headers; (void)headers_cap; if (body) *body = NULL; if (body_len) *body_len = 0; return 0; }
int   webpage_js_take_scroll(void* p, int* x, int* y) { (void)p; (void)x; (void)y; return 0; }
int   webpage_js_take_focus(void* p, DOMNode** n) { (void)p; (void)n; return 0; }
void  webpage_js_set_focus(void* p, DOMNode* n) { (void)p; (void)n; }
const char* webpage_js_take_title(void* p) { (void)p; return NULL; }
int   webpage_js_eval(void* p, const char* code, char* out, size_t cap) { (void)p;(void)code;if(out&&cap)snprintf(out,cap,"JavaScript unavailable");return 0; }
#else

#include "quickjs.h"
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#endif
#include "ws.h"
#include "ttf_render.h"
#include "font_manager.h"
#include "worker.h"
#include "media_backend.h"
#include "webrtc_backend.h"
#include "webgl_backend.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_write.h"
extern const char* webpage_js_prelude;
#ifndef _WIN32
/* Defined by the UI host. Weak so headless embedding/tests can auto-continue. */
extern int browser_debugger_pause_loop(void* js_page, const char* filename, const char* function_name,
                                       int line, int column, const char* stack,
                                       const char* locals, int is_breakpoint) __attribute__((weak));
extern void browser_debugger_pause_session_done(void* js_page) __attribute__((weak));
#endif
void webpage_wasm_init(JSContext *ctx);
void webpage_wasm_free(JSContext *ctx);
int webpage_form_url(DOMNode* control, DOMNode* submitter, char* out, int cap);
int css_media_matches(const char* query);

typedef struct { int parent, first, last, next, prev; } Lk;
typedef struct Timer { int id; JSValue fn; double due, interval; int repeat; struct Timer* next; } Timer;
typedef struct AReq { int id, used, streaming; NalAsyncReq nr; struct AReq* next; } AReq;
typedef struct WConn { int id, used; WsConn ws; struct WConn* next; } WConn;
typedef struct WkConn { int id, used; WorkerConn wk; struct WkConn* next; } WkConn;
typedef struct WebRtcEntry { MlbWebRtcPeer* backend; struct WebRtcEntry* next; } WebRtcEntry;
typedef struct DynamicImage { int node_id, kind, width, height, dirty; uint32_t* pixels; struct DynamicImage* next; } DynamicImage; /* kind 0=canvas, 1=video */
typedef struct MediaState { int node_id; char source[1200]; MediaInfo info; MediaBackend* backend; MediaByteQueue* stream_queue; struct MediaState* next; } MediaState;
typedef struct { char pattern[512]; char condition[256]; int line; } JSLineBreakpoint;
typedef struct Page {
    JSContext* ctx; DOMNode* head; PageBundle* bundle; char url[8192];
    DOMNode** nodes; Lk* lk; int nn, ncap;
    int lin_dirty, style_dirty; void* doc; int vw_seen, vh_seen;
    Timer* timers; int next_timer;
    char nav[1200]; int has_nav;
    char nav_method[8], nav_headers[256]; unsigned char* nav_body; size_t nav_body_len;
    int scroll_req, sx, sy; int focus_req, focus_id; char title[256]; int title_dirty;
    int cur_script; int active;
    AReq* areqs; int next_areq;
    WConn* wconns; int next_wconn;
    WkConn* wkconns; int next_wkconn;
    WebRtcEntry* webrtc_peers;
    DynamicImage* dynamic_images; int dynamic_images_dirty;
    MediaState* media;
    char drop_authorized_path[2048];
    uint32_t view_transition_pending_flags; /* capture, commit, skip-old, discard-pending */
    int debug_trace_enabled, debug_breakpoint_count;
    int debug_run_mode, debug_step_depth, debug_paused;
    JSLineBreakpoint debug_breakpoints[128];
} Page;

static JSRuntime* g_rt; static double g_deadline;
static int g_debug_hook_active;
static void debug_trace_hook(JSContext* ctx, const char* filename,
                             const char* function_name, int line, int column,
                             void* opaque);
static void debug_append_event(Page* p, const char* kind, const char* filename,
                               const char* function_name, int line, int column);
static void debug_pause_at_trace(JSContext* ctx, const char* filename,
                                 const char* function_name, int line, int column,
                                 int depth, int is_breakpoint);

/* Minimal dynamic SDL2 queued-audio output for Web Audio PCM. No SDL headers or
   build-time SDL dependency are required; unsupported systems simply report no device. */
#ifndef _WIN32
typedef uint32_t MSDL_AudioDeviceID;
typedef struct {
    int freq; uint16_t format; uint8_t channels, silence; uint16_t samples, padding;
    uint32_t size; void (*callback)(void*, uint8_t*, int); void* userdata;
} MSDL_AudioSpec;
typedef int (*MSDL_InitSubSystemFn)(uint32_t);
typedef MSDL_AudioDeviceID (*MSDL_OpenAudioDeviceFn)(const char*, int, const MSDL_AudioSpec*, MSDL_AudioSpec*, int);
typedef int (*MSDL_QueueAudioFn)(MSDL_AudioDeviceID, const void*, uint32_t);
typedef uint32_t (*MSDL_GetQueuedAudioSizeFn)(MSDL_AudioDeviceID);
typedef void (*MSDL_PauseAudioDeviceFn)(MSDL_AudioDeviceID, int);
typedef void (*MSDL_ClearQueuedAudioFn)(MSDL_AudioDeviceID);
typedef void (*MSDL_CloseAudioDeviceFn)(MSDL_AudioDeviceID);
static void* msdl_lib;
static MSDL_AudioDeviceID msdl_dev;
static MSDL_InitSubSystemFn msdl_init;
static MSDL_OpenAudioDeviceFn msdl_open;
static MSDL_QueueAudioFn msdl_queue;
static MSDL_GetQueuedAudioSizeFn msdl_queued;
static MSDL_PauseAudioDeviceFn msdl_pause;
static MSDL_ClearQueuedAudioFn msdl_clear;
static MSDL_CloseAudioDeviceFn msdl_close;
static int msdl_tried;
static int msdl_audio_start(int rate) {
    if (rate < 8000 || rate > 192000) rate = 44100;
    if (msdl_dev) return 1;
    if (!msdl_tried) {
        msdl_tried = 1;
        msdl_lib = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!msdl_lib) msdl_lib = dlopen("libSDL2.so", RTLD_NOW | RTLD_LOCAL);
        if (msdl_lib) {
#define MSDL_SYM(dst, name) do { *(void**)(&dst) = dlsym(msdl_lib, name); } while (0)
            MSDL_SYM(msdl_init, "SDL_InitSubSystem");
            MSDL_SYM(msdl_open, "SDL_OpenAudioDevice");
            MSDL_SYM(msdl_queue, "SDL_QueueAudio");
            MSDL_SYM(msdl_queued, "SDL_GetQueuedAudioSize");
            MSDL_SYM(msdl_pause, "SDL_PauseAudioDevice");
            MSDL_SYM(msdl_clear, "SDL_ClearQueuedAudio");
            MSDL_SYM(msdl_close, "SDL_CloseAudioDevice");
#undef MSDL_SYM
        }
    }
    if (!msdl_lib || !msdl_init || !msdl_open || !msdl_queue || !msdl_queued || !msdl_pause || !msdl_close) return 0;
    if (msdl_init(0x00000010u) != 0) return 0; /* SDL_INIT_AUDIO */
    MSDL_AudioSpec want, got; memset(&want, 0, sizeof want); memset(&got, 0, sizeof got);
    want.freq = rate;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    want.format = 0x9120; /* AUDIO_F32MSB */
#else
    want.format = 0x8120; /* AUDIO_F32LSB */
#endif
    want.channels = 2; want.samples = 512; want.callback = NULL;
    msdl_dev = msdl_open(NULL, 0, &want, &got, 0);
    if (!msdl_dev) return 0;
    if (got.freq != rate || got.channels != 2 || got.format != want.format) {
        msdl_close(msdl_dev); msdl_dev = 0; return 0;
    }
    if (msdl_clear) msdl_clear(msdl_dev);
    msdl_pause(msdl_dev, 0);
    return 1;
}
static void msdl_audio_stop(void) {
    if (msdl_dev) { if (msdl_clear) msdl_clear(msdl_dev); msdl_close(msdl_dev); msdl_dev = 0; }
}
#else
static int msdl_audio_start(int rate) { (void)rate; return 0; }
static void msdl_audio_stop(void) {}
#endif

static DOMNode* N(Page* p, int id);
static void flush_style(Page* p);
static DynamicImage* dyn_image(Page* p, int node_id, int kind, int create) {
    if (!p) return NULL;
    for (DynamicImage* im=p->dynamic_images; im; im=im->next) if (im->node_id==node_id && im->kind==kind) return im;
    if (!create) return NULL;
    DynamicImage* im=calloc(1,sizeof *im); if (!im) return NULL; im->node_id=node_id; im->kind=kind; im->next=p->dynamic_images; p->dynamic_images=im; return im;
}
static int dyn_image_set_rgba(Page* p, int node_id, int kind, const unsigned char* rgba, int w, int h) {
    if (!p || !rgba || w<1 || h<1 || (uint64_t)w*(uint64_t)h>4000000ull) return 0;
    DynamicImage* im=dyn_image(p,node_id,kind,1); if (!im) return 0;
    size_t count=(size_t)w*h; uint32_t* out=malloc(count*sizeof(uint32_t)); if (!out) return 0;
    for(size_t i=0;i<count;i++) out[i]=((uint32_t)rgba[i*4+3]<<24)|((uint32_t)rgba[i*4]<<16)|((uint32_t)rgba[i*4+1]<<8)|rgba[i*4+2];
    free(im->pixels); im->pixels=out; im->width=w; im->height=h; im->dirty=1; p->dynamic_images_dirty=1;
    DOMNode* n=N(p,node_id); if(n){ char key[64]; flush_style(p); snprintf(key,sizeof key,kind==1?"video:%d":"canvas:%d",n->idx); webpage_image_inject(key,out,w,h); }
    return 1;
}
static MediaState* media_state(Page* p,int node_id,int create) {
    for(MediaState*m=p?p->media:NULL;m;m=m->next)if(m->node_id==node_id)return m;
    if(!create||!p)return NULL;MediaState*m=calloc(1,sizeof *m);if(!m)return NULL;m->node_id=node_id;m->next=p->media;p->media=m;return m;
}
static void media_state_stop(MediaState*m){if(!m)return;if(m->backend){media_stop(m->backend);m->backend=NULL;}if(m->stream_queue){media_byte_queue_destroy(m->stream_queue);m->stream_queue=NULL;}}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6; }
static int interrupt_cb(JSRuntime* rt, void* op) { (void)rt; (void)op; return g_deadline > 0 && now_ms() > g_deadline; }
static Page* P(JSContext* c) { return JS_GetContextOpaque(c); }
static DOMNode* N(Page* p, int id) { return id >= 0 && id < p->nn ? p->nodes[id] : NULL; }
static int id_of(DOMNode* n) { return n ? (int)(intptr_t)n->aux - 1 : -1; }
static int arg_int(JSContext* c, JSValueConst v) { int32_t x = -1; JS_ToInt32(c, &x, v); return x; }
static JSValue js_str(JSContext* c, const char* s) { return s ? JS_NewString(c, s) : JS_NULL; }

typedef struct { char* p; size_t n, cap; } Buf;
static void b_add(Buf* b, const char* s, size_t n) { if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); } memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0; }
static void b_str(Buf* b, const char* s) { b_add(b, s, strlen(s)); }

/* ══════════════ дерево ══════════════ */
static int new_id(Page* p, DOMNode* n) {
    if (p->nn == p->ncap) { p->ncap = p->ncap ? p->ncap * 2 : 1024; p->nodes = realloc(p->nodes, (size_t)p->ncap * sizeof(DOMNode*)); p->lk = realloc(p->lk, (size_t)p->ncap * sizeof(Lk)); }
    p->nodes[p->nn] = n; n->aux = (void*)(intptr_t)(p->nn + 1);
    p->lk[p->nn].parent = p->lk[p->nn].first = p->lk[p->nn].last = p->lk[p->nn].next = p->lk[p->nn].prev = -1;
    return p->nn++;
}
static void mark_dirty(Page* p) { p->lin_dirty = 1; p->style_dirty = 1; if (p->doc) { webdoc_free(p->doc); p->doc = NULL; } }

static void unlink_node(Page* p, int id) {
    Lk* l = &p->lk[id]; if (l->parent < 0) return;
    Lk* pa = &p->lk[l->parent];
    if (l->prev >= 0) p->lk[l->prev].next = l->next; else pa->first = l->next;
    if (l->next >= 0) p->lk[l->next].prev = l->prev; else pa->last = l->prev;
    l->parent = l->prev = l->next = -1;
}
static void link_before(Page* p, int parent, int id, int ref) {
    unlink_node(p, id); Lk* l = &p->lk[id]; Lk* pa = &p->lk[parent];
    l->parent = parent;
    if (ref < 0 || p->lk[ref].parent != parent) { l->prev = pa->last; l->next = -1; if (pa->last >= 0) p->lk[pa->last].next = id; else pa->first = id; pa->last = id; }
    else { Lk* r = &p->lk[ref]; l->next = ref; l->prev = r->prev; if (r->prev >= 0) p->lk[r->prev].next = id; else pa->first = id; r->prev = id; }
}
static int is_ancestor(Page* p, int anc, int id) { for (int x = id; x >= 0; x = p->lk[x].parent) if (x == anc) return 1; return 0; }

/* Перестраивает idx/parent/subtree_end/next по дереву связей (порядок обхода документа). */
static void linearize(Page* p) {
    if (!p->lin_dirty) return;
    int root = id_of(p->head); int cap = p->nn ? p->nn : 1;
    int* order = malloc((size_t)cap * sizeof(int)); int* stack = malloc((size_t)cap * sizeof(int)); int* endidx = malloc((size_t)cap * sizeof(int));
    int n = 0, sp = 0; stack[sp++] = root;
    while (sp) { int id = stack[--sp]; order[n++] = id; int kids = 0; for (int c = p->lk[id].last; c >= 0; c = p->lk[c].prev) { if (sp < cap) stack[sp++] = c; kids++; } (void)kids; }
    int* pos = malloc((size_t)cap * sizeof(int)); for (int i = 0; i < p->nn; i++) pos[i] = -1;
    for (int i = 0; i < n; i++) pos[order[i]] = i;
    for (int i = 0; i < n; i++) { DOMNode* d = p->nodes[order[i]]; int par = p->lk[order[i]].parent; d->idx = i; d->parent = par >= 0 && pos[par] >= 0 ? pos[par] : -1; d->subtree_end = i; endidx[i] = i; d->next = i + 1 < n ? p->nodes[order[i + 1]] : NULL; }
    for (int i = n - 1; i > 0; i--) { int par = p->nodes[order[i]]->parent; if (par >= 0 && endidx[i] > endidx[par]) endidx[par] = endidx[i]; }
    for (int i = 0; i < n; i++) p->nodes[order[i]]->subtree_end = endidx[i];
    for (int i = 0; i < p->nn; i++) if (pos[i] < 0) { p->nodes[i]->idx = -1; p->nodes[i]->next = NULL; }   /* отсоединённые */
    free(order); free(stack); free(endidx); free(pos); p->lin_dirty = 0;
}

static SheetList sheets_for_page(Page* p, int* base_count);
static void free_page_sheets(Page* p, SheetList* sheets, int base_count);
static void flush_style(Page* p) {
    linearize(p);
    if (!p->style_dirty) return;
    int base_count = 0;
    SheetList sl = sheets_for_page(p, &base_count);
    webpage_recascade(p->head, &sl, p->bundle ? p->bundle->base : p->url);
    free_page_sheets(p, &sl, base_count);
    webpage_invalidate_layout(); p->style_dirty = 0;
}
static void flush_all(Page* p) { flush_style(p); webpage_force_layout_for(p ? p->head : NULL); }

/* ══════════════ атрибуты ══════════════ */
static void put(char* d, size_t cap, const char* s) { snprintf(d, cap, "%s", s); }
static int attrs_find(const char* a, const char* name, const char** vs, size_t* vl) {
    size_t nl = strlen(name);
    while (*a) { const char* sep = strchr(a, 1); if (!sep) break; const char* e = strchr(sep, 2); if (!e) break;
        if ((size_t)(sep - a) == nl && !strncmp(a, name, nl)) { *vs = sep + 1; *vl = (size_t)(e - sep - 1); return 1; } a = e + 1; }
    return 0;
}
static void attrs_del(DOMNode* n, const char* name) {
    char tmp[sizeof n->attrs]; size_t o = 0; const char* a = n->attrs; size_t nl = strlen(name);
    while (*a) { const char* sep = strchr(a, 1); if (!sep) break; const char* e = strchr(sep, 2); if (!e) break;
        if (!((size_t)(sep - a) == nl && !strncmp(a, name, nl))) { size_t l = (size_t)(e - a + 1); if (o + l < sizeof tmp) { memcpy(tmp + o, a, l); o += l; } } a = e + 1; }
    tmp[o] = 0; memcpy(n->attrs, tmp, o + 1);
}
static void attrs_set(DOMNode* n, const char* name, const char* val) {
    attrs_del(n, name); size_t nl = strlen(name), vl = strlen(val), l = strlen(n->attrs);
    if (vl > 160) vl = 160;
    if (l + nl + vl + 3 < sizeof n->attrs) { memcpy(n->attrs + l, name, nl); n->attrs[l + nl] = 1; memcpy(n->attrs + l + nl + 1, val, vl); n->attrs[l + nl + 1 + vl] = 2; n->attrs[l + nl + vl + 2] = 0; }
}
static int is_bool_attr(const char* k) { return !strcmp(k, "checked") || !strcmp(k, "disabled") || !strcmp(k, "selected") || !strcmp(k, "readonly") || !strcmp(k, "multiple"); }

static int attr_get(Page* p, DOMNode* n, const char* k, char* out, size_t cap) {
    (void)p; out[0] = 0;
    if (!strcmp(k, "id")) { put(out, cap, n->id); return n->id[0] != 0; }
    if (!strcmp(k, "class")) { put(out, cap, n->class_name); return n->class_name[0] != 0; }
    if (!strcmp(k, "style")) { put(out, cap, n->style_str); return n->style_str[0] != 0; }
    if (!strcmp(k, "srcset") && n->srcset[0]) { put(out, cap, n->srcset); return 1; }
    if (!strcmp(k, "sizes") && n->sizes[0]) { put(out, cap, n->sizes); return 1; }
    const char* vs; size_t vl;
    if (attrs_find(n->attrs, k, &vs, &vl)) { size_t l = vl < cap - 1 ? vl : cap - 1; memcpy(out, vs, l); out[l] = 0; return 1; }
    if (!strcmp(k, "href") && n->href[0]) { put(out, cap, n->href); return 1; }
    if (!strcmp(k, "src") && n->src[0]) { put(out, cap, n->src); return 1; }
    if (!strcmp(k, "value") && n->value[0]) { put(out, cap, n->value); return 1; }
    return 0;
}
static void attr_set(Page* p, DOMNode* n, const char* k, const char* v) {
    if (!strcmp(k, "class")) { put(n->class_name, sizeof n->class_name, v); return; }
    if (!strcmp(k, "style")) { put(n->style_str, sizeof n->style_str, v); return; }
    if (!strcmp(k, "id")) put(n->id, sizeof n->id, v);
    else if (!strcmp(k, "href")) webpage_resolve_url(p->url, v, n->href, sizeof n->href);
    else if (!strcmp(k, "src")) webpage_resolve_url(p->url, v, n->src, sizeof n->src);
    else if (!strcmp(k, "srcset")) put(n->srcset, sizeof n->srcset, v);
    else if (!strcmp(k, "sizes")) put(n->sizes, sizeof n->sizes, v);
    else if (!strcmp(k, "value")) put(n->value, sizeof n->value, v);
    else if (!strcmp(k, "name")) put(n->name, sizeof n->name, v);
    else if (!strcmp(k, "type")) { put(n->input_type, sizeof n->input_type, v); for (char* c = n->input_type; *c; c++) *c = (char)tolower((unsigned char)*c); }
    else if (!strcmp(k, "placeholder")) put(n->placeholder, sizeof n->placeholder, v);
    else if (!strcmp(k, "alt")) put(n->alt, sizeof n->alt, v);
    else if (!strcmp(k, "onclick")) put(n->onclick, sizeof n->onclick, v);
    else if (!strcmp(k, "checked")) n->checked = 1; else if (!strcmp(k, "disabled")) n->disabled = 1;
    else if (!strcmp(k, "selected")) n->selected = 1; else if (!strcmp(k, "readonly")) n->readonly = 1; else if (!strcmp(k, "multiple")) n->multiple = 1;
    else if (!strcmp(k, "colspan")) n->colspan = atoi(v) > 0 ? atoi(v) : 1; else if (!strcmp(k, "rowspan")) n->rowspan = atoi(v) > 0 ? atoi(v) : 1;
    else if (!strcmp(k, "width")) n->attr_w = atoi(v); else if (!strcmp(k, "height")) n->attr_h = atoi(v);
    attrs_set(n, k, v);
}
static void attr_remove(DOMNode* n, const char* k) {
    if (!strcmp(k, "class")) n->class_name[0] = 0; else if (!strcmp(k, "style")) n->style_str[0] = 0; else if (!strcmp(k, "id")) n->id[0] = 0;
    else if (!strcmp(k, "href")) n->href[0] = 0; else if (!strcmp(k, "src")) n->src[0] = 0;
    else if (!strcmp(k, "srcset")) n->srcset[0] = 0; else if (!strcmp(k, "sizes")) n->sizes[0] = 0; else if (!strcmp(k, "value")) n->value[0] = 0;
    else if (!strcmp(k, "checked")) n->checked = 0; else if (!strcmp(k, "disabled")) n->disabled = 0; else if (!strcmp(k, "selected")) n->selected = 0;
    else if (!strcmp(k, "readonly")) n->readonly = 0; else if (!strcmp(k, "multiple")) n->multiple = 0; else if (!strcmp(k, "onclick")) n->onclick[0] = 0;
    attrs_del(n, k);
}

/* ══════════════ текст и HTML ══════════════ */
static void text_of(Page* p, int id, Buf* b) {
    DOMNode* n = p->nodes[id];
    if (n->type == TAG_TEXT) { b_str(b, n->text); return; }
    if (p->lk[id].first < 0 && n->text[0]) { b_str(b, n->text); return; }     /* title/textarea/script: текст хранится в узле */
    for (int c = p->lk[id].first; c >= 0; c = p->lk[c].next) text_of(p, c, b);
}
static int has_shadow_host(Page* p, int id) {
    for (int par = p->lk[id].parent; par >= 0; par = p->lk[par].parent) {
        const char* value = NULL; size_t length = 0;
        if (attrs_find(p->nodes[par]->attrs, "data-ml-shadow-root", &value, &length)) return 1;
    }
    return 0;
}
static SheetList sheets_for_page(Page* p, int* base_count) {
    int nbase = p->bundle ? p->bundle->nsheets : 0, extra = 0;
    SheetSrc* base = p->bundle ? p->bundle->sheets : NULL;
    for (int i = 0; i < p->nn; i++) {
        DOMNode* n = p->nodes[i];
        if (n && n->idx >= 0 && !strcmp(n->tag, "style") && has_shadow_host(p, i)) extra++;
    }
    SheetSrc* all = calloc((size_t)nbase + (size_t)extra + 1, sizeof(SheetSrc));
    if (!all) { *base_count = nbase; return (SheetList){ base, nbase, nbase }; }
    if (nbase && base) memcpy(all, base, (size_t)nbase * sizeof(SheetSrc));
    int out = nbase;
    for (int i = 0; i < p->nn; i++) {
        DOMNode* n = p->nodes[i];
        if (!n || n->idx < 0 || strcmp(n->tag, "style") || !has_shadow_host(p, i)) continue;
        Buf css = {0}; text_of(p, i, &css);
        all[out].css = css.p ? css.p : strdup(""); all[out].href = NULL;
        const char* value = NULL; size_t length = 0;
        if (attrs_find(n->attrs, "media", &value, &length)) {
            if (length >= sizeof all[out].media) length = sizeof all[out].media - 1;
            memcpy(all[out].media, value, length); all[out].media[length] = 0;
        }
        out++;
    }
    *base_count = nbase;
    return (SheetList){ all, out, out };
}
static void free_page_sheets(Page* p, SheetList* sheets, int base_count) {
    if (!sheets || !sheets->v) return;
    if (p->bundle && sheets->v == p->bundle->sheets) return;
    for (int i = base_count; i < sheets->n; i++) free(sheets->v[i].css);
    free(sheets->v);
}
static void esc(Buf* b, const char* s, int attr) {
    for (; *s; s++) { if (*s == '&') b_str(b, "&amp;"); else if (*s == '<') b_str(b, "&lt;"); else if (*s == '>') b_str(b, "&gt;"); else if (attr && *s == '"') b_str(b, "&quot;"); else b_add(b, s, 1); }
}
static int is_void_tag(const char* t) { static const char* v = " area base br col embed hr img input link meta param source track wbr "; char k[40]; snprintf(k, sizeof k, " %s ", t); return strstr(v, k) != NULL; }
static void ser(Page* p, int id, Buf* b, int outer) {
    DOMNode* n = p->nodes[id];
    if (n->type == TAG_TEXT) { if (p->lk[id].parent >= 0 && (!strcmp(p->nodes[p->lk[id].parent]->tag, "script") || !strcmp(p->nodes[p->lk[id].parent]->tag, "style"))) b_str(b, n->text); else esc(b, n->text, 0); return; }
    if (outer) {
        b_str(b, "<"); b_str(b, n->tag);
        if (n->id[0] && !strstr(n->attrs, "id\x01")) { b_str(b, " id=\""); esc(b, n->id, 1); b_str(b, "\""); }
        if (n->class_name[0]) { b_str(b, " class=\""); esc(b, n->class_name, 1); b_str(b, "\""); }
        if (n->style_str[0]) { b_str(b, " style=\""); esc(b, n->style_str, 1); b_str(b, "\""); }
        const char* a = n->attrs;
        while (*a) { const char* sep = strchr(a, 1); if (!sep) break; const char* e = strchr(sep, 2); if (!e) break;
            b_str(b, " "); b_add(b, a, (size_t)(sep - a)); if (e > sep + 1) { b_str(b, "=\""); char* v = malloc((size_t)(e - sep)); memcpy(v, sep + 1, (size_t)(e - sep - 1)); v[e - sep - 1] = 0; esc(b, v, 1); free(v); b_str(b, "\""); } a = e + 1; }
        b_str(b, ">"); if (is_void_tag(n->tag)) return;
    }
    if (n->type == TAG_TEXTAREA && n->value[0] && p->lk[id].first < 0) esc(b, n->value, 0);
    for (int c = p->lk[id].first; c >= 0; c = p->lk[c].next) ser(p, c, b, 1);
    if (outer) { b_str(b, "</"); b_str(b, n->tag); b_str(b, ">"); }
}

static int add_text_nodes(Page* p, int parent, const char* s) {
    size_t len = strlen(s), pos = 0; int first = -1;
    while (pos < len) {
        size_t take = len - pos; if (take > 500) { take = 500; while (take > 100 && (s[pos + take] & 0xC0) == 0x80) take--; }
        DOMNode* t = webpage_new_node("#text"); char tmp[512]; snprintf(tmp, sizeof tmp, "%.*s", (int)take, s + pos); put(t->text, sizeof t->text, tmp);
        int id = new_id(p, t); if (first < 0) first = id; link_before(p, parent, id, -1); pos += take;
    }
    return first;
}
static void clear_children(Page* p, int id) { while (p->lk[id].first >= 0) unlink_node(p, p->lk[id].first); }

/* Разбирает HTML-фрагмент и подвешивает узлы к parent (перед ref). */
static void import_fragment(Page* p, int parent, int ref, const char* html) {
    size_t hl = strlen(html); char* wrap = malloc(hl + 32); snprintf(wrap, hl + 32, "<!doctype html><body>%s", html);
    SheetList sl = {0}; DOMNode* tmp = html_build(wrap, p->url, &sl); free(wrap);
    for (int i = 0; i < sl.n; i++) { free(sl.v[i].css); free(sl.v[i].href); } free(sl.v);
    if (!tmp) return;
    int n = 0; for (DOMNode* q = tmp; q; q = q->next) n++;
    DOMNode** nv = malloc((size_t)n * sizeof(DOMNode*)); int* ids = malloc((size_t)n * sizeof(int)); { int i = 0; for (DOMNode* q = tmp; q; q = q->next) nv[i++] = q; }
    int body = -1; for (int i = 1; i < n; i++) if (nv[i]->parent == 0 && !strcmp(nv[i]->tag, "body")) { body = i; break; }
    for (int i = 0; i < n; i++) ids[i] = -1;
    int body_end = body >= 0 ? nv[body]->subtree_end : -1;           /* до цикла: сам узел body освобождается на своей итерации */
    for (int i = 0; i < n; i++) {
        DOMNode* q = nv[i]; int inside = body >= 0 && i > body && i <= body_end;
        if (!inside) { free(q); continue; }
        q->next = NULL; ids[i] = new_id(p, q);
        int pi = q->parent; int target = pi == body ? parent : ids[pi];
        if (target < 0) continue;
        link_before(p, target, ids[i], pi == body ? ref : -1);
    }
    free(nv); free(ids);
}

/* ══════════════ вспомогательное для JS ══════════════ */
static JSValue ids_array(JSContext* c, int* ids, int n) { JSValue a = JS_NewArray(c); for (int i = 0; i < n; i++) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewInt32(c, ids[i])); return a; }
#define FN(name) static JSValue name(JSContext* c, JSValueConst t __attribute__((unused)), int argc __attribute__((unused)), JSValueConst* argv __attribute__((unused)))
#define NODE(i) DOMNode* n = N(P(c), arg_int(c, argv[i])); if (!n) return JS_NULL

static void log_exception(JSContext* c, const char* what) {
    JSValue ex = JS_GetException(c); const char* s = JS_ToCString(c, ex); JSValue st = JS_GetPropertyStr(c, ex, "stack"); const char* ss = JS_IsUndefined(st) ? NULL : JS_ToCString(c, st);
    gal_log("ERROR", "[JS] %s: %s%s%.300s", what, s ? s : "?", ss ? "\n" : "", ss ? ss : "");
    Page* page = (Page*)JS_GetContextOpaque(c);
    if (page && (page->debug_trace_enabled || page->debug_breakpoint_count)) {
        char summary[1200];
        snprintf(summary, sizeof(summary), "%s%s%s", s ? s : "JavaScript exception", ss ? "\n" : "", ss ? ss : "");
        debug_append_event(page, "EXCEPTION", what ? what : "<unknown>", summary, 0, 0);
    }
    if (ss) JS_FreeCString(c, ss); if (s) JS_FreeCString(c, s); JS_FreeValue(c, st); JS_FreeValue(c, ex);
}
/* Source-line tracing and breakpoint matching for the Sources panel. Conditions
   are intentionally evaluated by a tiny native grammar against primitive locals;
   arbitrary JS evaluation here would re-enter the active QuickJS interpreter. */
static void debug_clean_text(char* out, size_t cap, const char* in) {
    if (!out || cap == 0) return;
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        out[j++] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : (char)c;
    }
    out[j] = 0;
}

static void debug_reload_config(Page* p) {
    if (!p) return;
    int trace = 0, count = 0;
    JSLineBreakpoint parsed[128];
    memset(parsed, 0, sizeof(parsed));
    FILE* f = fopen("cache/js_debugger.cfg", "rb");
    if (f) {
        char row[1200];
        while (fgets(row, sizeof(row), f)) {
            row[strcspn(row, "\r\n")] = 0;
            if (!strncmp(row, "TRACE\t", 6)) {
                trace = atoi(row + 6) != 0;
            } else if (!strncmp(row, "BP\t", 3) && count < 128) {
                char* line_text = row + 3;
                char* tab = strchr(line_text, '\t');
                if (!tab) continue;
                *tab++ = 0;
                int line = atoi(line_text);
                char* condition = strchr(tab, '\t');
                if (condition) *condition++ = 0;
                if (line <= 0 || !*tab) continue;
                snprintf(parsed[count].pattern, sizeof(parsed[count].pattern), "%s", tab);
                if (condition) snprintf(parsed[count].condition, sizeof(parsed[count].condition), "%s", condition);
                parsed[count].line = line;
                count++;
            }
        }
        fclose(f);
    }
    p->debug_trace_enabled = trace;
    p->debug_breakpoint_count = count;
    if (count) memcpy(p->debug_breakpoints, parsed, (size_t)count * sizeof(parsed[0]));
    int active = trace || count > 0;
    if (g_rt && active != g_debug_hook_active) {
        JS_SetExecutionTraceHook(g_rt, active ? debug_trace_hook : NULL, NULL);
        g_debug_hook_active = active;
    }
}

static void debug_append_event(Page* p, const char* kind, const char* filename,
                               const char* function_name, int line, int column) {
    if (!p) return;
#ifdef _WIN32
    (void)_mkdir("cache");
#else
    (void)mkdir("cache", 0700);
#endif
    struct stat st;
    if (stat("cache/js_debug_events.log", &st) == 0 && st.st_size > 768 * 1024) {
        FILE* trim = fopen("cache/js_debug_events.log", "wb");
        if (trim) fclose(trim);
    }
    FILE* f = fopen("cache/js_debug_events.log", "ab");
    if (!f) return;
#ifndef _WIN32
    (void)flock(fileno(f), LOCK_EX);
#endif
    char page_text[1200], file_text[640], fn_text[768];
    debug_clean_text(page_text, sizeof(page_text), p->url);
    debug_clean_text(file_text, sizeof(file_text), filename);
    debug_clean_text(fn_text, sizeof(fn_text), function_name);
    fprintf(f, "%.0f\t%s\t%s\t%s\t%s\t%d\t%d\n",
            now_ms(), kind ? kind : "LINE", page_text, file_text,
            fn_text, line, column);
    fflush(f);
#ifndef _WIN32
    (void)flock(fileno(f), LOCK_UN);
#endif
    fclose(f);
}

typedef struct { int type; char text[512]; } DebugConditionValue;

static void debug_trim(char* s) {
    if (!s) return;
    char* a = s; while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
}

static int debug_identifier(const char* s) {
    if (!s || !(*s == '_' || *s == '$' || isalpha((unsigned char)*s))) return 0;
    for (s++; *s; s++) if (!(*s == '_' || *s == '$' || isalnum((unsigned char)*s))) return 0;
    return 1;
}

static int debug_read_literal(const char* raw, DebugConditionValue* out) {
    char buf[512];
    if (!raw || !out) return 0;
    snprintf(buf, sizeof(buf), "%s", raw); debug_trim(buf);
    memset(out, 0, sizeof(*out));
    size_t n = strlen(buf);
    if (n >= 2 && ((buf[0] == '\'' && buf[n-1] == '\'') || (buf[0] == '"' && buf[n-1] == '"'))) {
        buf[n-1] = 0; snprintf(out->text, sizeof(out->text), "%s", buf+1); out->type = JS_DEBUG_VALUE_STRING; return 1;
    }
    if (!strcmp(buf, "true") || !strcmp(buf, "false")) {
        out->type = JS_DEBUG_VALUE_BOOLEAN; snprintf(out->text, sizeof(out->text), "%s", buf); return 1;
    }
    if (!strcmp(buf, "null")) { out->type = JS_DEBUG_VALUE_NULL; strcpy(out->text, "null"); return 1; }
    if (!strcmp(buf, "undefined")) { out->type = JS_DEBUG_VALUE_UNDEFINED; strcpy(out->text, "undefined"); return 1; }
    char* end = NULL; (void)strtod(buf, &end);
    if (buf[0] && end && end != buf && *end == 0) {
        out->type = JS_DEBUG_VALUE_NUMBER; snprintf(out->text, sizeof(out->text), "%s", buf); return 1;
    }
    return 0;
}

static int debug_value_truthy(const DebugConditionValue* v) {
    if (!v) return 0;
    switch (v->type) {
    case JS_DEBUG_VALUE_UNDEFINED: case JS_DEBUG_VALUE_NULL: case JS_DEBUG_VALUE_UNINITIALIZED: return 0;
    case JS_DEBUG_VALUE_BOOLEAN: return !strcmp(v->text, "true");
    case JS_DEBUG_VALUE_NUMBER: { char* e=NULL; double d=strtod(v->text,&e); return e != v->text && d != 0.0 && !isnan(d); }
    case JS_DEBUG_VALUE_STRING: return v->text[0] != 0;
    case JS_DEBUG_VALUE_BIGINT: { char* e=NULL; long double d=strtold(v->text,&e); return e != v->text && d != 0.0L; }
    default: return 1;
    }
}

static int debug_value_number(const DebugConditionValue* v, double* out) {
    if (!v || !out) return 0;
    if (v->type == JS_DEBUG_VALUE_NUMBER || v->type == JS_DEBUG_VALUE_BIGINT) {
        char* end=NULL; double d=strtod(v->text,&end);
        if (end == v->text || !end || *end) return 0;
        *out=d; return 1;
    }
    if (v->type == JS_DEBUG_VALUE_BOOLEAN) { *out=!strcmp(v->text,"true")?1.0:0.0; return 1; }
    if (v->type == JS_DEBUG_VALUE_NULL) { *out=0.0; return 1; }
    if (v->type == JS_DEBUG_VALUE_STRING) {
        char* end=NULL; double d=strtod(v->text,&end);
        if (end == v->text) return 0;
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end) return 0;
        *out=d; return 1;
    }
    return 0;
}

/* Safe, intentionally small expression grammar: `local`, `!local`, or a
   primitive comparison (`local === 2`, `name == 'text'`, `x >= 1`). It never
   evaluates arbitrary JavaScript or invokes getters while the VM is paused. */
static int debug_eval_condition(JSContext* ctx, const char* condition) {
    char expr[256], lhs[128], rhs[128];
    const char* ops[] = {"===", "!==", "==", "!=", ">=", "<=", ">", "<"};
    const char* op = NULL; char* op_at = NULL;
    if (!condition || !*condition) return 1;
    snprintf(expr, sizeof(expr), "%s", condition); debug_trim(expr);
    if (!expr[0]) return 1;
    if (expr[0] == '!') {
        snprintf(lhs, sizeof(lhs), "%s", expr+1); debug_trim(lhs);
        if (!debug_identifier(lhs)) return -1;
        DebugConditionValue v; int type=0;
        if (!JS_GetCurrentLocalValue(ctx, lhs, v.text, sizeof(v.text), &type)) return 0;
        v.type=type; return !debug_value_truthy(&v);
    }
    for (size_t i=0; i<sizeof(ops)/sizeof(ops[0]); i++) {
        op_at = strstr(expr, ops[i]);
        if (op_at) { op=ops[i]; break; }
    }
    if (!op) {
        snprintf(lhs, sizeof(lhs), "%s", expr); debug_trim(lhs);
        if (!debug_identifier(lhs)) return -1;
        DebugConditionValue v; int type=0;
        if (!JS_GetCurrentLocalValue(ctx, lhs, v.text, sizeof(v.text), &type)) return 0;
        v.type=type; return debug_value_truthy(&v);
    }
    size_t left_len=(size_t)(op_at-expr);
    if (!left_len || left_len >= sizeof(lhs)) return -1;
    memcpy(lhs, expr, left_len); lhs[left_len]=0; debug_trim(lhs);
    snprintf(rhs, sizeof(rhs), "%s", op_at+strlen(op)); debug_trim(rhs);
    if (!debug_identifier(lhs)) return -1;
    DebugConditionValue a, b; int type=0;
    if (!JS_GetCurrentLocalValue(ctx, lhs, a.text, sizeof(a.text), &type)) return 0;
    a.type=type;
    if (!debug_read_literal(rhs, &b)) return -1;
    int strict = !strcmp(op, "===") || !strcmp(op, "!==");
    int negate = !strcmp(op, "!==") || !strcmp(op, "!=");
    if (!strcmp(op, "==") || !strcmp(op, "===") || !strcmp(op, "!=") || !strcmp(op, "!==")) {
        int equal=0;
        if (a.type == b.type) {
            if (a.type == JS_DEBUG_VALUE_NUMBER || a.type == JS_DEBUG_VALUE_BIGINT) {
                double av,bv; equal=debug_value_number(&a,&av)&&debug_value_number(&b,&bv)&&av==bv;
            } else equal=!strcmp(a.text,b.text);
        } else if (!strict && ((a.type==JS_DEBUG_VALUE_NULL && b.type==JS_DEBUG_VALUE_UNDEFINED) || (a.type==JS_DEBUG_VALUE_UNDEFINED && b.type==JS_DEBUG_VALUE_NULL))) equal=1;
        else if (!strict) { double av,bv; equal=debug_value_number(&a,&av)&&debug_value_number(&b,&bv)&&av==bv; }
        return negate ? !equal : equal;
    }
    if (a.type == JS_DEBUG_VALUE_STRING && b.type == JS_DEBUG_VALUE_STRING) {
        int cmp=strcmp(a.text,b.text);
        if (!strcmp(op,">")) return cmp>0; if (!strcmp(op,"<")) return cmp<0;
        if (!strcmp(op,">=")) return cmp>=0; return cmp<=0;
    }
    double av,bv; if (!debug_value_number(&a,&av) || !debug_value_number(&b,&bv)) return -1;
    if (!strcmp(op,">")) return av>bv; if (!strcmp(op,"<")) return av<bv;
    if (!strcmp(op,">=")) return av>=bv; return av<=bv;
}

static void debug_trace_hook(JSContext* ctx, const char* filename,
                             const char* function_name, int line, int column,
                             void* opaque) {
    (void)opaque;
    Page* p = (Page*)JS_GetContextOpaque(ctx);
    if (!p || line <= 0) return;
    int hit = 0;
    for (int i = 0; i < p->debug_breakpoint_count; i++) {
        JSLineBreakpoint* bp = &p->debug_breakpoints[i];
        /* Inline DOM scripts may be compiled through the shared prelude and
           expose that internal filename in QuickJS debug metadata. Also match
           the owning document URL so the UI's default source pattern works. */
        /* Match the compiled source filename only. Falling back to the owning
           page URL also matches shared <prelude> bytecode and can stop at an
           unrelated internal line with the same line number. Inline page
           scripts are compiled with the document URL in their debug metadata. */
        if (bp->line == line && bp->pattern[0] &&
            strstr(filename ? filename : "", bp->pattern)) {
            int condition_result = debug_eval_condition(ctx, bp->condition);
            if (condition_result < 0) {
                debug_append_event(p, "COND_ERROR", filename, bp->condition, line, column);
                continue; /* invalid/unsupported conditions fail closed */
            }
            if (condition_result) { hit = 1; break; }
        }
    }
    int depth = JS_GetCurrentStackDepth(ctx);
    int step_hit = 0;
    if (p->debug_run_mode == 1) step_hit = 1;                    /* step into */
    else if (p->debug_run_mode == 2 && depth <= p->debug_step_depth) step_hit = 1; /* over */
    else if (p->debug_run_mode == 3 && depth < p->debug_step_depth) step_hit = 1; /* out */
    if (p->debug_trace_enabled)
        debug_append_event(p, "LINE", filename, function_name, line, column);
    if (hit)
        debug_append_event(p, "BREAK", filename, function_name, line, column);
    else if (step_hit)
        debug_append_event(p, "STEP", filename, function_name, line, column);
#ifndef _WIN32
    if ((hit || step_hit) && !p->debug_paused) {
        p->debug_run_mode = 0;
        debug_pause_at_trace(ctx, filename, function_name, line, column, depth, hit);
    }
#else
    (void)depth; (void)step_hit; /* Windows process-isolation/debug UI transport not wired yet. */
#endif
}

static void rejection_cb(JSContext* c, JSValueConst promise, JSValueConst reason, JS_BOOL handled, void* op) {
    (void)promise; (void)op; if (handled) return; const char* s = JS_ToCString(c, reason); gal_log("WARN", "[JS] Uncaught (in promise): %.400s", s ? s : "?"); if (s) JS_FreeCString(c, s);
}
static void run_jobs(void) {
    JSContext* jc; int r;
    while ((r = JS_ExecutePendingJob(g_rt, &jc)) > 0) {}
    if (r < 0 && jc) log_exception(jc, "promise job");
}

/* ── дерево ── */
FN(d_tag) { NODE(0); return JS_NewString(c, n->tag); }
FN(d_type) { NODE(0); return JS_NewInt32(c, n->type == TAG_TEXT ? 3 : 1); }
FN(d_parent) { Page* p = P(c); int id = arg_int(c, argv[0]); if (id < 0 || id >= p->nn) return JS_NewInt32(c, -1); return JS_NewInt32(c, p->lk[id].parent); }
FN(d_first) { Page* p = P(c); int id = arg_int(c, argv[0]); return JS_NewInt32(c, id >= 0 && id < p->nn ? p->lk[id].first : -1); }
FN(d_last) { Page* p = P(c); int id = arg_int(c, argv[0]); return JS_NewInt32(c, id >= 0 && id < p->nn ? p->lk[id].last : -1); }
FN(d_next) { Page* p = P(c); int id = arg_int(c, argv[0]); return JS_NewInt32(c, id >= 0 && id < p->nn ? p->lk[id].next : -1); }
FN(d_prev) { Page* p = P(c); int id = arg_int(c, argv[0]); return JS_NewInt32(c, id >= 0 && id < p->nn ? p->lk[id].prev : -1); }
FN(d_children) {
    Page* p = P(c); int id = arg_int(c, argv[0]); JSValue a = JS_NewArray(c); if (id < 0 || id >= p->nn) return a; uint32_t i = 0;
    for (int k = p->lk[id].first; k >= 0; k = p->lk[k].next) JS_SetPropertyUint32(c, a, i++, JS_NewInt32(c, k));
    return a;
}
FN(d_create) { Page* p = P(c); const char* tag = JS_ToCString(c, argv[0]); char low[16]; snprintf(low, sizeof low, "%s", tag ? tag : "div"); for (char* q = low; *q; q++) *q = (char)tolower((unsigned char)*q); if (tag) JS_FreeCString(c, tag);
    DOMNode* n = webpage_new_node(low); if (!n) return JS_NULL; if (!strcmp(low, "input")) strcpy(n->input_type, "text"); return JS_NewInt32(c, new_id(p, n)); }
FN(d_createText) { Page* p = P(c); const char* s = JS_ToCString(c, argv[0]); DOMNode* n = webpage_new_node("#text"); put(n->text, sizeof n->text, s ? s : ""); if (s) JS_FreeCString(c, s); return JS_NewInt32(c, new_id(p, n)); }
static int clone_rec(Page* p, int id, int deep) {
    DOMNode* src = p->nodes[id]; DOMNode* n = malloc(sizeof(DOMNode)); memcpy(n, src, sizeof(DOMNode)); n->next = NULL; n->aux = NULL; n->pb[0] = n->pb[1] = n->pb[2] = NULL; int nid = new_id(p, n);
    if (deep) for (int k = p->lk[id].first; k >= 0; k = p->lk[k].next) { int cid = clone_rec(p, k, 1); link_before(p, nid, cid, -1); }
    return nid;
}
FN(d_clone) { Page* p = P(c); int id = arg_int(c, argv[0]); if (id < 0 || id >= p->nn) return JS_NULL; return JS_NewInt32(c, clone_rec(p, id, argc > 1 && JS_ToBool(c, argv[1]) > 0)); }
FN(d_insertBefore) {
    Page* p = P(c); int par = arg_int(c, argv[0]), ch = arg_int(c, argv[1]), ref = argc > 2 ? arg_int(c, argv[2]) : -1;
    if (par < 0 || par >= p->nn || ch < 0 || ch >= p->nn || ch == id_of(p->head) || is_ancestor(p, ch, par)) return JS_FALSE;
    link_before(p, par, ch, ref); mark_dirty(p); return JS_TRUE;
}
FN(d_remove) { Page* p = P(c); int id = arg_int(c, argv[0]); if (id < 0 || id >= p->nn || id == id_of(p->head)) return JS_FALSE; unlink_node(p, id); mark_dirty(p); return JS_TRUE; }

/* ── текст / html ── */
FN(d_text) { Page* p = P(c); int id = arg_int(c, argv[0]); if (id < 0 || id >= p->nn) return JS_NULL; Buf b = {0}; text_of(p, id, &b); JSValue r = JS_NewString(c, b.p ? b.p : ""); free(b.p); return r; }
FN(d_setText) {
    Page* p = P(c); int id = arg_int(c, argv[0]); if (id < 0 || id >= p->nn) return JS_UNDEFINED; const char* s = JS_ToCString(c, argv[1]); if (!s) return JS_EXCEPTION;
    DOMNode* n = p->nodes[id];
    if (n->type == TAG_TEXT) put(n->text, sizeof n->text, s); else { clear_children(p, id); if (s[0]) add_text_nodes(p, id, s); if (n->type == TAG_TEXTAREA) put(n->value, sizeof n->value, s); }
    JS_FreeCString(c, s); mark_dirty(p); return JS_UNDEFINED;
}
FN(d_innerHTML) { Page* p = P(c); int id = arg_int(c, argv[0]); if (id < 0 || id >= p->nn) return JS_NULL; Buf b = {0}; ser(p, id, &b, 0); JSValue r = JS_NewString(c, b.p ? b.p : ""); free(b.p); return r; }
FN(d_outerHTML) { Page* p = P(c); int id = arg_int(c, argv[0]); if (id < 0 || id >= p->nn) return JS_NULL; Buf b = {0}; ser(p, id, &b, 1); JSValue r = JS_NewString(c, b.p ? b.p : ""); free(b.p); return r; }
FN(d_setInnerHTML) {
    Page* p = P(c); int id = arg_int(c, argv[0]); if (id < 0 || id >= p->nn) return JS_UNDEFINED; const char* s = JS_ToCString(c, argv[1]); if (!s) return JS_EXCEPTION;
    clear_children(p, id); if (s[0]) import_fragment(p, id, -1, s); JS_FreeCString(c, s); mark_dirty(p); return JS_UNDEFINED;
}
FN(d_insertHTML) {                                   /* (id, position: 0 beforebegin,1 afterbegin,2 beforeend,3 afterend, html) */
    Page* p = P(c); int id = arg_int(c, argv[0]), pos = arg_int(c, argv[1]); if (id < 0 || id >= p->nn) return JS_UNDEFINED; const char* s = JS_ToCString(c, argv[2]); if (!s) return JS_EXCEPTION;
    int par = p->lk[id].parent; if (pos == 1) import_fragment(p, id, p->lk[id].first, s); else if (pos == 2) import_fragment(p, id, -1, s);
    else if (par >= 0) { if (pos == 0) import_fragment(p, par, id, s); else import_fragment(p, par, p->lk[id].next, s); }
    JS_FreeCString(c, s); mark_dirty(p); return JS_UNDEFINED;
}

/* ── атрибуты ── */
FN(d_attr) { NODE(0); const char* k = JS_ToCString(c, argv[1]); char v[2200]; int ok = k ? attr_get(P(c), n, k, v, sizeof v) : 0; if (!ok && k && is_bool_attr(k)) { int f = !strcmp(k, "checked") ? n->checked : !strcmp(k, "disabled") ? n->disabled : !strcmp(k, "selected") ? n->selected : !strcmp(k, "readonly") ? n->readonly : n->multiple; if (f) { v[0] = 0; ok = 1; } } if (k) JS_FreeCString(c, k); return ok ? JS_NewString(c, v) : JS_NULL; }
FN(d_setAttr) { Page* p = P(c); NODE(0); const char* k = JS_ToCString(c, argv[1]); const char* v = JS_ToCString(c, argv[2]); if (k && v) { char low[40]; snprintf(low, sizeof low, "%s", k); for (char* q = low; *q; q++) *q = (char)tolower((unsigned char)*q); attr_set(p, n, low, v); mark_dirty(p); } if (k) JS_FreeCString(c, k); if (v) JS_FreeCString(c, v); return JS_UNDEFINED; }
FN(d_removeAttr) { Page* p = P(c); NODE(0); const char* k = JS_ToCString(c, argv[1]); if (k) { attr_remove(n, k); mark_dirty(p); JS_FreeCString(c, k); } return JS_UNDEFINED; }
FN(d_attrNames) {
    NODE(0); JSValue a = JS_NewArray(c); uint32_t i = 0;
    if (n->id[0] && !strstr(n->attrs, "id\x01")) JS_SetPropertyUint32(c, a, i++, JS_NewString(c, "id"));
    if (n->class_name[0]) JS_SetPropertyUint32(c, a, i++, JS_NewString(c, "class"));
    if (n->style_str[0]) JS_SetPropertyUint32(c, a, i++, JS_NewString(c, "style"));
    const char *av; size_t al;
    if (n->srcset[0] && !attrs_find(n->attrs, "srcset", &av, &al)) JS_SetPropertyUint32(c, a, i++, JS_NewString(c, "srcset"));
    if (n->sizes[0] && !attrs_find(n->attrs, "sizes", &av, &al)) JS_SetPropertyUint32(c, a, i++, JS_NewString(c, "sizes"));
    for (const char* q = n->attrs; *q;) { const char* sep = strchr(q, 1); if (!sep) break; const char* e = strchr(sep, 2); if (!e) break; char nm[40]; snprintf(nm, sizeof nm, "%.*s", (int)(sep - q), q); JS_SetPropertyUint32(c, a, i++, JS_NewString(c, nm)); q = e + 1; }
    return a;
}
FN(d_prop) {
    NODE(0); const char* k = JS_ToCString(c, argv[1]); JSValue r = JS_UNDEFINED; if (!k) return JS_EXCEPTION;
    if (!strcmp(k, "value")) {
        if (n->type == TAG_INPUT && !strcmp(n->input_type, "file")) {
            const char* base = n->file_selected ? strrchr(n->value, '/') : NULL;
            const char* winbase = n->file_selected ? strrchr(n->value, '\\') : NULL;
            if (!base || (winbase && winbase > base)) base = winbase;
            base = base ? base + 1 : (n->file_selected ? n->value : "");
            char fake[2200]; snprintf(fake, sizeof fake, "%s%s", n->file_selected ? "C:\\fakepath\\" : "", base);
            r = JS_NewString(c, fake);
        } else r = JS_NewString(c, n->value);
    } else if (!strcmp(k, "checked")) r = JS_NewBool(c, n->checked); else if (!strcmp(k, "disabled")) r = JS_NewBool(c, n->disabled);
    else if (!strcmp(k, "selected")) r = JS_NewBool(c, n->selected); else if (!strcmp(k, "readonly")) r = JS_NewBool(c, n->readonly); else if (!strcmp(k, "multiple")) r = JS_NewBool(c, n->multiple);
    else if (!strcmp(k, "type")) r = JS_NewString(c, n->input_type); else if (!strcmp(k, "name")) r = JS_NewString(c, n->name); else if (!strcmp(k, "placeholder")) r = JS_NewString(c, n->placeholder);
    else if (!strcmp(k, "href")) r = JS_NewString(c, n->href); else if (!strcmp(k, "src")) r = JS_NewString(c, n->src); else if (!strcmp(k, "alt")) r = JS_NewString(c, n->alt);
    JS_FreeCString(c, k); return r;
}
FN(d_setProp) {
    Page* p = P(c); NODE(0); const char* k = JS_ToCString(c, argv[1]); if (!k) return JS_EXCEPTION;
    if (!strcmp(k, "value")) {
        const char* v = JS_ToCString(c, argv[2]);
        if (v && n->type == TAG_INPUT && !strcmp(n->input_type, "file")) {
            if (*v) { JS_FreeCString(c, v); JS_FreeCString(c, k); return JS_ThrowTypeError(c, "Cannot set a non-empty value on a file input"); }
            n->value[0] = 0; n->file_selected = 0;
        } else if (v) put(n->value, sizeof n->value, v);
        if (v) JS_FreeCString(c, v);
    }
    else { int b = JS_ToBool(c, argv[2]) > 0; if (!strcmp(k, "checked")) n->checked = (uint8_t)b; else if (!strcmp(k, "disabled")) { n->disabled = (uint8_t)b; if (b) attrs_set(n, "disabled", ""); else attrs_del(n, "disabled"); mark_dirty(p); } else if (!strcmp(k, "selected")) n->selected = (uint8_t)b; else if (!strcmp(k, "readonly")) n->readonly = (uint8_t)b; else if (!strcmp(k, "multiple")) n->multiple = (uint8_t)b; }
    JS_FreeCString(c, k); return JS_UNDEFINED;
}

/* ── запросы ── */
FN(d_query) {
    Page* p = P(c); linearize(p); int root = arg_int(c, argv[0]); int all = JS_ToBool(c, argv[2]) > 0; const char* s = JS_ToCString(c, argv[1]); if (!s) return JS_EXCEPTION;
    WebSel* sel = websel_compile(s); if (!sel) { JSValue e = JS_ThrowSyntaxError(c, "'%s' is not a valid selector", s); JS_FreeCString(c, s); return e; }
    JS_FreeCString(c, s);
    if (!p->doc) p->doc = webdoc_build(p->head);
    DOMNode* r = N(p, root); int lo = r ? r->idx + 1 : 0, hi = r ? r->subtree_end : p->head->subtree_end;
    if (r && r->idx < 0) { websel_free(sel); return all ? JS_NewArray(c) : JS_NewInt32(c, -1); }
    JSValue arr = JS_NewArray(c); uint32_t cnt = 0; int first = -1;
    DOMNode* q = r ? r->next : p->head;
    for (int i = lo; q && i <= hi; i++, q = q->next) { if (q->type != TAG_TEXT && websel_match(p->doc, sel, q)) { if (!all) { first = id_of(q); break; } JS_SetPropertyUint32(c, arr, cnt++, JS_NewInt32(c, id_of(q))); } }
    websel_free(sel); if (all) return arr; JS_FreeValue(c, arr); return JS_NewInt32(c, first);
}
FN(d_matches) { Page* p = P(c); linearize(p); NODE(0); const char* s = JS_ToCString(c, argv[1]); if (!s) return JS_EXCEPTION; WebSel* sel = websel_compile(s); JS_FreeCString(c, s); if (!sel) return JS_ThrowSyntaxError(c, "invalid selector"); if (!p->doc) p->doc = webdoc_build(p->head); int m = n->idx >= 0 && websel_match(p->doc, sel, n); websel_free(sel); return JS_NewBool(c, m); }
FN(d_closest) {
    Page* p = P(c); linearize(p); int id = arg_int(c, argv[0]); const char* s = JS_ToCString(c, argv[1]); if (!s) return JS_EXCEPTION; WebSel* sel = websel_compile(s); JS_FreeCString(c, s); if (!sel) return JS_ThrowSyntaxError(c, "invalid selector");
    if (!p->doc) p->doc = webdoc_build(p->head); int res = -1;
    for (int x = id; x >= 0 && x < p->nn; x = p->lk[x].parent) { DOMNode* q = p->nodes[x]; if (q->type != TAG_TEXT && q->idx >= 0 && websel_match(p->doc, sel, q)) { res = x; break; } }
    websel_free(sel); return JS_NewInt32(c, res);
}
FN(d_byId) { Page* p = P(c); linearize(p); const char* s = JS_ToCString(c, argv[0]); if (!s) return JS_EXCEPTION; int res = -1; for (DOMNode* q = p->head; q; q = q->next) if (q->type != TAG_TEXT && q->id[0] && !strcmp(q->id, s)) { res = id_of(q); break; } JS_FreeCString(c, s); return JS_NewInt32(c, res); }
FN(d_root) { return JS_NewInt32(c, id_of(P(c)->head)); }
FN(d_special) {                                       /* 0 head, 1 body */
    Page* p = P(c); int w = arg_int(c, argv[0]); int root = id_of(p->head);
    for (int k = p->lk[root].first; k >= 0; k = p->lk[k].next) if (!strcmp(p->nodes[k]->tag, w ? "body" : "head")) return JS_NewInt32(c, k);
    return JS_NewInt32(c, -1);
}

/* ── геометрия и стили ── */
FN(d_rect) {
    Page* p = P(c); NODE(0); flush_all(p); float r[4] = {0, 0, 0, 0}; int sx, sy, vw, vh; webpage_get_scroll(&sx, &sy, &vw, &vh);
    if (n->idx >= 0) webpage_node_rect(n, r);
    JSValue a = JS_NewArray(c); JS_SetPropertyUint32(c, a, 0, JS_NewFloat64(c, r[0] - sx)); JS_SetPropertyUint32(c, a, 1, JS_NewFloat64(c, r[1] - sy)); JS_SetPropertyUint32(c, a, 2, JS_NewFloat64(c, r[2])); JS_SetPropertyUint32(c, a, 3, JS_NewFloat64(c, r[3])); return a;
}
FN(d_hitTest) {
    Page* p = P(c); double x=0, y=0; (void)JS_ToFloat64(c,&x,argv[0]); (void)JS_ToFloat64(c,&y,argv[1]);
    flush_all(p);
    DOMNode* hit = webpage_hit_element(p->head, (int)lround(x), (int)lround(y) + webpage_viewport_top());
    return JS_NewInt32(c, hit ? id_of(hit) : -1);
}
/* прокрутка внутри элемента: [x, y, scrollW, scrollH, clientW, clientH] или [] для непрокручиваемого */
FN(d_getScroll) {
    Page* p = P(c); NODE(0); flush_all(p); JSValue a = JS_NewArray(c); float v[6];
    if (n->idx >= 0 && webpage_elem_scroll_get(n, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5])) for (int i = 0; i < 6; i++) JS_SetPropertyUint32(c, a, (uint32_t)i, JS_NewFloat64(c, v[i]));
    return a;
}
FN(d_setScroll) {
    Page* p = P(c); NODE(0); flush_all(p); double x = 0, y = 0; JS_ToFloat64(c, &x, argv[1]); JS_ToFloat64(c, &y, argv[2]);
    if (n->idx >= 0) { webpage_elem_scroll_set(n, (float)x, (float)y); mark_dirty(p); }
    return JS_UNDEFINED;
}
FN(d_setScrollSmooth) {
    Page* p = P(c); NODE(0); flush_all(p); double x = 0, y = 0; JS_ToFloat64(c, &x, argv[1]); JS_ToFloat64(c, &y, argv[2]);
    if (n->idx >= 0) { webpage_elem_scroll_set_smooth(n, (float)x, (float)y); mark_dirty(p); }
    return JS_UNDEFINED;
}
static void css_color(char* out, size_t cap, uint32_t c) { unsigned a = c >> 24; if (a == 255) snprintf(out, cap, "rgb(%u, %u, %u)", (c >> 16) & 255, (c >> 8) & 255, c & 255); else snprintf(out, cap, "rgba(%u, %u, %u, %.3g)", (c >> 16) & 255, (c >> 8) & 255, c & 255, a / 255.0); }
FN(d_computed) {
    Page* p = P(c); NODE(0); flush_all(p); const char* k = JS_ToCString(c, argv[1]); if (!k) return JS_EXCEPTION; const NodeStyle* st = &n->st; char v[128] = ""; float r[4] = {0, 0, 0, 0};
    static const char* DSP[] = {"none", "inline", "block", "inline-block", "flex", "inline-flex", "list-item", "table", "table-row-group", "table-row", "table-cell", "table-caption", "contents"};
    if (!strcmp(k, "display")) snprintf(v, sizeof v, "%s", st->is_grid ? "grid" : DSP[st->display <= 12 ? st->display : 1]);
    else if (!strcmp(k, "position")) snprintf(v, sizeof v, "%s", st->position == POS_ABSOLUTE ? "absolute" : st->position == POS_FIXED ? "fixed" : st->position == POS_RELATIVE ? "relative" : "static");
    else if (!strcmp(k, "visibility")) snprintf(v, sizeof v, "%s", st->visibility ? "hidden" : "visible");
    else if (!strcmp(k, "color")) css_color(v, sizeof v, st->color); else if (!strcmp(k, "background-color")) css_color(v, sizeof v, st->bg);
    else if (!strcmp(k, "font-size")) snprintf(v, sizeof v, "%gpx", st->font_size); else if (!strcmp(k, "font-weight")) snprintf(v, sizeof v, "%d", st->bold ? 700 : 400);
    else if (!strcmp(k, "font-style")) snprintf(v, sizeof v, "%s", st->italic ? "italic" : "normal"); else if (!strcmp(k, "font-family")) snprintf(v, sizeof v, "%s", n->font_family);
    else if (!strcmp(k, "opacity")) snprintf(v, sizeof v, "%g", st->opacity); else if (!strcmp(k, "z-index")) { if (st->has_z) snprintf(v, sizeof v, "%d", st->z_index); else strcpy(v, "auto"); }
    else if (!strcmp(k, "text-align")) snprintf(v, sizeof v, "%s", st->text_align == ALIGN_CENTER ? "center" : st->text_align == ALIGN_RIGHT ? "right" : st->text_align == ALIGN_JUSTIFY ? "justify" : st->text_align == ALIGN_END ? "end" : st->text_align == ALIGN_START ? "start" : "left");
    else if (!strcmp(k, "direction")) snprintf(v, sizeof v, "%s", st->direction ? "rtl" : "ltr");
    else if (!strcmp(k, "unicode-bidi")) {
        static const char* bidi_values[] = {"normal", "embed", "isolate", "bidi-override", "isolate-override", "plaintext"};
        snprintf(v, sizeof v, "%s", bidi_values[st->unicode_bidi <= 5 ? st->unicode_bidi : 0]);
    }
    else if (!strcmp(k, "overflow")) snprintf(v, sizeof v, "%s", st->overflow_hidden ? "hidden" : "visible"); else if (!strcmp(k, "cursor")) snprintf(v, sizeof v, "%s", st->pointer ? "pointer" : "auto");
    else if (!strcmp(k, "float")) snprintf(v, sizeof v, "%s", st->float_ == FLT_LEFT ? "left" : st->float_ == FLT_RIGHT ? "right" : "none");
    else if (!strcmp(k, "width") || !strcmp(k, "height")) { if (n->idx >= 0 && webpage_node_rect(n, r)) snprintf(v, sizeof v, "%gpx", k[0] == 'w' ? r[2] : r[3]); else strcpy(v, "auto"); }
    else if (!strncmp(k, "margin-", 7) || !strncmp(k, "padding-", 8)) { int pad = k[0] == 'p'; const char* sd = k + (pad ? 8 : 7); int i = !strcmp(sd, "top") ? 0 : !strcmp(sd, "right") ? 1 : !strcmp(sd, "bottom") ? 2 : 3; const Len* l = pad ? &st->padding[i] : &st->margin[i]; snprintf(v, sizeof v, "%gpx", l->px); }
    else if (!strncmp(k, "border-", 7) && strstr(k, "width")) { const char* sd = k + 7; int i = !strncmp(sd, "top", 3) ? 0 : !strncmp(sd, "right", 5) ? 1 : !strncmp(sd, "bottom", 6) ? 2 : 3; snprintf(v, sizeof v, "%gpx", st->border_w[i]); }
    else if (!strcmp(k, "line-height")) { if (st->lh_px > 0) snprintf(v, sizeof v, "%gpx", st->lh_px); else if (st->lh_mult > 0) snprintf(v, sizeof v, "%gpx", st->lh_mult * st->font_size); else strcpy(v, "normal"); }
    else if (!strcmp(k, "scroll-behavior")) snprintf(v, sizeof v, "%s", st->scroll_behavior ? "smooth" : "auto");
    else if (!strcmp(k, "zoom")) snprintf(v, sizeof v, "%g", st->zoom > 0 ? st->zoom : 1.0f);
    else if (!strcmp(k, "scroll-snap-type")) snprintf(v, sizeof v, "%s", st->scroll_snap_type == 3 ? "both mandatory" : st->scroll_snap_type == 2 ? "y mandatory" : st->scroll_snap_type == 1 ? "x mandatory" : "none");
    else if (!strcmp(k, "box-sizing")) snprintf(v, sizeof v, "%s", st->box_sizing ? "border-box" : "content-box");
    JS_FreeCString(c, k); return JS_NewString(c, v);
}
FN(d_mediaMatches) { const char* q = JS_ToCString(c, argv[0]); int m = q ? css_media_matches(q) : 0; if (q) JS_FreeCString(c, q); return JS_NewBool(c, m); }
FN(d_viewport) { int sx, sy, vw, vh; webpage_get_scroll(&sx, &sy, &vw, &vh); JSValue a = JS_NewArray(c); JS_SetPropertyUint32(c, a, 0, JS_NewInt32(c, vw)); JS_SetPropertyUint32(c, a, 1, JS_NewInt32(c, vh)); JS_SetPropertyUint32(c, a, 2, JS_NewInt32(c, sx)); JS_SetPropertyUint32(c, a, 3, JS_NewInt32(c, sy)); return a; }
FN(d_scrollTo) { Page* p = P(c); double x = 0, y = 0; JS_ToFloat64(c, &x, argv[0]); JS_ToFloat64(c, &y, argv[1]); p->scroll_req = 1; p->sx = (int)x; p->sy = (int)y; return JS_UNDEFINED; }

/* ── документ / навигация / сеть ── */
FN(d_title) { Page* p = P(c); flush_style(p); (void)p; linearize(p); for (DOMNode* q = p->head; q; q = q->next) if (!strcmp(q->tag, "title")) { Buf b = {0}; text_of(p, id_of(q), &b); JSValue r = JS_NewString(c, b.p ? b.p : ""); free(b.p); return r; } return JS_NewString(c, ""); }
FN(d_setTitle) {
    Page* p = P(c); const char* s = JS_ToCString(c, argv[0]); if (!s) return JS_EXCEPTION; DOMNode* title = NULL; int hd = -1; linearize(p);
    for (DOMNode* q = p->head; q; q = q->next) { if (!strcmp(q->tag, "title")) { title = q; break; } if (!strcmp(q->tag, "head")) hd = id_of(q); }
    if (!title && hd >= 0) { title = webpage_new_node("title"); link_before(p, hd, new_id(p, title), -1); }
    if (title) { int id = id_of(title); clear_children(p, id); add_text_nodes(p, id, s); mark_dirty(p); }
    put(p->title, sizeof p->title, s); p->title_dirty = 1; JS_FreeCString(c, s); return JS_UNDEFINED;
}
FN(d_url) { return JS_NewString(c, P(c)->url); }
FN(d_nav) { Page* p = P(c); const char* s = JS_ToCString(c, argv[0]); if (!s) return JS_EXCEPTION; if (!strncmp(s, "__", 2)) snprintf(p->nav, sizeof p->nav, "%s", s); else webpage_resolve_url(p->url, s, p->nav, sizeof p->nav); p->has_nav = p->nav[0] != 0; JS_FreeCString(c, s); return JS_UNDEFINED; }
FN(d_setUrl) { Page* p = P(c); const char* s = JS_ToCString(c, argv[0]); if (s) { char ab[1100]; webpage_resolve_url(p->url, s, ab, sizeof ab); if (ab[0]) put(p->url, sizeof p->url, ab); JS_FreeCString(c, s); } return JS_UNDEFINED; }
FN(d_fetch) {
    const char* s = JS_ToCString(c, argv[0]); if (!s) return JS_EXCEPTION; Page* p = P(c); char ab[1100]; webpage_resolve_url(p->url, s, ab, sizeof ab); JS_FreeCString(c, s);
    { const char* col = strchr(ab, ':'); const char* sl = strchr(ab, '/'); if (col && (!sl || col < sl) && strncmp(ab, "http", 4) != 0 && strncmp(ab, "file:", 5) != 0) return JS_NULL; }   /* data:, blob:, javascript: не грузим */
    char* txt = webpage_fetch_text_kind(ab, NAL_RESOURCE_CONNECT); if (!txt) return JS_NULL; JSValue r = JS_NewString(c, txt); free(txt); return r;
}

/* ── асинхронная сеть (fetch/XHR любых методов, не блокирует движок) ──
   D.fetchStart(method,url,headersRaw,body|null[,credentials,mode]) -> id (int, -1 при ошибке запуска); body — ArrayBuffer (или строка);
   D.fetchPoll(id) -> null пока не готово, иначе {status, headers, body: ArrayBuffer};
   D.fetchCancel(id) — отменить (например AbortController). */
static JSValue fetch_start_impl(JSContext* c, int argc, JSValueConst* argv, int streaming) {
    Page* p = P(c);
    const char* method = JS_ToCString(c, argv[0]); const char* url = JS_ToCString(c, argv[1]);
    const char* headers = JS_IsNull(argv[2]) || JS_IsUndefined(argv[2]) ? NULL : JS_ToCString(c, argv[2]);
    size_t blen = 0; const char* body = NULL; const char* body_str = NULL;
    /* тело: ArrayBuffer (бинарно-безопасно, основной путь) либо строка (совместимость) */
    if (argc > 3 && JS_IsString(argv[3])) { body_str = JS_ToCStringLen(c, &blen, argv[3]); body = body_str; }
    else if (argc > 3 && JS_IsObject(argv[3])) {
        size_t n = 0; uint8_t* ab = JS_GetArrayBuffer(c, &n, argv[3]);
        if (ab && n) { body = (const char*)ab; blen = n; } else if (!ab) JS_FreeValue(c, JS_GetException(c));
    }
    if (!method || !url) { if (method) JS_FreeCString(c, method); if (url) JS_FreeCString(c, url); if (headers) JS_FreeCString(c, headers); if (body_str) JS_FreeCString(c, body_str); return JS_NewInt32(c, -1); }
    char ab[1100]; webpage_resolve_url(p->url, url, ab, sizeof ab);
    int ok = 0; NalAsyncReq nr;
    /* политика одного источника и CORS: страница-инициатор, режим credentials (0 omit / 1 same-origin / 2 include) и mode (0 cors / 1 no-cors / 2 same-origin) от fetch/XHR */
    NalFetchOpts fo; fo.page_url = p->url; fo.credentials = 1; fo.mode = 0; fo.redirect = 0;
    if (argc > 4 && !JS_IsUndefined(argv[4])) fo.credentials = arg_int(c, argv[4]);
    if (argc > 5 && !JS_IsUndefined(argv[5])) fo.mode = arg_int(c, argv[5]);
    if (argc > 6 && !JS_IsUndefined(argv[6])) fo.redirect = arg_int(c, argv[6]);
    { const char* col = strchr(ab, ':'); const char* sl = strchr(ab, '/'); if (!(col && (!sl || col < sl) && strncmp(ab, "http", 4) != 0)) ok = streaming ? nal_async_start_stream_ex(method, ab, headers, body, blen, &fo, &nr) : nal_async_start_ex(method, ab, headers, body, blen, &fo, &nr); }
    JS_FreeCString(c, method); JS_FreeCString(c, url); if (headers) JS_FreeCString(c, headers); if (body_str) JS_FreeCString(c, body_str);
    if (!ok) return JS_NewInt32(c, -1);
    AReq* a = calloc(1, sizeof *a); if (!a) { nal_async_cancel(&nr); return JS_NewInt32(c, -1); }
    a->id = ++p->next_areq; a->used = 1; a->streaming = streaming; a->nr = nr; a->next = p->areqs; p->areqs = a;
    return JS_NewInt32(c, a->id);
}
FN(d_fetchSync) {
    Page* p = P(c);
    const char* method = argc > 0 ? JS_ToCString(c, argv[0]) : NULL;
    const char* input_url = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    const char* headers = argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2]) ? JS_ToCString(c, argv[2]) : NULL;
    size_t body_len = 0; const unsigned char* body = NULL; const char* body_str = NULL;
    if (argc > 3 && JS_IsString(argv[3])) { body_str = JS_ToCStringLen(c, &body_len, argv[3]); body = (const unsigned char*)body_str; }
    else if (argc > 3 && JS_IsObject(argv[3])) {
        uint8_t* ab = JS_GetArrayBuffer(c, &body_len, argv[3]);
        if (ab) body = ab; else JS_FreeValue(c, JS_GetException(c));
    }
    char resolved[1100] = ""; if (input_url) webpage_resolve_url(p->url, input_url, resolved, sizeof resolved);
    NalFetchOpts opts; opts.page_url = p->url; opts.credentials = argc > 4 ? arg_int(c, argv[4]) : 1;
    opts.mode = argc > 5 ? arg_int(c, argv[5]) : 0; opts.redirect = argc > 6 ? arg_int(c, argv[6]) : 0;
    NalSyncResponse rs; memset(&rs, 0, sizeof rs);
    int ok = method && resolved[0] && nal_sync_fetch_ex(method, resolved, headers, body, body_len, &opts, &rs);
    if (!ok && !rs.error[0]) snprintf(rs.error, sizeof rs.error, "synchronous request failed");
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "status", JS_NewInt32(c, ok ? rs.status : 0));
    JS_SetPropertyStr(c, o, "headers", JS_NewString(c, ok && rs.headers ? rs.headers : ""));
    JS_SetPropertyStr(c, o, "body", JS_NewArrayBufferCopy(c, ok && rs.body ? rs.body : (const uint8_t*)"", ok ? rs.body_len : 0));
    JS_SetPropertyStr(c, o, "url", JS_NewString(c, ok && rs.final_url[0] ? rs.final_url : resolved));
    JS_SetPropertyStr(c, o, "error", JS_NewString(c, ok ? "" : rs.error));
    nal_sync_response_free(&rs);
    if (method) JS_FreeCString(c, method); if (input_url) JS_FreeCString(c, input_url);
    if (headers) JS_FreeCString(c, headers); if (body_str) JS_FreeCString(c, body_str);
    return o;
}
FN(d_fetchStart) { return fetch_start_impl(c, argc, argv, 0); }
FN(d_fetchStreamStart) { return fetch_start_impl(c, argc, argv, 1); }
FN(d_fetchPoll) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (AReq* a = p->areqs; a; a = a->next) if (a->id == id && a->used) {
        int status = 0; char* hdrs = NULL; char* bodytxt = NULL; size_t blen = 0;
        if (!nal_async_poll_ex(&a->nr, &status, &hdrs, &bodytxt, &blen)) return JS_NULL;
        a->used = 0;
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "status", JS_NewInt32(c, status));
        JS_SetPropertyStr(c, o, "headers", JS_NewString(c, hdrs ? hdrs : ""));
        JS_SetPropertyStr(c, o, "body", JS_NewArrayBufferCopy(c, (const uint8_t*)(bodytxt ? bodytxt : ""), blen));   /* ArrayBuffer: тело бинарное */
        free(hdrs); free(bodytxt);
        return o;
    }
    return JS_NULL;                                                 /* неизвестный/уже забранный id — трактуем как «не готово» */
}
FN(d_fetchStreamHeaders) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (AReq* a = p->areqs; a; a = a->next) if (a->id == id && a->used && a->streaming) {
        int status = 0; char* headers = NULL; int rc = nal_async_stream_headers(&a->nr, &status, &headers);
        if (rc == 0) return JS_NULL;
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "status", JS_NewInt32(c, rc < 0 ? 0 : status));
        JS_SetPropertyStr(c, o, "headers", JS_NewString(c, headers ? headers : ""));
        JS_SetPropertyStr(c, o, "error", JS_NewBool(c, rc < 0));
        free(headers); return o;
    }
    return JS_NULL;
}
FN(d_fetchStreamRead) {
    Page* p = P(c); int id = arg_int(c, argv[0]); int cap = argc > 1 ? arg_int(c, argv[1]) : 16384;
    if (cap < 1) cap = 16384; if (cap > 65536) cap = 65536;
    for (AReq* a = p->areqs; a; a = a->next) if (a->id == id && a->used && a->streaming) {
        unsigned char* buf = malloc((size_t)cap); if (!buf) return JS_ThrowOutOfMemory(c);
        size_t n = 0; int done = 0, error = 0; int rc = nal_async_stream_read(&a->nr, buf, (size_t)cap, &n, &done, &error);
        if (rc == 0) { free(buf); return JS_NULL; }
        JSValue o = JS_NewObject(c);
        if (rc < 0) { done = 1; error = 1; n = 0; }
        JS_SetPropertyStr(c, o, "bytes", JS_NewArrayBufferCopy(c, buf, n));
        JS_SetPropertyStr(c, o, "done", JS_NewBool(c, done));
        JS_SetPropertyStr(c, o, "error", JS_NewBool(c, error || rc < 0));
        free(buf); return o;
    }
    return JS_NULL;
}
FN(d_fetchStreamClose) {
    Page* p = P(c); int id = arg_int(c, argv[0]); int cancel = argc > 1 ? JS_ToBool(c, argv[1]) : 0;
    for (AReq* a = p->areqs; a; a = a->next) if (a->id == id && a->used && a->streaming) {
        nal_async_stream_close(&a->nr, cancel); a->used = 0; break;
    }
    return JS_UNDEFINED;
}
FN(d_fetchCancel) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (AReq* a = p->areqs; a; a = a->next) if (a->id == id && a->used) { nal_async_cancel(&a->nr); a->used = 0; break; }
    return JS_UNDEFINED;
}

/* UTF-8 <-> ArrayBuffer (нативно: быстрее и корректнее JS-реализации; BOM при декодировании снимается, как в Fetch) */
FN(d_utf8Decode) {
    size_t n = 0; uint8_t* b = JS_GetArrayBuffer(c, &n, argv[0]);
    if (!b) { JS_FreeValue(c, JS_GetException(c)); return JS_NewString(c, ""); }
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { b += 3; n -= 3; }
    return JS_NewStringLen(c, (const char*)b, n);
}
FN(d_utf8Encode) {
    size_t n = 0; const char* s = JS_ToCStringLen(c, &n, argv[0]); if (!s) return JS_EXCEPTION;
    JSValue ab = JS_NewArrayBufferCopy(c, (const uint8_t*)s, n); JS_FreeCString(c, s); return ab;
}

/* ── localStorage (переживает переход на другую страницу и перезапуск браузера) ── */
static void page_origin(Page* p, char* out, size_t cap) {
    const char* u = p->url; const char* sep = strstr(u, "://");
    if (!sep || cap < 2) { if (cap) snprintf(out,cap,"null"); return; }
    size_t sch_n = (size_t)(sep-u); if (!sch_n || sch_n > 15) { snprintf(out,cap,"null"); return; }
    char scheme[16]; memcpy(scheme,u,sch_n); scheme[sch_n]=0;
    for (char* q=scheme;*q;q++) *q=(char)tolower((unsigned char)*q);
    const char* a=sep+3; const char* e=a; while(*e && *e!='/' && *e!='?' && *e!='#') e++;
    const char* host=a; for(const char* q=a;q<e;q++) if(*q=='@') host=q+1; /* origin не включает userinfo */
    size_t hn=(size_t)(e-host); if(!hn || hn>=512){snprintf(out,cap,"null");return;}
    char authority[512]; memcpy(authority,host,hn); authority[hn]=0;
    for(char* q=authority;*q;q++) *q=(char)tolower((unsigned char)*q);
    /* Убираем default port и тем самым объединяем варианты одного и того же origin. */
    if(!strcmp(scheme,"http")){size_t n=strlen(authority);if(n>=3&&!strcmp(authority+n-3,":80"))authority[n-3]=0;}
    if(!strcmp(scheme,"https")){size_t n=strlen(authority);if(n>=4&&!strcmp(authority+n-4,":443"))authority[n-4]=0;}
    snprintf(out,cap,"%s://%s",scheme,authority);
}
FN(d_lsList) { Page* p = P(c); char origin[512]; page_origin(p, origin, sizeof origin); char* j = db_ls_list_json(origin); JSValue r = JS_NewString(c, j ? j : "{}"); free(j); return r; }
FN(d_lsSet) {
    Page* p = P(c); char origin[512]; page_origin(p, origin, sizeof origin);
    const char* k = JS_ToCString(c, argv[0]); const char* v = JS_ToCString(c, argv[1]);
    if (k && v) db_ls_set(origin, k, v); if (k) JS_FreeCString(c, k); if (v) JS_FreeCString(c, v); return JS_UNDEFINED;
}
FN(d_lsRemove) { Page* p = P(c); char origin[512]; page_origin(p, origin, sizeof origin); const char* k = JS_ToCString(c, argv[0]); if (k) { db_ls_remove(origin, k); JS_FreeCString(c, k); } return JS_UNDEFINED; }
FN(d_lsClear) { Page* p = P(c); char origin[512]; page_origin(p, origin, sizeof origin); db_ls_clear(origin); return JS_UNDEFINED; }

/* ── IndexedDB bridge ── */
FN(d_idbMetaList) { Page* p=P(c); char o[512]; page_origin(p,o,sizeof o); char* j=db_idb_meta_list(o); JSValue r=JS_NewString(c,j?j:"[]"); free(j); return r; }
FN(d_idbMetaGet) { Page* p=P(c); char o[512]; page_origin(p,o,sizeof o); const char* n=JS_ToCString(c,argv[0]); if(!n)return JS_EXCEPTION; char* j=db_idb_meta_get(o,n); JS_FreeCString(c,n); if(!j)return JS_NULL; JSValue r=JS_NewString(c,j);free(j);return r; }
FN(d_idbMetaSet) { Page* p=P(c); char o[512];page_origin(p,o,sizeof o);const char* n=JS_ToCString(c,argv[0]);int v=arg_int(c,argv[1]);const char* st=JS_ToCString(c,argv[2]);if(n&&st)db_idb_meta_set(o,n,v,st);if(n)JS_FreeCString(c,n);if(st)JS_FreeCString(c,st);return JS_UNDEFINED; }
FN(d_idbDeleteDatabase) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);const char* n=JS_ToCString(c,argv[0]);if(n){db_idb_delete_database(o,n);JS_FreeCString(c,n);}return JS_UNDEFINED; }
FN(d_idbList) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);const char* n=JS_ToCString(c,argv[0]);const char* st=JS_ToCString(c,argv[1]);if(!n||!st){if(n)JS_FreeCString(c,n);if(st)JS_FreeCString(c,st);return JS_EXCEPTION;}char* j=db_idb_list(o,n,st);JSValue r=JS_NewString(c,j?j:"[]");free(j);JS_FreeCString(c,n);JS_FreeCString(c,st);return r; }
FN(d_idbGet) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);const char* n=JS_ToCString(c,argv[0]);const char* st=JS_ToCString(c,argv[1]);const char* k=JS_ToCString(c,argv[2]);if(!n||!st||!k){if(n)JS_FreeCString(c,n);if(st)JS_FreeCString(c,st);if(k)JS_FreeCString(c,k);return JS_EXCEPTION;}char* j=db_idb_get(o,n,st,k);JS_FreeCString(c,n);JS_FreeCString(c,st);JS_FreeCString(c,k);if(!j)return JS_NULL;JSValue r=JS_NewString(c,j);free(j);return r; }
FN(d_idbPut) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);const char* n=JS_ToCString(c,argv[0]);const char* st=JS_ToCString(c,argv[1]);const char* k=JS_ToCString(c,argv[2]);const char* v=JS_ToCString(c,argv[3]);if(n&&st&&k&&v)db_idb_put(o,n,st,k,v);if(n)JS_FreeCString(c,n);if(st)JS_FreeCString(c,st);if(k)JS_FreeCString(c,k);if(v)JS_FreeCString(c,v);return JS_UNDEFINED; }
FN(d_idbRemove) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);const char* n=JS_ToCString(c,argv[0]);const char* st=JS_ToCString(c,argv[1]);const char* k=JS_ToCString(c,argv[2]);if(n&&st&&k)db_idb_remove(o,n,st,k);if(n)JS_FreeCString(c,n);if(st)JS_FreeCString(c,st);if(k)JS_FreeCString(c,k);return JS_UNDEFINED; }
FN(d_idbClear) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);const char* n=JS_ToCString(c,argv[0]);const char* st=JS_ToCString(c,argv[1]);if(n&&st)db_idb_clear(o,n,st);if(n)JS_FreeCString(c,n);if(st)JS_FreeCString(c,st);return JS_UNDEFINED; }
FN(d_swList) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);char* j=db_sw_list(o);JSValue r=JS_NewString(c,j?j:"[]");free(j);return r; }
FN(d_swSet) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);const char* sc=JS_ToCString(c,argv[0]);const char* u=JS_ToCString(c,argv[1]);if(sc&&u)db_sw_set(o,sc,u);if(sc)JS_FreeCString(c,sc);if(u)JS_FreeCString(c,u);return JS_UNDEFINED; }
FN(d_swRemove) { Page* p=P(c);char o[512];page_origin(p,o,sizeof o);const char* sc=JS_ToCString(c,argv[0]);if(sc){db_sw_remove(o,sc);JS_FreeCString(c,sc);}return JS_UNDEFINED; }
FN(d_profileId) { (void)argv; (void)argc; return JS_NewInt32(c, active_profile_id); }

/* ── WebSocket (только ws://; wss:// отдаёт событие ошибки — нет TLS) ── */
FN(d_wsConnect) {
    Page* p = P(c); const char* u = JS_ToCString(c, argv[0]); if (!u) return JS_NewInt32(c, -1);
    WsConn wc; int ok = ws_start(u, P(c)->url, &wc); JS_FreeCString(c, u);
    if (!ok) return JS_NewInt32(c, -1);
    WConn* w = calloc(1, sizeof *w); w->id = ++p->next_wconn; w->used = 1; w->ws = wc; w->next = p->wconns; p->wconns = w;
    return JS_NewInt32(c, w->id);
}
FN(d_wsPoll) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (WConn* w = p->wconns; w; w = w->next) if (w->id == id && w->used) {
        char type = 0; char* text = NULL; int code = 0;
        if (!ws_poll(&w->ws, &type, &text, &code)) return JS_NULL;
        if (!w->ws.used) w->used = 0;
        JSValue o = JS_NewObject(c);
        char ts[2] = { type, 0 }; JS_SetPropertyStr(c, o, "type", JS_NewString(c, ts));
        if (text) { JS_SetPropertyStr(c, o, "data", JS_NewString(c, text)); free(text); }
        if (type == 'C') JS_SetPropertyStr(c, o, "code", JS_NewInt32(c, code));
        return o;
    }
    return JS_NULL;
}
FN(d_wsSend) { Page* p = P(c); int id = arg_int(c, argv[0]); const char* s = JS_ToCString(c, argv[1]); if (s) { for (WConn* w = p->wconns; w; w = w->next) if (w->id == id && w->used) ws_send(&w->ws, s); JS_FreeCString(c, s); } return JS_UNDEFINED; }
FN(d_wsClose) { Page* p = P(c); int id = arg_int(c, argv[0]); int code = argc > 1 ? arg_int(c, argv[1]) : 1000; for (WConn* w = p->wconns; w; w = w->next) if (w->id == id && w->used) { ws_close(&w->ws, code); w->used = 0; } return JS_UNDEFINED; }

/* ── Native in-tree WebRTC peer/data-channel bridge. ── */
static MlbWebRtcPeer* webrtc_find_peer(Page* p, int id) {
    if (!p || id < 0) return NULL;
    for (WebRtcEntry* e = p->webrtc_peers; e; e = e->next)
        if (mlb_webrtc_peer_id(e->backend) == id) return e->backend;
    return NULL;
}
static const char* webrtc_event_name(int kind) {
    switch (kind) {
        case MLB_RTC_EVENT_DESCRIPTION: return "description";
        case MLB_RTC_EVENT_CANDIDATE: return "candidate";
        case MLB_RTC_EVENT_CONNECTION_STATE: return "connectionstate";
        case MLB_RTC_EVENT_ICE_STATE: return "iceconnectionstate";
        case MLB_RTC_EVENT_GATHERING_STATE: return "icegatheringstate";
        case MLB_RTC_EVENT_SIGNALING_STATE: return "signalingstate";
        case MLB_RTC_EVENT_DATA_CHANNEL: return "datachannel";
        case MLB_RTC_EVENT_CHANNEL_OPEN: return "open";
        case MLB_RTC_EVENT_CHANNEL_CLOSED: return "close";
        case MLB_RTC_EVENT_CHANNEL_ERROR: return "error";
        case MLB_RTC_EVENT_MESSAGE: return "message";
        default: return "unknown";
    }
}
FN(d_webrtcAvailable) { (void)c; (void)t; (void)argc; (void)argv; return JS_NewBool(c, mlb_webrtc_available()); }
FN(d_webrtcCreate) {
    Page* p = P(c);
    const char* urls = (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) ? JS_ToCString(c, argv[0]) : "";
    if (!urls) return JS_EXCEPTION;
    int relay_only = argc > 1 && JS_ToBool(c, argv[1]) > 0;
    MlbWebRtcPeer* b = mlb_webrtc_peer_create(urls, relay_only);
    if (urls[0] || (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]))) JS_FreeCString(c, urls);
    if (!b) return JS_NewInt32(c, -1);
    WebRtcEntry* e = (WebRtcEntry*)calloc(1, sizeof(*e));
    if (!e) { mlb_webrtc_peer_destroy(b); return JS_ThrowOutOfMemory(c); }
    e->backend = b; e->next = p->webrtc_peers; p->webrtc_peers = e;
    return JS_NewInt32(c, mlb_webrtc_peer_id(b));
}
FN(d_webrtcSetLocal) {
    Page* p = P(c); MlbWebRtcPeer* b = webrtc_find_peer(p, arg_int(c, argv[0]));
    const char* type = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    if (!type) return JS_NewBool(c, 0);
    int rc = mlb_webrtc_set_local_description(b, type); JS_FreeCString(c, type);
    return JS_NewBool(c, rc == 0);
}
FN(d_webrtcSetRemote) {
    Page* p = P(c); MlbWebRtcPeer* b = webrtc_find_peer(p, arg_int(c, argv[0]));
    const char* sdp = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    const char* type = argc > 2 ? JS_ToCString(c, argv[2]) : NULL;
    if (!sdp || !type) { if (sdp) JS_FreeCString(c, sdp); if (type) JS_FreeCString(c, type); return JS_NewBool(c, 0); }
    int rc = mlb_webrtc_set_remote_description(b, sdp, type);
    JS_FreeCString(c, sdp); JS_FreeCString(c, type); return JS_NewBool(c, rc == 0);
}
FN(d_webrtcAddCandidate) {
    Page* p = P(c); MlbWebRtcPeer* b = webrtc_find_peer(p, arg_int(c, argv[0]));
    const char* cand = (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) ? JS_ToCString(c, argv[1]) : "";
    const char* mid = (argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) ? JS_ToCString(c, argv[2]) : NULL;
    if (!cand) { if (mid) JS_FreeCString(c, mid); return JS_NewBool(c, 0); }
    int rc = mlb_webrtc_add_candidate(b, cand, mid);
    if (cand[0] || (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))) JS_FreeCString(c, cand);
    if (mid) JS_FreeCString(c, mid);
    return JS_NewBool(c, rc == 0);
}
FN(d_webrtcCreateDataChannel) {
    Page* p = P(c); MlbWebRtcPeer* b = webrtc_find_peer(p, arg_int(c, argv[0]));
    const char* label = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    int own_protocol = (argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2]));
    const char* protocol = own_protocol ? JS_ToCString(c, argv[2]) : "";
    int ordered = argc > 3 ? JS_ToBool(c, argv[3]) > 0 : 1;
    int negotiated = argc > 4 ? JS_ToBool(c, argv[4]) > 0 : 0;
    int id = -1, has_lifetime = 0, has_retransmits = 0;
    uint32_t lifetime = 0, retransmits = 0, priority = 256u;
    if (argc > 5 && !JS_IsNull(argv[5]) && !JS_IsUndefined(argv[5])) { if (JS_ToInt32(c, &id, argv[5]) < 0) id = -1; }
    if (argc > 6 && !JS_IsNull(argv[6]) && !JS_IsUndefined(argv[6])) { has_lifetime = 1; if (JS_ToUint32(c, &lifetime, argv[6]) < 0) has_lifetime = 0; }
    if (argc > 7 && !JS_IsNull(argv[7]) && !JS_IsUndefined(argv[7])) { has_retransmits = 1; if (JS_ToUint32(c, &retransmits, argv[7]) < 0) has_retransmits = 0; }
    if (argc > 8 && !JS_IsNull(argv[8]) && !JS_IsUndefined(argv[8]) && JS_ToUint32(c, &priority, argv[8]) < 0) priority = 0u;
    int rc = label ? mlb_webrtc_create_data_channel(b, label, protocol, ordered, negotiated, id,
                    has_lifetime, lifetime, has_retransmits, retransmits, priority) : -1;
    if (label) JS_FreeCString(c, label);
    if (own_protocol && protocol) JS_FreeCString(c, protocol);
    return JS_NewInt32(c, rc);
}
FN(d_webrtcSend) {
    Page* p = P(c); int id = arg_int(c, argv[0]); int binary = argc > 2 && JS_ToBool(c, argv[2]) > 0;
    int rc = -1;
    if (!binary) {
        size_t len = 0; const char* s = JS_ToCStringLen(c, &len, argv[1]); if (!s) return JS_EXCEPTION;
        for (WebRtcEntry* e = p->webrtc_peers; e; e = e->next) { rc = mlb_webrtc_channel_send(e->backend, id, s, len, 0); if (rc == 0) break; }
        JS_FreeCString(c, s);
    } else {
        size_t len = 0; uint8_t* bytes = JS_GetArrayBuffer(c, &len, argv[1]);
        if (!bytes && len) return JS_ThrowTypeError(c, "RTCDataChannel.send expects an ArrayBuffer for binary messages");
        for (WebRtcEntry* e = p->webrtc_peers; e; e = e->next) { rc = mlb_webrtc_channel_send(e->backend, id, bytes, len, 1); if (rc == 0) break; }
    }
    return JS_NewBool(c, rc == 0);
}
FN(d_webrtcChannelClose) {
    Page* p = P(c); int id = arg_int(c, argv[0]); int rc = -1;
    for (WebRtcEntry* e = p->webrtc_peers; e; e = e->next) { rc = mlb_webrtc_channel_close(e->backend, id); if (rc == 0) break; }
    return JS_NewBool(c, rc == 0);
}
FN(d_webrtcBufferedAmount) {
    Page* p = P(c); int id = arg_int(c, argv[0]), amount = 0;
    for (WebRtcEntry* e = p->webrtc_peers; e; e = e->next) { amount = mlb_webrtc_channel_buffered_amount(e->backend, id); if (amount > 0) break; }
    return JS_NewInt32(c, amount);
}
FN(d_webrtcChannelIsOpen) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (WebRtcEntry* e = p->webrtc_peers; e; e = e->next) if (mlb_webrtc_channel_is_open(e->backend, id)) return JS_TRUE;
    return JS_FALSE;
}
FN(d_webrtcChannelStream) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (WebRtcEntry* e = p->webrtc_peers; e; e = e->next) {
        int stream = mlb_webrtc_channel_stream(e->backend, id);
        if (stream >= 0) return JS_NewInt32(c, stream);
    }
    return JS_NewInt32(c, -1);
}
FN(d_webrtcClose) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (WebRtcEntry** pp = &p->webrtc_peers; *pp; pp = &(*pp)->next) {
        WebRtcEntry* e = *pp;
        if (mlb_webrtc_peer_id(e->backend) == id) { *pp = e->next; mlb_webrtc_peer_destroy(e->backend); free(e); return JS_TRUE; }
    }
    return JS_FALSE;
}
FN(d_webrtcPoll) {
    Page* p = P(c); MlbWebRtcPeer* b = webrtc_find_peer(p, arg_int(c, argv[0]));
    MlbWebRtcEvent* e = mlb_webrtc_next_event(b); if (!e) return JS_NULL;
    JSValue o = JS_NewObject(c);
    JS_SetPropertyStr(c, o, "kind", JS_NewString(c, webrtc_event_name(e->kind)));
    JS_SetPropertyStr(c, o, "id", JS_NewInt32(c, e->id));
    JS_SetPropertyStr(c, o, "state", JS_NewInt32(c, e->state));
    if (e->mid) JS_SetPropertyStr(c, o, "mid", JS_NewString(c, e->mid));
    /* MessageEvent.data must be present even for a zero-length payload. */
    if (e->kind == MLB_RTC_EVENT_MESSAGE) {
        const uint8_t *bytes = (const uint8_t *)(e->text ? e->text : "");
        if (e->binary)
            JS_SetPropertyStr(c, o, "data", JS_NewArrayBufferCopy(c, bytes, e->length));
        else
            JS_SetPropertyStr(c, o, "data", JS_NewStringLen(c, (const char *)bytes, e->length));
    } else if (e->text) {
        if (e->kind == MLB_RTC_EVENT_DESCRIPTION)
            JS_SetPropertyStr(c, o, "sdp", JS_NewStringLen(c, e->text, e->length));
        else if (e->kind == MLB_RTC_EVENT_CANDIDATE)
            JS_SetPropertyStr(c, o, "candidate", JS_NewStringLen(c, e->text, e->length));
        else if (e->kind == MLB_RTC_EVENT_DATA_CHANNEL)
            JS_SetPropertyStr(c, o, "label", JS_NewStringLen(c, e->text, e->length));
        else if (e->kind == MLB_RTC_EVENT_CHANNEL_ERROR)
            JS_SetPropertyStr(c, o, "message", JS_NewStringLen(c, e->text, e->length));
    }
    if (e->kind == MLB_RTC_EVENT_MESSAGE) JS_SetPropertyStr(c, o, "binary", JS_NewBool(c, e->binary));
    if (e->kind == MLB_RTC_EVENT_DATA_CHANNEL) {
        JS_SetPropertyStr(c, o, "ordered", JS_NewBool(c, e->ordered));
        JS_SetPropertyStr(c, o, "priority", JS_NewUint32(c, e->priority));
        JS_SetPropertyStr(c, o, "maxPacketLifeTime", e->has_max_packet_lifetime ?
                          JS_NewUint32(c, e->max_packet_lifetime) : JS_NULL);
        JS_SetPropertyStr(c, o, "maxRetransmits", e->has_max_retransmits ?
                          JS_NewUint32(c, e->max_retransmits) : JS_NULL);
    }
    mlb_webrtc_event_free(e); return o;
}

/* ── Structured clone + Web Worker IPC (QuickJS binary object graph, capped at 64 MiB). ── */
#define MLB_CLONE_MAX_BYTES (64u * 1024u * 1024u)
#define MLB_CLONE_MAX_TRANSFER 1024

typedef struct { JSValue *items; int count; } MlbTransferList;
static void mlb_transfer_free(JSContext* c, MlbTransferList* list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) JS_FreeValue(c, list->items[i]);
    free(list->items); list->items = NULL; list->count = 0;
}
static int mlb_transfer_collect(JSContext* c, JSValueConst arg, MlbTransferList* out) {
    memset(out, 0, sizeof *out);
    if (JS_IsUndefined(arg) || JS_IsNull(arg)) return 1;
    if (!JS_IsArray(c, arg)) { JS_ThrowTypeError(c, "DataCloneError: transfer must be an array"); return 0; }
    JSValue lv = JS_GetPropertyStr(c, arg, "length"); uint32_t count = 0;
    if (JS_IsException(lv) || JS_ToUint32(c, &count, lv) < 0) { JS_FreeValue(c, lv); return 0; }
    JS_FreeValue(c, lv);
    if (count > MLB_CLONE_MAX_TRANSFER) { JS_ThrowRangeError(c, "DataCloneError: too many transferable objects"); return 0; }
    if (!count) return 1;
    out->items = calloc(count, sizeof(JSValue));
    if (!out->items) { JS_ThrowOutOfMemory(c); return 0; }
    for (uint32_t i = 0; i < count; i++) {
        JSValue item = JS_GetPropertyUint32(c, arg, i);
        if (JS_IsException(item)) { mlb_transfer_free(c, out); return 0; }
        size_t byte_len = 0;
        uint8_t* data = JS_GetArrayBuffer(c, &byte_len, item);
        if (!data) {
            JSValue exc = JS_GetException(c); JS_FreeValue(c, exc); JS_FreeValue(c, item);
            mlb_transfer_free(c, out);
            JS_ThrowTypeError(c, "DataCloneError: only attached ArrayBuffer objects are transferable"); return 0;
        }
        for (int j = 0; j < out->count; j++) {
            if (JS_StrictEq(c, out->items[j], item)) {
                JS_FreeValue(c, item); mlb_transfer_free(c, out);
                JS_ThrowTypeError(c, "DataCloneError: duplicate transferable object"); return 0;
            }
        }
        out->items[out->count++] = item;
    }
    return 1;
}
static uint8_t* mlb_clone_write(JSContext* c, JSValueConst value, size_t* out_len) {
    *out_len = 0;
    uint8_t* bytes = JS_WriteObject(c, out_len, value, JS_WRITE_OBJ_REFERENCE);
    if (bytes && *out_len > MLB_CLONE_MAX_BYTES) {
        js_free(c, bytes); *out_len = 0;
        JS_ThrowRangeError(c, "DataCloneError: cloned message exceeds 64 MiB"); return NULL;
    }
    return bytes;
}
static void mlb_transfer_detach(JSContext* c, MlbTransferList* list) {
    for (int i = 0; i < list->count; i++) JS_DetachArrayBuffer(c, list->items[i]);
}
FN(d_structuredClone) {
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst transfer_arg = argc > 1 ? argv[1] : JS_UNDEFINED;
    MlbTransferList transfer;
    if (!mlb_transfer_collect(c, transfer_arg, &transfer)) return JS_EXCEPTION;
    size_t len = 0; uint8_t* bytes = mlb_clone_write(c, value, &len);
    if (!bytes) { mlb_transfer_free(c, &transfer); return JS_EXCEPTION; }
    JSValue clone = JS_ReadObject(c, bytes, len, JS_READ_OBJ_REFERENCE);
    js_free(c, bytes);
    if (!JS_IsException(clone)) mlb_transfer_detach(c, &transfer);
    mlb_transfer_free(c, &transfer);
    return clone;
}

/* Web Worker: isolate each context in its own process; structured-clone bytes cross IPC. */
FN(d_workerStart) {
    Page* p = P(c); const char* s = JS_ToCString(c, argv[0]); if (!s) return JS_NewInt32(c, -1);
    char ab[1100]; webpage_resolve_url(p->url, s, ab, sizeof ab); JS_FreeCString(c, s);
    WorkerConn wc; int ok = worker_start(ab, &wc);
    if (!ok) return JS_NewInt32(c, -1);
    WkConn* w = calloc(1, sizeof *w); if (!w) { worker_terminate(&wc); return JS_NewInt32(c, -1); }
    w->id = ++p->next_wkconn; w->used = 1; w->wk = wc; w->next = p->wkconns; p->wkconns = w;
    return JS_NewInt32(c, w->id);
}
FN(d_workerPoll) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (WkConn* w = p->wkconns; w; w = w->next) if (w->id == id && w->used) {
        char type = 0; unsigned char* bytes = NULL; size_t len = 0;
        if (!worker_poll(&w->wk, &type, &bytes, &len)) return JS_NULL;
        if (!w->wk.used) w->used = 0;
        JSValue o = JS_NewObject(c); char ts[2] = { type, 0 };
        JS_SetPropertyStr(c, o, "type", JS_NewString(c, ts));
        if (type == 'M' && bytes) {
            JSValue data = JS_ReadObject(c, bytes, len, JS_READ_OBJ_REFERENCE);
            if (JS_IsException(data)) { JSValue exc = JS_GetException(c); JS_FreeValue(c, exc); data = JS_NULL; }
            JS_SetPropertyStr(c, o, "data", data);
        } else if ((type == 'E') && bytes) {
            JS_SetPropertyStr(c, o, "data", JS_NewStringLen(c, (const char*)bytes, len));
        }
        free(bytes); return o;
    }
    return JS_NULL;
}
FN(d_workerPost) {
    Page* p = P(c); int id = arg_int(c, argv[0]); WkConn* target = NULL;
    for (WkConn* w = p->wkconns; w; w = w->next) if (w->id == id && w->used && w->wk.used) { target = w; break; }
    if (!target) return JS_UNDEFINED;
    JSValueConst value = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst transfer_arg = argc > 2 ? argv[2] : JS_UNDEFINED;
    MlbTransferList transfer;
    if (!mlb_transfer_collect(c, transfer_arg, &transfer)) return JS_EXCEPTION;
    size_t len = 0; uint8_t* bytes = mlb_clone_write(c, value, &len);
    if (!bytes) { mlb_transfer_free(c, &transfer); return JS_EXCEPTION; }
    int ok = worker_post(&target->wk, bytes, len);
    js_free(c, bytes);
    if (ok) mlb_transfer_detach(c, &transfer);
    mlb_transfer_free(c, &transfer);
    if (!ok) return JS_ThrowTypeError(c, "Worker message transport failed");
    return JS_UNDEFINED;
}
FN(d_workerTerminate) { Page* p = P(c); int id = arg_int(c, argv[0]); for (WkConn* w = p->wkconns; w; w = w->next) if (w->id == id && w->used) { worker_terminate(&w->wk); w->used = 0; } return JS_UNDEFINED; }

/* ── Canvas 2D: сам буфер пикселей и вся геометрия (path fill/stroke/transform) живут в JS
   (js_prelude.js, как Uint8ClampedArray) — здесь только то, что нельзя честно сделать без движка
   шрифтов: измерение и растеризация текста через уже существующий ttf_render_string_ex. ── */
static void ab_free_cb(JSRuntime* rt, void* opaque, void* ptr) { (void)rt; (void)opaque; free(ptr); }
/* toDataURL(): кодируем в PNG (stb_image_write) и сами же — в base64 (без сторонних либ). */
static const char B64E[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char* b64_encode_alloc(const unsigned char* in, size_t n) {
    char* out = malloc(((n + 2) / 3) * 4 + 1); if (!out) return NULL; size_t i = 0, j = 0;
    for (; i + 3 <= n; i += 3) { unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2]; out[j++] = B64E[(v >> 18) & 63]; out[j++] = B64E[(v >> 12) & 63]; out[j++] = B64E[(v >> 6) & 63]; out[j++] = B64E[v & 63]; }
    size_t rem = n - i;
    if (rem == 1) { unsigned v = (unsigned)in[i] << 16; out[j++] = B64E[(v >> 18) & 63]; out[j++] = B64E[(v >> 12) & 63]; out[j++] = '='; out[j++] = '='; }
    else if (rem == 2) { unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8); out[j++] = B64E[(v >> 18) & 63]; out[j++] = B64E[(v >> 12) & 63]; out[j++] = B64E[(v >> 6) & 63]; out[j++] = '='; }
    out[j] = 0; return out;
}
typedef struct { unsigned char* d; size_t n, cap; } WBuf;
static void png_write_cb(void* ctx, void* data, int size) { WBuf* b = ctx; if (b->n + (size_t)size > b->cap) { b->cap = (b->n + (size_t)size) * 2 + 64; b->d = realloc(b->d, b->cap); } memcpy(b->d + b->n, data, (size_t)size); b->n += (size_t)size; }
FN(d_canvasToDataURL) {
    size_t bsz = 0; uint8_t* rgba = JS_GetArrayBuffer(c, &bsz, argv[0]);
    int w = arg_int(c, argv[1]), h = arg_int(c, argv[2]);
    if (!rgba || w <= 0 || h <= 0 || bsz < (size_t)w * h * 4) return JS_NewString(c, "data:,");
    WBuf b = {0}; stbi_write_png_to_func(png_write_cb, &b, w, h, 4, rgba, w * 4);
    if (!b.d) return JS_NewString(c, "data:,");
    char* b64 = b64_encode_alloc(b.d, b.n); free(b.d);
    if (!b64) return JS_NewString(c, "data:,");
    size_t total = strlen("data:image/png;base64,") + strlen(b64) + 1;
    char* out = malloc(total); snprintf(out, total, "data:image/png;base64,%s", b64); free(b64);
    JSValue r = JS_NewString(c, out); free(out); return r;
}
/* Приходит из JS раз в тик (батчится через defer, см. js_prelude.js) после любых изменений пикселей канваса:
   переносит RGBA-буфер в кэш картинок под синтетическим ключом "canvas:<idx узла>", откуда его подхватит
   обычный путь отрисовки C_IMG/blit_img (см. layout2.c: repl=R_CANVAS, webpage.h: webpage_image_inject). */
FN(d_canvasCommit) {
    Page* p=P(c); NODE(0); size_t bsz=0; uint8_t*rgba=JS_GetArrayBuffer(c,&bsz,argv[1]);
    int w=arg_int(c,argv[2]),h=arg_int(c,argv[3]);
    if(rgba&&w>0&&h>0&&w<=4096&&h<=4096&&(uint64_t)w*h<=4000000ull&&bsz>=(size_t)w*h*4){
        flush_style(p); (void)dyn_image_set_rgba(p,arg_int(c,argv[0]),0,rgba,w,h);
    }
    return JS_UNDEFINED;
}

static int media_resolve_allowed(Page*p,const char*input,char*out,size_t cap){
    if(!p||!input||!*input)return 0;webpage_resolve_url(p->url,input,out,cap);if(!out[0])return 0;
    int remote_page=!strncmp(p->url,"http://",7)||!strncmp(p->url,"https://",8);
    /* Remote pages may only fetch media over HTTP(S); don't expose FFmpeg's extra protocols to sites. */
    int remote_src=!strncmp(out,"http://",7)||!strncmp(out,"https://",8);
    if(remote_page&&!remote_src)return 0;
    if(remote_src)return 1;
    if(!remote_page&&(!strstr(out,"://")||!strncmp(out,"file://",7)))return 1;
    return 0;
}
FN(d_mediaFastSeekTarget){
    Page*p=P(c);int id=arg_int(c,argv[0]);double target=0.0;
    if(argc<2||JS_ToFloat64(c,&target,argv[1])||!isfinite(target)||target<0.0)return JS_NewFloat64(c,target);
    MediaState*m=media_state(p,id,0);
    if(!m||!m->source[0]||!m->info.has_video||!m->info.can_seek)return JS_NewFloat64(c,target);
    double selected=media_fast_seek_target(m->source,target);
    if(!isfinite(selected)||selected<0.0||selected>target)selected=target;
    if(isfinite(m->info.duration)&&selected>m->info.duration)selected=m->info.duration;
    return JS_NewFloat64(c,selected);
}
FN(d_mediaInfo){
    Page*p=P(c);int id=arg_int(c,argv[0]);const char*src=JS_ToCString(c,argv[1]);if(!src)return JS_EXCEPTION;
    char url[1200];int allowed=media_resolve_allowed(p,src,url,sizeof url);JS_FreeCString(c,src);MediaInfo info;memset(&info,0,sizeof info);
    int probed=allowed&&media_probe(url,&info);MediaState*m=media_state(p,id,1);
    if(m){media_state_stop(m);if(probed){snprintf(m->source,sizeof m->source,"%s",url);m->info=info;}else{m->source[0]=0;memset(&m->info,0,sizeof m->info);}}
    JSValue o=JS_NewObject(c);JS_SetPropertyStr(c,o,"supported",JS_NewBool(c,probed&&media_available()));JS_SetPropertyStr(c,o,"width",JS_NewInt32(c,info.width));JS_SetPropertyStr(c,o,"height",JS_NewInt32(c,info.height));JS_SetPropertyStr(c,o,"hasVideo",JS_NewBool(c,info.has_video));JS_SetPropertyStr(c,o,"hasAudio",JS_NewBool(c,info.has_audio));JS_SetPropertyStr(c,o,"canSeek",JS_NewBool(c,info.can_seek));JS_SetPropertyStr(c,o,"duration",JS_NewFloat64(c,info.duration));return o;
}
FN(d_mediaStart){
    Page*p=P(c);int id=arg_int(c,argv[0]);const char*src=JS_ToCString(c,argv[1]);double start=0,volume=1,rate=1.0;JS_ToFloat64(c,&start,argv[2]);JS_ToFloat64(c,&volume,argv[3]);if(argc>7&&!JS_IsUndefined(argv[7]))JS_ToFloat64(c,&rate,argv[7]);int video=JS_ToBool(c,argv[4])>0;int w=arg_int(c,argv[5]),h=arg_int(c,argv[6]);
    if(!src)return JS_EXCEPTION;char url[1200];int allowed=media_resolve_allowed(p,src,url,sizeof url);JS_FreeCString(c,src);if(!allowed)return JS_NewBool(c,0);
    MediaState*m=media_state(p,id,1);if(!m)return JS_NewBool(c,0);media_state_stop(m);
    if(strcmp(m->source,url)!=0||(!m->info.has_video&&!m->info.duration)){MediaInfo info;memset(&info,0,sizeof info);if(!media_probe(url,&info))return JS_NewBool(c,0);m->info=info;snprintf(m->source,sizeof m->source,"%s",url);}
    m->backend=media_start_rate(url,start,volume,rate,video,w,h,&m->info);return JS_NewBool(c,m->backend!=NULL);
}
FN(d_mediaSetVolume){
    Page*p=P(c);int id=arg_int(c,argv[0]);double volume=1.0;
    if(argc<2||JS_ToFloat64(c,&volume,argv[1]))return JS_EXCEPTION;
    if(!isfinite(volume)||volume<0.0||volume>1.0)return JS_NewBool(c,0);
    MediaState*m=media_state(p,id,0);
    return JS_NewBool(c,m&&m->backend&&media_set_volume(m->backend,volume));
}
FN(d_mediaPause){
    Page*p=P(c);int id=arg_int(c,argv[0]);MediaState*m=media_state(p,id,0);
    return JS_NewBool(c,m&&m->backend&&media_pause(m->backend));
}
FN(d_mediaResume){
    Page*p=P(c);int id=arg_int(c,argv[0]);MediaState*m=media_state(p,id,0);
    return JS_NewBool(c,m&&m->backend&&media_resume(m->backend));
}
FN(d_mediaPoll){
    Page*p=P(c);int id=arg_int(c,argv[0]);MediaState*m=media_state(p,id,0);if(!m||!m->backend)return JS_NewInt32(c,0);
    const unsigned char*rgba=NULL;int w=0,h=0;int result=media_poll(m->backend,&rgba,&w,&h);
    if(result==1&&rgba){(void)dyn_image_set_rgba(p,id,1,rgba,w,h);return JS_NewInt32(c,1);}
    return JS_NewInt32(c,result);
}
/* Internal bounded stream-input bridge used by the upcoming MediaSource layer.
   append returns 1 on acceptance, 0 on quota pressure, -1 on closed/invalid input. */
FN(d_mediaStreamStart){
    Page*p=P(c);int id=arg_int(c,argv[0]);int capacity=argc>1?arg_int(c,argv[1]):(int)MEDIA_BYTE_QUEUE_DEFAULT_CAPACITY;
    double volume=1.0;if(argc>2&&!JS_IsUndefined(argv[2]))JS_ToFloat64(c,&volume,argv[2]);
    int video=argc>3?JS_ToBool(c,argv[3])>0:1;int audio=argc>4?JS_ToBool(c,argv[4])>0:0;
    int w=argc>5?arg_int(c,argv[5]):0,h=argc>6?arg_int(c,argv[6]):0;
    double start=0.0,rate=1.0;if(argc>7&&!JS_IsUndefined(argv[7])&&JS_ToFloat64(c,&start,argv[7]))return JS_NewBool(c,0);if(argc>8&&!JS_IsUndefined(argv[8])&&JS_ToFloat64(c,&rate,argv[8]))return JS_NewBool(c,0);
    if(capacity<=0||(size_t)capacity>MEDIA_BYTE_QUEUE_MAX_CAPACITY||(!video&&!audio)||!isfinite(volume)||volume<0||volume>1||!isfinite(start)||start<0||!isfinite(rate)||rate<0.0625||rate>4.0)return JS_NewBool(c,0);
    MediaState*m=media_state(p,id,1);if(!m)return JS_NewBool(c,0);media_state_stop(m);
    m->stream_queue=media_byte_queue_create((size_t)capacity);if(!m->stream_queue)return JS_NewBool(c,0);
    m->backend=media_start_stream_at_rate(m->stream_queue,volume,video,audio,w,h,start,rate);
    if(!m->backend){media_byte_queue_destroy(m->stream_queue);m->stream_queue=NULL;return JS_NewBool(c,0);}
    memset(&m->info,0,sizeof m->info);m->source[0]=0;
    return JS_NewBool(c,1);
}
static uint8_t* media_get_buffer_source(JSContext*c,JSValueConst value,size_t*length){
    if(length)*length=0;
    uint8_t*data=JS_GetArrayBuffer(c,length,value);if(data)return data;
    JSValue ex=JS_GetException(c);JS_FreeValue(c,ex);
    size_t offset=0,bytes_per_element=0,n=0;JSValue ab=JS_GetTypedArrayBuffer(c,value,&offset,&n,&bytes_per_element);
    if(JS_IsException(ab)){ex=JS_GetException(c);JS_FreeValue(c,ex);return NULL;}
    size_t total=0;uint8_t*base=JS_GetArrayBuffer(c,&total,ab);JS_FreeValue(c,ab);
    if(!base||offset>total||n>total-offset)return NULL;
    if(length)*length=n;return base+offset;
}
FN(d_mediaStreamAppend){
    Page*p=P(c);int id=arg_int(c,argv[0]);MediaState*m=media_state(p,id,0);
    if(!m||!m->stream_queue||argc<2)return JS_NewInt32(c,-1);
    size_t n=0;uint8_t*data=media_get_buffer_source(c,argv[1],&n);
    if(!data||n==0)return JS_NewInt32(c,-1);
    return JS_NewInt32(c,media_byte_queue_write(m->stream_queue,data,n));
}
FN(d_mediaStreamEnd){
    Page*p=P(c);int id=arg_int(c,argv[0]);MediaState*m=media_state(p,id,0);
    if(!m||!m->stream_queue)return JS_NewBool(c,0);
    int failed=argc>1&&JS_ToBool(c,argv[1])>0;
    if(failed)media_byte_queue_fail(m->stream_queue);else media_byte_queue_close(m->stream_queue);
    return JS_NewBool(c,1);
}
FN(d_mediaStatus){
    Page*p=P(c);int id=arg_int(c,argv[0]);MediaState*m=media_state(p,id,0);MediaBackendStatus st;memset(&st,0,sizeof st);
    if(!m||!m->backend){st.state=MEDIA_BACKEND_FAILED;}
    else (void)media_get_status(m->backend,&st);
    JSValue o=JS_NewObject(c);
    JS_SetPropertyStr(c,o,"state",JS_NewInt32(c,(int)st.state));
    JS_SetPropertyStr(c,o,"canSeek",JS_NewBool(c,st.can_seek));
    JS_SetPropertyStr(c,o,"duration",JS_NewFloat64(c,st.duration));
    JS_SetPropertyStr(c,o,"hasClock",JS_NewBool(c,st.has_clock));
    JS_SetPropertyStr(c,o,"audioClock",JS_NewBool(c,st.audio_clock));
    JS_SetPropertyStr(c,o,"audioLatencyEstimate",JS_NewFloat64(c,st.audio_output_latency_estimate_seconds));
    JS_SetPropertyStr(c,o,"videoFramesDropped",JS_NewFloat64(c,(double)st.video_frames_dropped));
    JS_SetPropertyStr(c,o,"hasFirstVideoFrameTime",JS_NewBool(c,st.has_first_video_frame_time));
    JS_SetPropertyStr(c,o,"firstVideoFrameTime",JS_NewFloat64(c,st.first_video_frame_time));
    JS_SetPropertyStr(c,o,"hasVideoFrameTime",JS_NewBool(c,st.has_video_frame_time));
    JS_SetPropertyStr(c,o,"videoFrameTime",JS_NewFloat64(c,st.video_frame_time));
    JS_SetPropertyStr(c,o,"liveVolumeControl",JS_NewBool(c,st.live_volume_control));
    JS_SetPropertyStr(c,o,"audioVolume",JS_NewFloat64(c,st.audio_volume));
    JS_SetPropertyStr(c,o,"audioFramesConsumed",JS_NewFloat64(c,(double)st.audio_frames_consumed));
    JS_SetPropertyStr(c,o,"audioQueueBytes",JS_NewInt32(c,(int)st.audio_queue_bytes));
    JS_SetPropertyStr(c,o,"currentTime",JS_NewFloat64(c,st.current_time));
    JSValue ranges=JS_NewArray(c);
    for(size_t i=0;i<st.buffered_count&&i<MEDIA_MAX_BUFFERED_RANGES;i++){
        JSValue pair=JS_NewArray(c);
        JS_SetPropertyUint32(c,pair,0,JS_NewFloat64(c,st.buffered_start[i]));
        JS_SetPropertyUint32(c,pair,1,JS_NewFloat64(c,st.buffered_end[i]));
        JS_SetPropertyUint32(c,ranges,(uint32_t)i,pair);
    }
    JS_SetPropertyStr(c,o,"buffered",ranges);
    JS_SetPropertyStr(c,o,"inputQueuedBytes",JS_NewInt32(c,m&&m->stream_queue?(int)media_byte_queue_available(m->stream_queue):0));
    JS_SetPropertyStr(c,o,"inputFreeBytes",JS_NewInt32(c,m&&m->stream_queue?(int)media_byte_queue_free_space(m->stream_queue):0));
    JS_SetPropertyStr(c,o,"inputClosed",JS_NewBool(c,m&&m->stream_queue?media_byte_queue_is_closed(m->stream_queue):1));
    JS_SetPropertyStr(c,o,"inputFailed",JS_NewBool(c,m&&m->stream_queue?media_byte_queue_is_failed(m->stream_queue):0));
    return o;
}
FN(d_mediaStop){Page*p=P(c);int id=arg_int(c,argv[0]);MediaState*m=media_state(p,id,0);media_state_stop(m);return JS_UNDEFINED;}
FN(d_webglCall){
    int id=arg_int(c,argv[0]);const char*method=JS_ToCString(c,argv[1]);if(!method)return JS_EXCEPTION;
    int argc2=argc-2;if(argc2<0)argc2=0;if(argc2>16)argc2=16;WebGLArg a[16];memset(a,0,sizeof a);char*owned[16]={0};JSValue buffers[16];int hasbuf[16]={0};memset(buffers,0,sizeof buffers);
    for(int i=0;i<argc2;i++){
        JSValueConst v=argv[i+2];if(JS_IsString(v)){owned[i]=(char*)JS_ToCString(c,v);a[i].type=WEBGL_ARG_STRING;a[i].string=owned[i];}
        else if(JS_IsNumber(v)||JS_IsBool(v)||JS_IsNull(v)||JS_IsUndefined(v)){double x=0;if(JS_IsBool(v))x=JS_ToBool(c,v)>0;else (void)JS_ToFloat64(c,&x,v);a[i].type=WEBGL_ARG_NUMBER;a[i].number=x;}
        else {
            int is_matrix = !strcmp(method,"uniformMatrix2fv")||!strcmp(method,"uniformMatrix3fv")||!strcmp(method,"uniformMatrix4fv");
            int is_uniform_vector = !strcmp(method,"uniform1fv")||!strcmp(method,"uniform2fv")||!strcmp(method,"uniform3fv")||!strcmp(method,"uniform4fv")||!strcmp(method,"uniform1iv")||!strcmp(method,"uniform2iv")||!strcmp(method,"uniform3iv")||!strcmp(method,"uniform4iv");
            int data_arg=(!strcmp(method,"bufferData")&&i==1)||(!strcmp(method,"bufferSubData")&&i==2)||(is_matrix&&i==2)||(is_uniform_vector&&i==1)||(!strcmp(method,"texImage2D")&&i==8);
            if(data_arg){size_t off=0,len=0,bpe=0,absize=0;JSValue ab=JS_GetTypedArrayBuffer(c,v,&off,&len,&bpe);uint8_t*ptr=NULL;int elem=WEBGL_ELEM_U8;
                if(!JS_IsException(ab)){ptr=JS_GetArrayBuffer(c,&absize,ab);if(ptr&&off<=absize&&len<=absize-off){ptr+=off;a[i].type=WEBGL_ARG_BUFFER;a[i].data=ptr;a[i].size=len;a[i].elem_type=bpe==4?WEBGL_ELEM_F32:bpe==2?WEBGL_ELEM_U16:bpe==1?WEBGL_ELEM_U8:WEBGL_ELEM_U32;hasbuf[i]=1;}JS_FreeValue(c,ab);}
                else {JSValue ex=JS_GetException(c);JS_FreeValue(c,ex);ptr=JS_GetArrayBuffer(c,&len,v);if(ptr){a[i].type=WEBGL_ARG_BUFFER;a[i].data=ptr;a[i].size=len;a[i].elem_type=elem;hasbuf[i]=1;}}
            } else {a[i].type=WEBGL_ARG_NUMBER;a[i].number=0;}
        }
    }
    WebGLResult result;memset(&result,0,sizeof result);int ok=0;
    if(!strcmp(method,"init"))ok=webgl_context_create(id,argc2>0?(int)a[0].number:300,argc2>1?(int)a[1].number:150),result.type=WEBGL_RESULT_NUMBER,result.number=ok;
    else ok=webgl_call(id,method,argc2,a,&result);
    for(int i=0;i<argc2;i++)if(owned[i])JS_FreeCString(c,owned[i]);JS_FreeCString(c,method);
    if(!ok)return JS_UNDEFINED;if(result.type==WEBGL_RESULT_STRING)return JS_NewString(c,result.string);if(result.type==WEBGL_RESULT_NUMBER)return JS_NewFloat64(c,result.number);return JS_UNDEFINED;
}
static void webgl_arraybuffer_free(JSRuntime *rt, void *opaque, void *ptr) {
    (void)rt; (void)opaque;
    webgl_free_buffer(ptr);
}
FN(d_webglReadback){
    int id=arg_int(c,argv[0]),w=arg_int(c,argv[1]),h=arg_int(c,argv[2]);size_t len=0;unsigned char*buf=webgl_readback(id,w,h,&len);if(!buf)return JS_NULL;
    return JS_NewArrayBuffer(c,buf,len,webgl_arraybuffer_free,NULL,0);
}
FN(d_canvasMeasureText) {
    const char* text = JS_ToCString(c, argv[0]); const char* fam = JS_ToCString(c, argv[1]);
    int size = arg_int(c, argv[2]); int style = arg_int(c, argv[3]);
    float w = 0;
    if (text && fam) { TTFFont* f = font_manager_get(fam); if (f && size > 0) w = ttf_text_width_f(f, text, size, style); }
    if (text) JS_FreeCString(c, text); if (fam) JS_FreeCString(c, fam);
    return JS_NewFloat64(c, w);
}
/* buf — ArrayBuffer с пикселями канваса RGBA8888 (w*h*4 байт, alpha не используется/всегда непрозрачно —
   упрощение: у канваса в этом движке нет честного альфа-канала назначения, см. пояснение в js_prelude.js).
   Рендерит текст поверх буфера НА МЕСТЕ и возвращает ширину строки. */
FN(d_canvasText) {
    Page* p = P(c); (void)p;
    size_t bsz = 0; uint8_t* rgba = JS_GetArrayBuffer(c, &bsz, argv[0]);
    int w = arg_int(c, argv[1]), h = arg_int(c, argv[2]);
    const char* text = JS_ToCString(c, argv[3]); const char* fam = JS_ToCString(c, argv[4]);
    int size = arg_int(c, argv[5]), style = arg_int(c, argv[6]), colorRGB = arg_int(c, argv[7]);
    int x = arg_int(c, argv[8]), y = arg_int(c, argv[9]);
    float outw = 0;
    if (rgba && text && fam && w > 0 && h > 0 && bsz >= (size_t)w * h * 4) {
        TTFFont* f = font_manager_get(fam);
        if (f && size > 0) {
            uint32_t* words = malloc((size_t)w * h * sizeof(uint32_t));
            if (words) {
                for (long i = 0; i < (long)w * h; i++) words[i] = ((uint32_t)rgba[i * 4] << 16) | ((uint32_t)rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2];
                outw = (float)ttf_render_string_ex(f, text, words, w, 0, 0, w, h, size, x, y, (uint32_t)colorRGB & 0xFFFFFFu, style);
                for (long i = 0; i < (long)w * h; i++) { uint32_t wd = words[i]; rgba[i * 4] = (uint8_t)(wd >> 16); rgba[i * 4 + 1] = (uint8_t)(wd >> 8); rgba[i * 4 + 2] = (uint8_t)wd; rgba[i * 4 + 3] = 255; }
                free(words);
            }
        }
    }
    if (text) JS_FreeCString(c, text); if (fam) JS_FreeCString(c, fam);
    return JS_NewFloat64(c, outw);
}
/* Декодированные пиксели картинки (для canvas drawImage(imgEl,...)) — RGBA8888, ArrayBuffer. NULL при неудаче/незагруженности. */
FN(d_imageDecode) {
    Page* p = P(c); const char* s = JS_ToCString(c, argv[0]); if (!s) return JS_NULL;
    char ab[1100]; webpage_resolve_url(p->url, s, ab, sizeof ab); JS_FreeCString(c, s);
    ImgPix* im = webpage_image_get(ab); if (!im || !im->px) return JS_NULL;
    uint8_t* buf = malloc((size_t)im->w * im->h * 4); if (!buf) return JS_NULL;
    for (long i = 0; i < (long)im->w * im->h; i++) { uint32_t px = im->px[i]; buf[i * 4] = (uint8_t)(px >> 16); buf[i * 4 + 1] = (uint8_t)(px >> 8); buf[i * 4 + 2] = (uint8_t)px; buf[i * 4 + 3] = (uint8_t)(px >> 24); }
    JSValue ab_ = JS_NewArrayBuffer(c, buf, (size_t)im->w * im->h * 4, ab_free_cb, NULL, 0);
    JSValue o = JS_NewObject(c); JS_SetPropertyStr(c, o, "width", JS_NewInt32(c, im->w)); JS_SetPropertyStr(c, o, "height", JS_NewInt32(c, im->h)); JS_SetPropertyStr(c, o, "buffer", ab_);
    return o;
}

FN(d_log) { int lvl = arg_int(c, argv[0]); const char* s = JS_ToCString(c, argv[1]); if (s) { gal_log(lvl >= 3 ? "ERROR" : lvl == 2 ? "WARN" : "INFO", "[console] %.1000s", s); JS_FreeCString(c, s); } return JS_UNDEFINED; }
FN(d_now) { return JS_NewFloat64(c, now_ms()); }
FN(d_audioStart) { int rate=argc>0?arg_int(c,argv[0]):44100; return JS_NewBool(c,msdl_audio_start(rate)); }
FN(d_audioStop) { msdl_audio_stop(); return JS_UNDEFINED; }
FN(d_audioQueued) {
#ifndef _WIN32
    return JS_NewInt32(c, msdl_dev && msdl_queued ? (int)msdl_queued(msdl_dev) : 0);
#else
    return JS_NewInt32(c,0);
#endif
}
FN(d_audioWrite) {
#ifndef _WIN32
    if (!msdl_dev || !msdl_queue || !msdl_queued) return JS_NewBool(c,0);
    size_t n=0; uint8_t* data=JS_GetArrayBuffer(c,&n,argv[0]); if (!data || !n || n>1024*1024 || n%8) return JS_NewBool(c,0);
    /* Keep latency bounded: drop a block instead of adding an unbounded audio backlog. */
    if (msdl_queued(msdl_dev) > 44100u*2u*4u/5u) return JS_NewBool(c,0);
    return JS_NewBool(c,msdl_queue(msdl_dev,data,(uint32_t)n)==0);
#else
    return JS_NewBool(c,0);
#endif
}

/* Web Crypto's small, synchronous native primitives are kept behind the JS facade.
   Random bytes come from OpenSSL's OS-seeded CSPRNG; digest runs are bounded to 64 MiB. */
FN(d_cryptoRandom) {
#ifdef HAVE_OPENSSL_VAULT
    if (argc < 3) return JS_ThrowTypeError(c, "cryptoRandom expects an ArrayBuffer, offset and length");
    size_t total = 0; uint8_t* data = JS_GetArrayBuffer(c, &total, argv[0]);
    if (!data && total != 0) return JS_EXCEPTION;
    uint64_t off = 0, len = 0;
    if (JS_ToIndex(c, &off, argv[1]) < 0 || JS_ToIndex(c, &len, argv[2]) < 0) return JS_EXCEPTION;
    if (off > total || len > (uint64_t)total - off) return JS_ThrowRangeError(c, "cryptoRandom byte range is outside the buffer");
    if (len > 65536) return JS_ThrowRangeError(c, "getRandomValues may request at most 65536 bytes");
    if (len && RAND_bytes(data + (size_t)off, (int)len) != 1) return JS_ThrowTypeError(c, "system cryptographic random generator failed");
    return JS_UNDEFINED;
#else
    (void)argc; (void)argv;
    return JS_ThrowTypeError(c, "Web Crypto requires an OpenSSL-enabled build");
#endif
}
FN(d_cryptoDigest) {
#ifdef HAVE_OPENSSL_VAULT
    if (argc < 4) return JS_ThrowTypeError(c, "cryptoDigest expects algorithm, ArrayBuffer, offset and length");
    const char* raw = JS_ToCString(c, argv[0]); if (!raw) return JS_EXCEPTION;
    char name[32]; size_t nl = strlen(raw);
    if (!nl || nl >= sizeof name) { JS_FreeCString(c, raw); return JS_ThrowRangeError(c, "unsupported digest algorithm name"); }
    for (size_t i = 0; i <= nl; i++) name[i] = (char)toupper((unsigned char)raw[i]);
    JS_FreeCString(c, raw);
    if (strcmp(name, "SHA-1") && strcmp(name, "SHA-256") && strcmp(name, "SHA-384") && strcmp(name, "SHA-512"))
        return JS_ThrowTypeError(c, "unsupported Web Crypto digest algorithm (use SHA-1, SHA-256, SHA-384 or SHA-512)");
    const EVP_MD* md = EVP_get_digestbyname(name);
    if (!md) return JS_ThrowTypeError(c, "requested digest is unavailable in this OpenSSL build");
    size_t total = 0; uint8_t* data = JS_GetArrayBuffer(c, &total, argv[1]);
    if (!data && total != 0) return JS_EXCEPTION;
    uint64_t off = 0, len = 0;
    if (JS_ToIndex(c, &off, argv[2]) < 0 || JS_ToIndex(c, &len, argv[3]) < 0) return JS_EXCEPTION;
    if (off > total || len > (uint64_t)total - off) return JS_ThrowRangeError(c, "digest byte range is outside the buffer");
    if (len > 64u * 1024u * 1024u) return JS_ThrowRangeError(c, "digest input exceeds the 64 MiB implementation limit");
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int digest_len = 0;
    static const unsigned char empty = 0;
    const void* input = len ? (const void*)(data + (size_t)off) : (const void*)&empty;
    if (EVP_Digest(input, (size_t)len, digest, &digest_len, md, NULL) != 1)
        return JS_ThrowTypeError(c, "digest calculation failed");
    JSValue out = JS_NewArrayBufferCopy(c, digest, digest_len);
    OPENSSL_cleanse(digest, sizeof digest);
    return out;
#else
    (void)argc; (void)argv;
    return JS_ThrowTypeError(c, "crypto.subtle.digest requires an OpenSSL-enabled build");
#endif
}
FN(d_readSelectedFile) {
    Page* p = P(c); NODE(0); (void)p;
    if (!n || n->type != TAG_INPUT || strcmp(n->input_type, "file") || !n->file_selected || !n->value[0])
        return JS_NewArrayBufferCopy(c, (const uint8_t*)"", 0);
    FILE* f = fopen(n->value, "rb");
    if (!f) return JS_ThrowTypeError(c, "Selected file is no longer readable");
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return JS_ThrowTypeError(c, "Could not seek selected file"); }
    long sz = ftell(f);
    if (sz < 0 || sz > 32 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return JS_ThrowRangeError(c, "Selected file exceeds 32 MiB limit"); }
    size_t len = (size_t)sz; uint8_t* bytes = malloc(len ? len : 1);
    if (!bytes) { fclose(f); return JS_ThrowOutOfMemory(c); }
    size_t got = len ? fread(bytes, 1, len, f) : 0; int ok = got == len && !ferror(f); fclose(f);
    if (!ok) { free(bytes); return JS_ThrowTypeError(c, "Could not read selected file"); }
    JSValue result = JS_NewArrayBufferCopy(c, bytes, len); free(bytes); return result;
}

FN(d_readDroppedFile) {
    Page* p=P(c);const char* path=JS_ToCString(c,argv[0]);if(!path)return JS_EXCEPTION;
    if(!p->drop_authorized_path[0]||strcmp(path,p->drop_authorized_path)!=0){JS_FreeCString(c,path);return JS_ThrowTypeError(c,"File access is only allowed for an OS-dropped file during its drop event");}
    p->drop_authorized_path[0]=0;FILE*fp=fopen(path,"rb");JS_FreeCString(c,path);if(!fp)return JS_ThrowTypeError(c,"Could not read dropped file");
    if(fseek(fp,0,SEEK_END)!=0){fclose(fp);return JS_ThrowTypeError(c,"Could not seek dropped file");}long sz=ftell(fp);if(sz<0||sz>32*1024*1024){fclose(fp);return JS_ThrowRangeError(c,"Dropped file exceeds 32 MiB limit");}if(fseek(fp,0,SEEK_SET)!=0){fclose(fp);return JS_ThrowTypeError(c,"Could not seek dropped file");}
    uint8_t*buf=malloc((size_t)sz? (size_t)sz:1);if(!buf){fclose(fp);return JS_ThrowOutOfMemory(c);}size_t got=fread(buf,1,(size_t)sz,fp);fclose(fp);if(got!=(size_t)sz){free(buf);return JS_ThrowTypeError(c,"Could not read complete dropped file");}
    JSValue out=JS_NewArrayBufferCopy(c,buf,(size_t)sz);free(buf);return out;
}
FN(d_submit) {
    Page* p = P(c); NODE(0); DOMNode* submitter = NULL;
    if (argc > 1 && JS_IsNumber(argv[1])) { int sid = arg_int(c, argv[1]); if (sid >= 0 && sid < p->nn) submitter = p->nodes[sid]; }
    WebFormSubmission request;
    if (webpage_form_request(n, submitter, &request)) {
        snprintf(p->nav, sizeof p->nav, "%s", request.url); p->has_nav = 1;
        free(p->nav_body); p->nav_body = NULL; p->nav_body_len = 0; p->nav_method[0] = 0; p->nav_headers[0] = 0;
        if (!strcmp(request.method, "POST")) {
            snprintf(p->nav_method, sizeof p->nav_method, "%s", request.method);
            snprintf(p->nav_headers, sizeof p->nav_headers, "%s", request.headers);
            p->nav_body = request.body; p->nav_body_len = request.body_len; request.body = NULL; request.body_len = 0;
        }
        webpage_form_submission_free(&request);
    }
    return JS_UNDEFINED;
}
FN(d_focus) { Page* p = P(c); p->focus_req = 1; p->focus_id = arg_int(c, argv[0]); if (p->focus_id >= 0) p->active = p->focus_id; else p->active = -1; return JS_UNDEFINED; }
FN(d_active) { return JS_NewInt32(c, P(c)->active); }
FN(d_curScript) { return JS_NewInt32(c, P(c)->cur_script); }

/* The JS child cannot access the renderer framebuffer directly. It emits a small
   request in the existing DOM IPC reply; the UI process captures/composites there. */
FN(d_viewTransitionBegin) {
    Page* p = P(c);
    /* A new start supersedes older unserviced IPC requests. Preserve the skip
       bit so the renderer cancels any already-active snapshot, but let the new
       transition own capture/commit/discard state. */
    p->view_transition_pending_flags = (p->view_transition_pending_flags & 4u) | 1u;
    return JS_NewBool(c, 1);
}
FN(d_viewTransitionCommit) { P(c)->view_transition_pending_flags |= 2u; return JS_UNDEFINED; }
FN(d_viewTransitionSkip) { P(c)->view_transition_pending_flags |= 4u; return JS_UNDEFINED; }
FN(d_viewTransitionDiscard) { P(c)->view_transition_pending_flags |= 8u; return JS_UNDEFINED; }

/* ── таймеры ── */
FN(d_timerAdd) {
    Page* p = P(c); if (!JS_IsFunction(c, argv[0])) return JS_NewInt32(c, 0); double delay = 0; JS_ToFloat64(c, &delay, argv[1]); if (!(delay >= 0)) delay = 0;
    Timer* tm = calloc(1, sizeof *tm); tm->id = ++p->next_timer; tm->fn = JS_DupValue(c, argv[0]); tm->interval = delay < 1 ? 1 : delay; tm->due = now_ms() + delay; tm->repeat = argc > 2 && JS_ToBool(c, argv[2]) > 0;
    tm->next = p->timers; p->timers = tm; return JS_NewInt32(c, tm->id);
}
FN(d_timerClear) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (Timer** pp = &p->timers; *pp; pp = &(*pp)->next) if ((*pp)->id == id) { Timer* tm = *pp; *pp = tm->next; JS_FreeValue(c, tm->fn); free(tm); break; }
    return JS_UNDEFINED;
}

#define DEF(n) {#n, d_##n}
static const struct { const char* name; JSCFunction* fn; } DOMFN[] = {
    DEF(tag), DEF(type), DEF(parent), DEF(first), DEF(last), DEF(next), DEF(prev), DEF(children), DEF(create), DEF(createText), DEF(clone), DEF(insertBefore), DEF(remove),
    DEF(text), DEF(setText), DEF(innerHTML), DEF(outerHTML), DEF(setInnerHTML), DEF(insertHTML), DEF(attr), DEF(setAttr), DEF(removeAttr), DEF(attrNames), DEF(prop), DEF(setProp),
    DEF(query), DEF(matches), DEF(closest), DEF(byId), DEF(root), DEF(special), DEF(rect), DEF(hitTest), DEF(computed), DEF(mediaMatches), DEF(viewport), DEF(scrollTo), DEF(getScroll), DEF(setScroll), DEF(setScrollSmooth),
    DEF(title), DEF(setTitle), DEF(url), DEF(nav), DEF(setUrl), DEF(fetch), DEF(log), DEF(now), DEF(audioStart), DEF(audioStop), DEF(audioQueued), DEF(audioWrite), DEF(submit), DEF(focus), DEF(active), DEF(curScript), DEF(timerAdd), DEF(timerClear),
    DEF(fetchStart), DEF(fetchSync), DEF(fetchPoll), DEF(fetchCancel), DEF(fetchStreamStart), DEF(fetchStreamHeaders), DEF(fetchStreamRead), DEF(fetchStreamClose), DEF(readDroppedFile), DEF(readSelectedFile), DEF(utf8Decode), DEF(utf8Encode), DEF(lsList), DEF(lsSet), DEF(lsRemove), DEF(lsClear),
    DEF(idbMetaList), DEF(idbMetaGet), DEF(idbMetaSet), DEF(idbDeleteDatabase), DEF(idbList), DEF(idbGet), DEF(idbPut), DEF(idbRemove), DEF(idbClear),
    DEF(swList), DEF(swSet), DEF(swRemove),
    DEF(profileId), DEF(viewTransitionBegin), DEF(viewTransitionCommit), DEF(viewTransitionSkip), DEF(viewTransitionDiscard),
    DEF(wsConnect), DEF(wsPoll), DEF(wsSend), DEF(wsClose),
    DEF(webrtcAvailable), DEF(webrtcCreate), DEF(webrtcSetLocal), DEF(webrtcSetRemote), DEF(webrtcAddCandidate), DEF(webrtcCreateDataChannel), DEF(webrtcSend), DEF(webrtcChannelClose), DEF(webrtcBufferedAmount), DEF(webrtcChannelIsOpen), DEF(webrtcChannelStream), DEF(webrtcClose), DEF(webrtcPoll),
    DEF(structuredClone), DEF(workerStart), DEF(workerPoll), DEF(workerPost), DEF(workerTerminate), DEF(cryptoRandom), DEF(cryptoDigest),
    DEF(canvasMeasureText), DEF(canvasText), DEF(imageDecode), DEF(canvasCommit), DEF(canvasToDataURL), DEF(mediaInfo), DEF(mediaFastSeekTarget), DEF(mediaStart), DEF(mediaSetVolume), DEF(mediaPause), DEF(mediaResume), DEF(mediaStreamStart), DEF(mediaStreamAppend), DEF(mediaStreamEnd), DEF(mediaPoll), DEF(mediaStatus), DEF(mediaStop), DEF(webglCall), DEF(webglReadback)
};

/* ══════════════ модули ES ══════════════ */
static char* mod_normalize(JSContext* c, const char* base, const char* name, void* op) {
    (void)op; char out[1200]; webpage_resolve_url(base, name, out, sizeof out); return js_strdup(c, out[0] ? out : name);
}
static JSModuleDef* mod_load(JSContext* c, const char* name, void* op) {
    (void)op; char* src = webpage_fetch_text_kind(name, NAL_RESOURCE_SCRIPT);
    if (!src) { JS_ThrowReferenceError(c, "could not load module '%s'", name); return NULL; }
    JSValue v = JS_Eval(c, src, strlen(src), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY); free(src);
    if (JS_IsException(v)) return NULL;
    JSModuleDef* m = JS_VALUE_GET_PTR(v); JS_FreeValue(c, v); return m;
}

static void eval_script(Page* p, const char* code, const char* name, int module) {
    debug_reload_config(p);
    g_deadline = now_ms() + 5000;
    JSValue r = JS_Eval(p->ctx, code, strlen(code), name, module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) log_exception(p->ctx, name); JS_FreeValue(p->ctx, r);
    run_jobs(); g_deadline = 0;
}

int webpage_js_eval(void* page, const char* code, char* out, size_t cap) {
    Page* p = (Page*)page;
    if (out && cap) out[0] = 0;
    if (!p || !p->ctx || !code) { if (out && cap) snprintf(out, cap, "No JavaScript context for this page"); return 0; }
    debug_reload_config(p);
    g_deadline = now_ms() + 5000;
    JSValue r = JS_Eval(p->ctx, code, strlen(code), "<devtools-console>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(r);
    JSValue value = ok ? JS_DupValue(p->ctx, r) : JS_GetException(p->ctx);
    const char* str = JS_ToCString(p->ctx, value);
    if (out && cap) snprintf(out, cap, "%s", str ? str : (ok ? "undefined" : "JavaScript exception"));
    if (str) JS_FreeCString(p->ctx, str);
    JS_FreeValue(p->ctx, value); JS_FreeValue(p->ctx, r);
    run_jobs(); flush_style(p); g_deadline = 0;
    return ok;
}

void* js_local_attach(DOMNode* head, const char* url, PageBundle* bundle) {
    if (!head || !head->v2) { webpage_bundle_free(bundle); return NULL; }
    if (!g_rt) {
        g_rt = JS_NewRuntime(); if (!g_rt) { webpage_bundle_free(bundle); return NULL; }
        JS_SetMemoryLimit(g_rt, 512u * 1024 * 1024); JS_SetMaxStackSize(g_rt, 2u * 1024 * 1024);
        JS_SetInterruptHandler(g_rt, interrupt_cb, NULL); JS_SetHostPromiseRejectionTracker(g_rt, rejection_cb, NULL); JS_SetModuleLoaderFunc(g_rt, mod_normalize, mod_load, NULL);
    }
    Page* p = calloc(1, sizeof *p); p->head = head; p->bundle = bundle; snprintf(p->url, sizeof p->url, "%s", url ? url : ""); webpage_set_doc_url(p->url); p->focus_id = p->active = -1;
    p->ctx = JS_NewContext(g_rt); JS_SetContextOpaque(p->ctx, p);
    debug_reload_config(p);
    for (DOMNode* q = head; q; q = q->next) new_id(p, q);
    for (int i = 0; i < p->nn; i++) { DOMNode* q = p->nodes[i]; int par = q->parent; if (par >= 0 && par < p->nn) link_before(p, par, i, -1); }
    JSContext* c = p->ctx;
    webpage_wasm_init(c);
    JSValue g = JS_GetGlobalObject(c); JSValue dom = JS_NewObject(c);
    for (size_t i = 0; i < sizeof DOMFN / sizeof DOMFN[0]; i++) JS_SetPropertyStr(c, dom, DOMFN[i].name, JS_NewCFunction(c, DOMFN[i].fn, DOMFN[i].name, 0));
    JS_SetPropertyStr(c, g, "__dom", dom); JS_FreeValue(c, g);
    eval_script(p, webpage_js_prelude, "<prelude>", 0);
    if (bundle) for (int i = 0; i < bundle->nscripts; i++) {
        ScriptSrc* s = &bundle->scripts[i]; if (!s->text) continue;
        if ((!s->url || !s->url[0]) && !websec_allows_inline(NAL_RESOURCE_SCRIPT, s->nonce)) {
            gal_log("WARN", "CSP: blocked inline script in %s", url ? url : "document"); continue;
        }
        p->cur_script = s->node_idx; char nm[1200]; snprintf(nm, sizeof nm, "%s", s->url ? s->url : (url ? url : "inline"));
        eval_script(p, s->text, nm, s->module);
    }
    p->cur_script = -1; p->vw_seen = SCREEN_WIDTH; p->vh_seen = SCREEN_HEIGHT;
    JSValue fn = JS_GetPropertyStr(c, JS_GetGlobalObject(c), "__lifecycle");
    if (JS_IsFunction(c, fn)) { g_deadline = now_ms() + 3000; JSValue a = JS_NewString(c, "DOMContentLoaded"); JSValue r = JS_Call(c, fn, JS_UNDEFINED, 1, &a); if (JS_IsException(r)) log_exception(c, "DOMContentLoaded"); JS_FreeValue(c, r); JS_FreeValue(c, a); run_jobs();
        JSValue b = JS_NewString(c, "load"); r = JS_Call(c, fn, JS_UNDEFINED, 1, &b); if (JS_IsException(r)) log_exception(c, "load"); JS_FreeValue(c, r); JS_FreeValue(c, b); run_jobs(); g_deadline = 0; }
    JS_FreeValue(c, fn);
    if (bundle && bundle->nsheets) p->style_dirty = 1;      /* стили пересчитываются в родителе: подхватываем скачанные веб-шрифты и мутации скриптов */
    if (p->style_dirty || p->lin_dirty) flush_style(p);
    gal_log("INFO", "[JS] page scripts finished (%d scripts)", bundle ? bundle->nscripts : 0);
    return p;
}

void js_local_free(void* pp) {
    Page* p = pp; if (!p) return;
    for (Timer* t = p->timers; t;) { Timer* nx = t->next; JS_FreeValue(p->ctx, t->fn); free(t); t = nx; }
    for (AReq* a = p->areqs; a;) { AReq* nx = a->next; if (a->used) nal_async_cancel(&a->nr); free(a); a = nx; }     /* не оставляем недобитых curl при переходе на другую страницу */
    for (WConn* w = p->wconns; w;) { WConn* nx = w->next; if (w->used) ws_close(&w->ws, 1000); free(w); w = nx; }
    for (WkConn* w = p->wkconns; w;) { WkConn* nx = w->next; if (w->used) worker_terminate(&w->wk); free(w); w = nx; }
    for (WebRtcEntry* r = p->webrtc_peers; r;) { WebRtcEntry* nx = r->next; mlb_webrtc_peer_destroy(r->backend); free(r); r = nx; }
    for (MediaState* m=p->media; m;) { MediaState* nx=m->next; media_state_stop(m); free(m); m=nx; }
    for (DynamicImage* im=p->dynamic_images; im;) { DynamicImage* nx=im->next; free(im->pixels); free(im); im=nx; }
    /* Tear down EGL resources owned by this page process. */
    for (int i=0;i<p->nn;i++) webgl_context_destroy(i);
    webdoc_free(p->doc); webpage_wasm_free(p->ctx); JS_FreeContext(p->ctx); if (g_rt) JS_RunGC(g_rt);
    for (int i = 0; i < p->nn; i++) if (p->nodes[i]->idx < 0 && p->nodes[i] != p->head) { dom_free_pseudo(p->nodes[i]); free(p->nodes[i]); }       /* отсоединённые узлы */
    for (DOMNode* q = p->head; q; q = q->next) q->aux = NULL;
    free(p->nav_body); free(p->nodes); free(p->lk); webpage_bundle_free(p->bundle); free(p);
}

int js_local_tick(void* pp) {
    Page* p = pp; if (!p) return 0; debug_reload_config(p); int did = 0;
    for (int guard = 0; guard < 64; guard++) {
        double now = now_ms(); Timer* best = NULL;
        for (Timer* t = p->timers; t; t = t->next) if (t->due <= now && (!best || t->due < best->due)) best = t;
        if (!best) break;
        JSValue fn = JS_DupValue(p->ctx, best->fn); int id = best->id;
        if (best->repeat) best->due = now + best->interval; else { for (Timer** pp2 = &p->timers; *pp2; pp2 = &(*pp2)->next) if (*pp2 == best) { *pp2 = best->next; break; } JS_FreeValue(p->ctx, best->fn); free(best); }
        (void)id; g_deadline = now_ms() + 3000; JSValue r = JS_Call(p->ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) log_exception(p->ctx, "timer"); JS_FreeValue(p->ctx, r); JS_FreeValue(p->ctx, fn); run_jobs(); g_deadline = 0; did = 1;
    }
    if (JS_IsJobPending(g_rt)) { g_deadline = now_ms() + 3000; run_jobs(); g_deadline = 0; did = 1; }
    if (p->vw_seen != SCREEN_WIDTH || p->vh_seen != SCREEN_HEIGHT) {      /* размер окна изменился: @media пересчитываем заново */
        p->vw_seen = SCREEN_WIDTH; p->vh_seen = SCREEN_HEIGHT; if (p->bundle && p->bundle->nsheets) p->style_dirty = 1;
    }
    if (p->lin_dirty || p->style_dirty) { flush_style(p); did = 1; }
    return did;
}

static int js_local_dispatch_ex(void* pp, DOMNode* target, const char* type, const char* key, int mods,
                                int has_mouse, int x, int y, int button, int buttons) {
    Page* p = pp; if (!p || !target || target->aux == NULL) return 0; debug_reload_config(p); JSContext* c = p->ctx;
    if(type&&!strcmp(type,"__filedrop"))snprintf(p->drop_authorized_path,sizeof p->drop_authorized_path,"%s",key?key:"");
    JSValue g = JS_GetGlobalObject(c); JSValue fn = JS_GetPropertyStr(c, g, "__dispatch"); int prevented = 0;
    if (JS_IsFunction(c, fn)) {
        JSValue a[8] = { JS_NewInt32(c, id_of(target)), JS_NewString(c, type), JS_NewString(c, key ? key : ""), JS_NewInt32(c, mods),
                         JS_NewInt32(c, x), JS_NewInt32(c, y), JS_NewInt32(c, button), JS_NewInt32(c, buttons) };
        int argc = has_mouse ? 8 : 4;
        g_deadline = now_ms() + 3000; JSValue r = JS_Call(c, fn, JS_UNDEFINED, argc, a);
        if (JS_IsException(r)) log_exception(c, type); else prevented = JS_ToBool(c, r) > 0;
        JS_FreeValue(c, r); for (int i = 0; i < argc; i++) JS_FreeValue(c, a[i]); run_jobs(); g_deadline = 0;
    }
    if(type&&!strcmp(type,"__filedrop"))p->drop_authorized_path[0]=0;
    JS_FreeValue(c, fn); JS_FreeValue(c, g); return prevented;
}
int js_local_dispatch(void* pp, DOMNode* target, const char* type, const char* key, int mods) {
    return js_local_dispatch_ex(pp,target,type,key,mods,0,0,0,0,0);
}
int js_local_dispatch_mouse(void* pp, DOMNode* target, const char* type, int x, int y, int button, int buttons, int mods) {
    return js_local_dispatch_ex(pp,target,type,"",mods,1,x,y,button,buttons);
}


/* :hover/:active изменились — пересчитать стили; события transition/animation — в JS */
void js_local_restyle(void* pp) { Page* p = pp; if (!p) return; p->style_dirty = 1; flush_style(p); }
int js_local_anim_events(void* pp) {
    Page* p = pp; if (!p) return 0; DOMNode* n; int kind; char name[48]; float el; int guard = 0, dispatched = 0;
    while (guard++ < 32 && anim_pop_event(&n, &kind, name, &el)) {
        if (!n || n->aux == NULL || kind == AEV_TRANS_RUN) continue;
        const char* type = kind == AEV_TRANS_END ? "transitionend" : kind == AEV_ANIM_START ? "animationstart" : kind == AEV_ANIM_ITER ? "animationiteration" : "animationend";
        char key[96]; snprintf(key, sizeof key, "%s|%g", name, el); js_local_dispatch(p, n, type, key, 0); dispatched++;
    }
    return dispatched;
}

/* submit-событие формы перед отправкой; возвращает 1, если отправку можно продолжать */
int js_local_before_submit(void* pp, DOMNode* control) {
    Page* p = pp; if (!p || !control || !control->aux) return 1;
    int id = id_of(control);
    while (id >= 0 && id < p->nn && strcmp(p->nodes[id]->tag, "form") != 0) id = p->lk[id].parent;
    if (id < 0 || id >= p->nn) return 1;
    return !js_local_dispatch(p, p->nodes[id], "submit", "", 0);
}

const char* js_local_take_nav(void* pp) { Page* p = pp; if (!p || !p->has_nav) return NULL; p->has_nav = 0; return p->nav; }
static int js_local_take_nav_request(void* pp, char* method, size_t method_cap, char* headers, size_t headers_cap, unsigned char** body, size_t* body_len) {
    Page* p = pp; if (body) *body = NULL; if (body_len) *body_len = 0;
    if (!p || !p->nav_method[0]) return 0;
    if (method && method_cap) snprintf(method, method_cap, "%s", p->nav_method);
    if (headers && headers_cap) snprintf(headers, headers_cap, "%s", p->nav_headers);
    if (body) { *body = p->nav_body; p->nav_body = NULL; }
    else free(p->nav_body);
    if (body_len) *body_len = p->nav_body_len;
    p->nav_body_len = 0; p->nav_method[0] = 0; p->nav_headers[0] = 0;
    return 1;
}
int js_local_take_scroll(void* pp, int* x, int* y) { Page* p = pp; if (!p || !p->scroll_req) return 0; p->scroll_req = 0; *x = p->sx; *y = p->sy; return 1; }
int js_local_take_focus(void* pp, DOMNode** node) { Page* p = pp; if (!p || !p->focus_req) return 0; p->focus_req = 0; *node = p->focus_id >= 0 ? N(p, p->focus_id) : NULL; return 1; }
void js_local_set_focus(void* pp, DOMNode* node) { Page* p = pp; if (p) p->active = node ? id_of(node) : -1; }
const char* js_local_take_title(void* pp) { Page* p = pp; if (!p || !p->title_dirty) return NULL; p->title_dirty = 0; return p->title; }

/* Resolve the active ViewTransition.finished promise after the renderer's
   viewport cross-fade finishes (or immediately when no old-frame snapshot was
   available). This executes in the owning QuickJS runtime. */
static int js_local_view_transition_finished(void* pp) {
    Page* p = (Page*)pp;
    if (!p || !p->ctx) return 0;
    JSContext* c = p->ctx;
    JSValue global = JS_GetGlobalObject(c);
    JSValue fn = JS_GetPropertyStr(c, global, "__notifyViewTransitionFinished");
    int handled = 0;
    if (JS_IsFunction(c, fn)) {
        JSValue result = JS_Call(c, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(result)) log_exception(c, "ViewTransition.finished");
        else handled = JS_ToBool(c, result) > 0;
        JS_FreeValue(c, result);
        run_jobs();
        if (p->style_dirty || p->lin_dirty) flush_style(p);
    }
    JS_FreeValue(c, fn);
    JS_FreeValue(c, global);
    return handled;
}

/* ── Process-isolated page JS ──────────────────────────────────────────────
 * Each document gets its own QuickJS process. The UI process owns its DOM nodes;
 * mutations are returned as pointer-free node snapshots over a pipe. This keeps
 * QuickJS heaps/timers/worker connections out of the UI process without passing
 * raw pointers across the process boundary.
 */
#ifndef _WIN32
enum { JSC_TICK=1, JSC_DISPATCH, JSC_SUBMIT, JSC_TAKE_NAV, JSC_TAKE_NAV_REQUEST, JSC_TAKE_SCROLL,
       JSC_TAKE_FOCUS, JSC_SET_FOCUS, JSC_TAKE_TITLE, JSC_RESTYLE,
       JSC_ANIM_EVENTS, JSC_VIEW_TRANSITION_FINISHED, JSC_FREE, JSC_DEBUG_CONTROL };
enum { JSIPC_DEBUG_NONE=0, JSIPC_DEBUG_PAUSED=1 };
enum { JS_DEBUG_CONTINUE=0, JS_DEBUG_STEP_INTO=1, JS_DEBUG_STEP_OVER=2, JS_DEBUG_STEP_OUT=3 };
#define JSIPC_MAGIC 0x4a535031u /* JSP1 */
typedef struct {
    uint32_t magic;
    int32_t op, id, alt_id, mods, flags;
    int32_t checked, selected, disabled, readonly, file_selected;
    int32_t mouse_x, mouse_y, mouse_button, mouse_buttons;
    char type[32];
    char key[2048];
    char value[2048];
} JSIPCCommand;
typedef struct {
    uint32_t magic;
    int32_t status, result, x, y, focus_id, dyn_pending;
    uint32_t view_transition_flags; /* capture, commit, skip-old, discard-pending */
    uint32_t node_count;
    uint32_t scroll_count;
    uint32_t image_count;
    uint32_t payload_len;
    uint32_t snapshot_generation, snapshot_chunks;
    char method[8];
    char headers[256];
    char text[1200];
    /* Coalesced state from asynchronous JSC_TICK replies. */
    int32_t nav_pending, nav_request_pending;
    int32_t scroll_pending, scroll_x, scroll_y;
    int32_t focus_pending, async_focus_id, title_pending;
    char nav_url[1200];
    char title_value[1200];
    int32_t debug_state, debug_line, debug_column, debug_depth, debug_is_breakpoint;
    char debug_file[512];
    char debug_function[256];
    char debug_stack[2048];
    char debug_locals[2400];
} JSIPCReply;
typedef struct { int32_t node_idx; uint32_t kind, width, height; } JSIPCImageHead;
typedef struct { uint32_t generation, start, count, total; } JSIPCSnapshotChunk;
typedef struct { JSIPCImageHead head; uint32_t* pixels; size_t bytes; } JSIPCReceivedImage;
enum {
    JSRX_HEADER=0, JSRX_SNAPSHOT_CHUNK, JSRX_NODE_HEAD,
    JSRX_NODE_PB0, JSRX_NODE_PB1, JSRX_NODE_PB2,
    JSRX_SCROLL, JSRX_IMAGE_HEAD, JSRX_IMAGE_PIXELS, JSRX_PAYLOAD,
    JSRX_PUBLISH_IMAGES, JSRX_FINISH
};
#define JSIPC_RX_BUDGET (64u << 10)
#define JSIPC_RESPONSE_MAX_BYTES (128u << 20)
#define JSIPC_SNAPSHOT_CHUNK_NODES 16u
#define JSIPC_SNAPSHOT_PREP_NODES 128u
#define JSIPC_SNAPSHOT_COMMIT_NODES 128u
#define JSIPC_SNAPSHOT_RETIRE_PER_POLL 512u
#define JSIPC_SNAPSHOT_MAX_BYTES (64u << 20)
#define JSIPC_IMAGE_REPLY_MAX_BYTES (64u << 20)
static uint32_t g_jsipc_snapshot_generation;
typedef struct {
    DOMNode node;                 /* pointers next/aux/pb[] are zeroed before send */
    uint8_t has_pb0, has_pb1, has_pb2;
} JSNodeWireHead;
/* Additional per-node transaction metadata used while preparing an async commit. */
#define JSIPC_SNAPSHOT_PREP_OVERHEAD (2u * sizeof(uint8_t) + 6u * sizeof(void*))
#define JSIPC_SNAPSHOT_STAGE_NODE_BYTES (sizeof(JSNodeWireHead) + 3u * sizeof(PseudoBox) + JSIPC_SNAPSHOT_PREP_OVERHEAD)
#define JSIPC_DEFERRED_CAP 32u
/* While a trace hook owns the child interpreter, later queued commands must
   not be mistaken for debugger-control packets. They are parked here and
   consumed by the normal command loop after the paused operation completes. */
static JSIPCCommand g_jsipc_deferred[JSIPC_DEFERRED_CAP];
static unsigned g_jsipc_deferred_head, g_jsipc_deferred_count;
typedef struct {
    uint32_t magic;
    pid_t pid;
    int cmd_fd, reply_fd, dead;
    DOMNode* head;
    DOMNode** nodes_by_id;
    int node_cap;
    unsigned char* payload;
    size_t payload_len;
    char text[1200];
    /* Incremental async snapshot receive state. One bounded node packet is
       consumed per UI poll; the staged tree is published only after all packets
       validate. Synchronous calls may drain this state to preserve wire order. */
    int rx_snapshot_active;
    JSIPCReply rx_snapshot_reply;
    int rx_snapshot_op;
    JSNodeWireHead* rx_snapshot_nodes;
    PseudoBox* rx_snapshot_boxes;
    uint32_t rx_snapshot_chunk_no, rx_snapshot_next_node;
    /* Snapshot commit preparation is sliced across polls. The live DOM stays
       untouched until all indexes and replacement allocations validate. */
    int rx_snapshot_prep_phase, rx_snapshot_max_idx;
    uint32_t rx_snapshot_prep_cursor, rx_snapshot_order_cursor;
    unsigned char* rx_snapshot_seen;
    DOMNode** rx_snapshot_prepared;
    DOMNode** rx_snapshot_order;
    unsigned char* rx_snapshot_is_new;
    PseudoBox** rx_snapshot_pb0;
    PseudoBox** rx_snapshot_pb1;
    PseudoBox** rx_snapshot_pb2;
    PseudoBox** rx_snapshot_old_boxes;
    PseudoBox** rx_snapshot_retired_boxes;
    size_t rx_snapshot_retired_count, rx_snapshot_retired_cursor;
    /* Nonblocking wire parser. Every field retains its partial byte count
       across UI iterations; a single iteration consumes at most RX_BUDGET. */
    int rx_wire_active, rx_wire_phase;
    unsigned char rx_wire_header[sizeof(JSIPCReply)];
    size_t rx_wire_header_have, rx_wire_field_have;
    JSIPCReply rx_wire_reply;
    JSIPCSnapshotChunk rx_wire_chunk;
    JSNodeWireHead rx_wire_node;
    uint32_t rx_wire_chunk_remaining;
    L2ScrollState* rx_wire_scroll;
    size_t rx_wire_scroll_bytes, rx_wire_scroll_have;
    JSIPCReceivedImage* rx_wire_images;
    uint32_t rx_wire_images_done, rx_wire_images_published;
    int rx_wire_dom_committed;
    JSIPCImageHead rx_wire_image_head;
    uint32_t* rx_wire_image_pixels;
    size_t rx_wire_image_pixels_bytes, rx_wire_image_pixels_have;
    uint64_t rx_wire_image_bytes, rx_wire_staged_bytes;
    unsigned char* rx_wire_payload;
    size_t rx_wire_payload_have;
    /* Bounded async command queue plus a FIFO of replies expected from the
       child. A partially written command stays at the head with an offset;
       async enqueue itself never waits for the child to finish JS. */
    JSIPCCommand async_cmds[32];
    unsigned async_cmd_head, async_cmd_count;
    size_t async_cmd_offset;
    int async_ops[32];
    unsigned async_head, async_count;
    unsigned async_dropped;
    int async_overflow_warned;
    int async_vt_finish_pending;
    int async_nav_pending;
    char async_nav_url[1200];
    int async_nav_request_pending;
    char async_nav_method[8], async_nav_headers[256];
    unsigned char* async_nav_body;
    size_t async_nav_body_len;
    int async_scroll_pending, async_scroll_x, async_scroll_y;
    int async_focus_pending, async_focus_id;
    int async_title_pending;
    char async_title[1200];
} JSProcess;

static int js_ipc_write_all(int fd, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    while (len) { ssize_t n = write(fd, p, len); if (n < 0) { if (errno == EINTR) continue; return 0; } if (n == 0) return 0; p += n; len -= (size_t)n; }
    return 1;
}
static int js_ipc_read_all(int fd, void* data, size_t len) {
    unsigned char* p = (unsigned char*)data;
    while (len) { ssize_t n = read(fd, p, len); if (n < 0) { if (errno == EINTR) continue; return 0; } if (n == 0) return 0; p += n; len -= (size_t)n; }
    return 1;
}
static int g_debug_cmd_fd = -1;
static int g_debug_reply_fd = -1;
static int js_ipc_defer_command(const JSIPCCommand* c) {
    if (!c || g_jsipc_deferred_count >= JSIPC_DEFERRED_CAP) return 0;
    unsigned tail = (g_jsipc_deferred_head + g_jsipc_deferred_count) % JSIPC_DEFERRED_CAP;
    g_jsipc_deferred[tail] = *c;
    g_jsipc_deferred_count++;
    return 1;
}
static int js_ipc_child_read_command(int fd, JSIPCCommand* c) {
    if (!c) return 0;
    if (g_jsipc_deferred_count) {
        *c = g_jsipc_deferred[g_jsipc_deferred_head];
        g_jsipc_deferred_head = (g_jsipc_deferred_head + 1u) % JSIPC_DEFERRED_CAP;
        g_jsipc_deferred_count--;
        return 1;
    }
    return js_ipc_read_all(fd, c, sizeof(*c)) && c->magic == JSIPC_MAGIC;
}

/* Called from QuickJS' source-line hook in the child process. It sends one
   debugger notification, then consumes only a debugger-control command while
   the parent keeps pumping native UI events. The document command remains in
   flight and is replied to normally after execution resumes. */
static void debug_pause_at_trace(JSContext* ctx, const char* filename,
                                 const char* function_name, int line, int column,
                                 int depth, int is_breakpoint) {
    Page* page = (Page*)JS_GetContextOpaque(ctx);
    if (!page || g_debug_cmd_fd < 0 || g_debug_reply_fd < 0) return;
    JSIPCReply event; memset(&event, 0, sizeof(event));
    event.magic = JSIPC_MAGIC; event.status = 1;
    event.debug_state = JSIPC_DEBUG_PAUSED;
    event.debug_line = line; event.debug_column = column;
    event.debug_depth = depth; event.debug_is_breakpoint = !!is_breakpoint;
    snprintf(event.debug_file, sizeof(event.debug_file), "%s", filename ? filename : "<anonymous>");
    snprintf(event.debug_function, sizeof(event.debug_function), "%s", function_name ? function_name : "<script>");
    (void)JS_GetCurrentStackTrace(ctx, event.debug_stack, sizeof(event.debug_stack), 12);
    (void)JS_GetCurrentLocals(ctx, event.debug_locals, sizeof(event.debug_locals), 24);
    page->debug_paused = 1;
    if (!js_ipc_write_all(g_debug_reply_fd, &event, sizeof(event))) { page->debug_paused = 0; return; }
    for (;;) {
        JSIPCCommand control; memset(&control, 0, sizeof(control));
        if (!js_ipc_read_all(g_debug_cmd_fd, &control, sizeof(control)) || control.magic != JSIPC_MAGIC)
            break;
        if (control.op != JSC_DEBUG_CONTROL) {
            /* Normal commands may already be queued behind the request whose
               JS execution hit this breakpoint. Preserve FIFO ordering and
               let the command loop run them only after resume. */
            if (!js_ipc_defer_command(&control)) {
                fprintf(stderr, "[JS IPC] deferred debugger command queue overflow; continuing safely\n");
                continue;
            }
            continue;
        }
        int action = control.flags;
        if (action < JS_DEBUG_CONTINUE || action > JS_DEBUG_STEP_OUT) action = JS_DEBUG_CONTINUE;
        page->debug_run_mode = action;
        page->debug_step_depth = depth;
        break;
    }
    page->debug_paused = 0;
}

/* Forward declaration: the debugger pause handler must finish any partially
   written async command before appending a control packet to the same byte
   stream, otherwise fixed-size IPC framing can be corrupted. */
static int js_proxy_flush_async(JSProcess* p, int wait_for_writable);

/* Handle an in-band debugger pause notification independently from reading the
   reply header. The async byte parser uses this when a header arrives in pieces. */
static int js_proxy_handle_debug_pause(JSProcess* p, const JSIPCReply* r, int allow_debugger_ui) {
    if (!r || r->debug_state != JSIPC_DEBUG_PAUSED) return 1;
    int action = JS_DEBUG_CONTINUE;
#ifndef _WIN32
    if (allow_debugger_ui && browser_debugger_pause_loop)
        action = browser_debugger_pause_loop(p, r->debug_file, r->debug_function,
                    r->debug_line, r->debug_column, r->debug_stack,
                    r->debug_locals, r->debug_is_breakpoint);
#endif
    /* Fixed-size command framing: finish any queued command before appending
       DEBUG_CONTROL, otherwise a short write could splice adjacent records. */
    if (p && p->async_cmd_count && !js_proxy_flush_async(p, 1)) return 0;
    JSIPCCommand control; memset(&control, 0, sizeof(control));
    control.magic = JSIPC_MAGIC; control.op = JSC_DEBUG_CONTROL; control.flags = action;
    return p && js_ipc_write_all(p->cmd_fd, &control, sizeof(control));
}
/* Blocking reader retained for initial attach and explicitly synchronous APIs. */
static int js_proxy_read_reply(JSProcess* p, JSIPCReply* r, int allow_debugger_ui) {
    for (;;) {
        if (!js_ipc_read_all(p->reply_fd, r, sizeof(*r)) || r->magic != JSIPC_MAGIC) {
            if (browser_debugger_pause_session_done) browser_debugger_pause_session_done(p);
            return 0;
        }
        if (r->debug_state != JSIPC_DEBUG_PAUSED) {
            if (browser_debugger_pause_session_done) browser_debugger_pause_session_done(p);
            return 1;
        }
        if (!js_proxy_handle_debug_pause(p, r, allow_debugger_ui)) return 0;
    }
}
/* Bound the aggregate staged node/pseudo-box footprint before emitting a reply. */
#define JSIPC_SNAPSHOT_MAX_NODES 100000u
#define JSIPC_SNAPSHOT_MAX_SCROLL 100000u
static int js_ipc_snapshot_within_limits(const Page* p) {
    if (!p || p->nn < 0 || (uint32_t)p->nn > JSIPC_SNAPSHOT_MAX_NODES) return 0;
    if ((uint64_t)p->nn * (sizeof(JSNodeWireHead) + 3u * sizeof(PseudoBox)) > JSIPC_SNAPSHOT_MAX_BYTES) return 0;
    int sc = l2_scroll_state_export(p->head, NULL, 0);
    if (sc < 0 || (uint32_t)sc > JSIPC_SNAPSHOT_MAX_SCROLL) return 0;
    for (const DynamicImage* im = p->dynamic_images; im; im = im->next) {
        if (!im->dirty || !im->pixels) continue;
        if (im->width < 1 || im->height < 1 || im->width > 4096 || im->height > 4096 ||
            (uint64_t)im->width * (uint64_t)im->height > 4000000ull) return 0;
    }
    return 1;
}
static int js_ipc_write_snapshot(int fd, Page* p, JSIPCReply* r) {
    if (!p) return 1;
    /* Each bounded chunk is independently framed. The receiver validates the
       generation and exact contiguous range before committing the staged DOM. */
    for (uint32_t start = 0; start < r->node_count; ) {
        uint32_t count = r->node_count - start;
        if (count > JSIPC_SNAPSHOT_CHUNK_NODES) count = JSIPC_SNAPSHOT_CHUNK_NODES;
        JSIPCSnapshotChunk ch = { r->snapshot_generation, start, count, r->node_count };
        if (!js_ipc_write_all(fd, &ch, sizeof ch)) return 0;
        for (uint32_t i = start; i < start + count; i++) {
            DOMNode* n = p->nodes[i]; JSNodeWireHead h; memset(&h, 0, sizeof h);
            memcpy(&h.node, n, sizeof(DOMNode));
            h.node.next = NULL; h.node.aux = NULL; h.node.pb[0] = h.node.pb[1] = h.node.pb[2] = NULL;
            h.has_pb0 = n->pb[0] != NULL; h.has_pb1 = n->pb[1] != NULL; h.has_pb2 = n->pb[2] != NULL;
            if (!js_ipc_write_all(fd, &h, sizeof h)) return 0;
            if (h.has_pb0 && !js_ipc_write_all(fd, n->pb[0], sizeof(PseudoBox))) return 0;
            if (h.has_pb1 && !js_ipc_write_all(fd, n->pb[1], sizeof(PseudoBox))) return 0;
            if (h.has_pb2 && !js_ipc_write_all(fd, n->pb[2], sizeof(PseudoBox))) return 0;
        }
        start += count;
    }
    return 1;
}
static int js_ipc_send_reply(int fd, JSIPCReply* r, Page* p, int snapshot) {
    r->magic = JSIPC_MAGIC;
    r->view_transition_flags = p ? p->view_transition_pending_flags : 0;
    if (p) p->view_transition_pending_flags = 0;
    r->node_count = r->scroll_count = r->image_count = 0;
    r->snapshot_generation = r->snapshot_chunks = 0;
    if (snapshot && p) {
        /* Validate before writing the header: once a reply starts, the pipe must
           contain exactly the payload described by that header. */
        if (!js_ipc_snapshot_within_limits(p)) {
            r->status = 0;
            r->result = 0;
            snapshot = 0;
        } else {
            flush_style(p);
            r->node_count = (uint32_t)p->nn;
            r->snapshot_generation = ++g_jsipc_snapshot_generation;
            if (!r->snapshot_generation) r->snapshot_generation = ++g_jsipc_snapshot_generation;
            r->snapshot_chunks = (r->node_count + JSIPC_SNAPSHOT_CHUNK_NODES - 1u) / JSIPC_SNAPSHOT_CHUNK_NODES;
            int sc = l2_scroll_state_export(p->head, NULL, 0);
            if (sc > 0) r->scroll_count = (uint32_t)sc;
        }
    }
    uint64_t image_reply_bytes = 0;
    if (p) for (DynamicImage* im=p->dynamic_images; im; im=im->next) {
        DOMNode* n=N(p,im->node_id);
        if (!im->dirty || !im->pixels || !n || n->idx < 0 || im->width < 1 || im->height < 1 ||
            im->width > 4096 || im->height > 4096 || (uint64_t)im->width * im->height > 4000000ull || im->kind > 1) continue;
        uint64_t bytes = (uint64_t)im->width * im->height * sizeof(uint32_t);
        if (bytes > JSIPC_IMAGE_REPLY_MAX_BYTES - image_reply_bytes) continue;
        image_reply_bytes += bytes; r->image_count++;
    }
    if (!js_ipc_write_all(fd, r, sizeof *r)) return 0;
    if (snapshot && p) {
        if (!js_ipc_write_snapshot(fd, p, r)) return 0;
        if (r->scroll_count) {
            size_t bytes = (size_t)r->scroll_count * sizeof(L2ScrollState);
            L2ScrollState* state = malloc(bytes);
            if (!state) return 0;
            int n = l2_scroll_state_export(p->head, state, (int)r->scroll_count);
            int ok = n == (int)r->scroll_count && js_ipc_write_all(fd, state, bytes);
            free(state);
            if (!ok) return 0;
        }
    }
    if (p && r->image_count) {
        uint64_t sent_bytes = 0; uint32_t sent_images = 0;
        for (DynamicImage* im=p->dynamic_images; im && sent_images<r->image_count; im=im->next) {
            DOMNode* n=N(p,im->node_id);
            if (!im->dirty || !im->pixels || !n || n->idx<0 || im->width<1 || im->height<1 ||
                im->width>4096 || im->height>4096 || (uint64_t)im->width*im->height>4000000ull || im->kind>1) continue;
            uint64_t bytes64=(uint64_t)im->width*(uint64_t)im->height*sizeof(uint32_t);
            if (bytes64 > JSIPC_IMAGE_REPLY_MAX_BYTES - sent_bytes) continue;
            JSIPCImageHead ih; ih.node_idx=n->idx; ih.kind=(uint32_t)im->kind; ih.width=(uint32_t)im->width; ih.height=(uint32_t)im->height;
            size_t bytes=(size_t)bytes64;
            if (!js_ipc_write_all(fd,&ih,sizeof ih) || !js_ipc_write_all(fd,im->pixels,bytes)) return 0;
            im->dirty=0; sent_bytes += bytes64; sent_images++;
        }
        int remaining_dirty = 0;
        for (DynamicImage* im=p->dynamic_images; im; im=im->next) if (im->dirty && im->pixels) { remaining_dirty = 1; break; }
        p->dynamic_images_dirty = remaining_dirty;
    }
    if (r->payload_len) {
        if (!p || !p->nav_body || p->nav_body_len != r->payload_len || !js_ipc_write_all(fd, p->nav_body, r->payload_len)) return 0;
        free(p->nav_body); p->nav_body = NULL; p->nav_body_len = 0; p->nav_method[0] = 0; p->nav_headers[0] = 0;
    } else if (p && r->nav_request_pending) {
        free(p->nav_body); p->nav_body = NULL; p->nav_body_len = 0; p->nav_method[0] = 0; p->nav_headers[0] = 0;
    } else if (p && r->result && r->method[0] && !strcmp(r->method, "POST")) {
        free(p->nav_body); p->nav_body = NULL; p->nav_body_len = 0; p->nav_method[0] = 0; p->nav_headers[0] = 0;
    }
    return 1;
}
static int js_dispatch_event_is_async_safe(const char* type);

static void js_ipc_child_loop(int cmd_fd, int reply_fd, Page* page) {
    for (;;) {
        JSIPCCommand c; memset(&c, 0, sizeof c);
        if (!js_ipc_child_read_command(cmd_fd, &c)) break;
        JSIPCReply r; memset(&r, 0, sizeof r); r.status = page != NULL; r.focus_id = -1;
        if (!page) { js_ipc_send_reply(reply_fd, &r, NULL, 0); continue; }
        DOMNode* target = (c.id >= 0 && c.id < page->nn) ? page->nodes[c.id] : NULL;
        int snapshot = 0;
        switch (c.op) {
            case JSC_TICK: {
                r.result = js_local_tick(page);
                /* CSS transitions/keyframes must advance beside the animation records,
                   which live in this JS process, not in the isolated painter. */
                (void)anim_step(page->head);
                int anim_running = anim_active();
                int anim_events = js_local_anim_events(page);
                if (anim_events || (anim_running && webpage_anim_tick())) r.result = 1;

                /* Bundle state into the tick reply so the UI doesn't issue separate
                   blocking TAKE_NAV/SCROLL/FOCUS/TITLE calls after every frame. */
                const char* nav = js_local_take_nav(page);
                if (nav) { r.nav_pending = 1; snprintf(r.nav_url, sizeof r.nav_url, "%s", nav); }
                const char* title = js_local_take_title(page);
                if (title) { r.title_pending = 1; snprintf(r.title_value, sizeof r.title_value, "%s", title); }
                r.scroll_pending = js_local_take_scroll(page, &r.scroll_x, &r.scroll_y);
                DOMNode* focus_node = NULL;
                r.focus_pending = js_local_take_focus(page, &focus_node);
                r.async_focus_id = focus_node ? id_of(focus_node) : -1;
                if (page->nav_method[0]) {
                    r.nav_request_pending = 1;
                    snprintf(r.method, sizeof r.method, "%s", page->nav_method);
                    snprintf(r.headers, sizeof r.headers, "%s", page->nav_headers);
                    if (page->nav_body_len > (64u << 20)) { r.status = 0; break; }
                    r.payload_len = (uint32_t)page->nav_body_len;
                }
                snapshot = r.result != 0;
                break;
            }
            case JSC_DISPATCH:
                if (target && (c.flags & 1)) {
                    snprintf(target->value, sizeof target->value, "%s", c.value);
                    target->checked = (uint8_t)!!c.checked; target->selected = (uint8_t)!!c.selected;
                    target->disabled = (uint8_t)!!c.disabled; target->readonly = (uint8_t)!!c.readonly; target->file_selected = (uint8_t)!!c.file_selected;
                }
                if(!strcmp(c.type,"__filedrop")) r.result=js_local_dispatch_ex(page,target,c.type,c.key,c.mods,1,c.mouse_x,c.mouse_y,c.mouse_button,c.mouse_buttons);
                else r.result = (c.flags & 2) ? js_local_dispatch_mouse(page,target,c.type,c.mouse_x,c.mouse_y,c.mouse_button,c.mouse_buttons,c.mods)
                                         : js_local_dispatch(page, target, c.type, c.key, c.mods);
                /* Non-cancelable async events share their mutations with the
                   next coalesced tick. Sending the whole DOM tree for every
                   input/change/focus event magnifies large-document IPC cost. */
                snapshot = !js_dispatch_event_is_async_safe(c.type);
                break;
            case JSC_SUBMIT:
                if (target && (c.flags & 1)) {
                    snprintf(target->value, sizeof target->value, "%s", c.value);
                    target->checked = (uint8_t)!!c.checked; target->selected = (uint8_t)!!c.selected;
                    target->disabled = (uint8_t)!!c.disabled; target->readonly = (uint8_t)!!c.readonly; target->file_selected = (uint8_t)!!c.file_selected;
                }
                r.result = js_local_before_submit(page, target); snapshot = 1;
                break;
            case JSC_TAKE_NAV: {
                const char* v = js_local_take_nav(page); if (v) snprintf(r.text, sizeof r.text, "%s", v); r.result = v != NULL; break;
            }
            case JSC_TAKE_NAV_REQUEST:
                if (page->nav_method[0]) {
                    snprintf(r.method, sizeof r.method, "%s", page->nav_method);
                    snprintf(r.headers, sizeof r.headers, "%s", page->nav_headers);
                    if (page->nav_body_len > (64u << 20)) { r.status = 0; break; }
                    r.payload_len = (uint32_t)page->nav_body_len; r.result = 1;
                }
                break;
            case JSC_TAKE_SCROLL:
                r.result = js_local_take_scroll(page, &r.x, &r.y); break;
            case JSC_TAKE_FOCUS: {
                DOMNode* n = NULL; r.result = js_local_take_focus(page, &n);
                r.focus_id = n ? id_of(n) : -1; break;
            }
            case JSC_SET_FOCUS: js_local_set_focus(page, target); break;
            case JSC_TAKE_TITLE: {
                const char* v = js_local_take_title(page); if (v) snprintf(r.text, sizeof r.text, "%s", v); r.result = v != NULL; break;
            }
            case JSC_RESTYLE:
                webpage_set_hover(c.id >= 0 && c.id < page->nn ? page->nodes[c.id] : NULL);
                webpage_set_active(c.alt_id >= 0 && c.alt_id < page->nn ? page->nodes[c.alt_id] : NULL);
                js_local_restyle(page); webpage_dyn_clear_pending(); snapshot = 1; break;
            case JSC_ANIM_EVENTS: js_local_anim_events(page); snapshot = 1; break;
            case JSC_VIEW_TRANSITION_FINISHED:
                /* Keep the notification result separate from r.result, which
                   may be overwritten by dynamic-image bookkeeping below. */
                r.focus_id = js_local_view_transition_finished(page);
                snapshot = r.focus_id != 0;
                break;
            case JSC_FREE:
                js_local_free(page); page = NULL; r.status = 1;
                js_ipc_send_reply(reply_fd, &r, NULL, 0);
                close(cmd_fd); close(reply_fd); _exit(0);
            default: r.status = 0; break;
        }
        r.dyn_pending = webpage_dyn_pending();
        if (r.dyn_pending) webpage_dyn_clear_pending();
        if (page && page->dynamic_images_dirty) { r.result=1; snapshot=1; }
        if (!js_ipc_send_reply(reply_fd, &r, page, snapshot)) break;
    }
    if (page) js_local_free(page);
    close(cmd_fd); close(reply_fd); _exit(0);
}
static int js_proxy_grow_nodes(JSProcess* p, uint32_t count) {
    if (count <= (uint32_t)p->node_cap) return 1;
    int cap = p->node_cap ? p->node_cap : 64;
    while ((uint32_t)cap < count) { if (cap > 1<<26) return 0; cap *= 2; }
    DOMNode** v = realloc(p->nodes_by_id, (size_t)cap * sizeof(*v)); if (!v) return 0;
    for (int i = p->node_cap; i < cap; i++) v[i] = NULL;
    p->nodes_by_id = v; p->node_cap = cap; return 1;
}
static void js_proxy_snapshot_stage_clear(JSProcess* p) {
    if (!p) return;
    uint32_t n = p->rx_snapshot_reply.node_count;
    if (p->rx_snapshot_is_new && p->rx_snapshot_prepared) {
        for (uint32_t i = 0; i < n; i++)
            if (p->rx_snapshot_is_new[i] && p->rx_snapshot_prepared[i])
                free(p->rx_snapshot_prepared[i]);
    }
    if (p->rx_snapshot_pb0) for (uint32_t i = 0; i < n; i++) free(p->rx_snapshot_pb0[i]);
    if (p->rx_snapshot_pb1) for (uint32_t i = 0; i < n; i++) free(p->rx_snapshot_pb1[i]);
    if (p->rx_snapshot_pb2) for (uint32_t i = 0; i < n; i++) free(p->rx_snapshot_pb2[i]);
    free(p->rx_snapshot_nodes); p->rx_snapshot_nodes = NULL;
    free(p->rx_snapshot_boxes); p->rx_snapshot_boxes = NULL;
    free(p->rx_snapshot_seen); p->rx_snapshot_seen = NULL;
    free(p->rx_snapshot_prepared); p->rx_snapshot_prepared = NULL;
    free(p->rx_snapshot_order); p->rx_snapshot_order = NULL;
    free(p->rx_snapshot_is_new); p->rx_snapshot_is_new = NULL;
    free(p->rx_snapshot_pb0); p->rx_snapshot_pb0 = NULL;
    free(p->rx_snapshot_pb1); p->rx_snapshot_pb1 = NULL;
    free(p->rx_snapshot_pb2); p->rx_snapshot_pb2 = NULL;
    free(p->rx_snapshot_old_boxes); p->rx_snapshot_old_boxes = NULL;
    p->rx_snapshot_active = 0;
    p->rx_snapshot_chunk_no = p->rx_snapshot_next_node = 0;
    p->rx_snapshot_prep_phase = 0;
    p->rx_snapshot_prep_cursor = p->rx_snapshot_order_cursor = 0;
    p->rx_snapshot_max_idx = -1;
    p->rx_snapshot_op = 0;
    memset(&p->rx_snapshot_reply, 0, sizeof p->rx_snapshot_reply);
}
static int js_proxy_snapshot_stage_begin(JSProcess* p, const JSIPCReply* r, int op) {
    if (!p || !r || p->rx_snapshot_active || !r->node_count ||
        r->node_count > JSIPC_SNAPSHOT_MAX_NODES ||
        r->scroll_count > JSIPC_SNAPSHOT_MAX_SCROLL || !r->snapshot_generation ||
        r->snapshot_chunks != (r->node_count + JSIPC_SNAPSHOT_CHUNK_NODES - 1u) / JSIPC_SNAPSHOT_CHUNK_NODES ||
        (uint64_t)r->node_count * JSIPC_SNAPSHOT_STAGE_NODE_BYTES > JSIPC_SNAPSHOT_MAX_BYTES ||
        !js_proxy_grow_nodes(p, r->node_count)) return 0;
    p->rx_snapshot_reply = *r;
    p->rx_snapshot_nodes = calloc(r->node_count, sizeof(*p->rx_snapshot_nodes));
    p->rx_snapshot_boxes = calloc((size_t)r->node_count * 3u, sizeof(*p->rx_snapshot_boxes));
    p->rx_snapshot_seen = calloc(r->node_count, 1);
    p->rx_snapshot_prepared = calloc(r->node_count, sizeof(*p->rx_snapshot_prepared));
    p->rx_snapshot_order = calloc(r->node_count, sizeof(*p->rx_snapshot_order));
    p->rx_snapshot_is_new = calloc(r->node_count, 1);
    p->rx_snapshot_pb0 = calloc(r->node_count, sizeof(*p->rx_snapshot_pb0));
    p->rx_snapshot_pb1 = calloc(r->node_count, sizeof(*p->rx_snapshot_pb1));
    p->rx_snapshot_pb2 = calloc(r->node_count, sizeof(*p->rx_snapshot_pb2));
    p->rx_snapshot_old_boxes = calloc((size_t)r->node_count * 3u, sizeof(*p->rx_snapshot_old_boxes));
    if (!p->rx_snapshot_nodes || !p->rx_snapshot_boxes || !p->rx_snapshot_seen ||
        !p->rx_snapshot_prepared || !p->rx_snapshot_order || !p->rx_snapshot_is_new ||
        !p->rx_snapshot_pb0 || !p->rx_snapshot_pb1 || !p->rx_snapshot_pb2 ||
        !p->rx_snapshot_old_boxes) {
        js_proxy_snapshot_stage_clear(p); return 0;
    }
    p->rx_snapshot_op = op;
    p->rx_snapshot_chunk_no = p->rx_snapshot_next_node = 0;
    p->rx_snapshot_prep_phase = 1; /* validate indexes */
    p->rx_snapshot_prep_cursor = p->rx_snapshot_order_cursor = 0;
    p->rx_snapshot_max_idx = -1;
    p->rx_snapshot_active = 1;
    return 1;
}
static int js_proxy_snapshot_retire_step(JSProcess* p);
/* Validate, allocate, and apply a snapshot in bounded batches. Existing
   DOMNode addresses are stable API handles, so the in-place copy/relink phase
   cannot be copy-on-write. The UI loop checks dom_commit_pending and holds the
   last painted frame while phase 5/6 mutate the shared nodes. Return 0 to yield,
   1 when the complete tree has been published, and -1 on pre-commit error. */
static int js_proxy_snapshot_prepare_step(JSProcess* p) {
    if (!p || !p->rx_snapshot_active || !p->rx_snapshot_nodes) return -1;
    const uint32_t n = p->rx_snapshot_reply.node_count;
    uint32_t end;
    switch (p->rx_snapshot_prep_phase) {
    case 1: /* index bounds and uniqueness */
        end = p->rx_snapshot_prep_cursor + JSIPC_SNAPSHOT_PREP_NODES;
        if (end > n) end = n;
        for (uint32_t i = p->rx_snapshot_prep_cursor; i < end; i++) {
            int idx = p->rx_snapshot_nodes[i].node.idx;
            if (idx < -1 || (idx >= 0 && (uint32_t)idx >= n)) return -1;
            if (idx >= 0) {
                if (p->rx_snapshot_seen[idx]) return -1;
                p->rx_snapshot_seen[idx] = 1;
                if (idx > p->rx_snapshot_max_idx) p->rx_snapshot_max_idx = idx;
            }
        }
        p->rx_snapshot_prep_cursor = end;
        if (end == n) {
            p->rx_snapshot_prep_phase = 2;
            p->rx_snapshot_order_cursor = 0;
        }
        return 0;
    case 2: { /* ensure indexes form a contiguous preorder sequence */
        uint32_t limit = p->rx_snapshot_max_idx < 0 ? 0u : (uint32_t)p->rx_snapshot_max_idx + 1u;
        end = p->rx_snapshot_order_cursor + JSIPC_SNAPSHOT_PREP_NODES;
        if (end > limit) end = limit;
        for (uint32_t i = p->rx_snapshot_order_cursor; i < end; i++)
            if (!p->rx_snapshot_seen[i]) return -1;
        p->rx_snapshot_order_cursor = end;
        if (end == limit) {
            p->rx_snapshot_prep_phase = 3;
            p->rx_snapshot_prep_cursor = 0;
        }
        return 0;
    }
    case 3: /* allocate reusable nodes/pseudo boxes in bounded batches */
        end = p->rx_snapshot_prep_cursor + JSIPC_SNAPSHOT_PREP_NODES;
        if (end > n) end = n;
        for (uint32_t i = p->rx_snapshot_prep_cursor; i < end; i++) {
            DOMNode* node = p->nodes_by_id[i];
            if (!node) {
                node = calloc(1, sizeof(*node));
                if (!node) return -1;
                p->rx_snapshot_is_new[i] = 1;
            }
            p->rx_snapshot_prepared[i] = node;
            JSNodeWireHead* h = &p->rx_snapshot_nodes[i];
            if (h->has_pb0) {
                p->rx_snapshot_pb0[i] = malloc(sizeof(PseudoBox));
                if (!p->rx_snapshot_pb0[i]) return -1;
                memcpy(p->rx_snapshot_pb0[i], &p->rx_snapshot_boxes[(size_t)i*3u], sizeof(PseudoBox));
            }
            if (h->has_pb1) {
                p->rx_snapshot_pb1[i] = malloc(sizeof(PseudoBox));
                if (!p->rx_snapshot_pb1[i]) return -1;
                memcpy(p->rx_snapshot_pb1[i], &p->rx_snapshot_boxes[(size_t)i*3u+1u], sizeof(PseudoBox));
            }
            if (h->has_pb2) {
                p->rx_snapshot_pb2[i] = malloc(sizeof(PseudoBox));
                if (!p->rx_snapshot_pb2[i]) return -1;
                memcpy(p->rx_snapshot_pb2[i], &p->rx_snapshot_boxes[(size_t)i*3u+2u], sizeof(PseudoBox));
            }
            if (h->node.idx >= 0) p->rx_snapshot_order[h->node.idx] = node;
        }
        p->rx_snapshot_prep_cursor = end;
        if (end == n) {
            p->rx_snapshot_prep_phase = 4;
            p->rx_snapshot_order_cursor = 0;
        }
        return 0;
    case 4: { /* validate prepared order pointers in batches */
        uint32_t count = p->rx_snapshot_max_idx < 0 ? 0u : (uint32_t)p->rx_snapshot_max_idx + 1u;
        end = p->rx_snapshot_order_cursor + JSIPC_SNAPSHOT_PREP_NODES;
        if (end > count) end = count;
        for (uint32_t i = p->rx_snapshot_order_cursor; i < end; i++)
            if (!p->rx_snapshot_order[i]) return -1;
        p->rx_snapshot_order_cursor = end;
        if (end == count) {
            p->rx_snapshot_prep_phase = 5;
            p->rx_snapshot_prep_cursor = 0;
            p->rx_snapshot_order_cursor = 0;
        }
        return 0;
    }
    case 5: { /* bounded in-place node payload/pseudo-box transfer */
        if (p->rx_snapshot_retired_boxes) {
            /* Do not overwrite the previous retirement queue: synchronous drains
               make the same bounded progress when no ordinary UI poll is running. */
            (void)js_proxy_snapshot_retire_step(p);
            return 0;
        }
        end = p->rx_snapshot_prep_cursor + JSIPC_SNAPSHOT_COMMIT_NODES;
        if (end > n) end = n;
        for (uint32_t i = p->rx_snapshot_prep_cursor; i < end; i++) {
            DOMNode* node = p->rx_snapshot_prepared[i];
            p->rx_snapshot_old_boxes[(size_t)i*3u] = node->pb[0];
            p->rx_snapshot_old_boxes[(size_t)i*3u+1u] = node->pb[1];
            p->rx_snapshot_old_boxes[(size_t)i*3u+2u] = node->pb[2];
            memcpy(node, &p->rx_snapshot_nodes[i].node, sizeof(*node));
            node->next = NULL; node->aux = NULL;
            node->pb[0] = p->rx_snapshot_pb0[i]; node->pb[1] = p->rx_snapshot_pb1[i]; node->pb[2] = p->rx_snapshot_pb2[i];
            p->rx_snapshot_pb0[i] = p->rx_snapshot_pb1[i] = p->rx_snapshot_pb2[i] = NULL;
            if (p->rx_snapshot_is_new[i]) p->nodes_by_id[i] = node;
            p->rx_snapshot_is_new[i] = 0; /* ownership transferred */
        }
        p->rx_snapshot_prep_cursor = end;
        if (end == n) {
            p->rx_snapshot_prep_phase = 6;
            p->rx_snapshot_order_cursor = 0;
        }
        return 0;
    }
    case 6: { /* bounded relink; publish head only after every link is ready */
        int count = p->rx_snapshot_max_idx + 1;
        end = p->rx_snapshot_order_cursor + JSIPC_SNAPSHOT_COMMIT_NODES;
        if (end > (uint32_t)count) end = (uint32_t)count;
        for (uint32_t idx = p->rx_snapshot_order_cursor; idx < end; idx++)
            p->rx_snapshot_order[idx]->next = idx + 1u < (uint32_t)count ? p->rx_snapshot_order[idx+1u] : NULL;
        p->rx_snapshot_order_cursor = end;
        if (end != (uint32_t)count) return 0;

        /* Publication is now constant time: all payloads and links are staged
           in stable nodes, and the UI was prevented from traversing them while
           this commit gate was active. */
        p->head = count > 0 ? p->rx_snapshot_order[0] : NULL;
        webpage_invalidate_layout();
        p->rx_snapshot_retired_boxes = p->rx_snapshot_old_boxes;
        p->rx_snapshot_retired_count = (size_t)n * 3u;
        p->rx_snapshot_retired_cursor = 0;
        p->rx_snapshot_old_boxes = NULL;
        /* stage_clear releases temporary arrays but not transferred nodes/boxes. */
        js_proxy_snapshot_stage_clear(p);
        return 1;
    }
    default: return -1;
    }
}
static int js_proxy_snapshot_retire_step(JSProcess* p) {
    if (!p || !p->rx_snapshot_retired_boxes) return 0;
    size_t end = p->rx_snapshot_retired_cursor + JSIPC_SNAPSHOT_RETIRE_PER_POLL;
    if (end > p->rx_snapshot_retired_count) end = p->rx_snapshot_retired_count;
    for (size_t i = p->rx_snapshot_retired_cursor; i < end; i++)
        free(p->rx_snapshot_retired_boxes[i]);
    p->rx_snapshot_retired_cursor = end;
    if (end == p->rx_snapshot_retired_count) {
        free(p->rx_snapshot_retired_boxes);
        p->rx_snapshot_retired_boxes = NULL;
        p->rx_snapshot_retired_count = p->rx_snapshot_retired_cursor = 0;
    }
    return 1;
}

static int js_proxy_receive_snapshot(JSProcess* p, const JSIPCReply* r) {
    if (!r->node_count) {
        if (r->snapshot_chunks != 0) return 0;
        if (r->scroll_count) {
            if (r->scroll_count > JSIPC_SNAPSHOT_MAX_SCROLL) return 0;
            size_t bytes = (size_t)r->scroll_count * sizeof(L2ScrollState);
            L2ScrollState* state = malloc(bytes); if (!state) return 0;
            int ok = js_ipc_read_all(p->reply_fd, state, bytes);
            if (ok && p->head) l2_scroll_state_import(state, (int)r->scroll_count, p->head);
            free(state); return ok;
        }
        return 1;
    }
    if (!r->snapshot_generation ||
        r->snapshot_chunks != (r->node_count + JSIPC_SNAPSHOT_CHUNK_NODES - 1u) / JSIPC_SNAPSHOT_CHUNK_NODES)
        return 0;
    if ((uint64_t)r->node_count * JSIPC_SNAPSHOT_STAGE_NODE_BYTES > JSIPC_SNAPSHOT_MAX_BYTES ||
        !js_proxy_grow_nodes(p, r->node_count)) return 0;

    /* Stage all records first. A truncated or misframed snapshot must not
       partially mutate the live DOM. Pseudo-box storage is bounded separately. */
    JSNodeWireHead* staged = calloc(r->node_count, sizeof(*staged));
    PseudoBox* boxes = calloc((size_t)r->node_count * 3u, sizeof(*boxes));
    if (!staged || !boxes) { free(staged); free(boxes); return 0; }
    uint32_t expected_start = 0;
    int ok = 1;
    for (uint32_t chunk_no = 0; chunk_no < r->snapshot_chunks && ok; chunk_no++) {
        JSIPCSnapshotChunk ch;
        if (!js_ipc_read_all(p->reply_fd, &ch, sizeof ch)) { ok = 0; break; }
        uint32_t expected_count = r->node_count - expected_start;
        if (expected_count > JSIPC_SNAPSHOT_CHUNK_NODES) expected_count = JSIPC_SNAPSHOT_CHUNK_NODES;
        if (ch.generation != r->snapshot_generation || ch.total != r->node_count ||
            ch.start != expected_start || ch.count != expected_count || !ch.count) { ok = 0; break; }
        for (uint32_t j = 0; j < ch.count; j++) {
            uint32_t i = ch.start + j;
            JSNodeWireHead h;
            if (!js_ipc_read_all(p->reply_fd, &h, sizeof h)) { ok = 0; break; }
            if (h.has_pb0 > 1 || h.has_pb1 > 1 || h.has_pb2 > 1 ||
                h.node.idx < -1 || (h.node.idx >= 0 && (uint32_t)h.node.idx >= r->node_count)) { ok = 0; break; }
            h.node.next = NULL; h.node.aux = NULL; h.node.pb[0] = h.node.pb[1] = h.node.pb[2] = NULL;
            staged[i] = h;
            if (h.has_pb0 && !js_ipc_read_all(p->reply_fd, &boxes[(size_t)i*3u], sizeof(PseudoBox))) { ok = 0; break; }
            if (h.has_pb1 && !js_ipc_read_all(p->reply_fd, &boxes[(size_t)i*3u+1u], sizeof(PseudoBox))) { ok = 0; break; }
            if (h.has_pb2 && !js_ipc_read_all(p->reply_fd, &boxes[(size_t)i*3u+2u], sizeof(PseudoBox))) { ok = 0; break; }
        }
        expected_start += ch.count;
    }
    if (expected_start != r->node_count) ok = 0;

    /* Validate index uniqueness before changing any live node. */
    int max_idx = -1;
    unsigned char* seen = NULL;
    if (ok) {
        seen = calloc(r->node_count ? r->node_count : 1u, 1);
        if (!seen) ok = 0;
    }
    if (ok) for (uint32_t i = 0; i < r->node_count; i++) {
        int idx = staged[i].node.idx;
        if (idx >= 0) {
            if (seen[idx]) { ok = 0; break; }
            seen[idx] = 1;
            if (idx > max_idx) max_idx = idx;
        }
    }
    if (ok) for (int idx = 0; idx <= max_idx; idx++) {
        if (!seen[idx]) { ok = 0; break; }
    }
    if (ok) for (uint32_t i = 0; i < r->node_count; i++) {
        JSNodeWireHead* h = &staged[i];
        DOMNode* n = p->nodes_by_id[i];
        if (!n) { n = calloc(1, sizeof *n); if (!n) { ok = 0; break; } p->nodes_by_id[i] = n; }
        dom_free_pseudo(n);
        memcpy(n, &h->node, sizeof *n); n->next = NULL; n->aux = NULL; n->pb[0] = n->pb[1] = n->pb[2] = NULL;
        if (h->has_pb0) { n->pb[0] = malloc(sizeof(PseudoBox)); if (!n->pb[0]) { ok = 0; break; } memcpy(n->pb[0], &boxes[(size_t)i*3u], sizeof(PseudoBox)); }
        if (h->has_pb1) { n->pb[1] = malloc(sizeof(PseudoBox)); if (!n->pb[1]) { ok = 0; break; } memcpy(n->pb[1], &boxes[(size_t)i*3u+1u], sizeof(PseudoBox)); }
        if (h->has_pb2) { n->pb[2] = malloc(sizeof(PseudoBox)); if (!n->pb[2]) { ok = 0; break; } memcpy(n->pb[2], &boxes[(size_t)i*3u+2u], sizeof(PseudoBox)); }
    }
    free(seen); free(staged); free(boxes);
    if (!ok) return 0;

    int count = max_idx + 1;
    DOMNode** order = calloc((size_t)(count > 0 ? count : 1), sizeof(*order));
    if (!order) return 0;
    for (uint32_t i = 0; i < r->node_count; i++) {
        DOMNode* n = p->nodes_by_id[i];
        if (n && n->idx >= 0 && n->idx < count) order[n->idx] = n;
    }
    for (int i = 0; i < count; i++) {
        if (!order[i]) { free(order); return 0; }
        order[i]->next = i + 1 < count ? order[i + 1] : NULL;
    }
    if (count > 0) p->head = order[0];
    free(order);
    if (r->scroll_count) {
        if (r->scroll_count > JSIPC_SNAPSHOT_MAX_SCROLL) return 0;
        size_t bytes = (size_t)r->scroll_count * sizeof(L2ScrollState);
        L2ScrollState* state = malloc(bytes); if (!state) return 0;
        int read_ok = js_ipc_read_all(p->reply_fd, state, bytes);
        if (read_ok && p->head) l2_scroll_state_import(state, (int)r->scroll_count, p->head);
        free(state); if (!read_ok) return 0;
    }
    webpage_invalidate_layout();
    return 1;
}
static int js_proxy_receive_images(JSProcess* p, const JSIPCReply* r) {
    uint64_t total_bytes = 0;
    for (uint32_t i=0;i<r->image_count;i++) {
        JSIPCImageHead ih; if (!js_ipc_read_all(p->reply_fd,&ih,sizeof ih)) return 0;
        if (ih.node_idx<0 || ih.width<1 || ih.height<1 || ih.width>4096 || ih.height>4096 || (uint64_t)ih.width*ih.height>4000000ull || ih.kind>1) return 0;
        uint64_t bytes64=(uint64_t)ih.width*ih.height*sizeof(uint32_t);
        if (bytes64 > JSIPC_IMAGE_REPLY_MAX_BYTES - total_bytes) return 0;
        total_bytes += bytes64;
        size_t bytes=(size_t)bytes64; uint32_t*px=malloc(bytes); if(!px)return 0;
        int ok=js_ipc_read_all(p->reply_fd,px,bytes);
        if(ok){char key[64];snprintf(key,sizeof key,ih.kind==1?"video:%d":"canvas:%d",ih.node_idx);webpage_image_inject(key,px,(int)ih.width,(int)ih.height);}
        free(px); if(!ok)return 0;
    }
    if(r->image_count){webpage_invalidate_layout();}
    return 1;
}
static void js_proxy_view_transition_flags(JSProcess* p, uint32_t flags) {
    if (!p || !flags) return;
    /* If a new transition replaces an old one, cancel the old frame first. */
    if (flags & 4u) webpage_view_transition_cancel();
    if ((flags & 1u) && !(flags & 8u)) webpage_view_transition_capture(p->head);
    if (flags & 8u) webpage_view_transition_cancel();
    if ((flags & 2u) && !(flags & 8u)) webpage_view_transition_commit(p->head);
}

#define JS_ASYNC_IPC_CAP 32u
static int js_proxy_consume_reply(JSProcess* p, JSIPCReply* r, int op,
                                  int* changed, int* sync_dom) {
    if (sync_dom) *sync_dom = 0;
    if (!p || !r || r->magic != JSIPC_MAGIC) return 0;
    /* Validate every attacker-influenced length/count before allocating or
       consuming payload bytes. Invalid framing kills this child connection. */
    if (r->node_count > JSIPC_SNAPSHOT_MAX_NODES ||
        r->scroll_count > JSIPC_SNAPSHOT_MAX_SCROLL ||
        r->image_count > JSIPC_SNAPSHOT_MAX_NODES ||
        r->payload_len > (64u << 20)) return 0;
    if (changed) *changed = r->result != 0;
    if (r->node_count || r->scroll_count) {
        if (!js_proxy_receive_snapshot(p, r)) return 0;
        if (sync_dom) *sync_dom = 1;
        if (changed) *changed = 1;
    }
    if (r->image_count) {
        if (!js_proxy_receive_images(p, r)) return 0;
        if (sync_dom) *sync_dom = 1;
        if (changed) *changed = 1;
    }
    if (r->payload_len) {
        if (r->payload_len > (64u << 20)) return 0;
        unsigned char* data = malloc(r->payload_len);
        if (!data || !js_ipc_read_all(p->reply_fd, data, r->payload_len)) { free(data); return 0; }
        free(p->payload); p->payload = data; p->payload_len = r->payload_len;
    } else if (op == JSC_TAKE_NAV_REQUEST || (op == JSC_TICK && r->nav_request_pending)) {
        /* A zero-byte GET/form submission must not inherit an older payload. */
        free(p->payload); p->payload = NULL; p->payload_len = 0;
    }
    if (op == JSC_TICK) {
        if (r->nav_pending) { p->async_nav_pending = 1; snprintf(p->async_nav_url, sizeof p->async_nav_url, "%s", r->nav_url); }
        if (r->nav_request_pending) {
            p->async_nav_request_pending = 1;
            snprintf(p->async_nav_method, sizeof p->async_nav_method, "%s", r->method);
            snprintf(p->async_nav_headers, sizeof p->async_nav_headers, "%s", r->headers);
            free(p->async_nav_body); p->async_nav_body = p->payload; p->payload = NULL;
            p->async_nav_body_len = r->payload_len; p->payload_len = 0;
        }
        if (r->scroll_pending) { p->async_scroll_pending = 1; p->async_scroll_x = r->scroll_x; p->async_scroll_y = r->scroll_y; }
        if (r->focus_pending) { p->async_focus_pending = 1; p->async_focus_id = r->async_focus_id; }
        if (r->title_pending) { p->async_title_pending = 1; snprintf(p->async_title, sizeof p->async_title, "%s", r->title_value); }
    }
    if (r->view_transition_flags) {
        js_proxy_view_transition_flags(p, r->view_transition_flags);
        if (changed) *changed = 1;
    }
    return 1;
}
/* Reads a single field from the reply pipe. Nonblocking calls preserve partial
   data between polls and never read more than the per-poll budget. */
static int js_proxy_wire_read_field(JSProcess* p, void* dst, size_t len,
                                    size_t* have, int blocking, size_t* budget) {
    if (!p || !dst || !have || *have > len) return -1;
    while (*have < len) {
        size_t ask = len - *have;
        if (!blocking) {
            if (!budget || *budget == 0) return 0;
            if (ask > *budget) ask = *budget;
        }
        ssize_t n = read(p->reply_fd, (unsigned char*)dst + *have, ask);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && !blocking && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && blocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd fds; fds.fd = p->reply_fd; fds.events = POLLIN; fds.revents = 0;
            int pr;
            do { pr = poll(&fds, 1, 5000); } while (pr < 0 && errno == EINTR);
            if (pr > 0 && (fds.revents & (POLLIN | POLLHUP | POLLERR))) continue;
            return -1;
        }
        if (n <= 0) return -1;
        *have += (size_t)n;
        if (!blocking) *budget -= (size_t)n;
    }
    return 1;
}

static void js_proxy_wire_reset(JSProcess* p) {
    if (!p) return;
    if (p->rx_wire_images) {
        for (uint32_t i = 0; i < p->rx_wire_reply.image_count; i++) free(p->rx_wire_images[i].pixels);
    }
    free(p->rx_wire_images); p->rx_wire_images = NULL;
    free(p->rx_wire_scroll); p->rx_wire_scroll = NULL;
    free(p->rx_wire_image_pixels); p->rx_wire_image_pixels = NULL;
    free(p->rx_wire_payload); p->rx_wire_payload = NULL;
    p->rx_wire_image_pixels_bytes = p->rx_wire_image_pixels_have = 0;
    p->rx_wire_images_done = p->rx_wire_images_published = 0;
    p->rx_wire_dom_committed = 0;
    p->rx_wire_image_bytes = p->rx_wire_staged_bytes = 0;
    p->rx_wire_scroll_bytes = p->rx_wire_scroll_have = 0;
    p->rx_wire_payload_have = 0;
    p->rx_wire_chunk_remaining = 0;
    p->rx_wire_field_have = 0;
    p->rx_wire_header_have = 0;
    p->rx_wire_active = 0;
    p->rx_wire_phase = JSRX_HEADER;
    memset(&p->rx_wire_reply, 0, sizeof p->rx_wire_reply);
    memset(&p->rx_wire_chunk, 0, sizeof p->rx_wire_chunk);
    memset(&p->rx_wire_node, 0, sizeof p->rx_wire_node);
    memset(&p->rx_wire_image_head, 0, sizeof p->rx_wire_image_head);
    js_proxy_snapshot_stage_clear(p);
}

static int js_proxy_wire_set_phase(JSProcess* p, int phase) {
    p->rx_wire_phase = phase;
    p->rx_wire_field_have = 0;
    return 1;
}
static int js_proxy_wire_next_after_scroll(JSProcess* p) {
    const JSIPCReply* r = &p->rx_wire_reply;
    if (r->image_count) return js_proxy_wire_set_phase(p, JSRX_IMAGE_HEAD);
    if (r->payload_len) return js_proxy_wire_set_phase(p, JSRX_PAYLOAD);
    return js_proxy_wire_set_phase(p, JSRX_PUBLISH_IMAGES);
}
static int js_proxy_wire_begin_body(JSProcess* p) {
    const JSIPCReply* r = &p->rx_wire_reply;
    if (r->magic != JSIPC_MAGIC || r->node_count > JSIPC_SNAPSHOT_MAX_NODES ||
        r->scroll_count > JSIPC_SNAPSHOT_MAX_SCROLL || r->image_count > JSIPC_SNAPSHOT_MAX_NODES ||
        r->payload_len > (64u << 20)) return 0;
    uint64_t staged = (uint64_t)r->payload_len +
        (uint64_t)r->scroll_count * sizeof(L2ScrollState) +
        (uint64_t)r->node_count * JSIPC_SNAPSHOT_STAGE_NODE_BYTES +
        (uint64_t)r->image_count * sizeof(JSIPCReceivedImage);
    if (staged > JSIPC_RESPONSE_MAX_BYTES) return 0;
    p->rx_wire_staged_bytes = staged;
    if (r->node_count) {
        if (!js_proxy_snapshot_stage_begin(p, r, p->async_ops[p->async_head])) return 0;
    } else if (r->snapshot_generation || r->snapshot_chunks) return 0;
    if (r->scroll_count) {
        p->rx_wire_scroll_bytes = (size_t)r->scroll_count * sizeof(L2ScrollState);
        p->rx_wire_scroll = calloc(r->scroll_count, sizeof(L2ScrollState));
        if (!p->rx_wire_scroll) return 0;
    }
    if (r->image_count) {
        p->rx_wire_images = calloc(r->image_count, sizeof(JSIPCReceivedImage));
        if (!p->rx_wire_images) return 0;
    }
    if (r->payload_len) {
        p->rx_wire_payload = malloc(r->payload_len);
        if (!p->rx_wire_payload) return 0;
    }
    if (r->node_count) return js_proxy_wire_set_phase(p, JSRX_SNAPSHOT_CHUNK);
    if (r->scroll_count) return js_proxy_wire_set_phase(p, JSRX_SCROLL);
    return js_proxy_wire_next_after_scroll(p);
}
static int js_proxy_wire_validate_chunk(JSProcess* p) {
    const JSIPCReply* r = &p->rx_wire_reply;
    uint32_t want = r->node_count - p->rx_snapshot_next_node;
    if (want > JSIPC_SNAPSHOT_CHUNK_NODES) want = JSIPC_SNAPSHOT_CHUNK_NODES;
    if (p->rx_wire_chunk.generation != r->snapshot_generation ||
        p->rx_wire_chunk.total != r->node_count ||
        p->rx_wire_chunk.start != p->rx_snapshot_next_node ||
        p->rx_wire_chunk.count != want || !want ||
        p->rx_snapshot_chunk_no >= r->snapshot_chunks) return 0;
    p->rx_wire_chunk_remaining = want;
    return js_proxy_wire_set_phase(p, JSRX_NODE_HEAD);
}
/* Advances the oldest async reply. Return 0 for incomplete, 1 for completed,
   and -1 for invalid framing, EOF or allocation failure. In nonblocking mode
   at most RX_BUDGET bytes are consumed in this call, including partial headers,
   node records, scroll data, image pixels and request bodies. */
static int js_proxy_async_receive_step(JSProcess* p, int* changed, int blocking,
                                       int allow_debugger_ui) {
    if (changed) *changed = 0;
    if (!p || !p->async_count || p->dead) return -1;
    size_t budget = JSIPC_RX_BUDGET;
    if (!p->rx_wire_active) {
        p->rx_wire_active = 1;
        p->rx_wire_phase = JSRX_HEADER;
        p->rx_wire_header_have = 0;
        p->rx_wire_field_have = 0;
    }
    for (;;) {
        int got = 0;
        JSIPCReply* r = &p->rx_wire_reply;
        switch (p->rx_wire_phase) {
        case JSRX_HEADER:
            got = js_proxy_wire_read_field(p, p->rx_wire_header, sizeof(JSIPCReply),
                                           &p->rx_wire_header_have, blocking, &budget);
            if (got <= 0) return got;
            memcpy(r, p->rx_wire_header, sizeof(*r));
            if (r->magic != JSIPC_MAGIC) return -1;
            if (r->debug_state == JSIPC_DEBUG_PAUSED) {
                if (!js_proxy_handle_debug_pause(p, r, allow_debugger_ui)) return -1;
                p->rx_wire_active = 0; p->rx_wire_header_have = 0;
                memset(r, 0, sizeof(*r));
                return 0;
            }
            if (browser_debugger_pause_session_done) browser_debugger_pause_session_done(p);
            if (!js_proxy_wire_begin_body(p)) return -1;
            break;
        case JSRX_SNAPSHOT_CHUNK:
            got = js_proxy_wire_read_field(p, &p->rx_wire_chunk, sizeof(p->rx_wire_chunk),
                                           &p->rx_wire_field_have, blocking, &budget);
            if (got <= 0) return got;
            if (!js_proxy_wire_validate_chunk(p)) return -1;
            break;
        case JSRX_NODE_HEAD: {
            got = js_proxy_wire_read_field(p, &p->rx_wire_node, sizeof(p->rx_wire_node),
                                           &p->rx_wire_field_have, blocking, &budget);
            if (got <= 0) return got;
            JSNodeWireHead* h = &p->rx_wire_node;
            if (h->has_pb0 > 1 || h->has_pb1 > 1 || h->has_pb2 > 1 ||
                h->node.idx < -1 || (h->node.idx >= 0 && (uint32_t)h->node.idx >= r->node_count)) return -1;
            h->node.next = NULL; h->node.aux = NULL;
            h->node.pb[0] = h->node.pb[1] = h->node.pb[2] = NULL;
            p->rx_snapshot_nodes[p->rx_snapshot_next_node] = *h;
            if (h->has_pb0) js_proxy_wire_set_phase(p, JSRX_NODE_PB0);
            else if (h->has_pb1) js_proxy_wire_set_phase(p, JSRX_NODE_PB1);
            else if (h->has_pb2) js_proxy_wire_set_phase(p, JSRX_NODE_PB2);
            else {
                p->rx_snapshot_next_node++; p->rx_wire_chunk_remaining--;
                if (!p->rx_wire_chunk_remaining) {
                    p->rx_snapshot_chunk_no++;
                    if (p->rx_snapshot_next_node == r->node_count) {
                        js_proxy_wire_set_phase(p, r->scroll_count ? JSRX_SCROLL :
                            (r->image_count ? JSRX_IMAGE_HEAD : (r->payload_len ? JSRX_PAYLOAD : JSRX_PUBLISH_IMAGES)));
                    } else js_proxy_wire_set_phase(p, JSRX_SNAPSHOT_CHUNK);
                } else js_proxy_wire_set_phase(p, JSRX_NODE_HEAD);
            }
            break;
        }
        case JSRX_NODE_PB0:
        case JSRX_NODE_PB1:
        case JSRX_NODE_PB2: {
            uint32_t i = p->rx_snapshot_next_node;
            unsigned slot = p->rx_wire_phase == JSRX_NODE_PB0 ? 0u :
                            (p->rx_wire_phase == JSRX_NODE_PB1 ? 1u : 2u);
            PseudoBox* dst = &p->rx_snapshot_boxes[(size_t)i * 3u + slot];
            got = js_proxy_wire_read_field(p, dst, sizeof(PseudoBox),
                                           &p->rx_wire_field_have, blocking, &budget);
            if (got <= 0) return got;
            if (slot == 0 && p->rx_wire_node.has_pb1) js_proxy_wire_set_phase(p, JSRX_NODE_PB1);
            else if (slot <= 1 && p->rx_wire_node.has_pb2) js_proxy_wire_set_phase(p, JSRX_NODE_PB2);
            else {
                p->rx_snapshot_next_node++; p->rx_wire_chunk_remaining--;
                if (!p->rx_wire_chunk_remaining) {
                    p->rx_snapshot_chunk_no++;
                    if (p->rx_snapshot_next_node == r->node_count) {
                        js_proxy_wire_set_phase(p, r->scroll_count ? JSRX_SCROLL :
                            (r->image_count ? JSRX_IMAGE_HEAD : (r->payload_len ? JSRX_PAYLOAD : JSRX_PUBLISH_IMAGES)));
                    } else js_proxy_wire_set_phase(p, JSRX_SNAPSHOT_CHUNK);
                } else js_proxy_wire_set_phase(p, JSRX_NODE_HEAD);
            }
            break;
        }
        case JSRX_SCROLL:
            got = js_proxy_wire_read_field(p, p->rx_wire_scroll, p->rx_wire_scroll_bytes,
                                           &p->rx_wire_scroll_have, blocking, &budget);
            if (got <= 0) return got;
            js_proxy_wire_next_after_scroll(p);
            break;
        case JSRX_IMAGE_HEAD:
            got = js_proxy_wire_read_field(p, &p->rx_wire_image_head, sizeof(p->rx_wire_image_head),
                                           &p->rx_wire_field_have, blocking, &budget);
            if (got <= 0) return got;
            uint32_t image_node_limit = r->node_count ? r->node_count : (uint32_t)p->node_cap;
            if (p->rx_wire_image_head.node_idx < 0 || (uint32_t)p->rx_wire_image_head.node_idx >= image_node_limit ||
                p->rx_wire_image_head.width < 1 || p->rx_wire_image_head.height < 1 ||
                p->rx_wire_image_head.width > 4096 || p->rx_wire_image_head.height > 4096 ||
                (uint64_t)p->rx_wire_image_head.width * p->rx_wire_image_head.height > 4000000ull ||
                p->rx_wire_image_head.kind > 1) return -1;
            p->rx_wire_image_pixels_bytes = (size_t)p->rx_wire_image_head.width *
                p->rx_wire_image_head.height * sizeof(uint32_t);
            if (p->rx_wire_image_pixels_bytes > JSIPC_IMAGE_REPLY_MAX_BYTES - p->rx_wire_image_bytes ||
                p->rx_wire_image_pixels_bytes > JSIPC_RESPONSE_MAX_BYTES - p->rx_wire_staged_bytes) return -1;
            p->rx_wire_image_bytes += p->rx_wire_image_pixels_bytes;
            p->rx_wire_staged_bytes += p->rx_wire_image_pixels_bytes;
            p->rx_wire_image_pixels = malloc(p->rx_wire_image_pixels_bytes);
            if (!p->rx_wire_image_pixels) return -1;
            p->rx_wire_image_pixels_have = 0;
            js_proxy_wire_set_phase(p, JSRX_IMAGE_PIXELS);
            break;
        case JSRX_IMAGE_PIXELS:
            got = js_proxy_wire_read_field(p, p->rx_wire_image_pixels, p->rx_wire_image_pixels_bytes,
                                           &p->rx_wire_image_pixels_have, blocking, &budget);
            if (got <= 0) return got;
            p->rx_wire_images[p->rx_wire_images_done].head = p->rx_wire_image_head;
            p->rx_wire_images[p->rx_wire_images_done].pixels = p->rx_wire_image_pixels;
            p->rx_wire_images[p->rx_wire_images_done].bytes = p->rx_wire_image_pixels_bytes;
            p->rx_wire_image_pixels = NULL;
            p->rx_wire_images_done++;
            if (p->rx_wire_images_done < r->image_count) {
                memset(&p->rx_wire_image_head, 0, sizeof(p->rx_wire_image_head));
                js_proxy_wire_set_phase(p, JSRX_IMAGE_HEAD);
            } else if (r->payload_len) js_proxy_wire_set_phase(p, JSRX_PAYLOAD);
            else js_proxy_wire_set_phase(p, JSRX_PUBLISH_IMAGES);
            break;
        case JSRX_PAYLOAD:
            got = js_proxy_wire_read_field(p, p->rx_wire_payload, r->payload_len,
                                           &p->rx_wire_payload_have, blocking, &budget);
            if (got <= 0) return got;
            js_proxy_wire_set_phase(p, JSRX_PUBLISH_IMAGES);
            break;
        case JSRX_PUBLISH_IMAGES:
            /* Wire payload is now complete and validated. Publish the DOM once,
               then inject at most one image per UI poll before completing metadata. */
            if (!p->rx_wire_dom_committed) {
                if (p->rx_snapshot_active) {
                    if (p->rx_snapshot_next_node != r->node_count ||
                        p->rx_snapshot_chunk_no != r->snapshot_chunks) return -1;
                    int prep = js_proxy_snapshot_prepare_step(p);
                    if (prep < 0) return -1;
                    if (prep == 0) return 0; /* one bounded preparation batch per poll */
                }
                if (p->rx_wire_scroll && p->head)
                    l2_scroll_state_import(p->rx_wire_scroll, (int)r->scroll_count, p->head);
                p->rx_wire_dom_committed = 1;
                if (changed && (r->node_count || r->scroll_count)) *changed = 1;
                if (!blocking) return 0;
            }
            if (r->image_count && p->rx_wire_images_done != r->image_count) return -1;
            if (p->rx_wire_images_published < r->image_count) {
                uint32_t i = p->rx_wire_images_published++;
                char key[64];
                snprintf(key, sizeof key, p->rx_wire_images[i].head.kind == 1 ? "video:%d" : "canvas:%d",
                         p->rx_wire_images[i].head.node_idx);
                webpage_image_inject(key, p->rx_wire_images[i].pixels,
                    (int)p->rx_wire_images[i].head.width, (int)p->rx_wire_images[i].head.height);
                if (!blocking) { if (changed) *changed = 1; return 0; }
                break;
            }
            js_proxy_wire_set_phase(p, JSRX_FINISH);
            break;
        case JSRX_FINISH: {
            int did_snapshot = r->node_count != 0;
            if (r->payload_len) {
                free(p->payload); p->payload = p->rx_wire_payload; p->rx_wire_payload = NULL;
                p->payload_len = r->payload_len;
            } else if (p->async_ops[p->async_head] == JSC_TAKE_NAV_REQUEST ||
                       (p->async_ops[p->async_head] == JSC_TICK && r->nav_request_pending)) {
                free(p->payload); p->payload = NULL; p->payload_len = 0;
            }
            JSIPCReply meta = *r;
            meta.node_count = meta.scroll_count = meta.image_count = meta.payload_len = 0;
            meta.snapshot_generation = meta.snapshot_chunks = 0;
            meta.nav_request_pending = 0; /* body has already been consumed above */
            int op = p->async_ops[p->async_head];
            int meta_op = op == JSC_TAKE_NAV_REQUEST ? -1 : op;
            int changed_tail = 0;
            if (!js_proxy_consume_reply(p, &meta, meta_op, &changed_tail, NULL)) return -1;
            if (op == JSC_TICK && r->nav_request_pending) {
                p->async_nav_request_pending = 1;
                snprintf(p->async_nav_method, sizeof p->async_nav_method, "%s", r->method);
                snprintf(p->async_nav_headers, sizeof p->async_nav_headers, "%s", r->headers);
                free(p->async_nav_body); p->async_nav_body = p->payload; p->payload = NULL;
                p->async_nav_body_len = r->payload_len; p->payload_len = 0;
            }
            if (changed) *changed = changed_tail || did_snapshot || r->scroll_count || r->image_count || r->result ||
                r->nav_pending || r->nav_request_pending || r->scroll_pending || r->focus_pending || r->title_pending ||
                r->view_transition_flags;
            js_proxy_wire_reset(p);
            return 1;
        }
        default: return -1;
        }
        if (!blocking && budget == 0) return 0;
    }
}

static int js_proxy_async_enqueue(JSProcess* p, JSIPCCommand* c) {
    if (!p || p->magic != JSIPC_MAGIC || p->dead ||
        p->async_count + p->async_cmd_count >= JS_ASYNC_IPC_CAP) return 0;
    c->magic = JSIPC_MAGIC;
    unsigned tail = (p->async_cmd_head + p->async_cmd_count) % JS_ASYNC_IPC_CAP;
    p->async_cmds[tail] = *c;
    p->async_cmd_count++;
    return 1;
}
static int js_proxy_async_has_op(JSProcess* p, int op);
static void js_proxy_queue_vt_finish_if_possible(JSProcess* p) {
    if (!p || !p->async_vt_finish_pending || p->dead) return;
    if (js_proxy_async_has_op(p, JSC_VIEW_TRANSITION_FINISHED)) {
        p->async_vt_finish_pending = 0;
        return;
    }
    JSIPCCommand c; memset(&c, 0, sizeof c); c.op = JSC_VIEW_TRANSITION_FINISHED;
    if (js_proxy_async_enqueue(p, &c)) p->async_vt_finish_pending = 0;
}
static int js_proxy_async_has_op(JSProcess* p, int op) {
    if (!p) return 0;
    for (unsigned i = 0; i < p->async_count; i++)
        if (p->async_ops[(p->async_head + i) % JS_ASYNC_IPC_CAP] == op) return 1;
    for (unsigned i = 0; i < p->async_cmd_count; i++)
        if (p->async_cmds[(p->async_cmd_head + i) % JS_ASYNC_IPC_CAP].op == op) return 1;
    return 0;
}
static int js_proxy_flush_async(JSProcess* p, int wait_for_writable) {
    if (!p || p->dead) return 0;
    while (p->async_cmd_count) {
        JSIPCCommand* c = &p->async_cmds[p->async_cmd_head];
        const unsigned char* data = (const unsigned char*)c + p->async_cmd_offset;
        size_t remaining = sizeof(*c) - p->async_cmd_offset;
        ssize_t written = -1;
        if (wait_for_writable) {
            if (!js_ipc_write_all(p->cmd_fd, data, remaining)) { p->dead = 1; return 0; }
            p->async_cmd_offset = sizeof(*c);
        } else {
            int old_flags = fcntl(p->cmd_fd, F_GETFL, 0);
            if (old_flags < 0 || fcntl(p->cmd_fd, F_SETFL, old_flags | O_NONBLOCK) < 0) {
                p->dead = 1; return 0;
            }
            written = write(p->cmd_fd, data, remaining);
            int saved_errno = errno;
            (void)fcntl(p->cmd_fd, F_SETFL, old_flags);
            if (written < 0 && saved_errno == EINTR) continue;
            if (written < 0 && (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)) return 1;
            if (written <= 0) { p->dead = 1; return 0; }
            p->async_cmd_offset += (size_t)written;
            if (p->async_cmd_offset < sizeof(*c)) continue;
        }
        unsigned response_tail = (p->async_head + p->async_count) % JS_ASYNC_IPC_CAP;
        p->async_ops[response_tail] = c->op;
        p->async_count++;
        p->async_cmd_head = (p->async_cmd_head + 1u) % JS_ASYNC_IPC_CAP;
        p->async_cmd_count--;
        p->async_cmd_offset = 0;
    }
    return 1;
}
/* A synchronous operation must preserve wire ordering: consume all preceding
   queued responses before sending its request, otherwise replies could be
   accidentally attributed to the wrong call. */
static int js_proxy_drain_async(JSProcess* p, int allow_debugger_ui) {
    if (!js_proxy_flush_async(p, 1)) return 0;
    while (p && p->async_count && !p->dead) {
        int changed = 0;
        int step = js_proxy_async_receive_step(p, &changed, 1, allow_debugger_ui);
        if (step < 0) {
            p->dead = 1; p->async_count = 0; js_proxy_wire_reset(p); return 0;
        }
        if (step == 0) continue;
        p->async_head = (p->async_head + 1u) % JS_ASYNC_IPC_CAP;
        p->async_count--;
    }
    return p && !p->dead;
}
static int js_proxy_poll_async(JSProcess* p, int allow_debugger_ui) {
    /* Reclaim old pseudo boxes in small batches; they were detached atomically
       during snapshot commit, so retirement is no longer on the paint path. */
    (void)js_proxy_snapshot_retire_step(p);
    js_proxy_queue_vt_finish_if_possible(p);
    if (!js_proxy_flush_async(p, 0)) return 0;
    if (!p || !p->async_count || p->dead) return 0;

    int old_flags = fcntl(p->reply_fd, F_GETFL, 0);
    if (old_flags < 0 || fcntl(p->reply_fd, F_SETFL, old_flags | O_NONBLOCK) < 0) {
        p->dead = 1; js_proxy_wire_reset(p); return 0;
    }
    int changed = 0;
    int step = js_proxy_async_receive_step(p, &changed, 0, allow_debugger_ui);
    int saved_errno = errno;
    (void)fcntl(p->reply_fd, F_SETFL, old_flags);
    errno = saved_errno;
    if (step < 0) {
        p->dead = 1; p->async_count = 0; js_proxy_wire_reset(p); return 0;
    }
    if (step == 0) return changed;
    p->async_head = (p->async_head + 1u) % JS_ASYNC_IPC_CAP;
    p->async_count--;
    js_proxy_queue_vt_finish_if_possible(p);
    (void)js_proxy_flush_async(p, 0);
    return changed;
}
static int js_proxy_call(JSProcess* p, JSIPCCommand* c, JSIPCReply* r, int* sync_dom) {
    if (sync_dom) *sync_dom = 0;
    if (!p || p->magic != JSIPC_MAGIC || p->dead) return 0;
    if (!js_proxy_drain_async(p, 1)) return 0;
    c->magic = JSIPC_MAGIC;
    if (!js_ipc_write_all(p->cmd_fd, c, sizeof *c) || !js_proxy_read_reply(p, r, 1)) {
        p->dead = 1; return 0;
    }
    int changed = 0;
    if (!js_proxy_consume_reply(p, r, c->op, &changed, sync_dom)) {
        p->dead = 1; return 0;
    }
    return r->status;
}

static int js_proxy_id(JSProcess* p, const DOMNode* node) {
    if (!p || !node) return -1;
    /* IDs are private to the page process; resolve through the stable proxy map. */
    for (int i = 0; i < p->node_cap; i++) if (p->nodes_by_id[i] == node) return i;
    return -1;
}
static void js_proxy_target_state(JSIPCCommand* c, const DOMNode* n) {
    if (!n) return; c->flags |= 1; c->checked = n->checked; c->selected = n->selected;
    c->disabled = n->disabled; c->readonly = n->readonly; c->file_selected = n->file_selected;
    snprintf(c->value, sizeof c->value, "%s", n->value);
}
static void js_proxy_free(JSProcess* p) {
    if (!p) return;
    /* A page being replaced must not synchronously drain a large stale DOM
       snapshot. The isolated process has no shared resources requiring a graceful
       shutdown, so cancel outstanding IPC by terminating that child. */
    if (p->rx_snapshot_active || p->async_count || p->async_cmd_count) p->dead = 1;
    if (!p->dead) { JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_FREE; (void)js_proxy_call(p,&c,&r,NULL); }
    if (p->cmd_fd >= 0) close(p->cmd_fd); if (p->reply_fd >= 0) close(p->reply_fd);
    if (p->pid > 0) { int st; if (p->dead) kill(p->pid, SIGTERM); pid_t w; do { w = waitpid(p->pid, &st, 0); } while (w < 0 && errno == EINTR); }
    /* Active nodes are owned by the DOM linked list; detached JS-created nodes are not. */
    for (int i = 0; i < p->node_cap; i++) { DOMNode* n = p->nodes_by_id[i]; if (n && n->idx < 0 && n != p->head) { dom_free_pseudo(n); free(n); } }
    js_proxy_wire_reset(p);
    if (p->rx_snapshot_retired_boxes) {
        for (size_t i = p->rx_snapshot_retired_cursor; i < p->rx_snapshot_retired_count; i++)
            free(p->rx_snapshot_retired_boxes[i]);
        free(p->rx_snapshot_retired_boxes);
    }
    free(p->nodes_by_id); free(p->payload); free(p->async_nav_body); p->magic = 0; free(p);
}
#endif /* !_WIN32 */

/* Public API: Linux/macOS-style targets use one JS child process per document.
 * Windows keeps the existing in-process path until a CreateProcess transport is added. */
void* webpage_js_attach(DOMNode* head, const char* url, PageBundle* bundle) {
    if (!head || !head->v2) { webpage_bundle_free(bundle); return NULL; }
#ifdef _WIN32
    return js_local_attach(head, url, bundle);
#else
    signal(SIGPIPE, SIG_IGN);
    JSProcess* p = calloc(1, sizeof *p); if (!p) { webpage_bundle_free(bundle); return NULL; }
    p->magic = JSIPC_MAGIC; p->cmd_fd = p->reply_fd = -1; p->head = head;
    int initial = 0; for (DOMNode* n = head; n; n = n->next) initial++;
    if (!js_proxy_grow_nodes(p, (uint32_t)initial)) { free(p); webpage_bundle_free(bundle); return NULL; }
    { int i=0; for (DOMNode* n=head; n; n=n->next) p->nodes_by_id[i++] = n; }
    int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        if (to_child[0] >= 0) { close(to_child[0]); close(to_child[1]); }
        if (from_child[0] >= 0) { close(from_child[0]); close(from_child[1]); }
        free(p->nodes_by_id); free(p); webpage_bundle_free(bundle); return NULL;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(to_child[1]); close(from_child[0]);
        g_debug_cmd_fd = to_child[0]; g_debug_reply_fd = from_child[1];
        Page* page = js_local_attach(head, url, bundle);
        JSIPCReply r; memset(&r, 0, sizeof r); r.status = page != NULL; r.focus_id = -1;
        if (!js_ipc_send_reply(from_child[1], &r, page, page != NULL)) _exit(111);
        js_ipc_child_loop(to_child[0], from_child[1], page);
    }
    close(to_child[0]); close(from_child[1]);
    if (pid < 0) {
        close(to_child[1]); close(from_child[0]); free(p->nodes_by_id); free(p); webpage_bundle_free(bundle); return NULL;
    }
    p->pid = pid; p->cmd_fd = to_child[1]; p->reply_fd = from_child[0];
    webpage_bundle_free(bundle); /* the child owns its forked copy */
    JSIPCReply r;
    if (!js_proxy_read_reply(p, &r, 1) || !r.status ||
        (r.node_count && !js_proxy_receive_snapshot(p, &r))) {
        p->dead = 1; js_proxy_free(p); return NULL;
    }
    js_proxy_view_transition_flags(p, r.view_transition_flags);
    gal_log("INFO", "[JS] isolated page process pid=%ld (%u DOM nodes)", (long)p->pid, r.node_count);
    return p;
#endif
}
void webpage_js_free(void* page) {
#ifdef _WIN32
    js_local_free(page);
#else
    JSProcess* p = (JSProcess*)page; if (p && p->magic == JSIPC_MAGIC) js_proxy_free(p);
#endif
}
int webpage_js_tick(void* page) {
#ifdef _WIN32
    int changed = js_local_tick(page);
    int vt_frame = webpage_view_transition_tick();
    if (webpage_view_transition_take_finished())
        changed |= js_local_view_transition_finished(page);
    return changed || vt_frame;
#else
    JSProcess* p = (JSProcess*)page;
    JSIPCCommand c; JSIPCReply r; memset(&c, 0, sizeof c); c.op = JSC_TICK;
    int ok = js_proxy_call(p, &c, &r, NULL);
    int changed = ok ? r.result : 0;
    int vt_frame = webpage_view_transition_tick();
    if (webpage_view_transition_take_finished()) {
        JSIPCCommand finish; JSIPCReply fr;
        memset(&finish, 0, sizeof finish); memset(&fr, 0, sizeof fr);
        finish.op = JSC_VIEW_TRANSITION_FINISHED;
        int finish_ok = js_proxy_call(p, &finish, &fr, NULL);
        if (finish_ok && fr.focus_id) changed = 1;
    }
    return changed || vt_frame;
#endif
}
int webpage_js_tick_async(void* page) {
#ifdef _WIN32
    /* The Win32 in-process runtime has no process transport yet. */
    return js_local_tick(page);
#else
    JSProcess* p = (JSProcess*)page;
    if (!p || p->magic != JSIPC_MAGIC || p->dead) return 0;
    if (js_proxy_async_has_op(p, JSC_TICK)) return 1; /* coalesce frame ticks */
    JSIPCCommand c; memset(&c, 0, sizeof c); c.op = JSC_TICK;
    return js_proxy_async_enqueue(p, &c);
#endif
}
int webpage_js_view_transition_tick_async(void* page) {
    int changed = webpage_view_transition_tick();
    if (!webpage_view_transition_take_finished()) return changed;
#ifdef _WIN32
    /* In-process runtime: finish immediately without a process transport. */
    return js_local_view_transition_finished(page) || changed;
#else
    JSProcess* p = (JSProcess*)page;
    if (!p || p->magic != JSIPC_MAGIC || p->dead) return changed;
    p->async_vt_finish_pending = 1;
    js_proxy_queue_vt_finish_if_possible(p);
    return 1; /* completion must refresh the final frame once delivered */
#endif
}
int webpage_js_poll(void* page, int allow_debugger_ui) {
#ifdef _WIN32
    (void)page; (void)allow_debugger_ui; return 0;
#else
    return js_proxy_poll_async((JSProcess*)page, !!allow_debugger_ui);
#endif
}
int webpage_js_has_pending(void* page) {
#ifdef _WIN32
    (void)page; return 0;
#else
    JSProcess* p = (JSProcess*)page;
    return p && p->magic == JSIPC_MAGIC && !p->dead &&
           (p->async_count != 0 || p->async_cmd_count != 0 || p->rx_snapshot_active || p->rx_wire_active || p->rx_snapshot_retired_boxes != NULL);
#endif
}
/* True only during the in-place mutation/relink phases. The browser loop must
   hold the last complete frame and suppress tree-dependent input until false. */
int webpage_js_dom_commit_pending(void* page) {
#ifdef _WIN32
    (void)page; return 0;
#else
    JSProcess* p = (JSProcess*)page;
    return p && p->magic == JSIPC_MAGIC && !p->dead && p->rx_snapshot_active &&
           (p->rx_snapshot_prep_phase == 5 || p->rx_snapshot_prep_phase == 6);
#endif
}
static int js_dispatch_event_is_async_safe(const char* type) {
    /* These events are non-cancelable by the DOM event model or have no
       default action in this browser. Keep keydown, beforeinput, click,
       submit and pointer/mouse events synchronous because callers need the
       preventDefault result before deciding what native action to take. */
    static const char* const safe[] = {
        "input", "change", "focus", "blur", "focusin", "focusout",
        "scroll", "resize", "load", "unload", "pageshow", "pagehide",
        "animationstart", "animationend", "animationiteration",
        "transitionend", "transitionrun", "transitionstart", "transitioncancel",
        NULL
    };
    if (!type) return 0;
    for (int i = 0; safe[i]; i++) if (!strcmp(type, safe[i])) return 1;
    return 0;
}
int webpage_js_dispatch(void* page, DOMNode* target, const char* type, const char* key, int mods) {
#ifdef _WIN32
    return js_local_dispatch(page,target,type,key,mods);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_DISPATCH; c.id=js_proxy_id(p,target); c.mods=mods; snprintf(c.type,sizeof c.type,"%s",type?type:""); snprintf(c.key,sizeof c.key,"%s",key?key:""); js_proxy_target_state(&c,target);
    if (js_dispatch_event_is_async_safe(type)) {
        if (js_proxy_async_enqueue(p, &c)) return 0;
        /* Never fall back to synchronous IPC for a non-cancelable event: if
           JS is busy, that would freeze the UI exactly when backpressure is
           supposed to protect it. The bounded queue may drop events under
           saturation; key/click/submit still use the synchronous contract. */
        if (p && p->magic == JSIPC_MAGIC && !p->dead &&
            p->async_count + p->async_cmd_count >= JS_ASYNC_IPC_CAP) {
            p->async_dropped++;
            if (!p->async_overflow_warned) {
                p->async_overflow_warned = 1;
                gal_log("WARN", "[JS IPC] async DOM event queue full; dropping non-cancelable events until it drains");
            }
        }
        return 0;
    }
    return js_proxy_call(p,&c,&r,NULL) ? r.result : 0;
#endif
}
int webpage_js_dispatch_mouse(void* page, DOMNode* target, const char* type, int x, int y, int button, int buttons, int mods) {
#ifdef _WIN32
    return js_local_dispatch_mouse(page,target,type,x,y,button,buttons,mods);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_DISPATCH; c.id=js_proxy_id(p,target); c.mods=mods; c.flags=2; c.mouse_x=x; c.mouse_y=y; c.mouse_button=button; c.mouse_buttons=buttons; snprintf(c.type,sizeof c.type,"%s",type?type:""); js_proxy_target_state(&c,target); return js_proxy_call(p,&c,&r,NULL) ? r.result : 0;
#endif
}
int webpage_js_dispatch_file_drop(void* page, DOMNode* target, const char* path, int x, int y) {
#ifdef _WIN32
    return js_local_dispatch_ex(page,target,"__filedrop",path?path:"",0,1,x,y,0,0);
#else
    JSProcess* p=page;JSIPCCommand c;JSIPCReply r;memset(&c,0,sizeof c);c.op=JSC_DISPATCH;c.id=js_proxy_id(p,target);c.mods=0;c.flags=2;c.mouse_x=x;c.mouse_y=y;snprintf(c.type,sizeof c.type,"__filedrop");snprintf(c.key,sizeof c.key,"%s",path?path:"");js_proxy_target_state(&c,target);return js_proxy_call(p,&c,&r,NULL)?r.result:0;
#endif
}
int webpage_js_before_submit(void* page, DOMNode* control) {
#ifdef _WIN32
    return js_local_before_submit(page,control);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_SUBMIT; c.id=js_proxy_id(p,control); js_proxy_target_state(&c,control); return js_proxy_call(p,&c,&r,NULL) ? r.result : 1;
#endif
}
const char* webpage_js_take_nav(void* page) {
#ifdef _WIN32
    return js_local_take_nav(page);
#else
    JSProcess* p = (JSProcess*)page;
    if (!p || p->magic != JSIPC_MAGIC || !p->async_nav_pending) return NULL;
    p->async_nav_pending = 0;
    snprintf(p->text, sizeof p->text, "%s", p->async_nav_url);
    p->async_nav_url[0] = 0;
    return p->text;
#endif
}
int webpage_js_take_nav_request(void* page, char* method, size_t method_cap, char* headers, size_t headers_cap, unsigned char** body, size_t* body_len) {
#ifdef _WIN32
    return js_local_take_nav_request(page, method, method_cap, headers, headers_cap, body, body_len);
#else
    JSProcess* p = (JSProcess*)page;
    if (body) *body = NULL;
    if (body_len) *body_len = 0;
    if (!p || p->magic != JSIPC_MAGIC || !p->async_nav_request_pending) return 0;
    if (method && method_cap) snprintf(method, method_cap, "%s", p->async_nav_method);
    if (headers && headers_cap) snprintf(headers, headers_cap, "%s", p->async_nav_headers);
    if (body) { *body = p->async_nav_body; p->async_nav_body = NULL; }
    else { free(p->async_nav_body); p->async_nav_body = NULL; }
    if (body_len) *body_len = p->async_nav_body_len;
    p->async_nav_body_len = 0; p->async_nav_request_pending = 0;
    p->async_nav_method[0] = p->async_nav_headers[0] = 0;
    return 1;
#endif
}
int webpage_js_take_scroll(void* page, int* x, int* y) {
#ifdef _WIN32
    return js_local_take_scroll(page, x, y);
#else
    JSProcess* p = (JSProcess*)page;
    if (!p || p->magic != JSIPC_MAGIC || !p->async_scroll_pending) return 0;
    if (x) *x = p->async_scroll_x;
    if (y) *y = p->async_scroll_y;
    p->async_scroll_pending = 0;
    return 1;
#endif
}
int webpage_js_take_focus(void* page, DOMNode** node) {
#ifdef _WIN32
    return js_local_take_focus(page, node);
#else
    JSProcess* p = (JSProcess*)page;
    if (!p || p->magic != JSIPC_MAGIC || !p->async_focus_pending) return 0;
    if (node) *node = (p->async_focus_id >= 0 && p->async_focus_id < p->node_cap) ? p->nodes_by_id[p->async_focus_id] : NULL;
    p->async_focus_pending = 0;
    return 1;
#endif
}
void webpage_js_set_focus(void* page, DOMNode* node) {
#ifdef _WIN32
    js_local_set_focus(page, node);
#else
    JSProcess* p = (JSProcess*)page;
    JSIPCCommand c; memset(&c, 0, sizeof c); c.op = JSC_SET_FOCUS; c.id = js_proxy_id(p, node);
    (void)js_proxy_async_enqueue(p, &c);
#endif
}
const char* webpage_js_take_title(void* page) {
#ifdef _WIN32
    return js_local_take_title(page);
#else
    JSProcess* p = (JSProcess*)page;
    if (!p || p->magic != JSIPC_MAGIC || !p->async_title_pending) return NULL;
    p->async_title_pending = 0;
    snprintf(p->text, sizeof p->text, "%s", p->async_title);
    p->async_title[0] = 0;
    return p->text;
#endif
}
void webpage_js_restyle(void* page) {
#ifdef _WIN32
    js_local_restyle(page);
#else
    JSProcess* p = (JSProcess*)page; JSIPCCommand c; memset(&c, 0, sizeof c);
    c.op = JSC_RESTYLE; c.id = js_proxy_id(p, webpage_hover_node()); c.alt_id = js_proxy_id(p, webpage_active_node());
    if (!js_proxy_async_has_op(p, JSC_RESTYLE)) (void)js_proxy_async_enqueue(p, &c);
    webpage_dyn_clear_pending();
#endif
}
void webpage_js_anim_events(void* page) {
#ifdef _WIN32
    js_local_anim_events(page);
#else
    JSProcess* p = (JSProcess*)page; JSIPCCommand c; memset(&c, 0, sizeof c); c.op = JSC_ANIM_EVENTS;
    if (!js_proxy_async_has_op(p, JSC_ANIM_EVENTS)) (void)js_proxy_async_enqueue(p, &c);
#endif
}
#endif
