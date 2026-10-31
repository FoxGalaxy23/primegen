/* Растеризатор SVG. Конвейер: разбор XML → CSS-правила → обход дерева со стилем и матрицей → контуры (ломаные) →
   заливка/обводка в буфер покрытия (накопление знаковой площади, как в font-rs) → композиция в ARGB. */
#define _GNU_SOURCE
#include "svg.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef SVG_WITH_TTF
#include "font_manager.h"
#include "ttf_render.h"
#endif

#define SVG_MAX_NODES 40000
#define SVG_MAX_DEPTH 48
#define SVG_MAX_USE_DEPTH 12
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════ арена ══════════════ */
typedef struct ABlk { struct ABlk* next; size_t used, cap; char d[]; } ABlk;
typedef struct { ABlk* head; } Arena;
static void* aalloc(Arena* a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 ? n : 65536; ABlk* b = malloc(sizeof(ABlk) + cap); if (!b) return NULL;
        b->next = a->head; b->used = 0; b->cap = cap; a->head = b;
    }
    void* p = a->head->d + a->head->used; a->head->used += n; memset(p, 0, n); return p;
}
static char* astrn(Arena* a, const char* s, size_t n) { char* p = aalloc(a, n + 1); if (p) { memcpy(p, s, n); p[n] = 0; } return p; }
static void afree(Arena* a) { while (a->head) { ABlk* n = a->head->next; free(a->head); a->head = n; } }

/* ══════════════ XML ══════════════ */
typedef struct SNode {
    char* tag; char** an; char** av; int na;
    struct SNode *first, *last, *next, *parent;
    char* text; size_t tlen;                                  /* текст (нужен только для <style>) */
    int id;                                                   /* порядковый номер */
} SNode;
typedef struct { Arena ar; SNode* root; int nnodes; SNode** all; int cap_all; } Doc;

static const char* attr_get(const SNode* n, const char* name) {
    for (int i = 0; i < n->na; i++) if (!strcmp(n->an[i], name)) return n->av[i];
    return NULL;
}
static size_t xml_decode(const char* s, size_t n, char* out) {          /* сущности &amp; &lt; &gt; &quot; &apos; &#N; &#xH; */
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            const char* e = memchr(s + i, ';', n - i > 10 ? 10 : n - i);
            if (e) {
                size_t l = (size_t)(e - (s + i + 1)); const char* t = s + i + 1; unsigned cp = 0; int ok = 1;
                if (l == 3 && !memcmp(t, "amp", 3)) cp = '&'; else if (l == 2 && !memcmp(t, "lt", 2)) cp = '<'; else if (l == 2 && !memcmp(t, "gt", 2)) cp = '>';
                else if (l == 4 && !memcmp(t, "quot", 4)) cp = '"'; else if (l == 4 && !memcmp(t, "apos", 4)) cp = '\'';
                else if (l >= 2 && t[0] == '#') { char tmp[12]; memcpy(tmp, t + 1, l - 1); tmp[l - 1] = 0; cp = (unsigned)((tmp[0] == 'x' || tmp[0] == 'X') ? strtoul(tmp + 1, NULL, 16) : strtoul(tmp, NULL, 10)); }
                else ok = 0;
                if (ok && cp) {
                    if (cp < 0x80) out[o++] = (char)cp;
                    else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 63)); }
                    else if (cp < 0x10000) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
                    else { out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 63)); out[o++] = (char)(0x80 | ((cp >> 6) & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
                    i = (size_t)(e - s); continue;
                }
            }
        }
        out[o++] = s[i];
    }
    out[o] = 0; return o;
}
static const char* local_name(const char* s, size_t* len) {              /* отбрасываем префикс пространства имён (svg:path → path) */
    const char* c = memchr(s, ':', *len); if (c) { *len -= (size_t)(c + 1 - s); return c + 1; } return s;
}
static void doc_add_node(Doc* d, SNode* n) {
    if (d->nnodes == d->cap_all) { int nc = d->cap_all ? d->cap_all * 2 : 256; SNode** nv = realloc(d->all, (size_t)nc * sizeof(SNode*)); if (!nv) return; d->all = nv; d->cap_all = nc; }
    n->id = d->nnodes; d->all[d->nnodes++] = n;
}
static int xml_parse(Doc* d, const char* s, size_t n) {
    SNode* cur = NULL; size_t i = 0;
    while (i < n) {
        if (s[i] != '<') {                                                /* текст */
            size_t j = i; while (j < n && s[j] != '<') j++;
            if (cur && j > i) { size_t add = j - i; char* t = realloc(cur->text, cur->tlen + add + 1); if (t) { memcpy(t + cur->tlen, s + i, add); cur->tlen += add; t[cur->tlen] = 0; cur->text = t; } }
            i = j; continue;
        }
        if (i + 3 < n && !memcmp(s + i, "<!--", 4)) { const char* e = NULL; for (size_t k = i + 4; k + 2 < n; k++) if (!memcmp(s + k, "-->", 3)) { e = s + k + 3; break; } if (!e) return d->root != NULL; i = (size_t)(e - s); continue; }
        if (i + 8 < n && !memcmp(s + i, "<![CDATA[", 9)) {
            size_t k = i + 9; while (k + 2 < n && memcmp(s + k, "]]>", 3)) k++;
            if (cur && k > i + 9) { size_t add = k - i - 9; char* t = realloc(cur->text, cur->tlen + add + 1); if (t) { memcpy(t + cur->tlen, s + i + 9, add); cur->tlen += add; t[cur->tlen] = 0; cur->text = t; } }
            i = k + 3; continue;
        }
        if (i + 1 < n && (s[i + 1] == '?' || s[i + 1] == '!')) {                /* <?xml ?>, <!DOCTYPE …[…]> */
            int br = 0; size_t k = i + 2; while (k < n) { if (s[k] == '[') br++; else if (s[k] == ']') br--; else if (s[k] == '>' && br <= 0) break; k++; }
            i = k + 1; continue;
        }
        if (i + 1 < n && s[i + 1] == '/') {                               /* закрывающий тег */
            size_t k = i + 2; while (k < n && s[k] != '>') k++;
            if (cur) cur = cur->parent; i = k + 1; continue;
        }
        size_t k = i + 1; size_t ns = k; while (k < n && !isspace((unsigned char)s[k]) && s[k] != '>' && s[k] != '/') k++;
        size_t nl = k - ns; const char* nm = local_name(s + ns, &nl);
        if (d->nnodes >= SVG_MAX_NODES) return d->root != NULL;
        SNode* nd = aalloc(&d->ar, sizeof *nd); if (!nd) return 0;
        nd->tag = astrn(&d->ar, nm, nl); for (char* p = nd->tag; *p; p++) *p = (char)tolower((unsigned char)*p);
        int cap = 8; nd->an = aalloc(&d->ar, sizeof(char*) * (size_t)cap); nd->av = aalloc(&d->ar, sizeof(char*) * (size_t)cap);
        int selfclose = 0;
        for (;;) {                                                        /* атрибуты */
            while (k < n && isspace((unsigned char)s[k])) k++;
            if (k >= n) break;
            if (s[k] == '>') { k++; break; }
            if (s[k] == '/') { selfclose = 1; k++; continue; }
            size_t as = k; while (k < n && !isspace((unsigned char)s[k]) && s[k] != '=' && s[k] != '>' && s[k] != '/') k++;
            size_t al = k - as; const char* an = local_name(s + as, &al);
            char* nmv = astrn(&d->ar, an, al); for (char* p = nmv; *p; p++) *p = (char)tolower((unsigned char)*p);
            while (k < n && isspace((unsigned char)s[k])) k++;
            char* val = NULL;
            if (k < n && s[k] == '=') {
                k++; while (k < n && isspace((unsigned char)s[k])) k++;
                if (k < n && (s[k] == '"' || s[k] == '\'')) { char q = s[k++]; size_t vs = k; while (k < n && s[k] != q) k++; char* tmp = malloc(k - vs + 1); if (tmp) { size_t ol = xml_decode(s + vs, k - vs, tmp); val = astrn(&d->ar, tmp, ol); free(tmp); } if (k < n) k++; }
                else { size_t vs = k; while (k < n && !isspace((unsigned char)s[k]) && s[k] != '>') k++; val = astrn(&d->ar, s + vs, k - vs); }
            } else val = astrn(&d->ar, "", 0);
            if (!val) continue;
            if (nd->na == cap) { int nc = cap * 2; char** a2 = aalloc(&d->ar, sizeof(char*) * (size_t)nc); char** v2 = aalloc(&d->ar, sizeof(char*) * (size_t)nc); memcpy(a2, nd->an, sizeof(char*) * (size_t)cap); memcpy(v2, nd->av, sizeof(char*) * (size_t)cap); nd->an = a2; nd->av = v2; cap = nc; }
            if (!attr_get(nd, nmv)) { nd->an[nd->na] = nmv; nd->av[nd->na] = val; nd->na++; }
        }
        doc_add_node(d, nd);
        nd->parent = cur;
        if (cur) { if (cur->last) cur->last->next = nd; else cur->first = nd; cur->last = nd; }
        else if (!d->root && !strcmp(nd->tag, "svg")) d->root = nd;
        if (!cur && d->root != nd) { /* лишний корень вне svg — игнорируем */ }
        if (!selfclose) cur = nd;
        i = k;
    }
    return d->root != NULL;
}
static void doc_free(Doc* d) {
    for (int i = 0; i < d->nnodes; i++) free(d->all[i]->text);
    free(d->all); afree(&d->ar);
}
static SNode* doc_by_id_in(Doc* d, const char* id);
static SNode* doc_by_id(Doc* d, const char* id) { return doc_by_id_in(d, id); }
static SNode* doc_by_id_in(Doc* d, const char* id) {
    for (int i = 0; i < d->nnodes; i++) { const char* v = attr_get(d->all[i], "id"); if (v && !strcmp(v, id)) return d->all[i]; }
    return NULL;
}

/* ══════════════ числа, цвета, матрицы ══════════════ */
typedef struct { float a, b, c, d, e, f; } Mat;
static Mat mat_id(void) { Mat m = {1, 0, 0, 1, 0, 0}; return m; }
static Mat mat_mul(Mat m, Mat n) {                                        /* m * n: сначала n, затем m */
    Mat r = { m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d, m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f };
    return r;
}
static int mat_inv(Mat m, Mat* o) {
    float det = m.a * m.d - m.b * m.c; if (fabsf(det) < 1e-12f) return 0;
    float id = 1.0f / det; o->a = m.d * id; o->b = -m.b * id; o->c = -m.c * id; o->d = m.a * id; o->e = (m.c * m.f - m.d * m.e) * id; o->f = (m.b * m.e - m.a * m.f) * id; return 1;
}
static void mat_pt(Mat m, float x, float y, float* ox, float* oy) { *ox = m.a * x + m.c * y + m.e; *oy = m.b * x + m.d * y + m.f; }
static float mat_scale(Mat m) { float det = fabsf(m.a * m.d - m.b * m.c); return det > 1e-12f ? sqrtf(det) : 1.0f; }

