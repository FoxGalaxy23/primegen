/* HTML: токенайзер + построение дерева.
   Результат — список DOMNode в порядке обхода документа (pre-order) с индексами
   parent / subtree_end, поэтому старый код, ходящий по ->next, продолжает работать. */
#include "webpage.h"
#include "gal.h"
#include "nal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>

/* ══════════════ утилиты ══════════════ */
static int ci_prefix(const char* s, const char* pre) {
    while (*pre) { if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0; s++; pre++; }
    return 1;
}
static const char* find_ci(const char* s, const char* needle) {
    for (; *s; s++) if (ci_prefix(s, needle)) return s;
    return NULL;
}
char* webpage_strcasestr(const char* h, const char* n) { return (char*)find_ci(h, n); }
static void put_str(char* dst, size_t cap, const char* src, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n); dst[n] = 0;
}
static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* ══════════════ сущности ══════════════ */
static const char* LATIN1 =
    "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr deg plusmn sup2 sup3 "
    "acute micro para middot cedil sup1 ordm raquo frac14 frac12 frac34 iquest Agrave Aacute Acirc Atilde "
    "Auml Aring AElig Ccedil Egrave Eacute Ecirc Euml Igrave Iacute Icirc Iuml ETH Ntilde Ograve Oacute Ocirc "
    "Otilde Ouml times Oslash Ugrave Uacute Ucirc Uuml Yacute THORN szlig agrave aacute acirc atilde auml aring "
    "aelig ccedil egrave eacute ecirc euml igrave iacute icirc iuml eth ntilde ograve oacute ocirc otilde ouml "
    "divide oslash ugrave uacute ucirc uuml yacute thorn yuml";
static const char* EXTRA =
    "amp:38 lt:60 gt:62 quot:34 apos:39 hellip:8230 mdash:8212 ndash:8211 lsquo:8216 rsquo:8217 sbquo:8218 "
    "ldquo:8220 rdquo:8221 bdquo:8222 bull:8226 trade:8482 euro:8364 larr:8592 rarr:8594 uarr:8593 darr:8595 "
    "harr:8596 lArr:8656 rArr:8658 hArr:8660 dagger:8224 Dagger:8225 permil:8240 lsaquo:8249 rsaquo:8250 "
    "prime:8242 Prime:8243 oline:8254 frasl:8260 ensp:8194 emsp:8195 thinsp:8201 zwnj:8204 zwj:8205 lrm:8206 "
    "rlm:8207 minus:8722 lowast:8727 radic:8730 prop:8733 infin:8734 ang:8736 and:8743 or:8744 cap:8745 "
    "cup:8746 int:8747 there4:8756 sim:8764 cong:8773 asymp:8776 ne:8800 equiv:8801 le:8804 ge:8805 sub:8834 "
    "sup:8835 sube:8838 supe:8839 oplus:8853 otimes:8855 perp:8869 sdot:8901 loz:9674 spades:9824 clubs:9827 "
    "hearts:9829 diams:9830 check:10003 starf:9733 star:9734 phone:9742 alpha:945 beta:946 gamma:947 delta:948 "
    "epsilon:949 zeta:950 eta:951 theta:952 iota:953 kappa:954 lambda:955 mu:956 nu:957 xi:958 omicron:959 "
    "pi:960 rho:961 sigma:963 tau:964 upsilon:965 phi:966 chi:967 psi:968 omega:969 Alpha:913 Beta:914 "
    "Gamma:915 Delta:916 Theta:920 Lambda:923 Pi:928 Sigma:931 Phi:934 Psi:936 Omega:937 OElig:338 oelig:339 "
    "Scaron:352 scaron:353 Yuml:376 fnof:402 circ:710 tilde:732 lceil:8968 rceil:8969 lfloor:8970 rfloor:8971 "
    "part:8706 nabla:8711 isin:8712 sum:8721 prod:8719 forall:8704 exist:8707 empty:8709 hyphen:8208 ";

static int named_entity(const char* name, size_t n) {
    if (n == 0 || n > 10) return 0;
    const char* p = LATIN1; int code = 160;
    while (*p) {
        const char* e = p; while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && strncmp(p, name, n) == 0) return code;
        code++; p = *e ? e + 1 : e;
    }
    p = EXTRA;
    while (*p) {
        const char* c = strchr(p, ':'); if (!c) break;
        const char* e = c; while (*e && *e != ' ') e++;
        if ((size_t)(c - p) == n && strncmp(p, name, n) == 0) return atoi(c + 1);
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int put_utf8(char* d, uint32_t cp) {
    if (cp < 0x80) { d[0] = (char)cp; return 1; }
    if (cp < 0x800) { d[0] = 0xC0 | (cp >> 6); d[1] = 0x80 | (cp & 63); return 2; }
    if (cp < 0x10000) { d[0] = 0xE0 | (cp >> 12); d[1] = 0x80 | ((cp >> 6) & 63); d[2] = 0x80 | (cp & 63); return 3; }
    d[0] = 0xF0 | (cp >> 18); d[1] = 0x80 | ((cp >> 12) & 63); d[2] = 0x80 | ((cp >> 6) & 63); d[3] = 0x80 | (cp & 63); return 4;
}

/* Декодирует сущности из s[0..n) в out (capacity >= n+1). Возвращает длину. */
static size_t decode_entities(const char* s, size_t n, char* out, int in_attr) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (s[i] != '&') { out[o++] = s[i++]; continue; }
        size_t j = i + 1;
        if (j < n && s[j] == '#') {
            j++; int hex = 0; if (j < n && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
            uint32_t cp = 0; size_t st = j;
            while (j < n && (hex ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j])) && cp < 0x110000)
                cp = cp * (hex ? 16 : 10) + (isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower(s[j]) - 'a' + 10)), j++;
            if (j > st) {
                if (j < n && s[j] == ';') j++;
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
                if (cp >= 0x80 && cp < 0xA0) { static const uint16_t w[32] = {8364,129,8218,402,8222,8230,8224,8225,710,8240,352,8249,338,141,381,143,144,8216,8217,8220,8221,8226,8211,8212,732,8482,353,8250,339,157,382,376}; cp = w[cp - 0x80]; }
                o += put_utf8(out + o, cp); i = j; continue;
            }
        } else {
            size_t st = j;
            while (j < n && isalnum((unsigned char)s[j]) && j - st < 10) j++;
            int cp = named_entity(s + st, j - st);
            if (cp) {
                int semi = (j < n && s[j] == ';');
                int legacy = cp < 256 || cp == 38 || cp == 60 || cp == 62 || cp == 34;
                if (semi || (legacy && !in_attr && !(j < n && isalnum((unsigned char)s[j])))) {
                    if (semi) j++;
                    o += put_utf8(out + o, (uint32_t)cp); i = j; continue;
                }
            }
        }
        out[o++] = s[i++];
    }
    out[o] = 0;
    return o;
}

