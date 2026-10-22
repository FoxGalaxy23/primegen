/* Каскад: UA-стили + таблицы страницы + inline -> вычисленный NodeStyle каждого узла. */
#include "css_int.h"
#include "nal.h"
#include "gal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <strings.h>

extern int SCREEN_WIDTH, SCREEN_HEIGHT;
#define CUR_COLOR 0x01000000u

static const char* UA_CSS =
"html,body{display:block}"
"head,link,meta,script,style,title,base,noscript,template,area,datalist,param,source,track,colgroup,col,[hidden]{display:none}"
"body{margin:8px}"
"address,blockquote,center,dialog,div,figure,figcaption,footer,form,header,hr,legend,listing,main,p,pre,search,xmp,article,aside,"
"h1,h2,h3,h4,h5,h6,hgroup,nav,section,dir,dd,dl,dt,menu,ol,ul,details,summary,fieldset,optgroup{display:block}"
"li{display:list-item}"
"table{display:table;border-spacing:2px}caption{display:table-caption;text-align:center}thead,tbody,tfoot{display:table-row-group}"
"tr{display:table-row}td,th{display:table-cell;padding:1px}"
"input,button,select,textarea{display:inline-block}"
"blockquote{margin:1em 40px}dl{margin:1em 0}dd{margin-left:40px}ol,ul{margin:1em 0;padding-left:40px}"
"ol ol,ol ul,ul ol,ul ul{margin:0}ul{list-style-type:disc}ol{list-style-type:decimal}ul ul{list-style-type:circle}"
"nav ul,nav ol,menu{list-style-type:none}p{margin:1em 0}"
"h1{font-size:2em;margin:.67em 0;font-weight:bold}h2{font-size:1.5em;margin:.83em 0;font-weight:bold}"
"h3{font-size:1.17em;margin:1em 0;font-weight:bold}h4{margin:1.33em 0;font-weight:bold}"
"h5{font-size:.83em;margin:1.67em 0;font-weight:bold}h6{font-size:.67em;margin:2.33em 0;font-weight:bold}"
"b,strong,th{font-weight:bold}th{text-align:center}i,em,cite,var,dfn,address{font-style:italic}"
"u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
"small{font-size:.83em}big{font-size:1.17em}sub{vertical-align:sub;font-size:.83em}sup{vertical-align:super;font-size:.83em}"
"code,kbd,samp,tt,pre{font-family:monospace}pre{white-space:pre;margin:1em 0}"
"a:link{color:#0000ee;text-decoration:underline;cursor:pointer}"
"hr{margin:.5em auto;border:1px inset #9a9a9a;overflow:hidden}center{text-align:-webkit-center}table{text-align:start}mark{background-color:#ff0;color:#000}"
"input,button,select,textarea{font-size:13.333px;font-family:sans-serif;color:#000;margin:0}"
"input{padding:1px 2px;border:1px solid #767676;background-color:#fff;border-radius:2px}"
"textarea{padding:2px;border:1px solid #767676;background-color:#fff;white-space:pre-wrap;font-family:monospace}"
"select{padding:0 2px;border:1px solid #767676;background-color:#fff;border-radius:2px}"
"button,input[type=button],input[type=submit],input[type=reset]{padding:1px 6px;border:1px solid #767676;background-color:#efefef;text-align:center;border-radius:3px}"
"input[type=hidden]{display:none}input[type=checkbox],input[type=radio]{padding:0;border:0;background-color:transparent}"
"fieldset{margin:0 2px;padding:.35em .75em .625em;border:2px groove #c0c0c0}legend{padding:0 2px}"
"img{}iframe{border:2px inset #767676}"
"noscript,[hidden]{display:none}";

/* ══════════════ индекс правил ══════════════ */
typedef struct HB { char* key; Rule** v; int n, cap; struct HB* next; } HB;
#define HSZ 4096
typedef struct { HB* t[HSZ]; Rule** uni; int nuni, capuni; int has_pseudo; } RIndex;

static unsigned hstr(const char* s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }
static HB* hb_get(RIndex* ix, const char* key, int create) {
    unsigned h = hstr(key) % HSZ; HB* b;
    for (b = ix->t[h]; b; b = b->next) if (!strcmp(b->key, key)) return b;
    if (!create) return NULL;
    b = calloc(1, sizeof *b); b->key = strdup(key); b->next = ix->t[h]; ix->t[h] = b; return b;
}
static void hb_push(HB* b, Rule* r) { if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 4; b->v = realloc(b->v, (size_t)b->cap * sizeof(Rule*)); } b->v[b->n++] = r; }

static void index_rules(RIndex* ix, RuleSet* rs) {
    for (int i = 0; i < rs->n; i++) {
        Rule* r = rs->v[i]; Simple* last = &r->sel[r->nsel - 1]; char key[80];
        if (r->pseudo) ix->has_pseudo = 1;
        if (last->id[0]) { snprintf(key, sizeof key, "#%s", last->id); hb_push(hb_get(ix, key, 1), r); }
        else if (last->ncls) { snprintf(key, sizeof key, ".%s", last->cls[0]); hb_push(hb_get(ix, key, 1), r); }
        else if (last->tag[0]) { snprintf(key, sizeof key, "T%s", last->tag); hb_push(hb_get(ix, key, 1), r); }
        else { if (ix->nuni == ix->capuni) { ix->capuni = ix->capuni ? ix->capuni * 2 : 64; ix->uni = realloc(ix->uni, (size_t)ix->capuni * sizeof(Rule*)); } ix->uni[ix->nuni++] = r; }
    }
}
static void index_free(RIndex* ix) {
    for (int i = 0; i < HSZ; i++) for (HB* b = ix->t[i]; b;) { HB* n = b->next; free(b->key); free(b->v); free(b); b = n; }
    free(ix->uni);
}

/* ══════════════ документ ══════════════ */
typedef struct CP { const char* name; const char* val; struct CP* next; } CP;
typedef struct {
    DOMNode** nv; int n; int* prev; int* eidx; int* ecnt; CP** cps;
} Doc;

static DOMNode* par(Doc* d, DOMNode* n) { return n->parent >= 0 ? d->nv[n->parent] : NULL; }
static DOMNode* prev_el(Doc* d, DOMNode* n) { int p = d->prev[n->idx]; return p >= 0 ? d->nv[p] : NULL; }

static int has_word(const char* list, const char* w, size_t wl) {
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char* e = p; while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        if ((size_t)(e - p) == wl && strncmp(p, w, wl) == 0) return 1;
        p = e;
    }
    return 0;
}

static int attr_val(const DOMNode* n, const char* name, char* buf, size_t cap) {
    if (!strcmp(name, "id")) { snprintf(buf, cap, "%s", n->id); return n->id[0] != 0; }
    if (!strcmp(name, "class")) { snprintf(buf, cap, "%s", n->class_name); return n->class_name[0] != 0; }
    if (!strcmp(name, "style")) { snprintf(buf, cap, "%s", n->style_str); return n->style_str[0] != 0; }
    return webpage_attr_get(n, name, buf, cap);
}

static int match_simple(Doc* d, DOMNode* n, Simple* s);

static int match_sub(Doc* d, DOMNode* n, Simple* list) { for (Simple* s = list; s; s = s->alt) if (match_simple(d, n, s)) return 1; return 0; }

static int type_index(Doc* d, DOMNode* n, int from_end, int* total) {
    int idx = 1; DOMNode* p = prev_el(d, n);
    while (p) { if (!strcmp(p->tag, n->tag)) idx++; p = prev_el(d, p); }
    if (total || from_end) {
        int after = 0; DOMNode* par_n = par(d, n);
        if (par_n) for (int i = n->idx + 1; i <= par_n->subtree_end; i = d->nv[i]->subtree_end + 1) { if (d->nv[i]->type != TAG_TEXT && !strcmp(d->nv[i]->tag, n->tag)) after++; }
        if (total) *total = idx + after;
        if (from_end) idx = after + 1;
    }
    return idx;
}

static int nth_ok(int a, int b, int idx) { if (a == 0) return idx == b; int k = idx - b; return (k % a == 0) && (k / a >= 0); }

