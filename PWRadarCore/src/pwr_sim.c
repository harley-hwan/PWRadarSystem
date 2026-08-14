/* Synthetic receiver front end: one CPI of complex baseband I/Q for a
 * rotating-antenna surveillance radar. Covers thermal noise, point targets
 * with exact fractional-delay chirp insertion, intra-CPI range walk and
 * Doppler progression, Swerling 0-4 fluctuation, a two-way Gaussian antenna
 * pattern, correlated sea clutter (R^-3, 4/3-earth horizon cut-off, Gaussian
 * internal-motion spectrum), rain volume clutter (R^-2), a noise jammer seen
 * through the pattern, two-ray multipath lobing, transmit eclipsing and
 * optional range-ambiguity folding.
 *
 * Thermal noise variance is exactly 1.0 per complex sample, which is what makes
 * every other level in the model an SNR directly. A target whose
 * post-compression single-pulse SNR should be S is injected with per-sample
 * amplitude
 *
 *     A = sqrt(S) * sigma_pc
 *
 * because the compression filter is normalised for unit peak gain, so the peak
 * signal amplitude out is A and the noise amplitude out is sigma_pc.
 *
 * Clutter-to-noise ratio from the configuration is referenced to 1 km on
 * boresight.
 */
#include "pwr_core.h"

#include <string.h>

/* Reference range at which PWR_SimEnvironment::clutter_to_noise_db applies. */
#define PWR_CLUTTER_REF_RANGE_M   1000.0
/* 4/3-earth horizon constant: R_h[km] = 4.12 * sqrt(h[m]) */
#define PWR_HORIZON_K             4120.0
/* Sea-surface Fresnel reflection coefficient at low grazing angles. */
#define PWR_SEA_REFLECTION       (-0.9)

/* ==========================================================================
 *  Antenna pattern
 * ==========================================================================
 *  Gaussian approximation of the one-way power pattern inside the mainlobe:
 *      G(th)/G0 = exp( -2.7726 * (th / th_3dB)^2 )
 *  which is exactly -3 dB at th = th_3dB/2.  Two-way response is the square.
 *
 *  The Gaussian is only meaningful across the mainlobe; past the point where
 *  it has fallen to PWR_SIDELOBE_FLOOR the response crosses over to a 1/th^2
 *  envelope anchored at that level, which is the decay a real aperture's
 *  sidelobe peaks follow.  A *flat* floor instead - which is what this used to
 *  be - gives a jammer the same response at 5 degrees as at 180, and paints a
 *  uniform ring of sidelobe plots around any large close target.  The two
 *  constants below are a matched pair: the Gaussian passes through the floor
 *  at the knee, so the pattern is continuous to within a thousandth of a dB.
 *
 *  This is a smooth envelope, not a structured pattern: there are no discrete
 *  sidelobe peaks, no nulls and no backlobe, so it supports a sidelobe *level*
 *  argument but not sidelobe blanking or cancellation.
 * ------------------------------------------------------------------------ */
#define PWR_SIDELOBE_FLOOR        3.16228e-5   /* -45 dB, one way             */
#define PWR_SIDELOBE_KNEE_BW      1.933116     /* beamwidths to the floor     */

static double pwr_pattern_oneway(double offset_deg, double bw_deg)
{
    double u;
    if (!(bw_deg > 0.0)) { return 1.0; }
    u = fabs(offset_deg) / bw_deg;
    if (u <= PWR_SIDELOBE_KNEE_BW) { return exp(-2.7726 * u * u); }
    {
        const double k = PWR_SIDELOBE_KNEE_BW / u;
        return PWR_SIDELOBE_FLOOR * k * k;
    }
}

/* ==========================================================================
 *  Life cycle
 * ========================================================================== */
