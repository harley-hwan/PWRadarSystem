/* ==========================================================================
 *  PWRadarSystem - PWRadarCore
 *  ------------------------------------------------------------------------
 *  File    : pwr_engine.c
 *  Purpose : Engine life cycle, buffer management, the real-time worker, the
 *            triple-buffered frame publication scheme, and every exported
 *            entry point declared in pwr_api.h.
 *
 *  Frame publication
 *  -----------------
 *  Three identical frame stores rotate.  The producer always writes into a
 *  slot that is neither the newest published slot nor the slot a consumer is
 *  holding, so it never blocks and the consumer never sees a torn frame.
 *  With three slots such a target always exists.
 *
 *  Language: ISO C17
 * ========================================================================== */
#include "pwr_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 *  Diagnostics
 * ========================================================================== */
void pwr_log(struct PWR_Engine* e, PWR_LogLevel lvl, const char* fmt, ...)
{
    char    buf[PWR_ERRMSG_LEN];
    va_list ap;

    if (e == NULL || e->log_fn == NULL || (int32_t)lvl < e->log_level) { return; }
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    e->log_fn(e->log_user, (int32_t)lvl, buf);
}

void pwr_set_error(struct PWR_Engine* e, const char* fmt, ...)
{
    va_list ap;
    if (e == NULL) { return; }
    va_start(ap, fmt);
    (void)vsnprintf(e->err, sizeof(e->err), fmt, ap);
    va_end(ap);
    pwr_log(e, PWR_LOG_ERROR, "%s", e->err);
}

PWR_EXPORT(const char*) pwr_engine_last_error(const PWR_Engine* eng)
{
    return (eng != NULL) ? eng->err : "null engine";
}

/* --------------------------------------------------------------------------
 *  Fair acquisition of proc_lock for blocking mutators
 *  ---------------------------------------------------
 *  CRITICAL_SECTION and pthread mutexes are unfair: releasing one does not
 *  hand it to a waiter, it merely wakes the waiter, and by the time that
 *  thread is scheduled a saturated worker has long since re-locked for the
 *  next CPI.  A mutator blocked in plain pwr_mutex_lock() can therefore lose
 *  that race for seconds on end.  Raising mutator_waiting first makes the
 *  worker stand aside between CPIs until the mutator holds the lock, which
 *  bounds every blocking mutator at one CPI of latency.
 * ------------------------------------------------------------------------ */
static void pwr_engine_lock_proc_fair(PWR_Engine* e)
{
    (void)pwr_atomic_add_i32(&e->mutator_waiting, 1);
    pwr_mutex_lock(&e->proc_lock);
    (void)pwr_atomic_add_i32(&e->mutator_waiting, -1);
}

PWR_EXPORT(PWR_Status) pwr_engine_set_log(PWR_Engine* eng, PWR_LogFn fn,
                                          void* user, PWR_LogLevel min_level)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_engine_lock_proc_fair(eng);
    eng->log_fn    = fn;
    eng->log_user  = user;
    eng->log_level = (int32_t)min_level;
    pwr_mutex_unlock(&eng->proc_lock);
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Buffer management
 * ========================================================================== */
static void pwr_frames_free_one(PWR_FrameStore* fs)
{
    PWR_FREE(fs->range_profile_db);
    PWR_FREE(fs->range_profile_raw_db);
    PWR_FREE(fs->cfar_threshold_db);
    PWR_FREE(fs->doppler_spectrum_db);
    PWR_FREE(fs->rd_map_db);
    PWR_FREE(fs->ppi_video);
    PWR_FREE(fs->rti_db);
    PWR_FREE(fs->detections);
    PWR_FREE(fs->tracks);
    PWR_FREE(fs->truth);
    memset(&fs->header, 0, sizeof(fs->header));
}

void pwr_frames_release(struct PWR_Engine* e)
{
    uint32_t i;
    if (e == NULL) { return; }
    for (i = 0u; i < PWR_FRAME_SLOTS; ++i) { pwr_frames_free_one(&e->frames[i]); }
    e->publish_index = -1;
    e->write_index   = 0;
    e->held_index    = -1;
}

PWR_Status pwr_frames_alloc(struct PWR_Engine* e)
{
    uint32_t i;
    const uint32_t nr = e->n_range;
    const uint32_t nd = e->n_doppler;

    pwr_frames_release(e);
    for (i = 0u; i < PWR_FRAME_SLOTS; ++i)
    {
        PWR_FrameStore* fs = &e->frames[i];
        fs->range_profile_db     = PWR_ALLOC_ARRAY(pwr_real, nr);
        fs->range_profile_raw_db = PWR_ALLOC_ARRAY(pwr_real, nr);
        fs->cfar_threshold_db    = PWR_ALLOC_ARRAY(pwr_real, nr);
        fs->doppler_spectrum_db  = PWR_ALLOC_ARRAY(pwr_real, nd);
        fs->rd_map_db            = PWR_ALLOC_ARRAY(pwr_real, (size_t)nr * nd);
        fs->ppi_video            = PWR_ALLOC_ARRAY(uint8_t,  (size_t)nr * e->ppi_cells);
        fs->rti_db               = PWR_ALLOC_ARRAY(pwr_real, (size_t)nr * e->rti_rows);
        fs->detections           = PWR_ALLOC_ARRAY(PWR_Detection, PWR_MAX_DETECTIONS);
        fs->tracks               = PWR_ALLOC_ARRAY(PWR_Track,     PWR_MAX_TRACKS);
        fs->truth                = PWR_ALLOC_ARRAY(PWR_SimTarget, PWR_MAX_SIM_TARGETS);
        fs->refcount             = 0;
        if (fs->range_profile_db == NULL || fs->range_profile_raw_db == NULL ||
            fs->cfar_threshold_db == NULL || fs->doppler_spectrum_db == NULL ||
            fs->rd_map_db == NULL || fs->ppi_video == NULL ||
            fs->rti_db == NULL || fs->detections == NULL ||
            fs->tracks == NULL || fs->truth == NULL)
        {
            pwr_frames_release(e);
            return PWR_ERR_OUT_OF_MEMORY;
        }
    }
    return PWR_STATUS_OK;
}

static void pwr_engine_free_buffers(PWR_Engine* e)
{
    PWR_FREE(e->rx);
    PWR_FREE(e->pc);
    PWR_FREE(e->slow);
    PWR_FREE(e->fast_scratch);
    PWR_FREE(e->rd_pow);
    PWR_FREE(e->rd_db);
    PWR_FREE(e->profile_pow);
    PWR_FREE(e->profile_raw);
    PWR_FREE(e->thresh_prof);
    PWR_FREE(e->stc_gain);
    PWR_FREE(e->ppi_accum);
    PWR_FREE(e->rti);
    PWR_FREE(e->cluster_label);
    PWR_FREE(e->cluster_stack);
    PWR_FREE(e->doppler_window);

    if (e->plan_fast != NULL) { pwr_fft_plan_destroy(e->plan_fast); e->plan_fast = NULL; }
    if (e->plan_slow != NULL) { pwr_fft_plan_destroy(e->plan_slow); e->plan_slow = NULL; }
    pwr_waveform_release(&e->wf);
    pwr_cfar_release(&e->cfar);
    pwr_sim_release(&e->sim);
    pwr_frames_release(e);
}

