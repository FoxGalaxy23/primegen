#ifndef NODESTYLE_H
#define NODESTYLE_H
#include <stdint.h>

/* Значение длины: px + pct% + vw + vh (хватает для calc() вида «100% - 20px»). set=0 -> auto/none */
typedef struct { float px, pct, vw, vh, svw, svh, dvw, dvh, lh; uint8_t set; } Len;

enum { DSP_NONE, DSP_INLINE, DSP_BLOCK, DSP_INLINE_BLOCK, DSP_FLEX, DSP_INLINE_FLEX, DSP_LIST_ITEM,
       DSP_TABLE, DSP_ROW_GROUP, DSP_ROW, DSP_CELL, DSP_CAPTION, DSP_CONTENTS };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED };
enum { FLT_NONE, FLT_LEFT, FLT_RIGHT };
enum { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER, ALIGN_JUSTIFY, ALIGN_START, ALIGN_END };
enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum { VA_BASELINE, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUB, VA_SUPER };
enum { DECO_UNDERLINE = 1, DECO_LINE_THROUGH = 2, DECO_OVERLINE = 4 };
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_NONE };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAP };
enum { FD_ROW, FD_ROW_REV, FD_COL, FD_COL_REV };
enum { JC_START, JC_END, JC_CENTER, JC_BETWEEN, JC_AROUND, JC_EVENLY };
enum { AI_STRETCH, AI_START, AI_END, AI_CENTER, AI_BASELINE, AI_AUTO };
enum { BS_NONE, BS_SOLID, BS_DASHED, BS_DOTTED };


/* ── Эффекты: filter / backdrop-filter / clip-path / mask / transform, transition / animation (см. webpage/fx.c, anim.c) ── */
typedef struct { float px, pct; } FxLen;                         /* длина: px + % от размера бокса */
enum { FXO_BLUR = 1, FXO_BRIGHT, FXO_CONTRAST, FXO_GRAY, FXO_SEPIA, FXO_SAT, FXO_HUE, FXO_INVERT, FXO_OPACITY, FXO_SHADOW };
typedef struct { uint8_t kind; float v, x, y, blur; uint32_t color; } FxOp;   /* v: основное число (blur: px, hue: градусы, прочие: 0..n); shadow: x,y,blur,color */
typedef struct { uint8_t n; FxOp op[6]; } FxList;
enum { CP_NONE = 0, CP_INSET, CP_CIRCLE, CP_ELLIPSE, CP_POLYGON };
typedef struct {
    uint8_t kind, n, evenodd;                                    /* n — число вершин polygon */
    FxLen a[4];                                                  /* inset: top,right,bottom,left; circle: r,cx,cy; ellipse: rx,ry,cx,cy */
    float rad[4];                                                /* inset(... round r): радиусы, px */
    FxLen pt[16][2];                                             /* polygon */
} FxClip;
enum { XF_TRANSLATE = 1, XF_SCALE, XF_ROTATE, XF_SKEWX, XF_SKEWY, XF_MATRIX };
typedef struct { uint8_t kind; float v[6]; FxLen t[2]; } FxXfOp;  /* translate: t[0],t[1]; scale: v[0],v[1]; rotate/skew: v[0] (градусы); matrix: v[0..5] */
typedef struct { uint8_t n; FxXfOp op[6]; FxLen ox, oy; uint8_t has_origin; } FxXform;
enum { MK_NONE = 0, MK_LINEAR, MK_RADIAL, MK_IMAGE };
typedef struct {
    uint8_t kind, n, rep, size_mode;                             /* rep: 0 repeat, 1 x, 2 y, 3 no-repeat; size_mode: 0 auto, 1 cover, 2 contain, 3 явный */
    float angle; uint8_t a[6]; float p[6];                       /* остановки: альфа 0..255 и позиция 0..100 */
    Len w, h, px, py; char url[192];
} FxMask;
typedef struct { uint8_t kind; float a, b, c, d; } FxEase;      /* kind 0: cubic-bezier(a,b,c,d); 1: steps(a, b=0 end/1 start) */
typedef struct { uint64_t mask; float dur, delay; FxEase ease; } FxTrans;   /* mask: набор анимируемых свойств (0 — запись пуста) */
typedef struct { char name[40]; float dur, delay, iter; FxEase ease; uint8_t dir, fill, paused, composition; } FxAnim;   /* iter < 0 — infinite; composition 0 replace, 1 add, 2 accumulate */

