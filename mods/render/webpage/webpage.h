#ifndef WEBPAGE_H
#define WEBPAGE_H

#include "parser.h"
#include <stddef.h>
#include <stdint.h>

/* ── Публичный API движка ─────────────────────────────────────────
   parse_document()  — HTML -> дерево -> каскад стилей (см. parser.h)
   webpage_render()  — раскладка + отрисовка (вызывается из layout_and_render)
   webpage_hit_test  — узел под курсором (ссылка / кнопка / поле ввода / onclick)
   webpage_copy_selection — текст под прямоугольником выделения
   webpage_form_url  — URL GET-отправки формы, в которой лежит control          */
void     webpage_render(DOMNode* head, int top_offset, const char* page_url, int scroll_x, int scroll_y);
DOMNode* webpage_hit_element(DOMNode* head, int mx, int my);   /* глубочайший элемент под курсором (экранные координаты) */
int      webpage_viewport_top(void);
DOMNode* webpage_hit_test(DOMNode* head, int mx, int my);
int      webpage_copy_selection(char* out, int cap);
typedef struct {
    DOMNode* node;
    uint32_t offset; /* UTF-8 byte offset in the source text node */
    float visual_x, visual_y; /* screen-space caret location; helps disambiguate wrapped runs */
} WebTextPosition;
/* Find the nearest grapheme-safe text caret at a screen point. allow_nearest enables
 * endpoint clamping while dragging outside the original text run. */
int      webpage_text_position_at(DOMNode* head, int mx, int my, int allow_nearest, WebTextPosition* out);
void     webpage_text_selection_set(const WebTextPosition* anchor, const WebTextPosition* focus);
/* Move active text selection/caret: -1/+1 visual left/right, -2/+2 up/down,
 * -3/+3 Home/End, -4/+4 Ctrl+Home/End (logical document), -5/+5 Ctrl+Left/Right
 * (previous/next word), -6/+6 Ctrl+Up/Down (paragraph start/end). extend keeps
 * the original anchor (Shift+Arrow). Returns 1 if handled. */
int      webpage_text_selection_move_visual(DOMNode* head, int direction, int extend);
void     webpage_text_selection_clear(void);
int      webpage_text_selection_active(void);
typedef struct {
    char url[1200];
    char method[8];
    char headers[256];
    unsigned char* body;
    size_t body_len;
} WebFormSubmission;
int      webpage_click(DOMNode* head, DOMNode* n, char* url, int cap, WebFormSubmission* submission);
int      webpage_form_request(DOMNode* control, DOMNode* submitter, WebFormSubmission* out);
void     webpage_form_submission_free(WebFormSubmission* submission);
int      webpage_form_url(DOMNode* control, DOMNode* submitter, char* out, int cap);

typedef struct { char* css; char* href; char media[96]; } SheetSrc;   /* css != NULL: <style>, href != NULL: <link> */
typedef struct { SheetSrc* v; int n, cap; } SheetList;

/* ── Скрипты и бандл страницы (для JS-движка) ───────────────────── */
typedef struct { char* text; char* url; char* nonce; uint8_t module, async_, defer_; int node_idx; } ScriptSrc;
typedef struct PageBundle { SheetSrc* sheets; int nsheets; ScriptSrc* scripts; int nscripts; char base[1024]; } PageBundle;
int         webpage_bundle_write(const char* path);            /* сохранить бандл последнего parse_document */
PageBundle* webpage_bundle_read(const char* path);
PageBundle* webpage_bundle_take(void);                           /* забрать бандл последнего parse_document (владение переходит) */
void        webpage_bundle_free(PageBundle* b);
int         webpage_bundle_prefetch_css(void);                   /* докачать текст всех <link rel=stylesheet> бандла ДО paint (см. main.c: фоновый процесс) */
char*       webpage_fetch_text(const char* url);                 /* http(s) через NAL или локальный файл; malloc-строка или NULL */
char*       webpage_fetch_text_kind(const char* url, int resource_kind); /* загрузка с CSP-категорией ресурса */
DOMNode*    webpage_new_node(const char* tag);                   /* пустой узел с умолчаниями (тег "#text" — текстовый) */
void        webpage_recascade(DOMNode* head, SheetList* sheets, const char* base_url);   /* без сети */
/* селекторы для JS */
typedef struct WebSel WebSel;
WebSel*     websel_compile(const char* text);
void        websel_free(WebSel* s);
void*       webdoc_build(DOMNode* head);
void        webdoc_free(void* doc);
int         websel_match(void* doc, WebSel* sel, DOMNode* n);
void        webpage_invalidate_layout(void);
/* Renderer-owned snapshot used by the limited View Transition cross-fade. */
/* Supported View Transition pseudo-element selectors. The compositor applies a
 * bounded subset of computed opacity/background/transform styles and CSS keyframes
 * to named old/new image snapshots. */
