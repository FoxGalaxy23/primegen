#include "ttf_render.h"
#include "gal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if !defined(TTF_NO_WOFF) && defined(__has_include)
#  if __has_include(<zlib.h>)
#    include <zlib.h>
#    define TTF_HAVE_WOFF 1
#  endif
#endif

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define MAX_FONT_FILE   (128u * 1024u * 1024u)
#define MAX_GLYPH_SIDE  1024
#define MAX_STRIKE_GLYPHS 6000

/* ══════════════════════════════════════════════════════════════════
   Безопасное чтение big-endian данных из памяти.
   Любой выход за границы даёт 0 — битый шрифт не может уронить браузер.
   ══════════════════════════════════════════════════════════════════ */
static inline int in_range(const TTFFont* f, size_t off, size_t len) {
    return off <= f->data_len && len <= f->data_len - off;
}
static inline uint8_t rd8(const TTFFont* f, size_t off) {
    return in_range(f, off, 1) ? f->data[off] : 0;
}
static inline uint16_t rd16(const TTFFont* f, size_t off) {
    if (!in_range(f, off, 2)) return 0;
    return (uint16_t)((f->data[off] << 8) | f->data[off + 1]);
}
static inline int16_t rds16(const TTFFont* f, size_t off) { return (int16_t)rd16(f, off); }
static inline uint32_t rd32(const TTFFont* f, size_t off) {
    if (!in_range(f, off, 4)) return 0;
    return ((uint32_t)f->data[off] << 24) | ((uint32_t)f->data[off + 1] << 16) |
           ((uint32_t)f->data[off + 2] << 8) | f->data[off + 3];
}

/* ══════════════════════════════════════════════════════════════════
   WOFF 1 -> sfnt (zlib уже линкуется в проект)
   ══════════════════════════════════════════════════════════════════ */
#ifdef TTF_HAVE_WOFF
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(uint8_t* p, uint32_t v) { p[0] = v >> 8; p[1] = v; }

static uint8_t* woff_to_sfnt(const uint8_t* in, size_t in_len, size_t* out_len) {
    if (in_len < 44) return NULL;
    uint32_t flavor = be32(in + 4);
    unsigned nt = be16(in + 12);
    if (nt == 0 || nt > 256 || in_len < 44 + (size_t)nt * 20) return NULL;

    size_t total = 12 + (size_t)nt * 16;
    for (unsigned i = 0; i < nt; i++) {
        uint32_t orig = be32(in + 44 + i * 20 + 12);
        if (orig > MAX_FONT_FILE) return NULL;
        total += ((size_t)orig + 3) & ~(size_t)3;
        if (total > MAX_FONT_FILE) return NULL;
    }
    uint8_t* out = calloc(1, total);
    if (!out) return NULL;

    unsigned pow2 = 1, log2v = 0;
    while (pow2 * 2 <= nt) { pow2 *= 2; log2v++; }
    put32(out, flavor);
    put16(out + 4, nt);
    put16(out + 6, pow2 * 16);
    put16(out + 8, log2v);
    put16(out + 10, nt * 16 - pow2 * 16);

    size_t pos = 12 + (size_t)nt * 16;
    for (unsigned i = 0; i < nt; i++) {
        const uint8_t* e = in + 44 + i * 20;
        uint32_t off = be32(e + 4), comp = be32(e + 8), orig = be32(e + 12);
        if (off > in_len || comp > in_len - off) { free(out); return NULL; }
        memcpy(out + 12 + i * 16, e, 4);               /* tag */
        put32(out + 12 + i * 16 + 4, be32(e + 16));    /* checksum */
        put32(out + 12 + i * 16 + 8, (uint32_t)pos);
        put32(out + 12 + i * 16 + 12, orig);
        if (comp == orig) {
            memcpy(out + pos, in + off, orig);
        } else {
            uLongf dl = orig;
            if (uncompress(out + pos, &dl, in + off, comp) != Z_OK || dl != orig) { free(out); return NULL; }
        }
        pos += ((size_t)orig + 3) & ~(size_t)3;
    }
    *out_len = total;
    return out;
}
#endif

/* ══════════════════════════════════════════════════════════════════
   cmap: кодпоинт -> индекс глифа (форматы 4 и 12, бинарный поиск)
   ══════════════════════════════════════════════════════════════════ */
static uint16_t cmap4_lookup(const TTFFont* f, uint32_t sub, uint32_t cp) {
    if (cp > 0xFFFF) return 0;
    int seg = rd16(f, sub + 6) / 2;
    if (seg <= 0) return 0;
    uint32_t endc = sub + 14, startc = endc + seg * 2 + 2, delta = startc + seg * 2, rofs = delta + seg * 2;
    int lo = 0, hi = seg - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (rd16(f, endc + mid * 2) < cp) lo = mid + 1; else hi = mid;
    }
    if (rd16(f, endc + lo * 2) < cp) return 0;
    uint32_t start = rd16(f, startc + lo * 2);
    if (cp < start) return 0;
    uint32_t ro = rd16(f, rofs + lo * 2), d = rd16(f, delta + lo * 2);
    if (ro == 0) return (uint16_t)((cp + d) & 0xFFFF);
    uint16_t g = rd16(f, rofs + lo * 2 + ro + (cp - start) * 2);
    return g ? (uint16_t)((g + d) & 0xFFFF) : 0;
}

static uint16_t cmap12_lookup(const TTFFont* f, uint32_t sub, uint32_t cp) {
    uint32_t n = rd32(f, sub + 12);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, g = sub + 16 + mid * 12;
        if (cp < rd32(f, g)) hi = mid;
        else if (cp > rd32(f, g + 4)) lo = mid + 1;
        else { uint32_t gid = rd32(f, g + 8) + (cp - rd32(f, g)); return gid < 0x10000 ? (uint16_t)gid : 0; }
    }
    return 0;
}

static uint16_t glyph_index(TTFFont* f, uint32_t cp) {
    if (cp < 128) return f->ascii_gid[cp];
    uint32_t slot = (cp * 2654435761u) >> 23;                 /* 512 ячеек */
    if (f->cp_key[slot] == cp + 1) return f->cp_gid[slot];
    uint16_t g = f->cmap_fmt == 12 ? cmap12_lookup(f, f->cmap_sub, cp) : cmap4_lookup(f, f->cmap_sub, cp);
    if (g >= f->num_glyphs) g = 0;
    f->cp_key[slot] = cp + 1; f->cp_gid[slot] = g;
    return g;
}

