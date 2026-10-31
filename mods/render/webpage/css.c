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
static int g_allow_pseudo = 0;                    /* enabled for stylesheet rules and selector() */

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
    if (n == strlen("-webkit-line-clamp") && !strncasecmp(name, "-webkit-line-clamp", n)) return P_WEBKIT_LINE_CLAMP;
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

const RegisteredProperty* css_registered_property(const RuleSet* rs, const char* name) {
    if (!rs || !name) return NULL;
    for (const RegisteredProperty* r=rs->properties; r; r=r->next)
        if (!strcmp(r->name,name)) return r;
    return NULL;
}

static int registered_atom_valid(const char* syntax, const char* value) {
    char v[256]; size_t n=strlen(value);
    while (*value && isspace((unsigned char)*value)) value++;
    n=strlen(value); while(n && isspace((unsigned char)value[n-1])) n--;
    if (!n || n>=sizeof v) return 0;
    memcpy(v,value,n); v[n]=0;
    if (!strcmp(syntax,"*")) return 1;
    if (!strcmp(syntax,"<color>")) { uint32_t c; return css_parse_color(v,&c,0xFF000000u); }
    if (!strcmp(syntax,"<length>") || !strcmp(syntax,"<length-percentage>") || !strcmp(syntax,"<percentage>")) {
        Len l={0}; if (!css_parse_len(v,&l,16,16)) return 0;
        if (!strcmp(syntax,"<percentage>")) return strchr(v,'%')!=NULL;
        if (!strcmp(syntax,"<length>")) return l.pct==0 && !strchr(v,'%');
        return 1;
    }
    if (!strcmp(syntax,"<number>")) {
        char* e=NULL; double x=strtod(v,&e); return e!=v && *e==0 && isfinite(x);
    }
    if (!strcmp(syntax,"<integer>")) {
        char* e=NULL; (void)strtol(v,&e,10); return e!=v && *e==0;
    }
    if (!strcmp(syntax,"<angle>")) {
        char* e=NULL; double x=strtod(v,&e); if(e==v || !isfinite(x))return 0;
        return !strcasecmp(e,"deg") || !strcasecmp(e,"rad") || !strcasecmp(e,"grad") || !strcasecmp(e,"turn");
    }
    if (!strcmp(syntax,"<time>")) {
        char* e=NULL; double x=strtod(v,&e); if(e==v || !isfinite(x))return 0;
        return !strcasecmp(e,"s") || !strcasecmp(e,"ms");
    }
    if (!strcmp(syntax,"<custom-ident>")) {
        if (!strcasecmp(v,"initial")||!strcasecmp(v,"inherit")||!strcasecmp(v,"unset")||!strcasecmp(v,"revert")||!strcasecmp(v,"default")) return 0;
        if (!(isalpha((unsigned char)v[0]) || v[0]=='_' || (unsigned char)v[0]>=0x80)) return 0;
        for (const unsigned char* p=(const unsigned char*)v+1; *p; p++)
            if (!(isalnum(*p)||*p=='_'||*p=='-'||*p>=0x80)) return 0;
        return 1;
    }
    if (!strcmp(syntax,"<string>")) return (v[0]=='\'' || v[0]=='"') && n>=2 && v[n-1]==v[0];
    return 0;
}

int css_registered_value_valid(const RegisteredProperty* registration, const char* value) {
    if (!registration || !value) return 0;
    /* Computed values containing var() are validated after substitution by the consuming property. */
    if (strstr(value,"var(")) return 1;
    const char* syntax=registration->syntax;
    if (!strcmp(syntax,"*")) return 1;
    /* Support syntax alternatives (for example <length> | <percentage>) and
       the single-token repetition multipliers '+' / '#'. */
    const char* p=syntax;
    while (*p) {
        const char* sep=strchr(p,'|'); const char* e=sep?sep:p+strlen(p);
        while(p<e&&isspace((unsigned char)*p))p++;
        while(e>p&&isspace((unsigned char)e[-1]))e--;
        char atom[96]; size_t an=(size_t)(e-p); int list=0;
        if(an&& (p[an-1]=='+'||p[an-1]=='#')) { list= p[an-1]=='#' ? 2 : 1; an--; }
        if(an>0&&an<sizeof atom) {
            memcpy(atom,p,an); atom[an]=0;
            if (!list && registered_atom_valid(atom,value)) return 1;
            if (list) {
                char tmp[512]; size_t vl=strlen(value);
                if(vl<sizeof tmp) {
                    memcpy(tmp,value,vl+1); int ok=1, count=0; char* q=tmp;
                    while(*q) {
                        while(isspace((unsigned char)*q) || (list==2&&*q==',')) q++;
                        if(!*q)break;
                        char* start=q; while(*q && !isspace((unsigned char)*q) && (list!=2||*q!=',')) q++;
                        char hold=*q; *q=0; if(!registered_atom_valid(atom,start))ok=0; *q=hold; count++;
                        if(*q)q++;
                    }
                    if(ok&&count>0)return 1;
                }
            }
        }
        p=sep?sep+1:e;
    }
    return 0;
}

static int css_valid_property_name(const char* s) {
    if (!s || s[0]!='-' || s[1]!='-' || !s[2]) return 0;
    for (const unsigned char* p=(const unsigned char*)s+2; *p; p++)
        if (!(isalnum(*p)||*p=='_'||*p=='-'||*p>=0x80)) return 0;
    return 1;
}

static void css_parse_property_rule(Arena* a, RuleSet* rs, const char* prelude, const char* body, size_t len) {
    if (!a || !rs || !prelude) return;
    while(isspace((unsigned char)*prelude))prelude++;
    const char* ne=prelude; while(*ne&&!isspace((unsigned char)*ne))ne++;
    char name[64]; size_t nn=(size_t)(ne-prelude);
    if(!nn||nn>=sizeof name)return;
    memcpy(name,prelude,nn); name[nn]=0;
    while(isspace((unsigned char)*ne))ne++;
    if(*ne || !css_valid_property_name(name))return;
    char syntax[96]="", inherits[16]="", initial[256]=""; int has_syntax=0,has_inherits=0,has_initial=0;
    const char* p=body; const char* end=body+len;
    while(p<end) {
        while(p<end&&(isspace((unsigned char)*p)||*p==';'))p++;
        if(p>=end)break;
        const char* ns=p; while(p<end&&(isalnum((unsigned char)*p)||*p=='-'))p++;
        size_t nl=(size_t)(p-ns); while(p<end&&isspace((unsigned char)*p))p++;
        if(p>=end||*p!=':') { while(p<end&&*p!=';')p++; continue; }
        p++; while(p<end&&isspace((unsigned char)*p))p++;
        const char* vs=p; char quote=0; int depth=0;
        while(p<end) { char c=*p; if(quote){if(c==quote && (p==vs || p[-1]!='\\'))quote=0;} else if(c=='\''||c=='"')quote=c; else if(c=='(')depth++; else if(c==')'&&depth)depth--; else if(c==';'&&depth==0)break; p++; }
        const char* ve=p; while(ve>vs&&isspace((unsigned char)ve[-1]))ve--; if(p<end&&*p==';')p++;
        size_t vl=(size_t)(ve-vs);
        if(nl==6&&!strncasecmp(ns,"syntax",6)&&vl>=2&&vl<sizeof syntax&&(vs[0]=='\''||vs[0]=='"')&&ve[-1]==vs[0]) { memcpy(syntax,vs+1,vl-2);syntax[vl-2]=0;has_syntax=1; }
        else if(nl==8&&!strncasecmp(ns,"inherits",8)&&vl<sizeof inherits) { memcpy(inherits,vs,vl);inherits[vl]=0;has_inherits=1; }
        else if(nl==13&&!strncasecmp(ns,"initial-value",13)&&vl<sizeof initial) { memcpy(initial,vs,vl);initial[vl]=0;has_initial=1; }
    }
    if(!has_syntax||!has_inherits||(strcasecmp(inherits,"true")&&strcasecmp(inherits,"false")))return;
    if(strcmp(syntax,"*")!=0) {
        RegisteredProperty check={0}; snprintf(check.syntax,sizeof check.syntax,"%s",syntax);
        if(!has_initial || !initial[0] || strstr(initial,"var(") || !css_registered_value_valid(&check,initial))return;
    }
    RegisteredProperty* r=aalloc(a,sizeof *r); if(!r)return;
    memcpy(r->name,name,nn+1); snprintf(r->syntax,sizeof r->syntax,"%s",syntax);
    r->initial_value=has_initial?astrdup(a,initial,strlen(initial)):NULL;
    r->inherits=!strcasecmp(inherits,"true");
    r->next=rs->properties; rs->properties=r;
}

