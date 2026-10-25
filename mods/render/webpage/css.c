/* CSS: разбор значений, селекторов и таблиц стилей. */
#include "css_int.h"
#include "font_manager.h"
#include "nal.h"
#include "gal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <strings.h>

int css_fetch_budget = 0;

/* ══════════════ арена ══════════════ */
void* aalloc(Arena* a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    ArenaChunk* c = a->head;
    if (!c || c->cap - c->used < n) {
        size_t cap = n > 65536 ? n : 65536;
        c = malloc(sizeof(ArenaChunk) + cap);
        if (!c) return NULL;
        c->next = a->head; c->used = 0; c->cap = cap; a->head = c;
    }
    void* p = c->data + c->used; c->used += n; memset(p, 0, n);
    return p;
}
char* astrdup(Arena* a, const char* s, size_t n) {
    char* d = aalloc(a, n + 1); if (!d) return NULL;
    memcpy(d, s, n); d[n] = 0; return d;
}
void afree(Arena* a) { while (a->head) { ArenaChunk* n = a->head->next; free(a->head); a->head = n; } }

/* ══════════════ свойства ══════════════ */
static const char* PNAMES[] = { "", 
#define X(id, name) name,
    PROP_LIST(X)
#undef X
};
int css_prop_id(const char* name, size_t n) {
    if (n >= 2 && name[0] == '-' && name[1] == '-') return P_CUSTOM;
    if (n > 8 && name[0] == '-') {                         /* -webkit-/-moz-/-ms-/-o- */
        const char* d = memchr(name + 1, '-', n - 1);
        if (d) { n -= (size_t)(d + 1 - name); name = d + 1; }
    }
    for (int i = 1; i < P_CUSTOM; i++) if (strlen(PNAMES[i]) == n && strncasecmp(PNAMES[i], name, n) == 0) return i;
    if (n == 12 && strncasecmp(name, "grid-row-gap", 12) == 0) return P_ROW_GAP;
    if (n == 15 && strncasecmp(name, "grid-column-gap", 15) == 0) return P_COL_GAP;
    if (n == 8 && strncasecmp(name, "grid-gap", 8) == 0) return P_GAP;
    return P_NONE;
}

/* ══════════════ цвета ══════════════ */
static const char* NAMED =
 "aliceblue:f0f8ff antiquewhite:faebd7 aqua:00ffff aquamarine:7fffd4 azure:f0ffff beige:f5f5dc bisque:ffe4c4 black:000000 "
 "blanchedalmond:ffebcd blue:0000ff blueviolet:8a2be2 brown:a52a2a burlywood:deb887 cadetblue:5f9ea0 chartreuse:7fff00 "
 "chocolate:d2691e coral:ff7f50 cornflowerblue:6495ed cornsilk:fff8dc crimson:dc143c cyan:00ffff darkblue:00008b "
 "darkcyan:008b8b darkgoldenrod:b8860b darkgray:a9a9a9 darkgreen:006400 darkgrey:a9a9a9 darkkhaki:bdb76b darkmagenta:8b008b "
 "darkolivegreen:556b2f darkorange:ff8c00 darkorchid:9932cc darkred:8b0000 darksalmon:e9967a darkseagreen:8fbc8f "
 "darkslateblue:483d8b darkslategray:2f4f4f darkslategrey:2f4f4f darkturquoise:00ced1 darkviolet:9400d3 deeppink:ff1493 "
 "deepskyblue:00bfff dimgray:696969 dimgrey:696969 dodgerblue:1e90ff firebrick:b22222 floralwhite:fffaf0 forestgreen:228b22 "
 "fuchsia:ff00ff gainsboro:dcdcdc ghostwhite:f8f8ff gold:ffd700 goldenrod:daa520 gray:808080 green:008000 greenyellow:adff2f "
 "grey:808080 honeydew:f0fff0 hotpink:ff69b4 indianred:cd5c5c indigo:4b0082 ivory:fffff0 khaki:f0e68c lavender:e6e6fa "
 "lavenderblush:fff0f5 lawngreen:7cfc00 lemonchiffon:fffacd lightblue:add8e6 lightcoral:f08080 lightcyan:e0ffff "
 "lightgoldenrodyellow:fafad2 lightgray:d3d3d3 lightgreen:90ee90 lightgrey:d3d3d3 lightpink:ffb6c1 lightsalmon:ffa07a "
 "lightseagreen:20b2aa lightskyblue:87cefa lightslategray:778899 lightslategrey:778899 lightsteelblue:b0c4de lightyellow:ffffe0 "
 "lime:00ff00 limegreen:32cd32 linen:faf0e6 magenta:ff00ff maroon:800000 mediumaquamarine:66cdaa mediumblue:0000cd "
 "mediumorchid:ba55d3 mediumpurple:9370db mediumseagreen:3cb371 mediumslateblue:7b68ee mediumspringgreen:00fa9a "
 "mediumturquoise:48d1cc mediumvioletred:c71585 midnightblue:191970 mintcream:f5fffa mistyrose:ffe4e1 moccasin:ffe4b5 "
 "navajowhite:ffdead navy:000080 oldlace:fdf5e6 olive:808000 olivedrab:6b8e23 orange:ffa500 orangered:ff4500 orchid:da70d6 "
 "palegoldenrod:eee8aa palegreen:98fb98 paleturquoise:afeeee palevioletred:db7093 papayawhip:ffefd5 peachpuff:ffdab9 "
 "peru:cd853f pink:ffc0cb plum:dda0dd powderblue:b0e0e6 purple:800080 rebeccapurple:663399 red:ff0000 rosybrown:bc8f8f "
 "royalblue:4169e1 saddlebrown:8b4513 salmon:fa8072 sandybrown:f4a460 seagreen:2e8b57 seashell:fff5ee sienna:a0522d "
 "silver:c0c0c0 skyblue:87ceeb slateblue:6a5acd slategray:708090 slategrey:708090 snow:fffafa springgreen:00ff7f "
 "steelblue:4682b4 tan:d2b48c teal:008080 thistle:d8bfd8 tomato:ff6347 turquoise:40e0d0 violet:ee82ee wheat:f5deb3 "
 "white:ffffff whitesmoke:f5f5f5 yellow:ffff00 yellowgreen:9acd32 "
 "canvas:ffffff canvastext:000000 buttonface:efefef buttontext:000000 field:ffffff fieldtext:000000 linktext:0000ee "
 "graytext:6d6d6d highlight:3390ff highlighttext:ffffff ";

