/* Декодер Brotli по RFC 7932. Устроен просто: весь результат пишется в один растущий буфер (окно = уже выданные данные),
   коды Хаффмана декодируются по счётчикам длин (как в deflate; в Brotli порядок битов тот же). */
#include "brotli.h"
#include <stdlib.h>
#include <string.h>
#include "brotli_tables.inc"

typedef struct { const uint8_t* in; size_t n, pos; uint64_t acc; int nacc; int err; size_t over; } BR;      /* биты читаются с младшего */
static void br_fill(BR* b, int need) {
    while (b->nacc < need) {
        uint64_t byte = 0; if (b->pos < b->n) byte = b->in[b->pos++]; else b->over++;                       /* за концом входа — нули; ошибка, если их реально «потратили» */
        b->acc |= byte << b->nacc; b->nacc += 8;
    }
}
static uint32_t br_peek(BR* b, int n) { br_fill(b, n); return (uint32_t)(b->acc & ((1ull << n) - 1)); }
static void br_drop(BR* b, int n) { b->acc >>= n; b->nacc -= n; if (b->over * 8 > (size_t)b->nacc) b->err = 1; }   /* nacc считает и фиктивные биты */
static uint32_t br_read(BR* b, int n) { if (n == 0) return 0; uint32_t v = br_peek(b, n); br_drop(b, n); return v; }
static void br_align(BR* b) { int r = b->nacc & 7; if (r) br_drop(b, r); }                              /* дальше читаем целыми байтами */

