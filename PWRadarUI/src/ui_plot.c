#include "ui_plot.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define UI_PLOT_MAX_PTS 8192

/* ==========================================================================
 *  Tick engine
 * ==========================================================================
 *  The classic 1-2-5 selection: pick the power of ten below the raw step, then
 *  round the mantissa up to 1, 2, 2.5, 5 or 10.  Including 2.5 matters for dB
 *  axes, where 2.5 dB divisions read far better than 2 or 5.
 * ------------------------------------------------------------------------ */
static double ui_nice_step(double raw)
{
    double mag, norm;
    if (!(raw > 0.0)) { return 1.0; }
    mag  = pow(10.0, floor(log10(raw)));
    norm = raw / mag;
    if (norm <= 1.0)      { return 1.0  * mag; }
    else if (norm <= 2.0) { return 2.0  * mag; }
    else if (norm <= 2.5) { return 2.5  * mag; }
    else if (norm <= 5.0) { return 5.0  * mag; }
    return 10.0 * mag;
}

void ui_format_tick(char* out, size_t cap, double v, double step)
{
    int digits;
    if (fabs(v) < step * 1e-9) { v = 0.0; }

    /* Choose a fixed-point precision from the step, and fall back to
     * engineering notation when the magnitudes get out of hand. */
    if (step > 0.0 && (fabs(v) >= 1e6 || (fabs(v) > 0.0 && fabs(v) < 1e-3)))
    {
        (void)snprintf(out, cap, "%.3g", v);
        return;
    }
    digits = (step > 0.0) ? (int)ceil(-log10(step) + 0.2) : 0;
    if (digits < 0) { digits = 0; }
    if (digits > 6) { digits = 6; }
    (void)snprintf(out, cap, "%.*f", digits, v);
}

void ui_ticks_linear(UI_Ticks* t, double lo, double hi, int32_t want)
{
    double step, first;
    int32_t i;

    memset(t, 0, sizeof(*t));
    if (want < 2) { want = 2; }
    if (!(hi > lo))
    {
        t->count = 1;
        t->pos[0] = lo;
        ui_format_tick(t->text[0], sizeof(t->text[0]), lo, 1.0);
        t->minor_per = 1;
        return;
    }

    step  = ui_nice_step((hi - lo) / (double)want);
    first = ceil(lo / step - 1e-9) * step;

    /* 2.5 and 5 read best split into five minors, everything else into four. */
    {
        const double m = step / pow(10.0, floor(log10(step)));
        t->minor_per = (m > 2.2 && m < 2.8) ? 5 : ((m > 4.5) ? 5 : 4);
    }

    for (i = 0; i < UI_MAX_TICKS; ++i)
    {
        const double v = first + (double)i * step;
        if (v > hi + step * 1e-6) { break; }
        t->pos[t->count] = v;
        ui_format_tick(t->text[t->count], sizeof(t->text[0]), v, step);
        ++t->count;
    }
}

void ui_ticks_log(UI_Ticks* t, double lo, double hi)
{
    int32_t e0, e1, e;
    memset(t, 0, sizeof(*t));
    if (!(lo > 0.0)) { lo = 1e-12; }
    if (!(hi > lo))  { hi = lo * 10.0; }
    e0 = (int32_t)floor(log10(lo));
    e1 = (int32_t)ceil(log10(hi));
    for (e = e0; e <= e1 && t->count < UI_MAX_TICKS; ++e)
    {
        const double v = pow(10.0, (double)e);
        if (v < lo * 0.999 || v > hi * 1.001) { continue; }
        t->pos[t->count] = v;
        (void)snprintf(t->text[t->count], sizeof(t->text[0]), "1e%d", e);
        ++t->count;
    }
    t->minor_per = 9;
}

/* ==========================================================================
 *  Axes set-up
 * ========================================================================== */
void ui_axes_init(UI_Axes* a, const char* title,
                  const char* xlabel, const char* ylabel)
{
    memset(a, 0, sizeof(*a));
    a->grid         = 1;
    a->box_on       = 1;
    a->allow_zoom   = 1;
    a->allow_pan    = 1;
    a->allow_cursor = 1;
    a->xmin = 0.0; a->xmax = 1.0; a->ymin = 0.0; a->ymax = 1.0;
    a->xfull_min = 0.0; a->xfull_max = 1.0;
    a->yfull_min = 0.0; a->yfull_max = 1.0;
    if (title  != NULL) { (void)snprintf(a->title,  sizeof(a->title),  "%s", title); }
    if (xlabel != NULL) { (void)snprintf(a->xlabel, sizeof(a->xlabel), "%s", xlabel); }
    if (ylabel != NULL) { (void)snprintf(a->ylabel, sizeof(a->ylabel), "%s", ylabel); }
}

void ui_axes_set_lim(UI_Axes* a, double x0, double x1, double y0, double y1)
{
    if (x1 > x0) { a->xmin = x0; a->xmax = x1; }
    if (y1 > y0) { a->ymin = y0; a->ymax = y1; }
}