static const char* skip_sep(const char* p) { while (*p && (isspace((unsigned char)*p) || *p == ',')) p++; return p; }
static int parse_num(const char** pp, float* out) {                       /* число с экспонентой; сдвигает указатель */
    const char* p = skip_sep(*pp); char* e; 
    if (!(isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')) return 0;
    double v = strtod(p, &e); if (e == p) return 0;
    *out = (float)v; *pp = e; return 1;
}
/* длина с единицами; relto — база для процентов, fs — размер шрифта для em/ex */
static float parse_len(const char* s, float relto, float dflt) {
    if (!s || !*s) return dflt;
    char* e; double v = strtod(s, &e); if (e == s) return dflt;
    while (isspace((unsigned char)*e)) e++;
    if (!strncasecmp(e, "px", 2) || !*e) return (float)v;
    if (!strncasecmp(e, "pt", 2)) return (float)(v * 4.0 / 3.0);
    if (!strncasecmp(e, "pc", 2)) return (float)(v * 16.0);
    if (!strncasecmp(e, "mm", 2)) return (float)(v * 96.0 / 25.4);
    if (!strncasecmp(e, "cm", 2)) return (float)(v * 96.0 / 2.54);
    if (!strncasecmp(e, "in", 2)) return (float)(v * 96.0);
    if (!strncasecmp(e, "em", 2)) return (float)(v * 16.0);
    if (!strncasecmp(e, "ex", 2)) return (float)(v * 8.0);
    if (*e == '%') return (float)(v * relto / 100.0);
    return (float)v;
}
static int parse_transform(const char* s, Mat* out) {
    Mat m = mat_id();
    while (*s) {
        while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
        if (!*s) break;
        const char* nm = s; while (isalpha((unsigned char)*s)) s++;
        size_t nl = (size_t)(s - nm); while (isspace((unsigned char)*s)) s++;
        if (*s != '(') return 0; s++;
        float v[6] = {0}; int nv = 0; const char* p = s;
        while (nv < 6 && parse_num(&p, &v[nv])) nv++;
        p = skip_sep(p); if (*p != ')') return 0; s = p + 1;
        Mat t = mat_id();
        if (nl == 6 && !strncmp(nm, "matrix", 6) && nv == 6) { t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
        else if (nl == 9 && !strncmp(nm, "translate", 9) && nv >= 1) { t.e = v[0]; t.f = nv > 1 ? v[1] : 0; }
        else if (nl == 5 && !strncmp(nm, "scale", 5) && nv >= 1) { t.a = v[0]; t.d = nv > 1 ? v[1] : v[0]; }
        else if (nl == 6 && !strncmp(nm, "rotate", 6) && nv >= 1) {
            float r = v[0] * (float)M_PI / 180.0f, c = cosf(r), sn = sinf(r); t.a = c; t.b = sn; t.c = -sn; t.d = c;
            if (nv >= 3) { Mat t1 = mat_id(); t1.e = v[1]; t1.f = v[2]; Mat t2 = mat_id(); t2.e = -v[1]; t2.f = -v[2]; t = mat_mul(t1, mat_mul(t, t2)); }
        }
        else if (nl == 5 && !strncmp(nm, "skewX", 5) && nv >= 1) t.c = tanf(v[0] * (float)M_PI / 180.0f);
        else if (nl == 5 && !strncmp(nm, "skewY", 5) && nv >= 1) t.b = tanf(v[0] * (float)M_PI / 180.0f);
        else return 0;
        m = mat_mul(m, t);
    }
    *out = m; return 1;
}

static const struct { const char* n; uint32_t c; } NAMED[] = {
    {"black",0x000000},{"white",0xFFFFFF},{"red",0xFF0000},{"green",0x008000},{"blue",0x0000FF},{"yellow",0xFFFF00},{"cyan",0x00FFFF},{"aqua",0x00FFFF},
    {"magenta",0xFF00FF},{"fuchsia",0xFF00FF},{"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},{"maroon",0x800000},{"olive",0x808000},{"lime",0x00FF00},
    {"teal",0x008080},{"navy",0x000080},{"purple",0x800080},{"orange",0xFFA500},{"pink",0xFFC0CB},{"brown",0xA52A2A},{"gold",0xFFD700},{"indigo",0x4B0082},
    {"violet",0xEE82EE},{"coral",0xFF7F50},{"crimson",0xDC143C},{"salmon",0xFA8072},{"khaki",0xF0E68C},{"tan",0xD2B48C},{"beige",0xF5F5DC},{"ivory",0xFFFFF0},
    {"lightgray",0xD3D3D3},{"lightgrey",0xD3D3D3},{"darkgray",0xA9A9A9},{"darkgrey",0xA9A9A9},{"dimgray",0x696969},{"dimgrey",0x696969},{"gainsboro",0xDCDCDC},
    {"whitesmoke",0xF5F5F5},{"lightblue",0xADD8E6},{"skyblue",0x87CEEB},{"steelblue",0x4682B4},{"royalblue",0x4169E1},{"dodgerblue",0x1E90FF},{"darkblue",0x00008B},
    {"lightgreen",0x90EE90},{"darkgreen",0x006400},{"limegreen",0x32CD32},{"seagreen",0x2E8B57},{"forestgreen",0x228B22},{"tomato",0xFF6347},{"orangered",0xFF4500},
    {"darkred",0x8B0000},{"firebrick",0xB22222},{"hotpink",0xFF69B4},{"deeppink",0xFF1493},{"slategray",0x708090},{"slategrey",0x708090},{"lavender",0xE6E6FA},
    {"turquoise",0x40E0D0},{"chocolate",0xD2691E},{"sienna",0xA0522D},{"orchid",0xDA70D6},{"plum",0xDDA0DD},{"wheat",0xF5DEB3},{"azure",0xF0FFFF},{"snow",0xFFFAFA},
    {"darkorange",0xFF8C00},{"goldenrod",0xDAA520},{"mediumpurple",0x9370DB},{"rebeccapurple",0x663399},{"darkviolet",0x9400D3},{"midnightblue",0x191970},{NULL,0}
};
/* ARGB; kind: 0 — none, 1 — цвет, 2 — currentColor, 3 — url(), 4 — inherit/не распознано */
static int parse_color(const char* s, uint32_t* out, uint32_t cur, char* urlbuf, size_t ucap) {
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return 4;
    if (!strncasecmp(s, "none", 4)) return 0;
    if (!strncasecmp(s, "currentcolor", 12)) { *out = cur; return 2; }
    if (!strncasecmp(s, "inherit", 7)) return 4;
    if (!strncasecmp(s, "transparent", 11)) { *out = 0; return 1; }
    if (!strncasecmp(s, "url(", 4)) {
        const char* p = s + 4;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\'' || *p == '"') p++;
        if (*p == '#') p++;
        const char* start = p;
        while (*p && *p != ')' && *p != '\'' && *p != '"' && !isspace((unsigned char)*p)) p++;
        size_t url_len = (size_t)(p - start);
        if (!urlbuf || !ucap || url_len >= ucap) { if (urlbuf && ucap) urlbuf[0] = 0; return 4; }
        memcpy(urlbuf, start, url_len); urlbuf[url_len] = 0;
        return 3;
    }
    if (*s == '#') {
        unsigned v = 0; int n = 0; const char* p = s + 1; while (isxdigit((unsigned char)*p)) { v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10)); p++; n++; }
        if (n == 3) { unsigned r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15; *out = 0xFF000000u | (r * 17 << 16) | (g * 17 << 8) | b * 17; return 1; }
        if (n == 4) { unsigned r = (v >> 12) & 15, g = (v >> 8) & 15, b = (v >> 4) & 15, a = v & 15; *out = (a * 17 << 24) | (r * 17 << 16) | (g * 17 << 8) | b * 17; return 1; }
        if (n == 6) { *out = 0xFF000000u | v; return 1; }
        if (n == 8) { *out = ((v & 255) << 24) | (v >> 8); return 1; }
        return 4;
    }
    if (!strncasecmp(s, "rgb", 3) || !strncasecmp(s, "hsl", 3)) {
        int hsl = tolower((unsigned char)s[0]) == 'h'; const char* p = strchr(s, '('); if (!p) return 4; p++;
        float v[4] = {0, 0, 0, 1}; int pct[4] = {0}; int nv = 0;
        while (nv < 4) { p = skip_sep(p); if (*p == '/') p = skip_sep(p + 1); if (!parse_num(&p, &v[nv])) break; if (*p == '%') { pct[nv] = 1; p++; } else if (!strncasecmp(p, "deg", 3)) p += 3; nv++; }
        if (nv < 3) return 4;
        float a = nv > 3 ? (pct[3] ? v[3] / 100 : v[3]) : 1; if (a < 0) a = 0; if (a > 1) a = 1;
        float r, g, b;
        if (hsl) {
            float h = fmodf(v[0], 360); if (h < 0) h += 360; h /= 360; float sat = v[1] / 100, l = v[2] / 100;
            float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat, pp = 2 * l - q;
            float t[3] = {h + 1.0f / 3, h, h - 1.0f / 3}; float rgb[3];
            for (int i = 0; i < 3; i++) { float tt = t[i]; if (tt < 0) tt += 1; if (tt > 1) tt -= 1; rgb[i] = tt < 1.0f / 6 ? pp + (q - pp) * 6 * tt : tt < 0.5f ? q : tt < 2.0f / 3 ? pp + (q - pp) * (2.0f / 3 - tt) * 6 : pp; }
            r = rgb[0] * 255; g = rgb[1] * 255; b = rgb[2] * 255;
        } else { r = pct[0] ? v[0] * 2.55f : v[0]; g = pct[1] ? v[1] * 2.55f : v[1]; b = pct[2] ? v[2] * 2.55f : v[2]; }
        #define CL(x) ((x) < 0 ? 0u : (x) > 255 ? 255u : (unsigned)((x) + 0.5f))
        *out = ((unsigned)(a * 255 + 0.5f) << 24) | (CL(r) << 16) | (CL(g) << 8) | CL(b);
        #undef CL
        return 1;
    }
    for (int i = 0; NAMED[i].n; i++) if (!strncasecmp(s, NAMED[i].n, strlen(NAMED[i].n)) && !isalpha((unsigned char)s[strlen(NAMED[i].n)])) { *out = 0xFF000000u | NAMED[i].c; return 1; }
    return 4;
}

/* ══════════════ стиль ══════════════ */
typedef struct {
    int fill_kind, stroke_kind; uint32_t fill, stroke; char fill_url[64], stroke_url[64];
    float fill_op, stroke_op, opacity;                                    /* opacity накапливается вниз по дереву */
    float sw; int cap, join; float miter; float dash[16]; int ndash; float dashoff;
    int evenodd, clip_evenodd, hidden, none; uint32_t color; float stop_op; uint32_t stop_color; int has_stop_color;
    char filter_url[64], clip_url[64], mask_url[64];
    char font_family[128]; int font_weight, font_italic, text_anchor; float font_size, letter_spacing;
} Style;
static void style_default(Style* s, uint32_t cur) {
    memset(s, 0, sizeof *s); s->fill_kind = 1; s->fill = 0xFF000000u; s->stroke_kind = 0; s->fill_op = s->stroke_op = s->opacity = 1; s->sw = 1; s->miter = 4; s->color = cur; s->stop_op = 1; s->stop_color = 0xFF000000u;
    snprintf(s->font_family, sizeof s->font_family, "%s", "sans-serif"); s->font_weight = 400; s->font_size = 16.0f;
}

/* CSS-правила из <style>: простые селекторы (тег, .класс, #id, *, составные тег.класс#id, списки через запятую) */
typedef struct { char tag[24]; char id[48]; char cls[4][48]; int ncls; int spec; int order; const char* decls; size_t dlen; } Rule;
typedef struct { Rule* r; int n, cap; } Rules;
static void rules_parse(Rules* rs, Arena* ar, const char* css) {
    int order = rs->n;
    while (*css) {
        while (*css && isspace((unsigned char)*css)) css++;
        if (css[0] == '/' && css[1] == '*') { const char* e = strstr(css + 2, "*/"); if (!e) return; css = e + 2; continue; }
        if (*css == '@') { int depth = 0; while (*css) { if (*css == '{') depth++; else if (*css == '}') { depth--; if (depth <= 0) { css++; break; } } else if (*css == ';' && depth == 0) { css++; break; } css++; } continue; }
        const char* sel = css; while (*css && *css != '{') css++;
        if (!*css) return;
        size_t sl = (size_t)(css - sel); css++; const char* body = css; while (*css && *css != '}') css++;
        size_t bl = (size_t)(css - body); if (*css) css++;
        char* sb = astrn(ar, sel, sl); char* save = NULL;
        for (char* part = strtok_r(sb, ",", &save); part; part = strtok_r(NULL, ",", &save)) {
            while (isspace((unsigned char)*part)) part++; size_t pl = strlen(part); while (pl && isspace((unsigned char)part[pl - 1])) part[--pl] = 0;
            if (!*part || strpbrk(part, " >+~[:")) continue;                  /* составные/псевдо-селекторы не поддерживаем */
            if (rs->n == rs->cap) { int nc = rs->cap ? rs->cap * 2 : 32; Rule* nv = realloc(rs->r, (size_t)nc * sizeof(Rule)); if (!nv) return; rs->r = nv; rs->cap = nc; }
            Rule* r = &rs->r[rs->n++]; memset(r, 0, sizeof *r); r->order = order++; r->decls = body; r->dlen = bl;
            const char* p = part; int valid = 1;
            if (*p != '.' && *p != '#' && *p != '*') {
                const char* tag = p; while (*p && *p != '.' && *p != '#') p++;
                size_t len = (size_t)(p - tag);
                if (!len || len >= sizeof r->tag) valid = 0;
                else { for (size_t j = 0; j < len; j++) r->tag[j] = (char)tolower((unsigned char)tag[j]); r->tag[len] = 0; r->spec += 1; }
            } else if (*p == '*') p++;
            while (*p && valid) {
                if (*p == '.') {
                    p++; const char* cls = p; while (*p && *p != '.' && *p != '#') p++;
                    size_t len = (size_t)(p - cls);
                    if (!len || len >= sizeof r->cls[0] || r->ncls >= 4) { valid = 0; break; }
                    memcpy(r->cls[r->ncls], cls, len); r->cls[r->ncls][len] = 0; r->ncls++; r->spec += 10;
                } else if (*p == '#') {
                    if (r->id[0]) { valid = 0; break; }
                    p++; const char* id = p; while (*p && *p != '.' && *p != '#') p++;
                    size_t len = (size_t)(p - id);
                    if (!len || len >= sizeof r->id) { valid = 0; break; }
                    memcpy(r->id, id, len); r->id[len] = 0; r->spec += 100;
                } else { valid = 0; break; }
            }
            if (!valid) rs->n--; /* an unrepresentable selector must not degrade to a weaker one */
        }
    }
}
static int has_class(const char* list, const char* cls) {
    size_t cl = strlen(cls);
    for (const char* p = list; *p;) { while (isspace((unsigned char)*p)) p++; const char* s = p; while (*p && !isspace((unsigned char)*p)) p++; if ((size_t)(p - s) == cl && !strncmp(s, cls, cl)) return 1; }
    return 0;
}
static int rule_match(const Rule* r, const SNode* n) {
    if (r->tag[0] && strcmp(r->tag, n->tag)) return 0;
    if (r->id[0]) { const char* v = attr_get(n, "id"); if (!v || strcmp(v, r->id)) return 0; }
    for (int i = 0; i < r->ncls; i++) { const char* v = attr_get(n, "class"); if (!v || !has_class(v, r->cls[i])) return 0; }
    return 1;
}

static void style_decl(Style* s, const char* name, const char* val) {
    char url[64];
    if (!strcmp(name, "fill") || !strcmp(name, "stroke")) {
        uint32_t c = 0; url[0] = 0; int k = parse_color(val, &c, s->color, url, sizeof url);
        if (k == 4) return;
        if (name[0] == 'f') { s->fill_kind = k == 2 ? 1 : k; s->fill = c; snprintf(s->fill_url, sizeof s->fill_url, "%s", url); }
        else { s->stroke_kind = k == 2 ? 1 : k; s->stroke = c; snprintf(s->stroke_url, sizeof s->stroke_url, "%s", url); }
    }
    else if (!strcmp(name, "fill-opacity") || !strcmp(name, "stroke-opacity") || !strcmp(name, "opacity") || !strcmp(name, "stop-opacity")) {
        char* e; float v = strtof(val, &e); if (e == val) return; if (*e == '%') v /= 100; if (v < 0) v = 0; if (v > 1) v = 1;
        if (name[0] == 'f') s->fill_op = v; else if (name[0] == 'o') s->opacity = v; else if (name[1] == 't' && name[2] == 'r') s->stroke_op = v; else s->stop_op = v;
    }
    else if (!strcmp(name, "stroke-width")) { float v = parse_len(val, 100, -1); if (v >= 0) s->sw = v; }
    else if (!strcmp(name, "stroke-linecap")) s->cap = !strncmp(val, "round", 5) ? 1 : !strncmp(val, "square", 6) ? 2 : 0;
    else if (!strcmp(name, "stroke-linejoin")) s->join = !strncmp(val, "round", 5) ? 1 : !strncmp(val, "bevel", 5) ? 2 : 0;
    else if (!strcmp(name, "stroke-miterlimit")) { float v = strtof(val, NULL); if (v >= 1) s->miter = v; }
    else if (!strcmp(name, "stroke-dasharray")) {
        s->ndash = 0; if (!strncmp(val, "none", 4)) return;
        const char* p = val; float v;
        while (s->ndash < 16 && parse_num(&p, &v)) { if (*p == '%') p++; s->dash[s->ndash++] = v < 0 ? 0 : v; }
        float sum = 0; for (int i = 0; i < s->ndash; i++) sum += s->dash[i]; if (sum <= 0) s->ndash = 0;
    }
    else if (!strcmp(name, "stroke-dashoffset")) s->dashoff = parse_len(val, 100, 0);
    else if (!strcmp(name, "fill-rule")) s->evenodd = !strncmp(val, "evenodd", 7);
    else if (!strcmp(name, "clip-rule")) s->clip_evenodd = !strncmp(val, "evenodd", 7);
    else if (!strcmp(name, "color")) { uint32_t c = 0; url[0] = 0; int k = parse_color(val, &c, s->color, url, sizeof url); if (k == 1) s->color = c; }
    else if (!strcmp(name, "display")) s->none = !strncmp(val, "none", 4);
    else if (!strcmp(name, "visibility")) s->hidden = !strncmp(val, "hidden", 6) || !strncmp(val, "collapse", 8);
    else if (!strcmp(name, "filter") || !strcmp(name, "clip-path") || !strcmp(name, "mask")) {
        char id[64] = {0}; const char* p = strstr(val, "url(");
        if (p) {
            p += 4;
            while (isspace((unsigned char)*p) || *p == '\'' || *p == '\"') p++;
            if (*p == '#') p++;
            const char* start = p;
            while (*p && *p != ')' && *p != '\'' && *p != '\"' && !isspace((unsigned char)*p)) p++;
            size_t len = (size_t)(p - start);
            if (len < sizeof id) { memcpy(id, start, len); id[len] = 0; }
            /* Overlong fragment identifiers are ignored rather than aliased. */
        }
        if (!strcmp(name, "filter")) memcpy(s->filter_url, id, strlen(id) + 1);
        else if (!strcmp(name, "clip-path")) memcpy(s->clip_url, id, strlen(id) + 1);
        else memcpy(s->mask_url, id, strlen(id) + 1);
    }
    else if (!strcmp(name, "font-size")) { float v = parse_len(val, s->font_size, -1); if (v > 0 && v <= 4096) s->font_size = v; }
    else if (!strcmp(name, "font-family")) { while (isspace((unsigned char)*val)) val++; size_t n = strlen(val); while (n && isspace((unsigned char)val[n-1])) n--; if (n >= 2 && ((val[0] == '\'' && val[n-1] == '\'') || (val[0] == '\"' && val[n-1] == '\"'))) { val++; n -= 2; } if (n >= sizeof s->font_family) return; memcpy(s->font_family, val, n); s->font_family[n] = 0; }
    else if (!strcmp(name, "font-weight")) { if (!strncmp(val, "bold", 4)) s->font_weight = 700; else if (!strncmp(val, "normal", 6)) s->font_weight = 400; else { int v = atoi(val); if (v >= 100 && v <= 900) s->font_weight = v; } }
    else if (!strcmp(name, "font-style")) s->font_italic = !strncmp(val, "italic", 6) || !strncmp(val, "oblique", 7);
    else if (!strcmp(name, "text-anchor")) s->text_anchor = !strncmp(val, "middle", 6) ? 1 : !strncmp(val, "end", 3) ? 2 : 0;
    else if (!strcmp(name, "letter-spacing")) s->letter_spacing = parse_len(val, s->font_size, 0);
    else if (!strcmp(name, "stop-color")) { uint32_t c = 0; url[0] = 0; int k = parse_color(val, &c, s->color, url, sizeof url); if (k == 1 || k == 2) { s->stop_color = c; s->has_stop_color = 1; } }
}
static void style_decls(Style* s, const char* d, size_t len) {                 /* "имя: значение; имя: значение" */
    char* buf = malloc(len + 1); if (!buf) return; memcpy(buf, d, len); buf[len] = 0; char* save = NULL;
    for (char* item = strtok_r(buf, ";", &save); item; item = strtok_r(NULL, ";", &save)) {
        char* c = strchr(item, ':'); if (!c) continue; *c = 0; char* nm = item; char* v = c + 1;
        while (isspace((unsigned char)*nm)) nm++; char* ne = nm + strlen(nm); while (ne > nm && isspace((unsigned char)ne[-1])) *--ne = 0;
        while (isspace((unsigned char)*v)) v++; size_t vl = strlen(v); while (vl && isspace((unsigned char)v[vl - 1])) v[--vl] = 0;
        char* imp = strstr(v, "!important"); if (imp) { *imp = 0; vl = strlen(v); while (vl && isspace((unsigned char)v[vl - 1])) v[--vl] = 0; }
        for (char* p = nm; *p; p++) *p = (char)tolower((unsigned char)*p);
        style_decl(s, nm, v);
    }
    free(buf);
}
static const char* PRES_ATTRS[] = {"fill", "stroke", "fill-opacity", "stroke-opacity", "opacity", "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
    "stroke-dasharray", "stroke-dashoffset", "fill-rule", "clip-rule", "color", "display", "visibility", "stop-color", "stop-opacity",
    "filter", "clip-path", "mask", "font-size", "font-family", "font-weight", "font-style", "text-anchor", "letter-spacing", NULL};

/* Спрайты: <use href="#id"> может ссылаться на <symbol> из ДРУГОГО встроенного svg страницы. html.c сохраняет такие svg
   в cache/sprite_<хэш origin>_<хэш>.svg, а в svg, использующие <use>, добавляет комментарий <!--mlsprite:<хэш origin>-->.
   Распарсенные спрайты держим в памяти (парсинг сотен символов на каждую иконку был бы слишком дорог). */
typedef struct { Doc* docs[8]; int n; char key[32]; char sig[1025]; } ExtSet;
static ExtSet g_ext;
typedef struct { Doc* doc; Rules rules; uint32_t cur; float vw, vh; ExtSet* ext; SNode* bypass_effect_node; int clip_mode, text_active; float text_x, text_y; } Ctx;
static SNode* ctx_by_id(Ctx* cx, const char* id) {
    SNode* n = doc_by_id(cx->doc, id); if (n) return n;
    if (cx->ext) for (int i = 0; i < cx->ext->n; i++) { n = doc_by_id(cx->ext->docs[i], id); if (n) return n; }
    return NULL;
}
static int rule_cmp_idx(const void* a, const void* b, void* ud) { Rules* rs = ud; const Rule* x = &rs->r[*(const int*)a]; const Rule* y = &rs->r[*(const int*)b]; if (x->spec != y->spec) return x->spec - y->spec; return x->order - y->order; }
static void compute_style(Ctx* cx, const SNode* n, const Style* parent, Style* out) {
    float pop = parent->opacity; *out = *parent;
    out->opacity = 1; out->none = 0; out->stop_op = 1; out->has_stop_color = 0; out->stop_color = 0xFF000000u;
    /* SVG filter/clip/mask не наследуются. */ out->filter_url[0] = out->clip_url[0] = out->mask_url[0] = 0;
    for (int i = 0; PRES_ATTRS[i]; i++) { const char* v = attr_get(n, PRES_ATTRS[i]); if (v) style_decl(out, PRES_ATTRS[i], v); }
    int idx[64], ni = 0;
    for (int i = 0; i < cx->rules.n && ni < 64; i++) if (rule_match(&cx->rules.r[i], n)) idx[ni++] = i;
    if (ni > 1) qsort_r(idx, (size_t)ni, sizeof(int), rule_cmp_idx, &cx->rules);
    for (int i = 0; i < ni; i++) style_decls(out, cx->rules.r[idx[i]].decls, cx->rules.r[idx[i]].dlen);
    const char* st = attr_get(n, "style"); if (st) style_decls(out, st, strlen(st));
    out->opacity *= pop;
}

/* ══════════════ контуры ══════════════ */
typedef struct { float* xy; int n, cap; int* st; unsigned char* cl; int ns, scap; } Path;       /* subpath i: точки [st[i], st[i+1]) ; st[ns] = n */
static void path_free(Path* p) { free(p->xy); free(p->st); free(p->cl); memset(p, 0, sizeof *p); }
static int path_pt(Path* p, float x, float y) {
    if (p->n == p->cap) { int nc = p->cap ? p->cap * 2 : 64; float* nv = realloc(p->xy, (size_t)nc * 2 * sizeof(float)); if (!nv) return 0; p->xy = nv; p->cap = nc; }
    if (p->n > 200000) return 0;
    p->xy[p->n * 2] = x; p->xy[p->n * 2 + 1] = y; p->n++; return 1;
}
static void path_begin(Path* p, float x, float y) {
    if (p->ns == p->scap) { int nc = p->scap ? p->scap * 2 : 8; int* a = realloc(p->st, (size_t)(nc + 1) * sizeof(int)); unsigned char* b = realloc(p->cl, (size_t)nc); if (!a || !b) return; p->st = a; p->cl = b; p->scap = nc; }
    p->st[p->ns] = p->n; p->cl[p->ns] = 0; p->ns++; path_pt(p, x, y); p->st[p->ns] = p->n;
}
static void path_line(Path* p, float x, float y) { if (!p->ns) path_begin(p, x, y); else { path_pt(p, x, y); p->st[p->ns] = p->n; } }
static void path_close(Path* p) { if (p->ns) p->cl[p->ns - 1] = 1; }
static void cubic_rec(Path* p, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, float tol, int depth) {
    float dx = x3 - x0, dy = y3 - y0, d1 = fabsf((x1 - x3) * dy - (y1 - y3) * dx), d2 = fabsf((x2 - x3) * dy - (y2 - y3) * dx);
    float len2 = dx * dx + dy * dy; float flat = (d1 + d2) * (d1 + d2) <= tol * tol * (len2 > 1e-12f ? len2 : 1e-12f);
    if (len2 <= 1e-12f) { float m = fmaxf(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)), fmaxf(fabsf(x2 - x0), fabsf(y2 - y0))); flat = m <= tol; }
    if (depth >= 10 || flat) { path_line(p, x3, y3); return; }
    float x01 = (x0 + x1) / 2, y01 = (y0 + y1) / 2, x12 = (x1 + x2) / 2, y12 = (y1 + y2) / 2, x23 = (x2 + x3) / 2, y23 = (y2 + y3) / 2;
    float xa = (x01 + x12) / 2, ya = (y01 + y12) / 2, xb = (x12 + x23) / 2, yb = (y12 + y23) / 2, xm = (xa + xb) / 2, ym = (ya + yb) / 2;
    cubic_rec(p, x0, y0, x01, y01, xa, ya, xm, ym, tol, depth + 1); cubic_rec(p, xm, ym, xb, yb, x23, y23, x3, y3, tol, depth + 1);
}
static void path_cubic(Path* p, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, float tol) { cubic_rec(p, x0, y0, x1, y1, x2, y2, x3, y3, tol, 0); }
static void path_arc(Path* p, float x0, float y0, float rx, float ry, float rot, int large, int sweep, float x1, float y1, float tol) {
    if (x0 == x1 && y0 == y1) return;
    rx = fabsf(rx); ry = fabsf(ry); if (rx < 1e-6f || ry < 1e-6f) { path_line(p, x1, y1); return; }
    float phi = rot * (float)M_PI / 180.0f, cp = cosf(phi), sp = sinf(phi);
    float dx2 = (x0 - x1) / 2, dy2 = (y0 - y1) / 2, xp = cp * dx2 + sp * dy2, yp = -sp * dx2 + cp * dy2;
    float lam = (xp * xp) / (rx * rx) + (yp * yp) / (ry * ry); if (lam > 1) { float s = sqrtf(lam); rx *= s; ry *= s; }
    float num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp, den = rx * rx * yp * yp + ry * ry * xp * xp;
    float co = den > 0 ? sqrtf(fmaxf(0, num / den)) : 0; if (large == sweep) co = -co;
    float cxp = co * rx * yp / ry, cyp = -co * ry * xp / rx, cx = cp * cxp - sp * cyp + (x0 + x1) / 2, cy = sp * cxp + cp * cyp + (y0 + y1) / 2;
    float th1 = atan2f((yp - cyp) / ry, (xp - cxp) / rx), th2 = atan2f((-yp - cyp) / ry, (-xp - cxp) / rx), dth = th2 - th1;
    if (!isfinite(dth) || !isfinite(cx) || !isfinite(cy)) { path_line(p, x1, y1); return; }      /* вырожденные/нечисловые параметры дуги */
    if (sweep && dth < 0) dth += 2 * (float)M_PI; else if (!sweep && dth > 0) dth -= 2 * (float)M_PI;
    float r = fmaxf(rx, ry); float step = 2 * acosf(fmaxf(-1, fminf(1, 1 - tol / fmaxf(r, tol * 2)))); if (step < 0.02f) step = 0.02f;
    int n = (int)ceilf(fabsf(dth) / step); if (n < 2) n = 2; if (n > 512) n = 512;
    for (int i = 1; i <= n; i++) {
        float t = th1 + dth * i / n, ct = cosf(t), st = sinf(t); path_line(p, cp * rx * ct - sp * ry * st + cx, sp * rx * ct + cp * ry * st + cy);
    }
    p->xy[(p->n - 1) * 2] = x1; p->xy[(p->n - 1) * 2 + 1] = y1;
}
static void path_ellipse(Path* p, float cx, float cy, float rx, float ry, float tol) {
    if (!isfinite(rx) || !isfinite(ry) || !isfinite(cx) || !isfinite(cy)) return;
    float r = fmaxf(rx, ry); float step = 2 * acosf(fmaxf(-1, fminf(1, 1 - tol / fmaxf(r, tol * 2)))); if (step < 0.05f) step = 0.05f;
    int n = (int)ceilf(2 * (float)M_PI / step); if (n < 8) n = 8; if (n > 720) n = 720;
    for (int i = 0; i < n; i++) { float t = 2 * (float)M_PI * i / n; float x = cx + rx * cosf(t), y = cy + ry * sinf(t); if (i == 0) path_begin(p, x, y); else path_line(p, x, y); }
    path_close(p);
}
static void path_from_d(Path* p, const char* d, float tol) {
    float cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0, lqx = 0, lqy = 0; char cmd = 0, prev = 0; int closed = 0; const char* s = d;
    for (;;) {
        s = skip_sep(s); if (!*s) break;
        if (isalpha((unsigned char)*s)) { cmd = *s++; if (cmd == 'Z' || cmd == 'z') { path_close(p); cx = sx; cy = sy; closed = 1; prev = 'z'; continue; } }
        else if (!cmd) break;
        int rel = islower((unsigned char)cmd); char C = (char)toupper((unsigned char)cmd); float v[7]; int need = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 : C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0;
        if (!need) break;
        for (int i = 0; i < need; i++) {
            const char* q = skip_sep(s);
            if (C == 'A' && (i == 3 || i == 4)) { if (*q != '0' && *q != '1') return; v[i] = (float)(*q - '0'); s = q + 1; continue; }     /* флаги дуги могут идти слитно */
            if (!parse_num(&q, &v[i])) return; s = q;
        }
        float ox = rel ? cx : 0, oy = rel ? cy : 0;
        if (C != 'M' && closed) { path_begin(p, cx, cy); closed = 0; }          /* после Z следующий сегмент без M начинает новый подконтур в той же точке */
        switch (C) {
        case 'M': closed = 0; cx = v[0] + ox; cy = v[1] + oy; sx = cx; sy = cy; path_begin(p, cx, cy); cmd = rel ? 'l' : 'L'; break;
        case 'L': cx = v[0] + ox; cy = v[1] + oy; path_line(p, cx, cy); break;
        case 'H': cx = v[0] + ox; path_line(p, cx, cy); break;
        case 'V': cy = v[0] + oy; path_line(p, cx, cy); break;
        case 'C': { float x1 = v[0] + ox, y1 = v[1] + oy, x2 = v[2] + ox, y2 = v[3] + oy, x3 = v[4] + ox, y3 = v[5] + oy; if (!p->ns) path_begin(p, cx, cy); path_cubic(p, cx, cy, x1, y1, x2, y2, x3, y3, tol); lcx = x2; lcy = y2; cx = x3; cy = y3; break; }
        case 'S': { float x1 = (prev == 'C' || prev == 'S') ? 2 * cx - lcx : cx, y1 = (prev == 'C' || prev == 'S') ? 2 * cy - lcy : cy; float x2 = v[0] + ox, y2 = v[1] + oy, x3 = v[2] + ox, y3 = v[3] + oy; if (!p->ns) path_begin(p, cx, cy); path_cubic(p, cx, cy, x1, y1, x2, y2, x3, y3, tol); lcx = x2; lcy = y2; cx = x3; cy = y3; break; }
        case 'Q': { float qx = v[0] + ox, qy = v[1] + oy, x3 = v[2] + ox, y3 = v[3] + oy; if (!p->ns) path_begin(p, cx, cy);
            path_cubic(p, cx, cy, cx + 2.0f / 3 * (qx - cx), cy + 2.0f / 3 * (qy - cy), x3 + 2.0f / 3 * (qx - x3), y3 + 2.0f / 3 * (qy - y3), x3, y3, tol); lqx = qx; lqy = qy; cx = x3; cy = y3; break; }
        case 'T': { float qx = (prev == 'Q' || prev == 'T') ? 2 * cx - lqx : cx, qy = (prev == 'Q' || prev == 'T') ? 2 * cy - lqy : cy; float x3 = v[0] + ox, y3 = v[1] + oy; if (!p->ns) path_begin(p, cx, cy);
            path_cubic(p, cx, cy, cx + 2.0f / 3 * (qx - cx), cy + 2.0f / 3 * (qy - cy), x3 + 2.0f / 3 * (qx - x3), y3 + 2.0f / 3 * (qy - y3), x3, y3, tol); lqx = qx; lqy = qy; cx = x3; cy = y3; break; }
        case 'A': { float x3 = v[5] + ox, y3 = v[6] + oy; if (!p->ns) path_begin(p, cx, cy); path_arc(p, cx, cy, v[0], v[1], v[2], (int)v[3], (int)v[4], x3, y3, tol); cx = x3; cy = y3; break; }
        }
        prev = C;
    }
}

