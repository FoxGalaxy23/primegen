/* Раскладка: дерево боксов -> display list.
   Блоки (схлопывание отступов), строчный поток (переносы, baseline, выравнивание), float,
   inline-block, flex, таблицы, relative/absolute/fixed. */
#include "l2.h"
#include "css_int.h"
#include "font_manager.h"
#include "fx.h"
#include "gal.h"
#include "../ttf_render/shaping.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <strings.h>

extern TTFFont* g_ttf_font;

/* ══════════════ память ══════════════ */
typedef struct AC { struct AC* next; size_t used, cap; char d[]; } AC;
static AC* g_ac;
static void* la(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!g_ac || g_ac->cap - g_ac->used < n) {
        size_t cap = n > (1u << 20) ? n : (1u << 20);
        AC* c = malloc(sizeof(AC) + cap); if (!c) return NULL;
        c->next = g_ac; c->used = 0; c->cap = cap; g_ac = c;
    }
    void* p = g_ac->d + g_ac->used; g_ac->used += n; memset(p, 0, n); return p;
}
static AC* g_sc;                                   /* строки display list живут до следующей раскладки */
static char* sa(size_t n) {
    if (!g_sc || g_sc->cap - g_sc->used < n + 1) { size_t cap = n + 1 > (1u << 18) ? n + 1 : (1u << 18); AC* c = malloc(sizeof(AC) + cap); if (!c) return NULL; c->next = g_sc; c->used = 0; c->cap = cap; g_sc = c; }
    char* p = g_sc->d + g_sc->used; g_sc->used += n + 1; return p;
}
static void sa_reset(void) { while (g_sc) { AC* n = g_sc->next; free(g_sc); g_sc = n; } }
static AC* g_pa;                                   /* узлы ::before/::after: живут столько же, сколько display list */
static void* pa_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!g_pa || g_pa->cap - g_pa->used < n) { size_t cap = n > (1u << 20) ? n : (1u << 20); AC* c = malloc(sizeof(AC) + cap); if (!c) return NULL; c->next = g_pa; c->used = 0; c->cap = cap; g_pa = c; }
    void* p = g_pa->d + g_pa->used; g_pa->used += n; memset(p, 0, n); return p;
}
static void pa_reset(void) { while (g_pa) { AC* n = g_pa->next; free(g_pa); g_pa = n; } }
static void la_reset(void) { while (g_ac) { AC* n = g_ac->next; free(g_ac); g_ac = n; } }

/* ══════════════ display list ══════════════ */
Cmd* l2_dl; int l2_dn; static int g_dcap;
uint32_t l2_canvas_bg = 0xFFFFFFFFu; float l2_doc_w, l2_doc_h;
float* l2_rects; int l2_nrects;
static int g_fixed;
typedef struct Box Box;
static DOMNode** NV; static int NVN; static Box** GBOX; static float VW, VH, g_page_scroll_x, g_page_scroll_y;
void l2_set_page_scroll(float x, float y) { g_page_scroll_x=x<0?0:x; g_page_scroll_y=y<0?0:y; }

static int dl_add(Cmd c) {
    if (l2_dn == g_dcap) { g_dcap = g_dcap ? g_dcap * 2 : 4096; Cmd* n = realloc(l2_dl, (size_t)g_dcap * sizeof(Cmd)); if (!n) return -1; l2_dl = n; }
    c.fixed = (uint8_t)g_fixed; l2_dl[l2_dn] = c; return l2_dn++;
}
static void dl_shift(int s, int e, float dx, float dy) {
    for (int i = s; i < e; i++) { l2_dl[i].x += dx; l2_dl[i].y += dy; if (l2_dl[i].t == C_STK) { l2_dl[i].ex[1] += dy; l2_dl[i].ex[2] += dy; l2_dl[i].ex[6] += dx; l2_dl[i].ex[7] += dx; } }
}
void l2_release(void) { free(l2_dl); l2_dl = NULL; l2_dn = g_dcap = 0; free(l2_rects); l2_rects = NULL; l2_nrects = 0; }


/* ══════════════ стекинг-контексты (z-index) ══════════════
   Раскладка выдаёт display list в порядке обхода дерева. Чтобы элементы с z-index≠0 рисовались в правильном порядке,
   каждый контекст (позиционированный блок с z-index, flex-элемент с z-index, position:fixed, корень) запоминает свой
   диапазон команд [start,end). После раскладки диапазоны с z<0 переносятся в начало содержимого ближайшего
   охватывающего контекста, с z>0 — в его конец (по возрастанию z). При переносе из-под overflow-отсечения вокруг
   перенесённого диапазона восстанавливаются те же C_CLIP/C_UNCLIP, поэтому отсечение и прокрутка сохраняются. */
typedef struct {
    int start, cstart, cend, end, z, has_clip, lift;   /* lift: позиционированный элемент с z=auto (sticky) рисуется поверх обычного потока */
    int parent, first_child, last_child, next_sib;
    int cdepth_s, cdepth_c;          /* глубина стека отсечений: перед start и внутри содержимого (после собственного C_CLIP) */
    int* snap;                        /* индексы C_CLIP, активных перед start (только у переносимых) */
    unsigned char detach, bad;
} StkGrp;
static StkGrp* g_sg; static int g_sgn, g_sgcap;
static void grp_record(int start, int cstart, int cend, int end, int z, int has_clip, int lift) {
    if (end <= start) return;
    if (g_sgn == g_sgcap) { int nc = g_sgcap ? g_sgcap * 2 : 64; StkGrp* nv = realloc(g_sg, (size_t)nc * sizeof(StkGrp)); if (!nv) return; g_sg = nv; g_sgcap = nc; }
    StkGrp* g = &g_sg[g_sgn++]; memset(g, 0, sizeof *g);
    g->start = start; g->cstart = cstart < start ? start : cstart; g->cend = cend > end ? end : cend; g->end = end; g->z = z; g->has_clip = has_clip; g->lift = lift;
}
/* раскладка части поддерева выполняется заново (flex): забываем контексты, записанные после mark */
static void grp_trunc(int mark) { int k = 0; for (int i = 0; i < g_sgn; i++) if (g_sg[i].start < mark) g_sg[k++] = g_sg[i]; g_sgn = k; }

static Cmd* so_out; static int so_n, so_cap;
static void so_push(const Cmd* c) {
    if (so_n == so_cap) { so_cap = so_cap ? so_cap * 2 : 4096; Cmd* nv = realloc(so_out, (size_t)so_cap * sizeof(Cmd)); if (!nv) return; so_out = nv; }
    so_out[so_n++] = *c;
}
static void so_copy(int a, int b) { for (int i = a; i < b; i++) so_push(&l2_dl[i]); }
static int g_sgv;                                  /* индекс виртуального корня (весь список) */
static int so_zcmp(const void* a, const void* b) {
    const StkGrp* x = &g_sg[*(const int*)a]; const StkGrp* y = &g_sg[*(const int*)b];
    if (x->z != y->z) return x->z < y->z ? -1 : 1;
    return x->start < y->start ? -1 : x->start > y->start;
}
static void so_emit(int gi);
static void so_emit_detached(int di, int pi) {
    StkGrp* D = &g_sg[di]; int from = g_sg[pi].cdepth_c, to = D->cdepth_s;
    for (int j = from; j < to; j++) so_push(&l2_dl[D->snap[j]]);
    so_emit(di);
    for (int j = to - 1; j >= from; j--) { Cmd u; memset(&u, 0, sizeof u); u.t = C_UNCLIP; u.fixed = l2_dl[D->snap[j]].fixed; so_push(&u); }
}
static void so_emit(int gi) {
    StkGrp* G = &g_sg[gi]; int nd = 0; int* dk = NULL;
    for (int c = G->first_child; c >= 0; c = g_sg[c].next_sib) if (g_sg[c].detach) nd++;
    if (nd && (dk = malloc((size_t)nd * sizeof(int)))) { int k = 0; for (int c = G->first_child; c >= 0; c = g_sg[c].next_sib) if (g_sg[c].detach) dk[k++] = c; qsort(dk, (size_t)nd, sizeof(int), so_zcmp); }
    else nd = 0;
    so_copy(G->start, G->cstart);
    for (int k = 0; k < nd; k++) if (g_sg[dk[k]].z < 0) so_emit_detached(dk[k], gi);
    int i = G->cstart;
    for (int c = G->first_child; c >= 0; c = g_sg[c].next_sib) {
        StkGrp* C = &g_sg[c]; if (C->start < i || C->end > G->cend) continue;     /* защита от нарушенной вложенности */
        so_copy(i, C->start);
        if (!C->detach) so_emit(c);
        i = C->end;
    }
    if (i < G->cend) so_copy(i, G->cend);
    for (int k = 0; k < nd; k++) if (g_sg[dk[k]].z >= 0) so_emit_detached(dk[k], gi);   /* z=0 (sticky) идёт перед z>0 */
    so_copy(G->cend > i ? G->cend : i, G->end);
    free(dk);
}
static int so_ordcmp(const void* a, const void* b) {
    int ia = *(const int*)a, ib = *(const int*)b; const StkGrp* x = &g_sg[ia]; const StkGrp* y = &g_sg[ib];
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->end != y->end) return x->end > y->end ? -1 : 1;
    return ib - ia;                                 /* при совпадении диапазонов внешний записан позже — он первый */
}
static void stk_reorder(void) {
    int n = g_sgn, any = 0; if (n == 0 || l2_dn == 0) return;
    for (int i = 0; i < n; i++) if (g_sg[i].z != 0 || g_sg[i].lift) { any = 1; break; }
    if (!any) return;
    StkGrp* nv = realloc(g_sg, (size_t)(n + 1) * sizeof(StkGrp)); if (!nv) return; g_sg = nv; g_sgcap = n + 1;
    int* ord = malloc((size_t)n * sizeof(int)); int* stk = malloc((size_t)n * sizeof(int)); if (!ord || !stk) { free(ord); free(stk); return; }
    for (int i = 0; i < n; i++) ord[i] = i;
    qsort(ord, (size_t)n, sizeof(int), so_ordcmp);
    g_sgv = n; StkGrp* V = &g_sg[n]; memset(V, 0, sizeof *V); V->start = V->cstart = 0; V->cend = V->end = l2_dn; V->parent = V->first_child = V->last_child = V->next_sib = -1;
    for (int i = 0; i < n; i++) { g_sg[i].parent = g_sg[i].first_child = g_sg[i].last_child = g_sg[i].next_sib = -1; }
    int sp = 0;
    for (int k = 0; k < n; k++) {
        int gi = ord[k]; StkGrp* g = &g_sg[gi];
        while (sp && g_sg[stk[sp - 1]].end <= g->start) sp--;
        if (sp && g->end > g_sg[stk[sp - 1]].end) { g->bad = 1; continue; }              /* диапазоны пересекаются — не трогаем */
        int pi = sp ? stk[sp - 1] : n; g->parent = pi; StkGrp* P = &g_sg[pi];
        if (P->last_child >= 0) g_sg[P->last_child].next_sib = gi; else P->first_child = gi; P->last_child = gi;
        stk[sp++] = gi; g->detach = (g->z != 0 || g->lift);
    }
    /* глубина стека отсечений и снимки активных C_CLIP перед началом переносимых диапазонов */
    int cap = 64, depth = 0; int* cs = malloc((size_t)cap * sizeof(int)); int p = 0; int ok = cs != NULL;
    for (int i = 0; ok && i <= l2_dn; i++) {
        while (p < n && g_sg[ord[p]].start == i) {
            StkGrp* g = &g_sg[ord[p]]; g->cdepth_s = depth; g->cdepth_c = depth + (g->has_clip ? 1 : 0);
            if (g->detach && depth > 0) { g->snap = malloc((size_t)depth * sizeof(int)); if (g->snap) memcpy(g->snap, cs, (size_t)depth * sizeof(int)); else g->detach = 0; }
            else if (g->detach) g->snap = NULL;
            p++;
        }
        if (i == l2_dn) break;
        const Cmd* c = &l2_dl[i];
        if (c->t == C_CLIP) { if (depth == cap) { int* t = realloc(cs, (size_t)(cap * 2) * sizeof(int)); if (!t) { ok = 0; break; } cs = t; cap *= 2; } cs[depth++] = i; }
        else if (c->t == C_UNCLIP && depth > 0) depth--;
    }
    free(cs);
    if (ok) {
        V->cdepth_s = V->cdepth_c = 0; so_out = NULL; so_n = so_cap = 0;
        so_emit(n);
        if (so_out && so_n >= l2_dn) { free(l2_dl); l2_dl = so_out; l2_dn = so_n; g_dcap = so_cap; } else free(so_out);
        so_out = NULL; so_n = so_cap = 0;
    }
    for (int i = 0; i < n; i++) { free(g_sg[i].snap); g_sg[i].snap = NULL; }
    free(ord); free(stk);
}

/* ══════════════ прокрутка внутри элементов (overflow:auto|scroll) ══════════════ */
typedef L2ScrollState ScSt;
static ScSt* g_scr; static int g_scrn; static DOMNode* g_scr_head;
static float sc_clamp(float v, float mx) { return v < 0 ? 0 : v > mx ? mx : v; }
static void sc_prepare(DOMNode* head, int n) {
    if (head != g_scr_head) { free(g_scr); g_scr = NULL; g_scrn = 0; g_scr_head = head; }
    if (n > g_scrn) { ScSt* nv = calloc((size_t)n, sizeof(ScSt)); if (nv) { if (g_scr) memcpy(nv, g_scr, (size_t)g_scrn * sizeof(ScSt)); free(g_scr); g_scr = nv; g_scrn = n; } }
    for (int i = 0; i < g_scrn; i++) { g_scr[i].mx = g_scr[i].my = g_scr[i].sw = g_scr[i].sh = g_scr[i].cw = g_scr[i].ch = 0; g_scr[i].is = 0; }
}
/* заполняет таблицу по готовому display list (после patch_h и переноса диапазонов) */
static void sc_commit(void) {
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i];
        if (c->t != C_CLIP || !(c->flags & (CF_SCROLL_X | CF_SCROLL_Y)) || !c->node) continue;
        int k = c->node->idx; if (k < 0 || k >= g_scrn) continue;
        ScSt* e = &g_scr[k]; e->is = 1; e->sw = c->ex[0]; e->sh = c->ex[1]; e->cw = c->w; e->ch = c->h; e->mx = c->ex[2]; e->my = c->ex[3];
    }
}
int l2_scroll_get(const DOMNode* n, float* x, float* y) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (!n || !g_scr || n->idx < 0 || n->idx >= g_scrn) return 0;
    const ScSt* e = &g_scr[n->idx]; if (!e->is) return 0;
    if (x) *x = sc_clamp(e->x, e->mx); if (y) *y = sc_clamp(e->y, e->my); return 1;
}
int l2_scroll_max(const DOMNode* n, float* mx, float* my) {
    if (mx) *mx = 0;
    if (my) *my = 0;
    if (!n || !g_scr || n->idx < 0 || n->idx >= g_scrn || !g_scr[n->idx].is) return 0;
    if (mx) *mx = g_scr[n->idx].mx; if (my) *my = g_scr[n->idx].my; return 1;
}
int l2_scroll_size(const DOMNode* n, float* sw, float* sh, float* cw, float* ch) {
    if (!n || !g_scr || n->idx < 0 || n->idx >= g_scrn || !g_scr[n->idx].is) return 0;
    const ScSt* e = &g_scr[n->idx]; if (sw) *sw = e->sw; if (sh) *sh = e->sh; if (cw) *cw = e->cw; if (ch) *ch = e->ch; return 1;
}
static void l2_scroll_set_impl(const DOMNode* n, float x, float y, int apply_snap) {
    if (!n || !g_scr || n->idx < 0 || n->idx >= g_scrn) return;
    ScSt* e = &g_scr[n->idx];
    x = sc_clamp(x, e->mx); y = sc_clamp(y, e->my);
    /* CSS Scroll Snap: choose the closest direct-child snap area on enabled axes. */
    if (apply_snap && n->st.scroll_snap_type && l2_rects && n->idx < l2_nrects) {
        const float* cr = &l2_rects[n->idx * 4];
        float best_x = x, best_y = y, dx = 1e30f, dy = 1e30f;
        int use_x = (n->st.scroll_snap_type & 1) != 0, use_y = (n->st.scroll_snap_type & 2) != 0;
        for (int i = 0; i < NVN && i < l2_nrects; i++) {
            DOMNode* ch = NV ? NV[i] : NULL;
            if (!ch || ch->parent != n->idx || ch->st.scroll_snap_align == 0) continue;
            const float* r = &l2_rects[i * 4];
            if (r[2] <= 0 || r[3] <= 0) continue;
            if (use_x) {
                float t = r[0] - cr[0];
                if (ch->st.scroll_snap_align == 2) t -= (e->cw - r[2]) * 0.5f;
                else if (ch->st.scroll_snap_align == 3) t -= e->cw - r[2];
                t = sc_clamp(t, e->mx); float d = fabsf(t - x);
                if (d < dx) { dx = d; best_x = t; }
            }
            if (use_y) {
                float t = r[1] - cr[1];
                if (ch->st.scroll_snap_align == 2) t -= (e->ch - r[3]) * 0.5f;
                else if (ch->st.scroll_snap_align == 3) t -= e->ch - r[3];
                t = sc_clamp(t, e->my); float d = fabsf(t - y);
                if (d < dy) { dy = d; best_y = t; }
            }
        }
        x = best_x; y = best_y;
    }
    e->x = x; e->y = y;
}
void l2_scroll_set(const DOMNode* n, float x, float y) { l2_scroll_set_impl(n, x, y, 1); }
void l2_scroll_set_raw(const DOMNode* n, float x, float y) { l2_scroll_set_impl(n, x, y, 0); }
int l2_scroll_state_export(const DOMNode* head, L2ScrollState* out, int cap) {
    if (!head || g_scr_head != head || !g_scr || g_scrn <= 0) return 0;
    if (!out) return g_scrn;
    if (cap <= 0) return 0;
    int n = g_scrn < cap ? g_scrn : cap;
    if (n > 0) memcpy(out, g_scr, (size_t)n * sizeof(*out));
    return n;
}
void l2_scroll_state_import(const L2ScrollState* in, int count, DOMNode* head) {
    if (!head || count < 0) return;
    if (count > g_scrn) {
        ScSt* nv = realloc(g_scr, (size_t)count * sizeof(*nv));
        if (!nv) return;
        for (int i = g_scrn; i < count; i++) memset(&nv[i], 0, sizeof nv[i]);
        g_scr = nv; g_scrn = count;
    }
    if (in && count > 0) memcpy(g_scr, in, (size_t)count * sizeof(*in));
    for (int i = count; i < g_scrn; i++) { g_scr[i].is = 0; g_scr[i].mx = g_scr[i].my = g_scr[i].sw = g_scr[i].sh = g_scr[i].cw = g_scr[i].ch = 0; }
    g_scr_head = (DOMNode*)head;
}
void l2_scroll_offset_of_ancestors(const DOMNode* n, float* ox, float* oy) {
    *ox = *oy = 0; if (!n || !g_scr) return;
    int guard = 0;
    for (int p = n->parent; p >= 0 && p < NVN && p < g_scrn && guard < 4096; p = NV[p]->parent, guard++) {
        if (!g_scr[p].is) continue;
        *ox += sc_clamp(g_scr[p].x, g_scr[p].mx); *oy += sc_clamp(g_scr[p].y, g_scr[p].my);
    }
}

