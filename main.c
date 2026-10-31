#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <process.h>
#endif
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
static DOMNode* ime_composition_node = NULL;
static size_t ime_composition_base = 0;
static char ime_preedit_text[256];
static int ime_composition_active = 0;
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
#ifdef HAVE_RENDER
static WebTextPosition web_text_selection_anchor;
static int web_text_selection_anchor_valid;
#endif
char global_history[MAX_GLOBAL_HISTORY][256] = {{0}};
JSRuntime* global_js_runtime = NULL;

int context_menu_open = 0;
int context_menu_x = 0;
int context_menu_y = 0;
char internal_clipboard[32768] = {0};
static int find_bar_open = 0;
static int find_bar_focused = 0;
static char find_query[256] = {0};
static int find_match_count = 0;
static int find_match_index = -1;
#ifdef HAVE_RENDER
static DOMNode* find_highlight_node = NULL;
#endif
static int devtools_target_tab = -1;
static int devtools_panel = 0; /* 0=Elements, 1=Network, 2=Debugger/Runtime */
static char g_transient_download_dir[4096];
#ifdef HAVE_RENDER
static int browser_dom_get_attr(const DOMNode* node, const char* wanted, char* out, size_t cap);
#endif
#if defined(HAVE_UI) && defined(HAVE_RENDER)
/* In-place DOM snapshots preserve DOMNode pointer identity. During the bounded
   copy/relink window each tab's tree is temporarily non-traversable, so the UI
   holds its last frame and advances the commit before running normal work. */
static int browser_dom_commit_pending(void) {
    for (int i = 0; i < tab_count; i++)
        if (tabs[i].js_page && webpage_js_dom_commit_pending(tabs[i].js_page)) return 1;
    return 0;
}
static void browser_dom_commit_advance(void) {
    for (int i = 0; i < tab_count; i++)
        if (tabs[i].js_page) (void)webpage_js_poll(tabs[i].js_page, i == active_tab_idx);
}
#endif
static char ui_status_text[256] = {0};
static long ui_status_expiry = 0;
static int app_shutting_down = 0;
static int browser_debugger_quit_requested = 0;
#ifdef HAVE_UI
/* While a synchronous page-JS IPC is paused, keep the original tab selected
   in browser state and render a separate temporary tab selection. The staged
   selection is applied only after the in-flight JS IPC has completed. */
static int browser_debugger_ui_active = 0;
static int browser_debugger_paused_tab_idx = -1;
static int browser_debugger_display_tab_idx = -1;
static int browser_debugger_pending_tab_idx = -1;
static void* browser_debugger_paused_js_page = NULL;
static void browser_debugger_redraw_pause_tab(void);
static void browser_debugger_log_ui_event(const char* action, int index);
#endif
#ifdef HAVE_RENDER
/* A form request is inherited by the navigation worker after fork. */
static int g_pending_form_nav = 0;
static char g_pending_form_url[1200];
static char g_pending_form_method[8];
static char g_pending_form_headers[256];
static const unsigned char* g_pending_form_body;
static size_t g_pending_form_body_len;
#endif
static int browser_choose_file(char* out, size_t cap);
static int browser_choose_save_path(char* out, size_t cap, const char* title, const char* extension);
void clean_and_load_url(int index, const char* target_url, int record_history);
void create_new_tab(void);
void close_tab(int index);
static void input_backspace_utf8(char* text);
static void browser_create_tab_with_url(const char* url, int record_history);

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
    if (head && head->v2) { webpage_copy_selection(internal_clipboard + offset, (int)sizeof(internal_clipboard) - 1 - offset); return; }

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
                             if (offset + char_len < (int)sizeof(internal_clipboard) - 1) {
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

static int url_piece_append(char *out, size_t cap, size_t *used, const char *src, size_t n) {
    if (!out || !used || !src || *used >= cap || n > cap - *used - 1) return 0;
    memcpy(out + *used, src, n);
    *used += n;
    out[*used] = '\0';
    return 1;
}

/* Resolve favicon URL references without truncating them or dropping an HTTPS base scheme. */
static int resolve_page_url(const char* page_url, const char* raw, char* out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!raw || !*raw) return 0;
    size_t raw_len = strlen(raw);
    if (raw_len >= out_size) return 0;

    if (!strncasecmp(raw, "http://", 7) || !strncasecmp(raw, "https://", 8) ||
        !strncasecmp(raw, "data:", 5) || !strncasecmp(raw, "blob:", 5)) {
        memcpy(out, raw, raw_len + 1);
        return 1;
    }

    /* Other explicit schemes (notably javascript: and file:) are not favicon URLs. */
    const char *colon = strchr(raw, ':');
    const char *slashish = strpbrk(raw, "/?#");
    if (colon && (!slashish || colon < slashish)) return 0;
    if (!page_url) return 0;
    const char *sep = strstr(page_url, "://");
    if (!sep) return 0;
    size_t scheme_len = (size_t)(sep - page_url);
    int https = scheme_len == 5 && !strncasecmp(page_url, "https", 5);
    int http = scheme_len == 4 && !strncasecmp(page_url, "http", 4);
    if (!http && !https) return 0;
    size_t scheme_colon_len = scheme_len + 1;
    const char *authority = sep + 3;
    const char *authority_end = strpbrk(authority, "/?#");
    size_t origin_len = authority_end ? (size_t)(authority_end - page_url) : strlen(page_url);
    if (origin_len >= out_size) return 0;

    size_t used = 0;
    if (raw[0] == '/' && raw[1] == '/') {
        return url_piece_append(out, out_size, &used, page_url, scheme_colon_len) &&
               url_piece_append(out, out_size, &used, raw, raw_len);
    }
    if (raw[0] == '/') {
        return url_piece_append(out, out_size, &used, page_url, origin_len) &&
               url_piece_append(out, out_size, &used, raw, raw_len);
    }

    /* Strip an existing fragment; a query reference also replaces the old query. */
    size_t base_len = strcspn(page_url, "#");
    if (raw[0] == '#') {
        return url_piece_append(out, out_size, &used, page_url, base_len) &&
               url_piece_append(out, out_size, &used, raw, raw_len);
    }
    if (raw[0] == '?') {
        base_len = strcspn(page_url, "?#");
        if (base_len == origin_len) {
            if (!url_piece_append(out, out_size, &used, page_url, base_len) ||
                !url_piece_append(out, out_size, &used, "/", 1)) return 0;
        } else if (!url_piece_append(out, out_size, &used, page_url, base_len)) return 0;
        return url_piece_append(out, out_size, &used, raw, raw_len);
    }

    base_len = strcspn(page_url, "?#");
    if (base_len < origin_len) base_len = origin_len;
    const char *last_slash = NULL;
    for (const char *p = page_url; p < page_url + base_len; ++p) if (*p == '/') last_slash = p;
    if (!last_slash || last_slash < authority) {
        if (!url_piece_append(out, out_size, &used, page_url, origin_len) ||
            !url_piece_append(out, out_size, &used, "/", 1)) return 0;
    } else {
        size_t dir_len = (size_t)(last_slash + 1 - page_url);
        if (!url_piece_append(out, out_size, &used, page_url, dir_len)) return 0;
    }
    return url_piece_append(out, out_size, &used, raw, raw_len);
}

static void browser_persist_session(void) {
    if (app_shutting_down || anonymous_mode || module_db_disabled || !tab_count) return;
    for (int i = 0; i < tab_count; i++) db_save_session_tab(i, tabs[i].url, i == active_tab_idx);
    db_session_clear_from(tab_count);
}

static void browser_set_status(const char* text) {
    snprintf(ui_status_text, sizeof ui_status_text, "%s", text ? text : "");
    ui_status_expiry = (long)time(NULL) + 4;
    needs_redraw = 1;
}

/* The QuickJS process sends a pause notification while the parent is waiting
   for a DOM IPC reply. The debugger's nested cooperative UI pump keeps chrome,
   tab selection and address entry responsive without delivering page events to
   the paused JS runtime. Tab changes are staged until the outstanding IPC ends. */
int browser_debugger_pause_loop(void* js_page, const char* filename, const char* function_name,
                                int line, int column, const char* stack,
                                const char* locals, int is_breakpoint) {
#ifdef MLBROWSER_TEST_AUTO_CONTINUE
    /* The IPC integration test uses the real QuickJS child/parent transport,
       but auto-continues a breakpoint so it can assert deferred-command FIFO
       behavior without synthesizing GUI input. Never enabled in browser builds. */
    (void)js_page; (void)filename; (void)function_name; (void)line;
    (void)column; (void)stack; (void)locals; (void)is_breakpoint;
    return 0;
#endif
#ifdef HAVE_UI
    if (!browser_debugger_ui_active) {
        browser_debugger_ui_active = 1;
        int owner_tab = active_tab_idx;
#ifdef HAVE_RENDER
        for (int i = 0; i < tab_count; i++)
            if (tabs[i].js_page == js_page) { owner_tab = i; break; }
#endif
        browser_debugger_paused_tab_idx = owner_tab;
        browser_debugger_display_tab_idx = owner_tab;
        browser_debugger_paused_js_page = js_page;
    }
    if (browser_debugger_display_tab_idx < 0 || browser_debugger_display_tab_idx >= tab_count)
        browser_debugger_display_tab_idx = browser_debugger_paused_tab_idx;
#endif
    int sw = SCREEN_WIDTH > 0 ? SCREEN_WIDTH : 1024;
    int sh = SCREEN_HEIGHT > 0 ? SCREEN_HEIGHT : 768;
    int pw = sw - 32; if (pw > 600) pw = 600; if (pw < 260) pw = sw - 8;
    int ph = sh - 112; if (ph > 540) ph = 540; if (ph < 220) ph = sh - 8;
    int x = sw - pw - 16; if (x < 4) x = 4;
    int y = 88; if (y + ph > sh - 8) y = sh - ph - 8; if (y < 4) y = 4;
    char heading[256], location[900], fn[320];
    snprintf(heading, sizeof heading, "JavaScript paused — %s", is_breakpoint ? "breakpoint" : "step");
    snprintf(location, sizeof location, "%s:%d:%d", filename && filename[0] ? filename : "<anonymous>", line, column);
    snprintf(fn, sizeof fn, "Function: %s", function_name && function_name[0] ? function_name : "<script>");
#ifdef HAVE_UI
    browser_debugger_redraw_pause_tab();
#endif
    /* A card overlay instead of a full-window replacement leaves the rendered
       page/chrome visible. This matters when the user switches to another tab. */
    gal_draw_rect(x + 4, y + 4, pw, ph, 0x9CA3AF);
    gal_draw_rect(x, y, pw, ph, 0xF9FAFB);
    gal_draw_rect(x, y, pw, 40, 0x1F2937);
    gal_draw_ttf_string(x + 14, y + 12, heading, 0xFFFFFF, 15);
    gal_draw_ttf_string(x + 16, y + 54, location, 0x7C2D12, 12);
    gal_draw_ttf_string(x + 16, y + 74, fn, 0x111827, 12);
#ifdef HAVE_UI
    char tab_note[160];
    snprintf(tab_note, sizeof tab_note, "Paused tab %d; viewing tab %d. Ctrl+Tab switches; Ctrl+L opens address entry.",
             browser_debugger_paused_tab_idx + 1, browser_debugger_display_tab_idx + 1);
    gal_draw_ttf_string(x + 16, y + 94, tab_note, 0x374151, 10);
#else
    gal_draw_ttf_string(x + 16, y + 94, "Page input is held while JavaScript is paused.", 0x374151, 10);
#endif
    gal_draw_ttf_string(x + 16, y + 116, "Call stack:", 0x111827, 12);
    char stack_copy[2048]; snprintf(stack_copy, sizeof stack_copy, "%s", stack ? stack : "");
    int sy = y + 136, shown = 0; char* line_start = stack_copy;
    while (*line_start && shown < 5 && sy < y + 238 && sy < y + ph - 154) {
        char* line_end = strchr(line_start, '\n'); if (line_end) *line_end = 0;
        if (line_start[0]) { gal_draw_ttf_string(x + 20, sy, line_start, 0x374151, 10); sy += 16; shown++; }
        if (!line_end) break;
        line_start = line_end + 1;
    }
    if (!shown) gal_draw_ttf_string(x + 20, sy, "(no JavaScript stack frames available)", 0x6B7280, 10);
    int local_y = y + 246;
    if (ph >= 390) {
        gal_draw_ttf_string(x + 16, local_y, "Locals (primitive values; objects are placeholders):", 0x111827, 11);
        local_y += 19;
        char locals_copy[2400]; snprintf(locals_copy, sizeof locals_copy, "%s", locals ? locals : "");
        char* local_start = locals_copy; int local_shown = 0;
        while (*local_start && local_shown < 8 && local_y < y + ph - 64) {
            char* local_end = strchr(local_start, '\n'); if (local_end) *local_end = 0;
            if (local_start[0]) { gal_draw_ttf_string(x + 20, local_y, local_start, 0x374151, 10); local_y += 15; local_shown++; }
            if (!local_end) break;
            local_start = local_end + 1;
        }
        if (!local_shown) gal_draw_ttf_string(x + 20, local_y, "(no named locals available)", 0x6B7280, 10);
    }
    gal_draw_rect(x, y + ph - 48, pw, 48, 0xE5E7EB);
    gal_draw_ttf_string(x + 14, y + ph - 34, "F8/C Continue   F11/I Step into   F10/O Step over", 0x111827, 11);
    gal_draw_ttf_string(x + 14, y + ph - 17, "Shift+F11/U Step out   Esc Continue   Page input is suppressed", 0x111827, 10);
    gal_flush();

    for (;;) {
#ifdef HAVE_NET
        nal_download_poll();
#endif
        /* Keep other documents alive while this document is suspended in its
           QuickJS trace hook. Async ticks are coalesced per child process, and
           replies are consumed without allowing another tab to steal the
           single visible debugger overlay. */
#ifdef HAVE_RENDER
        int background_changed = 0;
        for (int i = 0; i < tab_count; i++) {
            if (i == browser_debugger_paused_tab_idx || !tabs[i].js_page) continue;
            (void)webpage_js_tick_async(tabs[i].js_page);
            if (webpage_js_poll(tabs[i].js_page, 0)) background_changed = 1;
        }
#else
        int background_changed = 0;
#endif
        GAL_Event ev = gal_poll_event();
        if (ev.type == GAL_EVENT_NONE) {
            usleep(8000);
            if (!background_changed) continue;
        }
        if (ev.type == GAL_EVENT_QUIT) { browser_debugger_quit_requested = 1; return 0; }
#ifdef HAVE_UI
        int redraw = background_changed;
        int key = (unsigned char)ev.key_char;
        if (!key && ev.text_len > 0) key = (unsigned char)ev.text[0];
        if (!key && ev.key_code > 0 && ev.key_code < 128) key = ev.key_code;
        if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
        int shortcut = (ev.key_code > 0 && ev.key_code < 128) ? tolower(ev.key_code) : key;
        if (ev.type == GAL_EVENT_KEY_PRESS) {
            if ((ev.modifiers & GAL_MOD_CTRL) && (ev.key_code == 0xFF09 || key == '\t')) {
                if (tab_count > 0) {
                    int dir = (ev.modifiers & GAL_MOD_SHIFT) ? -1 : 1;
                    browser_debugger_display_tab_idx = (browser_debugger_display_tab_idx + dir + tab_count) % tab_count;
                    browser_debugger_log_ui_event("TAB_SWITCH", browser_debugger_display_tab_idx);
                    url_bar_focused = 0; redraw = 1;
                }
            } else if ((ev.modifiers & GAL_MOD_CTRL) && shortcut == 't') {
                if (tab_count < MAX_TABS) {
                    int saved_active = active_tab_idx;
                    browser_create_tab_with_url("browser://history", 0);
                    browser_debugger_display_tab_idx = active_tab_idx;
                    active_tab_idx = saved_active;
                    browser_debugger_log_ui_event("NEW_TAB", browser_debugger_display_tab_idx);
                    url_bar_focused = 0; redraw = 1;
                }
            } else if ((ev.modifiers & GAL_MOD_CTRL) && shortcut == 'w') {
                int idx = browser_debugger_display_tab_idx;
                if (idx >= 0 && idx < tab_count && idx != browser_debugger_paused_tab_idx) {
                    int paused = browser_debugger_paused_tab_idx, saved_active = active_tab_idx;
                    close_tab(idx);
                    if (idx < paused) paused--;
                    if (browser_debugger_display_tab_idx > idx) browser_debugger_display_tab_idx--;
                    else if (browser_debugger_display_tab_idx == idx)
                        browser_debugger_display_tab_idx = tab_count ? (idx < tab_count ? idx : tab_count - 1) : -1;
                    browser_debugger_paused_tab_idx = paused;
                    active_tab_idx = paused >= 0 && paused < tab_count ? paused : saved_active;
                    browser_debugger_log_ui_event("CLOSE_TAB", idx);
                    url_bar_focused = 0; redraw = 1;
                }
            } else if ((ev.modifiers & GAL_MOD_CTRL) && shortcut == 'l') {
                int idx = browser_debugger_display_tab_idx;
                input_buffer[0] = 0;
                if (idx >= 0 && idx < tab_count) snprintf(input_buffer, sizeof input_buffer, "%s", tabs[idx].url);
                url_bar_focused = 1; redraw = 1;
            } else if (url_bar_focused && (ev.key_code == 0xFF0D || key == '\n' || key == '\r')) {
                int idx = browser_debugger_display_tab_idx;
                if (input_buffer[0] && idx >= 0 && idx < tab_count) {
                    if (idx == browser_debugger_paused_tab_idx && tab_count < MAX_TABS) {
                        int saved_active = active_tab_idx;
                        browser_create_tab_with_url(input_buffer, 1);
                        browser_debugger_display_tab_idx = active_tab_idx;
                        active_tab_idx = saved_active;
                    } else if (idx != browser_debugger_paused_tab_idx) {
                        clean_and_load_url(idx, input_buffer, 1);
                    }
                    browser_debugger_log_ui_event("NAVIGATE", browser_debugger_display_tab_idx);
                }
                url_bar_focused = 0; redraw = 1;
            } else if (url_bar_focused && (ev.key_code == 0xFF08 || ev.key_code == 0xFFFF)) {
                input_backspace_utf8(input_buffer); redraw = 1;
            } else if (url_bar_focused && !(ev.modifiers & (GAL_MOD_CTRL | GAL_MOD_ALT))) {
                const char* chunk = ev.text_len > 0 ? ev.text : NULL;
                size_t add = chunk ? (size_t)ev.text_len : 1u;
                if (!chunk && key >= 32 && key < 127) { char one = (char)key; chunk = &one; }
                int printable = chunk != NULL;
                if (chunk) for (size_t ci = 0; ci < add; ci++)
                    if ((unsigned char)chunk[ci] < 32 || (unsigned char)chunk[ci] == 127) printable = 0;
                size_t n = strlen(input_buffer);
                if (printable && add > 0 && add < sizeof input_buffer - n) {
                    memcpy(input_buffer + n, chunk, add); input_buffer[n + add] = 0; redraw = 1;
                }
            } else if (ev.key_code == 0xFF1B || key == 27) {
                if (url_bar_focused) { url_bar_focused = 0; redraw = 1; }
                else return 0;
            } else if (ev.key_code == 0xFFC5 || key == 'c' || key == 'q') return 0;
            else if (ev.key_code == 0xFFC7 || key == 'o') return 2;
            else if (ev.key_code == 0xFFC8) return (ev.modifiers & GAL_MOD_SHIFT) ? 3 : 1;
            else if (key == 'i') return 1;
            else if (key == 'u') return 3;
        } else if (ev.type == GAL_EVENT_MOUSE_DOWN && ev.mouse_button == GAL_MOUSE_LEFT) {
            int mx = ev.mouse_x, my = ev.mouse_y;
            if (my >= 48 && my < 80) {
                if (mx >= 5) {
                    int idx = (mx - 5) / 135;
                    int within = (mx - 5) % 135;
                    if (idx >= 0 && idx < tab_count && within < 130) {
                        if (within >= 110 && idx != browser_debugger_paused_tab_idx) {
                            int paused = browser_debugger_paused_tab_idx, saved_active = active_tab_idx;
                            close_tab(idx);
                            if (idx < paused) paused--;
                            if (browser_debugger_display_tab_idx > idx) browser_debugger_display_tab_idx--;
                            else if (browser_debugger_display_tab_idx == idx)
                                browser_debugger_display_tab_idx = tab_count ? (idx < tab_count ? idx : tab_count - 1) : -1;
                            browser_debugger_paused_tab_idx = paused;
                            active_tab_idx = paused >= 0 && paused < tab_count ? paused : saved_active;
                            browser_debugger_log_ui_event("CLOSE_TAB", idx);
                        } else {
                            browser_debugger_display_tab_idx = idx;
                            browser_debugger_log_ui_event("TAB_SWITCH", idx);
                        }
                        url_bar_focused = 0; redraw = 1;
                    } else {
                        int add_x = tab_count * 135 + 5;
                        if (mx >= add_x && mx < add_x + 30 && tab_count < MAX_TABS) {
                            int saved_active = active_tab_idx;
                            browser_create_tab_with_url("browser://history", 0);
                            browser_debugger_display_tab_idx = active_tab_idx;
                            active_tab_idx = saved_active;
                            browser_debugger_log_ui_event("NEW_TAB", browser_debugger_display_tab_idx);
                            redraw = 1;
                        }
                    }
                }
            } else if (my >= 10 && my < 42 && mx >= 185 && mx < sw - 245) {
                int idx = browser_debugger_display_tab_idx;
                input_buffer[0] = 0;
                if (idx >= 0 && idx < tab_count) snprintf(input_buffer, sizeof input_buffer, "%s", tabs[idx].url);
                url_bar_focused = 1; redraw = 1;
            }
        } else if (ev.type == GAL_EVENT_REDRAW) redraw = 1;
        if (redraw) {
            browser_debugger_redraw_pause_tab();
            /* Repaint the debugger card over the newly selected tab. The code
               is duplicated here intentionally to keep its frame lifetime local. */
            gal_draw_rect(x + 4, y + 4, pw, ph, 0x9CA3AF);
            gal_draw_rect(x, y, pw, ph, 0xF9FAFB);
            gal_draw_rect(x, y, pw, 40, 0x1F2937);
            gal_draw_ttf_string(x + 14, y + 12, heading, 0xFFFFFF, 15);
            gal_draw_ttf_string(x + 16, y + 54, location, 0x7C2D12, 12);
            gal_draw_ttf_string(x + 16, y + 74, fn, 0x111827, 12);
            char tab_note[160];
            snprintf(tab_note, sizeof tab_note, "Paused tab %d; viewing tab %d. Ctrl+Tab switches; Ctrl+L opens address entry.",
                     browser_debugger_paused_tab_idx + 1, browser_debugger_display_tab_idx + 1);
            gal_draw_ttf_string(x + 16, y + 94, tab_note, 0x374151, 10);
            gal_draw_ttf_string(x + 16, y + 116, "Call stack:", 0x111827, 12);
            char sc[2048]; snprintf(sc, sizeof sc, "%s", stack ? stack : ""); char* q = sc; int rows = 0; int py = y + 136;
            while (*q && rows < 5 && py < y + ph - 154) { char* e = strchr(q, '\n'); if (e) *e = 0; if (*q) { gal_draw_ttf_string(x + 20, py, q, 0x374151, 10); py += 16; rows++; } if (!e) break; q = e + 1; }
            gal_draw_ttf_string(x + 16, y + 246, "Locals (primitive values; objects are placeholders):", 0x111827, 11);
            char lc[2400]; snprintf(lc, sizeof lc, "%s", locals ? locals : ""); q = lc; rows = 0; py = y + 265;
            while (*q && rows < 8 && py < y + ph - 64) { char* e = strchr(q, '\n'); if (e) *e = 0; if (*q) { gal_draw_ttf_string(x + 20, py, q, 0x374151, 10); py += 15; rows++; } if (!e) break; q = e + 1; }
            gal_draw_rect(x, y + ph - 48, pw, 48, 0xE5E7EB);
            gal_draw_ttf_string(x + 14, y + ph - 34, "F8/C Continue   F11/I Step into   F10/O Step over", 0x111827, 11);
            gal_draw_ttf_string(x + 14, y + ph - 17, "Shift+F11/U Step out   Esc Continue   Page input is suppressed", 0x111827, 10);
            gal_flush();
        }
#else
        if (ev.type != GAL_EVENT_KEY_PRESS) continue;
        int key = ev.key_char;
        if (!key && ev.text_len > 0) key = ev.text[0];
        if (!key && ev.key_code > 0 && ev.key_code < 128) key = ev.key_code;
        if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
        if (ev.key_code == 0xFF1B || ev.key_code == 0xFFC5 || key == 'c' || key == 'q') return 0;
        if (ev.key_code == 0xFFC7 || key == 'o') return 2;
        if (ev.key_code == 0xFFC8) return (ev.modifiers & GAL_MOD_SHIFT) ? 3 : 1;
        if (key == 'i') return 1;
        if (key == 'u') return 3;
#endif
    }
}

