/* Отрисовка display list, hit-test, выделение текста, GET-отправка форм. */
#include "l2.h"
#include "layout.h"
#include "font_manager.h"
#include "gal.h"
#include "fx.h"
#include "../ttf_render/shaping.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <strings.h>
#ifndef _WIN32
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#endif

extern TTFFont* g_ttf_font;

static DOMNode* g_head; static DOMNode* g_last_head; static uint64_t g_fp; static int g_lw = -1, g_lh = -1;
static int g_top, g_sx, g_sy; static char g_url[600];
static int cl_x0, cl_y0, cl_x1, cl_y1;
static WebTextPosition g_text_sel_anchor, g_text_sel_focus;
static int g_text_sel_active;
static float g_text_sel_preferred_x = NAN;
static int g_sel_scroll_x, g_sel_scroll_y, g_sel_scroll_seen;
static DOMNode* g_sel_scroll_head;
static int text_selection_endpoints_live(DOMNode* head);

/* A bounded renderer-owned viewport snapshot plus optional named-element crops.
   The compositor implements a CSS-driven subset of the View Transition pseudo-tree:
   root/group/image-pair/old/new computed styles and opacity/background/transform
   keyframes. It still is not a complete spec pseudo-element tree/layout engine. */
static uint32_t* g_vt_snapshot;
static int g_vt_width, g_vt_height, g_vt_top;
static DOMNode* g_vt_head;
static int g_vt_active, g_vt_finished_pending;
static double g_vt_started_ms, g_vt_last_tick_ms;
static double g_vt_duration_ms = 240.0;
/* Named-element shared snapshots are bounded independently from the viewport
   snapshot. Pixel crops are only captured from the actually visible portion of
   a named element, and duplicate names are ignored (matching CSS View Transitions). */
#define VIEW_TRANSITION_MAX_NAMED 24
#define VIEW_TRANSITION_NAMED_BUDGET (8u * 1024u * 1024u)
typedef struct {
    char name[48];
    int x, y, w, h;                 /* captured viewport crop */
    int target_x, target_y, target_w, target_h; /* target border-box in document coords */
    int has_target;
    uint32_t* pixels;
    uint32_t* new_pixels;             /* capture from the new page's first clean paint */
    int new_w, new_h;
} VTNamedSnapshot;
static VTNamedSnapshot g_vt_named[VIEW_TRANSITION_MAX_NAMED];
static int g_vt_named_count, g_vt_named_targets_ready;
static size_t g_vt_named_bytes;
#define VIEW_TRANSITION_DURATION_MS 240.0

static double view_transition_now_ms(void) {
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
#else
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) timespec_get(&ts, TIME_UTC);
#endif
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ══════════════ примитивы ══════════════ */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int g_blend_mode;
static inline int blend_channel(int s, int d) {
    switch (g_blend_mode) {
        case 1: return s * d / 255;                                  /* multiply */
        case 2: return 255 - (255 - s) * (255 - d) / 255;            /* screen */
        case 3: return d < 128 ? (2 * s * d / 255) : 255 - 2 * (255 - s) * (255 - d) / 255;
        case 4: return s < d ? s : d;
        case 5: return s > d ? s : d;
        case 6: return abs(s - d);
        case 7: return s + d - 2 * s * d / 255;
        default: return s;
    }
}
static inline void blend(int x, int y, uint32_t rgb, int a) {
    if (a <= 0 || x < cl_x0 || x >= cl_x1 || y < cl_y0 || y >= cl_y1) return;
    uint32_t* p = &back_buffer[(size_t)y * SCREEN_WIDTH + x];
    uint32_t d = *p; int ia = 255 - a;
    int sr = blend_channel((rgb >> 16) & 255, (d >> 16) & 255);
    int sg = blend_channel((rgb >> 8) & 255, (d >> 8) & 255);
    int sb = blend_channel(rgb & 255, d & 255);
    *p = ((uint32_t)((sr * a + ((d >> 16) & 255) * ia + 127) / 255) << 16) |
         ((uint32_t)((sg * a + ((d >> 8) & 255) * ia + 127) / 255) << 8) |
          (uint32_t)((sb * a + (d & 255) * ia + 127) / 255);
}

static int alpha_of(uint32_t argb, float opacity) { float a = (argb >> 24) * opacity; return a < 0 ? 0 : a > 255 ? 255 : (int)(a + 0.5f); }

static void fill_rect_i(int x0, int y0, int x1, int y1, uint32_t argb, float opacity) {
    int a = alpha_of(argb, opacity); if (a <= 0) return;
    if (x0 < cl_x0) x0 = cl_x0; if (y0 < cl_y0) y0 = cl_y0; if (x1 > cl_x1) x1 = cl_x1; if (y1 > cl_y1) y1 = cl_y1;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) blend(x, y, argb, a);
}

static void webpage_view_transition_named_clear(void) {
    for (int i = 0; i < g_vt_named_count; i++) {
        free(g_vt_named[i].pixels);
        free(g_vt_named[i].new_pixels);
    }
    memset(g_vt_named, 0, sizeof g_vt_named);
    g_vt_named_count = g_vt_named_targets_ready = 0;
    g_vt_named_bytes = 0;
}

static int webpage_view_transition_name_valid(const char* name) {
    if (!name || !*name || !strcasecmp(name, "none") || !strcasecmp(name, "auto") ||
        !strcasecmp(name, "match-element") || !strcasecmp(name, "initial") ||
        !strcasecmp(name, "inherit") || !strcasecmp(name, "unset") ||
        !strcasecmp(name, "revert") || !strcasecmp(name, "revert-layer") ||
        !strcasecmp(name, "default")) return 0;
    /* The current cascade stores one custom-ident (no CSS escapes yet). */
    const unsigned char* p = (const unsigned char*)name;
    if (!(isalpha(*p) || *p == '_' || *p >= 0x80 ||
          (*p == '-' && (p[1] == '-' || isalpha(p[1]) || p[1] == '_' || p[1] >= 0x80)))) return 0;
    for (p++; *p; p++)
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p >= 0x80)) return 0;
    return 1;
}

static void webpage_view_transition_capture_named(DOMNode* head) {
    typedef struct { DOMNode* node; char name[48]; int count; } SeenName;
    SeenName seen[VIEW_TRANSITION_MAX_NAMED]; int nseen = 0;
    if (!head || !l2_rects || l2_nrects <= 0 || !back_buffer) return;
    /* Gather unique names first; duplicate names must not create ambiguous groups. */
    for (DOMNode* n = head; n; n = n->next) {
        const char* name = n->st.view_transition_name;
        if (!n->v2 || !webpage_view_transition_name_valid(name)) continue;
        int j = 0; for (; j < nseen; j++) if (!strcmp(seen[j].name, name)) break;
        if (j < nseen) { seen[j].count++; continue; }
        if (nseen >= VIEW_TRANSITION_MAX_NAMED) continue;
        seen[nseen].node = n; snprintf(seen[nseen].name, sizeof seen[nseen].name, "%s", name); seen[nseen].count = 1; nseen++;
    }
    for (int i = 0; i < nseen; i++) {
        DOMNode* n = seen[i].node;
        if (seen[i].count != 1 || !n || n->idx < 0 || n->idx >= l2_nrects) continue;
        const float* r = &l2_rects[n->idx * 4];
        if (!(r[2] > 0.0f && r[3] > 0.0f)) continue;
        int full_x = (int)floorf(r[0] - g_sx);
        int full_y = (int)floorf(r[1] - g_sy + g_top);
        int x0 = full_x, y0 = full_y;
        int x1 = (int)ceilf(r[0] - g_sx + r[2]);
        int y1 = (int)ceilf(r[1] - g_sy + g_top + r[3]);
        if (x0 < 0) x0 = 0;
        if (y0 < g_top) y0 = g_top;
        if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH;
        if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;
        if (x1 <= x0 || y1 <= y0) continue;
        int w = x1 - x0, h = y1 - y0;
        size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
        if (bytes > VIEW_TRANSITION_NAMED_BUDGET - g_vt_named_bytes) continue;
        uint32_t* crop = malloc(bytes);
        if (!crop) continue;
        for (int y = 0; y < h; y++)
            memcpy(crop + (size_t)y * (size_t)w,
                   back_buffer + (size_t)(y0 + y) * (size_t)SCREEN_WIDTH + (size_t)x0,
                   (size_t)w * sizeof(uint32_t));
        VTNamedSnapshot* out = &g_vt_named[g_vt_named_count++];
        memset(out, 0, sizeof *out);
        snprintf(out->name, sizeof out->name, "%s", seen[i].name);
        out->x = x0; out->y = y0; out->w = w; out->h = h; out->pixels = crop;
        out->target_x = x0; out->target_y = y0; out->target_w = w; out->target_h = h;
        g_vt_named_bytes += bytes;
    }
}

void webpage_view_transition_cancel(void) {
    free(g_vt_snapshot); g_vt_snapshot = NULL;
    webpage_view_transition_named_clear();
    g_vt_width = g_vt_height = g_vt_top = 0; g_vt_head = NULL;
    g_vt_active = 0; g_vt_finished_pending = 0; g_vt_started_ms = g_vt_last_tick_ms = 0.0; g_vt_duration_ms = VIEW_TRANSITION_DURATION_MS;
}

void webpage_view_transition_capture(DOMNode* head) {
    /* A first-paint/foreign-document request cannot form a valid old frame. */
    if (!head || head != g_last_head || !back_buffer || SCREEN_WIDTH <= 0 || SCREEN_HEIGHT <= 0) {
        webpage_view_transition_cancel();
        return;
    }
    size_t pixels = (size_t)SCREEN_WIDTH * (size_t)SCREEN_HEIGHT;
    if (pixels > (size_t)128 * 1024 * 1024 / sizeof(uint32_t)) {
        webpage_view_transition_cancel();
        return;
    }
    size_t bytes = pixels * sizeof(uint32_t);
    uint32_t* snap = malloc(bytes);
    if (!snap) { webpage_view_transition_cancel(); return; }
    memcpy(snap, back_buffer, bytes);
    free(g_vt_snapshot); g_vt_snapshot = snap;
    webpage_view_transition_named_clear();
    g_vt_width = SCREEN_WIDTH; g_vt_height = SCREEN_HEIGHT; g_vt_top = g_top;
    g_vt_head = head; g_vt_active = 0;
    g_vt_started_ms = g_vt_last_tick_ms = 0.0; g_vt_duration_ms = VIEW_TRANSITION_DURATION_MS;
    webpage_view_transition_capture_named(head);
}

void webpage_view_transition_commit(DOMNode* head) {
    if (!g_vt_snapshot || !head || head != g_vt_head || head != g_last_head ||
        g_vt_width != SCREEN_WIDTH || g_vt_height != SCREEN_HEIGHT) {
        webpage_view_transition_cancel();
        /* No old-frame snapshot was available (e.g. transition started before
           the first paint); don't leave JS's finished promise pending forever. */
        g_vt_finished_pending = 1;
        return;
    }
    g_vt_duration_ms = webpage_view_transition_duration_ms();
    if (!isfinite(g_vt_duration_ms) || g_vt_duration_ms < 1.0) g_vt_duration_ms = VIEW_TRANSITION_DURATION_MS;
    if (g_vt_duration_ms > 30000.0) g_vt_duration_ms = 30000.0;
    g_vt_started_ms = view_transition_now_ms();
    g_vt_last_tick_ms = 0.0;
    g_vt_active = 1;
}

int webpage_view_transition_tick(void) {
    if (!g_vt_active || !g_vt_snapshot) return 0;
    double now = view_transition_now_ms();
    if (g_vt_last_tick_ms > 0.0 && now - g_vt_last_tick_ms < 15.0) return 0;
    g_vt_last_tick_ms = now;
    if (now - g_vt_started_ms >= g_vt_duration_ms) {
        webpage_view_transition_cancel();
        g_vt_finished_pending = 1;
        return 1; /* request one final frame containing only the new page */
    }
    return 1;
}

int webpage_view_transition_take_finished(void) {
    int ready = g_vt_finished_pending;
    g_vt_finished_pending = 0;
    return ready;
}

static void webpage_view_transition_capture_targets(DOMNode* head) {
    /* Paint workers are forked per frame on POSIX, so recapture the incoming
       crop on every clean new-page frame instead of relying on child memory to
       persist into the next render. The inline renderer uses the same path. */
    for (int i = 0; i < g_vt_named_count; i++) {
        VTNamedSnapshot* snap = &g_vt_named[i];
        if (snap->new_pixels) {
            size_t old_bytes=(size_t)snap->new_w*(size_t)snap->new_h*sizeof(uint32_t);
            free(snap->new_pixels); snap->new_pixels=NULL; snap->new_w=snap->new_h=0;
            if (g_vt_named_bytes >= old_bytes) g_vt_named_bytes-=old_bytes; else g_vt_named_bytes=0;
        }
        DOMNode* match = NULL; int matches = 0;
        for (DOMNode* n = head; n; n = n->next) {
            if (n->v2 && webpage_view_transition_name_valid(n->st.view_transition_name) &&
                !strcmp(n->st.view_transition_name, snap->name)) { match = n; matches++; if (matches > 1) break; }
        }
        snap->has_target=0;
        if (matches != 1 || !match || !l2_rects || match->idx < 0 || match->idx >= l2_nrects) continue;
        const float* r = &l2_rects[match->idx * 4];
        if (!(r[2] > 0.0f && r[3] > 0.0f)) continue;
        snap->target_x = (int)lrintf(r[0]); snap->target_y = (int)lrintf(r[1]);
        snap->target_w = (int)lrintf(r[2]); snap->target_h = (int)lrintf(r[3]);
        if (snap->target_w < 1) snap->target_w = 1;
        if (snap->target_h < 1) snap->target_h = 1;
        snap->has_target = 1;

        /* Capture the clean incoming element before the viewport compositor
           changes the page pixels. Only the visible intersection is stored. */
        int x0=(int)floorf(r[0]-(float)g_sx), y0=(int)floorf(r[1]-(float)g_sy+(float)g_top);
        int x1=(int)ceilf(r[0]-(float)g_sx+r[2]), y1=(int)ceilf(r[1]-(float)g_sy+(float)g_top+r[3]);
        if (x0<0) x0=0; if (y0<g_top) y0=g_top;
        if (x1>SCREEN_WIDTH) x1=SCREEN_WIDTH; if (y1>SCREEN_HEIGHT) y1=SCREEN_HEIGHT;
        if (x1<=x0 || y1<=y0) continue;
        int w=x1-x0,h=y1-y0;
        if ((size_t)w > SIZE_MAX/(size_t)h/sizeof(uint32_t)) continue;
        size_t bytes=(size_t)w*(size_t)h*sizeof(uint32_t);
        if (bytes > VIEW_TRANSITION_NAMED_BUDGET-g_vt_named_bytes) continue;
        uint32_t* crop=malloc(bytes); if (!crop) continue;
        for (int y=0;y<h;y++) memcpy(crop+(size_t)y*(size_t)w,
            back_buffer+(size_t)(y0+y)*(size_t)SCREEN_WIDTH+(size_t)x0,(size_t)w*sizeof(uint32_t));
        snap->new_pixels=crop; snap->new_w=w; snap->new_h=h; g_vt_named_bytes+=bytes;
    }
    g_vt_named_targets_ready = 1;
}

static void vt_fill_rect_alpha(int x0,int y0,int x1,int y1,uint32_t color,float opacity) {
    int alpha=alpha_of(color,opacity); if(alpha<=0)return;
    if(x0<0)x0=0;if(y0<g_top)y0=g_top;
    if(x1>SCREEN_WIDTH)x1=SCREEN_WIDTH;if(y1>SCREEN_HEIGHT)y1=SCREEN_HEIGHT;
    if(x1<=x0||y1<=y0)return;
    unsigned ia=(unsigned)(255-alpha),sr=(color>>16)&255u,sg=(color>>8)&255u,sb=color&255u;
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) {
        uint32_t* dst=&back_buffer[(size_t)y*(size_t)SCREEN_WIDTH+(size_t)x]; uint32_t d=*dst;
        unsigned dr=(d>>16)&255u,dg=(d>>8)&255u,db=d&255u;
        *dst=(d&0xFF000000u)|(((sr*(unsigned)alpha+dr*ia+127u)/255u)<<16)|
              (((sg*(unsigned)alpha+dg*ia+127u)/255u)<<8)|((sb*(unsigned)alpha+db*ia+127u)/255u);
    }
}

static void vt_affine_mul(const float a[6], const float b[6], float o[6]) {
    o[0]=a[0]*b[0]+a[2]*b[1]; o[1]=a[1]*b[0]+a[3]*b[1];
    o[2]=a[0]*b[2]+a[2]*b[3]; o[3]=a[1]*b[2]+a[3]*b[3];
    o[4]=a[0]*b[4]+a[2]*b[5]+a[4]; o[5]=a[1]*b[4]+a[3]*b[5]+a[5];
}

static void vt_scale_clip_for_raster(FxClip* cp, float rx, float ry) {
    if (!cp) return;
    if (cp->kind==CP_INSET) {
        cp->a[0].px*=ry; cp->a[1].px*=rx; cp->a[2].px*=ry; cp->a[3].px*=rx;
        for (int i=0;i<4;i++) cp->rad[i]*=(rx+ry)*0.5f;
    } else if (cp->kind==CP_CIRCLE) {
        cp->a[0].px*=(rx+ry)*0.5f; cp->a[1].px*=rx; cp->a[2].px*=ry;
    } else if (cp->kind==CP_ELLIPSE) {
        cp->a[0].px*=rx; cp->a[1].px*=ry; cp->a[2].px*=rx; cp->a[3].px*=ry;
    } else if (cp->kind==CP_POLYGON) {
        for (int i=0;i<cp->n && i<16;i++) { cp->pt[i][0].px*=rx; cp->pt[i][1].px*=ry; }
    }
}

static void vt_scale_mask_for_raster(FxMask* mask, float rx, float ry) {
    if (!mask) return;
    mask->w.px*=rx; mask->h.px*=ry;
    mask->px.px*=rx; mask->py.px*=ry;
}

/* Paint a snapshot as a real effect layer. Coordinates are first morphed to the
   shared-element destination box, then CSS transforms are composed from the
   leaf pseudo-element outward through image-pair and group. Filters, masks,
   clip-path and opacity are applied in the corresponding nested-layer order. */