/* CSS Conditional Rules: conservative @supports evaluator. Unsupported declarations
 * are false instead of the old behavior that treated nearly every condition as true. */
static void css_trim_span(const char **s, size_t *n) {
    while (*n && isspace((unsigned char)**s)) { (*s)++; (*n)--; }
    while (*n && isspace((unsigned char)(*s)[*n - 1])) (*n)--;
}
static int css_outer_parens(const char *s, size_t n) {
    if (n < 2 || s[0] != '(' || s[n-1] != ')') return 0;
    int d = 0; char quote = 0; int esc = 0;
    for (size_t i=0; i<n; i++) {
        char c=s[i];
        if (quote) { if (esc) esc=0; else if (c=='\\') esc=1; else if (c==quote) quote=0; continue; }
        if (c=='\'' || c=='"') { quote=c; continue; }
        if (c=='(') d++;
        else if (c==')') { if (--d < 0) return 0; if (d==0 && i != n-1) return 0; }
    }
    return d==0 && !quote;
}
static int css_supported_value(int prop, const char *v) {
    while (isspace((unsigned char)*v)) v++;
    size_t n=strlen(v); while(n && isspace((unsigned char)v[n-1])) n--;
    if (!n) return 0;
    char val[256]; if (n>=sizeof val) return 0; memcpy(val,v,n); val[n]=0;
    if (!strcasecmp(val,"inherit") || !strcasecmp(val,"initial") || !strcasecmp(val,"unset") || !strcasecmp(val,"revert")) return 1;
    switch (prop) {
      case P_GRID_COLS:
        if (strcasestr(val, "masonry")) return 0;
        if (strcasestr(val, "subgrid")) return 1; /* direct nested column tracks are inherited */
        return 1;
      case P_GRID_ROWS:
        if (strcasestr(val, "masonry")) return 0;
        if (strcasestr(val, "subgrid")) return !strcasestr(val, "repeat(");
        return 1;
      case P_GRID_TEMPLATE: {
        if (strcasestr(val, "masonry")) return 0;
        const char* slash=strchr(val,'/');
        if (strcasestr(val,"subgrid")) {
            if (!slash) return 0;
            size_t row_n=(size_t)(slash-val); while(row_n && isspace((unsigned char)val[row_n-1]))row_n--;
            const char* row=val; while(*row && isspace((unsigned char)*row))row++;
            int rows_subgrid = row_n >= (size_t)(row-val)+7 && !strncasecmp(row,"subgrid",7);
            int cols_subgrid = strcasestr(slash+1,"subgrid")!=NULL;
            return rows_subgrid || cols_subgrid;
        }
        return 1;
      }
      case P_COLUMN_COUNT: {
        if (!strcasecmp(val,"auto")) return 1;
        char* e=NULL; long x=strtol(val,&e,10); while(e && isspace((unsigned char)*e))e++;
        return e && e!=val && !*e && x>0 && x<=32;
      }
      case P_COLUMN_WIDTH: {
        if (!strcasecmp(val,"auto")) return 1; Len l; return css_parse_len(val,&l,16,16) && l.set && (l.px+l.pct+l.vw+l.vh)>0;
      }
      case P_COLUMNS:
        return !strcasestr(val,"subgrid") && !strcasestr(val,"masonry");
      case P_TEXT_ORIENTATION:
        return !strcasecmp(val,"upright"); /* mixed requires sideways Latin glyph orientation */
      case P_TEXT_COMBINE_UPRIGHT:
        return !strcasecmp(val,"none") || !strncasecmp(val,"digits",6); /* only ASCII digit runs are combined by layout */
      case P_ANIMATION_TIMELINE:
        return !strcasecmp(val,"auto") || !strcasecmp(val,"none") || strcasestr(val,"scroll(") || strcasestr(val,"view(");
      case P_ANIMATION_COMPOSITION:
        return !strcasecmp(val,"replace") || !strcasecmp(val,"add") || !strcasecmp(val,"accumulate");
      case P_ANIMATION_RANGE: {
        if (!strcasecmp(val,"normal")) return 1;
        char* e=NULL; (void)strtof(val,&e); return e && e!=val && (strchr(val,'%') || !*e || isspace((unsigned char)*e));
      }
      case P_ANCHOR_NAME: case P_POSITION_ANCHOR:
        return !strcasecmp(val,"none") || (val[0]=='-' && val[1]=='-');
      case P_POSITION_AREA:
        return strcasestr(val,"top") || strcasestr(val,"bottom") || strcasestr(val,"left") || strcasestr(val,"right") || strcasestr(val,"center");
      case P_VIEW_TRANSITION_NAME: {
        if (!strcasecmp(val, "none")) return 1;
        if (!*val || !strcasecmp(val, "auto") || !strcasecmp(val, "match-element")) return 0;
        for (const unsigned char* p = (const unsigned char*)val; *p; p++) {
            if (isspace(*p) || strchr(",()\'\"", *p)) return 0;
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' || *p == '-' || *p >= 0x80 || (*p >= '0' && *p <= '9' && p != (const unsigned char*)val))) return 0;
        }
        return 1; /* named viewport snapshots/shared-element movement are implemented as a bounded subset */
      }
      case P_DISPLAY:
        return !strcasecmp(val,"none") || !strcasecmp(val,"inline") || !strcasecmp(val,"block") ||
          !strcasecmp(val,"inline-block") || !strcasecmp(val,"flex") || !strcasecmp(val,"inline-flex") ||
          !strcasecmp(val,"grid") || !strcasecmp(val,"inline-grid") || !strcasecmp(val,"list-item") ||
          !strcasecmp(val,"table") || !strcasecmp(val,"table-row") || !strcasecmp(val,"table-cell") ||
          !strcasecmp(val,"table-row-group") || !strcasecmp(val,"table-caption") || !strcasecmp(val,"contents") ||
          !strcasecmp(val,"flow-root");
      case P_POSITION:
        return !strcasecmp(val,"static") || !strcasecmp(val,"relative") || !strcasecmp(val,"absolute") ||
          !strcasecmp(val,"fixed") || !strcasecmp(val,"sticky");
      case P_WRITING_MODE:
        return !strcasecmp(val,"horizontal-tb") || !strcasecmp(val,"vertical-rl") || !strcasecmp(val,"vertical-lr");
      case P_WHITE_SPACE:
        return !strcasecmp(val,"normal") || !strcasecmp(val,"nowrap") || !strcasecmp(val,"pre") ||
          !strcasecmp(val,"pre-wrap") || !strcasecmp(val,"pre-line") || !strcasecmp(val,"break-spaces");
      case P_TEXT_WRAP:
        return !strcasecmp(val,"wrap") || !strcasecmp(val,"nowrap") || !strcasecmp(val,"balance") || !strcasecmp(val,"pretty");
      case P_CONTENT_VISIBILITY:
        return !strcasecmp(val,"visible") || !strcasecmp(val,"auto") || !strcasecmp(val,"hidden");
      case P_CONTAIN_INTRINSIC_SIZE: {
        if (!strcasecmp(val,"none")) return 1;
        const char* p=val; char t[128]; int got=0; while (*p) { while(isspace((unsigned char)*p))p++; if(!*p)break; size_t n=0; while(*p&&!isspace((unsigned char)*p)&&n+1<sizeof t)t[n++]=*p++;t[n]=0; if(!strcasecmp(t,"auto"))continue; Len l;if(!css_parse_len(t,&l,16,16))return 0;got++; }
        return got>0;
      }
      case P_CONTAINER_TYPE:
        return !strcasecmp(val,"normal") || !strcasecmp(val,"size") || !strcasecmp(val,"inline-size");
      case P_CONTAINER:
        return 1; /* shorthand is parsed by the style layer */
      case P_COLOR: case P_BG_COLOR: case P_BORDER_COLOR: {
        uint32_t col; return css_parse_color(val,&col,0xFF000000u);
      }
      case P_WIDTH: case P_HEIGHT: case P_MIN_W: case P_MAX_W: case P_MIN_H: case P_MAX_H:
      case P_INLINE_SIZE: case P_BLOCK_SIZE: case P_MIN_INLINE_SIZE: case P_MAX_INLINE_SIZE:
      case P_MIN_BLOCK_SIZE: case P_MAX_BLOCK_SIZE: {
        Len l; if (!strcasecmp(val,"auto") || !strcasecmp(val,"min-content") || !strcasecmp(val,"max-content") || !strcasecmp(val,"fit-content")) return 1;
        return css_parse_len(val,&l,16,16);
      }
      default: return 1;
    }
}
static int css_supports_decl(const char *s, size_t n) {
    css_trim_span(&s,&n); if (!n || n>511) return 0;
    int d=0; char quote=0; int esc=0; const char *colon=NULL;
    for (size_t i=0;i<n;i++) {
        char c=s[i];
        if (quote) { if(esc)esc=0; else if(c=='\\')esc=1; else if(c==quote)quote=0; continue; }
        if(c=='\''||c=='"'){quote=c;continue;}
        if(c=='(')d++; else if(c==')'){if(--d<0)return 0;}
        else if(c==':' && d==0){colon=s+i;break;}
    }
    if (!colon || d<0) return 0;
    const char *ns=s,*ne=colon,*vs=colon+1; size_t nl=(size_t)(ne-ns), vl=n-(size_t)(vs-s);
    while(nl && isspace((unsigned char)*ns)){ns++;nl--;}
    while(nl && isspace((unsigned char)ns[nl-1]))nl--;
    css_trim_span(&vs,&vl);
    if (!nl || !vl) return 0;
    if (nl>=2 && ns[0]=='-' && ns[1]=='-') return 1; /* all custom properties are syntactically valid */
    int prop=css_prop_id(ns,nl);
    if (prop==P_NONE || prop==P_CUSTOM) return 0;
    char vcopy[512]; if (vl>=sizeof vcopy) return 0; memcpy(vcopy,vs,vl); vcopy[vl]=0;
    return css_supported_value(prop,vcopy);
}
static int css_supports_eval(const char *s, size_t n, int depth) {
    if (depth>16) return 0;
    css_trim_span(&s,&n); if(!n) return 0;
    if (css_outer_parens(s,n)) return css_supports_eval(s+1,n-2,depth+1);
    if (n>=4 && !strncasecmp(s,"not",3) && isspace((unsigned char)s[3])) {
        int v=css_supports_eval(s+4,n-4,depth+1); return !v;
    }
    /* split at top-level logical operators; `and` binds more tightly than `or`. */
    for (int want_or=1; want_or>=0; want_or--) {
        int d=0; char quote=0; int esc=0;
        for(size_t i=0;i<n;i++) {
            char c=s[i];
            if(quote){if(esc)esc=0;else if(c=='\\')esc=1;else if(c==quote)quote=0;continue;}
            if(c=='\''||c=='"'){quote=c;continue;}
            if(c=='('){d++;continue;} if(c==')'){if(d==0)return 0;d--;continue;}
            if(d)continue;
            const char *op=want_or?"or":"and"; size_t ol=want_or?2:3;
            if(i+ol<=n && !strncasecmp(s+i,op,ol) && (i==0||isspace((unsigned char)s[i-1])) && (i+ol==n||isspace((unsigned char)s[i+ol]))) {
                int a=css_supports_eval(s,i,depth+1), b=css_supports_eval(s+i+ol,n-i-ol,depth+1);
                return want_or ? (a||b) : (a&&b);
            }
        }
        if(d!=0 || quote)return 0;
    }
    if (n>=9 && !strncasecmp(s,"selector(",9) && s[n-1]==')') {
        const char *q=s+9; size_t qn=n-10; css_trim_span(&q,&qn); if(!qn)return 0;
        Arena a={0}; int nsel=0; int old_allow=g_allow_pseudo; g_allow_pseudo=1;
        Simple *sel=css_parse_selector_ex(&a,q,qn,&nsel); g_allow_pseudo=old_allow;
        int ok=sel!=NULL&&nsel>0; afree(&a); return ok;
    }
    return css_supports_decl(s,n);
}
int css_supports_condition(const char *condition) {
    return condition ? css_supports_eval(condition,strlen(condition),0) : 0;
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
static float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }
static float linear_to_srgb(float v) { return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(fmaxf(0.0f, v), 1.0f / 2.4f) - 0.055f; }
/* A mix percentage is a trailing percentage token outside any nested color function. */
static int css_mix_strip_weight(char* value, float* weight) {
    size_t n = strlen(value);
    while (n && isspace((unsigned char)value[n - 1])) value[--n] = 0;
    if (!n || value[n - 1] != '%') return 0;
    char* num_end = value + n - 1;
    char* num = num_end;
    while (num > value && isspace((unsigned char)num[-1])) num--;
    while (num > value && (isdigit((unsigned char)num[-1]) || num[-1] == '.')) num--;
    if (num == num_end) return 0;
    char* sep = num;
    while (sep > value && isspace((unsigned char)sep[-1])) sep--;
    if (sep == num || sep == value) return 0; /* require a separate color token before the weight */
    *weight = strtof(num, NULL);
    while (sep > value && isspace((unsigned char)sep[-1])) sep--;
    *sep = 0;
    return 1;
}

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
    if (!strncasecmp(s, "oklch(", 6)) {
        /* The final ')' is mandatory: without this guard n - 7 could wrap to
           SIZE_MAX for input such as "oklch(" and reach memcpy with a negative size. */
        if (n < 7 || s[n - 1] != ')') return 0;
        char buf[192]; if (n >= sizeof buf) return 0; memcpy(buf, s + 6, n - 7); buf[n - 7] = 0;
        char* slash = strchr(buf, '/'); float alpha = 1.0f; if (slash) { *slash++ = 0; while (isspace((unsigned char)*slash)) slash++; alpha = strtof(slash, NULL); if (strchr(slash,'%')) alpha /= 100.0f; }
        float vals[3] = {0,0,0}; int j=0; char* q=buf;
        while (*q && j<3) { while (isspace((unsigned char)*q) || *q==',') q++; if (!*q) break; char* e; vals[j]=strtof(q,&e); if (e==q) break; if (strchr(q,'%') && (e==q || e[-1]=='%')) vals[j] /= 100.0f; q=e; if (*q=='%') q++; j++; }
        if (j < 3) return 0;
        float L=vals[0], C=vals[1], H=vals[2]; if (L>1) L/=100.0f; H*=0.01745329251994329577f;
        float aa=C*cosf(H), bb=C*sinf(H);
        float ll=L + 0.3963377774f*aa + 0.2158037573f*bb;
        float mm=L - 0.1055613458f*aa - 0.0638541728f*bb;
        float ss=L - 0.0894841775f*aa - 1.2914855480f*bb;
        ll=ll*ll*ll; mm=mm*mm*mm; ss=ss*ss*ss;
        float r= 4.0767416621f*ll - 3.3077115913f*mm + 0.2309699292f*ss;
        float g=-1.2684380046f*ll + 2.6097574011f*mm - 0.3413193965f*ss;
        float b=-0.0041960863f*ll - 0.7034186147f*mm + 1.7076147010f*ss;
        #define SRGB(v) ((v) <= 0.0031308f ? 12.92f*(v) : 1.055f*powf(fmaxf(0.0f,(v)),1.0f/2.4f)-0.055f)
        int R=clamp255(SRGB(r)*255), G=clamp255(SRGB(g)*255), B=clamp255(SRGB(b)*255);
        #undef SRGB
        int A=clamp255(alpha*255); *out=((uint32_t)A<<24)|((uint32_t)R<<16)|((uint32_t)G<<8)|(uint32_t)B; return 1;
    }
    if (!strncasecmp(s, "color-mix(", 10)) {
        const char* q = s + 10; char parts[3][160] = {{0}}; int cnt = 0, dep = 0; size_t k = 0;
        while (*q && cnt < 3) {
            if (*q == '(') dep++;
            else if (*q == ')') { if (dep == 0) break; dep--; }
            if (*q == ',' && dep == 0) { if (cnt >= 2) return 0; parts[cnt][k] = 0; cnt++; k = 0; q++; continue; }
            if (k < sizeof(parts[0]) - 1) parts[cnt][k++] = *q;
            q++;
        }
        parts[cnt][k] = 0;
        if (cnt != 2) return 0;
        for (int i = 0; i < 3; i++) {
            char* z = parts[i]; while (isspace((unsigned char)*z)) z++;
            if (z != parts[i]) memmove(parts[i], z, strlen(z) + 1);
            size_t l = strlen(parts[i]); while (l && isspace((unsigned char)parts[i][l - 1])) parts[i][--l] = 0;
        }
        int linear = !strncasecmp(parts[0], "in srgb-linear", 14);
        if (!linear && strncasecmp(parts[0], "in srgb", 7)) return 0;
        float weights[2] = {50.0f, 50.0f}; int has_weight[2] = {0, 0};
        for (int i = 0; i < 2; i++) has_weight[i] = css_mix_strip_weight(parts[i + 1], &weights[i]);
        if (has_weight[0] && !has_weight[1]) weights[1] = fmaxf(0.0f, 100.0f - weights[0]);
        else if (!has_weight[0] && has_weight[1]) weights[0] = fmaxf(0.0f, 100.0f - weights[1]);
        if (weights[0] < 0) weights[0] = 0; if (weights[1] < 0) weights[1] = 0;
        uint32_t c1, c2;
        if (!css_parse_color(parts[1], &c1, cur) || !css_parse_color(parts[2], &c2, cur)) return 0;
        float sum = weights[0] + weights[1]; if (sum <= 0.00001f) return 0;
        float t = weights[1] / sum;
        float a1 = ((c1 >> 24) & 255) / 255.0f, a2 = ((c2 >> 24) & 255) / 255.0f;
        float a = a1 * (1.0f - t) + a2 * t;
        float out_a = a * fminf(1.0f, sum / 100.0f);
        uint32_t out_rgb = 0;
        for (int shift = 16; shift >= 0; shift -= 8) {
            float u = ((c1 >> shift) & 255) / 255.0f, v = ((c2 >> shift) & 255) / 255.0f;
            if (linear) { u = srgb_to_linear(u); v = srgb_to_linear(v); }
            float mixed = a > 0.00001f ? (u * a1 * (1.0f - t) + v * a2 * t) / a : 0.0f;
            if (linear) mixed = linear_to_srgb(mixed);
            out_rgb = (out_rgb << 8) | (uint32_t)clamp255(mixed * 255.0f);
        }
        *out = ((uint32_t)clamp255(out_a * 255.0f) << 24) | out_rgb;
        return 1;
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
typedef struct { float em, rem, lh; } LCtx;

static int calc_expr(const char** pp, CVal* out, LCtx* lc, int depth);

static int pure_px(const CVal* c) { const Len* l=&c->v; return l->pct==0 && l->vw==0 && l->vh==0 && l->svw==0 && l->svh==0 && l->dvw==0 && l->dvh==0 && l->lh==0; }

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
    else if (!strncasecmp(p, "svw", 3)) { out->v.svw = num / 100.0f; p += 3; }
    else if (!strncasecmp(p, "svh", 3)) { out->v.svh = num / 100.0f; p += 3; }
    else if (!strncasecmp(p, "dvw", 3)) { out->v.dvw = num / 100.0f; p += 3; }
    else if (!strncasecmp(p, "dvh", 3)) { out->v.dvh = num / 100.0f; p += 3; }
    else if (!strncasecmp(p, "lvw", 3)) { out->v.vw = num / 100.0f; p += 3; }
    else if (!strncasecmp(p, "lvh", 3)) { out->v.vh = num / 100.0f; p += 3; }
    else if (!strncasecmp(p, "vmin", 4)) { out->v.px = num * fminf((float)SCREEN_WIDTH, (float)SCREEN_HEIGHT) / 100.0f; p += 4; }
    else if (!strncasecmp(p, "vmax", 4)) { out->v.px = num * fmaxf((float)SCREEN_WIDTH, (float)SCREEN_HEIGHT) / 100.0f; p += 4; }
    else if (!strncasecmp(p, "vw", 2)) { out->v.vw = num / 100.0f; p += 2; }
    else if (!strncasecmp(p, "vh", 2)) { out->v.vh = num / 100.0f; p += 2; }
    else if (!strncasecmp(p, "lh", 2)) { out->v.px = num * (lc->lh > 0 ? lc->lh : lc->em * 1.2f); p += 2; }
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
            CVal r = *len; r.v.px *= k; r.v.pct *= k; r.v.vw *= k; r.v.vh *= k; r.v.svw *= k; r.v.svh *= k; r.v.dvw *= k; r.v.dvh *= k; r.v.lh *= k; if (out->scalar && b.scalar) { r.scalar = 1; r.num = out->num * b.num; } else r.scalar = 0; *out = r;
        } else if (b.scalar && b.num != 0) { out->v.px /= b.num; out->v.pct /= b.num; out->v.vw /= b.num; out->v.vh /= b.num; out->v.svw /= b.num; out->v.svh /= b.num; out->v.dvw /= b.num; out->v.dvh /= b.num; out->v.lh /= b.num; out->num /= b.num; }
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
        out->v.px += k * b.v.px; out->v.pct += k * b.v.pct; out->v.vw += k * b.v.vw; out->v.vh += k * b.v.vh; out->v.svw += k * b.v.svw; out->v.svh += k * b.v.svh; out->v.dvw += k * b.v.dvw; out->v.dvh += k * b.v.dvh; out->v.lh += k * b.v.lh;
        out->scalar = out->scalar && b.scalar; out->num += k * b.num;
    }
}

