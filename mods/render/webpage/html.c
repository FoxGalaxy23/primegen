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

/* URL resolution is implemented in url.c so it can be independently fuzzed. */

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
static const char* SCOPE_STOP  = " td th table caption html button object marquee ";

/* The compact renderer still stores the DOM as a preorder linked list, but
 * the tree builder now tracks HTML5 insertion modes and deferred sibling
 * placement.  Stable creation indices are used until the final flatten pass. */
typedef enum {
    IM_INITIAL = 0, IM_BEFORE_HTML, IM_BEFORE_HEAD, IM_IN_HEAD, IM_AFTER_HEAD,
    IM_IN_BODY, IM_IN_TABLE, IM_IN_TABLE_BODY, IM_IN_ROW, IM_IN_CELL,
    IM_IN_CAPTION, IM_IN_COLGROUP, IM_IN_SELECT
} HtmlInsertionMode;

/* ══════════════ бандл страницы ══════════════ */
static PageBundle g_bundle;
static void bundle_reset(void) {
    for (int i = 0; i < g_bundle.nsheets; i++) { free(g_bundle.sheets[i].css); free(g_bundle.sheets[i].href); }
    for (int i = 0; i < g_bundle.nscripts; i++) { free(g_bundle.scripts[i].text); free(g_bundle.scripts[i].url); free(g_bundle.scripts[i].nonce); }
    free(g_bundle.sheets); free(g_bundle.scripts); memset(&g_bundle, 0, sizeof g_bundle);
}
static void bundle_add_script(char* text, char* url, char* nonce, int module, int async_, int defer_, int idx) {
    ScriptSrc* nv = realloc(g_bundle.scripts, (size_t)(g_bundle.nscripts + 1) * sizeof(ScriptSrc));
    if (!nv) { free(text); free(url); free(nonce); return; }
    g_bundle.scripts = nv;
    ScriptSrc* sc = &g_bundle.scripts[g_bundle.nscripts++]; memset(sc, 0, sizeof *sc);
    sc->text = text; sc->url = url; sc->nonce = nonce; sc->module = (uint8_t)module; sc->async_ = (uint8_t)async_; sc->defer_ = (uint8_t)defer_; sc->node_idx = idx;
}

/* ══════════════ состояние сборки ══════════════ */
typedef struct {
    DOMNode** v; int n, cap;
    int stack[256]; int sp;              /* индексы открытых элементов */
    int head, body, mode_head;           /* mode_head: ещё не встретили содержимое body */
    HtmlInsertionMode insertion_mode;
    int formatting[128], nformatting;    /* active formatting elements */
    int foster_parenting;
    char base[1024];
    SheetList* sheets;
    int skip_nl;
} PB;