static uint32_t hsl2rgb(float h, float s, float l) {
    h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
    float c = (1 - fabsf(2 * l - 1)) * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = l - c / 2, r, g, b;
    if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; } else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; } else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
    int R = (int)((r + m) * 255 + .5f), G = (int)((g + m) * 255 + .5f), B = (int)((b + m) * 255 + .5f);
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

static int clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + .5f); }

int css_parse_color(const char* s, uint32_t* out, uint32_t cur) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n - 1])) n--;
    if (!n) return 0;
    if (s[0] == '#') {
        char h[9]; size_t l = n - 1; if (l != 3 && l != 4 && l != 6 && l != 8) return 0;
        for (size_t i = 0; i < l; i++) if (!isxdigit((unsigned char)s[1 + i])) return 0;
        if (l <= 4) { for (size_t i = 0; i < l; i++) { h[2 * i] = s[1 + i]; h[2 * i + 1] = s[1 + i]; } l *= 2; } else memcpy(h, s + 1, l);
        h[l] = 0; uint32_t v = (uint32_t)strtoul(h, NULL, 16);
        *out = l == 8 ? ((v & 0xFF) << 24) | (v >> 8) : 0xFF000000u | v;
        return 1;
    }
    char low[32];
    if (n < 31 && isalpha((unsigned char)s[0]) && !strchr(s, '(')) {
        for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)s[i]); low[n] = 0;
        if (!strcmp(low, "transparent")) { *out = 0; return 1; }
        if (!strcmp(low, "currentcolor")) { *out = cur; return 1; }
        size_t ln = n; const char* p = NAMED;
        while (*p) {
            const char* c = strchr(p, ':'); if (!c) break;
            if ((size_t)(c - p) == ln && strncmp(p, low, ln) == 0) { *out = 0xFF000000u | (uint32_t)strtoul(c + 1, NULL, 16); return 1; }
            p = strchr(c, ' '); if (!p) break; p++;
        }
        return 0;
    }
    int fn = 0;
    if (!strncasecmp(s, "rgba(", 5)) fn = 5; else if (!strncasecmp(s, "rgb(", 4)) fn = 4;
    else if (!strncasecmp(s, "hsla(", 5)) fn = -5; else if (!strncasecmp(s, "hsl(", 4)) fn = -4;
    else if (!strncasecmp(s, "light-dark(", 11)) {                      /* берём светлую тему */
        char tmp[96]; const char* a = s + 11; int d = 0; size_t k = 0;
        while (*a && k < 95 && !(d == 0 && *a == ',')) { if (*a == '(') d++; if (*a == ')') d--; tmp[k++] = *a++; }
        tmp[k] = 0; return css_parse_color(tmp, out, cur);
    }
    if (!fn) return 0;
    int hsl = fn < 0; const char* p = s + (fn < 0 ? -fn : fn);
    float v[4] = {0, 0, 0, 1}; int pct[4] = {0}, cnt = 0;
    while (*p && *p != ')' && cnt < 4) {
        while (isspace((unsigned char)*p) || *p == ',' || *p == '/') p++;
        if (*p == ')' || !*p) break;
        char* e; v[cnt] = strtof(p, &e); if (e == p) { if (!strncasecmp(p, "none", 4)) { e = (char*)p + 4; } else return 0; }
        if (*e == '%') { pct[cnt] = 1; e++; }
        else if (!strncasecmp(e, "deg", 3)) e += 3; else if (!strncasecmp(e, "turn", 4)) { v[cnt] *= 360; e += 4; }
        p = e; cnt++;
    }
    if (cnt < 3) return 0;
    float a = cnt > 3 ? (pct[3] ? v[3] / 100.0f : v[3]) : 1.0f; if (a < 0) a = 0; if (a > 1) a = 1;
    uint32_t rgb;
    if (hsl) rgb = hsl2rgb(v[0], v[1] / 100.0f, v[2] / 100.0f);
    else rgb = ((uint32_t)clamp255(pct[0] ? v[0] * 2.55f : v[0]) << 16) | ((uint32_t)clamp255(pct[1] ? v[1] * 2.55f : v[1]) << 8) | (uint32_t)clamp255(pct[2] ? v[2] * 2.55f : v[2]);
    *out = ((uint32_t)(a * 255 + .5f) << 24) | rgb;
    return 1;
}

/* ══════════════ длины и calc() ══════════════ */
typedef struct { Len v; int scalar; float num; } CVal;
typedef struct { float em, rem; } LCtx;

static int calc_expr(const char** pp, CVal* out, LCtx* lc, int depth);

static int pure_px(const CVal* c) { return c->v.pct == 0 && c->v.vw == 0 && c->v.vh == 0; }

