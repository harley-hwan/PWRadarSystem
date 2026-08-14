/* The verification console.
 *
 *   +----------------------------------------------------------------+
 *   | toolbar: run control, scenario, scan state, throughput          |
 *   +---------+--------------------------+---------------------------+
 *   | control |  PPI scope   | R-D map   |  track table              |
 *   | panel   |--------------+-----------|                           |
 *   | (tabs)  |  A-scope     | RTI/spec  |  readouts                 |
 *   +---------+--------------------------+---------------------------+
 *   | status: cursor readout, stage timings, load factor              |
 *   +----------------------------------------------------------------+
 *
 * Every boundary is a draggable splitter and every display has its own zoom,
 * pan and data cursor, so whichever product is under investigation can be
 * enlarged without losing the others.
 *
 * Controls edit a local copy of PWR_RadarConfig and raise a dirty flag; the
 * copy is pushed to the engine once, at the end of the frame. That keeps a
 * slider drag from triggering one reconfiguration per pixel of travel, and it
 * means a rejected configuration can be reported without ever leaving the
 * engine half-applied.
 */
#include "ui_app.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 *  Small helpers
 * ========================================================================== */
static double app_max(double a, double b) { return (a > b) ? a : b; }
static int32_t app_maxi(int32_t a, int32_t b) { return (a > b) ? a : b; }

static void app_logf(App* a, const char* fmt, ...)
{
    va_list ap;
    const int32_t slot = (a->log_head + a->log_count) % APP_LOG_LINES;
    va_start(ap, fmt);
    (void)vsnprintf(a->log[slot], APP_LOG_CAP, fmt, ap);
    va_end(ap);
    if (a->log_count < APP_LOG_LINES) { ++a->log_count; }
    else { a->log_head = (a->log_head + 1) % APP_LOG_LINES; }
}

static void app_core_log(void* user, int32_t level, const char* msg)
{
    static const char* const tag[5] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };
    App* a = (App*)user;
    const int32_t k = (level >= 0 && level < 5) ? level : 2;
    app_logf(a, "[%s] %s", tag[k], msg);
}

/* Item callbacks for the drop-downs. */
static const char* app_item_window(void* u, int32_t i)
{
    (void)u; return pwr_window_name((PWR_WindowType)i);
}
static const char* app_item_mti(void* u, int32_t i)
{
    (void)u; return pwr_mti_name((PWR_MtiMode)i);
}
static const char* app_item_cfar(void* u, int32_t i)
{
    (void)u; return pwr_cfar_name((PWR_CfarType)i);
}
static const char* app_item_assoc(void* u, int32_t i)
{
    static const char* const n[PWR_ASSOC_COUNT] = {
        "Nearest neighbour", "Global (JV) nearest"
    };
    (void)u;
    return (i >= 0 && i < PWR_ASSOC_COUNT) ? n[i] : "?";
}
static const char* app_item_scenario(void* u, int32_t i)
{
    const char* s;
    (void)u;
    s = pwr_scenario_name((uint32_t)i);
    return (s != NULL) ? s : "";
}
static const char* app_item_cmap(void* u, int32_t i)
{
    (void)u; return ui_colormap_name((UI_ColormapId)i);
}
static const char* app_item_swerling(void* u, int32_t i)
{
    (void)u; return pwr_swerling_name((PWR_Swerling)i);
}

static uint32_t app_scenario_count(void)
{
    uint32_t n = 0u;
    while (pwr_scenario_name(n) != NULL) { ++n; }
    return n;
}

/* ==========================================================================
 *  Display history and truth-versus-track scoring
 * ========================================================================== */
static void app_history_clear(App* a)
{
    ui_pathset_clear(&a->track_paths);
    ui_pathset_clear(&a->truth_paths);
    ui_plot_history_clear(&a->plot_hist);
    (void)pwr_scorer_init(&a->verify, 0.0);
    a->ver_row_count    = 0;
    a->hist_last_time_s = 0.0;
}

/* Feeds one newly published frame into the display history. */
static void app_history_feed(App* a)
{
    const PWR_Frame* f = &a->frame;
    const double now = f->time_s;
    const double min_step = 25.0;       /* path decimation, metres */
    uint32_t i;

    /* Scenario time moved backwards: the engine was reset under us.  The
     * scorer detects the same seam for itself, but the drawn history is the
     * console's own and has to be dropped here. */
    if (now + 1e-6 < a->hist_last_time_s) { app_history_clear(a); }

    ui_pathset_begin_frame(&a->track_paths);
    ui_pathset_begin_frame(&a->truth_paths);
    for (i = 0u; i < f->track_count; ++i)
    {
        const PWR_Track* t = &f->tracks[i];
        ui_pathset_point(&a->track_paths, (int32_t)t->id, t->target_class,
                         t->state, (float)t->x_m, (float)t->y_m, now, min_step);
    }
    for (i = 0u; i < f->truth_count; ++i)
    {
        const PWR_SimTarget* t = &f->truth[i];
        if (t->enabled == 0) { continue; }
        ui_pathset_point(&a->truth_paths, t->id, t->target_class, 0,
                         (float)t->x_m, (float)t->y_m, now, min_step);
    }
    ui_pathset_prune(&a->track_paths, now, a->hist_retain_s);
    ui_pathset_prune(&a->truth_paths, now, a->hist_retain_s);

    for (i = 0u; i < f->detection_count; ++i)
    {
        const PWR_Detection* d = &f->detections[i];
        const double az = d->azimuth_deg * UI_PI / 180.0;
        ui_plot_history_push(&a->plot_hist,
                             (float)(d->range_m * sin(az)),
                             (float)(d->range_m * cos(az)),
                             now, (float)d->snr_db, d->assoc_track_id);
    }
    ui_plot_history_prune(&a->plot_hist, now, a->hist_retain_s);

    (void)pwr_scorer_update(&a->verify, f);
    a->hist_last_time_s = now;
}

/* ==========================================================================
 *  Life cycle
 * ========================================================================== */
int app_create(App* a, char* err, size_t err_cap)
{
    PWR_Status st;

    memset(a, 0, sizeof(*a));
    ui_colormap_init();

    if (pwr_abi_version() != (uint32_t)PWR_ABI_VERSION)
    {
        (void)snprintf(err, err_cap,
                       "PWRadarCore ABI mismatch: built against %d, loaded %u",
                       PWR_ABI_VERSION, pwr_abi_version());
        return 0;
    }

    a->plat = ui_plat_create("PWRadarSystem  -  PW Radar Detection Console",
                             1680, 980, err, err_cap);
    if (a->plat == NULL) { return 0; }

    ui_ctx_init(&a->ui);
    a->running     = 1;
    a->need_redraw = 1;

    /* ---- engine --------------------------------------------------------- */
    (void)pwr_config_default(&a->cfg);
    st = pwr_engine_create(&a->cfg, &a->eng);
    if (st != PWR_STATUS_OK)
    {
        (void)snprintf(err, err_cap, "engine creation failed: %s",
                       pwr_status_string(st));
        ui_plat_destroy(a->plat);
        a->plat = NULL;
        return 0;
    }
    (void)pwr_engine_set_log(a->eng, app_core_log, a, PWR_LOG_INFO);
    (void)pwr_engine_get_config(a->eng, &a->cfg);
    (void)pwr_engine_get_metrics(a->eng, &a->dm);
    (void)pwr_engine_get_environment(a->eng, &a->env);

    app_logf(a, "[INFO ] PWRadarCore %s, ABI %u", pwr_version_string(),
             pwr_abi_version());
    a->selftest_status = pwr_self_test(a->selftest, sizeof(a->selftest));
    app_logf(a, "[INFO ] self test: %s",
             (a->selftest_status == PWR_STATUS_OK) ? "all cases passed" : "FAILURES");

    a->scenario = 3;                    /* mixed air and surface picture */
    (void)pwr_engine_load_scenario(a->eng, (uint32_t)a->scenario);
    (void)pwr_engine_get_environment(a->eng, &a->env);
    (void)pwr_engine_start(a->eng);

    /* ---- layout --------------------------------------------------------- */
    a->f_left     = 0.185;
    a->f_right    = 0.235;
    a->f_centre_v = 0.50;
    a->f_centre_h = 0.56;
    a->f_right_h  = 0.62;

    /* ---- displays ------------------------------------------------------- */
    ui_ppi_init(&a->ppi);
    a->ppi.range_max_m      = a->dm.range_bin_spacing_m * (double)a->dm.range_bins;
    a->ppi.range_max_full_m = a->ppi.range_max_m;
    a->ppi.video_gain_db    = 5.0;

    /* ---- display history and verification ------------------------------- */
    ui_pathset_init(&a->track_paths);
    ui_pathset_init(&a->truth_paths);
    ui_plot_history_init(&a->plot_hist);
    a->hist_retain_s   = 300.0;
    a->ppi.track_paths = &a->track_paths;
    a->ppi.truth_paths = &a->truth_paths;
    a->ppi.plot_hist   = &a->plot_hist;
    app_history_clear(a);

    ui_axes_init(&a->ax_scope, "A-scope   range profile",
                 "range [km]", "SNR [dB]");
    ui_axes_set_full(&a->ax_scope, 0.0, a->ppi.range_max_m * 1e-3, -10.0, 90.0);
    (void)snprintf(a->ax_scope.xunit, sizeof(a->ax_scope.xunit), "km");
    (void)snprintf(a->ax_scope.yunit, sizeof(a->ax_scope.yunit), "dB");
    a->ax_scope.grid = 2;

    ui_axes_init(&a->ax_rd, "Range-Doppler map",
                 "range [km]", "range rate [m/s]");
    ui_axes_set_full(&a->ax_rd, 0.0, a->ppi.range_max_m * 1e-3,
                     -a->dm.unambiguous_velocity_mps,
                      a->dm.unambiguous_velocity_mps);
    (void)snprintf(a->ax_rd.xunit, sizeof(a->ax_rd.xunit), "km");
    (void)snprintf(a->ax_rd.yunit, sizeof(a->ax_rd.yunit), "m/s");
    a->ax_rd.grid = 0;

    ui_axes_init(&a->ax_rti, "RTI waterfall", "range [km]", "age [s]");
    ui_axes_set_full(&a->ax_rti, 0.0, a->ppi.range_max_m * 1e-3, 0.0, 1.0);
    (void)snprintf(a->ax_rti.xunit, sizeof(a->ax_rti.xunit), "km");
    (void)snprintf(a->ax_rti.yunit, sizeof(a->ax_rti.yunit), "s");
    a->ax_rti.grid = 0;

    ui_axes_init(&a->ax_spec, "Doppler spectrum", "range rate [m/s]", "SNR [dB]");
    ui_axes_set_full(&a->ax_spec, -a->dm.unambiguous_velocity_mps,
                     a->dm.unambiguous_velocity_mps, -10.0, 90.0);
    (void)snprintf(a->ax_spec.xunit, sizeof(a->ax_spec.xunit), "m/s");
    (void)snprintf(a->ax_spec.yunit, sizeof(a->ax_spec.yunit), "dB");
    a->ax_spec.grid = 2;

    a->cmap_rd       = UI_CMAP_PARULA;
    a->cmap_rti      = UI_CMAP_TURBO;
    a->clim_rd_lo    = -6.0;
    a->clim_rd_hi    = 54.0;
    a->clim_rti_lo   = 6.0;
    a->clim_rti_hi   = 50.0;
    a->auto_clim     = 1;
    a->smooth_rd     = 0;
    a->show_raw_video = 1;
    a->show_threshold = 1;
    a->lower_right   = APP_LR_RTI;

    a->tbl_tracks.row_h  = 20;
    a->tbl_tracks.selected = -1;
    a->tbl_plots.row_h   = 20;
    a->tbl_plots.selected = -1;
    a->tbl_targets.row_h = 20;
    a->tbl_targets.selected = -1;
    a->tbl_verify.row_h  = 20;
    a->tbl_verify.selected = -1;
    a->cursor_bin_ui = (int32_t)(a->dm.range_bins / 2u);

    return 1;
}

void app_destroy(App* a)
{
    if (a->have_frame != 0) { (void)pwr_engine_frame_release(a->eng); }
    if (a->eng != NULL) { pwr_engine_destroy(a->eng); a->eng = NULL; }
    ui_ppi_release(&a->ppi);
    ui_pathset_release(&a->track_paths);
    ui_pathset_release(&a->truth_paths);
    ui_plot_history_release(&a->plot_hist);
    free(a->rti_view);
    a->rti_view = NULL;
    if (a->plat != NULL) { ui_plat_destroy(a->plat); a->plat = NULL; }
}

/* ==========================================================================
 *  Toolbar
 * ========================================================================== */
