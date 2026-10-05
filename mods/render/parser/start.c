#include "parser.h"
#include "gal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

static void get_tag_name(const char* ptr, char* name, int max_len) {
    name[0] = '\0';
    if (*ptr != '<') return;
    ptr++;
    int i = 0;
    while (*ptr && *ptr != ' ' && *ptr != '\t' && *ptr != '>' && *ptr != '/' && i < max_len - 1) {
        name[i++] = *ptr++;
    }
    name[i] = '\0';
}

static void get_attr_value(const char* tag_start, const char* tag_close, const char* attr_name, char* dest, int max_len) {
    dest[0] = '\0';
    int attr_len = strlen(attr_name);
    const char* p = tag_start;
    while (p < tag_close) {
        if (strncasecmp(p, attr_name, attr_len) == 0 && (p[attr_len] == '=')) {
            const char* val_start = p + attr_len + 1;
            char quote = '\0';
            if (*val_start == '"' || *val_start == '\'') {
                quote = *val_start;
                val_start++;
            }
            const char* val_end = val_start;
            if (quote != '\0') {
                val_end = strchr(val_start, quote);
                if (!val_end || val_end > tag_close) val_end = tag_close;
            } else {
                while (val_end < tag_close && *val_end != ' ' && *val_end != '\t' && *val_end != '>') {
                    val_end++;
                }
            }
            int len = (int)(val_end - val_start);
            if (len >= max_len) len = max_len - 1;
            strncpy(dest, val_start, len);
            dest[len] = '\0';
            return;
        }
        p++;
    }
}

static void parse_color(const char* hex, uint32_t* out) {
    if (hex[0] == '#') hex++;
    int hlen = strlen(hex);
    if (hlen == 6) {
        *out = (uint32_t)strtoul(hex, NULL, 16);
    } else if (hlen == 3) {
        char r = hex[0];
        char g = hex[1];
        char b = hex[2];
        char expanded[7] = {r, r, g, g, b, b, '\0'};
        *out = (uint32_t)strtoul(expanded, NULL, 16);
    }
}

void parse_inline_style(DOMNode* node, const char* style_str) {
    if (!style_str) return;
    node->has_style = 1;
    
    const char* p = style_str;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        
        const char* prop_start = p;
        const char* colon = strchr(p, ':');
        if (!colon) break;
        
        int prop_len = (int)(colon - prop_start);
        while (prop_len > 0 && (prop_start[prop_len - 1] == ' ' || prop_start[prop_len - 1] == '\t')) {
            prop_len--;
        }

        p = colon + 1;
        while (*p == ' ' || *p == '\t') p++;
        
        const char* val_start = p;
        const char* semi = strchr(p, ';');
        const char* val_end = semi;
        if (!val_end) {
            val_end = p + strlen(p);
            p = val_end;
        } else {
            p = semi + 1;
        }
        
        int val_len = (int)(val_end - val_start);
        while (val_len > 0 && (val_start[val_len - 1] == ' ' || val_start[val_len - 1] == '\t')) {
            val_len--;
        }

        char prop[64] = {0};
        char val[128] = {0};
        if (prop_len >= (int)sizeof(prop)) prop_len = sizeof(prop) - 1;
        if (val_len >= (int)sizeof(val)) val_len = sizeof(val) - 1;
        strncpy(prop, prop_start, prop_len);
        strncpy(val, val_start, val_len);

        if (strcmp(prop, "position") == 0) {
            if (strcmp(val, "absolute") == 0) {
                node->is_absolute = 1;
            }
        } else if (strcmp(prop, "left") == 0) {
            node->left = atoi(val);
        } else if (strcmp(prop, "top") == 0) {
            node->top = atoi(val);
        } else if (strcmp(prop, "width") == 0) {
            node->width = atoi(val);
        } else if (strcmp(prop, "height") == 0) {
            node->height = atoi(val);
        } else if (strcmp(prop, "background-color") == 0 || strcmp(prop, "background") == 0) {
            parse_color(val, &node->bg_color);
        } else if (strcmp(prop, "color") == 0) {
            parse_color(val, &node->text_color);
        } else if (strcmp(prop, "font-family") == 0) {
            strncpy(node->font_family, val, sizeof(node->font_family) - 1);
            node->font_family[sizeof(node->font_family) - 1] = '\0';
        } else if (strcmp(prop, "text-align") == 0) {
            if (strcmp(val, "center") == 0) node->is_centered = 1;
        }
    }
}