#ifdef HAVE_UI
static void browser_debugger_log_ui_event(const char* action, int index) {
    (void)mkdir("cache", 0700);
    FILE* f = fopen("cache/js_debug_ui.log", "ab");
    if (!f) return;
    fprintf(f, "%ld\t%s\t%d\n", get_time_ms(), action ? action : "UI", index);
    fflush(f);
    fclose(f);
}

/* Called by the JS IPC client only when the paused script finally returns its
   normal reply. It delays active-tab switching until the in-flight browser
   event has unwound, preventing tab-index changes in the middle of dispatch. */
void browser_debugger_pause_session_done(void* js_page) {
    if (!browser_debugger_ui_active) return;
    /* Replies from another document may be drained while the current page is
       paused. Only the process that owns this pause may close the session. */
    if (browser_debugger_paused_js_page && browser_debugger_paused_js_page != js_page) return;
    if (browser_debugger_display_tab_idx >= 0 && browser_debugger_display_tab_idx < tab_count)
        browser_debugger_pending_tab_idx = browser_debugger_display_tab_idx;
    browser_debugger_ui_active = 0;
    browser_debugger_paused_tab_idx = -1;
    browser_debugger_display_tab_idx = -1;
    browser_debugger_paused_js_page = NULL;
}
#endif

static int browser_contains_ci(const char* haystack, const char* needle) {
    if (!needle || !*needle) return 0;
    if (!haystack) return 0;
    size_t n = strlen(needle);
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static void browser_origin_for_url(const char* url, char* out, size_t cap) {
    if (!out || !cap) return; out[0] = 0;
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) return;
    const char* end = strchr(url + (url[4] == 's' ? 8 : 7), '/');
    size_t n = end ? (size_t)(end - url) : strlen(url);
    if (n >= cap) n = cap - 1;
    memcpy(out, url, n); out[n] = 0;
}

static char* browser_html_escape(const char* in) {
    if (!in) return strdup("");
    size_t n = strlen(in); if (n > 4 * 1024 * 1024) n = 4 * 1024 * 1024;
    char* out = malloc(n * 6 + 1); if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        const char* r = NULL;
        switch (in[i]) { case '&': r="&amp;"; break; case '<': r="&lt;"; break; case '>': r="&gt;"; break; case '"': r="&quot;"; break; case '\'': r="&#39;"; break; default: out[j++]=in[i]; continue; }
        size_t m=strlen(r); memcpy(out+j,r,m); j+=m;
    }
    out[j]=0; return out;
}

static void browser_appendf(char** buf, size_t* len, size_t* cap, const char* fmt, ...) {
    va_list ap, cp; va_start(ap, fmt); va_copy(cp, ap);
    int need = vsnprintf(NULL, 0, fmt, cp); va_end(cp);
    if (need < 0) { va_end(ap); return; }
    if (*len + (size_t)need + 1 > *cap) {
        size_t nc = *cap ? *cap : 4096; while (*len + (size_t)need + 1 > nc) nc *= 2;
        char* n = realloc(*buf, nc); if (!n) { va_end(ap); return; } *buf = n; *cap = nc;
    }
    vsnprintf(*buf + *len, *cap - *len, fmt, ap); va_end(ap); *len += (size_t)need;
}

static void browser_scale_len(Len* l, float ratio) { if (l && l->set) l->px *= ratio; }
static void browser_scale_node(DOMNode* n, float ratio) {
    if (!n || !n->v2) return;
    NodeStyle* s = &n->st;
    s->font_size *= ratio; s->lh_px *= ratio; s->gap_r *= ratio; s->gap_c *= ratio; s->spacing *= ratio;
    for (int i=0;i<4;i++) { browser_scale_len(&s->margin[i],ratio); browser_scale_len(&s->padding[i],ratio); browser_scale_len(&s->inset[i],ratio); s->border_w[i]*=ratio; s->radius[i]*=ratio; }
    browser_scale_len(&s->width,ratio); browser_scale_len(&s->height,ratio); browser_scale_len(&s->min_w,ratio); browser_scale_len(&s->max_w,ratio); browser_scale_len(&s->min_h,ratio); browser_scale_len(&s->max_h,ratio);
    browser_scale_len(&s->bg_w,ratio); browser_scale_len(&s->bg_h,ratio); browser_scale_len(&s->bg_px,ratio); browser_scale_len(&s->bg_py,ratio);
    for (int i=0;i<s->filter.n;i++) { FxOp* o=&s->filter.op[i]; if(o->kind==FXO_BLUR||o->kind==FXO_SHADOW){o->v*=ratio;o->x*=ratio;o->y*=ratio;o->blur*=ratio;} }
    for (int i=0;i<s->backdrop.n;i++) { FxOp* o=&s->backdrop.op[i]; if(o->kind==FXO_BLUR||o->kind==FXO_SHADOW){o->v*=ratio;o->x*=ratio;o->y*=ratio;o->blur*=ratio;} }
    if (s->clip.kind) for(int i=0;i<4;i++) s->clip.a[i].px*=ratio;
    if (s->mask.kind) { browser_scale_len(&s->mask.w,ratio);browser_scale_len(&s->mask.h,ratio);browser_scale_len(&s->mask.px,ratio);browser_scale_len(&s->mask.py,ratio); }
    for (int i=0;i<s->xf.n;i++) { FxXfOp* x=&s->xf.op[i]; x->t[0].px*=ratio;x->t[1].px*=ratio; if(x->kind==XF_MATRIX){x->v[4]*=ratio;x->v[5]*=ratio;} }
    s->sh_x*=ratio; s->sh_y*=ratio; s->sh_blur*=ratio; s->sh_spread*=ratio;
}

static void browser_zoom_active(float target) {
    if (active_tab_idx < 0 || active_tab_idx >= tab_count || !tabs[active_tab_idx].dom_tree) return;
    if (target < 0.5f) target = 0.5f; if (target > 2.0f) target = 2.0f;
    BrowserTab* tab = &tabs[active_tab_idx]; float old = tab->zoom > 0.1f ? tab->zoom : 1.0f; if (fabsf(target-old) < 0.001f) return;
    float ratio = target / old; DOMNode* head = (DOMNode*)tab->dom_tree;
    for (DOMNode* n=head;n;n=n->next) browser_scale_node(n,ratio);
    tab->zoom = target;
#ifdef HAVE_RENDER
    webpage_invalidate_layout(); webpage_force_layout_for(head);
#endif
    char msg[100]; snprintf(msg,sizeof msg,"Page zoom: %d%%",(int)lroundf(target*100)); browser_set_status(msg);
}

#ifdef HAVE_RENDER
static void browser_find_next(int advance) {
    find_highlight_node = NULL;
    if (active_tab_idx < 0 || !tabs[active_tab_idx].dom_tree || !find_query[0]) { find_match_count=0;find_match_index=-1;return; }
    DOMNode* head=(DOMNode*)tabs[active_tab_idx].dom_tree; int count=0;
    for(DOMNode*n=head;n;n=n->next){const char*t=n->text;if(browser_contains_ci(t,find_query))count++;else if((n->type==TAG_INPUT||n->type==TAG_TEXTAREA)&&browser_contains_ci(n->value,find_query))count++;}
    find_match_count=count; if(!count){find_match_index=-1;browser_set_status("Text not found");return;}
    if (advance && find_match_index >= 0) find_match_index=(find_match_index+1)%count; else if(find_match_index<0||find_match_index>=count)find_match_index=0;
    int wanted=find_match_index, seen=0; DOMNode* chosen=NULL;
    for(DOMNode*n=head;n;n=n->next){const char*t=n->text;if(browser_contains_ci(t,find_query)){if(seen++==wanted){chosen=n;break;}}else if((n->type==TAG_INPUT||n->type==TAG_TEXTAREA)&&browser_contains_ci(n->value,find_query)){if(seen++==wanted){chosen=n;break;}}}
    if(chosen){find_highlight_node=chosen;float r[4];if(webpage_node_rect(chosen,r)){page_scroll_y=(int)r[1]-(SCREEN_HEIGHT/3);if(page_scroll_y<0)page_scroll_y=0;}else if(chosen->hit_y>80){page_scroll_y+=chosen->hit_y-(SCREEN_HEIGHT/3);if(page_scroll_y<0)page_scroll_y=0;}}
    char msg[120];snprintf(msg,sizeof msg,"Match %d of %d",find_match_index+1,count);browser_set_status(msg);
}

static void browser_draw_find_highlight(int top_offset) {
    if (!find_highlight_node || active_tab_idx < 0 || active_tab_idx >= tab_count ||
        tabs[active_tab_idx].dom_tree == NULL) return;
    /* The selected node may belong to another tab (or have been freed during navigation).
       Compare addresses against the active tree before dereferencing the pointer. */
    int belongs_to_active_tab = 0;
    for (DOMNode* n=(DOMNode*)tabs[active_tab_idx].dom_tree; n; n=n->next) {
        if (n == find_highlight_node) { belongs_to_active_tab = 1; break; }
    }
    if (!belongs_to_active_tab) return;
    float r[4];
    if (!webpage_node_rect(find_highlight_node, r)) return;
    int x=(int)lroundf(r[0]) - page_scroll_x;
    int y=(int)lroundf(r[1]) - page_scroll_y + top_offset;
    int w=(int)lroundf(r[2]);
    int h=(int)lroundf(r[3]);
    if (w < 3) w=3; if (h < 3) h=3;
    /* Draw a high-contrast outline over the selected match without touching page pixels. */
    uint32_t color=0xF59E0B;
    gal_draw_rect(x-2,y-2,w+4,2,color);
    gal_draw_rect(x-2,y+h,w+4,2,color);
    gal_draw_rect(x-2,y-2,2,h+4,color);
    gal_draw_rect(x+w,y-2,2,h+4,color);
}
#endif

static void browser_draw_find_bar(void) {
    if (!find_bar_open) return;
    int x=SCREEN_WIDTH-370; if(x<4)x=4; int y=84; int w=SCREEN_WIDTH-x-8; if(w<220)w=220;
    gal_draw_rect(x,y,w,38,0xFFFDF7); gal_draw_rect(x,y,w,1,0x4B5563); gal_draw_rect(x,y+37,w,1,0x4B5563);
    gal_draw_ttf_string(x+8,y+11,"Find:",0x111827,14);
    gal_draw_ttf_string(x+55,y+11,find_query,0x111827,14);
    char count[40];snprintf(count,sizeof count,"%s",find_query[0]?(find_match_count?"matches":"not found"):"Enter text");
    gal_draw_ttf_string(x+w-100,y+11,count,0x4B5563,12);
}

static int browser_ascii_contains(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    for (const char* p = haystack; *p; ++p) if (!strncasecmp(p, needle, n)) return 1;
    return 0;
}

static void browser_apply_password_autofill(int tab_idx) {
#ifndef HAVE_RENDER
    (void)tab_idx;
#else
    if (anonymous_mode || module_db_disabled || tab_idx<0 || tab_idx>=tab_count || !tabs[tab_idx].dom_tree) return;
    char origin[1200], username[256], password[2048];
    browser_origin_for_url(tabs[tab_idx].url,origin,sizeof origin);
    if(!origin[0]||!db_password_get_for_origin(origin,username,sizeof username,password,sizeof password))return;
    DOMNode* head=(DOMNode*)tabs[tab_idx].dom_tree; DOMNode* user_node=NULL; DOMNode* pass_node=NULL;
    int best_user=-1, best_pass=-1;
    for(DOMNode*n=head;n;n=n->next){
        if(n->type!=TAG_INPUT)continue;
        char autocomplete[128]="", name[256]="", id[256]="", type[64]="";
        (void)browser_dom_get_attr(n,"autocomplete",autocomplete,sizeof autocomplete);
        (void)browser_dom_get_attr(n,"name",name,sizeof name);
        (void)browser_dom_get_attr(n,"id",id,sizeof id);
        (void)browser_dom_get_attr(n,"type",type,sizeof type);
        if(browser_dom_get_attr(n,"disabled",type,sizeof type) || browser_dom_get_attr(n,"readonly",type,sizeof type)) continue;
        if(!strcmp(n->input_type,"password")) {
            if(browser_ascii_contains(autocomplete,"new-password")) continue;
            int score=0;
            if(browser_ascii_contains(autocomplete,"current-password")) score+=100;
            if(browser_ascii_contains(autocomplete,"one-time-code")) continue;
            if(!strcmp(n->input_type,"password")) score+=20;
            if(!n->value[0] && score>best_pass){best_pass=score;pass_node=n;}
            continue;
        }
        if(strcmp(n->input_type,"text") && strcmp(n->input_type,"email") && strcmp(n->input_type,"url")) continue;
        if(browser_ascii_contains(autocomplete,"off") || browser_ascii_contains(autocomplete,"new-password") ||
           browser_ascii_contains(autocomplete,"one-time-code") || !strcmp(n->input_type,"search") ||
           browser_ascii_contains(name,"search") || browser_ascii_contains(id,"search")) continue;
        int score=0;
        if(browser_ascii_contains(autocomplete,"username")) score+=120;
        if(browser_ascii_contains(autocomplete,"email")) score+=65;
        if(!strcmp(n->input_type,"email")) score+=35;
        if(browser_ascii_contains(name,"user")||browser_ascii_contains(id,"user"))score+=45;
        if(browser_ascii_contains(name,"login")||browser_ascii_contains(id,"login"))score+=50;
        if(browser_ascii_contains(name,"email")||browser_ascii_contains(id,"email"))score+=35;
        if(browser_ascii_contains(autocomplete,"name"))score-=30;
        if(!n->value[0] && score>best_user){best_user=score;user_node=n;}
    }
    /* Only fill fields that look like a login form; never blindly fill the first text input. */
    if(user_node && pass_node && best_user >= 20) {
        if(user_node && !user_node->value[0]) {snprintf(user_node->value,sizeof user_node->value,"%s",username);if(tabs[tab_idx].js_page){webpage_js_dispatch(tabs[tab_idx].js_page,user_node,"input",username,0);webpage_js_dispatch(tabs[tab_idx].js_page,user_node,"change","",0);}}
        if(pass_node && !pass_node->value[0]) {snprintf(pass_node->value,sizeof pass_node->value,"%s",password);if(tabs[tab_idx].js_page){webpage_js_dispatch(tabs[tab_idx].js_page,pass_node,"input","",0);webpage_js_dispatch(tabs[tab_idx].js_page,pass_node,"change","",0);}}
    }
    memset(password,0,sizeof password);
#endif
}