static void vt_composite_snapshot(const uint32_t* pixels, int sw, int sh,
                                  float dx, float dy, float dw, float dh,
                                  float opacity, const NodeStyle* const chain[3], int top_offset) {
    if (!pixels || sw<1 || sh<1 || !isfinite(dx) || !isfinite(dy) ||
        !isfinite(dw) || !isfinite(dh) || dw<1.0f || dh<1.0f || opacity<=0.0f) return;
    if (opacity>1.0f) opacity=1.0f;
    const float limit=1000000.0f;
    if (dx < -limit) dx=-limit; if (dx > limit) dx=limit;
    if (dy < -limit) dy=-limit; if (dy > limit) dy=limit;
    if (dw>limit) dw=limit; if (dh>limit) dh=limit;
    /* Bound intermediate memory to 8 MiB even for a CSS box larger than the
       viewport. A uniform raster scale keeps percentages and shapes sensible. */
    const double max_px=2.0*1024.0*1024.0;
    double area=(double)dw*(double)dh; float quality=area>max_px?(float)sqrt(max_px/area):1.0f;
    int rw=(int)lrintf(dw*quality), rh=(int)lrintf(dh*quality);
    if (rw<1) rw=1; if (rh<1) rh=1;
    if ((size_t)rw>SIZE_MAX/(size_t)rh/4u) return;
    FxImg* im=fx_img_new(0,0,rw,rh); if (!im) return;
    for (int y=0;y<rh;y++) {
        int sy=(int)((int64_t)y*sh/rh); if(sy>=sh)sy=sh-1;
        uint8_t* d=im->px+(size_t)y*(size_t)rw*4u;
        const uint32_t* src=pixels+(size_t)sy*(size_t)sw;
        for(int x=0;x<rw;x++,d+=4) {
            int sx=(int)((int64_t)x*sw/rw); if(sx>=sw)sx=sw-1;
            uint32_t c=src[sx]; d[0]=(uint8_t)(c>>16); d[1]=(uint8_t)(c>>8); d[2]=(uint8_t)c; d[3]=255;
        }
    }
    float rx=(float)rw/dw, ry=(float)rh/dh;
    /* Children paint first, then parent pseudo-layer effects. */
    for (int i=2;i>=0;i--) {
        const NodeStyle* st=chain?chain[i]:NULL; if (!st) continue;
        if (st->filter.n) fx_apply_filters(im,&st->filter);
        if (st->clip.kind) { FxClip cp=st->clip; vt_scale_clip_for_raster(&cp,rx,ry); fx_apply_clip(im,&cp,0,0,(float)rw,(float)rh); }
        if (st->mask.kind) { FxMask mask=st->mask; vt_scale_mask_for_raster(&mask,rx,ry); fx_apply_mask(im,&mask,0,0,(float)rw,(float)rh); }
        float op=st->op_own;
        if (!isfinite(op)) op=1.0f; if(op<0)op=0; if(op>1)op=1;
        if (op<0.9999f) {
            for(size_t q=0, n=(size_t)rw*(size_t)rh;q<n;q++) {
                uint8_t* px=im->px+q*4u;
                px[0]=(uint8_t)(px[0]*op+0.5f); px[1]=(uint8_t)(px[1]*op+0.5f);
                px[2]=(uint8_t)(px[2]*op+0.5f); px[3]=(uint8_t)(px[3]*op+0.5f);
            }
        }
    }
    /* Source-local coordinates -> shared-element box in viewport coordinates. */
    float m[6]={dw/(float)rw,0,0,dh/(float)rh,dx,dy};
    /* Each computed style matrix is box-relative. Compose leaf -> root so the
       root group transform is the outermost operation as in the pseudo tree. */
    for (int i=2;i>=0;i--) {
        const NodeStyle* st=chain?chain[i]:NULL; if (!st || !st->xf.n) continue;
        float q[6]={1,0,0,1,0,0}, tx=0,ty=0;
        int linear=fx_xform_compute(&st->xf,dx,dy,dw,dh,q,&tx,&ty);
        if (!linear) { q[0]=q[3]=1; q[1]=q[2]=0; q[4]=tx; q[5]=ty; }
        float out[6]; vt_affine_mul(q,m,out); memcpy(m,out,sizeof m);
    }
    int cy0=top_offset<g_top?g_top:top_offset;
    if (cy0<0) cy0=0; if (cy0>SCREEN_HEIGHT) cy0=SCREEN_HEIGHT;
    fx_composite(im,back_buffer,SCREEN_WIDTH,0,cy0,SCREEN_WIDTH,SCREEN_HEIGHT,m);
    fx_img_free(im);
}

static int webpage_view_transition_named_pixel(int x, int y, int* incoming) {
    for (int i=0;i<g_vt_named_count;i++) {
        VTNamedSnapshot* s=&g_vt_named[i];
        if (s->pixels && x>=s->x && x<s->x+s->w && y>=s->y && y<s->y+s->h) { *incoming=1; return 1; }
        if (s->new_pixels && s->has_target) {
            int tx=s->target_x-g_sx, ty=s->target_y-g_sy+g_top;
            if (x>=tx && x<tx+s->target_w && y>=ty && y<ty+s->target_h) { *incoming=0; return 1; }
        }
    }
    return 0;
}

static void webpage_view_transition_composite_named(double progress) {
    if (g_vt_named_count <= 0) return;
    double elapsed_ms = progress * g_vt_duration_ms;
    const NodeStyle* root_style=webpage_view_transition_style_at(NULL,WEBPAGE_VT_PSEUDO_ROOT,elapsed_ms);
    float root_op=root_style?root_style->op_own:1.0f;
    if(root_op<0)root_op=0;if(root_op>1)root_op=1;
    for (int i=0;i<g_vt_named_count;i++) {
        VTNamedSnapshot* s=&g_vt_named[i];
        if (!s->pixels || s->w<1 || s->h<1) continue;
        float x=(float)s->x, y=(float)s->y, w=(float)s->w, h=(float)s->h;
        if (s->has_target) {
            float tx=(float)(s->target_x-g_sx), ty=(float)(s->target_y-g_sy+g_top);
            float tw=(float)s->target_w, th=(float)s->target_h;
            x += (tx-x)*(float)progress; y += (ty-y)*(float)progress;
            w += (tw-w)*(float)progress; h += (th-h)*(float)progress;
        }
        float oldx=x,oldy=y,oldw=w,oldh=h;
        const NodeStyle* group_style=webpage_view_transition_style_at(s->name,WEBPAGE_VT_PSEUDO_GROUP,elapsed_ms);
        const NodeStyle* pair_style=webpage_view_transition_style_at(s->name,WEBPAGE_VT_PSEUDO_IMAGE_PAIR,elapsed_ms);
        float old_fade=webpage_view_transition_animates_opacity(s->name,WEBPAGE_VT_PSEUDO_OLD)?1.0f:(float)(1.0-progress);
        const NodeStyle* old_style=webpage_view_transition_style_at(s->name,WEBPAGE_VT_PSEUDO_OLD,elapsed_ms);
        const NodeStyle* old_chain[3]={group_style,pair_style,old_style};
        vt_composite_snapshot(s->pixels,s->w,s->h,oldx,oldy,oldw,oldh,
                              old_fade*root_op,old_chain,g_top);
        if (s->new_pixels && s->has_target) {
            float newx=x,newy=y,neww=w,newh=h;
            float new_fade=webpage_view_transition_animates_opacity(s->name,WEBPAGE_VT_PSEUDO_NEW)?1.0f:(float)progress;
            const NodeStyle* new_style=webpage_view_transition_style_at(s->name,WEBPAGE_VT_PSEUDO_NEW,elapsed_ms);
            const NodeStyle* new_chain[3]={group_style,pair_style,new_style};
            vt_composite_snapshot(s->new_pixels,s->new_w,s->new_h,newx,newy,neww,newh,
                                  new_fade*root_op,new_chain,g_top);
        }
    }
}

