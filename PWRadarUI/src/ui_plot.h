/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_plot.h
 *  Purpose : MATLAB-equivalent plotting: an axes object with automatic 1-2-5
 *            ticks, grids, linear and logarithmic scales, box zoom, pan,
 *            zoom-to-fit, a data cursor, a legend, line/stairs/area/scatter
 *            series, imagesc with a colour bar, and a polar PPI scope.
 *
 *  Correspondence with the MATLAB vocabulary an engineer already knows:
 *
 *      axes / xlim / ylim / xlabel / ylabel / title  -> UI_Axes fields
 *      plot                                          -> ui_plot_line*
 *      stairs                                        -> ui_plot_stairs
 *      area                                          -> ui_plot_area
 *      scatter                                       -> ui_plot_scatter
 *      semilogx / semilogy                           -> UI_Axes::xlog / ylog
 *      grid on / grid minor                          -> UI_Axes::grid
 *      legend                                        -> ui_legend
 *      imagesc + colormap + caxis + colorbar         -> ui_imagesc, ui_colorbar
 *      datacursormode                                -> UI_Axes::cursor_*
 *      zoom / pan / axis tight                       -> ui_axes_input
 *
 *  Language: ISO C17
 * ========================================================================== */
#ifndef PWRADAR_UI_PLOT_H
#define PWRADAR_UI_PLOT_H

#include "ui_colormap.h"
#include "ui_gfx.h"
#include "ui_widget.h"

#define UI_AXES_TITLE_CAP  96
#define UI_AXES_LABEL_CAP  64
#define UI_MAX_TICKS       32

typedef struct UI_Ticks
{
    double  pos[UI_MAX_TICKS];
    char    text[UI_MAX_TICKS][20];
    int32_t count;
    int32_t minor_per;          /* minor divisions between majors             */
} UI_Ticks;

typedef struct UI_Axes
{
    /* ---- geometry -------------------------------------------------------- */
    UI_Rect frame;              /* everything, labels included                */
    UI_Rect box;                /* the data area                              */

    /* ---- data window ---------------------------------------------------- */
    double  xmin, xmax, ymin, ymax;       /* the view                         */
    double  xfull_min, xfull_max;         /* full extent, for zoom reset      */
    double  yfull_min, yfull_max;
    int32_t xlog, ylog;

    /* ---- presentation --------------------------------------------------- */
    int32_t grid;               /* 0 none, 1 major, 2 major + minor           */
    int32_t box_on;
    int32_t y_right;            /* put the y axis on the right                */
    int32_t equal_aspect;
    char    title[UI_AXES_TITLE_CAP];
    char    xlabel[UI_AXES_LABEL_CAP];
    char    ylabel[UI_AXES_LABEL_CAP];
    char    xunit[16], yunit[16];

    /* ---- interaction state (persists between frames) --------------------- */
    int32_t allow_zoom, allow_pan, allow_cursor;
    int32_t zooming;            /* rubber-band box zoom in progress           */
    int32_t panning;
    int32_t zx0, zy0, zx1, zy1;
    double  pan_x0, pan_y0;
    double  pan_xmin, pan_xmax, pan_ymin, pan_ymax;
    int32_t cursor_on;          /* data cursor visible                        */
    double  cursor_x, cursor_y;
    int32_t cursor_locked;

    UI_Ticks xt, yt;
} UI_Axes;

/* --------------------------------------------------------------------------
 *  Set-up
 * ------------------------------------------------------------------------ */
void ui_axes_init(UI_Axes* a, const char* title,
                  const char* xlabel, const char* ylabel);
void ui_axes_set_lim(UI_Axes* a, double x0, double x1, double y0, double y1);
/** Sets both the view and the full extent, i.e. MATLAB's `axis([...])` on
 *  freshly plotted data. */
void ui_axes_set_full(UI_Axes* a, double x0, double x1, double y0, double y1);
void ui_axes_reset_view(UI_Axes* a);
void ui_axes_set_title(UI_Axes* a, const char* fmt, ...);

/** Computes `box` from `frame`, reserving room for the ticks, labels and
 *  title that are actually present. */
void ui_axes_layout(UI_Axes* a, UI_Rect frame);

/** Handles wheel zoom, drag pan, rubber-band zoom, double-click zoom-to-fit
 *  and the data cursor.  Returns 1 when the view changed. */