/* ══════════════ кодировки ══════════════ */
int webpage_is_utf8(const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p < 0x80) { p++; continue; }
        int n = (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || (*p & 0xFE) == 0xC0) return 0;
        for (int i = 1; i <= n; i++) if ((p[i] & 0xC0) != 0x80) return 0;
        p += n + 1;
    }
    return 1;
}

static const uint16_t CP1251_80[64] = {
    0x402,0x403,0x201A,0x453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x409,0x2039,0x40A,0x40C,0x40B,0x40F,
    0x452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x98,0x2122,0x459,0x203A,0x45A,0x45C,0x45B,0x45F,
    0xA0,0x40E,0x45E,0x408,0xA4,0x490,0xA6,0xA7,0x401,0xA9,0x404,0xAB,0xAC,0xAD,0xAE,0x407,
    0xB0,0xB1,0x406,0x456,0x491,0xB5,0xB6,0xB7,0x451,0x2116,0x454,0xBB,0x458,0x405,0x455,0x457 };
static const uint16_t CP1252_80[32] = {
    0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
    0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178 };

/* Возвращает malloc'нутую UTF-8 копию, если документ не в UTF-8 (по meta charset), иначе NULL. */
static char* convert_to_utf8(const char* in) {
    if (webpage_is_utf8(in)) return NULL;
    char head[4096]; put_str(head, sizeof head, in, strlen(in) < 4095 ? strlen(in) : 4095);
    const char* cs = find_ci(head, "charset=");
    char name[32] = "windows-1252";
    if (cs) { cs += 8; while (*cs == '"' || *cs == '\'' || *cs == ' ') cs++; size_t k = 0; while (k < 31 && (isalnum((unsigned char)cs[k]) || cs[k] == '-' || cs[k] == '_')) { name[k] = (char)tolower((unsigned char)cs[k]); k++; } name[k] = 0; }
    int kind = 0;                                            /* 0 = latin1/1252, 1 = 1251, 2 = koi8-r */
    if (strstr(name, "1251") || strstr(name, "cyrillic") || strstr(name, "8859-5")) kind = 1;
    else if (strstr(name, "koi8")) kind = 2;
    size_t n = strlen(in);
    char* out = malloc(n * 3 + 1); if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i]; uint32_t cp = c;
        if (c >= 0x80) {
            if (kind == 1) cp = c < 0xC0 ? CP1251_80[c - 0x80] : 0x410 + (c - 0xC0);
            else if (kind == 2) {
                static const char* lo = "юабцдефгхийклмнопярстужвьызшэщчъ";          /* 0xC0..0xDF (строчные, KOI8-R) */
                if (c >= 0xC0) { int idx = c & 0x1F; int upper = c >= 0xE0; const char* u = lo; for (int k = 0; k < idx; k++) u += 2; uint32_t base = ((unsigned char)u[0] & 0x1F) << 6 | ((unsigned char)u[1] & 0x3F); cp = upper ? base - 0x20 : base; }
                else if (c == 0xA3) cp = 0x451; else if (c == 0xB3) cp = 0x401; else cp = '?';
            } else cp = (c < 0xA0) ? CP1252_80[c - 0x80] : c;
        }
        o += put_utf8(out + o, cp);
    }
    out[o] = 0;
    return out;
}

/* ══════════════ URL ══════════════ */
void webpage_resolve_url(const char* base, const char* rel, char* out, size_t cap) {
    out[0] = 0;
    if (!rel || !*rel) return;
    while (is_ws(*rel)) rel++;
    if (ci_prefix(rel, "http://") || ci_prefix(rel, "https://") || ci_prefix(rel, "data:") || ci_prefix(rel, "javascript:") ||
        ci_prefix(rel, "mailto:") || ci_prefix(rel, "tel:") || rel[0] == '#') { put_str(out, cap, rel, strlen(rel)); return; }
    int web = base && (ci_prefix(base, "http://") || ci_prefix(base, "https://"));
    char tmp[2048];
    if (web) {
        const char* se = strstr(base, "://"); const char* auth = se + 3; const char* ae = auth + strcspn(auth, "/?#");
        size_t plen = strcspn(ae, "?#");
        if (rel[0] == '/' && rel[1] == '/') snprintf(tmp, sizeof tmp, "%.*s:%s", (int)(se - base), base, rel);
        else if (rel[0] == '/') snprintf(tmp, sizeof tmp, "%.*s%s", (int)(ae - base), base, rel);
        else if (rel[0] == '?') snprintf(tmp, sizeof tmp, "%.*s%.*s%s", (int)(ae - base), base, (int)plen, ae, rel);
        else {
            size_t d = plen; while (d > 0 && ae[d - 1] != '/') d--;
            if (d == 0) snprintf(tmp, sizeof tmp, "%.*s/%s", (int)(ae - base), base, rel);
            else snprintf(tmp, sizeof tmp, "%.*s%.*s%s", (int)(ae - base), base, (int)d, ae, rel);
        }
        /* убираем ./ и ../ в пути */
        char* ps = strstr(tmp, "://"); if (ps) { ps += 3; ps += strcspn(ps, "/?#"); }
        if (ps && *ps == '/') {
            char* qs = ps + strcspn(ps, "?#"); char* tail = strdup(qs); char* w = ps; const char* r = ps;
            while (r < qs && tail) {
                const char* seg = r + 1; const char* se2 = seg; while (se2 < qs && *se2 != '/') se2++;
                size_t sl = (size_t)(se2 - seg);
                if (sl == 1 && seg[0] == '.') { if (se2 == qs) *w++ = '/'; }
                else if (sl == 2 && seg[0] == '.' && seg[1] == '.') { if (w > ps) { w--; while (w > ps && *w != '/') w--; } if (se2 == qs) *w++ = '/'; }
                else { *w++ = '/'; memmove(w, seg, sl); w += sl; }
                r = se2;
            }
            if (tail) { memmove(w, tail, strlen(tail) + 1); free(tail); }
        }
    } else if (base && rel[0] != '/') {                       /* локальный файл */
        const char* sl = strrchr(base, '/');
        if (sl) snprintf(tmp, sizeof tmp, "%.*s/%s", (int)(sl - base), base, rel); else snprintf(tmp, sizeof tmp, "%s", rel);
    } else snprintf(tmp, sizeof tmp, "%s", rel);
    put_str(out, cap, tmp, strlen(tmp));
}

