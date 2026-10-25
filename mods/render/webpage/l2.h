#ifndef L2_H
#define L2_H

#include "webpage.h"
#include "ttf_render.h"

/* Раскладка порождает список команд отрисовки (display list) в координатах документа.
   Отрисовка (paint.c) применяет прокрутку и отсечение и не пересчитывает раскладку. */
enum { C_BOX, C_RECT, C_TEXT, C_IMG, C_BGIMG, C_CTRL, C_CLIP, C_UNCLIP, C_HIT, C_MARK,
       C_STK, C_ENDSTK, C_FX, C_ENDFX };   /* C_FX/C_ENDFX — слой эффектов элемента (filter/clip-path/mask/transform/backdrop-filter); node = элемент, x,y,w,h = border-box */   /* C_STK/C_ENDSTK — границы position:sticky-блока: между ними смещение считается при отрисовке */
/* C_CLIP.flags: бит 1 — прокрутка по Y, бит 2 — по X (overflow:auto|scroll); ex = {scrollW, scrollH, maxX, maxY}, node = прокручиваемый элемент.
   C_STK.flags: бит 1 — задан top, бит 2 — задан bottom; x,y,w,h — border-box элемента;
   ex = {top, y_верха_контейнера, y_низа_контейнера, bottom, left, right, x_левого_края_контейнера, x_правого_края} в координатах документа (контейнер — content-box родителя). */
enum { CF_SCROLL_Y = 1, CF_SCROLL_X = 2, SF_TOP = 1, SF_BOTTOM = 2, SF_LEFT = 4, SF_RIGHT = 8 };
enum { F_NOLEFT = 1, F_NORIGHT = 2 };

typedef struct {
    uint8_t  t, fixed, flags, tstyle, deco, ital;
    uint16_t weight;                /* C_TEXT: насыщенность и курсивность для выбора начертания веб-шрифта */
    float    x, y, w, h;
    DOMNode* node;          /* C_BOX/C_CTRL/C_IMG/C_HIT: узел; C_TEXT: владелец (для ссылок) */
    const char* s;          /* C_TEXT: строка; C_IMG/C_BGIMG: URL */
    const char* fam;        /* C_TEXT: font-family */
    int      size;          /* C_TEXT: размер, px */
    uint32_t color;         /* C_TEXT/C_RECT */
    float    rad;           /* C_RECT: радиус; C_IMG: см. node->st.radius */
    float    opacity;
    float    ex[8];         /* доп. параметры для C_CLIP (прокрутка) и C_STK (sticky), см. выше */
    int      mate;          /* C_FX/C_ENDFX: индекс парной команды */
} Cmd;

extern Cmd*  l2_dl;
extern int   l2_dn;
extern uint32_t l2_canvas_bg;
extern float l2_doc_w, l2_doc_h;
extern float* l2_rects;      /* по idx узла: x,y,w,h border-box в координатах документа (из C_MARK) */
extern int   l2_nrects;

typedef struct { float x, y, mx, my, sw, sh, cw, ch; uint8_t is; } L2ScrollState;
void l2_layout(DOMNode* head, int vw, int vh);   /* строит l2_dl */
void l2_release(void);
int  l2_scroll_state_export(const DOMNode* head, L2ScrollState* out, int cap); /* out==NULL: вернуть требуемое число записей */
void l2_scroll_state_import(const L2ScrollState* in, int count, DOMNode* head);

/* ── прокрутка внутри элементов (overflow:auto|scroll) ──
   Состояние хранится по idx узла и переживает перерасчёт раскладки (пока не сменился документ). */
int  l2_scroll_get(const DOMNode* n, float* x, float* y);            /* текущее (обрезанное по максимуму) смещение; 1 — элемент прокручиваемый */
int  l2_scroll_max(const DOMNode* n, float* mx, float* my);          /* максимальное смещение по осям */
int  l2_scroll_size(const DOMNode* n, float* sw, float* sh, float* cw, float* ch);   /* scrollWidth/Height и clientWidth/Height */
void l2_scroll_set(const DOMNode* n, float x, float y);              /* абсолютное смещение (обрезается при применении) */
void l2_scroll_offset_of_ancestors(const DOMNode* n, float* ox, float* oy);   /* сумма смещений прокручиваемых предков (без самого узла) */

int  webpage_ctrl_default_size(DOMNode* n, float* w, float* h);   /* размер контента формы (для layout) */

#endif