static int match_simple(Doc* d, DOMNode* n, Simple* s) {
    if (n->type == TAG_TEXT) return 0;
    if (s->tag[0] && strcmp(s->tag, n->tag)) return 0;
    if (s->id[0] && strcmp(s->id, n->id)) return 0;
    for (int i = 0; i < s->ncls; i++) if (!has_word(n->class_name, s->cls[i], strlen(s->cls[i]))) return 0;
    for (int i = 0; i < s->nat; i++) {
        char v[160]; if (!attr_val(n, s->at[i].n, v, sizeof v)) return 0;
        const char* w = s->at[i].v; size_t wl = strlen(w), vl = strlen(v);
        switch (s->at[i].op) {
            case '=': if (strcmp(v, w)) return 0; break;
            case '~': if (!has_word(v, w, wl)) return 0; break;
            case '|': if (strncmp(v, w, wl) || (v[wl] && v[wl] != '-')) return 0; break;
            case '^': if (!wl || strncmp(v, w, wl)) return 0; break;
            case '$': if (!wl || vl < wl || strcmp(v + vl - wl, w)) return 0; break;
            case '*': if (!wl || !strstr(v, w)) return 0; break;
            default: break;
        }
    }
    for (int i = 0; i < s->npc; i++) {
        int total = 0, ok = 1;
        switch (s->pc[i]) {
            case PC_FIRST_CHILD: ok = d->eidx[n->idx] == 1; break;
            case PC_LAST_CHILD: ok = n->parent >= 0 && d->eidx[n->idx] == d->ecnt[n->parent]; break;
            case PC_ONLY_CHILD: ok = d->eidx[n->idx] == 1 && n->parent >= 0 && d->ecnt[n->parent] == 1; break;
            case PC_FIRST_TYPE: ok = type_index(d, n, 0, NULL) == 1; break;
            case PC_LAST_TYPE: ok = type_index(d, n, 1, NULL) == 1; break;
            case PC_ONLY_TYPE: ok = type_index(d, n, 0, &total) == 1 && total == 1; break;
            case PC_NTH_CHILD: ok = nth_ok(s->pa[i], s->pb[i], d->eidx[n->idx]); break;
            case PC_NTH_LAST_CHILD: ok = n->parent >= 0 && nth_ok(s->pa[i], s->pb[i], d->ecnt[n->parent] - d->eidx[n->idx] + 1); break;
            case PC_NTH_TYPE: ok = nth_ok(s->pa[i], s->pb[i], type_index(d, n, 0, NULL)); break;
            case PC_ROOT: ok = n->idx == 0; break;
            case PC_EMPTY: ok = n->subtree_end == n->idx; break;
            case PC_LINK: ok = (!strcmp(n->tag, "a") || !strcmp(n->tag, "area")) && n->href[0]; break;
            case PC_CHECKED: ok = n->checked || n->selected; break;
            case PC_DISABLED: ok = n->disabled; break;
            case PC_ENABLED: ok = !n->disabled && (n->type == TAG_INPUT || n->type == TAG_BUTTON || n->type == TAG_SELECT || n->type == TAG_TEXTAREA); break;
            default: ok = 0; break;
        }
        if (!ok) return 0;
    }
    if (s->sub) { int m = match_sub(d, n, s->sub); if (s->sub_neg ? m : !m) return 0; }
    return 1;
}

static int match_at(Doc* d, DOMNode* n, Simple* sel, int i) {
    if (!match_simple(d, n, &sel[i])) return 0;
    if (i == 0) return 1;
    DOMNode* p;
    switch (sel[i].comb) {
        case '>': p = par(d, n); return p && match_at(d, p, sel, i - 1);
        case '+': p = prev_el(d, n); return p && match_at(d, p, sel, i - 1);
        case '~': for (p = prev_el(d, n); p; p = prev_el(d, p)) if (match_at(d, p, sel, i - 1)) return 1; return 0;
        default:  for (p = par(d, n); p; p = par(d, p)) if (match_at(d, p, sel, i - 1)) return 1; return 0;
    }
}

/* ══════════════ var() ══════════════ */
static const char* cp_find(CP* c, const char* name, size_t nl) {
    for (; c; c = c->next) if (strlen(c->name) == nl && !strncmp(c->name, name, nl)) return c->val;
    return NULL;
}
static int resolve_vars(CP* chain, const char* in, char* out, size_t cap, int depth) {
    size_t o = 0; const char* p = in;
    if (depth > 8) return 0;
    while (*p) {
        if (!strncmp(p, "var(", 4)) {
            const char* q = p + 4; int dd = 1; const char* comma = NULL; const char* e = q;
            while (*e && dd) { if (*e == '(') dd++; else if (*e == ')') { dd--; if (!dd) break; } else if (*e == ',' && dd == 1 && !comma) comma = e; e++; }
            if (!*e) return 0;
            const char* ne = comma ? comma : e; while (q < ne && isspace((unsigned char)*q)) q++; const char* ne2 = ne; while (ne2 > q && isspace((unsigned char)ne2[-1])) ne2--;
            const char* val = cp_find(chain, q, (size_t)(ne2 - q)); char tmp[1024];
            if (val) { if (!resolve_vars(chain, val, tmp, sizeof tmp, depth + 1)) return 0; }
            else if (comma) { char fb[512]; size_t fl = (size_t)(e - comma - 1); if (fl > 511) fl = 511; memcpy(fb, comma + 1, fl); fb[fl] = 0; if (!resolve_vars(chain, fb, tmp, sizeof tmp, depth + 1)) return 0; }
            else return 0;
            size_t tl = strlen(tmp); if (o + tl + 1 >= cap) return 0; memcpy(out + o, tmp, tl); o += tl; p = e + 1; continue;
        }
        if (o + 2 >= cap) return 0; out[o++] = *p++;
    }
    out[o] = 0; return 1;
}

/* ══════════════ применение значений ══════════════ */
typedef struct { float em, rem, pfs; uint32_t cur; DOMNode* parent; char* content; int has_content; DOMNode* owner; } Cx;

static int tok_next(const char** pp, char* buf, size_t cap) {
    const char* p = *pp; while (isspace((unsigned char)*p)) p++;
    if (!*p) return 0;
    size_t k = 0; int d = 0;
    while (*p && (d > 0 || !isspace((unsigned char)*p))) {
        if (*p == '"' || *p == '\'') { char q = *p; if (k + 1 < cap) buf[k++] = *p++; while (*p && *p != q) { if (k + 1 < cap) buf[k++] = *p; p++; } if (*p && k + 1 < cap) buf[k++] = *p++; continue; }
        if (*p == '(') d++; else if (*p == ')') d--;
        if (k + 1 < cap) buf[k++] = *p; p++;
    }
    buf[k] = 0; *pp = p; return 1;
}

static int len_of(Cx* c, const char* v, Len* out) { return css_parse_len(v, out, c->em, c->rem); }
static int col_of(Cx* c, const char* v, uint32_t* out) {
    if (!strcasecmp(v, "currentcolor")) { *out = CUR_COLOR; return 1; }
    return css_parse_color(v, out, c->cur);
}
static const char* g_base = NULL;
static float px_of(const Len* l, float pct_base) { return l->px + l->pct * pct_base / 100.0f + l->vw * SCREEN_WIDTH + l->vh * SCREEN_HEIGHT; }

/* 1..4 значения -> t r b l */
static void box4(Cx* c, const char* v, Len out[4]) {
    Len t[4]; int n = 0; char tk[128]; const char* p = v;
    while (n < 4 && tok_next(&p, tk, sizeof tk)) { if (!len_of(c, tk, &t[n])) return; if (!strcasecmp(tk, "auto")) t[n].set = 0; n++; }
    if (n == 0) return;
    if (n == 1) { out[0] = out[1] = out[2] = out[3] = t[0]; }
    else if (n == 2) { out[0] = out[2] = t[0]; out[1] = out[3] = t[1]; }
    else if (n == 3) { out[0] = t[0]; out[1] = out[3] = t[1]; out[2] = t[2]; }
    else { out[0] = t[0]; out[1] = t[1]; out[2] = t[2]; out[3] = t[3]; }
}
static void set_margin_auto(Len out[4], const char* v) { (void)out; (void)v; }

static int bstyle(const char* t) {
    if (!strcasecmp(t, "none") || !strcasecmp(t, "hidden")) return BS_NONE;
    if (!strcasecmp(t, "dashed")) return BS_DASHED; if (!strcasecmp(t, "dotted")) return BS_DOTTED;
    if (!strcasecmp(t, "solid") || !strcasecmp(t, "double") || !strcasecmp(t, "groove") || !strcasecmp(t, "ridge") || !strcasecmp(t, "inset") || !strcasecmp(t, "outset")) return BS_SOLID;
    return -1;
}

static void border_side(Cx* c, NodeStyle* st, int mask, const char* v) {
    char tk[128]; const char* p = v; float w = 3; int style = BS_NONE; uint32_t color = CUR_COLOR; Len l;
    while (tok_next(&p, tk, sizeof tk)) {
        int bs = bstyle(tk); uint32_t cc;
        if (bs >= 0) style = bs;
        else if (!strcasecmp(tk, "thin")) w = 1; else if (!strcasecmp(tk, "medium")) w = 3; else if (!strcasecmp(tk, "thick")) w = 5;
        else if ((isdigit((unsigned char)tk[0]) || tk[0] == '.') && len_of(c, tk, &l)) w = px_of(&l, 0);
        else if (col_of(c, tk, &cc)) color = cc;
    }
    for (int i = 0; i < 4; i++) if (mask & (1 << i)) { st->border_w[i] = w; st->border_style[i] = (uint8_t)style; st->border_c[i] = color; }
}

static void parse_shadow(Cx* c, NodeStyle* st, const char* v) {
    if (!strcasecmp(v, "none")) { st->sh_color = 0; return; }
    char first[256]; const char* e = v; int d = 0; size_t k = 0;
    while (*e && !(d == 0 && *e == ',') && k < 255) { if (*e == '(') d++; if (*e == ')') d--; first[k++] = *e++; } first[k] = 0;
    if (strstr(first, "inset")) return;
    float nums[4] = {0, 0, 0, 0}; int nn = 0; uint32_t color = 0x40000000; char tk[128]; const char* p = first; Len l;
    while (tok_next(&p, tk, sizeof tk)) {
        uint32_t cc;
        if ((isdigit((unsigned char)tk[0]) || tk[0] == '-' || tk[0] == '.') && len_of(c, tk, &l)) { if (nn < 4) nums[nn++] = px_of(&l, 0); }
        else if (col_of(c, tk, &cc)) color = cc;
    }
    if (nn < 2) return;
    st->sh_x = nums[0]; st->sh_y = nums[1]; st->sh_blur = nums[2]; st->sh_spread = nums[3]; st->sh_color = color;
}