PWR_Status pwr_sim_init(PWR_Simulator* s, const PWR_RadarConfig* cfg,
                        uint32_t n_cells)
{
    if (s == NULL || cfg == NULL) { return PWR_ERR_NULL_POINTER; }
    if (n_cells == 0u)            { return PWR_ERR_INVALID_ARGUMENT; }

    PWR_FREE(s->clutter_state);
    PWR_FREE(s->clutter_power);

    s->clutter_state = PWR_ALLOC_ARRAY(PWR_Complex, n_cells);
    s->clutter_power = PWR_ALLOC_ARRAY(pwr_real,    n_cells);
    if (s->clutter_state == NULL || s->clutter_power == NULL)
    {
        PWR_FREE(s->clutter_state);
        PWR_FREE(s->clutter_power);
        return PWR_ERR_OUT_OF_MEMORY;
    }
    s->clutter_cells = n_cells;
    if (s->next_auto_id <= 0) { s->next_auto_id = 1; }
    pwr_sim_reset(s, cfg);
    return PWR_STATUS_OK;
}

void pwr_sim_release(PWR_Simulator* s)
{
    if (s == NULL) { return; }
    PWR_FREE(s->clutter_state);
    PWR_FREE(s->clutter_power);
    s->clutter_cells = 0u;
}

void pwr_sim_reset(PWR_Simulator* s, const PWR_RadarConfig* cfg)
{
    uint32_t i;
    uint64_t seed;

    if (s == NULL || cfg == NULL) { return; }

    seed = (cfg->deterministic != 0) ? s->env.rng_seed
                                     : (s->env.rng_seed ^ 0x9E3779B97F4A7C15ULL);
    pwr_rng_seed(&s->rng,         seed,               1u);
    pwr_rng_seed(&s->rng_clutter, seed ^ 0xD1B54A32ULL, 2u);

    if (s->clutter_state != NULL)
    {
        memset(s->clutter_state, 0,
               (size_t)s->clutter_cells * sizeof(PWR_Complex));
    }

    /* Pulse-to-pulse clutter correlation for a Gaussian Doppler spectrum of
     * standard deviation sigma_f:  rho = exp( -2 * (pi * sigma_f * PRI)^2 ). */
    {
        const double pri   = 1.0 / cfg->prf_hz;
        const double sig_f = pwr_maxd(s->env.clutter_spread_hz, 0.0);
        const double a     = PWR_PI * sig_f * pri;
        s->clutter_rho = exp(-2.0 * a * a);
        s->clutter_rho = pwr_clampd(s->clutter_rho, 0.0, 0.999999);
    }

    for (i = 0u; i < s->target_count; ++i)
    {
        s->state[i].x = s->targets[i].x_m;
        s->state[i].y = s->targets[i].y_m;
        s->state[i].z = s->targets[i].z_m;
        s->state[i].amp_scale = 1.0;
        s->state[i].next_scan_update_s = 0.0;
        s->state[i].active = 0;
    }
}

void pwr_sim_advance(PWR_Simulator* s, double dt, double now_s)
{
    uint32_t i;
    if (s == NULL || !(dt > 0.0)) { return; }
    for (i = 0u; i < s->target_count; ++i)
    {
        const PWR_SimTarget* t = &s->targets[i];
        if (t->enabled == 0) { continue; }
        if (now_s < t->spawn_time_s) { continue; }
        s->state[i].x += t->vx_mps * dt;
        s->state[i].y += t->vy_mps * dt;
        s->state[i].z += t->vz_mps * dt;
        if (s->state[i].z < 0.0) { s->state[i].z = 0.0; }
    }
}

/* ==========================================================================
 *  Swerling fluctuation
 * ==========================================================================
 *  Returned value multiplies the *amplitude*, hence the square root of a
 *  unit-mean power fluctuation.
 * ------------------------------------------------------------------------ */
static double pwr_swerling_amp(PWR_Rng* rng, int32_t model)
{
    switch (model)
    {
    case PWR_SWERLING_1:
    case PWR_SWERLING_2:
        return sqrt(pwr_rng_exponential(rng));
    case PWR_SWERLING_3:
    case PWR_SWERLING_4:
        return sqrt(pwr_rng_chi2_norm(rng, 2u));
    case PWR_SWERLING_0:
    default:
        return 1.0;
    }
}

static int pwr_swerling_is_pulse_to_pulse(int32_t model)
{
    return (model == PWR_SWERLING_2 || model == PWR_SWERLING_4) ? 1 : 0;
}

/* ==========================================================================
 *  Kinematics helpers
 * ========================================================================== */