void ui_axes_set_full(UI_Axes* a, double x0, double x1, double y0, double y1)
{
    a->xfull_min = x0; a->xfull_max = x1;
    a->yfull_min = y0; a->yfull_max = y1;
    ui_axes_set_lim(a, x0, x1, y0, y1);
}

void ui_axes_reset_view(UI_Axes* a)
{
    a->xmin = a->xfull_min; a->xmax = a->xfull_max;
    a->ymin = a->yfull_min; a->ymax = a->yfull_max;
}

void ui_axes_set_title(UI_Axes* a, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(a->title, sizeof(a->title), fmt, ap);
    va_end(ap);
}

/* ==========================================================================
 *  Layout
 * ========================================================================== */
void ui_axes_layout(UI_Axes* a, UI_Rect frame)
{
    const int32_t tick_h = ui_font_line_height(UI_FONT_SMALL);
    int32_t left, right, top, bottom, wmax, i;

    a->frame = frame;

    /* Recompute the ticks first: the y tick label width sets the left inset. */
    if (a->ylog != 0) { ui_ticks_log(&a->yt, a->ymin, a->ymax); }
    else { ui_ticks_linear(&a->yt, a->ymin, a->ymax,
                           (frame.h > 220) ? 8 : ((frame.h > 120) ? 5 : 3)); }
    if (a->xlog != 0) { ui_ticks_log(&a->xt, a->xmin, a->xmax); }
    else { ui_ticks_linear(&a->xt, a->xmin, a->xmax,
                           (frame.w > 520) ? 10 : ((frame.w > 260) ? 6 : 4)); }

    wmax = 0;
    for (i = 0; i < a->yt.count; ++i)
    {
        const int32_t w = ui_text_width(UI_FONT_SMALL, a->yt.text[i]);
        if (w > wmax) { wmax = w; }
    }

    left   = wmax + 10;
    right  = 8;
    top    = (a->title[0] != '\0') ? (ui_font_line_height(UI_FONT_BOLD) + 6) : 6;
    bottom = tick_h + 8;

    if (a->ylabel[0] != '\0') { left += ui_font_line_height(UI_FONT_SMALL) + 2; }
    if (a->xlabel[0] != '\0') { bottom += ui_font_line_height(UI_FONT_SMALL); }
    if (a->y_right != 0) { const int32_t t = left; left = right + 4; right = t; }

    a->box = ui_rect(frame.x + left, frame.y + top,
                     (frame.w - left - right > 8) ? (frame.w - left - right) : 8,
                     (frame.h - top - bottom > 8) ? (frame.h - top - bottom) : 8);

    if (a->equal_aspect != 0)
    {
        /* Match the data-per-pixel scale on both axes, expanding the looser
         * one, which is what `axis equal` does. */
        const double sx = (a->xmax - a->xmin) / (double)a->box.w;
        const double sy = (a->ymax - a->ymin) / (double)a->box.h;
        if (sx > sy)
        {
            const double cy = 0.5 * (a->ymin + a->ymax);
            const double half = 0.5 * sx * (double)a->box.h;
            a->ymin = cy - half; a->ymax = cy + half;
        }
        else if (sy > sx)
        {
            const double cx = 0.5 * (a->xmin + a->xmax);
            const double half = 0.5 * sy * (double)a->box.w;
            a->xmin = cx - half; a->xmax = cx + half;
        }
    }
}

/* ==========================================================================
 *  Transforms
 * ========================================================================== */
double ui_axes_x2px(const UI_Axes* a, double x)
{
    if (a->xlog != 0)
    {
        const double l0 = log10((a->xmin > 0.0) ? a->xmin : 1e-12);
        const double l1 = log10((a->xmax > 0.0) ? a->xmax : 1.0);
        const double lv = log10((x > 0.0) ? x : 1e-12);
        return (l1 > l0) ? ((double)a->box.x + (lv - l0) / (l1 - l0) *
                            (double)a->box.w) : (double)a->box.x;
    }
    return (a->xmax > a->xmin)
        ? ((double)a->box.x + (x - a->xmin) / (a->xmax - a->xmin) * (double)a->box.w)
        : (double)a->box.x;
}

double ui_axes_y2px(const UI_Axes* a, double y)
{
    if (a->ylog != 0)
    {
        const double l0 = log10((a->ymin > 0.0) ? a->ymin : 1e-12);
        const double l1 = log10((a->ymax > 0.0) ? a->ymax : 1.0);
        const double lv = log10((y > 0.0) ? y : 1e-12);
        return (l1 > l0) ? ((double)(a->box.y + a->box.h) - (lv - l0) / (l1 - l0) *
                            (double)a->box.h) : (double)(a->box.y + a->box.h);
    }
    return (a->ymax > a->ymin)
        ? ((double)(a->box.y + a->box.h) -
           (y - a->ymin) / (a->ymax - a->ymin) * (double)a->box.h)
        : (double)(a->box.y + a->box.h);
}

