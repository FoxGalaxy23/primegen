/* transition и @keyframes-анимации.
   Вычисленный стиль узла (n->st) после каскада — «базовый». Поверх него anim_step() каждый кадр накладывает
   значения бегущих переходов и анимаций (n->st меняется на месте; следующий каскад снова даёт базу).
   Переход стартует в anim_cascade_end(), если базовое значение свойства изменилось, а у нового стиля есть transition для него:
   «от» берётся из видимого (наложенного) значения, сохранённого в anim_before_node(). */
#include "fx.h"
#include "gal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <strings.h>
#include <stddef.h>
#include <time.h>

double anim_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6; }

/* ══════════════ имена свойств → маска ══════════════ */
static const struct { const char* n; uint64_t m; } PN[] = {
    {"opacity", APM(AP_OPACITY)}, {"color", APM(AP_COLOR)}, {"background-color", APM(AP_BG)}, {"background", APM(AP_BG) | APM(AP_GRAD) | APM(AP_BGPOS)},
    {"background-image", APM(AP_GRAD)}, {"background-position", APM(AP_BGPOS)},
    {"border-color", APM(AP_BC0) | APM(AP_BC1) | APM(AP_BC2) | APM(AP_BC3)}, {"border-top-color", APM(AP_BC0)}, {"border-right-color", APM(AP_BC1)}, {"border-bottom-color", APM(AP_BC2)}, {"border-left-color", APM(AP_BC3)},
    {"border", APM(AP_BC0) | APM(AP_BC1) | APM(AP_BC2) | APM(AP_BC3) | APM(AP_BW0) | APM(AP_BW1) | APM(AP_BW2) | APM(AP_BW3)},
    {"border-width", APM(AP_BW0) | APM(AP_BW1) | APM(AP_BW2) | APM(AP_BW3)}, {"border-top-width", APM(AP_BW0)}, {"border-right-width", APM(AP_BW1)}, {"border-bottom-width", APM(AP_BW2)}, {"border-left-width", APM(AP_BW3)},
    {"width", APM(AP_WIDTH)}, {"height", APM(AP_HEIGHT)}, {"min-width", APM(AP_MINW)}, {"max-width", APM(AP_MAXW)}, {"min-height", APM(AP_MINH)}, {"max-height", APM(AP_MAXH)},
    {"margin", APM(AP_MG0) | APM(AP_MG1) | APM(AP_MG2) | APM(AP_MG3)}, {"margin-top", APM(AP_MG0)}, {"margin-right", APM(AP_MG1)}, {"margin-bottom", APM(AP_MG2)}, {"margin-left", APM(AP_MG3)},
    {"padding", APM(AP_PD0) | APM(AP_PD1) | APM(AP_PD2) | APM(AP_PD3)}, {"padding-top", APM(AP_PD0)}, {"padding-right", APM(AP_PD1)}, {"padding-bottom", APM(AP_PD2)}, {"padding-left", APM(AP_PD3)},
    {"inset", APM(AP_IN0) | APM(AP_IN1) | APM(AP_IN2) | APM(AP_IN3)}, {"top", APM(AP_IN0)}, {"right", APM(AP_IN1)}, {"bottom", APM(AP_IN2)}, {"left", APM(AP_IN3)},
    {"border-radius", APM(AP_RAD0) | APM(AP_RAD1) | APM(AP_RAD2) | APM(AP_RAD3)}, {"border-top-left-radius", APM(AP_RAD0)}, {"border-top-right-radius", APM(AP_RAD1)}, {"border-bottom-right-radius", APM(AP_RAD2)}, {"border-bottom-left-radius", APM(AP_RAD3)},
    {"font-size", APM(AP_FONTSZ)}, {"line-height", APM(AP_LH)}, {"box-shadow", APM(AP_SHADOW)}, {"flex-grow", APM(AP_FLEXG)}, {"flex", APM(AP_FLEXG)},
    {"gap", APM(AP_GAPR) | APM(AP_GAPC)}, {"row-gap", APM(AP_GAPR)}, {"column-gap", APM(AP_GAPC)}, {"grid-gap", APM(AP_GAPR) | APM(AP_GAPC)},
    {"filter", APM(AP_FILTER)}, {"backdrop-filter", APM(AP_BACKDROP)}, {"clip-path", APM(AP_CLIP)}, {"transform", APM(AP_XFORM)}, {"transform-origin", APM(AP_ORIGIN)},
    {NULL, 0}
};
uint64_t fx_prop_mask(const char* name, size_t n) {
    while (n && isspace((unsigned char)*name)) { name++; n--; } while (n && isspace((unsigned char)name[n - 1])) n--;
    if (n > 8 && name[0] == '-') { const char* d = memchr(name + 1, '-', n - 1); if (d) { n -= (size_t)(d + 1 - name); name = d + 1; } }
    if (n == 3 && !strncasecmp(name, "all", 3)) return AP_ALL_MASK;
    for (int i = 0; PN[i].n; i++) if (strlen(PN[i].n) == n && !strncasecmp(PN[i].n, name, n)) return PN[i].m;
    return 0;
}

