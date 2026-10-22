/* CFF + Type 2 charstring. Все обращения к данным проверяются по границам. */
#include "cff.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { const uint8_t* base; size_t len; uint32_t count; int offsize; size_t offs, data; } Index;       /* offs — позиция массива смещений, data — позиция (data-1) для смещения 1 */
typedef struct { size_t subrs_off; int have_subrs; Index subrs; } Priv;
struct CffFont {
    const uint8_t* d; size_t n;
    Index charstrings, gsubrs; Priv priv0;                       /* priv0 — для некорневых (не CID) шрифтов */
    int is_cid; Index fdarray; Priv* fds; int nfd; size_t fdselect; int fdsel_fmt;
    int nglyphs;
};

static int idx_open(const uint8_t* d, size_t n, size_t pos, Index* ix, size_t* end) {
    memset(ix, 0, sizeof *ix); ix->base = d; ix->len = n;
    if (pos + 2 > n) return 0;
    ix->count = (uint32_t)d[pos] << 8 | d[pos + 1];
    if (ix->count == 0) { if (end) *end = pos + 2; return 1; }
    if (pos + 3 > n) return 0;
    ix->offsize = d[pos + 2]; if (ix->offsize < 1 || ix->offsize > 4) return 0;
    ix->offs = pos + 3; size_t tbl = (size_t)(ix->count + 1) * (size_t)ix->offsize; if (ix->offs + tbl > n) return 0;
    ix->data = ix->offs + tbl - 1;
    uint32_t last = 0; const uint8_t* p = d + ix->offs + (size_t)ix->count * (size_t)ix->offsize; for (int i = 0; i < ix->offsize; i++) last = last << 8 | p[i];
    if (last < 1 || ix->data + last > n) return 0;
    if (end) *end = ix->data + last;
    return 1;
}
static int idx_get(const Index* ix, uint32_t i, const uint8_t** p, size_t* len) {
    if (i >= ix->count) return 0;
    const uint8_t* o = ix->base + ix->offs + (size_t)i * (size_t)ix->offsize; uint32_t a = 0, b = 0;
    for (int k = 0; k < ix->offsize; k++) a = a << 8 | o[k];
    o += ix->offsize; for (int k = 0; k < ix->offsize; k++) b = b << 8 | o[k];
    if (b < a || a < 1 || ix->data + b > ix->len) return 0;
    *p = ix->base + ix->data + a; *len = b - a; return 1;
}

