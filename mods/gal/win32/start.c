#include "gal.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef HAVE_LOGGER
  #include "logger.h"
#endif

#ifdef HAVE_RENDER
  #include "ttf_render.h"
  extern TTFFont* g_ttf_font;
#endif

static HWND hwnd = NULL;
static BITMAPINFO bmi;
static int mouse_down = 0;
static int mouse_moved = 0;
static int last_mouse_x = 0;
static int last_mouse_y = 0;

static GAL_Event pending_event;
static int has_pending_event = 0;

void gal_log(const char* level, const char* format, ...) {
#ifdef HAVE_LOGGER
    va_list args;
    va_start(args, format);
    custom_logger(level, format, args);
    va_end(args);
#else
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    printf("[%s] %s\n", level, buf);
#endif
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GAL_Event ev;
    ev.type = GAL_EVENT_NONE;
    ev.mouse_x = LOWORD(lParam);
    ev.mouse_y = HIWORD(lParam);
    ev.scroll_x = 0;
    ev.scroll_y = 0;
    ev.key_char = 0;
    ev.text[0] = '\0';
    ev.text_len = 0;
    ev.key_code = 0;

    switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            pending_event.type = GAL_EVENT_QUIT;
            has_pending_event = 1;
            break;
        case WM_SIZE:
            SCREEN_WIDTH = LOWORD(lParam);
            SCREEN_HEIGHT = HIWORD(lParam);
            if (back_buffer) free(back_buffer);
            back_buffer = (uint32_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = SCREEN_WIDTH;
            bmi.bmiHeader.biHeight = -SCREEN_HEIGHT; // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            pending_event.type = GAL_EVENT_REDRAW;
            has_pending_event = 1;
            break;
        case WM_LBUTTONDOWN:
            mouse_down = 1;
            mouse_moved = 0;
            last_mouse_x = ev.mouse_x;
            last_mouse_y = ev.mouse_y;
            ev.type = GAL_EVENT_MOUSE_DOWN;
            pending_event = ev;
            has_pending_event = 1;
            break;
        case WM_LBUTTONUP:
            ev.type = mouse_moved ? GAL_EVENT_MOUSE_UP : GAL_EVENT_MOUSE_CLICK;
            mouse_down = 0;
            mouse_moved = 0;
            pending_event = ev;
            has_pending_event = 1;
            break;
        case WM_MOUSEMOVE:
            ev.type = GAL_EVENT_MOUSE_MOVE;
            if (mouse_down && (abs(ev.mouse_x - last_mouse_x) > 3 || abs(ev.mouse_y - last_mouse_y) > 3)) {
                mouse_moved = 1;
            }
            pending_event = ev;
            has_pending_event = 1;
            break;
        case WM_MOUSEWHEEL: {
            POINT pt;
            pt.x = LOWORD(lParam);
            pt.y = HIWORD(lParam);
            ScreenToClient(hWnd, &pt);
            ev.mouse_x = pt.x;
            ev.mouse_y = pt.y;
            ev.type = GAL_EVENT_MOUSE_SCROLL;
            ev.scroll_y = -((short)HIWORD(wParam));
            pending_event = ev;
            has_pending_event = 1;
            break;
        }
        case WM_CHAR:
            ev.type = GAL_EVENT_KEY_PRESS;
            ev.key_char = (char)wParam;
            ev.text[0] = (char)wParam;
            ev.text[1] = '\0';
            ev.text_len = 1;
            ev.key_code = (int)wParam;
            pending_event = ev;
            has_pending_event = 1;
            break;
        case WM_KEYDOWN:
            // For special keys like arrows
            if (wParam >= VK_LEFT && wParam <= VK_DOWN) {
                ev.type = GAL_EVENT_KEY_PRESS;
                ev.key_char = 0;
                // Map Win32 virtual keys to X11-like codes used in main.c
                // VK_LEFT (25), VK_UP (26), VK_RIGHT (27), VK_DOWN (28)
                // main.c uses: 0xFF51 (Left), 0xFF52 (Up), 0xFF53 (Right), 0xFF54 (Down)
                if (wParam == VK_LEFT) ev.key_code = 0xFF51;
                else if (wParam == VK_UP) ev.key_code = 0xFF52;
                else if (wParam == VK_RIGHT) ev.key_code = 0xFF53;
                else if (wParam == VK_DOWN) ev.key_code = 0xFF54;
                pending_event = ev;
                has_pending_event = 1;
            } else if (wParam == VK_BACK) {
                ev.type = GAL_EVENT_KEY_PRESS;
                ev.key_code = 0xFF08; // X11 BackSpace
                ev.key_char = '\b';
                pending_event = ev;
                has_pending_event = 1;
            } else if (wParam == VK_RETURN) {
                ev.type = GAL_EVENT_KEY_PRESS;
                ev.key_code = 0xFF0D; // X11 Return
                ev.key_char = '\r';
                pending_event = ev;
                has_pending_event = 1;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            gal_flush();
            EndPaint(hWnd, &ps);
            break;
        }
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int gal_init(void) {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "GAL_Win32";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClass(&wc)) return 0;

    hwnd = CreateWindowEx(
        0, "GAL_Win32", "Meetlookers Browser Engine (Win32)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        SCREEN_WIDTH + 16, SCREEN_HEIGHT + 39, // Approximate for client size
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) return 0;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    back_buffer = (uint32_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = SCREEN_WIDTH;
    bmi.bmiHeader.biHeight = -SCREEN_HEIGHT;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

#ifdef HAVE_RENDER
    g_ttf_font = ttf_load("assets/font.ttf");
    if (!g_ttf_font) {
        gal_log("WARN", "TTF font not loaded — falling back to bitmap font");
    }
#endif

    return 1;
}

void gal_flush(void) {
    if (!hwnd || !back_buffer) return;
    HDC hdc = GetDC(hwnd);
    SetDIBitsToDevice(
        hdc, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
        0, 0, 0, SCREEN_HEIGHT,
        back_buffer, &bmi, DIB_RGB_COLORS
    );
    ReleaseDC(hwnd, hdc);
}

void gal_resize(int width, int height) {
    (void)width; (void)height;
    // This is handled in WM_SIZE
}

GAL_Event gal_poll_event(void) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (has_pending_event) break;
    }

    if (has_pending_event) {
        has_pending_event = 0;
        return pending_event;
    }

    GAL_Event ev;
    ev.type = GAL_EVENT_NONE;
    return ev;
}

GAL_Event gal_wait_event(void) {
    MSG msg;
    has_pending_event = 0;
    while (!has_pending_event && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (has_pending_event) {
        return pending_event;
    }
    GAL_Event ev = {0};
    ev.type = GAL_EVENT_QUIT;
    return ev;
}

void gal_close(void) {
#ifdef HAVE_RENDER
    if (g_ttf_font) { ttf_free(g_ttf_font); g_ttf_font = NULL; }
#endif
    if (back_buffer) free(back_buffer);
    if (hwnd) DestroyWindow(hwnd);
}

void gal_set_clipboard(const char* text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (hMem) {
        void* pMem = GlobalLock(hMem);
        if (pMem) {
            memcpy(pMem, text, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
    }
    CloseClipboard();
}

char* gal_get_clipboard(void) {
    if (!OpenClipboard(hwnd)) return NULL;
    HANDLE hData = GetClipboardData(CF_TEXT);
    char* result = NULL;
    if (hData) {
        void* pMem = GlobalLock(hData);
        if (pMem) {
            result = strdup((char*)pMem);
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return result;
}