static int calc_factor(const char** pp, CVal* out, LCtx* lc, int depth) {
    const char* p = *pp; while (isspace((unsigned char)*p)) p++;
    memset(out, 0, sizeof *out);
    if (depth > 8) return 0;
    if (!strncasecmp(p, "calc(", 5) || *p == '(' || !strncasecmp(p, "min(", 4) || !strncasecmp(p, "max(", 4) || !strncasecmp(p, "clamp(", 6)) {
        int kind = 0;                                   /* 0 calc/(), 1 min, 2 max, 3 clamp */
        if (*p == '(') p++; else if (!strncasecmp(p, "calc(", 5)) p += 5;
        else if (!strncasecmp(p, "min(", 4)) { kind = 1; p += 4; } else if (!strncasecmp(p, "max(", 4)) { kind = 2; p += 4; } else { kind = 3; p += 6; }
        CVal args[3]; int na = 0;
        for (;;) {
            if (na >= 3 || !calc_expr(&p, &args[na], lc, depth + 1)) return 0;
            na++; while (isspace((unsigned char)*p)) p++;
            if (*p == ',') { p++; continue; }
            break;
        }
        if (*p != ')') return 0;
        p++; *pp = p;
        if (kind == 0) { *out = args[0]; return 1; }
        int cmp = 1; for (int i = 0; i < na; i++) if (!pure_px(&args[i])) cmp = 0;
        if (kind == 3 && na == 3) {
            if (cmp) { float v = args[1].v.px; if (v < args[0].v.px) v = args[0].v.px; if (v > args[2].v.px) v = args[2].v.px; *out = args[1]; out->v.px = v; }
            else *out = args[1];
            return 1;
        }
        *out = args[0];
        if (cmp) for (int i = 1; i < na; i++) if (kind == 1 ? args[i].v.px < out->v.px : args[i].v.px > out->v.px) *out = args[i];
        return 1;
    }
    char* e; float num = strtof(p, &e);
    if (e == p) return 0;
    p = e; out->v.set = 1;
    if (*p == '%') { out->v.pct = num; p++; }
    else if (!strncasecmp(p, "px", 2)) { out->v.px = num; p += 2; }
    else if (!strncasecmp(p, "rem", 3)) { out->v.px = num * lc->rem; p += 3; }
    else if (!strncasecmp(p, "em", 2)) { out->v.px = num * lc->em; p += 2; }
    else if (!strncasecmp(p, "vmin", 4)) { out->v.vh = num / 100.0f; p += 4; }
    else if (!strncasecmp(p, "vmax", 4)) { out->v.vw = num / 100.0f; p += 4; }
    else if (!strncasecmp(p, "vw", 2)) { out->v.vw = num / 100.0f; p += 2; }
    else if (!strncasecmp(p, "vh", 2)) { out->v.vh = num / 100.0f; p += 2; }
    else if (!strncasecmp(p, "pt", 2)) { out->v.px = num * 4.0f / 3.0f; p += 2; }
    else if (!strncasecmp(p, "pc", 2)) { out->v.px = num * 16; p += 2; }
    else if (!strncasecmp(p, "cm", 2)) { out->v.px = num * 37.795f; p += 2; }
    else if (!strncasecmp(p, "mm", 2)) { out->v.px = num * 3.7795f; p += 2; }
    else if (!strncasecmp(p, "in", 2)) { out->v.px = num * 96; p += 2; }
    else if (!strncasecmp(p, "ch", 2) || !strncasecmp(p, "ex", 2)) { out->v.px = num * lc->em * 0.5f; p += 2; }
    else if (isalpha((unsigned char)*p)) return 0;                          /* неизвестная единица */
    else { out->scalar = 1; out->num = num; out->v.px = num; }
    *pp = p; return 1;
}

static int calc_term(const char** pp, CVal* out, LCtx* lc, int depth) {
    if (!calc_factor(pp, out, lc, depth)) return 0;
    for (;;) {
        const char* p = *pp; while (isspace((unsigned char)*p)) p++;
        if (*p != '*' && *p != '/') return 1;
        char op = *p++; *pp = p; CVal b;
        if (!calc_factor(pp, &b, lc, depth)) return 0;
        if (op == '*') {
            CVal* len = out->scalar ? &b : out; float k = out->scalar ? out->num : (b.scalar ? b.num : 1);
            CVal r = *len; r.v.px *= k; r.v.pct *= k; r.v.vw *= k; r.v.vh *= k; if (out->scalar && b.scalar) { r.scalar = 1; r.num = out->num * b.num; } else r.scalar = 0; *out = r;
        } else if (b.scalar && b.num != 0) { out->v.px /= b.num; out->v.pct /= b.num; out->v.vw /= b.num; out->v.vh /= b.num; out->num /= b.num; }
        else return 0;
    }
}

static int calc_expr(const char** pp, CVal* out, LCtx* lc, int depth) {
    if (!calc_term(pp, out, lc, depth)) return 0;
    for (;;) {
        const char* p = *pp; while (isspace((unsigned char)*p)) p++;
        if (*p != '+' && *p != '-') return 1;
        if (!isspace((unsigned char)p[-1]) && p != *pp) return 1;
        char op = *p++; *pp = p; CVal b;
        if (!calc_term(pp, &b, lc, depth)) return 0;
        float k = op == '+' ? 1.0f : -1.0f;
        out->v.px += k * b.v.px; out->v.pct += k * b.v.pct; out->v.vw += k * b.v.vw; out->v.vh += k * b.v.vh;
        out->scalar = out->scalar && b.scalar; out->num += k * b.num;
    }
}

int css_parse_len(const char* s, Len* out, float em, float rem) {
    while (isspace((unsigned char)*s)) s++;
    memset(out, 0, sizeof *out);
    if (!*s || !strncasecmp(s, "auto", 4) || !strncasecmp(s, "none", 4) || !strncasecmp(s, "normal", 6)) return 1;   /* set = 0 */
    LCtx lc = {em, rem}; CVal c; const char* p = s;
    if (!calc_expr(&p, &c, &lc, 0)) return 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p && *p != '!') return 0;
    *out = c.v; out->set = 1;
    return 1;
}

/* ══════════════ media queries ══════════════ */
extern int SCREEN_WIDTH, SCREEN_HEIGHT;