/* парсинг всех цветовых остановок linear-/radial-gradient() вместо использования только первой */
static void parse_gradient_stops(Cx* c, NodeStyle* st, const char* tk) {
    int radial = !strncasecmp(tk, "radial-gradient(", 16);
    const char* q = strchr(tk, '('); if (!q) return; q++;
    char a[160]; size_t k = 0; int d = 0; int n = 0; float angle = 180.0f; int first = 1;
    for (;; q++) {
        int end = 0;
        if (*q == '(') d++;
        else if (*q == ')') { if (d == 0) end = 1; else d--; }
        if ((*q == ',' && d == 0) || !*q || end) {
            a[k] = 0; char* sp = a; while (isspace((unsigned char)*sp)) sp++;
            size_t al = strlen(sp); while (al > 0 && isspace((unsigned char)sp[al - 1])) sp[--al] = 0;
            if (first) {
                first = 0;
                if (!radial && (strcasestr(sp, "deg") || !strncasecmp(sp, "to ", 3))) {
                    if (strcasestr(sp, "deg")) angle = strtof(sp, NULL);
                    else {
                        int rt = strcasestr(sp, "right") != NULL, lf = strcasestr(sp, "left") != NULL;
                        int tp = strcasestr(sp, "top") != NULL, bt = strcasestr(sp, "bottom") != NULL;
                        angle = (rt && tp) ? 45.0f : (rt && bt) ? 135.0f : (lf && bt) ? 225.0f : (lf && tp) ? 315.0f :
                                rt ? 90.0f : lf ? 270.0f : tp ? 0.0f : bt ? 180.0f : angle;
                    }
                    k = 0; if (!*q || end) break; continue;
                }
                if (radial && (strcasestr(sp, "circle") || strcasestr(sp, "ellipse") || strcasestr(sp, "at "))) { k = 0; if (!*q || end) break; continue; }
            }
            char cs[96] = {0}; float pos = -1; char* ce = sp; int dd = 0;
            while (*ce && !(dd == 0 && isspace((unsigned char)*ce))) { if (*ce == '(') dd++; if (*ce == ')') dd--; ce++; }
            size_t cl = (size_t)(ce - sp); if (cl > 95) cl = 95; memcpy(cs, sp, cl); cs[cl] = 0;
            while (isspace((unsigned char)*ce)) ce++; if (*ce) pos = strtof(ce, NULL);
            uint32_t col;
            if (n < 4 && css_parse_color(cs, &col, c->cur)) { st->bg_grad_c[n] = col; st->bg_grad_p[n] = pos; n++; }
            k = 0; if (!*q || end) break; continue;
        }
        if (k < sizeof(a) - 1) a[k++] = *q;
    }
    if (n >= 2) {
        for (int i = 0; i < n; i++) if (st->bg_grad_p[i] < 0) st->bg_grad_p[i] = 100.0f * i / (n - 1);
        st->bg_grad_type = radial ? 2 : 1; st->bg_grad_n = (uint8_t)n; st->bg_grad_angle = angle; st->bg = st->bg_grad_c[0];
    } else if (n == 1) { st->bg = st->bg_grad_c[0]; st->bg_grad_type = 0; }
}

static void parse_bg(Cx* c, NodeStyle* st, const char* v, int full) {
    char tk[16384]; const char* p = v; uint32_t col;                       /* data-URI в url(...) бывают длинными */
    if (full) { st->bg = 0; st->bg_url[0] = 0; st->bg_repeat = 0; st->bg_size_mode = 0; memset(&st->bg_px, 0, sizeof(Len) * 2); st->bg_px.pct = st->bg_py.pct = 0; st->bg_px.set = st->bg_py.set = 0; st->bg_grad_type = 0; }
    int posn = 0; int in_size = 0;
    while (tok_next(&p, tk, sizeof tk)) {
        if (!strncasecmp(tk, "url(", 4)) {
            const char* q = tk + 4; while (isspace((unsigned char)*q)) q++; char qc = (*q == '"' || *q == '\'') ? *q++ : 0; size_t k = 0;
            const char* qe = q; while (*qe && (qc ? *qe != qc : *qe != ')')) qe++;
            if (qe - q > 5 && !strncasecmp(q, "data:", 5)) { char dp[200]; if (!webpage_data_uri_to_file(q, (size_t)(qe - q), dp, sizeof dp)) dp[0] = 0; snprintf(st->bg_url, sizeof st->bg_url, "%s", dp); }   /* data: → файл кэша */
            else { while (*q && (qc ? *q != qc : *q != ')') && k < 190) st->bg_url[k++] = *q++; st->bg_url[k] = 0; }
        } else if (!strncasecmp(tk, "linear-gradient(", 16) || !strncasecmp(tk, "-webkit-linear-gradient(", 24) || !strncasecmp(tk, "radial-gradient(", 16)) {
            parse_gradient_stops(c, st, tk);
        } else if (!strcasecmp(tk, "no-repeat")) st->bg_repeat = 3; else if (!strcasecmp(tk, "repeat-x")) st->bg_repeat = 1;
        else if (!strcasecmp(tk, "repeat-y")) st->bg_repeat = 2; else if (!strcasecmp(tk, "repeat")) st->bg_repeat = 0;
        else if (!strcmp(tk, "/")) in_size = 1;
        else if (!strcasecmp(tk, "cover")) st->bg_size_mode = 1; else if (!strcasecmp(tk, "contain")) st->bg_size_mode = 2;
        else if (!strcasecmp(tk, "none") || !strcasecmp(tk, "scroll") || !strcasecmp(tk, "fixed") || !strcasecmp(tk, "local")) {}
        else if (css_parse_color(tk, &col, c->cur)) st->bg = col;
        else {                                                             /* позиция / размер */
            Len l; Len* dst = in_size ? (posn == 0 ? &st->bg_w : &st->bg_h) : (posn == 0 ? &st->bg_px : &st->bg_py);
            if (!strcasecmp(tk, "center")) { memset(&l, 0, sizeof l); l.pct = 50; l.set = 1; }
            else if (!strcasecmp(tk, "left") || !strcasecmp(tk, "top")) { memset(&l, 0, sizeof l); l.set = 1; }
            else if (!strcasecmp(tk, "right") || !strcasecmp(tk, "bottom")) { memset(&l, 0, sizeof l); l.pct = 100; l.set = 1; if (!strcasecmp(tk, "bottom") && posn == 0) dst = &st->bg_py; }
            else if (!len_of(c, tk, &l)) continue;
            *dst = l; posn++; if (in_size && posn > 1) in_size = 0;
            if (in_size && st->bg_size_mode < 3) st->bg_size_mode = 3;
        }
    }
    if (st->bg_w.set || st->bg_h.set) st->bg_size_mode = 3;
}

/* transform: translate()/translateX()/translateY() — остальное (rotate/scale/matrix) не поддерживается */
static void set_transform(Cx* c, NodeStyle* st, const char* val) {
    st->tr_x.set = st->tr_y.set = 0;
    const char* p = strcasestr(val, "translate"); if (!p) return;
    const char* op = strchr(p, '('); if (!op) return;
    char args[128]; const char* q = op + 1; size_t k = 0; while (*q && *q != ')' && k < 127) args[k++] = *q++; args[k] = 0;
    int isX = !strncasecmp(p, "translatex", 10), isY = !strncasecmp(p, "translatey", 10);
    char* comma = strchr(args, ','); if (comma) *comma = 0;
    char tkx[64]; const char* pp = args;
    if (tok_next(&pp, tkx, sizeof tkx)) { Len l; if (len_of(c, tkx, &l)) { if (isY) st->tr_y = l; else st->tr_x = l; } }
    if (comma && !isX && !isY) { pp = comma + 1; if (tok_next(&pp, tkx, sizeof tkx)) { Len l; if (len_of(c, tkx, &l)) st->tr_y = l; } }
}

static void set_family(DOMNode* n, const char* v) { snprintf(n->font_family, sizeof n->font_family, "%s", v); }

static float font_kw(const char* v, float pfs, int* ok) {
    *ok = 1;
    if (!strcasecmp(v, "xx-small")) return 9; if (!strcasecmp(v, "x-small")) return 10; if (!strcasecmp(v, "small")) return 13;
    if (!strcasecmp(v, "medium")) return 16; if (!strcasecmp(v, "large")) return 18; if (!strcasecmp(v, "x-large")) return 24;
    if (!strcasecmp(v, "xx-large")) return 32; if (!strcasecmp(v, "xxx-large")) return 48;
    if (!strcasecmp(v, "larger")) return pfs * 1.2f; if (!strcasecmp(v, "smaller")) return pfs / 1.2f;
    *ok = 0; return 0;
}

static void set_font_size(Cx* c, NodeStyle* st, const char* v) {
    int ok; float kw = font_kw(v, c->pfs, &ok);
    if (ok) { st->font_size = kw; return; }
    Len l; if (!css_parse_len(v, &l, c->pfs, c->rem) || !l.set) return;
    float px = l.px + l.pct * c->pfs / 100.0f + l.vw * SCREEN_WIDTH + l.vh * SCREEN_HEIGHT;
    if (px >= 0) st->font_size = px;
}

static void set_line_height(Cx* c, NodeStyle* st, const char* v) {
    if (!strcasecmp(v, "normal")) { st->lh_mult = 0; st->lh_px = 0; return; }
    char* e; float n = strtof(v, &e);
    if (e != v && !*e) { st->lh_mult = n; st->lh_px = 0; return; }
    Len l; if (css_parse_len(v, &l, c->em, c->rem) && l.set) { st->lh_px = l.px + l.pct * st->font_size / 100.0f; st->lh_mult = 0; }
}