double ui_axes_px2x(const UI_Axes* a, double px)
{
    const double t = (a->box.w > 0)
        ? ((px - (double)a->box.x) / (double)a->box.w) : 0.0;
    if (a->xlog != 0)
    {
        const double l0 = log10((a->xmin > 0.0) ? a->xmin : 1e-12);
        const double l1 = log10((a->xmax > 0.0) ? a->xmax : 1.0);
        return pow(10.0, l0 + t * (l1 - l0));
    }
    return a->xmin + t * (a->xmax - a->xmin);
}

double ui_axes_px2y(const UI_Axes* a, double py)
{
    const double t = (a->box.h > 0)
        ? (((double)(a->box.y + a->box.h) - py) / (double)a->box.h) : 0.0;
    if (a->ylog != 0)
    {
        const double l0 = log10((a->ymin > 0.0) ? a->ymin : 1e-12);
        const double l1 = log10((a->ymax > 0.0) ? a->ymax : 1.0);
        return pow(10.0, l0 + t * (l1 - l0));
    }
    return a->ymin + t * (a->ymax - a->ymin);
}

/* ==========================================================================
 *  Interaction
 * ==========================================================================
 *  Wheel          zoom about the pointer, both axes; +Shift x only, +Ctrl y only
 *  Left drag      rubber-band box zoom
 *  Middle drag    pan
 *  Right drag     pan (so a two-button mouse can still pan)
 *  Double click   zoom to fit
 *  Hover          data cursor readout
 * ------------------------------------------------------------------------ */
static void ui_axes_zoom_about(UI_Axes* a, double px, double py, double k,
                               int do_x, int do_y)
{
    if (do_x != 0)
    {
        const double x = ui_axes_px2x(a, px);
        if (a->xlog != 0)
        {
            const double l = log10((x > 0.0) ? x : 1e-12);
            const double l0 = log10((a->xmin > 0.0) ? a->xmin : 1e-12);
            const double l1 = log10((a->xmax > 0.0) ? a->xmax : 1.0);
            a->xmin = pow(10.0, l + (l0 - l) * k);
            a->xmax = pow(10.0, l + (l1 - l) * k);
        }
        else
        {
            a->xmin = x + (a->xmin - x) * k;
            a->xmax = x + (a->xmax - x) * k;
        }
    }
    if (do_y != 0)
    {
        const double y = ui_axes_px2y(a, py);
        if (a->ylog != 0)
        {
            const double l = log10((y > 0.0) ? y : 1e-12);
            const double l0 = log10((a->ymin > 0.0) ? a->ymin : 1e-12);
            const double l1 = log10((a->ymax > 0.0) ? a->ymax : 1.0);
            a->ymin = pow(10.0, l + (l0 - l) * k);
            a->ymax = pow(10.0, l + (l1 - l) * k);
        }
        else
        {
            a->ymin = y + (a->ymin - y) * k;
            a->ymax = y + (a->ymax - y) * k;
        }
    }
}