static void webpage_view_transition_composite(int top_offset) {
    if (!g_vt_active || !g_vt_snapshot) return;
    if (g_vt_head != g_last_head || g_vt_width != SCREEN_WIDTH || g_vt_height != SCREEN_HEIGHT) {
        /* Resize/root changes abort the visual effect, but finished must still
           settle for a live document instead of waiting for a frame that cannot run. */
        webpage_view_transition_cancel();
        g_vt_finished_pending = 1;
        return;
    }
    webpage_view_transition_capture_targets(g_last_head);
    double progress = (view_transition_now_ms() - g_vt_started_ms) / g_vt_duration_ms;
    if (progress <= 0.0) progress = 0.0;
    if (progress >= 1.0) return;
    unsigned old_a = (unsigned)((1.0 - progress) * 255.0 + 0.5);
    if (old_a > 255) old_a = 255;
    unsigned new_a = 255u - old_a;
    double elapsed_ms = progress * g_vt_duration_ms;
    const NodeStyle* root_style=webpage_view_transition_style_at(NULL,WEBPAGE_VT_PSEUDO_ROOT,elapsed_ms);
    float root_op=root_style?root_style->op_own:1.0f;
    if(root_op<0)root_op=0;if(root_op>1)root_op=1;
    unsigned ra=(unsigned)(root_op*255.0f+0.5f), ria=255u-ra;
    int y0 = top_offset > g_vt_top ? top_offset : g_vt_top;
    if (y0 < 0) y0 = 0; if (y0 > SCREEN_HEIGHT) y0 = SCREEN_HEIGHT;
    for (int y = y0; y < SCREEN_HEIGHT; ++y) {
        size_t off = (size_t)y * (size_t)SCREEN_WIDTH;
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            uint32_t oldp = g_vt_snapshot[off + (size_t)x], newp = back_buffer[off + (size_t)x];
            int incoming=0;
            if (webpage_view_transition_named_pixel(x,y,&incoming)) {
                /* At the old location expose the new page behind the outgoing
                   image; at the incoming location expose the old page behind
                   the incoming image. The two captures are composited below. */
                back_buffer[off + (size_t)x] = incoming ? newp : oldp;
                continue;
            }
            unsigned or_ = (oldp >> 16) & 255u, og = (oldp >> 8) & 255u, ob = oldp & 255u;
            unsigned nr = (newp >> 16) & 255u, ng = (newp >> 8) & 255u, nb = newp & 255u;
            unsigned r = (or_ * old_a + nr * new_a + 127u) / 255u;
            unsigned g = (og * old_a + ng * new_a + 127u) / 255u;
            unsigned b = (ob * old_a + nb * new_a + 127u) / 255u;
            r=(r*ra+nr*ria+127u)/255u; g=(g*ra+ng*ria+127u)/255u; b=(b*ra+nb*ria+127u)/255u;
            back_buffer[off + (size_t)x] = (newp & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
    if(root_style && (root_style->bg>>24))
        vt_fill_rect_alpha(0,top_offset,SCREEN_WIDTH,SCREEN_HEIGHT,root_style->bg,root_op);
    webpage_view_transition_composite_named(progress);
}

/* покрытие пикселя скруглённым прямоугольником */
static float rr_cov(float px, float py, float x, float y, float w, float h, const float r[4]) {
    float cx, cy, rad = 0;
    if (px < x + r[0] && py < y + r[0]) { cx = x + r[0]; cy = y + r[0]; rad = r[0]; }
    else if (px > x + w - r[1] && py < y + r[1]) { cx = x + w - r[1]; cy = y + r[1]; rad = r[1]; }
    else if (px > x + w - r[2] && py > y + h - r[2]) { cx = x + w - r[2]; cy = y + h - r[2]; rad = r[2]; }
    else if (px < x + r[3] && py > y + h - r[3]) { cx = x + r[3]; cy = y + h - r[3]; rad = r[3]; }
    else { float dx = fminf(px - x, x + w - px), dy = fminf(py - y, y + h - py); float c = fminf(dx, dy) + 0.5f; return c < 0 ? 0 : c > 1 ? 1 : c; }
    float d = hypotf(px - cx, py - cy) - rad; float c = 0.5f - d; return c < 0 ? 0 : c > 1 ? 1 : c;
}

static void resolve_radii(const NodeStyle* st, float w, float h, float out[4]) {
    float m = fminf(w, h) / 2;
    for (int i = 0; i < 4; i++) { float r = st->radius[i]; if (r < 0) r = -r * fminf(w, h); out[i] = r > m ? m : r < 0 ? 0 : r; }
}

static void fill_rrect(int x0, int y0, int x1, int y1, const float rad[4], uint32_t argb, float opacity) {
    int a = alpha_of(argb, opacity); if (a <= 0 || x1 <= x0 || y1 <= y0) return;
    if (rad[0] + rad[1] + rad[2] + rad[3] < 0.5f) { fill_rect_i(x0, y0, x1, y1, argb, opacity); return; }
    float w = (float)(x1 - x0), h = (float)(y1 - y0);
    int ys = y0 < cl_y0 ? cl_y0 : y0, ye = y1 > cl_y1 ? cl_y1 : y1, xs = x0 < cl_x0 ? cl_x0 : x0, xe = x1 > cl_x1 ? cl_x1 : x1;
    float mt = fmaxf(rad[0], rad[1]), mb = fmaxf(rad[2], rad[3]), ml = fmaxf(rad[0], rad[3]), mr = fmaxf(rad[1], rad[2]);
    for (int y = ys; y < ye; y++) {
        int corner_row = (y < y0 + mt + 1) || (y >= y1 - mb - 1);
        for (int x = xs; x < xe; x++) {
            if (corner_row || x < x0 + ml + 1 || x >= x1 - mr - 1) { float c = rr_cov(x + 0.5f, y + 0.5f, (float)x0, (float)y0, w, h, rad); blend(x, y, argb, (int)(a * c + 0.5f)); }
            else blend(x, y, argb, a);
        }
    }
}

/* ══════════════ градиенты (linear-/radial-gradient) ══════════════ */
static uint32_t grad_color_at(const NodeStyle* st, float t) {
    int n = st->bg_grad_n; if (n < 1) return st->bg;
    if (t <= st->bg_grad_p[0]) return st->bg_grad_c[0];
    if (t >= st->bg_grad_p[n - 1]) return st->bg_grad_c[n - 1];
    for (int i = 0; i < n - 1; i++) {
        float p0 = st->bg_grad_p[i], p1 = st->bg_grad_p[i + 1];
        if (t >= p0 && t <= p1) {
            float f = (p1 > p0) ? (t - p0) / (p1 - p0) : 0;
            uint32_t c0 = st->bg_grad_c[i], c1 = st->bg_grad_c[i + 1];
            int a0 = (c0 >> 24) & 255, r0 = (c0 >> 16) & 255, g0 = (c0 >> 8) & 255, b0 = c0 & 255;
            int a1 = (c1 >> 24) & 255, r1 = (c1 >> 16) & 255, g1 = (c1 >> 8) & 255, b1 = c1 & 255;
            uint32_t a = (uint32_t)(a0 + (a1 - a0) * f + 0.5f), r = (uint32_t)(r0 + (r1 - r0) * f + 0.5f);
            uint32_t g = (uint32_t)(g0 + (g1 - g0) * f + 0.5f), b = (uint32_t)(b0 + (b1 - b0) * f + 0.5f);
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return st->bg_grad_c[n - 1];
}

static void fill_rrect_grad(int x0, int y0, int x1, int y1, const float rad[4], const NodeStyle* st, float opacity) {
    if (x1 <= x0 || y1 <= y0) return;
    float w = (float)(x1 - x0), h = (float)(y1 - y0), cx = w * 0.5f, cy = h * 0.5f;
    float ang = st->bg_grad_angle * 3.14159265f / 180.0f, dirx = sinf(ang), diry = -cosf(ang);
    float half = fabsf(w * 0.5f * dirx) + fabsf(h * 0.5f * diry); if (half < 1) half = 1;
    float maxr = sqrtf(cx * cx + cy * cy); if (maxr < 1) maxr = 1;
    int ys = y0 < cl_y0 ? cl_y0 : y0, ye = y1 > cl_y1 ? cl_y1 : y1, xs = x0 < cl_x0 ? cl_x0 : x0, xe = x1 > cl_x1 ? cl_x1 : x1;
    for (int y = ys; y < ye; y++) for (int x = xs; x < xe; x++) {
        float cov = rr_cov(x + 0.5f, y + 0.5f, (float)x0, (float)y0, w, h, rad); if (cov <= 0) continue;
        float px = (x + 0.5f - x0) - cx, py = (y + 0.5f - y0) - cy, t;
        if (st->bg_grad_type == 2) t = sqrtf(px * px + py * py) / maxr * 100.0f;
        else if (st->bg_grad_type == 3) { float deg = atan2f(px, -py) * 57.295779513f; if (deg < 0) deg += 360.0f; t = fmodf(deg - st->bg_grad_angle + 360.0f, 360.0f) / 360.0f * 100.0f; }
        else t = ((px * dirx + py * diry) / half * 0.5f + 0.5f) * 100.0f;
        if (t < 0) t = 0; if (t > 100) t = 100;
        uint32_t col = grad_color_at(st, t); int a = alpha_of(col, opacity);
        blend(x, y, col, (int)(a * cov + 0.5f));
    }
}

static void draw_border(const NodeStyle* st, int x0, int y0, int x1, int y1, const float rad[4], float opacity) {
    float bw[4] = {st->border_w[0], st->border_w[1], st->border_w[2], st->border_w[3]};
    int w = x1 - x0, h = y1 - y0; if (w <= 0 || h <= 0) return;
    int ib[4]; for (int i = 0; i < 4; i++) { ib[i] = bw[i] > 0 ? (int)(bw[i] + 0.5f) : 0; if (bw[i] > 0 && ib[i] == 0) ib[i] = 1; }
    if (!(ib[0] | ib[1] | ib[2] | ib[3])) return;
    if (rad[0] + rad[1] + rad[2] + rad[3] < 0.5f) {
        for (int side = 0; side < 4; side++) {
            if (!ib[side] || st->border_style[side] == BS_NONE) continue;
            uint32_t c = st->border_c[side]; int t = ib[side]; int rx0 = x0, ry0 = y0, rx1 = x1, ry1 = y1;
            if (side == 0) ry1 = y0 + t; else if (side == 2) ry0 = y1 - t; else if (side == 1) { rx0 = x1 - t; ry0 = y0 + ib[0]; ry1 = y1 - ib[2]; } else { rx1 = x0 + t; ry0 = y0 + ib[0]; ry1 = y1 - ib[2]; }
            if (st->border_style[side] == BS_SOLID) fill_rect_i(rx0, ry0, rx1, ry1, c, opacity);
            else {
                int horiz = side == 0 || side == 2, len = horiz ? rx1 - rx0 : ry1 - ry0; int dash = st->border_style[side] == BS_DOTTED ? t : t * 3; if (dash < 2) dash = 2;
                for (int p = 0; p < len; p += dash * 2) { int q = p + dash < len ? p + dash : len; if (horiz) fill_rect_i(rx0 + p, ry0, rx0 + q, ry1, c, opacity); else fill_rect_i(rx0, ry0 + p, rx1, ry0 + q, c, opacity); }
            }
        }
        return;
    }
    uint32_t col = ib[0] ? st->border_c[0] : ib[3] ? st->border_c[3] : ib[1] ? st->border_c[1] : st->border_c[2];
    int a = alpha_of(col, opacity); if (a <= 0) return;
    float inr[4] = {fmaxf(rad[0] - fmaxf(ib[0], ib[3]), 0), fmaxf(rad[1] - fmaxf(ib[0], ib[1]), 0), fmaxf(rad[2] - fmaxf(ib[2], ib[1]), 0), fmaxf(rad[3] - fmaxf(ib[2], ib[3]), 0)};
    float ix = (float)(x0 + ib[3]), iy = (float)(y0 + ib[0]), iw = (float)(w - ib[1] - ib[3]), ih = (float)(h - ib[0] - ib[2]);
    float mt = fmaxf(fmaxf(rad[0], rad[1]), (float)ib[0]), mb = fmaxf(fmaxf(rad[2], rad[3]), (float)ib[2]);
    float ml = fmaxf(fmaxf(rad[0], rad[3]), (float)ib[3]), mr = fmaxf(fmaxf(rad[1], rad[2]), (float)ib[1]);
    int ys = y0 < cl_y0 ? cl_y0 : y0, ye = y1 > cl_y1 ? cl_y1 : y1, xs = x0 < cl_x0 ? cl_x0 : x0, xe = x1 > cl_x1 ? cl_x1 : x1;
    for (int y = ys; y < ye; y++) {
        int band = (y < y0 + mt + 1) || (y >= y1 - mb - 1);
        for (int x = xs; x < xe; x++) {
            if (!band && x >= x0 + ml + 1 && x < x1 - mr - 1) { int lim = (int)ceilf(x1 - mr - 1); if (lim - 1 > x) { x = lim - 1; continue; } }
            float co = rr_cov(x + 0.5f, y + 0.5f, (float)x0, (float)y0, (float)w, (float)h, rad);
            float ci = (iw > 0 && ih > 0) ? rr_cov(x + 0.5f, y + 0.5f, ix, iy, iw, ih, inr) : 0;
            float c = co * (1 - ci); if (c > 0) blend(x, y, col, (int)(a * c + 0.5f));
        }
    }
}

static void draw_shadow(const NodeStyle* st, int x0, int y0, int x1, int y1, const float rad[4], float opacity) {
    if (!(st->sh_color >> 24)) return;
    float blur = st->sh_blur < 0 ? 0 : st->sh_blur; int L = blur < 1 ? 1 : (int)fminf(blur, 14.0f);
    for (int k = L; k >= 1; k--) {
        float e = blur * k / L * 0.5f + st->sh_spread; float a = (float)(st->sh_color >> 24) * (1.0f - (float)(k - 1) / (L + 1)) * 2.0f / (L + 1) * 0.9f;
        uint32_t c = ((uint32_t)(a > 255 ? 255 : a) << 24) | (st->sh_color & 0xFFFFFF);
        float r2[4]; for (int i = 0; i < 4; i++) r2[i] = rad[i] + (e > 0 ? e : 0);
        fill_rrect((int)(x0 + st->sh_x - e), (int)(y0 + st->sh_y - e), (int)(x1 + st->sh_x + e), (int)(y1 + st->sh_y + e), r2, c, opacity);
    }
}

/* ══════════════ текст ══════════════ */
static uint32_t effective_text_color(uint32_t argb, float opacity, int sx, int sy) {
    int a = alpha_of(argb, opacity);
    if (a >= 255) return argb & 0xFFFFFF;
    uint32_t bg = 0xFFFFFF;
    if (sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SCREEN_HEIGHT) bg = back_buffer[(size_t)sy * SCREEN_WIDTH + sx] & 0xFFFFFF;
    int r = (int)((((argb >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255), g = (int)((((argb >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255), b = (int)(((argb & 255) * a + (bg & 255) * (255 - a)) / 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_text_mode(TTFFont* f, const char* s, int x, int y, int size, uint32_t argb, float opacity,
                           int style, int direction, int bidi_override) {
    if (!f || !s[0]) return;
    uint32_t col = effective_text_color(argb, opacity, x + 2, y + size / 2);
    ttf_render_string_ex_bidi(f, s, back_buffer, SCREEN_WIDTH, cl_x0, cl_y0, cl_x1, cl_y1,
                              size, x, y, col, style, direction, bidi_override);
}
static void draw_text(TTFFont* f, const char* s, int x, int y, int size, uint32_t argb, float opacity, int style) {
    draw_text_mode(f,s,x,y,size,argb,opacity,style,0,0);
}

static int sel_rect(int* sx, int* sy, int* sw, int* sh) {
    if (!layout_selection_active) return 0;
    *sx = layout_selection_x1 < layout_selection_x2 ? layout_selection_x1 : layout_selection_x2;
    *sy = layout_selection_y1 < layout_selection_y2 ? layout_selection_y1 : layout_selection_y2;
    *sw = abs(layout_selection_x2 - layout_selection_x1); *sh = abs(layout_selection_y2 - layout_selection_y1); return 1;
}
static int text_pos_compare(const WebTextPosition* a, const WebTextPosition* b) {
    if (!a || !b || !a->node || !b->node) return 0;
    if (a->node->idx != b->node->idx) return a->node->idx < b->node->idx ? -1 : 1;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    return 0;
}

static int text_selection_ordered(WebTextPosition* first, WebTextPosition* last) {
    if (!g_text_sel_active || !g_text_sel_anchor.node || !g_text_sel_focus.node) return 0;
    if (text_pos_compare(&g_text_sel_anchor, &g_text_sel_focus) <= 0) {
        *first = g_text_sel_anchor; *last = g_text_sel_focus;
    } else { *first = g_text_sel_focus; *last = g_text_sel_anchor; }
    return 1;
}

/* Map a C_TEXT run to the selected byte interval in its source DOM text node.
 * Runs whose display text was normalized (e.g. a multi-space run) can be selected
 * as a whole when they sit between the endpoint nodes, but are never used as caret
 * anchors because they do not have a one-to-one source-byte map. */
static int text_selection_cmd_range(const Cmd* c, size_t n, size_t* out_lo, size_t* out_hi) {
    WebTextPosition first, last;
    if (!c || c->t != C_TEXT || !c->node || c->node->idx < 0 || !out_lo || !out_hi ||
        !text_selection_ordered(&first, &last)) return 0;
    int idx = c->node->idx, aidx = first.node->idx, bidx = last.node->idx;
    if (idx < aidx || idx > bidx || n == 0) return 0;
    if (!c->text_offset_valid) {
        if ((idx > aidx && idx < bidx) ||
            (aidx == bidx && idx == aidx && first.offset == 0 && last.offset >= strlen(c->node->text))) {
            *out_lo = 0; *out_hi = n; return 1;
        }
        return 0;
    }
    size_t node_len = strlen(c->node->text), begin = c->text_offset;
    if (begin > node_len || n > node_len - begin) return 0;
    size_t lo = 0, hi = n;
    if (idx == aidx && first.offset > begin) lo = (size_t)first.offset - begin;
    if (idx == bidx) {
        if (last.offset <= begin) return 0;
        if ((size_t)last.offset < begin + hi) hi = (size_t)last.offset - begin;
    }
    if (lo > n) lo = n; if (hi > n) hi = n;
    if (hi <= lo) return 0;
    *out_lo = lo; *out_hi = hi; return 1;
}

static int rect_hit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) { return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by; }

static int text_base_direction(const NodeStyle* st) {
    if (!st) return 0;
    if (st->bidi_context == 3) return -1;
    if (st->bidi_context == 2) return 1;
    if (st->bidi_context == 1) return 0;
    return st->unicode_bidi == 5 ? -1 : (st->direction ? 1 : 0);
}
static int text_bidi_override(const NodeStyle* st) {
    return st && ((st->bidi_context == 1 || st->bidi_context == 2) || st->unicode_bidi == 3 || st->unicode_bidi == 4);
}

static void paint_text(const Cmd* c, int dx, int dy) {
    TTFFont* f = font_manager_get_styled(c->fam, c->weight ? c->weight : 400, c->ital); if (!f) return;
    int x = (int)floorf(c->x + dx + 0.5f), y = (int)floorf(c->y + dy + 0.5f);
    int sx = 0, sy = 0, sw = 0, sh = 0; const NodeStyle* ost = c->node ? &c->node->st : NULL;
    size_t n = strlen(c->s), sel_lo = 0, sel_hi = 0;
    int logical_range = g_text_sel_active && (!ost || ost->user_select != 1) &&
                        text_selection_cmd_range(c, n, &sel_lo, &sel_hi);
    size_t caret_local = 0;
    int logical_caret = g_text_sel_active && (!ost || ost->user_select != 1) &&
        text_pos_compare(&g_text_sel_anchor, &g_text_sel_focus) == 0 &&
        g_text_sel_focus.node == c->node && c->text_offset_valid &&
        g_text_sel_focus.offset >= c->text_offset &&
        (size_t)(g_text_sel_focus.offset - c->text_offset) <= n &&
        c->text_offset <= strlen(c->node->text) && n <= strlen(c->node->text) - c->text_offset &&
        memcmp(c->node->text + c->text_offset, c->s, n) == 0;
    if (logical_caret) caret_local = (size_t)(g_text_sel_focus.offset - c->text_offset);
    int rect_mode = !g_text_sel_active && (!ost || ost->user_select != 1) && sel_rect(&sx, &sy, &sw, &sh) &&
                    rect_hit(x, y, (int)c->w + 1, (int)c->h, sx, sy, sw, sh);
    if (logical_range || rect_mode || logical_caret) {
        int* offs = n <= (size_t)INT_MAX - 1 ? malloc((n + 1) * sizeof(int)) : NULL;
        if (offs) {
            ttf_text_offsets_ex_bidi(f, c->s, c->size, c->tstyle, offs, (int)n + 1, text_base_direction(ost), text_bidi_override(ost));
            for (size_t i = 0; i < n;) {
                size_t l = 1; unsigned char ch = (unsigned char)c->s[i]; if (ch >= 0xF0) l = 4; else if (ch >= 0xE0) l = 3; else if (ch >= 0xC0) l = 2; if (i + l > n) l = n - i;
                int selected = logical_range ? (i < sel_hi && i + l > sel_lo) : 0;
                int oa = offs[i], ob = offs[i + l];
                int cx0 = x + (oa < ob ? oa : ob), cw = abs(ob - oa);
                if (!logical_range && rect_mode) selected = cw > 0 && rect_hit(cx0, y, cw, (int)c->h, sx, sy, sw, sh);
                if (selected && cw > 0) fill_rect_i(cx0, y, cx0 + cw, y + (int)c->h, 0xFFBFDBFE, 1);
                i += l;
            }
            if (logical_caret && caret_local <= n) {
                float cy = (float)y + c->h * 0.5f;
                float tolerance = fmaxf(8.0f, c->h * 0.55f);
                if (fabsf(cy - g_text_sel_focus.visual_y) <= tolerance) {
                    int cx = x + offs[caret_local];
                    uint32_t caret_color = 0xFF000000u | (effective_text_color(c->color, c->opacity, cx, (int)cy) & 0xFFFFFFu);
                    fill_rect_i(cx, y, cx + 1, y + (int)fmaxf(1.0f, c->h), caret_color, 1.0f);
                }
            }
            free(offs);
        }
    }
    if (ost && (ost->ts_color >> 24) && !(ost->bg_clip_text && ost->bg_grad_type)) {
        int blur = (int)fminf(4.0f, fmaxf(0.0f, ost->ts_blur));
        for (int oy = -blur; oy <= blur; oy++) for (int ox = -blur; ox <= blur; ox++) {
            if (blur && ox*ox + oy*oy > blur*blur) continue;
            draw_text_mode(f, c->s, x + (int)ost->ts_x + ox, y + (int)ost->ts_y + oy, c->size, ost->ts_color, c->opacity * (blur ? 0.32f : 1.0f), c->tstyle, text_base_direction(ost), text_bidi_override(ost));
        }
    }
    if (ost && ost->bg_clip_text && ost->bg_grad_type) {                       /* -webkit-background-clip:text — красим по буквам вдоль градиента */
        size_t n = strlen(c->s); int* offs = malloc((n + 1) * sizeof(int));
        if (offs) {
            int measured_width = ttf_text_offsets_ex_bidi(f, c->s, c->size, c->tstyle, offs, (int)n + 1, text_base_direction(ost), text_bidi_override(ost));
            float fullw = (float)measured_width; if (fullw < 1) fullw = 1;
            size_t i = 0;
            while (i < n) {
                size_t l = 1; unsigned char ch = (unsigned char)c->s[i];
                if (ch >= 0xF0) l = 4; else if (ch >= 0xE0) l = 3; else if (ch >= 0xC0) l = 2;
                if (i + l > n) l = n - i;
                float mid = (offs[i] + offs[i + l]) * 0.5f, t = mid / fullw * 100.0f;
                uint32_t col = grad_color_at(ost, t);
                char tmp[8]; size_t cl = l < 7 ? l : 7; memcpy(tmp, c->s + i, cl); tmp[cl] = 0;
                draw_text_mode(f, tmp, x + (offs[i] < offs[i + l] ? offs[i] : offs[i + l]), y, c->size, (col & 0xFFFFFFu) | 0xFF000000u, c->opacity, c->tstyle, text_base_direction(ost), text_bidi_override(ost));
                i += l;
            }
            free(offs);
        } else draw_text_mode(f, c->s, x, y, c->size, c->color, c->opacity, c->tstyle, text_base_direction(ost), text_bidi_override(ost));
    } else draw_text_mode(f, c->s, x, y, c->size, c->color, c->opacity, c->tstyle, text_base_direction(ost), text_bidi_override(ost));
    if (c->deco) {
        int asc = ttf_font_ascent(f, c->size); int base = y + asc; int th = c->size >= 28 ? 2 : 1; int w = (int)(c->w + 0.5f);
        uint32_t col = effective_text_color(c->color, c->opacity, x + 2, y + c->size / 2);
        if (c->deco & DECO_UNDERLINE) fill_rect_i(x, base + 1 + c->size / 16, x + w, base + 1 + c->size / 16 + th, 0xFF000000u | col, 1);
        if (c->deco & DECO_LINE_THROUGH) fill_rect_i(x, base - c->size / 4, x + w, base - c->size / 4 + th, 0xFF000000u | col, 1);
        if (c->deco & DECO_OVERLINE) fill_rect_i(x, y, x + w, y + th, 0xFF000000u | col, 1);
    }
}

/* ══════════════ изображения ══════════════ */
static void blit_img(ImgPix* im, int x, int y, int w, int h, const float rad[4], float opacity) {
    if (!im) return;
    int ys = y < cl_y0 ? cl_y0 : y, ye = y + h > cl_y1 ? cl_y1 : y + h, xs = x < cl_x0 ? cl_x0 : x, xe = x + w > cl_x1 ? cl_x1 : x + w;
    int round = rad[0] + rad[1] + rad[2] + rad[3] >= 0.5f;
    for (int yy = ys; yy < ye; yy++) for (int xx = xs; xx < xe; xx++) {
        int sx = xx - x, sy = yy - y; if (sx >= im->w || sy >= im->h) continue;
        uint32_t p = im->px[(size_t)sy * im->w + sx]; float a = (p >> 24) * opacity;
        if (round) a *= rr_cov(xx + 0.5f, yy + 0.5f, (float)x, (float)y, (float)w, (float)h, rad);
        blend(xx, yy, p, (int)(a + 0.5f));
    }
}

static void paint_bgimg(const Cmd* c, int dx, int dy) {
    webpage_image_set_color(0xFF000000u);                                       /* у SVG-фона currentColor — чёрный (нет контекста элемента) */
    const NodeStyle* st = &c->node->st; ImgPix* src = webpage_image_get(c->s); if (!src) return;
    int x0 = (int)floorf(c->x + dx + 0.5f), y0 = (int)floorf(c->y + dy + 0.5f), w = (int)floorf(c->w + 0.5f), h = (int)floorf(c->h + 0.5f);
    x0 += (int)st->border_w[3]; y0 += (int)st->border_w[0]; w -= (int)(st->border_w[1] + st->border_w[3]); h -= (int)(st->border_w[0] + st->border_w[2]);
    if (w <= 0 || h <= 0) return;
    float iw = (float)src->w, ih = (float)src->h, tw = iw, th = ih;
    if (st->bg_size_mode == 1) { float s = fmaxf((float)w / iw, (float)h / ih); tw = iw * s; th = ih * s; }
    else if (st->bg_size_mode == 2) { float s = fminf((float)w / iw, (float)h / ih); tw = iw * s; th = ih * s; }
    else if (st->bg_size_mode == 3) {
        float lw = st->bg_w.set ? st->bg_w.px + st->bg_w.pct * w / 100.0f : -1, lh = st->bg_h.set ? st->bg_h.px + st->bg_h.pct * h / 100.0f : -1;
        if (lw >= 0 && lh >= 0) { tw = lw; th = lh; } else if (lw >= 0) { tw = lw; th = ih * lw / iw; } else if (lh >= 0) { th = lh; tw = iw * lh / ih; }
    }
    int itw = (int)(tw + 0.5f), ith = (int)(th + 0.5f); if (itw < 1 || ith < 1) return;
    ImgPix* im = (itw == src->w && ith == src->h) ? src : webpage_image_scaled(c->s, itw, ith); if (!im) return;
    float px = st->bg_px.set ? st->bg_px.px + st->bg_px.pct * (w - tw) / 100.0f : 0, py = st->bg_py.set ? st->bg_py.px + st->bg_py.pct * (h - th) / 100.0f : 0;
    int rx = st->bg_repeat == 0 || st->bg_repeat == 1, ry = st->bg_repeat == 0 || st->bg_repeat == 2;
    int save[4] = {cl_x0, cl_y0, cl_x1, cl_y1};
    if (x0 > cl_x0) cl_x0 = x0; if (y0 > cl_y0) cl_y0 = y0; if (x0 + w < cl_x1) cl_x1 = x0 + w; if (y0 + h < cl_y1) cl_y1 = y0 + h;
    float r0[4] = {0, 0, 0, 0}; float rad[4]; resolve_radii(st, (float)w, (float)h, rad); (void)r0;
    int startx = (int)px, starty = (int)py;
    if (rx) while (startx > 0) startx -= itw; if (ry) while (starty > 0) starty -= ith;
    for (int ty = starty; ty < h; ty += ith) { for (int tx = startx; tx < w; tx += itw) { blit_img(im, x0 + tx, y0 + ty, itw, ith, rad, c->opacity); if (!rx) break; } if (!ry) break; }
    cl_x0 = save[0]; cl_y0 = save[1]; cl_x1 = save[2]; cl_y1 = save[3];
}

/* ══════════════ элементы форм ══════════════ */
static const char* option_label(DOMNode* sel, char* buf, size_t cap) {
    DOMNode* pick = NULL; DOMNode* first = NULL;
    for (DOMNode* n = sel->next; n && n->idx <= sel->subtree_end; n = n->next) if (n->type == TAG_OPTION) { if (!first) first = n; if (n->selected && !pick) pick = n; }
    if (!pick) pick = first; buf[0] = 0; if (!pick) return buf;
    for (DOMNode* n = pick->next; n && n->idx <= pick->subtree_end; n = n->next) if (n->type == TAG_TEXT) { size_t l = strlen(buf); snprintf(buf + l, cap - l, "%s", n->text); }
    return buf;
}

static void paint_ctrl(const Cmd* c, int dx, int dy) {
    DOMNode* n = c->node; const NodeStyle* st = &n->st;
    int x = (int)floorf(c->x + dx + 0.5f), y = (int)floorf(c->y + dy + 0.5f), w = (int)(c->w + 0.5f), h = (int)(c->h + 0.5f);
    TTFFont* f = font_manager_get(n->font_family); int size = (int)(st->font_size + 0.5f); if (size < 1) size = 1;
    int asc = f ? ttf_font_ascent(f, size) : size, desc = f ? ttf_font_descent(f, size) : 0, lh = asc + desc;
    const char* t = n->input_type;
    int focused = (n == focused_node);
    if (n->type == TAG_IMG) { if (n->alt[0]) draw_text(f, n->alt, x, y, size, 0xFF777777u, c->opacity, 0); return; }
    if (n->type == TAG_INPUT && (!strcmp(t, "checkbox") || !strcmp(t, "radio"))) {
        int radio = !strcmp(t, "radio"); float rad[4] = {2, 2, 2, 2}; if (radio) rad[0] = rad[1] = rad[2] = rad[3] = 6.5f;
        int s = w < h ? w : h; int bx = x + (w - s) / 2, by = y + (h - s) / 2;
        fill_rrect(bx, by, bx + s, by + s, rad, n->checked ? 0xFF1A73E8u : 0xFF767676u, 1);
        float in[4] = {rad[0] > 1 ? rad[0] - 1 : 0, rad[1] > 1 ? rad[1] - 1 : 0, rad[2] > 1 ? rad[2] - 1 : 0, rad[3] > 1 ? rad[3] - 1 : 0};
        fill_rrect(bx + 1, by + 1, bx + s - 1, by + s - 1, in, n->checked ? 0xFF1A73E8u : 0xFFFFFFFFu, 1);
        if (n->checked) { if (radio) { float d[4] = {2.5f, 2.5f, 2.5f, 2.5f}; fill_rrect(bx + s / 2 - 2, by + s / 2 - 2, bx + s / 2 + 3, by + s / 2 + 3, d, 0xFFFFFFFFu, 1); }
            else { for (int i = 0; i < 3; i++) { fill_rect_i(bx + 3 + i, by + 6 + i, bx + 4 + i, by + 8 + i, 0xFFFFFFFFu, 1); } for (int i = 0; i < 6; i++) fill_rect_i(bx + 5 + i, by + 8 - i, bx + 6 + i, by + 10 - i, 0xFFFFFFFFu, 1); } }
        return;
    }
    if (n->type == TAG_INPUT && (!strcmp(t, "submit") || !strcmp(t, "button") || !strcmp(t, "reset"))) {
        const char* v = n->value[0] ? n->value : !strcmp(t, "submit") ? "Submit" : !strcmp(t, "reset") ? "Reset" : "";
        int tw = f ? (int)(ttf_text_width_f(f, v, size, 0) + 0.5f) : 0; draw_text(f, v, x + (w - tw) / 2, y + (h - lh) / 2, size, st->color, c->opacity, 0); return;
    }
    if (n->type == TAG_SELECT) {
        char lb[256]; option_label(n, lb, sizeof lb); draw_text(f, lb, x, y + (h - lh) / 2, size, st->color, c->opacity, 0);
        int ax = x + w - 12, ay = y + h / 2 - 2; for (int i = 0; i < 4; i++) fill_rect_i(ax + i, ay + i, ax + 10 - i, ay + i + 1, 0xFF555555u, 1); return;
    }
    if (n->type == TAG_TEXTAREA) {
        int save[4] = {cl_x0, cl_y0, cl_x1, cl_y1}; if (x > cl_x0) cl_x0 = x; if (y > cl_y0) cl_y0 = y; if (x + w < cl_x1) cl_x1 = x + w; if (y + h < cl_y1) cl_y1 = y + h;
        const char* v = n->value[0] ? n->value : n->placeholder; uint32_t col = n->value[0] ? st->color : 0xFF757575u;
        int ly = y; char line[512]; const char* p = v;
        while (*p && ly < y + h + lh) {
            size_t k = 0; int lw = 0;
            while (p[k] && p[k] != '\n' && k < sizeof line - 1) { line[k] = p[k]; line[k + 1] = 0; if (f && (int)ttf_text_width_f(f, line, size, 0) > w && k > 0) { line[k] = 0; break; } k++; }
            line[k] = 0; (void)lw; draw_text(f, line, x, ly, size, col, c->opacity, 0); ly += lh; p += k; if (*p == '\n') p++;
        }
        cl_x0 = save[0]; cl_y0 = save[1]; cl_x1 = save[2]; cl_y1 = save[3]; return;
    }
    /* текстовое поле */
    char shown[2100]; const char* v = n->value; uint32_t col = st->color; float op = c->opacity;
    if (!strcmp(t, "password")) { size_t l = strlen(v), k = 0; for (size_t i = 0; i < l && k < 2090; i++) if (((unsigned char)v[i] & 0xC0) != 0x80) { memcpy(shown + k, "\xE2\x80\xA2", 3); k += 3; } shown[k] = 0; v = shown; }
    if (!v[0] && !(focused && cursor_visible)) { v = n->placeholder; col = 0xFF757575u; }
    else if (!v[0]) v = "";
    int save[4] = {cl_x0, cl_y0, cl_x1, cl_y1}; if (x > cl_x0) cl_x0 = x; if (y - 2 > cl_y0) cl_y0 = y - 2; if (x + w < cl_x1) cl_x1 = x + w; if (y + h + 2 < cl_y1) cl_y1 = y + h + 2;
    int ty = y + (h - lh) / 2; int tw = f ? (int)(ttf_text_width_f(f, v, size, 0) + 0.5f) : 0; int tx = x;
    if (focused && tw > w - 2) tx = x + (w - 2 - tw);                                    /* прокрутка к концу */
    draw_text(f, v, tx, ty, size, col, op, 0);
    if (focused && cursor_visible) fill_rect_i(tx + tw + 1, ty, tx + tw + 2, ty + lh, 0xFF000000u | (st->color & 0xFFFFFF), 1);
    cl_x0 = save[0]; cl_y0 = save[1]; cl_x1 = save[2]; cl_y1 = save[3];
    if (focused && !st->no_ring) {                                                        /* кольцо фокуса */
        float rad[4]; float bw = st->border_w[0], px = st->padding[3].px, py = st->padding[0].px;
        int fx0 = (int)(x - bw - px - 1), fy0 = (int)(y - bw - py - 1), fx1 = (int)(x + w + bw + st->padding[1].px + 1), fy1 = (int)(y + h + bw + st->padding[2].px + 1);
        resolve_radii(st, (float)(fx1 - fx0), (float)(fy1 - fy0), rad);
        NodeStyle ring; memset(&ring, 0, sizeof ring); for (int i = 0; i < 4; i++) { ring.border_w[i] = 2; ring.border_c[i] = 0xFF1A73E8u; ring.border_style[i] = BS_SOLID; }
        draw_border(&ring, fx0, fy0, fx1, fy1, rad, 1);
    }
}

/* ══════════════ основной проход ══════════════ */
/* ── «Кадр» состояния обхода display list ──
   Один и тот же автомат используется для рисования, hit-test и прокрутки колесом, чтобы они видели страницу одинаково:
   отсечение (overflow), прокрутка внутри элементов (overflow:auto|scroll) и position:sticky меняют смещение dx/dy
   и область отсечения между командами C_CLIP/C_UNCLIP и C_STK/C_ENDSTK. Для обычных и fixed-команд кадры раздельные. */
#define FR_MAX 64
typedef struct {
    int cx0, cy0, cx1, cy1; float dx, dy, ry0, ry1; unsigned char kind;      /* сохранённое состояние до входа */
    float rx0, rx1, im[6]; unsigned char has_im; int ox0, oy0, ox1, oy1;       /* sticky по X; обратная матрица transform для hit-test; внешнее отсечение (экранные координаты) */
    float bx0, by0, bx1, by1, sw, sh, ox, oy; unsigned char fl;               /* для скроллбара: рамка контейнера (экран), размеры, смещения, флаги */
    const DOMNode* node;
} FrSave;
typedef struct {
    int pass; int cx0, cy0, cx1, cy1; float dx, dy, ry0, ry1;
    float rx0, rx1, im[6]; unsigned char has_im; int ox0, oy0, ox1, oy1;
    FrSave st[FR_MAX]; int sp;
    int popped; FrSave pop;                                                    /* только что закрытый прокручиваемый контейнер */
    const DOMNode* scroller; unsigned char scroller_fl; float s_ox, s_oy, s_mx, s_my;   /* только что открытый прокручиваемый контейнер (для колеса) */
} Frame;

static void fr_init(Frame* f, int pass, int x0, int y0, int x1, int y1) {
    memset(f, 0, sizeof *f); f->pass = pass; f->cx0 = x0; f->cy0 = y0; f->cx1 = x1; f->cy1 = y1;
    f->dx = pass ? 0.0f : (float)-g_sx; f->dy = pass ? (float)g_top : (float)(g_top - g_sy); f->ry0 = (float)y0; f->ry1 = (float)y1; f->rx0 = (float)x0; f->rx1 = (float)x1;
}
static void fr_sync(const Frame* f) { cl_x0 = f->cx0; cl_y0 = f->cy0; cl_x1 = f->cx1; cl_y1 = f->cy1; }
static void fr_push(Frame* f, unsigned char kind, FrSave** out) {
    FrSave* s = f->sp < FR_MAX ? &f->st[f->sp] : NULL;
    if (s) { memset(s, 0, sizeof *s); s->cx0 = f->cx0; s->cy0 = f->cy0; s->cx1 = f->cx1; s->cy1 = f->cy1; s->dx = f->dx; s->dy = f->dy; s->ry0 = f->ry0; s->ry1 = f->ry1; s->rx0 = f->rx0; s->rx1 = f->rx1; memcpy(s->im, f->im, sizeof s->im); s->has_im = f->has_im; s->ox0 = f->ox0; s->oy0 = f->oy0; s->ox1 = f->ox1; s->oy1 = f->oy1; s->kind = kind; }
    f->sp++; *out = s;
}
static int fr_is_ctl(const Cmd* c) { return c->t == C_CLIP || c->t == C_UNCLIP || c->t == C_STK || c->t == C_ENDSTK || c->t == C_FX || c->t == C_ENDFX; }
static void fr_restore(Frame* f, const FrSave* p) {
    f->cx0 = p->cx0; f->cy0 = p->cy0; f->cx1 = p->cx1; f->cy1 = p->cy1; f->dx = p->dx; f->dy = p->dy; f->ry0 = p->ry0; f->ry1 = p->ry1; f->rx0 = p->rx0; f->rx1 = p->rx1;
    memcpy(f->im, p->im, sizeof f->im); f->has_im = p->has_im; f->ox0 = p->ox0; f->oy0 = p->oy0; f->ox1 = p->ox1; f->oy1 = p->oy1;
}
/* точка экрана → локальные координаты внутри повёрнутого/масштабированного элемента */
static void fr_map(const Frame* f, int mx, int my, float* px, float* py) {
    if (!f->has_im) { *px = (float)mx; *py = (float)my; return; }
    *px = f->im[0] * (mx + 0.5f) + f->im[2] * (my + 0.5f) + f->im[4]; *py = f->im[1] * (mx + 0.5f) + f->im[3] * (my + 0.5f) + f->im[5];
}
static int fr_in_clip(const Frame* f, int mx, int my) {
    float px, py; fr_map(f, mx, my, &px, &py);
    if (f->has_im && (mx < f->ox0 || mx >= f->ox1 || my < f->oy0 || my >= f->oy1)) return 0;
    return px >= f->cx0 && px < f->cx1 && py >= f->cy0 && py < f->cy1;
}
/* применяет команду-«скобку» к кадру; для команд чужого прохода (fixed/не fixed) ничего не делает */
static void fr_ctl(Frame* f, const Cmd* c) {
    f->popped = 0; f->scroller = NULL;
    if (c->fixed != f->pass) return;
    FrSave* s = NULL;
    switch (c->t) {
    case C_CLIP: {
        int x0 = (int)floorf(c->x + f->dx + 0.5f), y0 = (int)floorf(c->y + f->dy + 0.5f), x1 = (int)floorf(c->x + c->w + f->dx + 0.5f), y1 = (int)floorf(c->y + c->h + f->dy + 0.5f);
        fr_push(f, 0, &s);
        if (s) { s->bx0 = (float)x0; s->by0 = (float)y0; s->bx1 = (float)x1; s->by1 = (float)y1; }
        if (x0 > f->cx0) f->cx0 = x0; if (y0 > f->cy0) f->cy0 = y0; if (x1 < f->cx1) f->cx1 = x1; if (y1 < f->cy1) f->cy1 = y1;
        if (f->cx1 < f->cx0) f->cx1 = f->cx0; if (f->cy1 < f->cy0) f->cy1 = f->cy0;
        if ((c->flags & (CF_SCROLL_X | CF_SCROLL_Y)) && c->node) {
            float ox = 0, oy = 0; l2_scroll_get(c->node, &ox, &oy);
            if (!(c->flags & CF_SCROLL_X)) ox = 0; if (!(c->flags & CF_SCROLL_Y)) oy = 0;
            if (s) { s->kind = 1; s->node = c->node; s->fl = c->flags; s->sw = c->ex[0]; s->sh = c->ex[1]; s->ox = ox; s->oy = oy; }
            f->scroller = c->node; f->scroller_fl = c->flags; f->s_ox = ox; f->s_oy = oy; f->s_mx = c->ex[2]; f->s_my = c->ex[3];
            f->dx -= ox; f->dy -= oy; f->ry0 = (float)y0; f->ry1 = (float)y1; f->rx0 = (float)x0; f->rx1 = (float)x1;
        }
        break; }
    case C_UNCLIP: {
        if (f->sp > 0 && f->sp <= FR_MAX && (f->st[f->sp - 1].kind == 2 || f->st[f->sp - 1].kind == 3)) return;      /* закрытие не того типа — игнорируем */
        if (f->sp > 0) { f->sp--; if (f->sp < FR_MAX) { FrSave* p = &f->st[f->sp]; if (p->kind == 1) { f->popped = 1; f->pop = *p; } fr_restore(f, p); } }
        break; }
    case C_STK: {
        fr_push(f, 2, &s);
        float ys = c->y + f->dy, shift = 0;
        if ((c->flags & SF_TOP) && f->ry0 + c->ex[0] > ys) {
            shift = f->ry0 + c->ex[0] - ys; float room = (c->ex[2] + f->dy) - c->h - ys; if (shift > room) shift = room; if (shift < 0) shift = 0;
        }
        if ((c->flags & SF_BOTTOM) && shift == 0 && f->ry1 - c->ex[3] - c->h < ys) {
            shift = f->ry1 - c->ex[3] - c->h - ys; float room = (c->ex[1] + f->dy) - ys; if (shift < room) shift = room; if (shift > 0) shift = 0;
        }
        float xs = c->x + f->dx, shx = 0;
        if ((c->flags & SF_LEFT) && f->rx0 + c->ex[4] > xs) { shx = f->rx0 + c->ex[4] - xs; float room = (c->ex[7] + f->dx) - c->w - xs; if (shx > room) shx = room; if (shx < 0) shx = 0; }
        if ((c->flags & SF_RIGHT) && shx == 0 && f->rx1 - c->ex[5] - c->w < xs) { shx = f->rx1 - c->ex[5] - c->w - xs; float room = (c->ex[6] + f->dx) - xs; if (shx < room) shx = room; if (shx > 0) shx = 0; }
        f->dy += shift; f->dx += shx; break; }
    case C_ENDSTK: {
        if (f->sp > 0 && (f->sp > FR_MAX || f->st[f->sp - 1].kind == 2)) { f->sp--; if (f->sp < FR_MAX) { FrSave* p = &f->st[f->sp]; f->dy = p->dy; f->dx = p->dx; } }
        break; }
    case C_FX: {
        if (!c->node) break; fr_push(f, 3, &s);
        const NodeStyle* st = &c->node->st; if (!st->xf.n) break;
        float tx = 0, ty = 0, m[6]; int lin = fx_xform_compute(&st->xf, c->x + f->dx, c->y + f->dy, c->w, c->h, m, &tx, &ty);
        if (!lin) { f->dx += tx; f->dy += ty; break; }
        float det = m[0] * m[3] - m[1] * m[2]; if (fabsf(det) < 1e-6f) break;
        float inv[6] = {m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, 0, 0}; inv[4] = -(inv[0] * m[4] + inv[2] * m[5]); inv[5] = -(inv[1] * m[4] + inv[3] * m[5]);
        if (f->has_im) { float o[6] = {inv[0] * f->im[0] + inv[2] * f->im[1], inv[1] * f->im[0] + inv[3] * f->im[1], inv[0] * f->im[2] + inv[2] * f->im[3], inv[1] * f->im[2] + inv[3] * f->im[3], 0, 0};
            o[4] = inv[0] * f->im[4] + inv[2] * f->im[5] + inv[4]; o[5] = inv[1] * f->im[4] + inv[3] * f->im[5] + inv[5]; memcpy(inv, o, sizeof inv); }
        else { f->ox0 = f->cx0; f->oy0 = f->cy0; f->ox1 = f->cx1; f->oy1 = f->cy1; }
        memcpy(f->im, inv, sizeof f->im); f->has_im = 1; f->cx0 = f->cy0 = -1000000; f->cx1 = f->cy1 = 1000000; break; }
    case C_ENDFX: {
        if (f->sp > 0 && (f->sp > FR_MAX || f->st[f->sp - 1].kind == 3)) { f->sp--; if (f->sp < FR_MAX) fr_restore(f, &f->st[f->sp]); }
        break; }
    default: break;
    }
}

/* полосы прокрутки (оверлейные, без места в раскладке) поверх закрытого прокручиваемого контейнера */
static void draw_elem_scrollbars(const FrSave* p) {
    float w = p->bx1 - p->bx0, h = p->by1 - p->by0; if (w < 20 || h < 20) return;
    float r4[4] = {3, 3, 3, 3};
    if ((p->fl & CF_SCROLL_Y) && p->sh > h + 0.5f) {
        float maxy = p->sh - h, th = h * h / p->sh; if (th < 24) th = 24; if (th > h) th = h;
        float ty = p->by0 + (h - th) * (maxy > 0 ? p->oy / maxy : 0);
        fill_rrect((int)(p->bx1 - 9), (int)ty, (int)(p->bx1 - 3), (int)(ty + th), r4, 0xFF8B8F98u, 0.75f);
    }
    if ((p->fl & CF_SCROLL_X) && p->sw > w + 0.5f) {
        float maxx = p->sw - w, tw = w * w / p->sw; if (tw < 24) tw = 24; if (tw > w) tw = w;
        float tx = p->bx0 + (w - tw) * (maxx > 0 ? p->ox / maxx : 0);
        fill_rrect((int)tx, (int)(p->by1 - 9), (int)(tx + tw), (int)(p->by1 - 3), r4, 0xFF8B8F98u, 0.75f);
    }
}

static void fx_group(Frame* f, int i, int j, int pass);
static void paint_range(const Frame* fp, int from, int to, int fixed_pass) {
    Frame f = *fp; fr_sync(&f);
    for (int i = from; i < to; i++) {
        const Cmd* c = &l2_dl[i];
        if (c->t == C_FX) { if (c->fixed == fixed_pass && c->mate > i && c->mate < to) { fx_group(&f, i, c->mate, fixed_pass); i = c->mate; fr_sync(&f); } continue; }
        if (c->t == C_ENDFX) continue;
        if (fr_is_ctl(c)) { fr_ctl(&f, c); fr_sync(&f); if (f.popped) draw_elem_scrollbars(&f.pop); continue; }
        if (c->fixed != fixed_pass) continue;
        int dx = (int)floorf(f.dx + 0.5f), dy = (int)floorf(f.dy + 0.5f);
        /* быстрый отказ: вне экрана */
        float sy0 = c->y + dy, sy1 = c->y + c->h + dy, sx0 = c->x + dx, sx1 = c->x + c->w + dx;
        if (c->t != C_HIT && (sy1 < cl_y0 - 40 || sy0 > cl_y1 + 40 || sx1 < cl_x0 - 40 || sx0 > cl_x1 + 40)) { if (c->t != C_BOX || (sy1 < cl_y0 - 400 || sy0 > cl_y1 + 400)) continue; }
        g_blend_mode = (c->node && c->node->st.blend_mode) ? c->node->st.blend_mode : 0;
        switch (c->t) {
        case C_BOX: {
            const NodeStyle* st = &c->node->st; int x0 = (int)floorf(c->x + dx + 0.5f), y0 = (int)floorf(c->y + dy + 0.5f), x1 = (int)floorf(c->x + c->w + dx + 0.5f), y1 = (int)floorf(c->y + c->h + dy + 0.5f);
            if (x1 <= x0 || y1 <= y0) break;
            float rad[4]; resolve_radii(st, (float)(x1 - x0), (float)(y1 - y0), rad);
            if (c->flags & (F_NOLEFT | F_NORIGHT)) { if (c->flags & F_NOLEFT) rad[0] = rad[3] = 0; if (c->flags & F_NORIGHT) rad[1] = rad[2] = 0; }
            draw_shadow(st, x0, y0, x1, y1, rad, st->opacity);
            if (!(c->flags & 0x80) && !st->bg_clip_text) {                     /* clip:text — сплошной фон не рисуем, см. paint_text */
                if (st->bg_grad_type) fill_rrect_grad(x0, y0, x1, y1, rad, st, st->opacity);
                else fill_rrect(x0, y0, x1, y1, rad, st->bg, st->opacity);
            }
            NodeStyle bs = *st; if (c->flags & F_NOLEFT) bs.border_w[3] = 0; if (c->flags & F_NORIGHT) bs.border_w[1] = 0;
            draw_border(&bs, x0, y0, x1, y1, rad, st->opacity);
            if (st->outline_set && st->outline_w > 0 && st->outline_style != BS_NONE) {
                int ow = (int)ceilf(st->outline_w); int off = (int)floorf(st->outline_offset + 0.5f);
                int ox0 = x0 - off - ow, oy0 = y0 - off - ow, ox1 = x1 + off + ow, oy1 = y1 + off + ow;
                uint32_t oc = st->outline_c ? st->outline_c : st->color;
                fill_rect_i(ox0, oy0, ox1, oy0 + ow, oc, st->opacity); fill_rect_i(ox0, oy1 - ow, ox1, oy1, oc, st->opacity);
                fill_rect_i(ox0, oy0 + ow, ox0 + ow, oy1 - ow, oc, st->opacity); fill_rect_i(ox1 - ow, oy0 + ow, ox1, oy1 - ow, oc, st->opacity);
            }
            break; }
        case C_RECT: {
            int x0 = (int)floorf(c->x + dx + 0.5f), y0 = (int)floorf(c->y + dy + 0.5f); int s = (int)(c->w + 0.5f); float rad[4] = {c->rad, c->rad, c->rad, c->rad};
            if (c->flags & 1) { fill_rrect(x0, y0, x0 + s, y0 + s, rad, c->color | 0xFF000000u, c->opacity); float in[4] = {c->rad - 1, c->rad - 1, c->rad - 1, c->rad - 1}; if (s > 2) fill_rrect(x0 + 1, y0 + 1, x0 + s - 1, y0 + s - 1, in, l2_canvas_bg, 1); }
            else fill_rrect(x0, y0, x0 + s, y0 + s, rad, c->color | 0xFF000000u, c->opacity);
            break; }
        case C_TEXT: paint_text(c, dx, dy); break;
        case C_IMG: {
            int x0 = (int)floorf(c->x + dx + 0.5f), y0 = (int)floorf(c->y + dy + 0.5f), w = (int)floorf(c->w + 0.5f), h = (int)floorf(c->h + 0.5f); if (w < 1 || h < 1) break;
            webpage_image_set_color(!strcmp(c->node->tag, "svg") ? c->node->st.color : 0xFF000000u);        /* currentColor встроенного svg = CSS color элемента */
            const NodeStyle* is = &c->node->st; ImgPix* src = webpage_image_get(c->s); if (!src) src = webpage_image_scaled(c->s, w, h);
            if (!src) break;
            float ar = src->h > 0 ? (float)src->w / (float)src->h : (float)w / (float)h;
            int dw = w, dh = h; int fit = is->object_fit;
            if (fit == 1 || fit == 2 || fit == 4) {
                float scale = fit == 2 ? fmaxf((float)w / src->w, (float)h / src->h) : fminf((float)w / src->w, (float)h / src->h);
                if (fit == 4) scale = fminf(1.0f, scale);
                if (fit == 2) scale = fmaxf((float)w / src->w, (float)h / src->h);
                dw = (int)fmaxf(1, floorf(src->w * scale + 0.5f)); dh = (int)fmaxf(1, floorf(src->h * scale + 0.5f));
            } else if (fit == 3) { dw = src->w; dh = src->h; }
            else { dw = w; dh = h; }
            (void)ar;
            int ix = x0 + (int)floorf((w - dw) * is->object_pos_x / 100.0f + 0.5f);
            int iy = y0 + (int)floorf((h - dh) * is->object_pos_y / 100.0f + 0.5f);
            ImgPix* im = webpage_image_scaled(c->s, dw, dh); if (!im) im = src;
            float rad[4]; resolve_radii(is, (float)w, (float)h, rad);
            if (fit == 2 || dw > w || dh > h || ix < x0 || iy < y0) {
                int sx0=cl_x0, sy0=cl_y0, sx1=cl_x1, sy1=cl_y1;
                if (cl_x0 < x0) cl_x0=x0; if (cl_y0 < y0) cl_y0=y0; if (cl_x1 > x0+w) cl_x1=x0+w; if (cl_y1 > y0+h) cl_y1=y0+h;
                float zero[4]={0,0,0,0}; blit_img(im, ix, iy, dw, dh, zero, c->opacity);
                cl_x0=sx0; cl_y0=sy0; cl_x1=sx1; cl_y1=sy1;
                /* rounded border is handled by the element's overflow clip when available */
            } else blit_img(im, ix, iy, dw, dh, rad, c->opacity);
            break; }
        case C_BGIMG: paint_bgimg(c, dx, dy); break;
        case C_CTRL: paint_ctrl(c, dx, dy); break;
        default: break;
        }
        g_blend_mode = 0;
    }
}


/* ══════════════ слои эффектов (C_FX … C_ENDFX) ══════════════ */
static uint32_t* g_lay[3][2]; static size_t g_laycap[3]; static int g_fxdepth;

static void range_bbox(const Frame* g, int i, int j, int pass, float* x0, float* y0, float* x1, float* y1) {
    int dx = (int)floorf(g->dx + 0.5f), dy = (int)floorf(g->dy + 0.5f);
    for (int k = i + 1; k < j; k++) {
        const Cmd* c = &l2_dl[k]; if (c->fixed != pass) continue;
        if (c->t == C_FX && c->node && c->node->st.xf.n) {                       /* вложенный transform расширяет границы слоя предка */
            float tx = 0, ty = 0, m[6], bx = c->x + dx, by = c->y + dy; int lin = fx_xform_compute(&c->node->st.xf, bx, by, c->w, c->h, m, &tx, &ty);
            float px[4] = {bx, bx + c->w, bx, bx + c->w}, py[4] = {by, by, by + c->h, by + c->h};
            for (int q = 0; q < 4; q++) { float X = lin ? m[0] * px[q] + m[2] * py[q] + m[4] : px[q] + tx, Y = lin ? m[1] * px[q] + m[3] * py[q] + m[5] : py[q] + ty;
                if (X < *x0) *x0 = X; if (X > *x1) *x1 = X; if (Y < *y0) *y0 = Y; if (Y > *y1) *y1 = Y; }
            continue;
        }
        if (c->t != C_BOX && c->t != C_RECT && c->t != C_TEXT && c->t != C_IMG && c->t != C_BGIMG && c->t != C_CTRL) continue;
        float ex = 0, ey0 = 0, ey1 = 0;
        if (c->t == C_BOX && c->node && (c->node->st.sh_color >> 24)) { const NodeStyle* s = &c->node->st; float e = s->sh_blur + fabsf(s->sh_spread) + 2; ex = fabsf(s->sh_x) + e; ey0 = fabsf(s->sh_y) + e; ey1 = ey0; }
        float ax = c->x + dx - ex, ay = c->y + dy - ey0, bx = c->x + c->w + dx + ex, by = c->y + c->h + dy + ey1;
        if (ax < *x0) *x0 = ax; if (ay < *y0) *y0 = ay; if (bx > *x1) *x1 = bx; if (by > *y1) *y1 = by;
    }
}
static int lay_get(int d, size_t need) {
    if (g_laycap[d] < need) { for (int k = 0; k < 2; k++) { free(g_lay[d][k]); g_lay[d][k] = malloc(need * 4); if (!g_lay[d][k]) { g_laycap[d] = 0; return 0; } } g_laycap[d] = need; }
    return 1;
}
static void fx_backdrop(const Cmd* c, const NodeStyle* st, float bx, float by) {
    int pad = fx_filter_pad(&st->backdrop); int x0 = (int)floorf(bx) - pad, y0 = (int)floorf(by) - pad, x1 = (int)ceilf(bx + c->w) + pad, y1 = (int)ceilf(by + c->h) + pad;
    int vx0 = (int)floorf(bx), vy0 = (int)floorf(by), vx1 = (int)ceilf(bx + c->w), vy1 = (int)ceilf(by + c->h);
    if (vx0 < cl_x0) vx0 = cl_x0; if (vy0 < cl_y0) vy0 = cl_y0; if (vx1 > cl_x1) vx1 = cl_x1; if (vy1 > cl_y1) vy1 = cl_y1; if (vx1 <= vx0 || vy1 <= vy0) return;
    if (x0 > vx0 - pad) x0 = vx0 - pad; if (y0 > vy0 - pad) y0 = vy0 - pad; if (x1 < vx1 + pad) x1 = vx1 + pad; if (y1 < vy1 + pad) y1 = vy1 + pad;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH; if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT; if (x1 <= x0 || y1 <= y0) return;
    FxImg* im = fx_img_new(x0, y0, x1 - x0, y1 - y0); if (!im) return;
    fx_img_from_buffer(im, back_buffer, SCREEN_WIDTH); fx_apply_filters(im, &st->backdrop);
    float rad[4]; resolve_radii(st, c->w, c->h, rad);
    fx_writeback(im, back_buffer, SCREEN_WIDTH, cl_x0, cl_y0, cl_x1, cl_y1, bx, by, c->w, c->h, rad);
    fx_img_free(im);
}
static void fx_group(Frame* f, int i, int j, int pass) {
    const Cmd* c = &l2_dl[i]; const NodeStyle* st = &c->node->st;
    int dx = (int)floorf(f->dx + 0.5f), dy = (int)floorf(f->dy + 0.5f);
    float bx = c->x + dx, by = c->y + dy;
    if (st->backdrop.n && c->w > 0 && c->h > 0 && (f->has_im == 0)) fx_backdrop(c, st, bx, by);
    float tx = 0, ty = 0, m[6]; int lin = st->xf.n ? fx_xform_compute(&st->xf, bx, by, c->w, c->h, m, &tx, &ty) : 0;
    int need = st->filter.n || st->clip.kind || st->mask.kind || lin;
    Frame g = *f;
    if (!lin) { g.dx += tx; g.dy += ty; bx += tx; by += ty; }
    if (!need || g_fxdepth >= 3) { paint_range(&g, i + 1, j, pass); fr_sync(f); return; }
    float x0 = bx, y0 = by, x1 = bx + c->w, y1 = by + c->h; range_bbox(&g, i, j, pass, &x0, &y0, &x1, &y1);
    int pad = fx_filter_pad(&st->filter);
    float rx0 = x0 - pad, ry0 = y0 - pad, rx1 = x1 + pad, ry1 = y1 + pad;
    if (!lin) { if (rx0 < cl_x0 - pad) rx0 = (float)(cl_x0 - pad); if (ry0 < cl_y0 - pad) ry0 = (float)(cl_y0 - pad); if (rx1 > cl_x1 + pad) rx1 = (float)(cl_x1 + pad); if (ry1 > cl_y1 + pad) ry1 = (float)(cl_y1 + pad); }
    int ix0 = (int)floorf(rx0), iy0 = (int)floorf(ry0), ix1 = (int)ceilf(rx1), iy1 = (int)ceilf(ry1);
    if (ix0 < 0) ix0 = 0; if (iy0 < 0) iy0 = 0; if (ix1 > SCREEN_WIDTH) ix1 = SCREEN_WIDTH; if (iy1 > SCREEN_HEIGHT) iy1 = SCREEN_HEIGHT;
    if (ix1 <= ix0 || iy1 <= iy0) { fr_sync(f); return; }
    int d = g_fxdepth; if (!lay_get(d, (size_t)SCREEN_WIDTH * (size_t)SCREEN_HEIGHT)) { paint_range(&g, i + 1, j, pass); fr_sync(f); return; }
    uint32_t* save_bb = back_buffer; int sv[4] = {cl_x0, cl_y0, cl_x1, cl_y1};
    g_fxdepth++;
    for (int k = 0; k < 2; k++) {
        uint32_t* L = g_lay[d][k]; uint32_t bg = k ? 0xFFFFFFu : 0x000000u;
        for (int y = iy0; y < iy1; y++) { uint32_t* row = L + (size_t)y * SCREEN_WIDTH; for (int x = ix0; x < ix1; x++) row[x] = bg; }
        back_buffer = L; Frame h = g; h.cx0 = ix0; h.cy0 = iy0; h.cx1 = ix1; h.cy1 = iy1; paint_range(&h, i + 1, j, pass);
    }
    g_fxdepth--; back_buffer = save_bb; cl_x0 = sv[0]; cl_y0 = sv[1]; cl_x1 = sv[2]; cl_y1 = sv[3];
    FxImg* im = fx_img_new(ix0, iy0, ix1 - ix0, iy1 - iy0);
    if (im) {
        fx_img_from_passes(im, g_lay[d][0], g_lay[d][1], SCREEN_WIDTH);
        if (st->filter.n) fx_apply_filters(im, &st->filter);
        if (st->clip.kind) fx_apply_clip(im, &st->clip, bx, by, c->w, c->h);
        if (st->mask.kind) fx_apply_mask(im, &st->mask, bx, by, c->w, c->h);
        fx_composite(im, back_buffer, SCREEN_WIDTH, cl_x0, cl_y0, cl_x1, cl_y1, lin ? m : NULL);
        fx_img_free(im);
    }
    fr_sync(f);
}

static void paint_pass(int fixed_pass, int base_x0, int base_y0, int base_x1, int base_y1) {
    Frame f; fr_init(&f, fixed_pass, base_x0, base_y0, base_x1, base_y1); paint_range(&f, 0, l2_dn, fixed_pass);
}

static int g_fingerprint_nodes;
static uint64_t fingerprint(DOMNode* head) {
    uint64_t h = 1469598103934665603ULL; int n = 0;
    for (DOMNode* p = head; p; p = p->next, n++) { h = (h ^ (uint64_t)(p->type + 1)) * 1099511628211ULL; h = (h ^ (unsigned char)p->text[0]) * 1099511628211ULL; h = (h ^ (unsigned char)p->text[1]) * 1099511628211ULL; h = (h ^ (uint64_t)(p->st.font_size * 10)) * 1099511628211ULL; }
    g_fingerprint_nodes = n;
    return h ^ (uint64_t)n ^ (uint64_t)(uintptr_t)head;
}

typedef struct {
    int checked, enabled;
    double window_start;
    unsigned frames;
    double frame_total, fingerprint_total, layout_total, paint_total;
    double frame_max;
} RenderProfile;
static RenderProfile g_render_profile;

static int render_profile_enabled(void) {
    if (!g_render_profile.checked) {
        const char* value = getenv("MLB_PROFILE_RENDER");
        g_render_profile.enabled = value && value[0] == '1';
        g_render_profile.checked = 1;
        if (g_render_profile.enabled) {
            g_render_profile.window_start = view_transition_now_ms();
            gal_log("INFO", "[perf] render profiling enabled; reports once per second");
        }
    }
    return g_render_profile.enabled;
}

static void render_profile_frame(double frame_ms, double fingerprint_ms,
                                 double layout_ms, double paint_ms) {
    RenderProfile* p = &g_render_profile;
    p->frames++;
    p->frame_total += frame_ms;
    p->fingerprint_total += fingerprint_ms;
    p->layout_total += layout_ms;
    p->paint_total += paint_ms;
    if (frame_ms > p->frame_max) p->frame_max = frame_ms;
    double now = view_transition_now_ms();
    double elapsed = now - p->window_start;
    if (elapsed < 1000.0) return;
    double denom = p->frames ? (double)p->frames : 1.0;
    gal_log("INFO", "[perf] render %.1f fps | frame avg %.2f ms max %.2f ms | fingerprint %.2f ms | layout %.2f ms | paint %.2f ms | DOM %d | draw-cmds %d | %dx%d",
            (double)p->frames * 1000.0 / elapsed, p->frame_total / denom, p->frame_max,
            p->fingerprint_total / denom, p->layout_total / denom, p->paint_total / denom,
            g_fingerprint_nodes, l2_dn, SCREEN_WIDTH, SCREEN_HEIGHT);
    p->window_start = now;
    p->frames = 0;
    p->frame_total = p->fingerprint_total = p->layout_total = p->paint_total = 0.0;
    p->frame_max = 0.0;
}

static void webpage_paint_loaded(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    if (!head) return;
    g_top = top_offset; g_sx = scroll_x; g_sy = scroll_y;
    snprintf(g_url, sizeof g_url, "%s", page_url ? page_url : "");
    g_last_head = head; g_head = head; g_fp = fingerprint(head);
    g_lw = SCREEN_WIDTH - 10; if (g_lw < 100) g_lw = 100;
    g_lh = SCREEN_HEIGHT - top_offset;
    if (getenv("WP_DEBUG")) gal_log("INFO", "[wp] doc %.0fx%.0f top=%d scroll=%d", l2_doc_w, l2_doc_h, top_offset, scroll_y);
    layout_content_width = (int)l2_doc_w - 10; layout_content_height = (int)l2_doc_h - 20;
    cl_x0 = 0; cl_y0 = top_offset; cl_x1 = SCREEN_WIDTH; cl_y1 = SCREEN_HEIGHT;
    fill_rect_i(0, top_offset, SCREEN_WIDTH, SCREEN_HEIGHT, l2_canvas_bg | 0xFF000000u, 1);
    paint_pass(0, 0, top_offset, SCREEN_WIDTH, SCREEN_HEIGHT);
    paint_pass(1, 0, top_offset, SCREEN_WIDTH, SCREEN_HEIGHT);
    webpage_view_transition_composite(top_offset);
}
static void webpage_layout_only(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    if (!head) return;
    g_top = top_offset; g_sx = scroll_x; g_sy = scroll_y;
    l2_set_page_scroll((float)scroll_x,(float)scroll_y);
    snprintf(g_url, sizeof g_url, "%s", page_url ? page_url : "");
    int vw = SCREEN_WIDTH - 10, vh = SCREEN_HEIGHT - top_offset; if (vw < 100) vw = 100;
    l2_layout(head, vw, vh);
    g_last_head = head; g_head = head; g_fp = fingerprint(head); g_lw = vw; g_lh = vh;
    layout_content_width = (int)l2_doc_w - 10; layout_content_height = (int)l2_doc_h - 20;
}
static int g_cv_scroll_known; static int g_cv_scroll_x, g_cv_scroll_y;
static void webpage_render_inline(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    if (!head) return;
    int profile = render_profile_enabled();
    double frame_started = profile ? view_transition_now_ms() : 0.0;
    l2_set_page_scroll((float)scroll_x,(float)scroll_y);
    int page_scroll_changed=!g_cv_scroll_known || g_cv_scroll_x!=scroll_x || g_cv_scroll_y!=scroll_y;
    if(page_scroll_changed) {
        for(DOMNode* q=head;q;q=q->next) if(q->st.content_visibility==1) { g_head=NULL; break; }
    }
    g_top = top_offset; g_sx = scroll_x; g_sy = scroll_y; snprintf(g_url, sizeof g_url, "%s", page_url ? page_url : "");
    int vw = SCREEN_WIDTH - 10, vh = SCREEN_HEIGHT - top_offset; if (vw < 100) vw = 100;
    if (anim_step(head) & 1) g_head = NULL;
    double fingerprint_started = profile ? view_transition_now_ms() : 0.0;
    uint64_t fp = fingerprint(head); g_last_head = head;
    double fingerprint_ms = profile ? view_transition_now_ms() - fingerprint_started : 0.0;
    double layout_ms = 0.0;
    if (head != g_head || fp != g_fp || vw != g_lw || vh != g_lh) {
        double layout_started = profile ? view_transition_now_ms() : 0.0;
        l2_layout(head, vw, vh); g_head = head; g_fp = fp; g_lw = vw; g_lh = vh;
        if (profile) layout_ms = view_transition_now_ms() - layout_started;
    }
    g_cv_scroll_known=1; g_cv_scroll_x=scroll_x; g_cv_scroll_y=scroll_y;
    double paint_started = profile ? view_transition_now_ms() : 0.0;
    webpage_paint_loaded(head, top_offset, page_url, scroll_x, scroll_y);
    if (profile) {
        double paint_ms = view_transition_now_ms() - paint_started;
        render_profile_frame(view_transition_now_ms() - frame_started,
                             fingerprint_ms, layout_ms, paint_ms);
    }
}

#ifndef _WIN32
/* Layout+paint worker: it gets a forked snapshot of the DOM, renders into a
 * shared pixel mapping, and sends the hit-test display list back over a pipe.
 * The UI process never runs layout/paint on this path. */
#define RENDER_IPC_MAGIC 0x52504e31u /* RPN1 */
typedef struct {
    uint32_t magic, status, cmd_count, rect_count, scroll_count;
    int32_t content_width, content_height;
    uint32_t canvas_bg;
    float doc_w, doc_h;
    int32_t worker_pid, layout_pid;
} RenderIPCHeader;
typedef struct {
    Cmd cmd;                 /* node/s/fam pointers are nulled before transmission */
    int32_t node_idx;
    uint32_t slen, flen;     /* include terminating NUL */
} RenderCmdWire;
typedef struct {
    Cmd* dl; int dn;
    float* rects; int nrects;
    L2ScrollState* scroll; int nscroll;
    char** strings; int nstrings, string_cap;
    int32_t* node_ids;
    RenderIPCHeader h;
} RenderIPCData;
static char** g_remote_strings;
static int g_remote_string_count;
static pid_t g_layout_child_pid;
static int g_worker_output_fd = -1;
static int g_render_isolation_logged;
static int g_render_fallback_logged;

static int render_ipc_write_all(int fd, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    while (len) { ssize_t n = write(fd, p, len); if (n < 0) { if (errno == EINTR) continue; return 0; } if (!n) return 0; p += n; len -= (size_t)n; }
    return 1;
}
static int render_ipc_read_all(int fd, void* data, size_t len) {
    unsigned char* p = (unsigned char*)data;
    while (len) { ssize_t n = read(fd, p, len); if (n < 0) { if (errno == EINTR) continue; return 0; } if (!n) return 0; p += n; len -= (size_t)n; }
    return 1;
}
static int render_child_send(int fd, DOMNode* head) {
    RenderIPCHeader h; memset(&h, 0, sizeof h);
    h.magic = RENDER_IPC_MAGIC; h.status = 1;
    h.worker_pid = (int32_t)getpid(); h.layout_pid = (int32_t)g_layout_child_pid;
    if (l2_dn < 0 || l2_dn > 2000000 || l2_nrects < 0 || l2_nrects > 1000000) h.status = 0;
    h.cmd_count = h.status ? (uint32_t)l2_dn : 0;
    h.rect_count = h.status ? (uint32_t)l2_nrects : 0;
    int sc = l2_scroll_state_export(head, NULL, 0);
    if (sc < 0 || sc > 1000000) h.status = 0; else h.scroll_count = h.status ? (uint32_t)sc : 0;
    h.content_width = layout_content_width; h.content_height = layout_content_height;
    h.canvas_bg = l2_canvas_bg; h.doc_w = l2_doc_w; h.doc_h = l2_doc_h;
    if (!render_ipc_write_all(fd, &h, sizeof h) || !h.status) return 0;
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i]; RenderCmdWire w; memset(&w, 0, sizeof w);
        memcpy(&w.cmd, c, sizeof(Cmd)); w.cmd.node = NULL; w.cmd.s = NULL; w.cmd.fam = NULL;
        w.node_idx = c->node ? c->node->idx : -1;
        size_t sl = c->s ? strlen(c->s) + 1 : 0, fl = c->fam ? strlen(c->fam) + 1 : 0;
        if (sl > 16u*1024u*1024u || fl > 1024u*1024u) return 0;
        w.slen = (uint32_t)sl; w.flen = (uint32_t)fl;
        if (!render_ipc_write_all(fd, &w, sizeof w)) return 0;
        if (sl && !render_ipc_write_all(fd, c->s, sl)) return 0;
        if (fl && !render_ipc_write_all(fd, c->fam, fl)) return 0;
    }
    if (h.rect_count && (!l2_rects || !render_ipc_write_all(fd, l2_rects, (size_t)h.rect_count * 4 * sizeof(float)))) return 0;
    if (h.scroll_count) {
        size_t bytes = (size_t)h.scroll_count * sizeof(L2ScrollState);
        L2ScrollState* state = malloc(bytes); if (!state) return 0;
        int n = l2_scroll_state_export(head, state, (int)h.scroll_count);
        int ok = n == (int)h.scroll_count && render_ipc_write_all(fd, state, bytes);
        free(state); if (!ok) return 0;
    }
    return 1;
}
static void render_ipc_data_clear(RenderIPCData* d) {
    if (!d) return;
    for (int i = 0; i < d->nstrings; i++) free(d->strings[i]);
    free(d->strings); free(d->dl); free(d->rects); free(d->scroll); free(d->node_ids); memset(d, 0, sizeof *d);
}
static int render_ipc_add_string(RenderIPCData* d, const char* s) {
    if (!s) return 1;
    if (d->nstrings >= d->string_cap) {
        int cap = d->string_cap ? d->string_cap * 2 : 256;
        if (cap < d->string_cap || cap > 4000000) return 0;
        char** v = realloc(d->strings, (size_t)cap * sizeof(*v)); if (!v) return 0;
        d->strings = v; d->string_cap = cap;
    }
    d->strings[d->nstrings++] = (char*)s; return 1;
}
static int render_parent_read(int fd, RenderIPCData* d) {
    memset(d, 0, sizeof *d);
    if (!render_ipc_read_all(fd, &d->h, sizeof d->h) || d->h.magic != RENDER_IPC_MAGIC || !d->h.status ||
        d->h.cmd_count > 2000000 || d->h.rect_count > 1000000 || d->h.scroll_count > 1000000) return 0;
    d->dn = (int)d->h.cmd_count; d->nrects = (int)d->h.rect_count; d->nscroll = (int)d->h.scroll_count;
    if (d->dn) {
        d->dl = calloc((size_t)d->dn, sizeof(*d->dl));
        d->node_ids = malloc((size_t)d->dn * sizeof(*d->node_ids));
        if (!d->dl || !d->node_ids) return 0;
    }
    if (d->nrects) { d->rects = malloc((size_t)d->nrects * 4 * sizeof(float)); if (!d->rects) return 0; }
    if (d->nscroll) { d->scroll = malloc((size_t)d->nscroll * sizeof(*d->scroll)); if (!d->scroll) return 0; }
    for (int i = 0; i < d->dn; i++) {
        RenderCmdWire w;
        if (!render_ipc_read_all(fd, &w, sizeof w) || w.slen > 16u*1024u*1024u || w.flen > 1024u*1024u) return 0;
        Cmd* c = &d->dl[i]; memcpy(c, &w.cmd, sizeof *c); c->node = NULL; c->s = NULL; c->fam = NULL;
        /* Keep strings separately allocated: Cmds reference them for hit-testing and copy selection. */
        if (w.slen) {
            char* str = malloc(w.slen); if (!str) return 0;
            if (!render_ipc_read_all(fd, str, w.slen)) { free(str); return 0; }
            str[w.slen - 1] = 0; if (!render_ipc_add_string(d, str)) { free(str); return 0; } c->s = str;
        }
        if (w.flen) {
            char* str = malloc(w.flen); if (!str) return 0;
            if (!render_ipc_read_all(fd, str, w.flen)) { free(str); return 0; }
            str[w.flen - 1] = 0; if (!render_ipc_add_string(d, str)) { free(str); return 0; } c->fam = str;
        }
        c->mate = (c->mate < -1 || c->mate >= d->dn) ? -1 : c->mate;
        d->node_ids[i] = w.node_idx;
    }
    if (d->nrects && !render_ipc_read_all(fd, d->rects, (size_t)d->nrects * 4 * sizeof(float))) return 0;
    if (d->nscroll && !render_ipc_read_all(fd, d->scroll, (size_t)d->nscroll * sizeof(*d->scroll))) return 0;
    return 1;
}
static int render_ipc_install(DOMNode* head, RenderIPCData* d) {
    int max_idx = -1;
    for (DOMNode* n = head; n; n = n->next) if (n->idx > max_idx) max_idx = n->idx;
    int map_count = max_idx + 1;
    DOMNode** map = calloc((size_t)(map_count > 0 ? map_count : 1), sizeof(*map));
    if (!map) return 0;
    for (DOMNode* n = head; n; n = n->next) if (n->idx >= 0 && n->idx < map_count) map[n->idx] = n;
    for (int i = 0; i < d->dn; i++) {
        int ni = d->node_ids ? d->node_ids[i] : -1;
        if (ni >= 0 && ni < map_count) d->dl[i].node = map[ni]; else d->dl[i].node = NULL;
    }
    free(map);
    for (int i = 0; i < g_remote_string_count; i++) free(g_remote_strings[i]);
    free(g_remote_strings); g_remote_strings = d->strings; g_remote_string_count = d->nstrings; d->strings = NULL; d->nstrings = d->string_cap = 0;
    l2_release();
    l2_dl = d->dl; l2_dn = d->dn; d->dl = NULL; d->dn = 0;
    l2_rects = d->rects; l2_nrects = d->nrects; d->rects = NULL; d->nrects = 0;
    l2_canvas_bg = d->h.canvas_bg; l2_doc_w = d->h.doc_w; l2_doc_h = d->h.doc_h;
    layout_content_width = d->h.content_width; layout_content_height = d->h.content_height;
    if (d->nscroll > 0) l2_scroll_state_import(d->scroll, d->nscroll, head);
    return 1;
}
static int webpage_layout_subprocess(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    int fds[2] = {-1, -1};
    g_layout_child_pid = 0;
    if (pipe(fds) != 0) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (g_worker_output_fd >= 0 && g_worker_output_fd != fds[1]) close(g_worker_output_fd);
        g_layout_child_pid = 0;
        webpage_layout_only(head, top_offset, page_url, scroll_x, scroll_y);
        int ok = render_child_send(fds[1], head); close(fds[1]); _exit(ok ? 0 : 127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return 0; }
    RenderIPCData data; int ok = render_parent_read(fds[0], &data);
    close(fds[0]); int st = 0; pid_t wr; do { wr = waitpid(pid, &st, 0); } while (wr < 0 && errno == EINTR);
    if (ok && WIFEXITED(st) && WEXITSTATUS(st) == 0) ok = render_ipc_install(head, &data);
    else ok = 0;
    render_ipc_data_clear(&data);
    if (ok) g_layout_child_pid = pid;
    return ok;
}
static void webpage_render_worker(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    g_layout_child_pid = 0;
    if (!webpage_layout_subprocess(head, top_offset, page_url, scroll_x, scroll_y)) {
        webpage_render_inline(head, top_offset, page_url, scroll_x, scroll_y);
        return;
    }
    webpage_paint_loaded(head, top_offset, page_url, scroll_x, scroll_y);
}
void webpage_render(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    if (g_vt_snapshot && head != g_vt_head) webpage_view_transition_cancel();
    l2_set_page_scroll((float)scroll_x,(float)scroll_y);
    if (g_sel_scroll_head != head) { g_sel_scroll_head = head; g_sel_scroll_seen = 0; }
    if (g_text_sel_active && g_sel_scroll_seen && g_sel_scroll_head == head && g_last_head == head) {
        float dx = (float)(g_sel_scroll_x - scroll_x), dy = (float)(g_sel_scroll_y - scroll_y);
        g_text_sel_anchor.visual_x += dx; g_text_sel_focus.visual_x += dx;
        g_text_sel_anchor.visual_y += dy; g_text_sel_focus.visual_y += dy;
    }
    g_sel_scroll_x = scroll_x; g_sel_scroll_y = scroll_y; g_sel_scroll_seen = 1;
    if (g_last_head != head) webpage_text_selection_clear();
    else if (g_text_sel_active) (void)text_selection_endpoints_live(head);
    if (!head) return;
    /* Forking layout and paint workers for every frame required a full
       framebuffer copy, display-list serialization, two process startups,
       and a synchronous wait. Keep this expensive isolation path opt-in. */
    const char* isolated = getenv("MLB_RENDER_ISOLATED");
    if (!(isolated && isolated[0] == '1') ||
        SCREEN_WIDTH <= 0 || SCREEN_HEIGHT <= 0 || !back_buffer) {
        webpage_render_inline(head, top_offset, page_url, scroll_x, scroll_y); return;
    }
    size_t pixels = (size_t)SCREEN_WIDTH * (size_t)SCREEN_HEIGHT;
    if (pixels > (size_t)128 * 1024 * 1024 / sizeof(uint32_t)) { webpage_render_inline(head, top_offset, page_url, scroll_x, scroll_y); return; }
    size_t bytes = pixels * sizeof(uint32_t);
    uint32_t* shared = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) { webpage_render_inline(head, top_offset, page_url, scroll_x, scroll_y); return; }
    memcpy(shared, back_buffer, bytes);
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) { munmap(shared, bytes); webpage_render_inline(head, top_offset, page_url, scroll_x, scroll_y); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]); back_buffer = shared; g_worker_output_fd = fds[1];
        webpage_render_worker(head, top_offset, page_url, scroll_x, scroll_y);
        int ok = render_child_send(fds[1], head); close(fds[1]); _exit(ok ? 0 : 127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]); munmap(shared, bytes); webpage_render_inline(head, top_offset, page_url, scroll_x, scroll_y); return;
    }
    RenderIPCData data; int ok = render_parent_read(fds[0], &data);
    close(fds[0]); int st = 0; pid_t wr; do { wr = waitpid(pid, &st, 0); } while (wr < 0 && errno == EINTR);
    if (ok && WIFEXITED(st) && WEXITSTATUS(st) == 0) ok = render_ipc_install(head, &data);
    else ok = 0;
    if (ok) {
        memcpy(back_buffer, shared, bytes);
        g_top = top_offset; g_sx = scroll_x; g_sy = scroll_y;
        snprintf(g_url, sizeof g_url, "%s", page_url ? page_url : "");
        g_last_head = head; g_head = head; g_fp = fingerprint(head);
        g_lw = SCREEN_WIDTH - 10; if (g_lw < 100) g_lw = 100; g_lh = SCREEN_HEIGHT - top_offset;
        if (data.h.layout_pid > 0 && !g_render_isolation_logged) {
            gal_log("INFO", "[render] layout process pid=%d; paint process pid=%d", data.h.layout_pid, data.h.worker_pid);
            g_render_isolation_logged = 1;
        } else if (data.h.layout_pid <= 0 && !g_render_fallback_logged) {
            gal_log("WARN", "[render] separate layout process failed; renderer used its inline fallback");
            g_render_fallback_logged = 1;
        }
    } else {
        if (!g_render_fallback_logged) { gal_log("WARN", "[render] child render IPC failed; using inline fallback for this frame"); g_render_fallback_logged = 1; }
        render_ipc_data_clear(&data);
        webpage_render_inline(head, top_offset, page_url, scroll_x, scroll_y);
    }
    render_ipc_data_clear(&data); munmap(shared, bytes);
}
#else
void webpage_render(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    webpage_render_inline(head, top_offset, page_url, scroll_x, scroll_y);
}
#endif

/* ══════════════ взаимодействие ══════════════ */
DOMNode* webpage_hit_test(DOMNode* head, int mx, int my) {
    (void)head; if (!l2_dl || my < g_top) return NULL;
    Frame fr[2]; fr_init(&fr[0], 0, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT); fr_init(&fr[1], 1, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    DOMNode* best[2] = {NULL, NULL};
    for (int i = 0; i < l2_dn; i++) {                       /* проход вперёд: побеждает команда, нарисованная позже (выше); fixed — выше обычных */
        const Cmd* c = &l2_dl[i]; int k = c->fixed ? 1 : 0; Frame* f = &fr[k];
        if (fr_is_ctl(c)) { fr_ctl(f, c); continue; }
        if (c->t != C_HIT) continue;
        if (c->node && c->node->st.pointer_events == 1) continue;
        if (!fr_in_clip(f, mx, my)) continue;     /* скрыто отсечением overflow */
        float x0 = c->x + f->dx, y0 = c->y + f->dy, hx, hy; fr_map(f, mx, my, &hx, &hy);
        if (hx >= x0 && hx < x0 + c->w && hy >= y0 && hy < y0 + c->h) best[k] = c->node;
    }
    return best[1] ? best[1] : best[0];
}

static int text_position_valid(const WebTextPosition* p) {
    if (!p || !p->node || p->node->type != TAG_TEXT || p->node->idx < 0) return 0;
    size_t len = strlen(p->node->text);
    if ((size_t)p->offset > len) return 0;
    /* A DOM mutation may leave an old endpoint inside a now-different UTF-8
       sequence. Reject continuation-byte offsets instead of copying broken UTF-8. */
    if ((size_t)p->offset < len &&
        (((unsigned char)p->node->text[p->offset] & 0xC0u) == 0x80u)) return 0;
    return 1;
}

void webpage_text_selection_clear(void) {
    memset(&g_text_sel_anchor, 0, sizeof g_text_sel_anchor);
    memset(&g_text_sel_focus, 0, sizeof g_text_sel_focus);
    g_text_sel_active = 0;
    g_text_sel_preferred_x = NAN;
}

int webpage_text_selection_active(void) { return g_text_sel_active; }

/* DOM mutation may remove selected text nodes while keeping the same root node.
 * Validate pointer membership by walking the live flat node list before reading
 * either endpoint; otherwise a later paint/copy could dereference a stale node. */
static int text_selection_endpoints_live(DOMNode* head) {
    if (!g_text_sel_active) return 1;
    int have_anchor = 0, have_focus = 0;
    for (DOMNode* n = head; n; n = n->next) {
        if (n == g_text_sel_anchor.node) have_anchor = 1;
        if (n == g_text_sel_focus.node) have_focus = 1;
        if (have_anchor && have_focus) return 1;
    }
    webpage_text_selection_clear();
    return 0;
}

void webpage_text_selection_set(const WebTextPosition* anchor, const WebTextPosition* focus) {
    if (!text_position_valid(anchor) || !text_position_valid(focus)) { webpage_text_selection_clear(); return; }
    g_text_sel_anchor = *anchor; g_text_sel_focus = *focus; g_text_sel_active = 1;
    g_text_sel_preferred_x = NAN;
}

int webpage_text_position_at(DOMNode* head, int mx, int my, int allow_nearest, WebTextPosition* out) {
    if (!head || !out || !l2_dl || head != g_last_head || my < g_top) return 0;
    Frame fr[2];
    fr_init(&fr[0], 0, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    fr_init(&fr[1], 1, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    const Cmd* best = NULL; float best_local_x = 0.0f;
    float best_x0 = 0.0f, best_y0 = 0.0f;
    float best_score = INFINITY;
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i];
        if (fr_is_ctl(c)) { fr_ctl(&fr[0], c); fr_ctl(&fr[1], c); continue; }
        if (c->t != C_TEXT || !c->node || c->node->type != TAG_TEXT || c->node->idx < 0 ||
            !c->text_offset_valid || !c->s || c->node->st.user_select == 1 ||
            c->node->st.pointer_events == 1 || !isfinite(c->x) || !isfinite(c->y) ||
            !isfinite(c->w) || !isfinite(c->h) || c->w < 0 || c->h <= 0) continue;
        Frame* f = &fr[c->fixed ? 1 : 0];
        if (!fr_in_clip(f, mx, my)) continue;
        float hx, hy; fr_map(f, mx, my, &hx, &hy);
        float x0 = c->x + f->dx, y0 = c->y + f->dy;
        float vx = hx < x0 ? x0 - hx : hx > x0 + c->w ? hx - (x0 + c->w) : 0.0f;
        float vy = hy < y0 ? y0 - hy : hy > y0 + c->h ? hy - (y0 + c->h) : 0.0f;
        if (!allow_nearest) { if (vx > 14.0f || vy > 10.0f) continue; }
        else if (vy > fmaxf(c->h * 2.0f, 40.0f)) continue;
        float score = vy * 4096.0f + vx;
        if (score < best_score) { best_score = score; best = c; best_local_x = hx - x0; best_x0 = x0; best_y0 = y0; }
    }
    if (!best) return 0;
    TTFFont* font = font_manager_get_styled(best->fam, best->weight ? best->weight : 400, best->ital);
    if (!font) return 0;
    size_t n = strlen(best->s), local = 0;
    if (n > UINT32_MAX - best->text_offset ||
        (size_t)best->text_offset + n > strlen(best->node->text)) return 0;
    if (!ttf_text_hit_test_ex_bidi(font, best->s, best->size, best->tstyle, best_local_x,
                                   text_base_direction(&best->node->st), text_bidi_override(&best->node->st), &local) || local > n) return 0;
    uint32_t absolute = best->text_offset + (uint32_t)local;
    if (absolute > strlen(best->node->text)) return 0;
    out->node = best->node; out->offset = absolute;
    out->visual_x = best_x0 + best_local_x;
    out->visual_y = best_y0 + best->h * 0.5f;
    /* Snap the stored visual point to the exact legal caret if the metrics API is available. */
    if (n <= (size_t)INT_MAX - 1) {
        int* caret_offsets = malloc((n + 1) * sizeof(*caret_offsets));
        if (caret_offsets) {
            int cw = ttf_text_offsets_ex_bidi(font, best->s, best->size, best->tstyle,
                caret_offsets, (int)n + 1, text_base_direction(&best->node->st),
                text_bidi_override(&best->node->st));
            if (cw >= 0 && local <= n)
                out->visual_x = best_x0 + caret_offsets[local];
            free(caret_offsets);
        }
    }
    return 1;
}

typedef struct {
    WebTextPosition pos;
    float x, y, top, bottom, height;
    int row;
} WebCaretStop;

static size_t caret_utf8_next(const unsigned char* s, size_t n, size_t i) {
    if (i >= n) return n;
    unsigned char c = s[i];
    size_t step = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 :
                  (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 1;
    if (step > n - i) return i + 1;
    for (size_t k = 1; k < step; ++k) if ((s[i + k] & 0xC0u) != 0x80u) return i + 1;
    if (step == 3 && c == 0xE0 && s[i + 1] < 0xA0) return i + 1;
    if (step == 3 && c == 0xED && s[i + 1] >= 0xA0) return i + 1;
    if (step == 4 && c == 0xF0 && s[i + 1] < 0x90) return i + 1;
    if (step == 4 && c == 0xF4 && s[i + 1] >= 0x90) return i + 1;
    return i + step;
}

static int caret_stop_append(WebCaretStop** stops, size_t* count, size_t* cap,
                             const WebCaretStop* stop) {
    if (!stops || !count || !cap || !stop || *count >= 250000) return 0;
    /* A display-list boundary can be emitted by two adjacent runs. Collapse only
       identical node/offset/visual positions; wrapped copies remain distinct. */
    if (*count) {
        const WebCaretStop* last = &(*stops)[*count - 1];
        if (last->pos.node == stop->pos.node && last->pos.offset == stop->pos.offset &&
            fabsf(last->x - stop->x) < 0.5f && fabsf(last->y - stop->y) < 0.5f) return 1;
    }
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        if (nc > 250000) nc = 250000;
        if (nc <= *cap || nc > SIZE_MAX / sizeof(**stops)) return 0;
        WebCaretStop* v = realloc(*stops, nc * sizeof(*v));
        if (!v) return 0;
        *stops = v; *cap = nc;
    }
    (*stops)[(*count)++] = *stop;
    return 1;
}

static int caret_stop_ycmp(const void* ap, const void* bp) {
    const WebCaretStop* a = ap; const WebCaretStop* b = bp;
    if (a->y < b->y) return -1; if (a->y > b->y) return 1;
    if (a->x < b->x) return -1; if (a->x > b->x) return 1;
    if (a->pos.node->idx != b->pos.node->idx) return a->pos.node->idx < b->pos.node->idx ? -1 : 1;
    return a->pos.offset < b->pos.offset ? -1 : a->pos.offset > b->pos.offset;
}
static int caret_stop_row_xcmp(const void* ap, const void* bp) {
    const WebCaretStop* a = ap; const WebCaretStop* b = bp;
    if (a->row != b->row) return a->row < b->row ? -1 : 1;
    if (a->x < b->x) return -1; if (a->x > b->x) return 1;
    if (a->y < b->y) return -1; if (a->y > b->y) return 1;
    if (a->pos.node->idx != b->pos.node->idx) return a->pos.node->idx < b->pos.node->idx ? -1 : 1;
    return a->pos.offset < b->pos.offset ? -1 : a->pos.offset > b->pos.offset;
}

static int collect_web_caret_stops(DOMNode* head, WebCaretStop** out, size_t* out_count) {
    if (!head || !out || !out_count || !l2_dl || head != g_last_head) return 0;
    *out = NULL; *out_count = 0;
    size_t cap = 0;
    Frame fr[2];
    fr_init(&fr[0], 0, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    fr_init(&fr[1], 1, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    for (int ci = 0; ci < l2_dn; ++ci) {
        const Cmd* c = &l2_dl[ci];
        if (fr_is_ctl(c)) { fr_ctl(&fr[0], c); fr_ctl(&fr[1], c); continue; }
        if (c->t != C_TEXT || !c->node || c->node->type != TAG_TEXT || c->node->idx < 0 ||
            !c->text_offset_valid || !c->s || c->node->st.user_select == 1 ||
            c->node->st.pointer_events == 1 || !isfinite(c->x) || !isfinite(c->y) ||
            !isfinite(c->w) || !isfinite(c->h) || c->w < 0 || c->h <= 0) continue;
        size_t n = strlen(c->s), node_len = strlen(c->node->text);
        if (!n || n > (size_t)INT_MAX - 1 || c->text_offset > node_len || n > node_len - c->text_offset ||
            memcmp(c->node->text + c->text_offset, c->s, n) != 0) continue;
        TTFFont* font = font_manager_get_styled(c->fam, c->weight ? c->weight : 400, c->ital);
        if (!font) continue;
        int* offsets = malloc((n + 1) * sizeof(*offsets));
        if (!offsets) { free(*out); *out = NULL; *out_count = 0; return 0; }
        if (ttf_text_offsets_ex_bidi(font, c->s, c->size, c->tstyle, offsets, (int)n + 1,
                                     text_base_direction(&c->node->st), text_bidi_override(&c->node->st)) < 0) {
            free(offsets); continue;
        }
        Frame* f = &fr[c->fixed ? 1 : 0];
        float x0 = c->x + f->dx, y0 = c->y + f->dy;
        WebCaretStop stop; memset(&stop, 0, sizeof stop);
        stop.pos.node = c->node; stop.pos.offset = c->text_offset;
        stop.top = y0; stop.bottom = y0 + c->h; stop.height = c->h;
        stop.y = y0 + c->h * 0.5f;
        stop.x = x0 + offsets[0]; stop.pos.visual_x = stop.x; stop.pos.visual_y = stop.y;
        if (!caret_stop_append(out, out_count, &cap, &stop)) {
            free(offsets); free(*out); *out = NULL; *out_count = 0; return 0;
        }
        size_t i = 0;
        while (i < n) {
            size_t next = caret_utf8_next((const unsigned char*)c->s, n, i);
            if (next <= i || next > n) break;
            /* HarfBuzz assigns the same x to every byte in one cluster. A changed
               caret x marks the next legal cluster boundary; this skips UTF-8
               continuation bytes, combining marks, and ZWJ interiors. */
            if (offsets[next] != offsets[i]) {
                memset(&stop, 0, sizeof stop);
                stop.pos.node = c->node;
                stop.pos.offset = c->text_offset + (uint32_t)next;
                stop.top = y0; stop.bottom = y0 + c->h; stop.height = c->h;
                stop.y = y0 + c->h * 0.5f; stop.x = x0 + offsets[next];
                stop.pos.visual_x = stop.x; stop.pos.visual_y = stop.y;
                if (!caret_stop_append(out, out_count, &cap, &stop)) {
                    free(offsets); free(*out); *out = NULL; *out_count = 0; return 0;
                }
            }
            i = next;
        }
        free(offsets);
    }
    if (!*out_count) { free(*out); *out = NULL; return 0; }
    /* Group text runs on the same visual line, allowing mixed font sizes without
       merging clearly separate adjacent lines. */
    qsort(*out, *out_count, sizeof(**out), caret_stop_ycmp);
    int row = -1; float row_center = 0.0f, row_height = 0.0f;
    for (size_t i = 0; i < *out_count; ++i) {
        WebCaretStop* s = &(*out)[i];
        int same = row >= 0 && fabsf(s->y - row_center) <= 0.44f * fmaxf(s->height, row_height);
        if (!same) { row++; row_center = s->y; row_height = s->height; }
        else {
            float top = fminf(row_center - row_height * 0.5f, s->top);
            float bottom = fmaxf(row_center + row_height * 0.5f, s->bottom);
            row_center = (top + bottom) * 0.5f; row_height = bottom - top;
        }
        s->row = row;
    }
    qsort(*out, *out_count, sizeof(**out), caret_stop_row_xcmp);
    return 1;
}

static int caret_stop_for_position(const WebCaretStop* stops, size_t count,
                                   const WebTextPosition* pos) {
    if (!stops || !pos || !pos->node) return -1;
    int best = -1; float best_dist = INFINITY;
    int coords_valid = isfinite(pos->visual_x) && isfinite(pos->visual_y) &&
                       (pos->visual_x != 0.0f || pos->visual_y != 0.0f);
    for (size_t i = 0; i < count; ++i) {
        if (stops[i].pos.node != pos->node || stops[i].pos.offset != pos->offset) continue;
        float dist = coords_valid ? fabsf(stops[i].x - pos->visual_x) + 2.0f * fabsf(stops[i].y - pos->visual_y) : 0.0f;
        if (best < 0 || dist < best_dist) { best = (int)i; best_dist = dist; }
    }
    return best;
}

typedef struct {
    WebTextPosition pos;
    int row;
    int paragraph_id;
} WebLogicalCaret;

static int logical_caret_cmp(const void* ap, const void* bp) {
    const WebLogicalCaret* a = ap; const WebLogicalCaret* b = bp;
    int c = text_pos_compare(&a->pos, &b->pos);
    if (c) return c;
    if (a->row != b->row) return a->row < b->row ? -1 : 1;
    if (a->pos.visual_x < b->pos.visual_x) return -1;
    if (a->pos.visual_x > b->pos.visual_x) return 1;
    return 0;
}

static int caret_block_container(const DOMNode* n) {
    if (!n || n->type == TAG_TEXT) return 0;
    switch (n->type) {
        case TAG_H: case TAG_P: case TAG_DIV: case TAG_HTML: case TAG_BODY:
        case TAG_MAIN: case TAG_SECTION: case TAG_ARTICLE: case TAG_HEADER:
        case TAG_FOOTER: case TAG_NAV: case TAG_ASIDE: case TAG_UL: case TAG_OL:
        case TAG_LI: case TAG_TABLE: case TAG_TR: case TAG_TD: case TAG_TH:
        case TAG_FORM: case TAG_FIELDSET:
            return 1;
        default: break;
    }
    const char* t = n->tag;
    return !strcmp(t, "blockquote") || !strcmp(t, "pre") || !strcmp(t, "address") ||
           !strcmp(t, "figure") || !strcmp(t, "figcaption") || !strcmp(t, "h1") ||
           !strcmp(t, "h2") || !strcmp(t, "h3") || !strcmp(t, "h4") ||
           !strcmp(t, "h5") || !strcmp(t, "h6") || !strcmp(t, "body") || !strcmp(t, "html");
}

static int caret_paragraph_id(DOMNode** by_idx, size_t node_count, DOMNode* text) {
    if (!by_idx || !text) return -1;
    int parent = text->parent;
    size_t guard = 0;
    while (parent >= 0 && (size_t)parent < node_count && guard++ < node_count) {
        DOMNode* p = by_idx[parent];
        if (!p) break;
        if (caret_block_container(p)) return p->idx;
        parent = p->parent;
    }
    return text->parent;
}

static int collect_logical_carets(DOMNode* head, const WebCaretStop* stops, size_t count,
                                  WebLogicalCaret** out, size_t* out_count) {
    if (!head || !stops || !count || !out || !out_count) return 0;
    *out = NULL; *out_count = 0;
    int max_idx = -1;
    for (DOMNode* n = head; n; n = n->next) if (n->idx > max_idx) max_idx = n->idx;
    if (max_idx < 0 || max_idx > 1000000 || (size_t)max_idx + 1 > SIZE_MAX / sizeof(DOMNode*)) return 0;
    size_t node_count = (size_t)max_idx + 1;
    DOMNode** by_idx = calloc(node_count, sizeof(*by_idx));
    WebLogicalCaret* logical = malloc(count * sizeof(*logical));
    if (!by_idx || !logical) { free(by_idx); free(logical); return 0; }
    for (DOMNode* n = head; n; n = n->next) if (n->idx >= 0 && (size_t)n->idx < node_count) by_idx[n->idx] = n;
    for (size_t i = 0; i < count; ++i) {
        logical[i].pos = stops[i].pos;
        logical[i].row = stops[i].row;
        logical[i].paragraph_id = caret_paragraph_id(by_idx, node_count, stops[i].pos.node);
    }
    free(by_idx);
    qsort(logical, count, sizeof(*logical), logical_caret_cmp);
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (n && text_pos_compare(&logical[n-1].pos, &logical[i].pos) == 0) {
            /* Keep the topmost display occurrence for wrapped duplicates. The
               active position is restored to its precise visual occurrence below. */
            continue;
        }
        if (n != i) logical[n] = logical[i];
        ++n;
    }
    *out = logical; *out_count = n;
    return n > 0;
}

static int logical_index_for_position(const WebLogicalCaret* logical, size_t count,
                                      const WebTextPosition* pos) {
    if (!logical || !pos) return -1;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = text_pos_compare(&logical[mid].pos, pos);
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return lo < count && text_pos_compare(&logical[lo].pos, pos) == 0 ? (int)lo : -1;
}

int webpage_text_selection_move_visual(DOMNode* head, int direction, int extend) {
    if (!head || !g_text_sel_active || !text_selection_endpoints_live(head) || direction == 0) return 0;
    WebCaretStop* stops = NULL; size_t count = 0;
    if (!collect_web_caret_stops(head, &stops, &count)) return 0;
    int ai = caret_stop_for_position(stops, count, &g_text_sel_anchor);
    int fi = caret_stop_for_position(stops, count, &g_text_sel_focus);
    if (ai < 0 || fi < 0) { free(stops); return 0; }
    int collapsed = text_pos_compare(&g_text_sel_anchor, &g_text_sel_focus) == 0;
    int word_move = direction == -5 || direction == 5;
    int paragraph_move = direction == -6 || direction == 6;
    int horizontal = direction == -1 || direction == 1 || word_move;
    int handled_direction = (direction >= -6 && direction <= 6 && direction != 0);
    if (!handled_direction) { free(stops); return 0; }
    int target = fi;
    int collapse_only = !extend && !collapsed;
    if (collapse_only) {
        if (direction == -1 || direction == 1) {
            target = direction < 0 ? (ai < fi ? ai : fi) : (ai > fi ? ai : fi);
        } else if (direction == -2 || direction == 2) {
            int ay = stops[ai].row, fy = stops[fi].row;
            target = direction < 0 ? (ay < fy ? ai : fi) : (ay > fy ? ai : fi);
        } else if (word_move || paragraph_move || direction == -4 || direction == 4) {
            int cmp = text_pos_compare(&g_text_sel_anchor, &g_text_sel_focus);
            target = direction < 0 ? (cmp <= 0 ? ai : fi) : (cmp >= 0 ? ai : fi);
        } else if (direction == -3 || direction == 3) {
            int row = stops[fi].row;
            for (size_t i = 0; i < count; ++i) if (stops[i].row == row) {
                if (direction < 0) { target = (int)i; break; }
                target = (int)i;
            }
        }
    } else if (direction == -4 || direction == 4) {
        /* Ctrl+Home/End follow source/DOM order, not visual x order. */
        target = fi;
        for (size_t i = 0; i < count; ++i) {
            int cmp = text_pos_compare(&stops[i].pos, &stops[target].pos);
            if ((direction < 0 && (cmp < 0 || (cmp == 0 && stops[i].y < stops[target].y))) ||
                (direction > 0 && (cmp > 0 || (cmp == 0 && stops[i].y > stops[target].y)))) target = (int)i;
        }
    } else if (direction == -3 || direction == 3) {
        int row = stops[fi].row; target = fi;
        for (size_t i = 0; i < count; ++i) if (stops[i].row == row) {
            if (direction < 0) { target = (int)i; break; }
            target = (int)i;
        }
    } else if (direction == -5 || direction == 5 || direction == -6 || direction == 6) {
        WebLogicalCaret* logical = NULL; size_t logical_count = 0;
        if (collect_logical_carets(head, stops, count, &logical, &logical_count)) {
            int li = logical_index_for_position(logical, logical_count, &stops[fi].pos);
            if (li >= 0) {
                /* Restore the exact wrapped occurrence for the active caret. */
                logical[li].pos.visual_x = stops[fi].x;
                logical[li].pos.visual_y = stops[fi].y;
                TTFLogicalCaretPosition* nav = malloc(logical_count * sizeof(*nav));
                if (nav) {
                    for (size_t i = 0; i < logical_count; ++i) {
                        nav[i].utf8 = logical[i].pos.node->text;
                        nav[i].byte_offset = logical[i].pos.offset;
                        nav[i].paragraph_id = logical[i].paragraph_id;
                    }
                    size_t wanted = (size_t)li;
                    int ok = word_move ? ttf_logical_caret_move_word(nav, logical_count, (size_t)li,
                                            direction < 0 ? -1 : 1, &wanted) :
                        ttf_logical_caret_move_paragraph(nav, logical_count, (size_t)li,
                                            direction < 0 ? -1 : 1, &wanted);
                    if (ok && wanted < logical_count) {
                        int ti = caret_stop_for_position(stops, count, &logical[wanted].pos);
                        if (ti >= 0) target = ti;
                    }
                    free(nav);
                }
            }
            free(logical);
        }
    } else if (direction == -1 || direction == 1 || direction == -2 || direction == 2) {
        TTFVisualCaretStop* visual = malloc(count * sizeof(*visual));
        if (visual) {
            for (size_t i = 0; i < count; ++i) {
                visual[i].x = stops[i].x; visual[i].y = stops[i].y;
                visual[i].height = stops[i].height; visual[i].row = stops[i].row;
            }
            float preferred = isfinite(g_text_sel_preferred_x) ? g_text_sel_preferred_x : stops[fi].x;
            size_t next = (size_t)fi;
            if (ttf_visual_caret_step(visual, count, (size_t)fi, direction, preferred, &next)) target = (int)next;
            free(visual);
            if (direction == -2 || direction == 2) g_text_sel_preferred_x = preferred;
        }
    }
    if (extend) g_text_sel_focus = stops[target].pos;
    else g_text_sel_anchor = g_text_sel_focus = stops[target].pos;
    if (horizontal || direction == -3 || direction == 3 || direction == -4 || direction == 4 || paragraph_move)
        g_text_sel_preferred_x = stops[target].x;
    g_text_sel_active = 1;
    free(stops);
    return target != fi || collapse_only;
}

static int webpage_copy_logical_selection(DOMNode* head, char* out, int cap) {
    WebTextPosition first, last;
    if (!out || cap < 1 || !text_selection_endpoints_live(head) ||
        !text_selection_ordered(&first, &last) || text_pos_compare(&first, &last) == 0) return 0;
    int max_idx = -1;
    for (DOMNode* n = head; n; n = n->next) if (n->idx > max_idx) max_idx = n->idx;
    if (max_idx < 0 || (size_t)max_idx >= SIZE_MAX) return 0;
    unsigned char* rendered = calloc((size_t)max_idx + 1, 1);
    if (!rendered) return 0;
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i];
        if (c->t == C_TEXT && c->node && c->node->type == TAG_TEXT && c->node->idx >= 0 && c->node->idx <= max_idx)
            rendered[c->node->idx] = 1;
    }
    size_t o = 0;
    for (DOMNode* n = head; n && o < (size_t)cap - 1; n = n->next) {
        if (n->type != TAG_TEXT || n->st.user_select == 1 || n->idx < first.node->idx || n->idx > last.node->idx ||
            n->idx < 0 || n->idx > max_idx || !rendered[n->idx]) continue;
        size_t len = strlen(n->text), begin = 0, end = len;
        if (n->idx == first.node->idx) begin = first.offset;
        if (n->idx == last.node->idx) end = last.offset;
        if (begin > len) begin = len; if (end > len) end = len;
        if (end <= begin) continue;
        size_t take = end - begin; if (take > (size_t)cap - 1 - o) take = (size_t)cap - 1 - o;
        memcpy(out + o, n->text + begin, take); o += take;
    }
    out[o] = 0; free(rendered); return (int)o;
}

int webpage_copy_selection(char* out, int cap) {
    if (!out || cap < 2) return 0;
    if (g_text_sel_active) return webpage_copy_logical_selection(g_last_head, out, cap);
    int sx, sy, sw, sh; if (!sel_rect(&sx, &sy, &sw, &sh)) return 0;
    int o = 0; float last_y = -1e9f; int any = 0;
    Frame fr; fr_init(&fr, 0, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i]; if (fr_is_ctl(c)) { fr_ctl(&fr, c); continue; }
        if (c->t != C_TEXT || c->fixed || (c->node && c->node->st.user_select == 1)) continue;
        int x = (int)(c->x + fr.dx + 0.5f), y = (int)(c->y + fr.dy + 0.5f);
        if (y + (int)c->h <= fr.cy0 || y >= fr.cy1 || x + (int)c->w <= fr.cx0 || x >= fr.cx1) continue;      /* скрытое отсечением */
        if (!rect_hit(x, y, (int)c->w + 1, (int)c->h, sx, sy, sw, sh)) continue;
        TTFFont* f = font_manager_get_styled(c->fam, c->weight ? c->weight : 400, c->ital); if (!f) continue;
        size_t n = strlen(c->s); int* offs = malloc((n + 1) * sizeof(int)); if (!offs) continue;
        const NodeStyle* text_st = c->node ? &c->node->st : NULL;
        int text_dir = text_base_direction(text_st);
        int text_override = text_bidi_override(text_st);
        ttf_text_offsets_ex_bidi(f, c->s, c->size, c->tstyle, offs, (int)n + 1, text_dir, text_override);
        int started = 0;
        for (size_t k = 0; k < n;) {
            size_t l = 1; unsigned char ch = (unsigned char)c->s[k]; if (ch >= 0xF0) l = 4; else if (ch >= 0xE0) l = 3; else if (ch >= 0xC0) l = 2; if (k + l > n) l = n - k;
            int oa = offs[k], ob = offs[k + l];
            int ox = oa < ob ? oa : ob, ow = abs(ob - oa);
            if (ow > 0 && rect_hit(x + ox, y, ow, (int)c->h, sx, sy, sw, sh)) {
                if (!started && any && o < cap - 2) out[o++] = (c->y - last_y > c->h * 0.5f) ? '\n' : ' ';
                started = 1; any = 1; for (size_t q = 0; q < l && o < cap - 1; q++) out[o++] = c->s[k + q];
            }
            k += l;
        }
        if (started) last_y = c->y;
        free(offs);
    }
    out[o] = 0; return o;
}

/* Form submissions are binary-safe so file inputs can produce real multipart bodies. */
typedef struct { unsigned char* p; size_t n, cap; } FormBuf;
#define FORM_MAX_BODY ((size_t)64 << 20)
#define FORM_MAX_FILE ((size_t)32 << 20)
static int form_buf_add(FormBuf* b, const void* p, size_t n) {
    if (!b || (!p && n) || n > FORM_MAX_BODY || b->n > FORM_MAX_BODY - n) return 0;
    size_t need = b->n + n;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < need) { if (cap > FORM_MAX_BODY / 2) { cap = FORM_MAX_BODY; break; } cap *= 2; }
        unsigned char* q = realloc(b->p, cap ? cap : 1); if (!q) return 0; b->p = q; b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, p, n); b->n += n; return 1;
}
static int form_buf_s(FormBuf* b, const char* s) { return form_buf_add(b, s ? s : "", s ? strlen(s) : 0); }
static int form_buf_c(FormBuf* b, char c) { return form_buf_add(b, &c, 1); }
static const char* form_basename(const char* path) {
    if (!path) return "";
    const char* a = strrchr(path, '/'); const char* b = strrchr(path, '\\');
    const char* p = !a ? b : (!b ? a : (a > b ? a : b));
    return p ? p + 1 : path;
}
static int form_urlencode(FormBuf* b, const char* s) {
    static const char hex[] = "0123456789ABCDEF";
    if (!s) s = "";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c)) {
            if (!form_buf_c(b, (char)c)) return 0;
        } else if (c == ' ') { if (!form_buf_c(b, '+')) return 0; }
        else { char x[3] = { '%', hex[c >> 4], hex[c & 15] }; if (!form_buf_add(b, x, sizeof x)) return 0; }
    }
    return 1;
}
static int form_mp_quote(FormBuf* b, const char* s) {
    if (!s) s = "";
    for (; *s; s++) {
        if (*s == '\r') { if (!form_buf_s(b, "%0D")) return 0; }
        else if (*s == '\n') { if (!form_buf_s(b, "%0A")) return 0; }
        else if (*s == '"') { if (!form_buf_s(b, "%22")) return 0; }
        else if (!form_buf_c(b, *s)) return 0;
    }
    return 1;
}
static int form_read_selected_file(const DOMNode* n, FormBuf* file) {
    if (!n || !n->file_selected || !n->value[0]) return 1; /* empty file control */
    FILE* f = fopen(n->value, "rb"); if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > FORM_MAX_FILE || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    size_t size = (size_t)sz;
    unsigned char* tmp = malloc(size ? size : 1);
    if (!tmp) { fclose(f); return 0; }
    size_t got = size ? fread(tmp, 1, size, f) : 0; int ok = got == size && !ferror(f); fclose(f);
    if (ok) ok = form_buf_add(file, tmp, size);
    free(tmp); return ok;
}
static int form_add_url_field(FormBuf* b, int* first, const char* name, const char* value) {
    if (!*first && !form_buf_c(b, '&')) return 0;
    *first = 0;
    return form_urlencode(b, name) && form_buf_c(b, '=') && form_urlencode(b, value);
}
static int form_add_text_part(FormBuf* b, const char* boundary, const char* name, const char* value) {
    return form_buf_s(b, "--") && form_buf_s(b, boundary) && form_buf_s(b, "\r\nContent-Disposition: form-data; name=\"") &&
        form_mp_quote(b, name) && form_buf_s(b, "\"\r\n\r\n") && form_buf_s(b, value) && form_buf_s(b, "\r\n");
}
static int form_add_file_part(FormBuf* b, const char* boundary, const DOMNode* input) {
    const char* path = (input && input->file_selected) ? input->value : "";
    const char* name = path[0] ? form_basename(path) : "";
    if (!form_buf_s(b, "--") || !form_buf_s(b, boundary) || !form_buf_s(b, "\r\nContent-Disposition: form-data; name=\"") ||
        !form_mp_quote(b, input->name) || !form_buf_s(b, "\"; filename=\"") || !form_mp_quote(b, name) ||
        !form_buf_s(b, "\"\r\nContent-Type: application/octet-stream\r\n\r\n")) return 0;
    if (path[0] && !form_read_selected_file(input, b)) return 0;
    return form_buf_s(b, "\r\n");
}
static int form_add_text_plain(FormBuf* b, int* first, const char* name, const char* value) {
    (void)first;
    return form_buf_s(b, name) && form_buf_c(b, '=') && form_buf_s(b, value) && form_buf_s(b, "\r\n");
}