/* font-weight → 100..900 (0 — не распознано). bolder/lighter без учёта родителя: 700 / 300 */
static int parse_weight_val(const char* v) {
    while (isspace((unsigned char)*v)) v++;
    if (!strncasecmp(v, "normal", 6)) return 400;
    if (!strncasecmp(v, "bold", 4) && strncasecmp(v, "bolder", 6)) return 700;
    if (!strncasecmp(v, "bolder", 6)) return 700;
    if (!strncasecmp(v, "lighter", 7)) return 300;
    if (isdigit((unsigned char)*v)) { int w = atoi(v); if (w >= 1 && w <= 1000) { w = (w + 50) / 100 * 100; return w < 100 ? 100 : w > 900 ? 900 : w; } }
    return 0;
}
static void apply_font_shorthand(Cx* c, DOMNode* n, NodeStyle* st, const char* v, int size_pass) {
    char tk[256]; const char* p = v; int bold = 0, ital = 0, wt = 400;
    if (!strcasecmp(v, "inherit")) return;
    while (tok_next(&p, tk, sizeof tk)) {
        if (!strcasecmp(tk, "italic") || !strcasecmp(tk, "oblique")) ital = 1;
        else if (!strcasecmp(tk, "normal") || !strcasecmp(tk, "small-caps")) {}
        else if (!strcasecmp(tk, "bold") || !strcasecmp(tk, "bolder") || (atoi(tk) >= 600 && isdigit((unsigned char)tk[0]) && !strchr(tk, 'p'))) { bold = 1; wt = parse_weight_val(tk); if (!wt) wt = 700; }
        else if (isdigit((unsigned char)tk[0]) && !strchr(tk, '.') && !strchr(tk, 'p') && !strchr(tk, 'e') && !strchr(tk, '%')) { int w = parse_weight_val(tk); if (w) wt = w; }   /* weight 100..500 */
        else if (!strcasecmp(tk, "lighter")) wt = 300;
        else {                                                             /* размер[/высота] */
            char sz[64] = "", lh[64] = ""; const char* sl = strchr(tk, '/');
            if (sl) { snprintf(sz, sizeof sz, "%.*s", (int)(sl - tk), tk); snprintf(lh, sizeof lh, "%s", sl + 1); } else snprintf(sz, sizeof sz, "%s", tk);
            if (size_pass) { set_font_size(c, st, sz); }
            else {
                st->bold = (uint8_t)bold; st->italic = (uint8_t)ital; st->weight = (uint16_t)wt;
                if (lh[0]) set_line_height(c, st, lh); else { st->lh_mult = 0; st->lh_px = 0; }
                while (isspace((unsigned char)*p)) p++;
                if (*p) set_family(n, p);
            }
            return;
        }
    }
}

/* content: "текст" / 'текст' / attr(x) / open-quote / close-quote / none / normal. counter()/url() пропускаются. */
static void put_utf8(char* out, size_t cap, size_t* o, unsigned cp) {
    char b[5]; int l;
    if (cp < 0x80) { b[0] = (char)cp; l = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 63)); l = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); l = 3; }
    else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); l = 4; }
    if (*o + (size_t)l + 1 >= cap) return;
    memcpy(out + *o, b, (size_t)l); *o += (size_t)l; out[*o] = 0;
}
/* сырой байт исходного текста: он уже в UTF-8, поэтому многобайтовые символы копируем как есть (put_utf8 по байтам их портил) */
static void put_raw(char* out, size_t cap, size_t* o, unsigned char ch) { if (*o + 2 >= cap) return; out[(*o)++] = (char)ch; out[*o] = 0; }
static void parse_content(Cx* c, const char* v) {
    char* out = c->content; size_t cap = 248, o = 0; out[0] = 0; c->has_content = 0;
    const char* p = v;
    while (isspace((unsigned char)*p)) p++;
    if (!*p || !strncasecmp(p, "none", 4) || !strncasecmp(p, "normal", 6)) return;
    c->has_content = 1;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\') {
                    p++;
                    if (isxdigit((unsigned char)*p)) {
                        unsigned cp = 0; int k = 0; while (k < 6 && isxdigit((unsigned char)*p)) { cp = cp * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10)); p++; k++; }
                        if (*p == ' ' || *p == '\t' || *p == '\n') p++;
                        if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
                        put_utf8(out, cap, &o, cp);
                    } else if (*p == '\n') p++;
                    else if (*p) { put_raw(out, cap, &o, (unsigned char)*p); p++; }
                } else { put_raw(out, cap, &o, (unsigned char)*p); p++; }
            }
            if (*p) p++;
        } else if (!strncasecmp(p, "attr(", 5)) {
            const char* e = strchr(p, ')'); if (!e) break;
            char nm[64]; size_t l = (size_t)(e - (p + 5)); if (l > 63) l = 63; memcpy(nm, p + 5, l); nm[l] = 0;
            char* t = nm; while (isspace((unsigned char)*t)) t++;
            size_t tl = strlen(t); while (tl && isspace((unsigned char)t[tl - 1])) t[--tl] = 0;
            char av[200]; if (c->owner && attr_val(c->owner, t, av, sizeof av)) for (char* q = av; *q; q++) put_raw(out, cap, &o, (unsigned char)*q);
            p = e + 1;
        } else if (!strncasecmp(p, "open-quote", 10)) { put_utf8(out, cap, &o, 0x201C); p += 10; }
        else if (!strncasecmp(p, "close-quote", 11)) { put_utf8(out, cap, &o, 0x201D); p += 11; }
        else if (*p == '/') break;                                              /* «content: X / "alt"» — альтернативный текст */
        else {                                                                   /* counter(...), url(...), no-open-quote и пр. */
            int d = 0; while (*p && (d > 0 || !isspace((unsigned char)*p))) { if (*p == '(') d++; else if (*p == ')') d--; p++; }
        }
    }
}

