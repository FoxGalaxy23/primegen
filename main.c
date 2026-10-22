#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <signal.h>
#endif
#include "gal.h"
#include "nal.h"

#ifdef HAVE_RENDER
  #include "parser.h"
#include "layout.h"
#include "css_engine.h"
#include "webpage.h"
#endif

#ifdef HAVE_UI
  #include "ui.h"
#include "net.h"
#include "js_engine.h"
#include "db.h"
#include "font_manager.h"
#endif

#ifdef HAVE_LOGGER
  #include "logger.h"
#endif

// Timer system for JS setInterval
typedef struct {
    int id;
    long last_run_ms;
    int delay_ms;
    char code[512];
    int active;
} JSInterval;

#define MAX_JS_INTERVALS 16
JSInterval g_intervals[MAX_JS_INTERVALS];

static long get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Global state variables
int anonymous_mode = 0;
int module_db_disabled = 0;
int module_logger_disabled = 0;
int module_net_disabled = 0;
int module_js_disabled = 0;
int needs_redraw = 1;
long last_cursor_toggle = 0;

// Variables needed by modules
int menu_open = 0;
char input_buffer[512] = {0};
int url_bar_focused = 0;
DOMNode* focused_node = NULL;
int cursor_visible = 0;
int global_history_count = 0;
char* nav_bar_template = NULL;

#ifdef HAVE_UI
BrowserTab tabs[MAX_TABS];
int tab_count = 0;
int active_tab_idx = -1;

// UI specific variables
int loading_progress = 0;
int page_scroll_x = 0;
int page_scroll_y = 0;
int page_content_width = 0;
int page_content_height = 0;
int selecting_text = 0;
int select_start_x = 0;
int select_start_y = 0;
char global_history[MAX_GLOBAL_HISTORY][256] = {{0}};
JSRuntime* global_js_runtime = NULL;

int context_menu_open = 0;
int context_menu_x = 0;
int context_menu_y = 0;
char internal_clipboard[1024] = {0};

static void browser_dom_set_value_callback(void* user_data, const char* path, const char* value) {
    (void)user_data;
    if (active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree) {
        char elem_name[64] = {0};
        const char* dot1 = strchr(path, '.');
        if (dot1) {
            const char* dot2 = strchr(dot1 + 1, '.');
            if (dot2) {
                size_t len2 = dot2 - (dot1 + 1);
                if (len2 >= 64) len2 = 63;
                memcpy(elem_name, dot1 + 1, len2);
                elem_name[len2] = '\0';
            }
        }

        DOMNode* curr = (DOMNode*)tabs[active_tab_idx].dom_tree;
        while (curr) {
            if (strcmp(curr->id, elem_name) == 0 || (curr->type == TAG_TEXTAREA && elem_name[0] && strstr(path, "display"))) {
                strncpy(curr->value, value, sizeof(curr->value) - 1);
                curr->value[sizeof(curr->value) - 1] = '\0';
                needs_redraw = 1;
                return;
            }
            curr = curr->next;
        }
    }
}
#endif