DOMNode* webpage_new_node(const char* tag) {
    DOMNode* n = calloc(1, sizeof(DOMNode)); if (!n) return NULL;
    n->v2 = 1; n->parent = -1; n->subtree_end = 0; n->html_before = -1; n->attr_w = n->attr_h = -1; n->colspan = n->rowspan = 1;
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

static DOMNode* mk_before(PB* pb, int parent, const char* tag, int before_idx) {
    DOMNode* n = mk(pb, parent, tag);
    if (n) n->html_before = before_idx;
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
    /* Responsive image attributes may exceed the compact selector-attribute table. */
    if (!strcmp(name, "srcset") && n->srcset[0]) { put_str(out, cap, n->srcset, strlen(n->srcset)); return 1; }
    if (!strcmp(name, "sizes") && n->sizes[0]) { put_str(out, cap, n->sizes, strlen(n->sizes)); return 1; }
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
        int blocked_inline_event = !strcmp(k, "onclick") && !websec_allows_inline(NAL_RESOURCE_SCRIPT, NULL);
        int blocked_inline_style = !strcmp(k, "style") && !websec_allows_inline(NAL_RESOURCE_STYLE, NULL);
        if (strcmp(k, "style") != 0 && strcmp(k, "class") != 0 && !blocked_inline_event && vl < 160 && strlen(k) < 32 &&
            apos + (int)strlen(k) + (int)vl + 3 < (int)sizeof n->attrs) {
            apos += snprintf(n->attrs + apos, sizeof n->attrs - apos, "%s%c%s%c", k, 1, v, 2);
        }
        if (!strcmp(k, "id")) put_str(n->id, sizeof n->id, v, vl);
        else if (!strcmp(k, "class")) put_str(n->class_name, sizeof n->class_name, v, vl);
        else if (!strcmp(k, "style")) { if (!blocked_inline_style) put_str(n->style_str, sizeof n->style_str, v, vl); }
        else if (!strcmp(k, "onclick")) { if (!blocked_inline_event) put_str(n->onclick, sizeof n->onclick, v, vl); }
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
        else if (!strcmp(k, "srcset") && (!strcmp(tag, "img") || !strcmp(tag, "source")))
            put_str(n->srcset, sizeof n->srcset, v, vl);
        else if (!strcmp(k, "sizes") && (!strcmp(tag, "img") || !strcmp(tag, "source")))
            put_str(n->sizes, sizeof n->sizes, v, vl);
        /* презентационные атрибуты */
        else if (!strcmp(k, "width")) { if (!is_cell || 1) pres_dim(n, "width", v); if (isdigit((unsigned char)*v) && !strchr(v, '%')) n->attr_w = atoi(v); }
        else if (!strcmp(k, "height")) { pres_dim(n, "height", v); if (isdigit((unsigned char)*v) && !strchr(v, '%')) n->attr_h = atoi(v); }
        else if (!strcmp(k, "bgcolor")) pres_color(n, "background-color", v);
        else if (!strcmp(k, "text") && !strcmp(tag, "body")) pres_color(n, "color", v);
        else if (!strcmp(k, "color") && !strcmp(tag, "font")) pres_color(n, "color", v);
        else if (!strcmp(k, "face") && !strcmp(tag, "font")) { char b[160]; snprintf(b, sizeof b, "font-family:%s", v); pres_add(n, b); }
        else if (!strcmp(k, "size") && !strcmp(tag, "font")) {
            static const int fs[8] = {10, 10, 13, 16, 18, 24, 32, 48}; int s = atoi(v); if (v[0] == '+' || v[0] == '-') s += 3;
            if (s < 1) s = 1;
            if (s > 7) s = 7;
            char b[40]; snprintf(b, sizeof b, "font-size:%dpx", fs[s]); pres_add(n, b);
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
        else if (!strcmp(k, "dir")) {
            /* HTML direction is a presentational hint; authored CSS direction
             * still wins in the normal cascade. dir=auto is resolved separately
             * by the UA in full engines and must not be mistaken for ltr here. */
            if (!strcasecmp(v, "rtl")) pres_add(n, "direction:rtl");
            else if (!strcasecmp(v, "ltr")) pres_add(n, "direction:ltr");
        }
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
    pb->insertion_mode = IM_IN_BODY;
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

static int stack_index_for_node(PB* pb, int node_idx) {
    for (int i = pb->sp - 1; i >= 0; --i) if (pb->stack[i] == node_idx) return i;
    return -1;
}

static void stack_remove_at(PB* pb, int at) {
    if (at < 0 || at >= pb->sp || at == 0) return;
    memmove(&pb->stack[at], &pb->stack[at + 1], (size_t)(pb->sp - at - 1) * sizeof pb->stack[0]);
    pb->sp--;
}

static HtmlInsertionMode reset_insertion_mode(PB* pb) {
    if (pb->mode_head) return pb->head >= 0 ? IM_IN_HEAD : IM_BEFORE_HEAD;
    for (int i = pb->sp - 1; i >= 0; --i) {
        const char* t = pb->v[pb->stack[i]]->tag;
        if (!strcmp(t, "select")) return IM_IN_SELECT;
        if (!strcmp(t, "td") || !strcmp(t, "th")) return IM_IN_CELL;
        if (!strcmp(t, "tr")) return IM_IN_ROW;
        if (!strcmp(t, "tbody") || !strcmp(t, "thead") || !strcmp(t, "tfoot")) return IM_IN_TABLE_BODY;
        if (!strcmp(t, "caption")) return IM_IN_CAPTION;
        if (!strcmp(t, "colgroup")) return IM_IN_COLGROUP;
        if (!strcmp(t, "table")) return IM_IN_TABLE;
        if (!strcmp(t, "head")) return IM_IN_HEAD;
        if (!strcmp(t, "body")) return IM_IN_BODY;
        if (!strcmp(t, "html")) return IM_AFTER_HEAD;
    }
    return IM_IN_BODY;
}

static int is_formatting_tag(const char* tag) {
    return in_list(" a b big code em font i s small strike strong tt u ", tag);
}

static int active_formatting_index(PB* pb, const char* tag) {
    /* A marker partitions the list. Elements before the most recent marker
     * must not participate in reconstruction/adoption for the current scope. */
    for (int i = pb->nformatting - 1; i >= 0; --i) {
        int id = pb->formatting[i];
        if (id < 0) break;
        if (id < pb->n && !strcmp(pb->v[id]->tag, tag)) return i;
    }
    return -1;
}

static int active_formatting_node_index(PB* pb, int node_idx) {
    for (int i = pb->nformatting - 1; i >= 0; --i) {
        if (pb->formatting[i] < 0) break;
        if (pb->formatting[i] == node_idx) return i;
    }
    return -1;
}

static void active_formatting_remove(PB* pb, int at) {
    if (at < 0 || at >= pb->nformatting) return;
    memmove(&pb->formatting[at], &pb->formatting[at + 1], (size_t)(pb->nformatting - at - 1) * sizeof pb->formatting[0]);
    pb->nformatting--;
}

static void active_formatting_marker(PB* pb) {
    if (pb->nformatting < (int)(sizeof pb->formatting / sizeof pb->formatting[0]))
        pb->formatting[pb->nformatting++] = -1;
}

static void clear_active_formatting_to_marker(PB* pb) {
    for (int i = pb->nformatting - 1; i >= 0; --i) {
        if (pb->formatting[i] == -1) {
            /* The marker itself and all entries after it are discarded. */
            pb->nformatting = i;
            return;
        }
    }
    pb->nformatting = 0;
}

static int is_special_html_element(const char* tag) {
    return in_list(" address applet area article aside base basefont bgsound blockquote body br button caption center col colgroup dd details dir div dl dt embed fieldset figcaption figure footer form frame frameset h1 h2 h3 h4 h5 h6 head header hgroup hr html iframe img input li link listing main marquee menu meta nav noembed noframes noscript object ol optgroup option p param plaintext pre script search section select source style summary table tbody td template textarea tfoot th thead title tr track ul wbr xmp ", tag);
}

static int is_table_stack_tag(const char* tag) {
    return in_list(" table caption colgroup tbody thead tfoot tr td th ", tag);
}

/* Return the table whose in-table insertion mode is active, or -1 when the
 * current node is ordinary body content (for example a foster-parented div). */
static int table_insertion_target(PB* pb) {
    if (pb->mode_head || pb->sp <= 0) return -1;
    const char* top = pb->v[pb->stack[pb->sp - 1]]->tag;
    if (!is_table_stack_tag(top)) return -1;
    int table = -1;
    for (int i = pb->sp - 1; i >= 0; --i) {
        const char* t = pb->v[pb->stack[i]]->tag;
        if (!strcmp(t, "td") || !strcmp(t, "th") || !strcmp(t, "caption")) return -1;
        if (!strcmp(t, "table")) { table = pb->stack[i]; break; }
    }
    return table;
}

static DOMNode* clone_element(PB* pb, int old_idx, int parent) {
    if (old_idx < 0 || old_idx >= pb->n) return NULL;
    DOMNode* old = pb->v[old_idx];
    DOMNode* n = mk(pb, parent, old->tag);
    if (!n) return NULL;
    int idx = n->idx;
    memcpy(n, old, sizeof *n);
    n->idx = idx; n->parent = parent; n->subtree_end = idx; n->html_before = -1; n->next = NULL;
    n->aux = NULL; n->pb[0] = n->pb[1] = n->pb[2] = NULL;
    return n;
}

/* Recreate formatting elements that remain active after another formatting
 * element was closed. This is the step missing from the old stack-only parser. */
static void reconstruct_active_formatting(PB* pb) {
    if (pb->nformatting <= 0 || pb->mode_head) return;
    int marker = -1;
    for (int i = pb->nformatting - 1; i >= 0; --i) {
        if (pb->formatting[i] < 0) { marker = i; break; }
    }
    int first = pb->nformatting - 1;
    if (first <= marker) return;
    if (stack_index_for_node(pb, pb->formatting[first]) >= 0) return;
    while (first > marker + 1 && stack_index_for_node(pb, pb->formatting[first - 1]) < 0) first--;
    for (int i = first; i < pb->nformatting; ++i) {
        if (pb->formatting[i] < 0) break;
        if (pb->sp >= 250) return;
        DOMNode* n = clone_element(pb, pb->formatting[i], cur_parent(pb));
        if (!n) return;
        pb->formatting[i] = n->idx;
        pb->stack[pb->sp++] = n->idx;
    }
}

/* HTML5 adoption-agency algorithm. This implementation preserves stable node
 * ids while the stack/list entries are replaced, then flatten_html_tree()
 * materializes the repaired hierarchy as the renderer's preorder list. */
static void adoption_agency(PB* pb, const char* tag) {
    for (int outer = 0; outer < 8; ++outer) {
        int af = active_formatting_index(pb, tag);
        if (af < 0) return;
        int formatting = pb->formatting[af];
        int fp = stack_index_for_node(pb, formatting);
        if (fp < 0) {
            active_formatting_remove(pb, af);
            return;
        }
        if (stack_find(pb, tag, SCOPE_STOP) < 0) return;

        /* The furthest block is the first special element encountered when
         * walking upward from the formatting element through stack order. */
        int furthest_pos = -1;
        for (int i = fp + 1; i < pb->sp; ++i) {
            if (is_special_html_element(pb->v[pb->stack[i]]->tag)) {
                furthest_pos = i;
                break;
            }
        }
        if (furthest_pos < 0) {
            pb->sp = fp; /* pop formatting element and every node above it */
            af = active_formatting_node_index(pb, formatting);
            active_formatting_remove(pb, af);
            return;
        }

        int furthest = pb->stack[furthest_pos];
        int common_ancestor = fp > 0 ? pb->stack[fp - 1] : pb->body;
        int last_node = furthest;
        int node = furthest;
        int bookmark = af;
        int inner = 0;
        for (;;) {
            inner++;
            int node_pos = stack_index_for_node(pb, node);
            if (node_pos <= fp) break;
            int previous = pb->stack[node_pos - 1];
            node = previous;
            if (node == formatting) break;

            int ai = active_formatting_node_index(pb, node);
            if (inner > 3 && ai >= 0) {
                if (ai < bookmark) bookmark--;
                active_formatting_remove(pb, ai);
                ai = -1;
            }
            if (ai < 0) {
                int remove_pos = stack_index_for_node(pb, node);
                if (remove_pos >= 0) stack_remove_at(pb, remove_pos);
                continue;
            }

            DOMNode* clone = clone_element(pb, node, -1);
            if (!clone) return;
            int node_stack_pos = stack_index_for_node(pb, node);
            if (node_stack_pos >= 0) pb->stack[node_stack_pos] = clone->idx;
            pb->formatting[ai] = clone->idx;
            if (last_node == furthest) bookmark = ai + 1;
            pb->v[last_node]->parent = clone->idx;
            pb->v[last_node]->html_before = -1;
            last_node = clone->idx;
            node = clone->idx;
        }

        /* Insert lastNode under the common ancestor. For table-context
         * ancestors, the spec's foster-parent location is immediately before
         * the table element. */
        if (common_ancestor >= 0 && common_ancestor < pb->n &&
            in_list(" table tbody tfoot thead tr ", pb->v[common_ancestor]->tag)) {
            int table = -1;
            for (int i = pb->sp - 1; i >= 0; --i) {
                if (!strcmp(pb->v[pb->stack[i]]->tag, "table")) { table = pb->stack[i]; break; }
            }
            if (table >= 0) {
                pb->v[last_node]->parent = pb->v[table]->parent;
                pb->v[last_node]->html_before = table;
            } else pb->v[last_node]->parent = common_ancestor;
        } else if (common_ancestor >= 0 && common_ancestor < pb->n) {
            pb->v[last_node]->parent = common_ancestor;
            pb->v[last_node]->html_before = -1;
        }

        DOMNode* replacement = clone_element(pb, formatting, -1);
        if (!replacement) return;
        for (int i = 0; i < pb->n; ++i) {
            if (i != replacement->idx && pb->v[i]->parent == furthest) {
                pb->v[i]->parent = replacement->idx;
                pb->v[i]->html_before = -1;
            }
        }
        replacement->parent = furthest;

        af = active_formatting_node_index(pb, formatting);
        if (af >= 0) {
            active_formatting_remove(pb, af);
            if (af < bookmark) bookmark--;
            if (bookmark < 0) bookmark = 0;
            if (bookmark > pb->nformatting) bookmark = pb->nformatting;
            if (pb->nformatting < (int)(sizeof pb->formatting / sizeof pb->formatting[0])) {
                memmove(&pb->formatting[bookmark + 1], &pb->formatting[bookmark],
                    (size_t)(pb->nformatting - bookmark) * sizeof pb->formatting[0]);
                pb->formatting[bookmark] = replacement->idx;
                pb->nformatting++;
            }
        }

        int old_pos = stack_index_for_node(pb, formatting);
        if (old_pos >= 0) stack_remove_at(pb, old_pos);
        furthest_pos = stack_index_for_node(pb, furthest);
        if (furthest_pos >= 0 && pb->sp < 250) {
            /* Stack positions run from root to current node. Insert the clone
             * immediately below the furthest block so future tokens are not
             * accidentally nested inside the reconstructed formatting node. */
            memmove(&pb->stack[furthest_pos + 1], &pb->stack[furthest_pos],
                (size_t)(pb->sp - furthest_pos) * sizeof pb->stack[0]);
            pb->stack[furthest_pos] = replacement->idx;
            pb->sp++;
        }
        return;
    }
}

static void push_implied_element(PB* pb, const char* tag) {
    if (pb->sp >= 250) return;
    DOMNode* n = mk(pb, cur_parent(pb), tag);
    if (n) pb->stack[pb->sp++] = n->idx;
}

static void close_open_table_row(PB* pb) {
    int i = stack_find(pb, "tr", " table html ");
    if (i >= 0) pb->sp = i;
}

static void close_open_table_section(PB* pb) {
    int i = -1;
    for (int k = pb->sp - 1; k >= 0; --k) {
        const char* t = pb->v[pb->stack[k]]->tag;
        if (!strcmp(t, "tbody") || !strcmp(t, "thead") || !strcmp(t, "tfoot")) { i = k; break; }
        if (!strcmp(t, "table") || !strcmp(t, "html")) break;
    }
    if (i >= 0) pb->sp = i;
}

/* A small but explicit subset of the in-table/in-table-body/in-row insertion
 * modes. It handles implied tbody/tr/colgroup and sends invalid table content
 * through foster parenting instead of making it a child of <table>. */
static int prepare_table_start(PB* pb, const char* name, Attr* a, int na, int* foster_before) {
    HtmlInsertionMode mode = reset_insertion_mode(pb);
    int table = table_insertion_target(pb);
    *foster_before = -1;
    if (table < 0) return 0;

    if (!strcmp(name, "table")) {
        int i = stack_find(pb, "table", " html ");
        if (i >= 0) pb->sp = i;
        return 0;
    }

    if (!strcmp(name, "tbody") || !strcmp(name, "thead") || !strcmp(name, "tfoot") ||
        !strcmp(name, "caption") || !strcmp(name, "colgroup")) {
        if (mode == IM_IN_ROW) close_open_table_row(pb);
        if (mode == IM_IN_TABLE_BODY || mode == IM_IN_ROW) close_open_table_section(pb);
        return 0;
    }

    if (!strcmp(name, "tr")) {
        if (mode == IM_IN_ROW) close_open_table_row(pb);
        else if (mode == IM_IN_TABLE) push_implied_element(pb, "tbody");
        return 0;
    }

    if (!strcmp(name, "td") || !strcmp(name, "th")) {
        if (mode == IM_IN_CELL) {
            int i = stack_find(pb, "td", " table html ");
            if (i < 0) i = stack_find(pb, "th", " table html ");
            if (i >= 0) pb->sp = i;
            mode = reset_insertion_mode(pb);
        }
        if (mode == IM_IN_TABLE) { push_implied_element(pb, "tbody"); mode = IM_IN_TABLE_BODY; }
        if (mode == IM_IN_TABLE_BODY) push_implied_element(pb, "tr");
        return 0;
    }

    if (!strcmp(name, "col")) {
        if (mode == IM_IN_TABLE) push_implied_element(pb, "colgroup");
        else if (mode != IM_IN_COLGROUP) { *foster_before = table; return 1; }
        return 0;
    }

    if (in_list(" style script template ", name)) return 0;
    /* `input type=hidden` is a special in-table token in HTML5. It is kept
     * inside the table; all other non-structural tokens are foster-parented. */
    if (!strcmp(name, "input")) {
        for (int i = 0; i < na; ++i) {
            if (!strcmp(a[i].name, "type") && a[i].val && !strcasecmp(a[i].val, "hidden")) return 0;
        }
    }
    *foster_before = table;
    return 1;
}

/* These table tokens close the current cell/caption and are then reprocessed
 * in the newly exposed insertion mode, as required by the HTML tree builder. */
static void prepare_table_context_for_start(PB* pb, const char* name) {
    HtmlInsertionMode mode = reset_insertion_mode(pb);
    if (mode == IM_IN_CELL && in_list(" caption col colgroup tbody td tfoot th thead tr ", name)) {
        int i = stack_find(pb, "td", " table html ");
        if (i < 0) i = stack_find(pb, "th", " table html ");
        if (i >= 0) {
            clear_active_formatting_to_marker(pb);
            pb->sp = i; /* closes the cell and anything above it */
        }
        mode = reset_insertion_mode(pb);
    }
    if (mode == IM_IN_CAPTION && in_list(" caption col colgroup tbody td tfoot th thead tr table ", name)) {
        int i = stack_find(pb, "caption", " table html ");
        if (i >= 0) {
            clear_active_formatting_to_marker(pb);
            pb->sp = i;
        }
        mode = reset_insertion_mode(pb);
    }
    if (mode == IM_IN_COLGROUP && strcmp(name, "col") != 0 &&
        !in_list(" html head body script style template ", name)) {
        int i = stack_find(pb, "colgroup", " table html ");
        if (i >= 0) pb->sp = i;
    }
}

static int open_cell_stack_index(PB* pb) {
    for (int i = pb->sp - 1; i >= 0; --i) {
        const char* tag = pb->v[pb->stack[i]]->tag;
        if (!strcmp(tag, "td") || !strcmp(tag, "th")) return i;
        if (!strcmp(tag, "table") || !strcmp(tag, "html")) break;
    }
    return -1;
}

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
        int foster_table = allws ? -1 : table_insertion_target(pb);
        if (foster_table < 0) reconstruct_active_formatting(pb);
        int par = foster_table >= 0 ? pb->v[foster_table]->parent : cur_parent(pb);
        if (par < 0) par = cur_parent(pb);
        /* внутри pre/textarea пробелы значимы; иначе разбиваем длинный текст на узлы <= 500 байт */
        size_t pos = st;
        while (pos < len) {
            size_t take = len - pos;
            if (take > 500) { take = 500; while (take > 100 && !is_ws((unsigned char)buf[pos + take])) take--; while (take > 1 && ((unsigned char)buf[pos + take] & 0xC0) == 0x80) take--; }
            DOMNode* t = foster_table >= 0 ? mk_before(pb, par, "#text", foster_table) : mk(pb, par, "#text"); if (!t) break;
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

    int foster_before = -1;
    if (!pb->mode_head) {
        pb->insertion_mode = reset_insertion_mode(pb);
        prepare_table_context_for_start(pb, name);
        pb->insertion_mode = reset_insertion_mode(pb);
        (void)prepare_table_start(pb, name, a, na, &foster_before);
        if (foster_before < 0 && is_formatting_tag(name)) reconstruct_active_formatting(pb);
        if (!strcmp(name, "a") && active_formatting_index(pb, "a") >= 0)
            adoption_agency(pb, "a");
    }

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
        /* Formatting element starts are handled by the active-formatting list
         * and adoption-agency algorithm above, rather than stack truncation. */
        else if (!strcmp(name, "button")) { int i = stack_find(pb, "button", " td th table html "); if (i >= 0) pop_to(pb, i); }
    }

    int parent = foster_before >= 0 ? pb->v[foster_before]->parent : cur_parent(pb);
    if (parent < 0) parent = cur_parent(pb);
    n = foster_before >= 0 ? mk_before(pb, parent, name, foster_before) : mk(pb, parent, name);
    if (!n) return;
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
            char media[96] = "", nonce[256] = ""; webpage_attr_get(n, "media", media, sizeof media); webpage_attr_get(n, "nonce", nonce, sizeof nonce);
            if (websec_allows_inline(NAL_RESOURCE_STYLE, nonce)) { char* css = malloc(len + 1); if (css) { memcpy(css, p, len); css[len] = 0; add_sheet(pb, css, NULL, media); } }
        } else if (!strcmp(name, "script")) {
            char ty[64] = "", nm[8] = ""; webpage_attr_get(n, "type", ty, sizeof ty); int nomod = webpage_attr_get(n, "nomodule", nm, sizeof nm);
            int is_js = !nomod && (!ty[0] || find_ci(ty, "javascript") || find_ci(ty, "ecmascript") || !strcasecmp(ty, "module"));
            if (is_js && (len > 0 || n->src[0])) {
                char* text = NULL; if (!n->src[0] && len > 0) { text = malloc(len + 1); if (text) { memcpy(text, p, len); text[len] = 0; } }
                char t2[8], t3[8], nonce[256] = ""; webpage_attr_get(n, "nonce", nonce, sizeof nonce);
                bundle_add_script(text, n->src[0] ? strdup(n->src) : NULL, strdup(nonce), !strcasecmp(ty, "module"), webpage_attr_get(n, "async", t2, sizeof t2), webpage_attr_get(n, "defer", t3, sizeof t3), n->idx);
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
    if (is_formatting_tag(name) && pb->nformatting < (int)(sizeof pb->formatting / sizeof pb->formatting[0]))
        pb->formatting[pb->nformatting++] = n->idx;
    else if (in_list(" applet button caption marquee object td th ", name))
        active_formatting_marker(pb);
    pb->insertion_mode = reset_insertion_mode(pb);
    if (!strcmp(name, "pre") || !strcmp(name, "listing")) pb->skip_nl = 1;
}

static void end_tag(PB* pb, const char* name) {
    if (!strcmp(name, "html") || !strcmp(name, "body")) return;
    if (!strcmp(name, "head")) { ensure_body(pb); return; }
    if (!strcmp(name, "br")) { Attr none[1]; const char* nul = ""; start_tag(pb, "br", none, 0, 1, &nul); return; }
    if (pb->mode_head) return;
    pb->insertion_mode = reset_insertion_mode(pb);

    if (!strcmp(name, "td") || !strcmp(name, "th")) {
        int i = open_cell_stack_index(pb);
        if (i >= 0 && !strcmp(pb->v[pb->stack[i]]->tag, name)) {
            clear_active_formatting_to_marker(pb);
            pb->sp = i;
        }
        pb->insertion_mode = reset_insertion_mode(pb);
        return;
    }
    if (!strcmp(name, "caption")) {
        int i = stack_find(pb, "caption", " table html ");
        if (i >= 0) {
            clear_active_formatting_to_marker(pb);
            pb->sp = i;
        }
        pb->insertion_mode = reset_insertion_mode(pb);
        return;
    }
    int open_cell = open_cell_stack_index(pb);
    if (open_cell >= 0 && in_list(" tr tbody thead tfoot ", name)) {
        clear_active_formatting_to_marker(pb);
        pb->sp = open_cell; /* reprocess the token with the cell no longer open */
    } else if (open_cell >= 0 && !strcmp(name, "table")) {
        clear_active_formatting_to_marker(pb);
    }
    if (is_formatting_tag(name) && active_formatting_index(pb, name) >= 0) {
        adoption_agency(pb, name);
        pb->insertion_mode = reset_insertion_mode(pb);
        return;
    }
    const char* stop = SCOPE_STOP;
    if (!strcmp(name, "table")) stop = " html ";
    else if (!strcmp(name, "td") || !strcmp(name, "th") || !strcmp(name, "tr") || !strcmp(name, "tbody") || !strcmp(name, "thead") || !strcmp(name, "tfoot") || !strcmp(name, "caption")) stop = " table html ";
    else if (in_list(" div span a b i u em strong small font section article nav header footer main ul ol li p label form ", name)) stop = " td th table caption html ";
    int i = stack_find(pb, name, stop);
    if (i >= 0) {
        if (in_list(" applet button marquee object ", name)) clear_active_formatting_to_marker(pb);
        pop_to(pb, i);
    }
    else if (!strcmp(name, "p")) {
        /* In HTML5 a stray </p> synthesizes an empty paragraph, then closes it. */
        Attr none[1]; const char* nul = "";
        start_tag(pb, "p", none, 0, 0, &nul);
        i = stack_find(pb, "p", SCOPE_STOP);
        if (i >= 0) pop_to(pb, i);
    }
    pb->insertion_mode = reset_insertion_mode(pb);
}

/* ── Responsive images: srcset/sizes/picture ─────────────────────────────── */
typedef struct { char url[512]; float value; char unit; } SrcCandidate;

static float viewport_width_px(void) { return SCREEN_WIDTH > 0 ? (float)SCREEN_WIDTH : 1024.0f; }
static float viewport_height_px(void) { return SCREEN_HEIGHT > 0 ? (float)SCREEN_HEIGHT : 768.0f; }

static int media_matches(const char *media) {
    if (!media || !*media) return 1;
    if (find_ci(media, "not all")) return 0;
    if (find_ci(media, "print") && !find_ci(media, "screen")) return 0;
    const char *p = media;
    while ((p = strchr(p, '(')) != NULL) {
        const char *end = strchr(p + 1, ')'); if (!end) break;
        char q[128]; size_t n = (size_t)(end - p - 1); if (n >= sizeof q) n = sizeof q - 1;
        memcpy(q, p + 1, n); q[n] = 0;
        char *colon = strchr(q, ':');
        if (colon) {
            *colon++ = 0;
            while (isspace((unsigned char)*q)) memmove(q, q + 1, strlen(q));
            for (char *c = q; *c; ++c) *c = (char)tolower((unsigned char)*c);
            while (isspace((unsigned char)*colon)) ++colon;
            char *ep = NULL; double v = strtod(colon, &ep);
            if (ep != colon) {
                double px = v;
                while (isspace((unsigned char)*ep)) ++ep;
                if (!strncasecmp(ep, "em", 2) || !strncasecmp(ep, "rem", 3)) px = v * 16.0;
                else if (!strncasecmp(ep, "vw", 2)) px = v * viewport_width_px() / 100.0;
                else if (!strncasecmp(ep, "vh", 2)) px = v * viewport_height_px() / 100.0;
                if (!strcmp(q, "min-width") && viewport_width_px() < px) return 0;
                if (!strcmp(q, "max-width") && viewport_width_px() > px) return 0;
                if (!strcmp(q, "min-height") && viewport_height_px() < px) return 0;
                if (!strcmp(q, "max-height") && viewport_height_px() > px) return 0;
            }
            if (!strcmp(q, "orientation")) {
                int landscape = viewport_width_px() >= viewport_height_px();
                if (!strcasecmp(colon, "portrait") && landscape) return 0;
                if (!strcasecmp(colon, "landscape") && !landscape) return 0;
            }
        }
        p = end + 1;
    }
    return 1;
}

static int image_type_supported(const char *type) {
    if (!type || !*type) return 1;
    while (isspace((unsigned char)*type)) ++type;
    char t[96]; snprintf(t, sizeof t, "%s", type);
    char *semi = strchr(t, ';'); if (semi) *semi = 0;
    for (char *c = t; *c; ++c) *c = (char)tolower((unsigned char)*c);
    if (!strcmp(t, "image/avif")) {
#ifdef HAVE_AVIF
        return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(t, "image/webp")) {
#ifdef HAVE_WEBP
        return 1;
#else
        return 0;
#endif
    }
    return !strcmp(t, "image/png") || !strcmp(t, "image/jpeg") || !strcmp(t, "image/jpg") ||
           !strcmp(t, "image/gif") || !strcmp(t, "image/bmp") || !strcmp(t, "image/svg+xml");
}

static int image_url_supported(const char *url) {
    if (!url || !*url) return 0;
    if (!strncasecmp(url, "data:image/avif", 15)) {
#ifdef HAVE_AVIF
        return 1;
#else
        return 0;
#endif
    }
    if (!strncasecmp(url, "data:image/webp", 15)) {
#ifdef HAVE_WEBP
        return 1;
#else
        return 0;
#endif
    }
    if (!strncasecmp(url, "data:", 5)) return 1;
    const char *end = url + strcspn(url, "?#");
    const char *dot = end; while (dot > url && dot[-1] != '.' && dot[-1] != '/') --dot;
    if (dot > url && dot[-1] == '.') {
        size_t n = (size_t)(end - dot);
        if ((n == 4 && !strncasecmp(dot, "avif", n))) {
#ifdef HAVE_AVIF
            return 1;
#else
            return 0;
#endif
        }
        if (n == 4 && !strncasecmp(dot, "webp", n)) {
#ifdef HAVE_WEBP
            return 1;
#else
            return 0;
#endif
        }
    }
    return 1; /* неизвестный URL может получить формат по фактической сигнатуре файла */
}

static float css_length_px(const char *s, float fallback) {
    while (isspace((unsigned char)*s)) ++s;
    if (!*s) return fallback;
    char *end = NULL; float v = strtof(s, &end); if (end == s || v <= 0) return fallback;
    while (isspace((unsigned char)*end)) ++end;
    if (!strncasecmp(end, "vw", 2)) return v * viewport_width_px() / 100.0f;
    if (!strncasecmp(end, "vh", 2)) return v * viewport_height_px() / 100.0f;
    if (!strncasecmp(end, "rem", 3) || !strncasecmp(end, "em", 2)) return v * 16.0f;
    if (!*end || !strncasecmp(end, "px", 2)) return v;
    return fallback;
}

static float sizes_slot_px(const char *sizes) {
    if (!sizes || !*sizes) return viewport_width_px(); /* HTML default: 100vw */
    const char *p = sizes;
    while (*p) {
        const char *comma = strchr(p, ','); size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len > 0 && len < 256) {
            char item[256]; memcpy(item, p, len); item[len] = 0;
            /* The final whitespace-delimited token is the slot length; preceding text is a media query. */
            char *tail = item + strlen(item); while (tail > item && isspace((unsigned char)tail[-1])) --tail; *tail = 0;
            char *lenstart = tail; while (lenstart > item && !isspace((unsigned char)lenstart[-1])) --lenstart;
            if (lenstart > item) {
                char *media_end = lenstart - 1; while (media_end > item && isspace((unsigned char)media_end[-1])) --media_end; *media_end = 0;
                if (media_matches(item)) return css_length_px(lenstart, viewport_width_px());
            } else return css_length_px(item, viewport_width_px());
        }
        if (!comma) break;
        p = comma + 1;
    }
    return viewport_width_px();
}

/* Parse the HTML srcset token pattern: URL, optional descriptor, comma. Keeping the URL token
   intact until whitespace lets data: URLs retain their embedded comma. */
static int srcset_parse(const char *srcset, SrcCandidate *out, int cap) {
    int count = 0; const char *p = srcset;
    while (p && *p && count < cap) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) ++p;
        if (!*p) break;
        const char *u = p; while (*p && !isspace((unsigned char)*p)) ++p;
        const char *ue = p; while (ue > u && ue[-1] == ',') --ue;
        if (ue == u) continue;
        int trailing_comma = ue < p;
        size_t un = (size_t)(ue - u); if (un >= sizeof out[count].url) { while (*p && *p != ',') ++p; if (*p) ++p; continue; }
        memcpy(out[count].url, u, un); out[count].url[un] = 0;
        out[count].value = 1.0f; out[count].unit = 'x';
        if (trailing_comma) { /* URL ended with a delimiter; next token starts a new candidate. */
            ++count; continue;
        }
        while (isspace((unsigned char)*p)) ++p;
        const char *d = p; while (*p && *p != ',') ++p;
        const char *de = p; while (de > d && isspace((unsigned char)de[-1])) --de;
        if (de > d) {
            char desc[64]; size_t dn = (size_t)(de - d); if (dn >= sizeof desc) dn = sizeof desc - 1;
            memcpy(desc, d, dn); desc[dn] = 0;
            char *ep = NULL; float val = strtof(desc, &ep);
            if (ep != desc && val > 0 && (*ep == 'w' || *ep == 'x') && ep[1] == 0) { out[count].value = val; out[count].unit = *ep; }
            else { while (isspace((unsigned char)*p)) ++p; if (*p == ',') ++p; continue; }
        }
        ++count;
        if (*p == ',') ++p;
    }
    return count;
}