/* Computes every cached dimension and the dB calibration constants. */
static void pwr_engine_compute_dims(PWR_Engine* e)
{
    const PWR_RadarConfig* cfg = &e->cfg;
    const double bin_raw = PWR_C_LIGHT / (2.0 * cfg->sample_rate_hz);
    uint32_t order = 0u;

    e->n_pulses   = cfg->pulses_per_cpi;
    e->n_samples  = e->dm.samples_per_pri;
    e->n_range    = e->dm.range_bins;
    e->n_doppler  = cfg->doppler_bins;
    e->n_fast_fft = e->dm.fast_time_fft_size;
    e->ppi_cells  = cfg->ppi_azimuth_cells;
    e->rti_rows   = cfg->rti_rows;

    e->range_decim  = (uint32_t)(e->dm.range_bin_spacing_m / bin_raw + 0.5);
    if (e->range_decim < 1u) { e->range_decim = 1u; }
    e->range_offset = (uint32_t)(cfg->range_start_m / bin_raw + 0.5);
    /* Guarantee the last accessed sample stays inside the receive window. */
    while (e->n_range > 1u &&
           e->range_offset + (e->n_range - 1u) * e->range_decim >= e->n_samples)
    {
        --e->n_range;
    }
    e->dm.range_bins = e->n_range;

    switch (cfg->mti_mode)
    {
    case PWR_MTI_TWO_PULSE:   order = 1u; break;
    case PWR_MTI_THREE_PULSE: order = 2u; break;
    case PWR_MTI_FOUR_PULSE:  order = 3u; break;
    default:                  order = 0u; break;
    }
    e->n_pulses_valid = (e->n_pulses > order) ? (e->n_pulses - order) : e->n_pulses;
    e->mti_offset     = 0u;

    /* ---- published range axis --------------------------------------------
     *  The velocity axis lives in pwr_engine_refresh_calibration() instead:
     *  it depends on the carrier through dm.wavelength_m, which can change on
     *  the lightweight reconfiguration path where this function never runs. */
    e->axis_range_first_m = (double)e->range_offset * bin_raw;
    e->axis_range_step_m  = e->dm.range_bin_spacing_m;

    if (e->cursor_range_bin >= e->n_range)
    {
        e->cursor_range_bin = e->n_range / 2u;
    }
}

/* Rebuilds the R^2 sensitivity-time-control ramp for the current geometry.
 * Shared by the calibration refresh and pwr_engine_set_stc() so the two can
 * never drift apart. */
static void pwr_engine_update_stc_gain(PWR_Engine* e)
{
    uint32_t i;
    if (e->stc_gain == NULL) { return; }
    for (i = 0u; i < e->n_range; ++i)
    {
        const double r = e->axis_range_first_m + (double)i * e->axis_range_step_m;
        double g = 1.0;
        if (e->cfg.stc_range_m > 1.0)
        {
            g = (r / e->cfg.stc_range_m) * (r / e->cfg.stc_range_m);
            g = pwr_clampd(g, 1.0e-3, 1.0);
        }
        e->stc_gain[i] = (pwr_real)g;
    }
}

/* Recomputes everything that depends on the tapers, the compression filter,
 * the carrier or the STC setting but not on any buffer dimension.  Called both
 * from the full allocation path and from the lightweight reconfiguration path,
 * so a change of taper never has to discard the PPI history or the track file. */
static void pwr_engine_refresh_calibration(PWR_Engine* e)
{
    uint32_t i;

    (void)pwr_window_generate((PWR_WindowType)e->cfg.doppler_window,
                              e->doppler_window, e->n_pulses_valid);
    for (i = e->n_pulses_valid; i < e->n_pulses; ++i) { e->doppler_window[i] = 0.0f; }
    e->doppler_win_cg   = pwr_window_coherent_gain(e->doppler_window, e->n_pulses_valid);
    e->doppler_win_enbw = pwr_window_enbw(e->doppler_window, e->n_pulses_valid);

    /* Measured, not assumed: pwr_waveform_build() normalises the filter for a
     * unit compressed peak and reports the exact amplitude gain it then applies
     * to unit-variance white input. */
    e->sigma_pc = (e->cfg.enable_pulse_compression != 0) ? e->wf.noise_gain : 1.0;
    {
        double s2 = 0.0;
        for (i = 0u; i < e->n_pulses_valid; ++i)
        {
            s2 += (double)e->doppler_window[i] * (double)e->doppler_window[i];
        }
        e->doppler_norm = 1.0 / (e->sigma_pc * sqrt(pwr_maxd(s2, 1e-30)));
    }

    /* Published velocity axis.  Recomputed here rather than with the buffer
     * dimensions so that a carrier-only reconfiguration (same geometry, new
     * wavelength) still recalibrates the Doppler axis. */
    e->axis_vel_step_mps  = e->dm.wavelength_m * e->cfg.prf_hz /
                            (2.0 * (double)e->n_doppler);
    e->axis_vel_first_mps = e->axis_vel_step_mps *
                            (1.0 - 0.5 * (double)e->n_doppler);

    pwr_engine_update_stc_gain(e);
}

