/* Растровые эффекты поверх слоя элемента: filter, backdrop-filter, clip-path, mask, аффинное преобразование.
   Слой хранится как премультиплицированный RGBA8. Его получают двумя проходами отрисовки (на чёрном и на белом фоне):
   для канала c: A = c·α (на чёрном), B = c·α + 255·(1−α) (на белом) → α = 1 − (B−A)/255, цвет·α = A. */
#include "fx.h"
#include "webpage.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint8_t u8(float v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }

FxImg* fx_img_new(int x0, int y0, int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    FxImg* im = calloc(1, sizeof *im); if (!im) return NULL;
    im->px = calloc((size_t)w * (size_t)h, 4); if (!im->px) { free(im); return NULL; }
    im->x0 = x0; im->y0 = y0; im->w = w; im->h = h; return im;
}
void fx_img_free(FxImg* im) { if (!im) return; free(im->px); free(im); }

void fx_img_from_passes(FxImg* im, const uint32_t* black, const uint32_t* white, int stride) {
    for (int y = 0; y < im->h; y++) {
        const uint32_t* a = black + (size_t)(im->y0 + y) * stride + im->x0; const uint32_t* b = white + (size_t)(im->y0 + y) * stride + im->x0;
        uint8_t* o = im->px + (size_t)y * im->w * 4;
        for (int x = 0; x < im->w; x++, o += 4) {
            int ar = (a[x] >> 16) & 255, ag = (a[x] >> 8) & 255, ab = a[x] & 255, br = (b[x] >> 16) & 255, bg = (b[x] >> 8) & 255, bb = b[x] & 255;
            int d = ((br - ar) + (bg - ag) + (bb - ab)) / 3; int al = 255 - d; if (al < 0) al = 0; if (al > 255) al = 255;
            if (al < 2) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            o[0] = (uint8_t)(ar > al ? al : ar); o[1] = (uint8_t)(ag > al ? al : ag); o[2] = (uint8_t)(ab > al ? al : ab); o[3] = (uint8_t)al;
        }
    }
}
void fx_img_from_buffer(FxImg* im, const uint32_t* buf, int stride) {
    for (int y = 0; y < im->h; y++) {
        const uint32_t* a = buf + (size_t)(im->y0 + y) * stride + im->x0; uint8_t* o = im->px + (size_t)y * im->w * 4;
        for (int x = 0; x < im->w; x++, o += 4) { o[0] = (a[x] >> 16) & 255; o[1] = (a[x] >> 8) & 255; o[2] = a[x] & 255; o[3] = 255; }
    }
}