int ui_axes_input(UI_Context* c, UI_Axes* a, uint32_t id)
{
    const int inside = (ui_popup_open(c) == 0) && (c->mouse_inside != 0) &&
                       ui_rect_contains(a->box, c->mouse_x, c->mouse_y);
    int changed = 0;

    if (inside != 0)
    {
        c->hot_next = id;
        if (a->allow_cursor != 0) { c->cursor = UI_CURSOR_CROSS; }
    }

    /* ---- data cursor ---------------------------------------------------- */
    if (a->allow_cursor != 0)
    {
        if (inside != 0 && a->cursor_locked == 0)
        {
            a->cursor_on = 1;
            a->cursor_x  = ui_axes_px2x(a, (double)c->mouse_x + 0.5);
            a->cursor_y  = ui_axes_px2y(a, (double)c->mouse_y + 0.5);
        }
        else if (a->cursor_locked == 0)
        {
            a->cursor_on = 0;
        }
        if (inside != 0 && c->pressed[UI_MB_RIGHT] != 0 && c->mods == 0u)
        {
            a->cursor_locked = (a->cursor_locked != 0) ? 0 : 1;
        }
    }

    /* ---- wheel zoom ----------------------------------------------------- */
    if (inside != 0 && c->wheel != 0 && a->allow_zoom != 0)
    {
        const double k = pow(0.85, (double)c->wheel);
        const int do_x = ((c->mods & UI_MOD_CTRL) == 0u) ? 1 : 0;
        const int do_y = ((c->mods & UI_MOD_SHIFT) == 0u) ? 1 : 0;
        ui_axes_zoom_about(a, (double)c->mouse_x + 0.5, (double)c->mouse_y + 0.5,
                           k, do_x, do_y);
        c->wheel = 0;
        changed = 1;
    }

    /* ---- zoom to fit ---------------------------------------------------- */
    if (inside != 0 && c->double_click != 0)
    {
        ui_axes_reset_view(a);
        a->zooming = 0;
        changed = 1;
    }

    /* ---- pan ------------------------------------------------------------ */
    if (a->allow_pan != 0)
    {
        if (a->panning == 0 && inside != 0 &&
            (c->pressed[UI_MB_MIDDLE] != 0 ||
             (c->pressed[UI_MB_LEFT] != 0 && (c->mods & UI_MOD_SHIFT) != 0u)))
        {
            a->panning  = 1;
            a->pan_x0   = (double)c->mouse_x;
            a->pan_y0   = (double)c->mouse_y;
            a->pan_xmin = a->xmin; a->pan_xmax = a->xmax;
            a->pan_ymin = a->ymin; a->pan_ymax = a->ymax;
        }
        if (a->panning != 0)
        {
            c->cursor = UI_CURSOR_HAND;
            if (c->down[UI_MB_MIDDLE] == 0 && c->down[UI_MB_LEFT] == 0)
            {
                a->panning = 0;
            }
            else
            {
                const double dx = (double)c->mouse_x - a->pan_x0;
                const double dy = (double)c->mouse_y - a->pan_y0;
                const double sx = (a->pan_xmax - a->pan_xmin) / (double)a->box.w;
                const double sy = (a->pan_ymax - a->pan_ymin) / (double)a->box.h;
                if (a->xlog == 0)
                {
                    a->xmin = a->pan_xmin - dx * sx;
                    a->xmax = a->pan_xmax - dx * sx;
                }
                if (a->ylog == 0)
                {
                    a->ymin = a->pan_ymin + dy * sy;
                    a->ymax = a->pan_ymax + dy * sy;
                }
                changed = 1;
            }
        }
    }

    /* ---- rubber-band box zoom ------------------------------------------- */
    if (a->allow_zoom != 0 && a->panning == 0)
    {
        if (a->zooming == 0 && inside != 0 && c->pressed[UI_MB_LEFT] != 0 &&
            (c->mods & UI_MOD_SHIFT) == 0u && c->double_click == 0)
        {
            a->zooming = 1;
            a->zx0 = c->mouse_x; a->zy0 = c->mouse_y;
            a->zx1 = c->mouse_x; a->zy1 = c->mouse_y;
        }
        else if (a->zooming != 0)
        {
            a->zx1 = c->mouse_x;
            a->zy1 = c->mouse_y;
            if (c->down[UI_MB_LEFT] == 0)
            {
                const int32_t dx = (a->zx1 > a->zx0) ? (a->zx1 - a->zx0)
                                                     : (a->zx0 - a->zx1);
                const int32_t dy = (a->zy1 > a->zy0) ? (a->zy1 - a->zy0)
                                                     : (a->zy0 - a->zy1);
                if (dx > 6 && dy > 6)
                {
                    const double x0 = ui_axes_px2x(a, (double)((a->zx0 < a->zx1) ? a->zx0 : a->zx1));
                    const double x1 = ui_axes_px2x(a, (double)((a->zx0 < a->zx1) ? a->zx1 : a->zx0));
                    const double y0 = ui_axes_px2y(a, (double)((a->zy0 < a->zy1) ? a->zy1 : a->zy0));
                    const double y1 = ui_axes_px2y(a, (double)((a->zy0 < a->zy1) ? a->zy0 : a->zy1));
                    ui_axes_set_lim(a, x0, x1, y0, y1);
                    changed = 1;
                }
                a->zooming = 0;
            }
        }
    }
    return changed;
}

/* ==========================================================================
 *  Drawing
 * ========================================================================== */
void ui_axes_draw_bg(UI_Canvas* cv, UI_Axes* a)
{
    int32_t i, k;

    ui_fill_rect(cv, a->box, UI_C_PLOT_BG);
    if (a->grid == 0) { return; }

    /* Minor grid first so majors sit on top. */
    if (a->grid >= 2)
    {
        for (i = 0; i + 1 < a->xt.count; ++i)
        {
            for (k = 1; k < a->xt.minor_per; ++k)
            {
                const double v = a->xt.pos[i] +
                    (a->xt.pos[i + 1] - a->xt.pos[i]) * (double)k /
                    (double)a->xt.minor_per;
                const int32_t px = (int32_t)(ui_axes_x2px(a, v) + 0.5);
                if (px > a->box.x && px < a->box.x + a->box.w)
                {
                    ui_vline(cv, px, a->box.y, a->box.y + a->box.h - 1,
                             UI_C_GRID_MINOR);
                }
            }
        }
        for (i = 0; i + 1 < a->yt.count; ++i)
        {
            for (k = 1; k < a->yt.minor_per; ++k)
            {
                const double v = a->yt.pos[i] +
                    (a->yt.pos[i + 1] - a->yt.pos[i]) * (double)k /
                    (double)a->yt.minor_per;
                const int32_t py = (int32_t)(ui_axes_y2px(a, v) + 0.5);
                if (py > a->box.y && py < a->box.y + a->box.h)
                {
                    ui_hline(cv, a->box.x, a->box.x + a->box.w - 1, py,
                             UI_C_GRID_MINOR);
                }
            }
        }
    }
    for (i = 0; i < a->xt.count; ++i)
    {
        const int32_t px = (int32_t)(ui_axes_x2px(a, a->xt.pos[i]) + 0.5);
        if (px >= a->box.x && px < a->box.x + a->box.w)
        {
            ui_vline(cv, px, a->box.y, a->box.y + a->box.h - 1, UI_C_GRID);
        }
    }
    for (i = 0; i < a->yt.count; ++i)
    {
        const int32_t py = (int32_t)(ui_axes_y2px(a, a->yt.pos[i]) + 0.5);
        if (py >= a->box.y && py < a->box.y + a->box.h)
        {
            ui_hline(cv, a->box.x, a->box.x + a->box.w - 1, py, UI_C_GRID);
        }
    }
}