int fx_style_has_fx(const NodeStyle* st) {
    if (st->filter.n || st->backdrop.n || st->clip.kind || st->mask.kind || st->xf.n) return 1;
    if (st->an_n && st->an[0].name[0]) return 1;
    for (int i = 0; i < st->tr_n; i++) if (st->tr[i].mask & AP_FX_MASK) return 1;
    return 0;
}

/* ══════════════ временные функции ══════════════ */
static float bez(float a, float b, float c, float d, float x) {            /* cubic-bezier(a,b,c,d) в точке x */
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 20; i++) {
        float cx = 3 * a * t * (1 - t) * (1 - t) + 3 * c * t * t * (1 - t) + t * t * t;
        if (fabsf(cx - x) < 1e-5f) break;
        if (cx < x) lo = t; else hi = t; t = (lo + hi) * 0.5f;
    }
    return 3 * b * t * (1 - t) * (1 - t) + 3 * d * t * t * (1 - t) + t * t * t;
}
float fx_ease(const FxEase* e, float t) {
    if (t <= 0) return 0; if (t >= 1) return 1;
    if (e->kind == 2) return t;
    if (e->kind == 1) { float n = e->a < 1 ? 1 : e->a; float s = e->b > 0.5f ? ceilf(t * n) : floorf(t * n); float r = s / n; return r > 1 ? 1 : r; }
    return bez(e->a, e->b, e->c, e->d, t);
}

/* ══════════════ свойства: сравнение / копирование / интерполяция ══════════════ */
enum { K_FLOAT = 1, K_COLOR, K_LEN, K_SPECIAL };
typedef struct { uint8_t kind; uint16_t off; } PD;
#define F(f) {K_FLOAT, (uint16_t)offsetof(NodeStyle, f)}
#define C(f) {K_COLOR, (uint16_t)offsetof(NodeStyle, f)}
#define L(f) {K_LEN, (uint16_t)offsetof(NodeStyle, f)}
#define S {K_SPECIAL, 0}
static const PD PDT[AP_COUNT] = {
    [AP_OPACITY] = F(op_own), [AP_COLOR] = C(color), [AP_BG] = C(bg), [AP_BC0] = C(border_c[0]), [AP_BC1] = C(border_c[1]), [AP_BC2] = C(border_c[2]), [AP_BC3] = C(border_c[3]),
    [AP_WIDTH] = L(width), [AP_HEIGHT] = L(height), [AP_MINW] = L(min_w), [AP_MAXW] = L(max_w), [AP_MINH] = L(min_h), [AP_MAXH] = L(max_h),
    [AP_MG0] = L(margin[0]), [AP_MG1] = L(margin[1]), [AP_MG2] = L(margin[2]), [AP_MG3] = L(margin[3]),
    [AP_PD0] = L(padding[0]), [AP_PD1] = L(padding[1]), [AP_PD2] = L(padding[2]), [AP_PD3] = L(padding[3]),
    [AP_IN0] = L(inset[0]), [AP_IN1] = L(inset[1]), [AP_IN2] = L(inset[2]), [AP_IN3] = L(inset[3]),
    [AP_BW0] = F(border_w[0]), [AP_BW1] = F(border_w[1]), [AP_BW2] = F(border_w[2]), [AP_BW3] = F(border_w[3]),
    [AP_RAD0] = F(radius[0]), [AP_RAD1] = F(radius[1]), [AP_RAD2] = F(radius[2]), [AP_RAD3] = F(radius[3]),
    [AP_FONTSZ] = F(font_size), [AP_SHADOW] = S, [AP_FLEXG] = F(flex_grow), [AP_GAPR] = F(gap_r), [AP_GAPC] = F(gap_c), [AP_LH] = F(lh_px),
    [AP_FILTER] = S, [AP_BACKDROP] = S, [AP_CLIP] = S, [AP_XFORM] = S, [AP_ORIGIN] = S, [AP_BGPOS] = S, [AP_GRAD] = S,
};
#undef F
#undef C
#undef L
#undef S
#define PF(st, o) ((float*)((char*)(st) + (o)))
#define PC(st, o) ((uint32_t*)((char*)(st) + (o)))
#define PL(st, o) ((Len*)((char*)(st) + (o)))

