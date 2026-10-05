#include "layout.h"
#include "image_loader.h"
#include "font_manager.h"
#include "webpage.h"
#include "gal.h"
#include "nal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>

#ifdef HAVE_UI
extern int loading_progress;
#endif

/*
static uint32_t utf8_decode(const char** text) {
    const unsigned char* s = (const unsigned char*)*text;
    if (*s == 0) return 0;
    uint32_t c = *s++;
    if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0 && s[0]) {
            c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
        } else if ((c & 0xF0) == 0xE0 && s[0] && s[1]) {
            c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
            s += 2;
        } else if ((c & 0xF8) == 0xF0 && s[0] && s[1] && s[2]) {
            c = ((c & 0x07) << 18) | ((s[0] & 0x3F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            s += 3;
        }
    }
    *text = (const char*)s;
    return c;
}
*/

/*
static void to_uppercase(char* dest, const char* src) {
    const char* ptr = src;
    while (*ptr) {
        uint32_t c = utf8_decode(&ptr);
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 0x0430 && c <= 0x044F) c -= 32; // Cyrillic
        else if (c == 0x0451) c = 0x0401;

        if (c < 0x80) {
            *dest++ = (char)c;
        } else if (c < 0x800) {
            *dest++ = (char)(0xC0 | (c >> 6));
            *dest++ = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            *dest++ = (char)(0xE0 | (c >> 12));
            *dest++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *dest++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *dest = '\0';
}
*/

static int ends_with_case(const char* str, const char* ext) {
    int str_len = strlen(str);
    int ext_len = strlen(ext);
    if (str_len < ext_len) return 0;
    return (strncasecmp(str + str_len - ext_len, ext, ext_len) == 0);
}

// Resolve URL and download image if it's remote, returning the path to the cached file
static const char* get_cached_image(const char* src, const char* page_url) {
    static char cached_path[512];
    
    // If local file, check if it exists in assets or directly
    if (strncmp(src, "http://", 7) != 0 && strncmp(src, "https://", 8) != 0) {
        if (!page_url || (strncmp(page_url, "http://", 7) != 0 && strncmp(page_url, "https://", 8) != 0)) {
            if (src[0] == '/') {
                snprintf(cached_path, sizeof(cached_path), "%s", src);
            } else {
                snprintf(cached_path, sizeof(cached_path), "assets/%s", src);
            }
            return cached_path;
        }
    }
    
    // Resolve absolute URL
    char absolute_url[1024] = {0};
    if (strncmp(src, "http://", 7) == 0 || strncmp(src, "https://", 8) == 0) {
        strncpy(absolute_url, src, sizeof(absolute_url) - 1);
    } else if (page_url) {
        char protocol[16] = "http://";
        if (strncmp(page_url, "https://", 8) == 0) {
            strcpy(protocol, "https://");
        }
        
        const char* host_start = page_url + strlen(protocol);
        char host[256] = {0};
        const char* slash = strchr(host_start, '/');
        if (slash) {
            strncpy(host, host_start, slash - host_start);
        } else {
            strcpy(host, host_start);
        }
        
        if (src[0] == '/') {
            snprintf(absolute_url, sizeof(absolute_url), "%s%s%s", protocol, host, src);
        } else {
            const char* last_slash = strrchr(page_url, '/');
            if (last_slash && last_slash > strchr(page_url, '/') + 1) {
                int dir_len = last_slash - page_url;
                strncpy(absolute_url, page_url, dir_len);
                strcat(absolute_url, "/");
                strcat(absolute_url, src);
            } else {
                snprintf(absolute_url, sizeof(absolute_url), "%s/%s", page_url, src);
            }
        }
    } else {
        snprintf(cached_path, sizeof(cached_path), "assets/%s", src);
        return cached_path;
    }
    
#ifdef _WIN32
    mkdir("cache");
#else
    mkdir("cache", 0777);
#endif

    // Create unique safe filename in cache/
    char safe_filename[512] = {0};
    int j = 0;
    for (int i = 0; absolute_url[i] != '\0' && j < 240; i++) {
        char c = absolute_url[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            safe_filename[j++] = c;
        } else {
            safe_filename[j++] = '_';
        }
    }
    
    if (ends_with_case(src, ".png")) strcat(safe_filename, ".png");
    else if (ends_with_case(src, ".jpg") || ends_with_case(src, ".jpeg")) strcat(safe_filename, ".jpg");
    else if (ends_with_case(src, ".bmp")) strcat(safe_filename, ".bmp");
    else strcat(safe_filename, ".png");
    
    snprintf(cached_path, sizeof(cached_path), "cache/%s", safe_filename);
    
    FILE* f = fopen(cached_path, "r");
    if (f) {
        fclose(f);
        return cached_path;
    }
    
    if (nal_download_file(absolute_url, cached_path)) {
        return cached_path;
    }
    
    return NULL;
}

