/* Built-in numerical acceptance suite. Exported so the console can run it at
 * start-up and a CI job can gate a build on it.
 *
 * Every case asserts a property a defect in the chain would break:
 *   T1  FFT agrees with a direct DFT and round-trips
 *   T2  window tapers are positive, unit-peak and have a sane ENBW
 *   T3  order statistics agree with a full sort
 *   T4  the linear-assignment solver reaches the known optimum
 *   T5  the 2x2 Kalman helpers invert and stay symmetric
 *   T6  a noise-free point target compresses into the correct range bin
 *   T7  a target's measured range rate matches its true range rate
 *   T8  the noise-only false-alarm rate tracks the design Pfa
 *   T9  a straight-line target is tracked to within a few tens of metres
 *   T10 every CFAR family achieves its design Pfa, not just cell averaging
 *   T11 a rotating antenna yields one dwell-merged plot per target per scan,
 *       centroided to a fraction of a beamwidth
 *   T12 a rotating track books one M-of-N attempt per revolution and retires
 *       with exactly one end-of-track report
 *   T13 measured detection probability matches the non-central chi-square
 *       prediction across a range of signal-to-noise ratios
 *   T14 a scenario survives a save / load / save round trip byte for byte,
 *       and an invalid file is refused rather than half-applied
 *   T15 a target past the effective-earth horizon falls into shadow instead of
 *       being seen at full strength
 *   T16 a cosecant-squared fill holds received power constant for a target at
 *       constant altitude
 *   T17 MTI cancels a sea better when the antenna turns slower, because scan
 *       modulation is part of the clutter spectrum
 *   T18 a compound-K sea produces more false plots than the Rayleigh field the
 *       CFAR multipliers are derived for
 *   T19 a hull longer than a resolution cell occupies several range cells
 *   T20 a turn neither blows up the tracker's error nor costs it the lock
 *   T21 a new track is seeded with a measured range rate only when the radar
 *       cannot have folded it
 *
 * T11 and T12 exist because everything above them stares: with
 * scan_rate_rpm == 0 the dwell-merge pass and the tracker's per-scan branch
 * are both skipped entirely, so the default 24 rpm configuration was the one
 * nothing covered.
 */
#include "pwr_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PWR_TestCtx
{
    char*  buf;
    size_t cap;
    size_t len;
    int    failures;
    int    cases;
} PWR_TestCtx;

static void pwr_tprintf(PWR_TestCtx* c, const char* fmt, ...)
{
    va_list ap;
    int n;
    if (c->buf == NULL || c->len >= c->cap) { return; }
    va_start(ap, fmt);
    n = vsnprintf(c->buf + c->len, c->cap - c->len, fmt, ap);
    va_end(ap);
    if (n > 0) { c->len += (size_t)n; }
    if (c->len > c->cap) { c->len = c->cap; }
}

static void pwr_check(PWR_TestCtx* c, const char* name, int ok, const char* detail)
{
    ++c->cases;
    if (ok == 0) { ++c->failures; }
    pwr_tprintf(c, "  [%s] %-34s %s\n", (ok != 0) ? "PASS" : "FAIL", name,
                (detail != NULL) ? detail : "");
}

/* ==========================================================================
 *  T1 - FFT
 * ========================================================================== */
static void pwr_test_fft(PWR_TestCtx* c)
{
    static const uint32_t sizes[] = { 2u, 4u, 8u, 16u, 32u, 64u, 256u, 1024u };
    char detail[96];
    double worst_fwd = 0.0, worst_rt = 0.0;
    uint32_t s;
    PWR_Rng rng;

    pwr_rng_seed(&rng, 0xC0FFEEu, 7u);

    for (s = 0u; s < sizeof(sizes) / sizeof(sizes[0]); ++s)
    {
        const uint32_t n = sizes[s];
        PWR_Complex* a = PWR_ALLOC_ARRAY(PWR_Complex, n);
        PWR_Complex* b = PWR_ALLOC_ARRAY(PWR_Complex, n);
        PWR_FftPlan* p = NULL;
        uint32_t i, k;
        double num = 0.0, den = 0.0, rt = 0.0;

        if (a == NULL || b == NULL) { PWR_FREE(a); PWR_FREE(b); continue; }
        for (i = 0u; i < n; ++i)
        {
            a[i] = pwr_c((pwr_real)(2.0 * pwr_rng_uniform(&rng) - 1.0),
                         (pwr_real)(2.0 * pwr_rng_uniform(&rng) - 1.0));
            b[i] = a[i];
        }
        if (pwr_fft_plan_create(&p, n) != PWR_STATUS_OK) { PWR_FREE(a); PWR_FREE(b); continue; }
        pwr_fft_run(p, b);

        for (k = 0u; k < n; ++k)
        {
            double sr = 0.0, si = 0.0, dr, di;
            for (i = 0u; i < n; ++i)
            {
                const double ang = -PWR_TWO_PI * (double)k * (double)i / (double)n;
                sr += (double)a[i].re * cos(ang) - (double)a[i].im * sin(ang);
                si += (double)a[i].re * sin(ang) + (double)a[i].im * cos(ang);
            }
            dr = (double)b[k].re - sr;
            di = (double)b[k].im - si;
            num += dr * dr + di * di;
            den += sr * sr + si * si;
        }
        pwr_ifft_run(p, b);
        for (i = 0u; i < n; ++i)
        {
            const double dr = (double)b[i].re - (double)a[i].re;
            const double di = (double)b[i].im - (double)a[i].im;
            rt += dr * dr + di * di;
        }
        if (den > 0.0)
        {
            const double rel = sqrt(num / den);
            if (rel > worst_fwd) { worst_fwd = rel; }
        }
        rt = sqrt(rt / (double)n);
        if (rt > worst_rt) { worst_rt = rt; }

        pwr_fft_plan_destroy(p);
        PWR_FREE(a);
        PWR_FREE(b);
    }

    (void)snprintf(detail, sizeof(detail),
                   "fwd rel %.2e, round trip %.2e", worst_fwd, worst_rt);
    pwr_check(c, "T1 FFT vs direct DFT",
              (worst_fwd < 1.0e-5 && worst_rt < 1.0e-5) ? 1 : 0, detail);
}

/* ==========================================================================
 *  T2 - windows
 * ========================================================================== */
static void pwr_test_windows(PWR_TestCtx* c)
{
    char detail[128];
    int ok = 1;
    int worst_type = -1;
    double worst_enbw = 0.0;
    int t;

    for (t = 0; t < PWR_WIN_COUNT; ++t)
    {
        pwr_real w[64];
        double cg, enbw, peak = 0.0;
        uint32_t i;

        if (pwr_window_generate((PWR_WindowType)t, w, 64u) != PWR_STATUS_OK) { ok = 0; break; }
        for (i = 0u; i < 64u; ++i)
        {
            if (!(w[i] >= 0.0f) || !(w[i] <= 1.0001f))
            {
                ok = 0;
                worst_type = t;
            }
            if ((double)w[i] > peak) { peak = (double)w[i]; }
        }
        cg   = pwr_window_coherent_gain(w, 64u);
        enbw = pwr_window_enbw(w, 64u);
        if (!(peak > 0.5) || !(cg > 0.15) || !(cg <= 1.0001) ||
            !(enbw >= 0.999) || !(enbw < 3.0))
        {
            ok = 0;
            worst_type = t;
            worst_enbw = enbw;
        }
    }
    if (ok != 0)
    {
        (void)snprintf(detail, sizeof(detail), "%d tapers checked", PWR_WIN_COUNT);
    }
    else
    {
        (void)snprintf(detail, sizeof(detail), "taper %d, ENBW %.3f",
                       worst_type, worst_enbw);
    }
    pwr_check(c, "T2 amplitude tapers", ok, detail);
}

/* ==========================================================================
 *  T3 - order statistics
 * ========================================================================== */
static void pwr_test_order_stats(PWR_TestCtx* c)
{
    pwr_real a[97], b[97], d[97];
    PWR_Rng rng;
    uint32_t i, k;
    int ok = 1;

    pwr_rng_seed(&rng, 4242u, 3u);
    for (i = 0u; i < 97u; ++i)
    {
        a[i] = (pwr_real)pwr_rng_uniform(&rng);
        b[i] = a[i];
    }
    pwr_sort_f32(b, 97u);
    for (i = 1u; i < 97u; ++i) { if (b[i] < b[i - 1u]) { ok = 0; } }
    for (k = 0u; k < 97u; k += 7u)
    {
        memcpy(d, a, sizeof(a));
        if (pwr_select_kth_f32(d, 97u, k) != b[k]) { ok = 0; }
    }
    pwr_check(c, "T3 sort and quickselect", ok, "97 samples");
}

/* ==========================================================================
 *  T4 - linear assignment
 * ========================================================================== */
static void pwr_test_assignment(PWR_TestCtx* c)
{
    /* 3x4 problem whose unique optimum (cost 6) is 0->1, 1->0, 2->3.
     * A greedy solver picks 0->0 first and lands on 8, so this genuinely
     * exercises the augmenting-path search. */
    static const double cost[12] = {
        1.0, 2.0, 9.0, 9.0,
        2.0, 4.0, 9.0, 9.0,
        9.0, 9.0, 9.0, 2.0
    };
    int32_t row[3];
    double u[8], v[8], minv[8];
    int32_t way[8], p[8];
    uint8_t used[8];
    double total = 0.0;
    int ok;
    char detail[64];
    uint32_t i;

    pwr_assign_jv(cost, 3u, 4u, 4u, row, u, v, minv, way, p, used);
    for (i = 0u; i < 3u; ++i)
    {
        if (row[i] < 0 || row[i] > 3) { total += 1000.0; }
        else { total += cost[i * 4u + (uint32_t)row[i]]; }
    }
    ok = (fabs(total - 6.0) < 1e-9) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail), "optimum 6, got %.1f", total);
    pwr_check(c, "T4 global nearest neighbour", ok, detail);
}