static float lf(float a, float b, float t) { return a + (b - a) * t; }
static uint32_t lcolor(uint32_t a, uint32_t b, float t) {
    int aa = a >> 24, ba = b >> 24;
    uint32_t ca = a, cb = b;
    if (aa == 0) ca = (b & 0xFFFFFF); if (ba == 0) cb = (a & 0xFFFFFF);          /* прозрачный → цвет другой стороны (без «серого» провала) */
    float A = lf((float)aa, (float)ba, t);
    uint32_t r = (uint32_t)(lf((float)((ca >> 16) & 255), (float)((cb >> 16) & 255), t) + 0.5f), g = (uint32_t)(lf((float)((ca >> 8) & 255), (float)((cb >> 8) & 255), t) + 0.5f), bl = (uint32_t)(lf((float)(ca & 255), (float)(cb & 255), t) + 0.5f);
    return ((uint32_t)(A + 0.5f) << 24) | (r << 16) | (g << 8) | bl;
}
static int len_lerpable(const Len* a, const Len* b) { return a->set && b->set; }
static void llen(Len* d, const Len* a, const Len* b, float t) {
    if (!len_lerpable(a, b)) { *d = t < 0.5f ? *a : *b; return; }
    d->px = lf(a->px, b->px, t); d->pct = lf(a->pct, b->pct, t); d->vw = lf(a->vw, b->vw, t); d->vh = lf(a->vh, b->vh, t); d->set = 1;
}
static void lfxlen(FxLen* d, const FxLen* a, const FxLen* b, float t) { d->px = lf(a->px, b->px, t); d->pct = lf(a->pct, b->pct, t); }

static FxOp op_identity(const FxOp* like) {
    FxOp o; memset(&o, 0, sizeof o); o.kind = like->kind;
    if (o.kind == FXO_BRIGHT || o.kind == FXO_CONTRAST || o.kind == FXO_SAT || o.kind == FXO_OPACITY) o.v = 1;
    if (o.kind == FXO_SHADOW) o.color = like->color & 0xFFFFFF;
    return o;
}
static void lfilter(FxList* d, const FxList* a, const FxList* b, float t) {
    int n = a->n > b->n ? a->n : b->n; FxList r; memset(&r, 0, sizeof r);
    for (int i = 0; i < n; i++) {
        FxOp x = i < a->n ? a->op[i] : op_identity(&b->op[i]), y = i < b->n ? b->op[i] : op_identity(&a->op[i]);
        if (x.kind != y.kind) { *d = t < 0.5f ? *a : *b; return; }
        FxOp o = x; o.v = lf(x.v, y.v, t); o.x = lf(x.x, y.x, t); o.y = lf(x.y, y.y, t); o.blur = lf(x.blur, y.blur, t); o.color = lcolor(x.color, y.color, t); r.op[i] = o;
    }
    r.n = (uint8_t)n; *d = r;
}
static FxXfOp xf_identity(const FxXfOp* like) { FxXfOp o; memset(&o, 0, sizeof o); o.kind = like->kind; if (o.kind == XF_SCALE) o.v[0] = o.v[1] = 1; if (o.kind == XF_MATRIX) o.v[0] = o.v[3] = 1; return o; }
static void lxform(FxXform* d, const FxXform* a, const FxXform* b, float t) {
    int n = a->n > b->n ? a->n : b->n; FxXform r = *a; r.n = 0;
    for (int i = 0; i < n; i++) {
        FxXfOp x = i < a->n ? a->op[i] : xf_identity(&b->op[i]), y = i < b->n ? b->op[i] : xf_identity(&a->op[i]);
        if (x.kind != y.kind) { d->n = (t < 0.5f ? a : b)->n; memcpy(d->op, (t < 0.5f ? a : b)->op, sizeof d->op); return; }
        FxXfOp o = x; for (int k = 0; k < 6; k++) o.v[k] = lf(x.v[k], y.v[k], t); lfxlen(&o.t[0], &x.t[0], &y.t[0], t); lfxlen(&o.t[1], &x.t[1], &y.t[1], t); r.op[i] = o;
    }
    r.n = (uint8_t)n; d->n = r.n; memcpy(d->op, r.op, sizeof d->op);
}
static void lclip(FxClip* d, const FxClip* a, const FxClip* b, float t) {
    if (a->kind != b->kind || a->kind == CP_NONE || (a->kind == CP_POLYGON && a->n != b->n)) { *d = t < 0.5f ? *a : *b; return; }
    FxClip r = *a;
    for (int i = 0; i < 4; i++) { lfxlen(&r.a[i], &a->a[i], &b->a[i], t); r.rad[i] = lf(a->rad[i], b->rad[i], t); }
    for (int i = 0; i < a->n; i++) { lfxlen(&r.pt[i][0], &a->pt[i][0], &b->pt[i][0], t); lfxlen(&r.pt[i][1], &a->pt[i][1], &b->pt[i][1], t); }
    *d = r;
}