static void apply(Cx* c, DOMNode* n, NodeStyle* st, int prop, const char* val, int size_pass) {
    Len l; uint32_t col; char tk[256];
    DOMNode* pa = c->parent;
    if (size_pass) {                                                        /* проход 0 — только размер шрифта */
        if (prop == P_FONT_SIZE) { if (!strcasecmp(val, "inherit")) st->font_size = c->pfs; else set_font_size(c, st, val); }
        else if (prop == P_FONT) apply_font_shorthand(c, n, st, val, 1);
        return;
    }
    if (!strcasecmp(val, "inherit") && pa) {
        const NodeStyle* ps = &pa->st;
        switch (prop) {
            case P_COLOR: st->color = ps->color; break; case P_BG_COLOR: st->bg = ps->bg; break;
            case P_FONT_WEIGHT: st->bold = ps->bold; st->weight = ps->weight; break; case P_FONT_STYLE: st->italic = ps->italic; break;
            case P_FONT_FAMILY: set_family(n, pa->font_family); break; case P_TEXT_ALIGN: st->text_align = ps->text_align; break;
            case P_DISPLAY: st->display = ps->display; break; case P_VISIBILITY: st->visibility = ps->visibility; break;
            case P_WIDTH: st->width = ps->width; break; case P_HEIGHT: st->height = ps->height; break;
            case P_LINE_HEIGHT: st->lh_px = ps->lh_px; st->lh_mult = ps->lh_mult; break;
            case P_TEXT_DECO: case P_TEXT_DECO_LINE: st->deco = ps->deco; break;
            case P_POSITION: st->position = ps->position; break; case P_BORDER_RADIUS: memcpy(st->radius, ps->radius, sizeof st->radius); break;
            default: break;
        }
        return;
    }
    if (!strcasecmp(val, "initial") || !strcasecmp(val, "unset") || !strcasecmp(val, "revert")) return;
    switch (prop) {
    case P_DISPLAY: {
        static const struct { const char* k; int d; } M[] = { {"none",DSP_NONE},{"block",DSP_BLOCK},{"inline",DSP_INLINE},{"inline-block",DSP_INLINE_BLOCK},
            {"flex",DSP_FLEX},{"-webkit-box",DSP_FLEX},{"inline-flex",DSP_INLINE_FLEX},{"-webkit-inline-box",DSP_INLINE_FLEX},{"grid",DSP_BLOCK},
            {"inline-grid",DSP_INLINE_BLOCK},{"list-item",DSP_LIST_ITEM},{"table",DSP_TABLE},{"inline-table",DSP_TABLE},{"table-row-group",DSP_ROW_GROUP},
            {"table-header-group",DSP_ROW_GROUP},{"table-footer-group",DSP_ROW_GROUP},{"table-row",DSP_ROW},{"table-cell",DSP_CELL},{"table-caption",DSP_CAPTION},
            {"table-column",DSP_NONE},{"table-column-group",DSP_NONE},{"contents",DSP_CONTENTS},{"flow-root",DSP_BLOCK},{NULL,0} };
        char w[40]; snprintf(w, sizeof w, "%s", val); for (char* q = w; *q; q++) if (isspace((unsigned char)*q)) { *q = 0; break; }
        st->is_grid = (!strcasecmp(w, "grid") || !strcasecmp(w, "inline-grid"));
        for (int i = 0; M[i].k; i++) if (!strcasecmp(w, M[i].k)) { st->display = (uint8_t)M[i].d; return; }
        st->display = DSP_INLINE; return; }
    case P_POSITION: st->sticky = (uint8_t)(!strcasecmp(val, "sticky") || !strcasecmp(val, "-webkit-sticky"));
        st->position = !strcasecmp(val, "absolute") ? POS_ABSOLUTE : !strcasecmp(val, "fixed") ? POS_FIXED : (!strcasecmp(val, "relative") || st->sticky) ? POS_RELATIVE : POS_STATIC; return;
    case P_FLOAT: st->float_ = (!strcasecmp(val, "left") || !strcasecmp(val, "inline-start")) ? FLT_LEFT : (!strcasecmp(val, "right") || !strcasecmp(val, "inline-end")) ? FLT_RIGHT : FLT_NONE; return;
    case P_CLEAR: st->clear = !strcasecmp(val, "both") ? 3 : !strcasecmp(val, "left") ? 1 : !strcasecmp(val, "right") ? 2 : 0; return;
    case P_VISIBILITY: st->visibility = (!strcasecmp(val, "hidden") || !strcasecmp(val, "collapse")); return;
    case P_OVERFLOW: case P_OVERFLOW_X: case P_OVERFLOW_Y: {
        /* вид значения: 0 visible, 1 hidden/clip (только отсечение), 2 auto/scroll (отсечение + прокрутка) */
        #define OV_KIND(v) (!strncasecmp((v), "auto", 4) || !strncasecmp((v), "scroll", 6) || !strncasecmp((v), "overlay", 7) ? 2 : (!strncasecmp((v), "visible", 7) || !*(v)) ? 0 : 1)
        if (prop == P_OVERFLOW) {                       /* overflow: <x> [<y>] */
            const char* v2 = val; while (*v2 && !isspace((unsigned char)*v2)) v2++; while (*v2 && isspace((unsigned char)*v2)) v2++;
            int kx = OV_KIND(val), ky = *v2 ? OV_KIND(v2) : kx;
            st->overflow_hidden = (uint8_t)(kx || ky); st->sc_x = (uint8_t)(kx == 2); st->sc_y = (uint8_t)(ky == 2); return;
        }
        int k = OV_KIND(val);
        if (k) st->overflow_hidden = 1;
        if (prop == P_OVERFLOW_X) st->sc_x = (uint8_t)(k == 2); else st->sc_y = (uint8_t)(k == 2);
        if (!k && !st->sc_x && !st->sc_y) { /* visible по обеим осям: сохраняем прежнее поведение — отсечение не отключаем, если задано другой осью */ }
        #undef OV_KIND
        return; }
    case P_TEXT_ALIGN: st->center_blocks = (!strcasecmp(val, "-webkit-center") || !strcasecmp(val, "-moz-center")); st->text_align = (!strcasecmp(val, "right") || !strcasecmp(val, "end")) ? ALIGN_RIGHT : (!strcasecmp(val, "center") || !strcasecmp(val, "-webkit-center")) ? ALIGN_CENTER : !strcasecmp(val, "justify") ? ALIGN_JUSTIFY : ALIGN_LEFT; return;
    case P_WHITE_SPACE: st->white_space = !strcasecmp(val, "nowrap") ? WS_NOWRAP : !strcasecmp(val, "pre") ? WS_PRE : (!strcasecmp(val, "pre-wrap") || !strcasecmp(val, "break-spaces")) ? WS_PRE_WRAP : !strcasecmp(val, "pre-line") ? WS_PRE_LINE : WS_NORMAL; return;
    case P_TEXT_DECO: case P_TEXT_DECO_LINE: {
        uint8_t d = 0; if (strcasestr(val, "underline")) d |= DECO_UNDERLINE; if (strcasestr(val, "line-through")) d |= DECO_LINE_THROUGH; if (strcasestr(val, "overline")) d |= DECO_OVERLINE;
        st->deco = d; return; }
    case P_FONT_WEIGHT: { int w = parse_weight_val(val); if (w) { st->weight = (uint16_t)w; st->bold = w >= 600; } return; }
    case P_FONT_STYLE: st->italic = (!strncasecmp(val, "italic", 6) || !strncasecmp(val, "oblique", 7)); return;
    case P_FONT_FAMILY: set_family(n, val); return;
    case P_FONT: apply_font_shorthand(c, n, st, val, 0); return;
    case P_LINE_HEIGHT: set_line_height(c, st, val); return;
    case P_TEXT_TRANSFORM: st->text_transform = !strcasecmp(val, "uppercase") ? TT_UPPER : !strcasecmp(val, "lowercase") ? TT_LOWER : !strcasecmp(val, "capitalize") ? TT_CAP : TT_NONE; return;
    case P_VERTICAL_ALIGN: st->vertical_align = !strcasecmp(val, "top") || !strcasecmp(val, "text-top") ? VA_TOP : !strcasecmp(val, "middle") ? VA_MIDDLE : !strcasecmp(val, "bottom") || !strcasecmp(val, "text-bottom") ? VA_BOTTOM : !strcasecmp(val, "sub") ? VA_SUB : !strcasecmp(val, "super") ? VA_SUPER : VA_BASELINE; return;
    case P_WORD_BREAK: st->break_word = !strcasecmp(val, "break-all") ? 2 : (st->break_word == 1 ? 1 : 0); return;
    case P_OVERFLOW_WRAP: case P_WORD_WRAP: st->break_word = (!strcasecmp(val, "break-word") || !strcasecmp(val, "anywhere")) ? 1 : 0; return;
    case P_OUTLINE: st->no_ring = (!strncasecmp(val, "none", 4) || !strncmp(val, "0", 1)); return;
    case P_CURSOR: st->pointer = !strcasecmp(val, "pointer"); return;
    case P_BOX_SIZING: st->box_sizing = !strcasecmp(val, "border-box"); return;
    case P_OPACITY: { float o = strtof(val, NULL); if (strchr(val, '%')) o /= 100.0f; st->opacity = o < 0 ? 0 : o > 1 ? 1 : o; return; }
    case P_Z_INDEX: if (!strcasecmp(val, "auto")) st->has_z = 0; else { st->has_z = 1; st->z_index = (int16_t)atoi(val); } return;
    case P_COLOR: if (col_of(c, val, &col)) st->color = col; return;
    case P_BG_COLOR: if (col_of(c, val, &col)) st->bg = col; return;
    case P_BACKGROUND: parse_bg(c, st, val, 1); return;
    case P_BG_IMAGE: parse_bg(c, st, val, 0); if (!strncasecmp(val, "none", 4)) st->bg_url[0] = 0; return;
    case P_BG_REPEAT: case P_BG_SIZE: case P_BG_POS: {
        NodeStyle tmp = *st; char b[300]; snprintf(b, sizeof b, "%s%s", prop == P_BG_SIZE ? "/ " : "", val);
        if (prop == P_BG_SIZE) { st->bg_size_mode = !strcasecmp(val, "cover") ? 1 : !strcasecmp(val, "contain") ? 2 : 0; if (st->bg_size_mode == 0) { const char* p = val; memset(&st->bg_w, 0, sizeof(Len) * 2); Len t[2]; int k = 0; while (k < 2 && tok_next(&p, tk, sizeof tk)) { if (len_of(c, tk, &t[k])) k++; else break; } if (k >= 1) { st->bg_w = t[0]; st->bg_h = k > 1 ? t[1] : t[0]; if (k == 1) st->bg_h.set = 0; st->bg_size_mode = 3; } } }
        else { char save[192]; strcpy(save, st->bg_url); uint32_t sb = st->bg; parse_bg(c, st, b, 0); strcpy(st->bg_url, save); st->bg = sb; (void)tmp; }
        return; }
    case P_WIDTH: if (len_of(c, val, &l)) st->width = l; return;
    case P_HEIGHT: if (len_of(c, val, &l)) st->height = l; return;
    case P_MIN_W: if (len_of(c, val, &l)) st->min_w = l; return; case P_MAX_W: if (len_of(c, val, &l)) st->max_w = l; return;
    case P_MIN_H: if (len_of(c, val, &l)) st->min_h = l; return; case P_MAX_H: if (len_of(c, val, &l)) st->max_h = l; return;
    case P_MARGIN: box4(c, val, st->margin); return;
    case P_MARGIN_T: if (len_of(c, val, &l)) st->margin[0] = l; return; case P_MARGIN_R: if (len_of(c, val, &l)) st->margin[1] = l; return;
    case P_MARGIN_B: if (len_of(c, val, &l)) st->margin[2] = l; return; case P_MARGIN_L: if (len_of(c, val, &l)) st->margin[3] = l; return;
    case P_PADDING: box4(c, val, st->padding); return;
    case P_PADDING_T: if (len_of(c, val, &l)) st->padding[0] = l; return; case P_PADDING_R: if (len_of(c, val, &l)) st->padding[1] = l; return;
    case P_PADDING_B: if (len_of(c, val, &l)) st->padding[2] = l; return; case P_PADDING_L: if (len_of(c, val, &l)) st->padding[3] = l; return;
    case P_INSET: box4(c, val, st->inset); return;
    case P_TOP: if (len_of(c, val, &l)) st->inset[0] = l; return; case P_RIGHT: if (len_of(c, val, &l)) st->inset[1] = l; return;
    case P_BOTTOM: if (len_of(c, val, &l)) st->inset[2] = l; return; case P_LEFT: if (len_of(c, val, &l)) st->inset[3] = l; return;
    case P_BORDER: border_side(c, st, 15, val); return;
    case P_BORDER_T: border_side(c, st, 1, val); return; case P_BORDER_R: border_side(c, st, 2, val); return;
    case P_BORDER_B: border_side(c, st, 4, val); return; case P_BORDER_L: border_side(c, st, 8, val); return;
    case P_BORDER_COLOR: case P_BORDER_STYLE: case P_BORDER_WIDTH: {
        const char* p = val; char t[4][64]; int k = 0; while (k < 4 && tok_next(&p, tk, sizeof tk)) snprintf(t[k++], 64, "%s", tk);
        if (!k) return; int map[4] = {0, k > 1 ? 1 : 0, k > 2 ? 2 : 0, k > 3 ? 3 : (k > 1 ? 1 : 0)};
        for (int i = 0; i < 4; i++) { const char* s = t[map[i]];
            if (prop == P_BORDER_COLOR) { if (col_of(c, s, &col)) st->border_c[i] = col; }
            else if (prop == P_BORDER_STYLE) { int b = bstyle(s); if (b >= 0) st->border_style[i] = (uint8_t)b; }
            else { if (!strcasecmp(s, "thin")) st->border_w[i] = 1; else if (!strcasecmp(s, "medium")) st->border_w[i] = 3; else if (!strcasecmp(s, "thick")) st->border_w[i] = 5; else if (len_of(c, s, &l)) st->border_w[i] = px_of(&l, 0); } }
        return; }
    case P_BC_T: case P_BC_R: case P_BC_B: case P_BC_L: if (col_of(c, val, &col)) st->border_c[prop - P_BC_T] = col; return;
    case P_BS_T: case P_BS_R: case P_BS_B: case P_BS_L: { int b = bstyle(val); if (b >= 0) st->border_style[prop - P_BS_T] = (uint8_t)b; return; }
    case P_BW_T: case P_BW_R: case P_BW_B: case P_BW_L: if (len_of(c, val, &l)) st->border_w[prop - P_BW_T] = px_of(&l, 0); return;
    case P_BORDER_RADIUS: {
        const char* p = val; float r[4]; int k = 0; char* sl = strchr(val, '/'); char head[128]; snprintf(head, sizeof head, "%.*s", sl ? (int)(sl - val) : 100, val); p = head;
        while (k < 4 && tok_next(&p, tk, sizeof tk)) { if (!len_of(c, tk, &l)) return; r[k++] = l.pct > 0 ? -l.pct / 100.0f : l.px; }
        if (!k) return; float o[4];
        if (k == 1) o[0] = o[1] = o[2] = o[3] = r[0]; else if (k == 2) { o[0] = o[2] = r[0]; o[1] = o[3] = r[1]; } else if (k == 3) { o[0] = r[0]; o[1] = o[3] = r[1]; o[2] = r[2]; } else memcpy(o, r, sizeof o);
        memcpy(st->radius, o, sizeof o); return; }
    case P_BR_TL: case P_BR_TR: case P_BR_BR: case P_BR_BL: if (len_of(c, val, &l)) st->radius[prop - P_BR_TL] = l.pct > 0 ? -l.pct / 100.0f : l.px; return;
    case P_BORDER_SPACING: if (len_of(c, val, &l)) st->spacing = l.px; return;
    case P_BORDER_COLLAPSE: if (!strcasecmp(val, "collapse")) st->spacing = 0; return;
    case P_BOX_SHADOW: parse_shadow(c, st, val); return;
    case P_BG_CLIP: st->bg_clip_text = strcasestr(val, "text") != NULL; return;
    case P_TEXT_FILL: if (strcasecmp(val, "transparent") && col_of(c, val, &col)) st->color = col; return;
    case P_TRANSFORM: set_transform(c, st, val); return;
    case P_CONTENT: if (c->content) parse_content(c, val); return;
    case P_LIST_STYLE: case P_LIST_STYLE_TYPE: {
        const char* p = val; while (tok_next(&p, tk, sizeof tk)) {
            if (!strcasecmp(tk, "none")) st->list_style = LS_NONE; else if (!strcasecmp(tk, "disc")) st->list_style = LS_DISC; else if (!strcasecmp(tk, "circle")) st->list_style = LS_CIRCLE;
            else if (!strcasecmp(tk, "square")) st->list_style = LS_SQUARE; else if (strcasestr(tk, "decimal") || strcasestr(tk, "alpha") || strcasestr(tk, "roman")) st->list_style = LS_DECIMAL;
        } return; }
    case P_FLEX_DIR: st->flex_dir = !strcasecmp(val, "column") ? FD_COL : !strcasecmp(val, "column-reverse") ? FD_COL_REV : !strcasecmp(val, "row-reverse") ? FD_ROW_REV : FD_ROW; return;
    case P_FLEX_WRAP: st->flex_wrap = strncasecmp(val, "nowrap", 6) != 0; return;
    case P_FLEX_FLOW: { const char* p = val; while (tok_next(&p, tk, sizeof tk)) { if (!strncasecmp(tk, "column", 6) || !strncasecmp(tk, "row", 3)) apply(c, n, st, P_FLEX_DIR, tk, 0); else apply(c, n, st, P_FLEX_WRAP, tk, 0); } return; }
    case P_FLEX_GROW: st->flex_grow = strtof(val, NULL); return;
    case P_FLEX_SHRINK: st->flex_shrink = strtof(val, NULL); return;
    case P_FLEX_BASIS: if (len_of(c, val, &l)) st->flex_basis = l; return;
    case P_FLEX: {
        if (!strcasecmp(val, "none")) { st->flex_grow = 0; st->flex_shrink = 0; memset(&st->flex_basis, 0, sizeof(Len)); return; }
        if (!strcasecmp(val, "auto")) { st->flex_grow = 1; st->flex_shrink = 1; memset(&st->flex_basis, 0, sizeof(Len)); return; }
        const char* p = val; int nn = 0; float g = 0, s = 1; Len b; memset(&b, 0, sizeof b); int have_b = 0;
        while (tok_next(&p, tk, sizeof tk)) {
            char* e; float f = strtof(tk, &e);
            if (e != tk && !*e && nn < 2) { if (nn == 0) g = f; else s = f; nn++; }
            else if (len_of(c, tk, &l)) { b = l; have_b = 1; }
        }
        st->flex_grow = g; st->flex_shrink = s;
        if (have_b) st->flex_basis = b; else { memset(&st->flex_basis, 0, sizeof(Len)); st->flex_basis.set = 1; }   /* «flex: 1» => basis 0 */
        return; }
    case P_JUSTIFY: st->justify = (!strcasecmp(val, "flex-end") || !strcasecmp(val, "end") || !strcasecmp(val, "right")) ? JC_END : !strcasecmp(val, "center") ? JC_CENTER : !strcasecmp(val, "space-between") ? JC_BETWEEN : !strcasecmp(val, "space-around") ? JC_AROUND : !strcasecmp(val, "space-evenly") ? JC_EVENLY : JC_START; return;
    case P_ALIGN_ITEMS: case P_ALIGN_SELF: {
        int a = (!strcasecmp(val, "flex-start") || !strcasecmp(val, "start") || !strcasecmp(val, "self-start")) ? AI_START : (!strcasecmp(val, "flex-end") || !strcasecmp(val, "end") || !strcasecmp(val, "self-end")) ? AI_END : !strcasecmp(val, "center") ? AI_CENTER : !strncasecmp(val, "baseline", 8) ? AI_BASELINE : !strcasecmp(val, "auto") ? AI_AUTO : AI_STRETCH;
        if (prop == P_ALIGN_ITEMS) st->align_items = (uint8_t)a; else st->align_self = (uint8_t)a; return; }
    case P_GAP: { const char* p = val; Len a, b; int k = 0; while (k < 2 && tok_next(&p, tk, sizeof tk)) { if (!len_of(c, tk, k ? &b : &a)) return; k++; } if (!k) return; st->gap_r = px_of(&a, 0); st->gap_c = k > 1 ? px_of(&b, 0) : st->gap_r; return; }
    case P_GRID_COLS: if (!strncasecmp(val, "none", 4)) st->grid_cols[0] = 0; else snprintf(st->grid_cols, sizeof st->grid_cols, "%s", val); return;
    case P_GRID_TEMPLATE: {                                          /* шорткат "строки / колонки"; строки (со списком имён областей в кавычках) идут в grid-template-areas */
        const char* sl = strrchr(val, '/');
        if (sl) { const char* p2 = sl + 1; while (isspace((unsigned char)*p2)) p2++; snprintf(st->grid_cols, sizeof st->grid_cols, "%s", p2); }
        if (strchr(val, '\'')) apply(c, n, st, P_GRID_AREAS, val, size_pass);
        return;
    }
    case P_GRID_AREAS: {                                             /* 'a a' 'b c' -> "a a\nb c" (без кавычек, по одной строке на ряд) */
        char* out = st->grid_areas; size_t cap = sizeof st->grid_areas, o = 0; out[0] = 0;
        const char* p2 = val; int row = 0;
        while (*p2 && o + 1 < cap) {
            while (*p2 && *p2 != '\'' && *p2 != '"') p2++;
            if (!*p2) break;
            char q = *p2++; const char* rs = p2; while (*p2 && *p2 != q) p2++;
            size_t rl = (size_t)(p2 - rs); if (*p2) p2++;
            if (row++) { if (o + 1 < cap) out[o++] = '\n'; }
            if (o + rl >= cap) rl = cap - o - 1;
            memcpy(out + o, rs, rl); o += rl; out[o] = 0;
        }
        return;
    }
    case P_GRID_AREA: { const char* p2 = val; while (isspace((unsigned char)*p2)) p2++; char* e = st->grid_area_name; size_t i = 0; while (p2[i] && !isspace((unsigned char)p2[i]) && p2[i] != '/' && i < sizeof st->grid_area_name - 1) { e[i] = p2[i]; i++; } e[i] = 0; return; }
    case P_GRID_COLUMN: {
        const char* sp = strcasestr(val, "span");
        if (sp) { int n = atoi(sp + 4); st->grid_span = (uint8_t)(n > 0 && n < 100 ? n : 1); }
        else { const char* sl = strchr(val, '/'); if (sl) { int a = atoi(val), b = atoi(sl + 1); st->grid_span = (uint8_t)(b < 0 ? 99 : (b > a ? b - a : 1)); } else st->grid_span = 1; }
        return; }
    case P_ROW_GAP: if (len_of(c, val, &l)) st->gap_r = px_of(&l, 0); return; case P_COL_GAP: if (len_of(c, val, &l)) st->gap_c = px_of(&l, 0); return;
    default: return;
    }
    (void)set_margin_auto;
}

