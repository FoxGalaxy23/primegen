#include "gal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <locale.h>
#include <stdarg.h>
#include <time.h>

#ifdef HAVE_LOGGER
  #include "logger.h"
#endif

#ifdef HAVE_RENDER
  #include "ttf_render.h"
  extern TTFFont* g_ttf_font;
#endif

static Display* display = NULL;
static Window window;
static int screen_num;
static GC gc;
static XIM xim = NULL;
static XIC xic = NULL;
static XImage* ximage = NULL;
static int mouse_down = 0;
static int mouse_down_x = 0;
static int mouse_down_y = 0;
static int mouse_moved = 0;

static Atom wmDeleteMessage;
static Atom CLIPBOARD, UTF8_STRING, TARGETS;
static char* clipboard_text = NULL;

void gal_log(const char* level, const char* format, ...) {
#ifdef HAVE_LOGGER
    va_list args;
    va_start(args, format);
    custom_logger(level, format, args);
    va_end(args);
#else
    (void)level; (void)format;
#endif
}

int gal_init(void) {
    setlocale(LC_ALL, "");
    display = XOpenDisplay(NULL);
    if (!display) return 0;
    XSetLocaleModifiers("");
    
    screen_num = DefaultScreen(display);
    window = XCreateSimpleWindow(
        display, RootWindow(display, screen_num), 
        10, 10, SCREEN_WIDTH, SCREEN_HEIGHT, 1,
        BlackPixel(display, screen_num), WhitePixel(display, screen_num)
    );
    XStoreName(display, window, "Meetlookers Browser Engine (X11)");
    gc = XCreateGC(display, window, 0, NULL);
    xim = XOpenIM(display, NULL, NULL, NULL);
    
    back_buffer = (uint32_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    ximage = XCreateImage(
        display, DefaultVisual(display, screen_num), 
        24, ZPixmap, 0, (char*)back_buffer, 
        SCREEN_WIDTH, SCREEN_HEIGHT, 32, 0
    );
    
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    
    wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDeleteMessage, 1);

    CLIPBOARD = XInternAtom(display, "CLIPBOARD", False);
    UTF8_STRING = XInternAtom(display, "UTF8_STRING", False);
    TARGETS = XInternAtom(display, "TARGETS", False);

    XMapWindow(display, window);
    if (xim) {
        xic = XCreateIC(xim,
                        XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                        XNClientWindow, window,
                        XNFocusWindow, window,
                        NULL);
        if (xic) XSetICFocus(xic);
    }
    XFlush(display);
    usleep(50000);

#ifdef HAVE_RENDER
    g_ttf_font = ttf_load("assets/font.ttf");
    if (!g_ttf_font) {
        gal_log("WARN", "TTF font not loaded — falling back to bitmap font");
    }
#endif

    return 1;
}