static int len_eq(const Len* a, const Len* b) { return a->set == b->set && a->px == b->px && a->pct == b->pct && a->vw == b->vw && a->vh == b->vh; }
static int ap_eq(int p, const NodeStyle* a, const NodeStyle* b) {
    const PD* pd = &PDT[p];
    switch (pd->kind) {
    case K_FLOAT: return *PF(a, pd->off) == *PF(b, pd->off);
    case K_COLOR: return *PC(a, pd->off) == *PC(b, pd->off);
    case K_LEN: return len_eq(PL(a, pd->off), PL(b, pd->off));
    }
    switch (p) {
    case AP_SHADOW: return a->sh_x == b->sh_x && a->sh_y == b->sh_y && a->sh_blur == b->sh_blur && a->sh_spread == b->sh_spread && a->sh_color == b->sh_color;
    case AP_FILTER: return !memcmp(&a->filter, &b->filter, sizeof(FxList)) || (a->filter.n == b->filter.n && !memcmp(a->filter.op, b->filter.op, sizeof(FxOp) * a->filter.n));
    case AP_BACKDROP: return a->backdrop.n == b->backdrop.n && !memcmp(a->backdrop.op, b->backdrop.op, sizeof(FxOp) * a->backdrop.n);
    case AP_CLIP: return !memcmp(&a->clip, &b->clip, sizeof(FxClip));
    case AP_XFORM: return a->xf.n == b->xf.n && !memcmp(a->xf.op, b->xf.op, sizeof(FxXfOp) * a->xf.n);
    case AP_ORIGIN: return a->xf.ox.px == b->xf.ox.px && a->xf.ox.pct == b->xf.ox.pct && a->xf.oy.px == b->xf.oy.px && a->xf.oy.pct == b->xf.oy.pct;
    case AP_BGPOS: return len_eq(&a->bg_px, &b->bg_px) && len_eq(&a->bg_py, &b->bg_py);
    case AP_GRAD: return a->bg_grad_type == b->bg_grad_type && a->bg_grad_n == b->bg_grad_n && a->bg_grad_angle == b->bg_grad_angle && !memcmp(a->bg_grad_c, b->bg_grad_c, sizeof a->bg_grad_c) && !memcmp(a->bg_grad_p, b->bg_grad_p, sizeof a->bg_grad_p);
    }
    return 1;
}
static void ap_copy(int p, NodeStyle* d, const NodeStyle* s) {
    const PD* pd = &PDT[p];
    switch (pd->kind) {
    case K_FLOAT: *PF(d, pd->off) = *PF(s, pd->off); return;
    case K_COLOR: *PC(d, pd->off) = *PC(s, pd->off); return;
    case K_LEN: *PL(d, pd->off) = *PL(s, pd->off); return;
    }
    switch (p) {
    case AP_SHADOW: d->sh_x = s->sh_x; d->sh_y = s->sh_y; d->sh_blur = s->sh_blur; d->sh_spread = s->sh_spread; d->sh_color = s->sh_color; return;
    case AP_FILTER: d->filter = s->filter; return; case AP_BACKDROP: d->backdrop = s->backdrop; return; case AP_CLIP: d->clip = s->clip; return;
    case AP_XFORM: d->xf.n = s->xf.n; memcpy(d->xf.op, s->xf.op, sizeof d->xf.op); return;
    case AP_ORIGIN: d->xf.ox = s->xf.ox; d->xf.oy = s->xf.oy; d->xf.has_origin = s->xf.has_origin; return;
    case AP_BGPOS: d->bg_px = s->bg_px; d->bg_py = s->bg_py; return;
    case AP_GRAD: d->bg_grad_type = s->bg_grad_type; d->bg_grad_n = s->bg_grad_n; d->bg_grad_angle = s->bg_grad_angle; memcpy(d->bg_grad_c, s->bg_grad_c, sizeof d->bg_grad_c); memcpy(d->bg_grad_p, s->bg_grad_p, sizeof d->bg_grad_p); return;
    }
}
static void ap_lerp(int p, NodeStyle* d, const NodeStyle* a, const NodeStyle* b, float t) {
    const PD* pd = &PDT[p];
    switch (pd->kind) {
    case K_FLOAT: {
        float x = *PF(a, pd->off), y = *PF(b, pd->off);
        if (p >= AP_RAD0 && p <= AP_RAD3 && ((x < 0) != (y < 0)) && x != 0 && y != 0) { *PF(d, pd->off) = t < 0.5f ? x : y; return; }     /* px ↔ % — дискретно */
        if (p == AP_LH && (x <= 0 || y <= 0)) { *PF(d, pd->off) = t < 0.5f ? x : y; return; }
        *PF(d, pd->off) = lf(x, y, t); return; }
    case K_COLOR: *PC(d, pd->off) = lcolor(*PC(a, pd->off), *PC(b, pd->off), t); return;
    case K_LEN: llen(PL(d, pd->off), PL(a, pd->off), PL(b, pd->off), t); return;
    }
    switch (p) {
    case AP_SHADOW: d->sh_x = lf(a->sh_x, b->sh_x, t); d->sh_y = lf(a->sh_y, b->sh_y, t); d->sh_blur = lf(a->sh_blur, b->sh_blur, t); d->sh_spread = lf(a->sh_spread, b->sh_spread, t); d->sh_color = lcolor(a->sh_color, b->sh_color, t); return;
    case AP_FILTER: lfilter(&d->filter, &a->filter, &b->filter, t); return;
    case AP_BACKDROP: lfilter(&d->backdrop, &a->backdrop, &b->backdrop, t); return;
    case AP_CLIP: lclip(&d->clip, &a->clip, &b->clip, t); return;
    case AP_XFORM: lxform(&d->xf, &a->xf, &b->xf, t); return;
    case AP_ORIGIN: lfxlen(&d->xf.ox, &a->xf.ox, &b->xf.ox, t); lfxlen(&d->xf.oy, &a->xf.oy, &b->xf.oy, t); d->xf.has_origin = 1; return;
    case AP_BGPOS: llen(&d->bg_px, &a->bg_px, &b->bg_px, t); llen(&d->bg_py, &a->bg_py, &b->bg_py, t); return;
    case AP_GRAD:
        if (a->bg_grad_type != b->bg_grad_type || a->bg_grad_n != b->bg_grad_n || !a->bg_grad_type) { ap_copy(p, d, t < 0.5f ? a : b); return; }
        d->bg_grad_type = a->bg_grad_type; d->bg_grad_n = a->bg_grad_n; d->bg_grad_angle = lf(a->bg_grad_angle, b->bg_grad_angle, t);
        for (int i = 0; i < 4; i++) { d->bg_grad_c[i] = lcolor(a->bg_grad_c[i], b->bg_grad_c[i], t); d->bg_grad_p[i] = lf(a->bg_grad_p[i], b->bg_grad_p[i], t); }
        d->bg = d->bg_grad_c[0]; return;
    }
}

