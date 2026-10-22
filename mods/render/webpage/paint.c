/* Отрисовка display list, hit-test, выделение текста, GET-отправка форм. */
#include "l2.h"
#include "layout.h"
#include "font_manager.h"
#include "gal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <strings.h>

extern TTFFont* g_ttf_font;

static DOMNode* g_head; static DOMNode* g_last_head; static uint64_t g_fp; static int g_lw = -1, g_lh = -1;
static int g_top, g_sx, g_sy; static char g_url[600];
static int cl_x0, cl_y0, cl_x1, cl_y1;

/* ══════════════ примитивы ══════════════ */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline void blend(int x, int y, uint32_t rgb, int a) {
    if (a <= 0 || x < cl_x0 || x >= cl_x1 || y < cl_y0 || y >= cl_y1) return;
    uint32_t* p = &back_buffer[(size_t)y * SCREEN_WIDTH + x];
    if (a >= 255) { *p = rgb & 0xFFFFFF; return; }
    uint32_t d = *p; int ia = 255 - a;
    *p = ((uint32_t)((((rgb >> 16) & 255) * a + ((d >> 16) & 255) * ia + 127) / 255) << 16) |
         ((uint32_t)((((rgb >> 8) & 255) * a + ((d >> 8) & 255) * ia + 127) / 255) << 8) |
          (uint32_t)(((rgb & 255) * a + (d & 255) * ia + 127) / 255);
}

static int alpha_of(uint32_t argb, float opacity) { float a = (argb >> 24) * opacity; return a < 0 ? 0 : a > 255 ? 255 : (int)(a + 0.5f); }

static void fill_rect_i(int x0, int y0, int x1, int y1, uint32_t argb, float opacity) {
    int a = alpha_of(argb, opacity); if (a <= 0) return;
    if (x0 < cl_x0) x0 = cl_x0; if (y0 < cl_y0) y0 = cl_y0; if (x1 > cl_x1) x1 = cl_x1; if (y1 > cl_y1) y1 = cl_y1;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) blend(x, y, argb, a);
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

static void draw_text(TTFFont* f, const char* s, int x, int y, int size, uint32_t argb, float opacity, int style) {
    if (!f || !s[0]) return;
    uint32_t col = effective_text_color(argb, opacity, x + 2, y + size / 2);
    ttf_render_string_ex(f, s, back_buffer, SCREEN_WIDTH, cl_x0, cl_y0, cl_x1, cl_y1, size, x, y, col, style);
}

static int sel_rect(int* sx, int* sy, int* sw, int* sh) {
    if (!layout_selection_active) return 0;
    *sx = layout_selection_x1 < layout_selection_x2 ? layout_selection_x1 : layout_selection_x2;
    *sy = layout_selection_y1 < layout_selection_y2 ? layout_selection_y1 : layout_selection_y2;
    *sw = abs(layout_selection_x2 - layout_selection_x1); *sh = abs(layout_selection_y2 - layout_selection_y1); return 1;
}
static int rect_hit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) { return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by; }

