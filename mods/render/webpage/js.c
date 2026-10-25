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

void* webpage_js_attach(DOMNode* head, const char* url, PageBundle* bundle);
void  webpage_js_free(void* page);
int   webpage_js_tick(void* page);
int   webpage_js_dispatch(void* page, DOMNode* target, const char* type, const char* key, int mods);
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
int   webpage_js_dispatch(void* p, DOMNode* t, const char* ty, const char* k, int m) { (void)p; (void)t; (void)ty; (void)k; (void)m; return 0; }
int   webpage_js_before_submit(void* p, DOMNode* c) { (void)p; (void)c; return 1; }
const char* webpage_js_take_nav(void* p) { (void)p; return NULL; }
int   webpage_js_take_scroll(void* p, int* x, int* y) { (void)p; (void)x; (void)y; return 0; }
int   webpage_js_take_focus(void* p, DOMNode** n) { (void)p; (void)n; return 0; }
void  webpage_js_set_focus(void* p, DOMNode* n) { (void)p; (void)n; }
const char* webpage_js_take_title(void* p) { (void)p; return NULL; }
#else

#include "quickjs.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#endif
#include "ws.h"
#include "ttf_render.h"
#include "font_manager.h"
#include "worker.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_write.h"
extern const char* webpage_js_prelude;
int webpage_form_url(DOMNode* control, DOMNode* submitter, char* out, int cap);
int css_media_matches(const char* query);

typedef struct { int parent, first, last, next, prev; } Lk;
typedef struct Timer { int id; JSValue fn; double due, interval; int repeat; struct Timer* next; } Timer;
typedef struct AReq { int id, used; NalAsyncReq nr; struct AReq* next; } AReq;
typedef struct WConn { int id, used; WsConn ws; struct WConn* next; } WConn;
typedef struct WkConn { int id, used; WorkerConn wk; struct WkConn* next; } WkConn;
typedef struct Page {
    JSContext* ctx; DOMNode* head; PageBundle* bundle; char url[1100];
    DOMNode** nodes; Lk* lk; int nn, ncap;
    int lin_dirty, style_dirty; void* doc; int vw_seen, vh_seen;
    Timer* timers; int next_timer;
    char nav[1200]; int has_nav; int scroll_req, sx, sy; int focus_req, focus_id; char title[256]; int title_dirty;
    int cur_script; int active;
    AReq* areqs; int next_areq;
    WConn* wconns; int next_wconn;
    WkConn* wkconns; int next_wkconn;
} Page;