/* ==========================================================================
 *  T5 - Kalman helpers
 * ========================================================================== */
static void pwr_test_linalg(PWR_TestCtx* c)
{
    const double A[4] = { 4.0, 1.0, 1.0, 3.0 };
    double Ai[4], I[4];
    int ok = 1;
    char detail[64];

    if (pwr_mat2_inv(A, Ai) != PWR_STATUS_OK) { ok = 0; }
    pwr_mat_mul(A, Ai, I, 2u);
    if (fabs(I[0] - 1.0) > 1e-12 || fabs(I[3] - 1.0) > 1e-12 ||
        fabs(I[1]) > 1e-12 || fabs(I[2]) > 1e-12) { ok = 0; }
    {
        const double d[2] = { 2.0, -1.0 };
        const double m = pwr_mahalanobis2(Ai, d);
        /* d' A^-1 d with A^-1 = [3 -1; -1 4]/11  ->  (12+4+4)/11 = 20/11 */
        if (fabs(m - 20.0 / 11.0) > 1e-12) { ok = 0; }
    }
    {
        double S[4] = { 1.0, 0.4, 0.6, 2.0 };
        pwr_mat_symmetrise(S, 2u);
        if (fabs(S[1] - S[2]) > 1e-15 || fabs(S[1] - 0.5) > 1e-15) { ok = 0; }
    }
    (void)snprintf(detail, sizeof(detail), "2x2 inverse, gate, symmetrise");
    pwr_check(c, "T5 tracker linear algebra", ok, detail);
}

/* ==========================================================================
 *  Shared harness for the end-to-end cases
 * ========================================================================== */
static PWR_Engine* pwr_test_engine(double range_span_m, int noise, int clutter)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread    = 0;         /* driven by pwr_engine_step()          */
    cfg.deterministic    = 1;
    cfg.scan_rate_rpm    = 0.0;       /* stare at 0 deg so the target is lit  */
    cfg.range_span_m     = range_span_m;
    cfg.range_bins       = 512u;
    cfg.ppi_azimuth_cells = 256u;
    cfg.rti_rows         = 32u;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { return NULL; }

    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = (noise != 0) ? 1 : 0;
    env.enable_sea_clutter   = (clutter != 0) ? 1 : 0;
    env.enable_rain          = 0;
    env.enable_jammer        = 0;
    env.enable_eclipsing     = 1;
    (void)pwr_engine_set_environment(e, &env);
    return e;
}

static void pwr_test_add_target(PWR_Engine* e, double range_m, double alt_m,
                                double range_rate_mps, double rcs)
{
    PWR_SimTarget t;
    memset(&t, 0, sizeof(t));
    /* Bearing 0 (North) so the boresight illuminates it; the radial direction
     * is +y, hence vy is the range rate. */
    t.x_m = 0.0;
    t.y_m = range_m;
    t.z_m = alt_m;
    t.vy_mps = range_rate_mps;
    t.rcs_m2 = rcs;
    t.swerling = PWR_SWERLING_0;
    t.target_class = PWR_CLASS_AIR;
    t.enabled = 1;
    (void)snprintf(t.label, sizeof(t.label), "SELFTEST");
    (void)pwr_engine_target_add(e, &t);
}

/* --------------------------------------------------------------------------
 *  Rotating harness
 *  ----------------
 *  The staring harness above cannot reach two of the most intricate pieces of
 *  the library.  With scan_rate_rpm == 0 the derived scan period is exactly
 *  zero, so pwr_plots_dwell_merge() returns on its first statement and the
 *  whole azimuth beam-splitting centroid never executes, while
 *  pwr_tracker_update() takes its staring branch and never books a per-scan
 *  M-of-N attempt or applies a scan-staleness retirement.  The default
 *  operating configuration rotates at 24 rpm, so those are precisely the paths
 *  a fielded configuration runs and the paths nothing was checking.
 * ------------------------------------------------------------------------ */
static PWR_Engine* pwr_test_engine_rotating(void)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;

    (void)pwr_config_default(&cfg);          /* 24 rpm, 1.6 deg beam           */
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.range_span_m      = 24000.0;
    /* Coarse on purpose: these cases run whole revolutions, and what they
     * check - one plot per dwell, one M-of-N attempt per revolution - does not
     * depend on range resolution.  A fine grid would only make the suite slow
     * enough to sit awkwardly in a build script. */
    cfg.range_bins        = 256u;
    cfg.ppi_azimuth_cells = 128u;
    cfg.rti_rows          = 16u;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { return NULL; }

    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 0;
    env.enable_rain          = 0;
    env.enable_jammer        = 0;
    env.enable_eclipsing     = 1;
    (void)pwr_engine_set_environment(e, &env);
    return e;
}

/* Places a target on a bearing, opening radially so its bearing is constant
 * and its range rate is well clear of the zero-Doppler censor notch. */
static int32_t pwr_test_add_polar(PWR_Engine* e, double bearing_deg,
                                  double range_m, double range_rate_mps,
                                  double rcs)
{
    PWR_SimTarget t;
    const double b = pwr_deg_to_rad(bearing_deg);
    memset(&t, 0, sizeof(t));
    t.x_m    = range_m * sin(b);
    t.y_m    = range_m * cos(b);
    t.z_m    = 0.0;
    t.vx_mps = range_rate_mps * sin(b);
    t.vy_mps = range_rate_mps * cos(b);
    t.rcs_m2 = rcs;
    t.swerling = PWR_SWERLING_0;
    t.target_class = PWR_CLASS_AIR;
    t.enabled = 1;
    (void)snprintf(t.label, sizeof(t.label), "ROTTEST");
    (void)pwr_engine_target_add(e, &t);
    return t.id;
}

/* ==========================================================================
 *  T6 - pulse compression range accuracy
 * ========================================================================== */
static void pwr_test_range_accuracy(PWR_TestCtx* c)
{
    PWR_Engine* e = pwr_test_engine(20000.0, 1, 0);
    const double truth = 12000.0;
    char detail[128];
    int ok = 0;
    double measured = -1.0;

    if (e == NULL) { pwr_check(c, "T6 range accuracy", 0, "engine creation failed"); return; }
    pwr_test_add_target(e, truth, 0.0, 0.0, 100.0);
    (void)pwr_engine_step(e, 2u);

    {
        PWR_Frame f;
        if (pwr_engine_frame_acquire(e, &f) == PWR_STATUS_OK)
        {
            uint32_t i, best = 0u;
            pwr_real peak = PWR_DB_FLOOR;
            for (i = 0u; i < f.range_bins; ++i)
            {
                if (f.range_profile_db[i] > peak)
                {
                    peak = f.range_profile_db[i];
                    best = i;
                }
            }
            measured = f.range_first_m + (double)best * f.range_step_m;
            /* Allow one bin of quantisation plus the slant/height correction. */
            ok = (fabs(measured - truth) <= 1.5 * f.range_step_m) ? 1 : 0;
            (void)pwr_engine_frame_release(e);
        }
    }
    (void)snprintf(detail, sizeof(detail), "truth %.0f m, peak %.0f m",
                   truth, measured);
    pwr_check(c, "T6 pulse compression range", ok, detail);
    pwr_engine_destroy(e);
}

/* ==========================================================================
 *  T7 - Doppler axis calibration
 * ========================================================================== */
static void pwr_test_doppler_accuracy(PWR_TestCtx* c)
{
    PWR_Engine* e = pwr_test_engine(20000.0, 1, 0);
    const double truth_rate = -35.0;     /* closing at 35 m/s */
    char detail[128];
    int ok = 0;
    double measured = -999.0;

    if (e == NULL) { pwr_check(c, "T7 Doppler axis", 0, "engine creation failed"); return; }
    pwr_test_add_target(e, 12000.0, 2000.0, truth_rate, 100.0);
    (void)pwr_engine_step(e, 2u);

    {
        PWR_Frame f;
        if (pwr_engine_frame_acquire(e, &f) == PWR_STATUS_OK)
        {
            uint32_t i, bj = 0u, br = 0u;
            pwr_real peak = PWR_DB_FLOOR;
            for (i = 0u; i < f.doppler_bins * f.range_bins; ++i)
            {
                if (f.rd_map_db[i] > peak)
                {
                    peak = f.rd_map_db[i];
                    bj = i / f.range_bins;
                    br = i % f.range_bins;
                }
            }
            PWR_UNUSED(br);
            measured = f.velocity_first_mps + (double)bj * f.velocity_step_mps;
            /* One Doppler bin of quantisation is the achievable accuracy. */
            ok = (fabs(measured - truth_rate) <= 1.5 * fabs(f.velocity_step_mps))
                 ? 1 : 0;
            (void)pwr_engine_frame_release(e);
        }
    }
    (void)snprintf(detail, sizeof(detail), "truth %.1f m/s, peak %.1f m/s",
                   truth_rate, measured);
    pwr_check(c, "T7 Doppler axis calibration", ok, detail);
    pwr_engine_destroy(e);
}

