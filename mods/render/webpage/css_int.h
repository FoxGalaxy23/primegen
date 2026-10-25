#ifndef CSS_INT_H
#define CSS_INT_H

#include "webpage.h"

char* webpage_strcasestr(const char* h, const char* n);   /* переносимая замена strcasestr (нет в mingw) */
#undef strcasestr
#define strcasestr webpage_strcasestr

/* ── арена ── */
typedef struct ArenaChunk { struct ArenaChunk* next; size_t used, cap; char data[]; } ArenaChunk;
typedef struct { ArenaChunk* head; } Arena;
void* aalloc(Arena* a, size_t n);
char* astrdup(Arena* a, const char* s, size_t n);
void  afree(Arena* a);

/* ── свойства ── */
#define PROP_LIST(X) \
 X(ALIGN_ITEMS,"align-items") X(ALIGN_SELF,"align-self") X(BACKGROUND,"background") X(BG_COLOR,"background-color") \
 X(BG_IMAGE,"background-image") X(BG_POS,"background-position") X(BG_REPEAT,"background-repeat") X(BG_SIZE,"background-size") \
 X(BORDER,"border") X(BORDER_T,"border-top") X(BORDER_R,"border-right") X(BORDER_B,"border-bottom") X(BORDER_L,"border-left") \
 X(BORDER_COLOR,"border-color") X(BORDER_STYLE,"border-style") X(BORDER_WIDTH,"border-width") \
 X(BC_T,"border-top-color") X(BC_R,"border-right-color") X(BC_B,"border-bottom-color") X(BC_L,"border-left-color") \
 X(BS_T,"border-top-style") X(BS_R,"border-right-style") X(BS_B,"border-bottom-style") X(BS_L,"border-left-style") \
 X(BW_T,"border-top-width") X(BW_R,"border-right-width") X(BW_B,"border-bottom-width") X(BW_L,"border-left-width") \
 X(BORDER_RADIUS,"border-radius") X(BR_TL,"border-top-left-radius") X(BR_TR,"border-top-right-radius") \
 X(BR_BR,"border-bottom-right-radius") X(BR_BL,"border-bottom-left-radius") X(BORDER_COLLAPSE,"border-collapse") \
 X(BORDER_SPACING,"border-spacing") X(BOTTOM,"bottom") X(BOX_SHADOW,"box-shadow") X(BOX_SIZING,"box-sizing") \
 X(CLEAR,"clear") X(COLOR,"color") X(CURSOR,"cursor") X(DISPLAY,"display") X(FLEX,"flex") X(FLEX_BASIS,"flex-basis") \
 X(FLEX_DIR,"flex-direction") X(FLEX_FLOW,"flex-flow") X(FLEX_GROW,"flex-grow") X(FLEX_SHRINK,"flex-shrink") \
 X(FLEX_WRAP,"flex-wrap") X(FLOAT,"float") X(FONT,"font") X(FONT_FAMILY,"font-family") X(FONT_SIZE,"font-size") \
 X(FONT_STYLE,"font-style") X(FONT_WEIGHT,"font-weight") X(GAP,"gap") X(GRID_COLS,"grid-template-columns") X(GRID_TEMPLATE,"grid-template") X(GRID_AREAS,"grid-template-areas") X(GRID_AREA,"grid-area") X(GRID_COLUMN,"grid-column") X(ROW_GAP,"row-gap") X(COL_GAP,"column-gap") \
 X(HEIGHT,"height") X(INSET,"inset") X(JUSTIFY,"justify-content") X(LEFT,"left") X(LETTER_SP,"letter-spacing") \
 X(LINE_HEIGHT,"line-height") X(LIST_STYLE,"list-style") X(LIST_STYLE_TYPE,"list-style-type") X(MARGIN,"margin") \
 X(MARGIN_T,"margin-top") X(MARGIN_R,"margin-right") X(MARGIN_B,"margin-bottom") X(MARGIN_L,"margin-left") \
 X(MAX_H,"max-height") X(MAX_W,"max-width") X(MIN_H,"min-height") X(MIN_W,"min-width") X(OPACITY,"opacity") \
 X(OUTLINE,"outline") X(OVERFLOW,"overflow") X(OVERFLOW_X,"overflow-x") X(OVERFLOW_Y,"overflow-y") X(PADDING,"padding") \
 X(PADDING_T,"padding-top") X(PADDING_R,"padding-right") X(PADDING_B,"padding-bottom") X(PADDING_L,"padding-left") \
 X(POSITION,"position") X(RIGHT,"right") X(TEXT_ALIGN,"text-align") X(TEXT_DECO,"text-decoration") \
 X(TEXT_DECO_LINE,"text-decoration-line") X(TEXT_TRANSFORM,"text-transform") X(TOP,"top") \
 X(VERTICAL_ALIGN,"vertical-align") X(VISIBILITY,"visibility") X(WHITE_SPACE,"white-space") X(WIDTH,"width") \
 X(WORD_BREAK,"word-break") X(OVERFLOW_WRAP,"overflow-wrap") X(WORD_WRAP,"word-wrap") X(Z_INDEX,"z-index") \
 X(BG_CLIP,"background-clip") X(TEXT_FILL,"text-fill-color") X(TRANSFORM,"transform") X(CONTENT,"content") \
 X(FILTER,"filter") X(BACKDROP_FILTER,"backdrop-filter") X(CLIP_PATH,"clip-path") X(MASK,"mask") X(MASK_IMAGE,"mask-image") \
 X(MASK_SIZE,"mask-size") X(MASK_POS,"mask-position") X(MASK_REPEAT,"mask-repeat") X(TRANSFORM_ORIGIN,"transform-origin") \
 X(TRANSITION,"transition") X(TRANSITION_PROPERTY,"transition-property") X(TRANSITION_DURATION,"transition-duration") \
 X(TRANSITION_DELAY,"transition-delay") X(TRANSITION_TIMING,"transition-timing-function") \
 X(ANIMATION,"animation") X(ANIMATION_NAME,"animation-name") X(ANIMATION_DURATION,"animation-duration") X(ANIMATION_DELAY,"animation-delay") \
 X(ANIMATION_TIMING,"animation-timing-function") X(ANIMATION_ITER,"animation-iteration-count") X(ANIMATION_DIR,"animation-direction") \
 X(ANIMATION_FILL,"animation-fill-mode") X(ANIMATION_PLAY,"animation-play-state") \
 X(GRID_ROWS,"grid-template-rows") X(GRID_AUTO_ROWS,"grid-auto-rows") X(GRID_AUTO_COLS,"grid-auto-columns") X(GRID_AUTO_FLOW,"grid-auto-flow") \
 X(GRID_ROW,"grid-row") X(GRID_ROW_START,"grid-row-start") X(GRID_ROW_END,"grid-row-end") X(GRID_COL_START,"grid-column-start") X(GRID_COL_END,"grid-column-end") \
 X(JUSTIFY_ITEMS,"justify-items") X(JUSTIFY_SELF,"justify-self") X(ALIGN_CONTENT,"align-content") \
 X(PLACE_ITEMS,"place-items") X(PLACE_SELF,"place-self") X(PLACE_CONTENT,"place-content")