int css_parse_len_lh(const char* s, Len* out, float em, float rem, float lh) {
    while (isspace((unsigned char)*s)) s++;
    memset(out, 0, sizeof *out);
    if (!*s || !strncasecmp(s, "auto", 4) || !strncasecmp(s, "none", 4) || !strncasecmp(s, "normal", 6)) return 1;   /* set = 0 */
    LCtx lc = {em, rem, lh > 0 ? lh : em * 1.2f}; CVal c; const char* p = s;
    if (!calc_expr(&p, &c, &lc, 0)) return 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p && *p != '!') return 0;
    *out = c.v; out->set = 1;
    return 1;
}
int css_parse_len(const char* s, Len* out, float em, float rem) {
    return css_parse_len_lh(s, out, em, rem, em * 1.2f);
}

/* ══════════════ media queries ══════════════ */
extern int SCREEN_WIDTH, SCREEN_HEIGHT;

static float mq_num(const char* v) {
    if (!v) return NAN;
    char* e = NULL;
    float n = strtof(v, &e);
    if (e == v || !isfinite(n)) return NAN;
    while (isspace((unsigned char)*e)) e++;
    if (!*e) return n;
    if (!strcasecmp(e, "px")) return n;
    if (!strcasecmp(e, "em") || !strcasecmp(e, "rem")) return n * 16.0f;
    if (!strcasecmp(e, "dppx")) return n;
    if (!strcasecmp(e, "dpi")) return n / 96.0f;
    if (!strcasecmp(e, "dpcm")) return n * 2.54f / 96.0f;
    return NAN;
}
static int mq_feature(const char* nm, const char* v) {
    float w = (float)SCREEN_WIDTH, h = (float)SCREEN_HEIGHT;
    if (!strcmp(nm, "min-width")) return v && w >= mq_num(v);
    if (!strcmp(nm, "max-width")) return v && w <= mq_num(v);
    if (!strcmp(nm, "width")) { float n = mq_num(v); return isfinite(n) && (int)w == (int)n; }
    if (!strcmp(nm, "min-height")) return v && h >= mq_num(v);
    if (!strcmp(nm, "max-height")) return v && h <= mq_num(v);
    if (!strcmp(nm, "height")) { float n = mq_num(v); return isfinite(n) && (int)h == (int)n; }
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
    char b[256];
    /* Never evaluate a truncated prefix of an attacker-controlled media query. */
    if (!q || n >= sizeof b) return 0;
    for (size_t i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)q[i]);
    b[n] = 0;
    const char* p = b; int neg = 0, res = 1;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '(') {
            const char* e = strchr(p, ')');
            if (!e) return 0;
            size_t l = (size_t)(e - p - 1);
            char in[128];
            if (l >= sizeof in) return 0;
            memcpy(in, p + 1, l); in[l] = 0;
            char* c = strchr(in, ':'); char nm[48]; const char* val = NULL; int ok = 0;
            if (c) {
                *c = 0; val = c + 1;
                while (isspace((unsigned char)*val)) val++;
                char* ve = (char*)val + strlen(val);
                while (ve > val && isspace((unsigned char)ve[-1])) *--ve = 0;
            }
            char* ne = in + strlen(in);
            while (ne > in && isspace((unsigned char)ne[-1])) *--ne = 0;
            char* ns = in;
            while (isspace((unsigned char)*ns)) ns++;
            size_t nl = strlen(ns);
            if (nl >= sizeof nm) return 0;
            memcpy(nm, ns, nl + 1);
            if (!c && (strstr(nm, ">=") || strstr(nm, "<="))) {
                char f[32], op[3], v2[32]; int consumed = 0;
                if (sscanf(nm, "%31s %2s %31s%n", f, op, v2, &consumed) == 3) {
                    const char* tail = nm + consumed;
                    while (isspace((unsigned char)*tail)) tail++;
                    if (!*tail && (!strcmp(op, ">=") || !strcmp(op, "<=")) &&
                        (!strcmp(f, "width") || !strcmp(f, "height"))) {
                        const char* mapped = !strcmp(f, "width")
                            ? (!strcmp(op, ">=") ? "min-width" : "max-width")
                            : (!strcmp(op, ">=") ? "min-height" : "max-height");
                        ok = mq_feature(mapped, v2);
                    }
                }
            } else {
                ok = mq_feature(nm, val);
            }
            res = res && ok; p = e + 1; continue;
        }
        const char* e = p; while (*e && !isspace((unsigned char)*e) && *e != '(') e++;
        size_t l = (size_t)(e - p);
        if (l == 3 && !strncmp(p, "not", 3)) neg = 1;
        else if ((l == 4 && !strncmp(p, "only", 4)) || (l == 3 && !strncmp(p, "and", 3)) ||
                 (l == 6 && !strncmp(p, "screen", 6)) || (l == 3 && !strncmp(p, "all", 3))) {}
        else res = 0;
        p = e;
    }
    return neg ? !res : res;
}