/* ══════════════ набор многоугольников и обводка ══════════════ */
typedef struct { float* xy; int n, cap; int* st; int np, pcap; } PolySet;       /* многоугольник i: точки [st[i], st[i+1]) */
static void ps_free(PolySet* s) { free(s->xy); free(s->st); memset(s, 0, sizeof *s); }
static void ps_begin(PolySet* s) {
    if (s->np == s->pcap) { int nc = s->pcap ? s->pcap * 2 : 32; int* nv = realloc(s->st, (size_t)(nc + 1) * sizeof(int)); if (!nv) return; s->st = nv; s->pcap = nc; }
    s->st[s->np++] = s->n; s->st[s->np] = s->n;
}
static void ps_pt(PolySet* s, float x, float y) {
    if (!s->np) return;
    if (s->n == s->cap) { int nc = s->cap ? s->cap * 2 : 256; float* nv = realloc(s->xy, (size_t)nc * 2 * sizeof(float)); if (!nv) return; s->xy = nv; s->cap = nc; }
    if (s->n > 2000000) return;
    s->xy[s->n * 2] = x; s->xy[s->n * 2 + 1] = y; s->n++; s->st[s->np] = s->n;
}
static void ps_end(PolySet* s) {                                          /* ориентируем многоугольник положительно: объединение = ненулевое правило */
    if (!s->np) return;
    int a = s->st[s->np - 1], b = s->n; double area = 0;
    for (int i = a; i < b; i++) { int j = i + 1 < b ? i + 1 : a; area += (double)s->xy[i * 2] * s->xy[j * 2 + 1] - (double)s->xy[j * 2] * s->xy[i * 2 + 1]; }
    if (area < 0) for (int i = 0; i < (b - a) / 2; i++) { float x = s->xy[(a + i) * 2], y = s->xy[(a + i) * 2 + 1]; int k = b - 1 - i; s->xy[(a + i) * 2] = s->xy[k * 2]; s->xy[(a + i) * 2 + 1] = s->xy[k * 2 + 1]; s->xy[k * 2] = x; s->xy[k * 2 + 1] = y; }
}
static void ps_circle(PolySet* s, float cx, float cy, float r, int n) {
    ps_begin(s); for (int i = 0; i < n; i++) { float t = 2 * (float)M_PI * i / n; ps_pt(s, cx + r * cosf(t), cy + r * sinf(t)); } ps_end(s);
}
typedef struct { float* xy; int n, cap; } Line;
static void line_add(Line* l, float x, float y) {
    if (l->n && fabsf(l->xy[(l->n - 1) * 2] - x) < 1e-5f && fabsf(l->xy[(l->n - 1) * 2 + 1] - y) < 1e-5f) return;
    if (l->n == l->cap) { int nc = l->cap ? l->cap * 2 : 32; float* nv = realloc(l->xy, (size_t)nc * 2 * sizeof(float)); if (!nv) return; l->xy = nv; l->cap = nc; }
    l->xy[l->n * 2] = x; l->xy[l->n * 2 + 1] = y; l->n++;
}
typedef struct { float hw; int cap, join; float miter; int ncirc; } SK;
static void stroke_line(PolySet* ps, const float* xy, int n, int closed, const SK* k) {
    float hw = k->hw;
    if (n == 1) {                                                         /* точка: видна только с круглым/квадратным окончанием */
        if (k->cap == 1) ps_circle(ps, xy[0], xy[1], hw, k->ncirc);
        else if (k->cap == 2) { ps_begin(ps); ps_pt(ps, xy[0] - hw, xy[1] - hw); ps_pt(ps, xy[0] + hw, xy[1] - hw); ps_pt(ps, xy[0] + hw, xy[1] + hw); ps_pt(ps, xy[0] - hw, xy[1] + hw); ps_end(ps); }
        return;
    }
    int ns = closed ? n : n - 1;
    for (int i = 0; i < ns; i++) {                                        /* тела сегментов */
        int j = (i + 1) % n; float dx = xy[j * 2] - xy[i * 2], dy = xy[j * 2 + 1] - xy[i * 2 + 1], l = sqrtf(dx * dx + dy * dy); if (l < 1e-9f) continue;
        float nx = -dy / l * hw, ny = dx / l * hw;
        ps_begin(ps); ps_pt(ps, xy[i * 2] + nx, xy[i * 2 + 1] + ny); ps_pt(ps, xy[j * 2] + nx, xy[j * 2 + 1] + ny); ps_pt(ps, xy[j * 2] - nx, xy[j * 2 + 1] - ny); ps_pt(ps, xy[i * 2] - nx, xy[i * 2 + 1] - ny); ps_end(ps);
    }
    for (int v = closed ? 0 : 1; v < (closed ? n : n - 1); v++) {          /* соединения */
        int a = (v + n - 1) % n, b = (v + 1) % n;
        float d1x = xy[v * 2] - xy[a * 2], d1y = xy[v * 2 + 1] - xy[a * 2 + 1], d2x = xy[b * 2] - xy[v * 2], d2y = xy[b * 2 + 1] - xy[v * 2 + 1];
        float l1 = sqrtf(d1x * d1x + d1y * d1y), l2 = sqrtf(d2x * d2x + d2y * d2y); if (l1 < 1e-9f || l2 < 1e-9f) continue;
        d1x /= l1; d1y /= l1; d2x /= l2; d2y /= l2;
        float cross = d1x * d2y - d1y * d2x, dot = d1x * d2x + d1y * d2y;
        if (fabsf(cross) < 1e-4f && dot > 0) continue;                    /* прямой участок */
        if (dot > 0.9995f) continue;                                      /* почти прямой (гладкая кривая) — зазор пренебрежимо мал */
        float sgn = cross > 0 ? -1.0f : 1.0f;
        float n1x = -d1y * sgn, n1y = d1x * sgn, n2x = -d2y * sgn, n2y = d2x * sgn;
        float vx = xy[v * 2], vy = xy[v * 2 + 1];
        if (k->join == 1) { ps_circle(ps, vx, vy, hw, k->ncirc); continue; }
        float ax = vx + n1x * hw, ay = vy + n1y * hw, bx = vx + n2x * hw, by = vy + n2y * hw;
        int miter_ok = 0; float tx = 0, ty = 0;
        if (k->join == 0) { float c2 = sqrtf((1 + dot) / 2); if (c2 > 1e-4f && 1.0f / c2 <= k->miter) { float m = hw / (1 + dot); tx = vx + (n1x + n2x) * m; ty = vy + (n1y + n2y) * m; miter_ok = 1; } }
        ps_begin(ps); ps_pt(ps, vx, vy); ps_pt(ps, ax, ay); if (miter_ok) ps_pt(ps, tx, ty); ps_pt(ps, bx, by); ps_end(ps);
    }
    if (!closed && k->cap) {                                              /* окончания */
        for (int e = 0; e < 2; e++) {
            int p = e ? n - 1 : 0, q = e ? n - 2 : 1; float dx = xy[p * 2] - xy[q * 2], dy = xy[p * 2 + 1] - xy[q * 2 + 1], l = sqrtf(dx * dx + dy * dy); if (l < 1e-9f) continue;
            dx /= l; dy /= l; float px = xy[p * 2], py = xy[p * 2 + 1];
            if (k->cap == 1) ps_circle(ps, px, py, hw, k->ncirc);
            else { float nx = -dy * hw, ny = dx * hw; ps_begin(ps); ps_pt(ps, px + nx, py + ny); ps_pt(ps, px + nx + dx * hw, py + ny + dy * hw); ps_pt(ps, px - nx + dx * hw, py - ny + dy * hw); ps_pt(ps, px - nx, py - ny); ps_end(ps); }
        }
    }
}
/* штрихи: режем ломаную на отрезки «включено» по dash-массиву */
static void stroke_dashed(PolySet* ps, const float* xy, int n, int closed, const SK* k, const float* dash, int nd, float off) {
    float dd[32]; int m = nd; memcpy(dd, dash, sizeof(float) * (size_t)nd); if (nd & 1) { memcpy(dd + nd, dash, sizeof(float) * (size_t)nd); m = nd * 2; }
    float total = 0; for (int i = 0; i < m; i++) total += dd[i]; if (total <= 0) { stroke_line(ps, xy, n, closed, k); return; }
    off = fmodf(off, total); if (off < 0) off += total;
    int di = 0; float rem = dd[0]; while (off > 0) { if (off >= rem) { off -= rem; di = (di + 1) % m; rem = dd[di]; } else { rem -= off; off = 0; } }
    Line cur = {0}; int on = !(di & 1); int cnt = closed ? n + 1 : n;
    float px = xy[0], py = xy[1]; if (on) line_add(&cur, px, py); int guard = 0;
    for (int i = 1; i < cnt; i++) {
        int j = i % n; float qx = xy[j * 2], qy = xy[j * 2 + 1], dx = qx - px, dy = qy - py, len = sqrtf(dx * dx + dy * dy), pos = 0;
        while (len - pos > rem && guard++ < 200000) {
            pos += rem; float t = len > 0 ? pos / len : 0; float mx = px + dx * t, my = py + dy * t;
            if (on) { line_add(&cur, mx, my); if (cur.n >= 1) stroke_line(ps, cur.xy, cur.n, 0, k); cur.n = 0; } else { cur.n = 0; line_add(&cur, mx, my); }
            on = !on; di = (di + 1) % m; rem = dd[di]; if (rem <= 0) rem = 1e-3f;
        }
        rem -= len - pos; if (on) line_add(&cur, qx, qy); px = qx; py = qy;
    }
    if (on && cur.n >= 1) stroke_line(ps, cur.xy, cur.n, 0, k);
    free(cur.xy);
}