/* ==========================================================================
 *  T8 - false-alarm rate on noise only
 * ==========================================================================
 *  The achieved rate is compared against the design Pfa with a generous
 *  one-decade window: the estimator is a finite-sample average over roughly
 *  half a million cells, and clustering merges adjacent alarms, so agreement
 *  to better than a decade is all that is meaningful.
 * ------------------------------------------------------------------------ */
static void pwr_test_pfa(PWR_TestCtx* c)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;
    char detail[160];
    int ok = 0;
    double achieved = 0.0;
    const double design = 1.0e-4;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.scan_rate_rpm     = 0.0;
    cfg.range_span_m      = 12000.0;
    cfg.range_bins        = 400u;
    cfg.ppi_azimuth_cells = 128u;
    cfg.rti_rows          = 16u;
    cfg.cfar.pfa          = design;
    cfg.cfar.censor_zero_doppler = 0;
    cfg.cluster.enable    = 0;        /* count raw cells, not clusters */
    cfg.cluster.min_cells = 1;
    cfg.cluster.min_snr_db = -100.0;
    cfg.tracker.enable    = 0;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK)
    {
        pwr_check(c, "T8 CFAR false-alarm rate", 0, "engine creation failed");
        return;
    }
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 0;
    env.enable_eclipsing     = 0;
    (void)pwr_engine_set_environment(e, &env);

    (void)pwr_engine_step(e, 24u);
    {
        PWR_Stats st;
        (void)pwr_engine_get_stats(e, &st);
        if (st.cells_tested_total > 0u)
        {
            achieved = (double)st.detection_total / (double)st.cells_tested_total;
        }
        ok = (achieved > design * 0.05 && achieved < design * 20.0) ? 1 : 0;
        (void)snprintf(detail, sizeof(detail),
                       "design %.1e, achieved %.2e over %llu cells",
                       design, achieved,
                       (unsigned long long)st.cells_tested_total);
    }
    pwr_check(c, "T8 CFAR false-alarm rate", ok, detail);
    pwr_engine_destroy(e);
}

/* ==========================================================================
 *  T9 - end-to-end tracking accuracy
 * ========================================================================== */
static void pwr_test_tracking(PWR_TestCtx* c)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;
    char detail[160];
    int ok = 0;
    double err = 1.0e9;
    uint32_t confirmed = 0u;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.scan_rate_rpm     = 0.0;       /* staring: one update per CPI          */
    cfg.range_span_m      = 24000.0;
    cfg.range_bins        = 512u;
    cfg.ppi_azimuth_cells = 128u;
    cfg.rti_rows          = 16u;
    cfg.tracker.confirm_m = 3;
    cfg.tracker.confirm_n = 5;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK)
    {
        pwr_check(c, "T9 track accuracy", 0, "engine creation failed");
        return;
    }
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 0;
    env.enable_eclipsing     = 1;
    (void)pwr_engine_set_environment(e, &env);
    pwr_test_add_target(e, 15000.0, 3000.0, -120.0, 20.0);

    (void)pwr_engine_step(e, 40u);
    {
        PWR_Frame f;
        if (pwr_engine_frame_acquire(e, &f) == PWR_STATUS_OK)
        {
            uint32_t i;
            /* Ground truth: the target started at y = 15000 m and closes at
             * 120 m/s along +y for f.time_s seconds. */
            const double truth_y = 15000.0 - 120.0 * f.time_s;
            for (i = 0u; i < f.track_count; ++i)
            {
                const double d = fabs(f.tracks[i].y_m - truth_y);
                if (f.tracks[i].state == PWR_TRACK_CONFIRMED) { ++confirmed; }
                if (d < err) { err = d; }
            }
            ok = (confirmed >= 1u && err < 400.0) ? 1 : 0;
            (void)snprintf(detail, sizeof(detail),
                           "%u confirmed, best |dy| = %.0f m at t = %.2f s",
                           confirmed, err, f.time_s);
            (void)pwr_engine_frame_release(e);
        }
        else
        {
            (void)snprintf(detail, sizeof(detail), "no frame published");
        }
    }
    pwr_check(c, "T9 end-to-end tracking", ok, detail);
    pwr_engine_destroy(e);
}

/* ==========================================================================
 *  T10 - threshold multipliers of every CFAR family
 * ==========================================================================
 *  Each family thresholds a different statistic and therefore needs its own
 *  multiplier.  Borrowing the cell-averaging one - which is what this used to
 *  do for the trimmed mean, and at half window for greatest-of and
 *  smallest-of - puts the achieved false-alarm rate out by more than an order
 *  of magnitude, silently, on a knob the operator is invited to turn.
 *
 *  The geometry here is deliberately critically sampled: rectangular tapers,
 *  no Doppler oversampling, sample rate equal to the bandwidth.  Neighbouring
 *  cells are then close to independent, which is what every multiplier
 *  assumes, so the achieved rate can be held to the design value instead of to
 *  a decade of it as in T8.  Peak selection and clustering are off, so what is
 *  counted is raw threshold crossings.
 * ------------------------------------------------------------------------ */
static void pwr_test_cfar_families(PWR_TestCtx* c)
{
    static const int32_t types[PWR_CFAR_COUNT] = {
        PWR_CFAR_CA, PWR_CFAR_GOCA, PWR_CFAR_SOCA, PWR_CFAR_OS, PWR_CFAR_TM
    };
    const double design = 1.0e-3;
    char detail[224];
    size_t len = 0u;
    int ok = 1;
    int t;

    for (t = 0; t < PWR_CFAR_COUNT; ++t)
    {
        PWR_RadarConfig cfg;
        PWR_SimEnvironment env;
        PWR_Engine* e = NULL;
        double ratio = 0.0;
        int n;

        (void)pwr_config_default(&cfg);
        cfg.worker_thread     = 0;
        cfg.deterministic     = 1;
        cfg.scan_rate_rpm     = 0.0;
        cfg.range_span_m      = 12000.0;
        cfg.range_bins        = 400u;
        cfg.ppi_azimuth_cells = 128u;
        cfg.rti_rows          = 16u;
        cfg.sample_rate_hz    = cfg.bandwidth_hz;   /* critically sampled     */
        cfg.doppler_bins      = cfg.pulses_per_cpi; /* no Doppler oversample  */
        cfg.range_window      = PWR_WIN_RECTANGULAR;
        cfg.doppler_window    = PWR_WIN_RECTANGULAR;
        cfg.mti_mode          = PWR_MTI_OFF;
        cfg.cfar.type                = types[t];
        cfg.cfar.pfa                 = design;
        cfg.cfar.censor_zero_doppler = 0;
        cfg.cfar.peak_selection      = 0;
        /* A range-only reference window, which is what OS and TM use anyway,
         * so all five families are compared over the same 24 cells.  It also
         * leaves the two halves exactly equal at 12 cells each - the condition
         * the greatest-of / smallest-of expressions are derived under - and
         * keeps the window small enough that a borrowed multiplier shows up as
         * a factor of several rather than a few per cent. */
        cfg.cfar.guard_doppler       = 0;
        cfg.cfar.train_doppler       = 0;
        cfg.cluster.enable     = 0;
        cfg.cluster.min_cells  = 1;
        cfg.cluster.min_snr_db = -100.0;
        cfg.tracker.enable     = 0;
        if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { ok = 0; break; }

        (void)pwr_engine_get_environment(e, &env);
        env.enable_thermal_noise = 1;
        env.enable_sea_clutter   = 0;
        env.enable_eclipsing     = 0;
        (void)pwr_engine_set_environment(e, &env);

        (void)pwr_engine_step(e, 40u);
        {
            PWR_Stats st;
            (void)pwr_engine_get_stats(e, &st);
            if (st.cells_tested_total > 0u)
            {
                ratio = ((double)st.detection_total /
                         (double)st.cells_tested_total) / design;
            }
            /* A saturated plot table would make the count a floor, not a rate. */
            if (st.detections_dropped != 0u) { ok = 0; }
        }
        pwr_engine_destroy(e);

        if (!(ratio > 0.5 && ratio < 2.0)) { ok = 0; }
        if (len < sizeof(detail))
        {
            n = snprintf(detail + len, sizeof(detail) - len, "%s%s %.2f",
                         (t > 0) ? ", " : "",
                         pwr_cfar_name((PWR_CfarType)types[t]), ratio);
            if (n > 0) { len += (size_t)n; }
            if (len > sizeof(detail)) { len = sizeof(detail); }
        }
    }
    pwr_check(c, "T10 CFAR family multipliers", ok, detail);
}

/* ==========================================================================
 *  T11 - dwell-merged plot while rotating (azimuth beam splitting)
 * ==========================================================================
 *  A target stays above threshold for more than one CPI as the beam sweeps
 *  across it, so the plot extractor has to fold those per-CPI hits - each
 *  stamped with a boresight quantised to the 1.54 deg the antenna turns in a
 *  CPI - into a single power-weighted plot.  Two things are under test and
 *  both are invisible to a staring configuration: that one target yields one
 *  plot per revolution rather than a string of them spread over degrees, and
 *  that the centroid is accurate to a fraction of a beamwidth.
 * ------------------------------------------------------------------------ */