/* ── DICT ── */
typedef struct { double v[48]; int n; } DStack;
static int dict_number(const uint8_t* p, size_t n, size_t* i, double* out) {
    uint8_t b0 = p[*i];
    if (b0 == 28) { if (*i + 3 > n) return 0; *out = (int16_t)(p[*i + 1] << 8 | p[*i + 2]); *i += 3; return 1; }
    if (b0 == 29) { if (*i + 5 > n) return 0; *out = (int32_t)((uint32_t)p[*i + 1] << 24 | p[*i + 2] << 16 | p[*i + 3] << 8 | p[*i + 4]); *i += 5; return 1; }
    if (b0 == 30) {                                                           /* вещественное в BCD-полубайтах */
        char buf[64]; int k = 0; size_t j = *i + 1; int done = 0;
        while (j < n && !done) {
            for (int h = 0; h < 2; h++) {
                int nb = h == 0 ? p[j] >> 4 : p[j] & 15; if (nb == 15) { done = 1; break; }
                if (k > 55) continue;
                if (nb <= 9) buf[k++] = (char)('0' + nb); else if (nb == 10) buf[k++] = '.'; else if (nb == 11) buf[k++] = 'E'; else if (nb == 12) { buf[k++] = 'E'; buf[k++] = '-'; } else if (nb == 14) buf[k++] = '-';
            }
            j++;
        }
        buf[k] = 0; *out = atof(buf); *i = j; return 1;
    }
    if (b0 >= 32 && b0 <= 246) { *out = (double)b0 - 139; (*i)++; return 1; }
    if (b0 >= 247 && b0 <= 250) { if (*i + 2 > n) return 0; *out = (double)((b0 - 247) * 256 + p[*i + 1] + 108); *i += 2; return 1; }
    if (b0 >= 251 && b0 <= 254) { if (*i + 2 > n) return 0; *out = (double)(-(b0 - 251) * 256 - p[*i + 1] - 108); *i += 2; return 1; }
    return 0;
}
/* вызывает cb(op, stack) для каждого оператора словаря; op для двухбайтовых = 0x0C00 | b1 */
static int dict_walk(const uint8_t* p, size_t n, int (*cb)(void*, int, const DStack*), void* ud) {
    DStack st; st.n = 0; size_t i = 0;
    while (i < n) {
        uint8_t b0 = p[i];
        if (b0 <= 21) { int op = b0; i++; if (b0 == 12) { if (i >= n) return 0; op = 0x0C00 | p[i++]; } if (!cb(ud, op, &st)) return 0; st.n = 0; }
        else { double v; if (!dict_number(p, n, &i, &v)) return 0; if (st.n < 48) st.v[st.n++] = v; }
    }
    return 1;
}
typedef struct { size_t charstrings, priv_off, priv_size, fdarray, fdselect, subrs; int have_priv, cid; } DictRes;
static int top_cb(void* ud, int op, const DStack* s) {
    DictRes* r = ud;
    if (op == 17 && s->n >= 1) r->charstrings = (size_t)s->v[0];
    else if (op == 18 && s->n >= 2) { r->priv_size = (size_t)s->v[0]; r->priv_off = (size_t)s->v[1]; r->have_priv = 1; }
    else if (op == 0x0C24 && s->n >= 1) r->fdarray = (size_t)s->v[0];
    else if (op == 0x0C25 && s->n >= 1) r->fdselect = (size_t)s->v[0];
    else if (op == 0x0C1E) r->cid = 1;
    else if (op == 19 && s->n >= 1) r->subrs = (size_t)s->v[0] + 1;           /* для private-словаря: смещение относительно его начала (+1 — признак «есть») */
    return 1;
}
static int load_priv(const CffFont* cf, size_t off, size_t size, Priv* pv) {
    (void)cf; memset(pv, 0, sizeof *pv);
    if (off + size > cf->n) return 0;
    DictRes r; memset(&r, 0, sizeof r); if (!dict_walk(cf->d + off, size, top_cb, &r)) return 0;
    if (r.subrs) { pv->subrs_off = off + r.subrs - 1; if (idx_open(cf->d, cf->n, pv->subrs_off, &pv->subrs, NULL)) pv->have_subrs = 1; }
    return 1;
}