enum {
    WEBPAGE_VT_PSEUDO_NONE = 0,
    WEBPAGE_VT_PSEUDO_BEFORE = 1,
    WEBPAGE_VT_PSEUDO_AFTER = 2,
    WEBPAGE_VT_PSEUDO_MARKER = 3,
    WEBPAGE_VT_PSEUDO_ROOT = 4,
    WEBPAGE_VT_PSEUDO_GROUP = 5,
    WEBPAGE_VT_PSEUDO_IMAGE_PAIR = 6,
    WEBPAGE_VT_PSEUDO_OLD = 7,
    WEBPAGE_VT_PSEUDO_NEW = 8
};
const NodeStyle* webpage_view_transition_style(const char* name, int pseudo);
const NodeStyle* webpage_view_transition_style_at(const char* name, int pseudo, double elapsed_ms);
double      webpage_view_transition_duration_ms(void);
int         webpage_view_transition_animates_opacity(const char* name, int pseudo);
void        webpage_view_transition_capture(DOMNode* head);
void        webpage_view_transition_commit(DOMNode* head);
void        webpage_view_transition_cancel(void);
int         webpage_view_transition_tick(void);
int         webpage_view_transition_take_finished(void);
void        webpage_force_layout(void);
void        webpage_force_layout_for(DOMNode* head);
void        webpage_get_scroll(int* x, int* y, int* vw, int* vh);
int         webpage_node_rect(DOMNode* n, float* out4);          /* border-box в координатах документа */
int         webpage_scroll_at(int mx, int my, int dx, int dy);   /* колесо над прокручиваемым контейнером: 1 — прокрутка выполнена */
int         webpage_elem_scroll_get(DOMNode* n, float* x, float* y, float* sw, float* sh, float* cw, float* ch);   /* 1 — элемент прокручиваемый */
void        webpage_elem_scroll_set(DOMNode* n, float x, float y);
void        webpage_elem_scroll_set_smooth(DOMNode* n, float x, float y); /* кадр анимации: без scroll-snap до её завершения */
void        font_manager_set_offline(int off);