static float mq_num(const char* v) {
    char* e; float n = strtof(v, &e);
    if (!strncasecmp(e, "em", 2) || !strncasecmp(e, "rem", 3)) n *= 16;
    return n;
}
static int mq_feature(const char* nm, const char* v) {
    float w = (float)SCREEN_WIDTH, h = (float)SCREEN_HEIGHT;
    if (!strcmp(nm, "min-width")) return v && w >= mq_num(v);
    if (!strcmp(nm, "max-width")) return v && w <= mq_num(v);
    if (!strcmp(nm, "width")) return v && (int)w == (int)mq_num(v);
    if (!strcmp(nm, "min-height")) return v && h >= mq_num(v);
    if (!strcmp(nm, "max-height")) return v && h <= mq_num(v);
    if (!strcmp(nm, "height")) return v && (int)h == (int)mq_num(v);
    if (!strcmp(nm, "orientation")) return v && (!strcmp(v, "landscape") == (w >= h));
    if (!strcmp(nm, "prefers-color-scheme")) return v && !strcmp(v, "light");
    if (!strcmp(nm, "prefers-reduced-motion")) return v && !strcmp(v, "no-preference");
    if (!strcmp(nm, "hover") || !strcmp(nm, "any-hover")) return v && !strcmp(v, "hover");
    if (!strcmp(nm, "pointer") || !strcmp(nm, "any-pointer")) return v && !strcmp(v, "fine");
    if (!strcmp(nm, "min-resolution") || !strcmp(nm, "-webkit-min-device-pixel-ratio")) return v && mq_num(v) <= 1.0f;
    if (!strcmp(nm, "max-resolution") || !strcmp(nm, "-webkit-max-device-pixel-ratio")) return v && mq_num(v) >= 1.0f;
    if (!strcmp(nm, "color") || !strcmp(nm, "scripting")) return 1;
    if (!strcmp(nm, "display-mode")) return v && !strcmp(v, "browser");
    return 0;
}

static int mq_one(const char* q, size_t n) {
    char b[256]; if (n > 255) n = 255;
    for (size_t i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)q[i]); b[n] = 0;
    const char* p = b; int neg = 0, res = 1;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '(') {
            const char* e = strchr(p, ')'); if (!e) return 0;
            char in[128]; size_t l = (size_t)(e - p - 1); if (l > 127) l = 127; memcpy(in, p + 1, l); in[l] = 0;
            char* c = strchr(in, ':'); char nm[48]; const char* val = NULL; int ok;
            if (c) { *c = 0; val = c + 1; while (isspace((unsigned char)*val)) val++; char* ve = (char*)val + strlen(val); while (ve > val && isspace((unsigned char)ve[-1])) *--ve = 0; }
            { char* ne = in + strlen(in); while (ne > in && isspace((unsigned char)ne[-1])) *--ne = 0; char* ns = in; while (isspace((unsigned char)*ns)) ns++; snprintf(nm, sizeof nm, "%s", ns); }
            if (!c && (strstr(nm, ">=") || strstr(nm, "<="))) {                       /* (width >= 600px) */
                char f[32], op[3], v2[32];
                if (sscanf(nm, "%31s %2s %31s", f, op, v2) == 3) ok = op[0] == '>' ? mq_feature(f[0] == 'w' ? "min-width" : "min-height", v2) : mq_feature(f[0] == 'w' ? "max-width" : "max-height", v2);
                else ok = 0;
            } else ok = mq_feature(nm, val);
            res = res && ok; p = e + 1; continue;
        }
        const char* e = p; while (*e && !isspace((unsigned char)*e) && *e != '(') e++;
        size_t l = (size_t)(e - p);
        if (l == 3 && !strncmp(p, "not", 3)) neg = 1;
        else if ((l == 4 && !strncmp(p, "only", 4)) || (l == 3 && !strncmp(p, "and", 3)) ||
                 (l == 6 && !strncmp(p, "screen", 6)) || (l == 3 && !strncmp(p, "all", 3))) {}
        else res = 0;                                     /* print, speech, tty ... */
        p = e;
    }
    return neg ? !res : res;
}

int css_media_matches(const char* q) {
    if (!q) return 1;
    while (isspace((unsigned char)*q)) q++;
    if (!*q) return 1;
    const char* s = q; int d = 0;
    for (const char* p = q;; p++) {
        if (*p == '(') d++; else if (*p == ')') d--;
        if ((*p == ',' && d == 0) || !*p) { if (mq_one(s, (size_t)(p - s))) return 1; if (!*p) break; s = p + 1; }
    }
    return 0;
}

/* ══════════════ объявления ══════════════ */
static const char* skip_str(const char* p) {
    char q = *p++;
    while (*p && *p != q) { if (*p == '\\' && p[1]) p++; p++; }
    return *p ? p + 1 : p;
}

Decl* css_parse_decls(Arena* a, const char* text, size_t len, int* out_n) {
    Decl* v = NULL; int n = 0, cap = 0;
    const char* p = text; const char* end = text + len;
    while (p < end) {
        const char* seg = p; int d = 0; const char* colon = NULL; int nested = 0;
        while (p < end) {
            if (*p == '"' || *p == '\'') { p = skip_str(p); continue; }
            if (*p == '/' && p + 1 < end && p[1] == '*') { const char* e = strstr(p + 2, "*/"); p = e ? e + 2 : end; continue; }
            if (*p == '(' || *p == '[') d++; else if (*p == ')' || *p == ']') d--;
            else if (*p == '{') { int bd = 1; p++; while (p < end && bd) { if (*p == '{') bd++; else if (*p == '}') bd--; p++; } nested = 1; break; }
            else if (*p == ':' && d == 0 && !colon) colon = p;
            else if (*p == ';' && d <= 0) break;
            p++;
        }
        const char* segend = nested ? p : p;
        if (p < end && *p == ';') p++;
        if (nested || !colon) continue;
        const char* ns = seg; while (ns < colon && isspace((unsigned char)*ns)) ns++;
        const char* ne = colon; while (ne > ns && isspace((unsigned char)ne[-1])) ne--;
        const char* vs = colon + 1; while (vs < segend && isspace((unsigned char)*vs)) vs++;
        const char* ve = segend; while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
        int imp = 0;
        if (ve - vs >= 10) {
            const char* t = ve - 9; if (!strncasecmp(t, "important", 9)) { const char* b = t; while (b > vs && isspace((unsigned char)b[-1])) b--; if (b > vs && b[-1] == '!') { imp = 1; ve = b - 1; while (ve > vs && isspace((unsigned char)ve[-1])) ve--; } }
        }
        if (ne <= ns || ve <= vs) continue;
        /* IE-хаки Bootstrap 2 и подобных: «width: auto\9», «color: red\0/», «prop: val !ie» — современные браузеры отбрасывают такое объявление целиком */
        if (ve - vs >= 2 && ve[-2] == '\\' && isdigit((unsigned char)ve[-1])) continue;
        if (ve - vs >= 3 && ve[-3] == '\\' && isdigit((unsigned char)ve[-2]) && ve[-1] == '/') continue;
        if (ve - vs >= 3 && !strncasecmp(ve - 3, "!ie", 3)) continue;
        int id = css_prop_id(ns, (size_t)(ne - ns));
        if (id == P_NONE) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, (size_t)cap * sizeof(Decl)); if (!v) return NULL; }
        v[n].prop = (short)id; v[n].imp = (uint8_t)imp;
        v[n].name = id == P_CUSTOM ? astrdup(a, ns, (size_t)(ne - ns)) : NULL;
        v[n].val = astrdup(a, vs, (size_t)(ve - vs));
        n++;
    }
    Decl* out = NULL;
    if (n) { out = aalloc(a, (size_t)n * sizeof(Decl)); if (out) memcpy(out, v, (size_t)n * sizeof(Decl)); }
    free(v);
    *out_n = out ? n : 0;
    return out;
}

