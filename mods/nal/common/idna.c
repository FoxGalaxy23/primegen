#include "idna.h"
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

static int utf8_next(const unsigned char **p, uint32_t *cp) {
    const unsigned char *s = *p;
    if (!*s) return 0;
    uint32_t c; unsigned n;
    if (s[0] < 0x80) { c = s[0]; n = 1; }
    else if (s[0] >= 0xC2 && s[0] <= 0xDF) { c = s[0] & 0x1F; n = 2; }
    else if (s[0] >= 0xE0 && s[0] <= 0xEF) { c = s[0] & 0x0F; n = 3; }
    else if (s[0] >= 0xF0 && s[0] <= 0xF4) { c = s[0] & 7; n = 4; }
    else return -1;
    for (unsigned i = 1; i < n; ++i) { if ((s[i] & 0xC0) != 0x80) return -1; c = (c << 6) | (s[i] & 0x3F); }
    if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && c < 0x10000) ||
        c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -1;
    *p += n; *cp = c; return 1;
}
static char encode_digit(unsigned d) { return (char)(d < 26 ? 'a' + d : '0' + (d - 26)); }
static unsigned adapt_bias(uint64_t delta, unsigned points, int first) {
    delta = first ? delta / 700 : delta / 2; delta += delta / points;
    unsigned k = 0; while (delta > 455) { delta /= 35; k += 36; }
    return k + (unsigned)((36 * delta) / (delta + 38));
}
static int puny_label(const uint32_t *in, size_t n, char *out, size_t cap) {
    size_t o = 0, b = 0;
    if (cap < 5) return 0;
    memcpy(out, "xn--", 4); o = 4;
    for (size_t i = 0; i < n; ++i) if (in[i] < 0x80) {
        unsigned char c = (unsigned char)in[i];
        if (!(isalnum(c) || c == '-')) return 0;
        if (o + 1 >= cap) return 0; out[o++] = (char)tolower(c); b++;
    }
    size_t h = b;
    if (b && h < n) { if (o + 1 >= cap) return 0; out[o++] = '-'; }
    uint32_t next = 128; uint64_t delta = 0; unsigned bias = 72;
    while (h < n) {
        uint32_t m = UINT32_MAX;
        for (size_t i = 0; i < n; ++i) if (in[i] >= next && in[i] < m) m = in[i];
        if (m == UINT32_MAX) return 0;
        uint64_t step = (uint64_t)(m - next) * (h + 1);
        if (UINT64_MAX - delta < step) return 0; delta += step; next = m;
        for (size_t i = 0; i < n; ++i) {
            if (in[i] < next) { if (delta == UINT64_MAX) return 0; delta++; }
            else if (in[i] == next) {
                uint64_t q = delta;
                for (unsigned k = 36;; k += 36) {
                    unsigned t = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
                    if (q < t) break;
                    unsigned d = t + (unsigned)((q - t) % (36 - t));
                    if (o + 1 >= cap) return 0; out[o++] = encode_digit(d);
                    q = (q - t) / (36 - t);
                }
                if (q >= 36 || o + 1 >= cap) return 0;
                out[o++] = encode_digit((unsigned)q);
                bias = adapt_bias(delta, (unsigned)(h + 1), h == b);
                delta = 0; h++;
            }
        }
        if (delta == UINT64_MAX || next == UINT32_MAX) return 0;
        delta++; next++;
    }
    out[o] = 0;
    if (o > 63) return 0;
    return 1;
}
static int ascii_label(const uint32_t *cp, size_t n, char *out, size_t cap) {
    if (!n || n > 63 || cap <= n) return 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = cp[i];
        if (c >= 0x80 || !(isalnum((unsigned char)c) || c == '-')) return 0;
        if ((i == 0 || i + 1 == n) && c == '-') return 0;
        out[i] = (char)tolower((unsigned char)c);
    }
    out[n] = 0; return 1;
}
static int valid_unicode_label(const uint32_t *cp, size_t n) {
    if (!n || (cp[0] == '-' || cp[n - 1] == '-')) return 0;
    for (size_t i=0;i<n;i++) {
        uint32_t c=cp[i];
        if (c <= 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x00AD || c == 0x034F ||
            c == 0x061C || c == 0x200B || c == 0x200C || c == 0x200D || c == 0x2060 ||
            c == 0xFEFF || (c & 0xFFFF) == 0xFFFE || (c & 0xFFFF) == 0xFFFF ||
            (c >= 0xFDD0 && c <= 0xFDEF)) return 0;
    }
    return 1;
}
int nidna_to_ascii(const char *input, char *output, size_t cap) {
    if (!input || !output || cap < 2) return 0;
    output[0] = 0;
    size_t inlen = strlen(input); if (!inlen || inlen > 1024) return 0;
    /* IDNA recognizes the three Unicode full-stop variants as label separators. */
    uint32_t cps[256]; size_t nc=0; const unsigned char *p=(const unsigned char*)input;
    while (*p) {
        uint32_t c; int r=utf8_next(&p,&c); if (r <= 0 || nc >= sizeof cps/sizeof cps[0]) return 0;
        if (c == 0x3002 || c == 0xFF0E || c == 0xFF61) c='.';
        cps[nc++]=c;
    }
    if (nc && cps[nc-1]=='.') nc--; /* absolute DNS name */
    if (!nc) return 0;
    size_t start=0, o=0;
    for (size_t i=0;i<=nc;i++) if (i==nc || cps[i]=='.') {
        size_t n=i-start; char label[64]; int ascii=1;
        for(size_t j=start;j<i;j++) if(cps[j]>=0x80) {ascii=0;break;}
        if (!n) return 0;
        if (ascii) { if(!ascii_label(cps+start,n,label,sizeof label)) return 0; }
        else { if(!valid_unicode_label(cps+start,n) || !puny_label(cps+start,n,label,sizeof label)) return 0; }
        size_t ll=strlen(label);
        if (o && o+1 >= cap) return 0;
        if (o) output[o++]='.';
        if (o+ll >= cap || o+ll > 253) return 0;
        memcpy(output+o,label,ll);o+=ll;start=i+1;
    }
    output[o]=0; return o<=253;
}