// Функция для загрузки стартовой страницы из assets
char* read_start_page() {
    const char* filename = anonymous_mode ? "assets/incognito_start_page.html" : "assets/start_page.html";
    FILE* f = fopen(filename, "r");
    if (!f) {
        if (anonymous_mode) {
             return strdup("<div style=\"background-color: #1F2937; color: #FFFFFF;\"><h>Incognito</h><p>Private browsing mode.</p></div>");
        }
        // Фолбек на случай, если файла нет в assets
        return strdup("<h>Meetlookers</h><p>Error: assets/start_page.html not found.</p>");
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = malloc(size + 1);
    if (buffer) {
        size_t read_bytes = fread(buffer, 1, size, f);
        buffer[read_bytes] = '\0';
    }
    fclose(f);
    return buffer;
}

#ifdef HAVE_UI
static int rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static void copy_selected_text(DOMNode* head) {
    if (!layout_selection_active) return;
    int offset = strlen(internal_clipboard);
    if (head && head->v2) { webpage_copy_selection(internal_clipboard + offset, 1023 - offset); return; }

    int sx = layout_selection_x1 < layout_selection_x2 ? layout_selection_x1 : layout_selection_x2;
    int sy = layout_selection_y1 < layout_selection_y2 ? layout_selection_y1 : layout_selection_y2;
    int sw = abs(layout_selection_x2 - layout_selection_x1);
    int sh = abs(layout_selection_y2 - layout_selection_y1);

    DOMNode* curr = head;
    while (curr) {
        if (curr->hit_w > 0 && curr->hit_h > 0) {
            if (rects_intersect(curr->hit_x, curr->hit_y, curr->hit_w, curr->hit_h, sx, sy, sw, sh)) {
                const char* txt = (curr->type == TAG_INPUT || curr->type == TAG_TEXTAREA) ? curr->value : curr->text;
                if (strlen(txt) > 0) {
                    int char_x = curr->hit_x;
                    const char* p = txt;
                    int font_size = 20;
                    if (curr->type == TAG_H) font_size = 32;
                    else if (curr->type == TAG_INPUT || curr->type == TAG_BUTTON) font_size = 14;

                    while (*p) {
                        char char_buf[8];
                        int char_len = 1;
                        unsigned char c = (unsigned char)*p;
                        if ((c & 0x80) == 0) char_len = 1;
                        else if ((c & 0xE0) == 0xC0) char_len = 2;
                        else if ((c & 0xF0) == 0xE0) char_len = 3;
                        else if ((c & 0xF8) == 0xF0) char_len = 4;
                        memcpy(char_buf, p, char_len);
                        char_buf[char_len] = '\0';
                        int cw = gal_ttf_text_width(char_buf, font_size);

                        if (rects_intersect(char_x, curr->hit_y, cw, font_size + 4, sx, sy, sw, sh)) {
                             if (offset + char_len < 1023) {
                                 memcpy(internal_clipboard + offset, p, char_len);
                                 offset += char_len;
                                 internal_clipboard[offset] = '\0';
                             }
                        }
                        char_x += cw;
                        p += char_len;
                    }
                }
            }
        }
        curr = curr->next;
    }
}

static void repaint_browser_chrome(void) {
    if (!nav_bar_template) return;
    gal_clear(0xFFFFFF);
    draw_browser_frame(tabs, tab_count, active_tab_idx);
    gal_flush();
}

static void resolve_page_url(const char* page_url, const char* raw, char* out, int out_size) {
    out[0] = '\0';
    if (!raw || raw[0] == '\0' || out_size <= 0) return;
    if (strncmp(raw, "http://", 7) == 0 || strncmp(raw, "https://", 8) == 0) {
        strncpy(out, raw, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    if (!page_url || strncmp(page_url, "http://", 7) != 0) {
        strncpy(out, raw, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    const char* host_start = page_url + 7;
    const char* slash = strchr(host_start, '/');
    char origin[256] = {0};
    if (slash) {
        int len = (int)(slash - page_url);
        if (len > 255) len = 255;
        strncpy(origin, page_url, len);
        origin[len] = '\0';
    } else {
        strncpy(origin, page_url, sizeof(origin) - 1);
    }

    if (raw[0] == '/') {
        snprintf(out, out_size, "%s%s", origin, raw);
    } else {
        snprintf(out, out_size, "%s/%s", origin, raw);
    }
}

#ifdef HAVE_RENDER
/* Создаёт JS-страницу для только что загруженного DOM вкладки и исполняет скрипты. */
static void attach_js(int index, const char* url, PageBundle* pb) {
    DOMNode* d = (DOMNode*)tabs[index].dom_tree;
    if (!d || !d->v2) { webpage_bundle_free(pb); return; }
    tabs[index].js_page = webpage_js_attach(d, (pb && pb->base[0]) ? pb->base : url, pb);
}
#endif

void load_url_in_tab(int index, const char* target_url, int record_history) {
    strncpy(tabs[index].url, target_url, 255);
    tabs[index].url[255] = '\0';
    strncpy(tabs[index].title, target_url, sizeof(tabs[index].title) - 1);
    tabs[index].title[sizeof(tabs[index].title) - 1] = '\0';
    tabs[index].favicon_url[0] = '\0';
    tabs[index].favicon_path[0] = '\0';
    tabs[index].is_hung = 0;
    loading_progress = 15;
    repaint_browser_chrome();

    if (record_history && !anonymous_mode) {
        db_add_history_entry(target_url, target_url);
    }

#ifdef HAVE_RENDER
    if (tabs[index].js_page) { webpage_js_free(tabs[index].js_page); tabs[index].js_page = NULL; }
    if (tabs[index].dom_tree) {
        free_dom((DOMNode*)tabs[index].dom_tree);
        tabs[index].dom_tree = NULL;
    }

    if (strncmp(target_url, "http://", 7) == 0 || strncmp(target_url, "https://", 8) == 0) {
        tabs[index].is_loading = 1;
        tabs[index].loading_start_time = (long)time(NULL);

#ifndef _WIN32
        pid_t pid = fork();
        if (pid == 0) {
            // Child process: fetch and parse
            char* page_source = network_http_get(target_url);
            if (page_source) {
                DOMNode* tree = parse_document(page_source, target_url);
                webpage_bundle_prefetch_css();               /* качаем внешние <link rel=stylesheet> ЗДЕСЬ, в фоне, вместе с HTML —
                                                                  а не позже синхронно в UI-потоке при каскаде (см. style.c) */
                char cache_name[256];
                snprintf(cache_name, sizeof(cache_name), "cache/tab_%d.dom", index);
                serialize_dom(tree, cache_name);
                { char bundle_name[256]; snprintf(bundle_name, sizeof(bundle_name), "cache/tab_%d.bundle", index); webpage_bundle_write(bundle_name); }

                // Also save title if possible (simple way: append to another file or use a header)
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "cache/tab_%d.meta", index);
                FILE* mf = fopen(meta_name, "w");
                if (mf) {
                    char title[128] = {0};
                    char icon_href[256] = {0};
                    extract_html_title(page_source, title, sizeof(title));
                    extract_html_icon_href(page_source, icon_href, sizeof(icon_href));
                    fprintf(mf, "%s\n%s\n", title, icon_href);
                    fclose(mf);
                }

                free_dom(tree);
                free(page_source);
            }
            _exit(0);
        } else if (pid > 0) {
            tabs[index].loading_pid = pid;
            return; // Return to main loop, don't block
        }
#else
        // Windows: for now, just load synchronously to avoid complex process/thread handling
        char* page_source = network_http_get(target_url);
        if (page_source) {
            if (tabs[index].dom_tree) free_dom((DOMNode*)tabs[index].dom_tree);
            tabs[index].dom_tree = parse_document(page_source, target_url);
            attach_js(index, target_url, webpage_bundle_take());
            free(page_source);
        }
        tabs[index].is_loading = 0;
#endif
    } else if (strncmp(target_url, "browser://", 10) == 0) {
        char* content = NULL;
        if (strcmp(target_url, "browser://history") == 0) {
            content = db_generate_history_html();
        } else if (strcmp(target_url, "browser://settings") == 0) {
            content = db_generate_profile_html();
        } else if (strcmp(target_url, "browser://downloads") == 0) {
            content = db_generate_downloads_html();
        }

        if (content) {
            tabs[index].dom_tree = parse_html(content);
            if (tabs[index].dom_tree) {
                css_load_and_apply((DOMNode*)tabs[index].dom_tree, "assets/style.css");
            }
            free(content);
        }
    } else if (strncmp(target_url, "https://", 8) == 0) {
        // HTTPS not supported - show error
        tabs[index].dom_tree = parse_html(
            "<h>Ошибка соединения</h>"
            "<p>HTTPS не поддерживается.</p>"
            "<p>Используйте http:// вместо https://</p>"
        );
    } else if (target_url[0] == '/' || (target_url[1] == ':' && target_url[2] == '\\')) {
        // Local file
        FILE* f = fopen(target_url, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            char* buf = malloc(sz + 1);
            if (buf) {
                fread(buf, 1, sz, f);
                buf[sz] = '\0';
                tabs[index].dom_tree = parse_document(buf, target_url);
                attach_js(index, target_url, webpage_bundle_take());
                free(buf);
            }
            fclose(f);
        } else {
             tabs[index].dom_tree = parse_html("<h1>File not found</h1>");
        }
    } else {
        // Local home or other
        char* start_html = read_start_page();
        if (start_html) {
            tabs[index].dom_tree = parse_document(start_html, "localhost/home");
            attach_js(index, target_url, webpage_bundle_take());
            free(start_html);
        }
    }
    loading_progress = 0;
#endif

    // Execute scripts in the loaded DOM
    if (tabs[index].dom_tree && global_js_runtime) {
        DOMNode* curr = (DOMNode*)tabs[index].dom_tree;
        while (curr) {
            if (curr->type == TAG_SCRIPT && !curr->v2 && strlen(curr->text) > 0) {
                gal_log("INFO", "Auto-executing <script>...");
                js_eval(global_js_runtime, curr->text);
            }
            curr = curr->next;
        }
    }

    // Manage history
    if (record_history) {
        BrowserTab* tab = &tabs[index];
        if (tab->history_index < 0 || strcmp(tab->history[tab->history_index].url, target_url) != 0) {
            int next_idx = tab->history_index + 1;
            if (next_idx < MAX_HISTORY) {
                strncpy(tab->history[next_idx].url, target_url, 255);
                tab->history[next_idx].url[255] = '\0';
                tab->history_index = next_idx;
                tab->history_count = next_idx + 1;
            }
        }
        if (!anonymous_mode) {
            if (global_history_count == 0 || strcmp(global_history[global_history_count - 1], target_url) != 0) {
                if (global_history_count < MAX_GLOBAL_HISTORY) {
                    strncpy(global_history[global_history_count], target_url, 255);
                    global_history[global_history_count][255] = '\0';
                    global_history_count++;
                } else {
                    for (int i = 1; i < MAX_GLOBAL_HISTORY; i++) {
                        strcpy(global_history[i - 1], global_history[i]);
                    }
                    strncpy(global_history[MAX_GLOBAL_HISTORY - 1], target_url, 255);
                    global_history[MAX_GLOBAL_HISTORY - 1][255] = '\0';
                }
            }
        }
    }
}

static void url_encode(const char* src, char* dest, int dest_size) {
    static const char* hex = "0123456789ABCDEF";
    int d = 0;
    for (int i = 0; src[i] && d < dest_size - 4; i++) {
        unsigned char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c)) {
            dest[d++] = c;
        } else {
            dest[d++] = '%';
            dest[d++] = hex[c >> 4];
            dest[d++] = hex[c & 15];
        }
    }
    dest[d] = '\0';
}

void clean_and_load_url(int index, const char* target_url, int record_history) {
    if (!target_url) return;
    
    char buf[512];
    strncpy(buf, target_url, 511);
    buf[511] = '\0';

    // Trim
    char* start = buf;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;

    int len = strlen(start);
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '\r' || start[len - 1] == '\n')) {
        start[--len] = '\0';
    }

    // Check for local home page variants
    if (strstr(start, "localhost/home") ||
        strcmp(start, "home") == 0 ||
        len == 0) {
        load_url_in_tab(index, "localhost/home", record_history);
    } else if (strncmp(start, "file://", 7) == 0) {
        load_url_in_tab(index, start + 7, record_history);
    } else if (strncmp(start, "http", 4) != 0 && strchr(start, '/') && strchr(start, '.') && (start[0] == '.' || access(start, R_OK) == 0)) {
        /* существующий локальный файл по относительному пути: ./my_browser examples/js-demo/index.html */
        char abs_path[4096];                  /* realpath требует буфер не меньше PATH_MAX */
#ifndef _WIN32
        if (realpath(start, abs_path)) load_url_in_tab(index, abs_path, record_history); else load_url_in_tab(index, start, record_history);
#else
        snprintf(abs_path, sizeof(abs_path), "%s", start); load_url_in_tab(index, abs_path, record_history);
#endif
    } else if (start[0] == '/' || (start[1] == ':' && start[2] == '\\')) {
        load_url_in_tab(index, start, record_history);
    } else {
        if (strncmp(start, "http://", 7) != 0 && strncmp(start, "https://", 8) != 0 && strncmp(start, "browser://", 10) != 0) {
            // Is it a domain or a search query?
            int is_search = 0;
            if (strchr(start, ' ')) is_search = 1;
            else if (!strchr(start, '.')) is_search = 1;

            if (is_search) {
                char encoded[512];
                url_encode(start, encoded, sizeof(encoded));
                char search_url[1024];
                snprintf(search_url, sizeof(search_url), "http://www.google.com/search?q=%s", encoded);
                load_url_in_tab(index, search_url, record_history);
            } else {
                char formatted[512];
                snprintf(formatted, sizeof(formatted), "http://%s", start);
                load_url_in_tab(index, formatted, record_history);
            }
        } else {
            load_url_in_tab(index, start, record_history);
        }
    }
    page_scroll_x = 0;
    page_scroll_y = 0;
    needs_redraw = 1;
}