void webpage_form_submission_free(WebFormSubmission* submission) {
    if (!submission) return; free(submission->body); memset(submission, 0, sizeof *submission);
}

int webpage_form_request(DOMNode* control, DOMNode* submitter, WebFormSubmission* out) {
    if (!out) return 0; memset(out, 0, sizeof *out);
    if (!g_head || !control) return 0;
    DOMNode** nv = NULL; int n = 0;
    for (DOMNode* p = g_head; p; p = p->next) n++;
    nv = malloc((size_t)n * sizeof(DOMNode*)); if (!nv) return 0;
    { int i = 0; for (DOMNode* p = g_head; p; p = p->next) nv[i++] = p; }
    DOMNode* form = control;
    while (form && strcmp(form->tag, "form") != 0) form = form->parent >= 0 && form->parent < n ? nv[form->parent] : NULL;
    if (!form) { free(nv); return 0; }

    char method[16] = "get", action[600] = "", enctype[96] = "application/x-www-form-urlencoded";
    webpage_attr_get(form, "method", method, sizeof method); webpage_attr_get(form, "action", action, sizeof action); webpage_attr_get(form, "enctype", enctype, sizeof enctype);
    if (submitter) {
        char override[600];
        if (webpage_attr_get(submitter, "formmethod", override, sizeof override) && override[0]) snprintf(method, sizeof method, "%s", override);
        if (webpage_attr_get(submitter, "formaction", override, sizeof override) && override[0]) snprintf(action, sizeof action, "%s", override);
        if (webpage_attr_get(submitter, "formenctype", override, sizeof override) && override[0]) snprintf(enctype, sizeof enctype, "%s", override);
    }
    int is_post = !strcasecmp(method, "post");
    snprintf(out->method, sizeof out->method, "%s", is_post ? "POST" : "GET");
    char resolved[1200]; webpage_resolve_url(g_url, action[0] ? action : g_url, resolved, sizeof resolved);
    char* hash = strchr(resolved, '#'); if (hash) *hash = 0;
    if (!resolved[0] || strlen(resolved) >= sizeof out->url) { free(nv); return 0; }

    int multipart = is_post && !strcasecmp(enctype, "multipart/form-data");
    int plain = is_post && !strcasecmp(enctype, "text/plain");
    FormBuf data = {0}; int first = 1, failed = 0;
    char boundary[80] = "";
    if (multipart) { static unsigned int seq; snprintf(boundary, sizeof boundary, "----MLBrowserBoundary%08lx%08x", (unsigned long)time(NULL), ++seq); }

    for (int i = form->idx + 1; i <= form->subtree_end && i < n; i++) {
        DOMNode* e = nv[i]; if (!e || e->type == TAG_TEXT || e->disabled || !e->name[0]) continue;
        const char* value = NULL; char label[256]; int is_file = 0, include = 0;
        if (e->type == TAG_INPUT) {
            const char* t = e->input_type;
            if (!strcmp(t, "submit") || !strcmp(t, "image")) { if (e != submitter) continue; value = e->value; include = 1; }
            else if (!strcmp(t, "button") || !strcmp(t, "reset")) continue;
            else if (!strcmp(t, "file")) { is_file = 1; include = 1; value = e->file_selected ? form_basename(e->value) : ""; }
            else if (!strcmp(t, "checkbox") || !strcmp(t, "radio")) { if (!e->checked) continue; value = e->value[0] ? e->value : "on"; include = 1; }
            else { value = e->value; include = 1; }
        } else if (e->type == TAG_TEXTAREA) { value = e->value; include = 1; }
        else if (e->type == TAG_SELECT) {
            DOMNode* firstopt = NULL; int emitted = 0;
            for (int k = e->idx + 1; k <= e->subtree_end && k < n; k++) if (nv[k]->type == TAG_OPTION && !nv[k]->disabled) {
                if (!firstopt) firstopt = nv[k];
                if (nv[k]->selected) { value = nv[k]->value; if (!value[0]) { option_label(e, label, sizeof label); value = label; }
                    if (multipart) failed = !form_add_text_part(&data, boundary, e->name, value);
                    else if (plain) failed = !form_add_text_plain(&data, &first, e->name, value);
                    else if (is_post || !strcasecmp(method, "get")) failed = !form_add_url_field(&data, &first, e->name, value);
                    if (failed) break; emitted = 1; if (!e->multiple) break;
                }
            }
            if (!failed && !emitted && firstopt && !e->multiple) {
                value = firstopt->value; if (!value[0]) { option_label(e, label, sizeof label); value = label; }
                if (multipart) failed = !form_add_text_part(&data, boundary, e->name, value);
                else if (plain) failed = !form_add_text_plain(&data, &first, e->name, value);
                else failed = !form_add_url_field(&data, &first, e->name, value);
            }
            if (failed) break; continue;
        } else if (e->type == TAG_BUTTON) { if (e != submitter) continue; value = e->value; include = 1; }
        if (!include) continue;
        if (multipart) failed = is_file ? !form_add_file_part(&data, boundary, e) : !form_add_text_part(&data, boundary, e->name, value);
        else if (plain) failed = is_file ? !form_add_text_plain(&data, &first, e->name, value) : !form_add_text_plain(&data, &first, e->name, value);
        else failed = !form_add_url_field(&data, &first, e->name, value);
        if (failed) break;
    }
    free(nv);
    if (failed || data.n > FORM_MAX_BODY) { free(data.p); return 0; }

    if (multipart) snprintf(out->headers, sizeof out->headers, "Content-Type: multipart/form-data; boundary=%s\n", boundary);
    else if (plain) snprintf(out->headers, sizeof out->headers, "Content-Type: text/plain; charset=UTF-8\n");
    else if (is_post) snprintf(out->headers, sizeof out->headers, "Content-Type: application/x-www-form-urlencoded\n");

    if (!is_post && data.n) {
        size_t ulen = strlen(resolved); int has_query = strchr(resolved, '?') != NULL;
        int sep = has_query ? ((ulen && (resolved[ulen-1] == '?' || resolved[ulen-1] == '&')) ? 0 : 1) : 0;
        if (ulen + (size_t)sep + data.n + 1 > sizeof out->url) { free(data.p); return 0; }
        memcpy(out->url, resolved, ulen); size_t pos = ulen;
        if (!has_query) out->url[pos++] = '?'; else if (sep) out->url[pos++] = '&';
        memcpy(out->url + pos, data.p, data.n); out->url[pos + data.n] = 0;
    } else snprintf(out->url, sizeof out->url, "%s", resolved);
    if (is_post) { out->body = data.p; out->body_len = data.n; }
    else free(data.p);
    return 1;
}