#define X(id, name) P_##id,
enum { P_NONE = 0, PROP_LIST(X) P_CUSTOM, P_COUNT };
#undef X
int css_prop_id(const char* name, size_t n);

/* ── правила ── */
enum { PC_FIRST_CHILD = 1, PC_LAST_CHILD, PC_ONLY_CHILD, PC_FIRST_TYPE, PC_LAST_TYPE, PC_ONLY_TYPE,
       PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_NTH_TYPE, PC_ROOT, PC_EMPTY, PC_LINK, PC_CHECKED, PC_DISABLED, PC_ENABLED, PC_HOVER, PC_ACTIVE, PC_FOCUS, PC_FOCUS_WITHIN = PC_FOCUS + 100, PC_NEVER };

typedef struct Simple {
    char tag[24];
    char id[48];
    char cls[4][40]; uint8_t ncls;
    struct { char n[24]; char v[48]; char op; } at[3]; uint8_t nat;
    uint8_t pc[6]; int16_t pa[6], pb[6]; uint8_t npc;
    struct Simple* sub; uint8_t sub_neg;          /* :not(...) / :is(...) — альтернативы через ->alt */
    struct Simple* alt;
    uint8_t pseudo;                               /* 0 — нет, 1 — ::before, 2 — ::after (только у последнего составного селектора) */
    char comb;                                    /* связь с левым соседом: ' ' '>' '+' '~' (0 у первого) */
} Simple;

typedef struct Decl { short prop; uint8_t imp; char* name; char* val; } Decl;   /* name — только для --custom */

typedef struct Rule {
    Simple* sel; int nsel;
    uint32_t spec; int order; int origin;         /* origin: 0 UA, 2 автор */
    uint8_t pseudo;                               /* 0 — обычное правило, 1/2 — правило для ::before/::after */
    Decl* decls; int ndecls;
} Rule;

typedef struct { float off; Decl* decls; int nd; } KfFrame;
typedef struct Keyframes { char name[48]; KfFrame* fr; int n; struct Keyframes* next; } Keyframes;
typedef struct { Rule** v; int n, cap; int next_order; Keyframes* kf; int has_trans; int has_dyn; } RuleSet;

Decl* css_parse_decls(Arena* a, const char* text, size_t len, int* out_n);
void  css_parse_sheet(Arena* a, RuleSet* rs, const char* css, const char* sheet_url, int origin, int depth);
int   css_parse_color(const char* s, uint32_t* argb, uint32_t cur_color);
int   css_parse_len(const char* s, Len* out, float em, float rem);
int   css_media_matches(const char* query);
extern int css_fetch_budget;
extern int css_dyn_seen;
Simple* css_parse_selector_ex(Arena* a, const char* s, size_t n, int* nsel);

#endif