float l2_timeline_progress(const DOMNode* n, int timeline, int axis) {
    if (!n || !NV || n->idx < 0 || n->idx >= NVN) return 0.0f;
    if (timeline == 2) { /* view(): progress as the element traverses the relevant viewport axis */
        if (!l2_rects || n->idx >= l2_nrects) return 0.0f;
        const float* r=&l2_rects[n->idx*4]; float ox=0,oy=0; l2_scroll_offset_of_ancestors(n,&ox,&oy);
        int horizontal = axis==2 ? 1 : axis==3 ? 0 : axis==1 ? n->st.writing_mode==0 : n->st.writing_mode!=0;
        float pos=horizontal?r[0]-ox-g_page_scroll_x:r[1]-oy-g_page_scroll_y;
        float size=horizontal?r[2]:r[3], viewport=horizontal?VW:VH; if(size<1)size=1;
        float denom=viewport+size; if(denom<=0)return 0.0f;
        float p=(viewport-pos)/denom; if(p<0)p=0;if(p>1)p=1;return p;
    }
    if (timeline != 1) return 0.0f;
    int horizontal=0;
    if(axis==2) horizontal=1;
    else if(axis==3) horizontal=0;
    else if(axis==1) horizontal=n->st.writing_mode==0;
    else horizontal=n->st.writing_mode!=0;
    int guard=0;
    for(int p=n->parent;p>=0&&p<NVN&&p<g_scrn&&guard++<4096;p=NV[p]->parent){
        const DOMNode* a=NV[p]; if(!g_scr[p].is)continue;
        if(horizontal && !a->st.sc_x)continue;
        if(!horizontal && !a->st.sc_y)continue;
        float max=horizontal?g_scr[p].mx:g_scr[p].my;
        if(max<=0.01f)continue;
        float cur=horizontal?sc_clamp(g_scr[p].x,g_scr[p].mx):sc_clamp(g_scr[p].y,g_scr[p].my);
        float pr=cur/max;if(pr<0)pr=0;if(pr>1)pr=1;return pr;
    }
    /* With no element scroller, scroll() uses the document viewport timeline. */
    float max = horizontal ? l2_doc_w - VW : l2_doc_h - VH;
    float cur = horizontal ? g_page_scroll_x : g_page_scroll_y;
    if (max < 0.01f) return 0.0f;
    float p=cur/max;if(p<0)p=0;if(p>1)p=1;return p;
}

/* ══════════════ боксы ══════════════ */
enum { K_BLOCK = 1, K_INLINE, K_TEXT, K_ATOM, K_BR };
enum { R_NONE, R_IMG, R_CTRL, R_PLACE, R_CANVAS, R_VIDEO };
typedef struct { TTFFont* f; int size, style, asc, desc; float lh; int weight, ital; } FM;
typedef struct AbsRef { Box* b; float sx, sy; struct AbsRef* next; } AbsRef;
typedef struct StkRef { int slot; DOMNode* node; struct StkRef* next; } StkRef;   /* sticky-потомок: слот C_STK, в который родитель допишет границы контейнера */
struct Box {
    DOMNode* n; const NodeStyle* st; Box *parent, *first, *last, *next;
    float zoom_scale;               /* cumulative CSS zoom used by layout, not a paint-only transform */
    uint8_t kind, anon, repl, oof, flt, skipbg, mforce, ic_ok;
    DOMNode* click; const char* text; FM* fm;
    float x, y, w, h, fb;
    float mt, mr, mb, ml, pt, pr, pb, pl, bt, br, bb, bl; uint8_t mla, mra; float fml, fmr;
    int slot, clipslot; AbsRef *abs, *abs_tail; StkRef* stk;
    ImgPix* img; float nat_w, nat_h; float ic_min, ic_max;
    float* grid_col_size; float* grid_row_size;
    int grid_col_count, grid_row_count, grid_col_origin, grid_row_origin;
    float grid_col_gap, grid_row_gap;
};
typedef struct { float x0, y0, x1, y1; } FRect;
#define FCMAX 160
typedef struct { FRect l[FCMAX], r[FCMAX]; int nl, nr; } FCtx;
typedef struct { FCtx* fc; Box* owner; } LS;

static float lenpx(const Len* l, float base) { return l->px + l->pct * base / 100.0f + (l->vw + l->svw + l->dvw) * VW + (l->vh + l->svh + l->dvh) * VH + l->lh; }
static float collapse2(float a, float b) { if (a >= 0 && b >= 0) return a > b ? a : b; if (a <= 0 && b <= 0) return a < b ? a : b; return a + b; }
static float fmaxf2(float a, float b) { return a > b ? a : b; }
static float fminf2(float a, float b) { return a < b ? a : b; }

/* ── шрифты ── */
static const char* fam_of(const Box* b) { return b->n ? b->n->font_family : (b->parent ? fam_of(b->parent) : ""); }
static FM* fm_of(Box* b) {
    if (b->fm) return b->fm;
    FM* m = la(sizeof(FM)); const NodeStyle* st = b->st;
    const char* fam = fam_of(b);
    /* Начертание выбирается по (семейство, насыщенность, курсив): у веб-шрифта может быть несколько @font-face
       (Font Awesome: regular 400 / solid 900). Синтетические bold/italic включаются, только если настоящего начертания нет. */
    int weight = st->weight ? st->weight : (st->bold ? 700 : 400), ital = st->italic ? 1 : 0;
    m->weight = weight; m->ital = ital;
    m->f = font_manager_get_styled(fam, weight, ital); if (!m->f) m->f = g_ttf_font;
    TTFFont* f_norm = font_manager_get_styled(fam, 400, ital), * f_noital = font_manager_get_styled(fam, weight, 0);
    int real_bold = m->f != f_norm, real_ital = m->f != f_noital;
    m->size = (int)(st->font_size + 0.5f); if (m->size < 1) m->size = 1; if (m->size > 400) m->size = 400;
    m->style = ((st->bold && !real_bold) ? TTF_BOLD : 0) | ((st->italic && !real_ital) ? TTF_ITALIC : 0);
    m->asc = m->f ? ttf_font_ascent(m->f, m->size) : (int)(m->size * 0.93f);
    m->desc = m->f ? ttf_font_descent(m->f, m->size) : (int)(m->size * 0.24f);
    m->lh = st->lh_px > 0 ? st->lh_px : st->lh_mult > 0 ? st->lh_mult * st->font_size : (float)(m->asc + m->desc);
    b->fm = m; return m;
}
static float text_w(FM* m, const char* s, int len) {
    char tmp[600]; if (len > 599) len = 599; memcpy(tmp, s, (size_t)len); tmp[len] = 0;
    return m->f ? ttf_text_width_f(m->f, tmp, m->size, m->style) : (float)len * m->size * 0.5f;
}

/* ══════════════ построение боксов ══════════════ */
static void zoom_len(Len* l, float z) {
    l->px *= z; l->vw *= z; l->vh *= z; l->svw *= z; l->svh *= z;
    l->dvw *= z; l->dvh *= z; l->lh *= z;
}
static void zoom_fxlen(FxLen* l, float z) { l->px *= z; }
static void zoom_fxlist(FxList* f, float z) {
    for (int i = 0; i < f->n; i++) if (f->op[i].kind == FXO_BLUR || f->op[i].kind == FXO_SHADOW) {
        if (f->op[i].kind == FXO_BLUR) f->op[i].v *= z;
        f->op[i].x *= z; f->op[i].y *= z; f->op[i].blur *= z;
    }
}
static void zoom_style_lengths(NodeStyle* s, float z) {
    s->font_size *= z; s->lh_px *= z; s->text_indent *= z; s->word_spacing *= z; s->outline_offset *= z; s->outline_w *= z;
    zoom_len(&s->text_indent_len, z); zoom_len(&s->word_spacing_len, z);
    s->gap_r *= z; s->gap_c *= z; s->spacing *= z;
    s->sh_x *= z; s->sh_y *= z; s->sh_blur *= z; s->sh_spread *= z;
    s->ts_x *= z; s->ts_y *= z; s->ts_blur *= z;
    for (int i = 0; i < 4; i++) {
        zoom_len(&s->margin[i], z); zoom_len(&s->padding[i], z); zoom_len(&s->inset[i], z);
        s->border_w[i] *= z; s->radius[i] *= z;
    }
    zoom_len(&s->width, z); zoom_len(&s->height, z); zoom_len(&s->min_w, z); zoom_len(&s->max_w, z);
    zoom_len(&s->column_width, z); zoom_len(&s->contain_intrinsic_w, z); zoom_len(&s->contain_intrinsic_h, z);
    s->column_rule_width *= z;
    zoom_len(&s->min_h, z); zoom_len(&s->max_h, z); zoom_len(&s->flex_basis, z);
    zoom_len(&s->bg_w, z); zoom_len(&s->bg_h, z); zoom_len(&s->bg_px, z); zoom_len(&s->bg_py, z);
    zoom_len(&s->mask.w, z); zoom_len(&s->mask.h, z); zoom_len(&s->mask.px, z); zoom_len(&s->mask.py, z);
    for (int i = 0; i < 4; i++) { s->clip.rad[i] *= z; zoom_fxlen(&s->clip.a[i], z); }
    for (int i = 0; i < s->clip.n && i < 16; i++) { zoom_fxlen(&s->clip.pt[i][0], z); zoom_fxlen(&s->clip.pt[i][1], z); }
    zoom_fxlen(&s->xf.ox, z); zoom_fxlen(&s->xf.oy, z);
    for (int i = 0; i < s->xf.n; i++) if (s->xf.op[i].kind == XF_TRANSLATE) {
        zoom_fxlen(&s->xf.op[i].t[0], z); zoom_fxlen(&s->xf.op[i].t[1], z);
    }
    zoom_fxlist(&s->filter, z); zoom_fxlist(&s->backdrop, z);
    /* Percentages are relative to the already-scaled containing box and stay unchanged. */
    s->zoom = 1.0f;
}
static Box* newbox(DOMNode* n, const NodeStyle* st, Box* parent) {
    Box* b = la(sizeof(Box)); b->n = n; b->parent = parent; b->slot = b->clipslot = -1; b->fb = -1e9f;
    if (n && GBOX && n->idx >= 0 && n->idx < NVN) GBOX[n->idx] = b;
    /* Anonymous inline wrappers reuse a style that has already been scaled. */
    if (parent && st == parent->st) {
        b->st = st; b->zoom_scale = parent->zoom_scale > 0 ? parent->zoom_scale : 1.0f; return b;
    }
    float parent_zoom = parent && parent->zoom_scale > 0 ? parent->zoom_scale : 1.0f;
    float own_zoom = st->zoom > 0 ? st->zoom : 1.0f;
    b->zoom_scale = parent_zoom * own_zoom;
    if (b->zoom_scale < 0.1f) b->zoom_scale = 0.1f;
    if (b->zoom_scale > 64.0f) b->zoom_scale = 64.0f;
    /* Keep the old zero-copy path for ordinary pages; zoomed boxes get a private
       style copy so line metrics and CSS-pixel lengths participate in layout. */
    if (fabsf(b->zoom_scale - 1.0f) <= 0.0001f) { b->st = st; return b; }
    NodeStyle* layout_style = la(sizeof(NodeStyle)); *layout_style = *st;
    zoom_style_lengths(layout_style, b->zoom_scale); b->st = layout_style; return b;
}
static void append(Box* p, Box* c) { c->parent = p; if (p->last) p->last->next = c; else p->first = c; p->last = c; }

static int clickable(const DOMNode* n) {
    return (n->type == TAG_A && n->href[0]) || n->type == TAG_BUTTON || n->type == TAG_INPUT || n->type == TAG_SELECT ||
           n->type == TAG_TEXTAREA || n->onclick[0] || (n->type == TAG_LABEL);
}

static const char* xform_text(Box* b) {
    const NodeStyle* st = b->st; const char* t = b->n->text;
    if (st->text_transform == TT_NONE) return t;
    size_t n = strlen(t); char* o = la(n + 1); int start = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i]; o[i] = (char)c;
        if (c < 0x80) {
            if (st->text_transform == TT_UPPER) o[i] = (char)toupper(c); else if (st->text_transform == TT_LOWER) o[i] = (char)tolower(c);
            else { o[i] = start ? (char)toupper(c) : (char)c; }
            start = isspace(c);
        } else if (c == 0xD0 || c == 0xD1) {                                   /* кириллица: базовые буквы */
            if (i + 1 < n) {
                unsigned char d = (unsigned char)t[i + 1]; o[i + 1] = (char)d; int up = st->text_transform == TT_UPPER || (st->text_transform == TT_CAP && start);
                if (up) { if (c == 0xD0 && d >= 0xB0 && d <= 0xBF) { o[i] = (char)0xD0; o[i + 1] = (char)(d - 0x20); }
                          else if (c == 0xD1 && d >= 0x80 && d <= 0x8F) { o[i] = (char)0xD0; o[i + 1] = (char)(d + 0x20); } }
                else if (st->text_transform == TT_LOWER) { if (c == 0xD0 && d >= 0x90 && d <= 0x9F) o[i + 1] = (char)(d + 0x20); else if (c == 0xD0 && d >= 0xA0 && d <= 0xAF) { o[i] = (char)0xD1; o[i + 1] = (char)(d - 0x20); } }
                i++; start = 0;
            }
        } else start = 0;
    }
    o[n] = 0; return o;
}

static int ctrl_kind(const DOMNode* n) {                                       /* 1 = поле/кнопка-input, 0 = не control */
    return n->type == TAG_INPUT || n->type == TAG_TEXTAREA || n->type == TAG_SELECT;
}
static int is_hidden_input(const DOMNode* n) { return n->type == TAG_INPUT && !strcmp(n->input_type, "hidden"); }

static void build_children(Box* parent, DOMNode* n, DOMNode* click);

/* Native <details> behavior: when closed, only its first <summary> subtree participates in layout.
   This also suppresses direct text nodes, which a CSS [hidden]-style rule alone cannot hide. */
static int hidden_by_closed_details(int idx) {
    if (!NV || idx < 0 || idx >= NVN) return 0;
    int child = idx, parent = NV[idx]->parent, guard = 0;
    while (parent >= 0 && parent < NVN && guard++ < 4096) {
        DOMNode* p = NV[parent];
        if (!strcmp(p->tag, "details")) {
            char open[8];
            if (!webpage_attr_get(p, "open", open, sizeof open)) {
                int summary = -1;
                for (int j = parent + 1; j <= p->subtree_end && j < NVN; j++) {
                    if (NV[j]->parent == parent && !strcmp(NV[j]->tag, "summary")) { summary = j; break; }
                }
                int direct = child, steps = 0;
                while (direct >= 0 && direct < NVN && NV[direct]->parent != parent && steps++ < 4096) direct = NV[direct]->parent;
                if (direct != summary) return 1;
            }
        }
        child = parent; parent = p->parent;
    }
    return 0;
}

static void build_elem(Box* parent, DOMNode* n, DOMNode* click, const PseudoBox* pseudo);
static void build_node(Box* parent, int idx, DOMNode* click) {
    if (hidden_by_closed_details(idx)) return;
    DOMNode* n = NV[idx];
    if (n->type == TAG_TEXT) {
        Box* b = newbox(n, &n->st, parent); b->kind = K_TEXT; b->text = xform_text(b); b->click = click; append(parent, b); return;
    }
    build_elem(parent, n, click, NULL);
}

/* ::before / ::after: фиктивный узел (живёт до следующей раскладки) + текстовый ребёнок с содержимым content */
static void build_pseudo(Box* parent, DOMNode* owner, int which, DOMNode* click) {
    const PseudoBox* pb = owner->pb[which]; if (!pb) return;
    DOMNode* pn = pa_alloc(sizeof(DOMNode)); if (!pn) return;
    pn->type = TAG_OTHER; snprintf(pn->tag, sizeof pn->tag, "%s", which ? "::after" : "::before");
    pn->idx = -1; pn->parent = owner->idx; pn->subtree_end = -1; pn->attr_w = pn->attr_h = -1; pn->colspan = pn->rowspan = 1;
    pn->st = pb->st; memcpy(pn->font_family, pb->font_family, sizeof pn->font_family);
    build_elem(parent, pn, click, pb);
}

