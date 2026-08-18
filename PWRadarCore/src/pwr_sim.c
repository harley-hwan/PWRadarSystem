/* Synthetic receiver front end: one CPI of complex baseband I/Q for a
 * rotating-antenna surveillance radar.
 *
 * Targets are point scatterers injected with exact fractional-delay chirp
 * insertion, intra-CPI range walk and Doppler progression, and Swerling 0-4
 * fluctuation. Geometry is on an effective earth: heights carry the bulge
 * correction, and a target past the smooth-sphere horizon falls into an
 * empirical diffraction shadow. The antenna is a Gaussian mainlobe with an
 * optional tilt and cosecant-squared fill in elevation, crossing over to a
 * 1/theta^2 sidelobe envelope at a configurable level; the azimuth cut is
 * re-evaluated per pulse, because the beam sweeps most of a beamwidth during
 * one coherent batch.
 *
 * The surface field is a compound clutter model: complex Gaussian speckle with
 * an AR(1) spectrum whose width is internal motion and antenna scan modulation
 * in quadrature, optionally modulated by a slowly varying Gamma texture that
 * makes it K-distributed, optionally carrying a bulk Doppler, and split
 * between a sea law (R^-3 to the sea horizon) and a land sector with its own
 * level, its own near-stationary spectrum and a coastline. Rain adds volume
 * clutter (R^-2). A noise jammer is seen through the pattern per pulse.
 * Transmit eclipsing and optional range-ambiguity folding close the list.
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

/* Rate of the beyond-horizon rolloff, per unit of fractional excess range.
 * Empirical, not a diffraction calculation: it gives roughly -20 dB two-way
 * ten per cent past the horizon and -40 dB at twenty.  A shadow of about the
 * right depth is far closer than the flat-earth alternative of seeing a target
 * at full strength however far below the horizon it sits. */
#define PWR_SHADOW_RATE           23.0
/* Range cells over which the compound-K texture is held constant, standing in
 * for the swell-scale correlation length of a real sea. */
#define PWR_TEXTURE_RUN_CELLS     8u

/* ==========================================================================
 *  Antenna pattern
 * ==========================================================================
 *  Gaussian approximation of the one-way power pattern inside the mainlobe:
 *      G(th)/G0 = exp( -2.7726 * (th / th_3dB)^2 )
 *  which is exactly -3 dB at th = th_3dB/2.  Two-way response is the square.
 *
 *  The Gaussian is only meaningful across the mainlobe; past the point where
 *  it has fallen to the configured sidelobe level the response crosses over to
 *  a 1/th^2 envelope anchored there, which is the decay a real aperture's
 *  sidelobe peaks follow.  A *flat* floor instead gives a jammer the same
 *  response at 5 degrees as at 180, and paints a uniform ring of sidelobe
 *  plots around any large close target.  The crossover is computed from the
 *  level so the two always meet exactly and the pattern stays continuous.
 *
 *  This is a smooth envelope, not a structured pattern: there are no discrete
 *  sidelobe peaks, no nulls and no backlobe, so it supports a sidelobe *level*
 *  argument but not sidelobe blanking or cancellation.
 * ------------------------------------------------------------------------ */
typedef struct PWR_Pattern
{
    double floor;       /* one-way power at the envelope anchor              */
    double knee_bw;     /* beamwidths at which the Gaussian reaches it       */
} PWR_Pattern;

static void pwr_pattern_prepare(PWR_Pattern* p, double sidelobe_db)
{
    p->floor   = pwr_db_to_pow(sidelobe_db);
    p->knee_bw = sqrt(log(1.0 / pwr_maxd(p->floor, 1e-12)) / 2.7726);
}

static double pwr_pattern_oneway(const PWR_Pattern* p, double offset_deg,
                                 double bw_deg)
{
    double u;
    if (!(bw_deg > 0.0)) { return 1.0; }
    u = fabs(offset_deg) / bw_deg;
    if (u <= p->knee_bw) { return exp(-2.7726 * u * u); }
    {
        const double k = p->knee_bw / u;
        return p->floor * k * k;
    }
}