int webpage_form_url(DOMNode* control, DOMNode* submitter, char* out, int cap) {
    WebFormSubmission request; if (!webpage_form_request(control, submitter, &request)) return 0;
    if (out && cap > 0) snprintf(out, (size_t)cap, "%s", request.url);
    webpage_form_submission_free(&request); return out && cap > 0;
}

/* Клик по элементу страницы. Возвращает: 0 — ничего, 1 — сфокусировать поле ввода,
   2 — перейти по url (отправка формы), 3 — обработано (переключение/кнопка). */
int webpage_click(DOMNode* head, DOMNode* n, char* url, int cap, WebFormSubmission* submission) {
    if (!n) return 0;
    const char* t = n->input_type;
    if (n->type == TAG_INPUT) {
        if (!strcmp(t, "checkbox")) { if (!n->disabled) n->checked = !n->checked; return 3; }
        if (!strcmp(t, "radio")) {
            if (n->disabled) return 3;
            for (DOMNode* p = head; p; p = p->next) if (p->type == TAG_INPUT && !strcmp(p->input_type, "radio") && p->name[0] && !strcmp(p->name, n->name)) p->checked = 0;
            n->checked = 1; return 3;
        }
        if (!strcmp(t, "submit") || !strcmp(t, "image")) {
            if (submission && webpage_form_request(n, n, submission)) { snprintf(url, (size_t)cap, "%s", submission->url); return 2; }
            return 3;
        }
        if (!strcmp(t, "button") || !strcmp(t, "reset") || !strcmp(t, "file") || !strcmp(t, "color") || !strcmp(t, "range")) return 3;
        return 1;
    }
    if (n->type == TAG_TEXTAREA) return 1;
    if (n->type == TAG_BUTTON) {
        if (!t[0] || !strcmp(t, "submit")) {
            if (submission && webpage_form_request(n, n, submission)) { snprintf(url, (size_t)cap, "%s", submission->url); return 2; }
            return 3;
        }
        return 3;
    }
    return 0;
}