int  ui_axes_input(UI_Context* c, UI_Axes* a, uint32_t id);

/** Background fill plus grid.  Call before any series. */
void ui_axes_draw_bg(UI_Canvas* cv, UI_Axes* a);
/** Box, ticks, tick labels, axis labels and title.  Call after the series so
 *  the frame is never overdrawn by data. */
void ui_axes_draw_frame(UI_Canvas* cv, UI_Axes* a);
/** Rubber band and cursor overlay.  Call last. */
void ui_axes_draw_overlay(UI_Canvas* cv, const UI_Axes* a);

/* --------------------------------------------------------------------------
 *  Coordinate transforms
 * ------------------------------------------------------------------------ */
double ui_axes_x2px(const UI_Axes* a, double x);
double ui_axes_y2px(const UI_Axes* a, double y);
double ui_axes_px2x(const UI_Axes* a, double px);
double ui_axes_px2y(const UI_Axes* a, double py);

/* --------------------------------------------------------------------------
 *  Series
 * ------------------------------------------------------------------------ */

/** y against a uniform x grid: x(i) = x0 + i*dx.  This is the shape every
 *  radar product has, so it avoids building an x array. */
void ui_plot_line(UI_Canvas* cv, const UI_Axes* a, const float* y, int32_t n,
                  double x0, double dx, UI_Color col, double width);
void ui_plot_line_xy(UI_Canvas* cv, const UI_Axes* a,
                     const double* x, const double* y, int32_t n,
                     UI_Color col, double width);
void ui_plot_stairs(UI_Canvas* cv, const UI_Axes* a, const float* y, int32_t n,
                    double x0, double dx, UI_Color col, double width);
/** Filled area between the trace and @p base. */
void ui_plot_area(UI_Canvas* cv, const UI_Axes* a, const float* y, int32_t n,
                  double x0, double dx, double base, UI_Color fill);
void ui_plot_scatter(UI_Canvas* cv, const UI_Axes* a,
                     const double* x, const double* y, int32_t n,
                     UI_Marker m, double size, UI_Color line, UI_Color fill);
/** Vertical / horizontal reference line across the whole axes. */
void ui_plot_vline(UI_Canvas* cv, const UI_Axes* a, double x, UI_Color col,
                   double width, int dashed);
void ui_plot_hline(UI_Canvas* cv, const UI_Axes* a, double y, UI_Color col,
                   double width, int dashed);

/* --------------------------------------------------------------------------
 *  Legend
 * ------------------------------------------------------------------------ */
typedef struct UI_LegendEntry
{
    const char* label;
    UI_Color    color;
    UI_Marker   marker;         /* UI_MARKER_NONE => a line swatch            */
} UI_LegendEntry;

/** @p corner: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right. */
void ui_legend(UI_Canvas* cv, const UI_Axes* a, const UI_LegendEntry* e,
               int32_t n, int32_t corner);

/* --------------------------------------------------------------------------
 *  Images
 * ------------------------------------------------------------------------ */

/** Draws a rows x cols field into the axes box, mapped through @p cmap between
 *  clim0 and clim1.  Row 0 is the lowest y, as MATLAB's imagesc with
 *  `axis xy`. */
void ui_imagesc(UI_Canvas* cv, const UI_Axes* a, const float* data,
                int32_t rows, int32_t cols, double clim0, double clim1,
                UI_ColormapId cmap, int smooth);

/** Vertical colour bar with ticks and an optional unit caption. */
void ui_colorbar(UI_Canvas* cv, UI_Rect r, UI_ColormapId cmap,
                 double lo, double hi, const char* unit);

/* --------------------------------------------------------------------------
 *  Tick engine (exposed because the PPI needs it too)
 * ------------------------------------------------------------------------ */

/** Fills @p t with "nice" 1-2-5 ticks spanning [lo, hi], aiming for about
 *  @p want divisions, formatted with an appropriate number of digits. */
void ui_ticks_linear(UI_Ticks* t, double lo, double hi, int32_t want);
/** Decade ticks for a logarithmic axis. */
void ui_ticks_log(UI_Ticks* t, double lo, double hi);
/** Formats @p v with a sensible number of significant digits for a step of
 *  @p step, into @p out. */
void ui_format_tick(char* out, size_t cap, double v, double step);

#endif /* PWRADAR_UI_PLOT_H */