static int choose_srcset(const char *srcset, const char *sizes, char *out, size_t cap) {
    SrcCandidate c[64]; int n = srcset_parse(srcset, c, (int)(sizeof c / sizeof c[0]));
    if (n <= 0) return 0;
    float slot = sizes_slot_px(sizes); if (slot < 1.0f) slot = viewport_width_px();
    int best_above_idx = -1, max_idx = -1; float best_above = 1e30f, highest = -1.0f;
    for (int i = 0; i < n; ++i) {
        if (!image_url_supported(c[i].url)) continue;
        float density = c[i].unit == 'w' ? c[i].value / slot : c[i].value;
        if (density > highest) { highest = density; max_idx = i; }
        if (density >= 1.0f && density < best_above) { best_above = density; best_above_idx = i; }
    }
    int best = best_above_idx >= 0 ? best_above_idx : max_idx;
    if (best < 0) return 0;
    snprintf(out, cap, "%s", c[best].url); return 1;
}

static void resolve_image_candidate(const char *base, const char *raw, char *out, size_t cap) {
    out[0] = 0;
    if (!raw || !*raw) return;
    if (!strncasecmp(raw, "data:", 5)) {
        char path[512]; if (webpage_data_uri_to_file(raw, strlen(raw), path, sizeof path)) snprintf(out, cap, "%s", path);
    } else webpage_resolve_url(base, raw, out, cap);
}

