/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_platform_win32.c
 *  Purpose : Win32 implementation of the platform layer.  user32 + gdi32 only.
 *
 *  The framebuffer is a DIB section selected into a memory DC, which makes
 *  presentation a single BitBlt with no format conversion and no intermediate
 *  copy.  WM_PAINT simply re-blits the last frame, so resizing and occlusion
 *  never show garbage.
 *
 *  Language: ISO C17
 * ========================================================================== */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#  define NOMINMAX 1
#endif
#ifndef UNICODE
/* The console is built as an ANSI application: it keeps the Win32 code free of
 * wide-string conversion for a UI whose text is all ASCII plus a handful of
 * symbols drawn by our own renderer. */
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_platform.h"

#define UI_EVENT_QUEUE   256u
#define UI_WNDCLASS_NAME "PWRadarUIWindow"

struct UI_Platform
{
    HWND        hwnd;
    HDC         mem_dc;
    HBITMAP     dib;
    HBITMAP     old_bitmap;
    uint32_t*   pixels;
    int32_t     width, height;
    HCURSOR     cursors[UI_CURSOR_COUNT];
    UI_Cursor   cursor;

    UI_Event    queue[UI_EVENT_QUEUE];
    uint32_t    q_head, q_tail;

    int32_t     mouse_x, mouse_y;
    uint32_t    mods;
    int         tracking;
    int         quit;
};

/* --------------------------------------------------------------------------
 *  Event queue
 * ------------------------------------------------------------------------ */
static void ui_push(UI_Platform* p, const UI_Event* ev)
{
    const uint32_t next = (p->q_tail + 1u) % UI_EVENT_QUEUE;
    if (next == p->q_head) { return; }          /* full: drop the newest */
    p->queue[p->q_tail] = *ev;
    p->q_tail = next;
}

static void ui_push_simple(UI_Platform* p, int32_t type)
{
    UI_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.x    = p->mouse_x;
    ev.y    = p->mouse_y;
    ev.mods = p->mods;
    ui_push(p, &ev);
}

/* --------------------------------------------------------------------------
 *  Framebuffer
 * ------------------------------------------------------------------------ */
static void ui_release_dib(UI_Platform* p)
{
    if (p->mem_dc != NULL && p->old_bitmap != NULL)
    {
        (void)SelectObject(p->mem_dc, p->old_bitmap);
        p->old_bitmap = NULL;
    }
    if (p->dib != NULL) { DeleteObject(p->dib); p->dib = NULL; }
    p->pixels = NULL;
}

static int ui_resize_dib(UI_Platform* p, int32_t w, int32_t h)
{
    BITMAPINFO bmi;
    void* bits = NULL;

    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }
    if (p->pixels != NULL && w == p->width && h == p->height) { return 1; }

    ui_release_dib(p);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)w;
    bmi.bmiHeader.biHeight      = -(LONG)h;      /* top-down rows */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    p->dib = CreateDIBSection(p->mem_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0u);
    if (p->dib == NULL || bits == NULL) { return 0; }
    p->old_bitmap = (HBITMAP)SelectObject(p->mem_dc, p->dib);
    p->pixels = (uint32_t*)bits;
    p->width  = w;
    p->height = h;
    memset(p->pixels, 0, (size_t)w * (size_t)h * sizeof(uint32_t));
    return 1;
}

/* --------------------------------------------------------------------------
 *  Key translation
 * ------------------------------------------------------------------------ */