/* ══════════════ для JS-движка ══════════════ */
void webpage_invalidate_layout(void) { g_head = NULL; }
void webpage_force_layout(void) {
    if (!g_last_head) return;
    webpage_force_layout_for(g_last_head);
}
void webpage_force_layout_for(DOMNode* head) {
    if (!head) return;
    if (g_top <= 0 || g_top >= SCREEN_HEIGHT) g_top = 80;
    int vw = SCREEN_WIDTH - 10, vh = SCREEN_HEIGHT - g_top;
    if (vw < 100) vw = 100; if (vh < 50) vh = 50;
    g_last_head = head; g_head = NULL;
    l2_layout(head, vw, vh); g_head = head; g_fp = fingerprint(head); g_lw = vw; g_lh = vh;
    layout_content_width = (int)l2_doc_w - 10; layout_content_height = (int)l2_doc_h - 20;
}
void webpage_get_scroll(int* x, int* y, int* vw, int* vh) { *x = g_sx; *y = g_sy; *vw = SCREEN_WIDTH - 10; *vh = SCREEN_HEIGHT - g_top; }
int webpage_viewport_top(void) { return g_top; }

/* border-box элемента в координатах документа; для inline-элементов — объединение текстовых фрагментов */
static int node_rect_raw(DOMNode* n, float* out) {
    if (!n || !l2_dl) return 0;
    if (l2_rects && n->idx >= 0 && n->idx < l2_nrects) { const float* r = &l2_rects[n->idx * 4]; if (r[2] > 0 || r[3] > 0) { memcpy(out, r, 4 * sizeof(float)); return 1; } }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; int any = 0;
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i]; if (c->t != C_TEXT || c->fixed || !c->node) continue;
        /* Text nodes have no C_MARK entry of their own. Match their own glyph runs;
           for elements, aggregate text descendants as before. */
        int match = n->type == TAG_TEXT ? c->node == n : (c->node->idx > n->idx && c->node->idx <= n->subtree_end);
        if (!match) continue;
        if (c->x < x0) x0 = c->x; if (c->y < y0) y0 = c->y; if (c->x + c->w > x1) x1 = c->x + c->w; if (c->y + c->h > y1) y1 = c->y + c->h; any = 1;
    }
    if (!any) return 0;
    out[0] = x0; out[1] = y0; out[2] = x1 - x0; out[3] = y1 - y0; return 1;
}