int layout_content_width = 0;
int layout_content_height = 0;
int layout_selection_active = 0;
int layout_selection_x1 = 0;
int layout_selection_y1 = 0;
int layout_selection_x2 = 0;
int layout_selection_y2 = 0;

static int rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static void draw_selection_if_needed(int x, int y, int w, int h) {
    if (!layout_selection_active || w <= 0 || h <= 0) return;
    int sx = layout_selection_x1 < layout_selection_x2 ? layout_selection_x1 : layout_selection_x2;
    int sy = layout_selection_y1 < layout_selection_y2 ? layout_selection_y1 : layout_selection_y2;
    int sw = abs(layout_selection_x2 - layout_selection_x1);
    int sh = abs(layout_selection_y2 - layout_selection_y1);
    if (sw < 2) sw = 2;
    if (sh < 2) sh = 2;
    if (rects_intersect(x, y, w, h, sx, sy, sw, sh)) {
        gal_draw_rect(x, y, w, h, 0xBFDBFE);
    }
}

/* Ширина текста тем же шрифтом, которым он будет нарисован (учитывает font-family узла). */
static int text_width_family(const char* str, int font_size, const char* family) {
    TTFFont* font = font_manager_get(family);
    if (!font) return gal_ttf_text_width(str, font_size);
    return ttf_text_width(font, str, font_size);
}

static void gal_draw_ttf_string_with_selection(int x, int y, const char* str, uint32_t color, int font_size, const char* family) {
    TTFFont* font = font_manager_get(family);
    if (!font) {                                   /* TTF не загружен — bitmap-шрифт из gal */
        gal_draw_ttf_string(x, y, str, color, font_size);
        return;
    }
    if (!layout_selection_active) {
        ttf_render_string(font, str, back_buffer, SCREEN_WIDTH, SCREEN_HEIGHT, font_size, x, y, color);
        return;
    }

    int sx = layout_selection_x1 < layout_selection_x2 ? layout_selection_x1 : layout_selection_x2;
    int sy = layout_selection_y1 < layout_selection_y2 ? layout_selection_y1 : layout_selection_y2;
    int sw = abs(layout_selection_x2 - layout_selection_x1);
    int sh = abs(layout_selection_y2 - layout_selection_y1);

    size_t len = strlen(str);
    /* Строки, не задетые выделением по вертикали, посимвольно не разбираем. */
    if (len > 0 && rects_intersect(x, y, 1 << 20, font_size + 4, sx, sy, sw, sh)) {
        int* offs = malloc((len + 1) * sizeof(int));
        if (offs) {
            /* Позиции глифов из рендерера — прямоугольники стыкуются и совпадают с рисунком. */
            ttf_text_offsets(font, str, font_size, offs, (int)len + 1);
            const char* p = str;
            while (*p) {
                int char_len = 1;
                unsigned char c = (unsigned char)*p;
                if ((c & 0x80) == 0) char_len = 1;
                else if ((c & 0xE0) == 0xC0) char_len = 2;
                else if ((c & 0xF0) == 0xE0) char_len = 3;
                else if ((c & 0xF8) == 0xF0) char_len = 4;
                size_t rest = strlen(p);
                if ((size_t)char_len > rest) char_len = (int)rest;

                int off = (int)(p - str);
                int cx = x + offs[off];
                int cw = offs[off + char_len] - offs[off];
                if (rects_intersect(cx, y, cw, font_size + 4, sx, sy, sw, sh)) {
                    gal_draw_rect(cx, y, cw, font_size + 4, 0xBFDBFE);
                }
                p += char_len;
            }
            free(offs);
        }
    }

    ttf_render_string(font, str, back_buffer, SCREEN_WIDTH, SCREEN_HEIGHT, font_size, x, y, color);
}