/* ══════════════ записи ══════════════ */
typedef struct { float off[24]; NodeStyle* st[24]; uint64_t pm[24]; int n; uint64_t all; } KfSet;
typedef struct {
    char name[40]; double start, paused_at; int paused; int last_iter; int started, ended; int active; FxAnim spec; KfSet kf;
} AnimInst;
typedef struct { double t0; float dur; FxEase ease; } TrRun;
typedef struct {
    DOMNode* node; NodeStyle base, from; uint64_t tmask, touched; TrRun tr[AP_COUNT];
    AnimInst an[3]; int nan; int layout_prev; double last_op_parent;
} Rec;

static Rec** g_recs; static int g_nrec, g_caprec;
static DOMNode* g_head_seen; static int g_first = 1;
static NodeStyle* g_old; static uint8_t* g_have; static int g_oldn, g_capture;
static int g_running; static int g_prev_tr;

typedef struct { DOMNode* node; int kind; char name[48]; float elapsed; } AEv;
static AEv g_ev[64]; static int g_ev_h, g_ev_t;
static void ev_push(DOMNode* n, int kind, const char* name, float el) {
    int nt = (g_ev_t + 1) % 64; if (nt == g_ev_h) return;
    AEv* e = &g_ev[g_ev_t]; e->node = n; e->kind = kind; snprintf(e->name, sizeof e->name, "%s", name); e->elapsed = el; g_ev_t = nt;
}
int anim_pop_event(DOMNode** node, int* kind, char* name, float* elapsed) {
    if (g_ev_h == g_ev_t) return 0; AEv* e = &g_ev[g_ev_h]; *node = e->node; *kind = e->kind; snprintf(name, 48, "%s", e->name); *elapsed = e->elapsed; g_ev_h = (g_ev_h + 1) % 64; return 1;
}

static void rec_free(Rec* r) { for (int k = 0; k < 3; k++) for (int i = 0; i < r->an[k].kf.n; i++) free(r->an[k].kf.st[i]); free(r); }
void anim_reset(void) {
    for (int i = 0; i < g_nrec; i++) rec_free(g_recs[i]);
    free(g_recs); g_recs = NULL; g_nrec = g_caprec = 0; free(g_old); free(g_have); g_old = NULL; g_have = NULL; g_oldn = 0; g_ev_h = g_ev_t = 0; g_running = 0; g_prev_tr = 0;
}
static Rec* rec_find(DOMNode* n) { for (int i = 0; i < g_nrec; i++) if (g_recs[i]->node == n) return g_recs[i]; return NULL; }
static Rec* rec_new(DOMNode* n) {
    Rec* r = calloc(1, sizeof *r); if (!r) return NULL; r->node = n;
    if (g_nrec == g_caprec) { g_caprec = g_caprec ? g_caprec * 2 : 16; g_recs = realloc(g_recs, (size_t)g_caprec * sizeof(Rec*)); }
    g_recs[g_nrec++] = r; return r;
}

