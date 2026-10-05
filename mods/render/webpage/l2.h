#ifndef L2_H
#define L2_H

#include "webpage.h"
#include "ttf_render.h"

/* Раскладка порождает список команд отрисовки (display list) в координатах документа.
   Отрисовка (paint.c) применяет прокрутку и отсечение и не пересчитывает раскладку. */
enum { C_BOX, C_RECT, C_TEXT, C_IMG, C_BGIMG, C_CTRL, C_CLIP, C_UNCLIP, C_HIT, C_MARK };
enum { F_NOLEFT = 1, F_NORIGHT = 2 };

typedef struct {
    uint8_t  t, fixed, flags, tstyle, deco;
    float    x, y, w, h;
    DOMNode* node;          /* C_BOX/C_CTRL/C_IMG/C_HIT: узел; C_TEXT: владелец (для ссылок) */
    const char* s;          /* C_TEXT: строка; C_IMG/C_BGIMG: URL */
    const char* fam;        /* C_TEXT: font-family */
    int      size;          /* C_TEXT: размер, px */
    uint32_t color;         /* C_TEXT/C_RECT */
    float    rad;           /* C_RECT: радиус; C_IMG: см. node->st.radius */
    float    opacity;
} Cmd;

extern Cmd*  l2_dl;
extern int   l2_dn;
extern uint32_t l2_canvas_bg;
extern float l2_doc_w, l2_doc_h;
extern float* l2_rects;      /* по idx узла: x,y,w,h border-box в координатах документа (из C_MARK) */
extern int   l2_nrects;

void l2_layout(DOMNode* head, int vw, int vh);   /* строит l2_dl */
void l2_release(void);

int  webpage_ctrl_default_size(DOMNode* n, float* w, float* h);   /* размер контента формы (для layout) */

#endif