/* ══════════════ растеризация ══════════════ */
typedef struct { int w, h; uint32_t* px; } Canvas;
typedef struct { float* a; int w, h, stride; } Acc;
static void acc_line(Acc* A, float x0, float y0, float x1, float y1) {
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return;        /* накопление знаковой площади (font-rs); x прижимается к [0,w] */
    if (y0 == y1) return;
    float dir = y0 < y1 ? 1.0f : -1.0f; if (y0 > y1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y1 <= 0 || y0 >= A->h) return;
    float dxdy = (x1 - x0) / (y1 - y0), x = x0; if (y0 < 0) { x -= y0 * dxdy; y0 = 0; }
    int ys = (int)y0, ye = (int)ceilf(y1 < A->h ? y1 : (float)A->h); float W = (float)A->w;
    for (int y = ys; y < ye; y++) {
        float row = A->stride * (float)y; (void)row; float* line = A->a + (size_t)A->stride * (size_t)y;
        float dy = fminf((float)(y + 1), y1) - fmaxf((float)y, y0), xn = x + dxdy * dy, d = dy * dir;
        float xa = x, xb = xn; if (xa > xb) { float t = xa; xa = xb; xb = t; }
        if (xa < 0) xa = 0; if (xb < 0) xb = 0; if (xa > W) xa = W; if (xb > W) xb = W;
        float x0f = floorf(xa); int x0i = (int)x0f; float x1c = ceilf(xb); int x1i = (int)x1c;
        if (x1i <= x0i + 1) { float xmf = 0.5f * (xa + xb) - x0f; line[x0i] += d - d * xmf; line[x0i + 1] += d * xmf; }
        else {
            float s = 1.0f / (xb - xa), x0fr = xa - x0f, a0 = 0.5f * s * (1 - x0fr) * (1 - x0fr), x1fr = xb - x1c + 1, am = 0.5f * s * x1fr * x1fr;
            line[x0i] += d * a0;
            if (x1i == x0i + 2) line[x0i + 1] += d * (1 - a0 - am);
            else { float a1 = s * (1.5f - x0fr); line[x0i + 1] += d * (a1 - a0); for (int xi = x0i + 2; xi < x1i - 1; xi++) line[xi] += d * s; float a2 = a1 + (float)(x1i - x0i - 3) * s; line[x1i - 1] += d * (1 - a2 - am); }
            line[x1i] += d * am;
        }
        x = xn;
    }
}
typedef struct { float off; uint32_t c; } Stop;
typedef struct { int type; int nstops; Stop st[32]; Mat inv; int spread; float x1, y1, x2, y2, cx, cy, r; } GradPaint;       /* inv: устройство → пространство градиента */
typedef struct { int kind; uint32_t color; GradPaint g; float alpha; } Paint;
static uint32_t grad_color(const GradPaint* g, float t) {
    if (g->spread == 1) { t = fmodf(t, 2.0f); if (t < 0) t += 2; if (t > 1) t = 2 - t; }
    else if (g->spread == 2) { t = t - floorf(t); }
    else { if (t < 0) t = 0; if (t > 1) t = 1; }
    if (t <= g->st[0].off) return g->st[0].c;
    for (int i = 1; i < g->nstops; i++) if (t <= g->st[i].off) {
        float span = g->st[i].off - g->st[i - 1].off, f = span > 1e-6f ? (t - g->st[i - 1].off) / span : 1; uint32_t a = g->st[i - 1].c, b = g->st[i].c; uint32_t o = 0;
        for (int sh = 0; sh < 32; sh += 8) { float va = (float)((a >> sh) & 255), vb = (float)((b >> sh) & 255); o |= (uint32_t)(va + (vb - va) * f + 0.5f) << sh; }
        return o;
    }
    return g->st[g->nstops - 1].c;
}
static void blend(uint32_t* dst, uint32_t src, float cov) {                     /* src ARGB × покрытие поверх dst (оба без предумножения) */
    float sa = ((src >> 24) & 255) / 255.0f * cov; if (sa <= 0.0005f) return;
    uint32_t d = *dst; float da = (d >> 24) / 255.0f, oa = sa + da * (1 - sa);
    float sr = (float)((src >> 16) & 255), sg = (float)((src >> 8) & 255), sb = (float)(src & 255), dr = (float)((d >> 16) & 255), dg = (float)((d >> 8) & 255), db = (float)(d & 255);
    float k = da * (1 - sa);
    unsigned r = (unsigned)((sr * sa + dr * k) / oa + 0.5f), g = (unsigned)((sg * sa + dg * k) / oa + 0.5f), b = (unsigned)((sb * sa + db * k) / oa + 0.5f), a = (unsigned)(oa * 255 + 0.5f);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; if (a > 255) a = 255;
    *dst = (a << 24) | (r << 16) | (g << 8) | b;
}
static void raster(Canvas* cv, Mat m, const PolySet* ps, int evenodd, const Paint* pt) {
    if (!ps->np || pt->kind == 0 || pt->alpha <= 0) return;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < ps->n; i++) { float x, y; mat_pt(m, ps->xy[i * 2], ps->xy[i * 2 + 1], &x, &y); if (x != x || y != y) return; if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; }
    if (!(maxx > 0 && maxy > 0 && minx < (float)cv->w && miny < (float)cv->h)) return;      /* вне холста (или NaN) — до перевода во int, иначе переполнение */
    int bx0 = (int)floorf(fmaxf(minx, 0)), by0 = (int)floorf(fmaxf(miny, 0)), bx1 = (int)ceilf(fminf(maxx, (float)cv->w)), by1 = (int)ceilf(fminf(maxy, (float)cv->h));
    if (bx1 <= bx0) bx1 = bx0 + 1; if (by1 <= by0) by1 = by0 + 1;
    Acc A; A.w = bx1 - bx0; A.h = by1 - by0; A.stride = A.w + 2; A.a = calloc((size_t)A.stride * (size_t)A.h + 4, sizeof(float)); if (!A.a) return;
    for (int p = 0; p < ps->np; p++) {
        int a = ps->st[p], b = ps->st[p + 1]; if (b - a < 2) continue;
        float fx, fy; mat_pt(m, ps->xy[a * 2], ps->xy[a * 2 + 1], &fx, &fy); fx -= (float)bx0; fy -= (float)by0; float px = fx, py = fy;
        for (int i = a + 1; i < b; i++) { float x, y; mat_pt(m, ps->xy[i * 2], ps->xy[i * 2 + 1], &x, &y); x -= (float)bx0; y -= (float)by0; acc_line(&A, px, py, x, y); px = x; py = y; }
        acc_line(&A, px, py, fx, fy);
    }
    for (int y = 0; y < A.h; y++) {
        float sum = 0; const float* line = A.a + (size_t)A.stride * (size_t)y; uint32_t* drow = cv->px + (size_t)(by0 + y) * (size_t)cv->w + bx0;
        for (int x = 0; x < A.w; x++) {
            sum += line[x]; float cov = fabsf(sum);
            if (evenodd) { cov = fmodf(cov, 2.0f); if (cov > 1) cov = 2 - cov; } else if (cov > 1) cov = 1;
            if (cov < 0.003f) continue;
            uint32_t c = pt->color;
            if (pt->kind == 2) {
                float gx, gy; mat_pt(pt->g.inv, (float)(bx0 + x) + 0.5f, (float)(by0 + y) + 0.5f, &gx, &gy);
                float t;
                if (pt->g.type == 0) { float dx = pt->g.x2 - pt->g.x1, dy = pt->g.y2 - pt->g.y1, l2 = dx * dx + dy * dy; t = l2 > 1e-12f ? ((gx - pt->g.x1) * dx + (gy - pt->g.y1) * dy) / l2 : 0; }
                else { float dx = gx - pt->g.cx, dy = gy - pt->g.cy; t = pt->g.r > 1e-9f ? sqrtf(dx * dx + dy * dy) / pt->g.r : 0; }
                c = grad_color(&pt->g, t);
            }
            blend(&drow[x], c, cov * pt->alpha);
        }
    }
    free(A.a);
}