static void input_backspace_utf8(char* text) {
    int len = strlen(text);
    if (len <= 0) return;
    len--;
    while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) {
        len--;
    }
    text[len] = '\0';
}

static void clamp_page_scroll(int top_offset) {
    if (page_scroll_x < 0) page_scroll_x = 0;
    if (page_scroll_y < 0) page_scroll_y = 0;

    int max_x = page_content_width - SCREEN_WIDTH + 20;
    int max_y = page_content_height - SCREEN_HEIGHT + top_offset + 20;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (page_scroll_x > max_x) page_scroll_x = max_x;
    if (page_scroll_y > max_y) page_scroll_y = max_y;
}

static void draw_scrollbars(int top_offset) {
    int viewport_w = SCREEN_WIDTH;
    int viewport_h = SCREEN_HEIGHT - top_offset;
    int content_w = page_content_width > viewport_w ? page_content_width : viewport_w;
    int content_h = page_content_height > SCREEN_HEIGHT ? page_content_height : SCREEN_HEIGHT;

    if (content_h > viewport_h + top_offset) {
        int track_h = viewport_h - 12;
        int thumb_h = viewport_h * track_h / content_h;
        if (thumb_h < 24) thumb_h = 24;
        int max_scroll = content_h - viewport_h - top_offset;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_y = top_offset + (page_scroll_y * (track_h - thumb_h)) / max_scroll;
        gal_draw_rect(SCREEN_WIDTH - 10, top_offset, 10, track_h, 0xF3F4F6);
        gal_draw_rect(SCREEN_WIDTH - 9, thumb_y, 8, thumb_h, 0x9CA3AF);
    }

    if (content_w > viewport_w) {
        int track_w = viewport_w - 12;
        int thumb_w = viewport_w * track_w / content_w;
        if (thumb_w < 24) thumb_w = 24;
        int max_scroll = content_w - viewport_w;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_x = (page_scroll_x * (track_w - thumb_w)) / max_scroll;
        gal_draw_rect(0, SCREEN_HEIGHT - 10, track_w, 10, 0xF3F4F6);
        gal_draw_rect(thumb_x, SCREEN_HEIGHT - 9, thumb_w, 8, 0x9CA3AF);
    }
}