static int browser_save_current_password(void) {
#ifndef HAVE_RENDER
    return 0;
#else
    if (anonymous_mode || module_db_disabled || active_tab_idx<0 || !tabs[active_tab_idx].dom_tree) return 0;
    char origin[1200], username[256]="", password[2048]=""; browser_origin_for_url(tabs[active_tab_idx].url,origin,sizeof origin); if(!origin[0])return 0;
    DOMNode* head=(DOMNode*)tabs[active_tab_idx].dom_tree;
    int username_too_long = 0;
    for (DOMNode* n = head; n; n = n->next) {
        if (n->type != TAG_INPUT) continue;
        if (!strcmp(n->input_type, "password")) {
            if (n->value[0]) snprintf(password, sizeof password, "%s", n->value);
        } else if (!username[0] && (!strcmp(n->input_type, "text") || !strcmp(n->input_type, "email")) && n->value[0]) {
            size_t user_len = strlen(n->value);
            if (user_len >= sizeof username) { username_too_long = 1; break; }
            memcpy(username, n->value, user_len + 1);
        }
    }
    if (username_too_long) {
        memset(password, 0, sizeof password);
        browser_set_status("Username too long to save safely");
        return 0;
    }
    if (!username[0] || !password[0]) { browser_set_status("No filled username/password fields found"); return 0; }
    int ok=db_password_save(origin,username,password);memset(password,0,sizeof password);browser_set_status(ok?"Login saved to encrypted password vault":"Could not save password (vault crypto unavailable)");return ok;
#endif
}

typedef struct {
    char id[64], name[128], version[32], script[96], matches[512];
} BrowserLocalExtension;

static int browser_extension_id_valid(const char* id) {
    if (!id || !*id || strlen(id) >= 64 || !strcmp(id,".") || !strcmp(id,"..")) return 0;
    for (const unsigned char* p=(const unsigned char*)id; *p; ++p)
        if (!(isalnum(*p) || *p=='_' || *p=='-')) return 0;
    return 1;
}

static int browser_extension_load(const char* id, BrowserLocalExtension* ext) {
    if (!ext || !browser_extension_id_valid(id)) return 0;
    memset(ext,0,sizeof(*ext)); snprintf(ext->id,sizeof ext->id,"%s",id);
    snprintf(ext->name,sizeof ext->name,"%s",id); snprintf(ext->version,sizeof ext->version,"1");
    snprintf(ext->script,sizeof ext->script,"content.js");
    char path[512];
#ifdef _WIN32
    snprintf(path,sizeof path,"extensions\\%s\\manifest.txt",id);
#else
    snprintf(path,sizeof path,"extensions/%s/manifest.txt",id);
#endif
    FILE* f=fopen(path,"rb"); if(!f)return 0;
    char line[600];
    while(fgets(line,sizeof line,f)){
        line[strcspn(line,"\r\n")]=0; char* eq=strchr(line,'=');if(!eq)continue;*eq++=0;
        while(*eq==' '||*eq=='\t')eq++;
        if(!strcmp(line,"name")){snprintf(ext->name,sizeof ext->name,"%.127s",eq);}
        else if(!strcmp(line,"version")){snprintf(ext->version,sizeof ext->version,"%.31s",eq);}
        else if(!strcmp(line,"script")){
            if(!*eq||strstr(eq,"..")||strchr(eq,'/')||strchr(eq,'\\')||strlen(eq)>=sizeof ext->script)continue;
            int safe=1;for(const unsigned char*q=(const unsigned char*)eq;*q;q++)if(!(isalnum(*q)||*q=='.'||*q=='_'||*q=='-'))safe=0;
            if(safe)snprintf(ext->script,sizeof ext->script,"%s",eq);
        }
        else if(!strcmp(line,"match")&&*eq){size_t used=strlen(ext->matches), add=strlen(eq);if(used&&used+1<sizeof ext->matches)ext->matches[used++]=';';if(add<sizeof ext->matches-used){memcpy(ext->matches+used,eq,add+1);}}
    }
    fclose(f);
    if(!ext->matches[0]) return 0;
    return 1;
}

static int browser_extension_enabled(const BrowserLocalExtension* ext) {
    if(!ext || anonymous_mode || module_db_disabled) return 0;
    char key[100];snprintf(key,sizeof key,"extension_enabled_%s",ext->id);
    char* value=db_get_personal_info(key);int enabled=value&&!strcmp(value,"1");free(value);return enabled;
}

static int browser_extension_set_enabled(const BrowserLocalExtension* ext, int enabled) {
    if(!ext || anonymous_mode || module_db_disabled) return 0;
    char key[100];snprintf(key,sizeof key,"extension_enabled_%s",ext->id);
    return db_save_personal_info(key,enabled?"1":"0");
}

static int browser_glob_match_ci(const char* pattern, const char* text) {
    if(!pattern||!text)return 0;
    const char* p=pattern,*t=text,*star=NULL,*retry=NULL;
    while(*t){
        if(*p=='?' || (*p && tolower((unsigned char)*p)==tolower((unsigned char)*t))){p++;t++;continue;}
        if(*p=='*'){star=p++;retry=t;continue;}
        if(star){p=star+1;t=++retry;continue;}
        return 0;
    }
    while(*p=='*')p++;
    return !*p;
}

static int browser_extension_matches_url(const BrowserLocalExtension* ext, const char* url) {
    if(!ext||!url||!ext->matches[0])return 0;
    const char* p=ext->matches;
    while(*p){const char* e=strchr(p,';');size_t n=e?(size_t)(e-p):strlen(p);while(n&&isspace((unsigned char)p[n-1]))n--;while(n&&isspace((unsigned char)*p)){p++;n--;}
        if(n){char pattern[512];if(n>=sizeof pattern)n=sizeof pattern-1;memcpy(pattern,p,n);pattern[n]=0;if(browser_glob_match_ci(pattern,url))return 1;}
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static int browser_extension_collect(BrowserLocalExtension* out, int max_count) {
    if (!out || max_count <= 0) return 0;
    (void)mkdir("extensions", 0755);
    int count=0;
#ifdef _WIN32
    WIN32_FIND_DATAA data;HANDLE h=FindFirstFileA("extensions\\*",&data);
    if(h!=INVALID_HANDLE_VALUE){do{if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))continue;if(!strcmp(data.cFileName,".")||!strcmp(data.cFileName,".."))continue;BrowserLocalExtension ext;if(browser_extension_load(data.cFileName,&ext)&&count<max_count)out[count++]=ext;}while(FindNextFileA(h,&data));FindClose(h);}
#else
    DIR* d=opendir("extensions");if(d){struct dirent* ent;while((ent=readdir(d))!=NULL){if(ent->d_name[0]=='.')continue;char path[512];snprintf(path,sizeof path,"extensions/%s",ent->d_name);struct stat st;if(lstat(path,&st)!=0||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))continue;BrowserLocalExtension ext;if(browser_extension_load(ent->d_name,&ext)&&count<max_count)out[count++]=ext;if(count>=max_count)break;}closedir(d);}
#endif
    return count;
}

static void browser_extensions_handle_url(const char* url) {
    const char* action=url?strstr(url,"action="):NULL;const char* idarg=url?strstr(url,"id="):NULL;
    if (!action || !idarg) return;
    action += 7;
    char action_name[24]={0};
    size_t an=strcspn(action,"&/?#");
    if (!an || an>=sizeof action_name) return;
    memcpy(action_name,action,an);
    if(strcmp(action_name,"toggle"))return;
    idarg+=3;char id[64]={0};size_t n=strcspn(idarg,"&/?#");if(!n||n>=sizeof id)return;memcpy(id,idarg,n);
    BrowserLocalExtension ext;if(!browser_extension_load(id,&ext)){browser_set_status("Extension manifest is invalid or missing");return;}
    if(anonymous_mode||module_db_disabled){browser_set_status("Extension settings require a non-private profile database");return;}
    int enabled=browser_extension_enabled(&ext);if(browser_extension_set_enabled(&ext,!enabled))browser_set_status(!enabled?"Local script extension enabled":"Local script extension disabled");else browser_set_status("Could not save extension preference");
}

static char* browser_make_extensions_html(void) {
    size_t cap=8192,len=0;char* html=malloc(cap);if(!html)return NULL;html[0]=0;
    browser_appendf(&html,&len,&cap,"<h>Local script extensions</h><p>MLBrowser currently supports explicitly enabled local content scripts, not the Chrome/Firefox WebExtensions API. Scripts run in the page JavaScript context at document end, only for matching URLs. Private mode disables them.</p><p><b>Install:</b> create extensions/ID/manifest.txt and content.js, then refresh this page.</p><pre>name=Example script\nversion=1\nmatch=https://example.com/*\nscript=content.js</pre><hr>");
    BrowserLocalExtension exts[64];int count=browser_extension_collect(exts,64);
    if(!count)browser_appendf(&html,&len,&cap,"<p>No valid local scripts found.</p>");
    for(int i=0;i<count;i++){
        char* name=browser_html_escape(exts[i].name);char* ver=browser_html_escape(exts[i].version);char* match=browser_html_escape(exts[i].matches);int enabled=browser_extension_enabled(&exts[i]);
        browser_appendf(&html,&len,&cap,"<section><h3>%s</h3><p>ID: %s · version %s · %s</p><p>URL match: %s</p><p><a href=\"browser://extensions?action=toggle&amp;id=%s\">%s</a></p></section><hr>",name?name:"",exts[i].id,ver?ver:"1",enabled?"Enabled":"Disabled",match?match:"",exts[i].id,enabled?"Disable":"Enable");
        free(name);free(ver);free(match);
    }
    browser_appendf(&html,&len,&cap,"<p><a href=\"browser://profile-backup\">Profile backup and manual transfer</a></p>");return html;
}

static void browser_run_extension_scripts(int tab_idx) {
#ifdef HAVE_RENDER
    if(tab_idx<0||tab_idx>=tab_count||!tabs[tab_idx].js_page||anonymous_mode||module_db_disabled)return;
    if(strncmp(tabs[tab_idx].url,"http://",7) && strncmp(tabs[tab_idx].url,"https://",8))return;
    BrowserLocalExtension exts[64];int count=browser_extension_collect(exts,64);
    for(int i=0;i<count;i++){
        if(!browser_extension_enabled(&exts[i])||!browser_extension_matches_url(&exts[i],tabs[tab_idx].url))continue;
        char path[512];
#ifdef _WIN32
        snprintf(path,sizeof path,"extensions\\%.63s\\%.95s",exts[i].id,exts[i].script);
#else
        snprintf(path,sizeof path,"extensions/%.63s/%.95s",exts[i].id,exts[i].script);
#endif
        FILE*f=fopen(path,"rb");if(!f)continue;
        if(fseek(f,0,SEEK_END)!=0){fclose(f);continue;}long n=ftell(f);if(n<0||n>256*1024){fclose(f);continue;}rewind(f);
        char* script=malloc((size_t)n+32);if(!script){fclose(f);continue;}size_t got=fread(script+13,1,(size_t)n,f);fclose(f);
        /* Wrap in a function to avoid leaking top-level declarations into page code. */
        memcpy(script,"(function(){\n",13);memcpy(script+13+got,"\n})();",7);script[13+got+6]=0;
        char result[256]={0};(void)webpage_js_eval(tabs[tab_idx].js_page,script,result,sizeof result);free(script);
        gal_log("INFO","Local script extension '%s' applied to %.200s",exts[i].id,tabs[tab_idx].url);
    }
#else
    (void)tab_idx;
#endif
}

static void browser_export_profile_backup(void) {
    if(anonymous_mode||module_db_disabled){browser_set_status("Profile backup requires the profile database and is unavailable in private mode");return;}
    char path[4096];if(!browser_choose_save_path(path,sizeof path,"Export MLBrowser profile backup","sqlite"))return;
    if(db_export_profile_backup(path))browser_set_status("Profile backup exported; keep the file private");else browser_set_status("Profile backup export failed");
}
static void browser_import_profile_backup(void) {
    if(anonymous_mode||module_db_disabled){browser_set_status("Profile restore requires the profile database and is unavailable in private mode");return;}
    char path[4096];if(!browser_choose_file(path,sizeof path))return;
    browser_set_status("Validating profile backup…");
    if(db_import_profile_backup(path))browser_set_status("Profile restored; password vault remains device-key encrypted") ;
    else browser_set_status("Import failed: not a valid MLBrowser SQLite profile backup");
}
static char* browser_make_profile_backup_html(void) {
    return strdup("<h>Profile backup / manual transfer</h><p>This is a manual export/import workflow, not cloud synchronization. A backup contains browsing history, bookmarks, session data, cookies, site storage, and preferences. Treat it as sensitive and store it securely.</p><p>The password vault is encrypted with a device-local key; copying a backup to another device does not sync that key or guarantee that saved passwords can be decrypted there.</p><p><button onclick=\"Browser.exportProfileBackup()\">Export profile backup</button></p><p><button onclick=\"Browser.importProfileBackup()\">Import profile backup (replaces local profile data)</button></p><p><a href=\"browser://extensions\">Local script extensions</a></p>");
}

static void browser_devtools_append_network(char** html, size_t* len, size_t* cap) {
    enum { KEEP_LINES = 80, LINE_CAP = 9000 };
    char* lines[KEEP_LINES] = {0}; size_t count=0, next=0;
    FILE* f=fopen("cache/network.log","rb");
    if(f){
        char line[LINE_CAP];
        while(fgets(line,sizeof line,f)){
            size_t n=strlen(line); if(n && line[n-1]=='\n') line[n-1]=0;
            char* copy=strdup(line); if(!copy) continue;
            free(lines[next]); lines[next]=copy; next=(next+1)%KEEP_LINES; if(count<KEEP_LINES)count++;
        }
        fclose(f);
    }
    browser_appendf(html,len,cap,"<h>Network</h><p>Metadata only: request method, status, duration, response bytes and URL. Bodies and credentials are not stored.</p><p><button onclick=\"Browser.refreshDevtools()\">Refresh</button> <button onclick=\"Browser.clearNetworkLog()\">Clear log</button></p><table style=\"width:100%\"><tr><th>Time</th><th>Method</th><th>Status</th><th>ms</th><th>Bytes</th><th>Kind</th><th>URL</th></tr>");
    if(!count) browser_appendf(html,len,cap,"<tr><td colspan=\"7\">No network entries yet. Refresh after loading a page.</td></tr>");
    for(size_t i=0;i<count;i++){
        size_t idx=(next+KEEP_LINES-count+i)%KEEP_LINES; char* row=lines[idx]; if(!row)continue;
        char* fields[7]={0}; fields[0]=row;
        for(int k=1;k<7;k++){char* tab=strchr(fields[k-1],'\t');if(!tab){fields[k]=(char*)"";for(int z=k+1;z<7;z++)fields[z]=(char*)"";break;}*tab=0;fields[k]=tab+1;}
        char* nl=strchr(fields[6],'\r');if(nl)*nl=0;nl=strchr(fields[6],'\n');if(nl)*nl=0;
        char* url=browser_html_escape(fields[6]); char* method=browser_html_escape(fields[1]); char* kind=browser_html_escape(fields[5]);
        browser_appendf(html,len,cap,"<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",fields[0],method?method:"",fields[2],fields[3],fields[4],kind?kind:"",url?url:"");
        free(url);free(method);free(kind);
    }
    browser_appendf(html,len,cap,"</table>");
    for(size_t i=0;i<KEEP_LINES;i++)free(lines[i]);
}

static void browser_refresh_devtools(void);

typedef struct { char pattern[512]; char condition[256]; int line; } BrowserDebugBreakpoint;

static int browser_debug_load_config(int* trace_enabled, BrowserDebugBreakpoint* out, int cap) {
    int trace = 0, count = 0;
    if (out && cap > 0) memset(out, 0, (size_t)cap * sizeof(*out));
    FILE* f = fopen("cache/js_debugger.cfg", "rb");
    if (f) {
        char row[1200];
        while (fgets(row, sizeof(row), f)) {
            row[strcspn(row, "\r\n")] = 0;
            if (!strncmp(row, "TRACE\t", 6)) trace = atoi(row + 6) != 0;
            else if (!strncmp(row, "BP\t", 3) && out && count < cap) {
                char* ln = row + 3; char* tab = strchr(ln, '\t');
                if (!tab) continue;
                *tab++ = 0; int line = atoi(ln);
                char* condition = strchr(tab, '\t');
                if (condition) *condition++ = 0;
                if (line <= 0 || !tab[0]) continue;
                snprintf(out[count].pattern, sizeof(out[count].pattern), "%s", tab);
                if (condition) snprintf(out[count].condition, sizeof(out[count].condition), "%s", condition);
                out[count].line = line; count++;
            }
        }
        fclose(f);
    }
    if (trace_enabled) *trace_enabled = trace;
    return count;
}

static int browser_debug_save_config(int trace_enabled, const BrowserDebugBreakpoint* bps, int count) {
    if (count < 0) count = 0; if (count > 128) count = 128;
    FILE* f = fopen("cache/js_debugger.cfg.tmp", "wb");
    if (!f) return 0;
    fprintf(f, "TRACE\t%d\n", trace_enabled ? 1 : 0);
    for (int i = 0; i < count; i++) {
        char safe[512]; size_t j = 0;
        for (size_t k = 0; bps[i].pattern[k] && j + 1 < sizeof(safe); k++) {
            unsigned char c = (unsigned char)bps[i].pattern[k];
            if (c == '\t' || c == '\r' || c == '\n') continue;
            safe[j++] = (char)c;
        }
        safe[j] = 0;
        char condition[256]; size_t cj = 0;
        for (size_t k = 0; bps[i].condition[k] && cj + 1 < sizeof(condition); k++) {
            unsigned char c = (unsigned char)bps[i].condition[k];
            if (c == '\t' || c == '\r' || c == '\n') continue;
            condition[cj++] = (char)c;
        }
        condition[cj] = 0;
        if (bps[i].line > 0 && safe[0]) fprintf(f, "BP\t%d\t%s\t%s\n", bps[i].line, safe, condition);
    }
    int ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename("cache/js_debugger.cfg.tmp", "cache/js_debugger.cfg") != 0) {
        remove("cache/js_debugger.cfg.tmp"); return 0;
    }
    return 1;
}

static void browser_debug_set_breakpoint(void) {
    if (active_tab_idx < 0 || active_tab_idx >= tab_count || !tabs[active_tab_idx].dom_tree) return;
    DOMNode* src = find_dom_node_by_id_in_tree((DOMNode*)tabs[active_tab_idx].dom_tree, "devtools-bp-file");
    DOMNode* ln = find_dom_node_by_id_in_tree((DOMNode*)tabs[active_tab_idx].dom_tree, "devtools-bp-line");
    DOMNode* cn = find_dom_node_by_id_in_tree((DOMNode*)tabs[active_tab_idx].dom_tree, "devtools-bp-condition");
    char pattern[512] = "", condition[256] = "";
    if (src) snprintf(pattern, sizeof(pattern), "%.*s", (int)sizeof(pattern) - 1, src->value);
    pattern[strcspn(pattern, "\r\n\t")] = 0;
    if (cn) snprintf(condition, sizeof(condition), "%.*s", (int)sizeof(condition)-1, cn->value);
    condition[strcspn(condition, "\r\n\t")] = 0;
    int line = ln ? atoi(ln->value) : 0;
    if (!pattern[0] || line < 1 || line > 1000000) {
        browser_set_status("Breakpoint needs a source substring and a line between 1 and 1000000"); return;
    }
    int trace = 0; BrowserDebugBreakpoint bps[128];
    int count = browser_debug_load_config(&trace, bps, 128);
    for (int i = 0; i < count; i++) {
        if (bps[i].line == line && !strcmp(bps[i].pattern, pattern) && !strcmp(bps[i].condition, condition)) {
            browser_set_status("Breakpoint already exists"); return;
        }
    }
    if (count >= 128) { browser_set_status("Breakpoint limit reached (128)"); return; }
    snprintf(bps[count].pattern, sizeof(bps[count].pattern), "%s", pattern);
    snprintf(bps[count].condition, sizeof(bps[count].condition), "%s", condition);
    bps[count].line = line; count++;
    browser_set_status(browser_debug_save_config(trace, bps, count) ? "JavaScript breakpoint added" : "Could not write debugger config");
    browser_refresh_devtools();
}