static void app_draw_toolbar(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    const PWR_RunState rs = pwr_engine_run_state(a->eng);
    int32_t x = r.x + UI_M_PAD;
    const int32_t by = r.y + (r.h - 26) / 2;
    char buf[128];

    ui_fill_gradient_v(&c->canvas, r, UI_C_PANEL_HI, UI_C_PANEL);
    ui_hline(&c->canvas, r.x, r.x + r.w - 1, r.y + r.h - 1, UI_C_BORDER);

    /* ---- run control ---------------------------------------------------- */
    if (rs == PWR_RUN_RUNNING)
    {
        if (ui_button_accent(c, ui_id("tb.pause", 0), ui_rect(x, by, 74, 26),
                             "\xE2\x96\x8C\xE2\x96\x8C  PAUSE", UI_C_WARN) != 0)
        {
            (void)pwr_engine_pause(a->eng);
        }
    }
    else
    {
        if (ui_button_accent(c, ui_id("tb.run", 0), ui_rect(x, by, 74, 26),
                             "\xE2\x96\xB6  RUN", UI_C_OK) != 0)
        {
            (void)pwr_engine_start(a->eng);
        }
    }
    x += 80;
    if (ui_button(c, ui_id("tb.step", 0), ui_rect(x, by, 58, 26), "STEP") != 0)
    {
        if (rs == PWR_RUN_RUNNING) { (void)pwr_engine_pause(a->eng); }
        a->step_pending += 1;
    }
    x += 64;
    if (ui_button(c, ui_id("tb.scan", 0), ui_rect(x, by, 66, 26), "+1 SCAN") != 0)
    {
        const uint32_t n = (a->dm.cpi_duration_s > 0.0 && a->dm.scan_period_s > 0.0)
            ? (uint32_t)(a->dm.scan_period_s / a->dm.cpi_duration_s) : 32u;
        if (rs == PWR_RUN_RUNNING) { (void)pwr_engine_pause(a->eng); }
        a->step_pending += (int32_t)n;
    }
    x += 72;
    if (ui_button(c, ui_id("tb.reset", 0), ui_rect(x, by, 60, 26), "RESET") != 0)
    {
        (void)pwr_engine_reset(a->eng);
        app_history_clear(a);
    }
    x += 74;

    /* ---- scenario ------------------------------------------------------- */
    {
        const int32_t w = 250;
        if (ui_combo(c, ui_id("tb.scen", 0), ui_rect(x, by, w, 26), NULL,
                     &a->scenario, (int32_t)app_scenario_count(),
                     app_item_scenario, NULL) != 0)
        {
            (void)pwr_engine_load_scenario(a->eng, (uint32_t)a->scenario);
            (void)pwr_engine_get_environment(a->eng, &a->env);
            app_history_clear(a);
        }
        x += w + 12;
    }

    /* ---- time scale ----------------------------------------------------- */
    {
        double ts = a->cfg.time_scale;
        if (ui_slider(c, ui_id("tb.ts", 0), ui_rect(x, by, 250, 26), "speed",
                      &ts, 0.05, 8.0, "%.2fx", 1) != 0)
        {
            a->cfg.time_scale = ts;
            (void)pwr_engine_set_time_scale(a->eng, ts);
        }
        x += 258;
    }

    /* ---- state and throughput ------------------------------------------- */
    {
        const char* sname = (rs == PWR_RUN_RUNNING) ? "RUNNING"
                          : ((rs == PWR_RUN_PAUSED) ? "PAUSED"
                          : ((rs == PWR_RUN_FAULTED) ? "FAULT" : "STOPPED"));
        const UI_Color scol = (rs == PWR_RUN_RUNNING) ? UI_C_OK
                            : ((rs == PWR_RUN_FAULTED) ? UI_C_ALARM : UI_C_WARN);
        ui_fill_circle(&c->canvas, (double)x + 7.0, (double)r.y + (double)r.h * 0.5,
                       5.0, scol);
        (void)ui_text(&c->canvas, UI_FONT_BOLD, x + 18,
                      r.y + (r.h + ui_font_ascent(UI_FONT_BOLD)) / 2 - 1,
                      scol, sname);
        x += 18 + ui_text_width(UI_FONT_BOLD, sname) + 18;

        (void)snprintf(buf, sizeof(buf),
                       "CPI %llu   scan %llu   %.1f CPI/s   load %.2f   UI %.0f fps",
                       (unsigned long long)a->stats.cpi_count,
                       (unsigned long long)a->stats.scan_count,
                       a->stats.cpi_rate_hz, a->stats.load_factor, a->fps);
        (void)ui_text(&c->canvas, UI_FONT_MONO, x,
                      r.y + (r.h + ui_font_ascent(UI_FONT_MONO)) / 2 - 1,
                      (a->stats.load_factor > 1.05) ? UI_C_WARN : UI_C_TEXT_DIM,
                      buf);
    }

    /* ---- help: the overlay reads the flag, nothing further to do -------- */
    (void)ui_toggle(c, ui_id("tb.help", 0),
                    ui_rect(r.x + r.w - 40 - UI_M_PAD, by, 40, 26), "?",
                    &a->show_help);
}

/* ==========================================================================
 *  Control panel
 * ========================================================================== */
static void app_panel_waveform(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Stack s;
    UI_Rect row, half[2];
    char buf[96];
    double v;

    ui_stack_begin(&s, r, 4);

    ui_label_dim(c, ui_stack_row(&s, 16), "TRANSMITTER / WAVEFORM");
    v = a->cfg.carrier_hz * 1e-9;
    if (ui_edit_double(c, ui_id("wf.fc", 0), ui_stack_row(&s, 24), "carrier [GHz]",
                       &v, 0.1, 100.0, "%.4f") != 0)
    { a->cfg.carrier_hz = v * 1e9; a->cfg_dirty = 1; }

    v = a->cfg.bandwidth_hz * 1e-6;
    if (ui_edit_double(c, ui_id("wf.b", 0), ui_stack_row(&s, 24), "bandwidth [MHz]",
                       &v, 0.05, 500.0, "%.3f") != 0)
    { a->cfg.bandwidth_hz = v * 1e6; a->cfg_dirty = 1; }

    v = a->cfg.pulse_width_s * 1e6;
    if (ui_slider(c, ui_id("wf.tp", 0), ui_stack_row(&s, 24), "pulse [\xC2\xB5s]",
                  &v, 0.5, 200.0, "%.1f", 1) != 0)
    { a->cfg.pulse_width_s = v * 1e-6; a->cfg_dirty = 1; }

    v = a->cfg.sample_rate_hz * 1e-6;
    if (ui_edit_double(c, ui_id("wf.fs", 0), ui_stack_row(&s, 24), "sample [MHz]",
                       &v, 0.05, 500.0, "%.3f") != 0)
    { a->cfg.sample_rate_hz = v * 1e6; a->cfg_dirty = 1; }

    v = a->cfg.prf_hz;
    if (ui_slider(c, ui_id("wf.prf", 0), ui_stack_row(&s, 24), "PRF [Hz]",
                  &v, 100.0, 20000.0, "%.0f", 1) != 0)
    { a->cfg.prf_hz = v; a->cfg_dirty = 1; }

    v = a->cfg.peak_power_w * 1e-3;
    if (ui_slider(c, ui_id("wf.pt", 0), ui_stack_row(&s, 24), "peak power [kW]",
                  &v, 0.1, 500.0, "%.1f", 1) != 0)
    { a->cfg.peak_power_w = v * 1e3; a->cfg_dirty = 1; }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "ANTENNA / SCAN");

    v = a->cfg.azimuth_beamwidth_deg;
    if (ui_slider(c, ui_id("wf.az", 0), ui_stack_row(&s, 24), "az BW [\xC2\xB0]",
                  &v, 0.3, 20.0, "%.2f", 0) != 0)
    { a->cfg.azimuth_beamwidth_deg = v; a->cfg_dirty = 1; }

    v = a->cfg.scan_rate_rpm;
    if (ui_slider(c, ui_id("wf.rpm", 0), ui_stack_row(&s, 24), "scan [rpm]",
                  &v, 0.0, 60.0, "%.1f", 0) != 0)
    {
        a->cfg.scan_rate_rpm = v;
        (void)pwr_engine_set_scan_rate(a->eng, v);
        (void)pwr_engine_get_metrics(a->eng, &a->dm);
    }

    v = a->cfg.tx_gain_db;
    if (ui_slider(c, ui_id("wf.g", 0), ui_stack_row(&s, 24), "gain [dBi]",
                  &v, 10.0, 55.0, "%.1f", 0) != 0)
    { a->cfg.tx_gain_db = v; a->cfg.rx_gain_db = v; a->cfg_dirty = 1; }

    v = a->cfg.system_loss_db;
    if (ui_slider(c, ui_id("wf.l", 0), ui_stack_row(&s, 24), "losses [dB]",
                  &v, 0.0, 25.0, "%.1f", 0) != 0)
    { a->cfg.system_loss_db = v; a->cfg_dirty = 1; }

    v = a->cfg.noise_figure_db;
    if (ui_slider(c, ui_id("wf.nf", 0), ui_stack_row(&s, 24), "noise fig [dB]",
                  &v, 0.5, 12.0, "%.1f", 0) != 0)
    { a->cfg.noise_figure_db = v; a->cfg_dirty = 1; }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "DERIVED");

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "%.3f m", a->dm.wavelength_m);
    ui_readout(c, row, "wavelength", buf, UI_C_TEXT);

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "%.1f m", a->dm.range_resolution_m);
    ui_readout(c, row, "range res", buf, UI_C_TEXT);

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "%.1f dB (TB %.0f)",
                   a->dm.pulse_compression_gain_db, a->dm.time_bandwidth_product);
    ui_readout(c, row, "compression", buf, UI_C_TEXT);

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "%.1f km", a->dm.unambiguous_range_m * 1e-3);
    ui_readout(c, row, "R unambig", buf,
               (a->dm.unambiguous_range_m < a->ppi.range_max_m) ? UI_C_WARN
                                                                : UI_C_TEXT);

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "\xC2\xB1%.1f m/s",
                   a->dm.unambiguous_velocity_mps);
    ui_readout(c, row, "V unambig", buf, UI_C_TEXT);

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "%.2f m/s", a->dm.velocity_resolution_mps);
    ui_readout(c, row, "V res", buf, UI_C_TEXT);

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "%.2f km", a->dm.blind_range_m * 1e-3);
    ui_readout(c, row, "blind range", buf, UI_C_TEXT);

    /* Pulses the antenna puts on a target while the mainlobe crosses it.  A CPI
     * longer than this spreads one coherent batch over more than a beamwidth,
     * so the advertised integration gain is not achieved and the azimuth
     * centroid has no beam-shape modulation left to split - the amber is the
     * only warning the operator gets that the geometry has left its envelope. */
    row = ui_stack_row(&s, 17);
    if (a->dm.scan_period_s > 0.0)
    {
        (void)snprintf(buf, sizeof(buf), "%.1f (CPI %u)",
                       a->dm.pulses_per_beamwidth, a->cfg.pulses_per_cpi);
        ui_readout(c, row, "pulses/beam", buf,
                   ((double)a->cfg.pulses_per_cpi > a->dm.pulses_per_beamwidth)
                       ? UI_C_WARN : UI_C_TEXT);
    }
    else
    {
        ui_readout(c, row, "pulses/beam", "staring", UI_C_TEXT);
    }

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "%.2f %%", a->dm.duty_cycle * 100.0);
    ui_readout(c, row, "duty cycle", buf,
               (a->dm.duty_cycle > a->cfg.duty_limit * 0.98) ? UI_C_WARN
                                                             : UI_C_TEXT);

    row = ui_stack_row(&s, 17);
    (void)snprintf(buf, sizeof(buf), "%.1f dBm", a->dm.noise_power_dbm);
    ui_readout(c, row, "noise power", buf, UI_C_TEXT);

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "DETECTION RANGE (13 dB SNR)");
    {
        static const double rcs[3] = { 0.1, 1.0, 100.0 };
        static const char* const nm[3] = { "0.1 m\xC2\xB2 UAV", "1 m\xC2\xB2 air",
                                           "100 m\xC2\xB2 ship" };
        int32_t k;
        ui_stack_row_split(&s, 0, 1, half);
        for (k = 0; k < 3; ++k)
        {
            row = ui_stack_row(&s, 17);
            (void)snprintf(buf, sizeof(buf), "%.1f km",
                           pwr_max_range_for_snr(&a->cfg, rcs[k], 13.0) * 1e-3);
            ui_readout(c, row, nm[k], buf, UI_C_SERIES_0);
        }
    }
}