/* ══════════════ фильтры ══════════════ */
static void box_pass(const uint8_t* src, uint8_t* dst, int w, int h, int r, int horiz) {
    int n = horiz ? w : h, lines = horiz ? h : w; int sx = horiz ? 4 : w * 4, sl = horiz ? w * 4 : 4; float inv = 1.0f / (2 * r + 1);
    for (int l = 0; l < lines; l++) {
        const uint8_t* s = src + (size_t)l * sl; uint8_t* d = dst + (size_t)l * sl; int acc[4] = {0, 0, 0, 0};
        for (int i = 0; i <= r && i < n; i++) for (int c = 0; c < 4; c++) acc[c] += s[(size_t)i * sx + c];
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 4; c++) d[(size_t)i * sx + c] = (uint8_t)(acc[c] * inv + 0.5f);
            int add = i + r + 1, sub = i - r;
            if (add < n) for (int c = 0; c < 4; c++) acc[c] += s[(size_t)add * sx + c];
            if (sub >= 0) for (int c = 0; c < 4; c++) acc[c] -= s[(size_t)sub * sx + c];
        }
    }
}
static void blur_px(uint8_t* px, int w, int h, float sigma) {
    if (sigma < 0.3f || w < 1 || h < 1) return;
    int n = 3; float wi = sqrtf(12.0f * sigma * sigma / n + 1); int wl = (int)floorf(wi); if (!(wl & 1)) wl--; if (wl < 1) wl = 1; int wu = wl + 2;
    float mi = (12.0f * sigma * sigma - n * wl * wl - 4.0f * n * wl - 3.0f * n) / (-4.0f * wl - 4.0f); int m = (int)(mi + 0.5f);
    uint8_t* tmp = malloc((size_t)w * h * 4); if (!tmp) return;
    for (int i = 0; i < n; i++) { int size = i < m ? wl : wu; int r = (size - 1) / 2; if (r < 1) r = size > 1 ? 1 : 0; if (r < 1) continue; box_pass(px, tmp, w, h, r, 1); box_pass(tmp, px, w, h, r, 0); }
    free(tmp);
}
int fx_filter_pad(const FxList* fl) {
    int pad = 0;
    for (int i = 0; i < fl->n; i++) {
        const FxOp* o = &fl->op[i];
        if (o->kind == FXO_BLUR) pad += (int)ceilf(o->v * 3) + 2;
        else if (o->kind == FXO_SHADOW) pad += (int)(fabsf(o->x) > fabsf(o->y) ? fabsf(o->x) : fabsf(o->y)) + (int)ceilf(o->blur * 3) + 2;
    }
    return pad;
}
static void apply_matrix(FxImg* im, const float m[9], float scale, float bias) {      /* c' = M·c (в непремультиплицированном виде) */
    uint8_t* p = im->px; size_t n = (size_t)im->w * im->h;
    for (size_t i = 0; i < n; i++, p += 4) {
        if (!p[3]) continue; float a = p[3] / 255.0f, r = p[0] / 255.0f / a, g = p[1] / 255.0f / a, b = p[2] / 255.0f / a;
        float R = clampf(m[0] * r + m[1] * g + m[2] * b, 0, 1), G = clampf(m[3] * r + m[4] * g + m[5] * b, 0, 1), B = clampf(m[6] * r + m[7] * g + m[8] * b, 0, 1);
        R = clampf(R * scale + bias, 0, 1); G = clampf(G * scale + bias, 0, 1); B = clampf(B * scale + bias, 0, 1);
        p[0] = u8(R * a * 255); p[1] = u8(G * a * 255); p[2] = u8(B * a * 255);
    }
}
static void apply_invert(FxImg* im, float amt) {
    uint8_t* p = im->px; size_t n = (size_t)im->w * im->h;
    for (size_t i = 0; i < n; i++, p += 4) {
        if (!p[3]) continue; float a = p[3] / 255.0f;
        for (int c = 0; c < 3; c++) { float v = p[c] / 255.0f / a; v = amt + v * (1 - 2 * amt); p[c] = u8(clampf(v, 0, 1) * a * 255); }
    }
}
static void apply_contrast(FxImg* im, float k) {
    uint8_t* p = im->px; size_t n = (size_t)im->w * im->h;
    for (size_t i = 0; i < n; i++, p += 4) {
        if (!p[3]) continue; float a = p[3] / 255.0f;
        for (int c = 0; c < 3; c++) { float v = p[c] / 255.0f / a; v = (v - 0.5f) * k + 0.5f; p[c] = u8(clampf(v, 0, 1) * a * 255); }
    }
}
static void apply_opacity(FxImg* im, float k) {
    uint8_t* p = im->px; size_t n = (size_t)im->w * im->h; k = clampf(k, 0, 1);
    for (size_t i = 0; i < n; i++, p += 4) { p[0] = u8(p[0] * k); p[1] = u8(p[1] * k); p[2] = u8(p[2] * k); p[3] = u8(p[3] * k); }
}
static void apply_shadow(FxImg* im, const FxOp* o) {
    FxImg* sh = fx_img_new(im->x0, im->y0, im->w, im->h); if (!sh) return;
    int dx = (int)lroundf(o->x), dy = (int)lroundf(o->y); int sa = o->color >> 24, sr = (o->color >> 16) & 255, sg = (o->color >> 8) & 255, sb = o->color & 255;
    for (int y = 0; y < im->h; y++) { int sy = y - dy; if (sy < 0 || sy >= im->h) continue;
        for (int x = 0; x < im->w; x++) { int sx = x - dx; if (sx < 0 || sx >= im->w) continue; uint8_t* d = sh->px + ((size_t)y * im->w + x) * 4; int a = im->px[((size_t)sy * im->w + sx) * 4 + 3];
            a = a * sa / 255; d[0] = (uint8_t)(sr * a / 255); d[1] = (uint8_t)(sg * a / 255); d[2] = (uint8_t)(sb * a / 255); d[3] = (uint8_t)a; } }
    blur_px(sh->px, sh->w, sh->h, o->blur * 0.5f);
    uint8_t* p = im->px; uint8_t* q = sh->px; size_t n = (size_t)im->w * im->h;                  /* изображение поверх тени */
    for (size_t i = 0; i < n; i++, p += 4, q += 4) { int ia = 255 - p[3]; for (int c = 0; c < 4; c++) p[c] = (uint8_t)(p[c] + (q[c] * ia + 127) / 255); }
    fx_img_free(sh);
}
void fx_apply_filters(FxImg* im, const FxList* fl) {
    for (int i = 0; i < fl->n; i++) {
        const FxOp* o = &fl->op[i]; float v = o->v;
        switch (o->kind) {
        case FXO_BLUR: blur_px(im->px, im->w, im->h, v * 0.5f > 0.3f ? v : v); break;
        case FXO_BRIGHT: { float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; apply_matrix(im, m, v, 0); break; }
        case FXO_CONTRAST: apply_contrast(im, v); break;
        case FXO_GRAY: { float a = clampf(1 - v, 0, 1); float m[9] = {0.2126f + 0.7874f * a, 0.7152f - 0.7152f * a, 0.0722f - 0.0722f * a, 0.2126f - 0.2126f * a, 0.7152f + 0.2848f * a, 0.0722f - 0.0722f * a, 0.2126f - 0.2126f * a, 0.7152f - 0.7152f * a, 0.0722f + 0.9278f * a}; apply_matrix(im, m, 1, 0); break; }
        case FXO_SEPIA: { float a = clampf(1 - v, 0, 1); float m[9] = {0.393f + 0.607f * a, 0.769f - 0.769f * a, 0.189f - 0.189f * a, 0.349f - 0.349f * a, 0.686f + 0.314f * a, 0.168f - 0.168f * a, 0.272f - 0.272f * a, 0.534f - 0.534f * a, 0.131f + 0.869f * a}; apply_matrix(im, m, 1, 0); break; }
        case FXO_SAT: { float s = v; float m[9] = {0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s, 0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s}; apply_matrix(im, m, 1, 0); break; }
        case FXO_HUE: { float r = v * 0.0174532925f, cs = cosf(r), sn = sinf(r);
            float m[9] = {0.213f + cs * 0.787f - sn * 0.213f, 0.715f - cs * 0.715f - sn * 0.715f, 0.072f - cs * 0.072f + sn * 0.928f, 0.213f - cs * 0.213f + sn * 0.143f, 0.715f + cs * 0.285f + sn * 0.140f, 0.072f - cs * 0.072f - sn * 0.283f, 0.213f - cs * 0.213f - sn * 0.787f, 0.715f - cs * 0.715f + sn * 0.715f, 0.072f + cs * 0.928f + sn * 0.072f}; apply_matrix(im, m, 1, 0); break; }
        case FXO_INVERT: apply_invert(im, clampf(v, 0, 1)); break;
        case FXO_OPACITY: apply_opacity(im, v); break;
        case FXO_SHADOW: apply_shadow(im, o); break;
        }
    }
}