void create_new_tab() {
    if (tab_count >= MAX_TABS) return;
    active_tab_idx = tab_count;
    BrowserTab* tab = &tabs[active_tab_idx];
    tab->dom_tree = NULL;
    tab->js_page = NULL;
    tab->history_index = -1;
    tab->history_count = 0;
    clean_and_load_url(active_tab_idx, "localhost/home", 1);
    tab_count++;
}

void close_tab(int index) {
    if (index < 0 || index >= tab_count) return;

#ifndef _WIN32
    if (tabs[index].is_loading && tabs[index].loading_pid > 0) {
        kill(tabs[index].loading_pid, SIGKILL);
        waitpid(tabs[index].loading_pid, NULL, 0);
    }
#endif

#ifdef HAVE_RENDER
    if (tabs[index].js_page) { webpage_js_free(tabs[index].js_page); tabs[index].js_page = NULL; }
    if (tabs[index].dom_tree) free_dom((DOMNode*)tabs[index].dom_tree);
#endif
    for (int i = index; i < tab_count - 1; i++) {
        tabs[i] = tabs[i + 1];
    }
    tab_count--;
    if (active_tab_idx >= tab_count) active_tab_idx = tab_count - 1;
}

void execute_onclick(const char* cmd) {
    if (!cmd || strlen(cmd) == 0) return;
    
    gal_log("INFO", "Executing JS action: %s", cmd);
    
    if (strcmp(cmd, "Browser.back()") == 0) {
        if (active_tab_idx >= 0) {
            BrowserTab* tab = &tabs[active_tab_idx];
            if (tab->history_index > 0) {
                tab->history_index--;
                clean_and_load_url(active_tab_idx, tab->history[tab->history_index].url, 0);
            }
        }
    }
    else if (strcmp(cmd, "Browser.forward()") == 0) {
        if (active_tab_idx >= 0) {
            BrowserTab* tab = &tabs[active_tab_idx];
            if (tab->history_index < tab->history_count - 1) {
                tab->history_index++;
                clean_and_load_url(active_tab_idx, tab->history[tab->history_index].url, 0);
            }
        }
    }
    else if (strcmp(cmd, "Browser.reload()") == 0) {
        if (active_tab_idx >= 0) {
            nal_cache_policy(NAL_CACHE_RELOAD);              /* «Обновить» — страница и ресурсы проверяются заново, а не берутся из кэша */
            clean_and_load_url(active_tab_idx, tabs[active_tab_idx].url, 0);
            nal_cache_policy(NAL_CACHE_NORMAL);
        }
    }
    else if (strcmp(cmd, "Browser.newTab()") == 0) {
        create_new_tab();
    }
    else if (strcmp(cmd, "Browser.openHistory()") == 0) {
        if (active_tab_idx >= 0) {
            clean_and_load_url(active_tab_idx, "browser://history", 1);
        }
    }
    else if (strcmp(cmd, "Browser.openSettings()") == 0) {
        if (active_tab_idx >= 0) {
            clean_and_load_url(active_tab_idx, "browser://settings", 1);
        }
    }
    else if (strcmp(cmd, "Browser.openDownloads()") == 0) {
        if (active_tab_idx >= 0) {
            clean_and_load_url(active_tab_idx, "browser://downloads", 1);
        }
    }
    else if (strcmp(cmd, "Browser.toggleMenu()") == 0) {
        menu_open = !menu_open;
    }
    else if (strcmp(cmd, "Browser.toggleIncognito()") == 0) {
        anonymous_mode = !anonymous_mode;
    }
    else if (strcmp(cmd, "Browser.openIncognito()") == 0) {
#ifdef _WIN32
        system("start my_browser.exe --incognito");
#else
        if (fork() == 0) {
            execl("./my_browser", "./my_browser", "--incognito", NULL);
            _exit(1);
        }
#endif
    }
    else if (strncmp(cmd, "Browser.closeTab(", 17) == 0) {
        int idx = atoi(cmd + 17);
        close_tab(idx);
    }
    else if (strncmp(cmd, "Browser.selectTab(", 18) == 0) {
        int idx = atoi(cmd + 18);
        if (idx >= 0 && idx < tab_count) {
            active_tab_idx = idx;
        }
    }
    else if (strncmp(cmd, "Browser.navigateUrl(", 20) == 0) {
        char url_to_go[256] = {0};
        const char* start = strchr(cmd, '\'');
        if (!start) start = strchr(cmd, '\"');
        if (start) {
            start++;
            const char* end = strchr(start, '\'');
            if (!end) end = strchr(start, '\"');
            if (end) {
                int len = end - start;
                if (len > 255) len = 255;
                strncpy(url_to_go, start, len);
                url_to_go[len] = '\0';
            }
        }
        if (active_tab_idx >= 0 && strlen(url_to_go) > 0) {
            clean_and_load_url(active_tab_idx, url_to_go, 1);
            menu_open = 0; // Close menu on navigation
        }
    }
    else {
        // Если это не системная команда Browser.*, пробуем выполнить как JS
        if (global_js_runtime) {
            js_eval(global_js_runtime, cmd);
        }
    }
}

