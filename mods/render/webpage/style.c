/* Каскад: UA-стили + таблицы страницы + inline -> вычисленный NodeStyle каждого узла. */
#include "css_int.h"
#include "nal.h"
#include "gal.h"
#include "fx.h"
#include "layout.h"
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
"bdo{unicode-bidi:bidi-override}bdi{unicode-bidi:isolate}"
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
"noscript,[hidden]{display:none}"
"details:not([open]) > :not(summary){display:none}";


/* ══════════════ :hover / :active / :focus ══════════════ */
static DOMNode* g_hover; static DOMNode* g_active; static int g_dyn_pending, g_uses_dyn;
static int g_keyboard_modality = 1;
int webpage_uses_dyn(void) { return g_uses_dyn; }
void webpage_set_hover(DOMNode* n) { if (n == g_hover) return; g_hover = n; if (g_uses_dyn) g_dyn_pending = 1; }
void webpage_set_active(DOMNode* n) { if (n == g_active) return; g_active = n; if (g_uses_dyn) g_dyn_pending = 1; }
void webpage_set_input_modality(int keyboard) { keyboard = keyboard != 0; if (keyboard == g_keyboard_modality) return; g_keyboard_modality = keyboard; if (g_uses_dyn) g_dyn_pending = 1; }
DOMNode* webpage_hover_node(void) { return g_hover; }
DOMNode* webpage_active_node(void) { return g_active; }
int  webpage_dyn_pending(void) { return g_dyn_pending; }
void webpage_dyn_clear_pending(void) { g_dyn_pending = 0; }
int webpage_dyn_match(int kind, const DOMNode* n) {
    const DOMNode* t = kind == PC_HOVER ? g_hover : kind == PC_ACTIVE ? g_active : focused_node;
    if (!t || t->idx < 0) return 0;
    if (kind == PC_FOCUS) return t == n;
    if (kind == PC_FOCUS_VISIBLE) {
        if (t != n) return 0;
        if (g_keyboard_modality) return 1;
        if (!strcmp(n->tag,"input") || !strcmp(n->tag,"textarea") || !strcmp(n->tag,"select")) return 1;
        char ce[16]; if (webpage_attr_get(n,"contenteditable",ce,sizeof ce) && strcasecmp(ce,"false")) return 1;
        return 0;
    }
    return n->idx <= t->idx && t->idx <= n->subtree_end;
}

/* ══════════════ индекс правил ══════════════ */
typedef struct HB { char* key; Rule** v; int n, cap; struct HB* next; } HB;
#define HSZ 4096
typedef struct { HB* t[HSZ]; Rule** uni; int nuni, capuni; int has_pseudo, has_marker; } RIndex;

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
        if (r->pseudo == 3) ix->has_marker = 1;
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
    DOMNode** nv; int n; int* prev; int* eidx; int* ecnt; CP** cps; RuleSet* rs;
} Doc;

/* Computed style snapshots for the View Transition pseudo-element tree. They
   are copied out of the transient stylesheet arena before style_apply returns,
   so the paint compositor never retains pointers into CSS parser storage. */
#define VT_STYLE_MAX_NAMES 24
#define VT_STYLE_ANIM_MAX 3
#define VT_STYLE_KF_MAX 24
#define VT_STYLE_KF_BUDGET (8u * 1024u * 1024u)
typedef struct { float off; uint64_t mask; NodeStyle* st; } VTStyleKeyframe;
typedef struct {
    FxAnim spec;
    VTStyleKeyframe frames[VT_STYLE_KF_MAX];
    int n;
    uint64_t all;
} VTStyleAnimation;
typedef struct {
    VTStyleAnimation animation[VT_STYLE_ANIM_MAX];
    NodeStyle evaluated;
} VTStyleAnimList;
typedef struct {
    char name[48];
    NodeStyle group, image_pair, old_image, new_image;
    VTStyleAnimList group_anim, image_pair_anim, old_anim, new_anim;
} VTStyleRecord;
static NodeStyle g_vt_root_style;
static VTStyleAnimList g_vt_root_anim;
static VTStyleRecord g_vt_style_records[VT_STYLE_MAX_NAMES];
static int g_vt_style_count;
static size_t g_vt_style_kf_bytes;
static uint64_t kf_build(void* ctx, DOMNode* n, const Decl* decls, int nd, NodeStyle* out, float root_fs);

static void vt_free_anim_list(VTStyleAnimList* list) {
    if (!list) return;
    for (int a=0;a<VT_STYLE_ANIM_MAX;a++) {
        VTStyleAnimation* an=&list->animation[a];
        for (int i=0;i<an->n;i++) {
            if (an->frames[i].st) {
                free(an->frames[i].st);
                if (g_vt_style_kf_bytes >= sizeof(NodeStyle)) g_vt_style_kf_bytes -= sizeof(NodeStyle);
                else g_vt_style_kf_bytes = 0;
            }
        }
        memset(an,0,sizeof *an);
    }
    memset(&list->evaluated,0,sizeof list->evaluated);
}

static void vt_free_all_anim_lists(void) {
    vt_free_anim_list(&g_vt_root_anim);
    for (int i=0;i<VT_STYLE_MAX_NAMES;i++) {
        vt_free_anim_list(&g_vt_style_records[i].group_anim);
        vt_free_anim_list(&g_vt_style_records[i].image_pair_anim);
        vt_free_anim_list(&g_vt_style_records[i].old_anim);
        vt_free_anim_list(&g_vt_style_records[i].new_anim);
    }
    g_vt_style_kf_bytes=0;
}

/* Renderer snapshot layers only paint opacity, background color and affine
   transform, so keyframe declarations outside this subset are intentionally
   not copied into the pseudo-element compositor. DOM animations remain full. */
#define VT_STYLE_ANIM_MASK (APM(AP_OPACITY) | APM(AP_BG) | APM(AP_XFORM) | APM(AP_ORIGIN) | APM(AP_FILTER) | APM(AP_CLIP))

static void vt_prepare_anim_list(Doc* d, RuleSet* rs, DOMNode* root, float root_fs,
                                 const NodeStyle* base, VTStyleAnimList* list) {
    if (!d || !rs || !root || !base || !list) return;
    for (int a=0;a<VT_STYLE_ANIM_MAX && a<base->an_n;a++) {
        const FxAnim* spec=&base->an[a];
        if (!spec->name[0] || !strcasecmp(spec->name,"none")) continue;
        const Keyframes* kf=NULL;
        for (const Keyframes* it=rs->kf;it;it=it->next) if (!strcmp(it->name,spec->name)) { kf=it; break; }
        if (!kf) continue;
        VTStyleAnimation* target=&list->animation[a];
        target->spec=*spec;
        for (int i=0;i<kf->n && target->n<VT_STYLE_KF_MAX;i++) {
            if (g_vt_style_kf_bytes + sizeof(NodeStyle) > VT_STYLE_KF_BUDGET) break;
            NodeStyle* frame=malloc(sizeof *frame);
            if (!frame) break;
            *frame=*base;
            uint64_t mask=kf_build(d,root,kf->fr[i].decls,kf->fr[i].nd,frame,root_fs) & VT_STYLE_ANIM_MASK;
            if (!mask) { free(frame); continue; }
            VTStyleKeyframe* dst=&target->frames[target->n++];
            dst->off=kf->fr[i].off; dst->mask=mask; dst->st=frame;
            target->all |= mask; g_vt_style_kf_bytes += sizeof(NodeStyle);
        }
        if (!target->n) memset(target,0,sizeof *target);
    }
}

static float vt_animation_progress(const FxAnim* spec, double elapsed_ms, int* applies) {
    *applies=0;
    if (!spec) return 0;
    double el=elapsed_ms-(double)spec->delay;
    double dur=(double)spec->dur;
    double iterations=(double)spec->iter;
    if (iterations < 0.0) iterations=1.0; /* infinite pseudo animation is bounded by the transition lifecycle */
    if (!isfinite(iterations) || iterations < 0.0) iterations=1.0;
    if (iterations > 1000000.0) iterations=1000000.0;
    if (el < 0.0) {
        if (!(spec->fill & 2u)) return 0.0f;
        *applies=1;
        return (spec->dir==1 || spec->dir==3) ? 1.0f : 0.0f;
    }
    if (iterations <= 0.0) return 0.0f;
    int after=0, iteration=0; float frac=0.0f;
    if (dur <= 0.0) {
        after=1;
        if (!(spec->fill & 1u)) return 0.0f;
        iteration=(int)ceil(iterations)-1; if (iteration<0) iteration=0;
        frac=(float)(iterations-floor(iterations)); if (frac==0.0f) frac=1.0f;
    } else {
        double total=dur*iterations;
        if (!isfinite(total) || total < 0.0) total=0.0;
        if (el >= total) {
            after=1;
            if (!(spec->fill & 1u)) return 0.0f;
            iteration=(int)ceil(iterations)-1; if (iteration<0) iteration=0;
            frac=(float)(iterations-floor(iterations)); if (frac==0.0f) frac=1.0f;
        } else {
            double scaled=el/dur;
            if (scaled<0) scaled=0;
            if (scaled>1000000.0) scaled=1000000.0;
            iteration=(int)floor(scaled); frac=(float)(scaled-(double)iteration);
        }
    }
    (void)after;
    if (spec->dir==1 || (spec->dir==2 && (iteration&1)) || (spec->dir==3 && !(iteration&1))) frac=1.0f-frac;
    *applies=1;
    return fx_ease(&spec->ease,frac);
}

static void vt_compose_animation_property(NodeStyle* out, const NodeStyle* effect,
                                         int prop, uint8_t composition) {
    uint64_t bit=APM(prop);
    if (composition==0) { anim_style_apply_mask(out,effect,effect,bit,1.0f); return; }
    if (prop==AP_OPACITY) {
        /* Additive opacity is a numeric sum; the renderable CSS value is clamped. */
        float v=out->op_own+effect->op_own;
        if (!isfinite(v) || v<0.0f) v=0.0f; if (v>1.0f) v=1.0f;
        out->op_own=v; out->opacity=v;
        return;
    }
    if (prop==AP_XFORM) {
        /* Transform-list addition concatenates operations. Accumulate is
           approximated with the same matrix/list order until DOMMatrix-backed
           transform accumulation is available. */
        int n=out->xf.n, en=effect->xf.n;
        if (n+en<=6) {
            memcpy(&out->xf.op[n],effect->xf.op,(size_t)en*sizeof(FxXfOp));
            out->xf.n=(uint8_t)(n+en);
        } else {
            /* Keep bounded and deterministic when an author supplies huge lists. */
            int take=6-n; if(take<0)take=0;
            if(take)memcpy(&out->xf.op[n],effect->xf.op,(size_t)take*sizeof(FxXfOp));
            out->xf.n=6;
        }
        return;
    }
    /* Color/filter/clip additive composition is not yet defined by this compact
       raster compositor; use replace rather than inventing numeric semantics. */
    anim_style_apply_mask(out,effect,effect,bit,1.0f);
}

static void vt_evaluate_animation(const VTStyleAnimation* animation, const NodeStyle* base,
                                  double elapsed_ms, NodeStyle* out) {
    if (!animation || !base || !out || !animation->n || !animation->all) return;
    int applies=0; float progress=vt_animation_progress(&animation->spec,elapsed_ms,&applies);
    if (!applies) return;
    NodeStyle effect=*base;
    for (int prop=0;prop<AP_COUNT;prop++) {
        uint64_t bit=APM(prop);
        if (!(animation->all & bit)) continue;
        int lo=-1,hi=-1;
        for (int i=0;i<animation->n;i++) if (animation->frames[i].mask & bit) {
            if (animation->frames[i].off <= progress) lo=i;
            if (animation->frames[i].off >= progress && hi<0) hi=i;
        }
        const NodeStyle* a=lo>=0?animation->frames[lo].st:base;
        const NodeStyle* b=hi>=0?animation->frames[hi].st:base;
        float oa=lo>=0?animation->frames[lo].off:0.0f;
        float ob=hi>=0?animation->frames[hi].off:1.0f;
        float t=ob>oa?(progress-oa)/(ob-oa):1.0f;
        if (t<0)t=0;if(t>1)t=1;
        anim_style_apply_mask(&effect,a,b,bit,t);
    }
    for (int prop=0;prop<AP_COUNT;prop++) if (animation->all & APM(prop))
        vt_compose_animation_property(out,&effect,prop,animation->spec.composition);
}