static void paint_text(const Cmd* c, int dx, int dy) {
    TTFFont* f = font_manager_get_styled(c->fam, c->weight ? c->weight : 400, c->ital); if (!f) return;
    int x = (int)floorf(c->x + dx + 0.5f), y = (int)floorf(c->y + dy + 0.5f);
    int sx, sy, sw, sh;
    if (sel_rect(&sx, &sy, &sw, &sh) && rect_hit(x, y, (int)c->w + 1, (int)c->h, sx, sy, sw, sh)) {
        size_t n = strlen(c->s); int* offs = malloc((n + 1) * sizeof(int));
        if (offs) {
            ttf_text_offsets_ex(f, c->s, c->size, c->tstyle, offs, (int)n + 1);
            for (size_t i = 0; i < n;) {
                size_t l = 1; unsigned char ch = (unsigned char)c->s[i]; if (ch >= 0xF0) l = 4; else if (ch >= 0xE0) l = 3; else if (ch >= 0xC0) l = 2; if (i + l > n) l = n - i;
                int cx0 = x + offs[i], cw = offs[i + l] - offs[i];
                if (rect_hit(cx0, y, cw, (int)c->h, sx, sy, sw, sh)) fill_rect_i(cx0, y, cx0 + cw, y + (int)c->h, 0xFFBFDBFE, 1);
                i += l;
            }
            free(offs);
        }
    }
    const NodeStyle* ost = c->node ? &c->node->st : NULL;
    if (ost && ost->bg_clip_text && ost->bg_grad_type) {                       /* -webkit-background-clip:text — красим по буквам вдоль градиента */
        size_t n = strlen(c->s); int* offs = malloc((n + 1) * sizeof(int));
        if (offs) {
            ttf_text_offsets_ex(f, c->s, c->size, c->tstyle, offs, (int)n + 1);
            float fullw = (float)offs[n]; if (fullw < 1) fullw = 1;
            size_t i = 0;
            while (i < n) {
                size_t l = 1; unsigned char ch = (unsigned char)c->s[i];
                if (ch >= 0xF0) l = 4; else if (ch >= 0xE0) l = 3; else if (ch >= 0xC0) l = 2;
                if (i + l > n) l = n - i;
                float mid = (offs[i] + offs[i + l]) * 0.5f, t = mid / fullw * 100.0f;
                uint32_t col = grad_color_at(ost, t);
                char tmp[8]; size_t cl = l < 7 ? l : 7; memcpy(tmp, c->s + i, cl); tmp[cl] = 0;
                draw_text(f, tmp, x + offs[i], y, c->size, (col & 0xFFFFFFu) | 0xFF000000u, c->opacity, c->tstyle);
                i += l;
            }
            free(offs);
        } else draw_text(f, c->s, x, y, c->size, c->color, c->opacity, c->tstyle);
    } else draw_text(f, c->s, x, y, c->size, c->color, c->opacity, c->tstyle);
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
    float bx0, by0, bx1, by1, sw, sh, ox, oy; unsigned char fl;               /* для скроллбара: рамка контейнера (экран), размеры, смещения, флаги */
    const DOMNode* node;
} FrSave;
typedef struct {
    int pass; int cx0, cy0, cx1, cy1; float dx, dy, ry0, ry1;
    FrSave st[FR_MAX]; int sp;
    int popped; FrSave pop;                                                    /* только что закрытый прокручиваемый контейнер */
    const DOMNode* scroller; unsigned char scroller_fl; float s_ox, s_oy, s_mx, s_my;   /* только что открытый прокручиваемый контейнер (для колеса) */
} Frame;