/* ══════════════ таблицы тегов ══════════════ */
static const struct { const char* n; int t; } TAGMAP[] = {
    {"a",TAG_A},{"img",TAG_IMG},{"div",TAG_DIV},{"button",TAG_BUTTON},{"input",TAG_INPUT},{"span",TAG_SPAN},
    {"html",TAG_HTML},{"head",TAG_HEAD},{"body",TAG_BODY},{"title",TAG_TITLE},{"meta",TAG_META},{"link",TAG_LINK},
    {"style",TAG_STYLE},{"script",TAG_SCRIPT},{"main",TAG_MAIN},{"section",TAG_SECTION},{"article",TAG_ARTICLE},
    {"header",TAG_HEADER},{"footer",TAG_FOOTER},{"nav",TAG_NAV},{"aside",TAG_ASIDE},{"ul",TAG_UL},{"ol",TAG_OL},
    {"li",TAG_LI},{"table",TAG_TABLE},{"tr",TAG_TR},{"td",TAG_TD},{"th",TAG_TH},{"thead",TAG_THEAD},
    {"tbody",TAG_TBODY},{"form",TAG_FORM},{"label",TAG_LABEL},{"select",TAG_SELECT},{"option",TAG_OPTION},
    {"textarea",TAG_TEXTAREA},{"fieldset",TAG_FIELDSET},{"legend",TAG_LEGEND},{"br",TAG_BR},{"hr",TAG_HR},
    {"iframe",TAG_IFRAME},{"video",TAG_VIDEO},{"audio",TAG_AUDIO},{"source",TAG_SOURCE},{"canvas",TAG_CANVAS},
    {"svg",TAG_SVG},{"picture",TAG_PICTURE},{"noscript",TAG_NOSCRIPT},{"p",TAG_P},
    {"h1",TAG_H},{"h2",TAG_H},{"h3",TAG_H},{"h4",TAG_H},{"h5",TAG_H},{"h6",TAG_H},{NULL,0}};

static int in_list(const char* list, const char* name) {           /* list: " a b c " */
    char key[40]; snprintf(key, sizeof key, " %s ", name);
    return strstr(list, key) != NULL;
}
static const char* VOID_TAGS   = " area base basefont br col embed hr img input keygen link meta param source track wbr frame ";
static const char* HEAD_TAGS   = " base link meta script style title noscript template ";
static const char* RAW_TAGS    = " script style xmp iframe noembed noframes noscript ";
static const char* RCDATA_TAGS = " textarea title ";
static const char* SKIP_TAGS   = " template canvas math ";
static const char* P_CLOSERS   = " address article aside blockquote center details dialog dir div dl fieldset figcaption figure footer form h1 h2 h3 h4 h5 h6 header hgroup hr main menu nav ol p pre section summary table ul li dd dt listing xmp ";
static const char* NO_WS_TEXT  = " html head table thead tbody tfoot tr colgroup select optgroup ul ol dl ";
static const char* SCOPE_STOP  = " td th table caption html button object marquee ";

/* ══════════════ бандл страницы ══════════════ */
static PageBundle g_bundle;
static void bundle_reset(void) {
    for (int i = 0; i < g_bundle.nsheets; i++) { free(g_bundle.sheets[i].css); free(g_bundle.sheets[i].href); }
    for (int i = 0; i < g_bundle.nscripts; i++) { free(g_bundle.scripts[i].text); free(g_bundle.scripts[i].url); }
    free(g_bundle.sheets); free(g_bundle.scripts); memset(&g_bundle, 0, sizeof g_bundle);
}
static void bundle_add_script(char* text, char* url, int module, int async_, int defer_, int idx) {
    g_bundle.scripts = realloc(g_bundle.scripts, (size_t)(g_bundle.nscripts + 1) * sizeof(ScriptSrc));
    ScriptSrc* sc = &g_bundle.scripts[g_bundle.nscripts++]; sc->text = text; sc->url = url; sc->module = (uint8_t)module; sc->async_ = (uint8_t)async_; sc->defer_ = (uint8_t)defer_; sc->node_idx = idx;
}

/* ══════════════ состояние сборки ══════════════ */
typedef struct {
    DOMNode** v; int n, cap;
    int stack[256]; int sp;              /* индексы открытых элементов */
    int head, body, mode_head;           /* mode_head: ещё не встретили содержимое body */
    char base[1024];
    SheetList* sheets;
    int skip_nl;
} PB;

DOMNode* webpage_new_node(const char* tag) {
    DOMNode* n = calloc(1, sizeof(DOMNode)); if (!n) return NULL;
    n->v2 = 1; n->parent = -1; n->subtree_end = 0; n->attr_w = n->attr_h = -1; n->colspan = n->rowspan = 1;
    n->bg_color = 0xFFFFFF; n->text_color = 0;
    put_str(n->tag, sizeof n->tag, tag, strlen(tag));
    n->type = TAG_OTHER;
    if (!strcmp(tag, "#text")) n->type = TAG_TEXT;
    else for (int i = 0; TAGMAP[i].n; i++) if (strcmp(TAGMAP[i].n, tag) == 0) { n->type = (TagType)TAGMAP[i].t; break; }
    return n;
}

static DOMNode* mk(PB* pb, int parent, const char* tag) {
    DOMNode* n = webpage_new_node(tag);
    if (!n) return NULL;
    if (pb->n == pb->cap) {
        pb->cap = pb->cap ? pb->cap * 2 : 256;
        DOMNode** nv = realloc(pb->v, (size_t)pb->cap * sizeof(DOMNode*));
        if (!nv) { free(n); return NULL; }
        pb->v = nv;
    }
    n->idx = pb->n; n->parent = parent; n->subtree_end = pb->n;
    pb->v[pb->n++] = n;
    return n;
}

static void add_sheet(PB* pb, char* css, char* href, const char* media) {
    SheetList* sl = pb->sheets;
    if (sl->n == sl->cap) { sl->cap = sl->cap ? sl->cap * 2 : 8; sl->v = realloc(sl->v, (size_t)sl->cap * sizeof(SheetSrc)); }
    SheetSrc* s = &sl->v[sl->n++]; memset(s, 0, sizeof *s);
    s->css = css; s->href = href; put_str(s->media, sizeof s->media, media ? media : "", media ? strlen(media) : 0);
}

int webpage_attr_get(const DOMNode* n, const char* name, char* out, size_t cap) {
    out[0] = 0;
    size_t nl = strlen(name);
    const char* p = n->attrs;
    while (*p) {
        const char* sep = strchr(p, 1); if (!sep) break;
        const char* end = strchr(sep, 2); if (!end) break;
        if ((size_t)(sep - p) == nl && strncmp(p, name, nl) == 0) { put_str(out, cap, sep + 1, (size_t)(end - sep - 1)); return 1; }
        p = end + 1;
    }
    return 0;
}