static void app_panel_process(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Stack s;
    char buf[96];
    double v;
    int32_t iv;

    ui_stack_begin(&s, r, 4);
    ui_label_dim(c, ui_stack_row(&s, 16), "COHERENT PROCESSING");

    iv = (int32_t)a->cfg.pulses_per_cpi;
    {
        /* Power-of-two only, so the slider works in the exponent. */
        int32_t e = 1;
        while ((1 << e) < iv && e < 8) { ++e; }
        if (ui_slider_int(c, ui_id("pr.np", 0), ui_stack_row(&s, 24),
                          "pulses / CPI", &e, 1, 8, "%.0f") != 0)
        {
            a->cfg.pulses_per_cpi = (uint32_t)(1 << e);
            if (a->cfg.doppler_bins < a->cfg.pulses_per_cpi)
            {
                a->cfg.doppler_bins = a->cfg.pulses_per_cpi;
            }
            a->cfg_dirty = 1;
        }
        (void)snprintf(buf, sizeof(buf), "%u pulses, CPI %.2f ms",
                       a->cfg.pulses_per_cpi, a->dm.cpi_duration_s * 1e3);
        ui_label_dim(c, ui_stack_row(&s, 15), buf);
    }
    {
        int32_t e = 1;
        while ((1 << e) < (int32_t)a->cfg.doppler_bins && e < 9) { ++e; }
        if (ui_slider_int(c, ui_id("pr.nd", 0), ui_stack_row(&s, 24),
                          "Doppler FFT", &e, 1, 9, "%.0f") != 0)
        {
            a->cfg.doppler_bins = (uint32_t)(1 << e);
            if (a->cfg.doppler_bins < a->cfg.pulses_per_cpi)
            {
                a->cfg.doppler_bins = a->cfg.pulses_per_cpi;
            }
            a->cfg_dirty = 1;
        }
        (void)snprintf(buf, sizeof(buf), "%u bins", a->cfg.doppler_bins);
        ui_label_dim(c, ui_stack_row(&s, 15), buf);
    }

    iv = a->cfg.range_window;
    if (ui_combo(c, ui_id("pr.rw", 0), ui_stack_row(&s, 24), "range taper",
                 &iv, PWR_WIN_COUNT, app_item_window, NULL) != 0)
    { a->cfg.range_window = iv; a->cfg_dirty = 1; }

    iv = a->cfg.doppler_window;
    if (ui_combo(c, ui_id("pr.dw", 0), ui_stack_row(&s, 24), "Doppler taper",
                 &iv, PWR_WIN_COUNT, app_item_window, NULL) != 0)
    { a->cfg.doppler_window = iv; a->cfg_dirty = 1; }

    iv = a->cfg.mti_mode;
    if (ui_combo(c, ui_id("pr.mti", 0), ui_stack_row(&s, 24), "MTI",
                 &iv, PWR_MTI_COUNT, app_item_mti, NULL) != 0)
    { a->cfg.mti_mode = iv; a->cfg_dirty = 1; }

    if (ui_checkbox(c, ui_id("pr.pc", 0), ui_stack_row(&s, 22),
                    "pulse compression", &a->cfg.enable_pulse_compression) != 0)
    { a->cfg_dirty = 1; }
    if (ui_checkbox(c, ui_id("pr.dop", 0), ui_stack_row(&s, 22),
                    "Doppler processing", &a->cfg.enable_doppler_processing) != 0)
    { a->cfg_dirty = 1; }
    if (ui_checkbox(c, ui_id("pr.stc", 0), ui_stack_row(&s, 22),
                    "sensitivity time control", &a->cfg.enable_stc) != 0)
    {
        (void)pwr_engine_set_stc(a->eng, a->cfg.enable_stc, a->cfg.stc_range_m);
    }
    v = a->cfg.stc_range_m * 1e-3;
    if (ui_slider(c, ui_id("pr.stcr", 0), ui_stack_row(&s, 24), "STC range [km]",
                  &v, 0.5, 40.0, "%.1f", 0) != 0)
    {
        a->cfg.stc_range_m = v * 1e3;
        (void)pwr_engine_set_stc(a->eng, a->cfg.enable_stc, a->cfg.stc_range_m);
    }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "RANGE WINDOW");

    v = a->cfg.range_span_m * 1e-3;
    if (ui_slider(c, ui_id("pr.span", 0), ui_stack_row(&s, 24), "span [km]",
                  &v, 2.0, 200.0, "%.1f", 1) != 0)
    { a->cfg.range_span_m = v * 1e3; a->cfg_dirty = 1; }

    iv = (int32_t)a->cfg.range_bins;
    if (ui_slider_int(c, ui_id("pr.rb", 0), ui_stack_row(&s, 24), "range bins",
                      &iv, 64, 2048, "%.0f") != 0)
    { a->cfg.range_bins = (uint32_t)iv; a->cfg_dirty = 1; }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "STAGE TIMING (ms, EWMA)");
    {
        static const char* const nm[7] = { "simulate", "compress", "MTI",
                                           "Doppler", "CFAR", "cluster", "track" };
        const double t[7] = { a->stats.t_simulate_ms, a->stats.t_pulse_compress_ms,
                              a->stats.t_mti_ms, a->stats.t_doppler_ms,
                              a->stats.t_cfar_ms, a->stats.t_cluster_ms,
                              a->stats.t_track_ms };
        const double budget = app_max(a->stats.t_total_ms, 0.01);
        int32_t k;
        for (k = 0; k < 7; ++k)
        {
            const UI_Rect row = ui_stack_row(&s, 17);
            const int32_t bw = (int32_t)((double)(row.w - 96) * t[k] / budget);
            ui_fill_rect(&c->canvas,
                         ui_rect(row.x + 96, row.y + 5, app_maxi(bw, 1), 7),
                         ui_colormap_sample((UI_ColormapId)UI_CMAP_TURBO,
                                            (double)k / 6.0));
            (void)snprintf(buf, sizeof(buf), "%.2f", t[k]);
            (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL,
                                  ui_rect(row.x, row.y, 60, row.h), 0,
                                  UI_C_TEXT_DIM, UI_ALIGN_LEFT, nm[k]);
            (void)ui_text_in_rect(&c->canvas, UI_FONT_MONO,
                                  ui_rect(row.x + 56, row.y, 36, row.h), 0,
                                  UI_C_TEXT, UI_ALIGN_RIGHT, buf);
        }
        {
            const UI_Rect row = ui_stack_row(&s, 18);
            (void)snprintf(buf, sizeof(buf), "%.2f ms / %.2f ms",
                           a->stats.t_total_ms, a->dm.cpi_duration_s * 1e3);
            ui_readout(c, row, "total / budget", buf,
                       (a->stats.load_factor > 1.0) ? UI_C_WARN : UI_C_OK);
        }
    }
}

static void app_panel_detect(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Stack s;
    char buf[96];
    double v;
    int32_t iv;
    int cfar_dirty = 0;

    ui_stack_begin(&s, r, 4);
    ui_label_dim(c, ui_stack_row(&s, 16), "CFAR DETECTOR");

    iv = a->cfg.cfar.type;
    if (ui_combo(c, ui_id("cf.type", 0), ui_stack_row(&s, 24), "estimator",
                 &iv, PWR_CFAR_COUNT, app_item_cfar, NULL) != 0)
    { a->cfg.cfar.type = iv; cfar_dirty = 1; }

    v = a->cfg.cfar.pfa;
    if (ui_slider(c, ui_id("cf.pfa", 0), ui_stack_row(&s, 24), "Pfa",
                  &v, 1e-10, 1e-2, "%.1e", 1) != 0)
    { a->cfg.cfar.pfa = v; cfar_dirty = 1; }

    v = a->cfg.cfar.extra_threshold_db;
    if (ui_slider(c, ui_id("cf.bias", 0), ui_stack_row(&s, 24), "bias [dB]",
                  &v, -10.0, 20.0, "%+.1f", 0) != 0)
    { a->cfg.cfar.extra_threshold_db = v; cfar_dirty = 1; }

    /* guard R / train R size the CFAR scratch buffers, so they go through the
     * deferred reconfigure path (applied once on mouse release) rather than
     * the per-frame hot path, which would rebuild the engine on every pixel
     * of slider travel. */
    iv = a->cfg.cfar.guard_range;
    if (ui_slider_int(c, ui_id("cf.gr", 0), ui_stack_row(&s, 24), "guard R",
                      &iv, 0, 16, "%.0f") != 0)
    { a->cfg.cfar.guard_range = iv; a->cfg_dirty = 1; }

    iv = a->cfg.cfar.train_range;
    if (ui_slider_int(c, ui_id("cf.tr", 0), ui_stack_row(&s, 24), "train R",
                      &iv, 2, 48, "%.0f") != 0)
    { a->cfg.cfar.train_range = iv; a->cfg_dirty = 1; }

    iv = a->cfg.cfar.guard_doppler;
    if (ui_slider_int(c, ui_id("cf.gd", 0), ui_stack_row(&s, 24), "guard D",
                      &iv, 0, 8, "%.0f") != 0)
    { a->cfg.cfar.guard_doppler = iv; cfar_dirty = 1; }

    iv = a->cfg.cfar.train_doppler;
    if (ui_slider_int(c, ui_id("cf.td", 0), ui_stack_row(&s, 24), "train D",
                      &iv, 0, 12, "%.0f") != 0)
    { a->cfg.cfar.train_doppler = iv; cfar_dirty = 1; }

    iv = a->cfg.cfar.os_rank;
    if (ui_slider_int(c, ui_id("cf.os", 0), ui_stack_row(&s, 24),
                      "OS rank (0=auto)", &iv, 0, 48, "%.0f") != 0)
    { a->cfg.cfar.os_rank = iv; cfar_dirty = 1; }

    if (ui_checkbox(c, ui_id("cf.zd", 0), ui_stack_row(&s, 22),
                    "censor zero Doppler", &a->cfg.cfar.censor_zero_doppler) != 0)
    { cfar_dirty = 1; }
    if (ui_checkbox(c, ui_id("cf.pk", 0), ui_stack_row(&s, 22),
                    "peak selection", &a->cfg.cfar.peak_selection) != 0)
    { cfar_dirty = 1; }

    if (cfar_dirty != 0) { (void)pwr_engine_set_cfar(a->eng, &a->cfg.cfar); }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "PLOT EXTRACTION");
    {
        int cl_dirty = 0;
        if (ui_checkbox(c, ui_id("cl.en", 0), ui_stack_row(&s, 22),
                        "cluster contiguous cells", &a->cfg.cluster.enable) != 0)
        { cl_dirty = 1; }
        iv = a->cfg.cluster.range_tolerance;
        if (ui_slider_int(c, ui_id("cl.tr", 0), ui_stack_row(&s, 24), "tol R",
                          &iv, 0, 8, "%.0f") != 0)
        { a->cfg.cluster.range_tolerance = iv; cl_dirty = 1; }
        iv = a->cfg.cluster.doppler_tolerance;
        if (ui_slider_int(c, ui_id("cl.td", 0), ui_stack_row(&s, 24), "tol D",
                          &iv, 0, 8, "%.0f") != 0)
        { a->cfg.cluster.doppler_tolerance = iv; cl_dirty = 1; }
        v = a->cfg.cluster.min_snr_db;
        if (ui_slider(c, ui_id("cl.snr", 0), ui_stack_row(&s, 24), "min SNR [dB]",
                      &v, 0.0, 30.0, "%.1f", 0) != 0)
        { a->cfg.cluster.min_snr_db = v; cl_dirty = 1; }
        if (cl_dirty != 0) { (void)pwr_engine_set_cluster(a->eng, &a->cfg.cluster); }
    }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "MEASURED");
    {
        UI_Rect row = ui_stack_row(&s, 17);
        (void)snprintf(buf, sizeof(buf), "%.2f dB", a->stats.measured_noise_floor_db);
        ui_readout(c, row, "noise floor", buf, UI_C_TEXT);

        row = ui_stack_row(&s, 17);
        (void)snprintf(buf, sizeof(buf), "%.2e", a->stats.measured_pfa);
        ui_readout(c, row, "achieved Pfa", buf,
                   (a->stats.measured_pfa > a->cfg.cfar.pfa * 8.0) ? UI_C_WARN
                                                                   : UI_C_OK);

        /* Pfa per cell is what the detector is designed to; false plots per
         * scan is what the operator actually sees, so show both.  Only cells
         * that receive a detection test can raise a false plot: the
         * zero-Doppler censor removes 2*guard+1 rows (clamped to the map)
         * from every CPI, the same convention the achieved-Pfa denominator
         * uses. */
        row = ui_stack_row(&s, 17);
        {
            double dop_tested = (double)a->dm.doppler_bins;
            double cells_per_cpi, cpi_per_scan;
            if (a->cfg.cfar.censor_zero_doppler != 0)
            {
                const double censored =
                    2.0 * (double)a->cfg.cfar.zero_doppler_guard + 1.0;
                dop_tested = (censored < dop_tested) ? (dop_tested - censored)
                                                     : 0.0;
            }
            cells_per_cpi = (double)a->dm.range_bins * dop_tested;
            cpi_per_scan = (a->dm.cpi_duration_s > 0.0 &&
                            a->dm.scan_period_s > 0.0)
                ? (a->dm.scan_period_s / a->dm.cpi_duration_s) : 1.0;
            (void)snprintf(buf, sizeof(buf), "%.2f",
                           a->cfg.cfar.pfa * cells_per_cpi * cpi_per_scan);
            ui_readout(c, row, "design plots / scan", buf, UI_C_TEXT);
        }
        row = ui_stack_row(&s, 17);
        (void)snprintf(buf, sizeof(buf), "%u", a->stats.detections_current);
        ui_readout(c, row, "plots this CPI", buf, UI_C_TEXT);

        row = ui_stack_row(&s, 17);
        (void)snprintf(buf, sizeof(buf), "%llu",
                       (unsigned long long)a->stats.cells_tested_total);
        ui_readout(c, row, "cells tested", buf, UI_C_TEXT_DIM);
    }
}

static void app_panel_track(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Stack s;
    char buf[96];
    double v;
    int32_t iv;
    int dirty = 0;

    ui_stack_begin(&s, r, 4);
    ui_label_dim(c, ui_stack_row(&s, 16), "TRACKER");

    if (ui_checkbox(c, ui_id("tk.en", 0), ui_stack_row(&s, 22),
                    "tracking enabled", &a->cfg.tracker.enable) != 0)
    { dirty = 1; }

    iv = a->cfg.tracker.assoc_mode;
    if (ui_combo(c, ui_id("tk.as", 0), ui_stack_row(&s, 24), "association",
                 &iv, PWR_ASSOC_COUNT, app_item_assoc, NULL) != 0)
    { a->cfg.tracker.assoc_mode = iv; dirty = 1; }

    v = a->cfg.tracker.process_noise_accel;
    if (ui_slider(c, ui_id("tk.q", 0), ui_stack_row(&s, 24),
                  "\xCF\x83 accel [m/s\xC2\xB2]", &v, 0.1, 60.0, "%.2f", 1) != 0)
    { a->cfg.tracker.process_noise_accel = v; dirty = 1; }

    v = a->cfg.tracker.meas_sigma_range_m;
    if (ui_slider(c, ui_id("tk.sr", 0), ui_stack_row(&s, 24), "\xCF\x83 range [m]",
                  &v, 1.0, 300.0, "%.1f", 1) != 0)
    { a->cfg.tracker.meas_sigma_range_m = v; dirty = 1; }

    v = a->cfg.tracker.meas_sigma_azimuth_deg;
    if (ui_slider(c, ui_id("tk.sa", 0), ui_stack_row(&s, 24), "\xCF\x83 az [\xC2\xB0]",
                  &v, 0.02, 5.0, "%.3f", 1) != 0)
    { a->cfg.tracker.meas_sigma_azimuth_deg = v; dirty = 1; }

    v = a->cfg.tracker.gate_sigma;
    if (ui_slider(c, ui_id("tk.gs", 0), ui_stack_row(&s, 24), "gate [\xCF\x83]",
                  &v, 1.0, 10.0, "%.2f", 0) != 0)
    { a->cfg.tracker.gate_sigma = v; dirty = 1; }

    v = a->cfg.tracker.gate_max_range_m;
    if (ui_slider(c, ui_id("tk.gm", 0), ui_stack_row(&s, 24), "gate cap [m]",
                  &v, 100.0, 8000.0, "%.0f", 1) != 0)
    { a->cfg.tracker.gate_max_range_m = v; dirty = 1; }

    iv = a->cfg.tracker.confirm_m;
    if (ui_slider_int(c, ui_id("tk.m", 0), ui_stack_row(&s, 24), "confirm M",
                      &iv, 1, 12, "%.0f") != 0)
    { a->cfg.tracker.confirm_m = iv; dirty = 1; }

    iv = a->cfg.tracker.confirm_n;
    if (ui_slider_int(c, ui_id("tk.n", 0), ui_stack_row(&s, 24), "of N",
                      &iv, 1, 16, "%.0f") != 0)
    { a->cfg.tracker.confirm_n = iv; dirty = 1; }

    iv = a->cfg.tracker.coast_misses;
    if (ui_slider_int(c, ui_id("tk.cm", 0), ui_stack_row(&s, 24), "coast after",
                      &iv, 1, 20, "%.0f") != 0)
    { a->cfg.tracker.coast_misses = iv; dirty = 1; }

    iv = a->cfg.tracker.delete_misses;
    if (ui_slider_int(c, ui_id("tk.dm", 0), ui_stack_row(&s, 24), "delete after",
                      &iv, 1, 40, "%.0f") != 0)
    { a->cfg.tracker.delete_misses = iv; dirty = 1; }

    iv = a->cfg.tracker.init_inhibit_m;
    if (ui_slider_int(c, ui_id("tk.ii", 0), ui_stack_row(&s, 24),
                      "init inhibit [m]", &iv, 0, 3000, "%.0f") != 0)
    { a->cfg.tracker.init_inhibit_m = iv; dirty = 1; }

    if (ui_checkbox(c, ui_id("tk.dg", 0), ui_stack_row(&s, 22),
                    "use Doppler in the gate",
                    &a->cfg.tracker.use_doppler_in_gate) != 0)
    { dirty = 1; }

    if (dirty != 0) { (void)pwr_engine_set_tracker(a->eng, &a->cfg.tracker); }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "TRACK FILE");
    {
        UI_Rect row = ui_stack_row(&s, 17);
        (void)snprintf(buf, sizeof(buf), "%u", a->stats.tracks_active);
        ui_readout(c, row, "active", buf, UI_C_TEXT);
        row = ui_stack_row(&s, 17);
        (void)snprintf(buf, sizeof(buf), "%u", a->stats.tracks_confirmed);
        ui_readout(c, row, "confirmed", buf, UI_C_TRACK_CONF);
        row = ui_stack_row(&s, 17);
        (void)snprintf(buf, sizeof(buf), "%u", a->stats.tracks_created_total);
        ui_readout(c, row, "created", buf, UI_C_TEXT_DIM);
        row = ui_stack_row(&s, 17);
        (void)snprintf(buf, sizeof(buf), "%u", a->stats.tracks_deleted_total);
        ui_readout(c, row, "deleted", buf, UI_C_TEXT_DIM);
    }
}