/* --------------------------------------------------------------------------
 *  Elevation cut, with tilt and an optional cosecant-squared fill.
 *
 *  Below and across the mainlobe the cut is the same Gaussian, centred on the
 *  tilt.  Above the upper -3 dB edge the fill takes over:
 *
 *      G(th) = G(edge) * ( sin(edge) / sin(th) )^2
 *
 *  For a target at constant altitude h, sin(th) ~ h/R, so G grows as R^2, G^2
 *  as R^4, and the two-way G^2/R^4 of the radar equation comes out constant -
 *  which is the entire point of shaping an antenna this way, and what the
 *  self-test checks.  Past the fill limit the envelope resumes from whatever
 *  level the fill had reached.
 * ------------------------------------------------------------------------ */
static double pwr_pattern_elevation(const PWR_Pattern* p,
                                    const PWR_RadarConfig* cfg, double el_deg)
{
    const double bw   = cfg->elevation_beamwidth_deg;
    const double tilt = cfg->elevation_tilt_deg;
    const double edge = tilt + 0.5 * bw;
    const double fill = cfg->elevation_csc2_deg;
    double g_edge, s_edge, s, r;

    if (!(fill > edge) || el_deg <= edge || !(edge > 0.25))
    {
        return pwr_pattern_oneway(p, el_deg - tilt, bw);
    }
    g_edge = 0.5;                       /* the upper -3 dB edge, exactly */
    s_edge = sin(pwr_deg_to_rad(edge));
    if (el_deg <= fill)
    {
        s = sin(pwr_deg_to_rad(el_deg));
        r = (s > 1.0e-9) ? (s_edge / s) : 1.0;
        return g_edge * r * r;
    }
    s = sin(pwr_deg_to_rad(fill));
    r = (s > 1.0e-9) ? (s_edge / s) : 1.0;
    {
        const double d = (el_deg - fill) / bw;
        return g_edge * r * r * exp(-2.7726 * d * d);
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

    PWR_FREE(s->clutter_texture);
    s->clutter_state   = PWR_ALLOC_ARRAY(PWR_Complex, n_cells);
    s->clutter_power   = PWR_ALLOC_ARRAY(pwr_real,    n_cells);
    s->clutter_texture = PWR_ALLOC_ARRAY(pwr_real,    n_cells);
    if (s->clutter_state == NULL || s->clutter_power == NULL ||
        s->clutter_texture == NULL)
    {
        PWR_FREE(s->clutter_state);
        PWR_FREE(s->clutter_power);
        PWR_FREE(s->clutter_texture);
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
    PWR_FREE(s->clutter_texture);
    s->clutter_cells = 0u;
}

/* --------------------------------------------------------------------------
 *  Clutter spectral width, and the AR(1) coefficient that reproduces it.
 *
 *  Internal motion and antenna scanning add in quadrature.  The scanning term
 *  is the classic 0.265 / t_dwell: the beam sweeping across a patch amplitude-
 *  modulates its return, and that modulation broadens the clutter spectrum
 *  whether or not the sea itself is moving.  Leaving it out makes the
 *  achievable MTI improvement factor a property of the sea alone, independent
 *  of how fast the antenna turns - which is not merely optimistic by about
 *  7 dB at the default 24 rpm, it is wrong in kind, because it says the
 *  rotation rate costs nothing.
 * ------------------------------------------------------------------------ */
double pwr_sim_clutter_spread_hz(const PWR_RadarConfig* cfg, double internal_hz)
{
    double scan_hz = 0.0;
    const double internal = pwr_maxd(internal_hz, 0.0);
    if (cfg->scan_rate_rpm > 0.0 && cfg->azimuth_beamwidth_deg > 0.0)
    {
        /* Time for the mainlobe to cross a point: beamwidth / (6 * rpm) deg/s. */
        const double dwell_s = cfg->azimuth_beamwidth_deg /
                               (6.0 * cfg->scan_rate_rpm);
        if (dwell_s > 1.0e-9) { scan_hz = 0.265 / dwell_s; }
    }
    return sqrt(internal * internal + scan_hz * scan_hz);
}

void pwr_sim_update_clutter_rho(PWR_Simulator* s, const PWR_RadarConfig* cfg)
{
    /* Pulse-to-pulse correlation for a Gaussian clutter spectrum of standard
     * deviation sigma_f:  rho = exp( -2 * (pi * sigma_f * PRI)^2 ). */
    const double pri   = 1.0 / cfg->prf_hz;
    const double sig_f = pwr_sim_clutter_spread_hz(cfg, s->clutter_spread_now_hz);
    const double a     = PWR_PI * sig_f * pri;
    s->clutter_rho = pwr_clampd(exp(-2.0 * a * a), 0.0, 0.999999);
}

/* True when the beam is looking into the land sector this CPI. */
static int pwr_sim_beam_on_land(const PWR_Simulator* s, double beam_az)
{
    if (!(s->env.land_width_deg > 0.0)) { return 0; }
    return (fabs(pwr_angle_diff(beam_az, s->env.land_bearing_deg)) <=
            0.5 * s->env.land_width_deg) ? 1 : 0;
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

    /* Open sea until the first CPI decides otherwise, and a flat texture the
     * next injection will fill if the shape parameter asks for one. */
    s->clutter_spread_now_hz = s->env.clutter_spread_hz;
    s->clutter_phase         = 0.0;
    s->texture_next_s        = -2.0;
    if (s->clutter_texture != NULL)
    {
        for (i = 0u; i < s->clutter_cells; ++i) { s->clutter_texture[i] = 1.0f; }
    }
    pwr_sim_update_clutter_rho(s, cfg);

    for (i = 0u; i < s->target_count; ++i)
    {
        s->state[i].x  = s->targets[i].x_m;
        s->state[i].y  = s->targets[i].y_m;
        s->state[i].z  = s->targets[i].z_m;
        s->state[i].vx = s->targets[i].vx_mps;
        s->state[i].vy = s->targets[i].vy_mps;
        s->state[i].vz = s->targets[i].vz_mps;
        s->state[i].amp_scale = 1.0;
        s->state[i].next_scan_update_s = 0.0;
        s->state[i].active = 0;
    }
}

/* Coordinated turn in the horizontal plane plus along-track acceleration.
 * Course and speed are integrated, then resolved back into components, so a
 * turn keeps the speed and an acceleration keeps the heading - which is what
 * the two controls are meant to mean, and what a tracker's motion model has to
 * cope with. */
void pwr_sim_advance(PWR_Simulator* s, double dt, double now_s)
{
    uint32_t i;
    if (s == NULL || !(dt > 0.0)) { return; }
    for (i = 0u; i < s->target_count; ++i)
    {
        const PWR_SimTarget* t = &s->targets[i];
        PWR_SimTargetState*  st = &s->state[i];
        if (t->enabled == 0) { continue; }
        if (now_s < t->spawn_time_s) { continue; }

        if (t->turn_rate_dps != 0.0 || t->accel_mps2 != 0.0)
        {
            const double sp = sqrt(st->vx * st->vx + st->vy * st->vy);
            double course = (sp > 1.0e-9) ? atan2(st->vx, st->vy) : 0.0;
            double speed  = sp + t->accel_mps2 * dt;
            course += pwr_deg_to_rad(t->turn_rate_dps) * dt;
            if (speed < 0.0) { speed = 0.0; }
            st->vx = speed * sin(course);
            st->vy = speed * cos(course);
        }

        st->x += st->vx * dt;
        st->y += st->vy * dt;
        st->z += st->vz * dt;
        if (st->z < 0.0) { st->z = 0.0; }
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
    double horizon_m;           /* smooth-sphere radar horizon for this target */
    double shadow;              /* one-way power factor beyond the horizon    */
    double eff_height_m;        /* height after the earth bulge is removed    */
} PWR_TargetGeom;

/* --------------------------------------------------------------------------
 *  Target geometry on an effective earth.
 *
 *  Rays are straight and the surface is flat once every height has had the
 *  earth bulge subtracted from it:
 *
 *      h'(d) = h - d^2 / (2 * k * a_earth)
 *
 *  which is the flat-earth-equivalent transformation, and it turns the curved
 *  problem into the one every stage downstream already assumes.  At the
 *  default 24 km display the bulge reaches 34 m, so a target's elevation angle
 *  and the multipath path difference are both wrong without it.
 *
 *  The correction alone does not hide a target below the horizon, so the
 *  smooth-sphere horizon is computed too and an empirical shadow applied past
 *  it.  Without that a surface radar sees a sea-skimmer at 200 km.
 * ------------------------------------------------------------------------ */
static void pwr_geom_compute(const PWR_SimTargetState* st,
                             double antenna_h, double refraction_k,
                             PWR_TargetGeom* g)
{
    const double dx  = st->x;
    const double dy  = st->y;
    const double gr  = sqrt(dx * dx + dy * dy);
    const double k   = (refraction_k > 0.1) ? refraction_k : (4.0 / 3.0);
    const double a_e = k * PWR_EARTH_RADIUS_M;
    const double bulge = gr * gr / (2.0 * a_e);
    const double dz  = (st->z - bulge) - antenna_h;
    const double sr  = sqrt(gr * gr + dz * dz);

    g->ground_range_m = gr;
    g->slant_range_m  = sr;
    g->eff_height_m   = st->z - bulge;
    g->azimuth_deg    = pwr_wrap360(pwr_rad_to_deg(atan2(dx, dy)));
    g->elevation_deg  = (gr > 1e-6) ? pwr_rad_to_deg(atan2(dz, gr)) : 90.0;
    /* From the propagated velocity, not the launch condition: a manoeuvring
     * target's range rate is whatever it is doing now. */
    g->range_rate_mps = (sr > 1e-6)
        ? ((dx * st->vx + dy * st->vy + dz * st->vz) / sr)
        : 0.0;

    {
        const double dh = sqrt(2.0 * a_e * pwr_maxd(antenna_h, 0.0)) +
                          sqrt(2.0 * a_e * pwr_maxd(st->z, 0.0));
        g->horizon_m = dh;
        /* Both heights at the surface is a degenerate configuration - the
         * horizon collapses to zero and everything would be shadowed - so it
         * is read as "no horizon modelled" rather than "nothing visible". */
        g->shadow = (gr <= dh || !(dh > 1.0))
            ? 1.0
            : exp(-PWR_SHADOW_RATE * (gr - dh) / dh);
    }
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
    /* Which surface this bearing looks at.  Land is stronger, very nearly
     * stationary, and it stands in front of the sea rather than beside it, so
     * a bearing with a coast on it carries a different level, a different
     * spectrum, and no sea clutter past the shoreline. */
    const int    on_land   = pwr_sim_beam_on_land(s, e->beam_azimuth_deg);
    const double land_ref  = pwr_db_to_pow(s->env.land_clutter_to_noise_db);
    const double coast_m   = pwr_maxd(s->env.land_range_min_m, 0.0);
    uint32_t i;

    if (s->clutter_power == NULL) { return; }

    /* The spectrum in force is the one belonging to the surface the beam is
     * on.  A single AR(1) state per cell cannot carry two spectra at once, so
     * on a coastal bearing the land width is used throughout - an
     * approximation that is good where the coast is close and the land
     * dominates the bearing, which is the case it exists for. */
    s->clutter_spread_now_hz = (on_land != 0)
        ? s->env.land_spread_hz : s->env.clutter_spread_hz;
    pwr_sim_update_clutter_rho(s, cfg);

    for (i = 0u; i < n; ++i)
    {
        const double r = pwr_maxd(r0 + (double)i * bin_m, bin_m);
        const int    is_land = (on_land != 0 && r >= coast_m) ? 1 : 0;
        double p = 0.0;

        if (s->env.enable_sea_clutter != 0 && is_land == 0)
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
        if (is_land != 0)
        {
            /* Same R^-3 cell-area law, no sea horizon: terrain stands above
             * the surface and stays illuminated well past the sea knee. */
            const double law = PWR_CLUTTER_REF_RANGE_M / r;
            p += land_ref * law * law * law;
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
    /* The antenna does not stand still for a coherent batch.  At the default
     * geometry it turns 1.54 degrees during one CPI against a 1.6 degree beam,
     * so freezing the pattern for the whole batch throws away the beam-shape
     * amplitude modulation that scan modulation and azimuth beam splitting are
     * both made of.  beam_azimuth_deg is the *centre* of the sweep, so the
     * boresight stamped on a plot stays the mean of the illumination that
     * produced it and the centroid gains no bias. */
    const double   sweep_deg = (dm->scan_period_s > 0.0)
        ? (360.0 * dm->cpi_duration_s / dm->scan_period_s) : 0.0;
    const double   az_first   = beam_az - 0.5 * sweep_deg;
    const double   az_step    = (n_pulses > 1u)
        ? (sweep_deg / (double)(n_pulses - 1u)) : 0.0;
    PWR_Pattern    pat;
    double noise_var = 0.0, jam_var = 0.0;
    uint32_t p, i, k;

    if (e->rx == NULL) { return; }
    pwr_pattern_prepare(&pat, cfg->sidelobe_level_db);

    /* ---------------------------------------------------------------------
     *  1.  Noise floor (thermal + broadband jamming through the pattern)
     * ------------------------------------------------------------------- */
    if (s->env.enable_thermal_noise != 0) { noise_var += 1.0; }
    if (s->env.enable_jammer != 0)
    {
        jam_var = pwr_db_to_pow(s->env.jammer_power_db) *
                  pwr_clampd(s->env.jammer_bandwidth_frac, 0.0, 1.0);
    }

    /* Receiver output beyond the compression copy window can never reach a
     * displayed range bin (the compressor does not even read it), so noise
     * and echoes are only synthesised up to fast_copy.  The tail of each
     * row stays zero from allocation. */
    if (noise_var > 0.0 || jam_var > 0.0)
    {
        const uint32_t n_gen = e->fast_copy;
        for (p = 0u; p < n_pulses; ++p)
        {
            PWR_Complex* row = &e->rx[(size_t)p * n_samples];
            double var = noise_var;
            if (jam_var > 0.0)
            {
                /* The jammer is continuous, so it is seen through the pattern
                 * at the boresight of each pulse, not of the batch.  That is
                 * what gives the strobe its azimuth profile. */
                const double az_p = az_first + az_step * (double)p;
                var += jam_var * pwr_pattern_oneway(&pat,
                    pwr_angle_diff(s->env.jammer_azimuth_deg, az_p),
                    cfg->azimuth_beamwidth_deg);
            }
            if (!(var > 0.0))
            {
                /* Clear rather than skip: the buffer carries the previous CPI. */
                memset(row, 0, (size_t)n_gen * sizeof(PWR_Complex));
                continue;
            }
            for (i = 0u; i < n_gen; ++i)
            {
                row[i] = pwr_rng_cgauss(&s->rng, var);
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
        double scat_proj = 0.0;
        uint32_t n_scat = 1u, sc;

        if (tg->enabled == 0) { st->active = 0; continue; }
        if (t0 < tg->spawn_time_s) { st->active = 0; continue; }
        if (tg->lifetime_s > 0.0 &&
            t0 > tg->spawn_time_s + tg->lifetime_s) { st->active = 0; continue; }
        st->active = 1;

        pwr_geom_compute(st, cfg->antenna_height_m,
                         s->env.refraction_k, &g);
        if (!(g.slant_range_m > 1.0)) { continue; }

        /* -- antenna weighting --------------------------------------------
         *  The elevation cut is fixed for the batch, but the azimuth cut is
         *  re-evaluated for every pulse inside the loop below, because the
         *  antenna sweeps most of a beamwidth during one CPI.  What is
         *  computed here is only the batch-centre value, used for the cull. */
        az_off  = pwr_angle_diff(g.azimuth_deg, beam_az);
        az_gain = pwr_pattern_oneway(&pat, az_off, cfg->azimuth_beamwidth_deg);
        el_gain = pwr_pattern_elevation(&pat, cfg, g.elevation_deg);
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
            /* Effective heights, not raw ones: the path difference between the
             * direct and surface-reflected rays is set by how far each end
             * stands above the *curved* surface, which is what the earth-bulge
             * correction already computed for the elevation angle.  Using the
             * raw altitude here while the geometry uses the corrected one is
             * the kind of split that puts the lobing pattern in the wrong
             * place - at 17 km the bulge is 17 m of a 3 km target, and the
             * path difference spans hundreds of radians. */
            const double dphi = 4.0 * PWR_PI * cfg->antenna_height_m *
                                pwr_maxd(g.eff_height_m, 0.0) /
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
        /* Elevation only: the azimuth cut joins per pulse. */
        snr_db += 20.0 * log10(pwr_maxd(el_gain, 1.0e-30));
        /* Diffraction shadow past the radar horizon.  A propagation factor on
         * the path, so the monostatic round trip charges it twice - the same
         * reason the one-way pattern is squared into pat2 above. */
        if (g.shadow < 1.0) { snr_db += 20.0 * log10(pwr_maxd(g.shadow, 1e-30)); }
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
        /* Culled on the batch-centre azimuth gain, which is the largest the
         * sweep will reach for a target the beam is passing. */
        if (!(amp * az_gain > 1.0e-9)) { continue; }

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

        /* -- extent: a row of scattering centres along the body axis -------
         *  Each centre is injected as its own echo at its own slant range, so
         *  it gets its own delay *and* its own carrier phase.  That is what
         *  makes an extended target behave like one: it fills several range
         *  cells, its centres interfere as the aspect changes - which is
         *  aspect-dependent fluctuation and glint, arriving for free out of
         *  the geometry rather than as a bolted-on model - and the resulting
         *  plot has real extent for the clustering to measure.
         *
         *  The along-track axis is the target's own course; a stationary
         *  target keeps whatever heading it was given.  Amplitude is split as
         *  1/sqrt(N) so the total cross-section is the one that was asked for.
         * ------------------------------------------------------------------ */
        n_scat = (tg->scatterers > 1 && tg->length_m > 0.0)
            ? (uint32_t)pwr_mini(tg->scatterers, PWR_MAX_SCATTERERS) : 1u;
        if (n_scat > 1u)
        {
            const double sp = sqrt(st->vx * st->vx + st->vy * st->vy);
            const double hx = (sp > 1.0e-6) ? (st->vx / sp) : 0.0;
            const double hy = (sp > 1.0e-6) ? (st->vy / sp) : 1.0;
            /* Component of the body axis along the line of sight: a beam-on
             * target projects almost nothing into range, a bow-on one projects
             * its whole length.  This is the aspect dependence of extent. */
            const double ux = st->x / pwr_maxd(g.slant_range_m, 1.0);
            const double uy = st->y / pwr_maxd(g.slant_range_m, 1.0);
            scat_proj = hx * ux + hy * uy;
            amp      /= sqrt((double)n_scat);
        }

        for (p = 0u; p < n_pulses; ++p)
        {
            PWR_Complex* row = &e->rx[(size_t)p * n_samples];
            const double tau = tau0 + tau_rate * (double)p;
            const double ph  = phase0 + PWR_TWO_PI * fd * pri * (double)p;
            /* Azimuth cut at this pulse's boresight.  Across a CPI this is the
             * beam-shape amplitude modulation: it is what a dwell centroid
             * splits to get sub-beamwidth azimuth, and what broadens a target's
             * Doppler response by the scan-modulation term. */
            const double az_p = az_first + az_step * (double)p;
            const double gz   = pwr_pattern_oneway(&pat,
                pwr_angle_diff(g.azimuth_deg, az_p),
                cfg->azimuth_beamwidth_deg);
            double a_p = amp * gz;

            if (pwr_swerling_is_pulse_to_pulse(tg->swerling) != 0)
            {
                a_p *= pwr_swerling_amp(&s->rng, tg->swerling);
            }

            for (sc = 0u; sc < n_scat; ++sc)
            {
                /* Offset of this scattering centre along the line of sight,
                 * in metres, from a uniform row spanning the body length. */
                const double frac = (n_scat > 1u)
                    ? ((double)sc / (double)(n_scat - 1u) - 0.5) : 0.0;
                const double dr   = frac * tg->length_m * scat_proj;
                const double tau_s = tau + 2.0 * dr / PWR_C_LIGHT * fs;
                /* Its own carrier phase, which is where the interference
                 * between centres - and hence glint - comes from. */
                const PWR_Complex carrier = pwr_cscale(
                    pwr_cexpj(ph - 2.0 * PWR_TWO_PI * dr / lambda),
                    (pwr_real)a_p);
                int32_t n_lo, n_hi, n_i;

                n_lo = (int32_t)ceil(tau_s);
                n_hi = (int32_t)floor(tau_s + (double)(n_tx - 1u));
                /* Clamp echo taps to the compression copy window; anything
                 * further out is never read by the chain. */
                if (n_hi < 0 || n_lo >= (int32_t)e->fast_copy) { continue; }
                if (n_lo < 0) { n_lo = 0; }
                if (n_hi > (int32_t)e->fast_copy - 1)
                {
                    n_hi = (int32_t)e->fast_copy - 1;
                }

                for (n_i = n_lo; n_i <= n_hi; ++n_i)
                {
                    /* Exact fractional-delay chirp: evaluate the analytic phase
                     * at the offset u = n - tau instead of interpolating. */
                    const double u = (double)n_i - tau_s;
                    const double t = (u - tx_centre) / fs;
                    const PWR_Complex sig = pwr_cmul(carrier,
                                             pwr_cexpj(PWR_PI * chirp_K * t * t));
                    row[n_i].re += sig.re;
                    row[n_i].im += sig.im;
                }
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
    const double   scale    = e->sigma_pc;
    const double   pri      = 1.0 / e->cfg.prf_hz;
    const double   dphi     = PWR_TWO_PI * s->env.clutter_mean_doppler_hz * pri;
    double rho, beta;
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

    /* Rebuilds the level profile, picks the sector's spectrum and refreshes
     * rho, so everything below reads the values this CPI actually needs. */
    pwr_sim_build_clutter_profile(s, e);
    rho  = s->clutter_rho;
    beta = sqrt(pwr_maxd(1.0 - rho * rho, 0.0));

    /* ---- compound-K texture ---------------------------------------------
     *  A Gamma modulation of unit mean, redrawn once a scan.  Sea texture
     *  decorrelates over seconds while the speckle decorrelates over
     *  milliseconds, so holding it across a scan is the right separation of
     *  timescales - and it is what makes the spikes persist long enough for a
     *  CFAR to be fooled by them, which a per-pulse redraw would average away.
     * ------------------------------------------------------------------- */
    if (s->clutter_texture != NULL)
    {
        const double nu = s->env.sea_shape_nu;
        if (!(nu > 0.0))
        {
            if (s->texture_next_s != -1.0)
            {
                for (i = 0u; i < n_range; ++i) { s->clutter_texture[i] = 1.0f; }
                s->texture_next_s = -1.0;   /* Rayleigh: nothing to refresh */
            }
        }
        else if (e->scenario_time_s >= s->texture_next_s ||
                 s->texture_next_s < 0.0)
        {
            /* Held over a run of cells rather than redrawn per bin: sea
             * texture is a swell-scale property with a correlation length of
             * hundreds of metres, not one resolution cell.  Drawing it per
             * cell would leave the CFAR reference window averaging over many
             * independent textures while the cell under test carried its own -
             * the worst case rather than the real one. */
            double tex = 1.0;
            for (i = 0u; i < n_range; ++i)
            {
                if ((i % PWR_TEXTURE_RUN_CELLS) == 0u)
                {
                    tex = pwr_rng_gamma(&s->rng_clutter, nu);
                }
                s->clutter_texture[i] = (pwr_real)tex;
            }
            s->texture_next_s = e->scenario_time_s +
                pwr_maxd(e->dm.scan_period_s, 1.0);
        }
    }

    for (p = 0u; p < n_pulses; ++p)
    {
        PWR_Complex* PWR_RESTRICT row = &e->pc[(size_t)p * n_range];
        /* Bulk drift, carried continuously across CPIs so the surface field
         * does not restart at zero Doppler every batch. */
        const PWR_Complex drift = pwr_cexpj(s->clutter_phase);
        for (i = first_bin; i < n_range; ++i)
        {
            const double pw = (double)s->clutter_power[i];
            PWR_Complex n1, z;
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
            if (s->clutter_texture != NULL)
            {
                a *= sqrt((double)s->clutter_texture[i]);
            }
            z = pwr_cmul(s->clutter_state[i], drift);
            row[i].re += (pwr_real)(a * (double)z.re);
            row[i].im += (pwr_real)(a * (double)z.im);
        }
        s->clutter_phase = fmod(s->clutter_phase + dphi, PWR_TWO_PI);
    }
}