static void pwr_test_rotating_dwell(PWR_TestCtx* c)
{
    PWR_Engine* e = pwr_test_engine_rotating();
    PWR_DerivedMetrics dm;
    const double truth_az = 137.0;
    const double r0       = 12000.0;
    const double rdot     = 20.0;
    const uint32_t scans  = 4u;
    char detail[176];
    uint32_t plots = 0u, cpi, n_cpi;
    double worst_az = 0.0, sum_az = 0.0, sum_snr = 0.0;
    int ok;

    if (e == NULL)
    {
        pwr_check(c, "T11 rotating dwell plot", 0, "engine creation failed");
        return;
    }
    (void)pwr_engine_get_metrics(e, &dm);
    /* The cross-section is chosen for a post-integration SNR around 30 dB:
     * high enough to be detected on every revolution, low enough that the
     * compression range sidelobes stay under the threshold.  A much stronger
     * target would put its own Taylor sidelobes above threshold as separate
     * clusters a bin or two away, and those are genuinely distinct plots -
     * the dwell merge is right not to fold them in, but they would make this
     * a test of sidelobe suppression rather than of azimuth centroiding. */
    (void)pwr_test_add_polar(e, truth_az, r0, rdot, 0.05);
    n_cpi = (uint32_t)((double)scans * dm.scan_period_s / dm.cpi_duration_s);

    for (cpi = 0u; cpi < n_cpi; ++cpi)
    {
        PWR_Frame f;
        uint32_t k;
        (void)pwr_engine_step(e, 1u);
        if (pwr_engine_frame_acquire(e, &f) != PWR_STATUS_OK) { continue; }
        for (k = 0u; k < f.detection_count; ++k)
        {
            const double truth_r = r0 + rdot * f.time_s;
            if (fabs(f.detections[k].range_m - truth_r) > 500.0) { continue; }
            {
                const double daz = fabs(pwr_angle_diff(
                    f.detections[k].azimuth_deg, truth_az));
                if (daz > worst_az) { worst_az = daz; }
                sum_az  += daz;
                sum_snr += f.detections[k].snr_db;
                ++plots;
            }
        }
        (void)pwr_engine_frame_release(e);
    }

    /* One plot per revolution, give or take a scan at either end, and a
     * centroid inside a third of the 1.6 deg beamwidth. */
    ok = (plots >= scans - 1u && plots <= scans + 1u && worst_az < 0.55) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail),
                   "%u plots / %u scans at %.0f dB, az err mean %.2f worst %.2f deg",
                   plots, scans,
                   (plots > 0u) ? sum_snr / (double)plots : 0.0,
                   (plots > 0u) ? sum_az / (double)plots : 0.0, worst_az);
    pwr_check(c, "T11 rotating dwell plot", ok, detail);
    pwr_engine_destroy(e);
}

/* ==========================================================================
 *  T12 - track life cycle while rotating
 * ==========================================================================
 *  While the antenna rotates a track is only illuminated once a revolution, so
 *  the M-of-N evidence has to be booked per scan and not per CPI - book it per
 *  CPI and a 2.5 s scan period retires every track within a fraction of a
 *  revolution.  This checks the attempt counter really does advance once per
 *  revolution (a per-CPI booking would show ~230 times more), that the track
 *  confirms, and that removing the target retires it through the scan
 *  staleness rule with exactly one PWR_TRACK_TERMINATED report - the
 *  end-of-track notification a consumer deletes on.
 * ------------------------------------------------------------------------ */
static void pwr_test_rotating_track(PWR_TestCtx* c)
{
    PWR_Engine* e = pwr_test_engine_rotating();
    PWR_DerivedMetrics dm;
    char detail[208];
    uint32_t cpi, per_scan, id = 0u, attempts = 0u, term_seen = 0u;
    uint32_t alive_scans = 5u, dead_scans = 7u;
    int32_t tgt_id;
    int confirmed = 0, ok;

    if (e == NULL)
    {
        pwr_check(c, "T12 rotating track life cycle", 0, "engine creation failed");
        return;
    }
    (void)pwr_engine_get_metrics(e, &dm);
    tgt_id   = pwr_test_add_polar(e, 42.0, 14000.0, -60.0, 30.0);
    per_scan = (uint32_t)(dm.scan_period_s / dm.cpi_duration_s + 0.5);

    /* ---- with the target present ------------------------------------- */
    for (cpi = 0u; cpi < alive_scans * per_scan; ++cpi)
    {
        PWR_Frame f;
        uint32_t k;
        (void)pwr_engine_step(e, 1u);
        if (pwr_engine_frame_acquire(e, &f) != PWR_STATUS_OK) { continue; }
        for (k = 0u; k < f.track_count; ++k)
        {
            const PWR_Track* t = &f.tracks[k];
            if (t->state != PWR_TRACK_CONFIRMED &&
                t->state != PWR_TRACK_COASTING) { continue; }
            if (fabs(t->range_m - 14000.0) > 2000.0) { continue; }
            confirmed = 1;
            id        = t->id;
            attempts  = t->update_attempts;
        }
        (void)pwr_engine_frame_release(e);
    }

    /* ---- target gone: the scan-staleness rules must retire it --------- */
    (void)pwr_engine_target_remove(e, tgt_id);
    for (cpi = 0u; cpi < dead_scans * per_scan; ++cpi)
    {
        PWR_Frame f;
        uint32_t k;
        (void)pwr_engine_step(e, 1u);
        if (pwr_engine_frame_acquire(e, &f) != PWR_STATUS_OK) { continue; }
        for (k = 0u; k < f.track_count; ++k)
        {
            if (f.tracks[k].id == id &&
                f.tracks[k].state == PWR_TRACK_TERMINATED) { ++term_seen; }
        }
        (void)pwr_engine_frame_release(e);
    }

    /* The attempt counter is the sharp assertion: one per revolution, so it
     * must stay within a scan of the revolutions elapsed and nowhere near the
     * CPI count it would reach under the staring branch. */
    ok = (confirmed != 0 &&
          attempts >= alive_scans - 1u && attempts <= alive_scans + 1u &&
          term_seen == 1u) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail),
                   "confirmed %d, %u attempts over %u scans (%u CPI/scan), "
                   "%u termination report(s)",
                   confirmed, attempts, alive_scans, per_scan, term_seen);
    pwr_check(c, "T12 rotating track life cycle", ok, detail);
    pwr_engine_destroy(e);
}

/* ==========================================================================
 *  T13 - detection probability against theory
 * ==========================================================================
 *  Everything above checks that the chain is self-consistent.  This checks it
 *  against radar theory from outside: for a non-fluctuating target in complex
 *  Gaussian noise, the cell power after coherent integration is non-central
 *  chi-square with two degrees of freedom, so
 *
 *      Pd = Q1( sqrt(2S), sqrt(2T) )
 *
 *  with S the post-integration signal-to-noise ratio and T the threshold in
 *  units of mean noise power.  Both are pinned down without fitting anything:
 *
 *    S  comes from the radar range equation plus the exact coherent gain.  The
 *       geometry removes every loss that would otherwise have to be guessed -
 *       the target sits on boresight at zero elevation with the antenna at
 *       ground level (unity two-way pattern), on an exact range bin (no
 *       straddle), and at exactly zero range rate under a rectangular Doppler
 *       taper (unity coherent gain, and a DFT that is exactly zero in every
 *       other bin, so the target cannot leak into its own CFAR reference
 *       window).
 *
 *    T  is measured, not assumed: the same configuration is run once with no
 *       target and the achieved false-alarm rate gives T = -ln(Pfa).  Reading
 *       the threshold back this way absorbs the CFAR loss that a fluctuating
 *       threshold estimate costs, which is why the reference window is also
 *       made large - the residual is then well inside the tolerance.
 * ------------------------------------------------------------------------ */

/* Poisson mixture of central chi-squares, which is Marcum Q1 in the form that
 * needs no Bessel function.  Every term is advanced by recurrence so no
 * factorial is ever formed.  S = 0 must return exp(-T), i.e. the false-alarm
 * rate - the check that this is the right expression. */
static double pwr_pd_nonfluctuating(double s, double t)
{
    double outer = exp(-s);     /* e^-S S^k / k!             */
    double term  = exp(-t);     /* e^-T T^j / j!             */
    double inner = term;        /* e^-T sum_{j<=k} T^j / j!  */
    double pd    = 0.0;
    uint32_t k;

    for (k = 0u; k < 8192u; ++k)
    {
        pd += outer * inner;
        if ((double)k > s && outer < 1.0e-15) { break; }
        term  *= t / (double)(k + 1u);
        inner += term;
        outer *= s / (double)(k + 1u);
    }
    return pwr_clampd(pd, 0.0, 1.0);
}

/* Builds the controlled geometry described above.  @p rcs of zero leaves the
 * scenario empty, which is the pass that measures the threshold. */