/* ══════════════ каскад ══════════════ */
typedef struct { Decl* d; int nd; uint64_t key; } Grp;
typedef struct { Grp* v; int n, cap; } GV;
static void gv_push(GV* g, Decl* d, int nd, uint64_t key) {
    if (!d || !nd) return;
    if (g->n == g->cap) { g->cap = g->cap ? g->cap * 2 : 32; g->v = realloc(g->v, (size_t)g->cap * sizeof(Grp)); }
    g->v[g->n].d = d; g->v[g->n].nd = nd; g->v[g->n].key = key; g->n++;
}
static int grp_cmp(const void* a, const void* b) { uint64_t x = ((const Grp*)a)->key, y = ((const Grp*)b)->key; return x < y ? -1 : x > y; }
static uint64_t mkkey(int origin, uint32_t spec, int order) { return ((uint64_t)origin << 56) | ((uint64_t)spec << 24) | (uint64_t)(order & 0xFFFFFF); }

static void init_style(DOMNode* n, DOMNode* pa) {
    NodeStyle* st = &n->st; memset(st, 0, sizeof *st);
    st->display = DSP_INLINE; st->font_size = 16; st->color = 0xFF000000u; st->opacity = 1; st->flex_shrink = 1;
    st->spacing = 2; st->border_style[0] = st->border_style[1] = st->border_style[2] = st->border_style[3] = BS_NONE;
    for (int i = 0; i < 4; i++) { st->border_w[i] = 3; st->border_c[i] = CUR_COLOR; }
    st->sh_color = 0; st->list_style = LS_DISC; st->align_self = AI_AUTO;
    for (int i = 0; i < 4; i++) { st->margin[i].set = 1; st->padding[i].set = 1; }
    st->grid_span = 1;
    if (pa) {
        const NodeStyle* ps = &pa->st;
        st->color = ps->color; st->font_size = ps->font_size; st->lh_px = ps->lh_px; st->lh_mult = ps->lh_mult; st->bold = ps->bold; st->weight = ps->weight; st->italic = ps->italic;
        st->text_align = ps->text_align; st->white_space = ps->white_space; st->text_transform = ps->text_transform; st->visibility = ps->visibility;
        st->center_blocks = ps->center_blocks; st->list_style = ps->list_style; st->pointer = ps->pointer; st->break_word = ps->break_word; st->spacing = ps->spacing;
        st->deco = ps->deco; snprintf(n->font_family, sizeof n->font_family, "%s", pa->font_family);
        /* фон-градиент и background-clip:text передаём вниз на анонимные текстовые узлы,
           иначе они не видят фон, объявленный на родительском элементе (h1, span и т.п.) */
        st->bg_clip_text = ps->bg_clip_text;
        st->bg_grad_type = ps->bg_grad_type; st->bg_grad_n = ps->bg_grad_n; st->bg_grad_angle = ps->bg_grad_angle;
        memcpy(st->bg_grad_c, ps->bg_grad_c, sizeof st->bg_grad_c); memcpy(st->bg_grad_p, ps->bg_grad_p, sizeof st->bg_grad_p);
    }
}

