/* Поиск по Public Suffix List: правила загружаются в хэш-таблицу при первом обращении. Алгоритм — publicsuffix.org/list: побеждает самое длинное правило,
   исключения (!) выигрывают у подстановок (*.), по умолчанию правило «*» (последняя метка — суффикс). */
#include "psl.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>
#include "psl_data.inc"

static char** g_tab; static size_t g_cap; static int g_ready;
static unsigned long hs(const char* s, size_t n) { unsigned long h = 5381; for (size_t i = 0; i < n; i++) h = h * 33 + (unsigned char)s[i]; return h; }
static void tab_add(const char* s, size_t n) {
    size_t i = hs(s, n) & (g_cap - 1);
    while (g_tab[i]) i = (i + 1) & (g_cap - 1);
    char* c = malloc(n + 1); if (!c) return; memcpy(c, s, n); c[n] = 0; g_tab[i] = c;
}
static void load(void) {
    if (g_ready) return; g_cap = 32768; g_tab = calloc(g_cap, sizeof(char*)); if (!g_tab) return;
    const char* p = kPslRules; while (*p) { const char* e = strchr(p, '\n'); if (!e) break; if (e > p) tab_add(p, (size_t)(e - p)); p = e + 1; }
    g_ready = 1;
}
static int has(const char* s, size_t n) {
    size_t i = hs(s, n) & (g_cap - 1);
    while (g_tab[i]) { if (strlen(g_tab[i]) == n && !memcmp(g_tab[i], s, n)) return 1; i = (i + 1) & (g_cap - 1); }
    return 0;
}
static int is_ip(const char* h) { struct in_addr a; struct in6_addr b; return inet_pton(AF_INET, h, &a) == 1 || inet_pton(AF_INET6, h, &b) == 1; }
/* число меток публичного суффикса хоста (0 — нет данных) */
static int suffix_labels(const char* host, int* total) {
    load(); if (!g_ready) return 0;
    char h[256]; size_t n = strlen(host); if (n == 0 || n >= sizeof h) return 0; for (size_t i = 0; i < n; i++) h[i] = (char)tolower((unsigned char)host[i]);
    if (h[n - 1] == '.') n--; h[n] = 0;
    const char* lab[64]; int nl = 0; lab[nl++] = h; for (size_t i = 0; i < n && nl < 64; i++) if (h[i] == '.') lab[nl++] = h + i + 1;
    *total = nl; int best = 1;                                                                    /* правило по умолчанию «*» */
    for (int i = 0; i < nl; i++) {
        const char* cand = lab[i]; size_t cl = strlen(cand);
        char ex[260]; if (cl + 2 < sizeof ex) { ex[0] = '!'; memcpy(ex + 1, cand, cl); if (has(ex, cl + 1)) return nl - i - 1; }        /* исключение: суффикс без первой метки */
        if (has(cand, cl)) { int k = nl - i; if (k > best) best = k; }
        if (i + 1 < nl) { const char* up = lab[i + 1]; size_t ul = strlen(up); char w[260]; if (ul + 3 < sizeof w) { w[0] = '*'; w[1] = '.'; memcpy(w + 2, up, ul); if (has(w, ul + 2)) { int k = nl - i; if (k > best) best = k; } } }
    }
    return best;
}
int psl_is_public_suffix(const char* domain) {
    if (!domain || !*domain || is_ip(domain)) return 0;
    int total = 0, sl = suffix_labels(domain, &total); return sl > 0 && sl >= total;
}
int psl_registrable_domain(const char* host, char* out, size_t cap) {
    if (!host || !*host || is_ip(host)) return 0;
    int total = 0, sl = suffix_labels(host, &total); if (sl <= 0 || sl >= total) return 0;
    int skip = total - sl - 1; const char* p = host; while (skip > 0 && *p) { if (*p == '.') skip--; p++; }
    size_t l = strlen(p); while (l && p[l - 1] == '.') l--; if (l + 1 > cap) return 0;
    for (size_t i = 0; i < l; i++) out[i] = (char)tolower((unsigned char)p[i]); out[l] = 0; return 1;
}