static void app_panel_scenario(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Stack s;
    char buf[96];
    double v;
    int dirty = 0;

    ui_stack_begin(&s, r, 4);
    ui_label_dim(c, ui_stack_row(&s, 16), "ENVIRONMENT");

    if (ui_checkbox(c, ui_id("en.n", 0), ui_stack_row(&s, 22),
                    "thermal noise", &a->env.enable_thermal_noise) != 0) { dirty = 1; }
    if (ui_checkbox(c, ui_id("en.sc", 0), ui_stack_row(&s, 22),
                    "sea clutter", &a->env.enable_sea_clutter) != 0) { dirty = 1; }

    v = a->env.sea_state;
    if (ui_slider(c, ui_id("en.ss", 0), ui_stack_row(&s, 24), "sea state",
                  &v, 0.0, 7.0, "%.1f", 0) != 0)
    { a->env.sea_state = v; dirty = 1; }

    v = a->env.clutter_to_noise_db;
    if (ui_slider(c, ui_id("en.cnr", 0), ui_stack_row(&s, 24), "CNR @1km [dB]",
                  &v, -10.0, 60.0, "%.1f", 0) != 0)
    { a->env.clutter_to_noise_db = v; dirty = 1; }

    v = a->env.clutter_spread_hz;
    if (ui_slider(c, ui_id("en.cs", 0), ui_stack_row(&s, 24), "clutter BW [Hz]",
                  &v, 0.1, 200.0, "%.1f", 1) != 0)
    { a->env.clutter_spread_hz = v; dirty = 1; }

    if (ui_checkbox(c, ui_id("en.rn", 0), ui_stack_row(&s, 22),
                    "rain", &a->env.enable_rain) != 0) { dirty = 1; }
    v = a->env.rain_rate_mmph;
    if (ui_slider(c, ui_id("en.rr", 0), ui_stack_row(&s, 24), "rain [mm/h]",
                  &v, 0.0, 50.0, "%.1f", 0) != 0)
    { a->env.rain_rate_mmph = v; dirty = 1; }

    if (ui_checkbox(c, ui_id("en.jm", 0), ui_stack_row(&s, 22),
                    "noise jammer", &a->env.enable_jammer) != 0) { dirty = 1; }
    v = a->env.jammer_azimuth_deg;
    if (ui_slider(c, ui_id("en.ja", 0), ui_stack_row(&s, 24), "jammer az [\xC2\xB0]",
                  &v, 0.0, 359.0, "%.0f", 0) != 0)
    { a->env.jammer_azimuth_deg = v; dirty = 1; }
    v = a->env.jammer_power_db;
    if (ui_slider(c, ui_id("en.jp", 0), ui_stack_row(&s, 24), "JNR [dB]",
                  &v, 0.0, 50.0, "%.1f", 0) != 0)
    { a->env.jammer_power_db = v; dirty = 1; }

    if (ui_checkbox(c, ui_id("en.mp", 0), ui_stack_row(&s, 22),
                    "surface multipath", &a->env.enable_multipath) != 0) { dirty = 1; }
    if (ui_checkbox(c, ui_id("en.ec", 0), ui_stack_row(&s, 22),
                    "transmit eclipsing", &a->env.enable_eclipsing) != 0) { dirty = 1; }
    if (ui_checkbox(c, ui_id("en.ra", 0), ui_stack_row(&s, 22),
                    "range ambiguity fold",
                    &a->env.enable_range_ambiguity) != 0) { dirty = 1; }

    if (dirty != 0) { (void)pwr_engine_set_environment(a->eng, &a->env); }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "INJECT A TARGET");
    {
        static double t_range_km = 15.0;
        static double t_bearing  = 45.0;
        static double t_alt_m    = 3000.0;
        static double t_course   = 225.0;
        static double t_speed    = 150.0;
        static double t_rcs      = 5.0;
        static int32_t t_swer    = PWR_SWERLING_1;
        UI_Rect half[2];

        (void)ui_slider(c, ui_id("in.r", 0), ui_stack_row(&s, 24), "range [km]",
                        &t_range_km, 0.5, 100.0, "%.1f", 0);
        (void)ui_slider(c, ui_id("in.b", 0), ui_stack_row(&s, 24),
                        "bearing [\xC2\xB0]", &t_bearing, 0.0, 359.0, "%.0f", 0);
        (void)ui_slider(c, ui_id("in.h", 0), ui_stack_row(&s, 24), "altitude [m]",
                        &t_alt_m, 0.0, 15000.0, "%.0f", 0);
        (void)ui_slider(c, ui_id("in.c", 0), ui_stack_row(&s, 24),
                        "course [\xC2\xB0]", &t_course, 0.0, 359.0, "%.0f", 0);
        (void)ui_slider(c, ui_id("in.s", 0), ui_stack_row(&s, 24), "speed [m/s]",
                        &t_speed, 0.0, 600.0, "%.0f", 0);
        (void)ui_slider(c, ui_id("in.rcs", 0), ui_stack_row(&s, 24),
                        "RCS [m\xC2\xB2]", &t_rcs, 0.01, 5000.0, "%.2f", 1);
        (void)ui_combo(c, ui_id("in.sw", 0), ui_stack_row(&s, 24), "fluctuation",
                       &t_swer, PWR_SWERLING_COUNT, app_item_swerling, NULL);

        ui_stack_row_split(&s, 26, 2, half);
        if (ui_button_accent(c, ui_id("in.add", 0), half[0], "ADD TARGET",
                             UI_C_CTRL_ON) != 0)
        {
            PWR_SimTarget t;
            const double b = t_bearing * UI_PI / 180.0;
            const double cs = t_course * UI_PI / 180.0;
            memset(&t, 0, sizeof(t));
            t.x_m = t_range_km * 1e3 * sin(b);
            t.y_m = t_range_km * 1e3 * cos(b);
            t.z_m = t_alt_m;
            t.vx_mps = t_speed * sin(cs);
            t.vy_mps = t_speed * cos(cs);
            t.rcs_m2 = t_rcs;
            t.swerling = t_swer;
            t.enabled = 1;
            (void)snprintf(t.label, sizeof(t.label), "MAN-%02u",
                           pwr_engine_target_count(a->eng) + 1u);
            if (pwr_engine_target_add(a->eng, &t) == PWR_STATUS_OK)
            {
                app_logf(a, "[INFO ] injected %s at %.1f km / %.0f\xC2\xB0",
                         t.label, t_range_km, t_bearing);
            }
            else
            {
                app_logf(a, "[WARN ] target list is full");
            }
        }
        if (ui_button(c, ui_id("in.clr", 0), half[1], "CLEAR ALL") != 0)
        {
            (void)pwr_engine_target_clear(a->eng);
            app_logf(a, "[INFO ] target list cleared");
        }
    }

    ui_separator(c, ui_stack_row(&s, 10));
    (void)snprintf(buf, sizeof(buf), "%u targets in the scenario",
                   pwr_engine_target_count(a->eng));
    ui_label_dim(c, ui_stack_row(&s, 16), buf);
}

static void app_panel_display(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Stack s;
    int32_t iv;
    double v;

    ui_stack_begin(&s, r, 4);

    ui_label_dim(c, ui_stack_row(&s, 16), "PPI SCOPE");
    iv = a->ppi.cmap;
    if (ui_combo(c, ui_id("dp.pc", 0), ui_stack_row(&s, 24), "colormap",
                 &iv, UI_CMAP_COUNT, app_item_cmap, NULL) != 0)
    { a->ppi.cmap = iv; }
    v = a->ppi.video_gain_db;
    (void)ui_slider(c, ui_id("dp.pg", 0), ui_stack_row(&s, 24), "video gain",
                    &v, -20.0, 20.0, "%+.1f", 0);
    a->ppi.video_gain_db = v;
    v = a->cfg.ppi_persistence_s;
    if (ui_slider(c, ui_id("dp.pp", 0), ui_stack_row(&s, 24), "persistence [s]",
                  &v, 0.0, 20.0, "%.1f", 0) != 0)
    { a->cfg.ppi_persistence_s = v; a->cfg_dirty = 1; }
    v = a->ppi.range_max_m * 1e-3;
    if (ui_slider(c, ui_id("dp.pr", 0), ui_stack_row(&s, 24), "scale [km]",
                  &v, 0.5, app_max(a->ppi.range_max_full_m * 1e-3, 1.0),
                  "%.1f", 0) != 0)
    { a->ppi.range_max_m = v * 1e3; }
    v = a->ppi.leader_time_s;
    if (ui_slider(c, ui_id("dp.pl", 0), ui_stack_row(&s, 24), "leader [s]",
                  &v, 15.0, 240.0, "%.0f", 1) != 0)
    { a->ppi.leader_time_s = v; }

    (void)ui_checkbox(c, ui_id("dp.v", 0), ui_stack_row(&s, 21),
                      "radar video", &a->ppi.show_video);
    (void)ui_checkbox(c, ui_id("dp.g", 0), ui_stack_row(&s, 21),
                      "rings and spokes", &a->ppi.show_rings);
    (void)ui_checkbox(c, ui_id("dp.b", 0), ui_stack_row(&s, 21),
                      "beam sweep", &a->ppi.show_beam);
    (void)ui_checkbox(c, ui_id("dp.d", 0), ui_stack_row(&s, 21),
                      "plots", &a->ppi.show_detections);
    (void)ui_checkbox(c, ui_id("dp.t", 0), ui_stack_row(&s, 21),
                      "tracks", &a->ppi.show_tracks);
    (void)ui_checkbox(c, ui_id("dp.tr", 0), ui_stack_row(&s, 21),
                      "history trails", &a->ppi.show_trails);
    (void)ui_checkbox(c, ui_id("dp.cv", 0), ui_stack_row(&s, 21),
                      "covariance ellipses", &a->ppi.show_gates);
    (void)ui_checkbox(c, ui_id("dp.gt", 0), ui_stack_row(&s, 21),
                      "ground truth overlay", &a->ppi.show_truth);
    (void)ui_checkbox(c, ui_id("dp.lb", 0), ui_stack_row(&s, 21),
                      "labels", &a->ppi.show_labels);

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "HISTORY / VERIFICATION");
    (void)ui_checkbox(c, ui_id("dp.hp", 0), ui_stack_row(&s, 21),
                      "full track paths", &a->ppi.show_track_paths);
    (void)ui_checkbox(c, ui_id("dp.ht", 0), ui_stack_row(&s, 21),
                      "truth paths", &a->ppi.show_truth_paths);
    (void)ui_checkbox(c, ui_id("dp.hh", 0), ui_stack_row(&s, 21),
                      "plot history", &a->ppi.show_plot_hist);
    (void)ui_checkbox(c, ui_id("dp.ha", 0), ui_stack_row(&s, 21),
                      "plot-track association", &a->ppi.show_assoc);
    (void)ui_checkbox(c, ui_id("dp.hd", 0), ui_stack_row(&s, 21),
                      "dwell hit/miss rings", &a->ppi.show_dwell);
    v = a->hist_retain_s;
    if (ui_slider(c, ui_id("dp.hr", 0), ui_stack_row(&s, 24), "history [s]",
                  &v, 10.0, 3600.0, "%.0f", 1) != 0)
    { a->hist_retain_s = v; }
    if (ui_button(c, ui_id("dp.hx", 0), ui_stack_row(&s, 24),
                  "CLEAR HISTORY") != 0)
    { app_history_clear(a); }

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "RANGE-DOPPLER MAP");
    iv = a->cmap_rd;
    if (ui_combo(c, ui_id("dp.rc", 0), ui_stack_row(&s, 24), "colormap",
                 &iv, UI_CMAP_COUNT, app_item_cmap, NULL) != 0)
    { a->cmap_rd = iv; }
    (void)ui_checkbox(c, ui_id("dp.ac", 0), ui_stack_row(&s, 21),
                      "auto colour limits", &a->auto_clim);
    (void)ui_slider(c, ui_id("dp.c0", 0), ui_stack_row(&s, 24), "clim low [dB]",
                    &a->clim_rd_lo, -20.0, 60.0, "%.1f", 0);
    (void)ui_slider(c, ui_id("dp.c1", 0), ui_stack_row(&s, 24), "clim high [dB]",
                    &a->clim_rd_hi, 0.0, 110.0, "%.1f", 0);
    (void)ui_checkbox(c, ui_id("dp.sm", 0), ui_stack_row(&s, 21),
                      "bilinear interpolation", &a->smooth_rd);

    ui_separator(c, ui_stack_row(&s, 10));
    ui_label_dim(c, ui_stack_row(&s, 16), "A-SCOPE / WATERFALL");
    (void)ui_checkbox(c, ui_id("dp.rv", 0), ui_stack_row(&s, 21),
                      "raw (pre-MTI) video", &a->show_raw_video);
    (void)ui_checkbox(c, ui_id("dp.th", 0), ui_stack_row(&s, 21),
                      "CFAR threshold trace", &a->show_threshold);
    iv = a->cmap_rti;
    if (ui_combo(c, ui_id("dp.wc", 0), ui_stack_row(&s, 24), "RTI colormap",
                 &iv, UI_CMAP_COUNT, app_item_cmap, NULL) != 0)
    { a->cmap_rti = iv; }
    iv = (int32_t)a->cfg.rti_rows;
    if (ui_slider_int(c, ui_id("dp.wr", 0), ui_stack_row(&s, 24), "RTI history",
                      &iv, 32, 512, "%.0f") != 0)
    { a->cfg.rti_rows = (uint32_t)iv; a->cfg_dirty = 1; }

    iv = a->cursor_bin_ui;
    if (ui_slider_int(c, ui_id("dp.cb", 0), ui_stack_row(&s, 24),
                      "spectrum gate", &iv, 0,
                      (int32_t)((a->dm.range_bins > 0u) ? a->dm.range_bins - 1u : 1u),
                      "%.0f") != 0)
    {
        a->cursor_bin_ui = iv;
        (void)pwr_engine_set_cursor_range_bin(a->eng, (uint32_t)iv);
    }
}