/* ══════════════ clip-path / mask ══════════════ */
void fx_len_resolve(const FxLen* l, float base, float* out) { *out = l->px + l->pct * base / 100.0f; }

static float rr_cover(float px, float py, float x, float y, float w, float h, const float r[4]) {
    float cx, cy, rad = 0;
    if (px < x + r[0] && py < y + r[0]) { cx = x + r[0]; cy = y + r[0]; rad = r[0]; }
    else if (px > x + w - r[1] && py < y + r[1]) { cx = x + w - r[1]; cy = y + r[1]; rad = r[1]; }
    else if (px > x + w - r[2] && py > y + h - r[2]) { cx = x + w - r[2]; cy = y + h - r[2]; rad = r[2]; }
    else if (px < x + r[3] && py > y + h - r[3]) { cx = x + r[3]; cy = y + h - r[3]; rad = r[3]; }
    else { float dx = fminf(px - x, x + w - px), dy = fminf(py - y, y + h - py); return clampf(fminf(dx, dy) + 0.5f, 0, 1); }
    return clampf(0.5f - (hypotf(px - cx, py - cy) - rad), 0, 1);
}
static int pt_in_poly(const float* xs, const float* ys, int n, float px, float py, int evenodd) {
    int wn = 0, cn = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float x1 = xs[j], y1 = ys[j], x2 = xs[i], y2 = ys[i];
        if ((y1 <= py) != (y2 <= py)) { float xi = x1 + (py - y1) / (y2 - y1) * (x2 - x1); if (px < xi) { cn ^= 1; wn += y2 > y1 ? 1 : -1; } }
    }
    return evenodd ? cn : wn != 0;
}
void fx_apply_clip(FxImg* im, const FxClip* cp, float bx, float by, float bw, float bh) {
    if (!cp->kind) return;
    float v[4][2]; (void)v;
    float xs[16], ys[16]; int n = cp->n;
    float ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0, rr[4] = {0, 0, 0, 0}, ccx = 0, ccy = 0, rx = 0, ry = 0;
    if (cp->kind == CP_INSET) {
        float t, r, b, l; fx_len_resolve(&cp->a[0], bh, &t); fx_len_resolve(&cp->a[1], bw, &r); fx_len_resolve(&cp->a[2], bh, &b); fx_len_resolve(&cp->a[3], bw, &l);
        ix0 = bx + l; iy0 = by + t; ix1 = bx + bw - r; iy1 = by + bh - b; float w2 = ix1 - ix0, h2 = iy1 - iy0, m = fminf(w2, h2) * 0.5f;
        for (int i = 0; i < 4; i++) rr[i] = cp->rad[i] > m ? (m > 0 ? m : 0) : cp->rad[i];
        if (w2 <= 0 || h2 <= 0) { memset(im->px, 0, (size_t)im->w * im->h * 4); return; }
    } else if (cp->kind == CP_CIRCLE || cp->kind == CP_ELLIPSE) {
        int ell = cp->kind == CP_ELLIPSE; const FxLen* pc = ell ? &cp->a[2] : &cp->a[1]; const FxLen* pd = ell ? &cp->a[3] : &cp->a[2];
        fx_len_resolve(pc, bw, &ccx); fx_len_resolve(pd, bh, &ccy); ccx += bx; ccy += by;
        float dl = ccx - bx, dr = bx + bw - ccx, dt = ccy - by, db = by + bh - ccy;
        float cs_x = fminf(dl, dr), cs_y = fminf(dt, db), fs_x = fmaxf(dl, dr), fs_y = fmaxf(dt, db);
        const FxLen* ra = &cp->a[0]; const FxLen* rb = ell ? &cp->a[1] : &cp->a[0];
        if (!ell) { float cs = fminf(cs_x, cs_y), fs = fmaxf(fs_x, fs_y); if (ra->px == -1 && ra->pct == 0) rx = cs; else if (ra->px == -2 && ra->pct == 0) rx = fs; else { float ref = sqrtf((bw * bw + bh * bh) * 0.5f); rx = ra->px + ra->pct * ref / 100.0f; } ry = rx; }
        else {
            if (ra->px == -1 && ra->pct == 0) rx = cs_x; else if (ra->px == -2 && ra->pct == 0) rx = fs_x; else rx = ra->px + ra->pct * bw / 100.0f;
            if (rb->px == -1 && rb->pct == 0) ry = cs_y; else if (rb->px == -2 && rb->pct == 0) ry = fs_y; else ry = rb->px + rb->pct * bh / 100.0f;
        }
        if (rx < 0.01f) rx = 0.01f; if (ry < 0.01f) ry = 0.01f;
    } else if (cp->kind == CP_POLYGON) {
        for (int i = 0; i < n; i++) { float px, py; fx_len_resolve(&cp->pt[i][0], bw, &px); fx_len_resolve(&cp->pt[i][1], bh, &py); xs[i] = bx + px; ys[i] = by + py; }
    }
    for (int y = 0; y < im->h; y++) {
        uint8_t* p = im->px + (size_t)y * im->w * 4; float fy = im->y0 + y + 0.5f;
        for (int x = 0; x < im->w; x++, p += 4) {
            if (!p[3]) continue; float fx = im->x0 + x + 0.5f, cov = 0;
            switch (cp->kind) {
            case CP_INSET: cov = (fx >= ix0 - 1 && fx <= ix1 + 1 && fy >= iy0 - 1 && fy <= iy1 + 1) ? rr_cover(fx, fy, ix0, iy0, ix1 - ix0, iy1 - iy0, rr) : 0; break;
            case CP_CIRCLE: case CP_ELLIPSE: { float dx = (fx - ccx) / rx, dy = (fy - ccy) / ry; float d = (sqrtf(dx * dx + dy * dy) - 1.0f) * fminf(rx, ry); cov = clampf(0.5f - d, 0, 1); break; }
            case CP_POLYGON: { int hit = 0; for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) hit += pt_in_poly(xs, ys, n, fx - 0.25f + sx * 0.5f, fy - 0.25f + sy * 0.5f, cp->evenodd); cov = hit / 4.0f; break; }
            }
            if (cov >= 0.999f) continue;
            if (cov <= 0.001f) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
            p[0] = u8(p[0] * cov); p[1] = u8(p[1] * cov); p[2] = u8(p[2] * cov); p[3] = u8(p[3] * cov);
        }
    }
}