typedef struct { uint16_t count[16]; uint16_t* sym; int single; int nsym; } Huff;
static void huff_free(Huff* h) { free(h->sym); h->sym = NULL; }
/* строит декодер по длинам кодов; 1 — корректный полный код */
static int huff_build(Huff* h, const uint8_t* len, int n) {
    memset(h->count, 0, sizeof h->count); h->single = 0; h->nsym = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    uint16_t offs[17]; offs[1] = 0; int left = 1;
    for (int l = 1; l <= 15; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return 0; offs[l + 1] = (uint16_t)(offs[l] + h->count[l]); }
    h->sym = malloc((size_t)(n > 1 ? n : 1) * sizeof(uint16_t)); if (!h->sym) return 0;
    for (int i = 0; i < n; i++) if (len[i]) { h->sym[offs[len[i]]++] = (uint16_t)i; h->nsym++; }
    return left == 0;
}
static int huff_dec(BR* b, const Huff* h) {
    if (h->single) return h->sym[0];
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; l++) {
        code |= (int)br_read(b, 1); int c = h->count[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    b->err = 1; return 0;
}
static int huff_single(Huff* h, int sym) { memset(h->count, 0, sizeof h->count); h->sym = malloc(2); if (!h->sym) return 0; h->sym[0] = (uint16_t)sym; h->single = 1; h->nsym = 1; return 1; }

/* чтение префиксного кода для алфавита из alpha символов (RFC 7932, разд. 3.4 и 3.5) */
static int read_prefix_code(BR* b, Huff* h, int alpha) {
    int hskip = (int)br_read(b, 2);
    if (hskip == 1) {                                                                                   /* простой код */
        int nsym = (int)br_read(b, 2) + 1; int bits = 0; while ((1 << bits) < alpha) bits++;
        int s[4];
        for (int i = 0; i < nsym; i++) { s[i] = (int)br_read(b, bits); if (s[i] >= alpha) return 0; for (int j = 0; j < i; j++) if (s[j] == s[i]) return 0; }
        if (nsym == 1) return huff_single(h, s[0]);
        uint8_t lens[1024]; if (alpha > 1024) return 0; memset(lens, 0, (size_t)alpha);
        if (nsym == 2) { lens[s[0]] = 1; lens[s[1]] = 1; }
        else if (nsym == 3) { lens[s[0]] = 1; lens[s[1]] = 2; lens[s[2]] = 2; }
        else { int ts = (int)br_read(b, 1); if (!ts) { for (int i = 0; i < 4; i++) lens[s[i]] = 2; } else { lens[s[0]] = 1; lens[s[1]] = 2; lens[s[2]] = 3; lens[s[3]] = 3; } }
        return huff_build(h, lens, alpha);
    }
    static const uint8_t order[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t cl_len[16] = {2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4};
    static const uint8_t cl_val[16] = {0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5};
    uint8_t cl[18]; memset(cl, 0, sizeof cl); int space = 32, ncodes = 0;
    for (int i = hskip; i < 18 && space > 0; i++) {
        uint32_t p = br_peek(b, 4); br_drop(b, cl_len[p]); int v = cl_val[p]; cl[order[i]] = (uint8_t)v;
        if (v) { space -= 32 >> v; ncodes++; }
        if (b->err) return 0;
    }
    if (!(ncodes == 1 || space == 0)) return 0;
    Huff clh; memset(&clh, 0, sizeof clh);
    if (ncodes == 1) { int only = 0; for (int i = 0; i < 18; i++) if (cl[i]) only = i; if (!huff_single(&clh, only)) return 0; }
    else if (!huff_build(&clh, cl, 18)) { huff_free(&clh); return 0; }
    uint8_t* lens = calloc((size_t)alpha, 1); if (!lens) { huff_free(&clh); return 0; }
    int sym = 0, sp = 32768, prev = 8, repeat = 0, rlen = 0;
    while (sym < alpha && sp > 0) {
        int c = huff_dec(b, &clh); if (b->err) break;
        if (c < 16) { repeat = 0; lens[sym++] = (uint8_t)c; if (c) { prev = c; sp -= 32768 >> c; } }
        else {
            int eb = c == 16 ? 2 : 3, nl = c == 16 ? prev : 0;
            if (rlen != nl) { repeat = 0; rlen = nl; }
            int old = repeat; if (repeat > 0) repeat = (repeat - 2) << eb;
            repeat += (int)br_read(b, eb) + 3; int delta = repeat - old;
            if (sym + delta > alpha) { b->err = 1; break; }
            for (int i = 0; i < delta; i++) lens[sym++] = (uint8_t)rlen;
            if (rlen) sp -= delta << (15 - rlen);
        }
    }
    huff_free(&clh);
    int ok = !b->err && sp == 0 && huff_build(h, lens, alpha); free(lens); return ok;
}

static uint32_t read_varlen(BR* b) {                                                                    /* NBLTYPES / NTREES */
    if (!br_read(b, 1)) return 1; int n = (int)br_read(b, 3); return (1u << n) + br_read(b, n) + 1;
}
static int imtf_and_map(BR* b, uint8_t* map, size_t size, int ntrees) {                                 /* карта контекстов (разд. 7.3) */
    int rlemax = br_read(b, 1) ? (int)br_read(b, 4) + 1 : 0; Huff h; memset(&h, 0, sizeof h);
    if (!read_prefix_code(b, &h, ntrees + rlemax)) { huff_free(&h); return 0; }
    size_t i = 0;
    while (i < size) {
        int s = huff_dec(b, &h); if (b->err) { huff_free(&h); return 0; }
        if (s == 0) map[i++] = 0;
        else if (s <= rlemax) { size_t cnt = (1u << s) + br_read(b, s); if (i + cnt > size) { huff_free(&h); return 0; } while (cnt--) map[i++] = 0; }
        else map[i++] = (uint8_t)(s - rlemax);
    }
    huff_free(&h);
    if (br_read(b, 1)) { uint8_t mtf[256]; for (int k = 0; k < 256; k++) mtf[k] = (uint8_t)k; for (size_t k = 0; k < size; k++) { int idx = map[k]; uint8_t v = mtf[idx]; memmove(mtf + 1, mtf, (size_t)idx); mtf[0] = v; map[k] = v; } }
    return !b->err;
}

typedef struct { uint32_t ntypes; Huff tcode, ccode; uint32_t type, prev, count; } BlockCat;
static const uint16_t kBlkBase[26] = {1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113, 145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289, 4337, 8433, 16625};
static const uint8_t kBlkExtra[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};
static void blk_count(BR* b, BlockCat* c) { int s = huff_dec(b, &c->ccode); if (s >= 26) { b->err = 1; return; } c->count = kBlkBase[s] + br_read(b, kBlkExtra[s]); }
static void blk_switch(BR* b, BlockCat* c) {
    int s = huff_dec(b, &c->tcode); uint32_t t = s == 0 ? c->prev : s == 1 ? c->type + 1 : (uint32_t)(s - 2);
    if (t >= c->ntypes) t -= c->ntypes;
    if (t >= c->ntypes) { b->err = 1; return; }
    c->prev = c->type; c->type = t; blk_count(b, c);
}
static int blk_init(BR* b, BlockCat* c) {                                                              /* NBLTYPES + коды типов/длин блоков + первая длина */
    memset(c, 0, sizeof *c); c->ntypes = read_varlen(b); c->prev = 1; c->type = 0;
    if (c->ntypes >= 2) {
        if (c->ntypes > 256) return 0;
        if (!read_prefix_code(b, &c->tcode, (int)c->ntypes + 2) || !read_prefix_code(b, &c->ccode, 26)) return 0;
        blk_count(b, c);
    } else c->count = 1u << 28;
    return !b->err;
}
static void blk_free(BlockCat* c) { huff_free(&c->tcode); huff_free(&c->ccode); }

static const uint16_t kInsBase[24] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26, 34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
static const uint8_t kInsExtra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const uint16_t kCopyBase[24] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
static const uint8_t kCopyExtra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};
static const uint8_t kCellIns[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
static const uint8_t kCellCopy[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};
static const uint8_t kDictBits[25] = {0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5};
static const uint32_t kDictOff[25] = {0, 0, 0, 0, 0, 4096, 9216, 21504, 35840, 44032, 53248, 63488, 74752, 87040, 93696, 100864, 104704, 106752, 108928, 113536, 115968, 118528, 119872, 121280, 122016};

typedef struct { uint8_t* p; size_t n, cap, max; int err; } Out;
static int out_need_ex(Out* o, size_t extra, int enforce) {                                              /* enforce=0: только место в буфере (длина слова словаря известна лишь после преобразования) */
    if (enforce && o->max && o->n + extra > o->max) { o->err = 1; return 0; }
    if (o->n + extra + 1 > o->cap) { size_t nc = o->cap ? o->cap : 65536; while (nc < o->n + extra + 1) nc *= 2; uint8_t* nv = realloc(o->p, nc); if (!nv) { o->err = 1; return 0; } o->p = nv; o->cap = nc; }
    return 1;
}
static int out_need(Out* o, size_t extra) { return out_need_ex(o, extra, 1); }
static size_t dict_word(uint8_t* dst, int len, uint32_t word_id, uint32_t tid) {                          /* слово словаря + преобразование (разд. 8) */
    const uint8_t* w = kBrDict + kDictOff[len] + (size_t)(word_id & ((1u << kDictBits[len]) - 1)) * (size_t)len;
    const uint8_t* t = kBrTransforms + tid * 3; const uint8_t* pre = kBrPrefixSuffix + kBrPrefixSuffixMap[t[0]]; const uint8_t* suf = kBrPrefixSuffix + kBrPrefixSuffixMap[t[2]];
    size_t o = 0; memcpy(dst + o, pre + 1, pre[0]); o += pre[0];
    int type = t[1], skip_first = 0, cut_last = 0;
    if (type >= 1 && type <= 9) cut_last = type; else if (type >= 12 && type <= 20) skip_first = type - 11;
    int wl = len - skip_first - cut_last; if (wl < 0) wl = 0;
    uint8_t* wd = dst + o; memcpy(wd, w + skip_first, (size_t)wl); o += (size_t)wl;
    if (type == 10 || type == 11) {                                                                       /* UPPERCASE_FIRST / ALL по UTF-8 */
        int i = 0;
        while (i < wl) {
            if (wd[i] < 0xC0) { if (wd[i] >= 'a' && wd[i] <= 'z') wd[i] ^= 32; i += 1; }
            else if (wd[i] < 0xE0) { if (i + 1 < wl) wd[i + 1] ^= 32; i += 2; }
            else { if (i + 2 < wl) wd[i + 2] ^= 5; i += 3; }
            if (type == 10) break;
        }
    }
    memcpy(dst + o, suf + 1, suf[0]); o += suf[0]; return o;
}

uint8_t* brotli_decode(const uint8_t* in, size_t in_len, size_t size_hint, size_t max_out, size_t* out_len) {
    BR b; memset(&b, 0, sizeof b); b.in = in; b.n = in_len;
    Out o; memset(&o, 0, sizeof o); o.max = max_out; if (size_hint && (!max_out || size_hint <= max_out)) { o.p = malloc(size_hint + 1); o.cap = o.p ? size_hint + 1 : 0; }
    int wbits;
    if (!br_read(&b, 1)) wbits = 16;
    else { int n = (int)br_read(&b, 3); if (n) wbits = 17 + n; else { int m = (int)br_read(&b, 3); if (m == 1) goto fail; wbits = m == 0 ? 17 : 8 + m; } }
    size_t window = ((size_t)1 << wbits) - 16;
    uint32_t ring[4] = {16, 15, 11, 4};                                                                   /* ring[3] — последняя дистанция */
    for (;;) {
        int islast = (int)br_read(&b, 1); if (islast && br_read(&b, 1)) break;
        int nn = (int)br_read(&b, 2); size_t mlen = 0;
        if (nn == 3) {                                                                                    /* метаданные: пропускаем */
            if (br_read(&b, 1)) goto fail; int sb = (int)br_read(&b, 2); size_t skip = 0;
            for (int i = 0; i < sb; i++) skip |= (size_t)br_read(&b, 8) << (8 * i); if (sb) skip += 1;
            br_align(&b); if (b.err) goto fail;
            size_t have = (size_t)(b.nacc / 8); while (have && skip) { br_drop(&b, 8); have--; skip--; }
            if (skip) { if (b.pos + skip > b.n) goto fail; b.pos += skip; }
            if (islast) break; continue;
        }
        nn += 4; for (int i = 0; i < nn; i++) mlen |= (size_t)br_read(&b, 4) << (4 * i); mlen += 1;
        if (nn > 4 && !(mlen - 1 >> (4 * (nn - 1)))) goto fail;
        if (b.err) goto fail;
        if (!islast && br_read(&b, 1)) {                                                                  /* несжатый блок */
            br_align(&b); if (b.err) goto fail; if (!out_need(&o, mlen)) goto fail;
            for (size_t i = 0; i < mlen; i++) { if (b.nacc >= 8) o.p[o.n++] = (uint8_t)br_read(&b, 8); else if (b.pos < b.n) o.p[o.n++] = b.in[b.pos++]; else goto fail; }
            continue;
        }
        BlockCat bl, bi, bd; memset(&bl, 0, sizeof bl); memset(&bi, 0, sizeof bi); memset(&bd, 0, sizeof bd);
        uint8_t *cmapL = NULL, *cmapD = NULL, *cmodes = NULL; Huff *hl = NULL, *hi = NULL, *hd = NULL; uint32_t ntl = 0, ntd = 0; int ok = 0;
        if (!blk_init(&b, &bl) || !blk_init(&b, &bi) || !blk_init(&b, &bd)) goto blockfail;
        int npostfix = (int)br_read(&b, 2); int ndirect = (int)br_read(&b, 4) << npostfix;
        cmodes = malloc(bl.ntypes); if (!cmodes) goto blockfail;
        for (uint32_t i = 0; i < bl.ntypes; i++) cmodes[i] = (uint8_t)br_read(&b, 2);
        ntl = read_varlen(&b); if (ntl > 256) goto blockfail;
        cmapL = calloc(64 * bl.ntypes, 1); if (!cmapL) goto blockfail;
        if (ntl >= 2) { if (!imtf_and_map(&b, cmapL, 64 * bl.ntypes, (int)ntl)) goto blockfail; }
        ntd = read_varlen(&b); if (ntd > 256) goto blockfail;
        cmapD = calloc(4 * bd.ntypes, 1); if (!cmapD) goto blockfail;
        if (ntd >= 2) { if (!imtf_and_map(&b, cmapD, 4 * bd.ntypes, (int)ntd)) goto blockfail; }
        hl = calloc(ntl, sizeof(Huff)); hi = calloc(bi.ntypes, sizeof(Huff)); hd = calloc(ntd, sizeof(Huff)); if (!hl || !hi || !hd) goto blockfail;
        for (uint32_t i = 0; i < ntl; i++) if (!read_prefix_code(&b, &hl[i], 256)) goto blockfail;
        for (uint32_t i = 0; i < bi.ntypes; i++) if (!read_prefix_code(&b, &hi[i], 704)) goto blockfail;
        for (uint32_t i = 0; i < ntd; i++) if (!read_prefix_code(&b, &hd[i], 16 + ndirect + (48 << npostfix))) goto blockfail;
        if (b.err) goto blockfail;
        {
            size_t remain = mlen;
            while (remain > 0) {
                if (bi.count == 0) blk_switch(&b, &bi); bi.count--;
                int cmd = huff_dec(&b, &hi[bi.type]); if (b.err) goto blockfail;
                int cell = cmd >> 6; int ic = kCellIns[cell] + ((cmd >> 3) & 7), cc = kCellCopy[cell] + (cmd & 7);
                size_t ins = kInsBase[ic] + br_read(&b, kInsExtra[ic]), cpy = kCopyBase[cc] + br_read(&b, kCopyExtra[cc]);
                if (b.err || ins > remain) goto blockfail;
                if (!out_need(&o, ins)) goto blockfail;
                for (size_t i = 0; i < ins; i++) {
                    if (bl.count == 0) blk_switch(&b, &bl); bl.count--;
                    uint8_t p1 = o.n ? o.p[o.n - 1] : 0, p2 = o.n > 1 ? o.p[o.n - 2] : 0;
                    const uint8_t* lut = kBrCtxLut + cmodes[bl.type] * 512; int ctx = lut[p1] | lut[256 + p2];
                    int tree = ntl >= 2 ? cmapL[bl.type * 64 + ctx] : 0;
                    o.p[o.n++] = (uint8_t)huff_dec(&b, &hl[tree]); if (b.err) goto blockfail;
                }
                remain -= ins; if (remain == 0) break;
                uint32_t dist; int is_ring_update = 1;
                if (cell < 2) { dist = ring[3]; is_ring_update = 0; }
                else {
                    if (bd.count == 0) blk_switch(&b, &bd); bd.count--;
                    int dctx = cpy > 4 ? 3 : (int)cpy - 2; int tree = ntd >= 2 ? cmapD[bd.type * 4 + dctx] : 0;
                    int dc = huff_dec(&b, &hd[tree]); if (b.err) goto blockfail;
                    if (dc < 16) {
                        static const int8_t kAdd[16] = {0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3};
                        int64_t d = dc < 4 ? (int64_t)ring[3 - dc] : (int64_t)ring[dc < 10 ? 3 : 2] + kAdd[dc];
                        if (dc == 0) is_ring_update = 0; if (d <= 0) goto blockfail; dist = (uint32_t)d;
                    } else if (dc < 16 + ndirect) dist = (uint32_t)(dc - 15);
                    else {
                        int x = dc - ndirect - 16; int hc = x >> npostfix, lc = x & ((1 << npostfix) - 1); int nb = 1 + (hc >> 1);
                        uint32_t offs = ((2u + (uint32_t)(hc & 1)) << nb) - 4; dist = ((offs + br_read(&b, nb)) << npostfix) + (uint32_t)lc + (uint32_t)ndirect + 1;
                    }
                }
                size_t maxd = o.n < window ? o.n : window;
                if (dist > maxd) {                                                                        /* ссылка на словарь */
                    if (cpy < 4 || cpy > 24) goto blockfail;
                    uint32_t wid = dist - (uint32_t)maxd - 1; uint32_t tid = wid >> kDictBits[cpy]; if (tid >= 121) goto blockfail;
                    if (!out_need_ex(&o, 64, 0)) goto blockfail;
                    size_t wl = dict_word(o.p + o.n, (int)cpy, wid, tid); if (wl > remain || (o.max && o.n + wl > o.max)) goto blockfail;
                    o.n += wl; remain -= wl; continue;
                }
                if (is_ring_update) { ring[0] = ring[1]; ring[1] = ring[2]; ring[2] = ring[3]; ring[3] = dist; }
                if (cpy > remain) goto blockfail; if (!out_need(&o, cpy)) goto blockfail;
                for (size_t i = 0; i < cpy; i++) { o.p[o.n] = o.p[o.n - dist]; o.n++; }
                remain -= cpy;
            }
        }
        ok = 1;
blockfail:
        blk_free(&bl); blk_free(&bi); blk_free(&bd);
        if (hl) for (uint32_t i = 0; i < ntl; i++) huff_free(&hl[i]);
        if (hi) for (uint32_t i = 0; i < bi.ntypes; i++) huff_free(&hi[i]);
        if (hd) for (uint32_t i = 0; i < ntd; i++) huff_free(&hd[i]);
        free(hl); free(hi); free(hd); free(cmapL); free(cmapD); free(cmodes);
        if (!ok) goto fail;
        if (islast) break;
    }
    if (b.err || o.err) goto fail;
    if (!o.p) { o.p = malloc(1); if (!o.p) goto fail; o.cap = 1; }
    if (o.n + 1 > o.cap) { uint8_t* nv = realloc(o.p, o.n + 1); if (!nv) goto fail; o.p = nv; }
    o.p[o.n] = 0; *out_len = o.n; return o.p;
fail:
    free(o.p); return NULL;
}