static void pres_add(DOMNode* n, const char* css) {
    size_t l = strlen(n->pres), a = strlen(css);
    if (l + a + 2 < sizeof n->pres) { memcpy(n->pres + l, css, a); n->pres[l + a] = ';'; n->pres[l + a + 1] = 0; }
}
static void pres_dim(DOMNode* n, const char* prop, const char* v) {
    char buf[96]; while (is_ws(*v)) v++;
    if (!*v) return;
    if (strchr(v, '%')) snprintf(buf, sizeof buf, "%s:%dpercent", prop, atoi(v));
    else if (isdigit((unsigned char)*v)) snprintf(buf, sizeof buf, "%s:%dpx", prop, atoi(v));
    else return;
    char* pc = strstr(buf, "percent"); if (pc) { pc[0] = '%'; pc[1] = 0; }
    pres_add(n, buf);
}
static void pres_color(DOMNode* n, const char* prop, const char* v) {
    char buf[96], c[40]; put_str(c, sizeof c, v, strlen(v));
    if (c[0] != '#' && (strlen(c) == 6 || strlen(c) == 3) && strspn(c, "0123456789abcdefABCDEF") == strlen(c)) snprintf(buf, sizeof buf, "%s:#%s", prop, c);
    else snprintf(buf, sizeof buf, "%s:%s", prop, c);
    pres_add(n, buf);
}

typedef struct { char name[32]; char* val; } Attr;

/* Заполняет узел из атрибутов. */
static void apply_attrs(PB* pb, DOMNode* n, Attr* a, int na) {
    const char* tag = n->tag; int is_tbl = !strcmp(tag, "table"), is_cell = !strcmp(tag, "td") || !strcmp(tag, "th");
    int apos = 0;
    for (int i = 0; i < na; i++) {
        const char* k = a[i].name; const char* v = a[i].val ? a[i].val : "";
        size_t vl = strlen(v);
        if (strcmp(k, "style") != 0 && strcmp(k, "class") != 0 && vl < 160 && strlen(k) < 32 &&
            apos + (int)strlen(k) + (int)vl + 3 < (int)sizeof n->attrs) {
            apos += snprintf(n->attrs + apos, sizeof n->attrs - apos, "%s%c%s%c", k, 1, v, 2);
        }
        if (!strcmp(k, "id")) put_str(n->id, sizeof n->id, v, vl);
        else if (!strcmp(k, "class")) put_str(n->class_name, sizeof n->class_name, v, vl);
        else if (!strcmp(k, "style")) put_str(n->style_str, sizeof n->style_str, v, vl);
        else if (!strcmp(k, "onclick")) put_str(n->onclick, sizeof n->onclick, v, vl);
        else if (!strcmp(k, "name")) put_str(n->name, sizeof n->name, v, vl);
        else if (!strcmp(k, "value")) put_str(n->value, sizeof n->value, v, vl);
        else if (!strcmp(k, "type")) { put_str(n->input_type, sizeof n->input_type, v, vl); for (char* c = n->input_type; *c; c++) *c = (char)tolower((unsigned char)*c); }
        else if (!strcmp(k, "placeholder")) put_str(n->placeholder, sizeof n->placeholder, v, vl);
        else if (!strcmp(k, "alt") || (!strcmp(k, "title") && !n->alt[0] && strcmp(tag, "img") == 0)) put_str(n->alt, sizeof n->alt, v, vl);
        else if (!strcmp(k, "checked")) n->checked = 1;
        else if (!strcmp(k, "disabled")) n->disabled = 1;
        else if (!strcmp(k, "readonly")) n->readonly = 1;
        else if (!strcmp(k, "selected")) n->selected = 1;
        else if (!strcmp(k, "multiple")) n->multiple = 1;
        else if (!strcmp(k, "colspan")) n->colspan = atoi(v) > 0 ? (atoi(v) > 1000 ? 1000 : atoi(v)) : 1;
        else if (!strcmp(k, "rowspan")) n->rowspan = atoi(v) > 0 ? (atoi(v) > 1000 ? 1000 : atoi(v)) : 1;
        else if (!strcmp(k, "size") && strcmp(tag, "font") != 0) n->size_attr = atoi(v);
        else if (!strcmp(k, "rows")) n->rows = atoi(v);
        else if (!strcmp(k, "cols")) n->cols = atoi(v);
        else if (!strcmp(k, "href") && n->type != TAG_LINK && n->type != TAG_OTHER) webpage_resolve_url(pb->base, v, n->href, sizeof n->href);
        else if (!strcmp(k, "href")) webpage_resolve_url(pb->base, v, n->href, sizeof n->href);
        else if (!strcmp(k, "src")) {
            if (!ci_prefix(v, "data:")) webpage_resolve_url(pb->base, v, n->src, sizeof n->src);
            else { char dp[200]; if (webpage_data_uri_to_file(v, strlen(v), dp, sizeof dp)) put_str(n->src, sizeof n->src, dp, strlen(dp)); }      /* data: → локальный файл кэша */
        }
        else if (!strcmp(k, "data-src") || !strcmp(k, "data-lazy-src") || !strcmp(k, "data-original")) { if (!n->src[0] && !strcmp(tag, "img")) webpage_resolve_url(pb->base, v, n->src, sizeof n->src); }
        else if (!strcmp(k, "srcset") && !strcmp(tag, "img") && !n->src[0]) {   /* берём первый URL из srcset */
            char first[512]; size_t l = strcspn(v, " ,"); put_str(first, sizeof first, v, l);
            webpage_resolve_url(pb->base, first, n->src, sizeof n->src);
        }
        /* презентационные атрибуты */
        else if (!strcmp(k, "width")) { if (!is_cell || 1) pres_dim(n, "width", v); if (isdigit((unsigned char)*v) && !strchr(v, '%')) n->attr_w = atoi(v); }
        else if (!strcmp(k, "height")) { pres_dim(n, "height", v); if (isdigit((unsigned char)*v) && !strchr(v, '%')) n->attr_h = atoi(v); }
        else if (!strcmp(k, "bgcolor")) pres_color(n, "background-color", v);
        else if (!strcmp(k, "text") && !strcmp(tag, "body")) pres_color(n, "color", v);
        else if (!strcmp(k, "color") && !strcmp(tag, "font")) pres_color(n, "color", v);
        else if (!strcmp(k, "face") && !strcmp(tag, "font")) { char b[160]; snprintf(b, sizeof b, "font-family:%s", v); pres_add(n, b); }
        else if (!strcmp(k, "size") && !strcmp(tag, "font")) {
            static const int fs[8] = {10, 10, 13, 16, 18, 24, 32, 48}; int s = atoi(v); if (v[0] == '+' || v[0] == '-') s += 3;
            if (s < 1) s = 1; if (s > 7) s = 7; char b[40]; snprintf(b, sizeof b, "font-size:%dpx", fs[s]); pres_add(n, b);
        }
        else if (!strcmp(k, "align")) {
            if (!strcasecmp(v, "center") || !strcasecmp(v, "left") || !strcasecmp(v, "right")) {
                char b[64];
                if (is_tbl && !strcasecmp(v, "center")) pres_add(n, "margin-left:auto;margin-right:auto");
                else if ((!strcmp(tag, "img") || is_tbl) && strcasecmp(v, "center")) { snprintf(b, sizeof b, "float:%s", v); pres_add(n, b); }
                else { snprintf(b, sizeof b, "text-align:%s%s", !strcasecmp(v, "center") ? "-webkit-" : "", v); pres_add(n, b); }
            }
        }
        else if (!strcmp(k, "valign")) { char b[48]; snprintf(b, sizeof b, "vertical-align:%s", v); pres_add(n, b); }
        else if (!strcmp(k, "nowrap")) pres_add(n, "white-space:nowrap");
        else if (!strcmp(k, "border") && (is_tbl || !strcmp(tag, "img")) && atoi(v) > 0) {
            char b[64]; snprintf(b, sizeof b, "border:%dpx %s #808080", atoi(v), is_tbl ? "outset" : "solid"); pres_add(n, b);
        }
        else if (!strcmp(k, "cellspacing") && is_tbl) { char b[48]; snprintf(b, sizeof b, "border-spacing:%dpx", atoi(v)); pres_add(n, b); }
        else if (!strcmp(k, "hidden")) { /* [hidden] обрабатывает UA-стиль через attrs */ }
    }
    if (n->attrs[0] == 0 && apos == 0) n->attrs[0] = 0;
    if (is_cell) {                                            /* cellpadding / border таблицы -> ячейкам */
        int t = n->parent; char v[40];
        while (t >= 0 && strcmp(pb->v[t]->tag, "table") != 0) t = pb->v[t]->parent;
        if (t >= 0) {
            if (webpage_attr_get(pb->v[t], "cellpadding", v, sizeof v)) { char b[48]; snprintf(b, sizeof b, "padding:%dpx", atoi(v)); pres_add(n, b); }
            if (webpage_attr_get(pb->v[t], "border", v, sizeof v) && atoi(v) > 0) pres_add(n, "border:1px solid #808080");
        }
    }
    if (n->type == TAG_INPUT && !n->input_type[0]) strcpy(n->input_type, "text");
}

