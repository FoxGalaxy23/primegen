#ifndef NODESTYLE_H
#define NODESTYLE_H
#include <stdint.h>

/* Значение длины: px + pct% + vw + vh (хватает для calc() вида «100% - 20px»). set=0 -> auto/none */
typedef struct { float px, pct, vw, vh; uint8_t set; } Len;

enum { DSP_NONE, DSP_INLINE, DSP_BLOCK, DSP_INLINE_BLOCK, DSP_FLEX, DSP_INLINE_FLEX, DSP_LIST_ITEM,
       DSP_TABLE, DSP_ROW_GROUP, DSP_ROW, DSP_CELL, DSP_CAPTION, DSP_CONTENTS };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED };
enum { FLT_NONE, FLT_LEFT, FLT_RIGHT };
enum { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER, ALIGN_JUSTIFY };
enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum { VA_BASELINE, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUB, VA_SUPER };
enum { DECO_UNDERLINE = 1, DECO_LINE_THROUGH = 2, DECO_OVERLINE = 4 };
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_NONE };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAP };
enum { FD_ROW, FD_ROW_REV, FD_COL, FD_COL_REV };
enum { JC_START, JC_END, JC_CENTER, JC_BETWEEN, JC_AROUND, JC_EVENLY };
enum { AI_STRETCH, AI_START, AI_END, AI_CENTER, AI_BASELINE, AI_AUTO };
enum { BS_NONE, BS_SOLID, BS_DASHED, BS_DOTTED };

typedef struct NodeStyle {
    uint8_t display, position, float_, clear, visibility, overflow_hidden, text_align, white_space;
    uint8_t deco, bold, italic, vertical_align, box_sizing, list_style, text_transform;
    uint8_t flex_dir, flex_wrap, justify, align_items, align_self, break_word, pointer, no_ring, center_blocks;
    uint8_t border_style[4];
    uint8_t has_z; int16_t z_index;
    float font_size, lh_px, lh_mult;
    float flex_grow, flex_shrink; Len flex_basis;
    Len width, height, min_w, max_w, min_h, max_h;
    Len margin[4], padding[4], inset[4];        /* порядок: top, right, bottom, left */
    float border_w[4]; uint32_t border_c[4];
    float radius[4];                            /* tl, tr, br, bl */
    uint32_t color, bg;                         /* 0xAARRGGBB; alpha 0 = прозрачный */
    float opacity, gap_r, gap_c, spacing;
    float sh_x, sh_y, sh_blur, sh_spread; uint32_t sh_color;
    char  bg_url[192];
    char  grid_cols[160]; uint8_t is_grid, grid_span;
    char  grid_areas[220];              /* сырое значение grid-template-areas, строки разделены '\n' */
    char  grid_area_name[32];           /* значение grid-area на самом элементе (имя области) */
    uint8_t bg_repeat, bg_size_mode;            /* repeat: 0 both,1 x,2 y,3 none; size: 0 auto,1 cover,2 contain,3 explicit */
    Len bg_w, bg_h, bg_px, bg_py;
    /* градиент фона: 0 нет, 1 linear-gradient, 2 radial-gradient */
    uint8_t bg_grad_type, bg_grad_n, bg_clip_text;
    float bg_grad_angle;                        /* градусы, для linear */
    uint32_t bg_grad_c[4];                       /* цвета остановок, 0xAARRGGBB */
    float bg_grad_p[4];                          /* позиции остановок, 0..100 */
    Len tr_x, tr_y;                              /* transform: translate()/translateX()/translateY() */
} NodeStyle;


/* Сгенерированный контент ::before / ::after (см. style.c: cascade_pseudo, layout2.c: build_pseudo) */
typedef struct PseudoBox {
    NodeStyle st;
    char font_family[128];
    char content[248];          /* уже разобранное значение content (без кавычек, escape-последовательности раскрыты) */
} PseudoBox;

#endif
