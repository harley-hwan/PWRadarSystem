/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_ppi.h
 *  Purpose : Plan-position-indicator scope: the display an operator actually
 *            watches.  Range rings and bearing spokes, log-compressed radar
 *            video painted from the engine's polar buffer, the rotating beam
 *            with its afterglow, plot and track symbology with history trails,
 *            an optional ground-truth overlay for verification, and a bearing
 *            and range readout under the cursor.
 *
 *  Polar to raster conversion
 *  --------------------------
 *  Every destination pixel needs an (azimuth cell, range gate) pair, which
 *  costs an atan2 and a sqrt.  At 700 x 700 that is half a million transcen-
 *  dental calls per frame, so the mapping is cached in an index table and
 *  rebuilt only when the geometry or the range scale changes.  Painting is then
 *  a pair of indexed fetches per pixel.
 *
 *  Language: ISO C17
 * ========================================================================== */
#ifndef PWRADAR_UI_PPI_H
#define PWRADAR_UI_PPI_H

#include "pwradar/pwr_api.h"

#include "ui_colormap.h"
#include "ui_gfx.h"
#include "ui_history.h"
#include "ui_widget.h"

typedef struct UI_Ppi
{
    /* ---- geometry -------------------------------------------------------- */
    UI_Rect  frame;
    UI_Rect  box;               /* the square the scope is inscribed in       */
    double   cx, cy;            /* scope centre, pixels                       */
    double   radius;            /* scope radius, pixels                       */

    /* ---- view ----------------------------------------------------------- */
    double   range_max_m;       /* range at the outer ring                    */
    double   range_max_full_m;  /* the engine's coverage, for zoom reset      */
    double   heading_up_deg;    /* rotate the picture (0 => north up)         */
    int32_t  cmap;              /* UI_ColormapId for the video                */
    double   video_gain_db;     /* operator brightness control                */
    double   leader_time_s;     /* velocity-vector length, seconds of travel  */

    /* ---- layers --------------------------------------------------------- */
    int32_t  show_video;
    int32_t  show_rings;
    int32_t  show_beam;
    int32_t  show_detections;
    int32_t  show_tracks;
    int32_t  show_trails;
    int32_t  show_truth;
    int32_t  show_labels;
    int32_t  show_gates;        /* draw the track covariance ellipse          */

    /* ---- history overlays (data owned by the application) ---------------
     *  The app accumulates long-term display history from the frames it has
     *  already acquired and lends it to the scope through these pointers, so
     *  the PPI can show full paths, scan-persistent plots, plot-to-track
     *  association and the per-dwell hit/miss evidence. */
    const UI_PathSet*     track_paths;
    const UI_PathSet*     truth_paths;
    const UI_PlotHistory* plot_hist;
    double   hist_retain_s;     /* fade / prune window for the overlays       */
    int32_t  show_track_paths;
    int32_t  show_truth_paths;
    int32_t  show_plot_hist;
    int32_t  show_assoc;        /* plot-to-track association lines            */
    int32_t  show_dwell;        /* per-dwell hit / miss rings on tracks       */

    /* ---- interaction ---------------------------------------------------- */
    int32_t  selected_track;    /* track id, 0 when nothing is selected       */
    int32_t  cursor_on;
    double   cursor_range_m;
    double   cursor_bearing_deg;

    /* ---- polar mapping cache -------------------------------------------- */
    uint16_t* lut_az;
    uint16_t* lut_rg;
    int32_t   lut_w, lut_h;
    int32_t   lut_cells, lut_bins;
    double    lut_range_max;
    double    lut_heading;
} UI_Ppi;

void ui_ppi_init(UI_Ppi* p);
void ui_ppi_release(UI_Ppi* p);

/** Computes the inscribed square and the centre from the panel rectangle. */
void ui_ppi_layout(UI_Ppi* p, UI_Rect frame);

/** Wheel zooms the range scale, left click selects the nearest track, and
 *  hovering updates the bearing and range readout.  Returns 1 when the
 *  selection changed. */
int  ui_ppi_input(UI_Context* c, UI_Ppi* p, uint32_t id, const PWR_Frame* f);

/** Paints the whole scope.  @p f may be NULL before the first frame arrives. */
void ui_ppi_draw(UI_Canvas* cv, UI_Ppi* p, const PWR_Frame* f);

/** Symbol colour for a track state, shared with the track table so the two
 *  displays never disagree. */
UI_Color ui_track_colour(int32_t state);

#endif /* PWRADAR_UI_PPI_H */
