/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_gfx.h
 *  Purpose : The software renderer.  Everything the console draws goes through
 *            here, so Windows and Linux produce byte-identical frames.
 *
 *  Conventions
 *  -----------
 *   * Colour is 0xAARRGGBB.  Alpha 255 is opaque; source-over blending.
 *   * Coordinates are floating point with pixel centres at (i + 0.5).  Integer
 *     entry points exist for crisp one-pixel UI chrome.
 *   * Every primitive is clipped to the canvas clip rectangle, which is a
 *     stack so a widget can nest its own clip and restore it.
 *   * The renderer and the plotting layer above it keep static scratch
 *     buffers, so all drawing must stay on a single thread.
 *
 *  Language: ISO C17
 * ========================================================================== */
#ifndef PWRADAR_UI_GFX_H
#define PWRADAR_UI_GFX_H

#include <stddef.h>
#include <stdint.h>

#include "ui_font.h"

/* Shared by every drawing translation unit; ISO C has no portable M_PI. */
#ifndef UI_PI
#define UI_PI 3.14159265358979323846
#endif

/* --------------------------------------------------------------------------
 *  Colour
 * ------------------------------------------------------------------------ */
typedef uint32_t UI_Color;

#define UI_RGB(r, g, b)                                                       \
    ((UI_Color)0xFF000000u | ((UI_Color)(uint8_t)(r) << 16) |                  \
     ((UI_Color)(uint8_t)(g) << 8) | (UI_Color)(uint8_t)(b))
#define UI_RGBA(r, g, b, a)                                                   \
    (((UI_Color)(uint8_t)(a) << 24) | ((UI_Color)(uint8_t)(r) << 16) |          \
     ((UI_Color)(uint8_t)(g) << 8) | (UI_Color)(uint8_t)(b))

#define UI_ALPHA_OF(c) ((uint8_t)(((c) >> 24) & 0xFFu))
#define UI_RED_OF(c)   ((uint8_t)(((c) >> 16) & 0xFFu))
#define UI_GREEN_OF(c) ((uint8_t)(((c) >> 8)  & 0xFFu))
#define UI_BLUE_OF(c)  ((uint8_t)( (c)        & 0xFFu))

/** Replaces the alpha channel, keeping the colour. */
UI_Color ui_color_alpha(UI_Color c, uint8_t a);
/** Linear interpolation in sRGB space; t is clamped to [0,1]. */
UI_Color ui_color_mix(UI_Color a, UI_Color b, double t);
/** Multiplies the RGB channels by @p k (k > 1 brightens), alpha preserved. */
UI_Color ui_color_scale(UI_Color c, double k);

/* --------------------------------------------------------------------------
 *  Geometry
 * ------------------------------------------------------------------------ */
typedef struct UI_Rect
{
    int32_t x, y, w, h;
} UI_Rect;

static inline UI_Rect ui_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    UI_Rect r; r.x = x; r.y = y; r.w = w; r.h = h; return r;
}

int     ui_rect_contains(UI_Rect r, int32_t x, int32_t y);
UI_Rect ui_rect_intersect(UI_Rect a, UI_Rect b);
UI_Rect ui_rect_inset(UI_Rect r, int32_t dx, int32_t dy);
int     ui_rect_empty(UI_Rect r);

/* --------------------------------------------------------------------------
 *  Canvas
 * ------------------------------------------------------------------------ */
#define UI_CLIP_STACK_DEPTH 32

typedef struct UI_Canvas
{
    uint32_t* px;
    int32_t   w, h;
    int32_t   stride;                       /* in pixels                      */
    UI_Rect   clip;
    UI_Rect   clip_stack[UI_CLIP_STACK_DEPTH];
    int32_t   clip_depth;
} UI_Canvas;

void ui_canvas_bind(UI_Canvas* c, uint32_t* px, int32_t w, int32_t h, int32_t stride);