static JSRuntime* g_rt; static double g_deadline;

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
    if (ss) JS_FreeCString(c, ss); if (s) JS_FreeCString(c, s); JS_FreeValue(c, st); JS_FreeValue(c, ex);
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
    DOMNode* src = p->nodes[id]; DOMNode* n = malloc(sizeof(DOMNode)); memcpy(n, src, sizeof(DOMNode)); n->next = NULL; n->aux = NULL; n->pb[0] = n->pb[1] = NULL; int nid = new_id(p, n);
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
    if (!strcmp(k, "value")) r = JS_NewString(c, n->value); else if (!strcmp(k, "checked")) r = JS_NewBool(c, n->checked); else if (!strcmp(k, "disabled")) r = JS_NewBool(c, n->disabled);
    else if (!strcmp(k, "selected")) r = JS_NewBool(c, n->selected); else if (!strcmp(k, "readonly")) r = JS_NewBool(c, n->readonly); else if (!strcmp(k, "multiple")) r = JS_NewBool(c, n->multiple);
    else if (!strcmp(k, "type")) r = JS_NewString(c, n->input_type); else if (!strcmp(k, "name")) r = JS_NewString(c, n->name); else if (!strcmp(k, "placeholder")) r = JS_NewString(c, n->placeholder);
    else if (!strcmp(k, "href")) r = JS_NewString(c, n->href); else if (!strcmp(k, "src")) r = JS_NewString(c, n->src); else if (!strcmp(k, "alt")) r = JS_NewString(c, n->alt);
    JS_FreeCString(c, k); return r;
}
FN(d_setProp) {
    Page* p = P(c); NODE(0); const char* k = JS_ToCString(c, argv[1]); if (!k) return JS_EXCEPTION;
    if (!strcmp(k, "value")) { const char* v = JS_ToCString(c, argv[2]); if (v) { put(n->value, sizeof n->value, v); JS_FreeCString(c, v); } }
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
    else if (!strcmp(k, "text-align")) snprintf(v, sizeof v, "%s", st->text_align == ALIGN_CENTER ? "center" : st->text_align == ALIGN_RIGHT ? "right" : st->text_align == ALIGN_JUSTIFY ? "justify" : "left");
    else if (!strcmp(k, "overflow")) snprintf(v, sizeof v, "%s", st->overflow_hidden ? "hidden" : "visible"); else if (!strcmp(k, "cursor")) snprintf(v, sizeof v, "%s", st->pointer ? "pointer" : "auto");
    else if (!strcmp(k, "float")) snprintf(v, sizeof v, "%s", st->float_ == FLT_LEFT ? "left" : st->float_ == FLT_RIGHT ? "right" : "none");
    else if (!strcmp(k, "width") || !strcmp(k, "height")) { if (n->idx >= 0 && webpage_node_rect(n, r)) snprintf(v, sizeof v, "%gpx", k[0] == 'w' ? r[2] : r[3]); else strcpy(v, "auto"); }
    else if (!strncmp(k, "margin-", 7) || !strncmp(k, "padding-", 8)) { int pad = k[0] == 'p'; const char* sd = k + (pad ? 8 : 7); int i = !strcmp(sd, "top") ? 0 : !strcmp(sd, "right") ? 1 : !strcmp(sd, "bottom") ? 2 : 3; const Len* l = pad ? &st->padding[i] : &st->margin[i]; snprintf(v, sizeof v, "%gpx", l->px); }
    else if (!strncmp(k, "border-", 7) && strstr(k, "width")) { const char* sd = k + 7; int i = !strncmp(sd, "top", 3) ? 0 : !strncmp(sd, "right", 5) ? 1 : !strncmp(sd, "bottom", 6) ? 2 : 3; snprintf(v, sizeof v, "%gpx", st->border_w[i]); }
    else if (!strcmp(k, "line-height")) { if (st->lh_px > 0) snprintf(v, sizeof v, "%gpx", st->lh_px); else if (st->lh_mult > 0) snprintf(v, sizeof v, "%gpx", st->lh_mult * st->font_size); else strcpy(v, "normal"); }
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
    char* txt = webpage_fetch_text(ab); if (!txt) return JS_NULL; JSValue r = JS_NewString(c, txt); free(txt); return r;
}

/* ── асинхронная сеть (fetch/XHR любых методов, не блокирует движок) ──
   D.fetchStart(method,url,headersRaw,body|null[,credentials,mode]) -> id (int, -1 при ошибке запуска); body — ArrayBuffer (или строка);
   D.fetchPoll(id) -> null пока не готово, иначе {status, headers, body: ArrayBuffer};
   D.fetchCancel(id) — отменить (например AbortController). */
FN(d_fetchStart) {
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
    NalFetchOpts fo; fo.page_url = p->url; fo.credentials = 1; fo.mode = 0;
    if (argc > 4 && !JS_IsUndefined(argv[4])) fo.credentials = arg_int(c, argv[4]);
    if (argc > 5 && !JS_IsUndefined(argv[5])) fo.mode = arg_int(c, argv[5]);
    { const char* col = strchr(ab, ':'); const char* sl = strchr(ab, '/'); if (!(col && (!sl || col < sl) && strncmp(ab, "http", 4) != 0)) ok = nal_async_start_ex(method, ab, headers, body, blen, &fo, &nr); }
    JS_FreeCString(c, method); JS_FreeCString(c, url); if (headers) JS_FreeCString(c, headers); if (body_str) JS_FreeCString(c, body_str);
    if (!ok) return JS_NewInt32(c, -1);
    AReq* a = calloc(1, sizeof *a); a->id = ++p->next_areq; a->used = 1; a->nr = nr; a->next = p->areqs; p->areqs = a;
    return JS_NewInt32(c, a->id);
}
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

/* ── Web Worker: полноценный процесс со своим JSRuntime (mods/render/webpage/worker.c) — не поток,
   изоляция через fork(), сообщения — JSON через пайпы. См. worker.h про ограничения. ── */
