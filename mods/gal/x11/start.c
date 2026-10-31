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
#include <wchar.h>
#include <ctype.h>

#ifdef HAVE_LOGGER
  #include "logger.h"
#endif

#ifdef HAVE_RENDER
  #include "ttf_render.h"
  extern TTFFont* g_ttf_font;
#endif

static Display* display = NULL;
static Window window;
static uint32_t* g_presented_pixels;
static size_t g_presented_capacity;
static int g_presented_valid;
#define GAL_DIRTY_TILE 32
#define GAL_DIRTY_MAX_RECTS 256

static void gal_present_snapshot_reset(void) {
    free(g_presented_pixels);
    g_presented_pixels = NULL;
    g_presented_capacity = 0;
    g_presented_valid = 0;
}

static int gal_present_snapshot_ensure(void) {
    if (SCREEN_WIDTH <= 0 || SCREEN_HEIGHT <= 0) return 0;
    size_t pixels = (size_t)SCREEN_WIDTH * (size_t)SCREEN_HEIGHT;
    if (pixels > SIZE_MAX / sizeof(uint32_t)) return 0;
    if (g_presented_capacity != pixels) {
        uint32_t* next = (uint32_t*)realloc(g_presented_pixels, pixels * sizeof(uint32_t));
        if (!next) { gal_present_snapshot_reset(); return 0; }
        g_presented_pixels = next;
        g_presented_capacity = pixels;
        g_presented_valid = 0;
    }
    return 1;
}