static int pick_cmap(TTFFont* f, uint32_t cmap) {
    int n = rd16(f, cmap + 2), best = -1;
    for (int i = 0; i < n; i++) {
        uint32_t rec = cmap + 4 + i * 8;
        int pid = rd16(f, rec), eid = rd16(f, rec + 2);
        uint32_t sub = cmap + rd32(f, rec + 4);
        int fmt = rd16(f, sub), score = -1;
        if (fmt != 4 && fmt != 12) continue;
        if (pid == 3 && eid == 10) score = 100;
        else if (pid == 0 && fmt == 12) score = 90;
        else if (pid == 3 && eid == 1) score = 80;
        else if (pid == 0) score = 70;
        if (score > best) { best = score; f->cmap_sub = sub; f->cmap_fmt = fmt; }
    }
    return best >= 0;
}

/* ══════════════════════════════════════════════════════════════════
   Контуры глифов (simple + composite)
   ══════════════════════════════════════════════════════════════════ */
typedef struct {
    float* x; float* y; uint8_t* on;
    int n, cap;
    int* ends; int nc, ccap;
} Outline;

static void ol_free(Outline* o) { free(o->x); free(o->y); free(o->on); free(o->ends); memset(o, 0, sizeof *o); }

static int ol_reserve(Outline* o, int extra_pts, int extra_contours) {
    if (o->n + extra_pts > o->cap) {
        int nc = (o->n + extra_pts) * 2 + 16;
        float* nx = realloc(o->x, nc * sizeof(float)); if (!nx) return 0; o->x = nx;
        float* ny = realloc(o->y, nc * sizeof(float)); if (!ny) return 0; o->y = ny;
        uint8_t* no = realloc(o->on, nc);              if (!no) return 0; o->on = no;
        o->cap = nc;
    }
    if (o->nc + extra_contours > o->ccap) {
        int nc = (o->nc + extra_contours) * 2 + 4;
        int* ne = realloc(o->ends, nc * sizeof(int)); if (!ne) return 0; o->ends = ne;
        o->ccap = nc;
    }
    return 1;
}

/* Диапазон байт глифа внутри glyf. Возвращает 0 для несуществующего/пустого. */
static int glyph_range(const TTFFont* f, int gid, uint32_t* start, uint32_t* end) {
    if (gid < 0 || gid >= f->num_glyphs) return 0;
    uint32_t a, b;
    if (f->loca_format == 0) {
        a = (uint32_t)rd16(f, f->loca_off + gid * 2) * 2;
        b = (uint32_t)rd16(f, f->loca_off + gid * 2 + 2) * 2;
    } else {
        a = rd32(f, f->loca_off + gid * 4);
        b = rd32(f, f->loca_off + gid * 4 + 4);
    }
    if (b <= a || b > f->glyf_len) return 0;
    *start = f->glyf_off + a; *end = f->glyf_off + b;
    return 1;
}

static float f2dot14(int16_t v) { return v / 16384.0f; }

static int outline_load(const TTFFont* f, int gid, Outline* out, int depth);

static int load_simple(const TTFFont* f, uint32_t p, uint32_t end, int nc, Outline* out) {
    uint32_t ends_at = p + 10;
    p = ends_at;
    if (p + (uint32_t)nc * 2 + 2 > end) return 0;
    int prev_end = -1;
    for (int i = 0; i < nc; i++) {
        int e = rd16(f, ends_at + i * 2);
        if (e <= prev_end) return 0;
        prev_end = e;
    }
    int total = prev_end + 1;
    if (total > 20000) return 0;
    uint32_t instr = rd16(f, p + nc * 2);
    p += nc * 2 + 2 + instr;
    if (p > end) return 0;

    uint8_t* flags = malloc(total);
    if (!flags) return 0;
    for (int i = 0; i < total;) {
        if (p >= end) goto bad;
        uint8_t fl = rd8(f, p++);
        flags[i++] = fl;
        if (fl & 8) {
            if (p >= end) goto bad;
            int rep = rd8(f, p++);
            while (rep-- > 0 && i < total) flags[i++] = fl;
        }
    }
    if (!ol_reserve(out, total, nc)) goto bad;
    int base = out->n;
    int v = 0;
    for (int i = 0; i < total; i++) {
        uint8_t fl = flags[i];
        if (fl & 2) { if (p >= end) goto bad; int b = rd8(f, p++); v += (fl & 16) ? b : -b; }
        else if (!(fl & 16)) { if (p + 2 > end) goto bad; v += rds16(f, p); p += 2; }
        out->x[base + i] = (float)v;
        out->on[base + i] = fl & 1;
    }
    v = 0;
    for (int i = 0; i < total; i++) {
        uint8_t fl = flags[i];
        if (fl & 4) { if (p >= end) goto bad; int b = rd8(f, p++); v += (fl & 32) ? b : -b; }
        else if (!(fl & 32)) { if (p + 2 > end) goto bad; v += rds16(f, p); p += 2; }
        out->y[base + i] = (float)v;
    }
    for (int i = 0; i < nc; i++) out->ends[out->nc + i] = base + rd16(f, ends_at + i * 2);
    out->n += total;
    out->nc += nc;
    free(flags);
    return 1;
bad:
    free(flags);
    return 0;
}