static const NodeStyle* vt_eval_style(const NodeStyle* base, VTStyleAnimList* anim, double elapsed_ms) {
    if (!base) return NULL;
    if (!anim) return base;
    anim->evaluated=*base;
    for (int i=0;i<VT_STYLE_ANIM_MAX;i++)
        vt_evaluate_animation(&anim->animation[i],base,elapsed_ms,&anim->evaluated);
    return &anim->evaluated;
}

const NodeStyle* webpage_view_transition_style(const char* name, int pseudo) {
    if (pseudo == WEBPAGE_VT_PSEUDO_ROOT) return &g_vt_root_style;
    if (!name || !*name) return NULL;
    for (int i=0; i<g_vt_style_count; i++) if (!strcmp(g_vt_style_records[i].name,name)) {
        switch (pseudo) {
            case WEBPAGE_VT_PSEUDO_GROUP: return &g_vt_style_records[i].group;
            case WEBPAGE_VT_PSEUDO_IMAGE_PAIR: return &g_vt_style_records[i].image_pair;
            case WEBPAGE_VT_PSEUDO_OLD: return &g_vt_style_records[i].old_image;
            case WEBPAGE_VT_PSEUDO_NEW: return &g_vt_style_records[i].new_image;
            default: return NULL;
        }
    }
    return NULL;
}

const NodeStyle* webpage_view_transition_style_at(const char* name, int pseudo, double elapsed_ms) {
    if (pseudo==WEBPAGE_VT_PSEUDO_ROOT)
        return vt_eval_style(&g_vt_root_style,&g_vt_root_anim,elapsed_ms);
    if (!name || !*name) return NULL;
    for (int i=0;i<g_vt_style_count;i++) if (!strcmp(g_vt_style_records[i].name,name)) {
        VTStyleRecord* rec=&g_vt_style_records[i];
        switch (pseudo) {
            case WEBPAGE_VT_PSEUDO_GROUP: return vt_eval_style(&rec->group,&rec->group_anim,elapsed_ms);
            case WEBPAGE_VT_PSEUDO_IMAGE_PAIR: return vt_eval_style(&rec->image_pair,&rec->image_pair_anim,elapsed_ms);
            case WEBPAGE_VT_PSEUDO_OLD: return vt_eval_style(&rec->old_image,&rec->old_anim,elapsed_ms);
            case WEBPAGE_VT_PSEUDO_NEW: return vt_eval_style(&rec->new_image,&rec->new_anim,elapsed_ms);
            default: return NULL;
        }
    }
    return NULL;
}

double webpage_view_transition_duration_ms(void) {
    double longest=240.0;
    VTStyleAnimList* lists[1+VT_STYLE_MAX_NAMES*4]; int count=0;
    lists[count++]=&g_vt_root_anim;
    for (int i=0;i<g_vt_style_count;i++) {
        lists[count++]=&g_vt_style_records[i].group_anim;
        lists[count++]=&g_vt_style_records[i].image_pair_anim;
        lists[count++]=&g_vt_style_records[i].old_anim;
        lists[count++]=&g_vt_style_records[i].new_anim;
    }
    for (int i=0;i<count;i++) for (int a=0;a<VT_STYLE_ANIM_MAX;a++) {
        VTStyleAnimation* an=&lists[i]->animation[a];
        if (!an->n || !an->spec.name[0]) continue;
        if (an->spec.iter<0.0f) continue; /* infinite animation cannot keep VT alive forever */
        double total=(double)an->spec.delay+(double)an->spec.dur*(double)an->spec.iter;
        if (!isfinite(total)) continue;
        if (total>30000.0) total=30000.0;
        if (total>longest) longest=total;
    }
    if (longest<1.0) longest=1.0;
    return longest;
}

int webpage_view_transition_animates_opacity(const char* name, int pseudo) {
    VTStyleAnimList* list=NULL;
    if (pseudo==WEBPAGE_VT_PSEUDO_ROOT) list=&g_vt_root_anim;
    else if (name && *name) for (int i=0;i<g_vt_style_count;i++) if (!strcmp(g_vt_style_records[i].name,name)) {
        VTStyleRecord* rec=&g_vt_style_records[i];
        switch (pseudo) {
            case WEBPAGE_VT_PSEUDO_GROUP: list=&rec->group_anim; break;
            case WEBPAGE_VT_PSEUDO_IMAGE_PAIR: list=&rec->image_pair_anim; break;
            case WEBPAGE_VT_PSEUDO_OLD: list=&rec->old_anim; break;
            case WEBPAGE_VT_PSEUDO_NEW: list=&rec->new_anim; break;
            default: break;
        }
        break;
    }
    if (!list) return 0;
    for (int a=0;a<VT_STYLE_ANIM_MAX;a++)
        if (list->animation[a].n && (list->animation[a].all & APM(AP_OPACITY))) return 1;
    return 0;
}

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
    if (s->sub_has) {
        int found = 0;
        for (int q = n->idx + 1; q <= n->subtree_end && q < d->n; q++) {
            DOMNode* child = d->nv[q];
            if (s->sub_direct && child->parent != n->idx) continue;
            if (child->type == TAG_TEXT) continue;
            if (match_sub(d, child, s->sub)) { found = 1; break; }
            if (s->sub_direct && child->parent == n->idx) { /* continue through immediate siblings only */ }
        }
        if (!found) return 0;
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
            case PC_HOVER: case PC_ACTIVE: case PC_FOCUS: case PC_FOCUS_VISIBLE: case PC_FOCUS_WITHIN: ok = webpage_dyn_match(s->pc[i], n); break;
            default: ok = 0; break;
        }
        if (!ok) return 0;
    }
    if (s->sub && !s->sub_has) { int m = match_sub(d, n, s->sub); if (s->sub_neg ? m : !m) return 0; }
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

/* Registered custom properties have a computed initial value and may disable
   inheritance. Inject their defaults before authored declarations so local
   declarations still win through the normal reverse-order CP chain. */
static void cp_seed_registered(const RuleSet* rs, CP** chain) {
    if (!rs || !chain) return;
    for (const RegisteredProperty* r=rs->properties; r; r=r->next) {
        if (r->inherits && cp_find(*chain,r->name,strlen(r->name))) continue;
        CP* cp=malloc(sizeof *cp); if(!cp) return;
        cp->name=r->name; cp->val=r->initial_value; cp->next=*chain; *chain=cp;
    }
}

/* ══════════════ применение значений ══════════════ */
typedef struct { float em, rem, pfs, lh, parent_lh; uint32_t cur; DOMNode* parent; char* content; int has_content; DOMNode* owner; Doc* doc; } Cx;

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

/* Like tok_next(), but refuse tokens that do not fit rather than silently
   treating a truncated identifier as a different, valid name. The token is
   still consumed so callers can continue safely after an oversized token. */
static int tok_next_exact(const char** pp, char* buf, size_t cap) {
    const char* start = *pp;
    while (isspace((unsigned char)*start)) start++;
    if (!*start) { if (cap) buf[0] = 0; return 0; }
    if (!tok_next(pp, buf, cap)) return 0;
    if (!cap || (size_t)(*pp - start) >= cap) {
        if (cap) buf[0] = 0;
        return -1;
    }
    return 1;
}

static int css_tokens_fit(const char* value, size_t max_token) {
    const char* p = value;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        const char* start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if ((size_t)(p - start) > max_token) return 0;
    }
    return 1;
}

static int len_of(Cx* c, const char* v, Len* out) { return css_parse_len_lh(v, out, c->em, c->rem, c->lh); }
static int col_of(Cx* c, const char* v, uint32_t* out) {
    if (!strcasecmp(v, "currentcolor")) { *out = CUR_COLOR; return 1; }
    return css_parse_color(v, out, c->cur);
}
static const char* g_base = NULL;
static float px_of(const Len* l, float pct_base) { return l->px + l->pct * pct_base / 100.0f + (l->vw + l->svw + l->dvw) * SCREEN_WIDTH + (l->vh + l->svh + l->dvh) * SCREEN_HEIGHT + l->lh; }

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
    int conic = !strncasecmp(tk, "conic-gradient(", 15);
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
                    if (conic && !strncasecmp(sp, "from ", 5)) angle = strtof(sp + 5, NULL);
                    else if (strcasestr(sp, "deg")) angle = strtof(sp, NULL);
                    else {
                        int rt = strcasestr(sp, "right") != NULL, lf = strcasestr(sp, "left") != NULL;
                        int tp = strcasestr(sp, "top") != NULL, bt = strcasestr(sp, "bottom") != NULL;
                        angle = (rt && tp) ? 45.0f : (rt && bt) ? 135.0f : (lf && bt) ? 225.0f : (lf && tp) ? 315.0f :
                                rt ? 90.0f : lf ? 270.0f : tp ? 0.0f : bt ? 180.0f : angle;
                    }
                    k = 0; if (!*q || end) break; continue;
                }
                if ((radial && (strcasestr(sp, "circle") || strcasestr(sp, "ellipse") || strcasestr(sp, "at "))) || (conic && !strncasecmp(sp, "at ", 3))) { k = 0; if (!*q || end) break; continue; }
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
        st->bg_grad_type = conic ? 3 : radial ? 2 : 1; st->bg_grad_n = (uint8_t)n; st->bg_grad_angle = angle; st->bg = st->bg_grad_c[0];
    } else if (n == 1) { st->bg = st->bg_grad_c[0]; st->bg_grad_type = 0; }
}