static void browser_debug_clear_breakpoints(void) {
    int trace = 0; (void)browser_debug_load_config(&trace, NULL, 0);
    browser_set_status(browser_debug_save_config(trace, NULL, 0) ? "Breakpoints cleared" : "Could not write debugger config");
    browser_refresh_devtools();
}

static void browser_debug_toggle_trace(void) {
    int trace = 0; BrowserDebugBreakpoint bps[128];
    int count = browser_debug_load_config(&trace, bps, 128);
    browser_set_status(browser_debug_save_config(!trace, bps, count) ? (!trace ? "JavaScript line trace enabled" : "JavaScript line trace disabled") : "Could not write debugger config");
    browser_refresh_devtools();
}

static void browser_debug_clear_events(void) {
    FILE* f = fopen("cache/js_debug_events.log", "wb");
    if (f) { fclose(f); browser_set_status("Debugger events cleared"); }
    else browser_set_status("No debugger event log to clear");
    browser_refresh_devtools();
}

static void browser_devtools_append_debug_events(char** html, size_t* len, size_t* cap) {
    enum { KEEP = 60, ROW_CAP = 1800 };
    char* rows[KEEP] = {0}; int count = 0, next = 0;
    FILE* f = fopen("cache/js_debug_events.log", "rb");
    if (f) {
        char row[ROW_CAP];
        while (fgets(row, sizeof(row), f)) {
            row[strcspn(row, "\r\n")] = 0;
            char* cp = strdup(row); if (!cp) continue;
            free(rows[next]); rows[next] = cp; next = (next + 1) % KEEP; if (count < KEEP) count++;
        }
        fclose(f);
    }
    browser_appendf(html, len, cap, "<h>Execution events</h><p>LINE records are source-line trace events. BREAK pauses execution. While paused use F8/C to continue, F11/I to step into, F10/O to step over, and Shift+F11/U to step out. The main UI event loop pauses while JavaScript is stopped.</p><p><button onclick=\"Browser.clearDebuggerEvents()\">Clear events</button> <button onclick=\"Browser.refreshDevtools()\">Refresh events</button></p><table style=\"width:100%%\"><tr><th>Time (ms)</th><th>Type</th><th>Line</th><th>Column</th><th>Source</th><th>Function</th><th>Page</th></tr>");
    if (!count) browser_appendf(html, len, cap, "<tr><td colspan=\"7\">No events recorded yet. Enable line tracing or add a trace breakpoint, then run page JavaScript.</td></tr>");
    for (int i = 0; i < count; i++) {
        int ix = (next - count + i + KEEP) % KEEP; char* row = rows[ix]; if (!row) continue;
        char* field[7] = {0}; field[0] = row;
        for (int k = 1; k < 7; k++) { char* tab = strchr(field[k-1], '\t'); if (!tab) { field[k] = (char*)""; for (int z=k+1; z<7; z++) field[z]=(char*)""; break; } *tab=0; field[k]=tab+1; }
        char* ts=browser_html_escape(field[0]); char* type=browser_html_escape(field[1]);
        char* source=browser_html_escape(field[3]); char* func=browser_html_escape(field[4]);
        char* page=browser_html_escape(field[2]); char* line_num=browser_html_escape(field[5]);
        char* column=browser_html_escape(field[6]);
        browser_appendf(html,len,cap,"<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",ts?ts:"",type?type:"",line_num?line_num:"",column?column:"",source?source:"",func?func:"",page?page:"");
        free(ts); free(type); free(source); free(func); free(page); free(line_num); free(column);
    }
    browser_appendf(html,len,cap,"</table>");
    for (int i=0;i<KEEP;i++) free(rows[i]);
}

static void browser_devtools_append_sources(char** html, size_t* len, size_t* cap, int target_idx) {
    int trace = 0; BrowserDebugBreakpoint bps[128];
    int bp_count = browser_debug_load_config(&trace, bps, 128);
    browser_appendf(html,len,cap,"<h>Sources / JavaScript Debugger</h><p>Matching breakpoints pause page JavaScript. While paused use F8/C to continue, F11/I to step into, F10/O to step over, and Shift+F11/U to step out. The pause overlay shows primitive arguments and locals; object values are placeholders. Conditional breakpoints support a local identifier, !identifier, or primitive comparisons such as count === 3, enabled, or !done. Conditions are evaluated by a restricted native parser and never execute arbitrary JavaScript.</p>");
    browser_appendf(html,len,cap,"<p><button onclick=\"Browser.toggleDebuggerTrace()\">%s line tracing</button> <button onclick=\"Browser.clearDebuggerBreakpoints()\">Clear breakpoints</button></p>",trace?"Disable":"Enable");
    char* bp_default = browser_html_escape(target_idx>=0&&target_idx<tab_count?tabs[target_idx].url:"");
    browser_appendf(html,len,cap,"<h>Breakpoint</h><p>Source filename substring (external script URL or document URL for inline scripts): <input id=\"devtools-bp-file\" type=\"text\" style=\"width:460px\" value=\"%s\"></p><p>Line: <input id=\"devtools-bp-line\" type=\"number\" value=\"1\" min=\"1\" max=\"1000000\"> Condition (optional): <input id=\"devtools-bp-condition\" type=\"text\" style=\"width:300px\" placeholder=\"count === 3\"><button onclick=\"Browser.setDebuggerBreakpoint()\">Add breakpoint</button></p>", bp_default?bp_default:"");
    free(bp_default);
    browser_appendf(html,len,cap,"<h>Breakpoints (%d)</h>",bp_count);
    if(!bp_count) browser_appendf(html,len,cap,"<p>No configured breakpoints.</p>");
    else { browser_appendf(html,len,cap,"<table><tr><th>Source substring</th><th>Line</th><th>Condition</th></tr>"); for(int i=0;i<bp_count;i++){char* ep=browser_html_escape(bps[i].pattern);char* ec=browser_html_escape(bps[i].condition);browser_appendf(html,len,cap,"<tr><td>%s</td><td>%d</td><td>%s</td></tr>",ep?ep:"",bps[i].line,ec?ec:"(always)");free(ep);free(ec);}browser_appendf(html,len,cap,"</table>"); }
    browser_devtools_append_debug_events(html,len,cap);
    if(target_idx<0||target_idx>=tab_count){browser_appendf(html,len,cap,"<p>No target tab.</p>");return;}
    char path[128];snprintf(path,sizeof path,"cache/tab_%d.source",target_idx);FILE*f=fopen(path,"rb");
    if(!f){browser_appendf(html,len,cap,"<p>No cached network source for this tab. Local source view is unavailable.</p>");return;}
    char* src=malloc(40001);if(!src){fclose(f);return;}size_t n=fread(src,1,40000,f);src[n]=0;fclose(f);
    char* esc=browser_html_escape(src);free(src);if(esc){browser_appendf(html,len,cap,"<details><summary>Cached document source (first 40 KB)</summary><pre>%s</pre></details>",esc);free(esc);}
}

static char* browser_make_devtools_html(int target_idx) {
    size_t cap=32768,len=0;char*html=malloc(cap);if(!html)return NULL;html[0]=0;
    char* target_url = browser_html_escape(target_idx>=0&&target_idx<tab_count?tabs[target_idx].url:"no active page");
    browser_appendf(&html,&len,&cap,"<h>Developer Tools</h><p>Inspect: %.1000s</p><p><button onclick=\"Browser.devtoolsElements()\">Elements</button> <button onclick=\"Browser.devtoolsNetwork()\">Network</button> <button onclick=\"Browser.devtoolsDebugger()\">Sources / Runtime</button> <button onclick=\"Browser.refreshDevtools()\">Refresh</button></p><hr>",target_url?target_url:"no active page"); free(target_url);
    browser_appendf(&html,&len,&cap,"<h>Console</h><p><input id=\"devtools-console\" type=\"text\" value=\"document.title\" style=\"width:620px;height:28px\"></p><p><button onclick=\"Browser.runDevtoolsConsole()\">Run JavaScript</button></p><p id=\"devtools-result\">Ready.</p><hr>");
    if(devtools_panel==1) browser_devtools_append_network(&html,&len,&cap);
    else if(devtools_panel==2) browser_devtools_append_sources(&html,&len,&cap,target_idx);
    else {
        browser_appendf(&html,&len,&cap,"<h>Elements</h>");
        if(target_idx>=0&&target_idx<tab_count&&tabs[target_idx].dom_tree){DOMNode*head=(DOMNode*)tabs[target_idx].dom_tree;int shown=0;for(DOMNode*n=head;n&&shown<250;n=n->next,shown++){char tag[32];snprintf(tag,sizeof tag,"%s",n->tag[0]?n->tag:(n->type==TAG_TEXT?"#text":"node"));char*et=browser_html_escape(tag),*ei=browser_html_escape(n->id),*ec=browser_html_escape(n->class_name),*tx=browser_html_escape(n->text);browser_appendf(&html,&len,&cap,"<p>%03d &lt;%s&gt; id=%s class=%s %s</p>",shown,et?et:"",ei?ei:"",ec?ec:"",tx?tx:"");free(et);free(ei);free(ec);free(tx);}if(shown==0)browser_appendf(&html,&len,&cap,"<p>Empty DOM.</p>");}else browser_appendf(&html,&len,&cap,"<p>Target page is no longer available.</p>");
    }
    browser_appendf(&html,&len,&cap,"<hr><p><a href=\"browser://bookmarks\">Bookmarks</a> | <a href=\"browser://passwords\">Password manager</a></p>");return html;
}

static void browser_refresh_devtools(void) {
    if(active_tab_idx<0||!tabs[active_tab_idx].url[0])return;
#ifdef HAVE_RENDER
    find_highlight_node=NULL;
#endif
    char*html=browser_make_devtools_html(devtools_target_tab);if(!html)return;
    if(tabs[active_tab_idx].js_page){webpage_js_free(tabs[active_tab_idx].js_page);tabs[active_tab_idx].js_page=NULL;}
    if(tabs[active_tab_idx].dom_tree){free_dom((DOMNode*)tabs[active_tab_idx].dom_tree);tabs[active_tab_idx].dom_tree=NULL;}
    tabs[active_tab_idx].dom_tree=parse_html(html);free(html);if(tabs[active_tab_idx].dom_tree)css_load_and_apply((DOMNode*)tabs[active_tab_idx].dom_tree,"assets/style.css");needs_redraw=1;
}

static void browser_run_devtools_console(void) {
#ifdef HAVE_RENDER
    if(devtools_target_tab<0||devtools_target_tab>=tab_count||!tabs[devtools_target_tab].js_page){browser_set_status("Target page has no JavaScript context");return;}
    DOMNode* in=find_dom_node_by_id_in_tree((DOMNode*)tabs[active_tab_idx].dom_tree,"devtools-console");char result[2048]="";
    if(!in||!in->value[0])snprintf(result,sizeof result,"Enter an expression first");else webpage_js_eval(tabs[devtools_target_tab].js_page,in->value,result,sizeof result);
    DOMNode* out=find_dom_node_by_id_in_tree((DOMNode*)tabs[active_tab_idx].dom_tree,"devtools-result");if(out)snprintf(out->text,sizeof out->text,"%.500s",result);browser_set_status(result);
#else
    browser_set_status("JavaScript debugger requires the render module");
#endif
}

static int browser_choose_file(char* out, size_t cap) {
    if(!out||cap<2)return 0;out[0]=0;
#ifdef _WIN32
    OPENFILENAMEA ofn; memset(&ofn,0,sizeof ofn); memset(out,0,cap);
    ofn.lStructSize=sizeof ofn; ofn.lpstrFile=out; ofn.nMaxFile=(DWORD)(cap>MAXDWORD?MAXDWORD:cap);
    ofn.lpstrFilter="All files\0*.*\0\0"; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_HIDEREADONLY;
    if(GetOpenFileNameA(&ofn))return 1;
    return 0;
#else
    const char* commands[]={"zenity --file-selection --title='Choose file' 2>/dev/null","kdialog --getopenfilename 2>/dev/null","yad --file-selection --title='Choose file' 2>/dev/null"};
    const char* names[]={"zenity","kdialog","yad"};
    for(size_t i=0;i<sizeof commands/sizeof commands[0];i++){char check[80];snprintf(check,sizeof check,"command -v %s >/dev/null 2>&1",names[i]);if(system(check)!=0)continue;FILE*fp=popen(commands[i],"r");if(!fp)continue;if(!fgets(out,(int)cap,fp)){pclose(fp);continue;}int rc=pclose(fp);out[strcspn(out,"\r\n")]=0;if(rc==0&&out[0])return 1;out[0]=0;}
    browser_set_status("Install zenity, kdialog or yad for the native file picker");return 0;
#endif
}

static int browser_choose_directory(char* out, size_t cap) {
    if (!out || cap < 2) return 0;
    out[0] = 0;
#ifdef _WIN32
    BROWSEINFOA bi; memset(&bi, 0, sizeof(bi));
    bi.lpszTitle = "Choose download folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return 0;
    int ok = SHGetPathFromIDListA(pidl, out) != 0;
    CoTaskMemFree(pidl);
    if (!ok) out[0] = 0;
    return ok && out[0];
#else
    const char* commands[] = {
        "zenity --file-selection --directory --title='Choose download folder' 2>/dev/null",
        "kdialog --getexistingdirectory 2>/dev/null",
        "yad --file-selection --directory --title='Choose download folder' 2>/dev/null"
    };
    const char* names[] = {"zenity", "kdialog", "yad"};
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        char check[80]; snprintf(check, sizeof(check), "command -v %s >/dev/null 2>&1", names[i]);
        if (system(check) != 0) continue;
        FILE* fp = popen(commands[i], "r"); if (!fp) continue;
        if (!fgets(out, (int)(cap > 32767 ? 32767 : cap), fp)) { pclose(fp); continue; }
        int rc = pclose(fp); out[strcspn(out, "\r\n")] = 0;
        if (rc == 0 && out[0]) return 1;
        out[0] = 0;
    }
    browser_set_status("Install zenity, kdialog or yad to choose a download folder");
    return 0;
#endif
}

static int browser_choose_save_path(char* out, size_t cap, const char* title, const char* extension) {
#ifndef _WIN32
    (void)title;
#endif
    if (!out || cap < 2) return 0;
    out[0] = 0;
#ifdef _WIN32
    snprintf(out, cap, "browser-export.%s", extension && *extension ? extension : "bin");
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = out;
    ofn.nMaxFile = (DWORD)(cap > MAXDWORD ? MAXDWORD : cap);
    ofn.lpstrFilter = (extension && !strcmp(extension, "pdf")) ? "PDF files\0*.pdf\0All files\0*.*\0\0" : "All files\0*.*\0\0";
    ofn.lpstrDefExt = extension && *extension ? extension : "bin";
    ofn.lpstrTitle = title; ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    if (GetSaveFileNameA(&ofn)) return 1;
    out[0] = 0; return 0;
#else
    const char* commands[] = {
        "zenity --file-selection --save --confirm-overwrite --title='Save browser output' 2>/dev/null",
        "kdialog --getsavefilename '' 2>/dev/null",
        "yad --file-selection --save --confirm-overwrite --title='Save browser output' 2>/dev/null"
    };
    const char* names[] = {"zenity", "kdialog", "yad"};
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        char check[80]; snprintf(check, sizeof(check), "command -v %s >/dev/null 2>&1", names[i]);
        if (system(check) != 0) continue;
        FILE* fp = popen(commands[i], "r"); if (!fp) continue;
        if (!fgets(out, (int)(cap > 32767 ? 32767 : cap), fp)) { pclose(fp); continue; }
        int rc = pclose(fp); out[strcspn(out, "\r\n")] = 0;
        if (rc == 0 && out[0]) {
            if (extension && *extension) {
                const char* slash = strrchr(out, '/'); const char* dot = strrchr(out, '.');
                if (!dot || (slash && dot < slash)) {
                    size_t n = strlen(out); if (n + strlen(extension) + 2 < cap) { out[n++] = '.'; strcpy(out+n, extension); }
                }
            }
            return 1;
        }
        out[0] = 0;
    }
    browser_set_status("No native save dialog available (install zenity, kdialog or yad)");
    return 0;
#endif
}

static void browser_input_file_click(DOMNode* node) {
    char path[2048];
    if (!node || !browser_choose_file(path, sizeof path)) return;
    snprintf(node->value, sizeof node->value, "%s", path);
    node->file_selected = 1;
#ifdef HAVE_RENDER
    if (node->v2 && active_tab_idx >= 0 && tabs[active_tab_idx].js_page) {
        webpage_js_dispatch(tabs[active_tab_idx].js_page, node, "input", path, 0);
        webpage_js_dispatch(tabs[active_tab_idx].js_page, node, "change", "", 0);
    }
#endif
    browser_set_status("File selected — ready for form upload.");
}


#ifdef HAVE_RENDER
/* The webpage DOM stores parsed attributes as name\1value\2 records. */
static int browser_dom_get_attr(const DOMNode* node, const char* wanted, char* out, size_t cap) {
    if (!node || !wanted || !out || cap < 1) return 0;
    out[0] = 0;
    const char* p = node->attrs;
    while (p && *p) {
        const char* sep = strchr(p, 1);
        if (!sep) break;
        const char* end = strchr(sep + 1, 2);
        if (!end) break;
        size_t name_len = (size_t)(sep - p);
        if (strlen(wanted) == name_len && !strncasecmp(p, wanted, name_len)) {
            size_t value_len = (size_t)(end - sep - 1);
            if (value_len >= cap) value_len = cap - 1;
            memcpy(out, sep + 1, value_len); out[value_len] = 0;
            return 1;
        }
        p = end + 1;
    }
    return 0;
}
#endif

static void browser_download_sanitize_name(char* name) {
    if (!name) return;
    for (unsigned char* p = (unsigned char*)name; *p; ++p) {
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') *p = '_';
    }
    while (name[0] == '.' && (name[1] == 0 || name[1] == '.')) memmove(name, name + 1, strlen(name));
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) snprintf(name, 256, "download.bin");
}