static PWR_Engine* pwr_test_pd_engine(double pfa, double range_m, double rcs)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.scan_rate_rpm     = 0.0;
    cfg.range_start_m     = 0.0;
    cfg.range_span_m      = 12000.0;
    cfg.range_bins        = 500u;     /* decimation 1: bin == delay sample    */
    cfg.ppi_azimuth_cells = 64u;
    cfg.rti_rows          = 16u;
    cfg.doppler_bins      = cfg.pulses_per_cpi;   /* no Doppler oversampling  */
    cfg.doppler_window    = PWR_WIN_RECTANGULAR;  /* coherent gain exactly N  */
    cfg.mti_mode          = PWR_MTI_OFF;
    cfg.enable_stc        = 0;
    /* A large reference window keeps the CFAR loss small enough that reading
     * the threshold back from the measured Pfa stays accurate. */
    cfg.cfar.type                = PWR_CFAR_CA;
    cfg.cfar.pfa                 = pfa;
    cfg.cfar.train_range         = 32;
    cfg.cfar.guard_range         = 3;
    cfg.cfar.train_doppler       = 8;
    cfg.cfar.guard_doppler       = 2;
    cfg.cfar.censor_zero_doppler = 0;   /* the target sits on the DC column   */
    cfg.cfar.peak_selection      = 0;   /* count raw threshold crossings      */
    cfg.cluster.enable     = 0;
    cfg.cluster.min_cells  = 1;
    cfg.cluster.min_snr_db = -100.0;
    cfg.tracker.enable     = 0;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { return NULL; }

    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 0;
    env.enable_rain          = 0;
    env.enable_jammer        = 0;
    env.enable_multipath     = 0;
    env.enable_eclipsing     = 0;
    (void)pwr_engine_set_environment(e, &env);

    if (rcs > 0.0)
    {
        PWR_SimTarget t;
        memset(&t, 0, sizeof(t));
        t.x_m = 0.0;              /* bearing 0, which is where the beam stares */
        t.y_m = range_m;
        /* Level with the antenna, so the only elevation offset is the earth
         * bulge - 3.05 m at this range, i.e. 0.024 degrees against a 20 degree
         * beam, which costs four millionths of a decibel. */
        t.z_m = 25.0;
        t.rcs_m2 = rcs;           /* stationary: exact bin, no range walk      */
        t.swerling = PWR_SWERLING_0;
        t.target_class = PWR_CLASS_AIR;
        t.enabled = 1;
        (void)snprintf(t.label, sizeof(t.label), "PDTEST");
        (void)pwr_engine_target_add(e, &t);
    }
    return e;
}

static void pwr_test_pd(PWR_TestCtx* c)
{
    /* Chosen to sit across the knee of the curve, where Pd moves about 0.18
     * per dB.  A saturated point proves nothing; here a half-decibel error
     * anywhere in the link budget, the compression calibration or the
     * coherent gain moves Pd by more than the tolerance below. */
    static const double s_post_db[4] = { 6.0, 8.0, 10.0, 12.0 };
    const double   design_pfa = 1.0e-4;
    const uint32_t n_cpi      = 700u;
    /* Exactly 300 delay samples at the default sample rate, so the compressed
     * peak lands on a bin centre. */
    const double   range_m    = 300.0 * PWR_C_LIGHT / (2.0 * 6.25e6);
    char detail[224];
    size_t len = 0u;
    double t_thresh = 0.0;
    int ok = 1;
    int i;

    /* ---- pass 1: no target, read the effective threshold back ---------- */
    {
        PWR_Engine* e = pwr_test_pd_engine(design_pfa, range_m, 0.0);
        PWR_Stats st;
        double pfa = 0.0;
        if (e == NULL) { pwr_check(c, "T13 Pd versus theory", 0, "engine creation failed"); return; }
        (void)pwr_engine_step(e, n_cpi);
        (void)pwr_engine_get_stats(e, &st);
        if (st.cells_tested_total > 0u)
        {
            pfa = (double)st.detection_total / (double)st.cells_tested_total;
        }
        pwr_engine_destroy(e);
        if (!(pfa > 0.0)) { pwr_check(c, "T13 Pd versus theory", 0, "no false alarms measured"); return; }
        t_thresh = -log(pfa);
    }

    /* ---- pass 2..: one run per signal-to-noise ratio ------------------- */
    for (i = 0; i < 4; ++i)
    {
        PWR_RadarConfig cfg;
        PWR_Engine* e = NULL;
        double rcs, s_lin, pd_meas = 0.0, pd_theory;
        uint32_t hits = 0u, cpi, zero_row, tgt_bin;
        int n;

        /* Solve for the cross-section that lands on this post-integration
         * SNR, so the link budget is part of what is under test. */
        (void)pwr_config_default(&cfg);
        {
            const double gain_db = 10.0 * log10((double)cfg.pulses_per_cpi);
            const double one_db  = pwr_snr_single_pulse_db(&cfg, 1.0, range_m);
            rcs = pow(10.0, (s_post_db[i] - gain_db - one_db) / 10.0);
        }
        s_lin = pow(10.0, s_post_db[i] / 10.0);

        e = pwr_test_pd_engine(design_pfa, range_m, rcs);
        if (e == NULL) { ok = 0; break; }
        zero_row = cfg.pulses_per_cpi / 2u - 1u;
        tgt_bin  = 300u;

        for (cpi = 0u; cpi < n_cpi; ++cpi)
        {
            PWR_Frame f;
            uint32_t k;
            int seen = 0;
            (void)pwr_engine_step(e, 1u);
            if (pwr_engine_frame_acquire(e, &f) != PWR_STATUS_OK) { continue; }
            for (k = 0u; k < f.detection_count && seen == 0; ++k)
            {
                const uint32_t rb = f.detections[k].range_bin;
                const uint32_t db = f.detections[k].doppler_bin;
                if (rb + 1u >= tgt_bin && rb <= tgt_bin + 1u &&
                    db + 1u >= zero_row && db <= zero_row + 1u) { seen = 1; }
            }
            hits += (uint32_t)seen;
            (void)pwr_engine_frame_release(e);
        }
        pwr_engine_destroy(e);

        pd_meas   = (double)hits / (double)n_cpi;
        pd_theory = pwr_pd_nonfluctuating(s_lin, t_thresh);
        if (fabs(pd_meas - pd_theory) > 0.09) { ok = 0; }
        if (len < sizeof(detail))
        {
            n = snprintf(detail + len, sizeof(detail) - len, "%s%.0fdB %.2f/%.2f",
                         (i > 0) ? ", " : "", s_post_db[i], pd_meas, pd_theory);
            if (n > 0) { len += (size_t)n; }
            if (len > sizeof(detail)) { len = sizeof(detail); }
        }
    }
    pwr_check(c, "T13 Pd versus theory (meas/theory)", ok, detail);
}

/* ==========================================================================
 *  T14 - scenario serialisation round trip
 * ==========================================================================
 *  Saving is only worth anything if it is lossless, and the sharpest way to
 *  say that is: write a scenario, load it into a second engine, write that one
 *  too, and require the two texts to be byte-identical.  Any field the writer
 *  drops, any double that loses a bit, any target that goes missing shows up
 *  as a difference - and unlike a field-by-field comparison this cannot go
 *  stale when a member is added.
 *
 *  The second half checks that a bad file is refused rather than half-applied,
 *  because a hand-edited scenario is untrusted input.
 * ------------------------------------------------------------------------ */
static void pwr_test_serialise(PWR_TestCtx* c)
{
    PWR_RadarConfig cfg;
    PWR_Engine* a = NULL;
    PWR_Engine* b = NULL;
    char* t1 = NULL;
    char* t2 = NULL;
    char  err[PWR_ERRMSG_LEN];
    char  detail[160];
    const size_t cap = 65536u;
    size_t n1 = 0u, n2 = 0u;
    uint32_t na = 0u, nb = 0u;
    int ok = 0, rejected = 0;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread = 0;
    /* Move a few values off the defaults so the test cannot pass by writing
     * nothing and reading nothing. */
    cfg.carrier_hz            = 9.41e9;
    cfg.azimuth_beamwidth_deg = 0.9;
    cfg.cfar.type             = PWR_CFAR_OS;
    cfg.cfar.pfa              = 3.7e-8;
    cfg.tracker.gate_sigma    = 3.25;

    t1 = (char*)PWR_ALLOC_ARRAY(char, cap);
    t2 = (char*)PWR_ALLOC_ARRAY(char, cap);
    if (t1 == NULL || t2 == NULL) { goto done; }

    if (pwr_engine_create(&cfg, &a) != PWR_STATUS_OK) { goto done; }
    if (pwr_engine_create(NULL, &b) != PWR_STATUS_OK) { goto done; }
    (void)pwr_engine_load_scenario(a, 3u);           /* six targets */
    /* The scenario builder resets the environment, so re-apply the config it
     * does not touch and take what the engine actually holds. */
    if (pwr_engine_scenario_save(a, t1, cap, &n1) != PWR_STATUS_OK) { goto done; }
    if (pwr_engine_scenario_load(b, t1, err, sizeof(err)) != PWR_STATUS_OK) { goto done; }
    if (pwr_engine_scenario_save(b, t2, cap, &n2) != PWR_STATUS_OK) { goto done; }

    na = pwr_engine_target_count(a);
    nb = pwr_engine_target_count(b);
    ok = (n1 == n2 && strcmp(t1, t2) == 0 && na == nb && na > 0u) ? 1 : 0;

    /* A file that does not validate must be refused outright. */
    {
        static const char* const bad =
            "[radar]\nprf_hz = 0\npulse_width_s = 0\n";
        PWR_RadarConfig before, after;
        (void)pwr_engine_get_config(b, &before);
        if (pwr_engine_scenario_load(b, bad, err, sizeof(err)) != PWR_STATUS_OK)
        {
            (void)pwr_engine_get_config(b, &after);
            rejected = (before.prf_hz == after.prf_hz) ? 1 : 0;
        }
        if (rejected == 0) { ok = 0; }
    }

done:
    (void)snprintf(detail, sizeof(detail),
                   "%u bytes, %u targets both sides, round trip %s, "
                   "invalid file %s",
                   (unsigned)n1, na, (n1 == n2 && n1 > 0u &&
                   t1 != NULL && t2 != NULL && strcmp(t1, t2) == 0)
                       ? "exact" : "DIFFERS",
                   (rejected != 0) ? "refused" : "ACCEPTED");
    pwr_check(c, "T14 scenario serialisation", ok, detail);
    pwr_engine_destroy(a);
    pwr_engine_destroy(b);
    PWR_FREE(t1);
    PWR_FREE(t2);
}

