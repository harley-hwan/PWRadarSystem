/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_platform_x11.c
 *  Purpose : X11 implementation of the platform layer.  libX11 only - no Xext,
 *            no toolkit, no GL.
 *
 *  The framebuffer is a client-side buffer wrapped in an XImage and pushed
 *  with XPutImage.  MIT-SHM would avoid the copy but drags in libXext, so the
 *  plain path is used deliberately: at the console's frame rate the transfer
 *  is not the bottleneck, and the dependency list stays at exactly one library
 *  that every X installation already provides.
 *
 *  Only 24/32-bit TrueColor visuals are supported, which is every desktop
 *  since roughly 2005; anything else is reported as an error rather than
 *  silently rendered wrong.
 *
 *  Language: ISO C17
 * ========================================================================== */
#if !defined(_WIN32)

#if !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1
#endif
#if !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "ui_platform.h"

#define UI_EVENT_QUEUE 256u

struct UI_Platform
{
    Display*  dpy;
    int       screen;
    Window    win;
    GC        gc;
    Visual*   visual;
    int       depth;
    XImage*   image;
    uint32_t* pixels;
    int32_t   width, height;
    Atom      wm_delete;
    Cursor    cursors[UI_CURSOR_COUNT];
    UI_Cursor cursor;

    UI_Event  queue[UI_EVENT_QUEUE];
    uint32_t  q_head, q_tail;

    int32_t   mouse_x, mouse_y;
    uint32_t  mods;
    Time      last_press_time;
    int32_t   last_press_x, last_press_y;
};

/* --------------------------------------------------------------------------
 *  Event queue
 * ------------------------------------------------------------------------ */
static void ui_push(UI_Platform* p, const UI_Event* ev)
{
    const uint32_t next = (p->q_tail + 1u) % UI_EVENT_QUEUE;
    if (next == p->q_head) { return; }
    p->queue[p->q_tail] = *ev;
    p->q_tail = next;
}

/* --------------------------------------------------------------------------
 *  Framebuffer
 * ------------------------------------------------------------------------ */
static void ui_release_image(UI_Platform* p)
{
    if (p->image != NULL)
    {
        /* XDestroyImage frees image->data, which is our buffer. */
        XDestroyImage(p->image);
        p->image  = NULL;
        p->pixels = NULL;
    }
    else if (p->pixels != NULL)
    {
        free(p->pixels);
        p->pixels = NULL;
    }
}

static int ui_resize_image(UI_Platform* p, int32_t w, int32_t h)
{
    uint32_t* buf;
    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }
    if (p->pixels != NULL && w == p->width && h == p->height) { return 1; }

    buf = (uint32_t*)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (buf == NULL) { return 0; }

    ui_release_image(p);
    p->image = XCreateImage(p->dpy, p->visual, (unsigned)p->depth, ZPixmap, 0,
                            (char*)buf, (unsigned)w, (unsigned)h, 32,
                            (int)((size_t)w * sizeof(uint32_t)));
    if (p->image == NULL) { free(buf); return 0; }
    p->pixels = buf;
    p->width  = w;
    p->height = h;
    return 1;
}

/* --------------------------------------------------------------------------
 *  Key translation
 * ------------------------------------------------------------------------ */
static int32_t ui_translate_keysym(KeySym ks)
{
    if (ks >= XK_a && ks <= XK_z) { return (int32_t)('A' + (ks - XK_a)); }
    if (ks >= XK_A && ks <= XK_Z) { return (int32_t)ks; }
    if (ks >= XK_0 && ks <= XK_9) { return (int32_t)ks; }
    if (ks >= XK_F1 && ks <= XK_F12)
    {
        return UI_KEY_F1 + (int32_t)(ks - XK_F1);
    }
    switch (ks)
    {
    case XK_Left:      return UI_KEY_LEFT;
    case XK_Right:     return UI_KEY_RIGHT;
    case XK_Up:        return UI_KEY_UP;
    case XK_Down:      return UI_KEY_DOWN;
    case XK_Home:      return UI_KEY_HOME;
    case XK_End:       return UI_KEY_END;
    case XK_Prior:     return UI_KEY_PAGE_UP;
    case XK_Next:      return UI_KEY_PAGE_DOWN;
    case XK_Insert:    return UI_KEY_INSERT;
    case XK_Delete:    return UI_KEY_DELETE;
    case XK_BackSpace: return UI_KEY_BACKSPACE;
    case XK_Tab:       return UI_KEY_TAB;
    case XK_Return:
    case XK_KP_Enter:  return UI_KEY_ENTER;
    case XK_Escape:    return UI_KEY_ESCAPE;
    case XK_space:     return UI_KEY_SPACE;
    case XK_plus:
    case XK_equal:     return '+';
    case XK_minus:     return '-';
    case XK_period:    return '.';
    case XK_comma:     return ',';
    default: break;
    }
    return UI_KEY_UNKNOWN;
}

