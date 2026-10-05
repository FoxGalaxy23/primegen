#include "ui.h"
#include "gal.h"
#include "parser.h"
#include "layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* current_nav_dom = NULL; // Holds DOMNode*
static DOMNode* cached_nav_template_dom = NULL;
static char* last_nav_template_ptr = NULL;

// Helper to create a DOMNode and parse its style
static DOMNode* create_dom_node(TagType type, const char* id, const char* text, const char* onclick, const char* style_str) {
    DOMNode* node = malloc(sizeof(DOMNode));
    memset(node, 0, sizeof(DOMNode));
    node->type = type;
    if (id) strcpy(node->id, id);
    if (text) strcpy(node->text, text);
    if (onclick) strcpy(node->onclick, onclick);
    if (style_str) {
        strcpy(node->style_str, style_str);
        parse_inline_style(node, style_str);
    }
    return node;
}

static DOMNode* find_node_by_id(DOMNode* head, const char* id) {
    DOMNode* curr = head;
    while (curr) {
        if (strcmp(curr->id, id) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

int draw_browser_frame(BrowserTab* tabs, int tab_count, int active_idx) {
    // 1. Free previous frame DOM tree
    if (current_nav_dom) {
        free_dom((DOMNode*)current_nav_dom);
        current_nav_dom = NULL;
    }

    // 2. Ensure we have a cached template DOM
    if (!nav_bar_template) return 0;

    if (nav_bar_template != last_nav_template_ptr) {
        if (cached_nav_template_dom) free_dom(cached_nav_template_dom);
        cached_nav_template_dom = parse_html(nav_bar_template);
        last_nav_template_ptr = nav_bar_template;
    }

    if (!cached_nav_template_dom) return 0;

    DOMNode* nav_dom = clone_dom(cached_nav_template_dom);
    if (!nav_dom) return 0;

    // 3. Find and update the static template nodes
    DOMNode* bg = find_node_by_id(nav_dom, "bg");
    if (bg) {
        bg->width = SCREEN_WIDTH;
        if (anonymous_mode) {
            bg->bg_color = 0x1F2937; // Dark slate background
        } else {
            bg->bg_color = 0xE5E7EB; // Light gray background
        }
    }

    DOMNode* tabs_container = find_node_by_id(nav_dom, "tabs-container");
    if (tabs_container) {
        tabs_container->width = SCREEN_WIDTH;
        if (anonymous_mode) {
            tabs_container->bg_color = 0x111827; // Dark tabs background
        } else {
            tabs_container->bg_color = 0xD1D5DB; // Light tabs background
        }
    }

    DOMNode* url_bar = find_node_by_id(nav_dom, "url-bar");
    if (url_bar) {
        url_bar->width = SCREEN_WIDTH - 250;
        if (anonymous_mode) {
            url_bar->bg_color = 0x374151;
            url_bar->text_color = 0xFFFFFF;
        } else {
            url_bar->bg_color = 0xFFFFFF;
            url_bar->text_color = 0x111111;
        }

        // Set value
        if (url_bar_focused) {
            strcpy(url_bar->value, input_buffer);
        } else {
            if (active_idx >= 0 && active_idx < tab_count) {
                strcpy(url_bar->value, tabs[active_idx].url);
            } else {
                url_bar->value[0] = '\0';
            }
        }
    }

    DOMNode* btn_menu = find_node_by_id(nav_dom, "btn-menu");
    if (btn_menu) {
        btn_menu->left = SCREEN_WIDTH - 55;
    }

    DOMNode* btn_settings = find_node_by_id(nav_dom, "btn-settings");
    if (btn_settings) {
        btn_settings->left = SCREEN_WIDTH - 150;
    }

    // Style Back/Fwd/Reload/Menu buttons based on mode
    DOMNode* b_back = find_node_by_id(nav_dom, "btn-back");
    DOMNode* b_fwd = find_node_by_id(nav_dom, "btn-forward");
    DOMNode* b_rel = find_node_by_id(nav_dom, "btn-reload");
    
    if (anonymous_mode) {
        if (b_back) { b_back->bg_color = 0x4B5563; b_back->text_color = 0xFFFFFF; }
        if (b_fwd) { b_fwd->bg_color = 0x4B5563; b_fwd->text_color = 0xFFFFFF; }
        if (b_rel) { b_rel->bg_color = 0x4B5563; b_rel->text_color = 0xFFFFFF; }
        if (btn_menu) { btn_menu->bg_color = 0x4B5563; btn_menu->text_color = 0xFFFFFF; }
        if (btn_settings) { btn_settings->bg_color = 0x4B5563; btn_settings->text_color = 0xFFFFFF; }
    } else {
        if (b_back) { b_back->bg_color = 0x9CA3AF; b_back->text_color = 0xFFFFFF; }
        if (b_fwd) { b_fwd->bg_color = 0x9CA3AF; b_fwd->text_color = 0xFFFFFF; }
        if (b_rel) { b_rel->bg_color = 0x9CA3AF; b_rel->text_color = 0xFFFFFF; }
        if (btn_menu) { btn_menu->bg_color = 0x9CA3AF; btn_menu->text_color = 0xFFFFFF; }
        if (btn_settings) { btn_settings->bg_color = 0x9CA3AF; btn_settings->text_color = 0xFFFFFF; }
    }

    // 4. Find the last node in the navigation DOM list to append dynamic nodes
    DOMNode* last = nav_dom;
    while (last && last->next) {
        last = last->next;
    }

    // 5. Append dynamic Tab buttons
    char style_buf[256];
    char click_buf[64];
    for (int i = 0; i < tab_count; i++) {
        int tab_x = i * 135 + 5;
        uint32_t tab_color = (i == active_idx) ? 0xFFFFFF : (anonymous_mode ? 0x374151 : 0x9CA3AF);
        uint32_t tab_text_color = (i == active_idx) ? 0x000000 : (anonymous_mode ? 0xFFFFFF : 0xFFFFFF);
        
        snprintf(style_buf, sizeof(style_buf), 
                 "position: absolute; left: %d; top: 48; width: 130; height: 30; background-color: #%06X; color: #%06X;", 
                 tab_x, tab_color, tab_text_color);
        
        char tab_title[32];
        char source_title[128] = {0};
        if (strlen(tabs[i].title) > 0) {
            strncpy(source_title, tabs[i].title, sizeof(source_title) - 1);
        } else if (strlen(tabs[i].url) > 0) {
            strncpy(source_title, tabs[i].url, sizeof(source_title) - 1);
        }
        if (strlen(source_title) > 0) {
            const char* display_url = source_title;
            if (strncmp(display_url, "http://", 7) == 0) display_url += 7;
            else if (strncmp(display_url, "https://", 8) == 0) display_url += 8;
            
            char icon_char = ((unsigned char)display_url[0] < 128 && display_url[0]) ? display_url[0] : '*';
            if (strlen(display_url) > 8) {
                snprintf(tab_title, sizeof(tab_title), "[%c] %.8s...", icon_char, display_url);
            } else {
                snprintf(tab_title, sizeof(tab_title), "[%c] %s", icon_char, display_url);
            }
        } else {
            snprintf(tab_title, sizeof(tab_title), "Tab %d", i + 1);
        }
        
        snprintf(click_buf, sizeof(click_buf), "Browser.selectTab(%d)", i);
        DOMNode* tab_node = create_dom_node(TAG_BUTTON, "", tab_title, click_buf, style_buf);
        last->next = tab_node;
        last = tab_node;

        // Tab close button
        snprintf(style_buf, sizeof(style_buf), 
                 "position: absolute; left: %d; top: 52; width: 16; height: 22; background-color: #EF4444; color: #FFFFFF;", 
                 tab_x + 110);
        snprintf(click_buf, sizeof(click_buf), "Browser.closeTab(%d)", i);
        DOMNode* close_node = create_dom_node(TAG_BUTTON, "", "x", click_buf, style_buf);
        last->next = close_node;
        last = close_node;
    }

    // Append Tab Add button (+)
    int add_x = tab_count * 135 + 5;
    snprintf(style_buf, sizeof(style_buf), 
             "position: absolute; left: %d; top: 48; width: 30; height: 30; background-color: #10B981; color: #FFFFFF;", 
             add_x);
    DOMNode* add_node = create_dom_node(TAG_BUTTON, "tab-add", "+", "Browser.newTab()", style_buf);
    last->next = add_node;
    last = add_node;

    // 6. Append dynamic Dropdown Menu if open
    if (menu_open) {
        int hist_items_to_show = global_history_count > 5 ? 5 : global_history_count;
        int menu_h = 90 + hist_items_to_show * 35;
        if (menu_h < 90) menu_h = 90;
        
        snprintf(style_buf, sizeof(style_buf), 
                 "position: absolute; left: %d; top: 45; width: 220; height: %d; background-color: #%s;", 
                 SCREEN_WIDTH - 230, menu_h, anonymous_mode ? "1F2937" : "F3F4F6");
        DOMNode* dropdown_bg = create_dom_node(TAG_DIV, "menu-dropdown", "", "", style_buf);
        last->next = dropdown_bg;
        last = dropdown_bg;

        // New Incognito Window button
        char anon_text[32] = "New Incognito Window";
        snprintf(style_buf, sizeof(style_buf), 
                 "position: absolute; left: %d; top: 50; width: 210; height: 30; background-color: #374151; color: #FFFFFF;",
                 SCREEN_WIDTH - 225);
        DOMNode* anon_node = create_dom_node(TAG_BUTTON, "menu-anon", anon_text, "Browser.openIncognito()", style_buf);
        last->next = anon_node;
        last = anon_node;

        // History button
        snprintf(style_buf, sizeof(style_buf), 
                 "position: absolute; left: %d; top: 85; width: 210; height: 30; background-color: #374151; color: #FFFFFF;", 
                 SCREEN_WIDTH - 225);
        DOMNode* history_node = create_dom_node(TAG_BUTTON, "menu-history", "History", "Browser.openHistory()", style_buf);
        last->next = history_node;
        last = history_node;

        // Downloads button
        snprintf(style_buf, sizeof(style_buf),
                 "position: absolute; left: %d; top: 120; width: 210; height: 30; background-color: #374151; color: #FFFFFF;",
                 SCREEN_WIDTH - 225);
        DOMNode* downloads_node = create_dom_node(TAG_BUTTON, "menu-downloads", "Downloads", "Browser.openDownloads()", style_buf);
        last->next = downloads_node;
        last = downloads_node;
    }

    // 7. Render navigation bar DOM tree
    layout_and_render(nav_dom, 0, NULL, 0, 0);

    // 8. Retain DOM tree for hit-testing in main loop
    current_nav_dom = nav_dom;

    // Draw separator line under navigation bar
    gal_draw_rect(0, 79, SCREEN_WIDTH, 2, 0x757575);

    return 80;
}