void ui_axes_draw_frame(UI_Canvas* cv, UI_Axes* a)
{
    const int32_t x_axis_y = a->box.y + a->box.h;
    const int32_t y_axis_x = (a->y_right != 0) ? (a->box.x + a->box.w) : a->box.x;
    const int32_t small_h  = ui_font_line_height(UI_FONT_SMALL);
    int32_t i;

    if (a->box_on != 0) { ui_frame_rect(cv, a->box, UI_C_AXIS); }
    else
    {
        ui_hline(cv, a->box.x, a->box.x + a->box.w - 1, x_axis_y - 1, UI_C_AXIS);
        ui_vline(cv, y_axis_x, a->box.y, x_axis_y - 1, UI_C_AXIS);
    }

    /* ---- x ticks -------------------------------------------------------- */
    for (i = 0; i < a->xt.count; ++i)
    {
        const int32_t px = (int32_t)(ui_axes_x2px(a, a->xt.pos[i]) + 0.5);
        if (px < a->box.x - 1 || px > a->box.x + a->box.w) { continue; }
        ui_vline(cv, px, x_axis_y - 4, x_axis_y - 1, UI_C_AXIS);
        (void)ui_text_aligned(cv, UI_FONT_SMALL, px,
                              x_axis_y + ui_font_ascent(UI_FONT_SMALL) + 3,
                              UI_C_TEXT_DIM, UI_ALIGN_CENTRE, a->xt.text[i]);
    }
    /* ---- y ticks -------------------------------------------------------- */
    for (i = 0; i < a->yt.count; ++i)
    {
        const int32_t py = (int32_t)(ui_axes_y2px(a, a->yt.pos[i]) + 0.5);
        if (py < a->box.y - 1 || py > a->box.y + a->box.h) { continue; }
        if (a->y_right != 0)
        {
            ui_hline(cv, y_axis_x - 4, y_axis_x - 1, py, UI_C_AXIS);
            (void)ui_text_aligned(cv, UI_FONT_SMALL, y_axis_x + 5,
                                  py + ui_font_ascent(UI_FONT_SMALL) / 2 - 1,
                                  UI_C_TEXT_DIM, UI_ALIGN_LEFT, a->yt.text[i]);
        }
        else
        {
            ui_hline(cv, y_axis_x, y_axis_x + 3, py, UI_C_AXIS);
            (void)ui_text_aligned(cv, UI_FONT_SMALL, y_axis_x - 6,
                                  py + ui_font_ascent(UI_FONT_SMALL) / 2 - 1,
                                  UI_C_TEXT_DIM, UI_ALIGN_RIGHT, a->yt.text[i]);
        }
    }

    /* ---- labels and title ---------------------------------------------- */
    if (a->xlabel[0] != '\0')
    {
        (void)ui_text_aligned(cv, UI_FONT_SMALL, a->box.x + a->box.w / 2,
                              a->frame.y + a->frame.h - 3, UI_C_TEXT_DIM,
                              UI_ALIGN_CENTRE, a->xlabel);
    }
    if (a->ylabel[0] != '\0')
    {
        (void)ui_text_vertical(cv, UI_FONT_SMALL,
                               a->frame.x + small_h - 3,
                               a->box.y + a->box.h / 2, UI_C_TEXT_DIM,
                               UI_ALIGN_CENTRE, a->ylabel);
    }
    if (a->title[0] != '\0')
    {
        (void)ui_text_aligned(cv, UI_FONT_BOLD, a->box.x,
                              a->frame.y + ui_font_ascent(UI_FONT_BOLD) + 1,
                              UI_C_TEXT, UI_ALIGN_LEFT, a->title);
    }
}