static void gal_present_full(void);
static int screen_num;
static GC gc;
static XIM xim = NULL;
static XIC xic = NULL;
static XImage* ximage = NULL;
static void gal_present_full(void) {
    XPutImage(display, window, gc, ximage, 0, 0, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    if (gal_present_snapshot_ensure()) {
        memcpy(g_presented_pixels, back_buffer,
               (size_t)SCREEN_WIDTH * (size_t)SCREEN_HEIGHT * sizeof(uint32_t));
        g_presented_valid = 1;
    }
}
static int mouse_down = 0;
static int mouse_down_x = 0;
static int mouse_down_y = 0;
static int mouse_moved = 0;
static GAL_Event gal_event_queue[64];
static int gal_qhead = 0, gal_qcount = 0;
static char xim_preedit_text[256];
static void gal_queue_event(const GAL_Event* e) {
    if (!e || e->type == GAL_EVENT_NONE || gal_qcount >= (int)(sizeof gal_event_queue / sizeof gal_event_queue[0])) return;
    gal_event_queue[(gal_qhead + gal_qcount) % (int)(sizeof gal_event_queue / sizeof gal_event_queue[0])] = *e; gal_qcount++;
}
static int gal_pop_queued(GAL_Event* e) {
    if (!gal_qcount) return 0; *e = gal_event_queue[gal_qhead]; gal_qhead = (gal_qhead + 1) % (int)(sizeof gal_event_queue / sizeof gal_event_queue[0]); gal_qcount--; return 1;
}
static GAL_Event composition_event(int kind, const char* text) {
    GAL_Event ev; memset(&ev,0,sizeof ev); ev.type=GAL_EVENT_COMPOSITION; ev.composition_type=kind;
    if (text) { size_t n=strlen(text); if(n>=sizeof ev.text)n=sizeof ev.text-1; memcpy(ev.text,text,n);ev.text[n]=0;ev.text_len=(int)n; } return ev;
}
static size_t utf8_byte_offset(const char* s, size_t chars) {
    size_t i=0; if(!s)return 0;
    while(s[i]&&chars){unsigned char c=(unsigned char)s[i];if((c&0xc0)!=0x80)chars--;i++;}
    return i;
}
static size_t utf8_complete_prefix(const char* s, size_t cap) {
    size_t i=0;while(s&&s[i]&&i<cap){unsigned char c=(unsigned char)s[i];size_t need=c<0x80?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:1;if(i+need>cap)break;i+=need;}return i;
}
static void xim_apply_preedit(int first,int remove_chars,const char* insert) {
    size_t oldlen=strlen(xim_preedit_text),a=utf8_byte_offset(xim_preedit_text,first<0?0:(size_t)first),b=utf8_byte_offset(xim_preedit_text,(size_t)(first<0?0:first)+(size_t)(remove_chars<0?0:remove_chars));
    if(a>oldlen)a=oldlen;if(b<a)b=a;if(b>oldlen)b=oldlen;
    char next[sizeof xim_preedit_text];size_t pos=0,cap=sizeof next-1;size_t n=a;if(n>cap)n=cap;memcpy(next,xim_preedit_text,n);pos=n;
    if(insert&&*insert&&pos<cap){n=utf8_complete_prefix(insert,cap-pos);memcpy(next+pos,insert,n);pos+=n;}
    if(b<oldlen&&pos<cap){n=utf8_complete_prefix(xim_preedit_text+b,cap-pos);memcpy(next+pos,xim_preedit_text+b,n);pos+=n;}
    next[pos]=0;memcpy(xim_preedit_text,next,pos+1);
}
static int xim_preedit_start(XIC ic, XPointer client, XPointer call) { (void)ic;(void)client;(void)call; xim_preedit_text[0]=0; GAL_Event e=composition_event(0,"");gal_queue_event(&e);return 256; }
static void xim_preedit_done(XIC ic, XPointer client, XPointer call) { (void)ic;(void)client;(void)call; GAL_Event e=composition_event(2,"");gal_queue_event(&e);xim_preedit_text[0]=0; }
static void xim_preedit_draw(XIC ic, XPointer client, XPointer call) {
    (void)ic;(void)client; XIMPreeditDrawCallbackStruct* d=(XIMPreeditDrawCallbackStruct*)call; char buf[256]={0};
    if(d&&d->text&&d->text->length>0){size_t n=(size_t)d->text->length;if(d->text->encoding_is_wchar){wchar_t tmp[256];if(n>255)n=255;if(n)memcpy(tmp,d->text->string.wide_char,n*sizeof(wchar_t));tmp[n]=0;size_t m=wcstombs(buf,tmp,sizeof buf-1);if(m==(size_t)-1)buf[0]=0;else buf[m<sizeof buf?m:sizeof buf-1]=0;}else{if(n>sizeof buf-1)n=sizeof buf-1;memcpy(buf,d->text->string.multi_byte,n);buf[n]=0;}}
    if(d)xim_apply_preedit(d->chg_first,d->chg_length,buf);
    GAL_Event e=composition_event(1,xim_preedit_text);gal_queue_event(&e);
}

static Atom wmDeleteMessage;
static Atom CLIPBOARD, UTF8_STRING, TARGETS;
static Atom xdnd_aware, xdnd_enter, xdnd_position, xdnd_status, xdnd_drop, xdnd_finished, xdnd_selection, xdnd_type_list, xdnd_action_copy, text_uri_list, xdnd_property;
static Window xdnd_source = None;
static int xdnd_accept = 0, xdnd_x = 0, xdnd_y = 0;
static char* clipboard_text = NULL;
static int uri_hex(int c){if(c>='0'&&c<='9')return c-'0';c=tolower((unsigned char)c);if(c>='a'&&c<='f')return c-'a'+10;return -1;}
static void queue_uri_list(const unsigned char* data, size_t n) {
    size_t i=0;while(i<n){size_t j=i;while(j<n&&data[j]!='\n'&&data[j]!='\r')j++;size_t len=j-i;
        if(len>7&&len<4096&&!memcmp(data+i,"file://",7)){
            char uri[4096];memcpy(uri,data+i,len);uri[len]=0;char* path=uri+7;
            if(!strncmp(path,"localhost/",10))path+=9;else if(*path!='/'&&strncmp(path,"localhost",9)!=0){i=j+1;continue;}
            char decoded[1024];size_t o=0;for(size_t k=0;path[k]&&o<sizeof decoded-1;k++){if(path[k]=='%'&&path[k+1]&&path[k+2]){int a=uri_hex(path[k+1]),b=uri_hex(path[k+2]);if(a>=0&&b>=0){decoded[o++]=(char)((a<<4)|b);k+=2;continue;}}decoded[o++]=path[k];}decoded[o]=0;
            if(decoded[0]=='/'&&o){GAL_Event e;memset(&e,0,sizeof e);e.type=GAL_EVENT_FILE_DROP;e.mouse_x=xdnd_x;e.mouse_y=xdnd_y;snprintf(e.file_path,sizeof e.file_path,"%s",decoded);gal_queue_event(&e);}
        }i=j+1;
    }
}
static void send_xdnd_message(Window target, Atom type, long d0, long d1, long d2, long d3, long d4){XEvent e;memset(&e,0,sizeof e);e.xclient.type=ClientMessage;e.xclient.display=display;e.xclient.window=target;e.xclient.message_type=type;e.xclient.format=32;e.xclient.data.l[0]=d0;e.xclient.data.l[1]=d1;e.xclient.data.l[2]=d2;e.xclient.data.l[3]=d3;e.xclient.data.l[4]=d4;XSendEvent(display,target,False,NoEventMask,&e);XFlush(display);}


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
    (void)gal_present_snapshot_ensure();
    
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    
    wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDeleteMessage, 1);

    CLIPBOARD = XInternAtom(display, "CLIPBOARD", False);
    UTF8_STRING = XInternAtom(display, "UTF8_STRING", False);
    TARGETS = XInternAtom(display, "TARGETS", False);
    xdnd_aware=XInternAtom(display,"XdndAware",False);xdnd_enter=XInternAtom(display,"XdndEnter",False);xdnd_position=XInternAtom(display,"XdndPosition",False);xdnd_status=XInternAtom(display,"XdndStatus",False);xdnd_drop=XInternAtom(display,"XdndDrop",False);xdnd_finished=XInternAtom(display,"XdndFinished",False);xdnd_selection=XInternAtom(display,"XdndSelection",False);xdnd_type_list=XInternAtom(display,"XdndTypeList",False);xdnd_action_copy=XInternAtom(display,"XdndActionCopy",False);text_uri_list=XInternAtom(display,"text/uri-list",False);xdnd_property=XInternAtom(display,"MLBrowserXdndSelection",False);
    { unsigned long version=5; XChangeProperty(display,window,xdnd_aware,XA_ATOM,32,PropModeReplace,(unsigned char*)&version,1); }

    XMapWindow(display, window);
    if (xim) {
        int callbacks_ok=0; XIMStyles* styles=NULL;
        if(XGetIMValues(xim,XNQueryInputStyle,&styles,NULL)==NULL&&styles){for(unsigned short i=0;i<styles->count_styles;i++)if(styles->supported_styles[i]==(XIMPreeditCallbacks|XIMStatusNothing))callbacks_ok=1;XFree(styles);}
        if(callbacks_ok){
            XIMCallback start_cb,draw_cb,done_cb;start_cb.client_data=draw_cb.client_data=done_cb.client_data=NULL;start_cb.callback=(XIMProc)xim_preedit_start;draw_cb.callback=(XIMProc)xim_preedit_draw;done_cb.callback=(XIMProc)xim_preedit_done;
            xic=XCreateIC(xim,XNInputStyle,XIMPreeditCallbacks|XIMStatusNothing,XNClientWindow,window,XNFocusWindow,window,XNPreeditStartCallback,&start_cb,XNPreeditDrawCallback,&draw_cb,XNPreeditDoneCallback,&done_cb,NULL);
        }
        if(!xic)xic=XCreateIC(xim,XNInputStyle,XIMPreeditNothing|XIMStatusNothing,XNClientWindow,window,XNFocusWindow,window,NULL);
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
        static int profile_checked, profile_enabled;
        static double window_started;
        static unsigned calls, rect_total, full_frames;
        static double put_total, flush_total, put_max, flush_max;
        double t0 = 0.0, t1 = 0.0, t2 = 0.0;
        int submitted = 0, rects = 0, full = 0;
        if (!profile_checked) {
            const char* value = getenv("MLB_PROFILE_RENDER");
            profile_enabled = value && value[0] == '1';
            profile_checked = 1;
            if (profile_enabled) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                window_started = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
            }
        }
        if (profile_enabled) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            t0 = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
        }

        if (!g_presented_valid || !gal_present_snapshot_ensure()) {
            gal_present_full();
            submitted = 1; rects = 1; full = 1;
        } else {
            int tile_cols = (SCREEN_WIDTH + GAL_DIRTY_TILE - 1) / GAL_DIRTY_TILE;
            int tile_rows = (SCREEN_HEIGHT + GAL_DIRTY_TILE - 1) / GAL_DIRTY_TILE;
            size_t tile_count = (size_t)tile_cols * (size_t)tile_rows;
            unsigned char* changed = tile_count ? (unsigned char*)calloc(tile_count, 1) : NULL;
            if (!changed) {
                gal_present_full();
                submitted = 1; rects = 1; full = 1;
            } else {
                size_t dirty_tiles = 0;
                int runs = 0;
                for (int ty = 0; ty < tile_rows; ty++) {
                    int y0 = ty * GAL_DIRTY_TILE;
                    int th = SCREEN_HEIGHT - y0;
                    if (th > GAL_DIRTY_TILE) th = GAL_DIRTY_TILE;
                    for (int tx = 0; tx < tile_cols; tx++) {
                        int x0 = tx * GAL_DIRTY_TILE;
                        int tw = SCREEN_WIDTH - x0;
                        if (tw > GAL_DIRTY_TILE) tw = GAL_DIRTY_TILE;
                        int dirty = 0;
                        for (int row = 0; row < th; row++) {
                            size_t off = (size_t)(y0 + row) * (size_t)SCREEN_WIDTH + (size_t)x0;
                            size_t bytes = (size_t)tw * sizeof(uint32_t);
                            if (memcmp(back_buffer + off, g_presented_pixels + off, bytes) != 0) {
                                dirty = 1; break;
                            }
                        }
                        if (dirty) { changed[(size_t)ty * (size_t)tile_cols + (size_t)tx] = 1; dirty_tiles++; }
                    }
                }
                for (int ty = 0; ty < tile_rows; ty++) {
                    int in_run = 0;
                    for (int tx = 0; tx <= tile_cols; tx++) {
                        int is_dirty = tx < tile_cols && changed[(size_t)ty * (size_t)tile_cols + (size_t)tx];
                        if (is_dirty && !in_run) { runs++; in_run = 1; }
                        else if (!is_dirty) in_run = 0;
                    }
                }
                /* Sparse frames use horizontal tile runs. Dense frames avoid
                   hundreds of tiny X requests and retain a single full blit. */
                if (dirty_tiles * 2 >= tile_count || runs > GAL_DIRTY_MAX_RECTS) {
                    gal_present_full();
                    submitted = 1; rects = 1; full = 1;
                } else if (dirty_tiles) {
                    for (int ty = 0; ty < tile_rows; ty++) {
                        int tx = 0;
                        int y0 = ty * GAL_DIRTY_TILE;
                        int th = SCREEN_HEIGHT - y0;
                        if (th > GAL_DIRTY_TILE) th = GAL_DIRTY_TILE;
                        while (tx < tile_cols) {
                            while (tx < tile_cols && !changed[(size_t)ty * (size_t)tile_cols + (size_t)tx]) tx++;
                            if (tx >= tile_cols) break;
                            int first = tx;
                            while (tx < tile_cols && changed[(size_t)ty * (size_t)tile_cols + (size_t)tx]) tx++;
                            int x0 = first * GAL_DIRTY_TILE;
                            int width = tx * GAL_DIRTY_TILE - x0;
                            if (x0 + width > SCREEN_WIDTH) width = SCREEN_WIDTH - x0;
                            XPutImage(display, window, gc, ximage, x0, y0, x0, y0,
                                      (unsigned int)width, (unsigned int)th);
                            for (int row = 0; row < th; row++) {
                                size_t off = (size_t)(y0 + row) * (size_t)SCREEN_WIDTH + (size_t)x0;
                                memcpy(g_presented_pixels + off, back_buffer + off,
                                       (size_t)width * sizeof(uint32_t));
                            }
                            submitted = 1; rects++;
                        }
                    }
                }
                free(changed);
            }
        }
        if (profile_enabled) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            t1 = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
        }
        if (submitted) XFlush(display);
        if (profile_enabled) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            t2 = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
            double put_ms = t1 - t0, flush_ms = t2 - t1;
            put_total += put_ms; flush_total += flush_ms;
            if (put_ms > put_max) put_max = put_ms;
            if (flush_ms > flush_max) flush_max = flush_ms;
            calls++; rect_total += (unsigned)rects; full_frames += (unsigned)full;
            if (t2 - window_started >= 1000.0) {
                double elapsed = t2 - window_started;
                gal_log("INFO", "[perf] X11 flush calls %.1f/s | dirty blits %u avg/call | full frames %u | diff+XPutImage avg %.2f ms max %.2f ms | XFlush avg %.2f ms max %.2f ms | framebuffer %dx%d",
                        (double)calls * 1000.0 / elapsed,
                        calls ? (unsigned)(rect_total / calls) : 0, full_frames,
                        calls ? put_total / calls : 0.0, put_max,
                        calls ? flush_total / calls : 0.0, flush_max,
                        SCREEN_WIDTH, SCREEN_HEIGHT);
                window_started = t2; calls = rect_total = full_frames = 0;
                put_total = flush_total = put_max = flush_max = 0.0;
            }
        }
    }
}