int css_media_matches(const char* q) {
    if (!q) return 1;
    while (isspace((unsigned char)*q)) q++;
    if (!*q) return 1;
    const char* s = q; int depth = 0;
    for (const char* p = q;; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            if (depth == 0) return 0;
            depth--;
        }
        if ((*p == ',' && depth == 0) || !*p) {
            if (depth != 0) return 0;
            if (mq_one(s, (size_t)(p - s))) return 1;
            if (!*p) break;
            s = p + 1;
        }
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
        else if (is_ident((unsigned char)*p)) {
            size_t il = read_ident(p, s->tag, sizeof s->tag, 1);
            if (!il || il >= sizeof s->tag) return 0;
            p += il; spec[2]++; any = 1;
        }
        else if (*p == '#') {
            p++; size_t il = read_ident(p, s->id, sizeof s->id, 0);
            if (!il || il >= sizeof s->id || s->id[0] == 0) return 0;
            p += il; spec[0]++; any = 1;
        }
        else if (*p == '.') {
            p++; char c[40]; size_t il = read_ident(p, c, sizeof c, 0);
            if (!il || il >= sizeof c || s->ncls >= 4) return 0;
            p += il; memcpy(s->cls[s->ncls++], c, strlen(c) + 1); spec[1]++; any = 1;
        }
        else if (*p == '[') {
            const char* e = p + 1; int d = 1; while (*e && d) { if (*e == '"' || *e == '\'') { e = skip_str(e); continue; } if (*e == '[') d++; else if (*e == ']') d--; e++; }
            if (d != 0 || s->nat >= 3) return 0; /* malformed or beyond our exact representation */
            const char* in = p + 1; size_t l = (size_t)(e - 1 - in);
            {
                size_t k = 0; while (k < l && !strchr("=~|^$*] ", in[k])) k++;
                size_t nl = k;
                if (!nl || nl >= sizeof s->at[s->nat].n) return 0;
                memcpy(s->at[s->nat].n, in, nl); s->at[s->nat].n[nl] = 0;
                for (char* c = s->at[s->nat].n; *c; c++) *c = (char)tolower((unsigned char)*c);
                while (k < l && in[k] == ' ') k++;
                if (k < l) {
                    char op = in[k]; if (op != '=') k++;
                    if (k < l && in[k] == '=') k++;
                    while (k < l && in[k] == ' ') k++;
                    const char* vs = in + k; size_t vl = l - k;
                    if (vl && (*vs == '"' || *vs == '\'')) { vs++; vl--; const char* q = memchr(vs, in[k], vl); if (q) vl = (size_t)(q - vs); }
                    else { size_t z = 0; while (z < vl && !isspace((unsigned char)vs[z])) z++; vl = z; }
                    if (vl >= sizeof s->at[s->nat].v) return 0;
                    memcpy(s->at[s->nat].v, vs, vl); s->at[s->nat].v[vl] = 0; s->at[s->nat].op = op;
                }
                s->nat++;
            }
            spec[1]++; p = e; any = 1;
        }
        else if (*p == ':') {
            {
                int dbl = p[1] == ':'; const char* q0 = p + (dbl ? 2 : 1); char pn[32];
                size_t pl = read_ident(q0, pn, sizeof pn, 1);
                if (!pl || pl >= sizeof pn) return 0;
                int pseudo = 0;
                if (!strcmp(pn, "before")) pseudo = WEBPAGE_VT_PSEUDO_BEFORE;
                else if (!strcmp(pn, "after")) pseudo = WEBPAGE_VT_PSEUDO_AFTER;
                else if (dbl && !strcmp(pn, "marker")) pseudo = WEBPAGE_VT_PSEUDO_MARKER;
                else if (dbl && !strcmp(pn, "view-transition")) pseudo = WEBPAGE_VT_PSEUDO_ROOT;
                else if (dbl && !strcmp(pn, "view-transition-group")) pseudo = WEBPAGE_VT_PSEUDO_GROUP;
                else if (dbl && !strcmp(pn, "view-transition-image-pair")) pseudo = WEBPAGE_VT_PSEUDO_IMAGE_PAIR;
                else if (dbl && !strcmp(pn, "view-transition-old")) pseudo = WEBPAGE_VT_PSEUDO_OLD;
                else if (dbl && !strcmp(pn, "view-transition-new")) pseudo = WEBPAGE_VT_PSEUDO_NEW;
                if (pseudo && (dbl || pseudo == WEBPAGE_VT_PSEUDO_BEFORE || pseudo == WEBPAGE_VT_PSEUDO_AFTER)) {
                    if (!g_allow_pseudo) return 0;
                    p = q0 + pl;
                    int needs_name = pseudo >= WEBPAGE_VT_PSEUDO_GROUP && pseudo <= WEBPAGE_VT_PSEUDO_NEW;
                    if (needs_name) {
                        if (*p != '(') return 0;
                        const char* e = strchr(p + 1, ')');
                        if (!e || (size_t)(e - (p + 1)) == 0 || (size_t)(e - (p + 1)) >= sizeof s->pseudo_arg) return 0;
                        const char* a0 = p + 1; const char* a1 = e;
                        while (a0 < a1 && isspace((unsigned char)*a0)) a0++;
                        while (a1 > a0 && isspace((unsigned char)a1[-1])) a1--;
                        if (a0 == a1 || (size_t)(a1-a0) >= sizeof s->pseudo_arg) return 0;
                        /* Accept one custom-ident only; reject CSS-wide keywords and lists. */
                        char name[48]; size_t nl = (size_t)(a1-a0); memcpy(name,a0,nl); name[nl]=0;
                        if (!strcmp(name,"none") || !strcmp(name,"auto") || !strcmp(name,"initial") ||
                            !strcmp(name,"inherit") || !strcmp(name,"unset") || strchr(name,' ') || strchr(name,'\t') || strchr(name,',')) return 0;
                        for (size_t z=0; z<nl; z++) {
                            unsigned char ch=(unsigned char)name[z];
                            if (!(isalnum(ch) || ch=='_' || ch=='-' || ch>=0x80) || (z==0 && isdigit(ch))) return 0;
                        }
                        memcpy(s->pseudo_arg,name,nl+1); p=e+1;
                    } else {
                        if (*p == '(') return 0; /* ::view-transition has no name argument */
                    }
                    s->pseudo = (uint8_t)pseudo; spec[2]++; any = 1; continue;
                }
                if (dbl || !strcmp(pn, "before") || !strcmp(pn, "after") || !strcmp(pn, "first-line") || !strcmp(pn, "first-letter")) return 0;
            }
            p++; char nm[32]; size_t nl = read_ident(p, nm, sizeof nm, 1);
            if (!nl || nl >= sizeof nm) return 0;
            p += nl;
            char arg[128] = "";
            if (*p == '(') {
                const char* e = p + 1; int d = 1;
                while (*e && d) { if (*e == '(') d++; else if (*e == ')') d--; e++; }
                if (d != 0) return 0;
                size_t l = (size_t)(e - 1 - (p + 1));
                if (l >= sizeof arg) return 0;
                memcpy(arg, p + 1, l); arg[l] = 0; p = e;
            }
            int k = 0, A = 0, B = 0;
            if (!strcmp(nm, "first-child")) k = PC_FIRST_CHILD; else if (!strcmp(nm, "last-child")) k = PC_LAST_CHILD; else if (!strcmp(nm, "only-child")) k = PC_ONLY_CHILD;
            else if (!strcmp(nm, "first-of-type")) k = PC_FIRST_TYPE; else if (!strcmp(nm, "last-of-type")) k = PC_LAST_TYPE; else if (!strcmp(nm, "only-of-type")) k = PC_ONLY_TYPE;
            else if (!strcmp(nm, "nth-child")) { k = PC_NTH_CHILD; parse_nth(arg, &A, &B); } else if (!strcmp(nm, "nth-last-child")) { k = PC_NTH_LAST_CHILD; parse_nth(arg, &A, &B); }
            else if (!strcmp(nm, "nth-of-type")) { k = PC_NTH_TYPE; parse_nth(arg, &A, &B); }
            else if (!strcmp(nm, "root")) k = PC_ROOT; else if (!strcmp(nm, "empty")) k = PC_EMPTY;
            else if (!strcmp(nm, "link") || !strcmp(nm, "any-link")) k = PC_LINK; else if (!strcmp(nm, "checked")) k = PC_CHECKED;
            else if (!strcmp(nm, "disabled")) k = PC_DISABLED; else if (!strcmp(nm, "enabled")) k = PC_ENABLED;
            else if (!strcmp(nm, "hover")) { k = PC_HOVER; css_dyn_seen = 1; } else if (!strcmp(nm, "active")) { k = PC_ACTIVE; css_dyn_seen = 1; }
            else if (!strcmp(nm, "focus")) { k = PC_FOCUS; css_dyn_seen = 1; } else if (!strcmp(nm, "focus-visible")) { k = PC_FOCUS_VISIBLE; css_dyn_seen = 1; } else if (!strcmp(nm, "focus-within")) { k = PC_FOCUS + 100; css_dyn_seen = 1; }
            else if (!strcmp(nm, "visited") || !strcmp(nm, "target") || !strcmp(nm, "placeholder-shown")) k = PC_NEVER;
            else if (!strcmp(nm, "not") || !strcmp(nm, "is") || !strcmp(nm, "where") || !strcmp(nm, "matches") || !strcmp(nm, "any") || !strcmp(nm, "has")) {
                if (s->sub) return 0;
                uint32_t sp[3] = {0, 0, 0}; int sv = g_allow_pseudo; g_allow_pseudo = 0;
                const char* subarg = arg; while (isspace((unsigned char)*subarg)) subarg++;
                s->sub_has = !strcmp(nm, "has");
                if (s->sub_has && *subarg == '>') { s->sub_direct = 1; subarg++; while (isspace((unsigned char)*subarg)) subarg++; }
                s->sub = parse_compound_list(a, subarg, strlen(subarg), sp); g_allow_pseudo = sv; if (!s->sub) return 0;
                s->sub_neg = !strcmp(nm, "not");
                if (strcmp(nm, "where")) { spec[0] += sp[0]; spec[1] += sp[1]; spec[2] += sp[2]; }
                any = 1; continue;
            }
            else if (!strcmp(nm, "defined") || !strcmp(nm, "lang") || !strcmp(nm, "dir")) { any = 1; continue; }
            else return 0;                                                       /* unsupported pseudo-class */
            if (s->npc >= 6) return 0;
            s->pc[s->npc] = (uint8_t)k; s->pa[s->npc] = (int16_t)A; s->pb[s->npc] = (int16_t)B; s->npc++;
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
    for (int i = 0; i + 1 < cnt; i++) if (tmp[i].pseudo) {
        /* View Transition pseudo-elements form a real selector tree. Other
           pseudo-elements still must be terminal in this compact selector AST. */
        if (tmp[i].pseudo < WEBPAGE_VT_PSEUDO_ROOT || tmp[i].pseudo > WEBPAGE_VT_PSEUDO_NEW) return NULL;
        if (tmp[i+1].comb != '>') return NULL;
    }
    for (int i=1; i<cnt; i++) if (tmp[i].pseudo >= WEBPAGE_VT_PSEUDO_ROOT && tmp[i].pseudo <= WEBPAGE_VT_PSEUDO_NEW &&
                                  tmp[i-1].pseudo && tmp[i-1].pseudo < WEBPAGE_VT_PSEUDO_ROOT) return NULL;
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

static int g_cq_active;
static uint8_t g_cq_flags;
static float g_cq_min_w, g_cq_max_w, g_cq_min_h, g_cq_max_h;
static char g_cq_name[40];
static float cq_len_value(const char* p) {
    while (isspace((unsigned char)*p) || *p=='(') p++;
    char* e=NULL; float v=strtof(p,&e); if (e==p) return 0;
    while (isspace((unsigned char)*e)) e++;
    if (!strncasecmp(e,"rem",3) || !strncasecmp(e,"em",2)) return v*16.0f;
    if (!strncasecmp(e,"vw",2) || !strncasecmp(e,"svw",3) || !strncasecmp(e,"dvw",3)) return v*(float)SCREEN_WIDTH/100.0f;
    if (!strncasecmp(e,"vh",2) || !strncasecmp(e,"svh",3) || !strncasecmp(e,"dvh",3)) return v*(float)SCREEN_HEIGHT/100.0f;
    if (*e=='%') return v*(float)SCREEN_WIDTH/100.0f;
    return v;
}
static void cq_apply_condition(char* cond) {
    char* begin = cond; while (isspace((unsigned char)*begin)) begin++;
    char* end = begin + strlen(begin); while (end > begin && isspace((unsigned char)end[-1])) *--end = 0;
    char* colon = strchr(begin, ':');
    if (colon) {
        *colon++ = 0;
        char* f = begin; while (isspace((unsigned char)*f)) f++;
        char* fe = f + strlen(f); while (fe > f && isspace((unsigned char)fe[-1])) *--fe = 0;
        while (isspace((unsigned char)*colon)) colon++;
        float v = cq_len_value(colon);
        if (!strcasecmp(f,"min-width") || !strcasecmp(f,"min-inline-size")) { g_cq_min_w=v; g_cq_flags|=1; }
        else if (!strcasecmp(f,"max-width") || !strcasecmp(f,"max-inline-size")) { g_cq_max_w=v; g_cq_flags|=2; }
        else if (!strcasecmp(f,"min-height") || !strcasecmp(f,"min-block-size")) { g_cq_min_h=v; g_cq_flags|=4; }
        else if (!strcasecmp(f,"max-height") || !strcasecmp(f,"max-block-size")) { g_cq_max_h=v; g_cq_flags|=8; }
        return;
    }
    /* Range syntax: (width >= 240px), (height < 40rem), and aliases. */
    char* f = begin; char* p = f;
    while (*p && !isspace((unsigned char)*p) && *p!='>' && *p!='<' && *p!='=') p++;
    if (p == f) return;
    char feature[40]; size_t fl=(size_t)(p-f); if (fl >= sizeof feature) return; memcpy(feature,f,fl); feature[fl]=0;
    while (isspace((unsigned char)*p)) p++;
    char op[3]={0,0,0};
    if (*p=='>' || *p=='<' || *p=='=') { op[0]=*p++; if (*p=='=') op[1]=*p++; }
    else return;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) return;
    float v=cq_len_value(p); int width=!strcasecmp(feature,"width")||!strcasecmp(feature,"inline-size");
    int height=!strcasecmp(feature,"height")||!strcasecmp(feature,"block-size");
    if (!width && !height) return;
    uint8_t minflag=width?1:4, maxflag=width?2:8;
    float* minv=width?&g_cq_min_w:&g_cq_min_h; float* maxv=width?&g_cq_max_w:&g_cq_max_h;
    if (!strcmp(op,">") || !strcmp(op,">=")) { *minv = v + (op[1] ? 0.0f : 0.02f); g_cq_flags|=minflag; }
    else if (!strcmp(op,"<") || !strcmp(op,"<=")) { *maxv = v - (op[1] ? 0.0f : 0.02f); g_cq_flags|=maxflag; }
    else if (!strcmp(op,"=")) { *minv=v; *maxv=v; g_cq_flags|=minflag|maxflag; }
}
static void copy_cq_to_rule(Rule* r) {
    r->has_container_query=(uint8_t)g_cq_active; r->cq_flags=g_cq_flags;
    r->cq_min_w=g_cq_min_w; r->cq_max_w=g_cq_max_w; r->cq_min_h=g_cq_min_h; r->cq_max_h=g_cq_max_h;
    snprintf(r->cq_name,sizeof r->cq_name,"%s",g_cq_name);
}
static void add_rule(RuleSet* rs, Rule* r) {
    if (rs->n == rs->cap) { rs->cap = rs->cap ? rs->cap * 2 : 512; rs->v = realloc(rs->v, (size_t)rs->cap * sizeof(Rule*)); }
    rs->v[rs->n++] = r;
}

static void parse_rule_block(Arena* a, RuleSet* rs, const char* sel, size_t sel_len, const char* body, size_t body_len, int origin) {
    int nd = 0; Decl* decls = css_parse_decls(a, body, body_len, &nd);
    if (!decls) return;
    for (int i = 0; i < nd; i++) if (decls[i].prop >= P_TRANSITION && decls[i].prop <= P_ANIMATION_RANGE) { rs->has_trans = 1; break; }
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
                r->origin = origin; r->decls = decls; r->ndecls = nd; copy_cq_to_rule(r); add_rule(rs, r);
            }
        }
        s = c + 1;
    }
}