static void fr_init(Frame* f, int pass, int x0, int y0, int x1, int y1) {
    memset(f, 0, sizeof *f); f->pass = pass; f->cx0 = x0; f->cy0 = y0; f->cx1 = x1; f->cy1 = y1;
    f->dx = pass ? 0.0f : (float)-g_sx; f->dy = pass ? (float)g_top : (float)(g_top - g_sy); f->ry0 = (float)y0; f->ry1 = (float)y1;
}
static void fr_sync(const Frame* f) { cl_x0 = f->cx0; cl_y0 = f->cy0; cl_x1 = f->cx1; cl_y1 = f->cy1; }
static void fr_push(Frame* f, unsigned char kind, FrSave** out) {
    FrSave* s = f->sp < FR_MAX ? &f->st[f->sp] : NULL;
    if (s) { memset(s, 0, sizeof *s); s->cx0 = f->cx0; s->cy0 = f->cy0; s->cx1 = f->cx1; s->cy1 = f->cy1; s->dx = f->dx; s->dy = f->dy; s->ry0 = f->ry0; s->ry1 = f->ry1; s->kind = kind; }
    f->sp++; *out = s;
}
static int fr_is_ctl(const Cmd* c) { return c->t == C_CLIP || c->t == C_UNCLIP || c->t == C_STK || c->t == C_ENDSTK; }
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
            f->dx -= ox; f->dy -= oy; f->ry0 = (float)y0; f->ry1 = (float)y1;
        }
        break; }
    case C_UNCLIP: {
        if (f->sp > 0 && f->sp <= FR_MAX && f->st[f->sp - 1].kind == 2) return;      /* закрытие не того типа — игнорируем */
        if (f->sp > 0) { f->sp--; if (f->sp < FR_MAX) { FrSave* p = &f->st[f->sp]; if (p->kind == 1) { f->popped = 1; f->pop = *p; }
            f->cx0 = p->cx0; f->cy0 = p->cy0; f->cx1 = p->cx1; f->cy1 = p->cy1; f->dx = p->dx; f->dy = p->dy; f->ry0 = p->ry0; f->ry1 = p->ry1; } }
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
        f->dy += shift; break; }
    case C_ENDSTK: {
        if (f->sp > 0 && (f->sp > FR_MAX || f->st[f->sp - 1].kind == 2)) { f->sp--; if (f->sp < FR_MAX) { FrSave* p = &f->st[f->sp]; f->dy = p->dy; f->dx = p->dx; } }
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

static void paint_pass(int fixed_pass, int base_x0, int base_y0, int base_x1, int base_y1) {
    Frame f; fr_init(&f, fixed_pass, base_x0, base_y0, base_x1, base_y1); fr_sync(&f);
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i];
        if (fr_is_ctl(c)) { fr_ctl(&f, c); fr_sync(&f); if (f.popped) draw_elem_scrollbars(&f.pop); continue; }
        if (c->fixed != fixed_pass) continue;
        int dx = (int)floorf(f.dx + 0.5f), dy = (int)floorf(f.dy + 0.5f);
        /* быстрый отказ: вне экрана */
        float sy0 = c->y + dy, sy1 = c->y + c->h + dy, sx0 = c->x + dx, sx1 = c->x + c->w + dx;
        if (c->t != C_HIT && (sy1 < cl_y0 - 40 || sy0 > cl_y1 + 40 || sx1 < cl_x0 - 40 || sx0 > cl_x1 + 40)) { if (c->t != C_BOX || (sy1 < cl_y0 - 400 || sy0 > cl_y1 + 400)) continue; }
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
            ImgPix* im = webpage_image_scaled(c->s, w, h); float rad[4]; resolve_radii(&c->node->st, (float)w, (float)h, rad);
            if (!im) im = webpage_image_get(c->s);
            blit_img(im, x0, y0, im ? (im->w < w ? im->w : w) : w, im ? (im->h < h ? im->h : h) : h, rad, c->opacity); break; }
        case C_BGIMG: paint_bgimg(c, dx, dy); break;
        case C_CTRL: paint_ctrl(c, dx, dy); break;
        default: break;
        }
    }
}

static uint64_t fingerprint(DOMNode* head) {
    uint64_t h = 1469598103934665603ULL; int n = 0;
    for (DOMNode* p = head; p; p = p->next, n++) { h = (h ^ (uint64_t)(p->type + 1)) * 1099511628211ULL; h = (h ^ (unsigned char)p->text[0]) * 1099511628211ULL; h = (h ^ (unsigned char)p->text[1]) * 1099511628211ULL; h = (h ^ (uint64_t)(p->st.font_size * 10)) * 1099511628211ULL; }
    return h ^ (uint64_t)n ^ (uint64_t)(uintptr_t)head;
}