/* ══════════════ градиенты ══════════════ */
static const char* grad_attr(Ctx* cx, SNode* g, const char* name) {                /* атрибут с учётом цепочки href */
    for (int d = 0; g && d < 8; d++) {
        const char* v = attr_get(g, name); if (v) return v;
        const char* h = attr_get(g, "href"); if (!h || h[0] != '#') return NULL; g = ctx_by_id(cx, h + 1);
    }
    return NULL;
}
static SNode* grad_stops_owner(Ctx* cx, SNode* g) {
    for (int d = 0; g && d < 8; d++) {
        for (SNode* c = g->first; c; c = c->next) if (!strcmp(c->tag, "stop")) return g;
        const char* h = attr_get(g, "href"); if (!h || h[0] != '#') return NULL; g = ctx_by_id(cx, h + 1);
    }
    return NULL;
}
/* заполняет Paint для url(#id); bbox — габариты геометрии в пользовательском пространстве; 0 — градиент не применим */
static int build_gradient(Ctx* cx, const char* id, Mat ctm, const float* bbox, Paint* out, uint32_t* solid_out, int* is_solid) {
    SNode* g = ctx_by_id(cx, id); *is_solid = 0;
    if (!g || (strcmp(g->tag, "lineargradient") && strcmp(g->tag, "radialgradient"))) return 0;
    SNode* so = grad_stops_owner(cx, g); if (!so) return 0;
    GradPaint* gp = &out->g; memset(gp, 0, sizeof *gp); gp->type = !strcmp(g->tag, "radialgradient");
    Style base; style_default(&base, cx->cur); float last = 0;
    for (SNode* c = so->first; c; c = c->next) {
        if (strcmp(c->tag, "stop") || gp->nstops >= 32) continue;
        Style st; compute_style(cx, c, &base, &st);
        const char* o = attr_get(c, "offset"); float off = 0; if (o) { char* e; off = strtof(o, &e); if (*e == '%') off /= 100; }
        if (off < last) off = last; if (off > 1) off = 1; last = off;
        uint32_t col = st.has_stop_color ? st.stop_color : 0xFF000000u; float a = ((col >> 24) & 255) / 255.0f * st.stop_op;
        gp->st[gp->nstops].off = off; gp->st[gp->nstops].c = ((uint32_t)(a * 255 + 0.5f) << 24) | (col & 0xFFFFFF); gp->nstops++;
    }
    if (gp->nstops == 0) return 0;
    if (gp->nstops == 1) { *solid_out = gp->st[0].c; *is_solid = 1; return 1; }
    const char* units = grad_attr(cx, g, "gradientunits"); int user = units && !strncmp(units, "userSpaceOnUse", 14);
    const char* sp = grad_attr(cx, g, "spreadmethod"); gp->spread = sp ? (!strncmp(sp, "reflect", 7) ? 1 : !strncmp(sp, "repeat", 6) ? 2 : 0) : 0;
    float bw = bbox[2] - bbox[0], bh = bbox[3] - bbox[1];
    if (!user && (bw <= 1e-6f || bh <= 1e-6f)) return 0;
    float rw = user ? cx->vw : 1, rh = user ? cx->vh : 1;
    #define GL(n, rel, d) ({ const char* _v = grad_attr(cx, g, n); _v ? (strchr(_v, '%') ? parse_len(_v, rel, d) : parse_len(_v, rel, d)) : (d); })
    if (gp->type == 0) {
        gp->x1 = GL("x1", rw, 0); gp->y1 = GL("y1", rh, 0); gp->x2 = GL("x2", rw, user ? cx->vw : 1); gp->y2 = GL("y2", rh, 0);
    } else {
        gp->cx = GL("cx", rw, user ? cx->vw / 2 : 0.5f); gp->cy = GL("cy", rh, user ? cx->vh / 2 : 0.5f); gp->r = GL("r", (rw + rh) / 2, user ? (cx->vw + cx->vh) / 4 : 0.5f);
    }
    #undef GL
    Mat gt = mat_id(); const char* gts = grad_attr(cx, g, "gradienttransform"); if (gts) parse_transform(gts, &gt);
    Mat total = ctm; if (!user) { Mat bm = {bw, 0, 0, bh, bbox[0], bbox[1]}; total = mat_mul(total, bm); }
    total = mat_mul(total, gt);
    if (!mat_inv(total, &gp->inv)) return 0;
    out->kind = 2; return 1;
}

/* ══════════════ обход дерева ══════════════ */
static void parse_par(const char* s, int* ax, int* ay, int* slice, int* none) {      /* preserveAspectRatio */
    *ax = *ay = 1; *slice = 0; *none = 0; if (!s) return;
    while (isspace((unsigned char)*s)) s++; if (!strncmp(s, "defer", 5)) { s += 5; while (isspace((unsigned char)*s)) s++; }
    if (!strncmp(s, "none", 4)) { *none = 1; return; }
    if (s[0] == 'x' && s[1] && s[2] == 'Y') { *ax = !strncmp(s + 1, "Min", 3) ? 0 : !strncmp(s + 1, "Max", 3) ? 2 : 1; *ay = !strncmp(s + 4, "Min", 3) ? 0 : !strncmp(s + 4, "Max", 3) ? 2 : 1; }
    if (strstr(s, "slice")) *slice = 1;
}
static Mat vb_matrix(float vx, float vy, float vw, float vh, const char* par, float W, float H) {
    int ax, ay, slice, none; parse_par(par, &ax, &ay, &slice, &none); Mat m = mat_id();
    if (vw <= 0 || vh <= 0) return m;
    float sx = W / vw, sy = H / vh;
    if (none) { m.a = sx; m.d = sy; m.e = -vx * sx; m.f = -vy * sy; return m; }
    float s = slice ? fmaxf(sx, sy) : fminf(sx, sy);
    float tx = (W - vw * s) * (ax == 0 ? 0 : ax == 1 ? 0.5f : 1), ty = (H - vh * s) * (ay == 0 ? 0 : ay == 1 ? 0.5f : 1);
    m.a = s; m.d = s; m.e = tx - vx * s; m.f = ty - vy * s; return m;
}
static int parse_viewbox(const char* s, float* v) { if (!s) return 0; const char* p = s; for (int i = 0; i < 4; i++) if (!parse_num(&p, &v[i])) return 0; return v[2] > 0 && v[3] > 0; }

static void path_bbox(const Path* p, float* b) {
    b[0] = b[1] = 1e30f; b[2] = b[3] = -1e30f;
    for (int i = 0; i < p->n; i++) { float x = p->xy[i * 2], y = p->xy[i * 2 + 1]; if (x < b[0]) b[0] = x; if (x > b[2]) b[2] = x; if (y < b[1]) b[1] = y; if (y > b[3]) b[3] = y; }
}
/* op — множитель непрозрачности (fill-opacity/stroke-opacity × накопленный opacity); альфа самого цвета учитывается здесь */
static int make_paint(Ctx* cx, int kind, uint32_t color, const char* url, float op, Mat ctm, const float* bbox, Paint* pt) {
    memset(pt, 0, sizeof *pt);
    if (kind == 0) return 0;
    if (kind == 1) { pt->kind = 1; pt->color = color; pt->alpha = ((color >> 24) / 255.0f) * op; return pt->alpha > 0; }
    uint32_t solid = 0; int is_solid = 0;
    if (!build_gradient(cx, url, ctm, bbox, pt, &solid, &is_solid)) return 0;      /* ссылка на отсутствующий градиент → без заливки */
    if (is_solid) { pt->kind = 1; pt->color = solid; pt->alpha = ((solid >> 24) / 255.0f) * op; } else pt->alpha = op;
    return pt->alpha > 0;
}
static uint32_t paint_sample(const Paint* pt, float dx, float dy) {
    if (pt->kind != 2) return pt->color;
    float gx, gy; mat_pt(pt->g.inv, dx, dy, &gx, &gy);
    float t;
    if (pt->g.type == 0) { float vx = pt->g.x2 - pt->g.x1, vy = pt->g.y2 - pt->g.y1, l2 = vx * vx + vy * vy; t = l2 > 1e-12f ? ((gx - pt->g.x1) * vx + (gy - pt->g.y1) * vy) / l2 : 0; }
    else { float vx = gx - pt->g.cx, vy = gy - pt->g.cy; t = pt->g.r > 1e-9f ? sqrtf(vx * vx + vy * vy) / pt->g.r : 0; }
    return grad_color(&pt->g, t);
}

static void draw_path(Ctx* cx, Canvas* cv, Mat ctm, Path* path, const Style* st) {
    if (path->n < 1 || st->none) return;
    float bbox[4]; path_bbox(path, bbox);
    if (cx->clip_mode) {
        Paint maskpt = {0}; maskpt.kind = 1; maskpt.color = 0xFFFFFFFFu;
        maskpt.alpha = st->opacity * (st->hidden ? 0.0f : 1.0f);
        PolySet ps = {0};
        for (int i = 0; i < path->ns; i++) { int a = path->st[i], b = path->st[i + 1]; if (b - a < 2) continue; ps_begin(&ps); for (int k = a; k < b; k++) ps_pt(&ps, path->xy[k * 2], path->xy[k * 2 + 1]); }
        raster(cv, ctm, &ps, st->clip_evenodd || st->evenodd, &maskpt); ps_free(&ps);
        return;
    }
    float sc = mat_scale(ctm);
    Paint pt;
    if (!st->hidden && st->fill_kind && make_paint(cx, st->fill_kind, st->fill, st->fill_url, st->fill_op * st->opacity, ctm, bbox, &pt)) {
        PolySet ps = {0};
        for (int i = 0; i < path->ns; i++) { int a = path->st[i], b = path->st[i + 1]; if (b - a < 2) continue; ps_begin(&ps); for (int k = a; k < b; k++) ps_pt(&ps, path->xy[k * 2], path->xy[k * 2 + 1]); }
        raster(cv, ctm, &ps, st->evenodd, &pt); ps_free(&ps);
    }
    if (!st->hidden && st->stroke_kind && st->sw > 0) {
        if (make_paint(cx, st->stroke_kind, st->stroke, st->stroke_url, st->stroke_op * st->opacity, ctm, bbox, &pt)) {
            SK k; k.hw = st->sw / 2; k.cap = st->cap; k.join = st->join; k.miter = st->miter;
            float devr = k.hw * sc; if (!isfinite(devr)) devr = 1; float ang = acosf(fmaxf(-1, fminf(1, 1 - 0.15f / fmaxf(devr, 0.3f)))); int nc = ang < 0.04f ? 72 : (int)ceilf((float)M_PI / ang); if (nc < 8) nc = 8; if (nc > 72) nc = 72; k.ncirc = nc;
            PolySet ps = {0};
            for (int i = 0; i < path->ns; i++) {
                int a = path->st[i], b = path->st[i + 1]; if (b - a < 1) continue;
                Line ln = {0}; for (int q = a; q < b; q++) line_add(&ln, path->xy[q * 2], path->xy[q * 2 + 1]);
                int closed = path->cl[i]; if (closed && ln.n > 1 && fabsf(ln.xy[0] - ln.xy[(ln.n - 1) * 2]) < 1e-5f && fabsf(ln.xy[1] - ln.xy[(ln.n - 1) * 2 + 1]) < 1e-5f) ln.n--;
                if (ln.n >= 1) { if (st->ndash > 0 && ln.n >= 2) stroke_dashed(&ps, ln.xy, ln.n, closed && ln.n > 2, &k, st->dash, st->ndash, st->dashoff); else stroke_line(&ps, ln.xy, ln.n, closed && ln.n > 2, &k); }
                free(ln.xy);
            }
            raster(cv, ctm, &ps, 0, &pt); ps_free(&ps);
        }
    }
}
static int is_skipped_tag(const char* t) {
    static const char* skip[] = {"defs", "symbol", "clippath", "mask", "marker", "pattern", "lineargradient", "radialgradient", "filter", "style", "title", "desc", "metadata", "script", "image", "foreignobject", "stop", "animate", "set", "view", NULL};
    for (int i = 0; skip[i]; i++) if (!strcmp(t, skip[i])) return 1; return 0;
}
static void render_node(Ctx* cx, Canvas* cv, SNode* n, Mat ctm, const Style* parent, int depth, int use_depth);
static void render_children(Ctx* cx, Canvas* cv, SNode* n, Mat ctm, const Style* st, int depth, int use_depth) {
    int sw = !strcmp(n->tag, "switch");
    for (SNode* c = n->first; c; c = c->next) { render_node(cx, cv, c, ctm, st, depth + 1, use_depth); if (sw && !is_skipped_tag(c->tag)) break; }
}

/* XML text nodes are retained on their parent; collapse SVG's default whitespace. */
static char* svg_text_content(const SNode* n) {
    if (!n->text || !n->tlen) return NULL;
    char* decoded = malloc(n->tlen + 1); if (!decoded) return NULL;
    size_t dl = xml_decode(n->text, n->tlen, decoded); (void)dl;
    const char* sp = attr_get(n, "space"); int preserve = sp && !strcmp(sp, "preserve");
    if (preserve) return decoded;
    size_t r = 0, w = 0; int pending = 0;
    while (decoded[r]) {
        unsigned char c = (unsigned char)decoded[r++];
        if (isspace(c)) { if (w) pending = 1; continue; }
        if (pending && w) decoded[w++] = ' ';
        pending = 0; decoded[w++] = (char)c;
    }
    decoded[w] = 0; return decoded;
}