void gal_flush(void) {
    if (ximage) {
        XPutImage(display, window, gc, ximage, 0, 0, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
        XFlush(display);
    }
}

void gal_resize(int width, int height) {
    if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) return;
    SCREEN_WIDTH = width;
    SCREEN_HEIGHT = height;
    if (ximage) { XDestroyImage(ximage); ximage = NULL; back_buffer = NULL; }
    back_buffer = (uint32_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    ximage = XCreateImage(
        display, DefaultVisual(display, screen_num), 24, ZPixmap, 0, 
        (char*)back_buffer, SCREEN_WIDTH, SCREEN_HEIGHT, 32, 0
    );
}

static GAL_Event process_x_event(XEvent* event) {
    GAL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = GAL_EVENT_NONE;

    if (event->type == ClientMessage) {
        if ((Atom)event->xclient.data.l[0] == wmDeleteMessage) {
            ev.type = GAL_EVENT_QUIT;
        }
    } else if (event->type == SelectionRequest) {
        XSelectionRequestEvent* req = &event->xselectionrequest;
        XEvent respond;
        memset(&respond, 0, sizeof(respond));
        respond.xselection.type = SelectionNotify;
        respond.xselection.display = req->display;
        respond.xselection.requestor = req->requestor;
        respond.xselection.selection = req->selection;
        respond.xselection.target = req->target;
        respond.xselection.time = req->time;
        respond.xselection.property = None;

        if (req->target == TARGETS) {
            Atom targets[] = { TARGETS, UTF8_STRING, XA_STRING };
            XChangeProperty(display, req->requestor, req->property, XA_ATOM, 32, PropModeReplace, (unsigned char*)targets, 3);
            respond.xselection.property = req->property;
        } else if (req->target == UTF8_STRING || req->target == XA_STRING) {
            if (clipboard_text) {
                XChangeProperty(display, req->requestor, req->property, req->target, 8, PropModeReplace, (unsigned char*)clipboard_text, strlen(clipboard_text));
                respond.xselection.property = req->property;
            }
        }
        XSendEvent(display, req->requestor, True, 0, &respond);
        XFlush(display);
        ev.type = GAL_EVENT_NONE;
    } else if (event->type == ConfigureNotify) {
        int new_w = event->xconfigure.width;
        int new_h = event->xconfigure.height;
        if (new_w != SCREEN_WIDTH || new_h != SCREEN_HEIGHT) {
            gal_resize(new_w, new_h);
            ev.type = GAL_EVENT_REDRAW;
        }
    } else if (event->type == Expose) {
        ev.type = GAL_EVENT_REDRAW;
    } else if (event->type == ButtonPress) {
        if (event->xbutton.button == Button1 || event->xbutton.button == Button2 || event->xbutton.button == Button3) {
            mouse_down = 1;
            mouse_moved = 0;
            mouse_down_x = event->xbutton.x;
            mouse_down_y = event->xbutton.y;
            ev.type = GAL_EVENT_MOUSE_DOWN;
            ev.mouse_x = event->xbutton.x;
            ev.mouse_y = event->xbutton.y;
            if (event->xbutton.button == Button1) ev.mouse_button = GAL_MOUSE_LEFT;
            else if (event->xbutton.button == Button2) ev.mouse_button = GAL_MOUSE_MIDDLE;
            else if (event->xbutton.button == Button3) ev.mouse_button = GAL_MOUSE_RIGHT;
        } else if (event->xbutton.button == Button4 || event->xbutton.button == Button5 ||
                   event->xbutton.button == 6 || event->xbutton.button == 7) {
            ev.type = GAL_EVENT_MOUSE_SCROLL;
            ev.mouse_x = event->xbutton.x;
            ev.mouse_y = event->xbutton.y;
            if (event->xbutton.button == Button4) ev.scroll_y = -80;
            else if (event->xbutton.button == Button5) ev.scroll_y = 80;
            else if (event->xbutton.button == 6) ev.scroll_x = -80;
            else if (event->xbutton.button == 7) ev.scroll_x = 80;
        }
    } else if (event->type == MotionNotify) {
        ev.type = GAL_EVENT_MOUSE_MOVE;
        ev.mouse_x = event->xmotion.x;
        ev.mouse_y = event->xmotion.y;
        if (mouse_down &&
            (abs(event->xmotion.x - mouse_down_x) > 3 || abs(event->xmotion.y - mouse_down_y) > 3)) {
            mouse_moved = 1;
        }
    } else if (event->type == ButtonRelease) {
        if (event->xbutton.button == Button1 || event->xbutton.button == Button2 || event->xbutton.button == Button3) {
            ev.type = mouse_moved ? GAL_EVENT_MOUSE_UP : GAL_EVENT_MOUSE_CLICK;
            ev.mouse_x = event->xbutton.x;
            ev.mouse_y = event->xbutton.y;
            if (event->xbutton.button == Button1) ev.mouse_button = GAL_MOUSE_LEFT;
            else if (event->xbutton.button == Button2) ev.mouse_button = GAL_MOUSE_MIDDLE;
            else if (event->xbutton.button == Button3) ev.mouse_button = GAL_MOUSE_RIGHT;
            mouse_down = 0;
            mouse_moved = 0;
        }
    } else if (event->type == KeyPress) {
        ev.type = GAL_EVENT_KEY_PRESS;
        char buf[32];
        KeySym keysym;
        Status status = 0;
        int len = 0;
        if (xic) {
            len = Xutf8LookupString(xic, &event->xkey, buf, sizeof(buf) - 1, &keysym, &status);
            if (status == XBufferOverflow) len = 0;
        } else {
            len = XLookupString(&event->xkey, buf, sizeof(buf) - 1, &keysym, NULL);
        }
        if (len > 0) {
            buf[len] = '\0';
            ev.key_char = buf[0];
            memcpy(ev.text, buf, len + 1);
            ev.text_len = len;
        } else {
            ev.key_char = 0;
        }
        ev.key_code = (int)keysym;
    }
    return ev;
}

GAL_Event gal_wait_event(void) {
    XEvent event;
    XNextEvent(display, &event);
    return process_x_event(&event);
}

GAL_Event gal_poll_event(void) {
    XEvent event;
    if (XPending(display)) {
        XNextEvent(display, &event);
        return process_x_event(&event);
    }
    GAL_Event ev;
    ev.type = GAL_EVENT_NONE;
    return ev;
}

void gal_close(void) {
    if (clipboard_text) free(clipboard_text);
#ifdef HAVE_RENDER
    if (g_ttf_font) { ttf_free(g_ttf_font); g_ttf_font = NULL; }
#endif
    if (xic) { XDestroyIC(xic); xic = NULL; }
    if (xim) { XCloseIM(xim); xim = NULL; }
    if (ximage) XDestroyImage(ximage);
    if (display) {
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
    }
}

void gal_set_clipboard(const char* text) {
    if (clipboard_text) free(clipboard_text);
    clipboard_text = strdup(text);
    XSetSelectionOwner(display, CLIPBOARD, window, CurrentTime);
    XFlush(display);
}

char* gal_get_clipboard(void) {
    Atom property = XInternAtom(display, "XSEL_DATA", False);
    XConvertSelection(display, CLIPBOARD, UTF8_STRING, property, window, CurrentTime);
    XFlush(display);

    XEvent event;
    int count = 0;
    while (count < 100) { // Max wait ~100ms
        if (XPending(display)) {
            XNextEvent(display, &event);
            if (event.type == SelectionNotify && event.xselection.selection == CLIPBOARD) {
                if (event.xselection.property == None) break;

                Atom type;
                int format;
                unsigned long nitems, bytes_after;
                unsigned char* prop_data = NULL;
                XGetWindowProperty(display, event.xselection.requestor, event.xselection.property, 0, 1024, True, AnyPropertyType, &type, &format, &nitems, &bytes_after, &prop_data);

                if (prop_data) {
                    char* result = strdup((char*)prop_data);
                    XFree(prop_data);
                    return result;
                }
                break;
            }
        }
        usleep(1000);
        count++;
    }
    return NULL;
}