static int outline_load(const TTFFont* f, int gid, Outline* out, int depth) {
    uint32_t p, end;
    if (depth > 6) return 0;
    if (!glyph_range(f, gid, &p, &end)) return 1;          /* пустой глиф — не ошибка */
    if (end - p < 10) return 1;
    int nc = rds16(f, p);

    if (nc >= 0) return nc == 0 ? 1 : load_simple(f, p, end, nc, out);

    /* составной глиф */
    p += 10;
    for (int comp = 0; comp < 64; comp++) {
        if (p + 4 > end) return 0;
        uint16_t flags = rd16(f, p);
        int cgid = rd16(f, p + 2);
        p += 4;
        int a1, a2;
        if (flags & 0x0001) { a1 = (flags & 2) ? rds16(f, p) : rd16(f, p); a2 = (flags & 2) ? rds16(f, p + 2) : rd16(f, p + 2); p += 4; }
        else { a1 = (flags & 2) ? (int8_t)rd8(f, p) : rd8(f, p); a2 = (flags & 2) ? (int8_t)rd8(f, p + 1) : rd8(f, p + 1); p += 2; }

        float xx = 1, xy = 0, yx = 0, yy = 1;
        if (flags & 0x0008)      { xx = yy = f2dot14(rds16(f, p)); p += 2; }
        else if (flags & 0x0040) { xx = f2dot14(rds16(f, p)); yy = f2dot14(rds16(f, p + 2)); p += 4; }
        else if (flags & 0x0080) { xx = f2dot14(rds16(f, p)); yx = f2dot14(rds16(f, p + 2));
                                   xy = f2dot14(rds16(f, p + 4)); yy = f2dot14(rds16(f, p + 6)); p += 8; }

        Outline child; memset(&child, 0, sizeof child);
        if (!outline_load(f, cgid, &child, depth + 1)) { ol_free(&child); return 0; }
        for (int i = 0; i < child.n; i++) {
            float x = child.x[i], y = child.y[i];
            child.x[i] = xx * x + xy * y;
            child.y[i] = yx * x + yy * y;
        }
        float dx = 0, dy = 0;
        if (flags & 0x0002) {
            dx = (float)a1; dy = (float)a2;
            if ((flags & 0x0800) && !(flags & 0x1000)) {
                dx *= hypotf(xx, xy); dy *= hypotf(yy, yx);
            }
        } else if (a1 >= 0 && a1 < out->n && a2 >= 0 && a2 < child.n) {
            dx = out->x[a1] - child.x[a2]; dy = out->y[a1] - child.y[a2];   /* совмещение по точкам */
        }
        if (!ol_reserve(out, child.n, child.nc)) { ol_free(&child); return 0; }
        int base = out->n;
        for (int i = 0; i < child.n; i++) {
            out->x[base + i] = child.x[i] + dx;
            out->y[base + i] = child.y[i] + dy;
            out->on[base + i] = child.on[i];
        }
        for (int i = 0; i < child.nc; i++) out->ends[out->nc + i] = base + child.ends[i];
        out->n += child.n; out->nc += child.nc;
        ol_free(&child);
        if (!(flags & 0x0020)) break;
    }
    return 1;
}

/* ══════════════════════════════════════════════════════════════════
   Растеризация: точное покрытие по площади (signed-area accumulation).
   Каждый отрезок контура добавляет в буфер вклад по площади в каждую
   ячейку; префиксная сумма по строке даёт покрытие 0..1. Это и есть
   честное сглаживание — граница глифа получает дробный альфа-канал,
   а перекрывающиеся контуры (составные глифы) не «вычитаются».
   ══════════════════════════════════════════════════════════════════ */
static void acc_line(float* a, int stride, int w, int h, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    float dir = 1.0f;
    if (y0 > y1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1.0f; }
    if (y1 <= 0.0f || y0 >= (float)h) return;
    float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0.0f) { x -= y0 * dxdy; y0 = 0.0f; }
    int ys = (int)y0;
    int ye = (int)ceilf(y1); if (ye > h) ye = h;
    for (int y = ys; y < ye; y++) {
        float* row = a + (size_t)y * stride;
        float dy = fminf((float)(y + 1), y1) - fmaxf((float)y, y0);
        float xn = x + dxdy * dy;
        float d = dy * dir;
        float xa = x < xn ? x : xn, xb = x < xn ? xn : x;
        if (xa < 0.0f) xa = 0.0f; else if (xa > (float)w) xa = (float)w;
        if (xb < 0.0f) xb = 0.0f; else if (xb > (float)w) xb = (float)w;
        float x0f = floorf(xa); int x0i = (int)x0f;
        float x1c = ceilf(xb);  int x1i = (int)x1c;
        if (x1i <= x0i + 1) {
            float xmf = 0.5f * (xa + xb) - x0f;
            row[x0i]     += d - d * xmf;
            row[x0i + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa);
            float xf = xa - x0f;
            float a0 = 0.5f * s * (1.0f - xf) * (1.0f - xf);
            float xe = xb - x1c + 1.0f;
            float am = 0.5f * s * xe * xe;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = s * (1.5f - xf);
                row[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) row[xi] += d * s;
                float a2 = a1 + (float)(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.0f - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xn;
    }
}

static void acc_quad(float* a, int stride, int w, int h,
                     float x0, float y0, float cx, float cy, float x1, float y1) {
    float dx = x0 - 2 * cx + x1, dy = y0 - 2 * cy + y1;
    int n = (int)ceilf(sqrtf(sqrtf(dx * dx + dy * dy) * 5.0f));   /* допуск ~0.05 px */
    if (n < 1) n = 1; else if (n > 32) n = 32;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n, mt = 1.0f - t;
        float qx = mt * mt * x0 + 2 * mt * t * cx + t * t * x1;
        float qy = mt * mt * y0 + 2 * mt * t * cy + t * t * y1;
        acc_line(a, stride, w, h, px, py, qx, qy);
        px = qx; py = qy;
    }
}

static void trace_contours(const Outline* o, float* acc, int stride, int w, int h) {
    int a = 0;
    for (int c = 0; c < o->nc; c++) {
        int b = o->ends[c], n = b - a + 1;
        if (n >= 3) {
            const float* X = o->x + a; const float* Y = o->y + a; const uint8_t* ON = o->on + a;
            int first_on = -1;
            for (int i = 0; i < n; i++) if (ON[i]) { first_on = i; break; }
            float sx, sy; int idx, cnt;
            if (first_on >= 0) { sx = X[first_on]; sy = Y[first_on]; idx = first_on + 1; cnt = n - 1; }
            else { sx = 0.5f * (X[n - 1] + X[0]); sy = 0.5f * (Y[n - 1] + Y[0]); idx = 0; cnt = n; }
            float cx = sx, cy = sy;
            int done = 0;
            while (done < cnt) {
                int i0 = idx % n;
                if (ON[i0]) {
                    acc_line(acc, stride, w, h, cx, cy, X[i0], Y[i0]);
                    cx = X[i0]; cy = Y[i0]; idx++; done++;
                } else {
                    float qx = X[i0], qy = Y[i0], ex, ey;
                    if (done + 1 < cnt) {
                        int i1 = (idx + 1) % n;
                        if (ON[i1]) { ex = X[i1]; ey = Y[i1]; idx += 2; done += 2; }
                        else { ex = 0.5f * (qx + X[i1]); ey = 0.5f * (qy + Y[i1]); idx += 1; done += 1; }
                    } else { ex = sx; ey = sy; idx++; done++; }
                    acc_quad(acc, stride, w, h, cx, cy, qx, qy, ex, ey);
                    cx = ex; cy = ey;
                }
            }
            acc_line(acc, stride, w, h, cx, cy, sx, sy);
        }
        a = b + 1;
    }
}