void anim_cascade_begin(DOMNode* head, int n, int uses_trans) {
    if (head != g_head_seen) { anim_reset(); g_head_seen = head; g_first = 1; } else g_first = 0;
    free(g_old); free(g_have); g_old = NULL; g_have = NULL; g_oldn = 0;
    g_capture = !g_first && (uses_trans || g_nrec > 0 || g_prev_tr);
    if (g_capture) { g_old = malloc((size_t)n * sizeof(NodeStyle)); g_have = calloc((size_t)n, 1); if (g_old && g_have) g_oldn = n; else g_capture = 0; }
}
void anim_before_node(DOMNode* n) {
    if (!g_capture || n->idx < 0 || n->idx >= g_oldn || n->type == TAG_TEXT) return;
    memcpy(&g_old[n->idx], &n->st, sizeof(NodeStyle)); g_have[n->idx] = 1;
}

static int tr_for(const NodeStyle* st, int p, FxTrans* out) {           /* последняя запись, покрывающая свойство */
    int f = 0; for (int i = 0; i < st->tr_n; i++) if (st->tr[i].mask & APM(p)) { *out = st->tr[i]; f = 1; } return f;
}

static void build_kfset(Rec* r, int k, const Keyframes* kf, const NodeStyle* base, float root_fs, KfBuildFn build, void* ctx) {
    AnimInst* a = &r->an[k]; for (int i = 0; i < a->kf.n; i++) free(a->kf.st[i]); memset(&a->kf, 0, sizeof a->kf);
    if (!kf) return;
    for (int i = 0; i < kf->n && a->kf.n < 24; i++) {
        NodeStyle* s = malloc(sizeof *s); if (!s) break; *s = *base;
        uint64_t m = build(ctx, r->node, kf->fr[i].decls, kf->fr[i].nd, s, root_fs);
        if (!m) { free(s); continue; }
        int j = a->kf.n++; a->kf.off[j] = kf->fr[i].off; a->kf.st[j] = s; a->kf.pm[j] = m; a->kf.all |= m;
    }
}

void anim_cascade_end(DOMNode** nv, int n, Keyframes* kfs, float root_fs, KfBuildFn build, void* ctx) {
    double now = anim_now_ms(); int any_tr = 0;
    /* удаляем записи исчезнувших узлов */
    for (int i = 0; i < g_nrec;) { DOMNode* d = g_recs[i]->node; if (d->idx < 0 || d->idx >= n || nv[d->idx] != d) { rec_free(g_recs[i]); g_recs[i] = g_recs[--g_nrec]; } else i++; }
    for (int i = 0; i < n; i++) {
        DOMNode* nd = nv[i]; if (nd->type == TAG_TEXT) continue; NodeStyle* st = &nd->st;
        if (st->display == DSP_NONE) { Rec* r0 = rec_find(nd); if (r0) { r0->tmask = 0; r0->touched = 0; for (int k = 0; k < 3; k++) r0->an[k].name[0] = 0; r0->nan = 0; } continue; }
        int want_tr = 0; for (int k = 0; k < st->tr_n; k++) if (st->tr[k].mask && (st->tr[k].dur > 0 || st->tr[k].delay != 0)) want_tr = 1;
        int want_an = 0; for (int k = 0; k < st->an_n; k++) if (st->an[k].name[0]) want_an = 1;
        if (want_tr) any_tr = 1;
        Rec* r = rec_find(nd);
        if (!r && !want_tr && !want_an) continue;
        int fresh = 0; if (!r) { r = rec_new(nd); if (!r) continue; fresh = 1; }
        /* ── transition ── */
        const NodeStyle* prev = fresh ? ((g_capture && g_have && i < g_oldn && g_have[i]) ? &g_old[i] : st) : &r->base;
        if (!fresh || (g_capture && g_have && i < g_oldn && g_have[i])) {
            const NodeStyle* eff = (g_capture && g_have && i < g_oldn && g_have[i]) ? &g_old[i] : prev;      /* видимое значение до каскада */
            int was_shown = eff->display != DSP_NONE;
            for (int p = 0; p < AP_COUNT; p++) {
                FxTrans t; int has = tr_for(st, p, &t);
                if (r->tmask & APM(p)) { if (!has || (t.dur <= 0 && t.delay == 0)) r->tmask &= ~APM(p); }
                if (!has || (t.dur <= 0 && t.delay == 0) || !was_shown || g_first) continue;
                if (ap_eq(p, prev, st)) continue;                                   /* целевое значение не менялось */
                if ((r->tmask & APM(p)) && ap_eq(p, &r->base, st)) continue;
                ap_copy(p, &r->from, eff);
                r->tr[p].t0 = now + t.delay; r->tr[p].dur = t.dur > 1 ? t.dur : 1; r->tr[p].ease = t.ease; r->tmask |= APM(p);
                ev_push(nd, AEV_TRANS_RUN, "", 0);
            }
        }
        r->base = *st;
        /* ── animation ── */
        AnimInst keep[3]; memcpy(keep, r->an, sizeof keep);
        for (int k = 0; k < 3; k++) { /* новые записи */ }
        for (int k = 0; k < 3; k++) {
            AnimInst* a = &r->an[k];
            if (k >= st->an_n || !st->an[k].name[0]) { for (int q = 0; q < a->kf.n; q++) free(a->kf.st[q]); memset(a, 0, sizeof *a); continue; }
            const FxAnim* sp = &st->an[k];
            if (strcmp(a->name, sp->name)) { for (int q = 0; q < a->kf.n; q++) free(a->kf.st[q]); memset(a, 0, sizeof *a); snprintf(a->name, sizeof a->name, "%s", sp->name); a->start = now; a->last_iter = -1; }
            if (sp->paused && !a->paused) { a->paused = 1; a->paused_at = now; } else if (!sp->paused && a->paused) { a->paused = 0; a->start += now - a->paused_at; }
            a->spec = *sp;
            const Keyframes* kf = NULL; for (const Keyframes* q = kfs; q; q = q->next) if (!strcmp(q->name, sp->name)) { kf = q; break; }
            build_kfset(r, k, kf, st, root_fs, build, ctx);
        }
        (void)keep;
        r->nan = st->an_n;
        if (!r->tmask && !want_an && !want_tr) { rec_free(r); for (int q = 0; q < g_nrec; q++) if (g_recs[q] == r) { g_recs[q] = g_recs[--g_nrec]; break; } }
    }
    g_prev_tr = any_tr;
    for (int i = 0; i < g_nrec; i++) { Rec* r = g_recs[i]; if (r->tmask) g_running = 1; for (int k = 0; k < 3; k++) if (r->an[k].name[0] && !r->an[k].ended) g_running = 1; }
    free(g_old); free(g_have); g_old = NULL; g_have = NULL; g_oldn = 0; g_capture = 0;
}

