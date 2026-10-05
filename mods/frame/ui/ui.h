#ifndef UI_H
#define UI_H

#define MAX_TABS 6
#define MAX_HISTORY 50
#define MAX_GLOBAL_HISTORY 100

typedef struct {
    char url[256];
} HistoryEntry;

typedef struct {
    char url[256];
    char title[128];
    char favicon_url[256];
    char favicon_path[512];
    void* dom_tree; // Храним DOM-дерево вкладки (кастим к DOMNode*)
    void* js_page;  // JS-страница (QuickJS) для DOM этой вкладки или NULL
    int is_active;
    
    // Process isolation & loading state
    int is_loading;
    int loading_pid;
    long loading_start_time;
    int is_hung;

    // Tab-specific history
    HistoryEntry history[MAX_HISTORY];
    int history_count;
    int history_index;
} BrowserTab;

// Global state externs
extern int anonymous_mode;
extern int menu_open;
extern char input_buffer[512];
extern int url_bar_focused;
extern int loading_progress;
extern char global_history[MAX_GLOBAL_HISTORY][256];
extern int global_history_count;
extern char* nav_bar_template;
extern void* current_nav_dom; // points to DOMNode of navigation bar

// Отрисовка интерфейса. Принимает вкладки, возвращает высоту статус-бара
int draw_browser_frame(BrowserTab* tabs, int tab_count, int active_idx);

#endif