static void build_elem(Box* parent, DOMNode* n, DOMNode* click, const PseudoBox* pseudo) {
    const NodeStyle* st = &n->st;
    if (st->display == DSP_NONE || is_hidden_input(n)) return;
    if (st->display == DSP_CONTENTS) { build_children(parent, n, clickable(n) ? n : click); return; }
    if (n->type == TAG_BR) { Box* b = newbox(n, st, parent); b->kind = K_BR; append(parent, b); return; }
    Box* b = newbox(n, st, parent);
    b->click = clickable(n) ? n : click;
    int repl = R_NONE;
    if (n->type == TAG_IMG) repl = R_IMG;
    else if (ctrl_kind(n)) repl = R_CTRL;
    else if (!strcmp(n->tag, "canvas")) { repl = R_CANVAS; snprintf(n->src, sizeof n->src, "canvas:%d", n->idx); }   /* см. images.c: webpage_image_inject — сюда JS кладёт пиксели через D.canvasCommit */
    else if (!strcmp(n->tag, "svg") && n->src[0]) repl = R_IMG;           /* встроенный svg: разметка сохранена в кэше, рисуется как картинка */
    else if (!strcmp(n->tag, "video")) { repl = R_VIDEO; snprintf(n->media_frame_key, sizeof n->media_frame_key, "video:%d", n->idx); }
    else if (!strcmp(n->tag, "iframe") || !strcmp(n->tag, "svg") || !strcmp(n->tag, "audio") ||
             !strcmp(n->tag, "object") || !strcmp(n->tag, "embed")) repl = R_PLACE;
    b->repl = (uint8_t)repl;
    if (st->position >= POS_ABSOLUTE) { b->oof = 1; b->kind = K_BLOCK; }
    else if (st->float_) { b->flt = 1; b->kind = K_BLOCK; }
    else if (repl) b->kind = (st->display == DSP_BLOCK || st->display == DSP_LIST_ITEM) ? K_BLOCK : K_ATOM;
    else switch (st->display) {
        case DSP_INLINE: b->kind = K_INLINE; break;
        case DSP_INLINE_BLOCK: case DSP_INLINE_FLEX: b->kind = K_ATOM; break;
        default: b->kind = K_BLOCK; break;
    }
    if (repl == R_IMG || repl == R_VIDEO) {
        const char* u = repl == R_VIDEO ? n->media_frame_key : n->src; ImgPix* px = u[0] ? webpage_image_get(u) : NULL; b->img = px;
        if (px) { b->nat_w = (float)px->w; b->nat_h = (float)px->h; }
        else { b->nat_w = n->attr_w > 0 ? (float)n->attr_w : (repl == R_VIDEO ? 300.0f : 0.0f); b->nat_h = n->attr_h > 0 ? (float)n->attr_h : (repl == R_VIDEO ? 150.0f : 0.0f); }
    }
    append(parent, b);
    if (pseudo) {
        if (pseudo->content[0]) {
            DOMNode* tn = pa_alloc(sizeof(DOMNode));
            if (tn) {
                tn->type = TAG_TEXT; snprintf(tn->tag, sizeof tn->tag, "#text"); tn->idx = -1; tn->parent = n->parent; tn->subtree_end = -1;
                snprintf(tn->text, sizeof tn->text, "%s", pseudo->content); tn->st = n->st; tn->st.display = DSP_INLINE; memcpy(tn->font_family, n->font_family, sizeof tn->font_family);
                Box* tb = newbox(tn, &tn->st, b); tb->kind = K_TEXT; tb->text = xform_text(tb); tb->click = b->click; append(b, tb);
            }
        }
    } else if (!repl && st->content_visibility != 2) build_children(b, n, b->click);
}

static void build_children(Box* parent, DOMNode* n, DOMNode* click) {
    if (n->pb[0]) build_pseudo(parent, n, 0, click);
    for (int i = n->idx + 1; i <= n->subtree_end; ) {
        DOMNode* c = NV[i];
        if (c->parent == n->idx) { build_node(parent, i, click); i = c->subtree_end + 1; } else i++;
    }
    if (n->pb[1]) build_pseudo(parent, n, 1, click);
}

static int is_block_level(const Box* c) { return c->kind == K_BLOCK && !c->oof && !c->flt; }
static int is_inline_level(const Box* c) { return c->kind == K_TEXT || c->kind == K_INLINE || c->kind == K_ATOM || c->kind == K_BR; }
static int ws_only(const Box* c) {
    if (c->kind != K_TEXT || c->st->white_space == WS_PRE || c->st->white_space == WS_PRE_WRAP) return 0;
    for (const char* p = c->text; *p; p++) if (!isspace((unsigned char)*p)) return 0; return 1;
}

static int is_flex_box(const Box* b) { return b && b->n && !b->anon && (b->st->display == DSP_FLEX || b->st->display == DSP_INLINE_FLEX || b->st->is_grid); }

static void normalize(Box* b) {
    for (Box* c = b->first; c; c = c->next) if (c->kind != K_TEXT && c->kind != K_BR) normalize(c);
    if (is_flex_box(b) && !b->repl) {                      /* каждый текстовый фрагмент — анонимный flex-элемент, inline-дети блокифицируются */
        Box* nf = NULL; Box* nl = NULL; Box* c = b->first;
        while (c) {
            Box* nx = c->next; c->next = NULL; Box* keep = c;
            if (c->kind == K_TEXT) { if (ws_only(c)) keep = NULL; else { Box* r = newbox(NULL, b->st, b); r->kind = K_BLOCK; r->anon = 1; r->first = r->last = c; c->parent = r; keep = r; } }
            else if (c->kind == K_BR) keep = NULL;
            else if (!c->oof && !c->flt && (c->kind == K_INLINE || c->kind == K_ATOM)) c->kind = K_BLOCK;
            if (keep) { if (nl) nl->next = keep; else nf = keep; nl = keep; }
            c = nx;
        }
        b->first = nf; b->last = nl; return;
    }
    if (b->kind != K_BLOCK || b->repl) return;
    int has_block = 0; for (Box* c = b->first; c; c = c->next) if (is_block_level(c)) { has_block = 1; break; }
    if (!has_block) return;
    Box* nf = NULL; Box* nl = NULL; Box* run = NULL; int run_content = 0;
    Box* c = b->first;
    while (c) {
        Box* nx = c->next; c->next = NULL;
        if (is_inline_level(c)) {
            if (!run) { run = newbox(NULL, b->st, b); run->kind = K_BLOCK; run->anon = 1; run_content = 0; }
            if (!ws_only(c)) run_content = 1;
            if (run->last) run->last->next = c; else run->first = c; run->last = c; c->parent = run;
        } else {
            if (run) { if (run_content) { if (nl) nl->next = run; else nf = run; nl = run; } run = NULL; }
            if (nl) nl->next = c; else nf = c; nl = c;
        }
        c = nx;
    }
    if (run && run_content) { if (nl) nl->next = run; else nf = run; nl = run; }
    b->first = nf; b->last = nl;
}

/* ══════════════ поля/размеры ══════════════ */
static void edges(Box* b, float cbw) {
    if (b->anon || !b->n || b->kind == K_TEXT) { b->mt = b->mr = b->mb = b->ml = b->pt = b->pr = b->pb = b->pl = b->bt = b->br = b->bb = b->bl = 0; b->mla = b->mra = 0; return; }
    const NodeStyle* s = b->st;
    b->mt = lenpx(&s->margin[0], cbw); b->mb = lenpx(&s->margin[2], cbw);
    b->mr = s->margin[1].set ? lenpx(&s->margin[1], cbw) : 0; b->ml = s->margin[3].set ? lenpx(&s->margin[3], cbw) : 0;
    b->mra = !s->margin[1].set; b->mla = !s->margin[3].set;
    if (b->kind == K_BLOCK && !b->flt && !b->oof && b->parent && b->parent->st && b->parent->st->center_blocks && b->ml == 0 && b->mr == 0) b->mla = b->mra = 1;   /* -webkit-center центрирует блоки */
    if (b->mforce) { b->ml = b->fml; b->mr = b->fmr; b->mla = b->mra = 0; }
    b->pt = lenpx(&s->padding[0], cbw); b->pr = lenpx(&s->padding[1], cbw); b->pb = lenpx(&s->padding[2], cbw); b->pl = lenpx(&s->padding[3], cbw);
    if (b->pt < 0) b->pt = 0; if (b->pr < 0) b->pr = 0; if (b->pb < 0) b->pb = 0; if (b->pl < 0) b->pl = 0;
    b->bt = s->border_w[0]; b->br = s->border_w[1]; b->bb = s->border_w[2]; b->bl = s->border_w[3];
}

int webpage_ctrl_default_size(DOMNode* n, float* w, float* h) {                 /* content-box */
    float fs = n->st.font_size; float cw = fs * 0.56f;
    const char* t = n->input_type;
    if (n->type == TAG_TEXTAREA) { *w = (n->cols > 0 ? n->cols : 20) * cw; *h = (n->rows > 0 ? n->rows : 2) * fs * 1.17f; return 1; }
    if (n->type == TAG_SELECT) {
        float mw = 0; int rows = n->size_attr > 1 ? n->size_attr : 1;
        for (int i = n->idx + 1; i <= n->subtree_end; i++) if (NV[i]->type == TAG_OPTION) { float l = 0; for (int k = i + 1; k <= NV[i]->subtree_end; k++) if (NV[k]->type == TAG_TEXT) l += (float)strlen(NV[k]->text) * cw; if (l > mw) mw = l; }
        *w = mw + fs * 1.6f; *h = rows * fs * 1.17f; return 1;
    }
    if (!strcmp(t, "checkbox") || !strcmp(t, "radio")) { *w = 13; *h = 13; return 1; }
    if (!strcmp(t, "range")) { *w = 129; *h = 16; return 1; }
    if (!strcmp(t, "color")) { *w = 50; *h = 27; return 1; }
    if (!strcmp(t, "file")) { *w = 220; *h = fs * 1.17f; return 1; }
    if (!strcmp(t, "submit") || !strcmp(t, "button") || !strcmp(t, "reset")) {
        const char* v = n->value[0] ? n->value : !strcmp(t, "submit") ? "Submit" : !strcmp(t, "reset") ? "Reset" : "";
        *w = (float)strlen(v) * cw; *h = fs * 1.17f; return 1;
    }
    if (!strcmp(t, "image")) { *w = n->attr_w > 0 ? (float)n->attr_w : 40; *h = n->attr_h > 0 ? (float)n->attr_h : 20; return 1; }
    *w = (n->size_attr > 0 ? n->size_attr : 20) * cw; *h = fs * 1.17f; return 1;
}

/* Размер контентной области замещаемого элемента. avail — ширина контейнера (для %), chd — определённая высота или -1 */
static void repl_size(Box* b, float avail, float chd, float* ow, float* oh) {
    const NodeStyle* s = b->st; float w = -1, h = -1, nw = 0, nh = 0;
    float ex_w = b->pl + b->pr + b->bl + b->br, ex_h = b->pt + b->pb + b->bt + b->bb;
    if (b->repl == R_IMG || b->repl == R_VIDEO) { nw = b->nat_w; nh = b->nat_h; }
    else if (b->repl == R_PLACE || b->repl == R_CANVAS) { nw = b->n->attr_w > 0 ? (float)b->n->attr_w : 300; nh = b->n->attr_h > 0 ? (float)b->n->attr_h : 150; if (b->repl == R_PLACE && !strcmp(b->n->tag, "svg") && b->n->attr_w <= 0) { nw = 0; nh = 0; } }
    else { webpage_ctrl_default_size(b->n, &nw, &nh); }
    int wp = s->width.set && s->width.pct != 0 && avail <= 0, hp = s->height.set && s->height.pct != 0 && chd < 0;
    if (s->width.set && !wp) { w = lenpx(&s->width, avail); if (s->box_sizing) w -= ex_w; }
    if (s->height.set && !hp) { h = lenpx(&s->height, chd >= 0 ? chd : 0); if (s->box_sizing) h -= ex_h; }
    if (b->repl != R_IMG && b->repl != R_VIDEO) { if (w < 0) w = nw; if (h < 0) h = nh; }
    else if (w < 0 && h < 0) { w = nw; h = nh; }
    else if (w < 0) w = nh > 0 ? h * nw / nh : nw;
    else if (h < 0) h = nw > 0 ? w * nh / nw : nh;
    if (s->aspect_ratio > 0.0f) {
        if (s->width.set && !s->height.set && w > 0) h = w / s->aspect_ratio;
        else if (s->height.set && !s->width.set && h > 0) w = h * s->aspect_ratio;
    }
    if (b->repl != R_CTRL && s->max_w.set && avail > 0) { float mw = lenpx(&s->max_w, avail) - (s->box_sizing ? ex_w : 0); if (w > mw) { if (!s->height.set && w > 0) h = h * mw / w; w = mw; } }
    if (b->repl != R_CTRL && s->min_w.set && avail > 0) { float mw = lenpx(&s->min_w, avail) - (s->box_sizing ? ex_w : 0); if (w < mw) { if (!s->height.set && w > 0) h = h * mw / w; w = mw; } }
    if (w < 0) w = 0; if (h < 0) h = 0;
    *ow = w; *oh = h;
}

/* ══════════════ поток строчных элементов: items ══════════════ */
enum { I_WORD, I_SPACE, I_BREAK, I_ATOM, I_OPEN, I_CLOSE, I_FLOAT, I_ABS };
typedef struct { uint8_t t, glue; Box* b; const char* s; int len; float w; int fs, fe; } Item;
typedef struct { Item* v; int n, cap; } IV;
static void iv_add(IV* iv, Item it) { if (iv->n == iv->cap) { iv->cap = iv->cap ? iv->cap * 2 : 64; iv->v = realloc(iv->v, (size_t)iv->cap * sizeof(Item)); } iv->v[iv->n++] = it; }

static int node_bidi_segment_scope(const DOMNode* node, TTFBidiScope* out) {
    uint8_t inner[TTF_BIDI_MAX_SCOPE_DEPTH];
    uintptr_t inner_ids[TTF_BIDI_MAX_SCOPE_DEPTH];
    unsigned count = 0;
    int guard = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    /* unicode-bidi is not inherited. Collect the explicit scopes from the text
     * owner to the root, then reverse them so ICU receives outer scopes first. */
    while (node && guard++ < 4096) {
        const NodeStyle* st = &node->st;
        uint8_t mode = TTF_BIDI_SEGMENT_NORMAL;
        switch (st->unicode_bidi) {
        case 1: mode = (uint8_t)(st->direction ? TTF_BIDI_SEGMENT_EMBED_RTL : TTF_BIDI_SEGMENT_EMBED_LTR); break;
        case 2: mode = (uint8_t)(st->direction ? TTF_BIDI_SEGMENT_ISOLATE_RTL : TTF_BIDI_SEGMENT_ISOLATE_LTR); break;
        case 3: mode = (uint8_t)(st->direction ? TTF_BIDI_SEGMENT_OVERRIDE_RTL : TTF_BIDI_SEGMENT_OVERRIDE_LTR); break;
        case 4: mode = (uint8_t)(st->direction ? TTF_BIDI_SEGMENT_ISOLATE_OVERRIDE_RTL : TTF_BIDI_SEGMENT_ISOLATE_OVERRIDE_LTR); break;
        case 5: mode = TTF_BIDI_SEGMENT_ISOLATE_FIRST_STRONG; break;
        default: break;
        }
        if (mode != TTF_BIDI_SEGMENT_NORMAL) {
            if (count >= TTF_BIDI_MAX_SCOPE_DEPTH) return 0;
            inner[count] = mode;
            inner_ids[count] = (uintptr_t)node;
            count++;
        }
        if (node->parent < 0 || node->parent >= NVN || !NV) break;
        node = NV[node->parent];
    }
    out->count = (uint8_t)count;
    for (unsigned i = 0; i < count; i++) {
        out->modes[i] = inner[count - i - 1];
        out->ids[i] = inner_ids[count - i - 1];
    }
    return 1;
}

/* Return true when node is node itself or a DOM descendant. */
static int bidi_node_within(const DOMNode* node, const DOMNode* ancestor) {
    int guard = 0;
    while (node && guard++ < 4096) {
        if (node == ancestor) return 1;
        if (!NV || node->parent < 0 || node->parent >= NVN) break;
        node = NV[node->parent];
    }
    return 0;
}

static int bidi_len_nonzero(const Len* l) {
    return l && (fabsf(l->px) > 0.01f || fabsf(l->pct) > 0.01f ||
                 fabsf(l->vw) > 0.01f || fabsf(l->vh) > 0.01f ||
                 fabsf(l->svw) > 0.01f || fabsf(l->svh) > 0.01f ||
                 fabsf(l->dvw) > 0.01f || fabsf(l->dvh) > 0.01f ||
                 fabsf(l->lh) > 0.01f);
}

/* Reposition text runs after line wrapping. The display-list and DOM order stay
 * logical (copy/selection rely on it), while x-coordinates are visual order.
 * An inline C_BOX can span multiple text runs when all runs between its first
 * and last descendant belong to that inline element. Before committing, the
 * pass verifies that each decorated fragment stays contiguous in visual order;
 * otherwise the whole line stays on the legacy path. */