static PWR_Status pwr_engine_alloc_buffers(PWR_Engine* e)
{
    PWR_Status st;
    uint32_t i;

    pwr_engine_free_buffers(e);
    pwr_engine_compute_dims(e);
    /* Open dwell plots carry the old grid's units; start afresh. */
    memset(e->dwell, 0, sizeof(e->dwell));

    st = pwr_fft_plan_create(&e->plan_fast, e->n_fast_fft);
    if (st != PWR_STATUS_OK) { pwr_set_error(e, "fast-time FFT plan (%u) failed", e->n_fast_fft); return st; }
    st = pwr_fft_plan_create(&e->plan_slow, e->n_doppler);
    if (st != PWR_STATUS_OK) { pwr_set_error(e, "Doppler FFT plan (%u) failed", e->n_doppler); return st; }

    st = pwr_waveform_build(&e->wf, &e->cfg, &e->dm, e->plan_fast);
    if (st != PWR_STATUS_OK) { pwr_set_error(e, "waveform construction failed"); return st; }

    e->rx           = PWR_ALLOC_ARRAY(PWR_Complex, (size_t)e->n_pulses * e->n_samples);
    e->pc           = PWR_ALLOC_ARRAY(PWR_Complex, (size_t)e->n_pulses * e->n_range);
    e->slow         = PWR_ALLOC_ARRAY(PWR_Complex, (size_t)e->n_range  * e->n_doppler);
    e->fast_scratch = PWR_ALLOC_ARRAY(PWR_Complex, e->n_fast_fft);
    e->rd_pow       = PWR_ALLOC_ARRAY(pwr_real,    (size_t)e->n_doppler * e->n_range);
    e->rd_db        = PWR_ALLOC_ARRAY(pwr_real,    (size_t)e->n_doppler * e->n_range);
    e->profile_pow  = PWR_ALLOC_ARRAY(pwr_real,    e->n_range);
    e->profile_raw  = PWR_ALLOC_ARRAY(pwr_real,    e->n_range);
    e->thresh_prof  = PWR_ALLOC_ARRAY(pwr_real,    e->n_range);
    e->stc_gain     = PWR_ALLOC_ARRAY(pwr_real,    e->n_range);
    e->ppi_accum    = PWR_ALLOC_ARRAY(uint16_t,    (size_t)e->ppi_cells * e->n_range);
    e->rti          = PWR_ALLOC_ARRAY(pwr_real,    (size_t)e->rti_rows * e->n_range);
    e->cluster_label = PWR_ALLOC_ARRAY(int32_t,    (size_t)e->n_doppler * e->n_range);
    e->cluster_stack = PWR_ALLOC_ARRAY(int32_t,    (size_t)e->n_doppler * e->n_range);
    e->doppler_window = PWR_ALLOC_ARRAY(pwr_real,  e->n_pulses);

    if (e->rx == NULL || e->pc == NULL || e->slow == NULL ||
        e->fast_scratch == NULL || e->rd_pow == NULL || e->rd_db == NULL ||
        e->profile_pow == NULL || e->profile_raw == NULL ||
        e->thresh_prof == NULL || e->stc_gain == NULL ||
        e->ppi_accum == NULL || e->rti == NULL ||
        e->cluster_label == NULL || e->cluster_stack == NULL ||
        e->doppler_window == NULL)
    {
        pwr_set_error(e, "out of memory allocating the data cube");
        return PWR_ERR_OUT_OF_MEMORY;
    }

    pwr_engine_refresh_calibration(e);

    st = pwr_cfar_alloc(&e->cfar, e->n_doppler, e->n_range, &e->cfg.cfar);
    if (st != PWR_STATUS_OK) { pwr_set_error(e, "CFAR workspace allocation failed"); return st; }

    st = pwr_sim_init(&e->sim, &e->cfg, e->n_range);
    if (st != PWR_STATUS_OK) { pwr_set_error(e, "simulator allocation failed"); return st; }

    st = pwr_frames_alloc(e);
    if (st != PWR_STATUS_OK) { pwr_set_error(e, "frame store allocation failed"); return st; }

    /* Neutral initial content so the first UI paint is well defined. */
    for (i = 0u; i < e->n_range; ++i)
    {
        e->profile_pow[i] = 1.0f;
        e->profile_raw[i] = 0.0f;
        e->thresh_prof[i] = 0.0f;
    }
    {
        const size_t total = (size_t)e->n_doppler * e->n_range;
        size_t k;
        for (k = 0u; k < total; ++k) { e->rd_pow[k] = 1.0f; e->rd_db[k] = 0.0f; }
    }
    {
        const size_t total = (size_t)e->rti_rows * e->n_range;
        size_t k;
        for (k = 0u; k < total; ++k) { e->rti[k] = PWR_DB_FLOOR; }
    }

    pwr_log(e, PWR_LOG_INFO,
            "geometry: %u pulses x %u samples -> %u range bins x %u Doppler bins "
            "(fast FFT %u, decim %u); compression: PSL %.1f dB, mainlobe %.2f "
            "bins, mismatch loss %.2f dB",
            e->n_pulses, e->n_samples, e->n_range, e->n_doppler,
            e->n_fast_fft, e->range_decim, e->wf.sidelobe_db,
            e->wf.mainlobe_bins, e->wf.mismatch_loss_db);
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Frame publication
 * ========================================================================== */
static int32_t pwr_frame_begin(PWR_Engine* e)
{
    int32_t chosen = -1;
    int32_t i;
    pwr_mutex_lock(&e->frame_lock);
    for (i = 0; i < (int32_t)PWR_FRAME_SLOTS; ++i)
    {
        if (i == e->publish_index || i == e->held_index) { continue; }
        chosen = i;
        break;
    }
    if (chosen < 0) { chosen = 0; }
    e->write_index = chosen;
    pwr_mutex_unlock(&e->frame_lock);
    return chosen;
}

void pwr_frame_publish(struct PWR_Engine* e)
{
    const int32_t slot = pwr_frame_begin(e);
    PWR_FrameStore* fs = &e->frames[slot];
    PWR_Frame* h  = &fs->header;
    const uint32_t nr = e->n_range;
    const uint32_t nd = e->n_doppler;
    uint32_t i, n;

    /* ---- 1-D products --------------------------------------------------- */
    for (i = 0u; i < nr; ++i)
    {
        fs->range_profile_db[i]     = pwr_pow_to_db(e->profile_pow[i]);
        fs->range_profile_raw_db[i] = pwr_pow_to_db(e->profile_raw[i]);
        fs->cfar_threshold_db[i]    = e->thresh_prof[i];
    }
    {
        const uint32_t cb = pwr_minu(e->cursor_range_bin, nr - 1u);
        for (i = 0u; i < nd; ++i)
        {
            fs->doppler_spectrum_db[i] = e->rd_db[(size_t)i * nr + cb];
        }
        h->cursor_range_bin = cb;
    }

    /* ---- 2-D products --------------------------------------------------- */
    memcpy(fs->rd_map_db, e->rd_db, (size_t)nr * nd * sizeof(pwr_real));
    {
        /* Narrow the Q8 accumulator into the published 8-bit video. */
        const size_t total = (size_t)nr * e->ppi_cells;
        size_t k;
        for (k = 0u; k < total; ++k)
        {
            fs->ppi_video[k] = (uint8_t)(e->ppi_accum[k] >> 8u);
        }
    }
    memcpy(fs->rti_db, e->rti, (size_t)nr * e->rti_rows * sizeof(pwr_real));
    h->rti_head = e->rti_head;

    /* ---- detections ----------------------------------------------------- */
    n = pwr_minu(e->detection_count, PWR_MAX_DETECTIONS);
    memcpy(fs->detections, e->detections, (size_t)n * sizeof(PWR_Detection));
    h->detections      = fs->detections;
    h->detection_count = n;

    /* ---- tracks --------------------------------------------------------- */
    {
        uint32_t out = 0u;
        uint32_t confirmed = 0u;
        for (i = 0u; i < PWR_MAX_TRACKS; ++i)
        {
            const PWR_TrackInternal* tk = &e->tracker.tracks[i];
            PWR_Track* dst;
            double sp;
            if (tk->state == PWR_TRACK_FREE) { continue; }
            dst = &fs->tracks[out++];
            memset(dst, 0, sizeof(*dst));
            dst->x_m   = tk->X[0];
            dst->y_m   = tk->X[1];
            dst->vx_mps = tk->X[2];
            dst->vy_mps = tk->X[3];
            dst->range_m     = sqrt(tk->X[0] * tk->X[0] + tk->X[1] * tk->X[1]);
            dst->azimuth_deg = pwr_wrap360(pwr_rad_to_deg(atan2(tk->X[0], tk->X[1])));
            sp = sqrt(tk->X[2] * tk->X[2] + tk->X[3] * tk->X[3]);
            dst->speed_mps  = sp;
            dst->course_deg = (sp >= e->cfg.tracker.min_speed_for_course)
                ? pwr_wrap360(pwr_rad_to_deg(atan2(tk->X[2], tk->X[3])))
                : 0.0;
            dst->radial_velocity_mps = tk->radial_velocity_mps;
            dst->pos_cov[0] = tk->P[0];
            dst->pos_cov[1] = tk->P[1];
            dst->pos_cov[2] = tk->P[4];
            dst->pos_cov[3] = tk->P[5];
            dst->first_time_s       = tk->first_time_s;
            dst->last_time_s        = e->scenario_time_s;
            dst->last_update_time_s = tk->last_update_time_s;
            dst->quality = pwr_clampd((double)tk->hits /
                (double)pwr_maxu(tk->update_attempts, 1u), 0.0, 1.0);
            dst->snr_db            = tk->snr_db;
            dst->innovation_norm   = tk->innovation_norm;
            dst->id                = tk->id;
            dst->hits              = tk->hits;
            dst->misses            = tk->misses;
            dst->consecutive_misses = tk->consecutive_misses;
            dst->update_attempts   = tk->update_attempts;
            dst->state             = tk->state;
            dst->target_class      = tk->target_class;
            dst->history_bits      = tk->history_bits;
            dst->dwell_state       = tk->dwell_state;
            dst->trail_count       = tk->trail_count;
            dst->trail_head        = tk->trail_head;
            memcpy(dst->trail, tk->trail, sizeof(dst->trail));
            if (tk->state == PWR_TRACK_CONFIRMED) { ++confirmed; }
            if (out >= PWR_MAX_TRACKS) { break; }
        }
        h->tracks      = fs->tracks;
        h->track_count = out;
        e->stats.tracks_active    = out;
        e->stats.tracks_confirmed = confirmed;
    }

    /* ---- ground truth --------------------------------------------------- */
    {
        uint32_t out = 0u;
        for (i = 0u; i < e->sim.target_count && out < PWR_MAX_SIM_TARGETS; ++i)
        {
            PWR_SimTarget* dst = &fs->truth[out];
            *dst = e->sim.targets[i];
            dst->x_m = e->sim.state[i].x;
            dst->y_m = e->sim.state[i].y;
            dst->z_m = e->sim.state[i].z;
            dst->enabled = (e->sim.targets[i].enabled != 0 &&
                            e->sim.state[i].active != 0) ? 1 : 0;
            ++out;
        }
        h->truth       = fs->truth;
        h->truth_count = out;
    }

    /* ---- header --------------------------------------------------------- */
    h->sequence               = e->stats.cpi_count;
    h->time_s                 = e->scenario_time_s;
    h->beam_azimuth_deg       = pwr_wrap360(e->beam_azimuth_deg);
    h->beam_azimuth_start_deg = pwr_wrap360(e->beam_azimuth_deg -
                                            0.5 * e->cfg.azimuth_beamwidth_deg);
    h->beam_azimuth_end_deg   = pwr_wrap360(e->beam_azimuth_deg +
                                            0.5 * e->cfg.azimuth_beamwidth_deg);
    h->range_bins             = nr;
    h->doppler_bins           = nd;
    h->ppi_az_cells           = e->ppi_cells;
    h->rti_rows               = e->rti_rows;
    h->range_first_m          = e->axis_range_first_m;
    h->range_step_m           = e->axis_range_step_m;
    h->velocity_first_mps     = e->axis_vel_first_mps;
    h->velocity_step_mps      = e->axis_vel_step_mps;
    h->noise_floor_db         = e->stats.measured_noise_floor_db;
    h->range_profile_db       = fs->range_profile_db;
    h->range_profile_raw_db   = fs->range_profile_raw_db;
    h->cfar_threshold_db      = fs->cfar_threshold_db;
    h->doppler_spectrum_db    = fs->doppler_spectrum_db;
    h->rd_map_db              = fs->rd_map_db;
    h->ppi_video              = fs->ppi_video;
    h->rti_db                 = fs->rti_db;
    h->stats                  = e->stats;
    h->metrics                = e->dm;

    pwr_mutex_lock(&e->frame_lock);
    e->publish_index = slot;
    ++e->stats.frames_published;
    pwr_mutex_unlock(&e->frame_lock);
}

PWR_EXPORT(PWR_Status) pwr_engine_frame_acquire(PWR_Engine* eng, PWR_Frame* out)
{
    PWR_Status st;
    if (eng == NULL || out == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_mutex_lock(&eng->frame_lock);
    if (eng->held_index >= 0)
    {
        st = PWR_ERR_INVALID_STATE;      /* acquire without a release */
    }
    else if (eng->publish_index < 0)
    {
        st = PWR_STATUS_NO_DATA;
    }
    else
    {
        eng->held_index = eng->publish_index;
        eng->frames[eng->held_index].refcount = 1;
        *out = eng->frames[eng->held_index].header;
        st = PWR_STATUS_OK;
    }
    pwr_mutex_unlock(&eng->frame_lock);
    return st;
}

PWR_EXPORT(PWR_Status) pwr_engine_frame_release(PWR_Engine* eng)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_mutex_lock(&eng->frame_lock);
    if (eng->held_index >= 0)
    {
        eng->frames[eng->held_index].refcount = 0;
        eng->held_index = -1;
    }
    pwr_mutex_unlock(&eng->frame_lock);
    return PWR_STATUS_OK;
}

PWR_EXPORT(uint64_t) pwr_engine_frame_sequence(const PWR_Engine* eng)
{
    if (eng == NULL) { return 0u; }
    return pwr_atomic_load_u64(&eng->stats.frames_published);
}

PWR_EXPORT(PWR_Status) pwr_engine_get_stats(const PWR_Engine* eng, PWR_Stats* out)
{
    if (eng == NULL || out == NULL) { return PWR_ERR_NULL_POINTER; }
    /* PWR_Stats is a plain aggregate of scalars written by a single producer;
     * a snapshot copy is coherent enough for a status readout, and taking
     * proc_lock here would stall the UI behind a whole CPI. */
    *out = eng->stats;
    out->run_state = pwr_atomic_load_i32(&eng->run_state);
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  One coherent processing interval
 * ========================================================================== */
static void pwr_engine_process_cpi(PWR_Engine* e)
{
    const double t_begin = pwr_plat_now_s();
    const double alpha   = 0.1;                 /* EWMA smoothing for timings */
    double t0, t1;

    t0 = t_begin;
    pwr_sim_generate_cpi(e);
    t1 = pwr_plat_now_s();
    e->stats.t_simulate_ms = pwr_ewma(e->stats.t_simulate_ms, (t1 - t0) * 1e3, alpha);

    t0 = t1;
    pwr_chain_pulse_compress(e);
    pwr_sim_add_clutter(e);
    t1 = pwr_plat_now_s();
    e->stats.t_pulse_compress_ms =
        pwr_ewma(e->stats.t_pulse_compress_ms, (t1 - t0) * 1e3, alpha);

    t0 = t1;
    pwr_chain_mti(e);
    t1 = pwr_plat_now_s();
    e->stats.t_mti_ms = pwr_ewma(e->stats.t_mti_ms, (t1 - t0) * 1e3, alpha);

    t0 = t1;
    pwr_chain_doppler(e);
    pwr_chain_products(e);
    t1 = pwr_plat_now_s();
    e->stats.t_doppler_ms = pwr_ewma(e->stats.t_doppler_ms, (t1 - t0) * 1e3, alpha);

    t0 = t1;
    pwr_cfar_run(e);
    t1 = pwr_plat_now_s();
    e->stats.t_cfar_ms = pwr_ewma(e->stats.t_cfar_ms, (t1 - t0) * 1e3, alpha);

    t0 = t1;
    pwr_cluster_run(e);
    pwr_plots_dwell_merge(e);
    t1 = pwr_plat_now_s();
    e->stats.t_cluster_ms = pwr_ewma(e->stats.t_cluster_ms, (t1 - t0) * 1e3, alpha);

    t0 = t1;
    pwr_tracker_update(e);
    t1 = pwr_plat_now_s();
    e->stats.t_track_ms = pwr_ewma(e->stats.t_track_ms, (t1 - t0) * 1e3, alpha);

    pwr_display_update_ppi(e);
    pwr_display_update_rti(e);

    /* ---- bookkeeping ---------------------------------------------------- */
    ++e->stats.cpi_count;
    e->stats.detections_current   = e->detection_count;
    e->stats.detection_total     += e->detection_count;
    e->stats.cells_tested_total  += e->cfar.cells_tested;
    e->stats.tracks_created_total = e->tracker.created_total;
    e->stats.tracks_deleted_total = e->tracker.deleted_total;
    e->stats.measured_pfa = (e->stats.cells_tested_total > 0u)
        ? (double)e->stats.detection_total / (double)e->stats.cells_tested_total
        : 0.0;
    e->stats.scenario_time_s = e->scenario_time_s;
    e->stats.wall_time_s     = pwr_plat_now_s() - e->wall_origin_s;

    pwr_frame_publish(e);

    /* ---- advance the scenario ------------------------------------------ */
    {
        const double dt = e->dm.cpi_duration_s;
        pwr_sim_advance(&e->sim, dt, e->scenario_time_s);
        e->scenario_time_s += dt;
        if (e->cfg.scan_rate_rpm > 0.0 && e->dm.scan_period_s > 0.0)
        {
            const double prev = e->beam_azimuth_deg;
            e->beam_azimuth_deg += 360.0 * dt / e->dm.scan_period_s;
            if (pwr_wrap360(e->beam_azimuth_deg) < pwr_wrap360(prev))
            {
                ++e->stats.scan_count;
            }
            e->beam_azimuth_deg = pwr_wrap360(e->beam_azimuth_deg);
        }
    }

    /* ---- timing / load -------------------------------------------------- */
    {
        const double now   = pwr_plat_now_s();
        const double total = (now - t_begin) * 1e3;
        e->stats.t_total_ms = pwr_ewma(e->stats.t_total_ms, total, alpha);
        e->stats.load_factor = (e->dm.cpi_duration_s > 0.0)
            ? (e->stats.t_total_ms * 1e-3 / e->dm.cpi_duration_s) : 0.0;
        if (e->last_cpi_wall_s > 0.0)
        {
            const double dt = now - e->last_cpi_wall_s;
            if (dt > 1e-9)
            {
                e->rate_ewma = pwr_ewma(e->rate_ewma, 1.0 / dt, 0.05);
                e->stats.cpi_rate_hz = e->rate_ewma;
            }
        }
        e->last_cpi_wall_s = now;
    }
}

/* ==========================================================================
 *  Hot-setter mailbox
 * ==========================================================================
 *  See the field block in pwr_core.h.  Setters deposit validated sections
 *  under pending_lock and return without ever waiting for the worker; the
 *  sections are folded into the live configuration here, at a CPI boundary,
 *  with proc_lock already held.
 * ------------------------------------------------------------------------ */

/* Caller must hold proc_lock. */
static void pwr_engine_apply_pending_locked(PWR_Engine* e)
{
    uint32_t           flags;
    PWR_CfarConfig     cfar;
    PWR_ClusterConfig  cluster;
    PWR_TrackerConfig  tracker;
    PWR_SimEnvironment env;
    double             time_scale, scan_rpm, stc_range;
    int32_t            stc_enable;

    pwr_mutex_lock(&e->pending_lock);
    flags = e->pending_flags;
    if (flags == 0u)
    {
        pwr_mutex_unlock(&e->pending_lock);
        return;
    }
    cfar       = e->pending_cfar;
    cluster    = e->pending_cluster;
    tracker    = e->pending_tracker;
    env        = e->pending_env;
    time_scale = e->pending_time_scale;
    scan_rpm   = e->pending_scan_rpm;
    stc_range  = e->pending_stc_range_m;
    stc_enable = e->pending_stc_enable;
    e->pending_flags = 0u;
    pwr_mutex_unlock(&e->pending_lock);

    if ((flags & PWR_PENDING_CFAR) != 0u)
    {
        /* The reference-window extent sizes the CFAR scratch and is always
         * routed through the full reconfigure path; never let a stale post
         * resize it out from under the allocated buffers. */
        cfar.train_range = e->cfg.cfar.train_range;
        cfar.guard_range = e->cfg.cfar.guard_range;
        e->cfg.cfar = cfar;
    }
    if ((flags & PWR_PENDING_CLUSTER) != 0u) { e->cfg.cluster = cluster; }
    if ((flags & PWR_PENDING_TRACKER) != 0u) { e->cfg.tracker = tracker; }
    if ((flags & PWR_PENDING_ENV) != 0u)
    {
        e->sim.env = env;
        /* Pulse-to-pulse clutter correlation for the new spectrum width. */
        {
            const double pri = 1.0 / e->cfg.prf_hz;
            const double a   = PWR_PI *
                pwr_maxd(e->sim.env.clutter_spread_hz, 0.0) * pri;
            e->sim.clutter_rho = pwr_clampd(exp(-2.0 * a * a), 0.0, 0.999999);
        }
    }
    if ((flags & PWR_PENDING_TIME_SCALE) != 0u)
    {
        e->cfg.time_scale = time_scale;
    }
    if ((flags & PWR_PENDING_SCAN_RATE) != 0u)
    {
        e->cfg.scan_rate_rpm = scan_rpm;
        (void)pwr_config_derive(&e->cfg, &e->dm);
        e->dm.range_bins = e->n_range;
    }
    if ((flags & PWR_PENDING_STC) != 0u)
    {
        e->cfg.enable_stc  = stc_enable;
        e->cfg.stc_range_m = stc_range;
        pwr_engine_update_stc_gain(e);
    }
}

/* Applies posted sections immediately when the engine is idle (stopped,
 * paused, or driven by pwr_engine_step with no worker).  When the worker is
 * mid-CPI the trylock fails and the post is picked up at the next boundary. */
static void pwr_engine_try_apply_pending(PWR_Engine* e)
{
    if (pwr_mutex_trylock(&e->proc_lock) != 0)
    {
        pwr_engine_apply_pending_locked(e);
        pwr_mutex_unlock(&e->proc_lock);
    }
}

/* ==========================================================================
 *  Worker thread
 * ========================================================================== */
static void pwr_worker_main(void* arg)
{
    PWR_Engine* e = (PWR_Engine*)arg;
    double deadline = pwr_plat_now_s();

    for (;;)
    {
        int32_t state;

        pwr_mutex_lock(&e->ctrl_lock);
        while (e->quit_flag == 0 && e->run_state != PWR_RUN_RUNNING)
        {
            pwr_cond_wait(&e->ctrl_cond, &e->ctrl_lock);
            deadline = pwr_plat_now_s();
        }
        state = e->run_state;
        pwr_mutex_unlock(&e->ctrl_lock);

        if (e->quit_flag != 0) { break; }
        if (state != PWR_RUN_RUNNING) { continue; }

        pwr_mutex_lock(&e->proc_lock);
        pwr_engine_apply_pending_locked(e);
        pwr_engine_process_cpi(e);
        pwr_mutex_unlock(&e->proc_lock);

        /* Hand proc_lock over when a mutator is queued: without this yield an
         * overloaded worker re-locks immediately and the mutator starves. */
        while (pwr_atomic_load_i32(&e->mutator_waiting) > 0 && e->quit_flag == 0)
        {
            pwr_plat_yield();
        }

        /* ---- pace to the wall clock ------------------------------------- */
        {
            double period = e->dm.cpi_duration_s /
                            pwr_maxd(e->cfg.time_scale, 1e-6);
            if (e->cfg.max_cpi_per_second > 0)
            {
                const double lim = 1.0 / (double)e->cfg.max_cpi_per_second;
                if (lim > period) { period = lim; }
            }
            deadline += period;
            {
                const double now = pwr_plat_now_s();
                if (deadline > now)
                {
                    pwr_plat_sleep_s(deadline - now);
                }
                else if (now - deadline > 0.25)
                {
                    /* Fell far behind: resynchronise instead of accumulating
                     * an unbounded backlog, and account the dropped frames. */
                    e->stats.frames_dropped +=
                        (uint64_t)((now - deadline) / pwr_maxd(period, 1e-9));
                    deadline = now;
                }
            }
        }
    }
    pwr_atomic_store_i32(&e->worker_alive, 0);
}

static PWR_Status pwr_engine_start_worker(PWR_Engine* e)
{
    PWR_Status st;
    if (e->worker != NULL) { return PWR_STATUS_OK; }
    e->quit_flag = 0;
    pwr_atomic_store_i32(&e->worker_alive, 1);
    st = pwr_thread_create(&e->worker, pwr_worker_main, e, "pwradar-dsp");
    if (st != PWR_STATUS_OK)
    {
        pwr_atomic_store_i32(&e->worker_alive, 0);
        pwr_set_error(e, "failed to create the processing thread");
    }
    return st;
}

static void pwr_engine_stop_worker(PWR_Engine* e)
{
    if (e->worker == NULL) { return; }
    pwr_mutex_lock(&e->ctrl_lock);
    e->quit_flag = 1;
    pwr_cond_broadcast(&e->ctrl_cond);
    pwr_mutex_unlock(&e->ctrl_lock);
    pwr_thread_join(e->worker);
    e->worker = NULL;
}

/* ==========================================================================
 *  Engine life cycle
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_engine_create(const PWR_RadarConfig* cfg,
                                         PWR_Engine** out_engine)
{
    PWR_Engine* e;
    PWR_RadarConfig local;
    PWR_Status st;
    char err[PWR_ERRMSG_LEN];

    if (out_engine == NULL) { return PWR_ERR_NULL_POINTER; }
    *out_engine = NULL;

    if (cfg != NULL) { local = *cfg; }
    else             { (void)pwr_config_default(&local); }
    (void)pwr_config_clamp(&local);
    st = pwr_config_validate(&local, err, sizeof(err));
    if (st != PWR_STATUS_OK) { return st; }

    e = (PWR_Engine*)pwr_aligned_calloc(1u, sizeof(*e), PWR_CACHELINE);
    if (e == NULL) { return PWR_ERR_OUT_OF_MEMORY; }

    e->cfg           = local;
    e->log_level     = PWR_LOG_INFO;
    e->publish_index = -1;
    e->held_index    = -1;
    e->run_state     = PWR_RUN_STOPPED;
    e->wall_origin_s = pwr_plat_now_s();
    (void)pwr_sim_environment_default(&e->sim.env);
    e->sim.next_auto_id = 1;
    snprintf(e->err, sizeof(e->err), "no error");

    if (pwr_mutex_init(&e->frame_lock)   != PWR_STATUS_OK ||
        pwr_mutex_init(&e->ctrl_lock)    != PWR_STATUS_OK ||
        pwr_mutex_init(&e->proc_lock)    != PWR_STATUS_OK ||
        pwr_mutex_init(&e->pending_lock) != PWR_STATUS_OK ||
        pwr_cond_init(&e->ctrl_cond)     != PWR_STATUS_OK)
    {
        pwr_engine_destroy(e);
        return PWR_ERR_THREAD;
    }

    (void)pwr_config_derive(&e->cfg, &e->dm);

    st = pwr_tracker_init(&e->tracker);
    if (st != PWR_STATUS_OK) { pwr_engine_destroy(e); return st; }

    st = pwr_engine_alloc_buffers(e);
    if (st != PWR_STATUS_OK) { pwr_engine_destroy(e); return st; }

    if (e->cfg.worker_thread != 0)
    {
        st = pwr_engine_start_worker(e);
        if (st != PWR_STATUS_OK) { pwr_engine_destroy(e); return st; }
    }

    *out_engine = e;
    return PWR_STATUS_OK;
}

PWR_EXPORT(void) pwr_engine_destroy(PWR_Engine* eng)
{
    if (eng == NULL) { return; }
    pwr_engine_stop_worker(eng);
    pwr_engine_free_buffers(eng);
    pwr_tracker_release(&eng->tracker);
    pwr_cond_destroy(&eng->ctrl_cond);
    pwr_mutex_destroy(&eng->pending_lock);
    pwr_mutex_destroy(&eng->proc_lock);
    pwr_mutex_destroy(&eng->ctrl_lock);
    pwr_mutex_destroy(&eng->frame_lock);
    pwr_aligned_free(eng);
}

/* ==========================================================================
 *  Execution control
 * ========================================================================== */
static void pwr_engine_set_state(PWR_Engine* e, PWR_RunState s)
{
    pwr_mutex_lock(&e->ctrl_lock);
    e->run_state = (int32_t)s;
    pwr_cond_broadcast(&e->ctrl_cond);
    pwr_mutex_unlock(&e->ctrl_lock);
}

PWR_EXPORT(PWR_Status) pwr_engine_start(PWR_Engine* eng)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    if (eng->cfg.worker_thread != 0 && eng->worker == NULL)
    {
        const PWR_Status st = pwr_engine_start_worker(eng);
        if (st != PWR_STATUS_OK) { return st; }
    }
    eng->last_cpi_wall_s = 0.0;
    pwr_engine_set_state(eng, PWR_RUN_RUNNING);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_stop(PWR_Engine* eng)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_engine_set_state(eng, PWR_RUN_STOPPED);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_pause(PWR_Engine* eng)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_engine_set_state(eng, PWR_RUN_PAUSED);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_resume(PWR_Engine* eng)
{
    return pwr_engine_start(eng);
}

PWR_EXPORT(PWR_RunState) pwr_engine_run_state(const PWR_Engine* eng)
{
    if (eng == NULL) { return PWR_RUN_STOPPED; }
    return (PWR_RunState)pwr_atomic_load_i32(&eng->run_state);
}

PWR_EXPORT(PWR_Status) pwr_engine_reset(PWR_Engine* eng)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }

    pwr_engine_lock_proc_fair(eng);
    eng->scenario_time_s = 0.0;
    eng->beam_azimuth_deg = 0.0;
    eng->rti_head = 0u;
    eng->detection_count = 0u;
    memset(eng->dwell, 0, sizeof(eng->dwell));
    eng->last_cpi_wall_s = 0.0;
    eng->rate_ewma = 0.0;
    eng->wall_origin_s = pwr_plat_now_s();
    memset(&eng->stats, 0, sizeof(eng->stats));
    pwr_tracker_reset(&eng->tracker);
    pwr_sim_reset(&eng->sim, &eng->cfg);
    if (eng->ppi_accum != NULL)
    {
        memset(eng->ppi_accum, 0,
               (size_t)eng->ppi_cells * eng->n_range * sizeof(uint16_t));
    }
    if (eng->rti != NULL)
    {
        const size_t total = (size_t)eng->rti_rows * eng->n_range;
        size_t k;
        for (k = 0u; k < total; ++k) { eng->rti[k] = PWR_DB_FLOOR; }
    }
    pwr_mutex_unlock(&eng->proc_lock);

    pwr_log(eng, PWR_LOG_INFO, "engine reset");
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_step(PWR_Engine* eng, uint32_t cpi_count)
{
    uint32_t i;
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    if (pwr_atomic_load_i32(&eng->run_state) == PWR_RUN_RUNNING)
    {
        return PWR_ERR_INVALID_STATE;   /* the worker owns the pipeline */
    }
    for (i = 0u; i < cpi_count; ++i)
    {
        pwr_engine_lock_proc_fair(eng);
        pwr_engine_apply_pending_locked(eng);
        pwr_engine_process_cpi(eng);
        pwr_mutex_unlock(&eng->proc_lock);
    }
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Reconfiguration
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_engine_get_config(const PWR_Engine* eng,
                                             PWR_RadarConfig* out)
{
    if (eng == NULL || out == NULL) { return PWR_ERR_NULL_POINTER; }
    *out = eng->cfg;
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_get_metrics(const PWR_Engine* eng,
                                              PWR_DerivedMetrics* out)
{
    if (eng == NULL || out == NULL) { return PWR_ERR_NULL_POINTER; }
    *out = eng->dm;
    return PWR_STATUS_OK;
}

/* True when the two configurations imply an identical processing grid, i.e.
 * the lightweight reconfiguration path is safe.  Beyond the buffer sizes this
 * must also cover the cached fast-time mapping (range_offset / range_decim,
 * reached through range_start_m and the derived bin spacing): those are only
 * recomputed on the full path, so treating them as "equal dimensions" would
 * leave the engine decimating on the old grid while the published metrics
 * describe the new one.  The doubles compare exactly because both sides come
 * from the same pure derivation of otherwise identical inputs. */
static int pwr_dims_equal(const PWR_RadarConfig* a, const PWR_RadarConfig* b)
{
    PWR_DerivedMetrics da, db;
    (void)pwr_config_derive(a, &da);
    (void)pwr_config_derive(b, &db);
    return (a->pulses_per_cpi     == b->pulses_per_cpi &&
            a->doppler_bins       == b->doppler_bins &&
            a->ppi_azimuth_cells  == b->ppi_azimuth_cells &&
            a->rti_rows           == b->rti_rows &&
            a->mti_mode           == b->mti_mode &&
            a->range_start_m      == b->range_start_m &&
            da.range_bins         == db.range_bins &&
            da.range_bin_spacing_m == db.range_bin_spacing_m &&
            da.samples_per_pri    == db.samples_per_pri &&
            da.fast_time_fft_size == db.fast_time_fft_size &&
            a->cfar.train_range   == b->cfar.train_range &&
            a->cfar.guard_range   == b->cfar.guard_range) ? 1 : 0;
}

PWR_EXPORT(PWR_Status) pwr_engine_reconfigure(PWR_Engine* eng,
                                              const PWR_RadarConfig* cfg,
                                              char* err, size_t err_cap)
{
    PWR_RadarConfig local;
    PWR_Status st;
    int needs_realloc, waveform_dirty;

    if (eng == NULL || cfg == NULL) { return PWR_ERR_NULL_POINTER; }
    local = *cfg;
    (void)pwr_config_clamp(&local);
    st = pwr_config_validate(&local, err, err_cap);
    if (st != PWR_STATUS_OK) { return st; }

    needs_realloc = (pwr_dims_equal(&eng->cfg, &local) == 0) ? 1 : 0;
    waveform_dirty = (local.range_window != eng->cfg.range_window ||
                      local.enable_pulse_compression !=
                          eng->cfg.enable_pulse_compression) ? 1 : 0;

    pwr_engine_lock_proc_fair(eng);
    {
        const PWR_SimEnvironment env  = eng->sim.env;
        /* Drain the hot-setter mailbox first so an older post can never
         * overwrite the full configuration being applied now. */
        pwr_engine_apply_pending_locked(eng);
        eng->cfg = local;
        (void)pwr_config_derive(&eng->cfg, &eng->dm);
        if (needs_realloc != 0)
        {
            PWR_SimTarget saved[PWR_MAX_SIM_TARGETS];
            const uint32_t n = eng->sim.target_count;
            memcpy(saved, eng->sim.targets, sizeof(saved));

            st = pwr_engine_alloc_buffers(eng);
            if (st == PWR_STATUS_OK)
            {
                eng->sim.env = env;
                eng->sim.target_count = n;
                memcpy(eng->sim.targets, saved, sizeof(saved));
                pwr_sim_reset(&eng->sim, &eng->cfg);
                /* The track file deliberately survives: tracker state is
                 * metric ENU with no grid indices, so a geometry change does
                 * not invalidate it.  Tracks left outside the new coverage
                 * stop receiving plots and retire through the scan-staleness
                 * rule, while everything still covered keeps its identity
                 * instead of spending three scans on re-confirmation. */
            }
        }
        else
        {
            /* Geometry unchanged.  Rebuild the compression filter only if the
             * range taper or the compression enable actually moved, then
             * refresh the tapers, the dB calibration and the STC ramp.  The
             * PPI history, the RTI waterfall and the track file all survive,
             * which is what an operator turning a knob expects. */
            if (waveform_dirty != 0)
            {
                st = pwr_waveform_build(&eng->wf, &eng->cfg, &eng->dm,
                                        eng->plan_fast);
            }
            if (st == PWR_STATUS_OK)
            {
                pwr_engine_refresh_calibration(eng);
                pwr_sim_reset(&eng->sim, &eng->cfg);
            }
        }
        /* A post that raced in while this reconfiguration held the lock
         * would otherwise sit until the next CPI - or forever if paused. */
        pwr_engine_apply_pending_locked(eng);
    }
    pwr_mutex_unlock(&eng->proc_lock);

    if (st != PWR_STATUS_OK)
    {
        if (err != NULL && err_cap > 0u)
        {
            (void)snprintf(err, err_cap, "%s", eng->err);
        }
        pwr_engine_set_state(eng, PWR_RUN_FAULTED);
        return st;
    }
    pwr_log(eng, PWR_LOG_INFO,
            "reconfigured: realloc=%d waveform=%d | %u x %u bins, "
            "PSL %.1f dB, mainlobe %.2f bins, mismatch %.2f dB, sigma_pc %.4f",
            needs_realloc, waveform_dirty, eng->n_range, eng->n_doppler,
            eng->wf.sidelobe_db, eng->wf.mainlobe_bins,
            eng->wf.mismatch_loss_db, eng->sigma_pc);
    return PWR_STATUS_OK;
}

/* --------------------------------------------------------------------------
 *  Fine-grained hot setters
 *  ------------------------
 *  Non-blocking by contract: each setter clamps/validates with the pure
 *  config helpers, deposits the new section into the mailbox and returns.
 *  pwr_engine_try_apply_pending() makes the change effective immediately
 *  whenever proc_lock happens to be free (stopped, paused, manual stepping);
 *  otherwise the worker folds it in at the next CPI boundary, so a setter
 *  never stalls a UI thread for the duration of a CPI.
 * ------------------------------------------------------------------------ */

PWR_EXPORT(PWR_Status) pwr_engine_set_cfar(PWR_Engine* eng,
                                           const PWR_CfarConfig* cfar)
{
    PWR_RadarConfig probe;
    char err[PWR_ERRMSG_LEN];
    if (eng == NULL || cfar == NULL) { return PWR_ERR_NULL_POINTER; }
    probe = eng->cfg;
    probe.cfar = *cfar;
    (void)pwr_config_clamp(&probe);
    if (pwr_config_validate(&probe, err, sizeof(err)) != PWR_STATUS_OK)
    {
        return PWR_ERR_CONFIG_INVALID;
    }
    /* The reference-window extent sizes the CFAR scratch, so route a change of
     * those through the full path; everything else is a section post. */
    if (probe.cfar.train_range != eng->cfg.cfar.train_range ||
        probe.cfar.guard_range != eng->cfg.cfar.guard_range)
    {
        return pwr_engine_reconfigure(eng, &probe, err, sizeof(err));
    }
    pwr_mutex_lock(&eng->pending_lock);
    eng->pending_cfar   = probe.cfar;
    eng->pending_flags |= PWR_PENDING_CFAR;
    pwr_mutex_unlock(&eng->pending_lock);
    pwr_engine_try_apply_pending(eng);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_set_cluster(PWR_Engine* eng,
                                              const PWR_ClusterConfig* cl)
{
    PWR_RadarConfig probe;
    if (eng == NULL || cl == NULL) { return PWR_ERR_NULL_POINTER; }
    probe = eng->cfg;
    probe.cluster = *cl;
    (void)pwr_config_clamp(&probe);
    pwr_mutex_lock(&eng->pending_lock);
    eng->pending_cluster = probe.cluster;
    eng->pending_flags  |= PWR_PENDING_CLUSTER;
    pwr_mutex_unlock(&eng->pending_lock);
    pwr_engine_try_apply_pending(eng);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_set_tracker(PWR_Engine* eng,
                                              const PWR_TrackerConfig* tk)
{
    PWR_RadarConfig probe;
    if (eng == NULL || tk == NULL) { return PWR_ERR_NULL_POINTER; }
    probe = eng->cfg;
    probe.tracker = *tk;
    (void)pwr_config_clamp(&probe);
    pwr_mutex_lock(&eng->pending_lock);
    eng->pending_tracker = probe.tracker;
    eng->pending_flags  |= PWR_PENDING_TRACKER;
    pwr_mutex_unlock(&eng->pending_lock);
    pwr_engine_try_apply_pending(eng);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_set_mti(PWR_Engine* eng, PWR_MtiMode mode)
{
    PWR_RadarConfig probe;
    char err[PWR_ERRMSG_LEN];
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    probe = eng->cfg;
    probe.mti_mode = (int32_t)mode;
    return pwr_engine_reconfigure(eng, &probe, err, sizeof(err));
}

PWR_EXPORT(PWR_Status) pwr_engine_set_windows(PWR_Engine* eng,
                                              PWR_WindowType range_win,
                                              PWR_WindowType doppler_win)
{
    PWR_RadarConfig probe;
    char err[PWR_ERRMSG_LEN];
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    probe = eng->cfg;
    probe.range_window   = (int32_t)range_win;
    probe.doppler_window = (int32_t)doppler_win;
    return pwr_engine_reconfigure(eng, &probe, err, sizeof(err));
}

PWR_EXPORT(PWR_Status) pwr_engine_set_scan_rate(PWR_Engine* eng, double rpm)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_mutex_lock(&eng->pending_lock);
    eng->pending_scan_rpm = pwr_clampd(rpm, 0.0, 120.0);
    eng->pending_flags   |= PWR_PENDING_SCAN_RATE;
    pwr_mutex_unlock(&eng->pending_lock);
    pwr_engine_try_apply_pending(eng);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_set_time_scale(PWR_Engine* eng, double scale)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_mutex_lock(&eng->pending_lock);
    eng->pending_time_scale = pwr_clampd(scale, 0.01, 100.0);
    eng->pending_flags     |= PWR_PENDING_TIME_SCALE;
    pwr_mutex_unlock(&eng->pending_lock);
    pwr_engine_try_apply_pending(eng);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_set_stc(PWR_Engine* eng, int32_t enable,
                                          double range_m)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_mutex_lock(&eng->pending_lock);
    eng->pending_stc_enable  = (enable != 0) ? 1 : 0;
    eng->pending_stc_range_m = pwr_clampd(range_m, 100.0, 1.0e6);
    eng->pending_flags      |= PWR_PENDING_STC;
    pwr_mutex_unlock(&eng->pending_lock);
    pwr_engine_try_apply_pending(eng);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_set_cursor_range_bin(PWR_Engine* eng,
                                                       uint32_t bin)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    if (eng->n_range == 0u) { return PWR_ERR_INVALID_STATE; }
    /* A single aligned 32-bit store: no lock needed, and a stale value for one
     * CPI is harmless for a cursor readout. */
    eng->cursor_range_bin = pwr_minu(bin, eng->n_range - 1u);
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Scenario management
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_engine_set_environment(PWR_Engine* eng,
                                                  const PWR_SimEnvironment* env)
{
    if (eng == NULL || env == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_mutex_lock(&eng->pending_lock);
    eng->pending_env = *env;
    eng->pending_env.sea_state = pwr_clampd(env->sea_state, 0.0, 9.0);
    eng->pending_flags |= PWR_PENDING_ENV;
    pwr_mutex_unlock(&eng->pending_lock);
    pwr_engine_try_apply_pending(eng);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_get_environment(const PWR_Engine* eng,
                                                  PWR_SimEnvironment* out)
{
    if (eng == NULL || out == NULL) { return PWR_ERR_NULL_POINTER; }
    *out = eng->sim.env;
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_target_add(PWR_Engine* eng, PWR_SimTarget* tgt)
{
    PWR_Status st = PWR_STATUS_OK;
    if (eng == NULL || tgt == NULL) { return PWR_ERR_NULL_POINTER; }

    pwr_engine_lock_proc_fair(eng);
    if (eng->sim.target_count >= PWR_MAX_SIM_TARGETS)
    {
        st = PWR_ERR_CAPACITY_EXCEEDED;
    }
    else
    {
        const uint32_t k = eng->sim.target_count;
        if (tgt->id == 0) { tgt->id = eng->sim.next_auto_id++; }
        else if (tgt->id >= eng->sim.next_auto_id)
        {
            eng->sim.next_auto_id = tgt->id + 1;
        }
        tgt->label[PWR_LABEL_LEN - 1u] = '\0';
        eng->sim.targets[k] = *tgt;
        eng->sim.state[k].x = tgt->x_m;
        eng->sim.state[k].y = tgt->y_m;
        eng->sim.state[k].z = tgt->z_m;
        eng->sim.state[k].amp_scale = 1.0;
        eng->sim.state[k].next_scan_update_s = 0.0;
        eng->sim.state[k].active = 0;
        eng->sim.target_count = k + 1u;
    }
    pwr_mutex_unlock(&eng->proc_lock);
    return st;
}

PWR_EXPORT(PWR_Status) pwr_engine_target_update(PWR_Engine* eng,
                                                const PWR_SimTarget* tgt)
{
    PWR_Status st = PWR_ERR_NOT_FOUND;
    uint32_t i;
    if (eng == NULL || tgt == NULL) { return PWR_ERR_NULL_POINTER; }

    pwr_engine_lock_proc_fair(eng);
    for (i = 0u; i < eng->sim.target_count; ++i)
    {
        if (eng->sim.targets[i].id != tgt->id) { continue; }
        {
            const int moved = (eng->sim.targets[i].x_m != tgt->x_m) ||
                              (eng->sim.targets[i].y_m != tgt->y_m) ||
                              (eng->sim.targets[i].z_m != tgt->z_m);
            eng->sim.targets[i] = *tgt;
            eng->sim.targets[i].label[PWR_LABEL_LEN - 1u] = '\0';
            if (moved != 0)
            {
                eng->sim.state[i].x = tgt->x_m;
                eng->sim.state[i].y = tgt->y_m;
                eng->sim.state[i].z = tgt->z_m;
            }
        }
        st = PWR_STATUS_OK;
        break;
    }
    pwr_mutex_unlock(&eng->proc_lock);
    return st;
}

PWR_EXPORT(PWR_Status) pwr_engine_target_remove(PWR_Engine* eng, int32_t id)
{
    PWR_Status st = PWR_ERR_NOT_FOUND;
    uint32_t i;
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }

    pwr_engine_lock_proc_fair(eng);
    for (i = 0u; i < eng->sim.target_count; ++i)
    {
        if (eng->sim.targets[i].id != id) { continue; }
        {
            const uint32_t last = eng->sim.target_count - 1u;
            if (i != last)
            {
                eng->sim.targets[i] = eng->sim.targets[last];
                eng->sim.state[i]   = eng->sim.state[last];
            }
            memset(&eng->sim.targets[last], 0, sizeof(eng->sim.targets[last]));
            memset(&eng->sim.state[last],   0, sizeof(eng->sim.state[last]));
            eng->sim.target_count = last;
        }
        st = PWR_STATUS_OK;
        break;
    }
    pwr_mutex_unlock(&eng->proc_lock);
    return st;
}

PWR_EXPORT(PWR_Status) pwr_engine_target_clear(PWR_Engine* eng)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_engine_lock_proc_fair(eng);
    memset(eng->sim.targets, 0, sizeof(eng->sim.targets));
    memset(eng->sim.state,   0, sizeof(eng->sim.state));
    eng->sim.target_count = 0u;
    pwr_mutex_unlock(&eng->proc_lock);
    return PWR_STATUS_OK;
}

PWR_EXPORT(uint32_t) pwr_engine_target_count(const PWR_Engine* eng)
{
    return (eng != NULL) ? eng->sim.target_count : 0u;
}

PWR_EXPORT(PWR_Status) pwr_engine_target_at(const PWR_Engine* eng,
                                            uint32_t index, PWR_SimTarget* out)
{
    if (eng == NULL || out == NULL) { return PWR_ERR_NULL_POINTER; }
    if (index >= eng->sim.target_count) { return PWR_ERR_OUT_OF_RANGE; }
    *out = eng->sim.targets[index];
    out->x_m = eng->sim.state[index].x;
    out->y_m = eng->sim.state[index].y;
    out->z_m = eng->sim.state[index].z;
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Utility exports
 * ========================================================================== */
PWR_EXPORT(uint32_t) pwr_abi_version(void) { return (uint32_t)PWR_ABI_VERSION; }

PWR_EXPORT(const char*) pwr_version_string(void)
{
#if defined(_MSC_VER)
    return PWR_VERSION_STRING " (MSVC " PWR_STRINGIFY(_MSC_VER) ", C17)";
#elif defined(__clang__)
    return PWR_VERSION_STRING " (Clang " __clang_version__ ", C17)";
#elif defined(__GNUC__)
    return PWR_VERSION_STRING " (GCC " __VERSION__ ", C17)";
#else
    return PWR_VERSION_STRING " (unknown compiler, C17)";
#endif
}

PWR_EXPORT(double) pwr_time_now_s(void) { return pwr_plat_now_s(); }
PWR_EXPORT(void)   pwr_sleep_s(double s) { pwr_plat_sleep_s(s); }

PWR_EXPORT(PWR_Status) pwr_window_fill(PWR_WindowType type, pwr_real* dst,
                                       uint32_t n)
{
    return pwr_window_generate(type, dst, n);
}

PWR_EXPORT(uint32_t) pwr_next_pow2(uint32_t v) { return pwr_pow2_ceil(v); }

PWR_EXPORT(PWR_Status) pwr_fft_forward(PWR_Complex* data, uint32_t n)
{
    PWR_FftPlan* p = NULL;
    PWR_Status st;
    if (data == NULL) { return PWR_ERR_NULL_POINTER; }
    st = pwr_fft_plan_create(&p, n);
    if (st != PWR_STATUS_OK) { return st; }
    pwr_fft_run(p, data);
    pwr_fft_plan_destroy(p);
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_fft_inverse(PWR_Complex* data, uint32_t n)
{
    PWR_FftPlan* p = NULL;
    PWR_Status st;
    if (data == NULL) { return PWR_ERR_NULL_POINTER; }
    st = pwr_fft_plan_create(&p, n);
    if (st != PWR_STATUS_OK) { return st; }
    pwr_ifft_run(p, data);
    pwr_fft_plan_destroy(p);
    return PWR_STATUS_OK;
}