typedef struct PWR_TargetGeom
{
    double slant_range_m;
    double ground_range_m;
    double azimuth_deg;         /* compass bearing                            */
    double elevation_deg;
    double range_rate_mps;      /* positive == receding                       */
} PWR_TargetGeom;

static void pwr_geom_compute(const PWR_SimTarget* t,
                             const PWR_SimTargetState* st,
                             double antenna_h,
                             PWR_TargetGeom* g)
{
    const double dx = st->x;
    const double dy = st->y;
    const double dz = st->z - antenna_h;
    const double gr = sqrt(dx * dx + dy * dy);
    const double sr = sqrt(dx * dx + dy * dy + dz * dz);

    g->ground_range_m = gr;
    g->slant_range_m  = sr;
    g->azimuth_deg    = pwr_wrap360(pwr_rad_to_deg(atan2(dx, dy)));
    g->elevation_deg  = (gr > 1e-6) ? pwr_rad_to_deg(atan2(dz, gr)) : 90.0;
    g->range_rate_mps = (sr > 1e-6)
        ? ((dx * t->vx_mps + dy * t->vy_mps + dz * t->vz_mps) / sr)
        : 0.0;
}

/* ==========================================================================
 *  Clutter power profile (recomputed whenever the beam moves)
 * ========================================================================== */
static void pwr_sim_build_clutter_profile(PWR_Simulator* s,
                                          const struct PWR_Engine* e)
{
    const PWR_RadarConfig* cfg = &e->cfg;
    const double r0      = e->axis_range_first_m;
    const double bin_m   = e->axis_range_step_m;
    const double horizon = PWR_HORIZON_K * sqrt(pwr_maxd(cfg->antenna_height_m, 0.5));
    const double sea_gain_db = 5.0 * (s->env.sea_state - 2.0);
    const double cnr_ref = pwr_db_to_pow(s->env.clutter_to_noise_db + sea_gain_db);
    const double rain_ref = (s->env.rain_rate_mmph > 0.0)
        ? pwr_db_to_pow(16.0 * log10(s->env.rain_rate_mmph) - 6.0) : 0.0;
    const double rain_max_m = s->env.rain_extent_km * 1000.0;
    const uint32_t n = s->clutter_cells;
    uint32_t i;

    if (s->clutter_power == NULL) { return; }

    for (i = 0u; i < n; ++i)
    {
        const double r = pwr_maxd(r0 + (double)i * bin_m, bin_m);
        double p = 0.0;

        if (s->env.enable_sea_clutter != 0)
        {
            /* Surface clutter: cell area grows as R, two-way spreading falls
             * as R^-4, hence an R^-3 power law, then an exponential knee at
             * the geometric horizon. */
            const double law = PWR_CLUTTER_REF_RANGE_M / r;
            double pc = cnr_ref * law * law * law;
            if (r > horizon)
            {
                pc *= exp(-3.0 * (r - horizon) / pwr_maxd(horizon, 1.0));
            }
            p += pc;
        }
        if (s->env.enable_rain != 0 && rain_ref > 0.0 && r <= rain_max_m)
        {
            /* Volume clutter: cell volume grows as R^2 -> R^-2 power law. */
            const double law = PWR_CLUTTER_REF_RANGE_M / r;
            p += rain_ref * law * law;
        }
        s->clutter_power[i] = (pwr_real)p;
    }
}

/* ==========================================================================
 *  One CPI of receiver output
 * ========================================================================== */