void ui_axes_draw_overlay(UI_Canvas* cv, const UI_Axes* a)
{
    /* ---- rubber band ---------------------------------------------------- */
    if (a->zooming != 0)
    {
        const int32_t x0 = (a->zx0 < a->zx1) ? a->zx0 : a->zx1;
        const int32_t x1 = (a->zx0 < a->zx1) ? a->zx1 : a->zx0;
        const int32_t y0 = (a->zy0 < a->zy1) ? a->zy0 : a->zy1;
        const int32_t y1 = (a->zy0 < a->zy1) ? a->zy1 : a->zy0;
        const UI_Rect r = ui_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        ui_fill_rect(cv, r, UI_RGBA(0x4F, 0xA8, 0xE8, 40));
        ui_frame_rect(cv, r, UI_C_FOCUS);
    }

    /* ---- data cursor ---------------------------------------------------- */
    if (a->cursor_on != 0)
    {
        const int32_t px = (int32_t)(ui_axes_x2px(a, a->cursor_x) + 0.5);
        const int32_t py = (int32_t)(ui_axes_y2px(a, a->cursor_y) + 0.5);
        char buf[80];
        int32_t tw, bx, by;

        ui_vline_dash(cv, px, a->box.y, a->box.y + a->box.h - 1,
                      ui_color_alpha(UI_C_CURSOR, 150), 3, 3);
        ui_hline_dash(cv, a->box.x, a->box.x + a->box.w - 1, py,
                      ui_color_alpha(UI_C_CURSOR, 150), 3, 3);
        ui_frame_circle(cv, (double)px + 0.5, (double)py + 0.5, 4.0,
                        UI_C_CURSOR, 1.4);

        (void)snprintf(buf, sizeof(buf), "%.4g %s   %.4g %s",
                       a->cursor_x, a->xunit, a->cursor_y, a->yunit);
        tw = ui_text_width(UI_FONT_MONO, buf);
        bx = px + 10;
        by = py - 10 - ui_font_line_height(UI_FONT_MONO);
        if (bx + tw + 10 > a->box.x + a->box.w) { bx = px - tw - 16; }
        if (by < a->box.y) { by = py + 10; }
        ui_fill_round_rect(cv, ui_rect(bx, by, tw + 10,
                                       ui_font_line_height(UI_FONT_MONO) + 6),
                           2.0, UI_RGBA(0x10, 0x14, 0x1A, 225));
        ui_frame_round_rect(cv, ui_rect(bx, by, tw + 10,
                                        ui_font_line_height(UI_FONT_MONO) + 6),
                            2.0, ui_color_alpha(UI_C_CURSOR, 180));
        (void)ui_text(cv, UI_FONT_MONO, bx + 5,
                      by + 3 + ui_font_ascent(UI_FONT_MONO), UI_C_CURSOR, buf);
        if (a->cursor_locked != 0)
        {
            (void)ui_text(cv, UI_FONT_SMALL, bx + 5,
                          by - 3, UI_C_CURSOR, "locked");
        }
    }
}

/* ==========================================================================
 *  Series
 * ==========================================================================
 *  Traces are decimated to at most one polyline vertex per destination column.
 *  For a 1000-bin range profile drawn into a 400 px axes that turns 1000
 *  segments into 400 while preserving the visible envelope, because each column
 *  keeps the extreme sample rather than a subsample - the same min/max
 *  decimation an oscilloscope uses, and the reason a sidelobe spike never
 *  disappears when the window is narrow.
 * ------------------------------------------------------------------------ */
static void ui_plot_trace_decim(UI_Canvas* cv, const UI_Axes* a,
                                const float* y, int32_t n,
                                double x0, double dx,
                                UI_Color col, double width, int stairs)
{
    static float px[UI_PLOT_MAX_PTS * 2];
    static float py[UI_PLOT_MAX_PTS * 2];
    int32_t out = 0;
    int32_t i;

    if (y == NULL || n < 1) { return; }

    /* Samples per destination column decides whether to decimate. */
    {
        const double span_px = fabs(ui_axes_x2px(a, x0 + dx * (double)(n - 1)) -
                                    ui_axes_x2px(a, x0));
        const int32_t cols = (int32_t)(span_px + 0.5);

        if (cols >= n || cols < 2)
        {
            for (i = 0; i < n && out + 2 < UI_PLOT_MAX_PTS * 2; ++i)
            {
                const double xv = x0 + dx * (double)i;
                if (stairs != 0 && i > 0)
                {
                    px[out] = (float)ui_axes_x2px(a, xv);
                    py[out] = py[out - 1];
                    ++out;
                }
                px[out] = (float)ui_axes_x2px(a, xv);
                py[out] = (float)ui_axes_y2px(a, (double)y[i]);
                ++out;
            }
        }
        else
        {
            int32_t c;
            for (c = 0; c < cols && out + 2 < UI_PLOT_MAX_PTS * 2; ++c)
            {
                const int32_t i0 = (int32_t)((int64_t)c * n / cols);
                int32_t i1 = (int32_t)((int64_t)(c + 1) * n / cols);
                float lo, hi;
                if (i1 <= i0) { i1 = i0 + 1; }
                if (i1 > n) { i1 = n; }
                lo = y[i0]; hi = y[i0];
                for (i = i0 + 1; i < i1; ++i)
                {
                    if (y[i] < lo) { lo = y[i]; }
                    if (y[i] > hi) { hi = y[i]; }
                }
                {
                    const double xv = x0 + dx * ((double)i0 + 0.5 *
                                                 (double)(i1 - i0 - 1));
                    const float xp = (float)ui_axes_x2px(a, xv);
                    /* Emit the pair in the order that keeps the polyline
                     * monotone through the column. */
                    px[out] = xp; py[out] = (float)ui_axes_y2px(a, (double)hi); ++out;
                    px[out] = xp; py[out] = (float)ui_axes_y2px(a, (double)lo); ++out;
                }
            }
        }
    }
    ui_clip_push(cv, a->box);
    ui_polyline(cv, px, py, out, col, width);
    ui_clip_pop(cv);
}