static const char* find_case_insensitive(const char* haystack, const char* needle) {
    int needle_len = strlen(needle);
    while (*haystack) {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
        haystack++;
    }
    return NULL;
}

static void append_utf8(char* dest, int max_len, uint32_t cp) {
    int len = strlen(dest);
    if (len >= max_len - 1) return;
    if (cp < 0x80) {
        dest[len++] = (char)cp;
    } else if (cp < 0x800 && len + 2 < max_len) {
        dest[len++] = (char)(0xC0 | (cp >> 6));
        dest[len++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000 && len + 3 < max_len) {
        dest[len++] = (char)(0xE0 | (cp >> 12));
        dest[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dest[len++] = (char)(0x80 | (cp & 0x3F));
    }
    dest[len] = '\0';
}

static void html_decode(const char* src, char* dest, int max_len) {
    dest[0] = '\0';
    while (*src && (int)strlen(dest) < max_len - 1) {
        if (*src == '&') {
            if (strncmp(src, "&amp;", 5) == 0) { strncat(dest, "&", max_len - strlen(dest) - 1); src += 5; continue; }
            if (strncmp(src, "&lt;", 4) == 0) { strncat(dest, "<", max_len - strlen(dest) - 1); src += 4; continue; }
            if (strncmp(src, "&gt;", 4) == 0) { strncat(dest, ">", max_len - strlen(dest) - 1); src += 4; continue; }
            if (strncmp(src, "&quot;", 6) == 0) { strncat(dest, "\"", max_len - strlen(dest) - 1); src += 6; continue; }
            if (strncmp(src, "&#", 2) == 0) {
                char* end = NULL;
                uint32_t cp = 0;
                if (src[2] == 'x' || src[2] == 'X') cp = (uint32_t)strtoul(src + 3, &end, 16);
                else cp = (uint32_t)strtoul(src + 2, &end, 10);
                if (end && *end == ';' && cp > 0) {
                    append_utf8(dest, max_len, cp);
                    src = end + 1;
                    continue;
                }
            }
        }
        int len = strlen(dest);
        dest[len] = *src++;
        dest[len + 1] = '\0';
    }
}

void extract_html_title(const char* html, char* dest, int max_len) {
    dest[0] = '\0';
    if (!html || max_len <= 0) return;
    const char* start = find_case_insensitive(html, "<title");
    if (!start) return;
    start = strchr(start, '>');
    if (!start) return;
    start++;
    const char* end = find_case_insensitive(start, "</title>");
    if (!end || end <= start) return;
    char raw[512];
    int len = (int)(end - start);
    if (len > (int)sizeof(raw) - 1) len = sizeof(raw) - 1;
    strncpy(raw, start, len);
    raw[len] = '\0';
    html_decode(raw, dest, max_len);
}

void extract_html_icon_href(const char* html, char* dest, int max_len) {
    dest[0] = '\0';
    if (!html || max_len <= 0) return;
    const char* p = html;
    while ((p = find_case_insensitive(p, "<link")) != NULL) {
        const char* close = strchr(p, '>');
        if (!close) return;
        char rel[128] = {0};
        get_attr_value(p, close, "rel", rel, sizeof(rel));
        if (find_case_insensitive(rel, "icon")) {
            get_attr_value(p, close, "href", dest, max_len);
            return;
        }
        p = close + 1;
    }
}

DOMNode* parse_html(const char* html) {
    if (!html) return NULL;
    
    DOMNode* head = NULL;
    DOMNode* current = NULL;
    
    const char* ptr = html;
    
    DOMNode* current_anchor = NULL; // Keep track of current anchor to append text
    
    while (*ptr != '\0') {
        // Skip whitespace
        if (*ptr == ' ' || *ptr == '\t' || *ptr == '\r' || *ptr == '\n') {
            ptr++;
            continue;
        }
        
        if (*ptr == '<') {
            // 1. Comment check
            if (strncmp(ptr, "<!--", 4) == 0) {
                const char* comment_end = strstr(ptr, "-->");
                if (comment_end) {
                    ptr = comment_end + 3;
                } else {
                    ptr += 4;
                }
                continue;
            }
            
            // 2. Closing tag check
            if (*(ptr + 1) == '/') {
                const char* tag_close = strchr(ptr, '>');
                if (tag_close) {
                    char tag_name[64] = {0};
                    get_tag_name(ptr + 1, tag_name, sizeof(tag_name));
                    if (strcasecmp(tag_name, "a") == 0) {
                        current_anchor = NULL;
                    }
                    ptr = tag_close + 1;
                } else {
                    ptr += 2;
                }
                continue;
            }
            
            // 3. Parse opening tag
            const char* tag_close = strchr(ptr, '>');
            if (!tag_close) {
                ptr++; // malformed tag, skip '<'
                continue;
            }
            
            char tag_name[64] = {0};
            get_tag_name(ptr, tag_name, sizeof(tag_name));
            
            TagType detected_type = TAG_NONE;
            if (strcasecmp(tag_name, "h") == 0 || strcasecmp(tag_name, "h1") == 0 ||
                strcasecmp(tag_name, "h2") == 0 || strcasecmp(tag_name, "h3") == 0 ||
                strcasecmp(tag_name, "h4") == 0 || strcasecmp(tag_name, "h5") == 0 ||
                strcasecmp(tag_name, "h6") == 0) {
                detected_type = TAG_H;
            } else if (strcasecmp(tag_name, "p") == 0) {
                detected_type = TAG_P;
            } else if (strcasecmp(tag_name, "a") == 0) {
                detected_type = TAG_A;
            } else if (strcasecmp(tag_name, "img") == 0) {
                detected_type = TAG_IMG;
            } else if (strcasecmp(tag_name, "div") == 0) {
                detected_type = TAG_DIV;
            } else if (strcasecmp(tag_name, "button") == 0) {
                detected_type = TAG_BUTTON;
            } else if (strcasecmp(tag_name, "input") == 0) {
                detected_type = TAG_INPUT;
            } else if (strcasecmp(tag_name, "span") == 0 || strcasecmp(tag_name, "b") == 0 ||
                       strcasecmp(tag_name, "i") == 0 || strcasecmp(tag_name, "strong") == 0 ||
                       strcasecmp(tag_name, "em") == 0 || strcasecmp(tag_name, "u") == 0 ||
                       strcasecmp(tag_name, "font") == 0) {
                detected_type = TAG_SPAN;
            } else if (strcasecmp(tag_name, "html") == 0) {
                detected_type = TAG_HTML;
            } else if (strcasecmp(tag_name, "head") == 0) {
                detected_type = TAG_HEAD;
            } else if (strcasecmp(tag_name, "body") == 0) {
                detected_type = TAG_BODY;
            } else if (strcasecmp(tag_name, "title") == 0) {
                detected_type = TAG_TITLE;
            } else if (strcasecmp(tag_name, "meta") == 0) {
                detected_type = TAG_META;
            } else if (strcasecmp(tag_name, "link") == 0) {
                detected_type = TAG_LINK;
            } else if (strcasecmp(tag_name, "style") == 0) {
                detected_type = TAG_STYLE;
            } else if (strcasecmp(tag_name, "script") == 0) {
                detected_type = TAG_SCRIPT;
                const char* script_end = find_case_insensitive(tag_close + 1, "</script>");
                if (script_end) {
                    int script_len = (int)(script_end - (tag_close + 1));
                    if (script_len > 0) {
                        DOMNode* node = malloc(sizeof(DOMNode));
                        memset(node, 0, sizeof(DOMNode));
                        node->type = TAG_SCRIPT;
                        if (script_len > 1023) script_len = 1023;
                        strncpy(node->text, tag_close + 1, script_len);
                        node->text[script_len] = '\0';
                        if (!head) { head = node; current = head; }
                        else { current->next = node; current = node; }
                    }
                    ptr = script_end + 9;
                    continue;
                }
            } else if (strcasecmp(tag_name, "main") == 0) {
                detected_type = TAG_MAIN;
            } else if (strcasecmp(tag_name, "section") == 0) {
                detected_type = TAG_SECTION;
            } else if (strcasecmp(tag_name, "article") == 0) {
                detected_type = TAG_ARTICLE;
            } else if (strcasecmp(tag_name, "header") == 0) {
                detected_type = TAG_HEADER;
            } else if (strcasecmp(tag_name, "footer") == 0) {
                detected_type = TAG_FOOTER;
            } else if (strcasecmp(tag_name, "nav") == 0) {
                detected_type = TAG_NAV;
            } else if (strcasecmp(tag_name, "aside") == 0) {
                detected_type = TAG_ASIDE;
            } else if (strcasecmp(tag_name, "ul") == 0) {
                detected_type = TAG_UL;
            } else if (strcasecmp(tag_name, "ol") == 0) {
                detected_type = TAG_OL;
            } else if (strcasecmp(tag_name, "li") == 0) {
                detected_type = TAG_LI;
            } else if (strcasecmp(tag_name, "table") == 0) {
                detected_type = TAG_TABLE;
            } else if (strcasecmp(tag_name, "tr") == 0) {
                detected_type = TAG_TR;
            } else if (strcasecmp(tag_name, "td") == 0) {
                detected_type = TAG_TD;
            } else if (strcasecmp(tag_name, "th") == 0) {
                detected_type = TAG_TH;
            } else if (strcasecmp(tag_name, "thead") == 0) {
                detected_type = TAG_THEAD;
            } else if (strcasecmp(tag_name, "tbody") == 0) {
                detected_type = TAG_TBODY;
            } else if (strcasecmp(tag_name, "form") == 0) {
                detected_type = TAG_FORM;
            } else if (strcasecmp(tag_name, "label") == 0) {
                detected_type = TAG_LABEL;
            } else if (strcasecmp(tag_name, "select") == 0) {
                detected_type = TAG_SELECT;
            } else if (strcasecmp(tag_name, "option") == 0) {
                detected_type = TAG_OPTION;
            } else if (strcasecmp(tag_name, "textarea") == 0) {
                detected_type = TAG_TEXTAREA;
            } else if (strcasecmp(tag_name, "fieldset") == 0) {
                detected_type = TAG_FIELDSET;
            } else if (strcasecmp(tag_name, "legend") == 0) {
                detected_type = TAG_LEGEND;
            } else if (strcasecmp(tag_name, "br") == 0) {
                detected_type = TAG_BR;
            } else if (strcasecmp(tag_name, "hr") == 0) {
                detected_type = TAG_HR;
            } else if (strcasecmp(tag_name, "iframe") == 0) {
                detected_type = TAG_IFRAME;
            } else if (strcasecmp(tag_name, "video") == 0) {
                detected_type = TAG_VIDEO;
            } else if (strcasecmp(tag_name, "audio") == 0) {
                detected_type = TAG_AUDIO;
            } else if (strcasecmp(tag_name, "source") == 0) {
                detected_type = TAG_SOURCE;
            } else if (strcasecmp(tag_name, "canvas") == 0) {
                detected_type = TAG_CANVAS;
            } else if (strcasecmp(tag_name, "svg") == 0) {
                detected_type = TAG_SVG;
            } else if (strcasecmp(tag_name, "picture") == 0) {
                detected_type = TAG_PICTURE;
            } else if (strcasecmp(tag_name, "noscript") == 0) {
                detected_type = TAG_NOSCRIPT;
            }
            
            if (detected_type != TAG_NONE) {
                DOMNode* node = malloc(sizeof(DOMNode));
                if (!node) { gal_log("ERROR", "Out of memory!"); return head; }
                memset(node, 0, sizeof(DOMNode));
                node->type = detected_type;
                node->bg_color = 0xFFFFFF;
                node->text_color = 0x000000;
                
                // Get attributes
                get_attr_value(ptr, tag_close, "href", node->href, sizeof(node->href));
                get_attr_value(ptr, tag_close, "src", node->src, sizeof(node->src));
                get_attr_value(ptr, tag_close, "id", node->id, sizeof(node->id));
                if (node->id[0] == '\0') {
                    get_attr_value(ptr, tag_close, "name", node->id, sizeof(node->id));
                }
                get_attr_value(ptr, tag_close, "class", node->class_name, sizeof(node->class_name));
                get_attr_value(ptr, tag_close, "style", node->style_str, sizeof(node->style_str));
                get_attr_value(ptr, tag_close, "onclick", node->onclick, sizeof(node->onclick));
                get_attr_value(ptr, tag_close, "value", node->value, sizeof(node->value));

                // Retro attributes
                char attr_val[128];
                get_attr_value(ptr, tag_close, "bgcolor", attr_val, sizeof(attr_val));
                if (attr_val[0]) { parse_color(attr_val, &node->bg_color); node->has_style = 1; }

                get_attr_value(ptr, tag_close, "text", attr_val, sizeof(attr_val));
                if (attr_val[0]) { parse_color(attr_val, &node->text_color); node->has_style = 1; }

                get_attr_value(ptr, tag_close, "color", attr_val, sizeof(attr_val));
                if (attr_val[0]) { parse_color(attr_val, &node->text_color); node->has_style = 1; }

                get_attr_value(ptr, tag_close, "align", attr_val, sizeof(attr_val));
                if (strcasecmp(attr_val, "center") == 0) { node->is_centered = 1; node->has_style = 1; }

                /* rows / cols for textarea */
                get_attr_value(ptr, tag_close, "rows", attr_val, sizeof(attr_val));
                if (attr_val[0]) node->rows = atoi(attr_val);
                get_attr_value(ptr, tag_close, "cols", attr_val, sizeof(attr_val));
                if (attr_val[0]) node->cols = atoi(attr_val);

                if (node->src[0] == '\0' && node->href[0] != '\0' && detected_type == TAG_IMG) {
                    strcpy(node->src, node->href);
                    node->href[0] = '\0';
                }
                
                if (strlen(node->style_str) > 0) {
                    parse_inline_style(node, node->style_str);
                }

                if (detected_type == TAG_A) {
                    current_anchor = node;
                }

                // Link node
                if (!head) { head = node; current = head; }
                else { current->next = node; current = node; }

                // For non-input and non-img tags, we can extract immediate text following the tag
                if (detected_type != TAG_IMG && detected_type != TAG_INPUT) {
                    const char* text_start = tag_close + 1;
                    const char* text_end = strchr(text_start, '<');
                    if (text_start && text_end && text_end > text_start) {
                        int text_len = (int)(text_end - text_start);
                        if (text_len > 511) text_len = 511;
                        char raw_text[512];
                        strncpy(raw_text, text_start, text_len);
                        raw_text[text_len] = '\0';
                        html_decode(raw_text, node->text, sizeof(node->text));
                        text_len = strlen(node->text);
                        for (int i = 0; i < text_len; i++) {
                            if (node->text[i] == '\n' || node->text[i] == '\r' || node->text[i] == '\t') {
                                node->text[i] = ' ';
                            }
                        }
                        // Trim leading spaces from normal block elements
                        if (detected_type != TAG_SPAN && detected_type != TAG_A) {
                            char* s = node->text;
                            while (*s == ' ') s++;
                            if (s != node->text) {
                                memmove(node->text, s, strlen(s) + 1);
                            }
                        }
                        ptr = text_end;
                        continue;
                    }
                }
            }
            
            ptr = tag_close + 1;
            continue;
        }
        
        // 5. Parse raw text runs
        const char* text_start = ptr;
        const char* text_end = strchr(ptr, '<');
        if (!text_end) text_end = ptr + strlen(ptr);
        
        int text_len = (int)(text_end - text_start);
        if (text_len > 0) {
            char text_buf[512];
            if (text_len > 511) text_len = 511;
                        char raw_text[512];
                        strncpy(raw_text, text_start, text_len);
                        raw_text[text_len] = '\0';
                        html_decode(raw_text, text_buf, sizeof(text_buf));
                        text_len = strlen(text_buf);
            for (int i = 0; i < text_len; i++) {
                if (text_buf[i] == '\n' || text_buf[i] == '\r' || text_buf[i] == '\t') {
                    text_buf[i] = ' ';
                }
            }
            
            // Only add if not entirely spaces (or if inside anchor)
            int has_non_space = 0;
            for (int i = 0; i < text_len; i++) {
                if (text_buf[i] != ' ') { has_non_space = 1; break; }
            }
            
            if (has_non_space || current_anchor) {
                if (current_anchor) {
                    // Append to active link text
                    int cur_len = strlen(current_anchor->text);
                    if (cur_len < 511) {
                        strncat(current_anchor->text, text_buf, 511 - cur_len);
                    }
                } else {
                    // Create inline span node
                    DOMNode* node = malloc(sizeof(DOMNode));
                    if (node) {
                        memset(node, 0, sizeof(DOMNode));
                        node->type = TAG_SPAN;
                        node->bg_color = 0xFFFFFF;
                        node->text_color = 0x000000;
                        strcpy(node->text, text_buf);
                        
                        if (!head) { head = node; current = head; }
                        else { current->next = node; current = node; }
                    }
                }
            }
        }
        ptr = text_end;
    }
    
    return head;
}

void dom_free_pseudo(DOMNode* n) {
    if (!n) return;
    free(n->pb[0]); free(n->pb[1]); n->pb[0] = n->pb[1] = NULL;
}

void free_dom(DOMNode* head) {
    while (head != NULL) {
        DOMNode* temp = head;
        head = head->next;
        dom_free_pseudo(temp);
        free(temp);
    }
}

DOMNode* clone_dom(DOMNode* head) {
    DOMNode* new_head = NULL;
    DOMNode* last = NULL;
    DOMNode* curr = head;
    while (curr) {
        DOMNode* node = malloc(sizeof(DOMNode));
        if (!node) break;
        memcpy(node, curr, sizeof(DOMNode));
        node->next = NULL;
        node->pb[0] = node->pb[1] = NULL;
        if (!new_head) new_head = node;
        else last->next = node;
        last = node;
        curr = curr->next;
    }
    return new_head;
}

void serialize_dom(DOMNode* head, const char* filename) {
    FILE* f = fopen(filename, "wb");
    if (!f) return;
    DOMNode* curr = head;
    DOMNode* tmp = malloc(sizeof(DOMNode));
    while (curr && tmp) {
        memcpy(tmp, curr, sizeof(DOMNode));
        tmp->pb[0] = tmp->pb[1] = NULL;            /* указатели не переживают границу процессов */
        fwrite(tmp, sizeof(DOMNode), 1, f);
        curr = curr->next;
    }
    free(tmp);
    fclose(f);
}

DOMNode* deserialize_dom(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;
    DOMNode* head = NULL;
    DOMNode* last = NULL;
    DOMNode tmp;
    while (fread(&tmp, sizeof(DOMNode), 1, f) == 1) {
        DOMNode* node = malloc(sizeof(DOMNode));
        if (!node) break;
        memcpy(node, &tmp, sizeof(DOMNode));
        node->next = NULL;
        node->pb[0] = node->pb[1] = NULL;
        if (!head) head = node;
        else last->next = node;
        last = node;
    }
    fclose(f);
    return head;
}