void pwr_sim_generate_cpi(struct PWR_Engine* e)
{
    PWR_Simulator* const s   = &e->sim;
    const PWR_RadarConfig* const cfg = &e->cfg;
    const PWR_DerivedMetrics* const dm = &e->dm;
    const uint32_t n_pulses  = e->n_pulses;
    const uint32_t n_samples = e->n_samples;
    const uint32_t n_tx      = e->wf.n_tx;
    const double   fs        = cfg->sample_rate_hz;
    const double   pri       = 1.0 / cfg->prf_hz;
    const double   lambda    = dm->wavelength_m;
    const double   chirp_K   = cfg->bandwidth_hz / cfg->pulse_width_s;
    const double   tx_centre = 0.5 * (double)(n_tx - 1u);
    const double   beam_az   = e->beam_azimuth_deg;
    const double   t0        = e->scenario_time_s;
    /* A target whose post-compression single-pulse SNR must be S is injected
     * with per-sample amplitude sqrt(S) * sigma_pc, because the compression
     * filter is normalised for unit peak gain and leaves the noise amplitude
     * at exactly sigma_pc. */
    const double   amp_ref   = e->sigma_pc;
    double noise_var = 0.0;
    uint32_t p, i, k;

    if (e->rx == NULL) { return; }

    /* ---------------------------------------------------------------------
     *  1.  Noise floor (thermal + broadband jamming through the pattern)
     * ------------------------------------------------------------------- */
    if (s->env.enable_thermal_noise != 0) { noise_var += 1.0; }
    if (s->env.enable_jammer != 0)
    {
        const double off = pwr_angle_diff(s->env.jammer_azimuth_deg, beam_az);
        const double g   = pwr_pattern_oneway(off, cfg->azimuth_beamwidth_deg);
        noise_var += pwr_db_to_pow(s->env.jammer_power_db) * g *
                     pwr_clampd(s->env.jammer_bandwidth_frac, 0.0, 1.0);
    }

    /* Receiver output beyond the compression copy window can never reach a
     * displayed range bin (the compressor does not even read it), so noise
     * and echoes are only synthesised up to fast_copy.  The tail of each
     * row stays zero from allocation. */
    if (noise_var > 0.0)
    {
        const uint32_t n_gen = e->fast_copy;
        for (p = 0u; p < n_pulses; ++p)
        {
            PWR_Complex* row = &e->rx[(size_t)p * n_samples];
            for (i = 0u; i < n_gen; ++i)
            {
                row[i] = pwr_rng_cgauss(&s->rng, noise_var);
            }
        }
    }
    else
    {
        memset(e->rx, 0, (size_t)n_pulses * n_samples * sizeof(PWR_Complex));
    }

    /* ---------------------------------------------------------------------
     *  2.  Targets
     * ------------------------------------------------------------------- */
    for (k = 0u; k < s->target_count; ++k)
    {
        PWR_SimTarget*      tg = &s->targets[k];
        PWR_SimTargetState* st = &s->state[k];
        PWR_TargetGeom      g;
        double snr_db, snr_lin, amp, az_off, el_gain, az_gain, pat2;
        double tau0, tau_rate, phase0, fd, mp_gain = 1.0;

        if (tg->enabled == 0) { st->active = 0; continue; }
        if (t0 < tg->spawn_time_s) { st->active = 0; continue; }
        if (tg->lifetime_s > 0.0 &&
            t0 > tg->spawn_time_s + tg->lifetime_s) { st->active = 0; continue; }
        st->active = 1;

        pwr_geom_compute(tg, st, cfg->antenna_height_m, &g);
        if (!(g.slant_range_m > 1.0)) { continue; }

        /* -- antenna weighting -------------------------------------------- */
        az_off  = pwr_angle_diff(g.azimuth_deg, beam_az);
        az_gain = pwr_pattern_oneway(az_off, cfg->azimuth_beamwidth_deg);
        el_gain = pwr_pattern_oneway(g.elevation_deg,
                                     cfg->elevation_beamwidth_deg);
        pat2    = (az_gain * el_gain) * (az_gain * el_gain);   /* two-way */
        /* Cheap early-out for a contribution that can never reach the
         * detector.  It has to sit well below the sidelobe floor's own
         * two-way value (1.0e-9): a guard at that level would reject the
         * whole sidelobe region, which is what used to make the floor
         * unreachable for echoes.  The real cull is the amplitude test after
         * the link budget, which is still ahead of the per-pulse loop. */
        if (pat2 < 1.0e-16) { continue; }

        /* -- Swerling fluctuation ----------------------------------------- */
        if (pwr_swerling_is_pulse_to_pulse(tg->swerling) == 0)
        {
            if (t0 >= st->next_scan_update_s)
            {
                st->amp_scale = pwr_swerling_amp(&s->rng, tg->swerling);
                st->next_scan_update_s = t0 + pwr_maxd(dm->scan_period_s, 0.1);
            }
        }

        /* -- multipath lobing --------------------------------------------- */
        if (s->env.enable_multipath != 0 && g.elevation_deg < 10.0 &&
            cfg->antenna_height_m > 0.1 && st->z > 0.1)
        {
            const double dphi = 4.0 * PWR_PI * cfg->antenna_height_m * st->z /
                                (lambda * g.slant_range_m);
            const PWR_Complex sum = pwr_cadd(pwr_c(1.0f, 0.0f),
                                     pwr_cscale(pwr_cexpj(dphi),
                                                (pwr_real)PWR_SEA_REFLECTION));
            /* One-way voltage propagation factor F = |1 + rho*exp(j*dphi)|. */
            mp_gain = (double)pwr_cabs(sum);
            mp_gain = pwr_clampd(mp_gain, 0.0, 2.0);
        }

        /* -- link budget --------------------------------------------------- */
        snr_db = pwr_snr_single_pulse_db(cfg, pwr_maxd(tg->rcs_m2, 1e-6),
                                        g.slant_range_m);
        snr_db += 10.0 * log10(pat2);
        snr_lin = pwr_db_to_pow(snr_db);
        /* The monostatic path traverses the interference pattern twice, so
         * received power carries F^4 and the amplitude F^2 - exactly as the
         * one-way antenna pattern is squared into pat2 above.  Charging F once
         * would halve every lobing null and peak in dB. */
        amp     = sqrt(snr_lin) * amp_ref * st->amp_scale * (mp_gain * mp_gain);
        /* Sensitivity time control is an attenuator ahead of the receiver: it
         * acts on everything arriving from a range gate - echo and clutter
         * alike - and never on the receiver's own thermal noise.  Applying it
         * here, and to the clutter injection, rather than to the compressed
         * cube is what makes it preserve signal-to-clutter ratio while
         * charging the near-in signal-to-noise ratio, which is the entire
         * point of the control. */
        amp    *= pwr_stc_gain_at(e, g.slant_range_m);
        if (!(amp > 1.0e-9)) { continue; }

        /* -- delay, delay rate and Doppler ------------------------------- */
        tau0     = 2.0 * g.slant_range_m / PWR_C_LIGHT * fs;      /* samples */
        tau_rate = 2.0 * g.range_rate_mps / PWR_C_LIGHT * fs * pri;
        fd       = -2.0 * g.range_rate_mps / lambda;
        phase0   = -2.0 * PWR_TWO_PI * g.slant_range_m / lambda;

        if (s->env.enable_range_ambiguity != 0)
        {
            tau0 = fmod(tau0, (double)n_samples);
            if (tau0 < 0.0) { tau0 += (double)n_samples; }
        }

        for (p = 0u; p < n_pulses; ++p)
        {
            PWR_Complex* row = &e->rx[(size_t)p * n_samples];
            const double tau = tau0 + tau_rate * (double)p;
            const double ph  = phase0 + PWR_TWO_PI * fd * pri * (double)p;
            PWR_Complex  carrier;
            double a_p = amp;
            int32_t n_lo, n_hi, n_i;

            if (pwr_swerling_is_pulse_to_pulse(tg->swerling) != 0)
            {
                a_p *= pwr_swerling_amp(&s->rng, tg->swerling);
            }
            carrier = pwr_cscale(pwr_cexpj(ph), (pwr_real)a_p);

            n_lo = (int32_t)ceil(tau);
            n_hi = (int32_t)floor(tau + (double)(n_tx - 1u));
            /* Clamp echo taps to the compression copy window; anything
             * further out is never read by the chain. */
            if (n_hi < 0 || n_lo >= (int32_t)e->fast_copy) { continue; }
            if (n_lo < 0) { n_lo = 0; }
            if (n_hi > (int32_t)e->fast_copy - 1) { n_hi = (int32_t)e->fast_copy - 1; }

            for (n_i = n_lo; n_i <= n_hi; ++n_i)
            {
                /* Exact fractional-delay chirp: evaluate the analytic phase
                 * at the offset u = n - tau instead of interpolating. */
                const double u = (double)n_i - tau;
                const double t = (u - tx_centre) / fs;
                const PWR_Complex sig = pwr_cmul(carrier,
                                                 pwr_cexpj(PWR_PI * chirp_K * t * t));
                row[n_i].re += sig.re;
                row[n_i].im += sig.im;
            }
        }
    }

    /* ---------------------------------------------------------------------
     *  3.  Transmit eclipsing - the receiver is blanked while transmitting
     * ------------------------------------------------------------------- */
    if (s->env.enable_eclipsing != 0)
    {
        const uint32_t blank = pwr_minu(n_tx, n_samples);
        for (p = 0u; p < n_pulses; ++p)
        {
            PWR_Complex* row = &e->rx[(size_t)p * n_samples];
            memset(row, 0, (size_t)blank * sizeof(PWR_Complex));
        }
    }
}