void ui_plot_line(UI_Canvas* cv, const UI_Axes* a, const float* y, int32_t n,
                  double x0, double dx, UI_Color col, double width)
{
    ui_plot_trace_decim(cv, a, y, n, x0, dx, col, width, 0);
}

void ui_plot_stairs(UI_Canvas* cv, const UI_Axes* a, const float* y, int32_t n,
                    double x0, double dx, UI_Color col, double width)
{
    ui_plot_trace_decim(cv, a, y, n, x0, dx, col, width, 1);
}

void ui_plot_line_xy(UI_Canvas* cv, const UI_Axes* a,
                     const double* x, const double* y, int32_t n,
                     UI_Color col, double width)
{
    static float px[UI_PLOT_MAX_PTS];
    static float py[UI_PLOT_MAX_PTS];
    int32_t i, out = 0;
    if (x == NULL || y == NULL || n < 2) { return; }
    for (i = 0; i < n && out < UI_PLOT_MAX_PTS; ++i)
    {
        px[out] = (float)ui_axes_x2px(a, x[i]);
        py[out] = (float)ui_axes_y2px(a, y[i]);
        ++out;
    }
    ui_clip_push(cv, a->box);
    ui_polyline(cv, px, py, out, col, width);
    ui_clip_pop(cv);
}

void ui_plot_area(UI_Canvas* cv, const UI_Axes* a, const float* y, int32_t n,
                  double x0, double dx, double base, UI_Color fill)
{
    const int32_t by = (int32_t)(ui_axes_y2px(a, base) + 0.5);
    int32_t c, cols;
    if (y == NULL || n < 1) { return; }
    cols = (int32_t)(fabs(ui_axes_x2px(a, x0 + dx * (double)(n - 1)) -
                          ui_axes_x2px(a, x0)) + 0.5);
    if (cols < 1) { return; }

    ui_clip_push(cv, a->box);
    for (c = 0; c < cols; ++c)
    {
        const int32_t i0 = (int32_t)((int64_t)c * n / cols);
        int32_t i1 = (int32_t)((int64_t)(c + 1) * n / cols);
        float hi;
        int32_t i, xp, yp;
        if (i1 <= i0) { i1 = i0 + 1; }
        if (i1 > n) { i1 = n; }
        hi = y[i0];
        for (i = i0 + 1; i < i1; ++i) { if (y[i] > hi) { hi = y[i]; } }
        xp = (int32_t)(ui_axes_x2px(a, x0 + dx * (double)i0) + 0.5);
        yp = (int32_t)(ui_axes_y2px(a, (double)hi) + 0.5);
        if (yp < by) { ui_fill_rect(cv, ui_rect(xp, yp, 1, by - yp), fill); }
        else         { ui_fill_rect(cv, ui_rect(xp, by, 1, yp - by), fill); }
    }
    ui_clip_pop(cv);
}

void ui_plot_scatter(UI_Canvas* cv, const UI_Axes* a,
                     const double* x, const double* y, int32_t n,
                     UI_Marker m, double size, UI_Color line, UI_Color fill)
{
    int32_t i;
    if (x == NULL || y == NULL) { return; }
    ui_clip_push(cv, a->box);
    for (i = 0; i < n; ++i)
    {
        ui_marker(cv, m, ui_axes_x2px(a, x[i]), ui_axes_y2px(a, y[i]),
                  size, line, fill, 1.3);
    }
    ui_clip_pop(cv);
}

void ui_plot_vline(UI_Canvas* cv, const UI_Axes* a, double x, UI_Color col,
                   double width, int dashed)
{
    const double px = ui_axes_x2px(a, x);
    if (px < (double)a->box.x - 1.0 || px > (double)(a->box.x + a->box.w)) { return; }
    ui_clip_push(cv, a->box);
    if (dashed != 0)
    {
        ui_line_dash(cv, px, (double)a->box.y, px,
                     (double)(a->box.y + a->box.h), col, width, 5.0, 4.0);
    }
    else
    {
        ui_line(cv, px, (double)a->box.y, px,
                (double)(a->box.y + a->box.h), col, width);
    }
    ui_clip_pop(cv);
}

void ui_plot_hline(UI_Canvas* cv, const UI_Axes* a, double y, UI_Color col,
                   double width, int dashed)
{
    const double py = ui_axes_y2px(a, y);
    if (py < (double)a->box.y - 1.0 || py > (double)(a->box.y + a->box.h)) { return; }
    ui_clip_push(cv, a->box);
    if (dashed != 0)
    {
        ui_line_dash(cv, (double)a->box.x, py,
                     (double)(a->box.x + a->box.w), py, col, width, 5.0, 4.0);
    }
    else
    {
        ui_line(cv, (double)a->box.x, py,
                (double)(a->box.x + a->box.w), py, col, width);
    }
    ui_clip_pop(cv);
}

/* ==========================================================================
 *  Legend
 * ========================================================================== */