typedef struct NodeStyle {
    uint8_t display, position, float_, clear, visibility, overflow_hidden, text_align, white_space;
    uint8_t deco, bold, italic, vertical_align, box_sizing, list_style, text_transform;
    uint8_t text_wrap, content_visibility; /* text-wrap: 0 wrap, 1 nowrap, 2 balance, 3 pretty, 4 stable */
    uint8_t text_orientation, text_combine_upright; /* vertical text orientation / short horizontal runs */
    uint8_t column_count, column_fill, column_span, column_rule_style;
    float column_rule_width; uint32_t column_rule_color;
    Len column_width, contain_intrinsic_w, contain_intrinsic_h;
    uint8_t grid_subgrid_cols, grid_subgrid_rows;
    uint8_t animation_timeline, animation_timeline_axis; /* 0 document time, 1 scroll(), 2 view() ; 0 block/y, 1 inline/x, 2 x, 3 y */
    float animation_range_start, animation_range_end; /* normalized range of timeline */
    char anchor_name[48], position_anchor[48], position_area[24], view_transition_name[48];
    uint16_t weight;                            /* font-weight 100..900 (0 — не задан: bold ? 700 : 400); bold = weight >= 600 */
    uint8_t flex_dir, flex_wrap, justify, align_items, align_self, break_word, pointer, no_ring, center_blocks;
    uint8_t pointer_events, user_select, text_overflow, object_fit, writing_mode, direction, unicode_bidi, bidi_context;
    uint8_t blend_mode, scroll_behavior, scroll_snap_type, scroll_snap_align;
    uint8_t container_type, container_name_set, counter_dummy;
    uint8_t border_style[4];
    uint8_t has_z; int16_t z_index;
    uint8_t sticky, sc_x, sc_y;                 /* position:sticky; overflow-x/y: auto|scroll (прокручиваемый контейнер) */
    float font_size, lh_px, lh_mult;
    float aspect_ratio, zoom, text_indent, word_spacing, outline_offset;
    Len text_indent_len, word_spacing_len;
    float object_pos_x, object_pos_y;
    int16_t order; uint8_t line_clamp;
    float flex_grow, flex_shrink; Len flex_basis;
    Len width, height, min_w, max_w, min_h, max_h;
    Len margin[4], padding[4], inset[4];        /* порядок: top, right, bottom, left */
    float border_w[4]; uint32_t border_c[4];
    float radius[4];                            /* tl, tr, br, bl */
    uint32_t color, bg;                         /* 0xAARRGGBB; alpha 0 = прозрачный */
    float opacity, gap_r, gap_c, spacing;
    float sh_x, sh_y, sh_blur, sh_spread; uint32_t sh_color;
    float ts_x, ts_y, ts_blur; uint32_t ts_color;
    float outline_w; uint32_t outline_c; uint8_t outline_style, outline_set;
    char container_name[40];
    char counter_reset[96], counter_increment[96], counter_set[96];
    char  bg_url[192];
    char  grid_cols[160]; uint8_t is_grid, grid_span;
    char  grid_areas[220];              /* сырое значение grid-template-areas, строки разделены '\n' */
    char  grid_area_name[32];           /* значение grid-area на самом элементе (имя области) */
    uint8_t bg_repeat, bg_size_mode;            /* repeat: 0 both,1 x,2 y,3 none; size: 0 auto,1 cover,2 contain,3 explicit */
    Len bg_w, bg_h, bg_px, bg_py;
    /* градиент фона: 0 нет, 1 linear-gradient, 2 radial-gradient, 3 conic-gradient */
    uint8_t bg_grad_type, bg_grad_n, bg_clip_text;
    float bg_grad_angle;                        /* градусы, для linear */
    uint32_t bg_grad_c[4];                       /* цвета остановок, 0xAARRGGBB */
    float bg_grad_p[4];                          /* позиции остановок, 0..100 */
    Len tr_x, tr_y;                              /* (устарело: теперь transform разбирается в xf) */
    /* эффекты и движение */
    FxList  filter, backdrop;
    FxClip  clip;
    FxMask  mask;
    FxXform xf;
    float   op_own;                              /* собственная opacity до умножения на родительскую (нужна анимации) */
    FxTrans tr[4]; uint8_t tr_n;                 /* transition: записи (свойства + длительность/задержка/функция) */
    float   tp_dur[4], tp_del[4]; FxEase tp_ease[4]; uint8_t tp_dur_n, tp_del_n, tp_ease_n;   /* сырые списки longhand-ов */
    uint64_t tp_prop[4]; uint8_t tp_prop_n;
    FxAnim  an[3]; uint8_t an_n;                 /* animation: до трёх одновременных */
    /* grid */
    char    grid_rows[160];                      /* grid-template-rows */
    char    grid_auto_r[48], grid_auto_c[48];    /* grid-auto-rows / grid-auto-columns */
    char    grid_row_s[24], grid_row_e[24], grid_col_s[24], grid_col_e[24];   /* grid-row/column-start/end у элемента (число, span N, имя) */
    uint8_t grid_flow;                           /* 0 row, 1 column; бит 2 — dense */
    uint8_t justify_items, justify_self, align_content;   /* AI_* (justify_*), JC_* (align_content) */
    uint8_t has_fx;                              /* есть filter/backdrop/clip-path/mask/transform → слой */
} NodeStyle;


/* Сгенерированный контент ::before / ::after (см. style.c: cascade_pseudo, layout2.c: build_pseudo) */
typedef struct PseudoBox {
    NodeStyle st;
    char font_family[128];
    char content[248];          /* уже разобранное значение content (без кавычек, escape-последовательности раскрыты) */
    int8_t content_state;       /* -1: none, 0: normal/не задано, 1: задано автором */
} PseudoBox;

#endif