/* ── JavaScript (QuickJS + DOM-примитивы), см. js.c ── */
void*       webpage_js_attach(DOMNode* head, const char* url, PageBundle* bundle);   /* создаёт JS-страницу, исполняет скрипты; владеет bundle */
void        webpage_js_free(void* page);
void        webpage_js_restyle(void* page);                                 /* пересчитать стили (после :hover/:active) */
void        webpage_js_anim_events(void* page);                             /* transitionend/animationend… → JS */
int         webpage_anim_tick(void);                                        /* 1 — идёт transition/animation и пора рисовать кадр */
void        webpage_set_hover(DOMNode* n);
void        webpage_set_input_modality(int keyboard); /* keyboard=1 enables :focus-visible heuristic */
void        webpage_set_active(DOMNode* n);
DOMNode*    webpage_hover_node(void);
DOMNode*    webpage_active_node(void);
int         webpage_dyn_pending(void);
void        webpage_dyn_clear_pending(void);
int         webpage_uses_dyn(void);
int         webpage_js_tick(void* page);                                    /* таймеры, промисы, пересчёт стилей; 1 — нужна перерисовка */
int         webpage_js_tick_async(void* page);                              /* поставить фоновый tick без ожидания; POSIX mailbox также переносит nav/title/scroll/focus */
int         webpage_js_view_transition_tick_async(void* page);             /* advance compositor timing and deliver finished notification asynchronously */
int         webpage_js_poll(void* page, int allow_debugger_ui);              /* обработать готовые ответы IPC; 1 — DOM/paint изменился */
int         webpage_js_has_pending(void* page);                             /* есть незавершённые асинхронные IPC-команды */
int         webpage_js_dom_commit_pending(void* page);                    /* DOM меняется порциями; до завершения нельзя рисовать/обрабатывать ввод */
int         webpage_js_dispatch(void* page, DOMNode* target, const char* type, const char* key, int mods);   /* 1 — preventDefault */
int         webpage_js_dispatch_mouse(void* page, DOMNode* target, const char* type, int client_x, int client_y, int button, int buttons, int mods);
int         webpage_js_dispatch_file_drop(void* page, DOMNode* target, const char* path, int client_x, int client_y);
int         webpage_js_before_submit(void* page, DOMNode* control);       /* submit-событие; 1 — можно отправлять */
const char* webpage_js_take_nav(void* page);                                /* POSIX: забрать nav из последнего async tick без IPC-roundtrip */
int         webpage_js_take_nav_request(void* page, char* method, size_t method_cap, char* headers, size_t headers_cap, unsigned char** body, size_t* body_len);                                /* навигация, запрошенная из JS (или NULL) */
int         webpage_js_take_scroll(void* page, int* x, int* y);             /* POSIX: результат async tick mailbox */
int         webpage_js_take_focus(void* page, DOMNode** node);               /* POSIX: результат async tick mailbox */
void        webpage_js_set_focus(void* page, DOMNode* node);
const char* webpage_js_take_title(void* page);                              /* POSIX: результат async tick mailbox */
int         webpage_js_eval(void* page, const char* code, char* out, size_t cap);  /* DevTools console */

/* ── Внутреннее: html.c -> style.c ──────────────────────────────── */

DOMNode* html_build(const char* utf8, const char* base_url, SheetList* sheets);
void     style_apply(DOMNode* head, SheetList* sheets, const char* base_url);
int      webpage_attr_get(const DOMNode* n, const char* name, char* out, size_t cap);
void     webpage_set_doc_url(const char* url);          /* текущий документ — для проверок безопасности ресурсов */
int      webpage_mixed_blocked(const char* url);        /* 1 — https-страница не должна грузить этот http-ресурс (смешанное содержимое) */
void     webpage_resolve_url(const char* base, const char* rel, char* out, size_t cap);
int      webpage_is_utf8(const char* s);

/* ── images.c ───────────────────────────────────────────────────── */
typedef struct { uint32_t* px; int w, h; } ImgPix;                /* 0xAARRGGBB */
int      webpage_image_path(const char* abs_url, char* out, size_t cap);   /* путь в кэше / локальный путь */
void     webpage_prefetch_images(DOMNode* head);
ImgPix*  webpage_image_get(const char* abs_url);                   /* декодированная картинка (кэш) или NULL */
ImgPix*  webpage_image_scaled(const char* abs_url, int w, int h);  /* масштабированная копия (кэш) или NULL */
void     webpage_image_inject(const char* key, const uint32_t* argb, int w, int h);   /* для canvas 2D: подменить/создать запись кэша по синтетическому ключу */
void     webpage_image_cache_clear(void);
void     webpage_image_set_color(uint32_t argb);                          /* currentColor для следующих webpage_image_scaled (встроенный <svg>) */
int      webpage_data_uri_to_file(const char* uri, size_t len, char* out, size_t cap);   /* data: → файл кэша, 1 — ok */

#endif
