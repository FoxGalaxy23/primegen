#include "font_manager.h"
#include "gal.h"
#include "nal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#  include <direct.h>
#  define FM_MKDIR(p) _mkdir(p)
#else
#  define FM_MKDIR(p) mkdir((p), 0777)
#endif

extern TTFFont* g_ttf_font; /* шрифт по умолчанию (загружается в gal_init) */

/* ══════════════════════════════════════════════════════════════════
   Мелкие утилиты
   ══════════════════════════════════════════════════════════════════ */
static int eq_nocase(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int starts_nocase(const char* s, const char* prefix) {
    while (*prefix) {
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

static const char* find_nocase(const char* s, const char* needle) {
    for (; *s; s++) if (starts_nocase(s, needle)) return s;
    return NULL;
}

static char* dup_n(const char* s, size_t n) {
    char* r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* Снять пробелы и кавычки по краям: "  'Open Sans' " -> "Open Sans". */
static void normalize_family(const char* src, size_t len, char* dst, size_t dst_sz) {
    while (len && (isspace((unsigned char)*src) || *src == '"' || *src == '\'')) { src++; len--; }
    while (len && (isspace((unsigned char)src[len - 1]) || src[len - 1] == '"' || src[len - 1] == '\'')) len--;
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int is_generic_family(const char* n) {
    static const char* const g[] = {
        "serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", "ui-sans-serif",
        "ui-serif", "ui-monospace", "ui-rounded", "math", "emoji", "fangsong",
        "-apple-system", "blinkmacsystemfont", NULL
    };
    for (int i = 0; g[i]; i++) if (eq_nocase(n, g[i])) return 1;
    return 0;
}

static int is_css_keyword(const char* n) {
    return eq_nocase(n, "inherit") || eq_nocase(n, "initial") || eq_nocase(n, "unset") || eq_nocase(n, "revert");
}

/* ══════════════════════════════════════════════════════════════════
   Реестр шрифтов
   ══════════════════════════════════════════════════════════════════ */
typedef struct {
    char     family[64];
    char     path[512];
    int      weight;      /* 100..900 */
    int      italic;
    TTFFont* font;        /* лениво загружается */
    int      failed;      /* загрузка не удалась — больше не пытаемся */
} FontEntry;

#define MAX_FONT_ENTRIES 256
#define LOOKUP_CACHE 8

typedef struct { char key[192]; TTFFont* val; int used; } LookupCache;

static FontEntry*  g_fonts = NULL;
static int         g_font_count = 0, g_font_cap = 0;
static LookupCache g_lookup[LOOKUP_CACHE];
static int         g_lookup_next = 0;

static void lookup_cache_reset(void) { memset(g_lookup, 0, sizeof g_lookup); g_lookup_next = 0; }

static void release_entry_font(FontEntry* e) {
    if (e->font && e->font != g_ttf_font) ttf_free(e->font);
    e->font = NULL;
}

void font_manager_cleanup(void) {
    for (int i = 0; i < g_font_count; i++) release_entry_font(&g_fonts[i]);
    free(g_fonts);
    g_fonts = NULL; g_font_count = g_font_cap = 0;
    lookup_cache_reset();
}

void font_manager_init(void) {
    font_manager_cleanup();
}

void font_manager_register_face(const char* family, const char* path, int weight, int italic) {
    if (!family || !path || !path[0]) return;
    char fam[64];
    normalize_family(family, strlen(family), fam, sizeof fam);
    if (!fam[0]) return;
    if (weight < 1 || weight > 1000) weight = 400;
    italic = italic ? 1 : 0;

    for (int i = 0; i < g_font_count; i++) {
        FontEntry* e = &g_fonts[i];
        if (e->weight == weight && e->italic == italic && eq_nocase(e->family, fam)) {
            if (strcmp(e->path, path) == 0) return;
            release_entry_font(e);
            snprintf(e->path, sizeof e->path, "%s", path);
            e->failed = 0;
            lookup_cache_reset();
            return;
        }
    }
    if (g_font_count >= MAX_FONT_ENTRIES) {
        gal_log("WARN", "Font registry full, ignoring '%s'", fam);
        return;
    }
    if (g_font_count == g_font_cap) {
        int nc = g_font_cap ? g_font_cap * 2 : 16;
        FontEntry* nf = realloc(g_fonts, (size_t)nc * sizeof(FontEntry));
        if (!nf) return;
        g_fonts = nf; g_font_cap = nc;
    }
    FontEntry* e = &g_fonts[g_font_count++];
    memset(e, 0, sizeof *e);
    snprintf(e->family, sizeof e->family, "%s", fam);
    snprintf(e->path, sizeof e->path, "%s", path);
    e->weight = weight; e->italic = italic;
    lookup_cache_reset();
}

void font_manager_register(const char* family, const char* path) {
    font_manager_register_face(family, path, 400, 0);
}

/* Ближайшее начертание семейства: сначала стиль, затем вес (по CSS: для лёгких весов
   сперва ищем легче, для тяжёлых — тяжелее). */
static int pick_entry(const char* family, int weight, int italic) {
    int best = -1, best_score = 0x7fffffff;
    for (int i = 0; i < g_font_count; i++) {
        const FontEntry* e = &g_fonts[i];
        if (!eq_nocase(e->family, family)) continue;
        int diff = e->weight - weight;
        int score = (diff < 0 ? -diff : diff) * 2;
        if (weight <= 500 ? diff > 0 : diff < 0) score += 1;
        if (e->italic != italic) score += 10000;
        if (e->failed) score += 100000;
        if (score < best_score) { best_score = score; best = i; }
    }
    return best;
}

static TTFFont* ensure_loaded(int i) {
    FontEntry* e = &g_fonts[i];
    if (e->font) return e->font;
    if (e->failed) return NULL;
    e->font = ttf_load(e->path);
    if (!e->font) {
        e->failed = 1;
        gal_log("WARN", "Failed to load font '%s' from %s - using the default font", e->family, e->path);
        return NULL;
    }
    ttf_set_fallback(e->font, g_ttf_font);     /* глифы, которых нет в веб-шрифте, берём из дефолтного */
    return e->font;
}

TTFFont* font_manager_get_styled(const char* family_list, int weight, int italic) {
    if (!family_list || !family_list[0]) return g_ttf_font;
    if (weight < 1 || weight > 1000) weight = 400;
    italic = italic ? 1 : 0;

    char key[192];
    int key_ok = snprintf(key, sizeof key, "%d|%d|%s", weight, italic, family_list) < (int)sizeof key;
    if (key_ok) {
        for (int i = 0; i < LOOKUP_CACHE; i++)
            if (g_lookup[i].used && strcmp(g_lookup[i].key, key) == 0) return g_lookup[i].val;
    }

    TTFFont* result = NULL;
    const char* p = family_list;
    while (*p && !result) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char* start = p;
        char quote = 0;
        while (*p && (quote || *p != ',')) {
            if (quote) { if (*p == quote) quote = 0; }
            else if (*p == '"' || *p == '\'') quote = *p;
            p++;
        }
        char name[64];
        normalize_family(start, (size_t)(p - start), name, sizeof name);
        if (!name[0] || is_css_keyword(name)) continue;

        int idx = pick_entry(name, weight, italic);
        if (idx >= 0) {
            TTFFont* f = ensure_loaded(idx);
            if (f) result = f;
        } else if (is_generic_family(name)) {
            result = g_ttf_font;
            break;
        }
    }
    if (!result) result = g_ttf_font;

    if (key_ok) {
        LookupCache* c = &g_lookup[g_lookup_next];
        g_lookup_next = (g_lookup_next + 1) % LOOKUP_CACHE;
        memcpy(c->key, key, sizeof c->key);
        c->val = result; c->used = 1;
    }
    return result;
}

TTFFont* font_manager_get(const char* family_list) {
    return font_manager_get_styled(family_list, 400, 0);
}

/* ══════════════════════════════════════════════════════════════════
   @font-face: выбор источника, URL, загрузка
   ══════════════════════════════════════════════════════════════════ */
static int parse_weight(const char* v) {
    if (!v) return 400;
    while (isspace((unsigned char)*v)) v++;
    if (starts_nocase(v, "bold")) return 700;      /* bold / bolder */
    if (starts_nocase(v, "lighter")) return 300;
    if (starts_nocase(v, "normal")) return 400;
    if (!isdigit((unsigned char)*v)) return 400;
    int lo = atoi(v), hi = lo;
    while (isdigit((unsigned char)*v)) v++;
    while (isspace((unsigned char)*v)) v++;
    if (isdigit((unsigned char)*v)) hi = atoi(v);   /* variable-шрифт: "100 900" */
    if (lo < 1) lo = 400;
    if (400 >= lo && 400 <= hi) return 400;         /* диапазон содержит regular */
    return lo;
}

static int parse_italic(const char* v) {
    return v && (find_nocase(v, "italic") || find_nocase(v, "oblique"));
}

/* Ранг источника: 0 — не поддерживаем, больше — лучше. */
static int rank_by_format(const char* fmt) {
    if (eq_nocase(fmt, "truetype") || eq_nocase(fmt, "truetype-variations") || eq_nocase(fmt, "collection")) return 3;
    if (eq_nocase(fmt, "opentype") || eq_nocase(fmt, "opentype-variations")) return 2;
    if (starts_nocase(fmt, "woff2")) return 0;
    if (starts_nocase(fmt, "woff")) return ttf_supports_woff() ? 2 : 0;
    if (eq_nocase(fmt, "embedded-opentype") || starts_nocase(fmt, "svg")) return 0;
    return 1;
}

static int rank_by_extension(const char* url, size_t len) {
    size_t end = 0;
    while (end < len && url[end] != '?' && url[end] != '#') end++;
    const char* dot = NULL;
    for (size_t i = 0; i < end; i++) if (url[i] == '.') dot = url + i;
    if (!dot) return 1;
    size_t el = (size_t)(url + end - dot);
    char ext[8] = {0};
    if (el >= sizeof ext) return 1;
    memcpy(ext, dot, el);
    if (eq_nocase(ext, ".ttf") || eq_nocase(ext, ".ttc")) return 3;
    if (eq_nocase(ext, ".otf")) return 2;
    if (eq_nocase(ext, ".woff2")) return 0;
    if (eq_nocase(ext, ".woff")) return ttf_supports_woff() ? 2 : 0;
    if (eq_nocase(ext, ".eot") || eq_nocase(ext, ".svg")) return 0;
    return 1;
}

static int rank_by_mime(const char* mime, size_t len) {
    char m[64];
    if (len >= sizeof m) return 0;
    memcpy(m, mime, len); m[len] = '\0';
    if (eq_nocase(m, "font/ttf") || eq_nocase(m, "font/truetype") || eq_nocase(m, "application/x-font-ttf") ||
        eq_nocase(m, "application/x-font-truetype") || eq_nocase(m, "application/font-sfnt")) return 3;
    if (eq_nocase(m, "font/otf") || eq_nocase(m, "application/x-font-opentype") || eq_nocase(m, "application/vnd.ms-opentype")) return 2;
    if (eq_nocase(m, "font/woff2") || eq_nocase(m, "application/font-woff2")) return 0;
    if (eq_nocase(m, "font/woff") || eq_nocase(m, "application/font-woff") || eq_nocase(m, "application/x-font-woff"))
        return ttf_supports_woff() ? 2 : 0;
    return 1;                                       /* application/octet-stream и т.п. — пробуем */
}

/* Разбирает значение src, выбирает лучший поддерживаемый url(...).
   Возвращает malloc'нутую строку с URL (без кавычек) или NULL. */
static char* choose_source(const char* src) {
    char* best = NULL;
    int best_rank = 0;
    const char* p = src;
    while ((p = find_nocase(p, "url("))) {
        p += 4;
        while (isspace((unsigned char)*p)) p++;
        char q = 0;
        if (*p == '"' || *p == '\'') q = *p++;
        const char* us = p;
        while (*p && (q ? *p != q : *p != ')')) p++;
        size_t ul = (size_t)(p - us);
        if (!q) while (ul && isspace((unsigned char)us[ul - 1])) ul--;
        const char* after = p;
        while (*after && *after != ')') after++;
        if (*after == ')') after++;

        char fmt[40] = {0};
        for (const char* r = after; *r && *r != ','; r++) {         /* format("woff2") этого же источника */
            if (starts_nocase(r, "format(")) {
                r += 7;
                while (*r == ' ' || *r == '"' || *r == '\'') r++;
                size_t k = 0;
                while (*r && *r != '"' && *r != '\'' && *r != ')' && k < sizeof fmt - 1) fmt[k++] = *r++;
                break;
            }
        }

        int rank;
        if (ul >= 5 && starts_nocase(us, "data:")) {
            const char* semi = us + 5;
            while (semi < us + ul && *semi != ';' && *semi != ',') semi++;
            rank = rank_by_mime(us + 5, (size_t)(semi - (us + 5)));
            if (rank > 0 && (fmt[0])) { int r2 = rank_by_format(fmt); rank = r2 < rank ? r2 : rank; }
            if (!find_nocase(us, ";base64,") || find_nocase(us, ";base64,") >= us + ul) rank = 0;
        } else if (fmt[0]) {
            rank = rank_by_format(fmt);
        } else {
            rank = rank_by_extension(us, ul);
        }

        if (ul > 0 && rank > best_rank) {
            char* copy = dup_n(us, ul);
            if (copy) { free(best); best = copy; best_rank = rank; }
        }
        p = after;
    }
    return best;
}

/* Убирает "." и ".." из пути URL (RFC 3986, упрощённо). */
static void remove_dot_segments(char* u) {
    char* host = strstr(u, "://");
    if (!host) return;
    char* ps = host + 3;
    ps += strcspn(ps, "/?#");
    if (*ps != '/') return;
    char* qs = ps + strcspn(ps, "?#");
    char* tail = strdup(qs);
    if (!tail) return;
    char* w = ps;
    const char* r = ps;
    while (r < qs) {
        const char* seg = r + 1;
        const char* se = seg;
        while (se < qs && *se != '/') se++;
        size_t sl = (size_t)(se - seg);
        if (sl == 1 && seg[0] == '.') {
            if (se == qs) *w++ = '/';
        } else if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            if (w > ps) { w--; while (w > ps && *w != '/') w--; }
            if (se == qs) *w++ = '/';
        } else {
            *w++ = '/';
            memmove(w, seg, sl);
            w += sl;
        }
        r = se;
    }
    memmove(w, tail, strlen(tail) + 1);
    free(tail);
}

/* Абсолютный URL из значения url(...) и адреса страницы/таблицы стилей. */
static char* resolve_url(const char* page_url, const char* url) {
    if (starts_nocase(url, "http://") || starts_nocase(url, "https://")) return dup_n(url, strlen(url));
    if (!page_url || !(starts_nocase(page_url, "http://") || starts_nocase(page_url, "https://"))) return NULL;

    const char* scheme_end = strstr(page_url, "://");
    const char* auth = scheme_end + 3;
    const char* auth_end = auth + strcspn(auth, "/?#");
    const char* path = auth_end;
    size_t path_len = strcspn(path, "?#");
    int origin_len = (int)(auth_end - page_url);

    size_t cap = strlen(page_url) + strlen(url) + 8;
    char* out = malloc(cap);
    if (!out) return NULL;

    if (url[0] == '/' && url[1] == '/') {
        snprintf(out, cap, "%.*s:%s", (int)(scheme_end - page_url), page_url, url);
    } else if (url[0] == '/') {
        snprintf(out, cap, "%.*s%s", origin_len, page_url, url);
    } else {
        size_t dir_len = path_len;
        while (dir_len > 0 && path[dir_len - 1] != '/') dir_len--;
        if (dir_len == 0) snprintf(out, cap, "%.*s/%s", origin_len, page_url, url);
        else snprintf(out, cap, "%.*s%.*s%s", origin_len, page_url, (int)dir_len, path, url);
    }
    remove_dot_segments(out);
    return out;
}

static unsigned long long fnv1a64(const char* s) {
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static int file_exists_nonempty(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

/* Файл похож на шрифт (а не на HTML-страницу с ошибкой)? */
static int looks_like_font(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char m[4] = {0};
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    if (n < 4) return 0;
    return (m[0] == 0 && m[1] == 1 && m[2] == 0 && m[3] == 0) || memcmp(m, "true", 4) == 0 ||
           memcmp(m, "ttcf", 4) == 0 || memcmp(m, "OTTO", 4) == 0 || memcmp(m, "wOFF", 4) == 0 ||
           memcmp(m, "wOF2", 4) == 0;
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int write_data_url(const char* data_url, const char* dest) {
    const char* comma = strchr(data_url, ',');
    if (!comma) return 0;
    size_t in_len = strlen(comma + 1);
    unsigned char* buf = malloc(in_len / 4 * 3 + 4);
    if (!buf) return 0;
    size_t n = 0; unsigned acc = 0; int bits = 0;
    for (const char* p = comma + 1; *p && *p != '='; p++) {
        int v = b64_val((unsigned char)*p);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; buf[n++] = (unsigned char)((acc >> bits) & 0xFF); }
    }
    FILE* f = fopen(dest, "wb");
    int ok = f && n > 0 && fwrite(buf, 1, n, f) == n;
    if (f) fclose(f);
    free(buf);
    if (!ok) remove(dest);
    return ok;
}

static int g_font_downloads = 0;
static int g_font_offline = 0;
void font_manager_set_offline(int off) { g_font_offline = off; }                    /* лимит загрузок шрифтов за одну загрузку страницы */

void font_manager_add_face(const char* family, const char* src,
                           const char* weight, const char* style, const char* page_url) {
    if (!family || !src) return;
    char fam[64];
    normalize_family(family, strlen(family), fam, sizeof fam);
    if (!fam[0]) return;

    char* chosen = choose_source(src);
    if (!chosen) {
        gal_log("WARN", "@font-face '%s': no supported font source (only WOFF2?) - using fallback font", fam);
        return;
    }

    FM_MKDIR("cache");
    char cache_path[128];
    int ok = 0;

    if (starts_nocase(chosen, "data:")) {
        snprintf(cache_path, sizeof cache_path, "cache/font_%016llx.font", fnv1a64(chosen));
        ok = file_exists_nonempty(cache_path) || write_data_url(chosen, cache_path);
    } else {
        char* abs_url = resolve_url(page_url, chosen);
        if (!abs_url) {
            gal_log("WARN", "@font-face '%s': cannot resolve URL '%s'", fam, chosen);
        } else {
            snprintf(cache_path, sizeof cache_path, "cache/font_%016llx.font", fnv1a64(abs_url));
            if (!file_exists_nonempty(cache_path)) {
                if (g_font_offline || g_font_downloads >= 12) { free(abs_url); free(chosen); return; }
                g_font_downloads++;
                gal_log("INFO", "Downloading font %s from %s", fam, abs_url);
                if (!nal_download_file(abs_url, cache_path) || !looks_like_font(cache_path)) {
                    gal_log("ERROR", "Failed to download font %s (not a font file)", fam);
                    remove(cache_path);
                } else ok = 1;
            } else ok = 1;
            free(abs_url);
        }
    }
    free(chosen);
    if (ok) font_manager_register_face(fam, cache_path, parse_weight(weight), parse_italic(style));
}

void font_manager_download(const char* family, const char* url, const char* page_url) {
    if (!family || !url) return;
    size_t cap = strlen(url) + 8;
    char* src = malloc(cap);
    if (!src) return;
    snprintf(src, cap, "url(\"%s\")", url);
    font_manager_add_face(family, src, NULL, NULL, page_url);
    free(src);
}
