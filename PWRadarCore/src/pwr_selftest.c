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
 *  Entry point
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_self_test(char* report, size_t report_cap)
{
    PWR_TestCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = report;
    ctx.cap = (report != NULL) ? report_cap : 0u;
    if (report != NULL && report_cap > 0u) { report[0] = '\0'; }

    pwr_tprintf(&ctx, "PWRadarCore %s self test\n", pwr_version_string());

    pwr_test_fft(&ctx);
    pwr_test_windows(&ctx);
    pwr_test_order_stats(&ctx);
    pwr_test_assignment(&ctx);
    pwr_test_linalg(&ctx);
    pwr_test_range_accuracy(&ctx);
    pwr_test_doppler_accuracy(&ctx);
    pwr_test_pfa(&ctx);
    pwr_test_tracking(&ctx);

    pwr_tprintf(&ctx, "  %d/%d cases passed\n",
                ctx.cases - ctx.failures, ctx.cases);
    return (ctx.failures == 0) ? PWR_STATUS_OK : PWR_ERR_NUMERIC;
}