FN(d_workerStart) {
    Page* p = P(c); const char* s = JS_ToCString(c, argv[0]); if (!s) return JS_NewInt32(c, -1);
    char ab[1100]; webpage_resolve_url(p->url, s, ab, sizeof ab); JS_FreeCString(c, s);
    WorkerConn wc; int ok = worker_start(ab, &wc);
    if (!ok) return JS_NewInt32(c, -1);
    WkConn* w = calloc(1, sizeof *w); w->id = ++p->next_wkconn; w->used = 1; w->wk = wc; w->next = p->wkconns; p->wkconns = w;
    return JS_NewInt32(c, w->id);
}
FN(d_workerPoll) {
    Page* p = P(c); int id = arg_int(c, argv[0]);
    for (WkConn* w = p->wkconns; w; w = w->next) if (w->id == id && w->used) {
        char type = 0; char* text = NULL;
        if (!worker_poll(&w->wk, &type, &text)) return JS_NULL;
        if (!w->wk.used) w->used = 0;
        JSValue o = JS_NewObject(c); char ts[2] = { type, 0 }; JS_SetPropertyStr(c, o, "type", JS_NewString(c, ts));
        if (text) { JS_SetPropertyStr(c, o, "data", JS_NewString(c, text)); free(text); }
        return o;
    }
    return JS_NULL;
}
FN(d_workerPost) { Page* p = P(c); int id = arg_int(c, argv[0]); const char* s = JS_ToCString(c, argv[1]); if (s) { for (WkConn* w = p->wkconns; w; w = w->next) if (w->id == id && w->used) worker_post(&w->wk, s); JS_FreeCString(c, s); } return JS_UNDEFINED; }
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
    Page* p = P(c); NODE(0);
    size_t bsz = 0; uint8_t* rgba = JS_GetArrayBuffer(c, &bsz, argv[1]);
    int w = arg_int(c, argv[2]), h = arg_int(c, argv[3]);
    if (rgba && w > 0 && h > 0 && bsz >= (size_t)w * h * 4) {
        uint32_t* words = malloc((size_t)w * h * sizeof(uint32_t));
        if (words) {
            for (long i = 0; i < (long)w * h; i++) words[i] = ((uint32_t)rgba[i * 4 + 3] << 24) | ((uint32_t)rgba[i * 4] << 16) | ((uint32_t)rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2];
            char key[32]; snprintf(key, sizeof key, "canvas:%d", n->idx);
            webpage_image_inject(key, words, w, h);
            free(words);
        }
    }
    (void)p; return JS_UNDEFINED;
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
FN(d_submit) { Page* p = P(c); NODE(0); char u[1200]; if (webpage_form_url(n, NULL, u, sizeof u)) { snprintf(p->nav, sizeof p->nav, "%s", u); p->has_nav = 1; } return JS_UNDEFINED; }
FN(d_focus) { Page* p = P(c); p->focus_req = 1; p->focus_id = arg_int(c, argv[0]); if (p->focus_id >= 0) p->active = p->focus_id; else p->active = -1; return JS_UNDEFINED; }
FN(d_active) { return JS_NewInt32(c, P(c)->active); }
FN(d_curScript) { return JS_NewInt32(c, P(c)->cur_script); }

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
    DEF(query), DEF(matches), DEF(closest), DEF(byId), DEF(root), DEF(special), DEF(rect), DEF(computed), DEF(mediaMatches), DEF(viewport), DEF(scrollTo), DEF(getScroll), DEF(setScroll),
    DEF(title), DEF(setTitle), DEF(url), DEF(nav), DEF(setUrl), DEF(fetch), DEF(log), DEF(now), DEF(submit), DEF(focus), DEF(active), DEF(curScript), DEF(timerAdd), DEF(timerClear),
    DEF(fetchStart), DEF(fetchPoll), DEF(fetchCancel), DEF(utf8Decode), DEF(utf8Encode), DEF(lsList), DEF(lsSet), DEF(lsRemove), DEF(lsClear),
    DEF(idbMetaList), DEF(idbMetaGet), DEF(idbMetaSet), DEF(idbDeleteDatabase), DEF(idbList), DEF(idbGet), DEF(idbPut), DEF(idbRemove), DEF(idbClear),
    DEF(swList), DEF(swSet), DEF(swRemove),
    DEF(profileId),
    DEF(wsConnect), DEF(wsPoll), DEF(wsSend), DEF(wsClose),
    DEF(workerStart), DEF(workerPoll), DEF(workerPost), DEF(workerTerminate),
    DEF(canvasMeasureText), DEF(canvasText), DEF(imageDecode), DEF(canvasCommit), DEF(canvasToDataURL)
};