static void parse_bg(Cx* c, NodeStyle* st, const char* v, int full) {
    char tk[16384]; const char* p = v; uint32_t col;                       /* data-URI в url(...) бывают длинными */
    if (full) { st->bg = 0; st->bg_url[0] = 0; st->bg_repeat = 0; st->bg_size_mode = 0; memset(&st->bg_px, 0, sizeof(Len) * 2); st->bg_px.pct = st->bg_py.pct = 0; st->bg_px.set = st->bg_py.set = 0; st->bg_grad_type = 0; }
    int posn = 0; int in_size = 0;
    while (tok_next(&p, tk, sizeof tk)) {
        if (!strncasecmp(tk, "url(", 4)) {
            const char* q = tk + 4; while (isspace((unsigned char)*q)) q++; char qc = (*q == '"' || *q == '\'') ? *q++ : 0;
            const char* qe = q; while (*qe && (qc ? *qe != qc : *qe != ')')) qe++;
            if (qe - q > 5 && !strncasecmp(q, "data:", 5)) {
                char dp[200];
                if (!webpage_data_uri_to_file(q, (size_t)(qe - q), dp, sizeof dp) || strlen(dp) >= sizeof st->bg_url) dp[0] = 0;
                memcpy(st->bg_url, dp, strlen(dp) + 1); /* data: → файл кэша */
            } else {
                size_t url_len = (size_t)(qe - q);
                if (url_len >= sizeof st->bg_url) {
                    /* Do not load a different URL formed from a truncated prefix. */
                    st->bg_url[0] = 0;
                } else {
                    memcpy(st->bg_url, q, url_len);
                    st->bg_url[url_len] = 0;
                }
            }
        } else if (!strncasecmp(tk, "linear-gradient(", 16) || !strncasecmp(tk, "-webkit-linear-gradient(", 24) || !strncasecmp(tk, "radial-gradient(", 16) || !strncasecmp(tk, "conic-gradient(", 15)) {
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

#include "style_fx.inc"

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
    Len l; if (!css_parse_len_lh(v, &l, c->pfs, c->rem, c->parent_lh) || !l.set) return;
    float px = px_of(&l, c->pfs);
    if (px >= 0) st->font_size = px;
}

static void set_line_height(Cx* c, NodeStyle* st, const char* v) {
    if (!strcasecmp(v, "normal")) { st->lh_mult = 0; st->lh_px = 0; return; }
    char* e; float n = strtof(v, &e);
    if (e != v && !*e) { st->lh_mult = n; st->lh_px = 0; return; }
    Len l; if (css_parse_len_lh(v, &l, c->em, c->rem, c->parent_lh) && l.set) { st->lh_px = px_of(&l, st->font_size); st->lh_mult = 0; }
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
            size_t size_len = sl ? (size_t)(sl - tk) : strlen(tk);
            size_t lh_len = sl ? strlen(sl + 1) : 0;
            if (size_len >= sizeof sz || lh_len >= sizeof lh) return;
            memcpy(sz, tk, size_len); sz[size_len] = 0;
            if (sl) { memcpy(lh, sl + 1, lh_len); lh[lh_len] = 0; }
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
typedef struct { char name[40]; int value, end; } CounterEntry;
static int collect_counter_values(Doc* d, DOMNode* owner, const char* wanted, int* values, int cap) {
    if (!d || !owner || owner->idx<0 || owner->idx>=d->n) return 0;
    CounterEntry stack[256]; int n=0;
    for (int ix=0; ix<=owner->idx && ix<d->n; ix++) {
        while (n>0 && stack[n-1].end < ix) n--;
        DOMNode* el=d->nv[ix]; if (!el || el->type==TAG_TEXT) continue;
        const char* p=el->st.counter_reset; char name[40], num[48];
        while (tok_next(&p,name,sizeof name)) {
            if (!strcasecmp(name,"none")) continue;
            int value=0; const char* save=p;
            if (tok_next(&p,num,sizeof num)) { char* e; long q=strtol(num,&e,10); if(e!=num && !*e) value=(int)q; else p=save; }
            if(n<256) { snprintf(stack[n].name,sizeof stack[n].name,"%s",name); stack[n].value=value; stack[n].end=el->subtree_end; n++; }
        }
        p=el->st.counter_increment;
        while (tok_next(&p,name,sizeof name)) {
            if (!strcasecmp(name,"none")) continue;
            int inc=1; const char* save=p;
            if (tok_next(&p,num,sizeof num)) { char* e; long q=strtol(num,&e,10); if(e!=num && !*e) inc=(int)q; else p=save; }
            int j; for(j=n-1;j>=0;j--) if(!strcmp(stack[j].name,name)) break;
            if(j<0 && n<256) { j=n++; snprintf(stack[j].name,sizeof stack[j].name,"%s",name); stack[j].value=0; stack[j].end=el->subtree_end; }
            if(j>=0) stack[j].value+=inc;
        }
        p=el->st.counter_set;
        while (tok_next(&p,name,sizeof name)) {
            if (!strcasecmp(name,"none")) continue;
            int value=0; const char* save=p;
            if (tok_next(&p,num,sizeof num)) { char* e; long q=strtol(num,&e,10); if(e!=num && !*e) value=(int)q; else p=save; }
            int j; for(j=n-1;j>=0;j--) if(!strcmp(stack[j].name,name)) break;
            if(j<0 && n<256) { j=n++; snprintf(stack[j].name,sizeof stack[j].name,"%s",name); stack[j].end=el->subtree_end; }
            if(j>=0) stack[j].value=value;
        }
    }
    int count=0; for(int i=0;i<n;i++) if(!strcmp(stack[i].name,wanted)) { if(count<cap && values) values[count]=stack[i].value; count++; }
    return count;
}
static void counter_format(char* out, size_t cap, int value, const char* style) {
    if (!strcasecmp(style,"upper-roman") || !strcasecmp(style,"lower-roman")) {
        static const int nums[]={1000,900,500,400,100,90,50,40,10,9,5,4,1};
        static const char* sym[]={"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
        if(value>0&&value<=3999) { size_t o=0; for(int i=0;i<13;i++) while(value>=nums[i]) { size_t l=strlen(sym[i]); if(o+l+1>=cap) break; memcpy(out+o,sym[i],l);o+=l;value-=nums[i]; } out[o]=0; if(!strcasecmp(style,"lower-roman")) for(char*p=out;*p;p++)*p=(char)tolower((unsigned char)*p); return; }
    }
    if (!strcasecmp(style,"lower-alpha") || !strcasecmp(style,"upper-alpha") || !strcasecmp(style,"lower-latin") || !strcasecmp(style,"upper-latin")) {
        if(value>0&&value<=18278) { char tmp[16]; int k=0; while(value>0&&k<15){value--;tmp[k++]=(char)((!strncasecmp(style,"upper",5)?'A':'a')+(value%26));value/=26;} size_t o=0;while(k&&o+1<cap)out[o++]=tmp[--k];out[o]=0;return; }
    }
    snprintf(out,cap,"%d",value);
}
static void content_counter(Cx* c, const char* expr, int is_many, char* out, size_t cap) {
    char name[40]="", style[32]="decimal", sep[40]="."; const char* p=expr;
    while(isspace((unsigned char)*p))p++; size_t n=0; while(*p && *p!=',' && !isspace((unsigned char)*p) && n<sizeof name-1)name[n++]=*p++;name[n]=0;
    while(isspace((unsigned char)*p))p++;
    if(*p==',') { p++; while(isspace((unsigned char)*p))p++; if(*p=='"'||*p=='\'') { char q=*p++;size_t k=0;while(*p&&*p!=q&&k<sizeof sep-1)sep[k++]=*p++;sep[k]=0;if(*p)p++; while(isspace((unsigned char)*p))p++; if(*p==',')p++; } if (p<expr+strlen(expr)) { while(isspace((unsigned char)*p))p++; if (*p=='"'||*p=='\'') {p++;size_t k=0;while(*p&&*p!='"'&&*p!='\''&&k<sizeof style-1)style[k++]=*p++;style[k]=0;} else {size_t k=0;while(*p && !isspace((unsigned char)*p)&&k<sizeof style-1)style[k++]=*p++;style[k]=0;} } }
    if (!c->doc || !c->owner || !name[0]) { snprintf(out,cap,"0"); return; }
    int vals[32]; int count=collect_counter_values(c->doc,c->owner,name,vals,32); if(count<=0){snprintf(out,cap,"0");return;}
    if (expr[0]=='#') (void)expr;
    if (is_many) { size_t o=0; int use=count<32?count:32; for(int i=0;i<use;i++){char b[32];counter_format(b,sizeof b,vals[i],style);if(i){size_t z=strlen(sep);if(o+z>=cap)break;memcpy(out+o,sep,z);o+=z;}size_t z=strlen(b);if(o+z>=cap)z=cap-o-1;memcpy(out+o,b,z);o+=z;if(o>=cap-1)break;}out[o]=0;}
    else counter_format(out,cap,vals[count-1],style);
}

static void parse_content(Cx* c, const char* v) {
    char* out = c->content; size_t cap = 248, o = 0; out[0] = 0; c->has_content = 0;
    const char* p = v;
    while (isspace((unsigned char)*p)) p++;
    if (!*p || !strncasecmp(p, "normal", 6)) return;
    if (!strncasecmp(p, "none", 4)) { c->has_content = -1; return; }
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
        else if (!strncasecmp(p,"counter(",8) || !strncasecmp(p,"counters(",9)) {
            int is_many=!strncasecmp(p,"counters(",9); const char* arg=p+(is_many?9:8); const char* e=arg; int dep=1;
            while(*e&&dep){if(*e=='(')dep++;else if(*e==')')dep--;if(dep)e++;}
            if(dep) break; char expr[160];size_t l=(size_t)(e-arg);if(l>159)l=159;memcpy(expr,arg,l);expr[l]=0;
            char tmp[160];content_counter(c,expr,is_many,tmp,sizeof tmp);for(char*q=tmp;*q;q++)put_raw(out,cap,&o,(unsigned char)*q);p=e+1;
        } else {                                                                   /* url(...), no-open-quote и прочие пока не отрисовываем */
            int d = 0; while (*p && (d > 0 || !isspace((unsigned char)*p))) { if (*p == '(') d++; if (*p == ')') d--; p++; }
        }
    }
}

static int logical_side(const NodeStyle* st, int inline_axis, int start) {
    int wm = st->writing_mode, rtl = st->direction != 0;
    if (wm == 0) {
        if (inline_axis) return (start != rtl) ? 3 : 1; /* LTR start=left; RTL start=right */
        return start ? 0 : 2;
    }
    if (inline_axis) return (start != rtl) ? 0 : 2; /* vertical inline direction: top/bottom */
    if (wm == 1) return start ? 1 : 3;              /* vertical-rl */
    return start ? 3 : 1;                            /* vertical-lr */
}
static void logical_pair(Cx* c, const char* v, Len out[4], const NodeStyle* st, int inline_axis, int base_side) {
    const char* p = v; char a[128], b[128]; Len la, lb;
    if (!tok_next(&p, a, sizeof a)) return;
    if (!tok_next(&p, b, sizeof b)) snprintf(b, sizeof b, "%s", a);
    if (!len_of(c, a, &la) || !len_of(c, b, &lb)) return;
    out[logical_side(st, inline_axis, 1)] = la;
    out[logical_side(st, inline_axis, 0)] = lb;
    (void)base_side;
}
static void parse_text_shadow(Cx* c, NodeStyle* st, const char* v) {
    st->ts_color = 0; st->ts_x = st->ts_y = st->ts_blur = 0;
    if (!strcasecmp(v, "none")) return;
    char first[256]; const char* e = v; int d = 0; size_t k = 0;
    while (*e && !(d == 0 && *e == ',') && k < sizeof(first)-1) { if (*e == '(') d++; if (*e == ')') d--; first[k++] = *e++; } first[k] = 0;
    float nums[3] = {0,0,0}; int nn = 0; uint32_t color = 0x80000000u; char tk[96]; const char* p = first; Len l;
    while (tok_next(&p, tk, sizeof tk)) { uint32_t cc;
        if ((isdigit((unsigned char)tk[0]) || tk[0]=='-' || tk[0]=='.') && len_of(c, tk, &l)) { if (nn < 3) nums[nn++] = px_of(&l, 0); }
        else if (col_of(c, tk, &cc)) color = cc;
    }
    if (nn >= 2) { st->ts_x = nums[0]; st->ts_y = nums[1]; st->ts_blur = nums[2]; st->ts_color = color; }
}
static void parse_outline(Cx* c, NodeStyle* st, const char* v) {
    if (!strcasecmp(v, "none") || !strncasecmp(v, "0", 1)) { st->outline_w = 0; st->outline_style = BS_NONE; st->outline_set = 1; st->no_ring = 1; return; }
    char tk[128]; const char* p = v; Len l; uint32_t col;
    float w = 3; int style = BS_SOLID; uint32_t color = st->color;
    while (tok_next(&p, tk, sizeof tk)) {
        int bs = bstyle(tk);
        if (bs >= 0) style = bs;
        else if (!strcasecmp(tk, "thin")) w = 1; else if (!strcasecmp(tk, "medium")) w = 3; else if (!strcasecmp(tk, "thick")) w = 5;
        else if ((isdigit((unsigned char)tk[0]) || tk[0]=='-' || tk[0]=='.') && len_of(c, tk, &l)) w = px_of(&l, 0);
        else if (col_of(c, tk, &col)) color = col;
    }
    st->outline_w = w; st->outline_style = (uint8_t)style; st->outline_c = color; st->outline_set = 1; st->no_ring = style == BS_NONE;
}
static void append_individual_transform(Cx* c, NodeStyle* st, int prop, const char* val) {
    FxXform tmp; memset(&tmp, 0, sizeof tmp); tmp.ox = st->xf.ox; tmp.oy = st->xf.oy; tmp.has_origin = st->xf.has_origin;
    char expr[256];
    if (!strcasecmp(val, "none")) return;
    if (prop == P_TRANSLATE) snprintf(expr, sizeof expr, "translate(%s)", val);
    else snprintf(expr, sizeof expr, "rotate(%s)", val);
    parse_xform(c, &tmp, expr);
    for (int i = 0; i < tmp.n && st->xf.n < 6; i++) st->xf.op[st->xf.n++] = tmp.op[i];
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
            case P_POINTER_EVENTS: st->pointer_events = ps->pointer_events; break; case P_USER_SELECT: st->user_select = ps->user_select; break;
            case P_WRITING_MODE: st->writing_mode = ps->writing_mode; break; case P_DIRECTION: st->direction = ps->direction; break;
            case P_TEXT_INDENT: st->text_indent_len = ps->text_indent_len; st->text_indent = ps->text_indent; break; case P_WORD_SPACING: st->word_spacing_len = ps->word_spacing_len; st->word_spacing = ps->word_spacing; break; case P_TEXT_SHADOW: st->ts_x=ps->ts_x; st->ts_y=ps->ts_y; st->ts_blur=ps->ts_blur; st->ts_color=ps->ts_color; break;
            case P_POSITION: st->position = ps->position; break; case P_BORDER_RADIUS: memcpy(st->radius, ps->radius, sizeof st->radius); break;
            default: break;
        }
        return;
    }
    if (!strcasecmp(val, "initial") || !strcasecmp(val, "unset") || !strcasecmp(val, "revert")) return;
    switch (prop) {
    case P_ASPECT_RATIO: {
        if (!strcasecmp(val, "auto")) { st->aspect_ratio = 0; return; }
        const char* p = val; while (isspace((unsigned char)*p)) p++; if (!strncasecmp(p, "auto", 4)) { p += 4; while (isspace((unsigned char)*p)) p++; }
        char* e = NULL; float a = strtof(p, &e); if (e == p || a <= 0) return; while (isspace((unsigned char)*e)) e++;
        float b = 1; if (*e == '/') { b = strtof(e + 1, &e); if (b <= 0) return; }
        st->aspect_ratio = a / b; return; }
    case P_ORDER: { long v = strtol(val, NULL, 10); if (v < -32768) v = -32768; if (v > 32767) v = 32767; st->order = (int16_t)v; return; }
    case P_TEXT_INDENT: if (len_of(c, val, &l)) { st->text_indent_len = l; st->text_indent = px_of(&l, SCREEN_WIDTH); } return;
    case P_WORD_SPACING: if (strcasecmp(val, "normal") == 0) { memset(&st->word_spacing_len, 0, sizeof st->word_spacing_len); st->word_spacing = 0; } else if (len_of(c, val, &l)) { st->word_spacing_len = l; st->word_spacing = px_of(&l, 0); } return;
    case P_INLINE_SIZE: case P_BLOCK_SIZE: case P_MIN_INLINE_SIZE: case P_MAX_INLINE_SIZE: case P_MIN_BLOCK_SIZE: case P_MAX_BLOCK_SIZE:
        if (len_of(c, val, &l)) {
            int inl = (prop == P_INLINE_SIZE || prop == P_MIN_INLINE_SIZE || prop == P_MAX_INLINE_SIZE);
            int ismin = (prop == P_MIN_INLINE_SIZE || prop == P_MIN_BLOCK_SIZE), ismax = (prop == P_MAX_INLINE_SIZE || prop == P_MAX_BLOCK_SIZE);
            int horizontal = st->writing_mode == 0;
            Len* dst;
            if (inl) dst = horizontal ? (ismin ? &st->min_w : ismax ? &st->max_w : &st->width) : (ismin ? &st->min_h : ismax ? &st->max_h : &st->height);
            else dst = horizontal ? (ismin ? &st->min_h : ismax ? &st->max_h : &st->height) : (ismin ? &st->min_w : ismax ? &st->max_w : &st->width);
            *dst = l;
        } return;
    case P_MARGIN_INLINE: logical_pair(c, val, st->margin, st, 1, 0); return;
    case P_MARGIN_BLOCK: logical_pair(c, val, st->margin, st, 0, 0); return;
    case P_PADDING_INLINE: logical_pair(c, val, st->padding, st, 1, 0); return;
    case P_PADDING_BLOCK: logical_pair(c, val, st->padding, st, 0, 0); return;
    case P_INSET_INLINE: logical_pair(c, val, st->inset, st, 1, 0); return;
    case P_INSET_BLOCK: logical_pair(c, val, st->inset, st, 0, 0); return;
    case P_MARGIN_INLINE_START: case P_MARGIN_INLINE_END: case P_MARGIN_BLOCK_START: case P_MARGIN_BLOCK_END:
    case P_PADDING_INLINE_START: case P_PADDING_INLINE_END: case P_PADDING_BLOCK_START: case P_PADDING_BLOCK_END:
    case P_INSET_INLINE_START: case P_INSET_INLINE_END: case P_INSET_BLOCK_START: case P_INSET_BLOCK_END: {
        if (!len_of(c, val, &l)) return;
        int inl = prop == P_MARGIN_INLINE_START || prop == P_MARGIN_INLINE_END || prop == P_PADDING_INLINE_START || prop == P_PADDING_INLINE_END || prop == P_INSET_INLINE_START || prop == P_INSET_INLINE_END;
        int start = prop == P_MARGIN_INLINE_START || prop == P_MARGIN_BLOCK_START || prop == P_PADDING_INLINE_START || prop == P_PADDING_BLOCK_START || prop == P_INSET_INLINE_START || prop == P_INSET_BLOCK_START;
        int side = logical_side(st, inl, start);
        if (prop == P_MARGIN_INLINE_START || prop == P_MARGIN_INLINE_END || prop == P_MARGIN_BLOCK_START || prop == P_MARGIN_BLOCK_END) st->margin[side] = l;
        else if (prop == P_PADDING_INLINE_START || prop == P_PADDING_INLINE_END || prop == P_PADDING_BLOCK_START || prop == P_PADDING_BLOCK_END) st->padding[side] = l;
        else st->inset[side] = l;
        return; }
    case P_OBJECT_FIT: st->object_fit = !strcasecmp(val,"contain") ? 1 : !strcasecmp(val,"cover") ? 2 : !strcasecmp(val,"none") ? 3 : !strcasecmp(val,"scale-down") ? 4 : 0; return;
    case P_OBJECT_POSITION: {
        char a[96], b[96]; const char* p = val; if (!tok_next(&p, a, sizeof a)) return; if (!tok_next(&p, b, sizeof b)) strcpy(b, "center");
        #define OBJ_POS(tok, dst) do { if (!strcasecmp((tok),"left") || !strcasecmp((tok),"top")) (dst)=0; else if (!strcasecmp((tok),"right") || !strcasecmp((tok),"bottom")) (dst)=100; else if (!strcasecmp((tok),"center")) (dst)=50; else (dst)=strtof((tok),NULL); } while(0)
        OBJ_POS(a, st->object_pos_x); OBJ_POS(b, st->object_pos_y);
        if (!strcasecmp(a,"top") || !strcasecmp(a,"bottom")) { st->object_pos_y = !strcasecmp(a,"top") ? 0 : 100; st->object_pos_x = !strcasecmp(b,"left") ? 0 : !strcasecmp(b,"right") ? 100 : 50; }
        #undef OBJ_POS
        return; }
    case P_POINTER_EVENTS: st->pointer_events = !strcasecmp(val,"none") ? 1 : 0; return;
    case P_USER_SELECT: st->user_select = !strcasecmp(val,"none") ? 1 : !strcasecmp(val,"all") ? 3 : !strcasecmp(val,"text") ? 2 : 0; return;
    case P_TEXT_SHADOW: parse_text_shadow(c, st, val); return;
    case P_TEXT_OVERFLOW: st->text_overflow = !strcasecmp(val,"ellipsis") ? 1 : 0; return;
    case P_WEBKIT_LINE_CLAMP: { int v = atoi(val); if (v < 0) v = 0; if (v > 255) v = 255; st->line_clamp = (uint8_t)v; if (v) st->overflow_hidden = 1; return; }
    case P_OUTLINE: parse_outline(c, st, val); return;
    case P_OUTLINE_OFFSET: if (len_of(c,val,&l)) st->outline_offset = px_of(&l,0); return;
    case P_MIX_BLEND_MODE: st->blend_mode = !strcasecmp(val,"multiply") ? 1 : !strcasecmp(val,"screen") ? 2 : !strcasecmp(val,"overlay") ? 3 : !strcasecmp(val,"darken") ? 4 : !strcasecmp(val,"lighten") ? 5 : !strcasecmp(val,"difference") ? 6 : !strcasecmp(val,"exclusion") ? 7 : 0; return;
    case P_SCROLL_BEHAVIOR: st->scroll_behavior = !strcasecmp(val,"smooth") ? 1 : 0; return;
    case P_SCROLL_SNAP_TYPE: { char t[40]; const char* p=val; if (!tok_next(&p,t,sizeof t) || !strcasecmp(t,"none")) st->scroll_snap_type=0; else if (!strcasecmp(t,"both")) st->scroll_snap_type=3; else if (!strcasecmp(t,"y")) st->scroll_snap_type=2; else st->scroll_snap_type=1; return; }
    case P_SCROLL_SNAP_ALIGN: { char t[40]; const char* p=val; if (!tok_next(&p,t,sizeof t)) st->scroll_snap_align=0; else st->scroll_snap_align=!strcasecmp(t,"start")?1:!strcasecmp(t,"center")?2:!strcasecmp(t,"end")?3:0; return; }
    case P_COUNTER_RESET:
        if (strlen(val) < sizeof st->counter_reset && css_tokens_fit(val, 39)) memcpy(st->counter_reset, val, strlen(val) + 1);
        else st->counter_reset[0] = 0;
        return;
    case P_COUNTER_INCREMENT:
        if (strlen(val) < sizeof st->counter_increment && css_tokens_fit(val, 39)) memcpy(st->counter_increment, val, strlen(val) + 1);
        else st->counter_increment[0] = 0;
        return;
    case P_COUNTER_SET:
        if (strlen(val) < sizeof st->counter_set && css_tokens_fit(val, 39)) memcpy(st->counter_set, val, strlen(val) + 1);
        else st->counter_set[0] = 0;
        return;
    case P_WRITING_MODE: st->writing_mode = !strcasecmp(val,"vertical-rl") ? 1 : !strcasecmp(val,"vertical-lr") ? 2 : 0; return;
    case P_DIRECTION: if (!strcasecmp(val,"rtl")) st->direction = 1; else if (!strcasecmp(val,"ltr")) st->direction = 0; return;
    case P_UNICODE_BIDI:
        st->unicode_bidi = !strcasecmp(val,"embed") ? 1 : !strcasecmp(val,"isolate") ? 2 :
                           !strcasecmp(val,"bidi-override") ? 3 : !strcasecmp(val,"isolate-override") ? 4 :
                           !strcasecmp(val,"plaintext") ? 5 : 0;
        return;
    case P_ZOOM: { float z = !strcasecmp(val,"normal") ? 1.0f : strtof(val,NULL); if (strchr(val,'%')) z /= 100.0f; if (z < 0.1f) z = 0.1f; if (z > 8.0f) z = 8.0f; st->zoom = z; return; }
    case P_TRANSLATE: case P_ROTATE: append_individual_transform(c, st, prop, val); return;
    case P_CONTAINER_TYPE: st->container_type = !strcasecmp(val,"size") ? 2 : !strcasecmp(val,"inline-size") ? 1 : 0; return;
    case P_CONTAINER_NAME: {
        char t[sizeof st->container_name]; const char* p=val;
        int status = tok_next_exact(&p, t, sizeof t);
        if (status > 0 && strcasecmp(t,"none")) { memcpy(st->container_name, t, strlen(t) + 1); st->container_name_set=1; }
        else { st->container_name[0]=0; st->container_name_set=0; }
        return;
    }
    case P_CONTAINER: {
        char a[sizeof st->container_name], b[48]; const char* p=val;
        a[0]=b[0]=0;
        int first = tok_next_exact(&p,a,sizeof a);
        if (first <= 0) { st->container_name[0]=0; st->container_name_set=0; return; }
        int second = tok_next(&p,b,sizeof b);
        if (!strcasecmp(a,"size")||!strcasecmp(a,"inline-size")||!strcasecmp(a,"normal")) {
            apply(c,n,st,P_CONTAINER_TYPE,a,0); st->container_name[0]=0; st->container_name_set=0;
        } else {
            if (a[0] && strcmp(a,"none")) { memcpy(st->container_name,a,strlen(a)+1); st->container_name_set=1; }
            if (second && b[0]) apply(c,n,st,P_CONTAINER_TYPE,b,0); else if (a[0]) apply(c,n,st,P_CONTAINER_TYPE,a,0);
        }
        return;
    }
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
    case P_TEXT_ALIGN:
        st->center_blocks = (!strcasecmp(val, "-webkit-center") || !strcasecmp(val, "-moz-center"));
        st->text_align = !strcasecmp(val,"start") ? ALIGN_START : !strcasecmp(val,"end") ? ALIGN_END :
                         !strcasecmp(val,"right") ? ALIGN_RIGHT : (!strcasecmp(val,"center") || !strcasecmp(val,"-webkit-center")) ? ALIGN_CENTER :
                         !strcasecmp(val,"justify") ? ALIGN_JUSTIFY : ALIGN_LEFT;
        return;
    case P_WHITE_SPACE: st->white_space = !strcasecmp(val, "nowrap") ? WS_NOWRAP : !strcasecmp(val, "pre") ? WS_PRE : (!strcasecmp(val, "pre-wrap") || !strcasecmp(val, "break-spaces")) ? WS_PRE_WRAP : !strcasecmp(val, "pre-line") ? WS_PRE_LINE : WS_NORMAL; return;
    case P_TEXT_WRAP: if (!strcasecmp(val,"nowrap")) st->text_wrap=1; else if (!strcasecmp(val,"balance")) st->text_wrap=2; else if (!strcasecmp(val,"pretty")) st->text_wrap=3; else if (!strcasecmp(val,"stable")) st->text_wrap=4; else if (!strcasecmp(val,"wrap")) st->text_wrap=0; return;
    case P_CONTENT_VISIBILITY: st->content_visibility = !strcasecmp(val,"hidden") ? 2 : !strcasecmp(val,"auto") ? 1 : 0; return;
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
        const char* p = val; char t[4][256]; int k = 0;
        while (k < 4 && tok_next(&p, tk, sizeof tk)) {
            size_t tl = strlen(tk);
            if (tl >= sizeof t[k]) return;
            memcpy(t[k], tk, tl + 1); k++;
        }
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
    case P_TRANSFORM: st->tr_x.set = st->tr_y.set = 0; parse_xform(c, &st->xf, val); return;
    case P_TRANSFORM_ORIGIN: parse_xform_origin(c, &st->xf, val); return;
    case P_FILTER: parse_filter_list(c, &st->filter, val); return;
    case P_BACKDROP_FILTER: parse_filter_list(c, &st->backdrop, val); return;
    case P_CLIP_PATH: parse_clip_path(c, &st->clip, val); return;
    case P_MASK: parse_mask_short(c, &st->mask, val); return;
    case P_MASK_IMAGE: { if (!strncasecmp(val, "none", 4)) { st->mask.kind = MK_NONE; return; } const char* mp = val; char mt[16384]; if (tok_next(&mp, mt, sizeof mt)) { FxMask* m = &st->mask; uint8_t keepk = m->kind; m->kind = MK_NONE; parse_mask_image(c, m, mt); if (!m->kind) m->kind = keepk == MK_NONE ? MK_NONE : keepk; } return; }
    case P_MASK_SIZE: parse_mask_pos_size(c, &st->mask, val, 1); return;
    case P_MASK_POS: parse_mask_pos_size(c, &st->mask, val, 0); return;
    case P_MASK_REPEAT: parse_mask_repeat(&st->mask, val); return;
    case P_TRANSITION: parse_transition_short(c, st, val); return;
    case P_TRANSITION_PROPERTY: parse_transition_list(c, st, 0, val); return;
    case P_TRANSITION_DURATION: parse_transition_list(c, st, 1, val); return;
    case P_TRANSITION_DELAY: parse_transition_list(c, st, 2, val); return;
    case P_TRANSITION_TIMING: parse_transition_list(c, st, 3, val); return;
    case P_ANIMATION: parse_animation_short(c, st, val); return;
    case P_ANIMATION_NAME: case P_ANIMATION_DURATION: case P_ANIMATION_DELAY: case P_ANIMATION_TIMING: case P_ANIMATION_ITER:
    case P_ANIMATION_DIR: case P_ANIMATION_FILL: case P_ANIMATION_PLAY: case P_ANIMATION_COMPOSITION:
        parse_animation_list(c, st, prop, val); return;
    case P_ANIMATION_TIMELINE: {
        if (!strcasecmp(val, "auto")) { st->animation_timeline = 0; return; }
        if (!strcasecmp(val, "none")) { st->animation_timeline = 3; return; }
        if (strcasestr(val, "view(")) st->animation_timeline = 2;
        else if (strcasestr(val, "scroll(")) st->animation_timeline = 1;
        else return;
        st->animation_timeline_axis = 0;
        if (strcasestr(val, "inline") || strcasestr(val, "x")) st->animation_timeline_axis = 1;
        if (strcasestr(val, " y") || strcasestr(val, "block")) st->animation_timeline_axis = 3;
        return;
    }
    case P_ANIMATION_RANGE: {
        if (!strcasecmp(val, "normal")) { st->animation_range_start = 0; st->animation_range_end = 1; return; }
        char a[48] = "", b[48] = ""; const char* p = val; while (isspace((unsigned char)*p)) p++;
        size_t k = 0; while (*p && !isspace((unsigned char)*p) && k + 1 < sizeof a) a[k++] = *p++; a[k] = 0;
        while (isspace((unsigned char)*p)) p++; k = 0; while (*p && !isspace((unsigned char)*p) && k + 1 < sizeof b) b[k++] = *p++; b[k] = 0;
        char* e = NULL; float x = strtof(a, &e); if (e == a) return; if (strchr(a, '%')) x /= 100.0f;
        float y = 1.0f; if (b[0]) { e = NULL; y = strtof(b, &e); if (e == b) return; if (strchr(b, '%')) y /= 100.0f; }
        if (x < 0) x = 0; if (x > 1) x = 1; if (y < x) y = x; if (y > 1) y = 1;
        st->animation_range_start = x; st->animation_range_end = y; return;
    }
    case P_COLUMN_COUNT: {
        if (!strcasecmp(val, "auto")) { st->column_count = 0; return; }
        char* e = NULL; long v = strtol(val, &e, 10); while (e && isspace((unsigned char)*e)) e++;
        if (e && e != val && !*e) { if (v < 1) v = 1; if (v > 32) v = 32; st->column_count = (uint8_t)v; } return;
    }
    case P_COLUMN_WIDTH:
        if (!strcasecmp(val, "auto")) { memset(&st->column_width, 0, sizeof st->column_width); return; }
        if (len_of(c, val, &l) && l.set && l.px + l.pct + l.vw + l.vh > 0) st->column_width = l; return;
    case P_COLUMNS: {
        st->column_count = 0; memset(&st->column_width, 0, sizeof st->column_width);
        const char* p = val;
        while (tok_next(&p, tk, sizeof tk)) {
            if (!strcasecmp(tk, "auto")) continue;
            char* e = NULL; long v = strtol(tk, &e, 10);
            if (e != tk && !*e) { if (v < 1) v = 1; if (v > 32) v = 32; st->column_count = (uint8_t)v; }
            else if (len_of(c, tk, &l) && l.set) st->column_width = l;
        } return;
    }
    case P_COLUMN_FILL: st->column_fill = !strcasecmp(val, "auto") ? 1 : 0; return;
    case P_COLUMN_SPAN: st->column_span = !strcasecmp(val, "all") ? 1 : 0; return;
    case P_COLUMN_RULE_WIDTH: {
        if (!strcasecmp(val, "thin")) st->column_rule_width = 1;
        else if (!strcasecmp(val, "medium")) st->column_rule_width = 3;
        else if (!strcasecmp(val, "thick")) st->column_rule_width = 5;
        else if (len_of(c, val, &l)) st->column_rule_width = px_of(&l, 0);
        if (st->column_rule_width < 0) st->column_rule_width = 0; return;
    }
    case P_COLUMN_RULE_STYLE: { int q = bstyle(val); if (q >= 0) st->column_rule_style = (uint8_t)q; return; }
    case P_COLUMN_RULE_COLOR: if (col_of(c, val, &col)) st->column_rule_color = col; return;
    case P_COLUMN_RULE: {
        st->column_rule_width = 3; st->column_rule_style = BS_SOLID; st->column_rule_color = st->color;
        const char* p = val;
        while (tok_next(&p, tk, sizeof tk)) {
            int bs = bstyle(tk); uint32_t cc;
            if (bs >= 0) st->column_rule_style = (uint8_t)bs;
            else if (!strcasecmp(tk, "thin")) st->column_rule_width = 1;
            else if (!strcasecmp(tk, "medium")) st->column_rule_width = 3;
            else if (!strcasecmp(tk, "thick")) st->column_rule_width = 5;
            else if (len_of(c, tk, &l)) st->column_rule_width = px_of(&l, 0);
            else if (col_of(c, tk, &cc)) st->column_rule_color = cc;
        } return;
    }
    case P_TEXT_ORIENTATION: st->text_orientation = !strcasecmp(val,"upright") ? 1 : !strcasecmp(val,"sideways") ? 2 : 0; return;
    case P_TEXT_COMBINE_UPRIGHT: st->text_combine_upright = (!strcasecmp(val,"all") || !strcasecmp(val,"digits")) ? 1 : 0; return;
    case P_CONTAIN_INTRINSIC_SIZE: {
        memset(&st->contain_intrinsic_w, 0, sizeof st->contain_intrinsic_w); memset(&st->contain_intrinsic_h, 0, sizeof st->contain_intrinsic_h);
        if (!strcasecmp(val, "none")) return;
        const char* p = val; char a[128], b[128]; int k = 0;
        if (tok_next(&p, a, sizeof a)) { if (!strcasecmp(a,"auto")) { if (tok_next(&p,b,sizeof b) && len_of(c,b,&l)) st->contain_intrinsic_w = st->contain_intrinsic_h = l; return; } if (len_of(c,a,&l)) { st->contain_intrinsic_h = l; k = 1; } else return; }
        if (tok_next(&p,b,sizeof b) && len_of(c,b,&l)) { if (k) st->contain_intrinsic_w = st->contain_intrinsic_h; st->contain_intrinsic_h = l; }
        else if(k) st->contain_intrinsic_w = st->contain_intrinsic_h;
        return;
    }
    case P_ANCHOR_NAME: {
        st->anchor_name[0] = 0; const char* p = val; char t[48];
        while (tok_next(&p, t, sizeof t)) if (!strcasecmp(t,"none")) { st->anchor_name[0]=0; return; } else if (t[0]=='-' && t[1]=='-') { snprintf(st->anchor_name,sizeof st->anchor_name,"%s",t); return; }
        return;
    }
    case P_POSITION_ANCHOR: {
        st->position_anchor[0]=0; const char* p=val; char t[48]; if (tok_next(&p,t,sizeof t) && t[0]=='-' && t[1]=='-') snprintf(st->position_anchor,sizeof st->position_anchor,"%s",t); return;
    }
    case P_POSITION_AREA: copy_trim(st->position_area,sizeof st->position_area,val); return;
    case P_VIEW_TRANSITION_NAME: copy_trim(st->view_transition_name,sizeof st->view_transition_name,val); return;
    case P_GRID_ROWS: if (!strncasecmp(val, "subgrid", 7) && (!val[7] || isspace((unsigned char)val[7]))) { st->grid_subgrid_rows=1; copy_trim(st->grid_rows, sizeof st->grid_rows, val); } else { st->grid_subgrid_rows=0; if (!strncasecmp(val, "none", 4)) st->grid_rows[0] = 0; else copy_trim(st->grid_rows, sizeof st->grid_rows, val); } return;
    case P_GRID_AUTO_ROWS: copy_trim(st->grid_auto_r, sizeof st->grid_auto_r, val); return;
    case P_GRID_AUTO_COLS: copy_trim(st->grid_auto_c, sizeof st->grid_auto_c, val); return;
    case P_GRID_AUTO_FLOW: st->grid_flow = (uint8_t)((strcasestr(val, "column") ? 1 : 0) | (strcasestr(val, "dense") ? 2 : 0)); return;
    case P_GRID_ROW: grid_line_pair(val, st->grid_row_s, sizeof st->grid_row_s, st->grid_row_e, sizeof st->grid_row_e); return;
    case P_GRID_ROW_START: copy_trim(st->grid_row_s, sizeof st->grid_row_s, val); return;
    case P_GRID_ROW_END: copy_trim(st->grid_row_e, sizeof st->grid_row_e, val); return;
    case P_GRID_COL_START: copy_trim(st->grid_col_s, sizeof st->grid_col_s, val); return;
    case P_GRID_COL_END: copy_trim(st->grid_col_e, sizeof st->grid_col_e, val); return;
    case P_JUSTIFY_ITEMS: st->justify_items = (uint8_t)ai_of(val, AI_STRETCH); return;
    case P_JUSTIFY_SELF: st->justify_self = (uint8_t)ai_of(val, AI_AUTO); return;
    case P_ALIGN_CONTENT: st->align_content = (uint8_t)jc_of(val); return;
    case P_PLACE_ITEMS: case P_PLACE_SELF: { const char* pp = val; char a1[48], a2[48]; if (!tok_next(&pp, a1, sizeof a1)) return; if (!tok_next(&pp, a2, sizeof a2)) snprintf(a2, sizeof a2, "%s", a1);
        if (prop == P_PLACE_ITEMS) { st->align_items = (uint8_t)ai_of(a1, AI_STRETCH); st->justify_items = (uint8_t)ai_of(a2, AI_STRETCH); } else { st->align_self = (uint8_t)ai_of(a1, AI_AUTO); st->justify_self = (uint8_t)ai_of(a2, AI_AUTO); } return; }
    case P_PLACE_CONTENT: { const char* pp = val; char a1[48], a2[48]; if (!tok_next(&pp, a1, sizeof a1)) return; if (!tok_next(&pp, a2, sizeof a2)) snprintf(a2, sizeof a2, "%s", a1); st->align_content = (uint8_t)jc_of(a1); st->justify = (uint8_t)jc_of(a2); return; }
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
    case P_GRID_COLS: if (!strncasecmp(val, "subgrid", 7) && (!val[7] || isspace((unsigned char)val[7]))) { st->grid_subgrid_cols=1; snprintf(st->grid_cols,sizeof st->grid_cols,"subgrid"); } else { st->grid_subgrid_cols=0; if (!strncasecmp(val,"none",4)) st->grid_cols[0]=0; else snprintf(st->grid_cols,sizeof st->grid_cols,"%s",val); } return;
    case P_GRID_TEMPLATE: {                                          /* шорткат «строки / колонки»; строки с именами областей в кавычках идут и в grid-template-areas */
        const char* sl = strrchr(val, '/');
        if (sl) { const char* p2 = sl + 1; while (isspace((unsigned char)*p2)) p2++; snprintf(st->grid_cols, sizeof st->grid_cols, "%s", p2); }
        size_t hl = sl ? (size_t)(sl - val) : strlen(val); char head[400]; if (hl > 399) hl = 399; memcpy(head, val, hl); head[hl] = 0;
        if (strchr(head, '\'') || strchr(head, '"')) {
            apply(c, n, st, P_GRID_AREAS, head, size_pass);
            char rows[160]; size_t ro = 0; rows[0] = 0; const char* q = head; int expect = 0;     /* после строки-области может идти размер ряда */
            while (*q) {
                while (isspace((unsigned char)*q)) q++; if (!*q) break;
                if (*q == '\'' || *q == '"') { if (expect && ro + 6 < sizeof rows) { memcpy(rows + ro, "auto ", 5); ro += 5; } char qc = *q++; while (*q && *q != qc) q++; if (*q) q++; expect = 1; continue; }
                const char* e = q; int d = 0; while (*e && (d > 0 || !isspace((unsigned char)*e)) && *e != '\'' && *e != '"') { if (*e == '(') d++; else if (*e == ')') d--; e++; }
                size_t tl = (size_t)(e - q); if (tl && ro + tl + 2 < sizeof rows) { memcpy(rows + ro, q, tl); ro += tl; rows[ro++] = ' '; } expect = 0; q = e ? e : q + 1; if (e == q && !*q) break;
            }
            if (expect && ro + 5 < sizeof rows) { memcpy(rows + ro, "auto ", 5); ro += 5; } rows[ro] = 0; copy_trim(st->grid_rows, sizeof st->grid_rows, rows);
        } else if (sl && strncasecmp(head, "none", 4) != 0) copy_trim(st->grid_rows, sizeof st->grid_rows, head);
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
    case P_GRID_AREA: {
        if (strchr(val, '/')) {                                       /* row-start / col-start / row-end / col-end */
            char part[4][256] = {{0}}; int k = split_slash4(val, part);
            copy_trim(st->grid_row_s, sizeof st->grid_row_s, part[0]); copy_trim(st->grid_col_s, sizeof st->grid_col_s, k > 1 ? part[1] : part[0]);
            copy_trim(st->grid_row_e, sizeof st->grid_row_e, k > 2 ? part[2] : (k > 1 ? "" : part[0])); copy_trim(st->grid_col_e, sizeof st->grid_col_e, k > 3 ? part[3] : (k > 2 ? "" : (k > 1 ? "" : part[0])));
            return;
        }
        const char* p2 = val; while (isspace((unsigned char)*p2)) p2++; char* e = st->grid_area_name; size_t i = 0; while (p2[i] && !isspace((unsigned char)p2[i]) && i < sizeof st->grid_area_name - 1) { e[i] = p2[i]; i++; } e[i] = 0; return; }
    case P_GRID_COLUMN: {
        grid_line_pair(val, st->grid_col_s, sizeof st->grid_col_s, st->grid_col_e, sizeof st->grid_col_e);
        const char* sp = strcasestr(val, "span");
        if (sp) { int n2 = atoi(sp + 4); st->grid_span = (uint8_t)(n2 > 0 && n2 < 100 ? n2 : 1); }
        else { const char* sl = strchr(val, '/'); if (sl) { int a2 = atoi(val), b2 = atoi(sl + 1); st->grid_span = (uint8_t)(b2 < 0 ? 99 : (b2 > a2 ? b2 - a2 : 1)); } else st->grid_span = 1; }
        return; }
    case P_ROW_GAP: if (len_of(c, val, &l)) st->gap_r = px_of(&l, 0); return; case P_COL_GAP: if (len_of(c, val, &l)) st->gap_c = px_of(&l, 0); return;
    default: return;
    }
    (void)set_margin_auto;
}


/* свойство-декларация → биты анимируемых свойств */
static uint64_t decl_ap_mask(int prop) {
    switch (prop) {
    case P_OPACITY: return APM(AP_OPACITY); case P_COLOR: return APM(AP_COLOR); case P_BG_COLOR: return APM(AP_BG);
    case P_BACKGROUND: return APM(AP_BG) | APM(AP_GRAD) | APM(AP_BGPOS); case P_BG_IMAGE: return APM(AP_GRAD); case P_BG_POS: return APM(AP_BGPOS);
    case P_BORDER_COLOR: case P_BORDER: return APM(AP_BC0) | APM(AP_BC1) | APM(AP_BC2) | APM(AP_BC3) | (prop == P_BORDER ? APM(AP_BW0) | APM(AP_BW1) | APM(AP_BW2) | APM(AP_BW3) : 0);
    case P_BC_T: return APM(AP_BC0); case P_BC_R: return APM(AP_BC1); case P_BC_B: return APM(AP_BC2); case P_BC_L: return APM(AP_BC3);
    case P_BORDER_T: return APM(AP_BC0) | APM(AP_BW0); case P_BORDER_R: return APM(AP_BC1) | APM(AP_BW1); case P_BORDER_B: return APM(AP_BC2) | APM(AP_BW2); case P_BORDER_L: return APM(AP_BC3) | APM(AP_BW3);
    case P_BORDER_WIDTH: return APM(AP_BW0) | APM(AP_BW1) | APM(AP_BW2) | APM(AP_BW3);
    case P_BW_T: return APM(AP_BW0); case P_BW_R: return APM(AP_BW1); case P_BW_B: return APM(AP_BW2); case P_BW_L: return APM(AP_BW3);
    case P_WIDTH: return APM(AP_WIDTH); case P_HEIGHT: return APM(AP_HEIGHT); case P_MIN_W: return APM(AP_MINW); case P_MAX_W: return APM(AP_MAXW); case P_MIN_H: return APM(AP_MINH); case P_MAX_H: return APM(AP_MAXH);
    case P_MARGIN: return APM(AP_MG0) | APM(AP_MG1) | APM(AP_MG2) | APM(AP_MG3); case P_MARGIN_T: return APM(AP_MG0); case P_MARGIN_R: return APM(AP_MG1); case P_MARGIN_B: return APM(AP_MG2); case P_MARGIN_L: return APM(AP_MG3);
    case P_PADDING: return APM(AP_PD0) | APM(AP_PD1) | APM(AP_PD2) | APM(AP_PD3); case P_PADDING_T: return APM(AP_PD0); case P_PADDING_R: return APM(AP_PD1); case P_PADDING_B: return APM(AP_PD2); case P_PADDING_L: return APM(AP_PD3);
    case P_INSET: return APM(AP_IN0) | APM(AP_IN1) | APM(AP_IN2) | APM(AP_IN3); case P_TOP: return APM(AP_IN0); case P_RIGHT: return APM(AP_IN1); case P_BOTTOM: return APM(AP_IN2); case P_LEFT: return APM(AP_IN3);
    case P_BORDER_RADIUS: return APM(AP_RAD0) | APM(AP_RAD1) | APM(AP_RAD2) | APM(AP_RAD3); case P_BR_TL: return APM(AP_RAD0); case P_BR_TR: return APM(AP_RAD1); case P_BR_BR: return APM(AP_RAD2); case P_BR_BL: return APM(AP_RAD3);
    case P_FONT_SIZE: return APM(AP_FONTSZ); case P_LINE_HEIGHT: return APM(AP_LH); case P_BOX_SHADOW: return APM(AP_SHADOW); case P_FLEX_GROW: return APM(AP_FLEXG);
    case P_GAP: return APM(AP_GAPR) | APM(AP_GAPC); case P_ROW_GAP: return APM(AP_GAPR); case P_COL_GAP: return APM(AP_GAPC);
    case P_FILTER: return APM(AP_FILTER); case P_BACKDROP_FILTER: return APM(AP_BACKDROP); case P_CLIP_PATH: return APM(AP_CLIP);
    case P_TRANSFORM: return APM(AP_XFORM); case P_TRANSFORM_ORIGIN: return APM(AP_ORIGIN);
    default: return 0;
    }
}

/* Стиль ключевого кадра: стиль узла + декларации кадра (для анимации) */
static uint64_t kf_build(void* ctx, DOMNode* n, const Decl* decls, int nd, NodeStyle* out, float root_fs) {
    Doc* d = ctx; DOMNode* pa = par(d, n); Cx cx; memset(&cx, 0, sizeof cx);
    cx.parent = pa; cx.pfs = pa ? pa->st.font_size : 16; cx.rem = root_fs; cx.owner = n; cx.doc = d; cx.parent_lh = pa ? (pa->st.lh_px > 0 ? pa->st.lh_px : pa->st.lh_mult > 0 ? pa->st.lh_mult * pa->st.font_size : pa->st.font_size * 1.2f) : cx.pfs * 1.2f; cx.lh = cx.parent_lh; CP* cps = d->cps ? d->cps[n->idx] : NULL; char resolved[1024];
    uint64_t mask = 0;
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < nd; i++) {
        const Decl* dc = &decls[i]; if (pass == 1) mask |= decl_ap_mask(dc->prop); if (dc->prop == P_CUSTOM || dc->prop == P_FONT_FAMILY || dc->prop == P_FONT || dc->prop == P_CONTENT) continue;
        const char* v = dc->val; if (strstr(v, "var(")) { if (!resolve_vars(cps, v, resolved, sizeof resolved, 0)) continue; v = resolved; }
        cx.em = pass == 0 ? cx.pfs : out->font_size; cx.cur = out->color;
        apply(&cx, n, out, dc->prop, v, pass == 0);
    }
    /* Keyframe builds do not run finish_style(); keep the animation-facing
       opacity field coherent with the declaration just applied. */
    if (mask & APM(AP_OPACITY)) out->op_own = out->opacity;
    return mask;
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
    st->display = DSP_INLINE; st->font_size = 16; st->color = 0xFF000000u; st->opacity = 1; st->flex_shrink = 1; st->zoom = 1.0f; st->object_pos_x = st->object_pos_y = 50.0f; st->outline_c = st->color; st->outline_w = 3; st->outline_style = BS_SOLID;
    st->spacing = 2; st->text_align = ALIGN_START; st->animation_range_start = 0.0f; st->animation_range_end = 1.0f; st->column_rule_color = 0xFF000000u; st->border_style[0] = st->border_style[1] = st->border_style[2] = st->border_style[3] = BS_NONE;
    for (int i = 0; i < 4; i++) { st->border_w[i] = 3; st->border_c[i] = CUR_COLOR; }
    st->sh_color = 0; st->list_style = LS_DISC; st->align_self = AI_AUTO;
    for (int i = 0; i < 4; i++) { st->margin[i].set = 1; st->padding[i].set = 1; }
    st->grid_span = 1; st->justify_self = AI_AUTO; st->xf.ox.pct = st->xf.oy.pct = 50;
    if (pa) {
        const NodeStyle* ps = &pa->st;
        st->color = ps->color; st->font_size = ps->font_size; st->lh_px = ps->lh_px; st->lh_mult = ps->lh_mult; st->bold = ps->bold; st->weight = ps->weight; st->italic = ps->italic;
        st->text_align = ps->text_align; st->white_space = ps->white_space; st->text_wrap = ps->text_wrap; st->bidi_context = ps->bidi_context; st->text_transform = ps->text_transform; st->visibility = ps->visibility;
        st->center_blocks = ps->center_blocks; st->list_style = ps->list_style; st->pointer = ps->pointer; st->break_word = ps->break_word; st->spacing = ps->spacing;
        st->deco = ps->deco; st->pointer_events = ps->pointer_events; st->user_select = ps->user_select; st->writing_mode = ps->writing_mode; st->direction = ps->direction; st->text_indent_len = ps->text_indent_len; st->text_indent = ps->text_indent; st->word_spacing_len = ps->word_spacing_len; st->word_spacing = ps->word_spacing; st->ts_x = ps->ts_x; st->ts_y = ps->ts_y; st->ts_blur = ps->ts_blur; st->ts_color = ps->ts_color; snprintf(n->font_family, sizeof n->font_family, "%s", pa->font_family);
        /* фон-градиент и background-clip:text передаём вниз на анонимные текстовые узлы,
           иначе они не видят фон, объявленный на родительском элементе (h1, span и т.п.) */
        st->bg_clip_text = ps->bg_clip_text;
        st->bg_grad_type = ps->bg_grad_type; st->bg_grad_n = ps->bg_grad_n; st->bg_grad_angle = ps->bg_grad_angle;
        memcpy(st->bg_grad_c, ps->bg_grad_c, sizeof st->bg_grad_c); memcpy(st->bg_grad_p, ps->bg_grad_p, sizeof st->bg_grad_p);
    }
}

/* Правила, подходящие узлу. pseudo: 0 — для самого элемента, 1/2 — для его ::before/::after */
static int container_query_matches(Doc* d, DOMNode* n, const Rule* r) {
    if (!r->has_container_query) return 1;
    for (DOMNode* a=par(d,n); a; a=par(d,a)) {
        const NodeStyle* st=&a->st;
        if (!st->container_type) continue;
        if (r->cq_name[0] && (!st->container_name_set || strcmp(st->container_name,r->cq_name))) continue;
        float w=0,h=0;
        if (st->width.set) w=px_of(&st->width, SCREEN_WIDTH);
        else if (a->idx==0) w=(float)SCREEN_WIDTH;
        if (st->height.set) h=px_of(&st->height, SCREEN_HEIGHT);
        else if (a->idx==0) h=(float)SCREEN_HEIGHT;
        if (r->cq_flags & 3) {
            if (st->container_type<1 || (!st->width.set && a->idx!=0)) continue;
            if (((r->cq_flags & 1) && w+0.01f<r->cq_min_w) || ((r->cq_flags & 2) && w-0.01f>r->cq_max_w)) return 0;
        }
        if (r->cq_flags & 12) {
            if (st->container_type<2 || (!st->height.set && a->idx!=0)) continue;
            if (((r->cq_flags & 4) && h+0.01f<r->cq_min_h) || ((r->cq_flags & 8) && h-0.01f>r->cq_max_h)) return 0;
        }
        return 1;
    }
    return 0;
}

static void gather_rules(Doc* d, RIndex* ix, DOMNode* n, GV* g, int pseudo) {
    Rule** cand[64]; int ncand[64]; int nc = 0; char key[80];
    if (n->id[0]) {
        size_t id_len = strlen(n->id);
        /* Selectors store ids exactly up to 47 bytes. Skip lookup for longer
           element ids instead of hashing a truncated key. */
        if (id_len < sizeof(((Simple*)0)->id) && id_len + 1 < sizeof key) {
            key[0] = '#'; memcpy(key + 1, n->id, id_len + 1);
            HB* b = hb_get(ix, key, 0); if (b) { cand[nc] = b->v; ncand[nc++] = b->n; }
        }
    }
    { const char* p = n->class_name; while (*p && nc < 60) {
        while (isspace((unsigned char)*p)) p++; const char* e = p; while (*e && !isspace((unsigned char)*e)) e++;
        if (e > p && e - p < 70) { key[0] = '.'; memcpy(key + 1, p, (size_t)(e - p)); key[1 + (e - p)] = 0; HB* b = hb_get(ix, key, 0); if (b) { cand[nc] = b->v; ncand[nc++] = b->n; } }
        p = e; } }
    { snprintf(key, sizeof key, "T%s", n->tag); HB* b = hb_get(ix, key, 0); if (b) { cand[nc] = b->v; ncand[nc++] = b->n; } }
    cand[nc] = ix->uni; ncand[nc++] = ix->nuni;
    for (int c = 0; c < nc; c++) for (int i = 0; i < ncand[c]; i++) {
        Rule* r = cand[c][i];
        if (r->pseudo != pseudo) continue;
        if (!container_query_matches(d,n,r)) continue;
        if (match_at(d, n, r->sel, r->nsel - 1)) gv_push(g, r->decls, r->ndecls, mkkey(r->origin, r->spec, r->order));
    }
}

/* Применяет отсортированные группы деклараций к узлу n (custom-свойства, font-size, затем остальное; обычные, затем !important) */
static void apply_groups(Doc* d, DOMNode* n, DOMNode* pa, GV* g, float root_fs, CP** cps, char* content_out, DOMNode* owner) {
    Cx cx; memset(&cx, 0, sizeof cx); cx.parent = pa; cx.pfs = pa ? pa->st.font_size : 16; cx.rem = root_fs; cx.em = cx.pfs; cx.cur = n->st.color; cx.doc = d; cx.parent_lh = pa ? (pa->st.lh_px > 0 ? pa->st.lh_px : pa->st.lh_mult > 0 ? pa->st.lh_mult * pa->st.font_size : pa->st.font_size * 1.2f) : cx.pfs * 1.2f; cx.lh = cx.parent_lh;
    cx.content = content_out; cx.has_content = 0; cx.owner = owner;
    (void)d;
    /* 1) registered-property defaults (element cascade only), then authored --x values. */
    if (!content_out) {
        cp_seed_registered(d->rs,cps);
        for (int imp = 0; imp < 2; imp++) for (int i = 0; i < g->n; i++) for (int k = 0; k < g->v[i].nd; k++) {
            Decl* dc = &g->v[i].d[k]; if (dc->prop != P_CUSTOM || dc->imp != imp) continue;
            const RegisteredProperty* rp=css_registered_property(d->rs,dc->name);
            if (rp && !css_registered_value_valid(rp,dc->val)) continue;
            CP* cp = malloc(sizeof(CP)); if(!cp) continue;
            cp->name = dc->name; cp->val = dc->val; cp->next = *cps; *cps = cp;   /* name/val live in arena */
        }
    }
    /* 2) font-size, then precompute line-height before resolving the lh unit in lengths. */
    char resolved[1024];
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            cx.em = n->st.font_size;
            /* `lh` on font/line-height itself is relative to the parent's line box. */
            cx.lh = cx.parent_lh;
            for (int imp = 0; imp < 2; imp++) for (int i = 0; i < g->n; i++) for (int k = 0; k < g->v[i].nd; k++) {
                Decl* dc = &g->v[i].d[k]; if (dc->imp != imp || (dc->prop != P_LINE_HEIGHT && dc->prop != P_FONT)) continue;
                const char* v = dc->val;
                if (strstr(v, "var(")) { if (!resolve_vars(*cps, v, resolved, sizeof resolved, 0)) continue; v = resolved; }
                apply(&cx, n, &n->st, dc->prop, v, 0);
            }
            cx.lh = n->st.lh_px > 0 ? n->st.lh_px : n->st.lh_mult > 0 ? n->st.lh_mult * n->st.font_size : n->st.font_size * 1.2f;
        }
        for (int imp = 0; imp < 2; imp++) for (int i = 0; i < g->n; i++) for (int k = 0; k < g->v[i].nd; k++) {
            Decl* dc = &g->v[i].d[k]; if (dc->imp != imp || dc->prop == P_CUSTOM) continue;
            const char* v = dc->val;
            if (strstr(v, "var(")) { if (!resolve_vars(*cps, v, resolved, sizeof resolved, 0)) continue; v = resolved; }
            cx.em = pass == 0 ? cx.pfs : n->st.font_size;
            if (pass == 0) cx.lh = cx.parent_lh; else cx.cur = n->st.color;
            apply(&cx, n, &n->st, dc->prop, v, pass == 0);
        }
    }
    if (content_out) { if (!cx.has_content) content_out[0] = 0; owner->aux = owner->aux; }
    /* пометка «content задан» передаётся через content_out[247]: 1 — есть */
    if (content_out) content_out[247] = (char)cx.has_content;
}