static int32_t ui_translate_key(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') { return (int32_t)vk; }
    if (vk >= '0' && vk <= '9') { return (int32_t)vk; }
    if (vk >= VK_F1 && vk <= VK_F12)
    {
        return UI_KEY_F1 + (int32_t)(vk - VK_F1);
    }
    switch (vk)
    {
    case VK_LEFT:   return UI_KEY_LEFT;
    case VK_RIGHT:  return UI_KEY_RIGHT;
    case VK_UP:     return UI_KEY_UP;
    case VK_DOWN:   return UI_KEY_DOWN;
    case VK_HOME:   return UI_KEY_HOME;
    case VK_END:    return UI_KEY_END;
    case VK_PRIOR:  return UI_KEY_PAGE_UP;
    case VK_NEXT:   return UI_KEY_PAGE_DOWN;
    case VK_INSERT: return UI_KEY_INSERT;
    case VK_DELETE: return UI_KEY_DELETE;
    case VK_BACK:   return UI_KEY_BACKSPACE;
    case VK_TAB:    return UI_KEY_TAB;
    case VK_RETURN: return UI_KEY_ENTER;
    case VK_ESCAPE: return UI_KEY_ESCAPE;
    case VK_SPACE:  return UI_KEY_SPACE;
    case VK_OEM_PLUS:   return '+';
    case VK_OEM_MINUS:  return '-';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_COMMA:  return ',';
    default: break;
    }
    return UI_KEY_UNKNOWN;
}

static uint32_t ui_current_mods(void)
{
    uint32_t m = 0u;
    if ((GetKeyState(VK_SHIFT)   & 0x8000) != 0) { m |= UI_MOD_SHIFT; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) { m |= UI_MOD_CTRL;  }
    if ((GetKeyState(VK_MENU)    & 0x8000) != 0) { m |= UI_MOD_ALT;   }
    return m;
}

/* --------------------------------------------------------------------------
 *  Window procedure
 * ------------------------------------------------------------------------ */
