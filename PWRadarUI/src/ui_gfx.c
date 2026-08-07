/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_gfx.c
 *  Purpose : Software renderer implementation.
 *  Language: ISO C17
 * ========================================================================== */
#include "ui_gfx.h"

#include <math.h>
#include <string.h>

/* ==========================================================================
 *  Colour helpers
 * ========================================================================== */
UI_Color ui_color_alpha(UI_Color c, uint8_t a)
{
    return (c & 0x00FFFFFFu) | ((UI_Color)a << 24);
}

UI_Color ui_color_mix(UI_Color a, UI_Color b, double t)
{
    int32_t ar, ag, ab, aa;
    if (t <= 0.0) { return a; }
    if (t >= 1.0) { return b; }
    aa = (int32_t)(UI_ALPHA_OF(a) + (UI_ALPHA_OF(b) - UI_ALPHA_OF(a)) * t + 0.5);
    ar = (int32_t)(UI_RED_OF(a)   + (UI_RED_OF(b)   - UI_RED_OF(a))   * t + 0.5);
    ag = (int32_t)(UI_GREEN_OF(a) + (UI_GREEN_OF(b) - UI_GREEN_OF(a)) * t + 0.5);
    ab = (int32_t)(UI_BLUE_OF(a)  + (UI_BLUE_OF(b)  - UI_BLUE_OF(a))  * t + 0.5);
    return UI_RGBA(ar, ag, ab, aa);
}

static int32_t ui_clamp255(double v)
{
    if (v <= 0.0)   { return 0; }
    if (v >= 255.0) { return 255; }
    return (int32_t)(v + 0.5);
}

UI_Color ui_color_scale(UI_Color c, double k)
{
    return UI_RGBA(ui_clamp255((double)UI_RED_OF(c)   * k),
                   ui_clamp255((double)UI_GREEN_OF(c) * k),
                   ui_clamp255((double)UI_BLUE_OF(c)  * k),
                   UI_ALPHA_OF(c));
}

/* ==========================================================================
 *  Geometry
 * ========================================================================== */
int ui_rect_contains(UI_Rect r, int32_t x, int32_t y)
{
    return (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) ? 1 : 0;
}