static float mask_alpha_at(const FxMask* m, float t) {
    if (t <= m->p[0]) return m->a[0] / 255.0f; if (t >= m->p[m->n - 1]) return m->a[m->n - 1] / 255.0f;
    for (int i = 0; i < m->n - 1; i++) if (t >= m->p[i] && t <= m->p[i + 1]) { float f = m->p[i + 1] > m->p[i] ? (t - m->p[i]) / (m->p[i + 1] - m->p[i]) : 0; return (m->a[i] + (m->a[i + 1] - m->a[i]) * f) / 255.0f; }
    return m->a[m->n - 1] / 255.0f;
}
void fx_apply_mask(FxImg* im, const FxMask* mk, float bx, float by, float bw, float bh) {
    if (!mk->kind) return;
    float tw = bw, th = bh; ImgPix* img = NULL; ImgPix* src = NULL;
    if (mk->kind == MK_IMAGE) { src = webpage_image_get(mk->url); if (!src) { memset(im->px, 0, (size_t)im->w * im->h * 4); return; } tw = (float)src->w; th = (float)src->h; }
    if (mk->size_mode == 1 || mk->size_mode == 2) { float s = mk->size_mode == 1 ? fmaxf(bw / tw, bh / th) : fminf(bw / tw, bh / th); tw *= s; th *= s; }
    else if (mk->size_mode == 3) {
        float lw = mk->w.set ? mk->w.px + mk->w.pct * bw / 100.0f : -1, lh = mk->h.set ? mk->h.px + mk->h.pct * bh / 100.0f : -1;
        if (lw >= 0 && lh >= 0) { tw = lw; th = lh; } else if (lw >= 0) { th = th * lw / tw; tw = lw; } else if (lh >= 0) { tw = tw * lh / th; th = lh; }
    }
    if (tw < 1 || th < 1) { memset(im->px, 0, (size_t)im->w * im->h * 4); return; }
    if (mk->kind == MK_IMAGE) { int itw = (int)(tw + 0.5f), ith = (int)(th + 0.5f); img = (itw == src->w && ith == src->h) ? src : webpage_image_scaled(mk->url, itw, ith); if (!img) img = src; }
    float ox = mk->px.set ? mk->px.px + mk->px.pct * (bw - tw) / 100.0f : 0, oy = mk->py.set ? mk->py.px + mk->py.pct * (bh - th) / 100.0f : 0;
    int rx = mk->rep == 0 || mk->rep == 1, ry = mk->rep == 0 || mk->rep == 2;
    float ang = mk->angle * 3.14159265f / 180.0f, dirx = sinf(ang), diry = -cosf(ang); float half = fabsf(tw * 0.5f * dirx) + fabsf(th * 0.5f * diry); if (half < 1) half = 1;
    float maxr = sqrtf(tw * tw * 0.25f + th * th * 0.25f); if (maxr < 1) maxr = 1;
    for (int y = 0; y < im->h; y++) {
        uint8_t* p = im->px + (size_t)y * im->w * 4; float ly = im->y0 + y + 0.5f - by - oy;
        for (int x = 0; x < im->w; x++, p += 4) {
            if (!p[3]) continue; float lx = im->x0 + x + 0.5f - bx - ox; float a;
            float ux = lx, uy = ly;
            if (rx) { ux = fmodf(lx, tw); if (ux < 0) ux += tw; } else if (lx < 0 || lx >= tw) { a = 0; goto done; }
            if (ry) { uy = fmodf(ly, th); if (uy < 0) uy += th; } else if (ly < 0 || ly >= th) { a = 0; goto done; }
            if (mk->kind == MK_LINEAR) { float px = ux - tw * 0.5f, py = uy - th * 0.5f; float t = ((px * dirx + py * diry) / half * 0.5f + 0.5f) * 100.0f; a = mask_alpha_at(mk, t); }
            else if (mk->kind == MK_RADIAL) { float px = ux - tw * 0.5f, py = uy - th * 0.5f; a = mask_alpha_at(mk, sqrtf(px * px + py * py) / maxr * 100.0f); }
            else { int ix = (int)ux, iy = (int)uy; if (img->w != (int)(tw + 0.5f)) { ix = (int)(ux / tw * img->w); iy = (int)(uy / th * img->h); } if (ix >= img->w) ix = img->w - 1; if (iy >= img->h) iy = img->h - 1; a = (img->px[(size_t)iy * img->w + ix] >> 24) / 255.0f; }
        done:
            if (a >= 0.999f) continue;
            p[0] = u8(p[0] * a); p[1] = u8(p[1] * a); p[2] = u8(p[2] * a); p[3] = u8(p[3] * a);
        }
    }
}