/* ══════════════ дерево ══════════════ */
static int cur_parent(PB* pb) { return pb->mode_head ? pb->head : (pb->sp ? pb->stack[pb->sp - 1] : pb->body); }

static void ensure_body(PB* pb) {
    if (!pb->mode_head) return;
    pb->mode_head = 0;
    if (pb->body < 0) { DOMNode* b = mk(pb, 0, "body"); pb->body = b ? b->idx : 0; }
    pb->sp = 0; pb->stack[pb->sp++] = pb->body;
}

static int stack_find(PB* pb, const char* tag, const char* stop_list) {
    for (int i = pb->sp - 1; i >= 0; i--) {
        const char* t = pb->v[pb->stack[i]]->tag;
        if (!strcmp(t, tag)) return i;
        if (stop_list && in_list(stop_list, t)) return -1;
    }
    return -1;
}
static void pop_to(PB* pb, int i) { if (i >= 1) pb->sp = i; }                 /* закрывает элемент i и всё, что выше */

static void add_text(PB* pb, const char* s, size_t n, int rcdata_entities, DOMNode* target) {
    if (n == 0) return;
    char* buf = malloc(n + 1); if (!buf) return;
    size_t len = rcdata_entities ? decode_entities(s, n, buf, 0) : (memcpy(buf, s, n), buf[n] = 0, n);
    size_t st = 0;
    if (pb->skip_nl) { pb->skip_nl = 0; if (buf[0] == '\n') st = 1; else if (buf[0] == '\r' && buf[1] == '\n') st = 2; }
    int allws = 1; for (size_t i = st; i < len; i++) if (!is_ws((unsigned char)buf[i])) { allws = 0; break; }
    if (len <= st) { free(buf); return; }
    if (!target) {
        if (pb->mode_head) { if (allws) { free(buf); return; } ensure_body(pb); }
        int par = cur_parent(pb);
        if (allws && in_list(NO_WS_TEXT, pb->v[par]->tag)) { free(buf); return; }
        /* внутри pre/textarea пробелы значимы; иначе разбиваем длинный текст на узлы <= 500 байт */
        size_t pos = st;
        while (pos < len) {
            size_t take = len - pos;
            if (take > 500) { take = 500; while (take > 100 && !is_ws((unsigned char)buf[pos + take])) take--; while (take > 1 && ((unsigned char)buf[pos + take] & 0xC0) == 0x80) take--; }
            DOMNode* t = mk(pb, par, "#text"); if (!t) break;
            t->type = TAG_TEXT; put_str(t->text, sizeof t->text, buf + pos, take);
            pos += take;
        }
    } else {
        put_str(target->text, sizeof target->text, buf + st, len - st);
    }
    free(buf);
}

static void close_p_if_open(PB* pb) {
    int i = stack_find(pb, "p", SCOPE_STOP);
    if (i >= 0) pop_to(pb, i);
}

/* Встроенный <svg>: разбор HTML содержимое выбрасывает, поэтому собираем разметку заново (корневой тег по атрибутам + тело),
   кладём в файл кэша по хэшу содержимого и записываем путь в n->src — дальше узел обрабатывается как заменяемый элемент-картинка
   (растеризация в images.c/svg.c; цвет currentColor берётся из CSS `color` элемента в момент отрисовки). */
static int mem_has_ci(const char* s, size_t len, const char* needle) {         /* поиск без учёта регистра в пределах len байт */
    size_t nl = strlen(needle); if (nl > len) return 0;
    for (size_t i = 0; i + nl <= len; i++) if (!strncasecmp(s + i, needle, nl)) return 1;
    return 0;
}
static void svg_capture(const char* base, DOMNode* n, const Attr* a, int na, const char* inner, size_t len) {
    if (len == 0 || len > (4u << 20)) return;
    size_t cap = len + 64; for (int i = 0; i < na; i++) cap += strlen(a[i].name) + (a[i].val ? strlen(a[i].val) * 6 : 0) + 8;
    char* m = malloc(cap); if (!m) return; size_t o = 0;
    /* спрайты: хэш origin страницы связывает svg с <use> и svg с <symbol> */
    char oh[24]; { const char* q = base ? strstr(base, "://") : NULL; size_t ol = q ? (size_t)(strchr(q + 3, '/') ? strchr(q + 3, '/') - base : (long)strlen(base)) : (base ? strlen(base) : 0);
        unsigned long long hh = 1469598103934665603ULL; for (size_t i = 0; i < ol && base; i++) { hh ^= (unsigned char)base[i]; hh *= 1099511628211ULL; } snprintf(oh, sizeof oh, "%016llx", hh); }
    int has_symbol = mem_has_ci(inner, len, "<symbol"), has_use = mem_has_ci(inner, len, "<use");
    o += (size_t)snprintf(m + o, cap - o, "<!--mlinline-->");
    if (has_use) o += (size_t)snprintf(m + o, cap - o, "<!--mlsprite:%s-->", oh);
    o += (size_t)snprintf(m + o, cap - o, "<svg");
    for (int i = 0; i < na; i++) {
        o += (size_t)snprintf(m + o, cap - o, " %s=\"", a[i].name);
        for (const char* v = a[i].val ? a[i].val : ""; *v && o + 8 < cap; v++) { if (*v == '"') { memcpy(m + o, "&quot;", 6); o += 6; } else m[o++] = *v; }
        m[o++] = '"';
    }
    m[o++] = '>'; memcpy(m + o, inner, len); o += len; memcpy(m + o, "</svg>", 6); o += 6; m[o] = 0;
    unsigned long long h = 1469598103934665603ULL; for (size_t i = 0; i < o; i++) { h ^= (unsigned char)m[i]; h *= 1099511628211ULL; }
    char path[64]; snprintf(path, sizeof path, "cache/svg_%016llx.svg", h);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0) { mkdir("cache", 0777); FILE* f = fopen(path, "wb"); if (f) { fwrite(m, 1, o, f); fclose(f); } }
    if (has_symbol) {                                                  /* спрайт-лист: запоминаем отдельно, чтобы <use href="#id"> из других svg находили символы */
        char sp[96]; snprintf(sp, sizeof sp, "cache/sprite_%s_%016llx.svg", oh, h);
        if (stat(sp, &st) != 0 || st.st_size == 0) { FILE* f = fopen(sp, "wb"); if (f) { fwrite(m, 1, o, f); fclose(f); } }
    }
    put_str(n->src, sizeof n->src, path, strlen(path)); free(m);
}