static int browser_hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void browser_filename_percent_decode(const char* in, char* out, size_t cap) {
    size_t o=0; if(!cap)return;
    for(size_t i=0;in&&in[i]&&o+1<cap;i++){
        if(in[i]=='%'&&in[i+1]&&in[i+2]){int a=browser_hexval((unsigned char)in[i+1]),b=browser_hexval((unsigned char)in[i+2]);if(a>=0&&b>=0){unsigned char c=(unsigned char)((a<<4)|b);if(c>=0x20&&c!=0x7f)out[o++]=(char)c;i+=2;continue;}}
        if(in[i]=='+' ) out[o++]='+'; else if((unsigned char)in[i]>=0x20 && (unsigned char)in[i]!=0x7f)out[o++]=in[i];
    }
    out[o]=0;
}
/* Returns 1 only for a successful top-level response explicitly marked attachment. */
static int browser_content_disposition_attachment(const char* headers, int status, char* filename, size_t cap) {
    if(filename&&cap)filename[0]=0;
    if(!headers || status<200 || status>=300 || !filename || cap<2) return 0;
    const char* p=headers;
    while(*p){
        const char* e=strpbrk(p,"\r\n"); size_t n=e?(size_t)(e-p):strlen(p);
        const char* colon=memchr(p,':',n);
        if(colon && (size_t)(colon-p)==19 && !strncasecmp(p,"Content-Disposition",19)){
            const char* v=colon+1; while(v<p+n&&isspace((unsigned char)*v))v++;
            const char* end=p+n; const char* semi=memchr(v,';',(size_t)(end-v)); size_t token=semi?(size_t)(semi-v):(size_t)(end-v);
            while(token&&isspace((unsigned char)v[token-1]))token--;
            if(token!=10||strncasecmp(v,"attachment",10)) return 0;
            char normal[512]="", extended[512]="";
            const char* q=semi?semi+1:end;
            while(q<end){
                while(q<end&&(isspace((unsigned char)*q)||*q==';')) q++;
                if(q>=end) break;
                const char* qe=memchr(q,';',(size_t)(end-q));if(!qe)qe=end;
                const char* eq=memchr(q,'=',(size_t)(qe-q));
                if(eq){const char* key=q;size_t kl=(size_t)(eq-key);while(kl&&isspace((unsigned char)key[kl-1]))kl--;const char* val=eq+1;while(val<qe&&isspace((unsigned char)*val))val++;const char* ve=qe;while(ve>val&&isspace((unsigned char)ve[-1]))ve--;if(ve>val+1&&(*val=='"'||*val=='\'')){char quote=*val++;ve=memchr(val,quote,(size_t)(ve-val));if(!ve)ve=qe;}
                    size_t vl=(size_t)(ve-val);char raw[512];if(vl>=sizeof raw)vl=sizeof raw-1;memcpy(raw,val,vl);raw[vl]=0;
                    if(kl==9&&!strncasecmp(key,"filename*",9)){char* marker=strstr(raw,"''");if(marker){browser_filename_percent_decode(marker+2,extended,sizeof extended);}else browser_filename_percent_decode(raw,extended,sizeof extended);}
                    else if(kl==8&&!strncasecmp(key,"filename",8)){browser_filename_percent_decode(raw,normal,sizeof normal);}
                }
                q=qe<end?qe+1:end;
            }
            snprintf(filename,cap,"%s",extended[0]?extended:normal);browser_download_sanitize_name(filename);return 1;
        }
        if(!e) break;
        p=e+1; if(*p=='\n'||*p=='\r') p++;
    }
    return 0;
}
static int browser_write_attachment_sidecar(int tab_idx, const char* url, const char* filename) {
    if(tab_idx<0||tab_idx>=MAX_TABS||!url||!*url||!filename||!*filename)return 0;
    (void)mkdir("cache",0755);char path[128];snprintf(path,sizeof path,"cache/tab_%d.attachment",tab_idx);
    FILE*f=fopen(path,"wb");if(!f)return 0;
    int ok=fprintf(f,"%s\n%s\n",url,filename)>0; if(fclose(f)!=0)ok=0;return ok;
}
static int browser_read_attachment_sidecar(int tab_idx, char* url, size_t url_cap, char* filename, size_t name_cap) {
    if(tab_idx<0||tab_idx>=MAX_TABS||!url||!url_cap||!filename||!name_cap)return 0;
    char path[128];snprintf(path,sizeof path,"cache/tab_%d.attachment",tab_idx);FILE*f=fopen(path,"rb");if(!f)return 0;
    int ok=fgets(url,(int)(url_cap>INT_MAX?INT_MAX:url_cap),f)!=NULL && fgets(filename,(int)(name_cap>INT_MAX?INT_MAX:name_cap),f)!=NULL;fclose(f);remove(path);
    if(!ok){url[0]=filename[0]=0;return 0;}url[strcspn(url,"\r\n")]=0;filename[strcspn(filename,"\r\n")]=0;
    browser_download_sanitize_name(filename);return url[0]&&filename[0];
}

static void browser_download_name_from_url(const char* url, char* out, size_t cap) {
    if (!out || cap < 2) return;
    out[0] = 0;
    const char* path = url ? url : "";
    const char* scheme = strstr(path, "://");
    if (scheme) {
        path = strchr(scheme + 3, '/');
        if (!path) path = "";
    }
    size_t n = strcspn(path, "?#");
    while (n && path[n - 1] == '/') --n;
    const char* base = path;
    for (size_t i = 0; i < n; ++i) if (path[i] == '/') base = path + i + 1;
    size_t blen = (size_t)(path + n - base);
    if (blen == 0) { snprintf(out, cap, "download.bin"); return; }
    if (blen >= cap) blen = cap - 1;
    size_t o = 0;
    for (size_t i = 0; i < blen && o + 1 < cap; ++i) {
        unsigned char c = (unsigned char)base[i];
        if (c == '%' && i + 2 < blen && isxdigit((unsigned char)base[i+1]) && isxdigit((unsigned char)base[i+2])) {
            char hex[3] = {base[i+1], base[i+2], 0}; c = (unsigned char)strtoul(hex, NULL, 16); i += 2;
        }
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        out[o++] = (char)c;
    }
    out[o] = 0;
    browser_download_sanitize_name(out);
}

static int browser_path_is_directory(const char* path) {
    struct stat st; return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int browser_default_download_directory(char* out, size_t cap) {
    if (!out || cap < 2) return 0;
    const char* home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (home && *home) {
        if (snprintf(out, cap, "%s/Downloads", home) >= (int)cap) return 0;
    } else snprintf(out, cap, "Downloads");
    if (mkdir(out, 0755) != 0 && errno != EEXIST) return 0;
    return browser_path_is_directory(out);
}

static int browser_get_download_directory(char* out, size_t cap) {
    if (!out || cap < 2) return 0;
    if (g_transient_download_dir[0] && browser_path_is_directory(g_transient_download_dir)) {
        snprintf(out, cap, "%s", g_transient_download_dir); return 1;
    }
    if (!anonymous_mode && !module_db_disabled) {
        char* saved = db_get_personal_info("downloads_directory");
        if (saved && browser_path_is_directory(saved)) {
            snprintf(out, cap, "%s", saved); free(saved); return strlen(out) < cap;
        }
        free(saved);
    }
    return browser_default_download_directory(out, cap);
}

static void browser_choose_download_directory(void) {
    char path[4096];
    if (!browser_choose_directory(path, sizeof(path))) return;
    if (!browser_path_is_directory(path)) { browser_set_status("Selected path is not a directory"); return; }
    snprintf(g_transient_download_dir, sizeof(g_transient_download_dir), "%s", path);
    if (!anonymous_mode && !module_db_disabled && !db_save_personal_info("downloads_directory", path)) {
        browser_set_status("Download folder selected for this session; could not persist preference");
    } else browser_set_status("Download folder updated");
}

static void browser_reset_download_directory(void) {
    g_transient_download_dir[0] = 0;
    if (!anonymous_mode && !module_db_disabled) (void)db_save_personal_info("downloads_directory", "");
    browser_set_status("Using default Downloads folder");
}

static int browser_start_download(const char* url, const char* requested_name) {
    if (!url || (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8))) {
        browser_set_status("Downloads require an HTTP or HTTPS URL"); return 0;
    }
    char filename[256];
    if (requested_name && requested_name[0]) snprintf(filename, sizeof(filename), "%s", requested_name);
    else browser_download_name_from_url(url, filename, sizeof(filename));
    browser_download_sanitize_name(filename);

    char dir[4096];
    if (!browser_get_download_directory(dir, sizeof(dir))) {
        browser_set_status("Could not resolve download folder"); return 0;
    }

    char dest[4096];
    int available = 0;
    for (int suffix = 0; suffix < 10000; ++suffix) {
        char unique[320];
        if (suffix == 0) snprintf(unique, sizeof(unique), "%s", filename);
        else {
            const char* dot = strrchr(filename, '.');
            if (dot && dot != filename) snprintf(unique, sizeof(unique), "%.*s (%d)%s", (int)(dot - filename), filename, suffix, dot);
            else snprintf(unique, sizeof(unique), "%s (%d)", filename, suffix);
        }
        if (snprintf(dest, sizeof(dest), "%s/%s", dir, unique) >= (int)sizeof(dest)) break;
        if (access(dest, F_OK) != 0) { available = 1; snprintf(filename, sizeof(filename), "%s", unique); break; }
    }
    if (!available) { browser_set_status("Could not choose a unique download filename"); return 0; }
    int id = nal_download_start(url, dest, filename);
    if (id <= 0) {
        /* Portable fallback for backends without a background worker. */
        browser_set_status("Downloading file…");
        if (!nal_download_file_kind(url, dest, NAL_RESOURCE_DOCUMENT)) {
            browser_set_status("Download failed"); return 0;
        }
        struct stat fs; long long bytes = stat(dest, &fs) == 0 ? (long long)fs.st_size : 0;
        int done_id = (!anonymous_mode && !module_db_disabled) ? db_add_download(url, dest, filename, bytes) : -1;
        if (done_id > 0) db_update_download_progress(done_id, bytes, "complete");
        char status[384]; snprintf(status, sizeof(status), "Download complete: %.250s", filename);
        browser_set_status(status); needs_redraw = 1; return 1;
    }
    char status[384]; snprintf(status, sizeof(status), "Download #%d started: %.250s", id, filename);
    browser_set_status(status);
    needs_redraw = 1;
    return 1;
}

static char* browser_build_source_html(const char* source, const char* source_url) {
    if (!source) source = "(source unavailable)";
    size_t cap = 4096, len = 0; char* html = malloc(cap); if (!html) return NULL; html[0] = 0;
    char* eu = browser_html_escape(source_url ? source_url : "");
    browser_appendf(&html, &len, &cap,
        "<h>Source: %.500s</h><p><a href=\"%s\">Back to page</a></p><hr><pre>",
        eu ? eu : "", eu ? eu : "");
    free(eu);
    const unsigned char* p = (const unsigned char*)source;
    size_t n = strlen(source); if (n > 250000) n = 250000;
    char piece[420]; size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        const char* r = NULL; size_t rn = 1;
        switch (p[i]) {
            case '&': r = "&amp;"; rn = 5; break;
            case '<': r = "&lt;"; rn = 4; break;
            case '>': r = "&gt;"; rn = 4; break;
            case '"': r = "&quot;"; rn = 6; break;
            default: break;
        }
        if (r && used + rn >= sizeof(piece)-1) {
            piece[used] = 0; browser_appendf(&html, &len, &cap, "%s", piece); used = 0;
        }
        if (r) { memcpy(piece + used, r, rn); used += rn; }
        else {
            if (used + 2 >= sizeof(piece)) { piece[used] = 0; browser_appendf(&html, &len, &cap, "%s", piece); used = 0; }
            piece[used++] = (char)p[i];
        }
        if (p[i] == '\n' || used > 320) { piece[used] = 0; browser_appendf(&html, &len, &cap, "%s", piece); used = 0; }
    }
    if (used) { piece[used] = 0; browser_appendf(&html, &len, &cap, "%s", piece); }
    browser_appendf(&html, &len, &cap, "</pre>");
    return html;
}

static int browser_read_tab_source(int index, const char* expected_url, char** out) {
    if (out) *out = NULL;
    if (!out || index < 0 || index >= MAX_TABS) return 0;
    char meta[128], saved_url[512]={0}; snprintf(meta,sizeof meta,"cache/tab_%d.source-url",index);
    FILE* mf=fopen(meta,"r"); if(!mf)return 0; if(!fgets(saved_url,sizeof saved_url,mf)){fclose(mf);return 0;} fclose(mf); saved_url[strcspn(saved_url,"\r\n")]=0;
    if (expected_url && strcmp(saved_url,expected_url)) return 0;
    char path[128]; snprintf(path, sizeof path, "cache/tab_%d.source", index);
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f); if (sz < 0 || sz > 8*1024*1024) { fclose(f); return 0; }
    rewind(f); char* buf = malloc((size_t)sz + 1); if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f); fclose(f); buf[got] = 0; *out = buf; return 1;
}

static void browser_view_source(void) {
#ifdef HAVE_RENDER
    if (active_tab_idx < 0 || active_tab_idx >= tab_count) return;
    char original[512]; snprintf(original, sizeof original, "%s", tabs[active_tab_idx].url);
    if (!strncmp(original, "view-source:", 12)) { browser_set_status("Already viewing source"); return; }
    char* source = NULL;
    if (!strncmp(original, "http://", 7) || !strncmp(original, "https://", 8)) {
        if (!browser_read_tab_source(active_tab_idx, original, &source)) source = network_http_get(original);
    } else {
        FILE* f = fopen(original, "rb");
        if (f) { fseek(f, 0, SEEK_END); long z = ftell(f); rewind(f); if (z >= 0 && z <= 2*1024*1024) { source = malloc((size_t)z+1); if (source) { size_t got=fread(source,1,(size_t)z,f); source[got]=0; } } fclose(f); }
    }
    if (!source) { browser_set_status("Could not load source for this page"); return; }
    char* html = browser_build_source_html(source, original); free(source); if (!html) return;
    if (tabs[active_tab_idx].js_page) { webpage_js_free(tabs[active_tab_idx].js_page); tabs[active_tab_idx].js_page=NULL; }
    if (tabs[active_tab_idx].dom_tree) free_dom((DOMNode*)tabs[active_tab_idx].dom_tree);
    tabs[active_tab_idx].dom_tree = parse_html(html); free(html);
    snprintf(tabs[active_tab_idx].url, sizeof tabs[active_tab_idx].url, "view-source:%.240s", original);
    snprintf(tabs[active_tab_idx].title, sizeof tabs[active_tab_idx].title, "Source: %.110s", original);
    if (tabs[active_tab_idx].dom_tree) css_load_and_apply((DOMNode*)tabs[active_tab_idx].dom_tree, "assets/style.css");
    page_scroll_x=page_scroll_y=0; needs_redraw=1; browser_persist_session(); browser_set_status("Viewing escaped page source");
#else
    browser_set_status("Source viewer requires the render module");
#endif
}

static void browser_pdf_escape_ascii(const char* in, char* out, size_t cap) {
    size_t j=0; if (!cap) return;
    for (size_t i=0; in && in[i] && j+2<cap; i++) {
        unsigned char c=(unsigned char)in[i];
        if (c=='\\' || c=='(' || c==')') { out[j++]='\\'; out[j++]=(char)c; }
        else if (c=='\n' || c=='\r' || c=='\t') out[j++]=' ';
        else if (c >= 32 && c < 127) out[j++]=(char)c;
        else if (c >= 128) { /* Standard PDF Helvetica has no UTF-8 mapping. */ if ((c & 0xC0) != 0x80) out[j++]='?'; }
        else out[j++]=' ';
    }
    out[j]=0;
}

