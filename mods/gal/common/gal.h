#ifndef GAL_H
#define GAL_H

#include <stdint.h>

extern int SCREEN_WIDTH;
extern int SCREEN_HEIGHT;
extern uint32_t* back_buffer;

typedef enum {
    GAL_EVENT_NONE,
    GAL_EVENT_QUIT,
    GAL_EVENT_REDRAW,
    GAL_EVENT_MOUSE_DOWN,
    GAL_EVENT_MOUSE_UP,
    GAL_EVENT_MOUSE_MOVE,
    GAL_EVENT_MOUSE_CLICK,
    GAL_EVENT_MOUSE_SCROLL,
    GAL_EVENT_KEY_PRESS
} GAL_EventType;

typedef enum {
    GAL_MOUSE_LEFT = 1,
    GAL_MOUSE_MIDDLE = 2,
    GAL_MOUSE_RIGHT = 3
} GAL_MouseButton;

typedef struct {
    GAL_EventType type;
    int mouse_x;
    int mouse_y;
    GAL_MouseButton mouse_button;
    int scroll_x;
    int scroll_y;
    char key_char;
    char text[32];
    int text_len;
    int key_code;
} GAL_Event;

int gal_init(void);
void gal_close(void);
void gal_clear(uint32_t color);
void gal_put_pixel(int x, int y, uint32_t color);
void gal_draw_rect(int x, int y, int w, int h, uint32_t color);
void gal_draw_char(int x, int y, char c, uint32_t color, int scale);
void gal_draw_string(int x, int y, const char* str, uint32_t color, int scale);
void gal_flush(void);
void gal_resize(int width, int height);

// TTF шрифты
void gal_draw_ttf_string(int x, int y, const char* str, uint32_t color, int font_size);
int  gal_ttf_text_width(const char* str, int font_size);

// Блиттинг изображений (массив пикселей → back_buffer)
void gal_blit_image(int x, int y, int w, int h, const uint32_t* pixels);
void gal_blit_image_scaled(int x, int y, int w, int h, const uint32_t* pixels, int src_w, int src_h);

// Clipboard support
void gal_set_clipboard(const char* text);
char* gal_get_clipboard(void); // Returns malloc'd string or NULL

// Теперь возвращает структуру события (блокирующий вызов)
GAL_Event gal_wait_event(void);
// Неблокирующий вызов
GAL_Event gal_poll_event(void);

void gal_log(const char* level, const char* format, ...);

#endif