void webpage_render(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    if (!head) return;
    g_top = top_offset; g_sx = scroll_x; g_sy = scroll_y; snprintf(g_url, sizeof g_url, "%s", page_url ? page_url : "");
    int vw = SCREEN_WIDTH - 10, vh = SCREEN_HEIGHT - top_offset; if (vw < 100) vw = 100;
    uint64_t fp = fingerprint(head); g_last_head = head;
    if (head != g_head || fp != g_fp || vw != g_lw || vh != g_lh) { l2_layout(head, vw, vh); g_head = head; g_fp = fp; g_lw = vw; g_lh = vh; }
    /* clamp_page_scroll() считает max = content - SCREEN + top_offset + 20 → подбираем значения так, чтобы низ страницы достижим ровно */
    if (getenv("WP_DEBUG")) gal_log("INFO", "[wp] doc %.0fx%.0f top=%d scroll=%d", l2_doc_w, l2_doc_h, top_offset, scroll_y);
    layout_content_width = (int)l2_doc_w - 10; layout_content_height = (int)l2_doc_h - 20;
    cl_x0 = 0; cl_y0 = top_offset; cl_x1 = SCREEN_WIDTH; cl_y1 = SCREEN_HEIGHT;
    fill_rect_i(0, top_offset, SCREEN_WIDTH, SCREEN_HEIGHT, l2_canvas_bg | 0xFF000000u, 1);       /* холст */
    paint_pass(0, 0, top_offset, SCREEN_WIDTH, SCREEN_HEIGHT);
    paint_pass(1, 0, top_offset, SCREEN_WIDTH, SCREEN_HEIGHT);
}