#ifdef SVG_WITH_TTF
static void draw_svg_text(Ctx* cx, Canvas* cv, const char* raw, float x, float baseline, Mat ctm, const Style* st) {
    if (!raw || !*raw || st->none || st->hidden) return;
    int fs = (int)floorf(st->font_size + 0.5f); if (fs < 1) fs = 1; if (fs > 512) fs = 512;
    TTFFont* font = font_manager_get_styled(st->font_family[0] ? st->font_family : "sans-serif", st->font_weight, st->font_italic);
    if (!font) return;
    float twf = ttf_text_width_f(font, raw, fs, (st->font_weight >= 600 ? TTF_BOLD : 0) | (st->font_italic ? TTF_ITALIC : 0));
    if (!(twf >= 0) || twf > 8190) return;
    float tw = twf + 4; int asc = ttf_font_ascent(font, fs), desc = ttf_font_descent(font, fs);
    if (asc < 1) asc = fs;
    if (desc < 0) desc = 0;
    int mw = (int)ceilf(tw); int mh = asc + desc + 6;
    if (mw < 4 || mh < 4 || (long)mw * mh > 16000000L) return;
    size_t count = (size_t)mw * (size_t)mh;
    uint32_t* black = calloc(count, sizeof(uint32_t)); uint32_t* white = malloc(count * sizeof(uint32_t));
    uint8_t* cov = malloc(count);
    if (!black || !white || !cov) { free(black); free(white); free(cov); return; }
    for (size_t i = 0; i < count; i++) white[i] = 0xFFFFFFu;
    int tstyle = (st->font_weight >= 600 ? TTF_BOLD : 0) | (st->font_italic ? TTF_ITALIC : 0);
    ttf_render_string_ex(font, raw, black, mw, 0, 0, mw, mh, fs, 2, 2, 0xFFFFFFu, tstyle);
    ttf_render_string_ex(font, raw, white, mw, 0, 0, mw, mh, fs, 2, 2, 0xFFFFFFu, tstyle);
    for (size_t i = 0; i < count; i++) {
        int dr = (int)((white[i] >> 16) & 255) - (int)((black[i] >> 16) & 255);
        int dg = (int)((white[i] >> 8) & 255) - (int)((black[i] >> 8) & 255);
        int db = (int)(white[i] & 255) - (int)(black[i] & 255);
        int a = 255 - (dr + dg + db) / 3; if (a < 0) a = 0; if (a > 255) a = 255; cov[i] = (uint8_t)a;
    }
    free(black); free(white);
    if (cx->clip_mode) {
        Mat inv; if (mat_inv(ctm, &inv)) {
            float y0 = baseline - asc;
            for (int py = 0; py < cv->h; py++) for (int px = 0; px < cv->w; px++) {
                float ux, uy; mat_pt(inv, px + 0.5f, py + 0.5f, &ux, &uy);
                int mx = (int)floorf(ux - x + 2), my = (int)floorf(uy - y0 + 2);
                if (mx < 0 || my < 0 || mx >= mw || my >= mh) continue;
                unsigned a = cov[(size_t)my * mw + mx]; if (!a) continue;
                uint32_t* dst = &cv->px[(size_t)py * cv->w + px]; blend(dst, 0xFFFFFFFFu, (float)a / 255.0f * st->opacity);
            }
        }
        free(cov); return;
    }
    if (!st->fill_kind) { free(cov); return; }
    float bbox[4] = {x, baseline - asc, x + twf, baseline + desc}; Paint pt;
    if (!make_paint(cx, st->fill_kind, st->fill, st->fill_url, st->fill_op * st->opacity, ctm, bbox, &pt)) { free(cov); return; }
    Mat inv; if (!mat_inv(ctm, &inv)) { free(cov); return; }
    /* Ограничиваем обход преобразованными углами текстовой маски. */
    float ax[4], ay[4]; mat_pt(ctm, x - 2, baseline - asc - 2, &ax[0], &ay[0]); mat_pt(ctm, x + mw - 2, baseline - asc - 2, &ax[1], &ay[1]);
    mat_pt(ctm, x - 2, baseline - asc + mh - 2, &ax[2], &ay[2]); mat_pt(ctm, x + mw - 2, baseline - asc + mh - 2, &ax[3], &ay[3]);
    float minx=ax[0], maxx=ax[0], miny=ay[0], maxy=ay[0]; for (int i=1;i<4;i++){if(ax[i]<minx)minx=ax[i];if(ax[i]>maxx)maxx=ax[i];if(ay[i]<miny)miny=ay[i];if(ay[i]>maxy)maxy=ay[i];}
    int x0=(int)fmaxf(0,floorf(minx)), x1=(int)fminf(cv->w,ceilf(maxx)), y0=(int)fmaxf(0,floorf(miny)), y1=(int)fminf(cv->h,ceilf(maxy));
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        float ux, uy; mat_pt(inv, px + 0.5f, py + 0.5f, &ux, &uy);
        int mx = (int)floorf(ux - x + 2), my = (int)floorf(uy - (baseline - asc) + 2);
        if (mx < 0 || my < 0 || mx >= mw || my >= mh) continue;
        unsigned a = cov[(size_t)my * mw + mx]; if (!a) continue;
        uint32_t color = paint_sample(&pt, px + 0.5f, py + 0.5f);
        blend(&cv->px[(size_t)py * cv->w + px], color, (float)a / 255.0f * pt.alpha);
    }
    free(cov);
}
#endif

static int canvas_user_bbox(const Canvas* cv, const uint32_t* px, Mat ctm, float b[4]) {
    int x0=cv->w, y0=cv->h, x1=0, y1=0;
    for (int y=0;y<cv->h;y++) for (int x=0;x<cv->w;x++) if (px[(size_t)y*cv->w+x] >> 24) { if(x<x0)x0=x;if(x+1>x1)x1=x+1;if(y<y0)y0=y;if(y+1>y1)y1=y+1; }
    Mat inv; if (x1<=x0 || y1<=y0 || !mat_inv(ctm,&inv)) return 0;
    float x[4], y[4]; mat_pt(inv,x0,y0,&x[0],&y[0]); mat_pt(inv,x1,y0,&x[1],&y[1]); mat_pt(inv,x0,y1,&x[2],&y[2]); mat_pt(inv,x1,y1,&x[3],&y[3]);
    b[0]=b[2]=x[0]; b[1]=b[3]=y[0];
    for(int i=1;i<4;i++){if(x[i]<b[0])b[0]=x[i];if(x[i]>b[2])b[2]=x[i];if(y[i]<b[1])b[1]=y[i];if(y[i]>b[3])b[3]=y[i];}
    return b[2]>b[0] && b[3]>b[1];
}
static float svg_num_attr(const SNode* n, const char* name, float rel, float def) {
    const char* s=attr_get(n,name); return s ? parse_len(s,rel,def) : def;
}
static Mat bbox_matrix(const float b[4]) { Mat m={b[2]-b[0],0,0,b[3]-b[1],b[0],b[1]}; return m; }
static void canvas_blend_over(Canvas* dst, const Canvas* src) {
    size_t n=(size_t)dst->w*dst->h; for(size_t i=0;i<n;i++) if(src->px[i]>>24) blend(&dst->px[i],src->px[i],1.0f);
}
static void svg_apply_clip(Ctx* cx, SNode* cp, Canvas* layer, Mat target_ctm, const Style* target_style, const float bbox[4], int have_bbox, int depth, int use_depth) {
    size_t n=(size_t)layer->w*layer->h; uint32_t* mpx=calloc(n,sizeof(uint32_t)); if(!mpx)return;
    Canvas mask={layer->w,layer->h,mpx}; Mat cm=target_ctm;
    const char* tr=attr_get(cp,"transform"); if(tr){Mat t;if(parse_transform(tr,&t))cm=mat_mul(cm,t);}
    const char* units=attr_get(cp,"clippathunits");
    if(units && !strncmp(units,"objectBoundingBox",17) && have_bbox) cm=mat_mul(cm,bbox_matrix(bbox));
    Ctx sub=*cx; sub.clip_mode=1; sub.bypass_effect_node=NULL; sub.text_active=0;
    Style clipbase=*target_style; clipbase.opacity=1.0f; clipbase.fill_op=clipbase.stroke_op=1.0f; clipbase.none=0; clipbase.hidden=0;
    render_children(&sub,&mask,cp,cm,&clipbase,depth+1,use_depth);
    for(size_t i=0;i<n;i++){ unsigned a=(mpx[i]>>24)&255; if(!a){layer->px[i]=0;continue;} unsigned da=(layer->px[i]>>24)&255; unsigned na=(da*a+127)/255; layer->px[i]=(layer->px[i]&0x00FFFFFFu)|(na<<24); }
    free(mpx);
}
static void svg_apply_mask(Ctx* cx, SNode* mn, Canvas* layer, Mat target_ctm, const Style* target_style, const float bbox[4], int have_bbox, int depth, int use_depth) {
    size_t n=(size_t)layer->w*layer->h; uint32_t* mpx=calloc(n,sizeof(uint32_t)); if(!mpx)return;
    Canvas mask={layer->w,layer->h,mpx}; Mat cm=target_ctm;
    const char* cu=attr_get(mn,"maskcontentunits");
    if(cu && !strncmp(cu,"objectBoundingBox",17) && have_bbox) cm=mat_mul(cm,bbox_matrix(bbox));
    Style base; style_default(&base,cx->cur); Style ms; compute_style(cx,mn,&base,&ms);
    Ctx sub=*cx; sub.bypass_effect_node=NULL; sub.clip_mode=0; sub.text_active=0;
    render_children(&sub,&mask,mn,cm,&ms,depth+1,use_depth);
    const char* type=attr_get(mn,"mask-type"); int alpha_mode=type && !strncmp(type,"alpha",5);
    /* SVG default region: x/y=-10%, width/height=120% of the target bbox. */
    float rx,ry,rw,rh;
    const char* units=attr_get(mn,"maskunits"); int objunits=!units || strncmp(units,"userSpaceOnUse",14)!=0;
    if(objunits && have_bbox){
        rx=bbox[0]+svg_num_attr(mn,"x",1,-0.1f)*(bbox[2]-bbox[0]);
        ry=bbox[1]+svg_num_attr(mn,"y",1,-0.1f)*(bbox[3]-bbox[1]);
        rw=svg_num_attr(mn,"width",1,1.2f)*(bbox[2]-bbox[0]);
        rh=svg_num_attr(mn,"height",1,1.2f)*(bbox[3]-bbox[1]);
    } else {
        rx=svg_num_attr(mn,"x",cx->vw,-0.1f*(have_bbox?bbox[2]-bbox[0]:cx->vw));
        ry=svg_num_attr(mn,"y",cx->vh,-0.1f*(have_bbox?bbox[3]-bbox[1]:cx->vh));
        rw=svg_num_attr(mn,"width",cx->vw,1.2f*(have_bbox?bbox[2]-bbox[0]:cx->vw));
        rh=svg_num_attr(mn,"height",cx->vh,1.2f*(have_bbox?bbox[3]-bbox[1]:cx->vh));
    }
    Mat inv; int invok=mat_inv(target_ctm,&inv);
    for(int y=0;y<layer->h;y++) for(int x=0;x<layer->w;x++){
        size_t i=(size_t)y*layer->w+x; uint32_t v=mpx[i]; unsigned ma=(v>>24)&255;
        if(!alpha_mode) ma=(ma*(unsigned)(0.2126f*((v>>16)&255)+0.7152f*((v>>8)&255)+0.0722f*(v&255)+0.5f)+127)/255;
        if(invok){float ux,uy;mat_pt(inv,x+0.5f,y+0.5f,&ux,&uy);if(ux<rx||uy<ry||ux>=rx+rw||uy>=ry+rh)ma=0;}
        unsigned da=(layer->px[i]>>24)&255, na=(da*ma+127)/255;
        layer->px[i]=(layer->px[i]&0x00FFFFFFu)|(na<<24);
    }
    free(mpx);
    (void)target_style;
}