static void trim_selector(const char** p, size_t* n) {
    while (*n && isspace((unsigned char)**p)) { (*p)++; (*n)--; }
    while (*n && isspace((unsigned char)(*p)[*n-1])) (*n)--;
}
static char* combine_nested_selectors(const char* parent, const char* child) {
    char* out=calloc(1,8192); if(!out) return NULL; size_t used=0;
    const char* pp=parent;
    while (*pp) {
        const char* pe=scan_until(pp,","); size_t pl=(size_t)(pe-pp); trim_selector(&pp,&pl);
        const char* cp=child;
        while (*cp) {
            const char* ce=scan_until(cp,","); size_t cl=(size_t)(ce-cp); trim_selector(&cp,&cl);
            char pbuf[1024], cbuf[1024], one[3072];
            if(pl>=sizeof pbuf) pl=sizeof pbuf-1; if(cl>=sizeof cbuf) cl=sizeof cbuf-1;
            memcpy(pbuf,pp,pl); pbuf[pl]=0; memcpy(cbuf,cp,cl); cbuf[cl]=0;
            one[0]=0;
            if (strchr(cbuf,'&')) {
                size_t z=0;
                for(const char* t=cbuf; *t && z+2<sizeof one;) {
                    if(*t=='&') { size_t q=strlen(pbuf); if(z+q+1>=sizeof one) break; memcpy(one+z,pbuf,q); z+=q; t++; }
                    else one[z++]=*t++;
                }
                one[z]=0;
            } else {
                const char* t=cbuf; while(isspace((unsigned char)*t))t++;
                if(*t=='>'||*t=='+'||*t=='~') snprintf(one,sizeof one,"%s %s",pbuf,t);
                else snprintf(one,sizeof one,"%s %s",pbuf,cbuf);
            }
            size_t ol=strlen(one); if(used+ol+3<8192) { if(used) {out[used++]=',';out[used++]=' ';} memcpy(out+used,one,ol); used+=ol; out[used]=0; }
            cp= *ce ? ce+1 : ce;
        }
        pp= *pe ? pe+1 : pe;
    }
    return out;
}
static void parse_nested_block(Arena* a, RuleSet* rs, const char* sel, size_t sel_len, const char* body, size_t body_len, int origin, int depth) {
    if (depth > 10) { parse_rule_block(a,rs,sel,sel_len,body,body_len,origin); return; }
    const char* p=body; const char* end=body+body_len; char direct[8192]; size_t dn=0; direct[0]=0;
    while (p<end) {
        const char* q=skip_ws(p); if(q>=end) break;
        const char* sep=scan_until(q,";{}");
        if(sep>=end) { size_t n=(size_t)(end-q); if(dn+n<sizeof direct-1) {memcpy(direct+dn,q,n);dn+=n;direct[dn]=0;} p=end; break; }
        if(*sep==';') { size_t n=(size_t)(sep+1-q); if(dn+n<sizeof direct-1) {memcpy(direct+dn,q,n);dn+=n;direct[dn]=0;} p=sep+1; continue; }
        if(*sep=='}') { size_t n=(size_t)(sep-q); if(dn+n<sizeof direct-1) {memcpy(direct+dn,q,n);dn+=n;direct[dn]=0;} p=sep+1; continue; }
        if(*sep=='{') {
            size_t clen=(size_t)(sep-q); const char* cs=q; trim_selector(&cs,&clen);
            if(dn) { parse_rule_block(a,rs,sel,sel_len,direct,dn,origin); dn=0; direct[0]=0; }
            const char* be=block_end(sep); const char* in=sep+1; size_t il=be>in?(size_t)(be-1-in):0;
            if(clen && cs[0]=='@') {
                char* at=malloc(clen+il+4); if(at) {memcpy(at,cs,clen);at[clen]='{';memcpy(at+clen+1,in,il);at[clen+1+il]='}';at[clen+2+il]=0; css_parse_sheet(a,rs,at,NULL,origin,depth+1);free(at);}
            } else if(clen) {
                /* `sel` may point into a larger stylesheet buffer and is not
                   necessarily NUL-terminated at sel_len. Selector combination
                   scans comma-separated lists, so bound it with an arena copy. */
                char* parent_sel=astrdup(a,sel,sel_len);
                char* csel=astrdup(a,cs,clen);
                char* merged=combine_nested_selectors(parent_sel,csel);
                if(merged) parse_nested_block(a,rs,merged,strlen(merged),in,il,origin,depth+1);
            }
            p=be;
        }
    }
    if(dn) parse_rule_block(a,rs,sel,sel_len,direct,dn,origin);
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
                            css_fetch_budget--; char* sub = nal_http_get_kind(abs, NAL_RESOURCE_STYLE);
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
            } else if (nl == 9 && !strncasecmp(ns, "container", 9)) {
                char q[512]; size_t ql=(size_t)(semi-ne); if(ql>511)ql=511; memcpy(q,ne,ql);q[ql]=0;
                int old_active=g_cq_active; uint8_t old_flags=g_cq_flags; float omw=g_cq_min_w, oxw=g_cq_max_w, omh=g_cq_min_h, oxh=g_cq_max_h; char oname[40]; snprintf(oname,sizeof oname,"%s",g_cq_name);
                g_cq_active=1; g_cq_flags=0; g_cq_min_w=0;g_cq_max_w=-1;g_cq_min_h=0;g_cq_max_h=-1;g_cq_name[0]=0;
                const char* paren=strchr(q,'('); if(paren) { const char* nm=q; while(isspace((unsigned char)*nm))nm++; if(paren>nm) { size_t nl2=(size_t)(paren-nm); while(nl2&&isspace((unsigned char)nm[nl2-1]))nl2--; if(nl2>0&&nl2<sizeof g_cq_name) { memcpy(g_cq_name,nm,nl2);g_cq_name[nl2]=0; } } }
                const char* tests=q;
                while((tests=strchr(tests,'('))!=NULL) { const char* ce=strchr(tests,')'); if(!ce)break; char cond[128]; size_t ln=(size_t)(ce-tests-1);if(ln>127)ln=127;memcpy(cond,tests+1,ln);cond[ln]=0;cq_apply_condition(cond); tests=ce+1; }
                char* in=malloc(il+1); if(in){memcpy(in,inner,il);in[il]=0;css_parse_sheet(a,rs,in,sheet_url,origin,depth+1);free(in);}
                g_cq_active=old_active;g_cq_flags=old_flags;g_cq_min_w=omw;g_cq_max_w=oxw;g_cq_min_h=omh;g_cq_max_h=oxh;snprintf(g_cq_name,sizeof g_cq_name,"%s",oname);
            } else if (nl == 8 && !strncasecmp(ns, "supports", 8)) {
                char cond[512]; size_t ql=(size_t)(semi-ne); if(ql>=sizeof cond) ql=sizeof cond-1;
                memcpy(cond,ne,ql); cond[ql]=0;
                if (css_supports_condition(cond)) { char* in=malloc(il+1); if(in){memcpy(in,inner,il);in[il]=0;css_parse_sheet(a,rs,in,sheet_url,origin,depth+1);free(in);} }
            } else if (nl == 5 && !strncasecmp(ns, "layer", 5)) {
                char* in = malloc(il + 1); if (in) { memcpy(in, inner, il); in[il] = 0; css_parse_sheet(a, rs, in, sheet_url, origin, depth + 1); free(in); }
            } else if (nl == 8 && !strncasecmp(ns, "property", 8) && origin != 0) {
                char prelude[96]; size_t ql=(size_t)(semi-ne); if(ql>=sizeof prelude)ql=sizeof prelude-1;
                memcpy(prelude,ne,ql);prelude[ql]=0;
                css_parse_property_rule(a,rs,prelude,inner,il);
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
        parse_nested_block(a, rs, p, (size_t)(brace - p), inner, il, origin, depth);
        p = bend;
    }
}

/* Для JS (querySelector и т.п.): разбор одного сложного селектора без разбивки по запятым. */
Simple* css_parse_selector_ex(Arena* a, const char* s, size_t n, int* nsel) { uint32_t spec; return parse_selector(a, s, n, nsel, &spec); }