CffFont* cff_open(const uint8_t* d, size_t n) {
    if (n < 4 || d[0] != 1) return NULL;                                    /* major version 1 (CFF2 — 2 — не поддерживаем) */
    size_t pos = d[2], end;
    Index nameidx, topidx, stridx; if (!idx_open(d, n, pos, &nameidx, &end) || !idx_open(d, n, end, &topidx, &end) || !idx_open(d, n, end, &stridx, &end)) return NULL;
    CffFont* f = calloc(1, sizeof *f); if (!f) return NULL; f->d = d; f->n = n;
    if (!idx_open(d, n, end, &f->gsubrs, NULL)) memset(&f->gsubrs, 0, sizeof f->gsubrs);
    const uint8_t* td; size_t tl; if (!idx_get(&topidx, 0, &td, &tl)) { free(f); return NULL; }
    DictRes r; memset(&r, 0, sizeof r); if (!dict_walk(td, tl, top_cb, &r) || !r.charstrings) { free(f); return NULL; }
    if (!idx_open(d, n, r.charstrings, &f->charstrings, NULL)) { free(f); return NULL; }
    f->nglyphs = (int)f->charstrings.count;
    if (r.cid && r.fdarray && r.fdselect) {
        f->is_cid = 1; if (!idx_open(d, n, r.fdarray, &f->fdarray, NULL)) { free(f); return NULL; }
        f->nfd = (int)f->fdarray.count; f->fds = calloc((size_t)(f->nfd ? f->nfd : 1), sizeof(Priv)); if (!f->fds) { free(f); return NULL; }
        for (int i = 0; i < f->nfd; i++) {
            const uint8_t* fd; size_t fl; if (!idx_get(&f->fdarray, (uint32_t)i, &fd, &fl)) continue;
            DictRes fr; memset(&fr, 0, sizeof fr); if (dict_walk(fd, fl, top_cb, &fr) && fr.have_priv) load_priv(f, fr.priv_off, fr.priv_size, &f->fds[i]);
        }
        f->fdselect = r.fdselect; if (f->fdselect >= n) { cff_close(f); return NULL; } f->fdsel_fmt = d[f->fdselect];
    } else if (r.have_priv) load_priv(f, r.priv_off, r.priv_size, &f->priv0);
    return f;
}
void cff_close(CffFont* f) { if (f) { free(f->fds); free(f); } }
int cff_num_glyphs(const CffFont* f) { return f ? f->nglyphs : 0; }

static int fd_of(const CffFont* f, int gid) {
    const uint8_t* d = f->d; size_t p = f->fdselect;
    if (f->fdsel_fmt == 0) { size_t q = p + 1 + (size_t)gid; return q < f->n ? d[q] : 0; }
    if (f->fdsel_fmt == 3) {
        if (p + 3 > f->n) return 0; int nr = d[p + 1] << 8 | d[p + 2]; p += 3;
        for (int i = 0; i < nr; i++) { size_t q = p + (size_t)i * 3; if (q + 5 > f->n) return 0; int first = d[q] << 8 | d[q + 1]; int next = d[q + 3] << 8 | d[q + 4]; if (gid >= first && gid < next) return d[q + 2]; }
    }
    return 0;
}