void ui_legend(UI_Canvas* cv, const UI_Axes* a, const UI_LegendEntry* e,
               int32_t n, int32_t corner)
{
    const int32_t lh  = ui_font_line_height(UI_FONT_SMALL);
    const int32_t pad = 5;
    const int32_t sw  = 20;
    int32_t wmax = 0, i, w, h, x, y;

    if (e == NULL || n <= 0) { return; }
    for (i = 0; i < n; ++i)
    {
        const int32_t tw = ui_text_width(UI_FONT_SMALL, e[i].label);
        if (tw > wmax) { wmax = tw; }
    }
    w = sw + 6 + wmax + 2 * pad;
    h = n * lh + 2 * pad;

    x = (corner == 1 || corner == 3) ? (a->box.x + a->box.w - w - 6) : (a->box.x + 6);
    y = (corner >= 2) ? (a->box.y + a->box.h - h - 6) : (a->box.y + 6);

    ui_fill_round_rect(cv, ui_rect(x, y, w, h), 3.0, UI_RGBA(0x10, 0x14, 0x1A, 205));
    ui_frame_round_rect(cv, ui_rect(x, y, w, h), 3.0, UI_C_BORDER);

    for (i = 0; i < n; ++i)
    {
        const int32_t ry = y + pad + i * lh;
        const double  cy = (double)ry + (double)lh * 0.5;
        if (e[i].marker == UI_MARKER_NONE)
        {
            ui_line(cv, (double)(x + pad), cy, (double)(x + pad + sw), cy,
                    e[i].color, 2.0);
        }
        else
        {
            ui_marker(cv, e[i].marker, (double)(x + pad + sw / 2), cy, 8.0,
                      e[i].color, UI_RGBA(0, 0, 0, 0), 1.4);
        }
        (void)ui_text(cv, UI_FONT_SMALL, x + pad + sw + 6,
                      ry + ui_font_ascent(UI_FONT_SMALL), UI_C_TEXT, e[i].label);
    }
}

/* ==========================================================================
 *  Images
 * ========================================================================== */
void ui_imagesc(UI_Canvas* cv, const UI_Axes* a, const float* data,
                int32_t rows, int32_t cols, double clim0, double clim1,
                UI_ColormapId cmap, int smooth)
{
    ui_clip_push(cv, a->box);
    /* The visible portion of the field is the part inside the current view, so
     * a zoomed axes shows a magnified sub-image rather than the whole map. */
    {
        const double x_lo = a->xfull_min;
        const double x_hi = a->xfull_max;
        const double y_lo = a->yfull_min;
        const double y_hi = a->yfull_max;
        const int32_t dx0 = (int32_t)floor(ui_axes_x2px(a, x_lo));
        const int32_t dx1 = (int32_t)ceil(ui_axes_x2px(a, x_hi));
        const int32_t dy1 = (int32_t)ceil(ui_axes_y2px(a, y_lo));
        const int32_t dy0 = (int32_t)floor(ui_axes_y2px(a, y_hi));
        ui_blit_field(cv, ui_rect(dx0, dy0, dx1 - dx0, dy1 - dy0),
                      data, rows, cols, clim0, clim1,
                      ui_colormap(cmap), 1, smooth);
    }
    ui_clip_pop(cv);
}

void ui_colorbar(UI_Canvas* cv, UI_Rect r, UI_ColormapId cmap,
                 double lo, double hi, const char* unit)
{
    const UI_Color* pal = ui_colormap(cmap);
    const int32_t bar_w = 14;
    const UI_Rect bar = ui_rect(r.x, r.y + 6, bar_w, (r.h - 12 > 8) ? (r.h - 12) : 8);
    UI_Ticks t;
    int32_t y, i;

    for (y = 0; y < bar.h; ++y)
    {
        const int32_t k = 255 - (int32_t)((double)y / (double)(bar.h - 1) * 255.0 + 0.5);
        ui_fill_rect(cv, ui_rect(bar.x, bar.y + y, bar.w, 1),
                     pal[(k < 0) ? 0 : ((k > 255) ? 255 : k)]);
    }
    ui_frame_rect(cv, bar, UI_C_AXIS);

    ui_ticks_linear(&t, lo, hi, (bar.h > 200) ? 8 : 5);
    for (i = 0; i < t.count; ++i)
    {
        const double f = (hi > lo) ? ((t.pos[i] - lo) / (hi - lo)) : 0.0;
        const int32_t py = bar.y + bar.h - 1 - (int32_t)(f * (double)(bar.h - 1) + 0.5);
        if (py < bar.y || py >= bar.y + bar.h) { continue; }
        ui_hline(cv, bar.x + bar.w, bar.x + bar.w + 3, py, UI_C_AXIS);
        (void)ui_text(cv, UI_FONT_SMALL, bar.x + bar.w + 6,
                      py + ui_font_ascent(UI_FONT_SMALL) / 2 - 1,
                      UI_C_TEXT_DIM, t.text[i]);
    }
    if (unit != NULL && unit[0] != '\0')
    {
        (void)ui_text_aligned(cv, UI_FONT_SMALL, bar.x + bar.w / 2,
                              r.y + ui_font_ascent(UI_FONT_SMALL),
                              UI_C_TEXT_DIM, UI_ALIGN_CENTRE, unit);
    }
}