int webpage_node_rect(DOMNode* n, float* out) {
    if (!node_rect_raw(n, out)) return 0;
    float ox, oy; l2_scroll_offset_of_ancestors(n, &ox, &oy); out[0] -= ox; out[1] -= oy;   /* прокрутка внутри предков-контейнеров */
    return 1;
}

/* Самый глубокий элемент под курсором (цель события для JS), не только кликабельный. */
DOMNode* webpage_hit_element(DOMNode* head, int mx, int my) {
    if (!head || !l2_dl || my < g_top) return NULL;
    Frame fr[2]; fr_init(&fr[0], 0, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT); fr_init(&fr[1], 1, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    const Cmd* best[2] = {NULL, NULL};
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i]; int k = c->fixed ? 1 : 0; Frame* f = &fr[k];
        if (fr_is_ctl(c)) { fr_ctl(f, c); continue; }
        if (c->t != C_MARK && c->t != C_TEXT && c->t != C_IMG && c->t != C_CTRL && c->t != C_HIT) continue;
        if (!c->node || (c->t == C_TEXT && c->node->parent < 0)) continue;
        if (c->node->st.pointer_events == 1) continue;
        if (!fr_in_clip(f, mx, my)) continue;
        float x0 = c->x + f->dx, y0 = c->y + f->dy, hx, hy; fr_map(f, mx, my, &hx, &hy);
        if (hx >= x0 && hx < x0 + c->w && hy >= y0 && hy < y0 + c->h) best[k] = c;
    }
    const Cmd* c = best[1] ? best[1] : best[0]; if (!c) return NULL;
    if (c->t == C_TEXT) { int par = c->node->parent; DOMNode* q = head; for (int k = 0; k < par && q; k++) q = q->next; return q; }
    return c->node;
}