/* ==========================================================================
 *  Right-hand tables
 * ========================================================================== */
static void app_cell_track(void* user, int32_t row, int32_t col,
                           char* out, size_t cap, UI_Color* fg)
{
    const App* a = (const App*)user;
    const PWR_Track* t;
    if (a->have_frame == 0 || row < 0 || (uint32_t)row >= a->frame.track_count)
    {
        return;
    }
    t = &a->frame.tracks[row];
    *fg = ui_track_colour(t->state);
    switch (col)
    {
    case 0: (void)snprintf(out, cap, "T%03u", t->id); break;
    case 1:
        /* Abbreviated so the column never clips: the colour already carries the
         * state, the text is a confirmation of it. */
        (void)snprintf(out, cap, "%s",
                       (t->state == PWR_TRACK_CONFIRMED) ? "CONF"
                     : ((t->state == PWR_TRACK_COASTING) ? "COAST"
                     : ((t->state == PWR_TRACK_TENTATIVE) ? "TENT" : "-")));
        break;
    case 2: (void)snprintf(out, cap, "%.2f", t->range_m * 1e-3); break;
    case 3: (void)snprintf(out, cap, "%05.1f", t->azimuth_deg); break;
    case 4: (void)snprintf(out, cap, "%.0f", t->speed_mps); break;
    case 5: (void)snprintf(out, cap, "%03.0f", t->course_deg); break;
    case 6: (void)snprintf(out, cap, "%.1f", (double)t->snr_db); break;
    case 7:
    {
        /* Last eight dwell attempts, oldest left, '#' == hit: the raw
         * M-of-N evidence behind the STATE column. */
        int32_t b;
        if (cap < 9u) { break; }
        for (b = 0; b < 8; ++b)
        {
            out[b] = (((t->history_bits >> (7 - b)) & 1u) != 0u) ? '#' : '.';
        }
        out[8] = '\0';
        break;
    }
    case 8: (void)snprintf(out, cap, "%s",
                           pwr_class_name((PWR_TargetClass)t->target_class)); break;
    default: break;
    }
}

static void app_cell_plot(void* user, int32_t row, int32_t col,
                          char* out, size_t cap, UI_Color* fg)
{
    const App* a = (const App*)user;
    const PWR_Detection* d;
    if (a->have_frame == 0 || row < 0 || (uint32_t)row >= a->frame.detection_count)
    {
        return;
    }
    d = &a->frame.detections[row];
    *fg = (d->ambiguous != 0) ? UI_C_WARN : UI_C_DETECTION;
    switch (col)
    {
    case 0: (void)snprintf(out, cap, "%d", row + 1); break;
    case 1: (void)snprintf(out, cap, "%.3f", d->range_m * 1e-3); break;
    case 2: (void)snprintf(out, cap, "%05.1f", d->azimuth_deg); break;
    case 3: (void)snprintf(out, cap, "%+.1f", d->radial_velocity_mps); break;
    case 4: (void)snprintf(out, cap, "%.1f", (double)d->snr_db); break;
    case 5: (void)snprintf(out, cap, "%.1f", (double)d->threshold_db); break;
    case 6: (void)snprintf(out, cap, "%u", d->cell_count); break;
    case 7:
        if (d->assoc_track_id > 0)
        {
            (void)snprintf(out, cap, "T%03d", d->assoc_track_id);
        }
        else
        {
            (void)snprintf(out, cap, "-");
            *fg = UI_C_TEXT_DIM;
        }
        break;
    default: break;
    }
}

/* ---- Verify tab: one row per truth target ------------------------------- */
static void app_cell_verify(void* user, int32_t row, int32_t col,
                            char* out, size_t cap, UI_Color* fg)
{
    App* a = (App*)user;
    const PWR_TargetScore* sc;
    const char* st;
    UI_Color sc_col;

    if (row < 0 || row >= a->ver_row_count) { return; }
    sc = &a->verify.targets[a->ver_rows[row]];

    if (sc->active == 0)                  { st = "OUT";    sc_col = UI_C_TEXT_FAINT; }
    else if (sc->paired_track_id != 0)    { st = "TRACK";  sc_col = UI_C_TRACK_CONF; }
    else if (sc->first_track_s >= 0.0)    { st = "LOST";   sc_col = UI_C_ALARM; }
    else                                  { st = "SEARCH"; sc_col = UI_C_WARN; }
    *fg = sc_col;

    switch (col)
    {
    case 0:
        (void)snprintf(out, cap, "%s", sc->label);
        *fg = UI_C_TRUTH;
        break;
    case 1:
        if (sc->paired_track_id != 0)
        {
            (void)snprintf(out, cap, "T%03d", sc->paired_track_id);
        }
        else { (void)snprintf(out, cap, "-"); }
        break;
    case 2: (void)snprintf(out, cap, "%s", st); break;
    case 3:
        if (sc->err_now_m >= 0.0)
        {
            (void)snprintf(out, cap, "%.0f", sc->err_now_m);
        }
        else { (void)snprintf(out, cap, "-"); }
        break;
    case 4:
        if (sc->err_n > 0u)
        {
            (void)snprintf(out, cap, "%.0f",
                           sc->err_rms_m);
        }
        else { (void)snprintf(out, cap, "-"); }
        break;
    case 5:
        if (sc->time_active_s > 0.0)
        {
            (void)snprintf(out, cap, "%.0f",
                           100.0 * sc->completeness);
        }
        else { (void)snprintf(out, cap, "-"); }
        break;
    case 6:
        if (sc->first_track_s >= 0.0 && sc->first_seen_s >= 0.0)
        {
            (void)snprintf(out, cap, "%.1f",
                           sc->first_track_s - sc->first_seen_s);
        }
        else { (void)snprintf(out, cap, "-"); }
        break;
    default: break;
    }
}

static void app_cell_target(void* user, int32_t row, int32_t col,
                            char* out, size_t cap, UI_Color* fg)
{
    App* a = (App*)user;
    PWR_SimTarget t;
    double range, bearing;
    if (pwr_engine_target_at(a->eng, (uint32_t)row, &t) != PWR_STATUS_OK) { return; }
    range   = sqrt(t.x_m * t.x_m + t.y_m * t.y_m);
    bearing = atan2(t.x_m, t.y_m) * 180.0 / UI_PI;
    if (bearing < 0.0) { bearing += 360.0; }
    *fg = (t.enabled != 0) ? UI_C_TRUTH : UI_C_TEXT_FAINT;
    switch (col)
    {
    case 0: (void)snprintf(out, cap, "%s", t.label); break;
    case 1: (void)snprintf(out, cap, "%.2f", range * 1e-3); break;
    case 2: (void)snprintf(out, cap, "%05.1f", bearing); break;
    case 3: (void)snprintf(out, cap, "%.0f", t.z_m); break;
    case 4: (void)snprintf(out, cap, "%.0f",
                           sqrt(t.vx_mps * t.vx_mps + t.vy_mps * t.vy_mps)); break;
    case 5: (void)snprintf(out, cap, "%.2f", t.rcs_m2); break;
    case 6: (void)snprintf(out, cap, "%s",
                           pwr_swerling_name((PWR_Swerling)t.swerling)); break;
    default: break;
    }
}

static void app_draw_right(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    static const char* const tabs[APP_RT_COUNT] = {
        "Tracks", "Plots", "Verify", "Targets", "Log"
    };
    const int32_t tab_h = 24;
    UI_Rect body;

    ui_fill_rect(&c->canvas, r, UI_C_PANEL);
    (void)ui_tabs(c, ui_id("rt.tabs", 0), ui_rect(r.x, r.y, r.w, tab_h),
                  &a->tab_right, tabs, APP_RT_COUNT);
    body = ui_rect(r.x + 1, r.y + tab_h + 1, r.w - 2, r.h - tab_h - 2);

    switch (a->tab_right)
    {
    case APP_RT_TRACKS:
    {
        static const UI_TableColumn cols[9] = {
            { "ID",     44, UI_ALIGN_LEFT,   UI_FONT_MONO },
            { "STATE",  54, UI_ALIGN_LEFT,   UI_FONT_SMALL },
            { "R km",   48, UI_ALIGN_RIGHT,  UI_FONT_MONO },
            { "AZ",     46, UI_ALIGN_RIGHT,  UI_FONT_MONO },
            { "V m/s",  44, UI_ALIGN_RIGHT,  UI_FONT_MONO },
            { "CRS",    38, UI_ALIGN_RIGHT,  UI_FONT_MONO },
            { "SNR",    40, UI_ALIGN_RIGHT,  UI_FONT_MONO },
            { "HIT",    64, UI_ALIGN_LEFT,   UI_FONT_MONO },
            { "CLASS",   0, UI_ALIGN_LEFT,   UI_FONT_SMALL }
        };
        const int32_t n = (a->have_frame != 0) ? (int32_t)a->frame.track_count : 0;
        if (ui_table(c, ui_id("rt.trk", 0), body, cols, 9, n,
                     &a->tbl_tracks, app_cell_track, a) != 0)
        {
            if (a->tbl_tracks.selected >= 0 && a->tbl_tracks.selected < n)
            {
                a->ppi.selected_track =
                    (int32_t)a->frame.tracks[a->tbl_tracks.selected].id;
            }
        }
        /* Keep the table selection in step with a pick made on the scope. */
        if (a->ppi.selected_track != 0 && a->have_frame != 0)
        {
            int32_t i;
            for (i = 0; i < n; ++i)
            {
                if ((int32_t)a->frame.tracks[i].id == a->ppi.selected_track)
                {
                    a->tbl_tracks.selected = i;
                    break;
                }
            }
        }
        break;
    }

    case APP_RT_PLOTS:
    {
        static const UI_TableColumn cols[8] = {
            { "#",      32, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "R km",   62, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "AZ",     52, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "Vr m/s", 56, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "SNR dB", 54, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "THR dB", 54, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "CELLS",  44, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "TRK",     0, UI_ALIGN_LEFT,  UI_FONT_MONO }
        };
        const int32_t n = (a->have_frame != 0)
            ? (int32_t)a->frame.detection_count : 0;
        (void)ui_table(c, ui_id("rt.plt", 0), body, cols, 8, n,
                       &a->tbl_plots, app_cell_plot, a);
        break;
    }

    case APP_RT_VERIFY:
    {
        static const UI_TableColumn cols[7] = {
            { "TARGET", 78, UI_ALIGN_LEFT,  UI_FONT_SMALL },
            { "TRK",    42, UI_ALIGN_LEFT,  UI_FONT_MONO },
            { "ST",     52, UI_ALIGN_LEFT,  UI_FONT_SMALL },
            { "ERR",    46, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "RMSE",   48, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "CMP%",   46, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "TTT",     0, UI_ALIGN_RIGHT, UI_FONT_MONO }
        };
        const int32_t foot_h = 4 * 17 + 8;
        const UI_Rect tbl = ui_rect(body.x, body.y, body.w,
                                    (body.h - foot_h > 60) ? (body.h - foot_h)
                                                           : 60);
        int32_t i;
        char buf[64];

        a->ver_row_count = 0;
        for (i = 0; i < (int32_t)PWR_MAX_SIM_TARGETS; ++i)
        {
            if (a->verify.targets[i].truth_id != 0 &&
                a->verify.targets[i].first_seen_s >= 0.0)
            {
                a->ver_rows[a->ver_row_count++] = i;
            }
        }
        (void)ui_table(c, ui_id("rt.ver", 0), tbl, cols, 7, a->ver_row_count,
                       &a->tbl_verify, app_cell_verify, a);

        /* ---- single-picture summary ------------------------------------- */
        {
            UI_Stack st;
            ui_stack_begin(&st, ui_rect(body.x + 6, tbl.y + tbl.h + 4,
                                        body.w - 12, foot_h - 4), 0);
            (void)snprintf(buf, sizeof(buf), "%d / %d",
                           a->verify.summary.truth_tracked,
                           a->verify.summary.truth_active);
            ui_readout(c, ui_stack_row(&st, 17), "truth under track", buf,
                       (a->verify.summary.truth_active > 0u &&
                        a->verify.summary.truth_tracked ==
                            a->verify.summary.truth_active)
                           ? UI_C_OK : UI_C_WARN);
            (void)snprintf(buf, sizeof(buf), "%u", a->verify.summary.spurious);
            ui_readout(c, ui_stack_row(&st, 17), "spurious confirmed", buf,
                       (a->verify.summary.spurious > 0u) ? UI_C_ALARM
                                                        : UI_C_TEXT_DIM);
            (void)snprintf(buf, sizeof(buf), "%u", a->verify.summary.redundant);
            ui_readout(c, ui_stack_row(&st, 17), "redundant tracks", buf,
                       (a->verify.summary.redundant > 0u) ? UI_C_WARN
                                                         : UI_C_TEXT_DIM);
            {
                double comp = 0.0, act = 0.0;
                for (i = 0; i < a->ver_row_count; ++i)
                {
                    const PWR_TargetScore* sc =
                        &a->verify.targets[a->ver_rows[i]];
                    comp += sc->time_tracked_s;
                    act  += sc->time_active_s;
                }
                if (act > 0.0)
                {
                    (void)snprintf(buf, sizeof(buf), "%.1f %%",
                                   100.0 * comp / act);
                }
                else { (void)snprintf(buf, sizeof(buf), "-"); }
                ui_readout(c, ui_stack_row(&st, 17), "track completeness",
                           buf, UI_C_TEXT);
            }
        }
        break;
    }

    case APP_RT_TARGETS:
    {
        static const UI_TableColumn cols[7] = {
            { "LABEL",  86, UI_ALIGN_LEFT,  UI_FONT_SMALL },
            { "R km",   58, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "AZ",     54, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "ALT m",  56, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "V m/s",  52, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "RCS",    58, UI_ALIGN_RIGHT, UI_FONT_MONO },
            { "FLUCT",   0, UI_ALIGN_LEFT,  UI_FONT_SMALL }
        };
        const int32_t n = (int32_t)pwr_engine_target_count(a->eng);
        if (ui_table(c, ui_id("rt.tgt", 0), body, cols, 7, n,
                     &a->tbl_targets, app_cell_target, a) != 0)
        {
            /* selection only */
        }
        break;
    }

    case APP_RT_LOG:
    default:
    {
        const int32_t lh = ui_font_line_height(UI_FONT_SMALL);
        int32_t i;
        ui_fill_rect(&c->canvas, body, UI_C_PLOT_BG);
        ui_frame_rect(&c->canvas, body, UI_C_BORDER);
        ui_clip_push(&c->canvas, ui_rect_inset(body, 4, 3));
        for (i = 0; i < a->log_count; ++i)
        {
            const int32_t idx = (a->log_head + a->log_count - 1 - i) % APP_LOG_LINES;
            const int32_t y = body.y + body.h - 6 - i * lh;
            const char* line = a->log[idx];
            UI_Color col = UI_C_TEXT_DIM;
            if (y < body.y) { break; }
            if (strstr(line, "[ERROR]") != NULL) { col = UI_C_ALARM; }
            else if (strstr(line, "[WARN ]") != NULL) { col = UI_C_WARN; }
            (void)ui_text(&c->canvas, UI_FONT_SMALL, body.x + 5, y, col, line);
        }
        ui_clip_pop(&c->canvas);
        break;
    }
    }
}