static void select_responsive_images(PB *pb) {
    for (int i = 0; i < pb->n; ++i) {
        DOMNode *img = pb->v[i]; if (img->type != TAG_IMG) continue;
        int selected = 0;
        if (img->parent >= 0 && img->parent < pb->n && !strcmp(pb->v[img->parent]->tag, "picture")) {
            int parent = img->parent;
            for (int j = parent + 1; j < i; ++j) {
                DOMNode *src = pb->v[j]; if (src->parent != parent || src->type != TAG_SOURCE) continue;
                char media[160] = "", type[96] = "";
                webpage_attr_get(src, "media", media, sizeof media);
                webpage_attr_get(src, "type", type, sizeof type);
                if (!media_matches(media) || !image_type_supported(type)) continue;
                char raw[512] = "";
                if (src->srcset[0]) choose_srcset(src->srcset, src->sizes, raw, sizeof raw);
                if (!raw[0] && src->src[0]) snprintf(raw, sizeof raw, "%s", src->src);
                if (!raw[0] || !image_url_supported(raw)) continue;
                resolve_image_candidate(pb->base, raw, img->src, sizeof img->src);
                if (img->src[0]) { selected = 1; break; }
            }
        }
        if (selected) continue;
        if (img->srcset[0]) {
            char raw[512] = "";
            if (choose_srcset(img->srcset, img->sizes, raw, sizeof raw)) {
                char resolved[512]; resolve_image_candidate(pb->base, raw, resolved, sizeof resolved);
                if (resolved[0]) snprintf(img->src, sizeof img->src, "%s", resolved);
            }
        }
    }
}