/* Пост-обработка вычисленного стиля (общая для элементов и псевдоэлементов) */
static void finish_style(DOMNode* n, DOMNode* pa) {
    NodeStyle* st = &n->st;
    /* -webkit-line-clamp is normally paired with display:-webkit-box; this engine's
       legacy box value maps to flex, but clamping is performed by the inline formatter. */
    if (st->line_clamp && st->display != DSP_NONE) { st->display = DSP_BLOCK; st->is_grid = 0; }
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
    if (st->bg_url[0] && strncmp(st->bg_url, "data:", 5) != 0 && strncmp(st->bg_url, "cache/data_", 11) != 0) {
        char ab[8192];
        webpage_resolve_url(g_base, st->bg_url, ab, sizeof ab);
        if (!ab[0] || strlen(ab) >= sizeof st->bg_url) st->bg_url[0] = 0;
        else memcpy(st->bg_url, ab, strlen(ab) + 1);
    }
    st->op_own = st->opacity; st->has_fx = (uint8_t)fx_style_has_fx(st);
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
    int content_state = (int)(signed char)pb->content[247]; pb->content[247] = 0;
    pb->content_state = (int8_t)content_state;
    NodeStyle* st = &tmp->st;
    /* ::before/::after require generated content; ::marker also styles the UA marker
       when `content` is normal or omitted. `content:none` suppresses it. */
    if ((which == 3 ? 1 : content_state == 1) && st->display != DSP_NONE) {
        finish_style(tmp, n);
        pb->st = tmp->st; memcpy(pb->font_family, tmp->font_family, sizeof pb->font_family);
        n->pb[which - 1] = pb;
    } else free(pb);
    free(tmp);
}

