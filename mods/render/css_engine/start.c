#include "css_engine.h"
#include "font_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ════════════════════════════════════════════════════════════════
   Вспомогательные утилиты
   ════════════════════════════════════════════════════════════════ */

static char* css_strdup(const char* s) {
    if (!s) return NULL;
    char* d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

static char* trim(char* str) {
    if (!str) return str;
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/* Удаляет комментарии CSS вида / * ... * / из копии строки. */
static char* strip_comments(const char* src) {
    size_t len = strlen(src);
    char*  out = malloc(len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        if (src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i < len - 1 && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i += 2;
        } else {
            out[j++] = src[i++];
        }
    }
    out[j] = '\0';
    return out;
}

/* ════════════════════════════════════════════════════════════════
   Разбор цветов
   ════════════════════════════════════════════════════════════════ */

typedef struct { const char* name; uint32_t rgb; } NamedColor;

static const NamedColor NAMED_COLORS[] = {
    {"black",   0x000000}, {"white",   0xFFFFFF}, {"red",     0xFF0000},
    {"green",   0x008000}, {"blue",    0x0000FF}, {"yellow",  0xFFFF00},
    {"cyan",    0x00FFFF}, {"magenta", 0xFF00FF}, {"orange",  0xFFA500},
    {"purple",  0x800080}, {"pink",    0xFFC0CB}, {"gray",    0x808080},
    {"grey",    0x808080}, {"silver",  0xC0C0C0}, {"navy",    0x000080},
    {"teal",    0x008080}, {"lime",    0x00FF00}, {"maroon",  0x800000},
    {"olive",   0x808000}, {"brown",   0xA52A2A}, {"coral",   0xFF7F50},
    {"salmon",  0xFA8072}, {"gold",    0xFFD700}, {"indigo",  0x4B0082},
    {"violet",  0xEE82EE}, {"wheat",   0xF5DEB3}, {"tan",     0xD2B48C},
    {"transparent", 0x000000}, /* special case — handled separately */
    {NULL, 0}
};

/**
 * Разбирает строку цвета и возвращает 0xRRGGBB.
 * Возвращает -1 если не удалось разобрать.
 */
static int32_t parse_color(const char* val) {
    if (!val || *val == '\0') return -1;

    /* #RGB или #RRGGBB */
    if (val[0] == '#') {
        const char* hex = val + 1;
        size_t hlen = strlen(hex);
        if (hlen == 6) {
            return (int32_t)strtoul(hex, NULL, 16);
        } else if (hlen == 3) {
            char exp[7] = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2], '\0'};
            return (int32_t)strtoul(exp, NULL, 16);
        }
        return -1;
    }

    /* rgb(r,g,b) */
    if (strncmp(val, "rgb(", 4) == 0) {
        int r, g, b;
        if (sscanf(val + 4, "%d,%d,%d", &r, &g, &b) == 3) {
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            return (int32_t)((r << 16) | (g << 8) | b);
        }
        return -1;
    }

    /* rgba(r,g,b,a) — альфа игнорируем (нет канала в DOMNode) */
    if (strncmp(val, "rgba(", 5) == 0) {
        int r, g, b; float a;
        if (sscanf(val + 5, "%d,%d,%d,%f", &r, &g, &b, &a) >= 3) {
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            return (int32_t)((r << 16) | (g << 8) | b);
        }
        return -1;
    }

    /* Именованные цвета */
    for (int i = 0; NAMED_COLORS[i].name; i++) {
        if (strcasecmp(val, NAMED_COLORS[i].name) == 0) {
            return (int32_t)NAMED_COLORS[i].rgb;
        }
    }

    return -1;
}

/* ════════════════════════════════════════════════════════════════
   Разбор единиц длины
   ════════════════════════════════════════════════════════════════ */