static void track_bounds(int x, int y, int w, int h, int scroll_x, int scroll_y) {
    int logical_right = x + scroll_x + w;
    int logical_bottom = y + scroll_y + h;
    if (logical_right > layout_content_width) layout_content_width = logical_right;
    if (logical_bottom > layout_content_height) layout_content_height = logical_bottom;
}

void layout_and_render(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y) {
    if (head && head->v2) { webpage_render(head, top_offset, page_url, scroll_x, scroll_y); return; }   /* новый движок */

    layout_content_width = 0;
    layout_content_height = 0;

    int current_y = top_offset + 20 - scroll_y;
    int current_x = 50 - scroll_x;
    int max_line_height = 0;

    /* Inherited style context from body/font tags */
    uint32_t inherited_text_color = 0x111111;
    uint32_t inherited_bg_color   = 0xFFFFFF;
    int      context_centered     = 0;  /* set by <td align=center> or <table> context */
    int      context_font_size    = 20; /* set by <font size=N> */

    DOMNode* node = head;
    while (node != NULL) {
        /* ── Inherit body/font context ── */
        if (node->type == TAG_BODY) {
            if (node->has_style) {
                inherited_bg_color   = node->bg_color;
                inherited_text_color = node->text_color;
            }
            node = node->next;
            continue;
        }
        if (node->type == TAG_TD || node->type == TAG_TH) {
            context_centered = node->is_centered;
            node = node->next;
            continue;
        }
        if (node->type == TAG_TR || node->type == TAG_THEAD || node->type == TAG_TBODY) {
            node = node->next;
            continue;
        }
        /* <font> tag — update color/size context then render text inline */
        if (node->type == TAG_SPAN && node->has_style && strlen(node->text) > 0 &&
            node->font_family[0] != '\0') {
            /* font tag rendered as styled span — fall through to SPAN case */
        }
        if (node->is_absolute) {
            int rx = node->left - scroll_x;
            int ry = node->top + top_offset - scroll_y;
            int rw = node->width;
            int rh = node->height;
            
            node->hit_x = rx;
            node->hit_y = ry;
            node->hit_w = rw;
            node->hit_h = rh;
            track_bounds(rx, ry, rw, rh, scroll_x, scroll_y);
            
            if (node->type == TAG_DIV || node->type == TAG_MAIN || node->type == TAG_SECTION ||
                node->type == TAG_ARTICLE || node->type == TAG_HEADER || node->type == TAG_FOOTER ||
                node->type == TAG_NAV || node->type == TAG_ASIDE || node->type == TAG_FORM ||
                node->type == TAG_UL || node->type == TAG_OL || node->type == TAG_BODY ||
                node->type == TAG_TABLE || node->type == TAG_TR || node->type == TAG_THEAD ||
                node->type == TAG_TBODY || node->type == TAG_FIELDSET || node->type == TAG_NOSCRIPT) {
                gal_draw_rect(rx, ry, rw, rh, node->bg_color);
            }
            else if (node->type == TAG_BUTTON) {
                gal_draw_rect(rx, ry, rw, rh, node->bg_color);
                
                // Рамка кнопки
                uint32_t border_color = 0x4B5563;
                gal_draw_rect(rx, ry, rw, 1, border_color);
                gal_draw_rect(rx, ry + rh - 1, rw, 1, border_color);
                gal_draw_rect(rx, ry, 1, rh, border_color);
                gal_draw_rect(rx + rw - 1, ry, 1, rh, border_color);
                
                int tw = text_width_family(node->text, 14, node->font_family);
                int tx = rx + (rw - tw) / 2;
                int ty = ry + (rh - 14) / 2;
                gal_draw_ttf_string_with_selection(tx, ty, node->text, node->text_color, 14, node->font_family);
            }
            else if (node->type == TAG_INPUT) {
                gal_draw_rect(rx, ry, rw, rh, node->bg_color);
#ifdef HAVE_UI
                if (strcmp(node->id, "url-bar") == 0 && loading_progress > 0) {
                    int progress_w = (rw - 4) * loading_progress / 100;
                    gal_draw_rect(rx + 2, ry + 2, progress_w, rh - 4, 0xBFDBFE);
                }
#endif
                draw_selection_if_needed(rx + 4, ry + 4, rw - 8, rh - 8);
                
                uint32_t border_color = 0x9CA3AF;
                gal_draw_rect(rx, ry, rw, 1, border_color);
                gal_draw_rect(rx, ry + rh - 1, rw, 1, border_color);
                gal_draw_rect(rx, ry, 1, rh, border_color);
                gal_draw_rect(rx + rw - 1, ry, 1, rh, border_color);
                
                char display_text[512];
                if (strlen(node->value) > 0) {
                    strcpy(display_text, node->value);
                } else {
                    strcpy(display_text, node->text);
                }
                gal_draw_ttf_string_with_selection(rx + 8, ry + (rh - 14) / 2, display_text, node->text_color, 14, node->font_family);

                if (node == focused_node && cursor_visible) {
                    int text_w = text_width_family(display_text, 14, node->font_family);
                    gal_draw_rect(rx + 8 + text_w + 1, ry + (rh - 16) / 2, 2, 16, node->text_color);
                }
            }
            else if (node->type == TAG_SPAN || node->type == TAG_LABEL || node->type == TAG_LI || node->type == TAG_TD) {
                gal_draw_ttf_string_with_selection(rx, ry + (rh - 14) / 2, node->text, node->has_style ? node->text_color : 0x000000, 14, node->font_family);
            }
            else if (node->type == TAG_IMG) {
                if (node->src[0] != '\0') {
                    const char* img_path = get_cached_image(node->src, page_url);
                    if (img_path) {
                        LoadedImage* img = image_load(img_path);
                        if (img) {
                            int dw = rw > 0 ? rw : img->width;
                            int dh = rh > 0 ? rh : img->height;
                            gal_blit_image_scaled(rx, ry, dw, dh, img->pixels, img->width, img->height);
                            image_free(img);
                        } else {
                            gal_draw_rect(rx, ry, rw > 0 ? rw : 200, rh > 0 ? rh : 30, 0xEEEEEE);
                        }
                    } else {
                        gal_draw_rect(rx, ry, rw > 0 ? rw : 200, rh > 0 ? rh : 30, 0xEEEEEE);
                    }
                }
            }
            node = node->next;
            continue;
        }

        if (node->type == TAG_IMG) {
            if (current_x > 50 - scroll_x) { current_y += max_line_height + 20; current_x = 50 - scroll_x; }
            current_y += 10;

            if (node->src[0] != '\0') {
                const char* img_path = get_cached_image(node->src, page_url);
                if (img_path) {
                    LoadedImage* img = image_load(img_path);
                    if (img) {
                        int draw_w = img->width;
                        int draw_h = img->height;
                        int max_w = SCREEN_WIDTH - 100;
                        if (draw_w > max_w) {
                            float ratio = (float)max_w / draw_w;
                            draw_w = max_w;
                            draw_h = (int)(draw_h * ratio);
                        }

                        if (draw_w != img->width || draw_h != img->height) {
                            gal_blit_image_scaled(50 - scroll_x, current_y, draw_w, draw_h, img->pixels, img->width, img->height);
                        } else {
                            gal_blit_image(50 - scroll_x, current_y, draw_w, draw_h, img->pixels);
                        }

                        track_bounds(50 - scroll_x, current_y, draw_w, draw_h, scroll_x, scroll_y);
                        current_y += draw_h + 10;
                        image_free(img);
                    } else {
                        gal_draw_rect(50 - scroll_x, current_y, 200, 30, 0xEEEEEE);
                        gal_draw_ttf_string(55 - scroll_x, current_y + 5, "[Image not found]", 0xFF0000, 14);
                        track_bounds(50 - scroll_x, current_y, 200, 30, scroll_x, scroll_y);
                        current_y += 40;
                    }
                } else {
                    gal_draw_rect(50 - scroll_x, current_y, 200, 30, 0xEEEEEE);
                    gal_draw_ttf_string(55 - scroll_x, current_y + 5, "[Image failed]", 0xFF0000, 14);
                    track_bounds(50 - scroll_x, current_y, 200, 30, scroll_x, scroll_y);
                    current_y += 40;
                }
            }
            current_x = 50;
            max_line_height = 0;
            node = node->next;
            continue;
        }

        if (node->type == TAG_HEAD || node->type == TAG_STYLE || node->type == TAG_SCRIPT ||
            node->type == TAG_TITLE || node->type == TAG_META || node->type == TAG_LINK) {
            node = node->next;
            continue;
        }

        if (strlen(node->text) == 0 && 
            node->type != TAG_DIV && 
            node->type != TAG_MAIN &&
            node->type != TAG_SECTION &&
            node->type != TAG_ARTICLE &&
            node->type != TAG_HEADER &&
            node->type != TAG_FOOTER &&
            node->type != TAG_NAV &&
            node->type != TAG_ASIDE &&
            node->type != TAG_FORM &&
            node->type != TAG_BUTTON &&
            node->type != TAG_INPUT &&
            node->type != TAG_TEXTAREA &&
            node->type != TAG_TABLE &&
            node->type != TAG_BR &&
            node->type != TAG_HR) {
            node = node->next;
            continue;
        }

        if (node->type == TAG_H) {
            if (current_x > 50 - scroll_x) { current_y += max_line_height + 20; current_x = 50 - scroll_x; }
            current_y += 20;

            int tw = text_width_family(node->text, 32, node->font_family);
            int tx = (node->is_centered || context_centered) ? (SCREEN_WIDTH - tw) / 2 - scroll_x : (50 - scroll_x);

            node->hit_x = tx;
            node->hit_y = current_y;
            node->hit_w = tw;
            node->hit_h = 40;

            uint32_t hcol = node->has_style ? node->text_color : inherited_text_color;
            gal_draw_ttf_string_with_selection(tx, current_y, node->text, hcol, 32, node->font_family);
            track_bounds(tx, current_y, node->hit_w, 40, scroll_x, scroll_y);
            current_y += 45; current_x = 50 - scroll_x; max_line_height = 0;
        } 
        else if (node->type == TAG_P) {
            if (current_x > 50 - scroll_x) { current_y += max_line_height + 20; current_x = 50 - scroll_x; }

            int tw = text_width_family(node->text, 20, node->font_family);
            int px = (node->is_centered || context_centered) ? (SCREEN_WIDTH - tw) / 2 - scroll_x : (50 - scroll_x);

            node->hit_x = px;
            node->hit_y = current_y;
            node->hit_w = tw;
            node->hit_h = 28;

            uint32_t pcol = node->has_style ? node->text_color : inherited_text_color;
            gal_draw_ttf_string_with_selection(px, current_y, node->text, pcol, 20, node->font_family);
            track_bounds(px, current_y, node->hit_w, 28, scroll_x, scroll_y);
            current_y += 30; current_x = 50 - scroll_x; max_line_height = 0;
        } 
        else if (node->type == TAG_SPAN || node->type == TAG_LABEL || node->type == TAG_LI ||
                 node->type == TAG_OPTION ||
                 node->type == TAG_LEGEND || node->type == TAG_SOURCE || node->type == TAG_SVG ||
                 node->type == TAG_PICTURE || node->type == TAG_IFRAME) {
            /* font-size hint: <font size="3"> maps roughly to: 1->10,2->13,3->16,4->18,5->24,6->32,7->48 */
            int fs = context_font_size;
            if (node->has_style && node->font_family[0] != '\0') {
                /* size attribute stored in cols for SPAN from <font size=> */
            }
            int string_width = text_width_family(node->text, fs, node->font_family);

            int span_x;
            if (context_centered) {
                /* Center the span on the screen */
                if (current_x > 50 - scroll_x) { current_y += max_line_height + fs + 2; current_x = 50 - scroll_x; max_line_height = 0; }
                span_x = (SCREEN_WIDTH - string_width) / 2 - scroll_x;
            } else {
                if (current_x + string_width > SCREEN_WIDTH - 50) {
                    current_x = 50 - scroll_x;
                    current_y += max_line_height + 15;
                    max_line_height = 0;
                }
                span_x = current_x;
            }

            node->hit_x = span_x;
            node->hit_y = current_y;
            node->hit_w = string_width;
            node->hit_h = fs + 8;

            uint32_t scol = node->has_style ? node->text_color : inherited_text_color;
            gal_draw_ttf_string_with_selection(span_x, current_y, node->text, scol, fs, node->font_family);
            track_bounds(span_x, current_y, string_width, fs + 8, scroll_x, scroll_y);

            if (!context_centered) {
                current_x += string_width + 10;
            }
            if (fs + 8 > max_line_height) max_line_height = fs + 8;
        }
        else if (node->type == TAG_A) {
            int string_width = text_width_family(node->text, 20, node->font_family);

            int ax;
            if (context_centered) {
                if (current_x > 50 - scroll_x) { current_y += max_line_height + 25; current_x = 50 - scroll_x; max_line_height = 0; }
                ax = (SCREEN_WIDTH - string_width) / 2 - scroll_x;
            } else {
                if (current_x + string_width > SCREEN_WIDTH - 50) {
                    current_x = 50 - scroll_x;
                    current_y += max_line_height + 15;
                    max_line_height = 0;
                }
                ax = current_x;
            }

            node->hit_x = ax;
            node->hit_y = current_y;
            node->hit_w = string_width;
            node->hit_h = 24;

            uint32_t acol = node->has_style ? node->text_color : (inherited_text_color == 0x111111 ? 0x0000FF : inherited_text_color);
            gal_draw_ttf_string_with_selection(ax, current_y, node->text, acol, 20, node->font_family);
            gal_draw_rect(ax, current_y + 22, string_width, 2, acol);
            track_bounds(ax, current_y, string_width, 24, scroll_x, scroll_y);

            if (!context_centered) current_x += string_width + 30;
            if (30 > max_line_height) max_line_height = 30;
        }
        else if (node->type == TAG_DIV || node->type == TAG_MAIN || node->type == TAG_SECTION ||
                 node->type == TAG_ARTICLE || node->type == TAG_HEADER || node->type == TAG_FOOTER ||
                 node->type == TAG_NAV || node->type == TAG_ASIDE || node->type == TAG_FORM ||
                 node->type == TAG_TABLE || node->type == TAG_UL || node->type == TAG_OL ||
                 node->type == TAG_HTML || node->type == TAG_FIELDSET ||
                 node->type == TAG_NOSCRIPT || node->type == TAG_VIDEO || node->type == TAG_AUDIO ||
                 node->type == TAG_CANVAS || node->type == TAG_SELECT) {
            if (current_x > 50 - scroll_x) { current_y += max_line_height + 20; current_x = 50 - scroll_x; }

            /* TABLE with width="100%" fills the viewport */
            int rw = node->width > 0 ? node->width : (SCREEN_WIDTH - 100);
            if (node->type == TAG_TABLE && node->width == 0) rw = SCREEN_WIDTH - 20;
            int rh = node->height > 0 ? node->height : (strlen(node->text) > 0 ? 40 : 10);

            int rx = (node->is_centered || context_centered) ? (SCREEN_WIDTH - rw) / 2 - scroll_x : (50 - scroll_x);

            /* For TABLE: paint bg_color of page (inherited) */
            if (node->type == TAG_TABLE) {
                if (node->has_style) {
                    gal_draw_rect(rx, current_y, rw, rh, node->bg_color);
                } /* else transparent: don't overdraw page background */
                track_bounds(rx, current_y, rw, rh, scroll_x, scroll_y);
                /* don't advance current_y for table — children handle it */
                node = node->next;
                continue;
            }

            node->hit_x = rx;
            node->hit_y = current_y;
            node->hit_w = rw;
            node->hit_h = rh;

            gal_draw_rect(rx, current_y, rw, rh, node->has_style ? node->bg_color : inherited_bg_color);
            if (strlen(node->text) > 0) {
                uint32_t dcol = node->has_style ? node->text_color : inherited_text_color;
                gal_draw_ttf_string(rx + 5, current_y + 5, node->text, dcol, 18);
            }
            track_bounds(rx, current_y, rw, rh, scroll_x, scroll_y);
            current_y += rh + 5;
            current_x = 50 - scroll_x;
            max_line_height = 0;
        }
        else if (node->type == TAG_BR) {
            current_y += max_line_height > 0 ? max_line_height : 20;
            current_x = 50 - scroll_x;
            max_line_height = 0;
        }
        else if (node->type == TAG_HR) {
            if (current_x > 50 - scroll_x) { current_y += max_line_height + 10; current_x = 50 - scroll_x; }
            uint32_t hr_col = inherited_text_color == 0x111111 ? 0xCCCCCC : inherited_text_color;
            gal_draw_rect(50 - scroll_x, current_y + 5, SCREEN_WIDTH - 100, 2, hr_col);
            current_y += 15;
            current_x = 50 - scroll_x;
            max_line_height = 0;
        }
        else if (node->type == TAG_BUTTON) {
            int rw = node->width > 0 ? node->width : 100;
            int rh = node->height > 0 ? node->height : 30;
            if (current_x + rw > SCREEN_WIDTH - 50) {
                current_x = 50 - scroll_x;
                current_y += max_line_height + 15;
                max_line_height = 0;
            }
            node->hit_x = current_x;
            node->hit_y = current_y;
            node->hit_w = rw;
            node->hit_h = rh;
            
            gal_draw_rect(current_x, current_y, rw, rh, node->bg_color);
            uint32_t border_color = 0x4B5563;
            gal_draw_rect(current_x, current_y, rw, 1, border_color);
            gal_draw_rect(current_x, current_y + rh - 1, rw, 1, border_color);
            gal_draw_rect(current_x, current_y, 1, rh, border_color);
            gal_draw_rect(current_x + rw - 1, current_y, 1, rh, border_color);
            
            int tw = text_width_family(node->text, 14, node->font_family);
            int tx = current_x + (rw - tw) / 2;
            int ty = current_y + (rh - 14) / 2;
            gal_draw_ttf_string_with_selection(tx, ty, node->text, node->text_color, 14, node->font_family);
            track_bounds(current_x, current_y, rw, rh, scroll_x, scroll_y);
            
            current_x += rw + 15;
            if (rh > max_line_height) max_line_height = rh;
        }
        else if (node->type == TAG_INPUT || node->type == TAG_TEXTAREA) {
            int is_textarea = (node->type == TAG_TEXTAREA);

            /* Compute dimensions: prefer explicit width/height, then rows/cols, then default */
            int char_w = gal_ttf_text_width("W", 14);  /* approx monospace char width */
            int line_h = 17;                             /* line height in pixels */
            int rw, rh;
            if (node->width > 0) {
                rw = node->width;
            } else if (is_textarea && node->cols > 0) {
                rw = node->cols * char_w + 16;
            } else {
                rw = is_textarea ? 400 : 200;
            }
            if (node->height > 0) {
                rh = node->height;
            } else if (is_textarea && node->rows > 0) {
                rh = node->rows * line_h + 10;
            } else {
                rh = is_textarea ? 200 : 30;
            }

            /* Centering: explicit or from context */
            int do_center = node->is_centered || context_centered;
            int rx = do_center ? (SCREEN_WIDTH - rw) / 2 - scroll_x : current_x;

            if (!do_center && rx + rw > SCREEN_WIDTH - 50) {
                rx = 50 - scroll_x;
                current_y += max_line_height + 15;
                max_line_height = 0;
            }
            node->hit_x = rx;
            node->hit_y = current_y;
            node->hit_w = rw;
            node->hit_h = rh;

            uint32_t inp_bg  = node->has_style ? node->bg_color  : 0xFFFFFF;
            uint32_t inp_col = node->has_style ? node->text_color : inherited_text_color;
            gal_draw_rect(rx, current_y, rw, rh, inp_bg);
            uint32_t border_color = node->has_style ? inp_col : 0x9CA3AF;

            gal_draw_rect(rx, current_y, rw, 1, border_color);
            gal_draw_rect(rx, current_y + rh - 1, rw, 1, border_color);
            gal_draw_rect(rx, current_y, 1, rh, border_color);
            gal_draw_rect(rx + rw - 1, current_y, 1, rh, border_color);

            char display_text[2048];
            if (strlen(node->value) > 0) {
                strncpy(display_text, node->value, sizeof(display_text) - 1);
                display_text[sizeof(display_text) - 1] = '\0';
            } else {
                strncpy(display_text, node->text, sizeof(display_text) - 1);
                display_text[sizeof(display_text) - 1] = '\0';
            }

            /* Choose monospace font for textarea */
            const char* txt_font = is_textarea ? "monospace" : node->font_family;
            if (is_textarea && node->font_family[0] != '\0') txt_font = node->font_family;

            if (is_textarea) {
                /* Multiline rendering */
                int line_y2 = current_y + 5;
                char tmp[2048];
                strncpy(tmp, display_text, sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                char* line = strtok(tmp, "\n");
                while (line && line_y2 + 14 < current_y + rh) {
                    gal_draw_ttf_string_with_selection(rx + 8, line_y2, line, inp_col, 14, txt_font);
                    line_y2 += 17;
                    line = strtok(NULL, "\n");
                }
            } else {
                gal_draw_ttf_string_with_selection(rx + 8, current_y + (rh - 14) / 2, display_text, inp_col, 14, node->font_family);

                if (node == focused_node && cursor_visible) {
                    int text_w = text_width_family(display_text, 14, node->font_family);
                    gal_draw_rect(rx + 8 + text_w + 1, current_y + (rh - 16) / 2, 2, 16, inp_col);
                }
            }

            track_bounds(rx, current_y, rw, rh, scroll_x, scroll_y);

            if (!do_center) {
                current_x = rx + rw + 15;
            } else {
                current_y += rh + 15;
                current_x = 50 - scroll_x;
            }
            if (rh > max_line_height) max_line_height = rh;
        }
        
        node = node->next;
    }

    track_bounds(0, current_y, current_x + scroll_x, max_line_height, scroll_x, scroll_y);
}