UI_Rect ui_rect_intersect(UI_Rect a, UI_Rect b)
{
    const int32_t x0 = (a.x > b.x) ? a.x : b.x;
    const int32_t y0 = (a.y > b.y) ? a.y : b.y;
    const int32_t x1 = ((a.x + a.w) < (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    const int32_t y1 = ((a.y + a.h) < (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return ui_rect(x0, y0, (x1 > x0) ? (x1 - x0) : 0, (y1 > y0) ? (y1 - y0) : 0);
}

UI_Rect ui_rect_inset(UI_Rect r, int32_t dx, int32_t dy)
{
    return ui_rect(r.x + dx, r.y + dy,
                   (r.w - 2 * dx > 0) ? (r.w - 2 * dx) : 0,
                   (r.h - 2 * dy > 0) ? (r.h - 2 * dy) : 0);
}

int ui_rect_empty(UI_Rect r) { return (r.w <= 0 || r.h <= 0) ? 1 : 0; }

/* ==========================================================================
 *  Canvas
 * ========================================================================== */
void ui_canvas_bind(UI_Canvas* c, uint32_t* px, int32_t w, int32_t h, int32_t stride)
{
    c->px         = px;
    c->w          = w;
    c->h          = h;
    c->stride     = stride;
    c->clip       = ui_rect(0, 0, w, h);
    c->clip_depth = 0;
}

void ui_clip_push(UI_Canvas* c, UI_Rect r)
{
    if (c->clip_depth < UI_CLIP_STACK_DEPTH)
    {
        c->clip_stack[c->clip_depth++] = c->clip;
    }
    c->clip = ui_rect_intersect(c->clip, r);
}

void ui_clip_pop(UI_Canvas* c)
{
    if (c->clip_depth > 0) { c->clip = c->clip_stack[--c->clip_depth]; }
}

void ui_clip_reset(UI_Canvas* c)
{
    c->clip       = ui_rect(0, 0, c->w, c->h);
    c->clip_depth = 0;
}

/* ==========================================================================
 *  Pixel writes
 * ========================================================================== */
static inline uint32_t ui_blend(uint32_t dst, UI_Color src, uint32_t a)
{
    /* Source-over with 8-bit alpha, computed on the two half-channel groups at
     * once so each pixel costs two multiplies rather than six. */
    const uint32_t ia = 255u - a;
    const uint32_t d_rb = dst & 0x00FF00FFu;
    const uint32_t d_g  = dst & 0x0000FF00u;
    const uint32_t s_rb = src & 0x00FF00FFu;
    const uint32_t s_g  = src & 0x0000FF00u;
    const uint32_t o_rb = ((d_rb * ia + s_rb * a) >> 8) & 0x00FF00FFu;
    const uint32_t o_g  = ((d_g  * ia + s_g  * a) >> 8) & 0x0000FF00u;
    return 0xFF000000u | o_rb | o_g;
}

void ui_pixel(UI_Canvas* c, int32_t x, int32_t y, UI_Color col)
{
    uint32_t a;
    if (!ui_rect_contains(c->clip, x, y)) { return; }
    a = UI_ALPHA_OF(col);
    if (a == 0u) { return; }
    if (a == 255u) { c->px[ui_px_at(c, x, y)] = 0xFF000000u | (col & 0xFFFFFFu); }
    else
    {
        uint32_t* p = &c->px[ui_px_at(c, x, y)];
        *p = ui_blend(*p, col, a);
    }
}

void ui_pixel_cov(UI_Canvas* c, int32_t x, int32_t y, UI_Color col, double cov)
{
    uint32_t a;
    if (cov <= 0.0) { return; }
    if (cov > 1.0) { cov = 1.0; }
    if (!ui_rect_contains(c->clip, x, y)) { return; }
    a = (uint32_t)((double)UI_ALPHA_OF(col) * cov + 0.5);
    if (a == 0u) { return; }
    {
        uint32_t* p = &c->px[ui_px_at(c, x, y)];
        *p = ui_blend(*p, col, a);
    }
}

/* ==========================================================================
 *  Rectangles
 * ========================================================================== */
void ui_clear(UI_Canvas* c, UI_Color col)
{
    ui_fill_rect(c, c->clip, col);
}

void ui_fill_rect(UI_Canvas* c, UI_Rect r, UI_Color col)
{
    const UI_Rect q = ui_rect_intersect(r, c->clip);
    const uint32_t a = UI_ALPHA_OF(col);
    int32_t y, x;

    if (ui_rect_empty(q) || a == 0u) { return; }

    if (a == 255u)
    {
        const uint32_t v = 0xFF000000u | (col & 0xFFFFFFu);
        for (y = q.y; y < q.y + q.h; ++y)
        {
            uint32_t* row = &c->px[ui_px_at(c, q.x, y)];
            for (x = 0; x < q.w; ++x) { row[x] = v; }
        }
    }
    else
    {
        for (y = q.y; y < q.y + q.h; ++y)
        {
            uint32_t* row = &c->px[ui_px_at(c, q.x, y)];
            for (x = 0; x < q.w; ++x) { row[x] = ui_blend(row[x], col, a); }
        }
    }
}

void ui_frame_rect(UI_Canvas* c, UI_Rect r, UI_Color col)
{
    if (r.w <= 0 || r.h <= 0) { return; }
    ui_fill_rect(c, ui_rect(r.x, r.y, r.w, 1), col);
    ui_fill_rect(c, ui_rect(r.x, r.y + r.h - 1, r.w, 1), col);
    ui_fill_rect(c, ui_rect(r.x, r.y, 1, r.h), col);
    ui_fill_rect(c, ui_rect(r.x + r.w - 1, r.y, 1, r.h), col);
}

void ui_frame_rect_w(UI_Canvas* c, UI_Rect r, UI_Color col, int32_t width)
{
    int32_t i;
    for (i = 0; i < width; ++i)
    {
        ui_frame_rect(c, ui_rect_inset(r, i, i), col);
    }
}

void ui_fill_gradient_v(UI_Canvas* c, UI_Rect r, UI_Color top, UI_Color bottom)
{
    const UI_Rect q = ui_rect_intersect(r, c->clip);
    int32_t y;
    if (ui_rect_empty(q) || r.h <= 0) { return; }
    for (y = q.y; y < q.y + q.h; ++y)
    {
        const double t = (r.h > 1) ? ((double)(y - r.y) / (double)(r.h - 1)) : 0.0;
        ui_fill_rect(c, ui_rect(q.x, y, q.w, 1), ui_color_mix(top, bottom, t));
    }
}

void ui_hline(UI_Canvas* c, int32_t x0, int32_t x1, int32_t y, UI_Color col)
{
    if (x1 < x0) { const int32_t t = x0; x0 = x1; x1 = t; }
    ui_fill_rect(c, ui_rect(x0, y, x1 - x0 + 1, 1), col);
}

void ui_vline(UI_Canvas* c, int32_t x, int32_t y0, int32_t y1, UI_Color col)
{
    if (y1 < y0) { const int32_t t = y0; y0 = y1; y1 = t; }
    ui_fill_rect(c, ui_rect(x, y0, 1, y1 - y0 + 1), col);
}

void ui_hline_dash(UI_Canvas* c, int32_t x0, int32_t x1, int32_t y,
                   UI_Color col, int32_t on, int32_t off)
{
    int32_t x;
    if (on <= 0)  { on = 1; }
    if (off <= 0) { off = 1; }
    if (x1 < x0) { const int32_t t = x0; x0 = x1; x1 = t; }
    for (x = x0; x <= x1; x += on + off)
    {
        const int32_t e = ((x + on - 1) < x1) ? (x + on - 1) : x1;
        ui_fill_rect(c, ui_rect(x, y, e - x + 1, 1), col);
    }
}

void ui_vline_dash(UI_Canvas* c, int32_t x, int32_t y0, int32_t y1,
                   UI_Color col, int32_t on, int32_t off)
{
    int32_t y;
    if (on <= 0)  { on = 1; }
    if (off <= 0) { off = 1; }
    if (y1 < y0) { const int32_t t = y0; y0 = y1; y1 = t; }
    for (y = y0; y <= y1; y += on + off)
    {
        const int32_t e = ((y + on - 1) < y1) ? (y + on - 1) : y1;
        ui_fill_rect(c, ui_rect(x, y, 1, e - y + 1), col);
    }
}

/* ==========================================================================
 *  Antialiased lines
 * ==========================================================================
 *  The segment is walked along its major axis; at each step a short span is
 *  filled perpendicular to it, with coverage taken from the true perpendicular
 *  distance.  Cost is O(length * width) rather than O(bounding box), so a long
 *  diagonal trace costs the same as a horizontal one of equal length.
 * ------------------------------------------------------------------------ */
void ui_line(UI_Canvas* c, double x0, double y0, double x1, double y1,
             UI_Color col, double width)
{
    double dx, dy, len, hw;

    if (width <= 0.0) { width = 1.0; }
    hw = 0.5 * width;
    dx = x1 - x0;
    dy = y1 - y0;
    len = sqrt(dx * dx + dy * dy);

    if (len < 1e-9)
    {
        ui_fill_circle(c, x0, y0, hw, col);
        return;
    }

    if (fabs(dx) >= fabs(dy))
    {
        /* x-major: for each column, shade a vertical span. */
        const double slope = dy / dx;
        const double scale = len / fabs(dx);        /* 1 / cos(theta)          */
        const double span  = hw * scale + 0.5;
        int32_t xa, xb, ix;

        if (x1 < x0)
        {
            const double tx = x0, ty = y0;
            x0 = x1; y0 = y1; x1 = tx; y1 = ty;
        }
        xa = (int32_t)floor(x0 - hw);
        xb = (int32_t)ceil(x1 + hw);
        if (xa < c->clip.x - 1) { xa = c->clip.x - 1; }
        if (xb > c->clip.x + c->clip.w) { xb = c->clip.x + c->clip.w; }

        for (ix = xa; ix <= xb; ++ix)
        {
            const double px = (double)ix + 0.5;
            /* Clamp to the segment so the ends get round-ish caps rather than
             * extending the infinite line. */
            const double t  = (px < x0) ? x0 : ((px > x1) ? x1 : px);
            const double cy = y0 + (t - x0) * slope;
            const double edge = (px < x0) ? (x0 - px) : ((px > x1) ? (px - x1) : 0.0);
            int32_t iy;
            if (edge > hw + 0.5) { continue; }
            for (iy = (int32_t)floor(cy - span); iy <= (int32_t)ceil(cy + span); ++iy)
            {
                const double d_perp = fabs(((double)iy + 0.5) - cy) / scale;
                const double d = sqrt(d_perp * d_perp + edge * edge);
                ui_pixel_cov(c, ix, iy, col, hw + 0.5 - d);
            }
        }
    }
    else
    {
        const double slope = dx / dy;
        const double scale = len / fabs(dy);
        const double span  = hw * scale + 0.5;
        int32_t ya, yb, iy;

        if (y1 < y0)
        {
            const double tx = x0, ty = y0;
            x0 = x1; y0 = y1; x1 = tx; y1 = ty;
        }
        ya = (int32_t)floor(y0 - hw);
        yb = (int32_t)ceil(y1 + hw);
        if (ya < c->clip.y - 1) { ya = c->clip.y - 1; }
        if (yb > c->clip.y + c->clip.h) { yb = c->clip.y + c->clip.h; }

        for (iy = ya; iy <= yb; ++iy)
        {
            const double py = (double)iy + 0.5;
            const double t  = (py < y0) ? y0 : ((py > y1) ? y1 : py);
            const double cx = x0 + (t - y0) * slope;
            const double edge = (py < y0) ? (y0 - py) : ((py > y1) ? (py - y1) : 0.0);
            int32_t ix;
            if (edge > hw + 0.5) { continue; }
            for (ix = (int32_t)floor(cx - span); ix <= (int32_t)ceil(cx + span); ++ix)
            {
                const double d_perp = fabs(((double)ix + 0.5) - cx) / scale;
                const double d = sqrt(d_perp * d_perp + edge * edge);
                ui_pixel_cov(c, ix, iy, col, hw + 0.5 - d);
            }
        }
    }
}

void ui_polyline(UI_Canvas* c, const float* xs, const float* ys, int32_t n,
                 UI_Color col, double width)
{
    int32_t i;
    if (xs == NULL || ys == NULL || n < 2) { return; }
    for (i = 1; i < n; ++i)
    {
        ui_line(c, (double)xs[i - 1], (double)ys[i - 1],
                   (double)xs[i],     (double)ys[i], col, width);
    }
}

void ui_line_dash(UI_Canvas* c, double x0, double y0, double x1, double y1,
                  UI_Color col, double width, double on, double off)
{
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = sqrt(dx * dx + dy * dy);
    double s = 0.0;
    if (len < 1e-9) { return; }
    if (on <= 0.0)  { on = 4.0; }
    if (off <= 0.0) { off = 4.0; }
    while (s < len)
    {
        const double e = (s + on < len) ? (s + on) : len;
        ui_line(c, x0 + dx * (s / len), y0 + dy * (s / len),
                   x0 + dx * (e / len), y0 + dy * (e / len), col, width);
        s = e + off;
    }
}

/* ==========================================================================
 *  Polygon fill
 * ==========================================================================
 *  Scanline fill with four vertical sub-samples and exact horizontal span
 *  coverage, which is enough antialiasing for the marker and symbol shapes the
 *  console draws while staying allocation free.
 * ------------------------------------------------------------------------ */
#define UI_POLY_MAX_VERTS 64
#define UI_POLY_SUBSAMP   4

void ui_fill_poly(UI_Canvas* c, const float* xs, const float* ys, int32_t n,
                  UI_Color col)
{
    double ymin = 1e30, ymax = -1e30;
    int32_t y0, y1, y, i, s;
    static float cov_row[4096];

    if (xs == NULL || ys == NULL || n < 3 || n > UI_POLY_MAX_VERTS) { return; }

    for (i = 0; i < n; ++i)
    {
        if ((double)ys[i] < ymin) { ymin = (double)ys[i]; }
        if ((double)ys[i] > ymax) { ymax = (double)ys[i]; }
    }
    y0 = (int32_t)floor(ymin);
    y1 = (int32_t)ceil(ymax);
    if (y0 < c->clip.y) { y0 = c->clip.y; }
    if (y1 > c->clip.y + c->clip.h - 1) { y1 = c->clip.y + c->clip.h - 1; }
    if (y1 < y0) { return; }

    for (y = y0; y <= y1; ++y)
    {
        const int32_t rx0 = c->clip.x;
        const int32_t rw  = (c->clip.w < 4096) ? c->clip.w : 4096;
        int any = 0;

        memset(cov_row, 0, (size_t)(uint32_t)rw * sizeof(float));

        for (s = 0; s < UI_POLY_SUBSAMP; ++s)
        {
            const double sy = (double)y + ((double)s + 0.5) / (double)UI_POLY_SUBSAMP;
            double xin[UI_POLY_MAX_VERTS];
            int32_t nx = 0, a, b;

            for (i = 0; i < n; ++i)
            {
                const double ay = (double)ys[i];
                const double by = (double)ys[(i + 1) % n];
                const double ax = (double)xs[i];
                const double bx = (double)xs[(i + 1) % n];
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy))
                {
                    const double t = (sy - ay) / (by - ay);
                    if (nx < UI_POLY_MAX_VERTS) { xin[nx++] = ax + t * (bx - ax); }
                }
            }
            /* Insertion sort: nx is tiny. */
            for (a = 1; a < nx; ++a)
            {
                const double key = xin[a];
                b = a - 1;
                while (b >= 0 && xin[b] > key) { xin[b + 1] = xin[b]; --b; }
                xin[b + 1] = key;
            }
            for (a = 0; a + 1 < nx; a += 2)
            {
                double xa = xin[a], xb = xin[a + 1];
                int32_t ix, ia, ib;
                if (xb <= xa) { continue; }
                if (xa < (double)rx0) { xa = (double)rx0; }
                if (xb > (double)(rx0 + rw)) { xb = (double)(rx0 + rw); }
                if (xb <= xa) { continue; }
                ia = (int32_t)floor(xa);
                ib = (int32_t)ceil(xb) - 1;
                for (ix = ia; ix <= ib; ++ix)
                {
                    const double l = (xa > (double)ix) ? xa : (double)ix;
                    const double r = (xb < (double)(ix + 1)) ? xb : (double)(ix + 1);
                    const int32_t k = ix - rx0;
                    if (r > l && k >= 0 && k < rw)
                    {
                        cov_row[k] += (float)((r - l) / (double)UI_POLY_SUBSAMP);
                        any = 1;
                    }
                }
            }
        }

        if (any != 0)
        {
            int32_t k;
            for (k = 0; k < rw; ++k)
            {
                if (cov_row[k] > 0.0f)
                {
                    ui_pixel_cov(c, rx0 + k, y, col, (double)cov_row[k]);
                }
            }
        }
    }
}

/* ==========================================================================
 *  Circles and arcs
 * ========================================================================== */
void ui_fill_circle(UI_Canvas* c, double cx, double cy, double r, UI_Color col)
{
    int32_t x0, x1, y0, y1, x, y;
    if (r <= 0.0) { return; }
    x0 = (int32_t)floor(cx - r - 1.0);
    x1 = (int32_t)ceil(cx + r + 1.0);
    y0 = (int32_t)floor(cy - r - 1.0);
    y1 = (int32_t)ceil(cy + r + 1.0);
    if (x0 < c->clip.x) { x0 = c->clip.x; }
    if (y0 < c->clip.y) { y0 = c->clip.y; }
    if (x1 > c->clip.x + c->clip.w - 1) { x1 = c->clip.x + c->clip.w - 1; }
    if (y1 > c->clip.y + c->clip.h - 1) { y1 = c->clip.y + c->clip.h - 1; }

    for (y = y0; y <= y1; ++y)
    {
        const double dy = ((double)y + 0.5) - cy;
        for (x = x0; x <= x1; ++x)
        {
            const double dx = ((double)x + 0.5) - cx;
            const double d  = sqrt(dx * dx + dy * dy);
            ui_pixel_cov(c, x, y, col, r + 0.5 - d);
        }
    }
}

void ui_frame_circle(UI_Canvas* c, double cx, double cy, double r,
                     UI_Color col, double width)
{
    const double hw = 0.5 * ((width > 0.0) ? width : 1.0);
    int32_t x0, x1, y0, y1, x, y;
    if (r <= 0.0) { return; }
    x0 = (int32_t)floor(cx - r - hw - 1.0);
    x1 = (int32_t)ceil(cx + r + hw + 1.0);
    y0 = (int32_t)floor(cy - r - hw - 1.0);
    y1 = (int32_t)ceil(cy + r + hw + 1.0);
    if (x0 < c->clip.x) { x0 = c->clip.x; }
    if (y0 < c->clip.y) { y0 = c->clip.y; }
    if (x1 > c->clip.x + c->clip.w - 1) { x1 = c->clip.x + c->clip.w - 1; }
    if (y1 > c->clip.y + c->clip.h - 1) { y1 = c->clip.y + c->clip.h - 1; }

    for (y = y0; y <= y1; ++y)
    {
        const double dy = ((double)y + 0.5) - cy;
        for (x = x0; x <= x1; ++x)
        {
            const double dx = ((double)x + 0.5) - cx;
            const double d  = fabs(sqrt(dx * dx + dy * dy) - r);
            ui_pixel_cov(c, x, y, col, hw + 0.5 - d);
        }
    }
}

void ui_arc(UI_Canvas* c, double cx, double cy, double r,
            double a0, double a1, UI_Color col, double width)
{
    /* Approximated by a polyline: one segment per ~2 px of arc length keeps the
     * chord error well under a tenth of a pixel. */
    const double sweep = a1 - a0;
    int32_t steps = (int32_t)(fabs(sweep) * r / 2.0) + 2;
    double px = 0.0, py = 0.0;
    int32_t i;
    if (r <= 0.0) { return; }
    if (steps > 4096) { steps = 4096; }
    for (i = 0; i <= steps; ++i)
    {
        const double a = a0 + sweep * ((double)i / (double)steps);
        const double x = cx + r * cos(a);
        const double y = cy - r * sin(a);
        if (i > 0) { ui_line(c, px, py, x, y, col, width); }
        px = x; py = y;
    }
}

/* ==========================================================================
 *  Rounded rectangles
 * ========================================================================== */
void ui_fill_round_rect(UI_Canvas* c, UI_Rect r, double radius, UI_Color col)
{
    const double maxr = 0.5 * ((r.w < r.h) ? (double)r.w : (double)r.h);
    int32_t x, y;
    if (ui_rect_empty(r)) { return; }
    if (radius > maxr) { radius = maxr; }
    if (radius <= 0.5) { ui_fill_rect(c, r, col); return; }

    /* Straight middle band plus four antialiased corner quadrants. */
    ui_fill_rect(c, ui_rect(r.x, r.y + (int32_t)radius, r.w,
                            r.h - 2 * (int32_t)radius), col);
    ui_fill_rect(c, ui_rect(r.x + (int32_t)radius, r.y,
                            r.w - 2 * (int32_t)radius, (int32_t)radius), col);
    ui_fill_rect(c, ui_rect(r.x + (int32_t)radius, r.y + r.h - (int32_t)radius,
                            r.w - 2 * (int32_t)radius, (int32_t)radius), col);
    {
        const double ri = radius;
        const double cxs[4] = { r.x + ri, r.x + r.w - ri, r.x + ri, r.x + r.w - ri };
        const double cys[4] = { r.y + ri, r.y + ri, r.y + r.h - ri, r.y + r.h - ri };
        const int32_t oxs[4] = { r.x, r.x + r.w - (int32_t)ri, r.x,
                                 r.x + r.w - (int32_t)ri };
        const int32_t oys[4] = { r.y, r.y, r.y + r.h - (int32_t)ri,
                                 r.y + r.h - (int32_t)ri };
        int32_t k;
        for (k = 0; k < 4; ++k)
        {
            for (y = oys[k]; y < oys[k] + (int32_t)ri; ++y)
            {
                for (x = oxs[k]; x < oxs[k] + (int32_t)ri; ++x)
                {
                    const double dx = ((double)x + 0.5) - cxs[k];
                    const double dy = ((double)y + 0.5) - cys[k];
                    ui_pixel_cov(c, x, y, col, ri + 0.5 - sqrt(dx * dx + dy * dy));
                }
            }
        }
    }
}

void ui_frame_round_rect(UI_Canvas* c, UI_Rect r, double radius, UI_Color col)
{
    const double maxr = 0.5 * ((r.w < r.h) ? (double)r.w : (double)r.h);
    if (ui_rect_empty(r)) { return; }
    if (radius > maxr) { radius = maxr; }
    if (radius <= 0.5) { ui_frame_rect(c, r, col); return; }
    {
        const double x0 = (double)r.x + 0.5;
        const double y0 = (double)r.y + 0.5;
        const double x1 = (double)(r.x + r.w) - 0.5;
        const double y1 = (double)(r.y + r.h) - 0.5;
        ui_line(c, x0 + radius, y0, x1 - radius, y0, col, 1.0);
        ui_line(c, x0 + radius, y1, x1 - radius, y1, col, 1.0);
        ui_line(c, x0, y0 + radius, x0, y1 - radius, col, 1.0);
        ui_line(c, x1, y0 + radius, x1, y1 - radius, col, 1.0);
        ui_arc(c, x0 + radius, y0 + radius, radius, 0.5 * UI_PI, UI_PI, col, 1.0);
        ui_arc(c, x1 - radius, y0 + radius, radius, 0.0, 0.5 * UI_PI, col, 1.0);
        ui_arc(c, x0 + radius, y1 - radius, radius, UI_PI, 1.5 * UI_PI, col, 1.0);
        ui_arc(c, x1 - radius, y1 - radius, radius, 1.5 * UI_PI, 2.0 * UI_PI, col, 1.0);
    }
}

/* ==========================================================================
 *  Markers
 * ========================================================================== */
void ui_marker(UI_Canvas* c, UI_Marker kind, double x, double y, double size,
               UI_Color line, UI_Color fill, double width)
{
    const double h = 0.5 * size;
    float px[8], py[8];

    switch (kind)
    {
    case UI_MARKER_NONE:
        break;

    case UI_MARKER_DOT:
        ui_fill_circle(c, x, y, h, line);
        break;

    case UI_MARKER_CIRCLE:
        if (UI_ALPHA_OF(fill) != 0u) { ui_fill_circle(c, x, y, h, fill); }
        ui_frame_circle(c, x, y, h, line, width);
        break;

    case UI_MARKER_SQUARE:
        if (UI_ALPHA_OF(fill) != 0u)
        {
            ui_fill_rect(c, ui_rect((int32_t)(x - h), (int32_t)(y - h),
                                    (int32_t)size, (int32_t)size), fill);
        }
        ui_line(c, x - h, y - h, x + h, y - h, line, width);
        ui_line(c, x + h, y - h, x + h, y + h, line, width);
        ui_line(c, x + h, y + h, x - h, y + h, line, width);
        ui_line(c, x - h, y + h, x - h, y - h, line, width);
        break;

    case UI_MARKER_DIAMOND:
        px[0] = (float)x;     py[0] = (float)(y - h);
        px[1] = (float)(x + h); py[1] = (float)y;
        px[2] = (float)x;     py[2] = (float)(y + h);
        px[3] = (float)(x - h); py[3] = (float)y;
        if (UI_ALPHA_OF(fill) != 0u) { ui_fill_poly(c, px, py, 4, fill); }
        ui_line(c, px[0], py[0], px[1], py[1], line, width);
        ui_line(c, px[1], py[1], px[2], py[2], line, width);
        ui_line(c, px[2], py[2], px[3], py[3], line, width);
        ui_line(c, px[3], py[3], px[0], py[0], line, width);
        break;

    case UI_MARKER_TRIANGLE_UP:
    case UI_MARKER_TRIANGLE_DOWN:
    {
        const double s = (kind == UI_MARKER_TRIANGLE_UP) ? -1.0 : 1.0;
        px[0] = (float)x;           py[0] = (float)(y + s * h);
        px[1] = (float)(x + h);     py[1] = (float)(y - s * h * 0.8);
        px[2] = (float)(x - h);     py[2] = (float)(y - s * h * 0.8);
        if (UI_ALPHA_OF(fill) != 0u) { ui_fill_poly(c, px, py, 3, fill); }
        ui_line(c, px[0], py[0], px[1], py[1], line, width);
        ui_line(c, px[1], py[1], px[2], py[2], line, width);
        ui_line(c, px[2], py[2], px[0], py[0], line, width);
        break;
    }

    case UI_MARKER_CROSS:
        ui_line(c, x - h, y - h, x + h, y + h, line, width);
        ui_line(c, x - h, y + h, x + h, y - h, line, width);
        break;

    case UI_MARKER_PLUS:
        ui_line(c, x - h, y, x + h, y, line, width);
        ui_line(c, x, y - h, x, y + h, line, width);
        break;

    case UI_MARKER_STAR:
        ui_line(c, x - h, y, x + h, y, line, width);
        ui_line(c, x, y - h, x, y + h, line, width);
        ui_line(c, x - h * 0.7, y - h * 0.7, x + h * 0.7, y + h * 0.7, line, width);
        ui_line(c, x - h * 0.7, y + h * 0.7, x + h * 0.7, y - h * 0.7, line, width);
        break;

    default:
        break;
    }
}

/* ==========================================================================
 *  Text
 * ========================================================================== */

/* Minimal UTF-8 decoder.  Returns the code point and advances *s; malformed
 * sequences yield U+FFFD and consume one byte, which keeps the renderer from
 * ever looping on bad input. */
static uint32_t ui_utf8_next(const char** s)
{
    const unsigned char* p = (const unsigned char*)*s;
    uint32_t cp;
    int extra, i;

    if (p[0] < 0x80u)      { *s += 1; return p[0]; }
    else if ((p[0] & 0xE0u) == 0xC0u) { cp = p[0] & 0x1Fu; extra = 1; }
    else if ((p[0] & 0xF0u) == 0xE0u) { cp = p[0] & 0x0Fu; extra = 2; }
    else if ((p[0] & 0xF8u) == 0xF0u) { cp = p[0] & 0x07u; extra = 3; }
    else { *s += 1; return 0xFFFDu; }

    for (i = 1; i <= extra; ++i)
    {
        if ((p[i] & 0xC0u) != 0x80u) { *s += 1; return 0xFFFDu; }
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3Fu);
    }
    *s += extra + 1;
    return cp;
}

static const UI_Glyph* ui_glyph_for(const UI_Font* f, uint32_t cp)
{
    /* ASCII is a direct index; the small symbol block is a linear scan. */
    if (cp >= 32u && cp < 127u) { return &f->glyphs[cp - 32u]; }
    {
        uint16_t i;
        for (i = 95u; i < f->count; ++i)
        {
            if ((uint32_t)f->codepoints[i] == cp) { return &f->glyphs[i]; }
        }
    }
    return NULL;
}

static void ui_draw_glyph(UI_Canvas* c, const UI_Font* f, const UI_Glyph* g,
                          int32_t pen_x, int32_t baseline, UI_Color col)
{
    const int32_t x0 = pen_x + g->bx;
    const int32_t y0 = baseline + g->by;
    const uint32_t base_a = UI_ALPHA_OF(col);
    int32_t gy, gx;

    if (g->w == 0u || g->h == 0u) { return; }

    for (gy = 0; gy < (int32_t)g->h; ++gy)
    {
        const int32_t py = y0 + gy;
        const uint8_t* srow = &f->atlas[(size_t)(uint32_t)gy * f->atlas_w + g->atlas_x];
        uint32_t* drow;
        if (py < c->clip.y || py >= c->clip.y + c->clip.h) { continue; }
        drow = &c->px[ui_row_at(c, py)];
        for (gx = 0; gx < (int32_t)g->w; ++gx)
        {
            const int32_t pxx = x0 + gx;
            const uint32_t cov = srow[gx];
            uint32_t a;
            if (cov == 0u) { continue; }
            if (pxx < c->clip.x || pxx >= c->clip.x + c->clip.w) { continue; }
            a = (cov * base_a) / 255u;
            if (a == 0u) { continue; }
            drow[pxx] = ui_blend(drow[pxx], col, a);
        }
    }
}

int32_t ui_text(UI_Canvas* c, UI_FontId fid, int32_t x, int32_t y,
                UI_Color col, const char* s)
{
    const UI_Font* f;
    int32_t pen = x;
    if (s == NULL || (int)fid < 0 || (int)fid >= UI_FONT_COUNT) { return 0; }
    f = &ui_fonts[fid];
    while (*s != '\0')
    {
        const uint32_t cp = ui_utf8_next(&s);
        const UI_Glyph* g = ui_glyph_for(f, cp);
        if (g == NULL) { continue; }
        if (c != NULL) { ui_draw_glyph(c, f, g, pen, y, col); }
        pen += (int32_t)g->advance;
    }
    return pen - x;
}

int32_t ui_text_width(UI_FontId fid, const char* s)
{
    return ui_text(NULL, fid, 0, 0, 0u, s);
}

int32_t ui_text_aligned(UI_Canvas* c, UI_FontId fid, int32_t x, int32_t y,
                        UI_Color col, UI_Align align, const char* s)
{
    const int32_t w = ui_text_width(fid, s);
    int32_t ax = x;
    if (align == UI_ALIGN_CENTRE) { ax = x - w / 2; }
    else if (align == UI_ALIGN_RIGHT) { ax = x - w; }
    return ui_text(c, fid, ax, y, col, s);
}

int32_t ui_text_vertical(UI_Canvas* c, UI_FontId fid, int32_t x, int32_t y,
                         UI_Color col, UI_Align align, const char* s)
{
    /* Rotated 90 degrees counter-clockwise: the pen advances upward and each
     * glyph's atlas rows map to destination columns. */
    const UI_Font* f;
    const int32_t total = ui_text_width(fid, s);
    int32_t pen;

    if (s == NULL || (int)fid < 0 || (int)fid >= UI_FONT_COUNT) { return 0; }
    f = &ui_fonts[fid];
    pen = y;
    if (align == UI_ALIGN_CENTRE) { pen = y + total / 2; }
    else if (align == UI_ALIGN_RIGHT) { pen = y + total; }

    while (*s != '\0')
    {
        const uint32_t cp = ui_utf8_next(&s);
        const UI_Glyph* g = ui_glyph_for(f, cp);
        int32_t gy, gx;
        if (g == NULL) { continue; }
        for (gy = 0; gy < (int32_t)g->h; ++gy)
        {
            const uint8_t* srow = &f->atlas[(size_t)(uint32_t)gy * f->atlas_w + g->atlas_x];
            const int32_t dx = x + g->by + gy;
            for (gx = 0; gx < (int32_t)g->w; ++gx)
            {
                const uint32_t cov = srow[gx];
                if (cov == 0u) { continue; }
                ui_pixel_cov(c, dx, pen - g->bx - gx, col, (double)cov / 255.0);
            }
        }
        pen -= (int32_t)g->advance;
    }
    return total;
}

int32_t ui_font_ascent(UI_FontId f)
{
    return ((int)f >= 0 && (int)f < UI_FONT_COUNT) ? (int32_t)ui_fonts[f].ascent : 0;
}
int32_t ui_font_descent(UI_FontId f)
{
    return ((int)f >= 0 && (int)f < UI_FONT_COUNT) ? (int32_t)ui_fonts[f].descent : 0;
}
int32_t ui_font_line_height(UI_FontId f)
{
    return ((int)f >= 0 && (int)f < UI_FONT_COUNT)
        ? (int32_t)ui_fonts[f].line_height : 0;
}

int32_t ui_text_in_rect(UI_Canvas* c, UI_FontId f, UI_Rect r, int32_t pad,
                        UI_Color col, UI_Align align, const char* s)
{
    const int32_t base = r.y + (r.h + ui_font_ascent(f) - ui_font_descent(f)) / 2;
    int32_t x = r.x + pad;
    if (align == UI_ALIGN_CENTRE) { x = r.x + r.w / 2; }
    else if (align == UI_ALIGN_RIGHT) { x = r.x + r.w - pad; }
    return ui_text_aligned(c, f, x, base, col, align, s);
}

/* ==========================================================================
 *  Field blitting
 * ==========================================================================
 *  Destination-driven resampling with precomputed source index tables, so the
 *  inner loop is an indexed palette lookup with no per-pixel arithmetic beyond
 *  the value-to-index scale.
 * ------------------------------------------------------------------------ */
#define UI_BLIT_MAX_DIM 4096

void ui_blit_field(UI_Canvas* c, UI_Rect dst,
                   const float* src, int32_t rows, int32_t cols,
                   double lo, double hi,
                   const UI_Color* palette,
                   int flip_y, int smooth)
{
    static int32_t col_idx[UI_BLIT_MAX_DIM];
    static float   col_frac[UI_BLIT_MAX_DIM];
    const UI_Rect q = ui_rect_intersect(dst, c->clip);
    const double span = (hi > lo) ? (hi - lo) : 1.0;
    const double scale = 255.0 / span;
    int32_t x, y;

    if (src == NULL || palette == NULL || rows < 1 || cols < 1) { return; }
    if (ui_rect_empty(q) || dst.w < 1 || dst.h < 1) { return; }
    if (q.w > UI_BLIT_MAX_DIM) { return; }

    for (x = 0; x < q.w; ++x)
    {
        const double u = ((double)(q.x + x - dst.x) + 0.5) / (double)dst.w *
                         (double)cols - 0.5;
        int32_t i = (int32_t)floor(u);
        double fr = u - (double)i;
        if (i < 0) { i = 0; fr = 0.0; }
        if (i > cols - 1) { i = cols - 1; fr = 0.0; }
        col_idx[x]  = i;
        col_frac[x] = (float)fr;
    }

    for (y = 0; y < q.h; ++y)
    {
        const double v = ((double)(q.y + y - dst.y) + 0.5) / (double)dst.h *
                         (double)rows - 0.5;
        int32_t r0 = (int32_t)floor(v);
        double  rf = v - (double)r0;
        int32_t r1;
        uint32_t* drow = &c->px[ui_px_at(c, q.x, q.y + y)];

        if (r0 < 0) { r0 = 0; rf = 0.0; }
        if (r0 > rows - 1) { r0 = rows - 1; rf = 0.0; }
        r1 = (r0 + 1 < rows) ? (r0 + 1) : r0;
        if (flip_y != 0)
        {
            r0 = rows - 1 - r0;
            r1 = rows - 1 - r1;
        }

        if (smooth == 0)
        {
            const float* srow = &src[(size_t)(uint32_t)r0 * (uint32_t)cols];
            for (x = 0; x < q.w; ++x)
            {
                const double val = (double)srow[col_idx[x]];
                int32_t k = (int32_t)((val - lo) * scale + 0.5);
                if (k < 0) { k = 0; } else if (k > 255) { k = 255; }
                drow[x] = palette[k];
            }
        }
        else
        {
            const float* s0 = &src[(size_t)(uint32_t)r0 * (uint32_t)cols];
            const float* s1 = &src[(size_t)(uint32_t)r1 * (uint32_t)cols];
            for (x = 0; x < q.w; ++x)
            {
                const int32_t i0 = col_idx[x];
                const int32_t i1 = (i0 + 1 < cols) ? (i0 + 1) : i0;
                const double fx = (double)col_frac[x];
                const double a = (double)s0[i0] * (1.0 - fx) + (double)s0[i1] * fx;
                const double b = (double)s1[i0] * (1.0 - fx) + (double)s1[i1] * fx;
                const double val = a * (1.0 - rf) + b * rf;
                int32_t k = (int32_t)((val - lo) * scale + 0.5);
                if (k < 0) { k = 0; } else if (k > 255) { k = 255; }
                drow[x] = palette[k];
            }
        }
    }
}

void ui_blit_field_u8(UI_Canvas* c, UI_Rect dst,
                      const uint8_t* src, int32_t rows, int32_t cols,
                      const UI_Color* palette, int flip_y, int smooth)
{
    static int32_t col_idx[UI_BLIT_MAX_DIM];
    const UI_Rect q = ui_rect_intersect(dst, c->clip);
    int32_t x, y;

    if (src == NULL || palette == NULL || rows < 1 || cols < 1) { return; }
    if (ui_rect_empty(q) || dst.w < 1 || dst.h < 1) { return; }
    if (q.w > UI_BLIT_MAX_DIM) { return; }
    (void)smooth;

    for (x = 0; x < q.w; ++x)
    {
        int32_t i = (int32_t)(((double)(q.x + x - dst.x) + 0.5) /
                              (double)dst.w * (double)cols);
        if (i < 0) { i = 0; }
        if (i > cols - 1) { i = cols - 1; }
        col_idx[x] = i;
    }
    for (y = 0; y < q.h; ++y)
    {
        int32_t r = (int32_t)(((double)(q.y + y - dst.y) + 0.5) /
                              (double)dst.h * (double)rows);
        const uint8_t* srow;
        uint32_t* drow = &c->px[ui_px_at(c, q.x, q.y + y)];
        if (r < 0) { r = 0; }
        if (r > rows - 1) { r = rows - 1; }
        if (flip_y != 0) { r = rows - 1 - r; }
        srow = &src[(size_t)(uint32_t)r * (uint32_t)cols];
        for (x = 0; x < q.w; ++x) { drow[x] = palette[srow[col_idx[x]]]; }
    }
}