/* ══════════════ взаимодействие ══════════════ */
DOMNode* webpage_hit_test(DOMNode* head, int mx, int my) {
    (void)head; if (!l2_dl || my < g_top) return NULL;
    Frame fr[2]; fr_init(&fr[0], 0, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT); fr_init(&fr[1], 1, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    DOMNode* best[2] = {NULL, NULL};
    for (int i = 0; i < l2_dn; i++) {                       /* проход вперёд: побеждает команда, нарисованная позже (выше); fixed — выше обычных */
        const Cmd* c = &l2_dl[i]; int k = c->fixed ? 1 : 0; Frame* f = &fr[k];
        if (fr_is_ctl(c)) { fr_ctl(f, c); continue; }
        if (c->t != C_HIT) continue;
        if (mx < f->cx0 || mx >= f->cx1 || my < f->cy0 || my >= f->cy1) continue;     /* скрыто отсечением overflow */
        float x0 = c->x + f->dx, y0 = c->y + f->dy;
        if (mx >= x0 && mx < x0 + c->w && my >= y0 && my < y0 + c->h) best[k] = c->node;
    }
    return best[1] ? best[1] : best[0];
}

int webpage_copy_selection(char* out, int cap) {
    int sx, sy, sw, sh; if (!sel_rect(&sx, &sy, &sw, &sh) || cap < 2) return 0;
    int o = 0; float last_y = -1e9f; int any = 0;
    Frame fr; fr_init(&fr, 0, 0, g_top, SCREEN_WIDTH, SCREEN_HEIGHT);
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i]; if (fr_is_ctl(c)) { fr_ctl(&fr, c); continue; }
        if (c->t != C_TEXT || c->fixed) continue;
        int x = (int)(c->x + fr.dx + 0.5f), y = (int)(c->y + fr.dy + 0.5f);
        if (y + (int)c->h <= fr.cy0 || y >= fr.cy1 || x + (int)c->w <= fr.cx0 || x >= fr.cx1) continue;      /* скрытое отсечением */
        if (!rect_hit(x, y, (int)c->w + 1, (int)c->h, sx, sy, sw, sh)) continue;
        TTFFont* f = font_manager_get_styled(c->fam, c->weight ? c->weight : 400, c->ital); if (!f) continue;
        size_t n = strlen(c->s); int* offs = malloc((n + 1) * sizeof(int)); if (!offs) continue;
        ttf_text_offsets_ex(f, c->s, c->size, c->tstyle, offs, (int)n + 1);
        int started = 0;
        for (size_t k = 0; k < n;) {
            size_t l = 1; unsigned char ch = (unsigned char)c->s[k]; if (ch >= 0xF0) l = 4; else if (ch >= 0xE0) l = 3; else if (ch >= 0xC0) l = 2; if (k + l > n) l = n - k;
            if (rect_hit(x + offs[k], y, offs[k + l] - offs[k], (int)c->h, sx, sy, sw, sh)) {
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

static void url_enc(const char* s, char* out, size_t cap, size_t* o) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') { if (*o + 1 < cap) out[(*o)++] = (char)c; }
        else if (c == ' ') { if (*o + 1 < cap) out[(*o)++] = '+'; }
        else if (*o + 3 < cap) { snprintf(out + *o, cap - *o, "%%%02X", c); *o += 3; }
    }
    out[*o] = 0;
}

int webpage_form_url(DOMNode* control, DOMNode* submitter, char* out, int cap) {
    if (!g_head || !control) return 0;
    DOMNode* form = control; DOMNode** nv = NULL; int n = 0;
    for (DOMNode* p = g_head; p; p = p->next) n++;
    nv = malloc((size_t)n * sizeof(DOMNode*)); if (!nv) return 0;
    { int i = 0; for (DOMNode* p = g_head; p; p = p->next) nv[i++] = p; }
    while (form && strcmp(form->tag, "form") != 0) form = form->parent >= 0 ? nv[form->parent] : NULL;
    if (!form) { free(nv); return 0; }
    char method[16] = "get", action[600] = ""; webpage_attr_get(form, "method", method, sizeof method); webpage_attr_get(form, "action", action, sizeof action);
    if (!strcasecmp(method, "post")) { free(nv); return 0; }
    char abs[900]; webpage_resolve_url(g_url, action[0] ? action : g_url, abs, sizeof abs);
    char* q = strchr(abs, '?'); if (q) *q = 0; char* h = strchr(abs, '#'); if (h) *h = 0;
    size_t o = 0; char* body = malloc((size_t)cap); if (!body) { free(nv); return 0; } body[0] = 0; int first = 1;
    for (int i = form->idx + 1; i <= form->subtree_end; i++) {
        DOMNode* e = nv[i]; if (e->type == TAG_TEXT || e->disabled || !e->name[0]) continue;
        const char* val = NULL; char lb[256];
        if (e->type == TAG_INPUT) {
            const char* t = e->input_type;
            if (!strcmp(t, "submit") || !strcmp(t, "button") || !strcmp(t, "image") || !strcmp(t, "reset") || !strcmp(t, "file")) { if (e != submitter || !strcmp(t, "button") || !strcmp(t, "reset")) continue; val = e->value; }
            else if (!strcmp(t, "checkbox") || !strcmp(t, "radio")) { if (!e->checked) continue; val = e->value[0] ? e->value : "on"; }
            else val = e->value;
        } else if (e->type == TAG_TEXTAREA) val = e->value;
        else if (e->type == TAG_SELECT) {
            DOMNode* pick = NULL; DOMNode* firstopt = NULL;
            for (int k = e->idx + 1; k <= e->subtree_end; k++) if (nv[k]->type == TAG_OPTION) { if (!firstopt) firstopt = nv[k]; if (nv[k]->selected && !pick) pick = nv[k]; }
            if (!pick) pick = firstopt; if (!pick) continue;
            val = pick->value; if (!val[0]) { option_label(e, lb, sizeof lb); val = lb; }
        } else if (e->type == TAG_BUTTON) { if (e != submitter) continue; val = e->value; }
        else continue;
        if (!first && o + 1 < (size_t)cap) body[o++] = '&'; first = 0;
        url_enc(e->name, body, (size_t)cap, &o); if (o + 1 < (size_t)cap) body[o++] = '='; body[o] = 0; url_enc(val ? val : "", body, (size_t)cap, &o);
    }
    snprintf(out, (size_t)cap, "%s%s%s", abs, body[0] ? "?" : "", body);
    free(body); free(nv); return 1;
}

/* Клик по элементу страницы. Возвращает: 0 — ничего, 1 — сфокусировать поле ввода,
   2 — перейти по url (отправка формы), 3 — обработано (переключение/кнопка). */
int webpage_click(DOMNode* head, DOMNode* n, char* url, int cap) {
    if (!n) return 0;
    const char* t = n->input_type;
    if (n->type == TAG_INPUT) {
        if (!strcmp(t, "checkbox")) { if (!n->disabled) n->checked = !n->checked; return 3; }
        if (!strcmp(t, "radio")) {
            if (n->disabled) return 3;
            for (DOMNode* p = head; p; p = p->next) if (p->type == TAG_INPUT && !strcmp(p->input_type, "radio") && p->name[0] && !strcmp(p->name, n->name)) p->checked = 0;
            n->checked = 1; return 3;
        }
        if (!strcmp(t, "submit") || !strcmp(t, "image")) return webpage_form_url(n, n, url, cap) ? 2 : 3;
        if (!strcmp(t, "button") || !strcmp(t, "reset") || !strcmp(t, "file") || !strcmp(t, "color") || !strcmp(t, "range")) return 3;
        return 1;
    }
    if (n->type == TAG_TEXTAREA) return 1;
    if (n->type == TAG_BUTTON) {
        if (!t[0] || !strcmp(t, "submit")) return webpage_form_url(n, n, url, cap) ? 2 : 3;
        return 3;
    }
    return 0;
}

/* ══════════════ для JS-движка ══════════════ */
void webpage_invalidate_layout(void) { g_head = NULL; }
void webpage_force_layout(void) {
    if (!g_last_head) return;
    int vw = g_lw > 0 ? g_lw : SCREEN_WIDTH - 10, vh = g_lh > 0 ? g_lh : SCREEN_HEIGHT - 80;
    l2_layout(g_last_head, vw, vh); g_head = g_last_head; g_fp = fingerprint(g_head); g_lw = vw; g_lh = vh;
}
void webpage_get_scroll(int* x, int* y, int* vw, int* vh) { *x = g_sx; *y = g_sy; *vw = SCREEN_WIDTH - 10; *vh = SCREEN_HEIGHT - g_top; }

/* border-box элемента в координатах документа; для inline-элементов — объединение текстовых фрагментов */
static int node_rect_raw(DOMNode* n, float* out) {
    if (!n || !l2_dl) return 0;
    if (l2_rects && n->idx >= 0 && n->idx < l2_nrects) { const float* r = &l2_rects[n->idx * 4]; if (r[2] > 0 || r[3] > 0) { memcpy(out, r, 4 * sizeof(float)); return 1; } }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; int any = 0;
    for (int i = 0; i < l2_dn; i++) {
        const Cmd* c = &l2_dl[i]; if (c->t != C_TEXT || c->fixed || !c->node) continue;
        if (c->node->idx <= n->idx || c->node->idx > n->subtree_end) continue;
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
        if (c->t != C_MARK && c->t != C_TEXT && c->t != C_IMG && c->t != C_CTRL) continue;
        if (!c->node || (c->t == C_TEXT && c->node->parent < 0)) continue;
        if (mx < f->cx0 || mx >= f->cx1 || my < f->cy0 || my >= f->cy1) continue;
        float x0 = c->x + f->dx, y0 = c->y + f->dy;
        if (mx >= x0 && mx < x0 + c->w && my >= y0 && my < y0 + c->h) best[k] = c;
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
        if (f->scroller && nc[k] < 64 && mx >= f->cx0 && mx < f->cx1 && my >= f->cy0 && my < f->cy1) { cand[k][nc[k]].n = f->scroller; cand[k][nc[k]].fl = f->scroller_fl; nc[k]++; }
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
