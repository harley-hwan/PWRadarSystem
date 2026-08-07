/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_app.h
 *  Purpose : The verification console itself: layout, the control panel, the
 *            four displays and the binding to PWRadarCore.
 *  Language: ISO C17
 * ========================================================================== */
#ifndef PWRADAR_UI_APP_H
#define PWRADAR_UI_APP_H

#include "pwradar/pwr_api.h"

#include "ui_plot.h"
#include "ui_ppi.h"
#include "ui_widget.h"

#define APP_LOG_LINES   64
#define APP_LOG_CAP     140

typedef enum App_CtrlTab
{
    APP_TAB_WAVEFORM = 0,
    APP_TAB_PROCESS,
    APP_TAB_DETECT,
    APP_TAB_TRACK,
    APP_TAB_SCENARIO,
    APP_TAB_DISPLAY,
    APP_TAB_COUNT
} App_CtrlTab;

typedef enum App_RightTab
{
    APP_RT_TRACKS = 0,
    APP_RT_PLOTS,
    APP_RT_TARGETS,
    APP_RT_LOG,
    APP_RT_COUNT
} App_RightTab;

typedef enum App_LowerRight
{
    APP_LR_RTI = 0,
    APP_LR_SPECTRUM,
    APP_LR_COUNT
} App_LowerRight;

typedef struct App
{
    /* ---- platform and UI ------------------------------------------------- */
    UI_Platform*        plat;
    UI_Context          ui;
    int                 running;
    int                 need_redraw;

    /* ---- engine --------------------------------------------------------- */
    PWR_Engine*         eng;
    PWR_RadarConfig     cfg;            /* the operator's editable copy       */
    PWR_SimEnvironment  env;
    PWR_DerivedMetrics  dm;
    PWR_Stats           stats;
    PWR_Frame           frame;
    int                 have_frame;
    int                 cfg_dirty;
    int                 env_dirty;
    char                cfg_err[PWR_ERRMSG_LEN];

    /* ---- layout fractions (draggable) ----------------------------------- */
    double              f_left;         /* control panel width               */
    double              f_right;        /* table panel width                 */
    double              f_centre_v;     /* PPI column vs plot column         */
    double              f_centre_h;     /* upper vs lower row of the plots   */
    double              f_right_h;      /* table vs readout in the right pane */

    /* ---- displays ------------------------------------------------------- */
    UI_Ppi              ppi;
    UI_Axes             ax_scope;       /* A-scope: range profile + threshold */
    UI_Axes             ax_rd;          /* range-Doppler imagesc              */
    UI_Axes             ax_rti;         /* range-time intensity waterfall     */
    UI_Axes             ax_spec;        /* Doppler spectrum at the cursor gate */

    int32_t             lower_right;    /* App_LowerRight                     */
    int32_t             cmap_rd, cmap_rti;
    double              clim_rd_lo, clim_rd_hi;
    double              clim_rti_lo, clim_rti_hi;
    int32_t             auto_clim;
    int32_t             smooth_rd;
    int32_t             show_raw_video;
    int32_t             show_threshold;

    /* ---- unrolled RTI ring ---------------------------------------------- */
    float*              rti_view;
    uint32_t            rti_rows, rti_cols;

    /* ---- panels --------------------------------------------------------- */
    int32_t             tab_ctrl;
    int32_t             tab_right;
    int32_t             scenario;
    UI_TableState       tbl_tracks;
    UI_TableState       tbl_plots;
    UI_TableState       tbl_targets;
    int32_t             cursor_bin_ui;
    int32_t             show_help;

    /* ---- diagnostics ---------------------------------------------------- */
    char                log[APP_LOG_LINES][APP_LOG_CAP];
    int32_t             log_head, log_count;
    double              fps;
    double              last_frame_s;
    char                selftest[2048];
    PWR_Status          selftest_status;
} App;

/** Creates the window, the engine and every view.  Returns 0 on failure with a
 *  reason written to @p err. */
int  app_create(App* a, char* err, size_t err_cap);
void app_destroy(App* a);

/** Pumps input, advances the display and presents one frame.  Returns 0 when
 *  the operator has closed the console. */
int  app_step(App* a);

/** Writes the current framebuffer as a binary PPM.  Used by the --capture mode
 *  so a headless build can produce a reference image for a render regression
 *  test without linking an image library. */
int  app_write_ppm(const App* a, const char* path);

#endif /* PWRADAR_UI_APP_H */