static void bidi_reposition_line_commands(int cmd_start, int cmd_end, int base_direction) {
    if (cmd_start < 0 || cmd_end <= cmd_start || cmd_end > l2_dn) return;
    int count = 0, box_count = 0;
    for (int i = cmd_start; i < cmd_end; i++) {
        if (l2_dl[i].t == C_TEXT) count++;
        else if (l2_dl[i].t == C_BOX) box_count++;
        else if (l2_dl[i].t != C_HIT) return;
    }
    if (count < 2) return;

    const char** segments = malloc((size_t)count * sizeof(*segments));
    size_t* lengths = malloc((size_t)count * sizeof(*lengths));
    size_t* visual = malloc((size_t)count * sizeof(*visual));
    size_t* visual_pos = malloc((size_t)count * sizeof(*visual_pos));
    int* cmd_index = malloc((size_t)count * sizeof(*cmd_index));
    int* hit_index = malloc((size_t)count * sizeof(*hit_index));
    float* flow_pad_left = calloc((size_t)count, sizeof(*flow_pad_left));
    float* flow_pad_right = calloc((size_t)count, sizeof(*flow_pad_right));
    float* visual_pad_left = calloc((size_t)count, sizeof(*visual_pad_left));
    float* visual_pad_right = calloc((size_t)count, sizeof(*visual_pad_right));
    float* new_x = malloc((size_t)count * sizeof(*new_x));
    int* box_index = box_count ? malloc((size_t)box_count * sizeof(*box_index)) : NULL;
    int* box_first = box_count ? malloc((size_t)box_count * sizeof(*box_first)) : NULL;
    int* box_last = box_count ? malloc((size_t)box_count * sizeof(*box_last)) : NULL;
    float* box_left = box_count ? malloc((size_t)box_count * sizeof(*box_left)) : NULL;
    float* box_right = box_count ? malloc((size_t)box_count * sizeof(*box_right)) : NULL;
    TTFBidiScope* scopes = calloc((size_t)count, sizeof(*scopes));
    if (!segments || !lengths || !visual || !visual_pos || !cmd_index || !hit_index ||
        !flow_pad_left || !flow_pad_right || !visual_pad_left || !visual_pad_right || !new_x ||
        (box_count && (!box_index || !box_first || !box_last || !box_left || !box_right)) || !scopes) {
        free(segments); free(lengths); free(visual); free(visual_pos); free(cmd_index); free(hit_index);
        free(flow_pad_left); free(flow_pad_right); free(visual_pad_left); free(visual_pad_right); free(new_x);
        free(box_index); free(box_first); free(box_last); free(box_left); free(box_right); free(scopes);
        return;
    }

    int n = 0, nb = 0, eligible = 1;
    for (int i = cmd_start; i < cmd_end; i++) {
        Cmd* c = &l2_dl[i];
        if (c->t == C_BOX) { box_index[nb++] = i; continue; }
        if (c->t != C_TEXT) continue;
        if (!c->s || !c->s[0] || !c->node || c->node->idx < 0 || !isfinite(c->x) || !isfinite(c->w) || c->w < 0.0f) {
            eligible = 0; break;
        }
        segments[n] = c->s; lengths[n] = strlen(c->s); cmd_index[n] = i;
        if (!node_bidi_segment_scope(c->node, &scopes[n])) { eligible = 0; break; }
        hit_index[n] = (i + 1 < cmd_end && l2_dl[i + 1].t == C_HIT) ? i + 1 : -1;
        n++;
    }

    /* Measure each box against the full source-order run range it owns. The
     * fragment edges (F_NOLEFT/F_NORIGHT) deliberately carry no padding to a
     * new line; this allows a single inline element to span wrapped lines. */
    for (int b = 0; eligible && b < nb; b++) {
        Cmd* box = &l2_dl[box_index[b]];
        if (!box->node || !isfinite(box->x) || !isfinite(box->w) || !isfinite(box->h) || box->w < 0.0f || box->h < 0.0f ||
            bidi_len_nonzero(&box->node->st.margin[1]) || bidi_len_nonzero(&box->node->st.margin[3])) {
            eligible = 0; break;
        }
        int first = -1, last = -1, descendants = 0;
        float content_left = INFINITY, content_right = -INFINITY;
        for (int j = 0; j < n; j++) {
            Cmd* text = &l2_dl[cmd_index[j]];
            if (!bidi_node_within(text->node, box->node)) continue;
            if (first < 0) first = j;
            last = j; descendants++;
            if (text->x < content_left) content_left = text->x;
            if (text->x + text->w > content_right) content_right = text->x + text->w;
        }
        if (descendants == 0 || first < 0 || last < first || last - first + 1 != descendants ||
            !isfinite(content_left) || !isfinite(content_right)) {
            eligible = 0; break;
        }
        /* A box cannot safely wrap around another inline sibling, atomic item,
         * or untracked gap. Reject it rather than stretching it across them. */
        float le = (box->flags & F_NOLEFT) ? 0.0f : content_left - box->x;
        float re = (box->flags & F_NORIGHT) ? 0.0f : (box->x + box->w) - content_right;
        if (!isfinite(le) || !isfinite(re) || le < -1.0f || re < -1.0f || le > 4096.0f || re > 4096.0f) {
            eligible = 0; break;
        }
        if (le < 0.0f) le = 0.0f;
        if (re < 0.0f) re = 0.0f;
        box_first[b] = first; box_last[b] = last; box_left[b] = le; box_right[b] = re;
        if (le > flow_pad_left[first]) flow_pad_left[first] = le;
        if (re > flow_pad_right[last]) flow_pad_right[last] = re;
    }

    if (eligible) {
        /* Verify the pre-reorder line geometry. Horizontal margins and atomics
         * are unsupported, but line-local spaces already included in a text run
         * remain part of that run's measured width. */
        for (int i = 1; i < n; i++) {
            const Cmd* prev = &l2_dl[cmd_index[i - 1]];
            const Cmd* cur = &l2_dl[cmd_index[i]];
            float prev_edge = prev->x + prev->w + flow_pad_right[i - 1];
            float cur_edge = cur->x - flow_pad_left[i];
            if (fabsf(prev_edge - cur_edge) > 2.5f || fabsf(prev->y - cur->y) > 1.0f) {
                eligible = 0; break;
            }
        }
    }

    if (eligible && ttf_bidi_reorder_segments_scoped(segments, lengths, scopes, (size_t)n, base_direction, visual)) {
        for (int v = 0; v < n; v++) {
            if (visual[v] >= (size_t)n) { eligible = 0; break; }
            visual_pos[visual[v]] = (size_t)v;
        }
        /* A decorated fragment must stay contiguous after bidi resolution. A
         * split would require true fragment-tree painting (including split
         * borders/radii), so keep the original geometry in that case. */
        for (int b = 0; eligible && b < nb; b++) {
            size_t minv = (size_t)n, maxv = 0;
            for (int j = box_first[b]; j <= box_last[b]; j++) {
                if (visual_pos[j] < minv) minv = visual_pos[j];
                if (visual_pos[j] > maxv) maxv = visual_pos[j];
            }
            if (maxv < minv || maxv - minv + 1 != (size_t)(box_last[b] - box_first[b] + 1)) {
                eligible = 0; break;
            }
            int left_logical = (int)visual[minv];
            int right_logical = (int)visual[maxv];
            if (box_left[b] > visual_pad_left[left_logical]) visual_pad_left[left_logical] = box_left[b];
            if (box_right[b] > visual_pad_right[right_logical]) visual_pad_right[right_logical] = box_right[b];
        }
        if (eligible) {
            float left = INFINITY;
            for (int i = 0; i < n; i++) {
                float edge = l2_dl[cmd_index[i]].x - flow_pad_left[i];
                if (edge < left) left = edge;
            }
            if (isfinite(left)) {
                float x = left;
                for (int v = 0; v < n; v++) {
                    size_t logical = visual[v];
                    Cmd* c = &l2_dl[cmd_index[logical]];
                    new_x[logical] = x + visual_pad_left[logical];
                    x += visual_pad_left[logical] + c->w + visual_pad_right[logical];
                }
                /* Commit only after all validation has completed. */
                for (int i = 0; i < n; i++) {
                    Cmd* c = &l2_dl[cmd_index[i]];
                    float dx = new_x[i] - c->x;
                    c->x = new_x[i];
                    if (hit_index[i] >= 0) l2_dl[hit_index[i]].x += dx;
                }
                for (int b = 0; b < nb; b++) {
                    Cmd* box = &l2_dl[box_index[b]];
                    float frag_left = INFINITY, frag_right = -INFINITY;
                    for (int j = box_first[b]; j <= box_last[b]; j++) {
                        Cmd* text = &l2_dl[cmd_index[j]];
                        if (text->x < frag_left) frag_left = text->x;
                        if (text->x + text->w > frag_right) frag_right = text->x + text->w;
                    }
                    if (isfinite(frag_left) && isfinite(frag_right) && frag_right >= frag_left) {
                        box->x = frag_left - box_left[b];
                        box->w = (frag_right + box_right[b]) - box->x;
                    }
                }
            }
        }
    }

    free(segments); free(lengths); free(visual); free(visual_pos); free(cmd_index); free(hit_index);
    free(flow_pad_left); free(flow_pad_right); free(visual_pad_left); free(visual_pad_right); free(new_x);
    free(box_index); free(box_first); free(box_last); free(box_left); free(box_right); free(scopes);
}

static float intr(Box* b, int mode);
static void lbb(Box* b, float x, float y, float avail_w, float forced_w, float forced_h, float ch_def, LS* ls);
static float layout_flow(Box* b, float cx, float cy, float cw, float ch, LS* ls);
static float layout_flex(Box* b, float cx, float cy, float cw, float ch, LS* ls);
static float layout_table(Box* b, float cx, float cy, float cw, LS* ls);
static float layout_grid(Box* b, float cx, float cy, float cw, float ch, LS* ls);
static float table_intr(Box* b, int mode);

typedef struct { float maxw; int measure; float avail; LS* ls; } FCfg;