/* ==========================================================================
 *  Distributed clutter, injected in the compressed domain
 * ==========================================================================
 *  A surface or volume clutter field is a dense superposition of echoes of the
 *  transmitted pulse, one per scatterer.  Adding an *unmodulated* random
 *  sequence to the raw I/Q would therefore be wrong twice over: the matched
 *  filter would smear it across a whole uncompressed pulse length instead of
 *  compressing it, and its level would come out low by the compression gain.
 *
 *  Convolving a random reflectivity field with the chirp before compression is
 *  exact but costs two extra fast-time FFTs per pulse - as much as the pulse
 *  compression itself.  Because compression is linear and the reflectivity
 *  field is white over the surface, the compressed clutter has precisely the
 *  same statistics as a white field added directly to the compressed data, up
 *  to a correlation length of one compressed pulse.  So the field is injected
 *  here, after pulse compression and before MTI, which is both exact in level
 *  and Doppler behaviour and considerably cheaper.
 *
 *  Pulse-to-pulse correlation follows an AR(1) recursion whose coefficient
 *  reproduces a Gaussian clutter spectrum of the configured width, so MTI and
 *  the Doppler filter bank see the right internal clutter motion.
 * ------------------------------------------------------------------------ */
void pwr_sim_add_clutter(struct PWR_Engine* e)
{
    PWR_Simulator* const s = &e->sim;
    const uint32_t n_pulses = e->n_pulses;
    const uint32_t n_range  = e->n_range;
    const double   rho      = s->clutter_rho;
    const double   beta     = sqrt(pwr_maxd(1.0 - rho * rho, 0.0));
    const double   scale    = e->sigma_pc;
    /* Range gates still inside the transmit blanking interval carry no echo. */
    const uint32_t first_bin = (s->env.enable_eclipsing != 0 && e->range_decim > 0u)
        ? pwr_minu((e->wf.n_tx > e->range_offset)
                       ? ((e->wf.n_tx - e->range_offset + e->range_decim - 1u) /
                          e->range_decim)
                       : 0u,
                   n_range)
        : 0u;
    uint32_t p, i;

    if (s->env.enable_sea_clutter == 0 && s->env.enable_rain == 0) { return; }
    if (s->clutter_power == NULL || s->clutter_cells < n_range)    { return; }
    if (e->stc_gain == NULL)                                       { return; }

    pwr_sim_build_clutter_profile(s, e);

    for (p = 0u; p < n_pulses; ++p)
    {
        PWR_Complex* PWR_RESTRICT row = &e->pc[(size_t)p * n_range];
        for (i = first_bin; i < n_range; ++i)
        {
            const double pw = (double)s->clutter_power[i];
            PWR_Complex n1;
            double a;
            if (!(pw > 0.0)) { continue; }
            n1 = pwr_rng_cgauss(&s->rng_clutter, 1.0);
            s->clutter_state[i].re = (pwr_real)(rho * (double)s->clutter_state[i].re +
                                                beta * (double)n1.re);
            s->clutter_state[i].im = (pwr_real)(rho * (double)s->clutter_state[i].im +
                                                beta * (double)n1.im);
            /* Clutter arrives through the same attenuator the echo does, so it
             * carries the STC ramp too - which is what makes the control
             * suppress near-in clutter instead of merely desensitising the
             * receiver.  e->stc_gain is all ones while STC is off. */
            a = sqrt(pw) * scale * (double)e->stc_gain[i];
            row[i].re += (pwr_real)(a * (double)s->clutter_state[i].re);
            row[i].im += (pwr_real)(a * (double)s->clutter_state[i].im);
        }
    }
}