/* Правила, подходящие узлу. pseudo: 0 — для самого элемента, 1/2 — для его ::before/::after */
static void gather_rules(Doc* d, RIndex* ix, DOMNode* n, GV* g, int pseudo) {
    Rule** cand[64]; int ncand[64]; int nc = 0; char key[80];
    if (n->id[0]) { snprintf(key, sizeof key, "#%s", n->id); HB* b = hb_get(ix, key, 0); if (b) { cand[nc] = b->v; ncand[nc++] = b->n; } }
    { const char* p = n->class_name; while (*p && nc < 60) {
        while (isspace((unsigned char)*p)) p++; const char* e = p; while (*e && !isspace((unsigned char)*e)) e++;
        if (e > p && e - p < 70) { key[0] = '.'; memcpy(key + 1, p, (size_t)(e - p)); key[1 + (e - p)] = 0; HB* b = hb_get(ix, key, 0); if (b) { cand[nc] = b->v; ncand[nc++] = b->n; } }
        p = e; } }
    { snprintf(key, sizeof key, "T%s", n->tag); HB* b = hb_get(ix, key, 0); if (b) { cand[nc] = b->v; ncand[nc++] = b->n; } }
    cand[nc] = ix->uni; ncand[nc++] = ix->nuni;
    for (int c = 0; c < nc; c++) for (int i = 0; i < ncand[c]; i++) {
        Rule* r = cand[c][i];
        if (r->pseudo != pseudo) continue;
        if (match_at(d, n, r->sel, r->nsel - 1)) gv_push(g, r->decls, r->ndecls, mkkey(r->origin, r->spec, r->order));
    }
}

/* Применяет отсортированные группы деклараций к узлу n (custom-свойства, font-size, затем остальное; обычные, затем !important) */
static void apply_groups(Doc* d, DOMNode* n, DOMNode* pa, GV* g, float root_fs, CP** cps, char* content_out, DOMNode* owner) {
    Cx cx; cx.parent = pa; cx.pfs = pa ? pa->st.font_size : 16; cx.rem = root_fs; cx.em = cx.pfs; cx.cur = n->st.color;
    cx.content = content_out; cx.has_content = 0; cx.owner = owner;
    (void)d;
    /* 1) пользовательские свойства --x (у ::before/::after свои --x не поддерживаются: берём цепочку владельца) */
    if (!content_out) for (int imp = 0; imp < 2; imp++) for (int i = 0; i < g->n; i++) for (int k = 0; k < g->v[i].nd; k++) {
        Decl* dc = &g->v[i].d[k]; if (dc->prop != P_CUSTOM || dc->imp != imp) continue;
        CP* cp = malloc(sizeof(CP)); cp->name = dc->name; cp->val = dc->val; cp->next = *cps; *cps = cp;   /* name/val живут в арене — освободим после каскада */
    }
    /* 2) font-size, затем всё остальное; обычные, затем !important */
    char resolved[1024];
    for (int pass = 0; pass < 2; pass++) for (int imp = 0; imp < 2; imp++) {
        if (pass == 1 && imp == 0) { cx.em = n->st.font_size; }
        for (int i = 0; i < g->n; i++) for (int k = 0; k < g->v[i].nd; k++) {
            Decl* dc = &g->v[i].d[k]; if (dc->imp != imp || dc->prop == P_CUSTOM) continue;
            const char* v = dc->val;
            if (strstr(v, "var(")) { if (!resolve_vars(*cps, v, resolved, sizeof resolved, 0)) continue; v = resolved; }
            if (pass == 0) cx.em = cx.pfs; else { cx.em = n->st.font_size; cx.cur = n->st.color; }
            apply(&cx, n, &n->st, dc->prop, v, pass == 0);
        }
    }
    if (content_out) { if (!cx.has_content) content_out[0] = 0; owner->aux = owner->aux; }
    /* пометка «content задан» передаётся через content_out[247]: 1 — есть */
    if (content_out) content_out[247] = (char)(cx.has_content ? 1 : 0);
}

/* Пост-обработка вычисленного стиля (общая для элементов и псевдоэлементов) */
static void finish_style(DOMNode* n, DOMNode* pa) {
    NodeStyle* st = &n->st;
    if ((st->float_ != FLT_NONE || st->position >= POS_ABSOLUTE) && st->display != DSP_NONE) {
        if (st->display == DSP_INLINE || st->display == DSP_INLINE_BLOCK || st->display == DSP_CELL || st->display == DSP_ROW || st->display == DSP_ROW_GROUP) st->display = DSP_BLOCK;
        else if (st->display == DSP_INLINE_FLEX) st->display = DSP_FLEX;
    }
    if (st->float_ != FLT_NONE && st->position >= POS_ABSOLUTE) st->float_ = FLT_NONE;
    for (int i = 0; i < 4; i++) {
        if (st->border_style[i] == BS_NONE) st->border_w[i] = 0;
        if (st->border_c[i] == CUR_COLOR) st->border_c[i] = st->color;
    }
    if ((st->sh_color & 0xFF000000u) == 0x01000000u) st->sh_color = st->color;
    if (st->bg_url[0] && strncmp(st->bg_url, "data:", 5) != 0 && strncmp(st->bg_url, "cache/data_", 11) != 0) { char ab[512]; webpage_resolve_url(g_base, st->bg_url, ab, sizeof ab); snprintf(st->bg_url, sizeof st->bg_url, "%s", ab); }
    if (pa) { st->opacity *= pa->st.opacity; if (st->display == DSP_INLINE_BLOCK || st->float_ || st->position >= POS_ABSOLUTE) st->deco = st->deco & 0; if (pa->st.deco && st->display != DSP_INLINE_BLOCK && !st->float_ && st->position < POS_ABSOLUTE) st->deco |= pa->st.deco; }
}

/* ::before / ::after: свой вычисленный стиль (наследует от элемента) + разобранный content. Хранится в n->pb[which-1]. */
static void cascade_pseudo(Doc* d, RIndex* ix, DOMNode* n, int which, float root_fs) {
    GV g = {0}; gather_rules(d, ix, n, &g, which);
    if (!g.n) { free(g.v); return; }
    if (g.n > 1) qsort(g.v, (size_t)g.n, sizeof(Grp), grp_cmp);
    PseudoBox* pb = calloc(1, sizeof *pb); DOMNode* tmp = calloc(1, sizeof(DOMNode));
    if (!pb || !tmp) { free(pb); free(tmp); free(g.v); return; }
    init_style(tmp, n);
    CP* cps = d->cps[n->idx];
    apply_groups(d, tmp, n, &g, root_fs, &cps, pb->content, n);
    free(g.v);
    int has = pb->content[247] == 1; pb->content[247] = 0;
    NodeStyle* st = &tmp->st;
    if (has && st->display != DSP_NONE) {
        finish_style(tmp, n);
        pb->st = tmp->st; memcpy(pb->font_family, tmp->font_family, sizeof pb->font_family);
        n->pb[which - 1] = pb;
    } else free(pb);
    free(tmp);
}