static void app_draw_readouts(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    const UI_Rect in = ui_group(c, r, "SELECTED TRACK / SYSTEM");
    UI_Stack s;
    char buf[128];
    const PWR_Track* sel = NULL;
    uint32_t i;

    ui_stack_begin(&s, in, 2);
    if (a->have_frame != 0 && a->ppi.selected_track != 0)
    {
        for (i = 0u; i < a->frame.track_count; ++i)
        {
            if ((int32_t)a->frame.tracks[i].id == a->ppi.selected_track)
            {
                sel = &a->frame.tracks[i];
                break;
            }
        }
    }

    if (sel != NULL)
    {
        const UI_Color col = ui_track_colour(sel->state);
        (void)snprintf(buf, sizeof(buf), "T%03u   %s   %s", sel->id,
                       pwr_track_state_name((PWR_TrackState)sel->state),
                       pwr_class_name((PWR_TargetClass)sel->target_class));
        (void)ui_text_in_rect(&c->canvas, UI_FONT_BOLD, ui_stack_row(&s, 19), 0,
                              col, UI_ALIGN_LEFT, buf);
        (void)snprintf(buf, sizeof(buf), "%.3f km", sel->range_m * 1e-3);
        ui_readout(c, ui_stack_row(&s, 17), "range", buf, UI_C_TEXT);
        (void)snprintf(buf, sizeof(buf), "%.2f\xC2\xB0", sel->azimuth_deg);
        ui_readout(c, ui_stack_row(&s, 17), "bearing", buf, UI_C_TEXT);
        (void)snprintf(buf, sizeof(buf), "%.1f m/s", sel->speed_mps);
        ui_readout(c, ui_stack_row(&s, 17), "speed", buf, UI_C_TEXT);
        (void)snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", sel->course_deg);
        ui_readout(c, ui_stack_row(&s, 17), "course", buf, UI_C_TEXT);
        (void)snprintf(buf, sizeof(buf), "%+.2f m/s", sel->radial_velocity_mps);
        ui_readout(c, ui_stack_row(&s, 17), "range rate", buf, UI_C_TEXT);
        (void)snprintf(buf, sizeof(buf), "%.1f dB", (double)sel->snr_db);
        ui_readout(c, ui_stack_row(&s, 17), "SNR", buf, UI_C_TEXT);
        (void)snprintf(buf, sizeof(buf), "%u / %u", sel->hits, sel->update_attempts);
        ui_readout(c, ui_stack_row(&s, 17), "hits / attempts", buf, UI_C_TEXT);
        {
            /* Last 16 dwell attempts, oldest left - the M-of-N evidence. */
            char pat[17];
            int32_t b;
            for (b = 0; b < 16; ++b)
            {
                pat[b] = (((sel->history_bits >> (15 - b)) & 1u) != 0u)
                       ? '#' : '.';
            }
            pat[16] = '\0';
            (void)snprintf(buf, sizeof(buf), "%s  (%d of %d)", pat,
                           a->cfg.tracker.confirm_m, a->cfg.tracker.confirm_n);
            ui_readout(c, ui_stack_row(&s, 17), "dwell window", buf, UI_C_TEXT);
        }
        ui_readout(c, ui_stack_row(&s, 17), "this dwell",
                   (sel->dwell_state == PWR_DWELL_HIT) ? "HIT"
                       : ((sel->dwell_state == PWR_DWELL_MISS) ? "MISS"
                                                               : "not in beam"),
                   (sel->dwell_state == PWR_DWELL_HIT) ? UI_C_OK
                       : ((sel->dwell_state == PWR_DWELL_MISS) ? UI_C_ALARM
                                                               : UI_C_TEXT_DIM));
        (void)snprintf(buf, sizeof(buf), "%.1f m",
                       sqrt(app_max(sel->pos_cov[0], 0.0)));
        ui_readout(c, ui_stack_row(&s, 17), "\xCF\x83 east", buf, UI_C_TEXT_DIM);
        (void)snprintf(buf, sizeof(buf), "%.1f m",
                       sqrt(app_max(sel->pos_cov[3], 0.0)));
        ui_readout(c, ui_stack_row(&s, 17), "\xCF\x83 north", buf, UI_C_TEXT_DIM);
        (void)snprintf(buf, sizeof(buf), "%.2f", (double)sel->innovation_norm);
        ui_readout(c, ui_stack_row(&s, 17), "innovation [\xCF\x83]", buf,
                   ((double)sel->innovation_norm > a->cfg.tracker.gate_sigma * 0.75)
                       ? UI_C_WARN : UI_C_OK);
        (void)snprintf(buf, sizeof(buf), "%.1f s",
                       sel->last_time_s - sel->first_time_s);
        ui_readout(c, ui_stack_row(&s, 17), "track age", buf, UI_C_TEXT_DIM);
    }
    else
    {
        (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL, ui_stack_row(&s, 19), 0,
                              UI_C_TEXT_FAINT, UI_ALIGN_LEFT,
                              "click a track on the PPI or in the table");
    }

    ui_separator(c, ui_stack_row(&s, 8));
    (void)snprintf(buf, sizeof(buf), "%.2f s", a->stats.scenario_time_s);
    ui_readout(c, ui_stack_row(&s, 17), "scenario time", buf, UI_C_TEXT_DIM);
    (void)snprintf(buf, sizeof(buf), "%llu / %llu",
                   (unsigned long long)a->stats.frames_published,
                   (unsigned long long)a->stats.frames_dropped);
    ui_readout(c, ui_stack_row(&s, 17), "frames pub / drop", buf,
               (a->stats.frames_dropped > 0u) ? UI_C_WARN : UI_C_TEXT_DIM);
    (void)snprintf(buf, sizeof(buf), "%s",
                   (a->selftest_status == PWR_STATUS_OK) ? "PASS" : "FAIL");
    ui_readout(c, ui_stack_row(&s, 17), "core self test", buf,
               (a->selftest_status == PWR_STATUS_OK) ? UI_C_OK : UI_C_ALARM);
    if (a->cfg_err[0] != '\0')
    {
        (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL, ui_stack_row(&s, 17), 0,
                              UI_C_ALARM, UI_ALIGN_LEFT, a->cfg_err);
    }
}

/* ==========================================================================
 *  Displays
 * ========================================================================== */
static void app_draw_scope(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Axes* ax = &a->ax_scope;
    UI_LegendEntry leg[3];
    int32_t n_leg = 0;

    ui_axes_layout(ax, r);
    (void)ui_axes_input(c, ax, ui_id("ax.scope", 0));
    ui_axes_draw_bg(&c->canvas, ax);

    if (a->have_frame != 0 && a->frame.range_profile_db != NULL)
    {
        const double x0 = a->frame.range_first_m * 1e-3;
        const double dx = a->frame.range_step_m * 1e-3;
        const int32_t n = (int32_t)a->frame.range_bins;

        if (a->show_raw_video != 0 && a->frame.range_profile_raw_db != NULL)
        {
            ui_plot_line(&c->canvas, ax, a->frame.range_profile_raw_db, n,
                         x0, dx, ui_color_alpha(UI_C_SERIES_1, 150), 1.0);
            leg[n_leg].label = "raw video";
            leg[n_leg].color = UI_C_SERIES_1;
            leg[n_leg].marker = UI_MARKER_NONE;
            ++n_leg;
        }
        ui_plot_line(&c->canvas, ax, a->frame.range_profile_db, n, x0, dx,
                     UI_C_SERIES_0, 1.4);
        leg[n_leg].label = "max over Doppler";
        leg[n_leg].color = UI_C_SERIES_0;
        leg[n_leg].marker = UI_MARKER_NONE;
        ++n_leg;

        if (a->show_threshold != 0 && a->frame.cfar_threshold_db != NULL)
        {
            ui_plot_line(&c->canvas, ax, a->frame.cfar_threshold_db, n, x0, dx,
                         ui_color_alpha(UI_C_SERIES_2, 210), 1.2);
            leg[n_leg].label = "CFAR threshold";
            leg[n_leg].color = UI_C_SERIES_2;
            leg[n_leg].marker = UI_MARKER_NONE;
            ++n_leg;
        }

        /* Plots that fell in this dwell, marked on the trace. */
        ui_clip_push(&c->canvas, ax->box);
        {
            uint32_t i;
            for (i = 0u; i < a->frame.detection_count; ++i)
            {
                const PWR_Detection* d = &a->frame.detections[i];
                ui_marker(&c->canvas, UI_MARKER_TRIANGLE_DOWN,
                          ui_axes_x2px(ax, d->range_m * 1e-3),
                          ui_axes_y2px(ax, (double)d->amplitude_db) - 9.0,
                          9.0, UI_C_DETECTION, ui_color_alpha(UI_C_DETECTION, 90),
                          1.2);
            }
        }
        ui_clip_pop(&c->canvas);

        /* The gate feeding the spectrum display. */
        ui_plot_vline(&c->canvas, ax,
                      (a->frame.range_first_m +
                       (double)a->frame.cursor_range_bin * a->frame.range_step_m) * 1e-3,
                      ui_color_alpha(UI_C_CURSOR, 130), 1.0, 1);
    }

    ui_axes_draw_frame(&c->canvas, ax);
    ui_legend(&c->canvas, ax, leg, n_leg, 1);
    ui_axes_draw_overlay(&c->canvas, ax);
}