/** Byte-free pixel address arithmetic.  Callers reach this only after clipping,
 *  so the coordinates are non-negative by construction; widening them through
 *  uint32_t states that explicitly instead of leaving an implicit
 *  signed-to-unsigned conversion at every call site. */
static inline size_t ui_px_at(const UI_Canvas* c, int32_t x, int32_t y)
{
    return (size_t)(uint32_t)y * (size_t)(uint32_t)c->stride + (size_t)(uint32_t)x;
}

/** Row base address; the column is added by the caller's loop. */
static inline size_t ui_row_at(const UI_Canvas* c, int32_t y)
{
    return (size_t)(uint32_t)y * (size_t)(uint32_t)c->stride;
}

void ui_clip_push(UI_Canvas* c, UI_Rect r);      /* intersects with the current */
void ui_clip_pop(UI_Canvas* c);
void ui_clip_reset(UI_Canvas* c);

/* --------------------------------------------------------------------------
 *  Primitives
 * ------------------------------------------------------------------------ */
void ui_clear(UI_Canvas* c, UI_Color col);
void ui_pixel(UI_Canvas* c, int32_t x, int32_t y, UI_Color col);

/** Coverage-weighted pixel write; cov is clamped to [0,1]. */
void ui_pixel_cov(UI_Canvas* c, int32_t x, int32_t y, UI_Color col, double cov);

void ui_fill_rect(UI_Canvas* c, UI_Rect r, UI_Color col);
void ui_frame_rect(UI_Canvas* c, UI_Rect r, UI_Color col);
void ui_frame_rect_w(UI_Canvas* c, UI_Rect r, UI_Color col, int32_t width);

/** Rounded rectangle, antialiased corners.  radius is clamped to fit. */
void ui_fill_round_rect(UI_Canvas* c, UI_Rect r, double radius, UI_Color col);
void ui_frame_round_rect(UI_Canvas* c, UI_Rect r, double radius, UI_Color col);

/** Vertical linear gradient - used for panel and button surfaces. */
void ui_fill_gradient_v(UI_Canvas* c, UI_Rect r, UI_Color top, UI_Color bottom);

void ui_hline(UI_Canvas* c, int32_t x0, int32_t x1, int32_t y, UI_Color col);
void ui_vline(UI_Canvas* c, int32_t x, int32_t y0, int32_t y1, UI_Color col);

/** Dashed axis-aligned rules, used for grid lines and cursors. */
void ui_hline_dash(UI_Canvas* c, int32_t x0, int32_t x1, int32_t y,
                   UI_Color col, int32_t on, int32_t off);
void ui_vline_dash(UI_Canvas* c, int32_t x, int32_t y0, int32_t y1,
                   UI_Color col, int32_t on, int32_t off);

/** Antialiased line of arbitrary width.  Cost is O(length * width), never
 *  O(bounding box), so long diagonals are cheap. */
void ui_line(UI_Canvas* c, double x0, double y0, double x1, double y1,
             UI_Color col, double width);

/** Antialiased polyline.  @p n points, x and y in separate arrays so the plot
 *  code can hand over its own buffers without repacking. */
void ui_polyline(UI_Canvas* c, const float* xs, const float* ys, int32_t n,
                 UI_Color col, double width);

/** Dashed antialiased line (grid lines inside rotated frames, gates, ...). */
void ui_line_dash(UI_Canvas* c, double x0, double y0, double x1, double y1,
                  UI_Color col, double width, double on, double off);

/** Antialiased arrow: a shaft plus a filled triangular head whose tip lies
 *  exactly at (x1, y1).  head_len is clamped to the segment length. */
void ui_arrow(UI_Canvas* c, double x0, double y0, double x1, double y1,
              UI_Color col, double width, double head_len);

/** Convex or concave polygon fill, even-odd rule, 4x vertically supersampled. */
void ui_fill_poly(UI_Canvas* c, const float* xs, const float* ys, int32_t n,
                  UI_Color col);