static void start_tag(PB* pb, const char* name, Attr* a, int na, int selfclose, const char** pp) {
    DOMNode* n;
    if (!strcmp(name, "html")) { n = pb->v[0]; apply_attrs(pb, n, a, na); return; }
    if (!strcmp(name, "head")) return;
    if (!strcmp(name, "body")) {
        int fresh = pb->body < 0; ensure_body(pb);
        n = pb->v[pb->body]; if (fresh || !n->class_name[0]) apply_attrs(pb, n, a, na);
        return;
    }
    if (pb->mode_head && !in_list(HEAD_TAGS, name)) ensure_body(pb);

    if (!pb->mode_head) {                                   /* неявные закрытия */
        if (in_list(P_CLOSERS, name)) close_p_if_open(pb);
        if (!strcmp(name, "li")) { int i = stack_find(pb, "li", " ul ol menu html "); if (i >= 0) pop_to(pb, i); }
        else if (!strcmp(name, "dd") || !strcmp(name, "dt")) { int i = stack_find(pb, "dd", " dl html "); if (i < 0) i = stack_find(pb, "dt", " dl html "); if (i >= 0) pop_to(pb, i); }
        else if (!strcmp(name, "option")) { int i = stack_find(pb, "option", " select datalist html "); if (i >= 0) pop_to(pb, i); }
        else if (!strcmp(name, "optgroup")) { int i = stack_find(pb, "option", " select html "); if (i >= 0) pop_to(pb, i); i = stack_find(pb, "optgroup", " select html "); if (i >= 0) pop_to(pb, i); }
        else if (!strcmp(name, "td") || !strcmp(name, "th")) { int i = stack_find(pb, "td", " table tr html "); if (i < 0) i = stack_find(pb, "th", " table tr html "); if (i >= 0) pop_to(pb, i); }
        else if (!strcmp(name, "tr")) { int i = stack_find(pb, "tr", " table html "); if (i >= 0) pop_to(pb, i); }
        else if (!strcmp(name, "thead") || !strcmp(name, "tbody") || !strcmp(name, "tfoot")) { for (int k = 0; k < 3; k++) { int i = stack_find(pb, k == 0 ? "tr" : k == 1 ? "tbody" : "thead", " table html "); if (i >= 0) pop_to(pb, i); } }
        else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) { const char* t = pb->v[cur_parent(pb)]->tag; if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) pop_to(pb, pb->sp - 1); }
        else if (!strcmp(name, "a")) { int i = stack_find(pb, "a", " td th table html button "); if (i >= 0) pop_to(pb, i); }
        else if (!strcmp(name, "button")) { int i = stack_find(pb, "button", " td th table html "); if (i >= 0) pop_to(pb, i); }
    }

    int parent = cur_parent(pb);
    n = mk(pb, parent, name); if (!n) return;
    apply_attrs(pb, n, a, na);
    int is_void = in_list(VOID_TAGS, name);

    if (!strcmp(name, "base") && n->href[0]) put_str(pb->base, sizeof pb->base, n->href, strlen(n->href));
    if (!strcmp(name, "link")) {
        char rel[64] = "", media[96] = "";
        webpage_attr_get(n, "rel", rel, sizeof rel); webpage_attr_get(n, "media", media, sizeof media);
        if (n->href[0] && find_ci(rel, "stylesheet") && !find_ci(rel, "alternate")) add_sheet(pb, NULL, strdup(n->href), media);
    }
    if (!strcmp(name, "br") || !strcmp(name, "wbr")) { /* пустые */ }

    if (is_void) return;
    /* содержимое, которое не разбирается как HTML */
    const char* p = *pp;
    int raw = in_list(RAW_TAGS, name), rc = in_list(RCDATA_TAGS, name);
    if (raw || rc || in_list(SKIP_TAGS, name) || !strcmp(name, "svg")) {
        char close[24]; snprintf(close, sizeof close, "</%s", name);
        const char* e = NULL;
        if (!strcmp(name, "svg")) {                          /* учитываем вложенные <svg> */
            int depth = 1; const char* q = p;
            while (depth > 0) {
                const char* o = find_ci(q, "<svg"); const char* c = find_ci(q, "</svg");
                if (!c) { e = NULL; break; }
                if (o && o < c && (is_ws(o[4]) || o[4] == '>')) { depth++; q = o + 4; } else { depth--; e = c; q = c + 5; }
            }
        } else e = find_ci(p, close);
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (!strcmp(name, "style") && len > 0) {
            char media[96] = ""; webpage_attr_get(n, "media", media, sizeof media);
            char* css = malloc(len + 1); if (css) { memcpy(css, p, len); css[len] = 0; add_sheet(pb, css, NULL, media); }
        } else if (!strcmp(name, "script")) {
            char ty[64] = "", nm[8] = ""; webpage_attr_get(n, "type", ty, sizeof ty); int nomod = webpage_attr_get(n, "nomodule", nm, sizeof nm);
            int is_js = !nomod && (!ty[0] || find_ci(ty, "javascript") || find_ci(ty, "ecmascript") || !strcasecmp(ty, "module"));
            if (is_js && (len > 0 || n->src[0])) {
                char* text = NULL; if (!n->src[0] && len > 0) { text = malloc(len + 1); if (text) { memcpy(text, p, len); text[len] = 0; } }
                char t2[8], t3[8], t4[8]; bundle_add_script(text, n->src[0] ? strdup(n->src) : NULL, !strcasecmp(ty, "module"), webpage_attr_get(n, "async", t2, sizeof t2), webpage_attr_get(n, "defer", t3, sizeof t3), n->idx); (void)t4;
            }
            if (len > 0 && len < 511) put_str(n->text, sizeof n->text, p, len);   /* для старого кода; движок JS берёт полный текст из бандла */
        } else if (!strcmp(name, "svg")) {
            svg_capture(pb->base, n, a, na, p, len);
        } else if (rc) {
            char* tmp = malloc(len + 1);
            if (tmp) {
                size_t dl = decode_entities(p, len, tmp, 0); size_t st = 0;
                if (!strcmp(name, "textarea") && tmp[0] == '\n') st = 1;
                put_str(n->value, sizeof n->value, tmp + st, dl - st); put_str(n->text, sizeof n->text, tmp + st, dl - st);
                free(tmp);
            }
        }
        if (e) { const char* gt = strchr(e, '>'); p = gt ? gt + 1 : e + strlen(e); } else p = p + len;
        *pp = p;
        return;
    }
    if (selfclose && pb->mode_head == 0 && 0) return;
    if (pb->sp < 250) pb->stack[pb->sp++] = n->idx;
    if (!strcmp(name, "pre") || !strcmp(name, "listing")) pb->skip_nl = 1;
}