static int vt_style_name_valid(const char* name) {
    if (!name || !*name || strlen(name) >= sizeof(((VTStyleRecord*)0)->name) ||
        !strcmp(name,"none") || !strcmp(name,"auto") || !strcmp(name,"initial") ||
        !strcmp(name,"inherit") || !strcmp(name,"unset") || !strcmp(name,"revert") ||
        !strcmp(name,"revert-layer")) return 0;
    const unsigned char* p=(const unsigned char*)name;
    if (!(isalpha(*p) || *p=='_' || *p=='-' || *p>=0x80)) return 0;
    for (p++; *p; p++) if (!(isalnum(*p) || *p=='_' || *p=='-' || *p>=0x80)) return 0;
    return 1;
}

static int vt_pseudo_parent_kind(int child) {
    switch (child) {
        case WEBPAGE_VT_PSEUDO_GROUP: return WEBPAGE_VT_PSEUDO_ROOT;
        case WEBPAGE_VT_PSEUDO_IMAGE_PAIR: return WEBPAGE_VT_PSEUDO_GROUP;
        case WEBPAGE_VT_PSEUDO_OLD: case WEBPAGE_VT_PSEUDO_NEW: return WEBPAGE_VT_PSEUDO_IMAGE_PAIR;
        default: return WEBPAGE_VT_PSEUDO_NONE;
    }
}