static LRESULT CALLBACK ui_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    UI_Platform* p = (UI_Platform*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    UI_Event ev;

    if (p == NULL)
    {
        if (msg == WM_NCCREATE)
        {
            const CREATESTRUCT* cs = (const CREATESTRUCT*)lp;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    p->mods = ui_current_mods();

    switch (msg)
    {
    case WM_CLOSE:
        p->quit = 1;
        ui_push_simple(p, UI_EV_QUIT);
        return 0;

    case WM_DESTROY:
        p->quit = 1;
        return 0;

    case WM_ERASEBKGND:
        return 1;                                /* the blit covers it all */

    case WM_SIZE:
    {
        const int32_t w = (int32_t)LOWORD(lp);
        const int32_t h = (int32_t)HIWORD(lp);
        if (w > 0 && h > 0 && ui_resize_dib(p, w, h) != 0)
        {
            memset(&ev, 0, sizeof(ev));
            ev.type   = UI_EV_RESIZE;
            ev.width  = w;
            ev.height = h;
            ui_push(p, &ev);
        }
        return 0;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (p->pixels != NULL)
        {
            (void)BitBlt(dc, 0, 0, p->width, p->height, p->mem_dc, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            SetCursor(p->cursors[p->cursor]);
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE:
        p->mouse_x = (int32_t)(short)LOWORD(lp);
        p->mouse_y = (int32_t)(short)HIWORD(lp);
        if (p->tracking == 0)
        {
            TRACKMOUSEEVENT tme;
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            (void)TrackMouseEvent(&tme);
            p->tracking = 1;
        }
        ui_push_simple(p, UI_EV_MOUSE_MOVE);
        return 0;

    case WM_MOUSELEAVE:
        p->tracking = 0;
        ui_push_simple(p, UI_EV_MOUSE_LEAVE);
        return 0;

    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
    case WM_LBUTTONUP:   case WM_MBUTTONUP:   case WM_RBUTTONUP:
    case WM_LBUTTONDBLCLK:
    {
        const int down = (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN ||
                          msg == WM_RBUTTONDOWN || msg == WM_LBUTTONDBLCLK);
        p->mouse_x = (int32_t)(short)LOWORD(lp);
        p->mouse_y = (int32_t)(short)HIWORD(lp);
        memset(&ev, 0, sizeof(ev));
        ev.type = down ? UI_EV_MOUSE_DOWN : UI_EV_MOUSE_UP;
        ev.x = p->mouse_x;
        ev.y = p->mouse_y;
        ev.mods = p->mods;
        ev.button = (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? UI_MB_MIDDLE
                  : ((msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? UI_MB_RIGHT
                                                                   : UI_MB_LEFT);
        /* A double click is reported as a left press with the shift-like
         * marker in `wheel`, which the widget layer reads as "activate". */
        if (msg == WM_LBUTTONDBLCLK) { ev.wheel = 2; }
        if (down) { SetCapture(hwnd); } else { (void)ReleaseCapture(); }
        ui_push(p, &ev);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        POINT pt;
        pt.x = (LONG)(short)LOWORD(lp);
        pt.y = (LONG)(short)HIWORD(lp);
        (void)ScreenToClient(hwnd, &pt);
        memset(&ev, 0, sizeof(ev));
        ev.type  = UI_EV_WHEEL;
        ev.x     = (int32_t)pt.x;
        ev.y     = (int32_t)pt.y;
        ev.mods  = p->mods;
        ev.wheel = (int32_t)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        p->mouse_x = ev.x;
        p->mouse_y = ev.y;
        ui_push(p, &ev);
        return 0;
    }

    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP:   case WM_SYSKEYUP:
    {
        const int down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        memset(&ev, 0, sizeof(ev));
        ev.type = down ? UI_EV_KEY_DOWN : UI_EV_KEY_UP;
        ev.key  = ui_translate_key(wp);
        ev.mods = p->mods;
        ev.x = p->mouse_x;
        ev.y = p->mouse_y;
        if (ev.key != UI_KEY_UNKNOWN) { ui_push(p, &ev); }
        if (msg == WM_SYSKEYDOWN && wp == VK_F10) { return 0; }
        break;
    }

    case WM_CHAR:
        if (wp >= 32u && wp != 127u)
        {
            memset(&ev, 0, sizeof(ev));
            ev.type      = UI_EV_TEXT;
            ev.codepoint = (uint32_t)wp;
            ev.mods      = p->mods;
            ui_push(p, &ev);
        }
        return 0;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------------------
 *  Life cycle
 * ------------------------------------------------------------------------ */
UI_Platform* ui_plat_create(const char* title, int32_t w, int32_t h,
                            char* err, size_t err_cap)
{
    UI_Platform* p;
    WNDCLASSEX wc;
    HINSTANCE inst = GetModuleHandle(NULL);
    RECT rect;
    DWORD style = WS_OVERLAPPEDWINDOW;

    p = (UI_Platform*)calloc(1u, sizeof(*p));
    if (p == NULL)
    {
        if (err != NULL) { (void)snprintf(err, err_cap, "out of memory"); }
        return NULL;
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc   = ui_wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = NULL;         /* handled through WM_SETCURSOR */
    wc.hbrBackground = NULL;
    wc.lpszClassName = UI_WNDCLASS_NAME;
    if (RegisterClassEx(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        if (err != NULL) { (void)snprintf(err, err_cap, "RegisterClassEx failed (%lu)",
                                          (unsigned long)GetLastError()); }
        free(p);
        return NULL;
    }

    p->cursors[UI_CURSOR_ARROW]   = LoadCursor(NULL, IDC_ARROW);
    p->cursors[UI_CURSOR_HAND]    = LoadCursor(NULL, IDC_HAND);
    p->cursors[UI_CURSOR_CROSS]   = LoadCursor(NULL, IDC_CROSS);
    p->cursors[UI_CURSOR_SIZE_WE] = LoadCursor(NULL, IDC_SIZEWE);
    p->cursors[UI_CURSOR_SIZE_NS] = LoadCursor(NULL, IDC_SIZENS);
    p->cursors[UI_CURSOR_TEXT]    = LoadCursor(NULL, IDC_IBEAM);
    p->cursor = UI_CURSOR_ARROW;

    rect.left = 0; rect.top = 0; rect.right = w; rect.bottom = h;
    (void)AdjustWindowRect(&rect, style, FALSE);

    p->hwnd = CreateWindowEx(0, UI_WNDCLASS_NAME,
                             (title != NULL) ? title : "PWRadarUI",
                             style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top,
                             NULL, NULL, inst, p);
    if (p->hwnd == NULL)
    {
        if (err != NULL) { (void)snprintf(err, err_cap, "CreateWindowEx failed (%lu)",
                                          (unsigned long)GetLastError()); }
        free(p);
        return NULL;
    }

    p->mem_dc = CreateCompatibleDC(NULL);
    if (p->mem_dc == NULL || ui_resize_dib(p, w, h) == 0)
    {
        if (err != NULL) { (void)snprintf(err, err_cap, "DIB creation failed"); }
        ui_plat_destroy(p);
        return NULL;
    }

    (void)ShowWindow(p->hwnd, SW_SHOW);
    (void)UpdateWindow(p->hwnd);
    return p;
}

void ui_plat_destroy(UI_Platform* p)
{
    if (p == NULL) { return; }
    ui_release_dib(p);
    if (p->mem_dc != NULL) { DeleteDC(p->mem_dc); }
    if (p->hwnd   != NULL) { DestroyWindow(p->hwnd); }
    free(p);
}

int ui_plat_poll(UI_Platform* p, UI_Event* out)
{
    MSG msg;
    if (p == NULL || out == NULL) { return 0; }

    while (p->q_head == p->q_tail)
    {
        if (PeekMessage(&msg, NULL, 0u, 0u, PM_REMOVE) == 0) { return 0; }
        (void)TranslateMessage(&msg);
        (void)DispatchMessage(&msg);
    }
    *out = p->queue[p->q_head];
    p->q_head = (p->q_head + 1u) % UI_EVENT_QUEUE;
    return 1;
}

void ui_plat_wait(UI_Platform* p, double timeout_s)
{
    DWORD ms;
    if (p == NULL) { return; }
    if (p->q_head != p->q_tail) { return; }
    ms = (timeout_s <= 0.0) ? 0u : (DWORD)(timeout_s * 1000.0 + 0.5);
    (void)MsgWaitForMultipleObjects(0u, NULL, FALSE, ms, QS_ALLINPUT);
}

uint32_t* ui_plat_pixels(UI_Platform* p) { return (p != NULL) ? p->pixels : NULL; }
int32_t   ui_plat_width(const UI_Platform* p)  { return (p != NULL) ? p->width  : 0; }
int32_t   ui_plat_height(const UI_Platform* p) { return (p != NULL) ? p->height : 0; }
int32_t   ui_plat_stride(const UI_Platform* p) { return (p != NULL) ? p->width  : 0; }

void ui_plat_present(UI_Platform* p)
{
    HDC dc;
    if (p == NULL || p->pixels == NULL) { return; }
    dc = GetDC(p->hwnd);
    if (dc != NULL)
    {
        (void)BitBlt(dc, 0, 0, p->width, p->height, p->mem_dc, 0, 0, SRCCOPY);
        (void)ReleaseDC(p->hwnd, dc);
    }
}

void ui_plat_set_cursor(UI_Platform* p, UI_Cursor c)
{
    if (p == NULL || (int)c < 0 || (int)c >= UI_CURSOR_COUNT) { return; }
    if (p->cursor != c)
    {
        p->cursor = c;
        SetCursor(p->cursors[c]);
    }
}

void ui_plat_set_title(UI_Platform* p, const char* title)
{
    if (p != NULL && title != NULL) { (void)SetWindowText(p->hwnd, title); }
}

double ui_plat_now_s(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
    QueryPerformanceCounter(&now);
    return (freq.QuadPart != 0)
        ? ((double)now.QuadPart / (double)freq.QuadPart) : 0.0;
}

void ui_plat_sleep_s(double s)
{
    /* Sleep() only resolves to the scheduler quantum, which would make frame
     * pacing lumpy; spin out the last millisecond so the presented cadence is
     * smooth without depending on winmm's timeBeginPeriod. */
    double deadline;
    if (s <= 0.0) { (void)SwitchToThread(); return; }
    deadline = ui_plat_now_s() + s;
    for (;;)
    {
        const double left = deadline - ui_plat_now_s();
        if (left <= 0.0) { break; }
        if (left > 0.002) { Sleep((DWORD)((left - 0.001) * 1000.0)); }
        else              { YieldProcessor(); }
    }
}

#else  /* !_WIN32 */

/* This translation unit is empty on non-Windows targets, and ISO C forbids an
 * empty translation unit, so declare one unused type. */
typedef int ui_platform_win32_unused_on_this_target;

#endif /* _WIN32 */