static void end_tag(PB* pb, const char* name) {
    if (!strcmp(name, "html") || !strcmp(name, "body") || !strcmp(name, "head")) return;
    if (!strcmp(name, "br")) { Attr none[1]; const char* nul = ""; start_tag(pb, "br", none, 0, 1, &nul); return; }
    if (pb->mode_head) return;
    const char* stop = SCOPE_STOP;
    if (!strcmp(name, "table")) stop = " html ";
    else if (!strcmp(name, "td") || !strcmp(name, "th") || !strcmp(name, "tr") || !strcmp(name, "tbody") || !strcmp(name, "thead") || !strcmp(name, "tfoot") || !strcmp(name, "caption")) stop = " table html ";
    else if (in_list(" div span a b i u em strong small font section article nav header footer main ul ol li p label form ", name)) stop = " td th table caption html ";
    int i = stack_find(pb, name, stop);
    if (i >= 0) pop_to(pb, i);
    else if (!strcmp(name, "form") || !strcmp(name, "p")) { /* пустой </p> игнорируем */ }
}

DOMNode* html_build(const char* html, const char* base_url, SheetList* sheets) {
    PB pb; memset(&pb, 0, sizeof pb);
    pb.sheets = sheets; pb.head = pb.body = -1; pb.mode_head = 1;
    put_str(pb.base, sizeof pb.base, base_url ? base_url : "", base_url ? strlen(base_url) : 0);
    webpage_set_doc_url(base_url);
    DOMNode* root = mk(&pb, -1, "html"); if (!root) return NULL;
    DOMNode* hd = mk(&pb, 0, "head"); pb.head = hd ? hd->idx : 0;

    const char* p = html;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;   /* BOM */
    while (*p) {
        if (*p != '<') {
            const char* e = strchr(p, '<'); size_t n = e ? (size_t)(e - p) : strlen(p);
            add_text(&pb, p, n, 1, NULL); p += n; continue;
        }
        if (!strncmp(p, "<!--", 4)) { const char* e = strstr(p + 4, "-->"); p = e ? e + 3 : p + strlen(p); continue; }
        if (!strncmp(p, "<![CDATA[", 9)) { const char* e = strstr(p, "]]>"); p = e ? e + 3 : p + strlen(p); continue; }
        if (p[1] == '!' || p[1] == '?') { const char* e = strchr(p, '>'); p = e ? e + 1 : p + strlen(p); continue; }
        if (p[1] == '/') {
            if (isalpha((unsigned char)p[2])) {
                char name[32]; size_t k = 0; const char* q = p + 2;
                while (*q && !is_ws(*q) && *q != '>' && *q != '/' && k < 31) name[k++] = (char)tolower((unsigned char)*q++);
                name[k] = 0; end_tag(&pb, name);
            }
            const char* e = strchr(p, '>'); p = e ? e + 1 : p + strlen(p); continue;
        }
        if (isalpha((unsigned char)p[1])) {
            char name[32]; size_t k = 0; const char* q = p + 1;
            while (*q && !is_ws(*q) && *q != '>' && *q != '/' && k < 31) name[k++] = (char)tolower((unsigned char)*q++);
            name[k] = 0;
            Attr attrs[48]; int na = 0; int selfclose = 0;
            while (*q && *q != '>') {
                while (is_ws(*q)) q++;
                if (*q == '/') { selfclose = 1; q++; continue; }
                if (*q == '>' || !*q) break;
                selfclose = 0;
                char an[32]; size_t al = 0;
                while (*q && !is_ws(*q) && *q != '=' && *q != '>' && !(*q == '/' && q[1] == '>')) { if (al < 31) an[al++] = (char)tolower((unsigned char)*q); q++; }
                an[al] = 0;
                while (is_ws(*q)) q++;
                char* val = NULL;
                if (*q == '=') {
                    q++; while (is_ws(*q)) q++;
                    const char *vs, *ve;
                    if (*q == '"' || *q == '\'') { char qc = *q++; vs = q; while (*q && *q != qc) q++; ve = q; if (*q) q++; }
                    else { vs = q; while (*q && !is_ws(*q) && *q != '>') q++; ve = q; }
                    val = malloc((size_t)(ve - vs) + 1);
                    if (val) decode_entities(vs, (size_t)(ve - vs), val, 1);
                }
                if (al && na < 48) { strcpy(attrs[na].name, an); attrs[na].val = val ? val : strdup(""); na++; } else free(val);
            }
            if (*q == '>') q++;
            const char* after = q;
            start_tag(&pb, name, attrs, na, selfclose, &after);
            for (int i = 0; i < na; i++) free(attrs[i].val);
            p = after; continue;
        }
        add_text(&pb, "<", 1, 0, NULL); p++;
    }
    ensure_body(&pb);

    /* связываем список, считаем subtree_end */
    for (int i = 0; i + 1 < pb.n; i++) pb.v[i]->next = pb.v[i + 1];
    for (int i = pb.n - 1; i > 0; i--) { int par = pb.v[i]->parent; if (par >= 0 && pb.v[i]->subtree_end > pb.v[par]->subtree_end) pb.v[par]->subtree_end = pb.v[i]->subtree_end; }
    DOMNode* head = pb.v[0];
    free(pb.v);
    return head;
}

/* URL текущего документа — для проверок безопасности ресурсов (смешанное содержимое). Выставляется при разборе (процесс загрузки),
   при каскаде стилей и при создании JS-страницы (основной процесс). */