/* --------------------------------------------------------------------------
 *  Peak of the published range profile inside a window around a slant range,
 *  which is how the propagation cases read a level without going through the
 *  detector - a target 25 dB into the shadow has no detection to read.
 * ------------------------------------------------------------------------ */
static double pwr_test_profile_peak(const PWR_Frame* f, double slant_m,
                                    int32_t half_bins)
{
    const int32_t c = (int32_t)((slant_m - f->range_first_m) /
                                f->range_step_m + 0.5);
    double best = -300.0;
    int32_t i;
    for (i = c - half_bins; i <= c + half_bins; ++i)
    {
        if (i < 0 || i >= (int32_t)f->range_bins) { continue; }
        if ((double)f->range_profile_db[i] > best)
        {
            best = (double)f->range_profile_db[i];
        }
    }
    return best;
}

/* ==========================================================================
 *  T15 - radar horizon on an effective earth
 * ==========================================================================
 *  Flat-earth geometry lets a surface radar see a sea-skimmer at any range the
 *  receive window covers, which is the single most optimistic thing a surface
 *  surveillance model can do.  With a 25 m antenna and a 5 m target the
 *  smooth-sphere horizon is 4.12*(sqrt(25)+sqrt(5)) = 29.8 km, so the check is
 *  that the level falls away far faster than R^-4 past that range and no
 *  faster than R^-4 before it.
 * ------------------------------------------------------------------------ */
static void pwr_test_horizon(PWR_TestCtx* c)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;
    char detail[192];
    double lvl[3] = { 0.0, 0.0, 0.0 };
    static const double gr[3] = { 20000.0, 25000.0, 33000.0 };
    double inside_drop = 0.0, shadow_drop = 0.0;
    int ok = 0, i;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.scan_rate_rpm     = 0.0;
    cfg.range_span_m      = 45000.0;
    cfg.range_bins        = 900u;
    cfg.ppi_azimuth_cells = 64u;
    cfg.rti_rows          = 16u;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK)
    {
        pwr_check(c, "T15 radar horizon", 0, "engine creation failed");
        return;
    }
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 0;
    env.enable_eclipsing     = 0;
    (void)pwr_engine_set_environment(e, &env);

    /* Three identical surface targets on the stared bearing, so one profile
     * carries all three levels and the comparison cannot drift. */
    for (i = 0; i < 3; ++i)
    {
        pwr_test_add_target(e, gr[i], 5.0, 0.0, 5000.0);
    }
    (void)pwr_engine_step(e, 4u);
    {
        PWR_Frame f;
        if (pwr_engine_frame_acquire(e, &f) == PWR_STATUS_OK)
        {
            for (i = 0; i < 3; ++i)
            {
                lvl[i] = pwr_test_profile_peak(&f, gr[i], 4);
            }
            (void)pwr_engine_frame_release(e);
        }
    }
    pwr_engine_destroy(e);

    /* 20 -> 25 km is inside the horizon: R^-4 alone, 3.88 dB.
     * 25 -> 33 km crosses it: R^-4 gives 4.82 dB, the shadow adds about 21. */
    inside_drop = lvl[0] - lvl[1];
    shadow_drop = lvl[1] - lvl[2];
    ok = (fabs(inside_drop - 3.88) < 2.0 && shadow_drop > 15.0 &&
          shadow_drop < 45.0) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail),
                   "horizon 29.8 km; 20->25 km %.1f dB (R^-4 = 3.9), "
                   "25->33 km %.1f dB", inside_drop, shadow_drop);
    pwr_check(c, "T15 radar horizon", ok, detail);
}

/* ==========================================================================
 *  T16 - cosecant-squared elevation coverage
 * ==========================================================================
 *  The defining property of a csc^2 antenna is that received power is constant
 *  for a target at constant altitude: the gain grows as R^2 across the fill,
 *  the two-way gain as R^4, and the R^-4 of the radar equation cancels exactly.
 *  Four targets at one altitude and four ranges therefore have to read the same
 *  level with the fill on - and wildly different levels with it off, which is
 *  the second half of the check and the reason the fill exists.
 * ------------------------------------------------------------------------ */
static void pwr_test_csc2(PWR_TestCtx* c)
{
    static const double gr[4] = { 4000.0, 8000.0, 12000.0, 16000.0 };
    const double alt = 3000.0;
    char detail[192];
    double spread[2] = { 0.0, 0.0 };
    int ok = 1, mode;

    for (mode = 0; mode < 2; ++mode)      /* 0 = plain Gaussian, 1 = csc^2 */
    {
        PWR_RadarConfig cfg;
        PWR_SimEnvironment env;
        PWR_Engine* e = NULL;
        double lo = 1.0e9, hi = -1.0e9;
        int i;

        (void)pwr_config_default(&cfg);
        cfg.worker_thread     = 0;
        cfg.deterministic     = 1;
        cfg.scan_rate_rpm     = 0.0;
        cfg.range_span_m      = 20000.0;
        /* Oversampled 2.5x in range with no decimation, so the compressed peak
         * is sampled finely enough that range straddle - which is otherwise
         * the largest term here, and has nothing to do with the elevation cut
         * under test - stays a few tenths of a decibel. */
        cfg.sample_rate_hz    = 12.5e6;
        cfg.range_bins        = 0u;
        cfg.ppi_azimuth_cells = 64u;
        cfg.rti_rows          = 16u;
        cfg.elevation_csc2_deg = (mode != 0) ? 40.0 : 0.0;
        if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { ok = 0; break; }
        (void)pwr_engine_get_environment(e, &env);
        env.enable_thermal_noise = 1;
        env.enable_sea_clutter   = 0;
        env.enable_eclipsing     = 0;
        (void)pwr_engine_set_environment(e, &env);
        for (i = 0; i < 4; ++i) { pwr_test_add_target(e, gr[i], alt, 0.0, 50.0); }

        (void)pwr_engine_step(e, 4u);
        {
            PWR_Frame f;
            if (pwr_engine_frame_acquire(e, &f) == PWR_STATUS_OK)
            {
                for (i = 0; i < 4; ++i)
                {
                    const double dz = alt - 25.0;
                    const double sl = sqrt(gr[i] * gr[i] + dz * dz);
                    const double v  = pwr_test_profile_peak(&f, sl, 4);
                    if (v < lo) { lo = v; }
                    if (v > hi) { hi = v; }
                }
                (void)pwr_engine_frame_release(e);
            }
        }
        pwr_engine_destroy(e);
        spread[mode] = hi - lo;
    }

    /* With the fill the four have to agree; without it the 36 degree target is
     * buried by the Gaussian while the 10 degree one is barely touched. */
    if (!(spread[1] < 1.0) || !(spread[0] > 20.0)) { ok = 0; }
    (void)snprintf(detail, sizeof(detail),
                   "constant-altitude spread: Gaussian %.1f dB, csc2 %.2f dB",
                   spread[0], spread[1]);
    pwr_check(c, "T16 cosecant-squared coverage", ok, detail);
}

/* ==========================================================================
 *  T17 - MTI clutter attenuation depends on the scan rate
 * ==========================================================================
 *  A delay-line canceller can only cancel what stays correlated pulse to
 *  pulse, so its improvement factor is set by the width of the clutter
 *  spectrum.  Two things widen that spectrum: the sea moving, and the beam
 *  sweeping across a patch - the classic 0.265 / t_dwell scan-modulation term.
 *  With only the first modelled, the achievable improvement factor is a
 *  property of the sea alone and turning the antenna faster costs nothing,
 *  which is wrong in kind rather than merely optimistic.
 *
 *  The check is therefore a comparison, not an absolute: the same sea at a
 *  quarter of the rotation rate must cancel measurably better.
 * ------------------------------------------------------------------------ */
static double pwr_test_clutter_power(double rpm, int32_t mti)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;
    double acc = 0.0;
    uint32_t frames = 0u, cpi;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.scan_rate_rpm     = rpm;
    cfg.mti_mode          = mti;
    cfg.range_span_m      = 12000.0;
    cfg.range_bins        = 400u;
    cfg.ppi_azimuth_cells = 64u;
    cfg.rti_rows          = 16u;
    cfg.tracker.enable    = 0;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { return -1.0; }
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 1;
    /* High enough that the residual after cancellation is still well clear of
     * the noise floor, which is what keeps the ratio below a measurement of
     * clutter attenuation rather than of the noise. */
    env.clutter_to_noise_db  = 60.0;
    env.clutter_spread_hz    = 12.0;
    env.enable_eclipsing     = 0;
    (void)pwr_engine_set_environment(e, &env);

    for (cpi = 0u; cpi < 40u; ++cpi)
    {
        PWR_Frame f;
        (void)pwr_engine_step(e, 1u);
        if (cpi < 20u) { continue; }        /* let the AR(1) field settle */
        if (pwr_engine_frame_acquire(e, &f) != PWR_STATUS_OK) { continue; }
        {
            /* Total power across the Doppler column of a mid-clutter gate:
             * the whole spectrum, not the notch, is what attenuation means. */
            const uint32_t rb = 160u;
            double sum = 0.0;
            uint32_t j;
            for (j = 0u; j < f.doppler_bins; ++j)
            {
                sum += pow(10.0, (double)f.rd_map_db[(size_t)j * f.range_bins + rb]
                                 / 10.0);
            }
            /* The map is calibrated so a noise-only cell reads 1.0, and the
             * canceller's coefficient normalisation leaves the total noise
             * power across the Doppler column unchanged - so exactly one unit
             * per bin is thermal, and subtracting it leaves clutter alone.
             * Without this the ratio saturates once the residual clutter falls
             * under the floor, which is well before the interesting cases. */
            acc += pwr_maxd(sum - (double)f.doppler_bins, 1.0e-9);
            ++frames;
        }
        (void)pwr_engine_frame_release(e);
    }
    pwr_engine_destroy(e);
    return (frames > 0u) ? (acc / (double)frames) : -1.0;
}