static int vt_simple_host_matches(Doc* d, DOMNode* root, const Simple* s) {
    int constrained=s->tag[0] || s->id[0] || s->ncls || s->nat || s->npc || s->sub;
    if (!constrained) return 1;
    Simple host=*s; host.pseudo=WEBPAGE_VT_PSEUDO_NONE; host.pseudo_arg[0]=0;
    return match_simple(d,root,&host);
}

static int vt_rule_matches(Doc* d, DOMNode* root, const Rule* r, int pseudo, const char* name) {
    if (!r || !r->nsel || r->pseudo!=pseudo) return 0;
    const Simple* last=&r->sel[r->nsel-1];
    if (pseudo==WEBPAGE_VT_PSEUDO_ROOT) {
        if (last->pseudo_arg[0]) return 0;
    } else if (!name || strcmp(last->pseudo_arg,name)) return 0;
    for (int i=r->nsel-1; i>0; i--) {
        const Simple* child=&r->sel[i]; const Simple* parent=&r->sel[i-1];
        if (child->pseudo < WEBPAGE_VT_PSEUDO_ROOT || child->pseudo > WEBPAGE_VT_PSEUDO_NEW ||
            parent->pseudo < WEBPAGE_VT_PSEUDO_ROOT || parent->pseudo > WEBPAGE_VT_PSEUDO_NEW) return 0;
        if (child->comb!='>' || vt_pseudo_parent_kind(child->pseudo)!=parent->pseudo) return 0;
        if (child->pseudo!=WEBPAGE_VT_PSEUDO_GROUP && strcmp(child->pseudo_arg,parent->pseudo_arg)) return 0;
        if (child->pseudo==WEBPAGE_VT_PSEUDO_GROUP && parent->pseudo!=WEBPAGE_VT_PSEUDO_ROOT &&
            strcmp(child->pseudo_arg,parent->pseudo_arg)) return 0;
        if (parent->pseudo==WEBPAGE_VT_PSEUDO_ROOT && !vt_simple_host_matches(d,root,parent)) return 0;
    }
    const Simple* first=&r->sel[0];
    if (first->pseudo==WEBPAGE_VT_PSEUDO_ROOT) return vt_simple_host_matches(d,root,first);
    /* A selector may start at any named pseudo-element; its ancestors are
       implicit in the transition tree unless the author explicitly chains them. */
    if (r->nsel==1) return vt_simple_host_matches(d,root,first);
    return 1;
}