/* ══════════════ transform ══════════════ */
static void mmul(const float a[6], const float b[6], float o[6]) {        /* o = a·b (affine 2x3, строки) */
    o[0] = a[0] * b[0] + a[2] * b[1]; o[1] = a[1] * b[0] + a[3] * b[1];
    o[2] = a[0] * b[2] + a[2] * b[3]; o[3] = a[1] * b[2] + a[3] * b[3];
    o[4] = a[0] * b[4] + a[2] * b[5] + a[4]; o[5] = a[1] * b[4] + a[3] * b[5] + a[5];
}
int fx_xform_compute(const FxXform* x, float bx, float by, float bw, float bh, float m[6], float* tx, float* ty) {
    float M[6] = {1, 0, 0, 1, 0, 0}; int linear = 0;
    float ox = x->ox.px + x->ox.pct * bw / 100.0f, oy = x->oy.px + x->oy.pct * bh / 100.0f;
    for (int i = 0; i < x->n; i++) {
        const FxXfOp* o = &x->op[i]; float O[6] = {1, 0, 0, 1, 0, 0}, t;
        switch (o->kind) {
        case XF_TRANSLATE: O[4] = o->t[0].px + o->t[0].pct * bw / 100.0f; O[5] = o->t[1].px + o->t[1].pct * bh / 100.0f; break;
        case XF_SCALE: O[0] = o->v[0]; O[3] = o->v[1]; linear = 1; break;
        case XF_ROTATE: t = o->v[0] * 0.0174532925f; O[0] = cosf(t); O[1] = sinf(t); O[2] = -sinf(t); O[3] = cosf(t); linear = 1; break;
        case XF_SKEWX: O[2] = tanf(o->v[0] * 0.0174532925f); linear = 1; break;
        case XF_SKEWY: O[1] = tanf(o->v[0] * 0.0174532925f); linear = 1; break;
        case XF_MATRIX: O[0] = o->v[0]; O[1] = o->v[1]; O[2] = o->v[2]; O[3] = o->v[3]; O[4] = o->v[4]; O[5] = o->v[5]; linear = 1; break;
        }
        float R[6]; mmul(M, O, R); memcpy(M, R, sizeof M);
    }
    if (!linear) { *tx = M[4]; *ty = M[5]; return 0; }
    float T1[6] = {1, 0, 0, 1, bx + ox, by + oy}, T2[6] = {1, 0, 0, 1, -(bx + ox), -(by + oy)}, A[6], B[6];
    mmul(T1, M, A); mmul(A, T2, B); memcpy(m, B, sizeof B); return 1;
}