/* ══════════════ селекторы ══════════════ */
static int is_ident(int c) { return isalnum(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\'; }

static size_t read_ident(const char* p, char* dst, size_t cap, int lower) {
    size_t k = 0, i = 0;
    while (p[i] && is_ident((unsigned char)p[i])) {
        if (p[i] == '\\') { i++; if (!p[i]) break; }
        if (k + 1 < cap) dst[k++] = lower ? (char)tolower((unsigned char)p[i]) : p[i];
        i++;
    }
    dst[k] = 0; return i;
}

int css_dyn_seen = 0;                             /* 1 — в разобранных таблицах встретились :hover/:active/:focus */
static int g_allow_pseudo = 0;                    /* 1 только пока разбираем селекторы правил таблицы стилей */
static Simple* parse_compound_list(Arena* a, const char* s, size_t n, uint32_t* spec);
static int parse_nth(const char* s, int* A, int* B) {
    while (isspace((unsigned char)*s)) s++;
    if (!strncasecmp(s, "odd", 3)) { *A = 2; *B = 1; return 1; }
    if (!strncasecmp(s, "even", 4)) { *A = 2; *B = 0; return 1; }
    const char* np = strchr(s, 'n');
    if (!np) { *A = 0; *B = atoi(s); return 1; }
    char* e; float av;
    if (np == s || (np == s + 1 && (*s == '-' || *s == '+'))) av = (*s == '-') ? -1 : 1; else av = strtof(s, &e);
    *A = (int)av; np++; while (isspace((unsigned char)*np)) np++;
    *B = (*np == '+' || *np == '-') ? atoi(np) : 0;
    if (*np == '+' || *np == '-') { const char* q = np + 1; while (isspace((unsigned char)*q)) q++; *B = (np[0] == '-' ? -1 : 1) * atoi(q); }
    return 1;
}

static int parse_compound(Arena* a, const char** pp, Simple* s, uint32_t* spec) {
    const char* p = *pp; int any = 0;
    while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '+' && *p != '~' && *p != ',') {
        if (*p == '*') { p++; any = 1; }
        else if (is_ident((unsigned char)*p)) { p += read_ident(p, s->tag, sizeof s->tag, 1); spec[2]++; any = 1; }
        else if (*p == '#') { p++; p += read_ident(p, s->id, sizeof s->id, 0); spec[0]++; any = 1; }
        else if (*p == '.') { p++; char c[40]; p += read_ident(p, c, sizeof c, 0); if (s->ncls < 4) strcpy(s->cls[s->ncls++], c); spec[1]++; any = 1; }
        else if (*p == '[') {
            const char* e = p + 1; int d = 1; while (*e && d) { if (*e == '"' || *e == '\'') { e = skip_str(e); continue; } if (*e == '[') d++; else if (*e == ']') d--; e++; }
            const char* in = p + 1; size_t l = (size_t)(e - 1 - in);
            if (s->nat < 3) {
                size_t k = 0; while (k < l && !strchr("=~|^$*] ", in[k])) k++;
                size_t nl = k < 23 ? k : 23; memcpy(s->at[s->nat].n, in, nl); s->at[s->nat].n[nl] = 0;
                for (char* c = s->at[s->nat].n; *c; c++) *c = (char)tolower((unsigned char)*c);
                while (k < l && in[k] == ' ') k++;
                if (k < l) {
                    char op = in[k]; if (op != '=') k++;
                    if (k < l && in[k] == '=') k++;
                    while (k < l && in[k] == ' ') k++;
                    const char* vs = in + k; size_t vl = l - k;
                    if (vl && (*vs == '"' || *vs == '\'')) { vs++; vl--; const char* q = memchr(vs, in[k], vl); if (q) vl = (size_t)(q - vs); }
                    else { size_t z = 0; while (z < vl && !isspace((unsigned char)vs[z])) z++; vl = z; }
                    if (vl > 47) vl = 47; memcpy(s->at[s->nat].v, vs, vl); s->at[s->nat].v[vl] = 0; s->at[s->nat].op = op;
                }
                s->nat++;
            }
            spec[1]++; p = e; any = 1;
        }
        else if (*p == ':') {
            {   /* ::before / ::after (и устаревшая запись :before / :after) — только в правилах таблиц стилей */
                int dbl = p[1] == ':'; const char* q0 = p + (dbl ? 2 : 1); char pn[32]; size_t pl = read_ident(q0, pn, sizeof pn, 1);
                if (dbl || !strcmp(pn, "before") || !strcmp(pn, "after") || !strcmp(pn, "first-line") || !strcmp(pn, "first-letter")) {
                    if (!g_allow_pseudo) return 0;
                    if (!strcmp(pn, "before")) s->pseudo = 1; else if (!strcmp(pn, "after")) s->pseudo = 2; else return 0;
                    p = q0 + pl; spec[2]++; any = 1; continue;
                }
            }
            p++; char nm[32]; p += read_ident(p, nm, sizeof nm, 1);
            char arg[128] = ""; 
            if (*p == '(') { const char* e = p + 1; int d = 1; while (*e && d) { if (*e == '(') d++; else if (*e == ')') d--; e++; } size_t l = (size_t)(e - 1 - (p + 1)); if (l > 127) l = 127; memcpy(arg, p + 1, l); arg[l] = 0; p = e; }
            int k = 0, A = 0, B = 0;
            if (!strcmp(nm, "first-child")) k = PC_FIRST_CHILD; else if (!strcmp(nm, "last-child")) k = PC_LAST_CHILD; else if (!strcmp(nm, "only-child")) k = PC_ONLY_CHILD;
            else if (!strcmp(nm, "first-of-type")) k = PC_FIRST_TYPE; else if (!strcmp(nm, "last-of-type")) k = PC_LAST_TYPE; else if (!strcmp(nm, "only-of-type")) k = PC_ONLY_TYPE;
            else if (!strcmp(nm, "nth-child")) { k = PC_NTH_CHILD; parse_nth(arg, &A, &B); } else if (!strcmp(nm, "nth-last-child")) { k = PC_NTH_LAST_CHILD; parse_nth(arg, &A, &B); }
            else if (!strcmp(nm, "nth-of-type")) { k = PC_NTH_TYPE; parse_nth(arg, &A, &B); }
            else if (!strcmp(nm, "root")) k = PC_ROOT; else if (!strcmp(nm, "empty")) k = PC_EMPTY;
            else if (!strcmp(nm, "link") || !strcmp(nm, "any-link")) k = PC_LINK; else if (!strcmp(nm, "checked")) k = PC_CHECKED;
            else if (!strcmp(nm, "disabled")) k = PC_DISABLED; else if (!strcmp(nm, "enabled")) k = PC_ENABLED;
            else if (!strcmp(nm, "hover")) { k = PC_HOVER; css_dyn_seen = 1; } else if (!strcmp(nm, "active")) { k = PC_ACTIVE; css_dyn_seen = 1; }
            else if (!strcmp(nm, "focus") || !strcmp(nm, "focus-visible")) { k = PC_FOCUS; css_dyn_seen = 1; } else if (!strcmp(nm, "focus-within")) { k = PC_FOCUS + 100; css_dyn_seen = 1; }
            else if (!strcmp(nm, "visited") || !strcmp(nm, "target") || !strcmp(nm, "placeholder-shown")) k = PC_NEVER;
            else if (!strcmp(nm, "not") || !strcmp(nm, "is") || !strcmp(nm, "where") || !strcmp(nm, "matches") || !strcmp(nm, "any")) {
                if (s->sub) return 0;
                uint32_t sp[3] = {0, 0, 0}; int sv = g_allow_pseudo; g_allow_pseudo = 0;
                s->sub = parse_compound_list(a, arg, strlen(arg), sp); g_allow_pseudo = sv; if (!s->sub) return 0;
                s->sub_neg = !strcmp(nm, "not");
                if (strcmp(nm, "where")) { spec[0] += sp[0]; spec[1] += sp[1]; spec[2] += sp[2]; }
                any = 1; continue;
            }
            else if (!strcmp(nm, "defined") || !strcmp(nm, "lang") || !strcmp(nm, "dir")) { any = 1; continue; }
            else return 0;                                                       /* :has(), :host и пр. */
            if (s->npc < 6) { s->pc[s->npc] = (uint8_t)k; s->pa[s->npc] = (int16_t)A; s->pb[s->npc] = (int16_t)B; s->npc++; }
            spec[1]++; any = 1;
        }
        else return 0;
    }
    *pp = p;
    return any;
}

/* Список простых селекторов через запятую (для :not(...)/:is(...)) */
static Simple* parse_compound_list(Arena* a, const char* s, size_t n, uint32_t* spec) {
    Simple* head = NULL; Simple** tail = &head;
    char* buf = astrdup(a, s, n); const char* p = buf; uint32_t first_spec[3] = {0, 0, 0}; int firstdone = 0;
    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (!*p) break;
        Simple* c = aalloc(a, sizeof(Simple)); uint32_t sp[3] = {0, 0, 0};
        if (!parse_compound(a, &p, c, sp)) return NULL;
        if (*p && !isspace((unsigned char)*p) && *p != ',') return NULL;
        while (isspace((unsigned char)*p)) p++;
        if (*p && *p != ',') return NULL;                                        /* комбинаторы внутри :not() не поддерживаем */
        if (!firstdone) { memcpy(first_spec, sp, sizeof sp); firstdone = 1; }
        *tail = c; tail = &c->alt;
    }
    for (int i = 0; i < 3; i++) spec[i] += first_spec[i];
    return head;
}