void gal_resize(int width, int height) {
    if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) return;
    SCREEN_WIDTH = width;
    SCREEN_HEIGHT = height;
    gal_present_snapshot_reset();
    if (ximage) { XDestroyImage(ximage); ximage = NULL; back_buffer = NULL; }
    back_buffer = (uint32_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    ximage = XCreateImage(
        display, DefaultVisual(display, screen_num), 24, ZPixmap, 0, 
        (char*)back_buffer, SCREEN_WIDTH, SCREEN_HEIGHT, 32, 0
    );
    (void)gal_present_snapshot_ensure();
}

static GAL_Event process_x_event(XEvent* event) {
    GAL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = GAL_EVENT_NONE;

    if (event->type == KeyPress || event->type == KeyRelease) {
        Bool filtered = XFilterEvent(event, window);
        if (filtered) { if (gal_pop_queued(&ev)) return ev; return ev; }
    }
    if (event->type == ClientMessage && event->xclient.message_type == xdnd_enter) {
        xdnd_source=(Window)event->xclient.data.l[0];xdnd_accept=0;
        Atom types[16];int nt=0;
        if(!(event->xclient.data.l[1]&1)){for(int i=2;i<5;i++)if(event->xclient.data.l[i])types[nt++]=(Atom)event->xclient.data.l[i];}
        else {Atom actual=None;int fmt=0;unsigned long n=0,after=0;unsigned char* prop=NULL;if(XGetWindowProperty(display,xdnd_source,xdnd_type_list,0,64,False,XA_ATOM,&actual,&fmt,&n,&after,&prop)==Success&&prop&&actual==XA_ATOM&&fmt==32){unsigned long* av=(unsigned long*)prop;for(unsigned long i=0;i<n&&nt<16;i++)types[nt++]=(Atom)av[i];}if(prop)XFree(prop);}
        for(int i=0;i<nt;i++)if(types[i]==text_uri_list)xdnd_accept=1;
        ev.type=GAL_EVENT_NONE;
    } else if(event->type == ClientMessage && event->xclient.message_type == xdnd_position) {
        xdnd_source=(Window)event->xclient.data.l[0];int rx=(short)((event->xclient.data.l[2]>>16)&0xffff),ry=(short)(event->xclient.data.l[2]&0xffff);Window child=None;XTranslateCoordinates(display,RootWindow(display,screen_num),window,rx,ry,&xdnd_x,&xdnd_y,&child);
        send_xdnd_message(xdnd_source,xdnd_status,(long)window,xdnd_accept?1:0,0,0,xdnd_accept?xdnd_action_copy:None);ev.type=GAL_EVENT_NONE;
    } else if(event->type == ClientMessage && event->xclient.message_type == xdnd_drop) {
        xdnd_source=(Window)event->xclient.data.l[0];if(xdnd_accept)XConvertSelection(display,xdnd_selection,text_uri_list,xdnd_property,window,(Time)event->xclient.data.l[2]);else send_xdnd_message(xdnd_source,xdnd_finished,(long)window,0,0,0,0);ev.type=GAL_EVENT_NONE;
    } else if(event->type==SelectionNotify && event->xselection.selection==xdnd_selection) {
        Atom actual=None;int fmt=0;unsigned long n=0,after=0;unsigned char* data=NULL;int before=gal_qcount;
        if(event->xselection.property!=None&&XGetWindowProperty(display,window,xdnd_property,0,262144,True,AnyPropertyType,&actual,&fmt,&n,&after,&data)==Success&&data&&fmt==8){queue_uri_list(data,(size_t)n);XFree(data);}else if(data)XFree(data);
        int accepted=gal_qcount>before;if(xdnd_source!=None)send_xdnd_message(xdnd_source,xdnd_finished,(long)window,accepted?1:0,0,0,accepted?xdnd_action_copy:0);xdnd_source=None;xdnd_accept=0;ev.type=GAL_EVENT_NONE;
    } else if (event->type == ClientMessage) {
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
        g_presented_valid = 0;
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
        ev.modifiers = 0;
        if (event->xkey.state & ControlMask) ev.modifiers |= GAL_MOD_CTRL;
        if (event->xkey.state & ShiftMask) ev.modifiers |= GAL_MOD_SHIFT;
        if (event->xkey.state & Mod1Mask) ev.modifiers |= GAL_MOD_ALT;
        if (event->xkey.state & Mod4Mask) ev.modifiers |= GAL_MOD_SUPER;
    }
    if(gal_qcount>0){if(ev.type!=GAL_EVENT_NONE)gal_queue_event(&ev);gal_pop_queued(&ev);return ev;}
    return ev;
}

GAL_Event gal_wait_event(void) {
    GAL_Event ev;XEvent event;
    while(1){if(gal_pop_queued(&ev))return ev;XNextEvent(display,&event);ev=process_x_event(&event);if(ev.type!=GAL_EVENT_NONE)return ev;}
}

GAL_Event gal_poll_event(void) {
    GAL_Event ev;XEvent event;if(gal_pop_queued(&ev))return ev;
    if (XPending(display)) { XNextEvent(display, &event); ev=process_x_event(&event); if(ev.type!=GAL_EVENT_NONE)return ev; if(gal_pop_queued(&ev))return ev; }
    memset(&ev,0,sizeof ev);ev.type = GAL_EVENT_NONE;return ev;
}

void gal_close(void) {
    if (clipboard_text) free(clipboard_text);
#ifdef HAVE_RENDER
    if (g_ttf_font) { ttf_free(g_ttf_font); g_ttf_font = NULL; }
#endif
    if (xic) { XDestroyIC(xic); xic = NULL; }
    if (xim) { XCloseIM(xim); xim = NULL; }
    if (ximage) XDestroyImage(ximage);
    ximage = NULL; back_buffer = NULL;
    gal_present_snapshot_reset();
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