static CSSLength parse_length(const char* val) {
    CSSLength l = {0.0f, UNIT_PX};
    if (!val || *val == '\0') { l.unit = UNIT_NONE; return l; }

    if (strcasecmp(val, "auto") == 0) { l.unit = UNIT_AUTO; return l; }

    char* endptr;
    l.value = strtof(val, &endptr);

    if (strncmp(endptr, "px", 2) == 0 || *endptr == '\0') {
        l.unit = UNIT_PX;
    } else if (*endptr == '%') {
        l.unit = UNIT_PERCENT;
    } else if (strncmp(endptr, "em", 2) == 0) {
        l.unit = UNIT_EM;
    } else if (strncmp(endptr, "rem", 3) == 0) {
        l.unit = UNIT_REM;
    } else {
        l.unit = UNIT_PX; /* fallback */
    }
    return l;
}

/* Shorthand «margin: top right bottom left» и его варианты */
static void expand_box_shorthand(const char* val,
                                 CSSLength* top, CSSLength* right,
                                 CSSLength* bottom, CSSLength* left) {
    char buf[256];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* parts[4];
    int   cnt = 0;
    char* tok = strtok(buf, " \t");
    while (tok && cnt < 4) { parts[cnt++] = tok; tok = strtok(NULL, " \t"); }

    if (cnt == 1) {
        *top = *right = *bottom = *left = parse_length(parts[0]);
    } else if (cnt == 2) {
        *top = *bottom = parse_length(parts[0]);
        *right = *left = parse_length(parts[1]);
    } else if (cnt == 3) {
        *top    = parse_length(parts[0]);
        *right  = *left = parse_length(parts[1]);
        *bottom = parse_length(parts[2]);
    } else if (cnt == 4) {
        *top    = parse_length(parts[0]);
        *right  = parse_length(parts[1]);
        *bottom = parse_length(parts[2]);
        *left   = parse_length(parts[3]);
    }
}

/* ════════════════════════════════════════════════════════════════
   Специфичность
   ════════════════════════════════════════════════════════════════ */

static CSSSpecificity calc_specificity(const char* selector) {
    CSSSpecificity sp = {0, 0, 0};
    if (!selector) return sp;

    const char* p = selector;
    while (*p) {
        if (*p == '#') { sp.ids++; p++; }
        else if (*p == '.' || *p == '[') { sp.classes++; p++; }
        else if (isalpha((unsigned char)*p)) { sp.tags++; }
        p++;
    }
    return sp;
}

static int specificity_cmp(CSSSpecificity a, CSSSpecificity b) {
    if (a.ids    != b.ids)    return a.ids    - b.ids;
    if (a.classes != b.classes) return a.classes - b.classes;
    return a.tags - b.tags;
}

/* ════════════════════════════════════════════════════════════════
   Сопоставление селекторов
   ════════════════════════════════════════════════════════════════ */

static const char* TAG_NAMES[] = {
    "", "h1", "p", "a", "img", "div", "button", "input", "span",
    "h2", "h3", "ul", "li", "nav", "header", "footer", "section",
    "article", "main", "form", "label", "select", "textarea", NULL
};

