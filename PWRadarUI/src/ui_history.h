/* Presentation-side history: full track and truth paths, and a scan-persistent
 * plot memory.
 *
 * The engine publishes only the live picture plus a short symbol trail -
 * long-term history is display state, so it is accumulated here from frames the
 * console has already acquired. Path points are coalesced on entry (minimum
 * spacing in metres) and aged out against a retention window, so hours of
 * scenario fit in a few hundred kilobytes and the draw cost stays bounded.
 */
#ifndef PWRADAR_UI_HISTORY_H
#define PWRADAR_UI_HISTORY_H

#include <stdint.h>

/* Per-path point ceiling: beyond this the oldest quarter is dropped. */
#define UI_PATH_MAX_POINTS   8192
/* Plot memory ceiling across all scans. */
#define UI_PLOT_HIST_CAP     16384

typedef struct UI_PathPoint
{
    float  x_m, y_m;            /* ENU metres                                 */
    double t_s;                 /* scenario time the point was recorded       */
} UI_PathPoint;

typedef struct UI_Path
{
    int32_t       key;          /* track id or truth id                       */
    int32_t       cls;          /* PWR_TargetClass, for symbology             */
    int32_t       state;        /* last PWR_TrackState (tracks only)          */
    int32_t       alive;        /* present in the latest frame                */
    int32_t       count, cap;
    UI_PathPoint* pts;          /* chronological, oldest first                */
} UI_Path;

typedef struct UI_PathSet
{
    UI_Path* paths;
    int32_t  count, cap;
} UI_PathSet;

typedef struct UI_PlotMark
{
    float   x_m, y_m;           /* ENU metres                                 */
    double  t_s;
    float   snr_db;
    int32_t assoc_track_id;     /* 0 == went unassociated                     */
} UI_PlotMark;

typedef struct UI_PlotHistory
{
    UI_PlotMark* marks;         /* chronological, oldest first                */
    int32_t      count, cap;
} UI_PlotHistory;

/* --------------------------------------------------------------------------
 *  Path sets
 * ------------------------------------------------------------------------ */
void ui_pathset_init(UI_PathSet* s);
void ui_pathset_release(UI_PathSet* s);
void ui_pathset_clear(UI_PathSet* s);

/** Marks every path dead.  Call once per new frame before feeding points;
 *  paths touched by ui_pathset_point() are revived. */
void ui_pathset_begin_frame(UI_PathSet* s);

/** Appends (x, y) at time t to the path of @p key, creating the path on
 *  first use.  A point closer than @p min_step_m to the stored tip refreshes
 *  the tip in place instead of growing the path, so a stationary target
 *  stays a single, always-fresh point. */
void ui_pathset_point(UI_PathSet* s, int32_t key, int32_t cls, int32_t state,
                      float x_m, float y_m, double t_s, double min_step_m);

/** Drops points older than now - retain_s and removes dead, emptied paths. */
void ui_pathset_prune(UI_PathSet* s, double now_s, double retain_s);

/* --------------------------------------------------------------------------
 *  Plot memory
 * ------------------------------------------------------------------------ */
void ui_plot_history_init(UI_PlotHistory* h);
void ui_plot_history_release(UI_PlotHistory* h);
void ui_plot_history_clear(UI_PlotHistory* h);
void ui_plot_history_push(UI_PlotHistory* h, float x_m, float y_m, double t_s,
                          float snr_db, int32_t assoc_track_id);
void ui_plot_history_prune(UI_PlotHistory* h, double now_s, double retain_s);

#endif /* PWRADAR_UI_HISTORY_H */