/* Маленькие вспомогательные функции для фильтров SVG. Буферы хранят straight ARGB. */
static unsigned svg_clamp_byte(float x){if(!(x>0))return 0;if(x>=255)return 255;return (unsigned)(x+0.5f);}
static uint32_t svg_over_pixel(uint32_t s,uint32_t d){
    float sa=((s>>24)&255)/255.0f, da=((d>>24)&255)/255.0f, oa=sa+da*(1-sa); if(oa<=1e-8f)return 0;
    float sr=(s>>16)&255,sg=(s>>8)&255,sb=s&255,dr=(d>>16)&255,dg=(d>>8)&255,db=d&255,k=da*(1-sa);
    unsigned r=svg_clamp_byte((sr*sa+dr*k)/oa),g=svg_clamp_byte((sg*sa+dg*k)/oa),b=svg_clamp_byte((sb*sa+db*k)/oa),a=svg_clamp_byte(oa*255);
    return (a<<24)|(r<<16)|(g<<8)|b;
}
static uint32_t svg_composite_pixel(uint32_t s,uint32_t d,const char* op,float k1,float k2,float k3,float k4){
    float sa=((s>>24)&255)/255.0f, da=((d>>24)&255)/255.0f;
    float sc[3]={((s>>16)&255)/255.0f,((s>>8)&255)/255.0f,(s&255)/255.0f};
    float dc[3]={((d>>16)&255)/255.0f,((d>>8)&255)/255.0f,(d&255)/255.0f};
    if(op && !strcmp(op,"arithmetic")){
        float a[4]={sc[0]*sa,sc[1]*sa,sc[2]*sa,sa}, b[4]={dc[0]*da,dc[1]*da,dc[2]*da,da}, o[4];
        for(int c=0;c<4;c++) o[c]=fmaxf(0.0f,fminf(1.0f,k1*a[c]*b[c]+k2*a[c]+k3*b[c]+k4));
        float oa=o[3]; if(oa<=1e-8f)return 0;
        return (svg_clamp_byte(oa*255)<<24)|(svg_clamp_byte(o[0]/oa*255)<<16)|(svg_clamp_byte(o[1]/oa*255)<<8)|svg_clamp_byte(o[2]/oa*255);
    }
    float fs=1.0f, fd=1.0f;
    if(op && !strcmp(op,"in")){fs=da;fd=0;}
    else if(op && !strcmp(op,"out")){fs=1-da;fd=0;}
    else if(op && !strcmp(op,"atop")){fs=da;fd=1-sa;}
    else if(op && !strcmp(op,"xor")){fs=1-da;fd=1-sa;}
    else if(op && !strcmp(op,"over")){fs=1;fd=1-sa;}
    else { /* SVG default operator is over. */ fs=1;fd=1-sa; }
    float outa=sa*fs+da*fd;
    if(outa<=1e-8f)return 0;
    float out[3];for(int c=0;c<3;c++)out[c]=(sc[c]*sa*fs+dc[c]*da*fd)/outa;
    return (svg_clamp_byte(outa*255)<<24)|(svg_clamp_byte(out[0]*255)<<16)|(svg_clamp_byte(out[1]*255)<<8)|svg_clamp_byte(out[2]*255);
}
static void svg_box_pass(uint8_t* src,uint8_t* dst,int w,int h,int radius,int horizontal){
    if(radius<1){memcpy(dst,src,(size_t)w*h*4);return;}
    int lines=horizontal?h:w, len=horizontal?w:h, span=radius*2+1;
    for(int line=0;line<lines;line++){
        unsigned sums[4]={0,0,0,0};
        for(int k=-radius;k<=radius;k++){int q=k<0?0:k>=len?len-1:k;size_t z=((size_t)(horizontal?line:q)*w+(horizontal?q:line))*4;for(int c=0;c<4;c++)sums[c]+=src[z+c];}
        for(int j=0;j<len;j++){
            size_t z=((size_t)(horizontal?line:j)*w+(horizontal?j:line))*4;
            for(int c=0;c<4;c++)dst[z+c]=(uint8_t)(sums[c]/(unsigned)span);
            int old=j-radius, add=j+radius+1; if(old<0)old=0;if(old>=len)old=len-1;if(add<0)add=0;if(add>=len)add=len-1;
            size_t zo=((size_t)(horizontal?line:old)*w+(horizontal?old:line))*4, za=((size_t)(horizontal?line:add)*w+(horizontal?add:line))*4;
            for(int c=0;c<4;c++)sums[c]=sums[c]-src[zo+c]+src[za+c];
        }
    }
}
static void svg_blur(uint32_t* px,int w,int h,float sigma){
    if(!(sigma>0.01f)||!isfinite(sigma)) return;
    if(sigma>32) sigma=32;
    size_t n=(size_t)w*h; uint8_t* a=malloc(n*4),*b=malloc(n*4);if(!a||!b){free(a);free(b);return;}
    for(size_t i=0;i<n;i++){uint32_t v=px[i];unsigned al=v>>24; a[i*4]=(uint8_t)((((v>>16)&255)*al+127)/255);a[i*4+1]=(uint8_t)((((v>>8)&255)*al+127)/255);a[i*4+2]=(uint8_t)(((v&255)*al+127)/255);a[i*4+3]=(uint8_t)al;}
    int radius=(int)ceilf(sigma*0.57735f); if(radius<1)radius=1;if(radius>24)radius=24;
    for(int it=0;it<3;it++){svg_box_pass(a,b,w,h,radius,1);svg_box_pass(b,a,w,h,radius,0);}
    for(size_t i=0;i<n;i++){unsigned al=a[i*4+3],r=0,g=0,bl=0;if(al){r=(a[i*4]*255u+al/2)/al;g=(a[i*4+1]*255u+al/2)/al;bl=(a[i*4+2]*255u+al/2)/al;if(r>255)r=255;if(g>255)g=255;if(bl>255)bl=255;}px[i]=(al<<24)|(r<<16)|(g<<8)|bl;}
    free(a);free(b);
}
static void svg_offset(const uint32_t* in,uint32_t* out,int w,int h,int dx,int dy){
    memset(out,0,(size_t)w*h*4); for(int y=0;y<h;y++){int sy=y-dy;if(sy<0||sy>=h)continue;for(int x=0;x<w;x++){int sx=x-dx;if(sx>=0&&sx<w)out[(size_t)y*w+x]=in[(size_t)sy*w+sx];}}
}
static void svg_color_matrix(const uint32_t* in,uint32_t* out,int w,int h,const SNode* p){
    const char* ty=attr_get(p,"type"),*vs=attr_get(p,"values"); float m[20]={1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,1,0};
    if(ty&&!strcmp(ty,"saturate")){float sat=vs?strtof(vs,NULL):1;float rr=.213f+.787f*sat,rg=.715f-.715f*sat,rb=.072f-.072f*sat;m[0]=rr;m[1]=rg;m[2]=rb;m[5]=.213f-.213f*sat;m[6]=.715f+.285f*sat;m[7]=.072f-.072f*sat;m[10]=.213f-.213f*sat;m[11]=.715f-.715f*sat;m[12]=.072f+.928f*sat;}
    else if(ty&&!strcasecmp(ty,"huerotate")){float deg=vs?strtof(vs,NULL):0,co=cosf(deg*(float)M_PI/180),si=sinf(deg*(float)M_PI/180);float a=.213f,b=.715f,c=.072f;m[0]=a+co*(1-a)+si*(-a);m[1]=b+co*(-b)+si*(-b);m[2]=c+co*(-c)+si*(1-c);m[5]=a+co*(-a)+si*.143f;m[6]=b+co*(1-b)+si*.140f;m[7]=c+co*(-c)+si*(-.283f);m[10]=a+co*(-a)+si*(-(1-a));m[11]=b+co*(-b)+si*b;m[12]=c+co*(1-c)+si*c;}
    else if(ty&&!strcasecmp(ty,"luminancetoalpha")){memset(m,0,sizeof m);m[15]=.2126f;m[16]=.7152f;m[17]=.0722f;}
    else if(vs){const char* q=vs;float vals[20];int nv=0;while(nv<20&&parse_num(&q,&vals[nv]))nv++;if(nv>=20)memcpy(m,vals,sizeof m);}
    size_t n=(size_t)w*h;for(size_t i=0;i<n;i++){uint32_t v=in[i];float r=((v>>16)&255)/255.f,g=((v>>8)&255)/255.f,b=(v&255)/255.f,a=(v>>24)/255.f,iv[4]={r,g,b,a};float ov[4];for(int row=0;row<4;row++)ov[row]=m[row*5]*iv[0]+m[row*5+1]*iv[1]+m[row*5+2]*iv[2]+m[row*5+3]*iv[3]+m[row*5+4];unsigned R=svg_clamp_byte(fmaxf(0,fminf(1,ov[0]))*255),G=svg_clamp_byte(fmaxf(0,fminf(1,ov[1]))*255),B=svg_clamp_byte(fmaxf(0,fminf(1,ov[2]))*255),A=svg_clamp_byte(fmaxf(0,fminf(1,ov[3]))*255);out[i]=(A<<24)|(R<<16)|(G<<8)|B;}
}
static uint32_t svg_blend_pixel(uint32_t s,uint32_t d,const char* mode){
    float sa=(s>>24)/255.f,da=(d>>24)/255.f;if(sa<=0)return d;if(da<=0)return s;
    float cs[3]={((s>>16)&255)/255.f,((s>>8)&255)/255.f,(s&255)/255.f},cd[3]={((d>>16)&255)/255.f,((d>>8)&255)/255.f,(d&255)/255.f};float oa=sa+da-sa*da,o[3];
    for(int c=0;c<3;c++){float b=cs[c];if(mode&&!strcmp(mode,"multiply"))b=cs[c]*cd[c];else if(mode&&!strcmp(mode,"screen"))b=cs[c]+cd[c]-cs[c]*cd[c];else if(mode&&!strcmp(mode,"darken"))b=fminf(cs[c],cd[c]);else if(mode&&!strcmp(mode,"lighten"))b=fmaxf(cs[c],cd[c]);o[c]=((1-da)*sa*cs[c]+(1-sa)*da*cd[c]+sa*da*b)/oa;}
    return (svg_clamp_byte(oa*255)<<24)|(svg_clamp_byte(o[0]*255)<<16)|(svg_clamp_byte(o[1]*255)<<8)|svg_clamp_byte(o[2]*255);
}
typedef struct { char name[64]; uint32_t* px; } SVGFilterResult;
static uint32_t* svg_filter_find(SVGFilterResult* r,int nr,const char* name,uint32_t* fallback){if(!name||!*name)return fallback;for(int i=nr-1;i>=0;i--)if(r[i].px&&r[i].name[0]&&!strcmp(r[i].name,name))return r[i].px;return fallback;}
static int svg_filter_future_uses(SNode* filter,SNode* after,const char* name){
    if(!name||!*name)return 0;
    for(SNode* p=after?after->next:filter->first;p;p=p->next){
        if(strncmp(p->tag,"fe",2))continue;
        const char* in=attr_get(p,"in"),*in2=attr_get(p,"in2");
        if((in&&!strcmp(in,name))||(in2&&!strcmp(in2,name)))return 1;
        if(!strcmp(p->tag,"femerge"))for(SNode* m=p->first;m;m=m->next){const char* mi=attr_get(m,"in");if(mi&&!strcmp(mi,name))return 1;}
    }
    return 0;
}
static void svg_filter_apply(Ctx* cx,SNode* filter,Canvas* layer,Mat ctm,const float bbox[4],int have_bbox){
    int w=layer->w,h=layer->h;size_t n=(size_t)w*h;if(n>12000000u)return;
    SVGFilterResult res[40];memset(res,0,sizeof res);int nr=0;
    uint32_t* source=malloc(n*4);if(!source)return;memcpy(source,layer->px,n*4);snprintf(res[nr].name,sizeof res[nr].name,"SourceGraphic");res[nr++].px=source;uint32_t* alpha=calloc(n,4);if(!alpha)goto done;for(size_t i=0;i<n;i++)alpha[i]=((source[i]>>24)<<24)|0xFFFFFFu;snprintf(res[nr].name,sizeof res[nr].name,"SourceAlpha");res[nr++].px=alpha;
    uint32_t* last=source;float scale=mat_scale(ctm);
    for(SNode* p=filter->first;p&&nr<38;p=p->next){const char* tg=p->tag;if(strncmp(tg,"fe",2))continue;const char* iname=attr_get(p,"in"),*in2name=attr_get(p,"in2"),*result=attr_get(p,"result");uint32_t* in=svg_filter_find(res,nr,iname,last);uint32_t* in2=svg_filter_find(res,nr,in2name,source);uint32_t* out=calloc(n,4);if(!out)break;
        if(!strcmp(tg,"fegaussianblur")){memcpy(out,in,n*4);const char* sd=attr_get(p,"stddeviation");float sx=0,sy=0;if(sd){const char* q=sd;parse_num(&q,&sx);if(!parse_num(&q,&sy))sy=sx;}float sig=fmaxf(sx,sy)*scale;svg_blur(out,w,h,sig);}
        else if(!strcmp(tg,"feoffset")){float dx=svg_num_attr(p,"dx",cx->vw,0),dy=svg_num_attr(p,"dy",cx->vh,0);float ox,oy;mat_pt(ctm,dx,dy,&ox,&oy);float zx,zy;mat_pt(ctm,0,0,&zx,&zy);svg_offset(in,out,w,h,(int)lroundf(ox-zx),(int)lroundf(oy-zy));}
        else if(!strcmp(tg,"fecolormatrix")){svg_color_matrix(in,out,w,h,p);}
        else if(!strcmp(tg,"feflood")){const char* fc=attr_get(p,"flood-color");uint32_t col=0xFF000000u;char url[64]={0};if(fc)parse_color(fc,&col,cx->cur,url,sizeof url);float op=svg_num_attr(p,"flood-opacity",1,1);unsigned a=svg_clamp_byte(((col>>24)&255)*op);uint32_t v=(a<<24)|(col&0xFFFFFFu);for(size_t i=0;i<n;i++)out[i]=v;}
        else if(!strcmp(tg,"feblend")){const char* mode=attr_get(p,"mode");for(size_t i=0;i<n;i++)out[i]=svg_blend_pixel(in[i],in2[i],mode);}
        else if(!strcmp(tg,"fecomposite")){const char* op=attr_get(p,"operator");float k1=svg_num_attr(p,"k1",1,0),k2=svg_num_attr(p,"k2",1,0),k3=svg_num_attr(p,"k3",1,0),k4=svg_num_attr(p,"k4",1,0);for(size_t i=0;i<n;i++)out[i]=svg_composite_pixel(in[i],in2[i],op,k1,k2,k3,k4);}
        else if(!strcmp(tg,"femerge")){for(SNode* mn=p->first;mn;mn=mn->next)if(!strcmp(mn->tag,"femergenode")){uint32_t* src=svg_filter_find(res,nr,attr_get(mn,"in"),source);for(size_t i=0;i<n;i++)out[i]=svg_over_pixel(src[i],out[i]);}}
        else if(!strcmp(tg,"fedropshadow")){memcpy(out,in,n*4);uint32_t* sh=calloc(n,4);if(sh){for(size_t i=0;i<n;i++){unsigned a=in[i]>>24;sh[i]=(a<<24)|0xFFFFFFu;}const char* sd=attr_get(p,"stddeviation");float sig=sd?strtof(sd,NULL)*scale:0;svg_blur(sh,w,h,sig);uint32_t* off=calloc(n,4);if(off){float dx=svg_num_attr(p,"dx",cx->vw,2),dy=svg_num_attr(p,"dy",cx->vh,2),ax,ay,zx,zy;mat_pt(ctm,dx,dy,&ax,&ay);mat_pt(ctm,0,0,&zx,&zy);svg_offset(sh,off,w,h,(int)lroundf(ax-zx),(int)lroundf(ay-zy));const char* fc=attr_get(p,"flood-color");uint32_t col=0xFF000000u;char uu[64]={0};if(fc)parse_color(fc,&col,cx->cur,uu,sizeof uu);float fop=svg_num_attr(p,"flood-opacity",1,1);for(size_t i=0;i<n;i++){unsigned a=((off[i]>>24)*((col>>24)&255)+127)/255;a=svg_clamp_byte(a*fop);uint32_t sv=(a<<24)|(col&0xFFFFFFu);out[i]=svg_over_pixel(in[i],sv);}free(off);}free(sh);}}
        else {memcpy(out,in,n*4);} /* неизвестный примитив сохраняет вход, не обрывая цепочку */
        /* Не держим всю цепочку RGBA в памяти: сохраняем промежуточный буфер только если на него ссылается будущий primitive. */
        if(last!=source && last!=alpha){
            for(int ri=2;ri<nr;ri++)if(res[ri].px==last){
                if(!svg_filter_future_uses(filter,p,res[ri].name)){free(res[ri].px);res[ri].px=NULL;}
                break;
            }
        }
        last=out;snprintf(res[nr].name,sizeof res[nr].name,"%s",result?result:"");res[nr++].px=out;
    }
    /* Фильтр по умолчанию выводится в bbox, расширенный на 10%; это также не даёт feFlood закрасить весь viewport. */
    float region[4]={0,0,(float)cx->vw,(float)cx->vh};
    const char* units=attr_get(filter,"filterunits");int obj=!units||strncmp(units,"userSpaceOnUse",14)!=0;
    if(obj&&have_bbox){float bw=bbox[2]-bbox[0],bh=bbox[3]-bbox[1];region[0]=bbox[0]+svg_num_attr(filter,"x",1,-.1f)*bw;region[1]=bbox[1]+svg_num_attr(filter,"y",1,-.1f)*bh;region[2]=region[0]+svg_num_attr(filter,"width",1,1.2f)*bw;region[3]=region[1]+svg_num_attr(filter,"height",1,1.2f)*bh;}
    else {float bw=have_bbox?bbox[2]-bbox[0]:cx->vw,bh=have_bbox?bbox[3]-bbox[1]:cx->vh;const char* fx=attr_get(filter,"x"),*fy=attr_get(filter,"y"),*fw=attr_get(filter,"width"),*fh=attr_get(filter,"height");region[0]=fx?parse_len(fx,cx->vw,0):(have_bbox?bbox[0]-.1f*bw:0);region[1]=fy?parse_len(fy,cx->vh,0):(have_bbox?bbox[1]-.1f*bh:0);region[2]=region[0]+(fw?parse_len(fw,cx->vw,0):1.2f*bw);region[3]=region[1]+(fh?parse_len(fh,cx->vh,0):1.2f*bh);}
    Mat inv;int invok=mat_inv(ctm,&inv);for(int y=0;y<h;y++)for(int x=0;x<w;x++){size_t i=(size_t)y*w+x;if(invok){float ux,uy;mat_pt(inv,x+.5f,y+.5f,&ux,&uy);if(ux<region[0]||uy<region[1]||ux>=region[2]||uy>=region[3]){layer->px[i]=0;continue;}}layer->px[i]=last[i];}
 done:
    for(int i=0;i<nr;i++)free(res[i].px);
}