static int browser_write_text_pdf(const char* path, const char* title, const char* url) {
    enum { MAX_LINES=400, LINE_CAP=192, LINES_PER_PAGE=48, MAX_PAGES=9 };
    char lines[MAX_LINES][LINE_CAP]; int nlines=0;
    snprintf(lines[nlines++],LINE_CAP,"%s",title&&*title?title:"Meetlookers page export");
    if (url && *url && nlines<MAX_LINES) snprintf(lines[nlines++],LINE_CAP,"URL: %.170s",url);
    if (active_tab_idx>=0 && active_tab_idx<tab_count && tabs[active_tab_idx].dom_tree) {
        for (DOMNode*n=(DOMNode*)tabs[active_tab_idx].dom_tree;n&&nlines<MAX_LINES;n=n->next) {
            const char* text=n->text;
            if ((!text||!*text) && (n->type==TAG_INPUT||n->type==TAG_TEXTAREA)) text=n->value;
            if (!text||!*text) continue;
            size_t at=0, len=strlen(text);
            while (at<len && nlines<MAX_LINES) {
                size_t take=len-at; if(take>LINE_CAP-1)take=LINE_CAP-1;
                while(take>1 && at+take<len && (((unsigned char)text[at+take]&0xC0)==0x80))take--;
                memcpy(lines[nlines],text+at,take);lines[nlines][take]=0;nlines++;at+=take;
            }
        }
    }
    int pages=(nlines+LINES_PER_PAGE-1)/LINES_PER_PAGE;if(pages<1)pages=1;if(pages>MAX_PAGES)pages=MAX_PAGES;
    FILE*f=fopen(path,"wb");if(!f)return 0;
    long offsets[4+MAX_PAGES*2]; memset(offsets,0,sizeof offsets);
    fprintf(f,"%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");
    offsets[1]=ftell(f);fprintf(f,"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    offsets[2]=ftell(f);fprintf(f,"2 0 obj\n<< /Type /Pages /Count %d /Kids [",pages);for(int p=0;p<pages;p++)fprintf(f,"%d 0 R ",4+p*2);fprintf(f,"] >>\nendobj\n");
    offsets[3]=ftell(f);fprintf(f,"3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");
    for(int p=0;p<pages;p++){
        int page_obj=4+p*2, content_obj=page_obj+1;char stream[18000];size_t used=0;
        used+=(size_t)snprintf(stream+used,sizeof(stream)-used,"BT\n/F1 10 Tf\n50 790 Td\n13 TL\n");
        int begin=p*LINES_PER_PAGE, end=begin+LINES_PER_PAGE;if(end>nlines)end=nlines;
        for(int i=begin;i<end&&used+300<sizeof(stream);i++){char esc[LINE_CAP*2];browser_pdf_escape_ascii(lines[i],esc,sizeof esc);used+=(size_t)snprintf(stream+used,sizeof(stream)-used,"(%s) Tj\nT*\n",esc);}
        used+=(size_t)snprintf(stream+used,sizeof(stream)-used,"ET\n");
        offsets[page_obj]=ftell(f);fprintf(f,"%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>\nendobj\n",page_obj,content_obj);
        offsets[content_obj]=ftell(f);fprintf(f,"%d 0 obj\n<< /Length %zu >>\nstream\n",content_obj,used);fwrite(stream,1,used,f);fprintf(f,"endstream\nendobj\n");
    }
    long xref=ftell(f);int objects=3+pages*2;fprintf(f,"xref\n0 %d\n0000000000 65535 f \n",objects+1);for(int id=1;id<=objects;id++)fprintf(f,"%010ld 00000 n \n",offsets[id]);fprintf(f,"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n",objects+1,xref);fclose(f);return 1;
}

static void browser_print_pdf(void) {
    if (active_tab_idx<0 || active_tab_idx>=tab_count) { browser_set_status("No active page to print"); return; }
    time_t now=time(NULL); char path[4096], temp[256];
#ifdef _WIN32
    int process_id=(int)_getpid();
#else
    int process_id=(int)getpid();
#endif
    if(!browser_choose_save_path(path,sizeof path,"Save printed page as PDF","pdf")) return;
    snprintf(temp,sizeof temp,"cache/print-%ld-%d.html",(long)now,process_id);
    mkdir("cache",0755);
    FILE*f=fopen(temp,"wb");
    if(f){
        char* original=NULL;
        const char* current_url=tabs[active_tab_idx].url;
        int use_original = (!strncmp(current_url,"http://",7)||!strncmp(current_url,"https://",8)) &&
                           browser_read_tab_source(active_tab_idx,current_url,&original);
        if(use_original){
            /* Keep the page's real markup/styles for pagination; the Python fetcher below
               restricts subresources to same-origin HTTP(S) and data: (never file:). */
            size_t n=strlen(original); if(n>8*1024*1024)n=8*1024*1024;
            (void)fwrite(original,1,n,f); free(original);
        } else {
            char* title=browser_html_escape(tabs[active_tab_idx].title);char* url=browser_html_escape(current_url);
            fprintf(f,"<!doctype html><html><head><meta charset=\"utf-8\"><title>%s</title><style>body{font:12pt sans-serif;max-width:860px;margin:36px auto;line-height:1.45;color:#111}h1{font-size:20pt}p{margin:.35em 0;overflow-wrap:anywhere}.meta{color:#555;font-size:9pt;border-bottom:1px solid #ccc;padding-bottom:12px}</style></head><body><h1>%s</h1><p class=\"meta\">%s</p>",title?title:"",title?title:"",url?url:"");free(title);free(url);
            if(tabs[active_tab_idx].dom_tree){for(DOMNode*n=(DOMNode*)tabs[active_tab_idx].dom_tree;n;n=n->next){const char*t=n->text;if((!t||!*t)&&(n->type==TAG_INPUT||n->type==TAG_TEXTAREA))t=n->value;if(!t||!*t)continue;char*e=browser_html_escape(t);if(e){fprintf(f,"<p>%s</p>",e);free(e);}}}
            fprintf(f,"</body></html>");
        }
        if(fclose(f)!=0)f=NULL;
    }
    int ok=0;
#ifndef _WIN32
    if(f){
        pid_t pid=fork();
        if(pid==0){execlp("python3","python3","tools/print_html_to_pdf.py",temp,path,tabs[active_tab_idx].url,(char*)NULL);_exit(127);}
        if(pid>0){int st=0;while(waitpid(pid,&st,0)<0&&errno==EINTR){};struct stat sb;if(WIFEXITED(st)&&WEXITSTATUS(st)==0&&stat(path,&sb)==0&&sb.st_size>100)ok=1;}
    }
#endif
    if(!ok)ok=browser_write_text_pdf(path,tabs[active_tab_idx].title,tabs[active_tab_idx].url);
    remove(temp);
    if(ok){char msg[256];snprintf(msg,sizeof msg,"PDF saved: %.220s",path);browser_set_status(msg);}else browser_set_status("PDF export failed: install Python/WeasyPrint or check permissions");
}

static void browser_restore_session(void) {
    if (anonymous_mode || module_db_disabled) { browser_set_status("Session storage is disabled"); return; }
    char urls[MAX_TABS][256]; int active_flags[MAX_TABS] = {0}; int any = 0, chosen = -1;
    memset(urls, 0, sizeof urls);
    for (int i = 0; i < MAX_TABS; i++) {
        if (db_session_get_url(i, urls[i], sizeof urls[i], &active_flags[i]) && urls[i][0]) any++;
    }
    if (!any) { browser_set_status("No saved session found"); return; }
    /* Cache all URLs before loading; loading one tab persists the session. */
    for (int i = 0; i < MAX_TABS; i++) {
        if (!urls[i][0]) continue;
        if (i < tab_count) clean_and_load_url(i, urls[i], 0);
        else if (tab_count < MAX_TABS) browser_create_tab_with_url(urls[i], 0);
        if (i < tab_count && active_flags[i]) chosen = i;
    }
    if (chosen >= 0 && chosen < tab_count) active_tab_idx = chosen;
    if (tab_count > 0) browser_persist_session();
    browser_set_status("Restored saved tabs");
}

#ifdef HAVE_RENDER
/* Создаёт JS-страницу для только что загруженного DOM вкладки и исполняет скрипты. */
static void attach_js(int index, const char* url, PageBundle* pb) {
    DOMNode* d = (DOMNode*)tabs[index].dom_tree;
    if (!d || !d->v2) { webpage_bundle_free(pb); return; }
    tabs[index].js_page = webpage_js_attach(d, (pb && pb->base[0]) ? pb->base : url, pb);
}

static int browser_navigate_form(int index, const WebFormSubmission* request, int record_history) {
    if (!request || !request->url[0]) return 0;
    if (strcmp(request->method, "POST") != 0) {
        clean_and_load_url(index, request->url, record_history);
        return 1;
    }
    if (strncmp(request->url, "http://", 7) != 0 && strncmp(request->url, "https://", 8) != 0) {
        browser_set_status("POST forms are supported only for HTTP and HTTPS URLs");
        return 0;
    }
    g_pending_form_nav = 1;
    snprintf(g_pending_form_url, sizeof g_pending_form_url, "%s", request->url);
    snprintf(g_pending_form_method, sizeof g_pending_form_method, "POST");
    snprintf(g_pending_form_headers, sizeof g_pending_form_headers, "%s", request->headers);
    g_pending_form_body = request->body;
    g_pending_form_body_len = request->body_len;
    clean_and_load_url(index, request->url, record_history);
    g_pending_form_nav = 0;
    g_pending_form_url[0] = g_pending_form_method[0] = g_pending_form_headers[0] = 0;
    g_pending_form_body = NULL;
    g_pending_form_body_len = 0;
    return 1;
}
#endif

void load_url_in_tab(int index, const char* target_url, int record_history) {
#ifdef HAVE_RENDER
    find_highlight_node = NULL;
#endif
#ifdef _WIN32
    int sync_attachment = 0;
    char sync_attachment_name[256] = {0};
    char sync_attachment_url[NAL_SYNC_URL_MAX] = {0};
#endif
    if (target_url && strncmp(target_url, "view-source:", 12) != 0) {
        char sidecar[128]; snprintf(sidecar, sizeof sidecar, "cache/tab_%d.attachment", index); remove(sidecar);
    }
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

    if (strncmp(target_url, "view-source:", 12) == 0) {
        const char* original = target_url + 12;
        char* source = NULL;
        if (index >= 0 && index < MAX_TABS && (!strncmp(original,"http://",7) || !strncmp(original,"https://",8))) {
            if (!browser_read_tab_source(index, original, &source)) source = network_http_get(original);
        } else if (original[0] == '/') {
            FILE* sf = fopen(original, "rb");
            if (sf) { fseek(sf,0,SEEK_END); long sz=ftell(sf); rewind(sf); if(sz>=0&&sz<=2*1024*1024){source=malloc((size_t)sz+1);if(source){size_t got=fread(source,1,(size_t)sz,sf);source[got]=0;}} fclose(sf); }
        }
        if (source) {
            char* source_html = browser_build_source_html(source, original); free(source);
            if (source_html) { tabs[index].dom_tree=parse_html(source_html); free(source_html); }
            if (tabs[index].dom_tree) css_load_and_apply((DOMNode*)tabs[index].dom_tree,"assets/style.css");
        } else {
            tabs[index].dom_tree=parse_html("<h>Source unavailable</h><p>The source was not cached and could not be fetched.</p>");
        }
        tabs[index].is_loading=0; loading_progress=0;
    } else if (strncmp(target_url, "http://", 7) == 0 || strncmp(target_url, "https://", 8) == 0) {
        tabs[index].is_loading = 1;
        tabs[index].loading_start_time = (long)time(NULL);

#ifndef _WIN32
        pid_t pid = fork();
        if (pid == 0) {
            // Child process: set the top-level security context before the first HTTP request.
            nal_set_document_url(target_url);
            char response_url[NAL_SYNC_URL_MAX]; snprintf(response_url, sizeof response_url, "%s", target_url);
            char response_headers[16384] = {0}; int response_status = 0;
            char* page_source = NULL;
#ifdef HAVE_RENDER
            if (g_pending_form_nav && !strcmp(g_pending_form_method, "POST") && !strcmp(g_pending_form_url, target_url)) {
                NalSyncResponse response;
                if (nal_sync_fetch_ex("POST", target_url, g_pending_form_headers, g_pending_form_body,
                                      g_pending_form_body_len, NULL, &response)) {
                    page_source = malloc(response.body_len + 1);
                    if (page_source) {
                        if (response.body_len) memcpy(page_source, response.body, response.body_len);
                        page_source[response.body_len] = 0;
                        if (response.final_url[0]) snprintf(response_url, sizeof response_url, "%s", response.final_url);
                        if (response.headers) snprintf(response_headers, sizeof response_headers, "%s", response.headers);
                        response_status = response.status;
                        nal_set_document_url(response_url);
                    }
                    nal_sync_response_free(&response);
                } else gal_log("WARN", "Form POST failed: %s", response.error);
            } else
#endif
            if (!page_source) page_source = network_http_get(target_url);
            if (page_source) {
                if (!response_headers[0]) (void)nal_get_last_response_metadata(response_headers, sizeof response_headers, response_url, sizeof response_url, &response_status);
                char attachment_name[256];
                if (browser_content_disposition_attachment(response_headers, response_status, attachment_name, sizeof attachment_name)) {
                    (void)browser_write_attachment_sidecar(index, response_url, attachment_name);
                    free(page_source);
                    _exit(0);
                }
                mkdir("cache", 0755);
                char source_name[256]; snprintf(source_name, sizeof source_name, "cache/tab_%d.source", index);
                FILE* sf = fopen(source_name, "wb"); if (sf) { fwrite(page_source, 1, strlen(page_source), sf); fclose(sf); }
                char source_url_name[256]; snprintf(source_url_name,sizeof source_url_name,"cache/tab_%d.source-url",index);
                sf=fopen(source_url_name,"w");if(sf){fprintf(sf,"%s\n",response_url);fclose(sf);}
                const char* effective_doc_url = nal_current_document_url();
                if (!effective_doc_url || !*effective_doc_url) effective_doc_url = response_url;
                DOMNode* tree = parse_document(page_source, response_url[0] ? effective_doc_url : target_url);
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
        // Windows: synchronous navigation; establish CSP/HSTS context before requesting the document.
        nal_set_document_url(target_url);
        char response_url[NAL_SYNC_URL_MAX]; snprintf(response_url, sizeof response_url, "%s", target_url);
        char response_headers[16384] = {0}; int response_status = 0;
        char* page_source = NULL;
#ifdef HAVE_RENDER
        if (g_pending_form_nav && !strcmp(g_pending_form_method, "POST") && !strcmp(g_pending_form_url, target_url)) {
            NalSyncResponse response;
            if (nal_sync_fetch_ex("POST", target_url, g_pending_form_headers, g_pending_form_body,
                                  g_pending_form_body_len, NULL, &response)) {
                page_source = malloc(response.body_len + 1);
                if (page_source) { if (response.body_len) memcpy(page_source, response.body, response.body_len); page_source[response.body_len] = 0;
                    if (response.final_url[0]) snprintf(response_url, sizeof response_url, "%s", response.final_url);
                    if (response.headers) snprintf(response_headers, sizeof response_headers, "%s", response.headers);
                    response_status = response.status; nal_set_document_url(response_url); }
                nal_sync_response_free(&response);
            } else gal_log("WARN", "Form POST failed: %s", response.error);
        } else
#endif
        page_source = network_http_get(target_url);
        if (page_source) {
            if (!response_headers[0]) (void)nal_get_last_response_metadata(response_headers, sizeof response_headers, response_url, sizeof response_url, &response_status);
            if (browser_content_disposition_attachment(response_headers, response_status, sync_attachment_name, sizeof sync_attachment_name)) {
                snprintf(sync_attachment_url, sizeof sync_attachment_url, "%s", response_url);
                free(page_source); page_source=NULL; sync_attachment=1;
                (void)browser_start_download(sync_attachment_url, sync_attachment_name);
            }
        }
        if (page_source) {
            mkdir("cache", 0755);
            char source_name[256]; snprintf(source_name, sizeof source_name, "cache/tab_%d.source", index);
            FILE* sf=fopen(source_name,"wb");if(sf){fwrite(page_source,1,strlen(page_source),sf);fclose(sf);}
            char source_url_name[256];snprintf(source_url_name,sizeof source_url_name,"cache/tab_%d.source-url",index);sf=fopen(source_url_name,"w");if(sf){fprintf(sf,"%s\n",response_url);fclose(sf);}
            if (tabs[index].dom_tree) free_dom((DOMNode*)tabs[index].dom_tree);
            const char* effective_doc_url = nal_current_document_url();
            if (!effective_doc_url || !*effective_doc_url) effective_doc_url = response_url;
            tabs[index].dom_tree = parse_document(page_source, effective_doc_url);
            attach_js(index, effective_doc_url, webpage_bundle_take());
            browser_apply_password_autofill(index);
            browser_run_extension_scripts(index);
            free(page_source);
        }
        tabs[index].is_loading = 0;
        if (sync_attachment) { loading_progress=0; clean_and_load_url(index, "browser://downloads", 0); return; }
#endif
    } else if (strncmp(target_url, "browser://", 10) == 0) {
        char* content = NULL;
        if (strcmp(target_url, "browser://history") == 0) {
            content = db_generate_history_html();
        } else if (strcmp(target_url, "browser://settings") == 0) {
            content = db_generate_profile_html();
        } else if (strncmp(target_url, "browser://downloads", 19) == 0) {
            /* Internal links use a strict action/id allowlist. */
            const char* action = strstr(target_url, "action=");
            const char* idarg = strstr(target_url, "id=");
            if (action && idarg) {
                action += 7;
                char action_name[24] = {0}; size_t an = strcspn(action, "&/?#");
                if (an > 0 && an < sizeof action_name) {
                    memcpy(action_name, action, an);
                    char* endp = NULL; long parsed = strtol(idarg + 3, &endp, 10);
                    if (parsed > 0 && parsed <= 2147483647L && endp != idarg + 3 && (*endp == 0 || *endp == '&' || *endp == '#')) {
                        int did = (int)parsed;
                        if (!strcmp(action_name, "pause")) (void)nal_download_pause(did);
                        else if (!strcmp(action_name, "continue")) (void)nal_download_continue(did);
                        else if (!strcmp(action_name, "cancel")) (void)nal_download_cancel(did);
                        else if (!strcmp(action_name, "resume")) {
                            if (!nal_download_continue(did)) (void)nal_download_resume_start(did);
                        }
                    }
                }
                /* Strip one-shot action parameters so refreshes never repeat pause/cancel/resume. */
                snprintf(tabs[index].url, sizeof(tabs[index].url), "browser://downloads");
            }
            content = db_generate_downloads_html();
        } else if (strcmp(target_url, "browser://bookmarks") == 0) {
            content = db_generate_bookmarks_html();
        } else if (strcmp(target_url, "browser://passwords") == 0) {
            content = db_generate_passwords_html();
        } else if (!strncmp(target_url, "browser://extensions", 20)) {
            browser_extensions_handle_url(target_url);
            snprintf(tabs[index].url, sizeof tabs[index].url, "browser://extensions");
            content = browser_make_extensions_html();
        } else if (!strncmp(target_url, "browser://profile-backup", 24)) {
            content = browser_make_profile_backup_html();
        } else if (strcmp(target_url, "browser://devtools") == 0) {
            content = browser_make_devtools_html(devtools_target_tab);
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
                browser_run_extension_scripts(index);
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
            browser_run_extension_scripts(index);
            free(start_html);
        }
    }
    loading_progress = 0;
#endif

#ifdef HAVE_RENDER
    if (tabs[index].dom_tree && fabsf(tabs[index].zoom - 1.0f) > 0.001f) {
        for (DOMNode* zn=(DOMNode*)tabs[index].dom_tree; zn; zn=zn->next) browser_scale_node(zn,tabs[index].zoom);
        webpage_invalidate_layout(); webpage_force_layout_for((DOMNode*)tabs[index].dom_tree);
    }
#endif
    if (tabs[index].dom_tree) browser_apply_password_autofill(index);

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
    
    char buf[1200];
    strncpy(buf, target_url, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

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
    browser_persist_session();
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

static void browser_debugger_redraw_pause_tab(void) {
#ifdef HAVE_UI
    int idx = browser_debugger_display_tab_idx;
    if (idx < 0 || idx >= tab_count) idx = active_tab_idx;
    if (idx < 0 || idx >= tab_count) return;
    int saved_scroll_x = page_scroll_x, saved_scroll_y = page_scroll_y;
    int render_scroll_x = idx == active_tab_idx ? saved_scroll_x : 0;
    int render_scroll_y = idx == active_tab_idx ? saved_scroll_y : 0;
    gal_clear(0xFFFFFF);
#ifdef HAVE_RENDER
    if (tabs[idx].is_loading) {
        gal_draw_string(100, 200, "Loading...", 0x4B5563, 2);
    } else if (tabs[idx].dom_tree) {
        layout_and_render((DOMNode*)tabs[idx].dom_tree, 80, tabs[idx].url, render_scroll_x, render_scroll_y);
        page_content_width = layout_content_width;
        page_content_height = layout_content_height;
        browser_draw_find_highlight(80);
    }
#endif
    int top = draw_browser_frame(tabs, tab_count, idx);
    if (idx != active_tab_idx) { page_scroll_x = 0; page_scroll_y = 0; }
    draw_scrollbars(top);
    browser_draw_find_bar();
    gal_flush();
    page_scroll_x = saved_scroll_x; page_scroll_y = saved_scroll_y;
#endif
}

static void browser_create_tab_with_url(const char* url, int record_history) {
    if (tab_count >= MAX_TABS) return;
    int idx = tab_count;
    memset(&tabs[idx], 0, sizeof tabs[idx]);
    tabs[idx].history_index = -1;
    tabs[idx].history_count = 0;
    tabs[idx].zoom = 1.0f;
    tabs[idx].loading_pid = 0;
    active_tab_idx = idx;
    tab_count++;
    clean_and_load_url(idx, (url && *url) ? url : "localhost/home", record_history);
    browser_persist_session();
}

void create_new_tab(void) { browser_create_tab_with_url("localhost/home", 1); }

void close_tab(int index) {
    if (index < 0 || index >= tab_count) return;
#ifdef HAVE_RENDER
    find_highlight_node = NULL;
#endif
    if (devtools_target_tab == index) devtools_target_tab = -1;
    else if (devtools_target_tab > index) devtools_target_tab--;

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
    browser_persist_session();
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
    else if (strcmp(cmd, "Browser.bookmarkCurrent()") == 0 || strcmp(cmd, "Browser.toggleBookmark()") == 0) {
        if (active_tab_idx >= 0 && !anonymous_mode && !module_db_disabled) {
            const char* u=tabs[active_tab_idx].url; int ok;
            if (db_bookmark_exists(u)) { db_bookmark_delete_url(u); browser_set_status("Bookmark removed"); ok=1; }
            else { ok=db_bookmark_add(u,tabs[active_tab_idx].title); browser_set_status(ok?"Page bookmarked":"Could not save bookmark"); }
            (void)ok;
        } else browser_set_status("Bookmarks are unavailable in private/DB-disabled mode");
    }
    else if (strcmp(cmd, "Browser.openBookmarks()") == 0) {
        if (active_tab_idx >= 0) clean_and_load_url(active_tab_idx,"browser://bookmarks",1);
    }
    else if (strcmp(cmd, "Browser.openPasswordManager()") == 0) {
        if (active_tab_idx >= 0) clean_and_load_url(active_tab_idx,"browser://passwords",1);
    }
    else if (strcmp(cmd, "Browser.openExtensions()") == 0) { if(active_tab_idx>=0) clean_and_load_url(active_tab_idx,"browser://extensions",1); }
    else if (strcmp(cmd, "Browser.openProfileBackup()") == 0) { if(active_tab_idx>=0) clean_and_load_url(active_tab_idx,"browser://profile-backup",1); }
    else if (strcmp(cmd, "Browser.exportProfileBackup()") == 0) browser_export_profile_backup();
    else if (strcmp(cmd, "Browser.importProfileBackup()") == 0) browser_import_profile_backup();
    else if (strcmp(cmd, "Browser.saveCurrentPassword()") == 0) {
        browser_save_current_password();
    }
    else if (strncmp(cmd, "Browser.removeBookmark(", 23) == 0) {
        int id=atoi(cmd+23); int ok=(!anonymous_mode&&!module_db_disabled)&&db_bookmark_delete(id);
        if (active_tab_idx>=0 && !strcmp(tabs[active_tab_idx].url,"browser://bookmarks")) clean_and_load_url(active_tab_idx,"browser://bookmarks",0);
        browser_set_status(ok?"Bookmark removed":"Could not remove bookmark");
    }
    else if (strncmp(cmd, "Browser.removePassword(", 23) == 0) {
        int id=atoi(cmd+23); int ok=(!anonymous_mode&&!module_db_disabled)&&db_password_delete(id);
        if (active_tab_idx>=0 && !strcmp(tabs[active_tab_idx].url,"browser://passwords")) clean_and_load_url(active_tab_idx,"browser://passwords",0);
        browser_set_status(ok?"Saved login removed":"Could not remove saved login");
    }
    else if (strcmp(cmd, "Browser.findInPage()") == 0) {
        find_bar_open=1; find_bar_focused=1; find_match_index=-1;
#ifdef HAVE_RENDER
        browser_find_next(0);
#endif
    }
    else if (strcmp(cmd, "Browser.zoomIn()") == 0) {
        if(active_tab_idx>=0) browser_zoom_active((tabs[active_tab_idx].zoom>0.1f?tabs[active_tab_idx].zoom:1.0f)*1.1f);
    }
    else if (strcmp(cmd, "Browser.zoomOut()") == 0) {
        if(active_tab_idx>=0) browser_zoom_active((tabs[active_tab_idx].zoom>0.1f?tabs[active_tab_idx].zoom:1.0f)/1.1f);
    }
    else if (strcmp(cmd, "Browser.zoomReset()") == 0) browser_zoom_active(1.0f);
    else if (strcmp(cmd, "Browser.printPDF()") == 0) browser_print_pdf();
    else if (strcmp(cmd, "Browser.viewSource()") == 0) browser_view_source();
    else if (strcmp(cmd, "Browser.openDevtools()") == 0) {
        if(active_tab_idx>=0 && tab_count<MAX_TABS) { devtools_target_tab=active_tab_idx; devtools_panel=0; browser_create_tab_with_url("browser://devtools",1); }
        else browser_set_status("No free tab for developer tools");
    }
    else if (strcmp(cmd, "Browser.refreshDevtools()") == 0) browser_refresh_devtools();
    else if (strcmp(cmd, "Browser.runDevtoolsConsole()") == 0) browser_run_devtools_console();
    else if (strcmp(cmd, "Browser.restoreSession()") == 0) browser_restore_session();
    else if (strcmp(cmd, "Browser.chooseDownloadDirectory()") == 0) browser_choose_download_directory();
    else if (strcmp(cmd, "Browser.resetDownloadDirectory()") == 0) browser_reset_download_directory();
    else if (strcmp(cmd, "Browser.devtoolsElements()") == 0) { devtools_panel=0; browser_refresh_devtools(); }
    else if (strcmp(cmd, "Browser.devtoolsNetwork()") == 0) { devtools_panel=1; browser_refresh_devtools(); }
    else if (strcmp(cmd, "Browser.devtoolsDebugger()") == 0) { devtools_panel=2; browser_refresh_devtools(); }
    else if (strcmp(cmd, "Browser.setDebuggerBreakpoint()") == 0) browser_debug_set_breakpoint();
    else if (strcmp(cmd, "Browser.clearDebuggerBreakpoints()") == 0) browser_debug_clear_breakpoints();
    else if (strcmp(cmd, "Browser.toggleDebuggerTrace()") == 0) browser_debug_toggle_trace();
    else if (strcmp(cmd, "Browser.clearDebuggerEvents()") == 0) browser_debug_clear_events();
    else if (strcmp(cmd, "Browser.clearNetworkLog()") == 0) { nal_monitor_clear(); browser_set_status("Network log cleared"); browser_refresh_devtools(); }
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
            browser_persist_session();
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
    if (!module_db_disabled && db_init()) db_download_recover_stale();
    font_manager_init();
    load_nav_template();
    const char* startup_url = NULL;
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') { startup_url = argv[i]; break; }
    if (startup_url) browser_create_tab_with_url(startup_url, 1);
    else {
        browser_restore_session();
        if (tab_count == 0) create_new_tab();
    }

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
    int dom_mouse_buttons = 0;
    int running = 1;
    while (running) { if (browser_debugger_quit_requested) { running = 0; break; }
#if defined(HAVE_UI) && defined(HAVE_RENDER)
        /* Never traverse a partially copied DOM or dispatch input to it. Each
           poll applies one bounded node/link batch; the previous framebuffer is
           kept untouched until every node and next link is complete. */
        if (browser_dom_commit_pending()) {
            browser_dom_commit_advance();
            if (browser_dom_commit_pending()) { usleep(1000); continue; }
            needs_redraw = 1;
        }
#endif
#ifdef HAVE_UI
        if (browser_debugger_pending_tab_idx >= 0) {
            if (browser_debugger_pending_tab_idx < tab_count) active_tab_idx = browser_debugger_pending_tab_idx;
            browser_debugger_pending_tab_idx = -1;
            browser_persist_session();
            needs_redraw = 1;
        }
#endif
        static int first=1; if(first){printf("LOOP START\n");fflush(stdout);first=0;}
#ifdef HAVE_NET
        nal_download_poll();
#endif
#if defined(HAVE_UI) && defined(HAVE_RENDER) && defined(HAVE_NET)
        /* Keep browser://downloads live without causing the action URL to execute again. */
        static long last_downloads_refresh_ms = 0;
        long now_downloads_ms = get_time_ms();
        if (active_tab_idx >= 0 && active_tab_idx < tab_count &&
            !strcmp(tabs[active_tab_idx].url, "browser://downloads") &&
            now_downloads_ms - last_downloads_refresh_ms >= 1000) {
            last_downloads_refresh_ms = now_downloads_ms;
            char* downloads_page = db_generate_downloads_html();
            if (downloads_page) {
                DOMNode* refreshed = parse_html(downloads_page);
                if (refreshed) {
                    if (tabs[active_tab_idx].dom_tree) free_dom((DOMNode*)tabs[active_tab_idx].dom_tree);
                    tabs[active_tab_idx].dom_tree = refreshed;
                    css_load_and_apply(refreshed, "assets/style.css");
                    needs_redraw = 1;
                }
                free(downloads_page);
            }
        }
#endif
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
                    char attachment_url[NAL_SYNC_URL_MAX] = {0}, attachment_name[256] = {0};
                    if (browser_read_attachment_sidecar(i, attachment_url, sizeof attachment_url, attachment_name, sizeof attachment_name)) {
                        (void)browser_start_download(attachment_url, attachment_name);
                        clean_and_load_url(i, "browser://downloads", 0);
                        needs_redraw = 1;
                        continue;
                    }
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
                            if (title[0]) {
                                size_t title_len = strlen(title);
                                if (title_len < sizeof tabs[i].title)
                                    memcpy(tabs[i].title, title, title_len + 1);
                            }
                        }
                        if (fgets(icon, sizeof(icon), mf)) {
                            icon[strcspn(icon, "\n")] = 0;
                            if (icon[0] && !resolve_page_url(tabs[i].url, icon, tabs[i].favicon_url, sizeof(tabs[i].favicon_url)))
                                tabs[i].favicon_url[0] = '\0';
                        }
                        fclose(mf);
                    }
                    if (tabs[i].dom_tree) {
                        css_load_and_apply((DOMNode*)tabs[i].dom_tree, "assets/style.css");
                        char bundle_name[256]; snprintf(bundle_name, sizeof(bundle_name), "cache/tab_%d.bundle", i);
                        attach_js(i, tabs[i].url, webpage_bundle_read(bundle_name));
                        browser_apply_password_autofill(i);
                        browser_run_extension_scripts(i);
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
        /* Inactive documents run timer/microtask turns through a bounded IPC
           queue. Polling is non-blocking; any sync DOM operation later drains
           older replies in order before issuing its own command. */
        for (int i = 0; i < tab_count; i++) {
            if (i == active_tab_idx || !tabs[i].js_page) continue;
            (void)webpage_js_tick_async(tabs[i].js_page);
            if (webpage_js_poll(tabs[i].js_page, 1)) needs_redraw = 1;
        }
#if defined(HAVE_UI) && defined(HAVE_RENDER)
        /* A background tab may have entered mutation phase in the loop above.
           Do not process tab-switch or page-input events until it ends. */
        if (browser_dom_commit_pending()) { needs_redraw = 1; continue; }
#endif
        if (active_tab_idx >= 0 && tabs[active_tab_idx].js_page) { /* async page tick + state mailbox */
            void* jp = tabs[active_tab_idx].js_page;
            (void)webpage_js_tick_async(jp);
            if (webpage_js_poll(jp, 1)) needs_redraw = 1;
#if defined(HAVE_UI) && defined(HAVE_RENDER)
            if (browser_dom_commit_pending()) { needs_redraw = 1; continue; }
#endif
            if (webpage_js_view_transition_tick_async(jp)) needs_redraw = 1;
            if (webpage_dyn_pending()) { webpage_dyn_clear_pending(); webpage_js_restyle(jp); needs_redraw = 1; }
            if (webpage_anim_tick()) { needs_redraw = 1; webpage_js_anim_events(jp); }

            /* Navigation, scroll, focus and title are delivered together by the
               async tick mailbox; none of these reads performs a synchronous IPC. */
            const char* jnav = webpage_js_take_nav(jp);
            if (jnav) {
                char nbuf[1200]; snprintf(nbuf, sizeof(nbuf), "%s", jnav); BrowserTab* jt = &tabs[active_tab_idx];
                char method[8] = "", headers[256] = ""; unsigned char* body = NULL; size_t body_len = 0;
                int has_request = webpage_js_take_nav_request(jp, method, sizeof method, headers, sizeof headers, &body, &body_len);
                if (!strcmp(nbuf, "__back__")) { free(body); if (jt->history_index > 0) { jt->history_index--; clean_and_load_url(active_tab_idx, jt->history[jt->history_index].url, 0); } }
                else if (!strcmp(nbuf, "__forward__")) { free(body); if (jt->history_index < jt->history_count - 1) { jt->history_index++; clean_and_load_url(active_tab_idx, jt->history[jt->history_index].url, 0); } }
                else if (has_request && !strcmp(method, "POST")) {
                    WebFormSubmission request; memset(&request, 0, sizeof request);
                    snprintf(request.url, sizeof request.url, "%s", nbuf); snprintf(request.method, sizeof request.method, "%s", method);
                    snprintf(request.headers, sizeof request.headers, "%s", headers); request.body = body; request.body_len = body_len;
                    browser_navigate_form(active_tab_idx, &request, 1); webpage_form_submission_free(&request);
                } else { free(body); clean_and_load_url(active_tab_idx, nbuf, 1); }
                needs_redraw = 1;
            } else {
                int jx, jy; DOMNode* jf; const char* jtitle;
                if (webpage_js_take_scroll(jp, &jx, &jy)) { page_scroll_x = jx; page_scroll_y = jy; clamp_page_scroll(80); needs_redraw = 1; }
                if (webpage_js_take_focus(jp, &jf)) { if (!jf || jf->type == TAG_INPUT || jf->type == TAG_TEXTAREA) focused_node = jf; needs_redraw = 1; }
                if ((jtitle = webpage_js_take_title(jp)) != NULL) { snprintf(tabs[active_tab_idx].title, sizeof tabs[active_tab_idx].title, "%s", jtitle); needs_redraw = 1; }
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
                    browser_draw_find_highlight(top_offset);
#endif
                }
            }
            top_offset = draw_browser_frame(tabs, tab_count, active_tab_idx);
            draw_scrollbars(top_offset);

            if (context_menu_open) {
                const char* items[] = { "Copy", "Paste", "Reload", "Settings", "Bookmark page", "Bookmarks", "Find in page", "Zoom in", "Zoom out", "Reset zoom", "Print / Save PDF", "View source", "Developer tools", "Save current login", "Password manager" };
                int mw=218, row_h=27, mh=(int)(sizeof(items)/sizeof(items[0]))*row_h+8;
                gal_draw_rect(context_menu_x,context_menu_y,mw,mh,0xFFFFFF);
                uint32_t border=0x9CA3AF;gal_draw_rect(context_menu_x,context_menu_y,mw,1,border);gal_draw_rect(context_menu_x,context_menu_y+mh-1,mw,1,border);gal_draw_rect(context_menu_x,context_menu_y,1,mh,border);gal_draw_rect(context_menu_x+mw-1,context_menu_y,1,mh,border);
                for(int ci=0;ci<(int)(sizeof(items)/sizeof(items[0]));ci++)gal_draw_ttf_string(context_menu_x+10,context_menu_y+7+ci*row_h,items[ci],0x111111,13);
            }
            browser_draw_find_bar();
            if (ui_status_text[0] && (long)time(NULL) <= ui_status_expiry) {
                int sw=SCREEN_WIDTH-24; if(sw>620)sw=620; if(sw<260)sw=260;
                int sx=(SCREEN_WIDTH-sw)/2, sy=SCREEN_HEIGHT-38;
                gal_draw_rect(sx,sy,sw,28,0xFFF7D6);gal_draw_rect(sx,sy,sw,1,0xD97706);gal_draw_ttf_string(sx+8,sy+7,ui_status_text,0x7C2D12,12);
            } else if (ui_status_text[0]) ui_status_text[0]=0;
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
#ifdef HAVE_RENDER
            if (ev.type == GAL_EVENT_FILE_DROP) {
                if(active_tab_idx>=0&&tabs[active_tab_idx].dom_tree&&tabs[active_tab_idx].js_page){
                    DOMNode* target=webpage_hit_element((DOMNode*)tabs[active_tab_idx].dom_tree,ev.mouse_x,ev.mouse_y);
                    if(!target)target=(DOMNode*)tabs[active_tab_idx].dom_tree;
                    if(target&&target->v2){int accepted=webpage_js_dispatch_file_drop(tabs[active_tab_idx].js_page,target,ev.file_path,ev.mouse_x,ev.mouse_y-top_offset);if(accepted)browser_set_status("Dropped file delivered to page");else browser_set_status("File dropped; page did not accept the drop");}
                }
                needs_redraw=1;
            }
            else if (ev.type == GAL_EVENT_COMPOSITION) {
                if(active_tab_idx>=0&&tabs[active_tab_idx].js_page&&tabs[active_tab_idx].dom_tree){
                    DOMNode* target=(ime_composition_active&&ime_composition_node&&ime_composition_node->v2)?ime_composition_node:((focused_node&&focused_node->v2)?focused_node:(DOMNode*)tabs[active_tab_idx].dom_tree);
                    if(ev.composition_type==0){
                        ime_composition_active=1;ime_composition_node=(focused_node&&focused_node->v2&&(focused_node->type==TAG_INPUT||focused_node->type==TAG_TEXTAREA))?focused_node:NULL;
                        ime_composition_base=ime_composition_node?strlen(ime_composition_node->value):0;ime_preedit_text[0]=0;
                    }
                    const char* ct=ev.composition_type==0?"compositionstart":ev.composition_type==1?"compositionupdate":"compositionend";
                    webpage_js_dispatch(tabs[active_tab_idx].js_page,target,ct,ev.text,ev.composition_type==1?16:0);
                    /* Keep the active preedit visible in native text controls, replacing
                       the previous composition rather than appending every IME update. */
                    DOMNode* edit=ime_composition_node&&ime_composition_node->v2?ime_composition_node:((focused_node&&focused_node->v2)?focused_node:NULL);
                    if(ev.composition_type==1&&ime_composition_active&&edit&&(edit->type==TAG_INPUT||edit->type==TAG_TEXTAREA)){
                        size_t cap=sizeof edit->value, base=ime_composition_base, add=(size_t)ev.text_len;if(base>=cap)base=cap-1;edit->value[base]=0;
                        if(add>cap-base-1)add=cap-base-1;memcpy(edit->value+base,ev.text,add);edit->value[base+add]=0;
                        size_t pn=add<sizeof ime_preedit_text-1?add:sizeof ime_preedit_text-1;memcpy(ime_preedit_text,ev.text,pn);ime_preedit_text[pn]=0;
                        webpage_js_dispatch(tabs[active_tab_idx].js_page,edit,"input",ev.text,16|32);
                    }
                    if(ev.composition_type==2){
                        if(ime_composition_active&&edit&&(edit->type==TAG_INPUT||edit->type==TAG_TEXTAREA)){size_t base=ime_composition_base;if(base>=sizeof edit->value)base=sizeof edit->value-1;edit->value[base]=0;}
                        if(ev.text_len>0&&edit&&(edit->type==TAG_INPUT||edit->type==TAG_TEXTAREA)){
                            int canceled=webpage_js_dispatch(tabs[active_tab_idx].js_page,edit,"beforeinput",ev.text,32);
                            if(!canceled){size_t cap=sizeof edit->value,len=strlen(edit->value),add=(size_t)ev.text_len;if(len<cap-1){if(add>cap-len-1)add=cap-len-1;memcpy(edit->value+len,ev.text,add);edit->value[len+add]=0;webpage_js_dispatch(tabs[active_tab_idx].js_page,edit,"input",ev.text,32);}}
                        }
                        ime_composition_active=0;ime_composition_node=NULL;ime_preedit_text[0]=0;
                    }
                }
                needs_redraw=1;
            }
#endif

            if (ev.type == GAL_EVENT_MOUSE_DOWN) {
#ifdef HAVE_RENDER
                webpage_set_input_modality(0);
#endif
#ifdef HAVE_RENDER
                if (ev.mouse_button == GAL_MOUSE_LEFT) dom_mouse_buttons |= 1;
                else if (ev.mouse_button == GAL_MOUSE_RIGHT) dom_mouse_buttons |= 2;
                else if (ev.mouse_button == GAL_MOUSE_MIDDLE) dom_mouse_buttons |= 4;
                if (active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree && ((DOMNode*)tabs[active_tab_idx].dom_tree)->v2) {
                    DOMNode* mouse_target = webpage_hit_element((DOMNode*)tabs[active_tab_idx].dom_tree, ev.mouse_x, ev.mouse_y);
                    if (webpage_uses_dyn()) webpage_set_active(mouse_target);
                    if (ev.mouse_y >= top_offset && tabs[active_tab_idx].js_page && mouse_target && mouse_target->v2) {
                        int db = ev.mouse_button == GAL_MOUSE_MIDDLE ? 1 : ev.mouse_button == GAL_MOUSE_RIGHT ? 2 : 0;
                        webpage_js_dispatch_mouse(tabs[active_tab_idx].js_page, mouse_target, "pointerdown", ev.mouse_x, ev.mouse_y-top_offset, db, dom_mouse_buttons, ev.modifiers);
                        webpage_js_dispatch_mouse(tabs[active_tab_idx].js_page, mouse_target, "mousedown", ev.mouse_x, ev.mouse_y-top_offset, db, dom_mouse_buttons, ev.modifiers);
                    }
                }
#endif
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
#ifdef HAVE_RENDER
                    web_text_selection_anchor_valid = 0;
                    webpage_text_selection_clear();
                    if (active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree &&
                        ((DOMNode*)tabs[active_tab_idx].dom_tree)->v2) {
                        WebTextPosition pos;
                        if (webpage_text_position_at((DOMNode*)tabs[active_tab_idx].dom_tree, ev.mouse_x, ev.mouse_y, 0, &pos)) {
                            /* Text focus replaces a previously focused form control. */
                            focused_node = NULL;
                            web_text_selection_anchor = pos;
                            web_text_selection_anchor_valid = 1;
                            webpage_text_selection_set(&web_text_selection_anchor, &web_text_selection_anchor);
                        }
                    }
#endif
                    needs_redraw = 1;
                }
#endif
            }
            else if (ev.type == GAL_EVENT_MOUSE_MOVE) {
#ifdef HAVE_RENDER
                if (active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree && ((DOMNode*)tabs[active_tab_idx].dom_tree)->v2) {
                    DOMNode* mouse_target = webpage_hit_element((DOMNode*)tabs[active_tab_idx].dom_tree, ev.mouse_x, ev.mouse_y);
                    if (webpage_uses_dyn()) webpage_set_hover(mouse_target);
                    if (ev.mouse_y >= top_offset && tabs[active_tab_idx].js_page && mouse_target && mouse_target->v2) {
                        webpage_js_dispatch_mouse(tabs[active_tab_idx].js_page, mouse_target, "pointermove", ev.mouse_x, ev.mouse_y-top_offset, 0, dom_mouse_buttons, ev.modifiers);
                        webpage_js_dispatch_mouse(tabs[active_tab_idx].js_page, mouse_target, "mousemove", ev.mouse_x, ev.mouse_y-top_offset, 0, dom_mouse_buttons, ev.modifiers);
                    }
                }
#endif
#ifdef HAVE_UI
                if (selecting_text) {
                    layout_selection_x1 = select_start_x;
                    layout_selection_y1 = select_start_y;
                    layout_selection_x2 = ev.mouse_x;
                    layout_selection_y2 = ev.mouse_y;
#ifdef HAVE_RENDER
                    if (web_text_selection_anchor_valid && active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree &&
                        ((DOMNode*)tabs[active_tab_idx].dom_tree)->v2) {
                        WebTextPosition focus;
                        if (webpage_text_position_at((DOMNode*)tabs[active_tab_idx].dom_tree, ev.mouse_x, ev.mouse_y, 1, &focus))
                            webpage_text_selection_set(&web_text_selection_anchor, &focus);
                    }
#endif
                    needs_redraw = 1;
                }
#endif
            }
            else if (ev.type == GAL_EVENT_MOUSE_UP) {
#ifdef HAVE_RENDER
                if (active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree && ((DOMNode*)tabs[active_tab_idx].dom_tree)->v2) {
                    DOMNode* mouse_target = webpage_hit_element((DOMNode*)tabs[active_tab_idx].dom_tree, ev.mouse_x, ev.mouse_y);
                    if (ev.mouse_y >= top_offset && tabs[active_tab_idx].js_page && mouse_target && mouse_target->v2) {
                        int db = ev.mouse_button == GAL_MOUSE_MIDDLE ? 1 : ev.mouse_button == GAL_MOUSE_RIGHT ? 2 : 0;
                        webpage_js_dispatch_mouse(tabs[active_tab_idx].js_page, mouse_target, "pointerup", ev.mouse_x, ev.mouse_y-top_offset, db, 0, ev.modifiers);
                        webpage_js_dispatch_mouse(tabs[active_tab_idx].js_page, mouse_target, "mouseup", ev.mouse_x, ev.mouse_y-top_offset, db, 0, ev.modifiers);
                    }
                }
                if (ev.mouse_button == GAL_MOUSE_LEFT) dom_mouse_buttons &= ~1;
                else if (ev.mouse_button == GAL_MOUSE_RIGHT) dom_mouse_buttons &= ~2;
                else if (ev.mouse_button == GAL_MOUSE_MIDDLE) dom_mouse_buttons &= ~4;
                webpage_set_active(NULL);
#endif
#ifdef HAVE_UI
                if (ev.mouse_button == GAL_MOUSE_LEFT) {
                    selecting_text = 0;
                    layout_selection_x2 = ev.mouse_x;
                    layout_selection_y2 = ev.mouse_y;
                    int tiny_drag = abs(layout_selection_x2 - layout_selection_x1) < 4 &&
                                    abs(layout_selection_y2 - layout_selection_y1) < 4;
#ifdef HAVE_RENDER
                    if (!tiny_drag && web_text_selection_anchor_valid && active_tab_idx >= 0 && tabs[active_tab_idx].dom_tree &&
                        ((DOMNode*)tabs[active_tab_idx].dom_tree)->v2) {
                        WebTextPosition focus;
                        if (webpage_text_position_at((DOMNode*)tabs[active_tab_idx].dom_tree, ev.mouse_x, ev.mouse_y, 1, &focus))
                            webpage_text_selection_set(&web_text_selection_anchor, &focus);
                    }
                    if (tiny_drag) {
                        /* A click on text establishes a collapsed caret, not an empty
                           rectangle-only selection. This also makes Ctrl+Arrow usable
                           without first dragging to create a non-empty range. */
                        if (web_text_selection_anchor_valid)
                            webpage_text_selection_set(&web_text_selection_anchor, &web_text_selection_anchor);
                        else
                            webpage_text_selection_clear();
                        web_text_selection_anchor_valid = 0;
                    }
#endif
                    if (tiny_drag) layout_selection_active = 0;
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
                    const int row_h=27, mw=218, item_count=15, mh=item_count*row_h+8;
                    if (mx >= context_menu_x && mx < context_menu_x + mw && my >= context_menu_y && my < context_menu_y + mh) {
                        int item=(my-context_menu_y-4)/row_h;
                        if(item==0) {
                            internal_clipboard[0]='\0';
                            copy_selected_text((DOMNode*)current_nav_dom);
                            if(active_tab_idx>=0&&tabs[active_tab_idx].dom_tree)copy_selected_text((DOMNode*)tabs[active_tab_idx].dom_tree);
                            if(internal_clipboard[0]){gal_set_clipboard(internal_clipboard);browser_set_status("Copied to system clipboard");}
                        } else if(item==1) {
                            char* clip=gal_get_clipboard();char* target=url_bar_focused?input_buffer:(focused_node?focused_node->value:NULL);
                            if(target&&clip){size_t len=strlen(target),max=511;if(len<max)strncat(target,clip,max-len);free(clip);needs_redraw=1;}
                        } else if(item==2) execute_onclick("Browser.reload()");
                        else if(item==3) execute_onclick("Browser.openSettings()");
                        else if(item==4) execute_onclick("Browser.toggleBookmark()");
                        else if(item==5) execute_onclick("Browser.openBookmarks()");
                        else if(item==6) execute_onclick("Browser.findInPage()");
                        else if(item==7) execute_onclick("Browser.zoomIn()");
                        else if(item==8) execute_onclick("Browser.zoomOut()");
                        else if(item==9) execute_onclick("Browser.zoomReset()");
                        else if(item==10) execute_onclick("Browser.printPDF()");
                        else if(item==11) execute_onclick("Browser.viewSource()");
                        else if(item==12) execute_onclick("Browser.openDevtools()");
                        else if(item==13) execute_onclick("Browser.saveCurrentPassword()");
                        else if(item==14) execute_onclick("Browser.openPasswordManager()");
                    }
                    context_menu_open=0; needs_redraw=1; continue;
                }

                if (ev.mouse_button == GAL_MOUSE_RIGHT) {
                    context_menu_open = 1;
                    context_menu_x = mx; context_menu_y = my;
                    int menu_w=218, menu_h=15*27+8;
                    if(context_menu_x+menu_w>SCREEN_WIDTH)context_menu_x=SCREEN_WIDTH-menu_w;
                    if(context_menu_y+menu_h>SCREEN_HEIGHT)context_menu_y=SCREEN_HEIGHT-menu_h;
                    if(context_menu_x<0)context_menu_x=0;if(context_menu_y<0)context_menu_y=0;
                    continue;
                }

                selecting_text = 0;
                layout_selection_active = 0;
#ifdef HAVE_RENDER
                webpage_text_selection_clear();
                web_text_selection_anchor_valid = 0;
#endif

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
                            if (jt && webpage_js_dispatch_mouse(tabs[active_tab_idx].js_page, jt, "click", mx, my-top_offset, 0, 0, ev.modifiers)) clicked = NULL;
                            needs_redraw = 1;
                            if (!tabs[active_tab_idx].js_page) clicked = NULL;      /* JS уже перевёл страницу */
                        }
                        if (clicked) {
                            if (clicked->type == TAG_INPUT && !strcmp(clicked->input_type,"file")) {
                                browser_input_file_click(clicked);
                            } else if (clicked->type == TAG_A && strlen(clicked->href) > 0) {
                                gal_log("INFO", "Link clicked! Navigating to: %s", clicked->href);
                                char href_copy[600]; snprintf(href_copy, sizeof(href_copy), "%s", clicked->href);
                                char download_name[256] = "";
                                int has_download_attr = 0;
#ifdef HAVE_RENDER
                                has_download_attr = browser_dom_get_attr(clicked, "download", download_name, sizeof(download_name));
#endif
                                if (has_download_attr) (void)browser_start_download(href_copy, download_name);
                                else clean_and_load_url(active_tab_idx, href_copy, 1);
                            } else if (clicked->v2) {
                                char form_url[1024]; WebFormSubmission form_req; memset(&form_req, 0, sizeof form_req);
                                int act = webpage_click((DOMNode*)tabs[active_tab_idx].dom_tree, clicked, form_url, sizeof(form_url), &form_req);
                                if (act == 1) { focused_node = clicked; gal_log("INFO", "Input focused in page.");
                                    if (tabs[active_tab_idx].js_page) { webpage_js_set_focus(tabs[active_tab_idx].js_page, clicked); webpage_js_dispatch(tabs[active_tab_idx].js_page, clicked, "focus", "", 0); } }
                                else if (act == 2) {
                                    if (!tabs[active_tab_idx].js_page || webpage_js_before_submit(tabs[active_tab_idx].js_page, clicked)) { gal_log("INFO", "Form submit: %s", form_req.url[0] ? form_req.url : form_url); browser_navigate_form(active_tab_idx, &form_req, 1); }
                                    webpage_form_submission_free(&form_req);
                                } else webpage_form_submission_free(&form_req);
                                if (act != 1 && act != 2 && strlen(clicked->onclick) > 0) execute_onclick(clicked->onclick);
                                needs_redraw = 1;
                            } else if (clicked->type == TAG_INPUT) {
                                focused_node = clicked;
                                gal_log("INFO", "Input focused in page.");
                            } else if (strlen(clicked->onclick) > 0) {
                                execute_onclick(clicked->onclick);
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
#ifdef HAVE_RENDER
                webpage_set_input_modality(1);
#endif
#ifdef HAVE_UI
                int key=ev.key_char;
                if(key>=1&&key<=26) key='a'+key-1;
                if(key>=32&&key<127) key=tolower((unsigned char)key);
                if(!key&&ev.text_len>0) key=tolower((unsigned char)ev.text[0]);
                if(!key&&ev.key_code>0&&ev.key_code<128) key=tolower(ev.key_code);
                int ctrl_pressed=(ev.modifiers&GAL_MOD_CTRL)!=0;
                int ctrl=ctrl_pressed;
                int shift=(ev.modifiers&GAL_MOD_SHIFT)!=0;
                if(ctrl) {
                    if(key=='f') { find_bar_open=1; find_bar_focused=1; find_match_index=-1; }
                    else if(key=='p'||(key=='s'&&!shift)) browser_print_pdf();
                    else if(key=='u') browser_view_source();
                    else if(key=='r') execute_onclick("Browser.reload()");
                    else if(key=='h') execute_onclick("Browser.openHistory()");
                    else if(key=='j') execute_onclick("Browser.openDownloads()");
                    else if(key=='d'&&!shift) execute_onclick("Browser.toggleBookmark()");
                    else if(key=='b'&&shift) execute_onclick("Browser.openBookmarks()");
                    else if(key=='i'&&shift) execute_onclick("Browser.openDevtools()");
                    else if(key=='s'&&shift) execute_onclick("Browser.openPasswordManager()");
                    else if(key=='l') { url_bar_focused=1; focused_node=NULL; if(active_tab_idx>=0)snprintf(input_buffer,sizeof input_buffer,"%s",tabs[active_tab_idx].url); }
                    else if(key=='t') create_new_tab();
                    else if(key=='w') { if(active_tab_idx>=0)close_tab(active_tab_idx); }
                    else if(key=='c') {
                        internal_clipboard[0]='\0'; copy_selected_text((DOMNode*)current_nav_dom);
                        if(active_tab_idx>=0&&tabs[active_tab_idx].dom_tree)copy_selected_text((DOMNode*)tabs[active_tab_idx].dom_tree);
                        if(internal_clipboard[0]){gal_set_clipboard(internal_clipboard);browser_set_status("Copied to system clipboard");}
                    }
                    else if(key=='v') {
                        char*clip=gal_get_clipboard();char*target=url_bar_focused?input_buffer:(focused_node?focused_node->value:NULL);
                        if(target&&clip){size_t len=strlen(target),cap=url_bar_focused?sizeof(input_buffer):sizeof(focused_node->value);if(len<cap-1)strncat(target,clip,cap-len-1);free(clip);needs_redraw=1;}
                    }
                    else if(key=='+'||key=='=') execute_onclick("Browser.zoomIn()");
                    else if(key=='-') execute_onclick("Browser.zoomOut()");
                    else if(key=='0') execute_onclick("Browser.zoomReset()");
                    else if(key=='e'&&shift) execute_onclick("Browser.restoreSession()");
                    else { /* unknown shortcut: let the page receive it */ ctrl=0; }
                    if(ctrl) { needs_redraw=1; continue; }
                }
                if (find_bar_open && find_bar_focused) {
                    if(ev.key_code==0xFF1B) { find_bar_open=0;find_bar_focused=0; }
                    else if(ev.key_char=='\r'||ev.key_char=='\n'||ev.key_code==0xFF0D) {
#ifdef HAVE_RENDER
                        browser_find_next(1);
#endif
                    } else if(ev.key_char=='\b'||ev.key_code==0xFF08) {
                        input_backspace_utf8(find_query);find_match_index=-1;
#ifdef HAVE_RENDER
                        browser_find_next(0);
#endif
                    } else if(ev.text_len>0&&(unsigned char)ev.text[0]>=32) {
                        size_t n=strlen(find_query);if(n+(size_t)ev.text_len<sizeof find_query){memcpy(find_query+n,ev.text,(size_t)ev.text_len);find_query[n+ev.text_len]=0;find_match_index=-1;
#ifdef HAVE_RENDER
                            browser_find_next(0);
#endif
                        }
                    }
                    needs_redraw=1;continue;
                }
                if(ev.key_code==0xFFc0) {
                    find_bar_open=1;find_bar_focused=0;
#ifdef HAVE_RENDER
                    browser_find_next(1);
#endif
                    needs_redraw=1;continue;
                }
                if(ev.key_code==0xFF1B&&find_bar_open) {find_bar_open=0;find_bar_focused=0;needs_redraw=1;continue;}
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
                    else if (ev.key_code == 0xFF50) strcpy(kn, "Home"); else if (ev.key_code == 0xFF57) strcpy(kn, "End");
                    else if (ev.text_len > 0 && (unsigned char)ev.text[0] >= 32) snprintf(kn, sizeof(kn), "%.*s", ev.text_len < 15 ? ev.text_len : 15, ev.text);
                    if (kn[0]) { void* jp = tabs[active_tab_idx].js_page; js_block = webpage_js_dispatch(jp, jt, "keydown", kn, ev.modifiers); if (tabs[active_tab_idx].js_page == jp) webpage_js_dispatch(jp, jt, "keyup", kn, ev.modifiers); }
                }
#endif
                if (!js_block) {
#ifdef HAVE_RENDER
                /* Once a logical text selection exists, browser-level arrows move its
                   caret in physical visual order (critical for mixed RTL/LTR runs).
                   Editable form controls and the URL bar retain their own key handling. */
                if (!url_bar_focused && !focused_node && active_tab_idx >= 0 &&
                    tabs[active_tab_idx].dom_tree &&
                    ((DOMNode*)tabs[active_tab_idx].dom_tree)->v2 &&
                    webpage_text_selection_active()) {
                    int move = 0;
                    if (ev.key_code == 0xFF51) move = ctrl_pressed ? -5 : -1; /* Ctrl+Left: previous word */
                    else if (ev.key_code == 0xFF53) move = ctrl_pressed ? 5 : 1; /* Ctrl+Right: next word */
                    else if (ev.key_code == 0xFF52) move = ctrl_pressed ? -6 : -2; /* Ctrl+Up: previous paragraph */
                    else if (ev.key_code == 0xFF54) move = ctrl_pressed ? 6 : 2; /* Ctrl+Down: next paragraph */
                    else if (ev.key_code == 0xFF50) move = ctrl_pressed ? -4 : -3; /* Home */
                    else if (ev.key_code == 0xFF57) move = ctrl_pressed ? 4 : 3;   /* End */
                    if (move && webpage_text_selection_move_visual(
                            (DOMNode*)tabs[active_tab_idx].dom_tree, move, shift)) {
                        layout_selection_active = 0;
                        selecting_text = 0;
                        web_text_selection_anchor_valid = 0;
                        needs_redraw = 1;
                        continue;
                    }
                }
#endif
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
                                int blocked=0;
#ifdef HAVE_RENDER
                                if(active_tab_idx>=0&&tabs[active_tab_idx].js_page)blocked=webpage_js_dispatch(tabs[active_tab_idx].js_page,focused_node,"beforeinput","\n",128);
#endif
                                int len = strlen(target_buf); if (!blocked&&len + 1 < max_len) { target_buf[len] = '\n'; target_buf[len + 1] = '\0';
#ifdef HAVE_RENDER
                                    if(active_tab_idx>=0&&tabs[active_tab_idx].js_page)webpage_js_dispatch(tabs[active_tab_idx].js_page,focused_node,"input","\n",128);
#endif
                                }
                            } else if (focused_node && focused_node->v2 && active_tab_idx >= 0) {
                                DOMNode* f = focused_node; focused_node = NULL; WebFormSubmission form_req; memset(&form_req, 0, sizeof form_req);
                                void* jp = tabs[active_tab_idx].js_page;
                                if (jp) webpage_js_dispatch(jp, f, "change", "", 0);
                                if (webpage_form_request(f, NULL, &form_req) && (!jp || webpage_js_before_submit(jp, f))) { gal_log("INFO", "Form submit (Enter): %s", form_req.url); browser_navigate_form(active_tab_idx, &form_req, 1); }
                                webpage_form_submission_free(&form_req);
                            } else {
                                focused_node = NULL;
                            }
                        }
                    }
                    else if (ev.key_char == '\b' || ev.key_code == 0xFF08) {
                        int edit_block = 0;
#ifdef HAVE_RENDER
                        if (!url_bar_focused && focused_node && focused_node->v2 && active_tab_idx >= 0 && tabs[active_tab_idx].js_page)
                            edit_block = webpage_js_dispatch(tabs[active_tab_idx].js_page, focused_node, "beforeinput", "", 64);
#endif
                        if (!edit_block) {
                            input_backspace_utf8(target_buf);
#ifdef HAVE_RENDER
                            if (!url_bar_focused && focused_node && focused_node->v2 && active_tab_idx >= 0 && tabs[active_tab_idx].js_page)
                                webpage_js_dispatch(tabs[active_tab_idx].js_page, focused_node, "input", "", 64);
#endif
                        }
                    }
                    else if (ev.text_len > 0 && (unsigned char)ev.text[0] >= 32) {
                        char tx[256]; int tn=ev.text_len; if(tn>(int)sizeof tx-1)tn=(int)sizeof tx-1; memcpy(tx,ev.text,(size_t)tn);tx[tn]=0;
                        int edit_block = 0;
#ifdef HAVE_RENDER
                        if (!url_bar_focused && focused_node && focused_node->v2 && active_tab_idx >= 0 && tabs[active_tab_idx].js_page)
                            edit_block = webpage_js_dispatch(tabs[active_tab_idx].js_page, focused_node, "beforeinput", tx, 0);
#endif
                        int len = strlen(target_buf);
                        if (!edit_block && len + ev.text_len < max_len) {
                            memcpy(target_buf + len, ev.text, (size_t)ev.text_len);
                            target_buf[len + ev.text_len] = '\0';
#ifdef HAVE_RENDER
                            if (!url_bar_focused && focused_node && focused_node->v2 && active_tab_idx >= 0 && tabs[active_tab_idx].js_page)
                                webpage_js_dispatch(tabs[active_tab_idx].js_page, focused_node, "input", tx, 0);
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
    browser_persist_session();
    app_shutting_down = 1;
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