/* Разбирает один сложный селектор в массив Simple (слева направо). */
static Simple* parse_selector(Arena* a, const char* s, size_t n, int* nsel, uint32_t* spec_out) {
    char* buf = astrdup(a, s, n); const char* p = buf;
    Simple tmp[8]; int cnt = 0; uint32_t spec[3] = {0, 0, 0}; char comb = 0;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '>' || *p == '+' || *p == '~') { comb = *p++; while (isspace((unsigned char)*p)) p++; }
        if (cnt >= 8) return NULL;
        memset(&tmp[cnt], 0, sizeof(Simple));
        if (!parse_compound(a, &p, &tmp[cnt], spec)) return NULL;
        tmp[cnt].comb = cnt == 0 ? 0 : (comb ? comb : ' ');
        comb = 0; cnt++;
        if (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '+' && *p != '~') return NULL;
    }
    if (!cnt) return NULL;
    for (int i = 0; i + 1 < cnt; i++) if (tmp[i].pseudo) return NULL;          /* псевдоэлемент допустим только в конце селектора */
    Simple* out = aalloc(a, (size_t)cnt * sizeof(Simple)); memcpy(out, tmp, (size_t)cnt * sizeof(Simple));
    *nsel = cnt;
    *spec_out = ((spec[0] > 255 ? 255 : spec[0]) << 16) | ((spec[1] > 255 ? 255 : spec[1]) << 8) | (spec[2] > 255 ? 255 : spec[2]);
    return out;
}