void load_nav_template() {
    FILE* f = fopen("assets/nav_bar.html", "r");
    if (!f) {
        nav_bar_template = strdup(
            "<div id=\"bg\" style=\"position: absolute; left: 0; top: 0; width: 1024; height: 80; background-color: #E5E7EB;\"></div>"
            "<button id=\"btn-back\" style=\"position: absolute; left: 10; top: 10; width: 45; height: 30; background-color: #9CA3AF; color: #FFFFFF;\" onclick=\"Browser.back()\">Back</button>"
            "<button id=\"btn-forward\" style=\"position: absolute; left: 60; top: 10; width: 45; height: 30; background-color: #9CA3AF; color: #FFFFFF;\" onclick=\"Browser.forward()\">Fwd</button>"
            "<button id=\"btn-reload\" style=\"position: absolute; left: 110; top: 10; width: 65; height: 30; background-color: #9CA3AF; color: #FFFFFF;\" onclick=\"Browser.reload()\">Reload</button>"
            "<input id=\"url-bar\" style=\"position: absolute; left: 185; top: 10; width: 680; height: 30; background-color: #FFFFFF; color: #111111;\" value=\"\">"
            "<button id=\"btn-menu\" style=\"position: absolute; left: 875; top: 10; width: 35; height: 30; background-color: #9CA3AF; color: #FFFFFF;\" onclick=\"Browser.toggleMenu()\">...</button>"
            "<div id=\"tabs-container\" style=\"position: absolute; left: 0; top: 45; width: 1024; height: 35; background-color: #D1D5DB;\"></div>"
        );
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    nav_bar_template = malloc(size + 1);
    size_t read_bytes = fread(nav_bar_template, 1, size, f);
    nav_bar_template[read_bytes] = '\0';
    fclose(f);
}

DOMNode* find_clicked_node(DOMNode* head, int mx, int my) {
    if (head && head->v2) return webpage_hit_test(head, mx, my);
    DOMNode* matched = NULL;
    DOMNode* curr = head;
    while (curr != NULL) {
        if (curr->hit_w > 0 && curr->hit_h > 0) {
            if (mx >= curr->hit_x && mx <= (curr->hit_x + curr->hit_w) &&
                my >= curr->hit_y && my <= (curr->hit_y + curr->hit_h)) {
                if (curr->type == TAG_INPUT ||
                    curr->type == TAG_BUTTON ||
                    (curr->type == TAG_A && strlen(curr->href) > 0) ||
                    strlen(curr->onclick) > 0) {
                    matched = curr;
                }
            }
        }
        curr = curr->next;
    }
    return matched;
}
#endif

int main(int argc, char** argv) { 
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--incognito") == 0) {
            anonymous_mode = 1;
        }
        else if (strcmp(argv[i], "--disable-db") == 0) {
            module_db_disabled = 1;
        }
        else if (strcmp(argv[i], "--disable-logger") == 0) {
            module_logger_disabled = 1;
        }
        else if (strcmp(argv[i], "--disable-net") == 0) {
            module_net_disabled = 1;
        }
        else if (strcmp(argv[i], "--disable-js") == 0) {
            module_js_disabled = 1;
        }
    }
#ifdef HAVE_LOGGER
    if (!module_logger_disabled) {
        logger_init_crash_handler();
    }
#endif
    if (!module_logger_disabled) {
        gal_log("INFO", "Браузер запускается...");
    }

    if (!gal_init()) {
        if (!module_logger_disabled) {
            gal_log("ERROR", "gal_init() не удался — нет X11 дисплея?");
        }
        return 1;
    }

    if (!module_net_disabled) {
        if (!nal_init()) {
            if (!module_logger_disabled) {
                gal_log("ERROR", "nal_init() не удался — сетевая подсистема не инициализирована");
            }
        }
    }

#ifdef HAVE_UI
    if (!module_db_disabled) db_init();
    font_manager_init();
    load_nav_template();
    create_new_tab();
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') { clean_and_load_url(active_tab_idx, argv[i], 1); break; }   /* ./my_browser <url> */

    if (!module_js_disabled) {
        // Инициализация автономной среды JS
        global_js_runtime = js_create_runtime();
        js_set_dom_callback(global_js_runtime, browser_dom_set_value_callback, NULL);

        FILE* js_file = fopen("assets/script.js", "r");
        if (js_file) {
            fseek(js_file, 0, SEEK_END);
            long js_size = ftell(js_file);
            fseek(js_file, 0, SEEK_SET);
            char* js_code = malloc(js_size + 1);
            fread(js_code, 1, js_size, js_file);
            js_code[js_size] = '\0';
            js_eval(global_js_runtime, js_code);
            free(js_code);
            fclose(js_file);
        }
    }
#else
    DOMNode* standalone_tree = NULL;
    #ifdef HAVE_RENDER
    char* start_html = read_start_page();
    if (start_html) {
        standalone_tree = parse_html(start_html);
        free(start_html);
    }
    #endif