static void app_draw_rd(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Axes* ax = &a->ax_rd;
    const int32_t cb_w = 52;
    const UI_Rect plot = ui_rect(r.x, r.y, r.w - cb_w, r.h);

    ui_axes_layout(ax, plot);
    (void)ui_axes_input(c, ax, ui_id("ax.rd", 0));
    ui_axes_draw_bg(&c->canvas, ax);

    if (a->have_frame != 0 && a->frame.rd_map_db != NULL)
    {
        if (a->auto_clim != 0)
        {
            a->clim_rd_lo = a->frame.noise_floor_db - 1.0;
            a->clim_rd_hi = a->frame.noise_floor_db + 52.0;
        }
        ui_imagesc(&c->canvas, ax, a->frame.rd_map_db,
                   (int32_t)a->frame.doppler_bins, (int32_t)a->frame.range_bins,
                   a->clim_rd_lo, a->clim_rd_hi,
                   (UI_ColormapId)a->cmap_rd, a->smooth_rd);

        /* Zero-Doppler notch and the plots for this dwell. */
        ui_plot_hline(&c->canvas, ax, 0.0, UI_RGBA(0xFF, 0xFF, 0xFF, 45), 1.0, 1);
        ui_clip_push(&c->canvas, ax->box);
        {
            uint32_t i;
            for (i = 0u; i < a->frame.detection_count; ++i)
            {
                const PWR_Detection* d = &a->frame.detections[i];
                ui_marker(&c->canvas, UI_MARKER_CIRCLE,
                          ui_axes_x2px(ax, d->range_m * 1e-3),
                          ui_axes_y2px(ax, d->radial_velocity_mps),
                          11.0, UI_RGB(0xFF, 0xFF, 0xFF), UI_RGBA(0, 0, 0, 0), 1.3);
            }
        }
        ui_clip_pop(&c->canvas);

        ui_axes_set_title(ax, "Range-Doppler map   AZ %.2f\xC2\xB0   %u x %u",
                          a->frame.beam_azimuth_deg, a->frame.range_bins,
                          a->frame.doppler_bins);
    }

    ui_axes_draw_frame(&c->canvas, ax);
    ui_axes_draw_overlay(&c->canvas, ax);
    ui_colorbar(&c->canvas, ui_rect(r.x + r.w - cb_w + 4, ax->box.y,
                                    cb_w - 8, ax->box.h),
                (UI_ColormapId)a->cmap_rd, a->clim_rd_lo, a->clim_rd_hi, "dB");
}

static void app_draw_rti(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Axes* ax = &a->ax_rti;
    const int32_t cb_w = 52;
    const UI_Rect plot = ui_rect(r.x, r.y, r.w - cb_w, r.h);

    /* Unroll the engine's ring so the oldest row is first: the blitter puts
     * source row 0 at the bottom of the axes, which puts "now" at the top. */
    if (a->have_frame != 0 && a->frame.rti_db != NULL)
    {
        const uint32_t rows = a->frame.rti_rows;
        const uint32_t cols = a->frame.range_bins;
        uint32_t k;
        if (a->rti_view == NULL || a->rti_rows != rows || a->rti_cols != cols)
        {
            free(a->rti_view);
            a->rti_view = (float*)malloc((size_t)rows * cols * sizeof(float));
            a->rti_rows = rows;
            a->rti_cols = cols;
        }
        if (a->rti_view != NULL)
        {
            for (k = 0u; k < rows; ++k)
            {
                const uint32_t src = (a->frame.rti_head + 1u + k) % rows;
                memcpy(&a->rti_view[(size_t)k * cols],
                       &a->frame.rti_db[(size_t)src * cols],
                       (size_t)cols * sizeof(float));
            }
        }
        ui_axes_set_full(ax, a->frame.range_first_m * 1e-3,
                         (a->frame.range_first_m +
                          (double)cols * a->frame.range_step_m) * 1e-3,
                         -(double)rows * a->dm.cpi_duration_s, 0.0);
    }

    ui_axes_layout(ax, plot);
    (void)ui_axes_input(c, ax, ui_id("ax.rti", 0));
    ui_axes_draw_bg(&c->canvas, ax);

    if (a->rti_view != NULL && a->rti_rows > 0u)
    {
        if (a->auto_clim != 0)
        {
            /* The waterfall shows the max over Doppler, whose noise floor sits
             * ln(N_doppler) above the per-cell floor, so the lower limit is
             * lifted to match; otherwise the whole background reads mid-scale
             * and real returns lose their contrast. */
            const double lift = 10.0 * log10((double)
                ((a->frame.doppler_bins > 1u) ? a->frame.doppler_bins : 2u)) * 0.55;
            a->clim_rti_lo = a->frame.noise_floor_db + lift;
            a->clim_rti_hi = a->clim_rti_lo + 44.0;
        }
        ui_imagesc(&c->canvas, ax, a->rti_view, (int32_t)a->rti_rows,
                   (int32_t)a->rti_cols, a->clim_rti_lo, a->clim_rti_hi,
                   (UI_ColormapId)a->cmap_rti, 0);
    }
    ui_axes_draw_frame(&c->canvas, ax);
    ui_axes_draw_overlay(&c->canvas, ax);
    ui_colorbar(&c->canvas, ui_rect(r.x + r.w - cb_w + 4, ax->box.y,
                                    cb_w - 8, ax->box.h),
                (UI_ColormapId)a->cmap_rti, a->clim_rti_lo, a->clim_rti_hi, "dB");
}

static void app_draw_spectrum(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    UI_Axes* ax = &a->ax_spec;

    ui_axes_layout(ax, r);
    (void)ui_axes_input(c, ax, ui_id("ax.spec", 0));
    ui_axes_draw_bg(&c->canvas, ax);

    if (a->have_frame != 0 && a->frame.doppler_spectrum_db != NULL)
    {
        const double x0 = a->frame.velocity_first_mps;
        const double dx = a->frame.velocity_step_mps;
        const int32_t n = (int32_t)a->frame.doppler_bins;
        ui_plot_area(&c->canvas, ax, a->frame.doppler_spectrum_db, n, x0, dx,
                     ax->ymin, ui_color_alpha(UI_C_SERIES_0, 55));
        ui_plot_line(&c->canvas, ax, a->frame.doppler_spectrum_db, n, x0, dx,
                     UI_C_SERIES_0, 1.5);
        ui_plot_hline(&c->canvas, ax, a->frame.noise_floor_db,
                      ui_color_alpha(UI_C_SERIES_2, 170), 1.0, 1);
        ui_axes_set_title(ax, "Doppler spectrum   gate %u  (%.3f km)",
                          a->frame.cursor_range_bin,
                          (a->frame.range_first_m +
                           (double)a->frame.cursor_range_bin *
                           a->frame.range_step_m) * 1e-3);
    }
    ui_axes_draw_frame(&c->canvas, ax);
    ui_axes_draw_overlay(&c->canvas, ax);
}

/* ==========================================================================
 *  Help overlay
 * ========================================================================== */
static void app_draw_help(App* a)
{
    static const char* const lines[] = {
        "PWRadarSystem verification console",
        "",
        "Any plot",
        "   wheel                zoom about the pointer",
        "   Ctrl + wheel         zoom the x axis only",
        "   Shift + wheel        zoom the y axis only",
        "   left drag            rubber-band box zoom",
        "   middle drag          pan     (or Shift + left drag)",
        "   double click         zoom to fit",
        "   right click          lock or release the data cursor",
        "",
        "PPI scope",
        "   wheel                range scale",
        "   left click           select the nearest track",
        "   double click         full scale and clear the selection",
        "",
        "Keyboard",
        "   Space                run / pause",
        "   S                    single CPI step",
        "   A                    advance one full scan",
        "   R                    reset the scenario",
        "   T                    ground-truth overlay",
        "   G                    covariance ellipses",
        "   H                    history overlays (paths, plots)",
        "   1 .. 6               control panel tab",
        "   F1                   this help",
        "",
        "Displays are independent: zoom the range-Doppler map while the",
        "A-scope stays at full span, or vice versa.  Every level on the",
        "range-Doppler map, the A-scope and the waterfall is calibrated",
        "in dB of signal-to-noise ratio, so the noise floor sits at 0 dB."
    };
    const int32_t n = (int32_t)(sizeof(lines) / sizeof(lines[0]));
    UI_Context* c = &a->ui;
    const int32_t lh = ui_font_line_height(UI_FONT_SMALL);
    const int32_t w = 520;
    const int32_t h = n * lh + 2 * UI_M_PAD + 8;
    const UI_Rect box = ui_rect((c->canvas.w - w) / 2, (c->canvas.h - h) / 2, w, h);
    int32_t i;

    ui_fill_rect(&c->canvas, ui_rect(0, 0, c->canvas.w, c->canvas.h),
                 UI_RGBA(0, 0, 0, 120));
    ui_fill_round_rect(&c->canvas, box, 4.0, UI_RGBA(0x18, 0x1C, 0x24, 250));
    ui_frame_round_rect(&c->canvas, box, 4.0, UI_C_BORDER_HI);
    for (i = 0; i < n; ++i)
    {
        const int32_t y = box.y + UI_M_PAD + ui_font_ascent(UI_FONT_SMALL) + i * lh;
        const UI_FontId f = (i == 0) ? UI_FONT_BOLD : UI_FONT_SMALL;
        const UI_Color col = (i == 0) ? UI_C_TEXT
                           : ((lines[i][0] != '\0' && lines[i][0] != ' ')
                                  ? UI_C_FOCUS : UI_C_TEXT_DIM);
        (void)ui_text(&c->canvas, f, box.x + UI_M_PAD + 2, y, col, lines[i]);
    }
}

/* ==========================================================================
 *  Status bar
 * ========================================================================== */
static void app_draw_status(App* a, UI_Rect r)
{
    UI_Context* c = &a->ui;
    char buf[240];

    ui_fill_rect(&c->canvas, r, UI_C_PANEL);
    ui_hline(&c->canvas, r.x, r.x + r.w - 1, r.y, UI_C_BORDER);

    if (a->ppi.cursor_on != 0)
    {
        (void)snprintf(buf, sizeof(buf),
                       "PPI cursor  %.3f km   %05.1f\xC2\xB0   |   gate %u   "
                       "bin %.1f m   noise %.2f dB",
                       a->ppi.cursor_range_m * 1e-3, a->ppi.cursor_bearing_deg,
                       a->have_frame ? a->frame.cursor_range_bin : 0u,
                       a->dm.range_bin_spacing_m,
                       a->stats.measured_noise_floor_db);
    }
    else
    {
        (void)snprintf(buf, sizeof(buf),
                       "%u range bins @ %.1f m   %u Doppler bins @ %.2f m/s   "
                       "CPI %.2f ms   scan %.2f s   plots %u   tracks %u/%u",
                       a->dm.range_bins, a->dm.range_bin_spacing_m,
                       a->dm.doppler_bins,
                       a->have_frame ? fabs(a->frame.velocity_step_mps) : 0.0,
                       a->dm.cpi_duration_s * 1e3, a->dm.scan_period_s,
                       a->stats.detections_current, a->stats.tracks_confirmed,
                       a->stats.tracks_active);
    }
    (void)ui_text_in_rect(&c->canvas, UI_FONT_MONO, r, UI_M_PAD, UI_C_TEXT_DIM,
                          UI_ALIGN_LEFT, buf);

    (void)snprintf(buf, sizeof(buf), "F1 help   |   %.180s",
                   (a->cfg_err[0] != '\0') ? a->cfg_err : "ready");
    (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL, r, UI_M_PAD,
                          (a->cfg_err[0] != '\0') ? UI_C_ALARM : UI_C_TEXT_FAINT,
                          UI_ALIGN_RIGHT, buf);
}

/* ==========================================================================
 *  Keyboard shortcuts
 * ========================================================================== */
static void app_hotkeys(App* a)
{
    UI_Context* c = &a->ui;
    int32_t i;

    /* Swallowed while a text field has focus so typing never trips a shortcut. */
    if (c->edit_id != 0u) { return; }

    for (i = 0; i < c->n_keys; ++i)
    {
        switch (c->keys[i])
        {
        case UI_KEY_SPACE:
            if (pwr_engine_run_state(a->eng) == PWR_RUN_RUNNING)
            { (void)pwr_engine_pause(a->eng); }
            else { (void)pwr_engine_start(a->eng); }
            break;
        case 'S':
            (void)pwr_engine_pause(a->eng);
            a->step_pending += 1;
            break;
        case 'A':
        {
            const uint32_t n = (a->dm.cpi_duration_s > 0.0 &&
                                a->dm.scan_period_s > 0.0)
                ? (uint32_t)(a->dm.scan_period_s / a->dm.cpi_duration_s) : 32u;
            (void)pwr_engine_pause(a->eng);
            a->step_pending += (int32_t)n;
            break;
        }
        case 'R':
            (void)pwr_engine_reset(a->eng);
            app_history_clear(a);
            break;
        case 'T': a->ppi.show_truth = (a->ppi.show_truth != 0) ? 0 : 1; break;
        case 'G': a->ppi.show_gates = (a->ppi.show_gates != 0) ? 0 : 1; break;
        case 'H':
        {
            const int32_t on = (a->ppi.show_track_paths != 0) ? 0 : 1;
            a->ppi.show_track_paths = on;
            a->ppi.show_plot_hist   = on;
            break;
        }
        case '1': a->tab_ctrl = APP_TAB_WAVEFORM; break;
        case '2': a->tab_ctrl = APP_TAB_PROCESS;  break;
        case '3': a->tab_ctrl = APP_TAB_DETECT;   break;
        case '4': a->tab_ctrl = APP_TAB_TRACK;    break;
        case '5': a->tab_ctrl = APP_TAB_SCENARIO; break;
        case '6': a->tab_ctrl = APP_TAB_DISPLAY;  break;
        case UI_KEY_F1:
        case '/': a->show_help = (a->show_help != 0) ? 0 : 1; break;
        case UI_KEY_ESCAPE: a->show_help = 0; break;
        default: break;
        }
    }
}

/* ==========================================================================
 *  Framebuffer capture
 * ========================================================================== */
/* Writes through a sibling temporary and renames on success.
 *
 * The destination is the input of a headless regression check, so a partial
 * file is worse than no file: a run killed part-way, or a full disk, would
 * otherwise leave a truncated image with a valid-looking PPM header.  Every
 * write is checked - ignoring fwrite and fclose lets a failed flush report
 * success, which is the same failure wearing a different hat. */