static void render_node(Ctx* cx, Canvas* cv, SNode* n, Mat ctm, const Style* parent, int depth, int use_depth) {
    if (depth > SVG_MAX_DEPTH || is_skipped_tag(n->tag)) return;
    Style st; compute_style(cx, n, parent, &st); if (st.none) return;
    int bypass = cx->bypass_effect_node == n;
    if (!bypass && (st.filter_url[0] || st.clip_url[0] || st.mask_url[0])) {
        /* Элемент с эффектами сначала изолируется в прозрачный слой. Это сохраняет backdrop и не затрагивает соседей. */
        size_t count = (size_t)cv->w * (size_t)cv->h;
        if (count > 12000000u) { /* На очень больших изображениях избегаем нескольких сотен мегабайт временных буферов. */
            /* Безопасный fallback: рендерим исходный элемент, не теряя его содержимое. */
        } else {
            uint32_t* layer_px = calloc(count, sizeof(uint32_t));
            if (layer_px) {
                Canvas layer = {cv->w, cv->h, layer_px}; Ctx sub = *cx; sub.bypass_effect_node = n;
                render_node(&sub, &layer, n, ctm, parent, depth, use_depth);
                Mat node_ctm = ctm; const char* nts = attr_get(n, "transform"); if (nts) { Mat nt; if (parse_transform(nts, &nt)) node_ctm = mat_mul(node_ctm, nt); }
                float ub[4]; int have_bbox = canvas_user_bbox(&layer, layer_px, node_ctm, ub);
                if (st.filter_url[0]) { SNode* fn = ctx_by_id(cx, st.filter_url); if (fn && !strcmp(fn->tag,"filter")) svg_filter_apply(cx, fn, &layer, node_ctm, ub, have_bbox); }
                if (st.clip_url[0]) { SNode* cp = ctx_by_id(cx, st.clip_url); if (cp && !strcmp(cp->tag,"clippath")) svg_apply_clip(cx, cp, &layer, node_ctm, &st, ub, have_bbox, depth, use_depth); }
                if (st.mask_url[0]) { SNode* mn = ctx_by_id(cx, st.mask_url); if (mn && !strcmp(mn->tag,"mask")) svg_apply_mask(cx, mn, &layer, node_ctm, &st, ub, have_bbox, depth, use_depth); }
                canvas_blend_over(cv, &layer);
                free(layer_px); return;
            }
        }
    }
    const char* ts = attr_get(n, "transform"); if (ts) { Mat t; if (parse_transform(ts, &t)) ctm = mat_mul(ctm, t); }
    if (!strcmp(n->tag, "text") || !strcmp(n->tag, "tspan")) {
#ifdef SVG_WITH_TTF
        float x = cx->text_active ? cx->text_x : 0.0f;
        float y = cx->text_active ? cx->text_y : 0.0f;
        const char* xs = attr_get(n, "x"), *ys = attr_get(n, "y"), *dxs = attr_get(n, "dx"), *dys = attr_get(n, "dy");
        if (xs) x = parse_len(xs, cx->vw, x); if (ys) y = parse_len(ys, cx->vh, y);
        if (dxs) x += parse_len(dxs, cx->vw, 0); if (dys) y += parse_len(dys, cx->vh, 0);
        char* txt = svg_text_content(n);
        if (txt && *txt) {
            TTFFont* font = font_manager_get_styled(st.font_family, st.font_weight, st.font_italic);
            float width = 0;
            if (font) { int fs=(int)floorf(st.font_size+0.5f); if(fs<1)fs=1;if(fs>512)fs=512; width=ttf_text_width_f(font,txt,fs,(st.font_weight>=600?TTF_BOLD:0)|(st.font_italic?TTF_ITALIC:0)); }
            if (st.text_anchor == 1) x -= width * 0.5f; else if (st.text_anchor == 2) x -= width;
            draw_svg_text(cx, cv, txt, x, y, ctm, &st); x += width;
        }
        free(txt);
        if (n->first) { Ctx tc = *cx; tc.text_active = 1; tc.text_x = x; tc.text_y = y; render_children(&tc, cv, n, ctm, &st, depth, use_depth); }
#else
        /* Standalone svg_cli builds without the browser font manager intentionally keep text inert. */
#endif
        return;
    }
    float tol = 0.2f / mat_scale(ctm);
    const char* tg = n->tag; Path p = {0};
    if (!strcmp(tg, "g") || !strcmp(tg, "a") || !strcmp(tg, "switch")) { render_children(cx, cv, n, ctm, &st, depth, use_depth); return; }
    if (!strcmp(tg, "svg")) {                                                 /* вложенный svg: позиция + viewBox */
        float x = parse_len(attr_get(n, "x"), cx->vw, 0), y = parse_len(attr_get(n, "y"), cx->vh, 0);
        float w = parse_len(attr_get(n, "width"), cx->vw, cx->vw), h = parse_len(attr_get(n, "height"), cx->vh, cx->vh); float vb[4];
        Mat t = mat_id(); t.e = x; t.f = y; Mat m = mat_mul(ctm, t);
        if (parse_viewbox(attr_get(n, "viewbox"), vb)) m = mat_mul(m, vb_matrix(vb[0], vb[1], vb[2], vb[3], attr_get(n, "preserveaspectratio"), w, h));
        render_children(cx, cv, n, m, &st, depth, use_depth); return;
    }
    if (!strcmp(tg, "use")) {
        const char* h = attr_get(n, "href"); if (!h || h[0] != '#' || use_depth >= SVG_MAX_USE_DEPTH) return;
        SNode* tgt = ctx_by_id(cx, h + 1); if (!tgt) return;
        float x = parse_len(attr_get(n, "x"), cx->vw, 0), y = parse_len(attr_get(n, "y"), cx->vh, 0);
        Mat t = mat_id(); t.e = x; t.f = y; Mat m = mat_mul(ctm, t);
        if (!strcmp(tgt->tag, "symbol") || !strcmp(tgt->tag, "svg")) {
            Style ss; compute_style(cx, tgt, &st, &ss); float vb[4];
            const char* uw = attr_get(n, "width"), * uh = attr_get(n, "height");
            if (parse_viewbox(attr_get(tgt, "viewbox"), vb)) {
                float w = uw ? parse_len(uw, cx->vw, cx->vw) : (attr_get(tgt, "width") ? parse_len(attr_get(tgt, "width"), cx->vw, cx->vw) : cx->vw);      /* по умолчанию 100% viewport */
                float hh = uh ? parse_len(uh, cx->vh, cx->vh) : (attr_get(tgt, "height") ? parse_len(attr_get(tgt, "height"), cx->vh, cx->vh) : cx->vh);
                m = mat_mul(m, vb_matrix(vb[0], vb[1], vb[2], vb[3], attr_get(tgt, "preserveaspectratio"), w, hh));
            }
            for (SNode* c = tgt->first; c; c = c->next) render_node(cx, cv, c, m, &ss, depth + 1, use_depth + 1);
        } else {                                                              /* обычный элемент: рисуем сам, стиль наследуется от use */
            if (tgt->parent && !strcmp(tgt->tag, "g")) render_children(cx, cv, tgt, m, &st, depth, use_depth + 1);
            else render_node(cx, cv, tgt, m, &st, depth + 1, use_depth + 1);
        }
        return;
    }
    if (!strcmp(tg, "path")) { const char* d = attr_get(n, "d"); if (d) path_from_d(&p, d, tol); }
    else if (!strcmp(tg, "rect")) {
        float x = parse_len(attr_get(n, "x"), cx->vw, 0), y = parse_len(attr_get(n, "y"), cx->vh, 0), w = parse_len(attr_get(n, "width"), cx->vw, 0), h = parse_len(attr_get(n, "height"), cx->vh, 0);
        if (w <= 0 || h <= 0) return;
        const char* rxs = attr_get(n, "rx"), * rys = attr_get(n, "ry"); float rx = rxs ? parse_len(rxs, cx->vw, 0) : -1, ry = rys ? parse_len(rys, cx->vh, 0) : -1;
        if (rx < 0 && ry < 0) rx = ry = 0; else if (rx < 0) rx = ry; else if (ry < 0) ry = rx;
        if (rx > w / 2) rx = w / 2; if (ry > h / 2) ry = h / 2;
        if (rx <= 0 || ry <= 0) { path_begin(&p, x, y); path_line(&p, x + w, y); path_line(&p, x + w, y + h); path_line(&p, x, y + h); path_close(&p); }
        else {
            path_begin(&p, x + rx, y); path_line(&p, x + w - rx, y); path_arc(&p, x + w - rx, y, rx, ry, 0, 0, 1, x + w, y + ry, tol);
            path_line(&p, x + w, y + h - ry); path_arc(&p, x + w, y + h - ry, rx, ry, 0, 0, 1, x + w - rx, y + h, tol);
            path_line(&p, x + rx, y + h); path_arc(&p, x + rx, y + h, rx, ry, 0, 0, 1, x, y + h - ry, tol);
            path_line(&p, x, y + ry); path_arc(&p, x, y + ry, rx, ry, 0, 0, 1, x + rx, y, tol); path_close(&p);
        }
    }
    else if (!strcmp(tg, "circle")) { float r = parse_len(attr_get(n, "r"), (cx->vw + cx->vh) / 2, 0); if (r <= 0) return; path_ellipse(&p, parse_len(attr_get(n, "cx"), cx->vw, 0), parse_len(attr_get(n, "cy"), cx->vh, 0), r, r, tol); }
    else if (!strcmp(tg, "ellipse")) { float rx = parse_len(attr_get(n, "rx"), cx->vw, 0), ry = parse_len(attr_get(n, "ry"), cx->vh, 0); if (rx <= 0 || ry <= 0) return; path_ellipse(&p, parse_len(attr_get(n, "cx"), cx->vw, 0), parse_len(attr_get(n, "cy"), cx->vh, 0), rx, ry, tol); }
    else if (!strcmp(tg, "line")) { path_begin(&p, parse_len(attr_get(n, "x1"), cx->vw, 0), parse_len(attr_get(n, "y1"), cx->vh, 0)); path_line(&p, parse_len(attr_get(n, "x2"), cx->vw, 0), parse_len(attr_get(n, "y2"), cx->vh, 0)); }
    else if (!strcmp(tg, "polyline") || !strcmp(tg, "polygon")) {
        const char* s = attr_get(n, "points"); float x, y; int first = 1;
        if (s) while (parse_num(&s, &x) && parse_num(&s, &y)) { if (first) { path_begin(&p, x, y); first = 0; } else path_line(&p, x, y); }
        if (tg[4] == 'g') path_close(&p);
    }
    else return;
    draw_path(cx, cv, ctm, &p, &st); path_free(&p);
}

/* ══════════════ публичный API ══════════════ */
static int has_marker(const char* src, size_t len, const char* mk) {            /* служебный комментарий в начале файла */
    size_t ml = strlen(mk), lim = len < 256 ? len : 256;
    for (size_t i = 0; i + ml <= lim; i++) if (!memcmp(src + i, mk, ml)) return 1;
    return 0;
}
static void ext_load(Ctx* cx, const char* src, size_t len) {
    const char* m = NULL; size_t lim = len < 256 ? len : 256;
    for (size_t i = 0; i + 13 < lim; i++) if (!memcmp(src + i, "<!--mlsprite:", 13)) { m = src + i + 13; break; }
    if (!m) return;
    char key[32]; size_t k = 0;
    while (m < src + len && *m != '-' && k < sizeof key - 1) {
        unsigned char ch = (unsigned char)*m;
        if (!(isalnum(ch) || ch == '_')) return;
        key[k++] = *m++;
    }
    key[k] = 0;
    if (!k || m >= src + len || *m != '-') return;
    char prefix[48]; snprintf(prefix, sizeof prefix, "sprite_%s_", key);
    DIR* d = opendir("cache"); if (!d) return;
    char names[16][64]; int nn = 0; struct dirent* de;
    while ((de = readdir(d)) && nn < 16) {
        size_t name_len = strlen(de->d_name);
        if (!strncmp(de->d_name, prefix, strlen(prefix)) && name_len < sizeof names[0]) {
            memcpy(names[nn], de->d_name, name_len + 1); nn++;
        }
    }
    closedir(d);
    /* 16 names of at most 62 bytes plus separators fit into 1025 bytes.
       Check each append nevertheless; never subtract from a capacity unsignedly. */
    char sig[1025]; size_t sig_used = 0; sig[0] = 0;
    for (int i = 0; i < nn; i++) {
        size_t name_len = strlen(names[i]);
        if (sig_used > sizeof sig - 2 || name_len > sizeof sig - sig_used - 2) { cx->ext = NULL; return; }
        memcpy(sig + sig_used, names[i], name_len); sig_used += name_len;
        sig[sig_used++] = ';'; sig[sig_used] = 0;
    }
    if (!strcmp(g_ext.key, key) && !strcmp(g_ext.sig, sig)) { cx->ext = &g_ext; return; }       /* уже загружены */
    for (int i = 0; i < g_ext.n; i++) { Doc* old = g_ext.docs[i]; doc_free(old); free(old); } g_ext.n = 0;
    snprintf(g_ext.key, sizeof g_ext.key, "%s", key); snprintf(g_ext.sig, sizeof g_ext.sig, "%s", sig);
    for (int i = 0; i < nn && g_ext.n < 8; i++) {
        char path[sizeof("cache/") - 1 + sizeof names[i]];
        size_t name_len = strlen(names[i]);
        if (name_len >= sizeof names[i] || name_len + sizeof("cache/") > sizeof path) continue;
        memcpy(path, "cache/", sizeof("cache/") - 1);
        memcpy(path + sizeof("cache/") - 1, names[i], name_len + 1);
        FILE* f = fopen(path, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 4 * 1024 * 1024) {
            char* buf = malloc((size_t)sz + 1);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                Doc* dd = calloc(1, sizeof *dd);
                if (dd && xml_parse(dd, buf, (size_t)sz)) g_ext.docs[g_ext.n++] = dd; else if (dd) { doc_free(dd); free(dd); }
            }
            free(buf);
        }
        fclose(f);
    }
    cx->ext = &g_ext;
}
int svg_looks_like(const char* buf, size_t n) {
    size_t i = 0; if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) i = 3;
    while (i < n && isspace((unsigned char)buf[i])) i++;
    if (i >= n || buf[i] != '<') return 0;
    size_t lim = n < 4096 ? n : 4096; char tmp[4097]; memcpy(tmp, buf, lim); tmp[lim] = 0;
    for (size_t k = 0; k < lim; k++) tmp[k] = (char)tolower((unsigned char)tmp[k]);
    return strstr(tmp, "<svg") != NULL;
}
static int load_doc(Doc* d, Ctx* cx, const char* src, size_t len) {
    memset(d, 0, sizeof *d); memset(cx, 0, sizeof *cx);
    if (len > 8u * 1024 * 1024) return 0;
    if (!xml_parse(d, src, len) || !d->root) { doc_free(d); return 0; }
    cx->doc = d; ext_load(cx, src, len);
    for (int i = 0; i < d->nnodes; i++) if (!strcmp(d->all[i]->tag, "style") && d->all[i]->text) rules_parse(&cx->rules, &d->ar, d->all[i]->text);
    return 1;
}
static void intrinsic_of(SNode* root, float* w, float* h) {
    const char* ws = attr_get(root, "width"), * hs = attr_get(root, "height"); float vb[4]; int hv = parse_viewbox(attr_get(root, "viewbox"), vb);
    int wp = ws && !strchr(ws, '%'), hp = hs && !strchr(hs, '%');
    float W = wp ? parse_len(ws, 0, 0) : 0, H = hp ? parse_len(hs, 0, 0) : 0;
    if (wp && hp && W > 0 && H > 0) { *w = W; *h = H; return; }
    if (hv) {
        if (wp && W > 0) { *w = W; *h = W * vb[3] / vb[2]; return; }
        if (hp && H > 0) { *h = H; *w = H * vb[2] / vb[3]; return; }
        *w = vb[2]; *h = vb[3]; return;
    }
    *w = wp && W > 0 ? W : 300; *h = hp && H > 0 ? H : 150;
}
int svg_intrinsic(const char* src, size_t len, float* w, float* h) {
    Doc d; Ctx cx; if (!load_doc(&d, &cx, src, len)) return 0;
    intrinsic_of(d.root, w, h); free(cx.rules.r); doc_free(&d); return 1;
}
uint32_t* svg_render(const char* src, size_t len, int W, int H, uint32_t cur_color) {
    if (W < 1 || H < 1 || W > 8192 || H > 8192 || (long)W * H > 40000000L) return NULL;
    Doc d; Ctx cx; if (!load_doc(&d, &cx, src, len)) return NULL;
    uint32_t* px = calloc((size_t)W * (size_t)H, 4); if (!px) { free(cx.rules.r); doc_free(&d); return NULL; }
    Canvas cv = {W, H, px}; cx.cur = cur_color;
    float iw, ih; intrinsic_of(d.root, &iw, &ih); float vb[4]; Mat m;
    const char* par = attr_get(d.root, "preserveaspectratio");
    if (parse_viewbox(attr_get(d.root, "viewbox"), vb)) { m = vb_matrix(vb[0], vb[1], vb[2], vb[3], par, (float)W, (float)H); cx.vw = vb[2]; cx.vh = vb[3]; }
    else if (has_marker(src, len, "<!--mlinline-->")) { m = mat_id(); cx.vw = (float)W; cx.vh = (float)H; }      /* встроенный svg без viewBox: пользовательские единицы = CSS-пиксели, масштабирования нет */
    else { m = vb_matrix(0, 0, iw, ih, par, (float)W, (float)H); cx.vw = iw; cx.vh = ih; }
    Style base; style_default(&base, cur_color); Style rs; compute_style(&cx, d.root, &base, &rs);
    if (!rs.none) render_children(&cx, &cv, d.root, m, &rs, 0, 0);
    free(cx.rules.r); doc_free(&d); return px;
}