static void vt_compute_pseudo_style(Doc* d, RuleSet* rs, DOMNode* root,
                                    const NodeStyle* inherited, int pseudo,
                                    const char* name, float root_fs, NodeStyle* out) {
    GV g={0};
    for (int i=0; i<rs->n; i++) {
        Rule* r=rs->v[i];
        if (r->pseudo != pseudo || !r->nsel) continue;
        /* Match against the generated View Transition pseudo tree rather than
           treating pseudo-elements as actual DOM ancestors. */
        if (vt_rule_matches(d,root,r,pseudo,name))
            gv_push(&g,r->decls,r->ndecls,mkkey(r->origin,r->spec,r->order));
    }
    if (g.n>1) qsort(g.v,(size_t)g.n,sizeof(Grp),grp_cmp);
    DOMNode parent_fake=*root, tmp; memset(&tmp,0,sizeof tmp);
    parent_fake.st=*inherited; tmp.idx=root->idx;
    init_style(&tmp,&parent_fake);
    CP* cp=d->cps[root->idx]; CP* base_cp=cp;
    apply_groups(d,&tmp,&parent_fake,&g,root_fs,&cp,NULL,root);
    finish_style(&tmp,&parent_fake);
    *out=tmp.st;
    while (cp && cp!=base_cp) { CP* next=cp->next; free(cp); cp=next; }
    free(g.v);
}