static void flatten(Box* c, IV* iv, FCfg* cfg, int* last_space) {
    for (Box* ch = c->first; ch; ch = ch->next) {
        if (ch->kind == K_TEXT) {
            FM* m = fm_of(ch); int ws = ch->st->white_space; const char* p = ch->text;
            int pre = ws == WS_PRE || ws == WS_PRE_WRAP;
            while (*p) {
                unsigned char cc = (unsigned char)*p;
                if (cc == '\n' && (pre || ws == WS_PRE_LINE)) { Item it = {I_BREAK, 0, ch, p, 1, 0, 0, 0}; iv_add(iv, it); *last_space = 0; p++; continue; }
                int isw = cc == ' ' || cc == '\t' || cc == '\n' || cc == '\r' || cc == '\f';
                if (isw) {
                    const char* e = p; while (*e && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\f' || (*e == '\n' && !(pre || ws == WS_PRE_LINE)))) e++;
                    if (pre) { int cnt = 0; for (const char* q = p; q < e; q++) cnt += *q == '\t' ? 8 : 1; Item it = {I_WORD, 0, ch, p, (int)(e - p), text_w(m, " ", 1) * cnt, 0, 0}; it.s = NULL; iv_add(iv, it); iv->v[iv->n - 1].len = -cnt; *last_space = 0; }
                    else if (!*last_space) { float ws = ch->st->word_spacing_len.set ? lenpx(&ch->st->word_spacing_len, cfg->avail) : ch->st->word_spacing; Item it = {I_SPACE, 0, ch, p, (int)(e - p), fmaxf2(0.0f, text_w(m, " ", 1) + ws), 0, 0}; iv_add(iv, it); *last_space = 1; }
                    p = e; if (p == e && e == p && cc == '\n' && !pre && ws != WS_PRE_LINE) {}
                    continue;
                }
                const char* e = p; while (*e && !(*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r' || *e == '\f')) e++;
                float w = text_w(m, p, (int)(e - p));
                int glue = iv->n > 0 && !*last_space && (iv->v[iv->n - 1].t == I_WORD || iv->v[iv->n - 1].t == I_OPEN || iv->v[iv->n - 1].t == I_CLOSE);
                if (!cfg->measure && ch->st->break_word && cfg->maxw > 8 && w > cfg->maxw) {           /* разрываем длинное слово */
                    const char* q = p;
                    while (q < e) {
                        const char* r = q; float acc = 0; const char* last = q;
                        while (r < e) { const char* nx = r + 1; while (nx < e && ((unsigned char)*nx & 0xC0) == 0x80) nx++; float cw2 = text_w(m, r, (int)(nx - r)); if (acc + cw2 > cfg->maxw && r > q) break; acc += cw2; r = nx; last = r; }
                        Item it = {I_WORD, 0, ch, q, (int)(last - q), acc, 0, 0}; iv_add(iv, it); q = last;
                    }
                } else { Item it = {I_WORD, (uint8_t)glue, ch, p, (int)(e - p), w, 0, 0}; iv_add(iv, it); }
                *last_space = 0; p = e;
            }
        } else if (ch->kind == K_BR) {
            Item it = {I_BREAK, 0, ch, NULL, 0, 0, 0, 0}; iv_add(iv, it); *last_space = 0;
        } else if (ch->kind == K_INLINE) {
            edges(ch, cfg->avail);
            Item o = {I_OPEN, 0, ch, NULL, 0, ch->ml + ch->bl + ch->pl, 0, 0}; iv_add(iv, o);
            flatten(ch, iv, cfg, last_space);
            Item cl = {I_CLOSE, 0, ch, NULL, 0, ch->pr + ch->br + ch->mr, 0, 0}; iv_add(iv, cl);
        } else if (ch->kind == K_ATOM) {
            edges(ch, cfg->avail);
            if (cfg->measure) { Item it = {I_ATOM, 0, ch, NULL, 0, intr(ch, 1) + ch->ml + ch->mr, 0, 0}; iv_add(iv, it); }
            else {
                int s = l2_dn; lbb(ch, 0, 0, cfg->avail, -1, -1, -1, cfg->ls);
                Item it = {I_ATOM, 0, ch, NULL, 0, ch->ml + ch->w + ch->mr, s, l2_dn}; iv_add(iv, it);
            }
            *last_space = 0;
        } else if (ch->kind == K_BLOCK && (ch->flt || ch->oof) && !cfg->measure) {
            if (ch->flt) { edges(ch, cfg->avail); int s = l2_dn; lbb(ch, 0, 0, cfg->avail, -1, -1, -1, cfg->ls); Item it = {I_FLOAT, 0, ch, NULL, 0, 0, s, l2_dn}; iv_add(iv, it); }
            else { Item it = {I_ABS, 0, ch, NULL, 0, 0, 0, 0}; iv_add(iv, it); }
        }
    }
}

/* min / max-content ширина потока строчных элементов (без переносов = сумма, min = самое длинное слово) */
static float measure_inline(Box* c, int mode) {
    IV iv = {0}; FCfg cfg = {0, 1, 0, NULL}; int ls_ = 1;
    flatten(c, &iv, &cfg, &ls_);
    float best = 0, cur = 0;
    for (int i = 0; i < iv.n; i++) {
        Item* it = &iv.v[i];
        if (it->t == I_BREAK) { if (cur > best) best = cur; cur = 0; continue; }
        if (mode == 0) {                                          /* min-content */
            if (it->t == I_WORD || it->t == I_ATOM) { if (it->w > best) best = it->w; }
        } else cur += it->w;
    }
    if (cur > best) best = cur;
    free(iv.v);
    return best;
}

/* ── float-контекст ── */
static void fc_range(FCtx* fc, float y0, float y1, float* L, float* R) {
    for (int i = 0; i < fc->nl; i++) if (fc->l[i].y1 > y0 && fc->l[i].y0 < y1 && fc->l[i].x1 > *L) *L = fc->l[i].x1;
    for (int i = 0; i < fc->nr; i++) if (fc->r[i].y1 > y0 && fc->r[i].y0 < y1 && fc->r[i].x0 < *R) *R = fc->r[i].x0;
}
static float fc_bottom(FCtx* fc, int mask) {
    float b = -1e9f;
    if (mask & 1) for (int i = 0; i < fc->nl; i++) if (fc->l[i].y1 > b) b = fc->l[i].y1;
    if (mask & 2) for (int i = 0; i < fc->nr; i++) if (fc->r[i].y1 > b) b = fc->r[i].y1;
    return b;
}
static float fc_next_bottom(FCtx* fc, float y0, float y1) {
    float nb = 1e9f;
    for (int i = 0; i < fc->nl; i++) if (fc->l[i].y1 > y0 && fc->l[i].y0 < y1 && fc->l[i].y1 < nb) nb = fc->l[i].y1;
    for (int i = 0; i < fc->nr; i++) if (fc->r[i].y1 > y0 && fc->r[i].y0 < y1 && fc->r[i].y1 < nb) nb = fc->r[i].y1;
    return nb;
}

/* Ставит уже разложенный в (0,0) float-бокс на место. cx0/cx1 — границы контейнера. */
static void place_float(Box* f, int s, int e, LS* ls, float cx0, float cx1, float y) {
    FCtx* fc = ls->fc; if (!fc) return;
    float mw = f->ml + f->w + f->mr, mh = f->mt + f->h + f->mb; int left = f->st->float_ == FLT_LEFT;
    float fy = y; float L, R;
    for (int guard = 0; guard < 40; guard++) {
        L = cx0; R = cx1; fc_range(fc, fy, fy + mh, &L, &R);
        if (R - L >= mw - 0.01f) break;
        float nb = fc_next_bottom(fc, fy, fy + mh); if (nb > 1e8f) break; fy = nb;
    }
    float fx = left ? L : R - mw;
    dl_shift(s, e, fx, fy + f->mt); f->x += fx; f->y += fy + f->mt;
    FRect r = {fx, fy, fx + mw, fy + mh};
    if (left) { if (fc->nl < FCMAX) fc->l[fc->nl++] = r; } else if (fc->nr < FCMAX) fc->r[fc->nr++] = r;
}

static void register_abs(LS* ls, Box* b, float sx, float sy) {
    Box* o = ls->owner; if (!o) return;
    AbsRef* r = la(sizeof(AbsRef)); r->b = b; r->sx = sx; r->sy = sy;
    if (o->abs_tail) o->abs_tail->next = r; else o->abs = r; o->abs_tail = r;
}

/* ══════════════ строчный поток ══════════════ */
typedef struct { Box* b; int slot; float x0; int cont; } OpenFrag;

static int has_visual_box(const NodeStyle* s) {
    return (s->bg >> 24) != 0 || s->border_w[0] > 0 || s->border_w[1] > 0 || s->border_w[2] > 0 || s->border_w[3] > 0 || ((s->sh_color >> 24) != 0);
}

static int column_metrics(const NodeStyle* st, float cw, float* gap_out, float* width_out) {
    int ncol = st->column_count;
    float gap = st->gap_c > 0 ? st->gap_c : st->font_size;
    float pref = st->column_width.set ? lenpx(&st->column_width, cw) : 0;
    if (ncol < 1 && pref > 0) { ncol = (int)((cw + gap) / (pref + gap)); if (ncol < 1) ncol = 1; if (ncol > 32) ncol = 32; }
    if (ncol < 1) ncol = 1;
    if (ncol > 32) ncol = 32;
    if (ncol == 1 && pref <= 0) { if (gap_out) *gap_out = gap; if (width_out) *width_out = cw; return 1; }
    float colw = (cw - gap * (ncol - 1)) / ncol;
    if (colw < 1) colw = 1;
    if (gap_out) *gap_out = gap;
    if (width_out) *width_out = colw;
    return ncol;
}
static void column_rule_emit(const NodeStyle* st, float x, float y, float colw, float gap, int ncol, float h) {
    if (!st || ncol < 2 || st->column_rule_width <= 0 || st->column_rule_style == BS_NONE || !(st->column_rule_color >> 24)) return;
    for (int i = 0; i < ncol - 1; i++) {
        Cmd q; memset(&q,0,sizeof q); q.t=C_RECT; q.x=x+(i+1)*colw+i*gap+(gap-st->column_rule_width)*0.5f; q.y=y; q.w=st->column_rule_width; q.h=h; q.color=st->column_rule_color; dl_add(q);
    }
}
static size_t utf8_unit(const unsigned char* s, size_t n) {
    if (!n) return 0;
    unsigned char c=s[0]; size_t k=c>=0xF0?4:c>=0xE0?3:c>=0xC0?2:1;
    return k>n?n:k;
}
/* Vertical writing baseline: one glyph per vertical advance, columns progress according
   to vertical-rl / vertical-lr. This is real vertical layout; glyph rotation and complex
   grapheme shaping remain intentionally separate limitations. */
static float layout_inline_vertical(Box* c, IV* iv, float x, float y, float w, LS* ls) {
    (void)ls;
    const NodeStyle* cs=c->st; FM* rootfm=fm_of(c);
    float adv=rootfm->lh>1?rootfm->lh:(float)rootfm->size*1.2f;
    float limit=cs->height.set?lenpx(&cs->height, VH):(VH-y);
    if(limit<adv)limit=adv;
    float col_x=(cs->writing_mode==1)?x+w-adv:x;
    float ycur=y, max_y=y, max_x=x+w;
    int rtl_columns=(cs->writing_mode==1), col=0;
    for(int i=0;i<iv->n;i++){
        Item* it=&iv->v[i];
        if(it->t==I_OPEN || it->t==I_CLOSE){
            /* Vertical margin/padding on inline fragments is approximated on the inline axis. */
            if(it->t==I_OPEN) ycur += it->b->ml + it->b->pl + it->b->bl;
            else ycur += it->b->pr + it->b->br + it->b->mr;
            continue;
        }
        if(it->t==I_FLOAT || it->t==I_ABS) {
            if(it->t==I_ABS) register_abs(ls,it->b,col_x,ycur);
            else place_float(it->b,it->fs,it->fe,ls,x,x+w,ycur);
            continue;
        }
        if(it->t==I_BREAK){
            col++; col_x=rtl_columns?x+w-(col+1)*adv:x+col*adv; ycur=y; continue;
        }
        if(it->t==I_ATOM){
            Box* a=it->b;
            if(ycur+a->h>y+limit && ycur>y){col++;col_x=rtl_columns?x+w-(col+1)*adv:x+col*adv;ycur=y;}
            dl_shift(it->fs,it->fe,col_x+(adv-a->w)*0.5f-a->x,ycur-a->y);
            a->x=col_x+(adv-a->w)*0.5f; a->y=ycur;
            ycur += a->h>adv?a->h:adv; if(ycur>max_y)max_y=ycur;
            continue;
        }
        const char* src=it->s; size_t len=src?(it->len>0?(size_t)it->len:0):(it->len<0?(size_t)-it->len:0);
        if(!src && it->len<0){
            for(size_t q=0;q<len;q++){
                if(ycur+adv>y+limit && ycur>y){col++;col_x=rtl_columns?x+w-(col+1)*adv:x+col*adv;ycur=y;}
                FM* fm=fm_of(it->b); char sp=' '; float tw=text_w(fm,&sp,1);
                char* ss=sa(2);if(ss){ss[0]=' ';ss[1]=0;Cmd cm;memset(&cm,0,sizeof cm);cm.t=C_TEXT;cm.x=col_x+(adv-tw)*0.5f;cm.y=ycur+(adv-(fm->asc+fm->desc))*0.5f;cm.w=tw;cm.h=(float)(fm->asc+fm->desc);cm.node=it->b->n;cm.s=ss;cm.fam=fam_of(it->b);cm.size=fm->size;cm.color=it->b->st->color;cm.tstyle=(uint8_t)fm->style;cm.weight=(uint16_t)fm->weight;cm.ital=(uint8_t)fm->ital;cm.deco=it->b->st->deco;cm.opacity=it->b->st->opacity;if(it->b->st->visibility==0)dl_add(cm);}
                ycur+=adv;if(ycur>max_y)max_y=ycur;
            }
            continue;
        }
        if(!src || !len)continue;
        if(it->b->st->text_combine_upright && len>=2 && len<=4) {
            int digits=1; for(size_t z=0;z<len;z++) if(src[z]<'0'||src[z]>'9') digits=0;
            if(digits) {
                if(ycur+adv>y+limit && ycur>y){col++;col_x=rtl_columns?x+w-(col+1)*adv:x+col*adv;ycur=y;}
                FM* fm=fm_of(it->b); float tw=text_w(fm,src,(int)len); char* ss=sa(len+1);
                if(ss){memcpy(ss,src,len);ss[len]=0;Cmd cm;memset(&cm,0,sizeof cm);cm.t=C_TEXT;cm.x=col_x+(adv-tw)*0.5f;cm.y=ycur+(adv-(fm->asc+fm->desc))*0.5f;cm.w=tw;cm.h=(float)(fm->asc+fm->desc);cm.node=it->b->n;cm.s=ss;cm.fam=fam_of(it->b);cm.size=fm->size;cm.color=it->b->st->color;cm.tstyle=(uint8_t)fm->style;cm.weight=(uint16_t)fm->weight;cm.ital=(uint8_t)fm->ital;cm.deco=it->b->st->deco;cm.opacity=it->b->st->opacity;if(it->b->st->visibility==0)dl_add(cm);}
                ycur+=adv;if(ycur>max_y)max_y=ycur;continue;
            }
        }
        size_t q=0;
        while(q<len){
            size_t unit=utf8_unit((const unsigned char*)src+q,len-q);
            if(!unit)break;
            if(ycur+adv>y+limit && ycur>y){col++;col_x=rtl_columns?x+w-(col+1)*adv:x+col*adv;ycur=y;}
            FM* fm=fm_of(it->b); float tw=text_w(fm,src+q,(int)unit);
            char* ss=sa(unit+1);
            if(ss){memcpy(ss,src+q,unit);ss[unit]=0;Cmd cm;memset(&cm,0,sizeof cm);cm.t=C_TEXT;cm.x=col_x+(adv-tw)*0.5f;cm.y=ycur+(adv-(fm->asc+fm->desc))*0.5f;cm.w=tw;cm.h=(float)(fm->asc+fm->desc);cm.node=it->b->n;cm.s=ss;cm.fam=fam_of(it->b);cm.size=fm->size;cm.color=it->b->st->color;cm.tstyle=(uint8_t)fm->style;cm.weight=(uint16_t)fm->weight;cm.ital=(uint8_t)fm->ital;cm.deco=it->b->st->deco;cm.opacity=it->b->st->opacity;if(it->b->st->visibility==0)dl_add(cm);if(it->b->click){Cmd h;memset(&h,0,sizeof h);h.t=C_HIT;h.x=cm.x;h.y=cm.y;h.w=cm.w;h.h=cm.h;h.node=it->b->click;dl_add(h);}}
            ycur+=adv;if(ycur>max_y)max_y=ycur;q+=unit;
        }
    }
    if(max_x<x+(col+1)*adv)max_x=x+(col+1)*adv;
    free(iv->v);iv->v=NULL;iv->n=iv->cap=0;
    return max_y-y;
}

static float layout_inline(Box* c, float x, float y, float w, LS* ls) {
    IV iv = {0}; FCfg cfg = {w, 0, w, ls}; int last_sp = 1;
    flatten(c, &iv, &cfg, &last_sp);
    FM* strut = fm_of(c); const NodeStyle* cs = c->st;
    if (cs->writing_mode != 0) return layout_inline_vertical(c, &iv, x, y, w, ls);
    float colgap = 0, colw = w; int ncols = column_metrics(cs, w, &colgap, &colw);
    int multicol = ncols > 1 && iv.n > 0; float xbase = x, ybase = y;
    int lines_per_col = 0; float col_target_h = 0; int auto_fill = cs->column_fill == 1;
    if (multicol) {
        if (cs->height.set) col_target_h = lenpx(&cs->height, VH);
        if (auto_fill && !cs->height.set) lines_per_col = 0x3fffffff; /* unconstrained auto-fill keeps the stream in one column */
        else if (col_target_h > 0 && auto_fill) lines_per_col = 0x3fffffff;
        else {
            /* Estimate total line count for balance. Width estimates are token based; actual shaping still decides breaks. */
            int lines = 1; float run = 0;
            for (int z=0; z<iv.n; z++) {
                Item* it=&iv.v[z];
                if (it->t==I_BREAK) { lines++; run=0; continue; }
                if (it->t==I_SPACE) { run += it->w; continue; }
                if (it->t!=I_WORD && it->t!=I_ATOM) continue;
                if (run>0 && run+it->w>colw) { lines++; run=it->w; } else run+=it->w;
            }
            lines_per_col=(lines+ncols-1)/ncols; if(lines_per_col<1)lines_per_col=1;
            if (col_target_h<=0) col_target_h=lines_per_col*(strut->lh>0?strut->lh:cs->font_size*1.2f);
        }
    }
    float cy = y; int i = 0; int ni = iv.n; int first_line = 1; int line_no = 0;
    int col = 0, col_line = 0, col_used = 1; float col_max_h = 0;
    OpenFrag stack[24]; int nst = 0;
    char* rbuf = NULL; size_t rcap = 0;

    while (i < ni) {
        if (cs->line_clamp && line_no >= cs->line_clamp) break;
        while (i < ni && iv.v[i].t == I_SPACE) i++;
        while (i < ni && (iv.v[i].t == I_FLOAT || iv.v[i].t == I_ABS)) {
            Item* it = &iv.v[i];
            if (it->t == I_FLOAT) place_float(it->b, it->fs, it->fe, ls, x, x + w, cy); else register_abs(ls, it->b, x, cy);
            i++; while (i < ni && iv.v[i].t == I_SPACE) i++;
        }
        if (i >= ni) break;
        float balance_limit=0;
        if (cs->text_wrap==2 || cs->text_wrap==3) {
            float total=0, widest=0; int p_end=i;
            for(;p_end<ni && iv.v[p_end].t!=I_BREAK;p_end++) {
                Item* pi=&iv.v[p_end];
                if(pi->t==I_WORD || pi->t==I_ATOM){total+=pi->w;if(pi->w>widest)widest=pi->w;}
                else if(pi->t==I_SPACE) total+=pi->w;
            }
            float avail=w-(line_no==0?(cs->text_indent_len.set?lenpx(&cs->text_indent_len,w):cs->text_indent):0);
            if(avail<1)avail=w>1?w:1;
            int lines=(int)ceilf(total/avail);if(lines<1)lines=1;
            balance_limit=total/lines;if(balance_limit<widest)balance_limit=widest;if(balance_limit>avail)balance_limit=avail;
        }
        float L = x, R = x + w; if (ls->fc) fc_range(ls->fc, cy, cy + strut->lh, &L, &R); if (line_no == 0) L += cs->text_indent_len.set ? lenpx(&cs->text_indent_len, w) : cs->text_indent;
        float wrap_limit=(R-L); if(balance_limit>0 && balance_limit<wrap_limit)wrap_limit=balance_limit;
        float lw = 0; int j = i, brk_end = -1, brk_next = -1; float brk_lw = 0; int forced = 0, jend = -1, next_i = -1;
        float lw_at_open_ok = 0; (void)lw_at_open_ok;
        for (; j < ni; j++) {
            Item* it = &iv.v[j];
            if (it->t == I_BREAK) { jend = j; next_i = j + 1; forced = 1; break; }
            if (it->t == I_FLOAT) { place_float(it->b, it->fs, it->fe, ls, x, x + w, cy); L = x; R = x + w; if (ls->fc) fc_range(ls->fc, cy, cy + strut->lh, &L, &R); continue; }
            if (it->t == I_ABS) { register_abs(ls, it->b, x + lw, cy); continue; }
            if (it->t == I_SPACE) {
                if (j > i && lw > 0) { brk_end = j; brk_next = j + 1; brk_lw = lw; }
                lw += it->w; continue;
            }
            int wrap_ok = it->b->st->text_wrap != 1 && it->b->st->white_space != WS_NOWRAP && it->b->st->white_space != WS_PRE;
            if ((it->t == I_WORD || it->t == I_ATOM) && wrap_ok && lw > 0 && lw + it->w > wrap_limit + 0.01f) {
                if (it->t == I_ATOM || !it->glue) { if (iv.v[j - 1].t != I_SPACE) { jend = j; next_i = j; } else { jend = j - 1; next_i = j; } goto line_done; }
                if (brk_end > i) { jend = brk_end; next_i = brk_next; lw = brk_lw; goto line_done; }
            }
            if ((it->t == I_WORD || it->t == I_ATOM) && wrap_ok && j > i && !it->glue && iv.v[j - 1].t != I_SPACE) { brk_end = j; brk_next = j; brk_lw = lw; }
            lw += it->w;
        }
        if (jend < 0) { jend = j; next_i = j; }
    line_done:
        {
        /* содержимое строки [i, jend): убираем хвостовые пробелы */
        int e = jend; while (e > i && iv.v[e - 1].t == I_SPACE) { lw -= iv.v[e - 1].w; e--; }
        if (jend != j && !forced && next_i == brk_next && brk_end == jend) { /* lw уже = brk_lw */ }
        /* метрики строки */
        float ab_max = 0, be_max = 0; float su = strut->lh, hl0 = (strut->lh - (strut->asc + strut->desc)) / 2;
        ab_max = strut->asc + hl0; be_max = strut->desc + hl0; (void)su;
        float top_need = 0, bot_need = 0;
        for (int k = i; k < e; k++) {
            Item* it = &iv.v[k]; float ab, be;
            if (it->t == I_WORD) { FM* m = fm_of(it->b); float hl = (m->lh - (m->asc + m->desc)) / 2; float off = it->b->st->vertical_align == VA_SUB ? 0.2f * m->size : it->b->st->vertical_align == VA_SUPER ? -0.33f * m->size : 0;
                ab = m->asc + hl - off; be = m->desc + hl + off; if (it->b->st->vertical_align == VA_TOP) { if (m->lh > top_need) top_need = m->lh; continue; } if (it->b->st->vertical_align == VA_BOTTOM) { if (m->lh > bot_need) bot_need = m->lh; continue; } }
            else if (it->t == I_ATOM) {
                Box* a = it->b; float oh = a->mt + a->h + a->mb; float fbrel = a->fb > -1e8f ? a->fb - a->y : -1e9f;
                ab = fbrel > -1e8f ? a->mt + fbrel : oh; if (a->repl == R_IMG || a->repl == R_PLACE || a->repl == R_CANVAS) ab = oh;
                if (a->st->vertical_align == VA_MIDDLE) ab = oh / 2 + 0.27f * strut->size;
                else if (a->st->vertical_align == VA_TOP) { if (oh > top_need) top_need = oh; continue; }
                else if (a->st->vertical_align == VA_BOTTOM) { if (oh > bot_need) bot_need = oh; continue; }
                be = oh - ab;
            } else continue;
            if (ab > ab_max) ab_max = ab; if (be > be_max) be_max = be;
        }
        float lh = ab_max + be_max;
        if (top_need > lh) { be_max += top_need - lh; lh = top_need; } if (bot_need > lh) { ab_max += bot_need - lh; lh = bot_need; }
        float base = cy + ab_max;
        if (first_line) { c->fb = base; first_line = 0; }
        /* выравнивание */
        L = x; R = x + w; if (ls->fc) fc_range(ls->fc, cy, cy + lh, &L, &R); if (line_no == 0) L += cs->text_indent_len.set ? lenpx(&cs->text_indent_len, w) : cs->text_indent;
        float space = (R - L) - lw, sx = L; int nsp = 0;
        int last_line = forced || jend >= ni;
        int line_align = cs->text_align;
        if (line_align == ALIGN_START) line_align = cs->direction ? ALIGN_RIGHT : ALIGN_LEFT;
        else if (line_align == ALIGN_END) line_align = cs->direction ? ALIGN_LEFT : ALIGN_RIGHT;
        if (line_align == ALIGN_CENTER) sx = L + (space > 0 ? space / 2 : 0); else if (line_align == ALIGN_RIGHT) sx = L + (space > 0 ? space : 0);
        float jextra = 0;
        if (cs->text_align == ALIGN_JUSTIFY && !last_line && space > 0) { for (int k = i; k < e; k++) if (iv.v[k].t == I_SPACE) nsp++; if (nsp) jextra = space / nsp; }
        /* эмиссия */
        float cx = sx; size_t rl = 0; float rx = 0; Box* rb = NULL; int rstart_k = -1; (void)rstart_k;
        const char* run_source_start = NULL; const char* run_source_next = NULL; int run_source_valid = 1;
        int clamp_ellipsis = cs->line_clamp && line_no + 1 >= cs->line_clamp && next_i < ni;
        int bd = cs->unicode_bidi == 5 ? -1 : (cs->direction ? 1 : 0);
        int line_cmd_start = l2_dn;
        int at_line_end = 0, overflow_ellipsis = 0;
        #define FLUSH_RUN() do { if (rb && rl > 0) { \
                FM* m = fm_of(rb); float off = rb->st->vertical_align == VA_SUB ? 0.2f * m->size : rb->st->vertical_align == VA_SUPER ? -0.33f * m->size : 0; \
                float ty = base + off - m->asc; \
                if (rb->st->vertical_align == VA_TOP) ty = cy - (m->lh - (m->asc + m->desc)) / 2 * 0 ; \
                int add_ell = at_line_end && clamp_ellipsis && !overflow_ellipsis; char* sbuf = sa(rl + (add_ell ? 4 : 1)); memcpy(sbuf, rbuf, rl); if (add_ell) { memcpy(sbuf + rl, "…", 3); rl += 3; } sbuf[rl] = 0; \
                Cmd cm; memset(&cm, 0, sizeof cm); cm.t = C_TEXT; cm.x = rx; cm.y = ty; cm.w = cx - rx; cm.h = (float)(m->asc + m->desc); cm.node = rb->n; cm.s = sbuf; cm.fam = fam_of(rb); \
                cm.size = m->size; cm.color = rb->st->color; cm.tstyle = (uint8_t)m->style; cm.weight = (uint16_t)m->weight; cm.ital = (uint8_t)m->ital; cm.deco = rb->st->deco; cm.opacity = rb->st->opacity; \
                if (run_source_valid && run_source_start && !add_ell && rb->text) { ptrdiff_t so = run_source_start - rb->text; if (so >= 0 && (size_t)so <= strlen(rb->text) && (size_t)so <= UINT32_MAX) { cm.text_offset = (uint32_t)so; cm.text_offset_valid = 1; } } \
                if (rb->st->visibility == 0) dl_add(cm); \
                if (rb->click) { Cmd h; memset(&h, 0, sizeof h); h.t = C_HIT; h.x = rx; h.y = ty; h.w = cx - rx; h.h = cm.h; h.node = rb->click; dl_add(h); } \
                } rl = 0; rb = NULL; run_source_start = run_source_next = NULL; run_source_valid = 1; } while (0)
        for (int k = i; k < e; k++) {
            Item* it = &iv.v[k];
            switch (it->t) {
            case I_OPEN: {
                FLUSH_RUN();
                cx += it->b->ml; float x0 = cx; cx += it->b->bl + it->b->pl;
                if (nst < 24) { OpenFrag* f = &stack[nst++]; f->b = it->b; f->x0 = x0; f->cont = 0; f->slot = -1;
                    if (has_visual_box(it->b->st)) { Cmd b; memset(&b, 0, sizeof b); b.t = C_BOX; b.node = it->b->n; b.flags = 0; f->slot = dl_add(b); } }
                break; }
            case I_CLOSE: {
                FLUSH_RUN(); cx += it->b->pr + it->b->br;
                for (int q = nst - 1; q >= 0; q--) if (stack[q].b == it->b) {
                    OpenFrag* f = &stack[q];
                    if (f->slot >= 0) { FM* m = fm_of(f->b); Cmd* cm = &l2_dl[f->slot]; cm->x = f->x0; cm->w = cx - f->x0; cm->y = base - m->asc - f->b->pt - f->b->bt; cm->h = m->asc + m->desc + f->b->pt + f->b->pb + f->b->bt + f->b->bb; cm->flags = (uint8_t)(f->cont ? F_NOLEFT : 0); }
                    for (int z = q; z + 1 < nst; z++) stack[z] = stack[z + 1]; nst--; break; }
                cx += it->b->mr; break; }
            case I_SPACE: {
                /* A collapsed space can belong to a separate DOM text node. Keep it
                 * as its own run instead of attaching it to a different node, so
                 * logical caret offsets and copy ranges remain tied to the source. */
                if (rb && rb != it->b) FLUSH_RUN();
                if (!rb && it->b) { rb = it->b; rx = cx; rl = 0; run_source_start = run_source_next = NULL; run_source_valid = 1; }
                if (rb) {
                    if (run_source_valid) {
                        if (it->s && it->len == 1 && it->s[0] == ' ' && (!run_source_next || run_source_next == it->s)) {
                            if (!run_source_start) run_source_start = it->s;
                            run_source_next = it->s + 1;
                        } else run_source_valid = 0;
                    }
                    if (rl + 2 > rcap) { rcap = rcap ? rcap * 2 : 256; rbuf = realloc(rbuf, rcap); }
                    if (rbuf && rl + 1 < rcap) rbuf[rl++] = ' ';
                }
                cx += it->w + jextra; break; }
            case I_WORD: {
                if (rb && rb != it->b) FLUSH_RUN();
                if (!rb) { rb = it->b; rx = cx; rl = 0; run_source_start = run_source_next = NULL; run_source_valid = 1; }
                size_t need = it->s ? (size_t)it->len : 0; if (!it->s) need = (size_t)(-it->len);
                if (run_source_valid) {
                    if (!it->s || it->len <= 0) run_source_valid = 0;
                    else if (!run_source_start) { run_source_start = it->s; run_source_next = it->s + need; }
                    else if (run_source_next == it->s) run_source_next = it->s + need;
                    else run_source_valid = 0;
                }
                /* text-overflow applies to clipped, single-line inline content. Keep whole UTF-8 code points and append an ellipsis. */
                if (it->s && cs->text_overflow == 1 && cs->overflow_hidden && cs->white_space == WS_NOWRAP) {
                    FM* tm = fm_of(it->b); float ew = text_w(tm, "…", 3);
                    if (cx + it->w + ew > R + 0.01f) {
                        float room = R - cx - ew; if (room < 0) room = 0;
                        size_t pos = 0; float acc = 0;
                        while (pos < need) {
                            size_t clen = 1; unsigned char ch = (unsigned char)it->s[pos];
                            if (ch >= 0xF0) clen = 4; else if (ch >= 0xE0) clen = 3; else if (ch >= 0xC0) clen = 2;
                            if (pos + clen > need) clen = need - pos;
                            float cw = text_w(tm, it->s + pos, (int)clen);
                            if (acc + cw > room) break;
                            acc += cw; pos += clen;
                        }
                        if (rl + pos + 5 > rcap) { size_t nc = (rl + pos + 5) * 2; char* nb = realloc(rbuf, nc); if (nb) { rbuf = nb; rcap = nc; } }
                        if (rbuf && rl + pos + 4 <= rcap) { if (pos) { memcpy(rbuf + rl, it->s, pos); rl += pos; } memcpy(rbuf + rl, "…", 3); rl += 3; cx += acc + ew; overflow_ellipsis = 1; run_source_valid = 0; }
                        k = e; break;
                    }
                }
                if (rl + need + 2 > rcap) { size_t nc = (rl + need + 2) * 2; char* nb = realloc(rbuf, nc); if (nb) { rbuf = nb; rcap = nc; } }
                if (rbuf && rl + need + 1 <= rcap) { if (it->s) { memcpy(rbuf + rl, it->s, need); rl += need; } else { memset(rbuf + rl, ' ', need); rl += need; } }
                cx += it->w; break; }
            case I_ATOM: {
                FLUSH_RUN(); Box* a = it->b; float oh = a->mt + a->h + a->mb; float fbrel = a->fb > -1e8f ? a->fb - a->y : -1e9f;
                float ab = fbrel > -1e8f ? a->mt + fbrel : oh; if (a->repl == R_IMG || a->repl == R_PLACE || a->repl == R_CANVAS) ab = oh;
                float top;
                if (a->st->vertical_align == VA_TOP) top = cy; else if (a->st->vertical_align == VA_BOTTOM) top = cy + lh - oh;
                else { if (a->st->vertical_align == VA_MIDDLE) ab = oh / 2 + 0.27f * strut->size; top = base - ab; }
                float dx = cx + a->ml - a->x, dy = top + a->mt - a->y;
                dl_shift(it->fs, it->fe, dx, dy); a->x += dx; a->y += dy; if (a->fb > -1e8f) a->fb += dy;
                cx += a->ml + a->w + a->mr; break; }
            default: break;
            }
        }
        at_line_end = 1; FLUSH_RUN(); at_line_end = 0;
        if (!clamp_ellipsis && cs->text_overflow != 1 && cs->text_align != ALIGN_JUSTIFY)
            bidi_reposition_line_commands(line_cmd_start, l2_dn, bd);
        /* незакрытые inline-элементы продолжаются на следующей строке */
        for (int q = 0; q < nst; q++) {
            OpenFrag* f = &stack[q];
            if (f->slot >= 0) { FM* m = fm_of(f->b); Cmd* cm = &l2_dl[f->slot]; cm->x = f->x0; cm->w = cx - f->x0; cm->y = base - m->asc - f->b->pt - f->b->bt; cm->h = m->asc + m->desc + f->b->pt + f->b->pb + f->b->bt + f->b->bb; cm->flags = (uint8_t)((f->cont ? F_NOLEFT : 0) | F_NORIGHT);
                Cmd b; memset(&b, 0, sizeof b); b.t = C_BOX; b.node = f->b->n; f->slot = dl_add(b); }
            f->x0 = x; f->cont = 1;
        }
        cy += lh;
        i = next_i; line_no++; col_line++;
        if (cy - (ybase) > col_max_h) col_max_h = cy - ybase;
        if (multicol && i < ni && col < ncols - 1 &&
            ((lines_per_col < 0x3fffffff && col_line >= lines_per_col) ||
             (lines_per_col >= 0x3fffffff && col_target_h > 0 && cy - ybase >= col_target_h))) {
            col++; col_used=col+1; col_line=0; x=xbase+col*(colw+colgap); w=colw; cy=ybase;
        }
        }
    }
    free(iv.v); free(rbuf);
    for (int q = 0; q < nst; q++) if (stack[q].slot >= 0) { Cmd* cm = &l2_dl[stack[q].slot]; cm->w = 0; cm->h = 0; }   /* хвост без содержимого */
    if (multicol) {
        float last_h=cy-ybase; if(last_h>col_max_h)col_max_h=last_h;
        column_rule_emit(cs,xbase,ybase,colw,colgap,col_used,col_target_h>col_max_h?col_target_h:col_max_h);
        return (col_target_h>0 && cs->height.set) ? col_target_h : col_max_h;
    }
    return cy - y;
}

/* ══════════════ блочный поток ══════════════ */
static int is_bfc(const Box* b) {
    if (b->parent && is_flex_box(b->parent) && !b->anon) return 1;
    return b->flt || b->oof || b->kind == K_ATOM || (b->st && b->st->overflow_hidden && !b->anon) ||
           (b->n && (b->st->display == DSP_FLEX || b->st->display == DSP_INLINE_FLEX || b->st->display == DSP_TABLE || b->st->display == DSP_CELL || b->st->display == DSP_INLINE_BLOCK)) || (b->n && b->n->idx == 0);
}
static Box* first_inflow_block(Box* b) { for (Box* c = b->first; c; c = c->next) if (is_block_level(c)) return c; return NULL; }
static Box* last_inflow_block(Box* b) { Box* r = NULL; for (Box* c = b->first; c; c = c->next) if (is_block_level(c)) r = c; return r; }
static int flow_container(const Box* b) { return b->kind == K_BLOCK && !b->repl && !b->anon && (b->st->display == DSP_BLOCK || b->st->display == DSP_LIST_ITEM) && !is_bfc(b); }
static int inline_only(const Box* b) { if (!b->first) return 1; for (Box* c = b->first; c; c = c->next) if (is_block_level(c)) return 0; return 1; }

static float mt_eff(Box* c, float cw) {
    edges(c, cw); float m = c->mt;
    if (flow_container(c) && c->bt == 0 && c->pt == 0) { Box* f = first_inflow_block(c); if (f) m = collapse2(m, mt_eff(f, cw)); }
    return m;
}
static float mb_eff(Box* c, float cw) {
    edges(c, cw); float m = c->mb;
    if (flow_container(c) && c->bb == 0 && c->pb == 0 && !c->st->height.set && !c->st->min_h.set) { Box* f = last_inflow_block(c); if (f) m = collapse2(m, mb_eff(f, cw)); }
    return m;
}

/* A conservative fragmentation fast path for plain text/inline-only children.
   It lets a paragraph continue in the next column at line boundaries without
   pretending that block backgrounds, borders, floats or replaced content can
   be fragmented safely. Rich block fragments stay on the legacy path below. */
static int mc_fragment_safe(Box* b, int root) {
    if (!b || !b->st || b->repl || b->oof || b->flt || b->kind == K_ATOM) return 0;
    int text_node = b->n && b->n->type == TAG_TEXT;
    if (!text_node && (has_visual_box(b->st) || b->st->overflow_hidden ||
        b->st->position != POS_STATIC || b->st->column_span ||
        (b->n && b->st->display == DSP_LIST_ITEM))) return 0;
    if (!root && !text_node && b->kind == K_BLOCK && !b->anon) return 0;
    for (Box* c = b->first; c; c = c->next) {
        int child_text = c->n && c->n->type == TAG_TEXT;
        if (c->kind == K_ATOM || (!child_text && c->kind == K_BLOCK && !c->anon)) return 0;
        if (!mc_fragment_safe(c, 0)) return 0;
    }
    return 1;
}
static int mc_fragmentable_children(Box** items, int k) {
    for (int i=0; i<k; i++) if (!mc_fragment_safe(items[i], 1)) return 0;
    return 1;
}
static float mc_fragment_text_children(Box* parent, Box** items, int k, float cx, float cy,
                                      float colw, float gap, int ncol, float target, LS* ls) {
    float stream=0, measured=0; int maxcol=0;
    for (int i=0; i<k; i++) {
        Box* c=items[i]; float mt=mt_eff(c,colw), mb=mb_eff(c,colw);
        int cmd0=l2_dn;
        lbb(c,cx,cy+stream+mt,colw,-1,-1,-1,ls);
        float h=mt+c->h+mb; if (h<0) h=0;
        /* The safe-path predicate excludes children that could emit a block
           background/clip or a replaced item; all visible commands are line-local. */
        for (int j=cmd0; j<l2_dn; j++) {
            Cmd* q=&l2_dl[j]; if(q->fixed) continue;
            if(q->t==C_TEXT || q->t==C_HIT) {
                float off=q->y-cy; int col=off>0?(int)floorf(off/target):0;
                if(col<0) col=0;
                if(col>1024) col=1024;
                float stride=colw+gap; q->x += col*stride; q->y -= col*target;
                if(col>maxcol)maxcol=col;
            } else if(q->t==C_MARK && q->node) {
                float top=q->y-cy, bot=top+q->h;
                int first=top>0?(int)floorf(top/target):0;
                int last=bot>0?(int)floorf(fmaxf(0,bot-0.01f)/target):first;
                if(first<0) first=0;
                if(last<first) last=first;
                if(last>1024) last=1024;
                q->x=cx+first*(colw+gap); q->y=cy;
                q->w=(last-first)*(colw+gap)+colw; q->h=target;
                if(last>maxcol)maxcol=last;
            }
        }
        stream+=h; measured+=h;
    }
    (void)parent;
    int used=maxcol+1; if(used<1)used=1;
    if(used>ncol) used=ncol; /* overflow columns remain clipped by the containing flow for now */
    column_rule_emit(parent->st,cx,cy,colw,gap,used,target);
    float content_h=measured<target?measured:target;
    if(parent->st->height.set && target>0) content_h=target;
    return content_h;
}

static float layout_multicol_flow(Box* b, float cx, float cy, float cw, float ch, LS* ls) {
    const NodeStyle* st=b->st; float gap=0,colw=cw; int ncol=column_metrics(st,cw,&gap,&colw); if(ncol<2)return 0;
    int count=0; for(Box* c=b->first;c;c=c->next) if(!c->oof)count++;
    Box** items=la(sizeof(Box*)*(size_t)(count?count:1)); float* hs=la(sizeof(float)*(size_t)(count?count:1)); if(!items||!hs)return 0;
    int k=0; for(Box* c=b->first;c;c=c->next) if(!c->oof)items[k++]=c;
    int mark=l2_dn, gmark=g_sgn; float measured=0; FCtx measure_fc; memset(&measure_fc,0,sizeof measure_fc); LS mls=*ls; mls.fc=&measure_fc;
    /* First measure each direct child at the actual column width; discard the provisional display commands. */
    for(int i=0;i<k;i++){
        Box* c=items[i]; if(c->flt) { hs[i]=0; continue; }
        float mt=mt_eff(c,colw), mb=mb_eff(c,colw); lbb(c,cx,cy+measured+mt,colw,-1,-1,ch,&mls);
        hs[i]=mt+c->h+mb; if(hs[i]<0)hs[i]=0; measured+=hs[i];
    }
    l2_dn=mark; grp_trunc(gmark);
    float target=st->height.set?lenpx(&st->height,ch>=0?ch:VH):(st->column_fill==1?measured:measured/ncol);
    if(target<st->font_size*1.2f)target=st->font_size*1.2f;
    int has_special=0;
    for(Box* c=b->first;c;c=c->next) if(c->oof || c->flt || (c->st->column_span && c->st->column_span==1)) { has_special=1; break; }
    int safe_frag = !has_special && k>0 && target>0 && mc_fragmentable_children(items,k);
    if(safe_frag) {
        float frag_h=mc_fragment_text_children(b,items,k,cx,cy,colw,gap,ncol,target,ls);
        return frag_h;
    }
    int col=0; float y=cy; float maxh=0; int used=1; float colx=cx;
    for(Box* c=b->first;c;c=c->next){
        if(c->oof){register_abs(ls,c,cx,y);continue;}
        if(c->flt){edges(c,colw);int ds=l2_dn;lbb(c,0,0,colw,-1,-1,ch,ls);place_float(c,ds,l2_dn,ls,colx,colx+colw,y);continue;}
        int ii=0;while(ii<k&&items[ii]!=c)ii++;float est=ii<k?hs[ii]:0;
        if(c->st->column_span && c->st->column_span==1){
            lbb(c,cx,y,cw,-1,-1,ch,ls); y+=c->h; if(y-cy>maxh)maxh=y-cy; col++; if(col>=ncol)col=ncol-1; used=col+1; colx=cx+col*(colw+gap); y=cy; continue;
        }
        if(col<ncol-1 && y>cy+0.01f && y+est>cy+target+0.01f){if(y-cy>maxh)maxh=y-cy;col++;used=col+1;colx=cx+col*(colw+gap);y=cy;}
        float mt=mt_eff(c,colw), mb=mb_eff(c,colw); lbb(c,colx,y+mt,colw,-1,-1,ch,ls);
        if(c->fb>-1e8f && b->fb<-1e8f)b->fb=c->fb;
        y+=mt+c->h+mb;
    }
    if(y-cy>maxh)maxh=y-cy;
    column_rule_emit(st,cx,cy,colw,gap,used,st->height.set?target:maxh);
    if(st->height.set && ch>=0 && target>maxh)maxh=target;
    return maxh;
}
static float layout_flow(Box* b, float cx, float cy, float cw, float ch, LS* ls) {
    if (inline_only(b)) return layout_inline(b, cx, cy, cw, ls);
    { float cg=0,cwcol=cw; if(column_metrics(b->st,cw,&cg,&cwcol)>1) return layout_multicol_flow(b,cx,cy,cw,ch,ls); }
    int coll_top = flow_container(b) && b->bt == 0 && b->pt == 0;
    int coll_bot = flow_container(b) && b->bb == 0 && b->pb == 0 && !b->st->height.set && !b->st->min_h.set;
    float y = cy, prev_mb = 0; int first = 1;
    for (Box* c = b->first; c; c = c->next) {
        if (c->oof) { register_abs(ls, c, cx, y + prev_mb); continue; }
        if (c->flt) {
            edges(c, cw); int s = l2_dn; lbb(c, 0, 0, cw, -1, -1, -1, ls); place_float(c, s, l2_dn, ls, cx, cx + cw, y + prev_mb); continue;
        }
        float mt = mt_eff(c, cw), mb = mb_eff(c, cw);
        float gap = (first && coll_top) ? 0 : collapse2(prev_mb, mt);
        float ytop = y + gap;
        int cleared = 0;
        if (c->st->clear && ls->fc) { float bot = fc_bottom(ls->fc, c->st->clear); if (bot > ytop) { ytop = bot; cleared = 1; } }
        float lx = cx, lw = cw;
        if (ls->fc && c->kind == K_BLOCK && !c->anon && c->n && is_bfc(c)) {           /* BFC-блок не перекрывает флоаты: сужаем или опускаем ниже */
            const NodeStyle* cs = c->st; edges(c, cw);
            for (int g = 0; g < 40; g++) {
                float L = cx, R = cx + cw; fc_range(ls->fc, ytop, ytop + 1, &L, &R);
                if (L <= cx + 0.01f && R >= cx + cw - 0.01f) break;
                float need = (cs->width.set && !(cs->width.pct && cw <= 0) ? lenpx(&cs->width, cw) + (cs->box_sizing ? 0 : c->pl + c->pr + c->bl + c->br) : intr(c, 0)) + c->ml + c->mr;
                if (need <= (R - L) + 0.01f) { lx = L; lw = R - L; break; }
                float nb = fc_next_bottom(ls->fc, ytop, ytop + 1); if (nb > 1e8f) break; ytop = nb;
            }
        }
        lbb(c, lx, ytop, lw, -1, -1, ch, ls);
        if (c->fb > -1e8f && b->fb < -1e8f) b->fb = c->fb;
        int empty = c->h == 0 && c->bt == 0 && c->bb == 0 && c->pt == 0 && c->pb == 0;
        if (empty && !c->st->height.set && !cleared) { prev_mb = collapse2(collapse2(prev_mb, mt), mb); if (first && coll_top) { } continue; }
        y = ytop + c->h; prev_mb = mb; first = 0;
    }
    if (!coll_bot) y += prev_mb;
    return y - cy;
}

static void patch_h(Box* b, float nh) {
    if (b->slot >= 0) l2_dl[b->slot].h = nh;
    if (b->clipslot >= 0) {
        Cmd* cc = &l2_dl[b->clipslot]; cc->h = nh - b->bt - b->bb;
        if (cc->flags & CF_SCROLL_Y) cc->ex[3] = cc->ex[1] > cc->h ? cc->ex[1] - cc->h : 0;     /* клиентская высота изменилась */
    }
    b->h = nh;
}

/* ══════════════ flex и таблицы ══════════════ */
#include "l2_flex.inc"
#include "l2_grid.inc"
#include "l2_table.inc"

/* ══════════════ размеры по содержимому ══════════════ */
static float intr(Box* b, int mode) {
    if (b->ic_ok & (1 << mode)) return mode ? b->ic_max : b->ic_min;
    float r = 0;
    if (b->kind == K_TEXT || b->kind == K_BR) r = 0;
    else {
        edges(b, 0);
        float ex = b->pl + b->pr + b->bl + b->br; const NodeStyle* s = b->st;
        if (b->repl) { float w, h; repl_size(b, 0, -1, &w, &h); r = w + ex; }
        else if (s->width.set && s->width.pct == 0 && !b->anon) { r = lenpx(&s->width, 0) + (s->box_sizing ? 0 : ex); }
        else {
            float c = 0;
            if (b->n && s->display == DSP_TABLE) c = table_intr(b, mode);
            else if (b->n && (s->display == DSP_FLEX || s->display == DSP_INLINE_FLEX)) {
                int col = s->flex_dir >= FD_COL; int cnt = 0; float sum = 0, mx = 0;
                for (Box* k = b->first; k; k = k->next) { if (k->oof) continue; edges(k, 0); float w = intr(k, mode) + k->ml + k->mr; sum += w; if (w > mx) mx = w; cnt++; }
                float gap = s->gap_c * (cnt > 1 ? cnt - 1 : 0);
                c = col ? mx : (mode == 0 && s->flex_wrap ? mx : sum + gap);
            } else {
                int io = inline_only(b); if (io) c = measure_inline(b, mode);
                float run = 0;                                     /* подряд идущие флоаты стоят в одну строку: max-content = сумма */
                for (Box* k = b->first; k; k = k->next) {
                    if (k->oof) continue;
                    if (!k->flt && io) continue;
                    edges(k, 0); float w = intr(k, mode) + k->ml + k->mr;
                    if (k->flt) { if (mode) { run += w; if (run > c) c = run; } else if (w > c) c = w; continue; }
                    run = 0; if (w > c) c = w;
                }
            }
            r = c + ex;
            if (b->n && b->st->min_w.set && b->st->min_w.pct == 0) r = fmaxf2(r, lenpx(&b->st->min_w, 0) + (s->box_sizing ? 0 : ex));
            if (b->n && b->st->max_w.set && b->st->max_w.pct == 0) r = fminf2(r, lenpx(&b->st->max_w, 0) + (s->box_sizing ? 0 : ex));
        }
    }
    if (mode) { b->ic_max = r; b->ic_ok |= 2; } else { b->ic_min = r; b->ic_ok |= 1; }
    return r;
}

/* ══════════════ блок: размеры, фон, содержимое ══════════════ */

static void process_abs(Box* b, float px, float py, float pw, float ph, int fixed_cb);

static void lbb(Box* b, float x, float y, float avail_w, float forced_w, float forced_h, float ch_def, LS* ls) {
    const NodeStyle* s = b->st; int start = l2_dn;
    int stkslot = -1;                                   /* position:sticky: границы блока для смещения при отрисовке */
    if (s->sticky && !b->anon && b->n && b->kind != K_INLINE && !b->oof) { Cmd sc0; memset(&sc0, 0, sizeof sc0); sc0.t = C_STK; sc0.node = b->n; stkslot = dl_add(sc0); }
    int markslot = -1; if (!b->anon && b->n && b->n->v2) { Cmd mc; memset(&mc, 0, sizeof mc); mc.t = C_MARK; mc.node = b->n; markslot = dl_add(mc); }
    int fxslot = -1; if (!b->anon && b->n && s->has_fx && b->kind != K_INLINE) { Cmd fc; memset(&fc, 0, sizeof fc); fc.t = C_FX; fc.node = b->n; fxslot = dl_add(fc); }
    edges(b, avail_w);
    int shrink = forced_w < 0 && (b->kind == K_ATOM || b->flt || b->oof || (b->n && !b->anon && s->display == DSP_TABLE)) && !s->width.set;
    float ex_w = b->pl + b->pr + b->bl + b->br, ex_h = b->pt + b->pb + b->bt + b->bb;
    float bw;
    if (b->repl) {
        float cw2, chh; repl_size(b, avail_w, ch_def, &cw2, &chh);
        bw = forced_w >= 0 ? forced_w : cw2 + ex_w;
        if (forced_h < 0) forced_h = chh + ex_h;
    } else if (forced_w >= 0) bw = forced_w;
    else if (s->width.set && !(s->width.pct && avail_w <= 0)) { bw = lenpx(&s->width, avail_w); if (!s->box_sizing) bw += ex_w; }
    else if (shrink) { float mx = intr(b, 1), mn = intr(b, 0); float av = avail_w - b->ml - b->mr; bw = fminf2(fmaxf2(mn, av), mx); if (bw > mx) bw = mx; }
    else bw = avail_w - b->ml - b->mr;
    if (!b->repl || b->repl == R_PLACE || b->repl == R_CANVAS) {
        if (forced_w < 0) {
            if (s->max_w.set && !b->anon) { float m = lenpx(&s->max_w, avail_w) + (s->box_sizing ? 0 : ex_w); if (bw > m) bw = m; }
            if (s->min_w.set && !b->anon) { float m = lenpx(&s->min_w, avail_w) + (s->box_sizing ? 0 : ex_w); if (bw < m) bw = m; }
        }
    }
    if (bw < ex_w) bw = ex_w;
    float ml = b->ml, mr = b->mr;
    if ((b->mla || b->mra) && !b->anon && b->kind == K_BLOCK && !b->flt && !b->oof) {   /* auto-поля */
        float free = avail_w - bw - (b->mla ? 0 : ml) - (b->mra ? 0 : mr);
        if (free < 0) free = 0;
        if (b->mla && b->mra) ml = mr = free / 2; else if (b->mla) ml = free; else mr = free;
    }
    float bx = x + ml;
    float cx = bx + b->bl + b->pl, cw = bw - ex_w; if (cw < 0) cw = 0;

    int slot = -1, bgslot = -1;
    int visual = !b->anon && b->n && (has_visual_box(s) || (b->repl == R_CTRL));
    if (b->skipbg) visual = !b->anon && ((s->border_w[0] + s->border_w[1] + s->border_w[2] + s->border_w[3]) > 0 || (s->sh_color >> 24));
    if (visual && s->visibility == 0) { Cmd c; memset(&c, 0, sizeof c); c.t = C_BOX; c.node = b->n; c.flags = b->skipbg ? 0x80 : 0; slot = dl_add(c); }
    b->slot = slot;
    if (!b->anon && b->n && s->bg_url[0] && s->visibility == 0) { Cmd c; memset(&c, 0, sizeof c); c.t = C_BGIMG; c.node = b->n; c.s = s->bg_url; c.opacity = s->opacity; bgslot = dl_add(c); }
    int clip = -1;
    if (s->overflow_hidden && !b->anon) { Cmd c; memset(&c, 0, sizeof c); c.t = C_CLIP; clip = dl_add(c); }
    b->clipslot = clip;
    int cstart = l2_dn;                                  /* начало содержимого блока (для переноса дочерних стекинг-контекстов) */
    if (b->click && !b->anon && b->n && b->kind != K_INLINE && (b->n->type == TAG_A || b->n->onclick[0] || b->n->type == TAG_BUTTON)) { }

    float fh = -1;                                                                     /* явная высота контента */
    if (forced_h >= 0) fh = forced_h - ex_h;
    else if (s->height.set && !b->repl && !(s->height.pct && ch_def < 0)) { fh = lenpx(&s->height, ch_def >= 0 ? ch_def : 0); if (!s->box_sizing) { } else fh -= ex_h; }
    if (fh >= 0 && forced_h < 0 && s->height.set && !s->box_sizing) { /* content-box: fh уже контентная */ }
    float ch_child = fh >= 0 ? fh : -1;
    if (b->n && b->n->idx <= 1 && ch_child < 0) ch_child = -1;

    FCtx local; LS ls2 = *ls;
    if (is_bfc(b) && !b->anon) { memset(&local, 0, sizeof local); ls2.fc = &local; }
    Box* saved_owner = ls2.owner; int made_owner = 0;
    if (!b->anon && b->n && (s->position != POS_STATIC || b->n->idx == 0)) { ls2.owner = b; made_owner = 1; }
    (void)saved_owner;

    float content_h = 0; b->fb = -1e9f;
    float cy = y + b->bt + b->pt;
    int cv_skip = !b->repl && !b->anon && b->n && s->content_visibility == 2;
    if (!cv_skip && !b->repl && !b->anon && b->n && s->content_visibility == 1 && s->contain_intrinsic_h.set) {
        float ph = lenpx(&s->contain_intrinsic_h, VH);
        float visual_top = y - g_page_scroll_y;
        if (visual_top > VH * 1.25f || visual_top + ph < -VH * 0.25f) cv_skip = 1;
    }
    if (cv_skip) {
        if (s->contain_intrinsic_h.set) content_h = lenpx(&s->contain_intrinsic_h, ch_def >= 0 ? ch_def : VH);
        else if (fh >= 0) content_h = fh;
        else if (s->height.set && !(s->height.pct && ch_def < 0)) content_h = lenpx(&s->height, ch_def >= 0 ? ch_def : 0);
        if (content_h < 0) content_h = 0;
    } else if (b->repl) content_h = 0;
    else if (!b->anon && b->n && s->display == DSP_TABLE) content_h = layout_table(b, cx, cy, cw, &ls2);
    else if (!b->anon && b->n && s->is_grid) content_h = layout_grid(b, cx, cy, cw, ch_child, &ls2);
    else if (!b->anon && b->n && (s->display == DSP_FLEX || s->display == DSP_INLINE_FLEX)) content_h = layout_flex(b, cx, cy, cw, ch_child, &ls2);
    else content_h = layout_flow(b, cx, cy, cw, ch_child, &ls2);
    if (is_bfc(b) && !b->anon && ls2.fc) { float bot = fc_bottom(ls2.fc, 3); if (bot > cy + content_h) content_h = bot - cy; }
    if (!b->repl && s->aspect_ratio > 0.0f && s->width.set && !s->height.set && fh < 0) {
        float content_w = bw - ex_w; if (content_w > 0) content_h = content_w / s->aspect_ratio;
    }

    float bh;
    if (fh >= 0) bh = fh + ex_h; else bh = content_h + ex_h;
    if (forced_h >= 0) bh = forced_h;
    if (!b->anon && forced_h < 0) {
        if (s->min_h.set) { float m = lenpx(&s->min_h, ch_def >= 0 ? ch_def : 0); if (s->min_h.pct && ch_def < 0) m = 0; float mb2 = s->box_sizing ? m : m + ex_h; if (bh < mb2) bh = mb2; }
        if (s->max_h.set && !(s->max_h.pct && ch_def < 0)) { float m = lenpx(&s->max_h, ch_def >= 0 ? ch_def : 0); float mb2 = s->box_sizing ? m : m + ex_h; if (bh > mb2) bh = mb2; }
    }
    b->x = bx; b->y = y; b->w = bw; b->h = bh;
    if (markslot >= 0) { Cmd* mc = &l2_dl[markslot]; mc->x = bx; mc->y = y; mc->w = bw; mc->h = bh; }
    for (StkRef* r = b->stk; r; r = r->next)             /* sticky-потомки: контейнер = content-box этого блока */
        if (r->slot >= 0 && r->slot < l2_dn && l2_dl[r->slot].t == C_STK && l2_dl[r->slot].node == r->node) {
            l2_dl[r->slot].ex[1] = y + b->bt + b->pt; l2_dl[r->slot].ex[2] = y + bh - b->bb - b->pb;
            l2_dl[r->slot].ex[6] = bx + b->bl + b->pl; l2_dl[r->slot].ex[7] = bx + bw - b->br - b->pr;
        }
    if (slot >= 0) { Cmd* c = &l2_dl[slot]; c->x = bx; c->y = y; c->w = bw; c->h = bh; }
    if (bgslot >= 0) { Cmd* c = &l2_dl[bgslot]; c->x = bx; c->y = y; c->w = bw; c->h = bh; }
    if (clip >= 0) { Cmd* c = &l2_dl[clip]; c->x = bx + b->bl; c->y = y + b->bt; c->w = bw - b->bl - b->br; c->h = bh - b->bt - b->bb; }
    if (b->repl) {
        Cmd c; memset(&c, 0, sizeof c); c.node = b->n; c.opacity = s->opacity; c.x = bx + b->bl + b->pl; c.y = y + b->bt + b->pt; c.w = bw - ex_w; c.h = bh - ex_h;
        if ((b->repl == R_IMG && b->img && s->visibility == 0) || (b->repl == R_CANVAS && s->visibility == 0) || (b->repl == R_VIDEO && s->visibility == 0)) { c.t = C_IMG; c.s = b->repl == R_VIDEO ? b->n->media_frame_key : b->n->src; dl_add(c); }
        else if (b->repl == R_CTRL && s->visibility == 0) { c.t = C_CTRL; dl_add(c); }
        else if (b->repl == R_IMG && s->visibility == 0 && !b->img && b->n->alt[0]) { c.t = C_CTRL; dl_add(c); }
    }
    if (made_owner && b->abs) {
        float px = bx + b->bl, py = y + b->bt, pw = bw - b->bl - b->br, ph = bh - b->bt - b->bb;
        if (b->n->idx == 0) { px = 0; py = 0; pw = VW; ph = VH; }
        process_abs(b, px, py, pw, ph, 0);
    }
    int cend = l2_dn;                                    /* конец содержимого (до закрытия отсечения) */
    if (clip >= 0 && (s->sc_x || s->sc_y) && b->n && b->n->idx > 0 && b->n->idx < NVN && NV[b->n->idx] == b->n && strcmp(b->n->tag, "body") != 0) {
        /* прокручиваемый контейнер: считаем границы содержимого, вложенные отсечения учитываем их собственной рамкой */
        Cmd* cc = &l2_dl[clip]; float px0 = cc->x, py0 = cc->y, pw = cc->w, ph = cc->h;
        float mr = px0 + pw, mbm = py0 + ph; int depth = 0;
        for (int i = cstart; i < cend; i++) {
            const Cmd* q = &l2_dl[i];
            if (q->t == C_CLIP) { if (depth == 0 && !q->fixed) { if (q->x + q->w > mr) mr = q->x + q->w; if (q->y + q->h > mbm) mbm = q->y + q->h; } depth++; continue; }
            if (q->t == C_UNCLIP) { if (depth > 0) depth--; continue; }
            if (depth > 0 || q->fixed) continue;
            if (q->t != C_BOX && q->t != C_TEXT && q->t != C_IMG && q->t != C_RECT && q->t != C_CTRL && q->t != C_BGIMG && q->t != C_MARK) continue;
            if (q->w <= 0 && q->h <= 0) continue;
            if (q->x + q->w > mr) mr = q->x + q->w; if (q->y + q->h > mbm) mbm = q->y + q->h;
        }
        float sw = mr + b->pr - px0, sh = mbm + b->pb - py0; if (sw < pw) sw = pw; if (sh < ph) sh = ph; if (sw > 1e6f) sw = 1e6f; if (sh > 1e6f) sh = 1e6f;
        cc->node = b->n; cc->flags = (uint8_t)((s->sc_y ? CF_SCROLL_Y : 0) | (s->sc_x ? CF_SCROLL_X : 0));
        cc->ex[0] = sw; cc->ex[1] = sh; cc->ex[2] = s->sc_x && sw > pw ? sw - pw : 0; cc->ex[3] = s->sc_y && sh > ph ? sh - ph : 0;
        /* sticky-дети самого контейнера ограничены не его высотой, а всей прокручиваемой областью */
        for (StkRef* r = b->stk; r; r = r->next)
            if (r->slot >= 0 && r->slot < l2_dn && l2_dl[r->slot].t == C_STK && l2_dl[r->slot].node == r->node && l2_dl[r->slot].ex[2] < py0 + sh - b->pb) { l2_dl[r->slot].ex[2] = py0 + sh - b->pb; if (l2_dl[r->slot].ex[7] < px0 + sw - b->pr) l2_dl[r->slot].ex[7] = px0 + sw - b->pr; }
    }
    if (clip >= 0) { Cmd c; memset(&c, 0, sizeof c); c.t = C_UNCLIP; dl_add(c); }
    if (b->click && !b->anon && b->n && (b->kind != K_INLINE) && !b->repl && (b->n->type == TAG_A || b->n->onclick[0] || b->n->type == TAG_BUTTON)) {
        Cmd h; memset(&h, 0, sizeof h); h.t = C_HIT; h.x = bx; h.y = y; h.w = bw; h.h = bh; h.node = b->n; dl_add(h);
    }
    if (b->repl && b->click && b->n && s->visibility == 0) { Cmd h; memset(&h, 0, sizeof h); h.t = C_HIT; h.x = bx; h.y = y; h.w = bw; h.h = bh; h.node = b->click; dl_add(h); }
    if (!b->anon && b->n && s->display == DSP_LIST_ITEM) {                              /* ::marker / UA list marker */
        const PseudoBox* mp = b->n->pb[2];
        int custom_marker = mp && mp->content_state == 1;
        int suppress_marker = mp && mp->content_state == -1;
        const NodeStyle* ms = mp ? &mp->st : s;
        const char* mfam = mp && mp->font_family[0] ? mp->font_family : fam_of(b);
        float base = b->fb > -1e8f ? b->fb : y + b->bt + b->pt + fm_of(b)->asc;
        if (!suppress_marker && (custom_marker || s->list_style != LS_NONE)) {
            int weight = ms->weight ? ms->weight : (ms->bold ? 700 : 400), ital = ms->italic ? 1 : 0;
            TTFFont* ff = font_manager_get_styled(mfam, weight, ital); if (!ff) ff = g_ttf_font;
            TTFFont* fnorm = font_manager_get_styled(mfam, 400, ital), *fnoital = font_manager_get_styled(mfam, weight, 0);
            FM mm; memset(&mm, 0, sizeof mm); mm.f=ff; mm.size=(int)(ms->font_size+0.5f); if(mm.size<1)mm.size=1; if(mm.size>400)mm.size=400;
            mm.weight=weight; mm.ital=ital; mm.style=((ms->bold && ff==fnorm)?TTF_BOLD:0)|((ms->italic && ff==fnoital)?TTF_ITALIC:0);
            mm.asc=ff?ttf_font_ascent(ff,mm.size):(int)(mm.size*.93f); mm.desc=ff?ttf_font_descent(ff,mm.size):(int)(mm.size*.24f);
            Cmd c; memset(&c, 0, sizeof c); c.color=ms->color; c.opacity=ms->opacity;
            if (custom_marker) {
                const char* t=mp->content; size_t tl=strlen(t); if(tl>599)tl=599;
                float tw=text_w(&mm,t,(int)tl);
                if (tl) { c.t=C_TEXT; c.s=t; c.x=cx-tw-6; c.y=base-mm.asc; c.w=tw; c.h=(float)(mm.asc+mm.desc); c.node=b->n; c.fam=mfam; c.size=mm.size; c.tstyle=(uint8_t)mm.style; c.weight=(uint16_t)mm.weight; c.ital=(uint8_t)mm.ital; dl_add(c); }
            } else {
                int idx=1; { DOMNode* n=b->n; int par=n->parent; if(par>=0) for(int i=par+1;i<n->idx;i++) if(NV[i]->parent==par && NV[i]->st.display==DSP_LIST_ITEM) idx++; char v[16]; if(par>=0 && webpage_attr_get(NV[par],"start",v,sizeof v))idx+=atoi(v)-1; }
                float sz=mm.size*.32f;
                if(s->list_style==LS_DECIMAL) {
                    char* t=sa(24); snprintf(t,24,"%d.",idx); float tw=text_w(&mm,t,(int)strlen(t));
                    c.t=C_TEXT; c.s=t; c.x=cx-tw-6; c.y=base-mm.asc; c.w=tw; c.h=(float)(mm.asc+mm.desc); c.node=b->n; c.fam=mfam; c.size=mm.size; c.tstyle=(uint8_t)mm.style; c.weight=(uint16_t)mm.weight; c.ital=(uint8_t)mm.ital; dl_add(c);
                } else {
                    c.t=C_RECT; c.w=c.h=sz+1; c.x=cx-sz-12; c.y=base-mm.size*.36f-sz/2; c.rad=s->list_style==LS_SQUARE?0:c.w/2;
                    if(s->list_style==LS_CIRCLE){c.rad=c.w/2;c.flags=1;} dl_add(c);
                }
            }
        }
    }
    if (s->position == POS_RELATIVE && !b->anon && !s->sticky) {
        float dx = 0, dy = 0;
        if (s->inset[3].set) dx = lenpx(&s->inset[3], avail_w); else if (s->inset[1].set) dx = -lenpx(&s->inset[1], avail_w);
        if (s->inset[0].set) dy = lenpx(&s->inset[0], ch_def >= 0 ? ch_def : 0); else if (s->inset[2].set) dy = -lenpx(&s->inset[2], ch_def >= 0 ? ch_def : 0);
        if (dx != 0 || dy != 0) { dl_shift(start, l2_dn, dx, dy); b->x += dx; b->y += dy; if (b->fb > -1e8f) b->fb += dy; }
    }
    if (fxslot >= 0) {                                   /* слой эффектов: border-box уже с учётом relative-сдвига */
        Cmd* fc = &l2_dl[fxslot]; fc->x = b->x; fc->y = b->y; fc->w = b->w; fc->h = b->h;
        Cmd e; memset(&e, 0, sizeof e); e.t = C_ENDFX; e.node = b->n; int ei = dl_add(e); if (ei >= 0) { l2_dl[ei].mate = fxslot; l2_dl[fxslot].mate = ei; }
    }
    if (stkslot >= 0) {                                  /* sticky: итоговый border-box + смещения top/bottom; контейнер допишет родитель */
        Cmd* sk = &l2_dl[stkslot]; sk->x = b->x; sk->y = b->y; sk->w = b->w; sk->h = b->h; sk->flags = 0;
        if (s->inset[0].set) { sk->flags |= SF_TOP; sk->ex[0] = lenpx(&s->inset[0], VH); }
        if (s->inset[2].set) { sk->flags |= SF_BOTTOM; sk->ex[3] = lenpx(&s->inset[2], VH); }
        if (s->inset[3].set) { sk->flags |= SF_LEFT; sk->ex[4] = lenpx(&s->inset[3], VW); }
        if (s->inset[1].set) { sk->flags |= SF_RIGHT; sk->ex[5] = lenpx(&s->inset[1], VW); }
        sk->ex[1] = 0; sk->ex[2] = 1e9f; sk->ex[6] = -1e9f; sk->ex[7] = 1e9f;
        Box* sp = b->parent; while (sp && (sp->anon || !sp->n)) sp = sp->parent;
        if (sp) { StkRef* r = la(sizeof(StkRef)); if (r) { r->slot = stkslot; r->node = b->n; r->next = sp->stk; sp->stk = r; } }
        Cmd e; memset(&e, 0, sizeof e); e.t = C_ENDSTK; dl_add(e);
    }
    if (!b->anon && b->n) {                              /* стекинг-контекст */
        int is_ctx = 0, zg = 0; int item = b->parent && is_flex_box(b->parent);
        if (s->has_z && (s->position != POS_STATIC || item)) { zg = s->z_index; is_ctx = 1; }
        else if (s->position == POS_FIXED || b->n->idx == 0 || s->sticky || fxslot >= 0) is_ctx = 1;
        if (is_ctx) grp_record(start, cstart, cend, l2_dn, zg, clip >= 0, s->sticky && b->n->idx != 0);
    }
}

/* ══════════════ абсолютное и фиксированное позиционирование ══════════════ */
static void place_positioned(Box* c, float sx, float sy, float px, float py, float pw, float ph, LS* ls) {
    const NodeStyle* s = c->st; edges(c, pw);
    int fixed = s->position == POS_FIXED; int save_fixed = g_fixed; if (fixed) g_fixed = 1;
    int has_l = s->inset[3].set, has_r = s->inset[1].set, has_t = s->inset[0].set, has_b = s->inset[2].set;
    float l = has_l ? lenpx(&s->inset[3], pw) : 0, r = has_r ? lenpx(&s->inset[1], pw) : 0, t = has_t ? lenpx(&s->inset[0], ph) : 0, bt2 = has_b ? lenpx(&s->inset[2], ph) : 0;
    float fw = -1, fhh = -1;
    if (!s->width.set && has_l && has_r) fw = pw - l - r - c->ml - c->mr;
    if (!s->height.set && has_t && has_b) fhh = ph - t - bt2 - c->mt - c->mb;
    FCtx fc; memset(&fc, 0, sizeof fc); LS l2s = *ls; l2s.fc = &fc; l2s.owner = ls->owner;
    int s0 = l2_dn;
    lbb(c, 0, 0, pw, fw, fhh, ph, &l2s);
    float bx = has_l ? px + l + c->ml : has_r ? px + pw - r - c->mr - c->w : sx + c->ml;
    float by = has_t ? py + t + c->mt : has_b ? py + ph - bt2 - c->mb - c->h : sy + c->mt;
    if (c->mla && c->mra && has_l && has_r) bx = px + l + (pw - l - r - c->w) / 2;
    /* Basic CSS anchor positioning. Named anchors participate in document order; the
       first visible matching anchor supplies the position-area reference rectangle. */
    if (s->position_anchor[0] && GBOX) {
        Box* a=NULL;
        for(int ai=0;ai<NVN;ai++) if(GBOX[ai] && NV[ai]->st.anchor_name[0] &&
            !strcmp(NV[ai]->st.anchor_name,s->position_anchor)){a=GBOX[ai];break;}
        if(a) {
            const char* area=s->position_area;
            if(strcasestr(area,"left")) bx=a->x-c->w-c->mr;
            else if(strcasestr(area,"right")) bx=a->x+a->w+c->ml;
            else bx=a->x+(a->w-c->w)*0.5f;
            if(strcasestr(area,"top") || strcasestr(area,"block-start")) by=a->y-c->h-c->mb;
            else if(strcasestr(area,"bottom") || strcasestr(area,"block-end")) by=a->y+a->h+c->mt;
            else by=a->y+(a->h-c->h)*0.5f;
        }
    }
    dl_shift(s0, l2_dn, bx - c->x, by - c->y); c->x = bx; c->y = by;
    g_fixed = save_fixed;
}

static void process_abs(Box* b, float px, float py, float pw, float ph, int fixed_cb) {
    (void)fixed_cb;
    AbsRef* r = b->abs; b->abs = b->abs_tail = NULL;
    for (; r; r = r->next) {
        Box* c = r->b; LS ls = {NULL, b};
        if (c->st->position == POS_FIXED) place_positioned(c, r->sx, r->sy, 0, 0, VW, VH, &ls);
        else place_positioned(c, r->sx, r->sy, px, py, pw, ph, &ls);
    }
}

/* ══════════════ вход ══════════════ */
void l2_layout(DOMNode* head, int vw, int vh) {
    l2_release(); la_reset(); sa_reset(); pa_reset(); g_fixed = 0; g_sgn = 0;
    free(GBOX); GBOX=NULL;
    VW = (float)vw; VH = (float)vh; l2_canvas_bg = 0xFFFFFFFFu; l2_doc_w = VW; l2_doc_h = 0;
    int n = 0; for (DOMNode* p = head; p; p = p->next) n++;
    free(NV); NV = malloc((size_t)(n ? n : 1) * sizeof(DOMNode*)); NVN = n;
    GBOX = calloc((size_t)(n ? n : 1), sizeof(Box*));
    { int i = 0; for (DOMNode* p = head; p; p = p->next) NV[i++] = p; }
    sc_prepare(head, n);
    if (!n || !head->v2) return;
    /* фон холста: html, иначе body */
    DOMNode* html = NV[0]; DOMNode* body = NULL;
    for (int i = 1; i < n; i++) if (NV[i]->parent == 0 && !strcmp(NV[i]->tag, "body")) { body = NV[i]; break; }
    Box* root = newbox(html, &html->st, NULL); root->kind = K_BLOCK;
    if (html->st.bg >> 24) l2_canvas_bg = html->st.bg | 0xFF000000u;
    else if (body && (body->st.bg >> 24)) l2_canvas_bg = body->st.bg | 0xFF000000u;
    build_children(root, html, NULL);
    if (!(html->st.bg >> 24) && body) for (Box* c = root->first; c; c = c->next) if (c->n == body) c->skipbg = 1;
    if (html->st.bg >> 24) root->skipbg = 1;
    normalize(root);
    LS ls; FCtx fc; memset(&fc, 0, sizeof fc); ls.fc = &fc; ls.owner = root;
    float mt = mt_eff(root, VW);
    lbb(root, 0, mt, VW, -1, -1, VH, &ls);
    stk_reorder();                                                  /* порядок рисования по z-index */
    { int* st = malloc((size_t)(l2_dn + 1) * sizeof(int)); int sp = 0;     /* пары C_FX/C_ENDFX после перестановки */
      if (st) { for (int i = 0; i < l2_dn; i++) { if (l2_dl[i].t == C_FX) { l2_dl[i].mate = -1; st[sp++] = i; } else if (l2_dl[i].t == C_ENDFX) { if (sp) { int o = st[--sp]; l2_dl[o].mate = i; l2_dl[i].mate = o; } else l2_dl[i].mate = -1; } } free(st); } }
    sc_commit();                                                    /* размеры прокручиваемых контейнеров */
    l2_doc_h = root->y + root->h + mb_eff(root, VW); if (l2_doc_h < 1) l2_doc_h = 1;
    if (root->w > l2_doc_w) l2_doc_w = root->w;
    for (int i = 0; i < l2_dn; i++) { Cmd* c = &l2_dl[i]; if (c->fixed) continue; if ((c->t == C_BOX || c->t == C_TEXT || c->t == C_IMG) && c->y + c->h > l2_doc_h && c->y + c->h < 60000 && c->y >= 0) l2_doc_h = c->y + c->h; }
    l2_rects = calloc((size_t)(n ? n : 1) * 4, sizeof(float)); l2_nrects = n;
    if (l2_rects) for (int i = 0; i < l2_dn; i++) { const Cmd* c = &l2_dl[i]; if (c->t == C_MARK && !c->fixed && c->node && c->node->idx < n) { float* r = &l2_rects[c->node->idx * 4]; r[0] = c->x; r[1] = c->y; r[2] = c->w; r[3] = c->h; } }
    if (getenv("MLB_DUMP")) {                                       /* отладка: геометрия и ключевые стили узлов */
        for (int i = 0; i < n; i++) { DOMNode* q = NV[i]; if (q->type == TAG_TEXT || !l2_rects) continue; float* r = &l2_rects[i * 4];
            fprintf(stderr, "DUMP %3d p%3d <%s%s%s> disp=%d flt=%d pos=%d ovf=%d rect=[%.0f %.0f %.0f %.0f] w=%.0f%%+%.0f mg=[%.0f %.0f %.0f %.0f] pd=[%.0f %.0f %.0f %.0f]\n", q->idx, q->parent, q->tag, q->class_name[0] ? "." : "", q->class_name,
                q->st.display, q->st.float_, q->st.position, q->st.overflow_hidden, r[0], r[1], r[2], r[3], q->st.width.pct, q->st.width.px,
                q->st.margin[0].px, q->st.margin[1].px, q->st.margin[2].px, q->st.margin[3].px, q->st.padding[0].px, q->st.padding[1].px, q->st.padding[2].px, q->st.padding[3].px); }
    }
    la_reset();                                                    /* боксы больше не нужны (строки C_TEXT живут в отдельной арене) */
}