/* Convert the stable-id construction graph into the renderer's preorder list.
 * Normal children retain source order; foster-parented nodes are moved just
 * before their table. This makes tree repair visible through the legacy
 * parent/subtree_end fields without invalidating parser stacks mid-parse. */
static int flatten_html_tree(PB* pb) {
    int n = pb->n;
    if (n <= 0) return 0;
    int *first = malloc((size_t)n * sizeof(int));
    int *last = malloc((size_t)n * sizeof(int));
    int *next = malloc((size_t)n * sizeof(int));
    int *prev = malloc((size_t)n * sizeof(int));
    int *map = malloc((size_t)n * sizeof(int));
    int *walk = malloc((size_t)n * sizeof(int));
    DOMNode **ordered = malloc((size_t)n * sizeof(DOMNode*));
    if (!first || !last || !next || !prev || !map || !walk || !ordered) {
        free(first); free(last); free(next); free(prev); free(map); free(walk); free(ordered);
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        first[i] = last[i] = next[i] = prev[i] = -1;
        map[i] = -1;
    }

    /* Prevent malformed/adoption-corrupted edges from causing cycles. */
    for (int i = 1; i < n; ++i) {
        int p = pb->v[i]->parent;
        int steps = 0, q = p;
        while (q >= 0 && q < n && steps++ <= n) {
            if (q == i || steps > n) { p = pb->body >= 0 && pb->body < n ? pb->body : 0; break; }
            q = pb->v[q]->parent;
        }
        if (p < 0 || p >= n || p == i) p = pb->body >= 0 && pb->body < n && i != pb->body ? pb->body : 0;
        pb->v[i]->parent = p;
    }
    pb->v[0]->parent = -1;

    for (int i = 1; i < n; ++i) {
        int p = pb->v[i]->parent;
        if (last[p] < 0) first[p] = i;
        else { next[last[p]] = i; prev[i] = last[p]; }
        last[p] = i;
    }

    /* Insert foster-parented children before the table they belong beside. */
    for (int i = 1; i < n; ++i) {
        int target = pb->v[i]->html_before;
        int parent = pb->v[i]->parent;
        if (target < 0 || target >= n || target == i || pb->v[target]->parent != parent) continue;
        int before_prev = prev[i], after_next = next[i];
        if (before_prev >= 0) next[before_prev] = after_next; else first[parent] = after_next;
        if (after_next >= 0) prev[after_next] = before_prev; else last[parent] = before_prev;
        int target_prev = prev[target];
        prev[i] = target_prev; next[i] = target;
        prev[target] = i;
        if (target_prev >= 0) next[target_prev] = i; else first[parent] = i;
        if (last[parent] < 0) last[parent] = i;
    }

    int used = 0, top = 0;
    walk[top++] = 0;
    while (top > 0 && used < n) {
        int id = walk[--top];
        if (id < 0 || id >= n || map[id] == -2) continue;
        map[id] = -2; ordered[used++] = pb->v[id];
        for (int child = last[id]; child >= 0; child = prev[child]) {
            if (top < n) walk[top++] = child;
        }
    }
    /* A disconnected node should never happen; keep it in the returned list. */
    for (int i = 0; i < n && used < n; ++i) if (map[i] != -2) ordered[used++] = pb->v[i];
    for (int i = 0; i < n; ++i) map[pb->v[i]->idx] = -1;
    for (int i = 0; i < n; ++i) map[ordered[i]->idx] = i;

    for (int i = 0; i < n; ++i) {
        DOMNode* node = ordered[i];
        int old_parent = node->parent;
        node->idx = i;
        node->parent = old_parent >= 0 && old_parent < n ? map[old_parent] : -1;
        node->subtree_end = i;
        node->html_before = -1;
        node->next = i + 1 < n ? ordered[i + 1] : NULL;
    }
    for (int i = n - 1; i > 0; --i) {
        int p = ordered[i]->parent;
        if (p >= 0 && p < n && ordered[i]->subtree_end > ordered[p]->subtree_end)
            ordered[p]->subtree_end = ordered[i]->subtree_end;
    }
    for (int i = 0; i < g_bundle.nscripts; ++i) {
        int old = g_bundle.scripts[i].node_idx;
        if (old >= 0 && old < n) g_bundle.scripts[i].node_idx = map[old];
    }

    free(pb->v); pb->v = ordered; pb->cap = n;
    free(first); free(last); free(next); free(prev); free(map); free(walk);
    return 1;
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

    /* Flatten only after tree repair: formatting reconstruction and foster parenting can reparent nodes. */
    if (!flatten_html_tree(&pb)) {
        for (int i = 0; i + 1 < pb.n; i++) pb.v[i]->next = pb.v[i + 1];
        if (pb.n) pb.v[pb.n - 1]->next = NULL;
    }
    select_responsive_images(&pb);
    DOMNode* head = pb.v[0];
    free(pb.v);
    return head;
}