static int matches_selector(const DOMNode* node, const char* selector) {
    if (!node || !selector || *selector == '\0') return 0;

    /* *, universal */
    if (strcmp(selector, "*") == 0) return 1;

    /* #id */
    if (selector[0] == '#') {
        return node->id && strcmp(node->id, selector + 1) == 0;
    }

    /* .class — поддержка нескольких классов через пробел в class_name */
    if (selector[0] == '.') {
        if (!node->class_name) return 0;
        const char* cls = selector + 1;
        /* node->class_name может быть "btn primary active" */
        char tmp[256];
        strncpy(tmp, node->class_name, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* tok = strtok(tmp, " \t");
        while (tok) {
            if (strcmp(tok, cls) == 0) return 1;
            tok = strtok(NULL, " \t");
        }
        return 0;
    }

    /* Тег */
    if (node->type > 0) {
        for (int i = 1; TAG_NAMES[i]; i++) {
            if (node->type == i)
                return strcasecmp(TAG_NAMES[i], selector) == 0;
        }
    }

    return 0;
}

/* ════════════════════════════════════════════════════════════════
   Разбор CSS
   ════════════════════════════════════════════════════════════════ */

static CSSDeclaration* parse_declarations(const char* block) {
    if (!block) return NULL;

    CSSDeclaration* head = NULL;
    CSSDeclaration* tail = NULL;

    char* buf = css_strdup(block);
    char* save;
    char* rule = strtok_r(buf, ";", &save);

    while (rule) {
        char* colon = strchr(rule, ':');
        if (colon) {
            *colon = '\0';
            char* prop = trim(rule);
            char* val  = trim(colon + 1);

            if (*prop && *val) {
                CSSDeclaration* decl = calloc(1, sizeof(CSSDeclaration));
                decl->property = css_strdup(prop);
                decl->value    = css_strdup(val);

                if (!head) head = tail = decl;
                else { tail->next = decl; tail = decl; }
            }
        }
        rule = strtok_r(NULL, ";", &save);
    }

    free(buf);
    return head;
}

CSSStylesheet* css_parse(const char* css_data) {
    if (!css_data) return NULL;
    return css_parse_ex(css_data, NULL);
}

CSSStylesheet* css_parse_ex(const char* css_data, const char* page_url) {
    if (!css_data) return NULL;

    CSSStylesheet* sheet = calloc(1, sizeof(CSSStylesheet));
    if (!sheet) return NULL;

    char* data = strip_comments(css_data);
    if (!data) { free(sheet); return NULL; }

    char* p = data;

    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '@') {
            if (strncmp(p, "@font-face", 10) == 0) {
                char* brace = strchr(p, '{');
                if (brace) {
                    char* end_brace = strchr(brace, '}');
                    if (end_brace) {
                        *end_brace = '\0';
                        CSSDeclaration* decls = parse_declarations(brace + 1);
                        char family[64] = {0};
                        const char* src_value = NULL;
                        const char* weight_value = NULL;
                        const char* style_value = NULL;
                        CSSDeclaration* d = decls;
                        while (d) {
                            if (strcmp(d->property, "font-family") == 0) {
                                strncpy(family, d->value, 63);
                            } else if (strcmp(d->property, "src") == 0) {
                                src_value = d->value;            /* весь список url(...) format(...) — выбор делает font_manager */
                            } else if (strcmp(d->property, "font-weight") == 0) {
                                weight_value = d->value;
                            } else if (strcmp(d->property, "font-style") == 0) {
                                style_value = d->value;
                            }
                            d = d->next;
                        }
                        if (family[0] && src_value) {
                            font_manager_add_face(family, src_value, weight_value, style_value, page_url);
                        }
                        // Free decls
                        while (decls) {
                            CSSDeclaration* next = decls->next;
                            free(decls->property); free(decls->value); free(decls);
                            decls = next;
                        }
                        *end_brace = '}';
                        p = end_brace + 1;
                        continue;
                    }
                }
            }
            char* brace = strchr(p, '{');
            if (!brace) break;
            /* Считаем вложенные фигурные скобки */
            int depth = 1;
            p = brace + 1;
            while (*p && depth > 0) {
                if (*p == '{') depth++;
                else if (*p == '}') depth--;
                p++;
            }
            continue;
        }

        char* sel_end = strchr(p, '{');
        if (!sel_end) break;

        *sel_end = '\0';
        char* selector = trim(p);

        char* blk_end = strchr(sel_end + 1, '}');
        if (!blk_end) break;

        *blk_end = '\0';
        char* block = sel_end + 1;

        /* Selector может быть списком через запятую: "h1, h2, .title" */
        char sel_buf[512];
        strncpy(sel_buf, selector, sizeof(sel_buf) - 1);
        sel_buf[sizeof(sel_buf) - 1] = '\0';

        char* sel_save;
        char* single = strtok_r(sel_buf, ",", &sel_save);
        while (single) {
            single = trim(single);
            if (*single) {
                CSSRule* rule = calloc(1, sizeof(CSSRule));
                rule->selector    = css_strdup(single);
                rule->specificity = calc_specificity(single);
                rule->declarations = parse_declarations(block);

                /* Добавляем в конец (порядок объявления — часть каскада) */
                rule->next = NULL;
                if (!sheet->rules) {
                    sheet->rules = rule;
                } else {
                    CSSRule* cur = sheet->rules;
                    while (cur->next) cur = cur->next;
                    cur->next = rule;
                }
                sheet->rule_count++;
            }
            single = strtok_r(NULL, ",", &sel_save);
        }

        p = blk_end + 1;
    }

    free(data);
    return sheet;
}