static char g_doc_url[1100];
void webpage_set_doc_url(const char* u) { snprintf(g_doc_url, sizeof g_doc_url, "%s", u ? u : ""); }
int webpage_mixed_blocked(const char* url) {
    if (!nal_mixed_content_blocked(g_doc_url, url)) return 0;
    gal_log("WARN", "Mixed Content: заблокирован небезопасный ресурс %s на странице %s", url, g_doc_url); return 1;
}

/* Текст ресурса: http(s) через NAL, иначе локальный файл. */
char* webpage_fetch_text(const char* url) {
    if (!url) return NULL;
    if (webpage_mixed_blocked(url)) return NULL;
    if (ci_prefix(url, "http://") || ci_prefix(url, "https://")) return nal_http_get(url);
    const char* path = ci_prefix(url, "file://") ? url + 7 : url;
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 8 * 1024 * 1024) { fclose(f); return NULL; }
    char* b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f); b[got] = 0; fclose(f); return b;
}

PageBundle* webpage_bundle_take(void) {
    PageBundle* b = malloc(sizeof *b); if (!b) return NULL;
    *b = g_bundle; memset(&g_bundle, 0, sizeof g_bundle); return b;
}

/* Докачивает текст всех внешних <link rel=stylesheet> бандла ПРЯМО СЕЙЧАС (синхронно), пока
 * вызывающий код ещё в фоновом процессе (main.c форкает child на весь http(s)-запрос страницы).
 * Раньше это делалось лениво в style.c при каскаде — уже в родительском/UI-процессе, синхронными
 * curl-запросами один за другим. На страницах с несколькими внешними таблицами стилей (обычное
 * дело — Wikipedia грузит их минимум две) это блокировало интерфейс и, если что-то на середине
 * пути обрывалось (таймаут/медленное соединение), часть CSS просто не успевала долиться к моменту
 * отрисовки — получался наполовину стилизованный, "поломанный" рендер. Возвращает число реально
 * докачанных таблиц.
 */
int webpage_bundle_prefetch_css(void) {
    int got = 0, budget = 10;
    for (int i = 0; i < g_bundle.nsheets && budget > 0; i++) {
        SheetSrc* s = &g_bundle.sheets[i];
        if (!s->css && s->href && !strncmp(s->href, "http", 4) && !webpage_mixed_blocked(s->href)) {
            s->css = nal_http_get(s->href);
            budget--; if (s->css) got++;
        }
    }
    return got;
}

void webpage_bundle_free(PageBundle* b) {
    if (!b) return;
    for (int i = 0; i < b->nsheets; i++) { free(b->sheets[i].css); free(b->sheets[i].href); }
    for (int i = 0; i < b->nscripts; i++) { free(b->scripts[i].text); free(b->scripts[i].url); }
    free(b->sheets); free(b->scripts); free(b);
}

static void wr_str(FILE* f, const char* s) { uint32_t n = s ? (uint32_t)strlen(s) + 1 : 0; fwrite(&n, 4, 1, f); if (n) fwrite(s, 1, n, f); }
static char* rd_str(FILE* f) { uint32_t n; if (fread(&n, 4, 1, f) != 1 || n == 0 || n > 64u * 1024 * 1024) return NULL; char* s = malloc(n); if (!s) return NULL; if (fread(s, 1, n, f) != n) { free(s); return NULL; } s[n - 1] = 0; return s; }

int webpage_bundle_write(const char* path) {
    FILE* f = fopen(path, "wb"); if (!f) return 0;
    fwrite("MLB1", 1, 4, f); wr_str(f, g_bundle.base);
    fwrite(&g_bundle.nsheets, 4, 1, f);
    for (int i = 0; i < g_bundle.nsheets; i++) { SheetSrc* s = &g_bundle.sheets[i]; wr_str(f, s->css); wr_str(f, s->href); wr_str(f, s->media); }
    fwrite(&g_bundle.nscripts, 4, 1, f);
    for (int i = 0; i < g_bundle.nscripts; i++) { ScriptSrc* s = &g_bundle.scripts[i]; wr_str(f, s->text); wr_str(f, s->url); uint8_t fl[3] = {s->module, s->async_, s->defer_}; fwrite(fl, 1, 3, f); fwrite(&s->node_idx, 4, 1, f); }
    fclose(f); return 1;
}

PageBundle* webpage_bundle_read(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    char m[4]; PageBundle* b = calloc(1, sizeof *b); int ok = b && fread(m, 1, 4, f) == 4 && !memcmp(m, "MLB1", 4);
    if (ok) { char* base = rd_str(f); if (base) { snprintf(b->base, sizeof b->base, "%s", base); free(base); } ok = fread(&b->nsheets, 4, 1, f) == 1 && b->nsheets >= 0 && b->nsheets < 1000; }
    if (ok) { b->sheets = calloc((size_t)b->nsheets + 1, sizeof(SheetSrc)); for (int i = 0; ok && i < b->nsheets; i++) { b->sheets[i].css = rd_str(f); b->sheets[i].href = rd_str(f); char* md = rd_str(f); if (md) { snprintf(b->sheets[i].media, sizeof b->sheets[i].media, "%s", md); free(md); } } }
    if (ok) ok = fread(&b->nscripts, 4, 1, f) == 1 && b->nscripts >= 0 && b->nscripts < 1000;
    if (ok) { b->scripts = calloc((size_t)b->nscripts + 1, sizeof(ScriptSrc)); for (int i = 0; ok && i < b->nscripts; i++) { ScriptSrc* s = &b->scripts[i]; s->text = rd_str(f); s->url = rd_str(f); uint8_t fl[3]; ok = fread(fl, 1, 3, f) == 3 && fread(&s->node_idx, 4, 1, f) == 1; if (ok) { s->module = fl[0]; s->async_ = fl[1]; s->defer_ = fl[2]; } } }
    fclose(f); if (!ok) { webpage_bundle_free(b); return NULL; } return b;
}

DOMNode* parse_document(const char* html, const char* base_url) {
    if (!html) return NULL;
    bundle_reset();
    char* conv = convert_to_utf8(html);
    SheetList sheets = {0};
    DOMNode* head = html_build(conv ? conv : html, base_url, &sheets);
    if (head) {
        /* внешние скрипты скачиваем здесь (в дочернем процессе загрузки) — родитель исполняет без сети */
        int fetched = 0;
        for (int i = 0; i < g_bundle.nscripts; i++) {
            ScriptSrc* sc = &g_bundle.scripts[i];
            if (!sc->text && sc->url && fetched < 24) { fetched++; sc->text = webpage_fetch_text(sc->url); if (sc->text && strlen(sc->text) > 4u * 1024 * 1024) { free(sc->text); sc->text = NULL; } }
        }
        style_apply(head, &sheets, base_url);
        webpage_prefetch_images(head);
    }
    snprintf(g_bundle.base, sizeof g_bundle.base, "%s", base_url ? base_url : "");
    g_bundle.sheets = sheets.v; g_bundle.nsheets = sheets.n;      /* владение таблицами стилей переходит бандлу */
    free(conv);
    return head;
}