/* URL текущего документа — для проверок безопасности ресурсов (смешанное содержимое). Выставляется при разборе (процесс загрузки),
   при каскаде стилей и при создании JS-страницы (основной процесс). */
static char g_doc_url[8192];
void webpage_set_doc_url(const char* u) {
    /* Let NAL canonicalize HSTS first; mirror the effective value, never a truncated input. */
    nal_set_document_url(u ? u : "");
    const char* effective = nal_current_document_url();
    if (!effective) effective = "";
    size_t n = strlen(effective);
    if (n >= sizeof g_doc_url) g_doc_url[0] = 0;
    else memcpy(g_doc_url, effective, n + 1);
}
int webpage_mixed_blocked(const char* url) {
    if (!nal_mixed_content_blocked(g_doc_url, url)) return 0;
    gal_log("WARN", "Mixed Content: заблокирован небезопасный ресурс %s на странице %s", url, g_doc_url); return 1;
}

/* Текст ресурса: http(s) через NAL, иначе локальный файл. */
char* webpage_fetch_text_kind(const char* url, int resource_kind) {
    if (!url) return NULL;
    if (webpage_mixed_blocked(url)) return NULL;
    if (ci_prefix(url, "http://") || ci_prefix(url, "https://")) return nal_http_get_kind(url, resource_kind);
    const char* path = ci_prefix(url, "file://") ? url + 7 : url;
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 8 * 1024 * 1024) { fclose(f); return NULL; }
    char* b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f); b[got] = 0; fclose(f); return b;
}
char* webpage_fetch_text(const char* url) { return webpage_fetch_text_kind(url, NAL_RESOURCE_OTHER); }

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
            s->css = nal_http_get_kind(s->href, NAL_RESOURCE_STYLE);
            budget--; if (s->css) got++;
        }
    }
    return got;
}