void css_stylesheet_free(CSSStylesheet* sheet) {
    if (!sheet) return;
    CSSRule* rule = sheet->rules;
    while (rule) {
        CSSRule* nr = rule->next;
        free(rule->selector);
        CSSDeclaration* decl = rule->declarations;
        while (decl) {
            CSSDeclaration* nd = decl->next;
            free(decl->property);
            free(decl->value);
            free(decl);
            decl = nd;
        }
        free(rule);
        rule = nr;
    }
    free(sheet);
}

/* ════════════════════════════════════════════════════════════════
   Инициализация вычисленных стилей (значения по умолчанию браузера)
   ════════════════════════════════════════════════════════════════ */

void css_computed_style_init(CSSComputedStyle* s) {
    memset(s, 0, sizeof(*s));
    s->bg_color     = -1;
    s->text_color   = -1;
    s->border_color = -1;

    s->width.unit   = UNIT_AUTO;
    s->height.unit  = UNIT_AUTO;

    CSSLength zero_px = {0.0f, UNIT_PX};
    s->margin_top = s->margin_right = s->margin_bottom = s->margin_left = zero_px;
    s->padding_top = s->padding_right = s->padding_bottom = s->padding_left = zero_px;

    s->font_size.value = 16.0f; s->font_size.unit = UNIT_PX;
    s->font_weight  = 400;
    s->font_italic  = 0;

    s->border_width.value = 0; s->border_width.unit = UNIT_PX;
    strcpy(s->border_style, "none");

    s->display    = DISPLAY_INLINE;
    s->opacity    = -1.0f;
    s->visibility = -1;
    strcpy(s->text_align, "left");
    s->font_family[0] = '\0';

    s->line_height.value = 1.2f; s->line_height.unit = UNIT_NONE;
}

/* ════════════════════════════════════════════════════════════════
   Применение одного объявления к вычисленному стилю
   ════════════════════════════════════════════════════════════════ */