/* Синтетический bold: сдвигаем каждую точку контура наружу по биссектрисе нормалей. */
static void embolden_outline(Outline* o, float d) {
    float* ox = malloc((size_t)o->n * sizeof(float));
    float* oy = malloc((size_t)o->n * sizeof(float));
    if (!ox || !oy) { free(ox); free(oy); return; }
    memset(ox, 0, (size_t)o->n * sizeof(float)); memset(oy, 0, (size_t)o->n * sizeof(float));
    int a = 0;
    for (int c = 0; c < o->nc; c++) {
        int b = o->ends[c], cnt = b - a + 1;
        if (cnt >= 3) {
            for (int i = 0; i < cnt; i++) {
                int p = a + i, pr = a + (i + cnt - 1) % cnt, nx = a + (i + 1) % cnt;
                float e1x = o->x[p] - o->x[pr], e1y = o->y[p] - o->y[pr];
                float e2x = o->x[nx] - o->x[p], e2y = o->y[nx] - o->y[p];
                float l1 = hypotf(e1x, e1y), l2 = hypotf(e2x, e2y);
                if (l1 < 1e-6f || l2 < 1e-6f) continue;
                e1x /= l1; e1y /= l1; e2x /= l2; e2y /= l2;
                float n1x = -e1y, n1y = e1x, n2x = -e2y, n2y = e2x;
                float k = 1.0f + n1x * n2x + n1y * n2y;
                if (k < 0.5f) k = 0.5f;                       /* ограничиваем «шипы» на острых углах */
                ox[p] = (n1x + n2x) * d / k;
                oy[p] = (n1y + n2y) * d / k;
            }
        }
        a = b + 1;
    }
    for (int i = 0; i < o->n; i++) { o->x[i] += ox[i]; o->y[i] += oy[i]; }
    free(ox); free(oy);
}

static void rasterize_glyph(const TTFFont* f, int gid, float scale, float fx, int style, TTFGlyphBmp* g) {
    memset(g, 0, sizeof *g);
    Outline o; memset(&o, 0, sizeof o);
    if (!outline_load(f, gid, &o, 0) || o.n == 0) { ol_free(&o); return; }
    if (style & TTF_BOLD) embolden_outline(&o, 0.028f * f->units_per_em);
    if (style & TTF_ITALIC) for (int i = 0; i < o.n; i++) o.x[i] += 0.2126f * o.y[i];

    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < o.n; i++) {
        float X = o.x[i] * scale + fx, Y = o.y[i] * scale;
        if (X < minx) minx = X;
        if (X > maxx) maxx = X;
        if (Y < miny) miny = Y;
        if (Y > maxy) maxy = Y;
    }
    int left = (int)floorf(minx), right = (int)ceilf(maxx);
    int top = (int)ceilf(maxy), bottom = (int)floorf(miny);
    int w = right - left, h = top - bottom;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > MAX_GLYPH_SIDE || h > MAX_GLYPH_SIDE) { ol_free(&o); return; }

    for (int i = 0; i < o.n; i++) {                        /* в координаты битмапа (y вниз) */
        o.x[i] = o.x[i] * scale + fx - (float)left;
        o.y[i] = (float)top - o.y[i] * scale;
    }
    int stride = w + 2;
    float* acc = calloc((size_t)stride * h, sizeof(float));
    uint8_t* px = malloc((size_t)w * h);
    if (!acc || !px) { free(acc); free(px); ol_free(&o); return; }

    trace_contours(&o, acc, stride, w, h);

    for (int y = 0; y < h; y++) {
        const float* row = acc + (size_t)y * stride;
        float s = 0;
        for (int x = 0; x < w; x++) {
            s += row[x];
            float c = fabsf(s); if (c > 1.0f) c = 1.0f;
            px[(size_t)y * w + x] = (uint8_t)(c * 255.0f + 0.5f);
        }
    }
    free(acc); ol_free(&o);
    g->px = px; g->w = (uint16_t)w; g->h = (uint16_t)h;
    g->left = (int16_t)left; g->top = (int16_t)top;
}

/* ══════════════════════════════════════════════════════════════════
   Кэш глифов
   ══════════════════════════════════════════════════════════════════ */
static void strike_clear(TTFStrike* s) {
    if (s->slots) {
        for (uint32_t i = 0; i < s->cap; i++) free(s->slots[i].bmp.px);
        free(s->slots);
    }
    s->slots = NULL; s->cap = s->count = 0;
}

static int strike_init(TTFStrike* s) {
    s->cap = 256; s->count = 0;
    s->slots = calloc(s->cap, sizeof(TTFCacheSlot));
    if (!s->slots) { s->cap = 0; return 0; }
    return 1;
}

static uint32_t key_hash(uint32_t key) { return (key * 2654435761u) >> 7; }

static TTFCacheSlot* strike_insert_slot(TTFStrike* s, uint32_t key) {
    uint32_t mask = s->cap - 1, i = key_hash(key) & mask;
    while (s->slots[i].key && s->slots[i].key != key) i = (i + 1) & mask;
    return &s->slots[i];
}

static int strike_grow(TTFStrike* s) {
    uint32_t old_cap = s->cap; TTFCacheSlot* old = s->slots;
    TTFCacheSlot* ns = calloc(old_cap * 2, sizeof(TTFCacheSlot));
    if (!ns) return 0;
    s->slots = ns; s->cap = old_cap * 2;
    for (uint32_t i = 0; i < old_cap; i++)
        if (old[i].key) *strike_insert_slot(s, old[i].key) = old[i];
    free(old);
    return 1;
}