/* ══════════════ модули ES ══════════════ */
static char* mod_normalize(JSContext* c, const char* base, const char* name, void* op) {
    (void)op; char out[1200]; webpage_resolve_url(base, name, out, sizeof out); return js_strdup(c, out[0] ? out : name);
}
static JSModuleDef* mod_load(JSContext* c, const char* name, void* op) {
    (void)op; char* src = webpage_fetch_text(name);
    if (!src) { JS_ThrowReferenceError(c, "could not load module '%s'", name); return NULL; }
    JSValue v = JS_Eval(c, src, strlen(src), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY); free(src);
    if (JS_IsException(v)) return NULL;
    JSModuleDef* m = JS_VALUE_GET_PTR(v); JS_FreeValue(c, v); return m;
}

static void eval_script(Page* p, const char* code, const char* name, int module) {
    g_deadline = now_ms() + 5000;
    JSValue r = JS_Eval(p->ctx, code, strlen(code), name, module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) log_exception(p->ctx, name); JS_FreeValue(p->ctx, r);
    run_jobs(); g_deadline = 0;
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
    for (DOMNode* q = head; q; q = q->next) new_id(p, q);
    for (int i = 0; i < p->nn; i++) { DOMNode* q = p->nodes[i]; int par = q->parent; if (par >= 0 && par < p->nn) link_before(p, par, i, -1); }
    JSContext* c = p->ctx; JSValue g = JS_GetGlobalObject(c); JSValue dom = JS_NewObject(c);
    for (size_t i = 0; i < sizeof DOMFN / sizeof DOMFN[0]; i++) JS_SetPropertyStr(c, dom, DOMFN[i].name, JS_NewCFunction(c, DOMFN[i].fn, DOMFN[i].name, 0));
    JS_SetPropertyStr(c, g, "__dom", dom); JS_FreeValue(c, g);
    eval_script(p, webpage_js_prelude, "<prelude>", 0);
    if (bundle) for (int i = 0; i < bundle->nscripts; i++) {
        ScriptSrc* s = &bundle->scripts[i]; if (!s->text) continue;
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
    webdoc_free(p->doc); JS_FreeContext(p->ctx); if (g_rt) JS_RunGC(g_rt);
    for (int i = 0; i < p->nn; i++) if (p->nodes[i]->idx < 0 && p->nodes[i] != p->head) { dom_free_pseudo(p->nodes[i]); free(p->nodes[i]); }       /* отсоединённые узлы */
    for (DOMNode* q = p->head; q; q = q->next) q->aux = NULL;
    free(p->nodes); free(p->lk); webpage_bundle_free(p->bundle); free(p);
}

int js_local_tick(void* pp) {
    Page* p = pp; if (!p) return 0; int did = 0;
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

int js_local_dispatch(void* pp, DOMNode* target, const char* type, const char* key, int mods) {
    Page* p = pp; if (!p || !target || target->aux == NULL) return 0; JSContext* c = p->ctx;
    JSValue g = JS_GetGlobalObject(c); JSValue fn = JS_GetPropertyStr(c, g, "__dispatch"); int prevented = 0;
    if (JS_IsFunction(c, fn)) {
        JSValue a[4] = { JS_NewInt32(c, id_of(target)), JS_NewString(c, type), JS_NewString(c, key ? key : ""), JS_NewInt32(c, mods) };
        g_deadline = now_ms() + 3000; JSValue r = JS_Call(c, fn, JS_UNDEFINED, 4, a);
        if (JS_IsException(r)) log_exception(c, type); else prevented = JS_ToBool(c, r) > 0;
        JS_FreeValue(c, r); for (int i = 0; i < 4; i++) JS_FreeValue(c, a[i]); run_jobs(); g_deadline = 0;
    }
    JS_FreeValue(c, fn); JS_FreeValue(c, g); return prevented;
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
int js_local_take_scroll(void* pp, int* x, int* y) { Page* p = pp; if (!p || !p->scroll_req) return 0; p->scroll_req = 0; *x = p->sx; *y = p->sy; return 1; }
int js_local_take_focus(void* pp, DOMNode** node) { Page* p = pp; if (!p || !p->focus_req) return 0; p->focus_req = 0; *node = p->focus_id >= 0 ? N(p, p->focus_id) : NULL; return 1; }
void js_local_set_focus(void* pp, DOMNode* node) { Page* p = pp; if (p) p->active = node ? id_of(node) : -1; }
const char* js_local_take_title(void* pp) { Page* p = pp; if (!p || !p->title_dirty) return NULL; p->title_dirty = 0; return p->title; }

/* ── Process-isolated page JS ──────────────────────────────────────────────
 * Each document gets its own QuickJS process. The UI process owns its DOM nodes;
 * mutations are returned as pointer-free node snapshots over a pipe. This keeps
 * QuickJS heaps/timers/worker connections out of the UI process without passing
 * raw pointers across the process boundary.
 */
#ifndef _WIN32
enum { JSC_TICK=1, JSC_DISPATCH, JSC_SUBMIT, JSC_TAKE_NAV, JSC_TAKE_SCROLL,
       JSC_TAKE_FOCUS, JSC_SET_FOCUS, JSC_TAKE_TITLE, JSC_RESTYLE,
       JSC_ANIM_EVENTS, JSC_FREE };
#define JSIPC_MAGIC 0x4a535031u /* JSP1 */
typedef struct {
    uint32_t magic;
    int32_t op, id, alt_id, mods, flags;
    int32_t checked, selected, disabled, readonly;
    char type[32];
    char key[128];
    char value[2048];
} JSIPCCommand;
typedef struct {
    uint32_t magic;
    int32_t status, result, x, y, focus_id, dyn_pending;
    uint32_t node_count;
    uint32_t scroll_count;
    char text[1200];
} JSIPCReply;
typedef struct {
    DOMNode node;                 /* pointers next/aux/pb[] are zeroed before send */
    uint8_t has_pb0, has_pb1;
} JSNodeWireHead;
typedef struct {
    uint32_t magic;
    pid_t pid;
    int cmd_fd, reply_fd, dead;
    DOMNode* head;
    DOMNode** nodes_by_id;
    int node_cap;
    char text[1200];
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
static void js_ipc_write_snapshot(int fd, Page* p, JSIPCReply* r) {
    r->node_count = 0;
    if (!p) return;
    /* Keep indices and styles coherent even when a script only changed structure. */
    flush_style(p);
    r->node_count = (uint32_t)p->nn;
    for (int i = 0; i < p->nn; i++) {
        DOMNode* n = p->nodes[i]; JSNodeWireHead h; memset(&h, 0, sizeof h);
        memcpy(&h.node, n, sizeof(DOMNode));
        h.node.next = NULL; h.node.aux = NULL; h.node.pb[0] = h.node.pb[1] = NULL;
        h.has_pb0 = n->pb[0] != NULL; h.has_pb1 = n->pb[1] != NULL;
        if (!js_ipc_write_all(fd, &h, sizeof h)) return;
        if (h.has_pb0 && !js_ipc_write_all(fd, n->pb[0], sizeof(PseudoBox))) return;
        if (h.has_pb1 && !js_ipc_write_all(fd, n->pb[1], sizeof(PseudoBox))) return;
    }
}
static int js_ipc_send_reply(int fd, JSIPCReply* r, Page* p, int snapshot) {
    r->magic = JSIPC_MAGIC;
    r->node_count = r->scroll_count = 0;
    if (snapshot && p) {
        /* Flush before the header so node_count and layout state are exact. */
        flush_style(p);
        r->node_count = (uint32_t)p->nn;
        int sc = l2_scroll_state_export(p->head, NULL, 0);
        if (sc > 0 && sc <= 1000000) r->scroll_count = (uint32_t)sc;
    }
    if (!js_ipc_write_all(fd, r, sizeof *r)) return 0;
    if (snapshot && p) {
        js_ipc_write_snapshot(fd, p, r);
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
    return 1;
}
static void js_ipc_child_loop(int cmd_fd, int reply_fd, Page* page) {
    for (;;) {
        JSIPCCommand c; memset(&c, 0, sizeof c);
        if (!js_ipc_read_all(cmd_fd, &c, sizeof c) || c.magic != JSIPC_MAGIC) break;
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
                /* Keep DOM snapshots near a display-frame cadence instead of
                   copying the whole DOM on every timer tick. */
                if (anim_events || (anim_running && webpage_anim_tick())) r.result = 1;
                snapshot = r.result != 0;
                break;
            }
            case JSC_DISPATCH:
                if (target && (c.flags & 1)) {
                    snprintf(target->value, sizeof target->value, "%s", c.value);
                    target->checked = (uint8_t)!!c.checked; target->selected = (uint8_t)!!c.selected;
                    target->disabled = (uint8_t)!!c.disabled; target->readonly = (uint8_t)!!c.readonly;
                }
                r.result = js_local_dispatch(page, target, c.type, c.key, c.mods);
                snapshot = 1;
                break;
            case JSC_SUBMIT:
                if (target && (c.flags & 1)) {
                    snprintf(target->value, sizeof target->value, "%s", c.value);
                    target->checked = (uint8_t)!!c.checked; target->selected = (uint8_t)!!c.selected;
                    target->disabled = (uint8_t)!!c.disabled; target->readonly = (uint8_t)!!c.readonly;
                }
                r.result = js_local_before_submit(page, target); snapshot = 1;
                break;
            case JSC_TAKE_NAV: {
                const char* v = js_local_take_nav(page); if (v) snprintf(r.text, sizeof r.text, "%s", v); r.result = v != NULL; break;
            }
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
            case JSC_FREE:
                js_local_free(page); page = NULL; r.status = 1;
                js_ipc_send_reply(reply_fd, &r, NULL, 0);
                close(cmd_fd); close(reply_fd); _exit(0);
            default: r.status = 0; break;
        }
        r.dyn_pending = webpage_dyn_pending();
        if (r.dyn_pending) webpage_dyn_clear_pending();
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
static int js_proxy_receive_snapshot(JSProcess* p, const JSIPCReply* r) {
    if (!r->node_count) {
        if (r->scroll_count) {
            if (r->scroll_count > 1000000) return 0;
            size_t bytes = (size_t)r->scroll_count * sizeof(L2ScrollState);
            L2ScrollState* state = malloc(bytes); if (!state) return 0;
            int ok = js_ipc_read_all(p->reply_fd, state, bytes);
            if (ok && p->head) l2_scroll_state_import(state, (int)r->scroll_count, p->head);
            free(state); return ok;
        }
        return 1;
    }
    if (!js_proxy_grow_nodes(p, r->node_count)) return 0;
    int max_idx = -1;
    /* First pass: read each record, update stable parent-owned node allocations. */
    for (uint32_t i = 0; i < r->node_count; i++) {
        JSNodeWireHead h; PseudoBox b0, b1;
        if (!js_ipc_read_all(p->reply_fd, &h, sizeof h)) return 0;
        if (h.has_pb0 && !js_ipc_read_all(p->reply_fd, &b0, sizeof b0)) return 0;
        if (h.has_pb1 && !js_ipc_read_all(p->reply_fd, &b1, sizeof b1)) return 0;
        DOMNode* n = p->nodes_by_id[i];
        if (!n) { n = calloc(1, sizeof *n); if (!n) return 0; p->nodes_by_id[i] = n; }
        dom_free_pseudo(n);
        memcpy(n, &h.node, sizeof *n); n->next = NULL; n->aux = NULL; n->pb[0] = n->pb[1] = NULL;
        if (h.has_pb0) { n->pb[0] = malloc(sizeof(PseudoBox)); if (!n->pb[0]) return 0; memcpy(n->pb[0], &b0, sizeof b0); }
        if (h.has_pb1) { n->pb[1] = malloc(sizeof(PseudoBox)); if (!n->pb[1]) return 0; memcpy(n->pb[1], &b1, sizeof b1); }
        if (n->idx > max_idx) max_idx = n->idx;
    }
    /* Rebuild the renderer's preorder linked list without leaking child-process pointers. */
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
        if (r->scroll_count > 1000000) return 0;
        size_t bytes = (size_t)r->scroll_count * sizeof(L2ScrollState);
        L2ScrollState* state = malloc(bytes); if (!state) return 0;
        int ok = js_ipc_read_all(p->reply_fd, state, bytes);
        if (ok && p->head) l2_scroll_state_import(state, (int)r->scroll_count, p->head);
        free(state); if (!ok) return 0;
    }
    webpage_invalidate_layout();
    return 1;
}
static int js_proxy_call(JSProcess* p, JSIPCCommand* c, JSIPCReply* r, int* sync_dom) {
    if (sync_dom) *sync_dom = 0;
    if (!p || p->magic != JSIPC_MAGIC || p->dead) return 0;
    c->magic = JSIPC_MAGIC;
    if (!js_ipc_write_all(p->cmd_fd, c, sizeof *c) || !js_ipc_read_all(p->reply_fd, r, sizeof *r) || r->magic != JSIPC_MAGIC) {
        p->dead = 1; return 0;
    }
    if (r->node_count) {
        if (!js_proxy_receive_snapshot(p, r)) { p->dead = 1; return 0; }
        if (sync_dom) *sync_dom = 1;
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
    c->disabled = n->disabled; c->readonly = n->readonly;
    snprintf(c->value, sizeof c->value, "%s", n->value);
}
static void js_proxy_free(JSProcess* p) {
    if (!p) return;
    if (!p->dead) { JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_FREE; (void)js_proxy_call(p,&c,&r,NULL); }
    if (p->cmd_fd >= 0) close(p->cmd_fd); if (p->reply_fd >= 0) close(p->reply_fd);
    if (p->pid > 0) { int st; if (p->dead) kill(p->pid, SIGTERM); pid_t w; do { w = waitpid(p->pid, &st, 0); } while (w < 0 && errno == EINTR); }
    /* Active nodes are owned by the DOM linked list; detached JS-created nodes are not. */
    for (int i = 0; i < p->node_cap; i++) { DOMNode* n = p->nodes_by_id[i]; if (n && n->idx < 0 && n != p->head) { dom_free_pseudo(n); free(n); } }
    free(p->nodes_by_id); p->magic = 0; free(p);
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
    if (!js_ipc_read_all(p->reply_fd, &r, sizeof r) || r.magic != JSIPC_MAGIC || !r.status ||
        (r.node_count && !js_proxy_receive_snapshot(p, &r))) {
        p->dead = 1; js_proxy_free(p); return NULL;
    }
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
    return js_local_tick(page);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_TICK; return js_proxy_call(p,&c,&r,NULL) ? r.result : 0;
#endif
}
int webpage_js_dispatch(void* page, DOMNode* target, const char* type, const char* key, int mods) {
#ifdef _WIN32
    return js_local_dispatch(page,target,type,key,mods);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_DISPATCH; c.id=js_proxy_id(p,target); c.mods=mods; snprintf(c.type,sizeof c.type,"%s",type?type:""); snprintf(c.key,sizeof c.key,"%s",key?key:""); js_proxy_target_state(&c,target); return js_proxy_call(p,&c,&r,NULL) ? r.result : 0;
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
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_TAKE_NAV; if (!js_proxy_call(p,&c,&r,NULL) || !r.result) return NULL; snprintf(p->text,sizeof p->text,"%s",r.text); return p->text;
#endif
}
int webpage_js_take_scroll(void* page, int* x, int* y) {
#ifdef _WIN32
    return js_local_take_scroll(page,x,y);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_TAKE_SCROLL; if (!js_proxy_call(p,&c,&r,NULL) || !r.result) return 0; if(x)*x=r.x; if(y)*y=r.y; return 1;
#endif
}
int webpage_js_take_focus(void* page, DOMNode** node) {
#ifdef _WIN32
    return js_local_take_focus(page,node);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_TAKE_FOCUS; if (!js_proxy_call(p,&c,&r,NULL) || !r.result) return 0; if(node)*node=(r.focus_id>=0 && r.focus_id<p->node_cap)?p->nodes_by_id[r.focus_id]:NULL; return 1;
#endif
}
void webpage_js_set_focus(void* page, DOMNode* node) {
#ifdef _WIN32
    js_local_set_focus(page,node);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_SET_FOCUS; c.id=js_proxy_id(p,node); (void)js_proxy_call(p,&c,&r,NULL);
#endif
}
const char* webpage_js_take_title(void* page) {
#ifdef _WIN32
    return js_local_take_title(page);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_TAKE_TITLE; if (!js_proxy_call(p,&c,&r,NULL) || !r.result) return NULL; snprintf(p->text,sizeof p->text,"%s",r.text); return p->text;
#endif
}
void webpage_js_restyle(void* page) {
#ifdef _WIN32
    js_local_restyle(page);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_RESTYLE; c.id=js_proxy_id(p, webpage_hover_node()); c.alt_id=js_proxy_id(p, webpage_active_node()); (void)js_proxy_call(p,&c,&r,NULL); webpage_dyn_clear_pending();
#endif
}
void webpage_js_anim_events(void* page) {
#ifdef _WIN32
    js_local_anim_events(page);
#else
    JSProcess* p=page; JSIPCCommand c; JSIPCReply r; memset(&c,0,sizeof c); c.op=JSC_ANIM_EVENTS; (void)js_proxy_call(p,&c,&r,NULL);
#endif
}
#endif