static void apply_declaration(CSSComputedStyle* s,
                              const char* prop, const char* val) {
    /* ── Цвета ── */
    if (strcmp(prop, "background-color") == 0 ||
        strcmp(prop, "background") == 0) {
        int32_t c = parse_color(val);
        if (c >= 0) s->bg_color = c;
        return;
    }
    if (strcmp(prop, "color") == 0) {
        int32_t c = parse_color(val);
        if (c >= 0) s->text_color = c;
        return;
    }
    if (strcmp(prop, "border-color") == 0) {
        int32_t c = parse_color(val);
        if (c >= 0) s->border_color = c;
        return;
    }

    /* ── Размеры ── */
    if (strcmp(prop, "width") == 0)  { s->width  = parse_length(val); return; }
    if (strcmp(prop, "height") == 0) { s->height = parse_length(val); return; }

    /* ── Margin ── */
    if (strcmp(prop, "margin") == 0) {
        expand_box_shorthand(val,
            &s->margin_top, &s->margin_right,
            &s->margin_bottom, &s->margin_left);
        return;
    }
    if (strcmp(prop, "margin-top") == 0)    { s->margin_top    = parse_length(val); return; }
    if (strcmp(prop, "margin-right") == 0)  { s->margin_right  = parse_length(val); return; }
    if (strcmp(prop, "margin-bottom") == 0) { s->margin_bottom = parse_length(val); return; }
    if (strcmp(prop, "margin-left") == 0)   { s->margin_left   = parse_length(val); return; }

    /* ── Padding ── */
    if (strcmp(prop, "padding") == 0) {
        expand_box_shorthand(val,
            &s->padding_top, &s->padding_right,
            &s->padding_bottom, &s->padding_left);
        return;
    }
    if (strcmp(prop, "padding-top") == 0)    { s->padding_top    = parse_length(val); return; }
    if (strcmp(prop, "padding-right") == 0)  { s->padding_right  = parse_length(val); return; }
    if (strcmp(prop, "padding-bottom") == 0) { s->padding_bottom = parse_length(val); return; }
    if (strcmp(prop, "padding-left") == 0)   { s->padding_left   = parse_length(val); return; }

    /* ── Шрифт ── */
    if (strcmp(prop, "font-size") == 0) {
        /* Ключевые слова */
        if (strcasecmp(val, "small") == 0)        { s->font_size.value = 13.0f; s->font_size.unit = UNIT_PX; }
        else if (strcasecmp(val, "medium") == 0)  { s->font_size.value = 16.0f; s->font_size.unit = UNIT_PX; }
        else if (strcasecmp(val, "large") == 0)   { s->font_size.value = 18.0f; s->font_size.unit = UNIT_PX; }
        else if (strcasecmp(val, "x-large") == 0) { s->font_size.value = 24.0f; s->font_size.unit = UNIT_PX; }
        else                                       { s->font_size = parse_length(val); }
        return;
    }
    if (strcmp(prop, "font-weight") == 0) {
        if (strcasecmp(val, "bold") == 0)        s->font_weight = 700;
        else if (strcasecmp(val, "bolder") == 0) s->font_weight = 900;
        else if (strcasecmp(val, "lighter") == 0)s->font_weight = 300;
        else if (strcasecmp(val, "normal") == 0) s->font_weight = 400;
        else s->font_weight = atoi(val);
        return;
    }
    if (strcmp(prop, "font-style") == 0) {
        s->font_italic = (strcasecmp(val, "italic") == 0 ||
                          strcasecmp(val, "oblique") == 0) ? 1 : 0;
        return;
    }
    if (strcmp(prop, "font-family") == 0) {
        strncpy(s->font_family, val, sizeof(s->font_family) - 1);
        s->font_family[sizeof(s->font_family) - 1] = '\0';
        return;
    }

    /* ── Border ── */
    if (strcmp(prop, "border-width") == 0) { s->border_width = parse_length(val); return; }
    if (strcmp(prop, "border-style") == 0) {
        strncpy(s->border_style, val, sizeof(s->border_style) - 1);
        return;
    }
    /* border: width style color */
    if (strcmp(prop, "border") == 0) {
        char tmp[256];
        strncpy(tmp, val, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* tok = strtok(tmp, " \t");
        while (tok) {
            /* Если начинается с цифры — ширина */
            if (isdigit((unsigned char)*tok) || *tok == '.') {
                s->border_width = parse_length(tok);
            } else {
                int32_t c = parse_color(tok);
                if (c >= 0) s->border_color = c;
                else strncpy(s->border_style, tok, sizeof(s->border_style) - 1);
            }
            tok = strtok(NULL, " \t");
        }
        return;
    }

    /* ── Display ── */
    if (strcmp(prop, "display") == 0) {
        if (strcasecmp(val, "block") == 0)        s->display = DISPLAY_BLOCK;
        else if (strcasecmp(val, "inline-block") == 0) s->display = DISPLAY_INLINE_BLOCK;
        else if (strcasecmp(val, "flex") == 0)    s->display = DISPLAY_FLEX;
        else if (strcasecmp(val, "none") == 0)    s->display = DISPLAY_NONE;
        else                                       s->display = DISPLAY_INLINE;
        return;
    }

    /* ── Прочее ── */
    if (strcmp(prop, "opacity") == 0) {
        s->opacity = strtof(val, NULL);
        if (s->opacity < 0.0f) s->opacity = 0.0f;
        if (s->opacity > 1.0f) s->opacity = 1.0f;
        return;
    }
    if (strcmp(prop, "visibility") == 0) {
        s->visibility = (strcasecmp(val, "visible") == 0) ? 1 : 0;
        return;
    }
    if (strcmp(prop, "text-align") == 0) {
        strncpy(s->text_align, val, sizeof(s->text_align) - 1);
        return;
    }
    if (strcmp(prop, "line-height") == 0) {
        s->line_height = parse_length(val);
        return;
    }
}

/* ════════════════════════════════════════════════════════════════
   Вычисление каскада
   ════════════════════════════════════════════════════════════════ */

int css_compute_style(const CSSStylesheet* sheet,
                      const DOMNode*       node,
                      CSSComputedStyle*    out) {
    if (!sheet || !node || !out) return -1;
    css_computed_style_init(out);

    /* Собираем все подходящие правила в массив, затем сортируем
       по специфичности (stable sort: порядок объявления уже сохранён
       в связном списке — идём по нему, добавляем с сохранением порядка). */

    typedef struct { const CSSRule* rule; int order; } Match;

    int cap = 32;
    Match* matches = malloc(cap * sizeof(Match));
    if (!matches) return -1;
    int cnt = 0, order = 0;

    const CSSRule* rule = sheet->rules;
    while (rule) {
        if (matches_selector(node, rule->selector)) {
            if (cnt == cap) {
                cap *= 2;
                matches = realloc(matches, cap * sizeof(Match));
                if (!matches) return -1;
            }
            matches[cnt].rule  = rule;
            matches[cnt].order = order;
            cnt++;
        }
        order++;
        rule = rule->next;
    }

    /* Insertion sort по специфичности (малые наборы — O(n²) OK) */
    for (int i = 1; i < cnt; i++) {
        Match key = matches[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp = specificity_cmp(matches[j].rule->specificity,
                                      key.rule->specificity);
            if (cmp > 0 || (cmp == 0 && matches[j].order > key.order)) break;
            matches[j + 1] = matches[j];
            j--;
        }
        matches[j + 1] = key;
    }

    /* Применяем от наименьшей специфичности к наибольшей */
    for (int i = 0; i < cnt; i++) {
        const CSSDeclaration* decl = matches[i].rule->declarations;
        while (decl) {
            apply_declaration(out, decl->property, decl->value);
            decl = decl->next;
        }
    }

    free(matches);
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   Обратная совместимость — старый API
   ════════════════════════════════════════════════════════════════ */

static void write_back_to_node(DOMNode* node, const CSSComputedStyle* s) {
    if (s->bg_color   >= 0) node->bg_color   = (uint32_t)s->bg_color;
    if (s->text_color >= 0) node->text_color  = (uint32_t)s->text_color;
    if (s->width.unit  == UNIT_PX) node->width  = (int)s->width.value;
    if (s->height.unit == UNIT_PX) node->height = (int)s->height.value;
    if (s->font_family[0] != '\0') {
        strncpy(node->font_family, s->font_family, sizeof(node->font_family) - 1);
        node->font_family[sizeof(node->font_family) - 1] = '\0';
    }
}

void css_apply_stylesheet(DOMNode* head, const char* css_data) {
    if (!css_data || !head || head->v2) return;

    CSSStylesheet* sheet = css_parse(css_data);
    if (!sheet) return;

    DOMNode* curr = head;
    while (curr) {
        CSSComputedStyle style;
        if (css_compute_style(sheet, curr, &style) == 0) {
            write_back_to_node(curr, &style);
        }
        curr = curr->next;
    }

    css_stylesheet_free(sheet);
}

void css_load_and_apply(DOMNode* head, const char* filename) {
    css_load_and_apply_ex(head, filename, NULL);
}

void css_load_and_apply_ex(DOMNode* head, const char* filename, const char* page_url) {
    if (!filename || !head || head->v2) return;

    FILE* f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "css_engine: cannot open '%s'\n", filename);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);

    char* buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return; }

    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);

    CSSStylesheet* sheet = css_parse_ex(buf, page_url);
    if (!sheet) { free(buf); return; }

    DOMNode* curr = head;
    while (curr) {
        CSSComputedStyle style;
        if (css_compute_style(sheet, curr, &style) == 0) {
            write_back_to_node(curr, &style);
        }
        curr = curr->next;
    }

    css_stylesheet_free(sheet);
    free(buf);
}