static TTFStrike* get_strike(TTFFont* f, int size, int style) {
    size |= (style & 3) << 12;
    f->clock++;
    int victim = 0;
    for (int i = 0; i < TTF_NUM_STRIKES; i++) {
        if (f->strikes[i].slots && f->strikes[i].size == size) { f->strikes[i].stamp = f->clock; return &f->strikes[i]; }
    }
    for (int i = 0; i < TTF_NUM_STRIKES; i++) {
        if (!f->strikes[i].slots) { victim = i; break; }
        if (f->strikes[i].stamp < f->strikes[victim].stamp) victim = i;
    }
    TTFStrike* s = &f->strikes[victim];
    strike_clear(s);
    s->size = size; s->stamp = f->clock;
    if (!strike_init(s)) return NULL;
    return s;
}

static const TTFGlyphBmp* strike_get(const TTFFont* f, TTFStrike* s, int gid, int sub, float scale) {
    uint32_t key = (((uint32_t)gid << 2) | (uint32_t)sub) + 1;
    TTFCacheSlot* slot = strike_insert_slot(s, key);
    if (slot->key == key) return &slot->bmp;

    if (s->count >= MAX_STRIKE_GLYPHS) {                    /* не даём кэшу расти бесконечно */
        int size = s->size; uint32_t st = s->stamp;
        strike_clear(s); s->size = size; s->stamp = st;
        if (!strike_init(s)) return NULL;
    } else if ((s->count + 1) * 2 > s->cap) {
        if (!strike_grow(s)) return NULL;
    }
    slot = strike_insert_slot(s, key);
    slot->key = key;
    rasterize_glyph(f, gid, scale, (float)sub / 4.0f, (s->size >> 12) & 3, &slot->bmp);
    s->count++;
    return &slot->bmp;
}

/* ══════════════════════════════════════════════════════════════════
   Метрики и кернинг
   ══════════════════════════════════════════════════════════════════ */
static int advance_units(const TTFFont* f, int gid) {
    int idx = gid < f->num_hmetrics ? gid : f->num_hmetrics - 1;
    return rd16(f, f->hmtx_off + (uint32_t)idx * 4);
}

static int coverage_index(const TTFFont* f, uint32_t cov, int gid) {
    int fmt = rd16(f, cov), n = rd16(f, cov + 2);
    if (fmt == 1) {
        int lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2, g = rd16(f, cov + 4 + mid * 2);
            if (g == gid) return mid;
            if (g < gid) lo = mid + 1; else hi = mid - 1;
        }
    } else if (fmt == 2) {
        int lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2; uint32_t r = cov + 4 + mid * 6;
            int s = rd16(f, r), e = rd16(f, r + 2);
            if (gid < s) hi = mid - 1;
            else if (gid > e) lo = mid + 1;
            else return rd16(f, r + 4) + (gid - s);
        }
    }
    return -1;
}

static int class_of(const TTFFont* f, uint32_t cd, int gid) {
    int fmt = rd16(f, cd);
    if (fmt == 1) {
        int start = rd16(f, cd + 2), n = rd16(f, cd + 4);
        if (gid >= start && gid < start + n) return rd16(f, cd + 6 + (gid - start) * 2);
    } else if (fmt == 2) {
        int n = rd16(f, cd + 2), lo = 0, hi = n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2; uint32_t r = cd + 4 + mid * 6;
            int s = rd16(f, r), e = rd16(f, r + 2);
            if (gid < s) hi = mid - 1;
            else if (gid > e) lo = mid + 1;
            else return rd16(f, r + 4);
        }
    }
    return 0;
}

static int popcount16(unsigned v) { int c = 0; while (v) { c += v & 1; v >>= 1; } return c; }

/* PairPos subtable. applied=1, если подтаблица покрывает пару (значение может быть 0). */
static int pair_subtable(const TTFFont* f, uint32_t sub, int l, int r, int* applied) {
    *applied = 0;
    int fmt = rd16(f, sub);
    int ci = coverage_index(f, sub + rd16(f, sub + 2), l);
    if (ci < 0) return 0;
    unsigned vf1 = rd16(f, sub + 4), vf2 = rd16(f, sub + 6);
    int s1 = popcount16(vf1) * 2, s2 = popcount16(vf2) * 2;
    int xoff = popcount16(vf1 & 3) * 2;

    if (fmt == 1) {
        if (ci >= rd16(f, sub + 8)) return 0;
        uint32_t ps = sub + rd16(f, sub + 10 + ci * 2);
        int cnt = rd16(f, ps), rec = 2 + s1 + s2, lo = 0, hi = cnt - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2; uint32_t rp = ps + 2 + (uint32_t)mid * rec;
            int sg = rd16(f, rp);
            if (sg == r) { *applied = 1; return (vf1 & 4) ? rds16(f, rp + 2 + xoff) : 0; }
            if (sg < r) lo = mid + 1; else hi = mid - 1;
        }
        return 0;
    }
    if (fmt == 2) {
        int c1n = rd16(f, sub + 12), c2n = rd16(f, sub + 14);
        int c1 = class_of(f, sub + rd16(f, sub + 8), l), c2 = class_of(f, sub + rd16(f, sub + 10), r);
        if (c1 >= c1n || c2 >= c2n) return 0;
        uint32_t rec = sub + 16 + ((uint32_t)c1 * c2n + c2) * (s1 + s2);
        *applied = 1;
        return (vf1 & 4) ? rds16(f, rec + xoff) : 0;
    }
    return 0;
}

static int legacy_kern(const TTFFont* f, int l, int r) {
    if (!f->kern_off) return 0;
    uint32_t k = f->kern_off;
    int nt = rd16(f, k + 2);
    uint32_t p = k + 4;
    for (int t = 0; t < nt; t++) {
        int len = rd16(f, p + 2), cov = rd16(f, p + 4);
        if ((cov & 1) && (cov >> 8) == 0) {                 /* горизонтальный, формат 0 */
            int np = rd16(f, p + 6), lo = 0, hi = np - 1;
            uint32_t want = ((uint32_t)l << 16) | (uint32_t)r;
            while (lo <= hi) {
                int mid = (lo + hi) / 2; uint32_t rp = p + 14 + (uint32_t)mid * 6;
                uint32_t have = rd32(f, rp);
                if (have == want) return rds16(f, rp + 4);
                if (have < want) lo = mid + 1; else hi = mid - 1;
            }
        }
        if (len < 6) break;
        p += len;
    }
    return 0;
}