/* ══════════════ шаг ══════════════ */
static int rec_cmp(const void* a, const void* b) { const Rec* x = *(Rec* const*)a; const Rec* y = *(Rec* const*)b; return x->node->idx - y->node->idx; }

static void anim_apply_kf(Rec* r, AnimInst* a, float p, uint64_t* touched) {
    const KfSet* ks = &a->kf; NodeStyle* st = &r->node->st;
    for (int pr = 0; pr < AP_COUNT; pr++) {
        if (!(ks->all & APM(pr))) continue;
        int lo = -1, hi = -1;                                                       /* ближайшие кадры, задающие свойство, слева и справа от p */
        for (int i = 0; i < ks->n; i++) if (ks->pm[i] & APM(pr)) { if (ks->off[i] <= p) lo = i; if (ks->off[i] >= p && hi < 0) hi = i; }
        const NodeStyle* A = lo >= 0 ? ks->st[lo] : &r->base; float oa = lo >= 0 ? ks->off[lo] : 0;
        const NodeStyle* B = hi >= 0 ? ks->st[hi] : &r->base; float ob = hi >= 0 ? ks->off[hi] : 1;
        float t = ob > oa ? (p - oa) / (ob - oa) : 0; if (t < 0) t = 0; if (t > 1) t = 1;
        if (lo >= 0 && hi >= 0 && ks->off[lo] == ks->off[hi]) { ap_copy(pr, st, ks->st[hi]); *touched |= APM(pr); continue; }
        ap_lerp(pr, st, A, B, fx_ease(&a->spec.ease, t)); *touched |= APM(pr);
    }
}