/* ── интерпретатор Type 2 ── */
typedef struct {
    const CffFont* f; const Priv* pv; const CffSink* sink;
    double st[96]; int sp; double x, y; int nstems, width_seen, open, finished, ops;
} RT;
static int subr_bias(uint32_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }
static void rt_move(RT* r, double dx, double dy) { if (r->open) r->sink->close(r->sink->ud); r->x += dx; r->y += dy; r->sink->move(r->sink->ud, (float)r->x, (float)r->y); r->open = 1; }
static void rt_line(RT* r, double dx, double dy) { if (!r->open) { r->sink->move(r->sink->ud, (float)r->x, (float)r->y); r->open = 1; } r->x += dx; r->y += dy; r->sink->line(r->sink->ud, (float)r->x, (float)r->y); }
static void rt_curve(RT* r, double a, double b, double c, double d, double e, double g) {
    if (!r->open) { r->sink->move(r->sink->ud, (float)r->x, (float)r->y); r->open = 1; }
    double x1 = r->x + a, y1 = r->y + b, x2 = x1 + c, y2 = y1 + d, x3 = x2 + e, y3 = y2 + g;
    r->sink->curve(r->sink->ud, (float)x1, (float)y1, (float)x2, (float)y2, (float)x3, (float)y3); r->x = x3; r->y = y3;
}
static int run(RT* r, const uint8_t* p, size_t n, int depth) {
    if (depth > 10) return -1;
    size_t i = 0;
    while (i < n && !r->finished) {
        if (++r->ops > 200000) return -1;
        uint8_t b = p[i++];
        if (b >= 32 || b == 28) {                                                         /* число */
            double v;
            if (b == 28) { if (i + 2 > n) return -1; v = (int16_t)(p[i] << 8 | p[i + 1]); i += 2; }
            else if (b <= 246) v = (double)b - 139;
            else if (b <= 250) { if (i >= n) return -1; v = (double)((b - 247) * 256 + p[i++] + 108); }
            else if (b <= 254) { if (i >= n) return -1; v = (double)(-(b - 251) * 256 - p[i++] - 108); }
            else { if (i + 4 > n) return -1; v = (int32_t)((uint32_t)p[i] << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3]) / 65536.0; i += 4; }
            if (r->sp >= 96) return -1; r->st[r->sp++] = v; continue;
        }
        double* a = r->st; int ns = r->sp;
        #define NEED(k) if (ns < (k)) return -1
        switch (b) {
        case 1: case 3: case 18: case 23:                                                 /* hstem vstem hstemhm vstemhm */
            if (ns & 1) { r->width_seen = 1; } r->nstems += ns / 2; r->sp = 0; break;
        case 19: case 20:                                                                 /* hintmask cntrmask */
            r->nstems += ns / 2; r->sp = 0; i += (size_t)(r->nstems + 7) / 8; if (i > n) return -1; break;
        case 21: { int k = ns; if (k > 2) { a++; k--; } NEED(0); if (k < 2) return -1; rt_move(r, a[k - 2], a[k - 1]); r->sp = 0; break; }
        case 22: { int k = ns; if (k > 1) { a++; k--; } if (k < 1) return -1; rt_move(r, a[0], 0); r->sp = 0; break; }
        case 4:  { int k = ns; if (k > 1) { a++; k--; } if (k < 1) return -1; rt_move(r, 0, a[0]); r->sp = 0; break; }
        case 5: for (int k = 0; k + 1 < ns; k += 2) rt_line(r, a[k], a[k + 1]); r->sp = 0; break;
        case 6: case 7: { int h = b == 6; for (int k = 0; k < ns; k++, h = !h) { if (h) rt_line(r, a[k], 0); else rt_line(r, 0, a[k]); } r->sp = 0; break; }
        case 8: for (int k = 0; k + 5 < ns; k += 6) rt_curve(r, a[k], a[k + 1], a[k + 2], a[k + 3], a[k + 4], a[k + 5]); r->sp = 0; break;
        case 24: { int k = 0; for (; k + 7 < ns; k += 6) rt_curve(r, a[k], a[k + 1], a[k + 2], a[k + 3], a[k + 4], a[k + 5]); if (k + 1 < ns) rt_line(r, a[k], a[k + 1]); r->sp = 0; break; }
        case 25: { int k = 0; for (; k + 7 < ns; k += 2) rt_line(r, a[k], a[k + 1]); if (k + 5 < ns) rt_curve(r, a[k], a[k + 1], a[k + 2], a[k + 3], a[k + 4], a[k + 5]); r->sp = 0; break; }
        case 26: { int k = 0; double dx1 = 0; if (ns & 1) dx1 = a[k++]; for (; k + 3 < ns; k += 4) { rt_curve(r, dx1, a[k], a[k + 1], a[k + 2], 0, a[k + 3]); dx1 = 0; } r->sp = 0; break; }
        case 27: { int k = 0; double dy1 = 0; if (ns & 1) dy1 = a[k++]; for (; k + 3 < ns; k += 4) { rt_curve(r, a[k], dy1, a[k + 1], a[k + 2], a[k + 3], 0); dy1 = 0; } r->sp = 0; break; }
        case 30: case 31: {
            int horiz = b == 31;
            for (int k = 0; k + 3 < ns; k += 4) {
                int last = (ns - k == 5); double extra = last ? a[k + 4] : 0;
                if (horiz) rt_curve(r, a[k], 0, a[k + 1], a[k + 2], extra, a[k + 3]); else rt_curve(r, 0, a[k], a[k + 1], a[k + 2], a[k + 3], extra);
                horiz = !horiz;
            }
            r->sp = 0; break; }
        case 10: case 29: {
            NEED(1); const Index* ix = b == 10 ? (r->pv && r->pv->have_subrs ? &r->pv->subrs : NULL) : &r->f->gsubrs; if (!ix) return -1;
            int idx = (int)a[ns - 1] + subr_bias(ix->count); r->sp--;
            const uint8_t* sp; size_t sl; if (idx < 0 || !idx_get(ix, (uint32_t)idx, &sp, &sl)) return -1;
            int rc = run(r, sp, sl, depth + 1); if (rc < 0) return rc; break; }
        case 11: return 0;
        case 14: r->finished = 1; if (r->open) { r->sink->close(r->sink->ud); r->open = 0; } return 0;
        case 12: {
            if (i >= n) return -1; uint8_t b1 = p[i++];
            switch (b1) {
            case 35: NEED(12); rt_curve(r, a[0], a[1], a[2], a[3], a[4], a[5]); rt_curve(r, a[6], a[7], a[8], a[9], a[10], a[11]); r->sp = 0; break;
            case 34: NEED(7); rt_curve(r, a[0], 0, a[1], a[2], a[3], 0); rt_curve(r, a[4], 0, a[5], -a[2], a[6], 0); r->sp = 0; break;
            case 36: NEED(9); rt_curve(r, a[0], a[1], a[2], a[3], a[4], 0); rt_curve(r, a[5], 0, a[6], a[7], a[8], -(a[1] + a[3] + a[7])); r->sp = 0; break;
            case 37: { NEED(11); double dx = a[0] + a[2] + a[4] + a[6] + a[8], dy = a[1] + a[3] + a[5] + a[7] + a[9];
                rt_curve(r, a[0], a[1], a[2], a[3], a[4], a[5]);
                if (fabs(dx) > fabs(dy)) rt_curve(r, a[6], a[7], a[8], a[9], a[10], -dy); else rt_curve(r, a[6], a[7], a[8], a[9], -dx, a[10]);
                r->sp = 0; break; }
            case 9: NEED(1); a[ns - 1] = fabs(a[ns - 1]); break;
            case 10: NEED(2); a[ns - 2] += a[ns - 1]; r->sp--; break;
            case 11: NEED(2); a[ns - 2] -= a[ns - 1]; r->sp--; break;
            case 12: NEED(2); a[ns - 2] = a[ns - 1] != 0 ? a[ns - 2] / a[ns - 1] : 0; r->sp--; break;
            case 14: NEED(1); a[ns - 1] = -a[ns - 1]; break;
            case 18: NEED(1); r->sp--; break;
            case 24: NEED(2); a[ns - 2] *= a[ns - 1]; r->sp--; break;
            case 26: NEED(1); a[ns - 1] = sqrt(fabs(a[ns - 1])); break;
            case 27: NEED(1); if (ns >= 96) return -1; a[ns] = a[ns - 1]; r->sp++; break;
            case 28: NEED(2); { double t = a[ns - 1]; a[ns - 1] = a[ns - 2]; a[ns - 2] = t; } break;
            default: r->sp = 0; break;                                                    /* неизвестные/редкие операторы — сбрасываем стек */
            }
            break; }
        default: r->sp = 0; break;
        }
        #undef NEED
    }
    return 0;
}
int cff_glyph(const CffFont* f, int gid, const CffSink* sink) {
    if (!f || gid < 0 || gid >= f->nglyphs) return 0;
    const uint8_t* cs; size_t cl; if (!idx_get(&f->charstrings, (uint32_t)gid, &cs, &cl)) return 0;
    RT r; memset(&r, 0, sizeof r); r.f = f; r.sink = sink;
    if (f->is_cid) { int fd = fd_of(f, gid); r.pv = fd < f->nfd ? &f->fds[fd] : NULL; } else r.pv = &f->priv0;
    int rc = run(&r, cs, cl, 0);
    if (r.open) sink->close(sink->ud);
    return rc >= 0;
}