/* ══════════════ таблица стилей ══════════════ */
/* unicode-range покрывает базовую латиницу или кириллицу? (нет дескриптора — да) */
static int urange_ok(const char* inner, size_t il) {
    const char* p = strcasestr(inner, "unicode-range");
    if (!p || p >= inner + il) return 1;
    p = strchr(p, ':'); if (!p) return 1;
    const char* end = p; while (end < inner + il && *end != ';' && *end != '}') end++;
    for (const char* q = p; q < end; q++) {
        if ((q[0] == 'U' || q[0] == 'u') && q[1] == '+') {
            char* e; unsigned lo = (unsigned)strtoul(q + 2, &e, 16), hi = lo;
            if (*e == '-') hi = (unsigned)strtoul(e + 1, &e, 16);
            else if (*e == '?') { unsigned m = 0; const char* w = e; while (*w == '?') { m = m * 16 + 15; w++; } lo = lo << (4 * (int)(w - e)); hi = lo | m; }
            if ((lo <= 0x61 && hi >= 0x61) || (lo <= 0x430 && hi >= 0x430)) return 1;
        }
    }
    return 0;
}

static const char* skip_ws(const char* p) {
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '*') { const char* e = strstr(p + 2, "*/"); p = e ? e + 2 : p + strlen(p); continue; }
        if (!strncmp(p, "<!--", 4)) { p += 4; continue; }
        if (!strncmp(p, "-->", 3)) { p += 3; continue; }
        return p;
    }
}
/* p — на '{'; возвращает указатель за парной '}' */
static const char* block_end(const char* p) {
    int d = 0;
    while (*p) {
        if (*p == '"' || *p == '\'') { p = skip_str(p); continue; }
        if (p[0] == '/' && p[1] == '*') { const char* e = strstr(p + 2, "*/"); p = e ? e + 2 : p + strlen(p); continue; }
        if (*p == '{') d++; else if (*p == '}') { d--; if (d == 0) return p + 1; }
        p++;
    }
    return p;
}
/* Идёт до одного из stops на верхнем уровне скобок. */
static const char* scan_until(const char* p, const char* stops) {
    int d = 0;
    while (*p) {
        if (*p == '"' || *p == '\'') { p = skip_str(p); continue; }
        if (p[0] == '/' && p[1] == '*') { const char* e = strstr(p + 2, "*/"); p = e ? e + 2 : p + strlen(p); continue; }
        if (*p == '(' || *p == '[') d++; else if (*p == ')' || *p == ']') d--;
        else if (d <= 0 && strchr(stops, *p)) return p;
        p++;
    }
    return p;
}

static void add_rule(RuleSet* rs, Rule* r) {
    if (rs->n == rs->cap) { rs->cap = rs->cap ? rs->cap * 2 : 512; rs->v = realloc(rs->v, (size_t)rs->cap * sizeof(Rule*)); }
    rs->v[rs->n++] = r;
}

static void parse_rule_block(Arena* a, RuleSet* rs, const char* sel, size_t sel_len, const char* body, size_t body_len, int origin) {
    int nd = 0; Decl* decls = css_parse_decls(a, body, body_len, &nd);
    if (!decls) return;
    for (int i = 0; i < nd; i++) if (decls[i].prop >= P_TRANSITION && decls[i].prop <= P_ANIMATION_PLAY) { rs->has_trans = 1; break; }
    char* selz = astrdup(a, sel, sel_len);                 /* NUL-терминированная копия: scan_until не должен уходить за селектор */
    const char* s = selz; const char* end = selz + sel_len;
    while (s < end) {
        const char* c = scan_until(s, ","); if (c > end) c = end;
        const char* ss = s; while (ss < c && isspace((unsigned char)*ss)) ss++;
        const char* se = c; while (se > ss && isspace((unsigned char)se[-1])) se--;
        if (se > ss) {
            int nsel = 0; uint32_t spec = 0;
            g_allow_pseudo = 1; Simple* sp = parse_selector(a, ss, (size_t)(se - ss), &nsel, &spec); g_allow_pseudo = 0;
            if (sp) {
                Rule* r = aalloc(a, sizeof(Rule)); r->sel = sp; r->nsel = nsel; r->spec = spec; r->order = rs->next_order++; r->pseudo = sp[nsel - 1].pseudo;
                r->origin = origin; r->decls = decls; r->ndecls = nd; add_rule(rs, r);
            }
        }
        s = c + 1;
    }
}

