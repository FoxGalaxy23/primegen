#ifndef FX_H
#define FX_H

/* Эффекты (filter/backdrop-filter/clip-path/mask/transform) и движение (transition/animation).
   fx.c   — слои, фильтры, маски, отсечение, аффинное преобразование при отрисовке;
   anim.c — transition / @keyframes: интерполяция вычисленного стиля по времени. */
#include "parser.h"
#include "css_int.h"

/* ── анимируемые свойства (биты маски) ── */
enum {
    AP_OPACITY, AP_COLOR, AP_BG, AP_BC0, AP_BC1, AP_BC2, AP_BC3,
    AP_WIDTH, AP_HEIGHT, AP_MINW, AP_MAXW, AP_MINH, AP_MAXH,
    AP_MG0, AP_MG1, AP_MG2, AP_MG3, AP_PD0, AP_PD1, AP_PD2, AP_PD3, AP_IN0, AP_IN1, AP_IN2, AP_IN3,
    AP_BW0, AP_BW1, AP_BW2, AP_BW3, AP_RAD0, AP_RAD1, AP_RAD2, AP_RAD3,
    AP_FONTSZ, AP_SHADOW, AP_FLEXG, AP_GAPR, AP_GAPC, AP_LH,
    AP_FILTER, AP_BACKDROP, AP_CLIP, AP_XFORM, AP_ORIGIN, AP_BGPOS, AP_GRAD,
    AP_COUNT
};
#define APM(x) (1ULL << (x))
#define AP_ALL_MASK ((1ULL << AP_COUNT) - 1)
#define AP_LAYOUT_MASK (APM(AP_WIDTH) | APM(AP_HEIGHT) | APM(AP_MINW) | APM(AP_MAXW) | APM(AP_MINH) | APM(AP_MAXH) | \
    APM(AP_MG0) | APM(AP_MG1) | APM(AP_MG2) | APM(AP_MG3) | APM(AP_PD0) | APM(AP_PD1) | APM(AP_PD2) | APM(AP_PD3) | \
    APM(AP_IN0) | APM(AP_IN1) | APM(AP_IN2) | APM(AP_IN3) | APM(AP_BW0) | APM(AP_BW1) | APM(AP_BW2) | APM(AP_BW3) | \
    APM(AP_FONTSZ) | APM(AP_FLEXG) | APM(AP_GAPR) | APM(AP_GAPC) | APM(AP_LH))
#define AP_FX_MASK (APM(AP_FILTER) | APM(AP_BACKDROP) | APM(AP_CLIP) | APM(AP_XFORM) | APM(AP_ORIGIN))

uint64_t fx_prop_mask(const char* name, size_t n);          /* имя CSS-свойства → биты (all → все; none/неизвестное → 0) */
float    fx_ease(const FxEase* e, float t);                  /* временная функция: t∈[0,1] → прогресс */
int      fx_style_has_fx(const NodeStyle* st);               /* нужен ли слой (проверка по вычисленному стилю) */


/* ── fx.c: растровые операции над слоем (премультиплицированный RGBA8) ── */
typedef struct { int x0, y0, w, h; uint8_t* px; } FxImg;           /* px: w*h*4, порядок R,G,B,A */
FxImg*  fx_img_new(int x0, int y0, int w, int h);
void    fx_img_free(FxImg* im);
void    fx_img_from_passes(FxImg* im, const uint32_t* black, const uint32_t* white, int stride);   /* слой по двум проходам: на чёрном и на белом фоне */
void    fx_img_from_buffer(FxImg* im, const uint32_t* buf, int stride);                              /* непрозрачный снимок экрана */
void    fx_apply_filters(FxImg* im, const FxList* fl);
void    fx_apply_clip(FxImg* im, const FxClip* cp, float bx, float by, float bw, float bh);
void    fx_apply_mask(FxImg* im, const FxMask* mk, float bx, float by, float bw, float bh);
void    fx_composite(const FxImg* im, uint32_t* dst, int stride, int cx0, int cy0, int cx1, int cy1, const float* m);   /* m: affine слой→экран (NULL — перенос 0) */
void    fx_writeback(const FxImg* im, uint32_t* dst, int stride, int cx0, int cy0, int cx1, int cy1, float bx, float by, float bw, float bh, const float rad[4]);   /* backdrop: заменяет пиксели внутри бокса */
int     fx_filter_pad(const FxList* fl);                                                           /* запас вокруг слоя под blur/drop-shadow */
int     fx_xform_compute(const FxXform* x, float bx, float by, float bw, float bh, float m[6], float* tx, float* ty);   /* 1 — есть не-перенос (m — полная матрица), 0 — только перенос (tx,ty) */
void    fx_len_resolve(const FxLen* l, float base, float* out);

/* ── anim.c ── */
void     anim_cascade_begin(DOMNode* head, int n, int uses_trans);         /* перед каскадом: запоминает прежний (видимый) стиль узлов */
void     anim_before_node(DOMNode* n);                                      /* перед init_style узла */
typedef uint64_t (*KfBuildFn)(void* ctx, DOMNode* n, const Decl* decls, int nd, NodeStyle* out, float root_fs);
void     anim_cascade_end(DOMNode** nv, int n, Keyframes* kf, float root_fs, KfBuildFn build, void* ctx);   /* после каскада: запуск transition/animation */
int      anim_step(DOMNode* head);                                          /* перед раскладкой/отрисовкой: накладывает значения на текущее время; бит 1 — нужна раскладка */
int      anim_active(void);                                                 /* 1 — идёт анимация (нужны кадры) */
void     anim_reset(void);                                                  /* смена документа */
int      anim_pop_event(DOMNode** node, int* kind, char* name, float* elapsed);   /* очередь transitionend/animationend…: 1 — событие выдано */
double   anim_now_ms(void);

enum { AEV_TRANS_END = 1, AEV_ANIM_START, AEV_ANIM_ITER, AEV_ANIM_END, AEV_TRANS_RUN };

/* ── динамические псевдоклассы (:hover/:active/:focus) ── */
void     webpage_set_hover(DOMNode* n);                                     /* узел под курсором (NULL — нет) */
void     webpage_set_active(DOMNode* n);
int      webpage_dyn_pending(void);                                         /* 1 — состояние hover/active изменилось и страница использует такие правила: нужен пересчёт */
void     webpage_dyn_clear_pending(void);
int      webpage_dyn_match(int kind, const DOMNode* n);                     /* kind: PC_HOVER/PC_ACTIVE/PC_FOCUS/PC_FOCUS_WITHIN */
int      webpage_uses_dyn(void);

#endif
