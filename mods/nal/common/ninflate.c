/* Распаковка DEFLATE/gzip/zlib. Алгоритм — каноническое декодирование Хаффмана по счётчикам длин кодов
   (по мотивам puff Марка Адлера), с растущим выходным буфером, проверкой CRC-32 (gzip) и Adler-32 (zlib). */
#include "ninflate.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const unsigned char* in; size_t inlen, incnt;
    uint32_t bitbuf; int bitcnt;
    unsigned char* out; size_t outlen, outcap, maxout;
    int err;
} St;
typedef struct { short count[MAXBITS + 1]; short symbol[FIXLCODES]; } Huff;

static int get_bits(St* s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) { s->err = 1; return 0; }
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt; s->bitcnt += 8;
    }
    s->bitbuf = val >> need; s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}
static int out_reserve(St* s, size_t extra) {
    if (s->maxout && s->outlen + extra > s->maxout) { s->err = 2; return 0; }
    if (s->outlen + extra + 1 > s->outcap) {
        size_t nc = s->outcap ? s->outcap : 4096; while (nc < s->outlen + extra + 1) nc *= 2;
        unsigned char* nv = realloc(s->out, nc); if (!nv) { s->err = 3; return 0; } s->out = nv; s->outcap = nc;
    }
    return 1;
}
static int decode_sym(St* s, const Huff* h) {
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= MAXBITS; len++) {
        code |= get_bits(s, 1); if (s->err) return -1;
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int construct(Huff* h, const short* length, int n) {
    int symbol, len, left; short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0; for (len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++) if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}
static int codes(St* s, const Huff* lencode, const Huff* distcode) {
    static const short lens[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    int symbol;
    do {
        symbol = decode_sym(s, lencode); if (symbol < 0) { s->err = s->err ? s->err : 4; return -1; }
        if (symbol < 256) { if (!out_reserve(s, 1)) return -1; s->out[s->outlen++] = (unsigned char)symbol; }
        else if (symbol > 256) {
            symbol -= 257; if (symbol >= 29) { s->err = 4; return -1; }
            int len = lens[symbol] + get_bits(s, lext[symbol]); if (s->err) return -1;
            symbol = decode_sym(s, distcode); if (symbol < 0 || symbol >= 30) { s->err = s->err ? s->err : 4; return -1; }
            size_t dist = (size_t)dists[symbol] + (size_t)get_bits(s, dext[symbol]); if (s->err) return -1;
            if (dist > s->outlen) { s->err = 4; return -1; }
            if (!out_reserve(s, (size_t)len)) return -1;
            for (int k = 0; k < len; k++) { s->out[s->outlen] = s->out[s->outlen - dist]; s->outlen++; }
        }
    } while (symbol != 256);
    return 0;
}
static int fixed_block(St* s) {
    static int virgin = 1; static Huff lencode, distcode;
    if (virgin) {
        short lengths[FIXLCODES]; int symbol;
        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        construct(&distcode, lengths, MAXDCODES); virgin = 0;
    }
    return codes(s, &lencode, &distcode);
}
static int dynamic_block(St* s) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[MAXLCODES + MAXDCODES]; Huff lencode, distcode;
    int nlen = get_bits(s, 5) + 257, ndist = get_bits(s, 5) + 1, ncode = get_bits(s, 4) + 4; if (s->err) return -1;
    if (nlen > MAXLCODES || ndist > MAXDCODES) { s->err = 4; return -1; }
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)get_bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->err) return -1;
    if (construct(&lencode, lengths, 19) != 0) { s->err = 4; return -1; }
    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode_sym(s, &lencode); if (symbol < 0) { s->err = s->err ? s->err : 4; return -1; }
        if (symbol < 16) lengths[index++] = (short)symbol;
        else {
            int len = 0, rep;
            if (symbol == 16) { if (index == 0) { s->err = 4; return -1; } len = lengths[index - 1]; rep = 3 + get_bits(s, 2); }
            else if (symbol == 17) rep = 3 + get_bits(s, 3); else rep = 11 + get_bits(s, 7);
            if (s->err || index + rep > nlen + ndist) { s->err = 4; return -1; }
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) { s->err = 4; return -1; }
    int err = construct(&lencode, lengths, nlen); if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) { s->err = 4; return -1; }
    err = construct(&distcode, lengths + nlen, ndist); if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) { s->err = 4; return -1; }
    return codes(s, &lencode, &distcode);
}
/* *used — сколько байт входа съедено (нужно для склейки gzip-членов и чтения трейлера) */
static unsigned char* inflate_raw(const unsigned char* in, size_t inlen, size_t* used, size_t* outlen, size_t maxout) {
    St s; memset(&s, 0, sizeof s); s.in = in; s.inlen = inlen; s.maxout = maxout;
    int last;
    do {
        last = get_bits(&s, 1); int type = get_bits(&s, 2); if (s.err) break;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.incnt + 4 > s.inlen) { s.err = 1; break; }
            unsigned len = s.in[s.incnt] | (s.in[s.incnt + 1] << 8), nlen = s.in[s.incnt + 2] | (s.in[s.incnt + 3] << 8); s.incnt += 4;
            if (len != (~nlen & 0xffff)) { s.err = 4; break; }
            if (s.incnt + len > s.inlen) { s.err = 1; break; }
            if (!out_reserve(&s, len)) break;
            memcpy(s.out + s.outlen, s.in + s.incnt, len); s.outlen += len; s.incnt += len;
        } else if (type == 1) { if (fixed_block(&s)) break; }
        else if (type == 2) { if (dynamic_block(&s)) break; }
        else { s.err = 4; break; }
    } while (!last);
    if (s.err) { free(s.out); return NULL; }
    if (!s.out) { s.out = malloc(1); if (!s.out) return NULL; }
    s.out[s.outlen] = 0; *outlen = s.outlen; if (used) *used = s.incnt;
    return s.out;
}
static uint32_t crc32_calc(const unsigned char* p, size_t n) {
    static uint32_t tab[256]; static int init = 0;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; } init = 1; }
    uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t adler32_calc(const unsigned char* p, size_t n) {
    uint32_t a = 1, b = 0; for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; } return (b << 16) | a;
}
unsigned char* n_deflate_raw(const unsigned char* in, size_t in_len, size_t* out_len, size_t max_out) {
    size_t used; return inflate_raw(in, in_len, &used, out_len, max_out);
}
unsigned char* n_gunzip(const unsigned char* in, size_t n, size_t* out_len, size_t max_out) {
    unsigned char* acc = NULL; size_t acclen = 0, pos = 0; int members = 0;
    while (pos + 18 <= n && in[pos] == 0x1f && in[pos + 1] == 0x8b) {                 /* допускаем несколько склеенных членов */
        if (in[pos + 2] != 8) goto fail;
        unsigned flg = in[pos + 3]; size_t p = pos + 10;
        if (flg & 4) { if (p + 2 > n) goto fail; size_t xlen = in[p] | (in[p + 1] << 8); p += 2 + xlen; }
        if (flg & 8) { while (p < n && in[p]) p++; p++; }
        if (flg & 16) { while (p < n && in[p]) p++; p++; }
        if (flg & 2) p += 2;
        if (p >= n) goto fail;
        size_t used = 0, ol = 0; unsigned char* o = inflate_raw(in + p, n - p, &used, &ol, max_out ? (max_out > acclen ? max_out - acclen : 1) : 0);
        if (!o) goto fail;
        p += used; if (p + 8 > n) { free(o); goto fail; }
        uint32_t crc = in[p] | (in[p + 1] << 8) | (in[p + 2] << 16) | ((uint32_t)in[p + 3] << 24);
        if (crc != crc32_calc(o, ol)) { free(o); goto fail; }
        p += 8;
        unsigned char* nv = realloc(acc, acclen + ol + 1); if (!nv) { free(o); goto fail; }
        acc = nv; memcpy(acc + acclen, o, ol); acclen += ol; free(o); members++; pos = p;
        if (max_out && acclen > max_out) goto fail;
    }
    if (!members) goto fail;
    acc[acclen] = 0; *out_len = acclen; return acc;
fail:
    free(acc); return NULL;
}
unsigned char* n_zlib_decode(const unsigned char* in, size_t n, size_t* out_len, size_t max_out) {
    if (n < 6 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return NULL;
    size_t used = 0; unsigned char* o = inflate_raw(in + 2, n - 2, &used, out_len, max_out); if (!o) return NULL;
    if (2 + used + 4 <= n) {
        const unsigned char* t = in + 2 + used; uint32_t ad = ((uint32_t)t[0] << 24) | (t[1] << 16) | (t[2] << 8) | t[3];
        if (ad != adler32_calc(o, *out_len)) { free(o); return NULL; }
    }
    return o;
}
unsigned char* n_http_deflate(const unsigned char* in, size_t n, size_t* out_len, size_t max_out) {
    unsigned char* o = n_zlib_decode(in, n, out_len, max_out); if (o) return o;
    return n_deflate_raw(in, n, out_len, max_out);
}