int anim_step(DOMNode* head) {
    (void)head; int flags = 0; g_running = 0;
    if (!g_nrec) return 0;
    double now = anim_now_ms();
    qsort(g_recs, (size_t)g_nrec, sizeof(Rec*), rec_cmp);
    int total = 0; for (DOMNode* q = g_recs[0]->node; q; q = q->next) total++;
    DOMNode** nv = NULL; int nvn = 0;
    { /* вектор узлов от начала документа: нужен для родительской opacity */
        int cnt = 0; for (DOMNode* q = head; q; q = q->next) cnt++; nv = malloc((size_t)(cnt ? cnt : 1) * sizeof(DOMNode*)); if (!nv) return 0;
        for (DOMNode* q = head; q; q = q->next) nv[nvn++] = q; (void)total;
    }
    for (int ri = 0; ri < g_nrec; ri++) {
        Rec* r = g_recs[ri]; DOMNode* nd = r->node; NodeStyle* st = &nd->st; if (nd->idx < 0 || nd->idx >= nvn || nv[nd->idx] != nd) continue;
        uint64_t prev_touched = r->touched, touched = 0; uint32_t prev_color = st->color;
        for (int p = 0; p < AP_COUNT; p++) if (prev_touched & APM(p)) ap_copy(p, st, &r->base);              /* возврат к базе */
        uint32_t base_color_applied = st->color; (void)base_color_applied;
        /* transition */
        for (int p = 0; p < AP_COUNT; p++) {
            if (!(r->tmask & APM(p))) continue;
            double t = (now - r->tr[p].t0) / r->tr[p].dur;
            if (t >= 1) { r->tmask &= ~APM(p); ev_push(nd, AEV_TRANS_END, "", r->tr[p].dur / 1000.0f); touched |= APM(p); continue; }       /* на последнем кадре — база */
            if (t < 0) t = 0; g_running = 1;
            ap_lerp(p, st, &r->from, &r->base, fx_ease(&r->tr[p].ease, (float)t)); touched |= APM(p);
        }
        /* animation */
        for (int k = 0; k < 3; k++) {
            AnimInst* a = &r->an[k]; if (!a->name[0] || !a->kf.n) continue; const FxAnim* sp = &a->spec;
            double el = (a->paused ? a->paused_at : now) - a->start - sp->delay; double dur = sp->dur;
            int fill_b = sp->fill & 2, fill_f = sp->fill & 1; float prog = 0; int apply = 0;
            if (dur <= 0) { if (el >= 0) { if (!a->ended) { a->ended = 1; ev_push(nd, AEV_ANIM_END, a->name, 0); } if (fill_f) { prog = (sp->dir == 1 || sp->dir == 3) ? 0.0f : 1.0f; apply = 1; } } else if (fill_b) { prog = (sp->dir == 1 || sp->dir == 3) ? 1.0f : 0.0f; apply = 1; } }
            else if (el < 0) { if (fill_b) { prog = (sp->dir == 1 || sp->dir == 3) ? 1.0f : 0.0f; apply = 1; } g_running = 1; }
            else {
                double total_d = sp->iter < 0 ? 1e30 : dur * sp->iter; int it; float frac;
                if (!a->started) { a->started = 1; ev_push(nd, AEV_ANIM_START, a->name, 0); }
                if (el >= total_d) {
                    if (!a->ended) { a->ended = 1; ev_push(nd, AEV_ANIM_END, a->name, (float)(total_d / 1000.0)); }
                    if (fill_f) { double fi = sp->iter; it = (int)ceil(fi) - 1; if (it < 0) it = 0; frac = (float)(fi - floor(fi)); if (frac == 0) frac = 1; apply = 1; goto have_it; }
                } else {
                    it = (int)(el / dur); frac = (float)(el / dur - it); apply = 1; if (!a->paused) g_running = 1;
                    if (it != a->last_iter) { if (a->last_iter >= 0) ev_push(nd, AEV_ANIM_ITER, a->name, (float)(it * dur / 1000.0)); a->last_iter = it; }
                have_it:;
                    int rev = sp->dir == 1 || (sp->dir == 2 && (it & 1)) || (sp->dir == 3 && !(it & 1));
                    prog = rev ? 1.0f - frac : frac;
                }
            }
            if (apply) anim_apply_kf(r, a, prog, &touched);
        }
        r->touched = touched;
        /* производные значения */
        uint64_t changed = touched | prev_touched;
        if (changed & APM(AP_OPACITY)) {
            DOMNode* pa = nd->parent >= 0 && nd->parent < nvn ? nv[nd->parent] : NULL; st->opacity = st->op_own * (pa ? pa->st.opacity : 1.0f);
            for (DOMNode* q = nd->next; q && q->idx <= nd->subtree_end; q = q->next) { DOMNode* qp = q->parent >= 0 && q->parent < nvn ? nv[q->parent] : NULL; if (q->type != TAG_TEXT) q->st.opacity = q->st.op_own * (qp ? qp->st.opacity : 1.0f); else q->st.opacity = qp ? qp->st.opacity : 1.0f; }
        }
        if ((changed & APM(AP_COLOR)) && st->color != prev_color) {
            for (DOMNode* q = nd->next; q && q->idx <= nd->subtree_end; q = q->next) if (q->st.color == prev_color) q->st.color = st->color;
        }
        if (changed & (AP_LAYOUT_MASK | APM(AP_OPACITY) | APM(AP_COLOR))) flags |= 1;
        st->has_fx = (uint8_t)fx_style_has_fx(st);
    }
    free(nv);
    return flags;
}
int anim_active(void) { return g_running; }

int webpage_anim_tick(void) {
    static double last; if (!g_nrec) return 0;
    if (!g_running && g_ev_h == g_ev_t) return 0;
    double now = anim_now_ms(); if (now - last < 15.0) return 0; last = now; return 1;
}