int app_write_ppm(const App* a, const char* path)
{
    const uint32_t* px = ui_plat_pixels(a->plat);
    const int32_t w = ui_plat_width(a->plat);
    const int32_t h = ui_plat_height(a->plat);
    const int32_t stride = ui_plat_stride(a->plat);
    unsigned char* row;
    char*  tmp;
    size_t plen;
    FILE*  f;
    int32_t y, x;
    int ok = 1;

    if (px == NULL || w < 1 || h < 1 || path == NULL) { return 0; }

    plen = strlen(path);
    tmp  = (char*)malloc(plen + 5u);
    if (tmp == NULL) { return 0; }
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5u);

    f = fopen(tmp, "wb");
    if (f == NULL) { free(tmp); return 0; }
    row = (unsigned char*)malloc((size_t)w * 3u);
    if (row == NULL) { (void)fclose(f); (void)remove(tmp); free(tmp); return 0; }

    if (fprintf(f, "P6\n%d %d\n255\n", (int)w, (int)h) < 0) { ok = 0; }
    for (y = 0; ok != 0 && y < h; ++y)
    {
        const uint32_t* src = &px[(size_t)(uint32_t)y * (uint32_t)stride];
        for (x = 0; x < w; ++x)
        {
            row[x * 3 + 0] = (unsigned char)((src[x] >> 16) & 0xFFu);
            row[x * 3 + 1] = (unsigned char)((src[x] >> 8)  & 0xFFu);
            row[x * 3 + 2] = (unsigned char)( src[x]        & 0xFFu);
        }
        if (fwrite(row, 1u, (size_t)w * 3u, f) != (size_t)w * 3u) { ok = 0; }
    }
    free(row);
    /* fclose flushes; a write error can surface only here. */
    if (fclose(f) != 0) { ok = 0; }

    if (ok != 0)
    {
        /* rename refuses an existing destination on Windows, so clear it
         * first.  The window between the two is the price of staying inside
         * ISO C rather than reaching for MoveFileEx. */
        (void)remove(path);
        if (rename(tmp, path) != 0) { ok = 0; }
    }
    if (ok == 0) { (void)remove(tmp); }
    free(tmp);
    return ok;
}

/* ==========================================================================
 *  Frame
 * ========================================================================== */
int app_step(App* a)
{
    UI_Event ev;
    const double t_begin = ui_plat_now_s();
    int32_t W, H;

    /* ---- input ---------------------------------------------------------- */
    while (ui_plat_poll(a->plat, &ev) != 0)
    {
        if (ev.type == UI_EV_QUIT) { a->running = 0; return 0; }
        ui_ctx_event(&a->ui, &ev);
    }

    /* ---- queued single-stepping ------------------------------------------
     *  STEP / +1 SCAN only queue CPIs; they are drained here with a ~20 ms
     *  budget per UI frame, sized from the measured CPI time.  A full-scan
     *  step (a couple of hundred CPIs) therefore animates at interactive
     *  frame rates instead of freezing the console for seconds. */
    if (pwr_engine_run_state(a->eng) == PWR_RUN_RUNNING)
    {
        a->step_pending = 0;
    }
    else if (a->step_pending > 0)
    {
        const double per_cpi_ms = app_max(a->stats.t_total_ms, 0.1);
        int32_t chunk = (int32_t)(20.0 / per_cpi_ms);
        if (chunk < 1)  { chunk = 1; }
        if (chunk > 32) { chunk = 32; }
        if (chunk > a->step_pending) { chunk = a->step_pending; }
        (void)pwr_engine_step(a->eng, (uint32_t)chunk);
        a->step_pending -= chunk;
    }

    /* ---- newest radar frame --------------------------------------------- */
    if (a->have_frame != 0)
    {
        (void)pwr_engine_frame_release(a->eng);
        a->have_frame = 0;
    }
    if (pwr_engine_frame_acquire(a->eng, &a->frame) == PWR_STATUS_OK)
    {
        a->have_frame = 1;
        a->stats      = a->frame.stats;
        a->dm         = a->frame.metrics;
        if (a->frame.sequence != a->hist_last_seq)
        {
            app_history_feed(a);
            a->hist_last_seq = a->frame.sequence;
        }
    }
    else
    {
        (void)pwr_engine_get_stats(a->eng, &a->stats);
    }
    a->ppi.hist_retain_s = a->hist_retain_s;

    W = ui_plat_width(a->plat);
    H = ui_plat_height(a->plat);
    if (W < 8 || H < 8) { ui_plat_sleep_s(0.02); return 1; }

    ui_frame_begin(&a->ui, ui_plat_pixels(a->plat), W, H,
                   ui_plat_stride(a->plat), t_begin);
    app_hotkeys(a);
    ui_clear(&a->ui.canvas, UI_C_DESK);

    /* ---- layout --------------------------------------------------------- */
    {
        const int32_t sp = UI_M_SPLITTER;
        const UI_Rect toolbar = ui_rect(0, 0, W, UI_M_TOOLBAR_H);
        const UI_Rect status  = ui_rect(0, H - UI_M_STATUS_H, W, UI_M_STATUS_H);
        const int32_t body_y  = toolbar.h;
        const int32_t body_h  = H - toolbar.h - status.h;
        int32_t lw = (int32_t)((double)W * a->f_left);
        int32_t rw = (int32_t)((double)W * a->f_right);
        UI_Rect left, centre, right;

        if (lw < 220) { lw = 220; }
        if (rw < 260) { rw = 260; }
        if (lw + rw + 320 > W) { rw = W - lw - 320; if (rw < 200) { rw = 200; } }

        left   = ui_rect(0, body_y, lw, body_h);
        centre = ui_rect(lw + sp, body_y, W - lw - rw - 2 * sp, body_h);
        right  = ui_rect(W - rw, body_y, rw, body_h);

        app_draw_toolbar(a, toolbar);

        /* ---- control panel ---------------------------------------------- */
        {
            static const char* const tabs[APP_TAB_COUNT] = {
                "RF", "DSP", "CFAR", "Track", "Scene", "View"
            };
            const int32_t tab_h = 24;
            UI_Rect body;
            ui_fill_rect(&a->ui.canvas, left, UI_C_PANEL);
            (void)ui_tabs(&a->ui, ui_id("ct.tabs", 0),
                          ui_rect(left.x, left.y, left.w, tab_h),
                          &a->tab_ctrl, tabs, APP_TAB_COUNT);
            body = ui_rect(left.x + UI_M_PAD, left.y + tab_h + UI_M_GAP,
                           left.w - 2 * UI_M_PAD,
                           left.h - tab_h - UI_M_GAP - UI_M_PAD);
            ui_clip_push(&a->ui.canvas, body);
            switch (a->tab_ctrl)
            {
            case APP_TAB_WAVEFORM: app_panel_waveform(a, body); break;
            case APP_TAB_PROCESS:  app_panel_process(a, body);  break;
            case APP_TAB_DETECT:   app_panel_detect(a, body);   break;
            case APP_TAB_TRACK:    app_panel_track(a, body);    break;
            case APP_TAB_SCENARIO: app_panel_scenario(a, body); break;
            case APP_TAB_DISPLAY:  app_panel_display(a, body);  break;
            default: break;
            }
            ui_clip_pop(&a->ui.canvas);
            ui_frame_rect(&a->ui.canvas, left, UI_C_BORDER);
        }

        (void)ui_splitter_v(&a->ui, ui_id("sp.left", 0),
                            ui_rect(left.x + left.w, left.y, sp, left.h),
                            W, &a->f_left, 0.12, 0.35);
        /* The right pane grows as the splitter is dragged left, so the extent
         * passed here is negated to flip the sign of the drag. */
        (void)ui_splitter_v(&a->ui, ui_id("sp.right", 0),
                            ui_rect(right.x - sp, right.y, sp, right.h),
                            -W, &a->f_right, 0.15, 0.40);

        /* ---- centre: four displays -------------------------------------- */
        {
            int32_t cvw = (int32_t)((double)centre.w * a->f_centre_v);
            int32_t chh = (int32_t)((double)centre.h * a->f_centre_h);
            UI_Rect ul, ur, ll, lr;

            if (cvw < 200) { cvw = 200; }
            if (cvw > centre.w - 200) { cvw = centre.w - 200; }
            if (chh < 140) { chh = 140; }
            if (chh > centre.h - 140) { chh = centre.h - 140; }

            ul = ui_rect(centre.x, centre.y, cvw, chh);
            ur = ui_rect(centre.x + cvw + sp, centre.y,
                         centre.w - cvw - sp, chh);
            ll = ui_rect(centre.x, centre.y + chh + sp, cvw,
                         centre.h - chh - sp);
            lr = ui_rect(centre.x + cvw + sp, centre.y + chh + sp,
                         centre.w - cvw - sp, centre.h - chh - sp);

            /* PPI */
            if (a->have_frame != 0)
            {
                const double full = a->frame.range_first_m +
                    (double)a->frame.range_bins * a->frame.range_step_m;
                if (fabs(a->ppi.range_max_full_m - full) > 1.0)
                {
                    a->ppi.range_max_full_m = full;
                    if (a->ppi.range_max_m > full) { a->ppi.range_max_m = full; }
                }
            }
            ui_ppi_layout(&a->ppi, ul);
            (void)ui_ppi_input(&a->ui, &a->ppi, ui_id("ppi", 0),
                               (a->have_frame != 0) ? &a->frame : NULL);
            ui_ppi_draw(&a->ui.canvas, &a->ppi,
                        (a->have_frame != 0) ? &a->frame : NULL);
            ui_frame_rect(&a->ui.canvas, ul, UI_C_BORDER);

            /* Range-Doppler */
            ui_fill_rect(&a->ui.canvas, ur, UI_C_PANEL);
            app_draw_rd(a, ui_rect_inset(ur, 4, 4));
            ui_frame_rect(&a->ui.canvas, ur, UI_C_BORDER);

            /* A-scope */
            ui_fill_rect(&a->ui.canvas, ll, UI_C_PANEL);
            app_draw_scope(a, ui_rect_inset(ll, 4, 4));
            ui_frame_rect(&a->ui.canvas, ll, UI_C_BORDER);

            /* RTI or spectrum, selectable */
            ui_fill_rect(&a->ui.canvas, lr, UI_C_PANEL);
            {
                static const char* const t2[APP_LR_COUNT] = {
                    "RTI waterfall", "Doppler spectrum"
                };
                const UI_Rect tb = ui_rect(lr.x + 1, lr.y + 1, lr.w - 2, 22);
                (void)ui_tabs(&a->ui, ui_id("lr.tabs", 0), tb,
                              &a->lower_right, t2, APP_LR_COUNT);
                if (a->lower_right == APP_LR_RTI)
                {
                    app_draw_rti(a, ui_rect(lr.x + 4, lr.y + 26,
                                            lr.w - 8, lr.h - 30));
                }
                else
                {
                    app_draw_spectrum(a, ui_rect(lr.x + 4, lr.y + 26,
                                                 lr.w - 8, lr.h - 30));
                }
            }
            ui_frame_rect(&a->ui.canvas, lr, UI_C_BORDER);

            (void)ui_splitter_v(&a->ui, ui_id("sp.cv", 0),
                                ui_rect(centre.x + cvw, centre.y, sp, centre.h),
                                centre.w, &a->f_centre_v, 0.20, 0.80);
            (void)ui_splitter_h(&a->ui, ui_id("sp.ch", 0),
                                ui_rect(centre.x, centre.y + chh, centre.w, sp),
                                centre.h, &a->f_centre_h, 0.20, 0.80);
        }

        /* ---- right pane -------------------------------------------------- */
        {
            int32_t th = (int32_t)((double)right.h * a->f_right_h);
            if (th < 150) { th = 150; }
            if (th > right.h - 150) { th = right.h - 150; }
            app_draw_right(a, ui_rect(right.x, right.y, right.w, th));
            app_draw_readouts(a, ui_rect(right.x + 2, right.y + th + sp,
                                         right.w - 4, right.h - th - sp - 2));
            (void)ui_splitter_h(&a->ui, ui_id("sp.rh", 0),
                                ui_rect(right.x, right.y + th, right.w, sp),
                                right.h, &a->f_right_h, 0.25, 0.85);
        }

        app_draw_status(a, status);
    }

    if (a->show_help != 0) { app_draw_help(a); }

    ui_frame_end(&a->ui);
    ui_plat_set_cursor(a->plat, a->ui.cursor);
    ui_plat_present(a->plat);

    /* ---- deferred reconfiguration --------------------------------------- */
    if (a->cfg_dirty != 0 && a->ui.active == 0u && a->ui.edit_id == 0u)
    {
        char err[PWR_ERRMSG_LEN];
        const PWR_Status st = pwr_engine_reconfigure(a->eng, &a->cfg,
                                                     err, sizeof(err));
        if (st != PWR_STATUS_OK)
        {
            (void)snprintf(a->cfg_err, sizeof(a->cfg_err), "%s", err);
            app_logf(a, "[WARN ] configuration rejected: %s", err);
            (void)pwr_engine_get_config(a->eng, &a->cfg);   /* roll back */
        }
        else
        {
            a->cfg_err[0] = '\0';
            (void)pwr_engine_get_config(a->eng, &a->cfg);
            (void)pwr_engine_get_metrics(a->eng, &a->dm);
            ui_axes_set_full(&a->ax_rd,
                             a->frame.range_first_m * 1e-3,
                             (a->dm.range_bin_spacing_m *
                              (double)a->dm.range_bins) * 1e-3,
                             -a->dm.unambiguous_velocity_mps,
                              a->dm.unambiguous_velocity_mps);
            ui_axes_set_full(&a->ax_spec, -a->dm.unambiguous_velocity_mps,
                             a->dm.unambiguous_velocity_mps, -10.0, 90.0);
            ui_axes_set_full(&a->ax_scope, 0.0,
                             (a->dm.range_bin_spacing_m *
                              (double)a->dm.range_bins) * 1e-3, -10.0, 90.0);
            if (a->cursor_bin_ui >= (int32_t)a->dm.range_bins)
            {
                a->cursor_bin_ui = (int32_t)a->dm.range_bins / 2;
                (void)pwr_engine_set_cursor_range_bin(a->eng,
                                                      (uint32_t)a->cursor_bin_ui);
            }
        }
        a->cfg_dirty = 0;
    }

    /* ---- pace the presentation ------------------------------------------ */
    {
        const double dt = ui_plat_now_s() - t_begin;
        const double target = 1.0 / 60.0;
        a->fps = (a->last_frame_s > 0.0)
            ? (0.9 * a->fps + 0.1 / app_max(t_begin - a->last_frame_s, 1e-6))
            : 60.0;
        a->last_frame_s = t_begin;
        if (dt < target) { ui_plat_sleep_s(target - dt); }
    }
    return a->running;
}