/* ══════════════ вывод ══════════════ */
void fx_composite(const FxImg* im, uint32_t* dst, int stride, int cx0, int cy0, int cx1, int cy1, const float* m) {
    int x0 = im->x0, y0 = im->y0, x1 = im->x0 + im->w, y1 = im->y0 + im->h;
    if (!m) {
        if (x0 < cx0) x0 = cx0; if (y0 < cy0) y0 = cy0; if (x1 > cx1) x1 = cx1; if (y1 > cy1) y1 = cy1;
        for (int y = y0; y < y1; y++) { const uint8_t* s = im->px + ((size_t)(y - im->y0) * im->w + (x0 - im->x0)) * 4; uint32_t* d = dst + (size_t)y * stride + x0;
            for (int x = x0; x < x1; x++, s += 4, d++) { if (!s[3]) continue; int ia = 255 - s[3]; uint32_t o = *d;
                *d = ((uint32_t)(s[0] + (((o >> 16) & 255) * ia + 127) / 255) << 16) | ((uint32_t)(s[1] + (((o >> 8) & 255) * ia + 127) / 255) << 8) | (uint32_t)(s[2] + ((o & 255) * ia + 127) / 255); } }
        return;
    }
    float det = m[0] * m[3] - m[1] * m[2]; if (fabsf(det) < 1e-6f) return;
    float inv[6] = {m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, 0, 0}; inv[4] = -(inv[0] * m[4] + inv[2] * m[5]); inv[5] = -(inv[1] * m[4] + inv[3] * m[5]);
    float cxs[4] = {(float)im->x0, (float)x1, (float)im->x0, (float)x1}, cys[4] = {(float)im->y0, (float)im->y0, (float)y1, (float)y1}; float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    for (int i = 0; i < 4; i++) { float X = m[0] * cxs[i] + m[2] * cys[i] + m[4], Y = m[1] * cxs[i] + m[3] * cys[i] + m[5]; if (X < mnx) mnx = X; if (X > mxx) mxx = X; if (Y < mny) mny = Y; if (Y > mxy) mxy = Y; }
    int bx0 = (int)floorf(mnx) - 1, by0 = (int)floorf(mny) - 1, bx1 = (int)ceilf(mxx) + 1, by1 = (int)ceilf(mxy) + 1;
    if (bx0 < cx0) bx0 = cx0; if (by0 < cy0) by0 = cy0; if (bx1 > cx1) bx1 = cx1; if (by1 > cy1) by1 = cy1;
    for (int y = by0; y < by1; y++) for (int x = bx0; x < bx1; x++) {
        float sx = inv[0] * (x + 0.5f) + inv[2] * (y + 0.5f) + inv[4] - im->x0 - 0.5f, sy = inv[1] * (x + 0.5f) + inv[3] * (y + 0.5f) + inv[5] - im->y0 - 0.5f;
        int ix = (int)floorf(sx), iy = (int)floorf(sy); float fx = sx - ix, fy = sy - iy; float acc[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; k++) {
            int qx = ix + (k & 1), qy = iy + (k >> 1); float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
            if (qx < 0 || qy < 0 || qx >= im->w || qy >= im->h) continue; const uint8_t* s = im->px + ((size_t)qy * im->w + qx) * 4;
            for (int c = 0; c < 4; c++) acc[c] += s[c] * wgt;
        }
        if (acc[3] < 0.5f) continue; int a = (int)(acc[3] + 0.5f), ia = 255 - a; uint32_t* d = dst + (size_t)y * stride + x; uint32_t o = *d;
        int r = (int)(acc[0] + 0.5f) + (int)(((o >> 16) & 255) * ia + 127) / 255, g = (int)(acc[1] + 0.5f) + (int)(((o >> 8) & 255) * ia + 127) / 255, b = (int)(acc[2] + 0.5f) + (int)((o & 255) * ia + 127) / 255;
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; *d = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

void fx_writeback(const FxImg* im, uint32_t* dst, int stride, int cx0, int cy0, int cx1, int cy1, float bx, float by, float bw, float bh, const float rad[4]) {
    int x0 = (int)floorf(bx), y0 = (int)floorf(by), x1 = (int)ceilf(bx + bw), y1 = (int)ceilf(by + bh);
    if (x0 < cx0) x0 = cx0; if (y0 < cy0) y0 = cy0; if (x1 > cx1) x1 = cx1; if (y1 > cy1) y1 = cy1;
    if (x0 < im->x0) x0 = im->x0; if (y0 < im->y0) y0 = im->y0; if (x1 > im->x0 + im->w) x1 = im->x0 + im->w; if (y1 > im->y0 + im->h) y1 = im->y0 + im->h;
    int rounded = rad[0] + rad[1] + rad[2] + rad[3] >= 0.5f;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float cov = rounded ? rr_cover(x + 0.5f, y + 0.5f, bx, by, bw, bh, rad) : 1.0f; if (cov <= 0) continue;
        const uint8_t* s = im->px + ((size_t)(y - im->y0) * im->w + (x - im->x0)) * 4; uint32_t* d = dst + (size_t)y * stride + x; uint32_t o = *d;
        float a = s[3] / 255.0f * cov; int ia = 255 - (int)(a * 255 + 0.5f);
        int r = (int)(s[0] * cov) + (int)(((o >> 16) & 255) * ia + 127) / 255 + (s[3] < 255 ? 0 : 0), g = (int)(s[1] * cov) + (int)(((o >> 8) & 255) * ia + 127) / 255, b = (int)(s[2] * cov) + (int)((o & 255) * ia + 127) / 255;
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; *d = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