static int kern_units(TTFFont* f, int l, int r) {
    if (!l || !r || (!f->kern_nsubs && !f->kern_off)) return 0;
    uint32_t key = (((uint32_t)l << 16) | (uint32_t)r) + 1;
    TTFKernEntry* e = &f->kern_cache[(key * 2654435761u) >> 22 & (TTF_KERN_CACHE - 1)];
    if (e->key == key) return e->val;

    int total = 0;
    if (f->kern_nsubs) {
        int done_lookup = -1;
        for (int i = 0; i < f->kern_nsubs; i++) {
            if (f->kern_subs[i].lookup == done_lookup) continue;
            int applied, v = pair_subtable(f, f->kern_subs[i].off, l, r, &applied);
            if (applied) { total += v; done_lookup = f->kern_subs[i].lookup; }
        }
    } else {
        total = legacy_kern(f, l, r);
    }
    e->key = key; e->val = (int16_t)total;
    return total;
}

static int cmp_u16(const void* a, const void* b) { return (int)*(const uint16_t*)a - (int)*(const uint16_t*)b; }

static void parse_gpos_kern(TTFFont* f) {
    uint32_t g = f->gpos_off;
    if (!g || rd16(f, g) != 1) return;
    uint32_t flist = g + rd16(f, g + 6), llist = g + rd16(f, g + 8);
    int nfeat = rd16(f, flist), nl_total = rd16(f, llist);
    uint16_t lookups[64]; int nl = 0;
    for (int i = 0; i < nfeat; i++) {
        if (rd32(f, flist + 2 + i * 6) != TAG('k', 'e', 'r', 'n')) continue;
        uint32_t feat = flist + rd16(f, flist + 2 + i * 6 + 4);
        int cnt = rd16(f, feat + 2);
        for (int j = 0; j < cnt; j++) {
            uint16_t li = rd16(f, feat + 4 + j * 2);
            int dup = 0;
            for (int k = 0; k < nl; k++) if (lookups[k] == li) { dup = 1; break; }
            if (!dup && nl < 64 && li < nl_total) lookups[nl++] = li;
        }
    }
    qsort(lookups, nl, sizeof(uint16_t), cmp_u16);          /* lookup'ы применяются по порядку индексов */
    for (int k = 0; k < nl; k++) {
        uint32_t lk = llist + rd16(f, llist + 2 + lookups[k] * 2);
        int type = rd16(f, lk), nsub = rd16(f, lk + 4);
        for (int s = 0; s < nsub; s++) {
            uint32_t sub = lk + rd16(f, lk + 6 + s * 2);
            int t = type;
            if (t == 9) {                                    /* Extension */
                if (rd16(f, sub) != 1) continue;
                t = rd16(f, sub + 2);
                sub += rd32(f, sub + 4);
            }
            if (t != 2) continue;
            if (f->kern_nsubs >= TTF_MAX_KERN_SUBS) return;
            f->kern_subs[f->kern_nsubs].off = sub;
            f->kern_subs[f->kern_nsubs].lookup = (uint8_t)k;
            f->kern_nsubs++;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
   Загрузка
   ══════════════════════════════════════════════════════════════════ */
static void font_destroy(TTFFont* f) {
    if (!f) return;
    for (int i = 0; i < TTF_NUM_STRIKES; i++) strike_clear(&f->strikes[i]);
    free(f->data);
    free(f);
}

TTFFont* ttf_load_memory(uint8_t* data, size_t len, const char* name) {
    if (!data) return NULL;
    const char* label = name ? name : "(memory)";

#ifdef TTF_HAVE_WOFF
    if (len >= 4 && memcmp(data, "wOFF", 4) == 0) {
        size_t sl = 0; uint8_t* sfnt = woff_to_sfnt(data, len, &sl);
        free(data);
        if (!sfnt) { gal_log("WARN", "Font %s: invalid WOFF", label); return NULL; }
        data = sfnt; len = sl;
    }
#endif
    if (len >= 4 && memcmp(data, "wOF2", 4) == 0) {
        gal_log("WARN", "Font %s: WOFF2 is not supported", label);
        free(data); return NULL;
    }

    TTFFont* f = calloc(1, sizeof *f);
    if (!f) { free(data); return NULL; }
    f->data = data; f->data_len = len;
    strncpy(f->filepath, label, sizeof(f->filepath) - 1);

    uint32_t base = 0, tag = rd32(f, 0);
    if (tag == TAG('t', 't', 'c', 'f')) base = rd32(f, 12);       /* первый шрифт коллекции */
    tag = rd32(f, base);
    if (tag == TAG('O', 'T', 'T', 'O')) {
        gal_log("WARN", "Font %s: CFF outlines are not supported", label);
        font_destroy(f); return NULL;
    }
    if (tag != 0x00010000 && tag != TAG('t', 'r', 'u', 'e')) {
        gal_log("WARN", "Font %s: not a TrueType font", label);
        font_destroy(f); return NULL;
    }

    uint32_t head = 0, maxp = 0, cmap = 0, hhea = 0;
    int nt = rd16(f, base + 4);
    for (int i = 0; i < nt; i++) {
        uint32_t rec = base + 12 + i * 16, t = rd32(f, rec), off = rd32(f, rec + 8), ln = rd32(f, rec + 12);
        if (!in_range(f, off, ln)) continue;
        switch (t) {
            case TAG('h', 'e', 'a', 'd'): head = off; if (ln < 54) head = 0; break;
            case TAG('m', 'a', 'x', 'p'): maxp = off; break;
            case TAG('c', 'm', 'a', 'p'): cmap = off; break;
            case TAG('h', 'h', 'e', 'a'): hhea = off; if (ln < 36) hhea = 0; break;
            case TAG('l', 'o', 'c', 'a'): f->loca_off = off; f->loca_len = ln; break;
            case TAG('g', 'l', 'y', 'f'): f->glyf_off = off; f->glyf_len = ln; break;
            case TAG('h', 'm', 't', 'x'): f->hmtx_off = off; f->hmtx_len = ln; break;
            case TAG('G', 'P', 'O', 'S'): f->gpos_off = off; break;
            case TAG('k', 'e', 'r', 'n'): f->kern_off = off; break;
        }
    }
    if (!head || !maxp || !cmap || !hhea || !f->loca_off || !f->glyf_off || !f->hmtx_off) {
        gal_log("WARN", "Font %s: required TrueType tables are missing", label);
        font_destroy(f); return NULL;
    }

    f->units_per_em = (int16_t)rd16(f, head + 18);
    f->loca_format = rds16(f, head + 50);
    f->num_glyphs = rd16(f, maxp + 4);
    f->ascent = rds16(f, hhea + 4);
    f->descent = rds16(f, hhea + 6);
    f->line_gap = rds16(f, hhea + 8);
    f->num_hmetrics = rd16(f, hhea + 34);

    int loca_entries = (int)(f->loca_len / (f->loca_format ? 4 : 2));
    if (f->num_glyphs > loca_entries - 1) f->num_glyphs = loca_entries - 1;
    if ((uint32_t)f->num_hmetrics * 4 > f->hmtx_len) f->num_hmetrics = (int)(f->hmtx_len / 4);
    if (f->units_per_em < 16 || f->units_per_em > 16384 || f->num_glyphs < 1 || f->num_hmetrics < 1) {
        gal_log("WARN", "Font %s: invalid head/maxp/hhea data", label);
        font_destroy(f); return NULL;
    }
    if (f->ascent == 0 && f->descent == 0) { f->ascent = f->units_per_em * 4 / 5; f->descent = -(f->units_per_em / 5); }
    if (!pick_cmap(f, cmap)) {
        gal_log("WARN", "Font %s: no usable Unicode cmap", label);
        font_destroy(f); return NULL;
    }

    for (int c = 0; c < 128; c++) {
        uint16_t g = f->cmap_fmt == 12 ? cmap12_lookup(f, f->cmap_sub, c) : cmap4_lookup(f, f->cmap_sub, c);
        f->ascii_gid[c] = g < f->num_glyphs ? g : 0;
    }
    parse_gpos_kern(f);

    gal_log("INFO", "TTF loaded: %s (UPM=%d, asc=%d, desc=%d, glyphs=%d, kern=%s)", label,
            f->units_per_em, f->ascent, f->descent, f->num_glyphs,
            f->kern_nsubs ? "GPOS" : (f->kern_off ? "kern" : "none"));
    return f;
}

TTFFont* ttf_load(const char* path) {
    if (!path) return NULL;
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 12 || (unsigned long)sz > MAX_FONT_FILE) { fclose(fp); return NULL; }
    uint8_t* buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { free(buf); return NULL; }
    return ttf_load_memory(buf, (size_t)sz, path);
}

void ttf_free(TTFFont* font) { font_destroy(font); }

int ttf_supports_woff(void) {
#ifdef TTF_HAVE_WOFF
    return 1;
#else
    return 0;
#endif
}

void ttf_set_fallback(TTFFont* font, TTFFont* fallback) {
    if (font && font != fallback) font->fallback = fallback;
}

int ttf_has_glyph(TTFFont* font, uint32_t cp) {
    return font && glyph_index(font, cp) != 0;
}

/* ══════════════════════════════════════════════════════════════════
   Текст
   ══════════════════════════════════════════════════════════════════ */
static uint32_t utf8_next(const uint8_t** pp, const uint8_t* end) {
    const uint8_t* p = *pp;
    uint32_t c = *p++;
    if (c < 0x80) { *pp = p; return c; }
    int n; uint32_t cp;
    if ((c & 0xE0) == 0xC0)      { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { *pp = p; return 0xFFFD; }
    for (int i = 0; i < n; i++) {
        if (p >= end || (*p & 0xC0) != 0x80) { *pp = p; return 0xFFFD; }
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    *pp = p;
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

/* Глиф для кодпоинта: сам шрифт, затем цепочка fallback'ов. Иначе .notdef. */
static TTFFont* resolve_glyph(TTFFont* f, uint32_t cp, int* gid) {
    TTFFont* cur = f;
    for (int depth = 0; cur && depth < 4; depth++, cur = cur->fallback) {
        int g = glyph_index(cur, cp);
        if (g) { *gid = g; return cur; }
    }
    *gid = 0;
    return f;
}

typedef struct {
    uint32_t* buf; int bw, bh;      /* bw — stride */
    int cx0, cy0, cx1, cy1;         /* прямоугольник отсечения */
    int x0, baseline, style;
    int r, g, b;
    const uint8_t* lut;
} DrawCtx;

static uint8_t g_lut_dark[256], g_lut_light[256];
static int g_lut_ready = 0;

static void init_luts(void) {
    /* Смешивание в sRGB-пространстве делает тёмный текст на светлом чуть
       «жирнее», а светлый на тёмном — «тоньше». Компенсируем кривой покрытия. */
    for (int i = 0; i < 256; i++) {
        float a = i / 255.0f;
        g_lut_dark[i]  = (uint8_t)(powf(a, 0.92f) * 255.0f + 0.5f);
        g_lut_light[i] = (uint8_t)(powf(a, 0.72f) * 255.0f + 0.5f);
    }
    g_lut_ready = 1;
}

static void blit_glyph(const DrawCtx* dc, const TTFGlyphBmp* g, int gx, int gy) {
    if (!g->px) return;
    int c0 = gx < dc->cx0 ? dc->cx0 - gx : 0;
    int c1 = gx + g->w > dc->cx1 ? dc->cx1 - gx : g->w;
    if (c0 >= c1) return;
    const uint32_t fg = ((uint32_t)dc->r << 16) | ((uint32_t)dc->g << 8) | (uint32_t)dc->b;
    for (int row = 0; row < g->h; row++) {
        int py = gy + row;
        if (py < dc->cy0 || py >= dc->cy1) continue;
        const uint8_t* src = g->px + (size_t)row * g->w;
        uint32_t* dst = dc->buf + (size_t)py * dc->bw + gx;
        for (int col = c0; col < c1; col++) {
            int a = src[col];
            if (!a) continue;
            a = dc->lut[a];
            if (a >= 255) { dst[col] = fg; continue; }
            uint32_t d = dst[col];
            int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, ia = 255 - a;
            dst[col] = ((uint32_t)((dc->r * a + dr * ia + 127) / 255) << 16) |
                       ((uint32_t)((dc->g * a + dg * ia + 127) / 255) << 8) |
                        (uint32_t)((dc->b * a + db * ia + 127) / 255);
        }
    }
}

static int is_space_cp(uint32_t cp) {
    return (cp >= 0x2000 && cp <= 0x200A) || cp == 0x1680 || cp == 0x205F || cp == 0x3000;
}

/* Единый проход для измерения и отрисовки — ширина всегда совпадает с рендером. */
static float run_text(TTFFont* f, const char* text, size_t nbytes, int size, int style, const DrawCtx* dc, float* marks) {
    const uint8_t* p = (const uint8_t*)text;
    const uint8_t* end = p + nbytes;
    float pen = 0.0f;
    TTFFont* prev_font = NULL; int prev_gid = 0;
    TTFFont* strike_font = NULL; TTFStrike* strike = NULL;

    while (p < end) {
        const uint8_t* cstart = p;
        uint32_t cp = utf8_next(&p, end);
        if (marks) for (const uint8_t* q = cstart; q < p; q++) marks[q - (const uint8_t*)text] = pen;
        if (cp == '\t') {
            int sp = glyph_index(f, ' ');
            pen += 4.0f * advance_units(f, sp) * size / f->units_per_em;
            prev_font = NULL; continue;
        }
        if (cp == '\n' || cp == '\r') cp = ' ';
        else if (cp == 0xA0 || cp == 0x202F) cp = ' ';
        else if (cp < 0x20 || cp == 0x7F || cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) ||
                 (cp >= 0x2028 && cp <= 0x202E) || cp == 0x2060 || cp == 0xFEFF ||
                 (cp >= 0xFE00 && cp <= 0xFE0F)) continue;

        int gid;
        TTFFont* uf = resolve_glyph(f, cp, &gid);
        float sc = (float)size / uf->units_per_em;
        if (!gid && is_space_cp(cp)) {                 /* экзотический пробел без глифа */
            pen += size * 0.3f; prev_font = NULL; continue;
        }
        if (uf == prev_font && prev_gid) pen += kern_units(uf, prev_gid, gid) * sc;
        if (marks) for (const uint8_t* q = cstart; q < p; q++) marks[q - (const uint8_t*)text] = pen;

        if (dc) {
            float gx = (float)dc->x0 + pen;
            int ix = (int)floorf(gx);
            int sub = (int)((gx - (float)ix) * 4.0f + 0.5f);
            if (sub >= 4) { ix++; sub = 0; }
            if (uf != strike_font) { strike_font = uf; strike = get_strike(uf, size, style); }
            if (strike) {
                const TTFGlyphBmp* g = strike_get(uf, strike, gid, sub, sc);
                if (g) blit_glyph(dc, g, ix + g->left, dc->baseline - g->top);
            }
        }
        pen += (advance_units(uf, gid) + ((style & TTF_BOLD) ? 0.056f * uf->units_per_em : 0.0f)) * sc;
        prev_font = uf; prev_gid = gid;
    }
    if (marks) marks[nbytes] = pen;
    return pen;
}

static int valid_size(int s) { return s > 0 && s <= 512; }

int ttf_render_string_ex(TTFFont* font, const char* text, uint32_t* buffer, int stride,
                         int cx0, int cy0, int cx1, int cy1,
                         int font_size, int x, int y, uint32_t color, int style) {
    if (!font || !text || !buffer || !valid_size(font_size)) return 0;
    if (!g_lut_ready) init_luts();
    float scale = (float)font_size / font->units_per_em;
    int baseline = y + (int)floorf(font->ascent * scale + 0.5f);

    DrawCtx dc;
    dc.buf = buffer; dc.bw = stride; dc.bh = cy1;
    dc.cx0 = cx0; dc.cy0 = cy0; dc.cx1 = cx1; dc.cy1 = cy1;
    dc.x0 = x; dc.baseline = baseline; dc.style = style;
    dc.r = (color >> 16) & 255; dc.g = (color >> 8) & 255; dc.b = color & 255;
    dc.lut = ((dc.r * 77 + dc.g * 151 + dc.b * 28) >> 8) < 128 ? g_lut_dark : g_lut_light;

    /* Строка вне области отсечения по вертикали — только меряем, не растеризуем. */
    int margin = font_size * 2;
    int visible = baseline + margin > cy0 && baseline - margin < cy1 && x < cx1;
    float w = run_text(font, text, strlen(text), font_size, style, visible ? &dc : NULL, NULL);
    return (int)floorf(w + 0.5f);
}

int ttf_render_string(TTFFont* font, const char* text, uint32_t* buffer, int buf_w, int buf_h,
                      int font_size, int x, int y, uint32_t color) {
    return ttf_render_string_ex(font, text, buffer, buf_w, 0, 0, buf_w, buf_h, font_size, x, y, color, 0);
}

float ttf_text_width_f(TTFFont* font, const char* text, int font_size, int style) {
    if (!font || !text || !valid_size(font_size)) return 0.0f;
    return run_text(font, text, strlen(text), font_size, style, NULL, NULL);
}

int ttf_text_width_n(TTFFont* font, const char* text, size_t nbytes, int font_size) {
    if (!font || !text || !valid_size(font_size)) return 0;
    size_t n = strlen(text);
    if (nbytes > n) nbytes = n;
    return (int)floorf(run_text(font, text, nbytes, font_size, 0, NULL, NULL) + 0.5f);
}

int ttf_text_width(TTFFont* font, const char* text, int font_size) {
    if (!text) return 0;
    return ttf_text_width_n(font, text, strlen(text), font_size);
}

int ttf_font_ascent(TTFFont* font, int font_size) {
    if (!font || !valid_size(font_size)) return 0;
    return (int)floorf(font->ascent * (float)font_size / font->units_per_em + 0.5f);
}

int ttf_font_descent(TTFFont* font, int font_size) {
    if (!font || !valid_size(font_size)) return 0;
    return (int)floorf(-font->descent * (float)font_size / font->units_per_em + 0.5f);
}

int ttf_text_offsets_ex(TTFFont* font, const char* text, int font_size, int style, int* out, int cap) {
    if (!font || !text || !out || !valid_size(font_size)) return 0;
    size_t n = strlen(text);
    if (cap < 0 || (size_t)cap < n + 1) return 0;
    float* marks = malloc((n + 1) * sizeof(float));
    if (!marks) return 0;
    run_text(font, text, n, font_size, style, NULL, marks);
    for (size_t i = 0; i <= n; i++) out[i] = (int)floorf(marks[i] + 0.5f);
    free(marks);
    return out[n];
}

int ttf_text_offsets(TTFFont* font, const char* text, int font_size, int* out, int cap) {
    return ttf_text_offsets_ex(font, text, font_size, 0, out, cap);
}