static void vt_add_style_name(const char* name) {
    if (!vt_style_name_valid(name)) return;
    for (int i=0;i<g_vt_style_count;i++) if (!strcmp(g_vt_style_records[i].name,name)) return;
    if (g_vt_style_count>=VT_STYLE_MAX_NAMES) return;
    snprintf(g_vt_style_records[g_vt_style_count++].name,sizeof g_vt_style_records[0].name,"%s",name);
}

static void vt_compute_styles(Doc* d, RuleSet* rs, float root_fs) {
    vt_free_all_anim_lists();
    memset(&g_vt_root_style,0,sizeof g_vt_root_style);
    memset(g_vt_style_records,0,sizeof g_vt_style_records);
    g_vt_style_count=0;
    if (!d || d->n<=0 || !d->nv[0]) return;
    DOMNode* root=d->nv[0];
    vt_compute_pseudo_style(d,rs,root,&root->st,WEBPAGE_VT_PSEUDO_ROOT,"",root_fs,&g_vt_root_style);
    vt_prepare_anim_list(d,rs,root,root_fs,&g_vt_root_style,&g_vt_root_anim);
    for (DOMNode* n=root; n; n=n->next) vt_add_style_name(n->st.view_transition_name);
    for (int i=0;i<rs->n;i++) {
        Rule* r=rs->v[i];
        if (r->pseudo>=WEBPAGE_VT_PSEUDO_GROUP && r->pseudo<=WEBPAGE_VT_PSEUDO_NEW && r->nsel)
            vt_add_style_name(r->sel[r->nsel-1].pseudo_arg);
    }
    for (int i=0;i<g_vt_style_count;i++) {
        VTStyleRecord* rec=&g_vt_style_records[i];
        vt_compute_pseudo_style(d,rs,root,&g_vt_root_style,WEBPAGE_VT_PSEUDO_GROUP,rec->name,root_fs,&rec->group);
        vt_prepare_anim_list(d,rs,root,root_fs,&rec->group,&rec->group_anim);
        vt_compute_pseudo_style(d,rs,root,&rec->group,WEBPAGE_VT_PSEUDO_IMAGE_PAIR,rec->name,root_fs,&rec->image_pair);
        vt_prepare_anim_list(d,rs,root,root_fs,&rec->image_pair,&rec->image_pair_anim);
        vt_compute_pseudo_style(d,rs,root,&rec->image_pair,WEBPAGE_VT_PSEUDO_OLD,rec->name,root_fs,&rec->old_image);
        vt_prepare_anim_list(d,rs,root,root_fs,&rec->old_image,&rec->old_anim);
        vt_compute_pseudo_style(d,rs,root,&rec->image_pair,WEBPAGE_VT_PSEUDO_NEW,rec->name,root_fs,&rec->new_image);
        vt_prepare_anim_list(d,rs,root,root_fs,&rec->new_image,&rec->new_anim);
    }
}

static int pseudo_capable(const DOMNode* n) {
    switch (n->type) { case TAG_IMG: case TAG_INPUT: case TAG_TEXTAREA: case TAG_SELECT: case TAG_BR: case TAG_HR: case TAG_TEXT: case TAG_IFRAME: case TAG_VIDEO: case TAG_AUDIO: case TAG_CANVAS: case TAG_SVG: return 0; default: break; }
    return strcmp(n->tag, "hr") && strcmp(n->tag, "object") && strcmp(n->tag, "embed") && strcmp(n->tag, "svg");
}

static void cascade_node(Doc* d, RIndex* ix, RuleSet* rs, DOMNode* n, float root_fs) {
    (void)rs;
    DOMNode* pa = par(d, n);
    anim_before_node(n);
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
    if (st->unicode_bidi == 3 || st->unicode_bidi == 4)
        st->bidi_context = st->direction ? 2 : 1; /* propagate the inline override context to descendant text nodes */
    else if (st->unicode_bidi == 5)
        st->bidi_context = 3; /* plaintext: first-strong paragraph direction */
    if (n->idx == 0) st->display = DSP_BLOCK;
    if (st->display == DSP_NONE) return;
    finish_style(n, pa);
    if (ix->has_pseudo && pseudo_capable(n)) { cascade_pseudo(d, ix, n, 1, root_fs); cascade_pseudo(d, ix, n, 2, root_fs); }
    if (ix->has_marker && st->display == DSP_LIST_ITEM && pseudo_capable(n)) cascade_pseudo(d, ix, n, 3, root_fs);
}

static int g_style_offline = 0;
void style_apply(DOMNode* head, SheetList* sheets, const char* base_url) {
    Arena A = {0}; RuleSet rs = {0}; RIndex ix; memset(&ix, 0, sizeof ix);
    css_fetch_budget = g_style_offline ? 0 : 12; g_base = base_url; webpage_set_doc_url(base_url);
    css_parse_sheet(&A, &rs, UA_CSS, NULL, 0, 0);
    css_dyn_seen = 0; int nsheet = 0;
    for (int i = 0; i < sheets->n; i++) {
        SheetSrc* s = &sheets->v[i];
        if (!css_media_matches(s->media)) continue;
        if (!s->css && s->href && nsheet < 10 && css_fetch_budget > 0 && !strncmp(s->href, "http", 4) && !webpage_mixed_blocked(s->href)) {      /* скачиваем и сохраняем текст в бандле */
            css_fetch_budget--; nsheet++;
            s->css = nal_http_get_kind(s->href, NAL_RESOURCE_STYLE);
        }
        if (s->css) css_parse_sheet(&A, &rs, s->css, s->href ? s->href : base_url, 2, 0);
    }
    index_rules(&ix, &rs); g_uses_dyn = css_dyn_seen;

    int n = 0; for (DOMNode* p = head; p; p = p->next) n++;
    Doc d; memset(&d,0,sizeof d); d.rs = &rs; d.n = n; d.nv = malloc((size_t)n * sizeof(DOMNode*)); d.prev = malloc((size_t)n * sizeof(int));
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
    if (g_hover && (g_hover->idx < 0 || g_hover->idx >= n || d.nv[g_hover->idx] != g_hover)) g_hover = NULL;
    if (g_active && (g_active->idx < 0 || g_active->idx >= n || d.nv[g_active->idx] != g_active)) g_active = NULL;
    anim_cascade_begin(head, n, rs.has_trans);

    float root_fs = 16;
    for (int i = 0; i < n;) {
        DOMNode* p = d.nv[i];
        cascade_node(&d, &ix, &rs, p, root_fs);
        if (i == 0) root_fs = p->st.font_size;
        if (p->type != TAG_TEXT && p->st.display == DSP_NONE) { i = p->subtree_end + 1; continue; }   /* поддерево скрыто */
        i++;
    }
    anim_cascade_end(d.nv, n, rs.kf, root_fs, kf_build, &d);
    vt_compute_styles(&d,&rs,root_fs);
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