static uint32_t ui_translate_state(unsigned int state)
{
    uint32_t m = 0u;
    if ((state & ShiftMask)   != 0u) { m |= UI_MOD_SHIFT; }
    if ((state & ControlMask) != 0u) { m |= UI_MOD_CTRL;  }
    if ((state & Mod1Mask)    != 0u) { m |= UI_MOD_ALT;   }
    return m;
}

/* --------------------------------------------------------------------------
 *  Life cycle
 * ------------------------------------------------------------------------ */
UI_Platform* ui_plat_create(const char* title, int32_t w, int32_t h,
                            char* err, size_t err_cap)
{
    UI_Platform* p;
    XSetWindowAttributes swa;
    XVisualInfo want, *got;
    int n_visuals = 0;

    p = (UI_Platform*)calloc(1u, sizeof(*p));
    if (p == NULL)
    {
        if (err != NULL) { (void)snprintf(err, err_cap, "out of memory"); }
        return NULL;
    }

    p->dpy = XOpenDisplay(NULL);
    if (p->dpy == NULL)
    {
        if (err != NULL)
        {
            (void)snprintf(err, err_cap,
                           "cannot open the X display (is DISPLAY set?)");
        }
        free(p);
        return NULL;
    }
    p->screen = DefaultScreen(p->dpy);

    /* Insist on a 24- or 32-bit TrueColor visual so the framebuffer layout
     * matches 0xAARRGGBB with no per-pixel conversion. */
    memset(&want, 0, sizeof(want));
    want.screen = p->screen;
    want.class  = TrueColor;
    want.depth  = 24;
    got = XGetVisualInfo(p->dpy, VisualScreenMask | VisualClassMask | VisualDepthMask,
                         &want, &n_visuals);
    if (got == NULL || n_visuals == 0)
    {
        if (got != NULL) { XFree(got); }
        want.depth = 32;
        got = XGetVisualInfo(p->dpy, VisualScreenMask | VisualClassMask |
                             VisualDepthMask, &want, &n_visuals);
    }
    if (got == NULL || n_visuals == 0)
    {
        if (err != NULL)
        {
            (void)snprintf(err, err_cap,
                           "no 24/32-bit TrueColor visual on this display");
        }
        if (got != NULL) { XFree(got); }
        XCloseDisplay(p->dpy);
        free(p);
        return NULL;
    }
    p->visual = got[0].visual;
    p->depth  = got[0].depth;
    XFree(got);

    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = BlackPixel(p->dpy, p->screen);
    swa.border_pixel     = BlackPixel(p->dpy, p->screen);
    swa.colormap         = XCreateColormap(p->dpy, RootWindow(p->dpy, p->screen),
                                           p->visual, AllocNone);
    swa.event_mask       = ExposureMask | StructureNotifyMask |
                           KeyPressMask | KeyReleaseMask |
                           ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | LeaveWindowMask | FocusChangeMask;

    p->win = XCreateWindow(p->dpy, RootWindow(p->dpy, p->screen),
                           0, 0, (unsigned)w, (unsigned)h, 0,
                           p->depth, InputOutput, p->visual,
                           CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
                           &swa);
    if (p->win == 0)
    {
        if (err != NULL) { (void)snprintf(err, err_cap, "XCreateWindow failed"); }
        XCloseDisplay(p->dpy);
        free(p);
        return NULL;
    }

    XStoreName(p->dpy, p->win, (title != NULL) ? title : "PWRadarUI");
    p->wm_delete = XInternAtom(p->dpy, "WM_DELETE_WINDOW", False);
    (void)XSetWMProtocols(p->dpy, p->win, &p->wm_delete, 1);

    /* Ask the window manager not to shrink the console below a usable size. */
    {
        XSizeHints* sh = XAllocSizeHints();
        if (sh != NULL)
        {
            sh->flags      = PMinSize;
            sh->min_width  = 960;
            sh->min_height = 640;
            XSetWMNormalHints(p->dpy, p->win, sh);
            XFree(sh);
        }
    }

    p->gc = XCreateGC(p->dpy, p->win, 0u, NULL);
    if (p->gc == NULL || ui_resize_image(p, w, h) == 0)
    {
        if (err != NULL) { (void)snprintf(err, err_cap, "XImage creation failed"); }
        ui_plat_destroy(p);
        return NULL;
    }

    p->cursors[UI_CURSOR_ARROW]   = XCreateFontCursor(p->dpy, XC_left_ptr);
    p->cursors[UI_CURSOR_HAND]    = XCreateFontCursor(p->dpy, XC_hand2);
    p->cursors[UI_CURSOR_CROSS]   = XCreateFontCursor(p->dpy, XC_crosshair);
    p->cursors[UI_CURSOR_SIZE_WE] = XCreateFontCursor(p->dpy, XC_sb_h_double_arrow);
    p->cursors[UI_CURSOR_SIZE_NS] = XCreateFontCursor(p->dpy, XC_sb_v_double_arrow);
    p->cursors[UI_CURSOR_TEXT]    = XCreateFontCursor(p->dpy, XC_xterm);
    p->cursor = UI_CURSOR_ARROW;
    XDefineCursor(p->dpy, p->win, p->cursors[UI_CURSOR_ARROW]);

    XMapWindow(p->dpy, p->win);
    (void)XFlush(p->dpy);
    return p;
}