void css_parse_sheet(Arena* a, RuleSet* rs, const char* css, const char* sheet_url, int origin, int depth) {
    if (!css || depth > 6) return;
    const char* p = css;
    while (*(p = skip_ws(p))) {
        if (*p == '@') {
            const char* ns = p + 1; const char* ne = ns; while (isalpha((unsigned char)*ne) || *ne == '-') ne++;
            size_t nl = (size_t)(ne - ns);
            const char* semi = scan_until(ne, ";{"); 
            if (*semi != '{') {                                                   /* @import / @charset / @layer a,b; */
                if (nl == 6 && !strncasecmp(ns, "import", 6) && css_fetch_budget > 0) {
                    const char* q = skip_ws(ne); char url[1024] = ""; 
                    if (!strncasecmp(q, "url(", 4)) { q += 4; while (isspace((unsigned char)*q)) q++; char qc = (*q == '"' || *q == '\'') ? *q++ : 0; size_t k = 0; while (*q && (qc ? *q != qc : *q != ')') && k < 1000) url[k++] = *q++; url[k] = 0; while (*q && *q != ')') q++; if (*q) q++; }
                    else if (*q == '"' || *q == '\'') { char qc = *q++; size_t k = 0; while (*q && *q != qc && k < 1000) url[k++] = *q++; url[k] = 0; if (*q) q++; }
                    char media[128] = ""; size_t ml = (size_t)(semi - q); if (ml > 127) ml = 127; memcpy(media, q, ml); media[ml] = 0;
                    if (url[0] && css_media_matches(media)) {
                        char abs[1200]; webpage_resolve_url(sheet_url, url, abs, sizeof abs);
                        if (!strncmp(abs, "http", 4) && !webpage_mixed_blocked(abs)) {
                            css_fetch_budget--; char* sub = nal_http_get(abs);
                            if (sub) { css_parse_sheet(a, rs, sub, abs, origin, depth + 1); free(sub); }
                        }
                    }
                }
                p = *semi ? semi + 1 : semi; continue;
            }
            const char* bend = block_end(semi);
            const char* inner = semi + 1; size_t il = (size_t)(bend - 1 - inner); if (bend <= inner) il = 0;
            if (nl >= 9 && !strncasecmp(ne - 9, "keyframes", 9)) {                  /* @keyframes / @-webkit-keyframes */
                const char* q = skip_ws(ne); char nm[48]; size_t k = 0;
                if (*q == '"' || *q == '\'') { char qc = *q++; while (*q && *q != qc && k < 47) nm[k++] = *q++; }
                else while (q < semi && !isspace((unsigned char)*q) && k < 47) nm[k++] = *q++;
                nm[k] = 0;
                if (nm[0]) {
                    Keyframes* kf = aalloc(a, sizeof *kf); snprintf(kf->name, sizeof kf->name, "%s", nm);
                    int cap = 0; const char* r = inner; const char* rend = inner + il;
                    while (r < rend) {
                        r = skip_ws(r); if (r >= rend) break;
                        const char* br = scan_until(r, "{;}"); if (br >= rend || *br != '{') break;
                        const char* be = block_end(br); const char* in2 = br + 1; size_t il2 = be > in2 ? (size_t)(be - 1 - in2) : 0;
                        int nd = 0; Decl* dd = css_parse_decls(a, in2, il2, &nd);
                        char* selz = astrdup(a, r, (size_t)(br - r)); char* tk = selz;
                        while (*tk) {                                                    /* «0%, 50%» — несколько смещений на один блок */
                            while (isspace((unsigned char)*tk) || *tk == ',') tk++; if (!*tk) break;
                            float off = -1; char* te = tk; while (*te && *te != ',' && !isspace((unsigned char)*te)) te++;
                            char sv = *te; *te = 0;
                            if (!strcasecmp(tk, "from")) off = 0; else if (!strcasecmp(tk, "to")) off = 1;
                            else { char* e2; float v = strtof(tk, &e2); if (e2 != tk && *e2 == '%') off = v / 100.0f; }
                            *te = sv; tk = te;
                            if (off >= 0 && off <= 1 && nd) {
                                if (kf->n == cap) { int nc = cap ? cap * 2 : 8; KfFrame* nf = aalloc(a, (size_t)nc * sizeof(KfFrame)); if (kf->n) memcpy(nf, kf->fr, (size_t)kf->n * sizeof(KfFrame)); kf->fr = nf; cap = nc; }
                                kf->fr[kf->n].off = off; kf->fr[kf->n].decls = dd; kf->fr[kf->n].nd = nd; kf->n++;
                            }
                        }
                        r = be;
                    }
                    for (int i = 1; i < kf->n; i++) { KfFrame t = kf->fr[i]; int j = i - 1; while (j >= 0 && kf->fr[j].off > t.off) { kf->fr[j + 1] = kf->fr[j]; j--; } kf->fr[j + 1] = t; }   /* по возрастанию смещения (устойчиво) */
                    kf->next = rs->kf; rs->kf = kf; rs->has_trans = 1;
                }
            } else
            if (nl == 5 && !strncasecmp(ns, "media", 5)) {
                char q[256]; size_t ql = (size_t)(semi - ne); if (ql > 255) ql = 255; memcpy(q, ne, ql); q[ql] = 0;
                if (css_media_matches(q)) { char* in = malloc(il + 1); if (in) { memcpy(in, inner, il); in[il] = 0; css_parse_sheet(a, rs, in, sheet_url, origin, depth + 1); free(in); } }
            } else if ((nl == 8 && !strncasecmp(ns, "supports", 8)) || (nl == 5 && !strncasecmp(ns, "layer", 5))) {
                const char* q = skip_ws(ne);
                if (!(nl == 8 && !strncasecmp(q, "not", 3))) { char* in = malloc(il + 1); if (in) { memcpy(in, inner, il); in[il] = 0; css_parse_sheet(a, rs, in, sheet_url, origin, depth + 1); free(in); } }
            } else if (nl == 9 && !strncasecmp(ns, "font-face", 9) && origin != 0) {
                int nd = 0; Decl* d = css_parse_decls(a, inner, il, &nd);
                const char *fam = NULL, *src = NULL, *wt = NULL, *st = NULL;
                for (int i = 0; i < nd; i++) {
                    if (d[i].prop == P_FONT_FAMILY) fam = d[i].val; else if (d[i].prop == P_FONT_WEIGHT) wt = d[i].val; else if (d[i].prop == P_FONT_STYLE) st = d[i].val;
                }
                /* src не входит в PROP_LIST — достаём вручную */
                const char* sp = strcasestr(inner, "src"); 
                char* srcbuf = NULL;
                while (sp && sp < inner + il) {
                    const char* q = sp + 3; while (q < inner + il && isspace((unsigned char)*q)) q++;
                    if (*q == ':' && (sp == inner || isspace((unsigned char)sp[-1]) || sp[-1] == ';')) {
                        const char* e = scan_until(q + 1, ";}"); if (e > inner + il) e = inner + il;
                        srcbuf = astrdup(a, q + 1, (size_t)(e - q - 1)); break;
                    }
                    sp = strcasestr(sp + 3, "src");
                }
                src = srcbuf;
                if (fam && src && urange_ok(inner, il)) font_manager_add_face(fam, src, wt, st, sheet_url);
            }
            p = bend; continue;
        }
        const char* brace = scan_until(p, "{;}");
        if (*brace == '}') { p = brace + 1; continue; }
        if (*brace == ';') { p = brace + 1; continue; }
        if (!*brace) break;
        const char* bend = block_end(brace);
        const char* inner = brace + 1; size_t il = bend > inner ? (size_t)(bend - 1 - inner) : 0;
        parse_rule_block(a, rs, p, (size_t)(brace - p), inner, il, origin);
        p = bend;
    }
}

/* Для JS (querySelector и т.п.): разбор одного сложного селектора без разбивки по запятым. */
Simple* css_parse_selector_ex(Arena* a, const char* s, size_t n, int* nsel) { uint32_t spec; return parse_selector(a, s, n, nsel, &spec); }