static int pseudo_capable(const DOMNode* n) {
    switch (n->type) { case TAG_IMG: case TAG_INPUT: case TAG_TEXTAREA: case TAG_SELECT: case TAG_BR: case TAG_HR: case TAG_TEXT: case TAG_IFRAME: case TAG_VIDEO: case TAG_AUDIO: case TAG_CANVAS: case TAG_SVG: return 0; default: break; }
    return strcmp(n->tag, "hr") && strcmp(n->tag, "object") && strcmp(n->tag, "embed") && strcmp(n->tag, "svg");
}

static void cascade_node(Doc* d, RIndex* ix, RuleSet* rs, DOMNode* n, float root_fs) {
    (void)rs;
    DOMNode* pa = par(d, n);
    dom_free_pseudo(n);
    init_style(n, pa);
    d->cps[n->idx] = pa ? d->cps[pa->idx] : NULL;
    if (n->type == TAG_TEXT) { n->st.display = DSP_INLINE; return; }

    GV g = {0}; Arena tmp = {0};
    gather_rules(d, ix, n, &g, 0);
    if (n->pres[0]) { int nd = 0; Decl* pd = css_parse_decls(&tmp, n->pres, strlen(n->pres), &nd); gv_push(&g, pd, nd, mkkey(1, 0, 0)); }
    if (n->style_str[0]) { int nd = 0; Decl* sd = css_parse_decls(&tmp, n->style_str, strlen(n->style_str), &nd); gv_push(&g, sd, nd, mkkey(3, 0, 0)); }
    if (g.n > 1) qsort(g.v, (size_t)g.n, sizeof(Grp), grp_cmp);
    apply_groups(d, n, pa, &g, root_fs, &d->cps[n->idx], NULL, n);
    free(g.v); afree(&tmp);

    NodeStyle* st = &n->st;
    if (n->idx == 0) st->display = DSP_BLOCK;
    if (st->display == DSP_NONE) return;
    finish_style(n, pa);
    if (ix->has_pseudo && pseudo_capable(n)) { cascade_pseudo(d, ix, n, 1, root_fs); cascade_pseudo(d, ix, n, 2, root_fs); }
}

static int g_style_offline = 0;
void style_apply(DOMNode* head, SheetList* sheets, const char* base_url) {
    Arena A = {0}; RuleSet rs = {0}; RIndex ix; memset(&ix, 0, sizeof ix);
    css_fetch_budget = g_style_offline ? 0 : 12; g_base = base_url; webpage_set_doc_url(base_url);
    css_parse_sheet(&A, &rs, UA_CSS, NULL, 0, 0);
    int nsheet = 0;
    for (int i = 0; i < sheets->n; i++) {
        SheetSrc* s = &sheets->v[i];
        if (!css_media_matches(s->media)) continue;
        if (!s->css && s->href && nsheet < 10 && css_fetch_budget > 0 && !strncmp(s->href, "http", 4) && !webpage_mixed_blocked(s->href)) {      /* скачиваем и сохраняем текст в бандле */
            css_fetch_budget--; nsheet++;
            s->css = nal_http_get(s->href);
        }
        if (s->css) css_parse_sheet(&A, &rs, s->css, s->href ? s->href : base_url, 2, 0);
    }
    index_rules(&ix, &rs);

    int n = 0; for (DOMNode* p = head; p; p = p->next) n++;
    Doc d; d.n = n; d.nv = malloc((size_t)n * sizeof(DOMNode*)); d.prev = malloc((size_t)n * sizeof(int));
    d.eidx = calloc((size_t)n, sizeof(int)); d.ecnt = calloc((size_t)n, sizeof(int)); d.cps = calloc((size_t)n, sizeof(CP*));
    int* lastel = malloc((size_t)n * sizeof(int));
    { int i = 0; for (DOMNode* p = head; p; p = p->next) d.nv[i++] = p; }
    for (int i = 0; i < n; i++) lastel[i] = -1;
    for (int i = 0; i < n; i++) {
        DOMNode* p = d.nv[i]; d.prev[i] = -1;
        if (p->type == TAG_TEXT || p->parent < 0) continue;
        d.prev[i] = lastel[p->parent]; lastel[p->parent] = i;
        d.ecnt[p->parent]++; d.eidx[i] = d.ecnt[p->parent];
    }
    free(lastel);

    float root_fs = 16;
    for (int i = 0; i < n;) {
        DOMNode* p = d.nv[i];
        cascade_node(&d, &ix, &rs, p, root_fs);
        if (i == 0) root_fs = p->st.font_size;
        if (p->type != TAG_TEXT && p->st.display == DSP_NONE) { i = p->subtree_end + 1; continue; }   /* поддерево скрыто */
        i++;
    }
    /* освобождаем CP-цепочки (узлы, аллоцированные malloc) */
    for (int i = 0; i < n; i++) {
        CP* c = d.cps[i]; DOMNode* pa = d.nv[i]->parent >= 0 ? d.nv[d.nv[i]->parent] : NULL; CP* stop = pa ? d.cps[pa->idx] : NULL;
        (void)stop; (void)c;
    }
    { /* каждая цепочка = собственные записи + указатель на родительскую; освобождаем по собственным */
        for (int i = n - 1; i >= 0; i--) {
            CP* c = d.cps[i]; CP* pc = d.nv[i]->parent >= 0 ? d.cps[d.nv[i]->parent] : NULL;
            while (c && c != pc) { CP* nx = c->next; free(c); c = nx; }
            d.cps[i] = NULL;                                                /* родитель i-го уже обработан позже (i-1 < i) — порядок безопасен */
        }
    }
    free(d.nv); free(d.prev); free(d.eidx); free(d.ecnt); free(d.cps);
    index_free(&ix); free(rs.v); afree(&A);
}

/* Пересчёт каскада после изменений DOM скриптом: без сети, стили берутся из бандла. */
void webpage_recascade(DOMNode* head, SheetList* sheets, const char* base_url) {
    g_style_offline = 1; font_manager_set_offline(1);
    style_apply(head, sheets, base_url);
    g_style_offline = 0; font_manager_set_offline(0);
}

/* ══════════════ селекторы для JS ══════════════ */
struct WebSel { Arena a; Simple** sel; int* nsel; int n; };

WebSel* websel_compile(const char* text) {
    WebSel* w = calloc(1, sizeof *w); if (!w) return NULL;
    size_t len = strlen(text); const char* s = text; const char* end = text + len; int cap = 0;
    while (s < end) {
        const char* c = s; int d = 0; char q = 0;
        while (c < end) { if (q) { if (*c == q) q = 0; } else if (*c == '"' || *c == '\'') q = *c; else if (*c == '(' || *c == '[') d++; else if (*c == ')' || *c == ']') d--; else if (*c == ',' && d == 0) break; c++; }
        const char* ss = s; while (ss < c && isspace((unsigned char)*ss)) ss++;
        const char* se = c; while (se > ss && isspace((unsigned char)se[-1])) se--;
        if (se == ss) { websel_free(w); return NULL; }
        int ns = 0; Simple* sp = css_parse_selector_ex(&w->a, ss, (size_t)(se - ss), &ns);
        if (!sp) { websel_free(w); return NULL; }
        if (w->n == cap) { cap = cap ? cap * 2 : 4; w->sel = realloc(w->sel, (size_t)cap * sizeof(Simple*)); w->nsel = realloc(w->nsel, (size_t)cap * sizeof(int)); }
        w->sel[w->n] = sp; w->nsel[w->n] = ns; w->n++;
        s = c + 1;
    }
    if (!w->n) { websel_free(w); return NULL; }
    return w;
}
void websel_free(WebSel* w) { if (!w) return; afree(&w->a); free(w->sel); free(w->nsel); free(w); }

void* webdoc_build(DOMNode* head) {
    int n = 0; for (DOMNode* p = head; p; p = p->next) n++;
    Doc* d = calloc(1, sizeof *d); if (!d) return NULL;
    d->n = n; d->nv = malloc((size_t)n * sizeof(DOMNode*)); d->prev = malloc((size_t)n * sizeof(int));
    d->eidx = calloc((size_t)n, sizeof(int)); d->ecnt = calloc((size_t)n, sizeof(int)); d->cps = NULL;
    int* lastel = malloc((size_t)n * sizeof(int));
    { int i = 0; for (DOMNode* p = head; p; p = p->next) d->nv[i++] = p; }
    for (int i = 0; i < n; i++) lastel[i] = -1;
    for (int i = 0; i < n; i++) {
        DOMNode* p = d->nv[i]; d->prev[i] = -1;
        if (p->type == TAG_TEXT || p->parent < 0) continue;
        d->prev[i] = lastel[p->parent]; lastel[p->parent] = i; d->ecnt[p->parent]++; d->eidx[i] = d->ecnt[p->parent];
    }
    free(lastel); return d;
}
void webdoc_free(void* doc) { Doc* d = doc; if (!d) return; free(d->nv); free(d->prev); free(d->eidx); free(d->ecnt); free(d); }
int websel_match(void* doc, WebSel* w, DOMNode* n) {
    Doc* d = doc; if (!d || !w || !n || n->type == TAG_TEXT || n->idx < 0 || n->idx >= d->n) return 0;
    for (int i = 0; i < w->n; i++) if (match_at(d, n, w->sel[i], w->nsel[i] - 1)) return 1;
    return 0;
}