void webpage_bundle_free(PageBundle* b) {
    if (!b) return;
    for (int i = 0; i < b->nsheets; i++) { free(b->sheets[i].css); free(b->sheets[i].href); }
    for (int i = 0; i < b->nscripts; i++) { free(b->scripts[i].text); free(b->scripts[i].url); free(b->scripts[i].nonce); }
    free(b->sheets); free(b->scripts); free(b);
}

static void wr_str(FILE* f, const char* s) { uint32_t n = s ? (uint32_t)strlen(s) + 1 : 0; fwrite(&n, 4, 1, f); if (n) fwrite(s, 1, n, f); }
static char* rd_str(FILE* f) { uint32_t n; if (fread(&n, 4, 1, f) != 1 || n == 0 || n > 64u * 1024 * 1024) return NULL; char* s = malloc(n); if (!s) return NULL; if (fread(s, 1, n, f) != n) { free(s); return NULL; } s[n - 1] = 0; return s; }

int webpage_bundle_write(const char* path) {
    FILE* f = fopen(path, "wb"); if (!f) return 0;
    fwrite("MLB2", 1, 4, f); wr_str(f, g_bundle.base);
    fwrite(&g_bundle.nsheets, 4, 1, f);
    for (int i = 0; i < g_bundle.nsheets; i++) { SheetSrc* s = &g_bundle.sheets[i]; wr_str(f, s->css); wr_str(f, s->href); wr_str(f, s->media); }
    fwrite(&g_bundle.nscripts, 4, 1, f);
    for (int i = 0; i < g_bundle.nscripts; i++) { ScriptSrc* s = &g_bundle.scripts[i]; wr_str(f, s->text); wr_str(f, s->url); wr_str(f, s->nonce); uint8_t fl[3] = {s->module, s->async_, s->defer_}; fwrite(fl, 1, 3, f); fwrite(&s->node_idx, 4, 1, f); }
    fclose(f); return 1;
}