void ui_plat_destroy(UI_Platform* p)
{
    int i;
    if (p == NULL) { return; }
    if (p->dpy != NULL)
    {
        for (i = 0; i < UI_CURSOR_COUNT; ++i)
        {
            if (p->cursors[i] != 0) { XFreeCursor(p->dpy, p->cursors[i]); }
        }
        ui_release_image(p);
        if (p->gc  != NULL) { XFreeGC(p->dpy, p->gc); }
        if (p->win != 0)    { XDestroyWindow(p->dpy, p->win); }
        XCloseDisplay(p->dpy);
    }
    free(p);
}

static void ui_pump(UI_Platform* p)
{
    while (XPending(p->dpy) > 0)
    {
        XEvent xe;
        UI_Event ev;
        XNextEvent(p->dpy, &xe);
        memset(&ev, 0, sizeof(ev));

        switch (xe.type)
        {
        case ClientMessage:
            if ((Atom)xe.xclient.data.l[0] == p->wm_delete)
            {
                ev.type = UI_EV_QUIT;
                ui_push(p, &ev);
            }
            break;

        case ConfigureNotify:
            if (xe.xconfigure.width != p->width ||
                xe.xconfigure.height != p->height)
            {
                if (ui_resize_image(p, xe.xconfigure.width,
                                    xe.xconfigure.height) != 0)
                {
                    ev.type   = UI_EV_RESIZE;
                    ev.width  = p->width;
                    ev.height = p->height;
                    ui_push(p, &ev);
                }
            }
            break;

        case Expose:
            /* Re-present the last frame so occlusion never shows garbage. */
            if (p->image != NULL && xe.xexpose.count == 0)
            {
                (void)XPutImage(p->dpy, p->win, p->gc, p->image, 0, 0, 0, 0,
                                (unsigned)p->width, (unsigned)p->height);
            }
            break;

        case MotionNotify:
            p->mouse_x = xe.xmotion.x;
            p->mouse_y = xe.xmotion.y;
            p->mods    = ui_translate_state(xe.xmotion.state);
            ev.type = UI_EV_MOUSE_MOVE;
            ev.x = p->mouse_x; ev.y = p->mouse_y; ev.mods = p->mods;
            ui_push(p, &ev);
            break;

        case LeaveNotify:
            ev.type = UI_EV_MOUSE_LEAVE;
            ev.x = p->mouse_x; ev.y = p->mouse_y;
            ui_push(p, &ev);
            break;

        case ButtonPress:
        case ButtonRelease:
            p->mouse_x = xe.xbutton.x;
            p->mouse_y = xe.xbutton.y;
            p->mods    = ui_translate_state(xe.xbutton.state);
            if (xe.xbutton.button == Button4 || xe.xbutton.button == Button5)
            {
                if (xe.type == ButtonPress)
                {
                    ev.type  = UI_EV_WHEEL;
                    ev.wheel = (xe.xbutton.button == Button4) ? 1 : -1;
                    ev.x = p->mouse_x; ev.y = p->mouse_y; ev.mods = p->mods;
                    ui_push(p, &ev);
                }
                break;
            }
            ev.type = (xe.type == ButtonPress) ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP;
            ev.x = p->mouse_x; ev.y = p->mouse_y; ev.mods = p->mods;
            ev.button = (xe.xbutton.button == Button2) ? UI_MB_MIDDLE
                      : ((xe.xbutton.button == Button3) ? UI_MB_RIGHT : UI_MB_LEFT);
            if (xe.type == ButtonPress && ev.button == UI_MB_LEFT)
            {
                /* X11 has no double-click notion, so synthesise one with the
                 * conventional 400 ms / 4 px window and flag it the same way
                 * the Win32 back end does. */
                const long dt = (long)xe.xbutton.time - (long)p->last_press_time;
                if (dt > 0 && dt < 400 &&
                    labs(p->last_press_x - ev.x) <= 4 &&
                    labs(p->last_press_y - ev.y) <= 4)
                {
                    ev.wheel = 2;
                }
                p->last_press_time = xe.xbutton.time;
                p->last_press_x = ev.x;
                p->last_press_y = ev.y;
            }
            ui_push(p, &ev);
            break;

        case KeyPress:
        case KeyRelease:
        {
            KeySym ks = NoSymbol;
            char   buf[32];
            int    n;

            p->mods = ui_translate_state(xe.xkey.state);
            n = XLookupString(&xe.xkey, buf, (int)sizeof(buf) - 1, &ks, NULL);

            ev.type = (xe.type == KeyPress) ? UI_EV_KEY_DOWN : UI_EV_KEY_UP;
            ev.key  = ui_translate_keysym(ks);
            ev.mods = p->mods;
            ev.x = p->mouse_x; ev.y = p->mouse_y;
            if (ev.key != UI_KEY_UNKNOWN) { ui_push(p, &ev); }

            if (xe.type == KeyPress && n > 0)
            {
                int i;
                for (i = 0; i < n; ++i)
                {
                    const unsigned char c = (unsigned char)buf[i];
                    if (c >= 32u && c != 127u)
                    {
                        UI_Event te;
                        memset(&te, 0, sizeof(te));
                        te.type      = UI_EV_TEXT;
                        te.codepoint = (uint32_t)c;
                        te.mods      = p->mods;
                        ui_push(p, &te);
                    }
                }
            }
            break;
        }

        default:
            break;
        }
    }
}

