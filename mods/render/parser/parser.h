#ifndef PARSER_H
#define PARSER_H

#include <stdint.h>
#include "nodestyle.h"

typedef enum { 
    TAG_NONE,
    TAG_H,
    TAG_P,
    TAG_A,
    TAG_IMG,
    TAG_DIV,
    TAG_BUTTON,
    TAG_INPUT,
    TAG_SPAN,
    TAG_HTML,
    TAG_HEAD,
    TAG_BODY,
    TAG_TITLE,
    TAG_META,
    TAG_LINK,
    TAG_STYLE,
    TAG_SCRIPT,
    TAG_MAIN,
    TAG_SECTION,
    TAG_ARTICLE,
    TAG_HEADER,
    TAG_FOOTER,
    TAG_NAV,
    TAG_ASIDE,
    TAG_UL,
    TAG_OL,
    TAG_LI,
    TAG_TABLE,
    TAG_TR,
    TAG_TD,
    TAG_TH,
    TAG_THEAD,
    TAG_TBODY,
    TAG_FORM,
    TAG_LABEL,
    TAG_SELECT,
    TAG_OPTION,
    TAG_TEXTAREA,
    TAG_FIELDSET,
    TAG_LEGEND,
    TAG_BR,
    TAG_HR,
    TAG_IFRAME,
    TAG_VIDEO,
    TAG_AUDIO,
    TAG_SOURCE,
    TAG_CANVAS,
    TAG_SVG,
    TAG_PICTURE,
    TAG_NOSCRIPT,
    TAG_TEXT,       /* текстовый узел (движок webpage) */
    TAG_OTHER       /* любой другой элемент (см. node->tag) */
} TagType;

typedef struct DOMNode {
    TagType type;
    char text[512];
    char href[512]; // Будем сохранять ссылки для кликов
    char src[512];  // Путь к изображению для <img>
    int hit_x, hit_y, hit_w, hit_h; // Координаты на экране для детекции клика
    
    // Style and interaction attributes
    char id[128];
    char class_name[256];
    char style_str[1024];
    char onclick[256];
    char value[2048]; // Для полей ввода
    char font_family[128];

    int has_style;
    uint32_t bg_color;
    uint32_t text_color;
    int width;
    int height;
    int left;
    int top;
    int is_absolute;
    int is_centered;
    int rows;   /* textarea rows attribute */
    int cols;   /* textarea cols attribute */

    struct DOMNode* next;

    /* ── Движок webpage (parse_document): дерево + вычисленный стиль ── */
    uint8_t v2;                     /* 1 = узел создан parse_document */
    uint8_t checked, disabled, readonly, selected, multiple;
    char    tag[16];                /* имя тега в нижнем регистре */
    char    input_type[16];
    char    placeholder[96];
    char    alt[128];
    char    name[64];
    char    attrs[384];             /* "имя\1значение\2..." — для селекторов [attr] */
    char    pres[256];              /* CSS из презентационных атрибутов (bgcolor, align ...) */
    int     idx, parent, subtree_end;   /* позиция в списке, родитель, последний потомок */
    int     attr_w, attr_h, colspan, rowspan, size_attr;
    void*   aux;                    /* движок JS: id узла + 1 (не сериализуется осмысленно) */
    NodeStyle st;
    PseudoBox* pb[2];               /* ::before / ::after (malloc, владелец — узел; см. dom_free_pseudo) */
} DOMNode;

DOMNode* parse_html(const char* html);
void free_dom(DOMNode* head);
void dom_free_pseudo(DOMNode* n);   /* освобождает сгенерированные ::before/::after узла */
DOMNode* clone_dom(DOMNode* head);
void parse_inline_style(DOMNode* node, const char* style_str);
void extract_html_title(const char* html, char* dest, int max_len);
DOMNode* parse_document(const char* html, const char* base_url); /* модуль webpage */
void extract_html_icon_href(const char* html, char* dest, int max_len);

void serialize_dom(DOMNode* head, const char* filename);
DOMNode* deserialize_dom(const char* filename);

#endif
