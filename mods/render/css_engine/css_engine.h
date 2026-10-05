#ifndef CSS_ENGINE_H
#define CSS_ENGINE_H

#include "parser.h"
#include <stdint.h>

/* ── Единицы измерения ─────────────────────────────────────────── */
typedef enum {
    UNIT_PX = 0,
    UNIT_PERCENT,
    UNIT_EM,
    UNIT_REM,
    UNIT_AUTO,
    UNIT_NONE
} CSSUnit;

typedef struct {
    float    value;
    CSSUnit  unit;
} CSSLength;

/* ── display ───────────────────────────────────────────────────── */
typedef enum {
    DISPLAY_INLINE = 0,
    DISPLAY_BLOCK,
    DISPLAY_INLINE_BLOCK,
    DISPLAY_FLEX,
    DISPLAY_NONE
} CSSDisplay;

/* ── Специфичность (weight) ─────────────────────────────────────── */
typedef struct {
    int ids;        /* #id          → 100 */
    int classes;    /* .cls, [attr] →  10 */
    int tags;       /* div, p       →   1 */
} CSSSpecificity;

/* ── Одно объявление (prop: value) ─────────────────────────────── */
typedef struct CSSDeclaration {
    char* property;
    char* value;
    struct CSSDeclaration* next;
} CSSDeclaration;

/* ── Одно CSS-правило (selector { ... }) ───────────────────────── */
typedef struct CSSRule {
    char*           selector;
    CSSSpecificity  specificity;
    CSSDeclaration* declarations;
    struct CSSRule* next;
} CSSRule;

/* ── Весь разобранный stylesheet ───────────────────────────────── */
typedef struct {
    CSSRule* rules;
    int      rule_count;
} CSSStylesheet;

/* ── Вычисленные стили одного узла ─────────────────────────────── */
typedef struct {
    /* Цвета (0xRRGGBB, -1 = не задан) */
    int32_t  bg_color;
    int32_t  text_color;
    int32_t  border_color;

    /* Блочная модель */
    CSSLength width, height;
    CSSLength margin_top, margin_right, margin_bottom, margin_left;
    CSSLength padding_top, padding_right, padding_bottom, padding_left;

    /* Шрифт */
    CSSLength font_size;
    int       font_weight;   /* 100-900 */
    int       font_italic;   /* bool    */

    /* Border */
    CSSLength border_width;
    char      border_style[16]; /* solid, dashed, dotted, none */

    /* Прочее */
    CSSDisplay display;
    float      opacity;      /* 0.0-1.0, <0 = не задано */
    int        visibility;   /* 1=visible, 0=hidden, -1=не задано */
    char       text_align[16];
    CSSLength  line_height;
    char       font_family[64];
} CSSComputedStyle;

/* ════════════════════════════════════════════════════════════════
   Публичный API
   ════════════════════════════════════════════════════════════════ */

/**
 * Разбирает CSS-строку и возвращает stylesheet.
 * Вызывающая сторона должна освободить его через css_stylesheet_free().
 */
CSSStylesheet* css_parse(const char* css_data);
CSSStylesheet* css_parse_ex(const char* css_data, const char* page_url);

/**
 * Освобождает память, выделенную css_parse().
 */
void css_stylesheet_free(CSSStylesheet* sheet);

/**
 * Вычисляет финальные стили узла с учётом каскада и специфичности.
 * Заполняет out. Возвращает 0 при успехе.
 */
int css_compute_style(const CSSStylesheet* sheet,
                      const DOMNode*       node,
                      CSSComputedStyle*    out);

/**
 * Применяет вычисленные стили напрямую к полям DOMNode
 * (обратная совместимость со старым API).
 */
void css_apply_stylesheet(DOMNode* head, const char* css_data);

/**
 * Загружает CSS-файл и применяет его к дереву.
 */
void css_load_and_apply(DOMNode* head, const char* filename);
void css_load_and_apply_ex(DOMNode* head, const char* filename, const char* page_url);

/**
 * Инициализирует CSSComputedStyle значениями по умолчанию.
 */
void css_computed_style_init(CSSComputedStyle* s);

#endif /* CSS_ENGINE_H */