int ui_plat_poll(UI_Platform* p, UI_Event* out)
{
    if (p == NULL || out == NULL) { return 0; }
    if (p->q_head == p->q_tail) { ui_pump(p); }
    if (p->q_head == p->q_tail) { return 0; }
    *out = p->queue[p->q_head];
    p->q_head = (p->q_head + 1u) % UI_EVENT_QUEUE;
    return 1;
}

void ui_plat_wait(UI_Platform* p, double timeout_s)
{
    int fd;
    fd_set fds;
    struct timeval tv;

    if (p == NULL) { return; }
    if (p->q_head != p->q_tail || XPending(p->dpy) > 0) { return; }

    fd = ConnectionNumber(p->dpy);
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec  = (time_t)((timeout_s > 0.0) ? timeout_s : 0.0);
    tv.tv_usec = (suseconds_t)(((timeout_s > 0.0) ? timeout_s : 0.0) -
                               (double)tv.tv_sec) * 1000000;
    (void)select(fd + 1, &fds, NULL, NULL, &tv);
}

uint32_t* ui_plat_pixels(UI_Platform* p) { return (p != NULL) ? p->pixels : NULL; }
int32_t   ui_plat_width(const UI_Platform* p)  { return (p != NULL) ? p->width  : 0; }
int32_t   ui_plat_height(const UI_Platform* p) { return (p != NULL) ? p->height : 0; }
int32_t   ui_plat_stride(const UI_Platform* p) { return (p != NULL) ? p->width  : 0; }

void ui_plat_present(UI_Platform* p)
{
    if (p == NULL || p->image == NULL) { return; }
    (void)XPutImage(p->dpy, p->win, p->gc, p->image, 0, 0, 0, 0,
                    (unsigned)p->width, (unsigned)p->height);
    (void)XFlush(p->dpy);
}

void ui_plat_set_cursor(UI_Platform* p, UI_Cursor c)
{
    if (p == NULL || (int)c < 0 || (int)c >= UI_CURSOR_COUNT) { return; }
    if (p->cursor != c)
    {
        p->cursor = c;
        XDefineCursor(p->dpy, p->win, p->cursors[c]);
    }
}

void ui_plat_set_title(UI_Platform* p, const char* title)
{
    if (p != NULL && title != NULL) { XStoreName(p->dpy, p->win, title); }
}

double ui_plat_now_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0.0; }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void ui_plat_sleep_s(double s)
{
    struct timespec req, rem;
    if (s <= 0.0) { return; }
    req.tv_sec  = (time_t)s;
    req.tv_nsec = (long)((s - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec > 999999999L) { req.tv_nsec = 999999999L; }
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) { req = rem; }
}

#else  /* _WIN32 */

/* Empty on Windows; ISO C forbids an empty translation unit. */
typedef int ui_platform_x11_unused_on_this_target;

#endif /* !_WIN32 */