void ui_fill_circle(UI_Canvas* c, double cx, double cy, double r, UI_Color col);
void ui_frame_circle(UI_Canvas* c, double cx, double cy, double r,
                     UI_Color col, double width);
/** Circular arc from a0 to a1 radians, measured counter-clockwise from +x. */
void ui_arc(UI_Canvas* c, double cx, double cy, double r,
            double a0, double a1, UI_Color col, double width);

/* --------------------------------------------------------------------------
 *  Markers (MATLAB's plot marker vocabulary)
 * ------------------------------------------------------------------------ */
typedef enum UI_Marker
{
    UI_MARKER_NONE = 0,
    UI_MARKER_CIRCLE,
    UI_MARKER_SQUARE,
    UI_MARKER_DIAMOND,
    UI_MARKER_TRIANGLE_UP,
    UI_MARKER_TRIANGLE_DOWN,
    UI_MARKER_CROSS,
    UI_MARKER_PLUS,
    UI_MARKER_STAR,
    UI_MARKER_DOT,
    UI_MARKER_COUNT
} UI_Marker;

void ui_marker(UI_Canvas* c, UI_Marker kind, double x, double y, double size,
               UI_Color line, UI_Color fill, double width);

/* --------------------------------------------------------------------------
 *  Text
 * ------------------------------------------------------------------------ */
typedef enum UI_Align
{
    UI_ALIGN_LEFT   = 0,
    UI_ALIGN_CENTRE = 1,
    UI_ALIGN_RIGHT  = 2
} UI_Align;

/** Draws @p s with its baseline at y and its left edge at x.  Returns the
 *  advance in pixels.  @p s is UTF-8; unmapped code points are skipped. */
int32_t ui_text(UI_Canvas* c, UI_FontId f, int32_t x, int32_t y,
                UI_Color col, const char* s);

/** Same, but the anchor is interpreted per @p align. */
int32_t ui_text_aligned(UI_Canvas* c, UI_FontId f, int32_t x, int32_t y,
                        UI_Color col, UI_Align align, const char* s);

/** Draws text rotated 90 degrees counter-clockwise, reading bottom-to-top.
 *  Used for the y-axis label, exactly as MATLAB's ylabel does. */
int32_t ui_text_vertical(UI_Canvas* c, UI_FontId f, int32_t x, int32_t y,
                         UI_Color col, UI_Align align, const char* s);

int32_t ui_text_width(UI_FontId f, const char* s);
int32_t ui_font_ascent(UI_FontId f);
int32_t ui_font_descent(UI_FontId f);
int32_t ui_font_line_height(UI_FontId f);

/** Convenience: centres a single line vertically inside @p r. */
int32_t ui_text_in_rect(UI_Canvas* c, UI_FontId f, UI_Rect r, int32_t pad,
                        UI_Color col, UI_Align align, const char* s);

/* --------------------------------------------------------------------------
 *  Image blit through a colour lookup
 * ------------------------------------------------------------------------ */

/**
 *  Draws a rows x cols scalar field into @p dst, mapping values through a
 *  256-entry palette between @p lo and @p hi.
 *
 *  @param src      row-major, src[r * cols + c]
 *  @param flip_y   1 => source row 0 lands at the bottom of the rectangle,
 *                  which is what an axes with an upward y-axis needs
 *  @param smooth   1 => bilinear sampling, 0 => nearest neighbour
 *
 *  Sampling is driven by per-destination-pixel index tables built once per
 *  call, so the inner loop is a straight indexed copy.
 */
void ui_blit_field(UI_Canvas* c, UI_Rect dst,
                   const float* src, int32_t rows, int32_t cols,
                   double lo, double hi,
                   const UI_Color* palette,
                   int flip_y, int smooth);

/** Same for an 8-bit field (the PPI video buffer). */
void ui_blit_field_u8(UI_Canvas* c, UI_Rect dst,
                      const uint8_t* src, int32_t rows, int32_t cols,
                      const UI_Color* palette, int flip_y, int smooth);

#endif /* PWRADAR_UI_GFX_H */