static void pwr_test_scan_modulation(PWR_TestCtx* c)
{
    char detail[192];
    double ca_fast = 0.0, ca_slow = 0.0;
    int ok = 0;
    double p_off, p_on;

    p_off = pwr_test_clutter_power(24.0, PWR_MTI_OFF);
    p_on  = pwr_test_clutter_power(24.0, PWR_MTI_TWO_PULSE);
    if (p_off > 0.0 && p_on > 0.0) { ca_fast = 10.0 * log10(p_off / p_on); }

    p_off = pwr_test_clutter_power(6.0, PWR_MTI_OFF);
    p_on  = pwr_test_clutter_power(6.0, PWR_MTI_TWO_PULSE);
    if (p_off > 0.0 && p_on > 0.0) { ca_slow = 10.0 * log10(p_off / p_on); }

    ok = (ca_fast > 12.0 && ca_fast < 40.0 &&
          ca_slow - ca_fast > 3.0) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail),
                   "2-pulse clutter attenuation %.1f dB at 24 rpm, %.1f dB at "
                   "6 rpm (+%.1f dB for the slower sweep)",
                   ca_fast, ca_slow, ca_slow - ca_fast);
    pwr_check(c, "T17 scan modulation limits MTI", ok, detail);
}

/* ==========================================================================
 *  T18 - a spiky sea breaks the Rayleigh false-alarm design
 * ==========================================================================
 *  Every CFAR multiplier in this library is derived for exponentially
 *  distributed cell power.  While the simulated sea was complex Gaussian too,
 *  the detector and the simulator shared one assumption and any false-alarm
 *  rate measured against it was self-fulfilling - the single most important
 *  thing a maritime radar model can get wrong, because spiky sea clutter is
 *  precisely what CFAR design exists to survive.
 *
 *  With the compound-K texture switched on, the same design Pfa has to produce
 *  visibly more false plots.  That difference is the whole point: it is the
 *  margin a real detector has to buy back with a different estimator, a
 *  different threshold, or scan-to-scan integration.
 * ------------------------------------------------------------------------ */
static uint64_t pwr_test_clutter_alarms(double nu)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;
    PWR_Stats st;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.scan_rate_rpm     = 24.0;      /* texture refreshes once a scan */
    cfg.range_span_m      = 24000.0;
    cfg.range_bins        = 300u;
    cfg.ppi_azimuth_cells = 128u;
    cfg.rti_rows          = 16u;
    cfg.cfar.pfa          = 1.0e-6;
    cfg.tracker.enable    = 0;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { return 0u; }
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 1;
    /* High enough that the sea still dominates the thermal floor at the far
     * edge of the display: the R^-3 law costs 41 dB across 24 km, and if the
     * outer half of the map is noise-limited then most false alarms are
     * thermal and say nothing about the clutter distribution.  Eclipsing is
     * off for the same reason - it blanks the first 3 km, which is exactly
     * where the sea is strongest. */
    env.clutter_to_noise_db  = 65.0;
    env.enable_eclipsing     = 0;
    env.sea_shape_nu         = nu;
    (void)pwr_engine_set_environment(e, &env);

    /* Three revolutions, so several independent texture realisations. */
    (void)pwr_engine_step(e, 705u);
    (void)pwr_engine_get_stats(e, &st);
    pwr_engine_destroy(e);
    return st.detection_total;
}

static void pwr_test_k_clutter(PWR_TestCtx* c)
{
    char detail[176];
    const uint64_t rayleigh = pwr_test_clutter_alarms(0.0);
    const uint64_t k2 = pwr_test_clutter_alarms(2.0);
    const uint64_t k1 = pwr_test_clutter_alarms(0.6);
    const uint64_t k0 = pwr_test_clutter_alarms(0.2);
    /* The claim is the ordering, not a number: a spikier sea must cost more
     * false alarms at every step, and the spikiest case must cost several
     * times the Rayleigh design.  A model that produced the same rate for any
     * shape would not have a texture at all. */
    const int ok = (rayleigh > 0u && k2 > rayleigh && k1 > k2 && k0 > k1 &&
                    k0 > rayleigh * 3u) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail),
                   "false plots/3 scans: Rayleigh %llu, nu=2 %llu, 0.6 %llu, 0.2 %llu",
                   (unsigned long long)rayleigh, (unsigned long long)k2,
                   (unsigned long long)k1, (unsigned long long)k0);
    pwr_check(c, "T18 spiky sea defeats a Rayleigh CFAR", ok, detail);
}

/* ==========================================================================
 *  Entry point
 * ========================================================================== */
/* ==========================================================================
 *  T19 - an extended target occupies more than one range cell
 * ==========================================================================
 *  Until a target could be longer than a resolution cell, every plot in this
 *  model was one cell wide - which meant the clustering tolerances, the
 *  maximum-cluster reject and the dwell-merge gates had all been tuned against
 *  a stimulus that could never exercise them.  A 150 m ship at 30 m resolution
 *  has to span several cells, and the check is that it does: the -10 dB extent
 *  of the compressed response grows with the body length rather than staying
 *  at the pulse width.
 * ------------------------------------------------------------------------ */
static double pwr_test_echo_extent(double length_m, int32_t scatterers)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;
    PWR_SimTarget t;
    const double range_m = 12000.0;
    double extent = -1.0;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.scan_rate_rpm     = 0.0;
    cfg.range_span_m      = 20000.0;
    cfg.range_bins        = 0u;          /* no decimation: 24 m bins */
    cfg.ppi_azimuth_cells = 64u;
    cfg.rti_rows          = 16u;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { return -1.0; }
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 0;
    env.enable_eclipsing     = 0;
    (void)pwr_engine_set_environment(e, &env);

    memset(&t, 0, sizeof(t));
    t.x_m = 0.0;
    t.y_m = range_m;
    t.z_m = 25.0;
    t.vy_mps = 40.0;          /* radial, so the body axis projects into range */
    t.rcs_m2 = 4000.0;        /* a ship-sized cross-section */
    t.length_m    = length_m;
    t.scatterers  = scatterers;
    t.swerling    = PWR_SWERLING_0;
    t.enabled     = 1;
    (void)snprintf(t.label, sizeof(t.label), "EXTENT");
    (void)pwr_engine_target_add(e, &t);

    (void)pwr_engine_step(e, 2u);
    {
        PWR_Frame f;
        if (pwr_engine_frame_acquire(e, &f) == PWR_STATUS_OK)
        {
            const double peak = pwr_test_profile_peak(&f, range_m, 24);
            const int32_t c = (int32_t)((range_m - f.range_first_m) /
                                        f.range_step_m + 0.5);
            int32_t lo = -1, hi = -1, i;
            for (i = c - 24; i <= c + 24; ++i)
            {
                if (i < 0 || i >= (int32_t)f.range_bins) { continue; }
                if ((double)f.range_profile_db[i] < peak - 10.0) { continue; }
                if (lo < 0) { lo = i; }
                hi = i;
            }
            /* First to last bin above -10 dB, so interference nulls between
             * scattering centres do not truncate the measurement. */
            extent = (lo >= 0) ? ((double)(hi - lo + 1) * f.range_step_m) : 0.0;
            (void)pwr_engine_frame_release(e);
        }
    }
    pwr_engine_destroy(e);
    return extent;
}

static void pwr_test_extent(PWR_TestCtx* c)
{
    char detail[176];
    const double pt  = pwr_test_echo_extent(0.0, 0);
    const double ship = pwr_test_echo_extent(150.0, 12);
    const int ok = (pt > 0.0 && ship > pt * 2.5 && ship < 400.0) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail),
                   "-10 dB echo extent: point %.0f m, 150 m hull %.0f m "
                   "(range resolution 30 m)", pt, ship);
    pwr_check(c, "T19 extended target range extent", ok, detail);
}

/* ==========================================================================
 *  T20 - a turn must not cost the tracker its lock
 * ==========================================================================
 *  This case was written to measure how badly a constant-velocity filter loses
 *  a turning target, on the way to justifying an interacting-multiple-model
 *  bank.  It measured something else.  The 2.8x penalty it originally recorded
 *  came from the process-noise model, not from the motion model: the discrete
 *  Q was applied once per CPI while measurements arrived once per revolution,
 *  so the filter accumulated a two-hundred-and-thirty-fourth of the manoeuvre
 *  allowance the operator had asked for, its gate stayed shut on a target that
 *  had turned, and the track was lost and re-acquired.  With the continuous-
 *  time Q the same turn costs nothing measurable.
 *
 *  So the assertion is now the property that fix bought, and it is stated in
 *  absolute metres rather than as a ratio - a ratio moves whenever either leg
 *  moves and hides which change did what.  Both legs sit near the ground-plane
 *  projection bias of a two-dimensional tracker fed slant range (about 150 m
 *  at this geometry), which is the floor of what this measurement can resolve.
 * ------------------------------------------------------------------------ */