PageBundle* webpage_bundle_read(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    char m[4]; PageBundle* b = calloc(1, sizeof *b); int ok = b && fread(m, 1, 4, f) == 4 && (!memcmp(m, "MLB1", 4) || !memcmp(m, "MLB2", 4)); int v2 = ok && !memcmp(m, "MLB2", 4);
    if (ok) { char* base = rd_str(f); if (base) { snprintf(b->base, sizeof b->base, "%s", base); free(base); } ok = fread(&b->nsheets, 4, 1, f) == 1 && b->nsheets >= 0 && b->nsheets < 1000; }
    if (ok) { b->sheets = calloc((size_t)b->nsheets + 1, sizeof(SheetSrc)); for (int i = 0; ok && i < b->nsheets; i++) { b->sheets[i].css = rd_str(f); b->sheets[i].href = rd_str(f); char* md = rd_str(f); if (md) { snprintf(b->sheets[i].media, sizeof b->sheets[i].media, "%s", md); free(md); } } }
    if (ok) ok = fread(&b->nscripts, 4, 1, f) == 1 && b->nscripts >= 0 && b->nscripts < 1000;
    if (ok) { b->scripts = calloc((size_t)b->nscripts + 1, sizeof(ScriptSrc)); for (int i = 0; ok && i < b->nscripts; i++) { ScriptSrc* s = &b->scripts[i]; s->text = rd_str(f); s->url = rd_str(f); if (v2) s->nonce = rd_str(f); uint8_t fl[3]; ok = fread(fl, 1, 3, f) == 3 && fread(&s->node_idx, 4, 1, f) == 1; if (ok) { s->module = fl[0]; s->async_ = fl[1]; s->defer_ = fl[2]; } } }
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
            if (!sc->text && sc->url && fetched < 24) { fetched++; sc->text = webpage_fetch_text_kind(sc->url, NAL_RESOURCE_SCRIPT); if (sc->text && strlen(sc->text) > 4u * 1024 * 1024) { free(sc->text); sc->text = NULL; } }
        }
        style_apply(head, &sheets, base_url);
        webpage_prefetch_images(head);
    }
    snprintf(g_bundle.base, sizeof g_bundle.base, "%s", base_url ? base_url : "");
    g_bundle.sheets = sheets.v; g_bundle.nsheets = sheets.n;      /* владение таблицами стилей переходит бандлу */
    free(conv);
    return head;
}
