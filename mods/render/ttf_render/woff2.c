/* WOFF2 → sfnt (W3C WOFF File Format 2.0). Разбор и пересборка с нуля; Brotli — brotli.c. */
#include "woff2.h"
#include "brotli.h"
#include <stdlib.h>
#include <string.h>

static const char* const kKnown[63] = {"cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf", "loca", "prep", "CFF ", "VORG", "EBDT",
    "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL",
    "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar", "mort", "morx", "opbd", "prop",
    "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"};

typedef struct { uint8_t tag[4]; uint8_t flags; uint32_t orig_len, tlen; const uint8_t* data; uint8_t* built; size_t built_len; int transformed; } Tbl;
typedef struct { const uint8_t* p; size_t n, pos; int err; } Rd;
static uint32_t rd8(Rd* r) { if (r->pos + 1 > r->n) { r->err = 1; return 0; } return r->p[r->pos++]; }
static uint32_t rd16(Rd* r) { if (r->pos + 2 > r->n) { r->err = 1; return 0; } uint32_t v = (uint32_t)r->p[r->pos] << 8 | r->p[r->pos + 1]; r->pos += 2; return v; }
static uint32_t rd32(Rd* r) { uint32_t a = rd16(r); return a << 16 | rd16(r); }
static uint32_t base128(Rd* r) {                                                          /* UIntBase128: до 5 байт, старший бит — «есть продолжение» */
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        uint32_t b = rd8(r); if (r->err) return 0; if (i == 0 && b == 0x80) { r->err = 1; return 0; }
        if (v & 0xFE000000u) { r->err = 1; return 0; }
        v = v << 7 | (b & 0x7F); if (!(b & 0x80)) return v;
    }
    r->err = 1; return 0;
}
static uint32_t u255(Rd* r) {                                                             /* 255UInt16 */
    uint32_t b = rd8(r);
    if (b == 253) return rd16(r);
    if (b == 255) return 253 + rd8(r);
    if (b == 254) return 506 + rd8(r);
    return b;
}
typedef struct { uint8_t* p; size_t n, cap; int err; } Buf;
static void bput(Buf* b, const void* d, size_t n) {
    if (b->err) return;
    if (b->n + n > b->cap) { size_t nc = b->cap ? b->cap : 4096; while (nc < b->n + n) nc *= 2; uint8_t* nv = realloc(b->p, nc); if (!nv) { b->err = 1; return; } b->p = nv; b->cap = nc; }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void b8(Buf* b, uint32_t v) { uint8_t c = (uint8_t)v; bput(b, &c, 1); }
static void b16(Buf* b, uint32_t v) { uint8_t c[2] = {(uint8_t)(v >> 8), (uint8_t)v}; bput(b, c, 2); }
static void b32(Buf* b, uint32_t v) { uint8_t c[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; bput(b, c, 4); }
static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ── glyf/loca: обратное преобразование ── */
typedef struct { int16_t xmin; } GInfo;
static int with_sign(int flag, int v) { return (flag & 1) ? v : -v; }

static int rebuild_glyf(const uint8_t* d, size_t n, Tbl* glyf, Tbl* loca, GInfo** ginfo_out, uint32_t* nglyphs_out) {
    Rd h = {d, n, 0, 0}; rd16(&h); uint32_t opt = rd16(&h), ng = rd16(&h), index_fmt = rd16(&h);
    uint32_t sz[7]; for (int i = 0; i < 7; i++) sz[i] = rd32(&h);
    if (h.err) return 0;
    size_t off = h.pos; const uint8_t* st[7]; size_t total = 0;
    for (int i = 0; i < 7; i++) { if (total + sz[i] > n - off) return 0; st[i] = d + off + total; total += sz[i]; }
    const uint8_t* overlap = NULL; if (opt & 1) { size_t ob = (ng + 7) >> 3; if (off + total + ob > n) return 0; overlap = d + off + total; }
    Rd ncont = {st[0], sz[0], 0, 0}, npts = {st[1], sz[1], 0, 0}, flg = {st[2], sz[2], 0, 0}, gly = {st[3], sz[3], 0, 0}, comp = {st[4], sz[4], 0, 0}, ins = {st[6], sz[6], 0, 0};
    size_t bbox_bm = ((ng + 31) >> 5) << 2; if (bbox_bm > sz[5]) return 0;
    const uint8_t* bbm = st[5]; Rd bbx = {st[5] + bbox_bm, sz[5] - bbox_bm, 0, 0};
    Buf out; memset(&out, 0, sizeof out); uint32_t* offs = malloc(((size_t)ng + 1) * sizeof(uint32_t)); GInfo* gi = calloc(ng ? ng : 1, sizeof(GInfo)); if (!offs || !gi) { free(offs); free(gi); return 0; }
    int ok = 0;
    for (uint32_t g = 0; g < ng; g++) {
        offs[g] = (uint32_t)out.n;
        int nc = (int16_t)rd16(&ncont); if (ncont.err) goto done;
        if (nc == 0) continue;
        int have_bbox = (bbm[g >> 3] & (0x80 >> (g & 7))) != 0;
        uint8_t bb[8]; int bbox[4] = {0, 0, 0, 0};
        if (nc > 0) {
            uint32_t* endp = malloc((size_t)nc * sizeof(uint32_t)); if (!endp) goto done; uint32_t total_pts = 0;
            for (int c = 0; c < nc; c++) { uint32_t np = u255(&npts); total_pts += np; endp[c] = total_pts - 1; }
            if (npts.err || total_pts > 70000) { free(endp); goto done; }
            int32_t* xs = malloc((size_t)(total_pts ? total_pts : 1) * 4 * 2); uint8_t* fl = malloc(total_pts ? total_pts : 1); if (!xs || !fl) { free(endp); free(xs); free(fl); goto done; }
            int32_t* ys = xs + total_pts; int x = 0, y = 0;
            if (flg.pos + total_pts > flg.n) { free(endp); free(xs); free(fl); goto done; }
            for (uint32_t i = 0; i < total_pts; i++) {
                uint32_t f = flg.p[flg.pos++]; int on = !(f >> 7); f &= 0x7F; int dx, dy, nb = f < 84 ? 1 : f < 120 ? 2 : f < 124 ? 3 : 4;
                if (gly.pos + (size_t)nb > gly.n) { free(endp); free(xs); free(fl); goto done; }
                const uint8_t* t = gly.p + gly.pos;
                if (f < 10) { dx = 0; dy = with_sign((int)f, (int)(((f & 14) << 7) + t[0])); }
                else if (f < 20) { dx = with_sign((int)f, (int)((((f - 10) & 14) << 7) + t[0])); dy = 0; }
                else if (f < 84) { int b0 = (int)f - 20, b1 = t[0]; dx = with_sign((int)f, 1 + (b0 & 0x30) + (b1 >> 4)); dy = with_sign((int)f >> 1, 1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F)); }
                else if (f < 120) { int b0 = (int)f - 84; dx = with_sign((int)f, 1 + ((b0 / 12) << 8) + t[0]); dy = with_sign((int)f >> 1, 1 + (((b0 % 12) >> 2) << 8) + t[1]); }
                else if (f < 124) { dx = with_sign((int)f, (t[0] << 4) + (t[1] >> 4)); dy = with_sign((int)f >> 1, ((t[1] & 0x0F) << 8) + t[2]); }
                else { dx = with_sign((int)f, (t[0] << 8) + t[1]); dy = with_sign((int)f >> 1, (t[2] << 8) + t[3]); }
                gly.pos += (size_t)nb; x += dx; y += dy; xs[i] = x; ys[i] = y; fl[i] = (uint8_t)on;
            }
            uint32_t ilen = u255(&gly); if (gly.err || ins.pos + ilen > ins.n) { free(endp); free(xs); free(fl); goto done; }
            int xmin = 32767, ymin = 32767, xmax = -32768, ymax = -32768;
            for (uint32_t i = 0; i < total_pts; i++) { if (xs[i] < xmin) xmin = xs[i]; if (xs[i] > xmax) xmax = xs[i]; if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
            if (total_pts == 0) xmin = ymin = xmax = ymax = 0;
            if (have_bbox) { Rd* r = &bbx; for (int i = 0; i < 4; i++) bbox[i] = (int16_t)rd16(r); if (r->err) { free(endp); free(xs); free(fl); goto done; } }
            else { bbox[0] = xmin; bbox[1] = ymin; bbox[2] = xmax; bbox[3] = ymax; }
            gi[g].xmin = (int16_t)bbox[0];
            b16(&out, (uint32_t)nc); for (int i = 0; i < 4; i++) b16(&out, (uint32_t)bbox[i]);
            for (int c = 0; c < nc; c++) b16(&out, endp[c]);
            b16(&out, ilen); bput(&out, ins.p + ins.pos, ilen); ins.pos += ilen;
            /* флаги с повторами + координаты в компактной записи TrueType */
            uint8_t* fo = malloc(total_pts ? total_pts : 1); Buf xb, yb; memset(&xb, 0, sizeof xb); memset(&yb, 0, sizeof yb);
            int px = 0, py = 0; if (!fo) { free(endp); free(xs); free(fl); goto done; }
            for (uint32_t i = 0; i < total_pts; i++) {
                int ddx = xs[i] - px, ddy = ys[i] - py; uint8_t f = fl[i] ? 1 : 0;
                if (i == 0 && overlap && (overlap[g >> 3] & (0x80 >> (g & 7)))) f |= 0x40;
                if (ddx == 0) f |= 0x10; else if (ddx >= -255 && ddx <= 255) { f |= 0x02; if (ddx > 0) f |= 0x10; b8(&xb, (uint32_t)(ddx < 0 ? -ddx : ddx)); } else b16(&xb, (uint32_t)(int16_t)ddx);
                if (ddy == 0) f |= 0x20; else if (ddy >= -255 && ddy <= 255) { f |= 0x04; if (ddy > 0) f |= 0x20; b8(&yb, (uint32_t)(ddy < 0 ? -ddy : ddy)); } else b16(&yb, (uint32_t)(int16_t)ddy);
                fo[i] = f; px = xs[i]; py = ys[i];
            }
            for (uint32_t i = 0; i < total_pts;) {
                uint32_t j = i + 1; while (j < total_pts && fo[j] == fo[i] && j - i < 256) j++;
                if (j - i >= 2) { b8(&out, fo[i] | 0x08); b8(&out, j - i - 1); } else b8(&out, fo[i]);
                i = j;
            }
            bput(&out, xb.p, xb.n); bput(&out, yb.p, yb.n); free(xb.p); free(yb.p); free(fo); free(endp); free(xs); free(fl);
        } else {                                                                                  /* составной глиф */
            Buf cb; memset(&cb, 0, sizeof cb); int more = 1, have_ins = 0;
            while (more) {
                uint32_t f = rd16(&comp), gid = rd16(&comp); if (comp.err) goto done;
                b16(&cb, f); b16(&cb, gid);
                size_t na = (f & 1) ? 4 : 2, nt = (f & 8) ? 2 : (f & 0x40) ? 4 : (f & 0x80) ? 8 : 0;
                if (comp.pos + na + nt > comp.n) { free(cb.p); goto done; }
                bput(&cb, comp.p + comp.pos, na + nt); comp.pos += na + nt; more = (f & 0x20) != 0; if (f & 0x100) have_ins = 1;
            }
            if (!have_bbox) { free(cb.p); goto done; }
            for (int i = 0; i < 4; i++) bbox[i] = (int16_t)rd16(&bbx); if (bbx.err) { free(cb.p); goto done; }
            gi[g].xmin = (int16_t)bbox[0];
            b16(&out, 0xFFFF); for (int i = 0; i < 4; i++) b16(&out, (uint32_t)bbox[i]);
            bput(&out, cb.p, cb.n); free(cb.p);
            if (have_ins) { uint32_t ilen = u255(&gly); if (gly.err || ins.pos + ilen > ins.n) goto done; b16(&out, ilen); bput(&out, ins.p + ins.pos, ilen); ins.pos += ilen; }
        }
        while (out.n & 3) b8(&out, 0);                                                            /* каждый глиф выравнивается по 4 байтам */
    }
    offs[ng] = (uint32_t)out.n;
    if (out.err) goto done;
    {
        Buf lb; memset(&lb, 0, sizeof lb);
        for (uint32_t g = 0; g <= ng; g++) { if (index_fmt == 0) b16(&lb, offs[g] >> 1); else b32(&lb, offs[g]); }
        if (lb.err) { free(lb.p); goto done; }
        glyf->built = out.p; glyf->built_len = out.n; out.p = NULL; loca->built = lb.p; loca->built_len = lb.n;
    }
    *ginfo_out = gi; gi = NULL; *nglyphs_out = ng; ok = 1;
done:
    free(out.p); free(offs); free(gi); return ok;
}

static int rebuild_hmtx(const uint8_t* d, size_t n, uint32_t nhm, uint32_t nglyphs, const GInfo* gi, Tbl* t) {
    if (n < 1) return 0; uint32_t fl = d[0]; d++; n--;
    if (fl & 0xFC) return 0; int has_lsb = !(fl & 1), has_lsb2 = !(fl & 2); if (has_lsb && has_lsb2) return 0;
    if (nhm > nglyphs || nhm == 0) return 0;
    size_t need = 2 * (size_t)nhm + (has_lsb ? 2 * (size_t)nhm : 0) + (has_lsb2 ? 2 * (size_t)(nglyphs - nhm) : 0); if (n < need) return 0;
    const uint8_t* adv = d; const uint8_t* lsb = adv + 2 * nhm; const uint8_t* lsb2 = lsb + (has_lsb ? 2 * nhm : 0);
    Buf o; memset(&o, 0, sizeof o);
    for (uint32_t i = 0; i < nhm; i++) { bput(&o, adv + 2 * i, 2); if (has_lsb) bput(&o, lsb + 2 * i, 2); else b16(&o, (uint32_t)(uint16_t)gi[i].xmin); }
    for (uint32_t i = nhm; i < nglyphs; i++) { if (has_lsb2) bput(&o, lsb2 + 2 * (i - nhm), 2); else b16(&o, (uint32_t)(uint16_t)gi[i].xmin); }
    if (o.err) { free(o.p); return 0; }
    t->built = o.p; t->built_len = o.n; return 1;
}
static uint32_t csum(const uint8_t* p, size_t n) { uint32_t s = 0; size_t i = 0; for (; i + 4 <= n; i += 4) s += be32(p + i); if (i < n) { uint8_t t[4] = {0, 0, 0, 0}; memcpy(t, p + i, n - i); s += be32(t); } return s; }
static int tagcmp(const void* a, const void* b) { return memcmp(((const Tbl*)a)->tag, ((const Tbl*)b)->tag, 4); }

uint8_t* woff2_to_sfnt(const uint8_t* in, size_t in_len, size_t* out_len) {
    Rd r = {in, in_len, 0, 0};
    if (in_len < 48 || memcmp(in, "wOF2", 4)) return NULL;
    rd32(&r); uint32_t flavor = rd32(&r); rd32(&r); uint32_t nt = rd16(&r); rd16(&r); rd32(&r); uint32_t comp_size = rd32(&r); r.pos = 48;
    if (r.err || nt == 0 || nt > 200 || flavor == 0x74746366u) return NULL;                       /* коллекции (ttcf) не поддерживаем */
    Tbl* t = calloc(nt, sizeof(Tbl)); if (!t) return NULL;
    uint8_t* sfnt = NULL; uint8_t* raw = NULL; GInfo* gi = NULL; uint32_t ngl = 0; size_t total_raw = 0; int i;
    for (i = 0; i < (int)nt; i++) {
        uint32_t fl = rd8(&r); t[i].flags = (uint8_t)fl;
        if ((fl & 0x3F) == 0x3F) { for (int k = 0; k < 4; k++) t[i].tag[k] = (uint8_t)rd8(&r); } else memcpy(t[i].tag, kKnown[fl & 0x3F], 4);
        t[i].orig_len = base128(&r); int ver = (int)(fl >> 6); int is_gl = !memcmp(t[i].tag, "glyf", 4) || !memcmp(t[i].tag, "loca", 4);
        t[i].transformed = is_gl ? ver != 3 : ver != 0; t[i].tlen = t[i].orig_len;
        if (t[i].transformed) { t[i].tlen = base128(&r); if (!memcmp(t[i].tag, "loca", 4) && t[i].tlen != 0) goto fail; }
        if (r.err) goto fail;
        total_raw += t[i].tlen; if (total_raw > (size_t)256 << 20) goto fail;
    }
    if (r.pos + comp_size > in_len) goto fail;
    size_t rl = 0; raw = brotli_decode(in + r.pos, comp_size, total_raw, total_raw + 16, &rl); if (!raw || rl != total_raw) goto fail;
    size_t pos = 0; Tbl *tglyf = NULL, *tloca = NULL, *thmtx = NULL, *thhea = NULL, *tmaxp = NULL;
    for (i = 0; i < (int)nt; i++) {
        t[i].data = raw + pos; pos += t[i].tlen;
        if (!memcmp(t[i].tag, "glyf", 4)) tglyf = &t[i]; else if (!memcmp(t[i].tag, "loca", 4)) tloca = &t[i];
        else if (!memcmp(t[i].tag, "hmtx", 4)) thmtx = &t[i]; else if (!memcmp(t[i].tag, "hhea", 4)) thhea = &t[i]; else if (!memcmp(t[i].tag, "maxp", 4)) tmaxp = &t[i];
    }
    if (tglyf && tglyf->transformed) { if (!tloca || !rebuild_glyf(tglyf->data, tglyf->tlen, tglyf, tloca, &gi, &ngl)) goto fail; }
    if (thmtx && thmtx->transformed) {
        if (!gi || !thhea || thhea->transformed || thhea->tlen < 36 || !tmaxp) goto fail;
        uint32_t nhm = be16(thhea->data + 34); if (!rebuild_hmtx(thmtx->data, thmtx->tlen, nhm, ngl, gi, thmtx)) goto fail;
    }
    for (i = 0; i < (int)nt; i++) if (!t[i].built && (&t[i] == tglyf ? 0 : 1)) { /* таблицы без преобразования отдаются как есть */ }
    qsort(t, nt, sizeof(Tbl), tagcmp);
    size_t hdr = 12 + 16 * (size_t)nt, size = hdr;
    for (i = 0; i < (int)nt; i++) { size_t l = t[i].built ? t[i].built_len : t[i].tlen; size += (l + 3) & ~(size_t)3; }
    sfnt = calloc(size + 4, 1); if (!sfnt) goto fail;
    uint32_t pw = 1, es = 0; while ((pw << 1) <= nt) { pw <<= 1; es++; }
    sfnt[0] = (uint8_t)(flavor >> 24); sfnt[1] = (uint8_t)(flavor >> 16); sfnt[2] = (uint8_t)(flavor >> 8); sfnt[3] = (uint8_t)flavor;
    sfnt[4] = (uint8_t)(nt >> 8); sfnt[5] = (uint8_t)nt; sfnt[6] = (uint8_t)((pw * 16) >> 8); sfnt[7] = (uint8_t)(pw * 16); sfnt[8] = (uint8_t)(es >> 8); sfnt[9] = (uint8_t)es;
    sfnt[10] = (uint8_t)(((nt - pw) * 16) >> 8); sfnt[11] = (uint8_t)((nt - pw) * 16);
    size_t off = hdr; size_t head_off = 0;
    for (i = 0; i < (int)nt; i++) {
        const uint8_t* src = t[i].built ? t[i].built : t[i].data; size_t l = t[i].built ? t[i].built_len : t[i].tlen;
        memcpy(sfnt + off, src, l);
        uint8_t* e = sfnt + 12 + 16 * (size_t)i; memcpy(e, t[i].tag, 4);
        if (!memcmp(t[i].tag, "head", 4)) { head_off = off; if (l >= 12) memset(sfnt + off + 8, 0, 4); }       /* checkSumAdjustment считается ниже */
        uint32_t cs = csum(sfnt + off, l); e[4] = (uint8_t)(cs >> 24); e[5] = (uint8_t)(cs >> 16); e[6] = (uint8_t)(cs >> 8); e[7] = (uint8_t)cs;
        e[8] = (uint8_t)(off >> 24); e[9] = (uint8_t)(off >> 16); e[10] = (uint8_t)(off >> 8); e[11] = (uint8_t)off; e[12] = (uint8_t)(l >> 24); e[13] = (uint8_t)(l >> 16); e[14] = (uint8_t)(l >> 8); e[15] = (uint8_t)l;
        off += (l + 3) & ~(size_t)3;
    }
    if (head_off) { uint32_t total = csum(sfnt, size); uint32_t adj = 0xB1B0AFBAu - total; sfnt[head_off + 8] = (uint8_t)(adj >> 24); sfnt[head_off + 9] = (uint8_t)(adj >> 16); sfnt[head_off + 10] = (uint8_t)(adj >> 8); sfnt[head_off + 11] = (uint8_t)adj; }
    *out_len = size;
    for (i = 0; i < (int)nt; i++) free(t[i].built);
    free(t); free(raw); free(gi); return sfnt;
fail:
    for (int k = 0; k < (int)nt; k++) free(t[k].built);
    free(t); free(raw); free(gi); free(sfnt); return NULL;
}