static double pwr_test_turn_rmse(double turn_rate_dps, double* out_complete)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;
    PWR_Scorer sc;
    PWR_SimTarget t;
    uint32_t cpi, n_cpi;
    double worst = 0.0;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.range_bins        = 512u;
    cfg.ppi_azimuth_cells = 128u;
    cfg.rti_rows          = 16u;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { return -1.0; }
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 0;
    (void)pwr_engine_set_environment(e, &env);
    (void)pwr_scorer_init(&sc, 0.0);

    memset(&t, 0, sizeof(t));
    t.x_m = 9000.0;
    t.y_m = 9000.0;
    t.z_m = 2000.0;
    t.vx_mps = -110.0;
    t.vy_mps = 0.0;
    t.rcs_m2 = 12.0;
    t.turn_rate_dps = turn_rate_dps;
    t.swerling = PWR_SWERLING_0;
    t.enabled  = 1;
    (void)snprintf(t.label, sizeof(t.label), "TURN");
    (void)pwr_engine_target_add(e, &t);

    n_cpi = (uint32_t)(24.0 / (32.0 / 3000.0));
    for (cpi = 0u; cpi < n_cpi; ++cpi)
    {
        PWR_Frame f;
        (void)pwr_engine_step(e, 1u);
        if (pwr_engine_frame_acquire(e, &f) != PWR_STATUS_OK) { continue; }
        (void)pwr_scorer_update(&sc, &f);
        (void)pwr_engine_frame_release(e);
    }
    {
        uint32_t i;
        for (i = 0u; i < PWR_MAX_SIM_TARGETS; ++i)
        {
            if (sc.targets[i].err_n == 0u) { continue; }
            if (sc.targets[i].err_rms_m > worst) { worst = sc.targets[i].err_rms_m; }
            if (out_complete != NULL) { *out_complete = sc.targets[i].completeness; }
        }
    }
    pwr_engine_destroy(e);
    return worst;
}

static void pwr_test_manoeuvre(PWR_TestCtx* c)
{
    char detail[208];
    double cs = 0.0, ct = 0.0;
    const double straight = pwr_test_turn_rmse(0.0, &cs);
    const double turning  = pwr_test_turn_rmse(6.0, &ct);
    /* The turn must not blow the error up and must not cost the lock.  Both
     * bounds are absolute: a ratio would have passed the broken build too, and
     * a completeness floor is what catches the lose-and-re-acquire cycle that
     * a shut gate produces - which an RMSE alone hides, because the segments
     * it does hold are accurate. */
    const int ok = (straight > 0.0 && turning > 0.0 &&
                    straight < 260.0 && turning < 300.0 &&
                    cs > 0.6 && ct > 0.6) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail),
                   "straight %.0f m / %.0f%% held, 6 deg/s turn %.0f m / %.0f%% "
                   "held (floor is the ~150 m slant-to-ground bias)",
                   straight, 100.0 * cs, turning, 100.0 * ct);
    pwr_check(c, "T20 a turn does not cost the tracker its lock", ok, detail);
}

/* ==========================================================================
 *  T21 - a track is only seeded with a range rate that can be believed
 * ==========================================================================
 *  The Doppler axis spans exactly +/- Vua, so every measured range rate
 *  arrives already folded and a single plot cannot say which fold it came
 *  from.  Seeding a new track's velocity from a folded reading does not just
 *  lose accuracy, it can point the track the wrong way: a target closing at
 *  120 m/s against a 73.7 m/s interval reads as +27 m/s, and the filter then
 *  starts by believing the target is opening.
 *
 *  What decides whether the reading can be believed is not its value - a fold
 *  lands anywhere in the interval with equal ease - but whether this radar can
 *  fold an admissible target at all.  So the rule is Vua >= max_speed_mps, and
 *  this case checks both sides of it on the same geometry: with the default
 *  interval the new track must start stationary, and with a speed limit inside
 *  the interval it must start at the measured range rate.
 *
 *  Neither leg can pass by accident: the first fails if the folded value is
 *  used, the second fails if the seed is dropped unconditionally.
 * ------------------------------------------------------------------------ */
static double pwr_test_birth_speed(double max_speed_mps, double range_rate_mps)
{
    PWR_RadarConfig cfg;
    PWR_SimEnvironment env;
    PWR_Engine* e = NULL;
    double speed = -1.0;
    uint32_t cpi;

    (void)pwr_config_default(&cfg);
    cfg.worker_thread     = 0;
    cfg.deterministic     = 1;
    cfg.scan_rate_rpm     = 0.0;          /* stare, so the target is lit at once */
    cfg.range_span_m      = 24000.0;
    cfg.range_bins        = 512u;
    cfg.ppi_azimuth_cells = 64u;
    cfg.rti_rows          = 16u;
    cfg.tracker.max_speed_mps = max_speed_mps;
    if (pwr_engine_create(&cfg, &e) != PWR_STATUS_OK) { return -1.0; }
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = 0;
    (void)pwr_engine_set_environment(e, &env);
    pwr_test_add_target(e, 15000.0, 3000.0, range_rate_mps, 40.0);

    /* The first frame that carries a track carries it at birth. */
    for (cpi = 0u; cpi < 12u && speed < 0.0; ++cpi)
    {
        PWR_Frame f;
        (void)pwr_engine_step(e, 1u);
        if (pwr_engine_frame_acquire(e, &f) != PWR_STATUS_OK) { continue; }
        if (f.track_count > 0u) { speed = f.tracks[0].speed_mps; }
        (void)pwr_engine_frame_release(e);
    }
    pwr_engine_destroy(e);
    return speed;
}

static void pwr_test_birth_seed(PWR_TestCtx* c)
{
    char detail[208];
    /* Vua = lambda*PRF/4 = 73.72 m/s at the default geometry. */
    const double folded   = pwr_test_birth_speed(600.0, -120.0);
    const double believed = pwr_test_birth_speed(60.0,  -40.0);
    const int ok = (folded >= 0.0 && believed >= 0.0 &&
                    folded < 5.0 && fabs(believed - 40.0) < 8.0) ? 1 : 0;
    (void)snprintf(detail, sizeof(detail),
                   "birth speed: ambiguous radar %.1f m/s (must be ~0, the "
                   "plot reads +27 for a -120 target), unambiguous %.1f m/s "
                   "(must be ~40)", folded, believed);
    pwr_check(c, "T21 velocity seed only when unambiguous", ok, detail);
}

/* --------------------------------------------------------------------------
 *  Two entry points, one suite
 *  ---------------------------
 *  The cases divide cleanly by cost.  Most are algebraic or single-CPI checks
 *  that run in milliseconds; a handful are statistical or scan-length cases
 *  that have to integrate over thousands of coherent intervals to say anything
 *  at all.  Running the second group is the right thing for a gate and the
 *  wrong thing for a build script, which needs to answer "did that compile
 *  into something sane" in a second or two - a script that sits silent for
 *  minutes is indistinguishable from one that has hung, and the operator stops
 *  reading its output.
 *
 *  So: the quick set is every case that does not need to integrate, and it is
 *  what a build runs.  The full set adds the four that do, and it is what
 *  CTest and CI run.  There is no third behaviour and no case in neither.
 * ------------------------------------------------------------------------ */
static PWR_Status pwr_self_test_run(char* report, size_t report_cap, int full)
{
    PWR_TestCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = report;
    ctx.cap = (report != NULL) ? report_cap : 0u;
    if (report != NULL && report_cap > 0u) { report[0] = '\0'; }

    pwr_tprintf(&ctx, "PWRadarCore %s self test%s\n", pwr_version_string(),
                (full != 0) ? "" : " (quick)");

    pwr_test_fft(&ctx);
    pwr_test_windows(&ctx);
    pwr_test_order_stats(&ctx);
    pwr_test_assignment(&ctx);
    pwr_test_linalg(&ctx);
    pwr_test_range_accuracy(&ctx);
    pwr_test_doppler_accuracy(&ctx);
    pwr_test_pfa(&ctx);
    pwr_test_tracking(&ctx);
    pwr_test_cfar_families(&ctx);
    pwr_test_serialise(&ctx);
    pwr_test_horizon(&ctx);
    pwr_test_csc2(&ctx);
    pwr_test_scan_modulation(&ctx);
    pwr_test_extent(&ctx);
    pwr_test_birth_seed(&ctx);

    if (full != 0)
    {
        /* Whole revolutions, and Monte-Carlo over detection and clutter
         * statistics: seconds each, and the reason the full suite is a gate
         * rather than a build step. */
        pwr_test_rotating_dwell(&ctx);
        pwr_test_rotating_track(&ctx);
        pwr_test_pd(&ctx);
        pwr_test_k_clutter(&ctx);
        pwr_test_manoeuvre(&ctx);
    }
    else
    {
        pwr_tprintf(&ctx, "  [SKIP] rotating dwell, rotating track life cycle, "
                          "Pd versus theory, K clutter\n"
                          "         (integrate over thousands of CPIs; run "
                          "--selftest for the full gate)\n");
    }

    pwr_tprintf(&ctx, "  %d/%d cases passed\n",
                ctx.cases - ctx.failures, ctx.cases);
    return (ctx.failures == 0) ? PWR_STATUS_OK : PWR_ERR_NUMERIC;
}

PWR_EXPORT(PWR_Status) pwr_self_test(char* report, size_t report_cap)
{
    return pwr_self_test_run(report, report_cap, 1);
}

PWR_EXPORT(PWR_Status) pwr_self_test_quick(char* report, size_t report_cap)
{
    return pwr_self_test_run(report, report_cap, 0);
}