#endif

    int top_offset = 0;
    int running = 1;
    while (running) { static int first=1; if(first){printf("LOOP START\n");fflush(stdout);first=0;}
#ifdef HAVE_UI
        // Update JS intervals from JS variables
        if (global_js_runtime) {
            for (int i = 0; i < global_js_runtime->var_count; i++) {
                if (strncmp(global_js_runtime->vars[i].name, "__interval_", 11) == 0) {
                    int id = atoi(global_js_runtime->vars[i].name + 11);
                    const char* val = global_js_runtime->vars[i].string;

                    // find or create slot
                    int slot = -1;
                    for (int s = 0; s < MAX_JS_INTERVALS; s++) {
                        if (g_intervals[s].id == id) { slot = s; break; }
                        if (slot == -1 && !g_intervals[s].active) slot = s;
                    }

                    if (slot != -1) {
                        if (val[0] == '\0') {
                            g_intervals[slot].active = 0;
                            g_intervals[slot].id = 0;
                        } else if (!g_intervals[slot].active) {
                            g_intervals[slot].id = id;
                            g_intervals[slot].active = 1;
                            g_intervals[slot].last_run_ms = get_time_ms();
                            char* pipe = strchr(val, '|');
                            if (pipe) {
                                g_intervals[slot].delay_ms = atoi(val);
                                strncpy(g_intervals[slot].code, pipe + 1, sizeof(g_intervals[slot].code) - 1);
                            }
                        } else {
                            // already active, maybe update delay/code?
                            char* pipe = strchr(val, '|');
                            if (pipe) {
                                g_intervals[slot].delay_ms = atoi(val);
                                strncpy(g_intervals[slot].code, pipe + 1, sizeof(g_intervals[slot].code) - 1);
                            }
                        }
                    }
                }
            }

            // Run intervals
            long now_ms = get_time_ms();
            for (int s = 0; s < MAX_JS_INTERVALS; s++) {
                if (g_intervals[s].active && (now_ms - g_intervals[s].last_run_ms >= g_intervals[s].delay_ms)) {
                    js_eval(global_js_runtime, g_intervals[s].code);
                    g_intervals[s].last_run_ms = now_ms;
                    needs_redraw = 1;
                }
            }
        }

        // Handle subprocesses and update needs_redraw if finished
#ifndef _WIN32
        for (int i = 0; i < tab_count; i++) {
            if (tabs[i].is_loading) {
                int status;
                pid_t res = waitpid(tabs[i].loading_pid, &status, WNOHANG);
                if (res == tabs[i].loading_pid) {
                    tabs[i].is_loading = 0;
                    tabs[i].loading_pid = 0;
                    tabs[i].is_hung = 0;
                    char cache_name[256];
                    snprintf(cache_name, sizeof(cache_name), "cache/tab_%d.dom", i);
                    tabs[i].dom_tree = deserialize_dom(cache_name);

                    char meta_name[256];
                    snprintf(meta_name, sizeof(meta_name), "cache/tab_%d.meta", i);
                    FILE* mf = fopen(meta_name, "r");
                    if (mf) {
                        char title[128], icon[256];
                        if (fgets(title, sizeof(title), mf)) {
                            title[strcspn(title, "\n")] = 0;
                            if (title[0]) strncpy(tabs[i].title, title, sizeof(tabs[i].title)-1);
                        }
                        if (fgets(icon, sizeof(icon), mf)) {
                            icon[strcspn(icon, "\n")] = 0;
                            if (icon[0]) resolve_page_url(tabs[i].url, icon, tabs[i].favicon_url, sizeof(tabs[i].favicon_url));
                        }
                        fclose(mf);
                    }
                    if (tabs[i].dom_tree) {
                        css_load_and_apply((DOMNode*)tabs[i].dom_tree, "assets/style.css");
                        char bundle_name[256]; snprintf(bundle_name, sizeof(bundle_name), "cache/tab_%d.bundle", i);
                        attach_js(i, tabs[i].url, webpage_bundle_read(bundle_name));
                    }
                    needs_redraw = 1;
                } else if (time(NULL) - tabs[i].loading_start_time > 10) {
                    if (!tabs[i].is_hung) { tabs[i].is_hung = 1; needs_redraw = 1; }
                }
            }
        }
#endif
#endif

#ifdef HAVE_RENDER
        if (active_tab_idx >= 0 && tabs[active_tab_idx].js_page) {                /* таймеры/промисы страницы, навигация и прокрутка из JS */
            void* jp = tabs[active_tab_idx].js_page;
            if (webpage_js_tick(jp)) needs_redraw = 1;
            const char* jnav = webpage_js_take_nav(jp);
            if (jnav) {
                char nbuf[1200]; snprintf(nbuf, sizeof(nbuf), "%s", jnav); BrowserTab* jt = &tabs[active_tab_idx];
                if (!strcmp(nbuf, "__back__")) { if (jt->history_index > 0) { jt->history_index--; clean_and_load_url(active_tab_idx, jt->history[jt->history_index].url, 0); } }
                else if (!strcmp(nbuf, "__forward__")) { if (jt->history_index < jt->history_count - 1) { jt->history_index++; clean_and_load_url(active_tab_idx, jt->history[jt->history_index].url, 0); } }
                else clean_and_load_url(active_tab_idx, nbuf, 1);
                needs_redraw = 1;
            } else {
                int jx, jy; DOMNode* jf; const char* jtitle;
                if (webpage_js_take_scroll(jp, &jx, &jy)) { page_scroll_x = jx; page_scroll_y = jy; clamp_page_scroll(80); needs_redraw = 1; }
                if (webpage_js_take_focus(jp, &jf)) { if (!jf || jf->type == TAG_INPUT || jf->type == TAG_TEXTAREA) focused_node = jf; needs_redraw = 1; }
                if ((jtitle = webpage_js_take_title(jp)) != NULL) { snprintf(tabs[active_tab_idx].title, sizeof(tabs[active_tab_idx].title), "%s", jtitle); needs_redraw = 1; }
            }
        }
#endif

        if (needs_redraw) {
            gal_clear(0xFFFFFF);

#ifdef HAVE_UI
            top_offset = 80;
            if (active_tab_idx >= 0) {
                if (tabs[active_tab_idx].is_hung) {
                    gal_draw_string(100, 200, "TAB HUNG! This site is taking too long.", 0xFF0000, 2);
                    gal_draw_string(100, 240, "You can still use other tabs.", 0x000000, 1);
                } else if (tabs[active_tab_idx].is_loading) {
                    gal_draw_string(100, 200, "Loading...", 0x4B5563, 2);
                } else if (tabs[active_tab_idx].dom_tree) {
#ifdef HAVE_RENDER
                    layout_and_render((DOMNode*)tabs[active_tab_idx].dom_tree, top_offset,
                                      tabs[active_tab_idx].url, page_scroll_x, page_scroll_y);
                    page_content_width = layout_content_width;
                    page_content_height = layout_content_height;
#endif
                }
            }
            top_offset = draw_browser_frame(tabs, tab_count, active_tab_idx);
            draw_scrollbars(top_offset);

            if (context_menu_open) {
                int mw = 150;
                int mh = 130;
                gal_draw_rect(context_menu_x, context_menu_y, mw, mh, 0xFFFFFF);
                uint32_t border = 0x9CA3AF;
                gal_draw_rect(context_menu_x, context_menu_y, mw, 1, border);
                gal_draw_rect(context_menu_x, context_menu_y + mh - 1, mw, 1, border);
                gal_draw_rect(context_menu_x, context_menu_y, 1, mh, border);
                gal_draw_rect(context_menu_x + mw - 1, context_menu_y, 1, mh, border);

                gal_draw_ttf_string(context_menu_x + 10, context_menu_y + 10, "Copy", 0x111111, 14);
                gal_draw_ttf_string(context_menu_x + 10, context_menu_y + 40, "Paste", 0x111111, 14);
                gal_draw_ttf_string(context_menu_x + 10, context_menu_y + 70, "Reload", 0x111111, 14);
                gal_draw_ttf_string(context_menu_x + 10, context_menu_y + 100, "Settings", 0x111111, 14);
            }
#else
            top_offset = 0;
#ifdef HAVE_RENDER
            if (standalone_tree) layout_and_render(standalone_tree, top_offset, "", 0, 0);
#endif
#endif
            gal_flush();
            needs_redraw = 0;
        }

        // Cursor blinking logic (doesn't set needs_redraw unless it changes)
        long now = (long)time(NULL);
        if (now != last_cursor_toggle) {
            cursor_visible = !cursor_visible;
            last_cursor_toggle = now;
            needs_redraw = 1;
        }

        int processed_any = 0;
        GAL_Event ev;
        while ((ev = gal_poll_event()).type != GAL_EVENT_NONE) {
            processed_any = 1;
            if (ev.type == GAL_EVENT_QUIT) {
                running = 0;
                break;
            }

            if (ev.type == GAL_EVENT_MOUSE_DOWN) {
#ifdef HAVE_UI
                if (ev.mouse_button == GAL_MOUSE_LEFT) {
                    select_start_x = ev.mouse_x;
                    select_start_y = ev.mouse_y;
                    selecting_text = 1;
                    layout_selection_active = 1;
                    layout_selection_x1 = select_start_x;
                    layout_selection_y1 = select_start_y;
                    layout_selection_x2 = ev.mouse_x;
                    layout_selection_y2 = ev.mouse_y;
                    needs_redraw = 1;
                }
#endif
            }
            else if (ev.type == GAL_EVENT_MOUSE_MOVE) {
#ifdef HAVE_UI
                if (selecting_text) {
                    layout_selection_x1 = select_start_x;
                    layout_selection_y1 = select_start_y;
                    layout_selection_x2 = ev.mouse_x;
                    layout_selection_y2 = ev.mouse_y;
                    needs_redraw = 1;
                }
#endif
            }
            else if (ev.type == GAL_EVENT_MOUSE_UP) {
#ifdef HAVE_UI
                if (ev.mouse_button == GAL_MOUSE_LEFT) {
                    selecting_text = 0;
                    layout_selection_x2 = ev.mouse_x;
                    layout_selection_y2 = ev.mouse_y;
                    if (abs(layout_selection_x2 - layout_selection_x1) < 4 &&
                        abs(layout_selection_y2 - layout_selection_y1) < 4) {
                        layout_selection_active = 0;
                    }
                    needs_redraw = 1;
                }
#endif
            }
            else if (ev.type == GAL_EVENT_MOUSE_CLICK) {
                int mx = ev.mouse_x;
                int my = ev.mouse_y;
                needs_redraw = 1;

#ifdef HAVE_UI
                if (context_menu_open) {
                    if (mx >= context_menu_x && mx <= context_menu_x + 150 &&
                        my >= context_menu_y && my <= context_menu_y + 130) {
                        int item = (my - context_menu_y) / 33;
                        if (item == 0) {
                            internal_clipboard[0] = '\0';
                            copy_selected_text((DOMNode*)current_nav_dom);
                            if (active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree) {
                                copy_selected_text((DOMNode*)tabs[active_tab_idx].dom_tree);
                            }
                            if (strlen(internal_clipboard) > 0) {
                                gal_set_clipboard(internal_clipboard);
                                gal_log("INFO", "Copied to system clipboard.");
                            }
                        }
                        else if (item == 1) {
                             char* system_clip = gal_get_clipboard();
                             char* target_buf = url_bar_focused ? input_buffer : (focused_node ? focused_node->value : NULL);
                             if (target_buf && system_clip) {
                                 int len = strlen(target_buf);
                                 int rem = 511 - len;
                                 strncat(target_buf, system_clip, rem);
                                 free(system_clip);
                             }
                        }
                        else if (item == 2) execute_onclick("Browser.reload()");
                        else if (item == 3) execute_onclick("Browser.openSettings()");
                    }
                    context_menu_open = 0;
                    continue;
                }

                if (ev.mouse_button == GAL_MOUSE_RIGHT) {
                    context_menu_open = 1;
                    context_menu_x = mx;
                    context_menu_y = my;
                    continue;
                }

                selecting_text = 0;
                layout_selection_active = 0;

                DOMNode* nav_clicked = find_clicked_node((DOMNode*)current_nav_dom, mx, my);
                if (my < top_offset || nav_clicked) {
                    DOMNode* clicked = nav_clicked;
                    if (clicked) {
                        if (clicked->type == TAG_INPUT) {
                            url_bar_focused = 1;
                            focused_node = clicked;
                            strcpy(input_buffer, clicked->value);
                        } else {
                            url_bar_focused = 0;
                            focused_node = NULL;
                            if (strlen(clicked->onclick) > 0) {
                                execute_onclick(clicked->onclick);
                            }
                        }
                    } else {
                        url_bar_focused = 0;
                        focused_node = NULL;
                    }
                }
                else {
                    url_bar_focused = 0;
                    focused_node = NULL;
                    if (active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree) {
#ifdef HAVE_RENDER
                        DOMNode* clicked = find_clicked_node((DOMNode*)tabs[active_tab_idx].dom_tree, mx, my);
                        if (tabs[active_tab_idx].js_page) {                       /* событие click в JS; preventDefault отменяет действие по умолчанию */
                            DOMNode* jt = webpage_hit_element((DOMNode*)tabs[active_tab_idx].dom_tree, mx, my);
                            if (!jt) jt = clicked;
                            if (jt && webpage_js_dispatch(tabs[active_tab_idx].js_page, jt, "click", "", 0)) clicked = NULL;
                            needs_redraw = 1;
                            if (!tabs[active_tab_idx].js_page) clicked = NULL;      /* JS уже перевёл страницу */
                        }
                        if (clicked) {
                            if (clicked->type == TAG_A && strlen(clicked->href) > 0) {
                                gal_log("INFO", "Link clicked! Navigating to: %s", clicked->href);
                                char href_copy[600]; snprintf(href_copy, sizeof(href_copy), "%s", clicked->href);
                                clean_and_load_url(active_tab_idx, href_copy, 1);
                            } else if (clicked->v2) {
                                char form_url[1024];
                                int act = webpage_click((DOMNode*)tabs[active_tab_idx].dom_tree, clicked, form_url, sizeof(form_url));
                                if (act == 1) { focused_node = clicked; gal_log("INFO", "Input focused in page.");
                                    if (tabs[active_tab_idx].js_page) { webpage_js_set_focus(tabs[active_tab_idx].js_page, clicked); webpage_js_dispatch(tabs[active_tab_idx].js_page, clicked, "focus", "", 0); } }
                                else if (act == 2) {
                                    if (!tabs[active_tab_idx].js_page || webpage_js_before_submit(tabs[active_tab_idx].js_page, clicked)) { gal_log("INFO", "Form submit: %s", form_url); clean_and_load_url(active_tab_idx, form_url, 1); }
                                }
                                else if (strlen(clicked->onclick) > 0) execute_onclick(clicked->onclick);
                                needs_redraw = 1;
                            } else if (clicked->type == TAG_INPUT) {
                                focused_node = clicked;
                                gal_log("INFO", "Input focused in page.");
                            }
                        }
#endif
                    }
                }
#endif
            }
            else if (ev.type == GAL_EVENT_MOUSE_SCROLL) {
#ifdef HAVE_UI
                if (ev.mouse_y >= top_offset) {
                    /* сначала пробуем прокрутить контейнер под курсором (overflow:auto|scroll); если он упёрся в край — страница */
                    if (webpage_scroll_at(ev.mouse_x, ev.mouse_y, ev.scroll_x, ev.scroll_y)) { needs_redraw = 1; }
                    else {
                        page_scroll_x += ev.scroll_x;
                        page_scroll_y += ev.scroll_y;
                        clamp_page_scroll(top_offset);
                        needs_redraw = 1;
                    }
                }
#endif
            }
            else if (ev.type == GAL_EVENT_KEY_PRESS) {
                needs_redraw = 1;
#ifdef HAVE_UI
                int js_block = 0;
#ifdef HAVE_RENDER
                if (!url_bar_focused && active_tab_idx >= 0 && tabs[active_tab_idx].js_page && tabs[active_tab_idx].dom_tree) {
                    DOMNode* jt = (focused_node && focused_node->v2) ? focused_node : (DOMNode*)tabs[active_tab_idx].dom_tree;
                    char kn[16] = "";
                    if (ev.key_char == '\r' || ev.key_char == '\n' || ev.key_code == 0xFF0D) strcpy(kn, "Enter");
                    else if (ev.key_char == '\b' || ev.key_code == 0xFF08) strcpy(kn, "Backspace");
                    else if (ev.key_code == 0xFF1B) strcpy(kn, "Escape");
                    else if (ev.key_code == 0xFF52) strcpy(kn, "ArrowUp"); else if (ev.key_code == 0xFF54) strcpy(kn, "ArrowDown");
                    else if (ev.key_code == 0xFF51) strcpy(kn, "ArrowLeft"); else if (ev.key_code == 0xFF53) strcpy(kn, "ArrowRight");
                    else if (ev.text_len > 0 && (unsigned char)ev.text[0] >= 32) snprintf(kn, sizeof(kn), "%.*s", ev.text_len < 15 ? ev.text_len : 15, ev.text);
                    if (kn[0]) { void* jp = tabs[active_tab_idx].js_page; js_block = webpage_js_dispatch(jp, jt, "keydown", kn, 0); if (tabs[active_tab_idx].js_page == jp) webpage_js_dispatch(jp, jt, "keyup", kn, 0); }
                }
#endif
                if (!js_block) {
                if (focused_node || url_bar_focused) {
                    char* target_buf = url_bar_focused ? input_buffer : focused_node->value;
                    int max_len = 511;

                    if (ev.key_char == '\r' || ev.key_char == '\n' || ev.key_code == 0xFF0D) {
                        if (url_bar_focused) {
                            if (active_tab_idx >= 0 && strlen(input_buffer) > 0) {
                                clean_and_load_url(active_tab_idx, input_buffer, 1);
                                url_bar_focused = 0;
                                focused_node = NULL;
                            }
                        } else {
                            if (focused_node && focused_node->v2 && focused_node->type == TAG_TEXTAREA) {
                                int len = strlen(target_buf); if (len + 1 < max_len) { target_buf[len] = '\n'; target_buf[len + 1] = '\0'; }
                            } else if (focused_node && focused_node->v2 && active_tab_idx >= 0) {
                                char form_url[1024]; DOMNode* f = focused_node; focused_node = NULL;
                                void* jp = tabs[active_tab_idx].js_page;
                                if (jp) webpage_js_dispatch(jp, f, "change", "", 0);
                                if (webpage_form_url(f, NULL, form_url, sizeof(form_url)) && (!jp || webpage_js_before_submit(jp, f))) { gal_log("INFO", "Form submit (Enter): %s", form_url); clean_and_load_url(active_tab_idx, form_url, 1); }
                            } else {
                                focused_node = NULL;
                            }
                        }
                    }
                    else if (ev.key_char == '\b' || ev.key_code == 0xFF08) {
                        input_backspace_utf8(target_buf);
#ifdef HAVE_RENDER
                        if (!url_bar_focused && focused_node && focused_node->v2 && tabs[active_tab_idx].js_page) webpage_js_dispatch(tabs[active_tab_idx].js_page, focused_node, "input", "", 0);
#endif
                    }
                    else if (ev.text_len > 0 && (unsigned char)ev.text[0] >= 32) {
                        int len = strlen(target_buf);
                        if (len + ev.text_len < max_len) {
                            memcpy(target_buf + len, ev.text, ev.text_len);
                            target_buf[len + ev.text_len] = '\0';
#ifdef HAVE_RENDER
                            if (!url_bar_focused && focused_node && focused_node->v2 && tabs[active_tab_idx].js_page) { char tx[16]; snprintf(tx, sizeof(tx), "%.*s", ev.text_len < 15 ? ev.text_len : 15, ev.text); webpage_js_dispatch(tabs[active_tab_idx].js_page, focused_node, "input", tx, 0); }
#endif
                        }
                    }
                } else {
                    if (ev.key_code == 0xFF54 || ev.key_code == 0xFF56) page_scroll_y += 80;
                    else if (ev.key_code == 0xFF52 || ev.key_code == 0xFF55) page_scroll_y -= 80;
                    else if (ev.key_code == 0xFF53) page_scroll_x += 80;
                    else if (ev.key_code == 0xFF51) page_scroll_x -= 80;
                    clamp_page_scroll(top_offset);
                }
                }
#endif
            }
        }

        if (!processed_any) {
            usleep(10000);
        }
    }

#ifdef HAVE_UI
    while (tab_count > 0) close_tab(0);
    if (nav_bar_template) free(nav_bar_template);
    if (current_nav_dom) free_dom((DOMNode*)current_nav_dom);
    font_manager_cleanup();
    db_close();
#else
#ifdef HAVE_RENDER
    if (standalone_tree) free_dom(standalone_tree);
#endif
#endif

    gal_close();
    nal_close();
    return 0;
}
