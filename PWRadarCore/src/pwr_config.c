/* Defaults, validation, clamping and every derived radar metric. Pure
 * functions - no engine state is touched.
 *
 * Conventions fixed here and honoured everywhere else:
 *   - range bin index == matched-filter delay sample, so
 *     range(i) = range_first_m + i * range_step_m
 *   - radial velocity is range rate, positive == receding, fd = -2*rdot/lambda
 *   - the published Doppler axis increases with range rate, so column 0 is the
 *     fastest closing target (see pwr_chain_doppler)
 *   - azimuth is compass bearing: 0 == North (+y), 90 == East (+x)
 */
#include "pwr_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 *  Defaults
 * ==========================================================================
 *  The default set describes an S-band coastal surveillance PW radar sized so
 *  that a single CPU core sustains real time while still exhibiting every
 *  effect the verification console is meant to show (Doppler ambiguity, sea
 *  clutter, eclipsing, range sidelobes, scan-to-scan fluctuation).
 * ------------------------------------------------------------------------ */
PWR_EXPORT(PWR_Status) pwr_config_default(PWR_RadarConfig* cfg)
{
    if (cfg == NULL) { return PWR_ERR_NULL_POINTER; }
    memset(cfg, 0, sizeof(*cfg));

    /* ---- transmitter / waveform ----------------------------------------- */
    cfg->carrier_hz          = 3.05e9;      /* S-band                        */
    cfg->bandwidth_hz        = 5.0e6;       /* -> 30 m range resolution      */
    cfg->pulse_width_s       = 20.0e-6;     /* TB = 100 -> 20 dB compression */
    cfg->sample_rate_hz      = 6.25e6;      /* 1.25x oversampled             */
    cfg->prf_hz              = 3000.0;      /* 50 km unambiguous range       */
    cfg->peak_power_w        = 20.0e3;
    cfg->duty_limit          = 0.10;

    /* ---- antenna / RF budget ------------------------------------------- */
    /* 29 dB is what a 1.6 deg x 20 deg aperture actually delivers
     * (G ~ 26000 / (az_bw * el_bw)), so the link budget stays self-consistent
     * with the pattern used by the simulator. */
    cfg->tx_gain_db          = 29.0;
    cfg->rx_gain_db          = 29.0;
    cfg->noise_figure_db     = 3.0;
    cfg->system_loss_db      = 8.0;
    cfg->receiver_bandwidth_hz = 0.0;       /* 0 -> use sample_rate_hz       */
    cfg->azimuth_beamwidth_deg   = 1.6;
    cfg->elevation_beamwidth_deg = 20.0;
    /* No tilt and no fill by default: a plain Gaussian beam on the horizon.
     * Both are opt-in so that every published figure in the documentation
     * describes the configuration the defaults actually build. */
    cfg->elevation_tilt_deg  = 0.0;
    cfg->elevation_csc2_deg  = 0.0;
    cfg->sidelobe_level_db   = -45.0;
    cfg->antenna_height_m    = 25.0;
    cfg->scan_rate_rpm       = 24.0;        /* 2.5 s scan period             */

    /* ---- signal processing --------------------------------------------- */
    cfg->range_start_m       = 0.0;
    cfg->range_span_m        = 24000.0;
    cfg->pulses_per_cpi      = 32u;
    cfg->doppler_bins        = 64u;         /* 2x Doppler oversampling       */
    cfg->range_bins          = 1000u;
    cfg->range_window        = PWR_WIN_TAYLOR_35DB;
    cfg->doppler_window      = PWR_WIN_HAMMING;
    cfg->mti_mode            = PWR_MTI_OFF; /* Doppler filtering does the job */
    cfg->enable_pulse_compression = 1;
    cfg->enable_doppler_processing = 1;
    cfg->enable_stc          = 0;
    cfg->stc_range_m         = 6000.0;

    /* ---- CFAR ----------------------------------------------------------- */
    cfg->cfar.type                = PWR_CFAR_CA;
    /* Pfa is a per-cell figure, so the sensible value follows from the cell
     * *rate*, not from habit.  This geometry tests 1000 x 64 cells per CPI and
     * runs ~234 CPI per scan, i.e. 1.5e7 cells per revolution; a design target
     * of roughly one false plot per scan therefore puts Pfa near 1e-7.  The
     * often-quoted 1e-6 would deliver fifteen false plots every scan and bury
     * the track file in tentative tracks. */
    cfg->cfar.pfa                 = 1.0e-7;
    cfg->cfar.extra_threshold_db  = 0.0;
    cfg->cfar.guard_range         = 3;
    cfg->cfar.guard_doppler       = 2;
    cfg->cfar.train_range         = 12;
    cfg->cfar.train_doppler       = 4;
    cfg->cfar.os_rank             = 0;      /* 0 -> 0.75 * N                 */
    cfg->cfar.trim_low            = 2;
    cfg->cfar.trim_high           = 4;
    cfg->cfar.censor_zero_doppler = 1;
    cfg->cfar.zero_doppler_guard  = 1;
    cfg->cfar.peak_selection      = 1;

    /* ---- clustering ----------------------------------------------------- */
    cfg->cluster.enable            = 1;
    cfg->cluster.range_tolerance   = 3;
    cfg->cluster.doppler_tolerance = 3;
    cfg->cluster.min_cells         = 1;
    cfg->cluster.max_cells         = 400;
    cfg->cluster.min_snr_db        = 8.0;

    /* ---- tracker -------------------------------------------------------- */
    cfg->tracker.enable                 = 1;
    cfg->tracker.assoc_mode             = PWR_ASSOC_GLOBAL;
    cfg->tracker.process_noise_accel    = 6.0;
    cfg->tracker.meas_sigma_range_m     = 20.0;
    cfg->tracker.meas_sigma_azimuth_deg = 0.5;
    cfg->tracker.meas_sigma_velocity_mps = 3.0;
    cfg->tracker.gate_sigma             = 4.0;
    cfg->tracker.gate_max_range_m       = 2500.0;
    cfg->tracker.init_velocity_sigma    = 150.0;
    cfg->tracker.max_speed_mps          = 600.0;
    cfg->tracker.min_speed_for_course   = 1.5;
    cfg->tracker.confirm_m              = 3;
    cfg->tracker.confirm_n              = 5;
    cfg->tracker.coast_misses           = 2;
    cfg->tracker.delete_misses          = 6;
    cfg->tracker.use_doppler_in_gate    = 1;
    cfg->tracker.init_inhibit_m          = 600;

    /* ---- display feeds -------------------------------------------------- */
    cfg->ppi_azimuth_cells   = 1024u;
    cfg->rti_rows            = 256u;
    cfg->ppi_persistence_s   = 6.0;

    /* ---- execution ------------------------------------------------------ */
    cfg->time_scale          = 1.0;
    cfg->max_cpi_per_second  = 0;
    cfg->worker_thread       = 1;
    cfg->deterministic       = 1;

    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_sim_environment_default(PWR_SimEnvironment* env)
{
    if (env == NULL) { return PWR_ERR_NULL_POINTER; }
    memset(env, 0, sizeof(*env));
    env->sea_state             = 2.0;
    env->clutter_to_noise_db   = 20.0;
    env->clutter_spread_hz     = 12.0;
    env->rain_rate_mmph        = 0.0;
    env->rain_extent_km        = 8.0;
    env->refraction_k          = 4.0 / 3.0;     /* standard atmosphere */
    env->clutter_mean_doppler_hz = 0.0;
    env->sea_shape_nu          = 0.0;           /* Rayleigh unless asked    */
    env->land_clutter_to_noise_db = 45.0;
    env->land_spread_hz        = 1.0;           /* terrain barely moves     */
    env->land_bearing_deg      = 0.0;
    env->land_width_deg        = 0.0;           /* no land: open sea        */
    env->land_range_min_m      = 3000.0;
    env->jammer_azimuth_deg    = 120.0;
    env->jammer_power_db       = 15.0;
    env->jammer_bandwidth_frac = 1.0;
    env->rng_seed              = 0x5150524152ULL;   /* "PWRAR" */
    env->enable_thermal_noise  = 1;
    env->enable_sea_clutter    = 1;
    env->enable_rain           = 0;
    env->enable_jammer         = 0;
    env->enable_multipath      = 0;
    env->enable_eclipsing      = 1;
    env->enable_range_ambiguity = 0;
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Clamping
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_config_clamp(PWR_RadarConfig* cfg)
{
    if (cfg == NULL) { return PWR_ERR_NULL_POINTER; }

    cfg->carrier_hz     = pwr_clampd(cfg->carrier_hz, 1.0e8, 1.0e11);
    cfg->bandwidth_hz   = pwr_clampd(cfg->bandwidth_hz, 1.0e4, 5.0e8);
    cfg->pulse_width_s  = pwr_clampd(cfg->pulse_width_s, 1.0e-7, 2.0e-3);
    cfg->sample_rate_hz = pwr_clampd(cfg->sample_rate_hz,
                                     cfg->bandwidth_hz, 1.0e9);
    cfg->prf_hz         = pwr_clampd(cfg->prf_hz, 50.0, 1.0e5);
    cfg->peak_power_w   = pwr_clampd(cfg->peak_power_w, 1.0, 1.0e8);
    cfg->duty_limit     = pwr_clampd(cfg->duty_limit, 0.001, 0.5);

    /* Duty-cycle ceiling: shorten the pulse rather than lower the PRF, which
     * would silently change the unambiguous range the operator selected. */
    if (cfg->pulse_width_s * cfg->prf_hz > cfg->duty_limit)
    {
        cfg->pulse_width_s = cfg->duty_limit / cfg->prf_hz;
    }

    cfg->tx_gain_db      = pwr_clampd(cfg->tx_gain_db, 0.0, 70.0);
    cfg->rx_gain_db      = pwr_clampd(cfg->rx_gain_db, 0.0, 70.0);
    cfg->noise_figure_db = pwr_clampd(cfg->noise_figure_db, 0.0, 20.0);
    cfg->system_loss_db  = pwr_clampd(cfg->system_loss_db, 0.0, 30.0);
    cfg->azimuth_beamwidth_deg   = pwr_clampd(cfg->azimuth_beamwidth_deg, 0.2, 30.0);
    cfg->elevation_beamwidth_deg = pwr_clampd(cfg->elevation_beamwidth_deg, 0.5, 90.0);
    cfg->elevation_tilt_deg = pwr_clampd(cfg->elevation_tilt_deg, -30.0, 60.0);
    cfg->elevation_csc2_deg = pwr_clampd(cfg->elevation_csc2_deg, 0.0, 85.0);
    cfg->sidelobe_level_db  = pwr_clampd(cfg->sidelobe_level_db, -80.0, -10.0);
    cfg->antenna_height_m = pwr_clampd(cfg->antenna_height_m, 0.0, 2000.0);
    cfg->scan_rate_rpm    = pwr_clampd(cfg->scan_rate_rpm, 0.0, 120.0);

    cfg->range_start_m = pwr_clampd(cfg->range_start_m, 0.0, 1.0e6);
    cfg->range_span_m  = pwr_clampd(cfg->range_span_m, 200.0, 1.0e6);

    cfg->pulses_per_cpi = pwr_pow2_ceil(pwr_clampu32(cfg->pulses_per_cpi,
                                                     PWR_MIN_PULSES_PER_CPI,
                                                     PWR_MAX_PULSES_PER_CPI));
    cfg->doppler_bins   = pwr_pow2_ceil(pwr_maxu(cfg->doppler_bins,
                                                 cfg->pulses_per_cpi));
    if (cfg->doppler_bins > PWR_MAX_DOPPLER_BINS)
    {
        cfg->doppler_bins = PWR_MAX_DOPPLER_BINS;
    }
    if (cfg->range_bins != 0u)
    {
        cfg->range_bins = pwr_clampu32(cfg->range_bins,
                                       PWR_MIN_RANGE_BINS, PWR_MAX_RANGE_BINS);
    }

    cfg->range_window   = pwr_clampi(cfg->range_window, 0, PWR_WIN_COUNT - 1);
    cfg->doppler_window = pwr_clampi(cfg->doppler_window, 0, PWR_WIN_COUNT - 1);
    cfg->mti_mode       = pwr_clampi(cfg->mti_mode, 0, PWR_MTI_COUNT - 1);
    /* A canceller of order k consumes k pulses; keep at least 2 usable. */
    while (cfg->mti_mode > PWR_MTI_DC_REMOVAL &&
           cfg->pulses_per_cpi <= (uint32_t)cfg->mti_mode)
    {
        --cfg->mti_mode;
    }
    cfg->stc_range_m = pwr_clampd(cfg->stc_range_m, 100.0, 1.0e6);

    cfg->cfar.type = pwr_clampi(cfg->cfar.type, 0, PWR_CFAR_COUNT - 1);
    cfg->cfar.pfa  = pwr_clampd(cfg->cfar.pfa, 1.0e-12, 1.0e-1);
    cfg->cfar.extra_threshold_db =
        pwr_clampd(cfg->cfar.extra_threshold_db, -20.0, 40.0);
    cfg->cfar.guard_range     = pwr_clampi(cfg->cfar.guard_range, 0, 32);
    cfg->cfar.guard_doppler   = pwr_clampi(cfg->cfar.guard_doppler, 0, 16);
    cfg->cfar.train_range     = pwr_clampi(cfg->cfar.train_range, 1, 64);
    cfg->cfar.train_doppler   = pwr_clampi(cfg->cfar.train_doppler, 0, 16);
    cfg->cfar.zero_doppler_guard = pwr_clampi(cfg->cfar.zero_doppler_guard, 0, 16);

    cfg->cluster.range_tolerance   = pwr_clampi(cfg->cluster.range_tolerance, 0, 16);
    cfg->cluster.doppler_tolerance = pwr_clampi(cfg->cluster.doppler_tolerance, 0, 16);
    cfg->cluster.min_cells = pwr_clampi(cfg->cluster.min_cells, 1, 64);
    cfg->cluster.max_cells = pwr_clampi(cfg->cluster.max_cells, 1, 100000);
    cfg->cluster.min_snr_db = pwr_clampd(cfg->cluster.min_snr_db, -10.0, 60.0);

    cfg->tracker.assoc_mode = pwr_clampi(cfg->tracker.assoc_mode, 0,
                                         PWR_ASSOC_COUNT - 1);

    cfg->tracker.process_noise_accel =
        pwr_clampd(cfg->tracker.process_noise_accel, 0.01, 200.0);
    cfg->tracker.meas_sigma_range_m =
        pwr_clampd(cfg->tracker.meas_sigma_range_m, 0.5, 5000.0);
    cfg->tracker.meas_sigma_azimuth_deg =
        pwr_clampd(cfg->tracker.meas_sigma_azimuth_deg, 0.01, 20.0);
    cfg->tracker.meas_sigma_velocity_mps =
        pwr_clampd(cfg->tracker.meas_sigma_velocity_mps, 0.1, 200.0);
    cfg->tracker.gate_sigma = pwr_clampd(cfg->tracker.gate_sigma, 1.0, 12.0);
    cfg->tracker.gate_max_range_m =
        pwr_clampd(cfg->tracker.gate_max_range_m, 50.0, 50000.0);
    cfg->tracker.init_velocity_sigma =
        pwr_clampd(cfg->tracker.init_velocity_sigma, 1.0, 1000.0);
    cfg->tracker.max_speed_mps = pwr_clampd(cfg->tracker.max_speed_mps, 5.0, 2000.0);
    cfg->tracker.confirm_n = pwr_clampi(cfg->tracker.confirm_n, 1, 31);
    cfg->tracker.confirm_m = pwr_clampi(cfg->tracker.confirm_m, 1,
                                        cfg->tracker.confirm_n);
    cfg->tracker.init_inhibit_m = pwr_clampi(cfg->tracker.init_inhibit_m, 0, 20000);
    cfg->tracker.coast_misses  = pwr_clampi(cfg->tracker.coast_misses, 1, 60);
    cfg->tracker.delete_misses = pwr_clampi(cfg->tracker.delete_misses,
                                            cfg->tracker.coast_misses, 120);

    cfg->ppi_azimuth_cells = pwr_clampu32(cfg->ppi_azimuth_cells, 64u,
                                          PWR_MAX_PPI_AZ_CELLS);
    cfg->rti_rows = pwr_clampu32(cfg->rti_rows, 16u, PWR_MAX_RTI_ROWS);
    cfg->ppi_persistence_s = pwr_clampd(cfg->ppi_persistence_s, 0.0, 60.0);

    cfg->time_scale = pwr_clampd(cfg->time_scale, 0.01, 100.0);
    cfg->max_cpi_per_second = pwr_clampi(cfg->max_cpi_per_second, 0, 100000);

    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Validation
 * ========================================================================== */
static void pwr_verr(char* err, size_t cap, const char* fmt, ...)
{
    va_list ap;
    if (err == NULL || cap == 0u) { return; }
    va_start(ap, fmt);
    (void)vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

PWR_EXPORT(PWR_Status) pwr_config_validate(const PWR_RadarConfig* cfg,
                                           char* err, size_t err_cap)
{
    double samples_per_pri;
    uint32_t n_tx;

    if (err != NULL && err_cap > 0u) { err[0] = '\0'; }
    if (cfg == NULL) { return PWR_ERR_NULL_POINTER; }

    if (!(cfg->carrier_hz > 0.0) || !(cfg->bandwidth_hz > 0.0) ||
        !(cfg->pulse_width_s > 0.0) || !(cfg->sample_rate_hz > 0.0) ||
        !(cfg->prf_hz > 0.0))
    {
        pwr_verr(err, err_cap, "non-positive RF/waveform parameter");
        return PWR_ERR_CONFIG_INVALID;
    }
    if (cfg->sample_rate_hz < cfg->bandwidth_hz)
    {
        pwr_verr(err, err_cap,
                 "sample rate %.3f MHz below Nyquist for %.3f MHz bandwidth",
                 cfg->sample_rate_hz * 1e-6, cfg->bandwidth_hz * 1e-6);
        return PWR_ERR_CONFIG_INVALID;
    }
    if (cfg->pulse_width_s * cfg->prf_hz > cfg->duty_limit + 1e-12)
    {
        pwr_verr(err, err_cap,
                 "duty cycle %.3f%% exceeds the %.3f%% limit",
                 cfg->pulse_width_s * cfg->prf_hz * 100.0,
                 cfg->duty_limit * 100.0);
        return PWR_ERR_CONFIG_INVALID;
    }

    samples_per_pri = cfg->sample_rate_hz / cfg->prf_hz;
    n_tx            = (uint32_t)(cfg->pulse_width_s * cfg->sample_rate_hz + 0.5);
    if (n_tx < 2u)
    {
        pwr_verr(err, err_cap,
                 "pulse spans %u samples: raise the pulse width or sample rate",
                 n_tx);
        return PWR_ERR_CONFIG_INVALID;
    }
    if (samples_per_pri < (double)n_tx + 4.0)
    {
        pwr_verr(err, err_cap,
                 "PRI (%.0f samples) shorter than the pulse (%u samples)",
                 samples_per_pri, n_tx);
        return PWR_ERR_CONFIG_INVALID;
    }
    if (samples_per_pri > 1.0e7)
    {
        pwr_verr(err, err_cap, "PRI of %.0f samples exceeds the working limit",
                 samples_per_pri);
        return PWR_ERR_CONFIG_INVALID;
    }
    if (cfg->pulses_per_cpi < PWR_MIN_PULSES_PER_CPI ||
        cfg->pulses_per_cpi > PWR_MAX_PULSES_PER_CPI)
    {
        pwr_verr(err, err_cap, "pulses per CPI (%u) outside [%u, %u]",
                 cfg->pulses_per_cpi, PWR_MIN_PULSES_PER_CPI,
                 PWR_MAX_PULSES_PER_CPI);
        return PWR_ERR_CONFIG_INVALID;
    }
    if (!pwr_is_pow2(cfg->pulses_per_cpi))
    {
        pwr_verr(err, err_cap, "pulses per CPI (%u) must be a power of two",
                 cfg->pulses_per_cpi);
        return PWR_ERR_CONFIG_INVALID;
    }
    if (!pwr_is_pow2(cfg->doppler_bins) ||
        cfg->doppler_bins < cfg->pulses_per_cpi ||
        cfg->doppler_bins > PWR_MAX_DOPPLER_BINS)
    {
        pwr_verr(err, err_cap,
                 "Doppler FFT size (%u) must be a power of two in [%u, %u]",
                 cfg->doppler_bins, cfg->pulses_per_cpi, PWR_MAX_DOPPLER_BINS);
        return PWR_ERR_CONFIG_INVALID;
    }
    if (cfg->mti_mode > PWR_MTI_DC_REMOVAL &&
        cfg->pulses_per_cpi <= (uint32_t)cfg->mti_mode)
    {
        pwr_verr(err, err_cap,
                 "MTI order %d needs more than %u pulses per CPI",
                 cfg->mti_mode, cfg->pulses_per_cpi);
        return PWR_ERR_CONFIG_INVALID;
    }
    if (cfg->range_bins != 0u &&
        (cfg->range_bins < PWR_MIN_RANGE_BINS ||
         cfg->range_bins > PWR_MAX_RANGE_BINS))
    {
        pwr_verr(err, err_cap, "range bins (%u) outside [%u, %u]",
                 cfg->range_bins, PWR_MIN_RANGE_BINS, PWR_MAX_RANGE_BINS);
        return PWR_ERR_CONFIG_INVALID;
    }
    /* The CFAR window must fit inside the map. */
    {
        const int32_t win_r = 2 * (cfg->cfar.guard_range + cfg->cfar.train_range) + 1;
        const int32_t win_d = 2 * (cfg->cfar.guard_doppler + cfg->cfar.train_doppler) + 1;
        if (win_d > (int32_t)cfg->doppler_bins)
        {
            pwr_verr(err, err_cap,
                     "CFAR Doppler window (%d cells) wider than the map (%u)",
                     win_d, cfg->doppler_bins);
            return PWR_ERR_CONFIG_INVALID;
        }
        if (cfg->range_bins != 0u && win_r > (int32_t)cfg->range_bins)
        {
            pwr_verr(err, err_cap,
                     "CFAR range window (%d cells) wider than the map (%u)",
                     win_r, cfg->range_bins);
            return PWR_ERR_CONFIG_INVALID;
        }
    }
    if (!(cfg->cfar.pfa > 0.0) || !(cfg->cfar.pfa < 1.0))
    {
        pwr_verr(err, err_cap, "Pfa must lie strictly inside (0, 1)");
        return PWR_ERR_CONFIG_INVALID;
    }
    if (!(cfg->time_scale > 0.0))
    {
        pwr_verr(err, err_cap, "time scale must be positive");
        return PWR_ERR_CONFIG_INVALID;
    }
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Derived metrics
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_config_derive(const PWR_RadarConfig* cfg,
                                         PWR_DerivedMetrics* out)
{
    double lambda, bin_raw, rx_bw;
    uint32_t n_tx, spp, span_samples, decim, bins;

    if (cfg == NULL || out == NULL) { return PWR_ERR_NULL_POINTER; }
    memset(out, 0, sizeof(*out));

    lambda  = PWR_C_LIGHT / cfg->carrier_hz;
    bin_raw = PWR_C_LIGHT / (2.0 * cfg->sample_rate_hz);
    rx_bw   = (cfg->receiver_bandwidth_hz > 0.0) ? cfg->receiver_bandwidth_hz
                                                 : cfg->sample_rate_hz;

    n_tx = (uint32_t)(cfg->pulse_width_s * cfg->sample_rate_hz + 0.5);
    if (n_tx < 1u) { n_tx = 1u; }
    spp  = (uint32_t)(cfg->sample_rate_hz / cfg->prf_hz);
    if (spp < 2u) { spp = 2u; }

    /* Range window -> bin count -> decimation. */
    span_samples = (uint32_t)(cfg->range_span_m / bin_raw + 0.5);
    if (span_samples < PWR_MIN_RANGE_BINS) { span_samples = PWR_MIN_RANGE_BINS; }
    if (span_samples > spp)                { span_samples = spp; }

    /* Decimation is the *rounded* ratio, not the ceiling.  Rounding matters:
     * a 24.0 km span at 23.98 m per raw sample needs 1001 samples, so asking
     * for 1000 bins would otherwise trip the ceiling to a decimation of two
     * and silently halve the range sampling.  Rounding keeps the requested bin
     * count authoritative and lets the covered span differ from the request by
     * at most half a bin. */
    if (cfg->range_bins == 0u)
    {
        bins  = pwr_minu(span_samples, PWR_MAX_RANGE_BINS);
        decim = (bins > 0u) ? ((span_samples + bins / 2u) / bins) : 1u;
    }
    else
    {
        bins  = cfg->range_bins;
        decim = (span_samples + bins / 2u) / bins;
    }
    if (decim < 1u) { decim = 1u; }
    if (bins < 1u)  { bins  = 1u; }
    /* Keep every bin inside the receive window. */
    {
        const uint32_t offset = (uint32_t)(cfg->range_start_m / bin_raw + 0.5);
        uint32_t avail = (spp > offset) ? (spp - offset) : 1u;
        uint32_t maxbins = avail / decim;
        if (maxbins < 1u) { maxbins = 1u; }
        if (bins > maxbins) { bins = maxbins; }
        if (bins < PWR_MIN_RANGE_BINS)
        {
            bins = pwr_minu(PWR_MIN_RANGE_BINS, maxbins);
            if (bins < 1u) { bins = 1u; }
        }
        out->samples_per_pri = spp;

        /* The fast-time FFT only has to span the samples that can reach a
         * displayed range bin - the gate window plus one uncompressed pulse -
         * with two further pulse lengths of zero padding so the circular
         * convolution's wrap-around (the matched taps and the equaliser's
         * acausal tail) lands in silence.  A short displayed span therefore
         * buys a proportionally smaller transform; the full PRI stays the
         * upper bound.  The engine keeps the matching copy width in
         * fast_copy. */
        {
            const uint32_t need  = offset + (bins - 1u) * decim + 1u + n_tx;
            const uint32_t full  = pwr_pow2_ceil(spp + n_tx);
            uint32_t       gated = pwr_pow2_ceil(need + 2u * n_tx);
            if (gated > full) { gated = full; }
            out->fast_time_fft_size = gated;
        }
    }

    out->wavelength_m               = lambda;
    out->range_resolution_m         = PWR_C_LIGHT / (2.0 * cfg->bandwidth_hz);
    out->compressed_pulse_width_s   = 1.0 / cfg->bandwidth_hz;
    out->time_bandwidth_product     = cfg->pulse_width_s * cfg->bandwidth_hz;
    out->pulse_compression_gain_db  =
        10.0 * log10(pwr_maxd(out->time_bandwidth_product, 1.0));
    out->coherent_integration_gain_db =
        10.0 * log10((double)cfg->pulses_per_cpi);
    out->unambiguous_range_m        = PWR_C_LIGHT / (2.0 * cfg->prf_hz);
    out->unambiguous_velocity_mps   = lambda * cfg->prf_hz / 4.0;
    out->velocity_resolution_mps    =
        lambda * cfg->prf_hz / (2.0 * (double)cfg->pulses_per_cpi);
    out->blind_range_m              = PWR_C_LIGHT * cfg->pulse_width_s / 2.0;
    out->cpi_duration_s             = (double)cfg->pulses_per_cpi / cfg->prf_hz;
    out->duty_cycle                 = cfg->pulse_width_s * cfg->prf_hz;
    out->average_power_w            = cfg->peak_power_w * out->duty_cycle;
    out->noise_power_w              = PWR_BOLTZMANN * PWR_T0_KELVIN * rx_bw *
                                      pwr_db_to_pow(cfg->noise_figure_db);
    out->noise_power_dbm            = 10.0 * log10(out->noise_power_w) + 30.0;
    out->range_bin_spacing_m        = bin_raw * (double)decim;
    out->range_bins                 = bins;
    out->doppler_bins               = cfg->doppler_bins;
    out->azimuth_cell_deg           = 360.0 / (double)pwr_maxu(cfg->ppi_azimuth_cells, 1u);
    out->scan_period_s              = (cfg->scan_rate_rpm > 0.0)
                                      ? (60.0 / cfg->scan_rate_rpm) : 0.0;
    out->pulses_per_beamwidth       = (cfg->scan_rate_rpm > 0.0)
        ? (cfg->azimuth_beamwidth_deg / 360.0 * out->scan_period_s * cfg->prf_hz)
        : (double)cfg->pulses_per_cpi;

    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Link budget
 * ==========================================================================
 *  Energy form of the radar range equation.  After matched filtering the
 *  achievable single-pulse SNR depends on the transmitted *energy* Pt*Tp and
 *  not on the receiver bandwidth, which is why no B term appears:
 *
 *      SNR = Pt * Tp * Gt * Gr * lambda^2 * sigma
 *            ---------------------------------------
 *            (4*pi)^3 * R^4 * k * T0 * F * L
 * ------------------------------------------------------------------------ */
PWR_EXPORT(double) pwr_snr_single_pulse_db(const PWR_RadarConfig* cfg,
                                           double rcs_m2, double range_m)
{
    double lambda, num_db, den_db;

    if (cfg == NULL || !(range_m > 0.0) || !(rcs_m2 > 0.0))
    {
        return -300.0;
    }
    lambda = PWR_C_LIGHT / cfg->carrier_hz;

    num_db = 10.0 * log10(cfg->peak_power_w)
           + 10.0 * log10(cfg->pulse_width_s)
           + cfg->tx_gain_db + cfg->rx_gain_db
           + 20.0 * log10(lambda)
           + 10.0 * log10(rcs_m2);

    den_db = 30.0 * log10(4.0 * PWR_PI)
           + 40.0 * log10(range_m)
           + 10.0 * log10(PWR_BOLTZMANN * PWR_T0_KELVIN)
           + cfg->noise_figure_db
           + cfg->system_loss_db;

    return num_db - den_db;
}

PWR_EXPORT(double) pwr_max_range_for_snr(const PWR_RadarConfig* cfg,
                                         double rcs_m2,
                                         double required_snr_db)
{
    double snr_at_1km;
    if (cfg == NULL || !(rcs_m2 > 0.0)) { return 0.0; }
    snr_at_1km = pwr_snr_single_pulse_db(cfg, rcs_m2, 1000.0);
    /* SNR falls as R^-4  ->  R = 1 km * 10^((SNR_1km - SNR_req)/40) */
    return 1000.0 * pow(10.0, (snr_at_1km - required_snr_db) / 40.0);
}
