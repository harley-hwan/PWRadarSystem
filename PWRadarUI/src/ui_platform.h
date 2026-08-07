/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_platform.h
 *  Purpose : The whole operating-system surface of the console, and nothing
 *            more: one window, an input queue, and a 32-bit framebuffer that
 *            gets blitted.  Every pixel above this layer is produced by the
 *            software renderer in ui_gfx.c, so Windows and Linux render
 *            identically down to the pixel.
 *
 *            Windows implementation: ui_platform_win32.c  (user32 + gdi32)
 *            Linux   implementation: ui_platform_x11.c    (libX11)
 *
 *  Pixel format
 *  ------------
 *  One uint32_t per pixel, 0xAARRGGBB in host byte order.  That is byte order
 *  B,G,R,A in memory on little-endian, which is exactly what a Win32 BI_RGB
 *  DIB and an X11 24/32-bit TrueColor visual both expect, so no conversion
 *  happens on either platform.
 *
 *  Language: ISO C17
 * ========================================================================== */
#ifndef PWRADAR_UI_PLATFORM_H
#define PWRADAR_UI_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef struct UI_Platform UI_Platform;

/* --------------------------------------------------------------------------
 *  Input
 * ------------------------------------------------------------------------ */
typedef enum UI_EventType
{
    UI_EV_NONE = 0,
    UI_EV_QUIT,
    UI_EV_RESIZE,
    UI_EV_MOUSE_DOWN,
    UI_EV_MOUSE_UP,
    UI_EV_MOUSE_MOVE,
    UI_EV_MOUSE_LEAVE,
    UI_EV_WHEEL,
    UI_EV_KEY_DOWN,
    UI_EV_KEY_UP,
    UI_EV_TEXT
} UI_EventType;

enum UI_MouseButton
{
    UI_MB_LEFT   = 0,
    UI_MB_MIDDLE = 1,
    UI_MB_RIGHT  = 2
};

enum UI_Modifier
{
    UI_MOD_SHIFT = 1u << 0,
    UI_MOD_CTRL  = 1u << 1,
    UI_MOD_ALT   = 1u << 2
};

/* Key codes above the ASCII range so printable keys keep their ASCII value. */
enum UI_Key
{
    UI_KEY_UNKNOWN   = 0,
    UI_KEY_BACKSPACE = 8,
    UI_KEY_TAB       = 9,
    UI_KEY_ENTER     = 13,
    UI_KEY_ESCAPE    = 27,
    UI_KEY_SPACE     = 32,
    UI_KEY_DELETE    = 127,
    UI_KEY_LEFT      = 256,
    UI_KEY_RIGHT,
    UI_KEY_UP,
    UI_KEY_DOWN,
    UI_KEY_HOME,
    UI_KEY_END,
    UI_KEY_PAGE_UP,
    UI_KEY_PAGE_DOWN,
    UI_KEY_INSERT,
    UI_KEY_F1,  UI_KEY_F2,  UI_KEY_F3,  UI_KEY_F4,
    UI_KEY_F5,  UI_KEY_F6,  UI_KEY_F7,  UI_KEY_F8,
    UI_KEY_F9,  UI_KEY_F10, UI_KEY_F11, UI_KEY_F12
};

typedef struct UI_Event
{
    int32_t  type;          /* UI_EventType                                   */
    int32_t  x, y;          /* cursor position in client pixels               */
    int32_t  button;        /* UI_MouseButton for the button events           */
    int32_t  wheel;         /* signed detents, +1 == scroll away from user    */
    int32_t  key;           /* UI_Key / ASCII for key events                  */
    uint32_t mods;          /* UI_Modifier bitmask                            */
    int32_t  width, height; /* new client size for UI_EV_RESIZE               */
    uint32_t codepoint;     /* Unicode scalar for UI_EV_TEXT                  */
} UI_Event;

typedef enum UI_Cursor
{
    UI_CURSOR_ARROW = 0,
    UI_CURSOR_HAND,
    UI_CURSOR_CROSS,
    UI_CURSOR_SIZE_WE,
    UI_CURSOR_SIZE_NS,
    UI_CURSOR_TEXT,
    UI_CURSOR_COUNT
} UI_Cursor;

/* --------------------------------------------------------------------------
 *  Life cycle
 * ------------------------------------------------------------------------ */

/** Creates the window and its backing framebuffer.  Returns NULL on failure,
 *  writing a reason into @p err when it is non-NULL. */
UI_Platform* ui_plat_create(const char* title, int32_t w, int32_t h,
                            char* err, size_t err_cap);
void         ui_plat_destroy(UI_Platform* p);

/** Drains one event.  Returns 0 when the queue is empty. */
int          ui_plat_poll(UI_Platform* p, UI_Event* out);

/** Blocks until an event arrives or @p timeout_s elapses.  Used to idle
 *  cheaply while the radar is paused. */
void         ui_plat_wait(UI_Platform* p, double timeout_s);

/** The framebuffer to draw into.  Valid until the next UI_EV_RESIZE. */
uint32_t*    ui_plat_pixels(UI_Platform* p);
int32_t      ui_plat_width(const UI_Platform* p);
int32_t      ui_plat_height(const UI_Platform* p);
int32_t      ui_plat_stride(const UI_Platform* p);   /* in pixels             */

/** Copies the framebuffer to the screen. */
void         ui_plat_present(UI_Platform* p);

void         ui_plat_set_cursor(UI_Platform* p, UI_Cursor c);
void         ui_plat_set_title(UI_Platform* p, const char* title);

/** Monotonic seconds and a yielding sleep.  Separate from the core library's
 *  clock so the UI has no link-time dependency on it for timing alone. */
double       ui_plat_now_s(void);
void         ui_plat_sleep_s(double s);

#endif /* PWRADAR_UI_PLATFORM_H */