/* Колесо мыши над прокручиваемым контейнером: прокручиваем самый вложенный контейнер под курсором, который ещё может
   двигаться в этом направлении (иначе событие уходит наружу — к предку или к странице). 1 — прокрутка выполнена. */
int webpage_scroll_at(int mx, int my, int dx, int dy) {
    if (!l2_dl || my < g_top || (!dx && !dy)) return 0;
    Frame fr[2]; fr_init(&fr[0], 0, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT); fr_init(&fr[1], 1, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    struct { const DOMNode* n; unsigned char fl; } cand[2][64]; int nc[2] = {0, 0};
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i]; if (!fr_is_ctl(c)) continue;
        int k = c->fixed ? 1 : 0; Frame* f = &fr[k]; fr_ctl(f, c);
        if (f->scroller && nc[k] < 64 && fr_in_clip(f, mx, my)) { cand[k][nc[k]].n = f->scroller; cand[k][nc[k]].fl = f->scroller_fl; nc[k]++; }
    }
    for (int pass = 1; pass >= 0; pass--) for (int j = nc[pass] - 1; j >= 0; j--) {
        const DOMNode* n = cand[pass][j].n; unsigned fl = cand[pass][j].fl; float x, y, mxs, mys; l2_scroll_get(n, &x, &y); l2_scroll_max(n, &mxs, &mys);
        float nx = x, ny = y; int moved = 0;
        if (dy && (fl & CF_SCROLL_Y) && mys > 0) { float t = y + (float)dy; if (t < 0) t = 0; if (t > mys) t = mys; if (t != y) { ny = t; moved = 1; } }
        if (dx && (fl & CF_SCROLL_X) && mxs > 0) { float t = x + (float)dx; if (t < 0) t = 0; if (t > mxs) t = mxs; if (t != x) { nx = t; moved = 1; } }
        if (moved) { l2_scroll_set(n, nx, ny); return 1; }
    }
    return 0;
}

/* Для JS: scrollTop/scrollLeft/scrollHeight/clientHeight элементов-контейнеров */
int  webpage_elem_scroll_get(DOMNode* n, float* x, float* y, float* sw, float* sh, float* cw, float* ch) {
    if (!l2_scroll_get(n, x, y)) return 0; l2_scroll_size(n, sw, sh, cw, ch); return 1;
}
void webpage_elem_scroll_set(DOMNode* n, float x, float y) { l2_scroll_set(n, x, y); }
void webpage_elem_scroll_set_smooth(DOMNode* n, float x, float y) { l2_scroll_set_raw(n, x, y); }
