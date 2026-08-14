/* Canned verification scenarios. Each isolates one behaviour a PW radar
 * processor has to be checked against, so the whole acceptance list can be
 * walked from the console without editing a scenario file.
 */
#include "pwr_core.h"

#include <stdio.h>
#include <string.h>

typedef struct PWR_ScenarioDef
{
    const char* name;
    void (*build)(PWR_Engine* e);
} PWR_ScenarioDef;

/* ---- helpers ------------------------------------------------------------ */
static void pwr_add(PWR_Engine* e, const char* label,
                    double x, double y, double z,
                    double vx, double vy, double vz,
                    double rcs, int32_t swerling, int32_t cls)
{
    PWR_SimTarget t;
    memset(&t, 0, sizeof(t));
    t.x_m = x; t.y_m = y; t.z_m = z;
    t.vx_mps = vx; t.vy_mps = vy; t.vz_mps = vz;
    t.rcs_m2 = rcs;
    t.swerling = swerling;
    t.target_class = cls;
    t.enabled = 1;
    t.lifetime_s = 0.0;
    (void)snprintf(t.label, sizeof(t.label), "%s", (label != NULL) ? label : "");
    (void)pwr_engine_target_add(e, &t);
}

/* Places a target at a bearing / range, moving on a given course and speed. */
static void pwr_add_polar(PWR_Engine* e, const char* label,
                          double bearing_deg, double range_m, double alt_m,
                          double course_deg, double speed_mps,
                          double rcs, int32_t swerling, int32_t cls)
{
    const double b = pwr_deg_to_rad(bearing_deg);
    const double c = pwr_deg_to_rad(course_deg);
    pwr_add(e, label,
            range_m * sin(b), range_m * cos(b), alt_m,
            speed_mps * sin(c), speed_mps * cos(c), 0.0,
            rcs, swerling, cls);
}

static void pwr_env(PWR_Engine* e, int sea, double cnr_db, int rain, int jam)
{
    PWR_SimEnvironment env;
    (void)pwr_engine_get_environment(e, &env);
    env.enable_thermal_noise = 1;
    env.enable_sea_clutter   = (sea != 0) ? 1 : 0;
    env.sea_state            = (double)sea;
    env.clutter_to_noise_db  = cnr_db;
    env.enable_rain          = (rain != 0) ? 1 : 0;
    env.rain_rate_mmph       = (rain != 0) ? 8.0 : 0.0;
    env.enable_jammer        = (jam != 0) ? 1 : 0;
    env.enable_eclipsing     = 1;
    (void)pwr_engine_set_environment(e, &env);
}

/* ==========================================================================
 *  Scenario 0 - noise only.  Used to verify the achieved false-alarm rate
 *  against the design Pfa.
 * ========================================================================== */
static void pwr_sc_noise_only(PWR_Engine* e)
{
    pwr_env(e, 0, 0.0, 0, 0);
}

/* ==========================================================================
 *  Scenario 1 - single inbound aircraft, Swerling 1.  Baseline detection and
 *  track-initiation check.
 * ========================================================================== */
static void pwr_sc_single_air(PWR_Engine* e)
{
    pwr_env(e, 0, 0.0, 0, 0);
    pwr_add_polar(e, "TGT-01 AIR", 45.0, 18000.0, 3000.0, 225.0, 180.0,
                  6.0, PWR_SWERLING_1, PWR_CLASS_AIR);
}

/* ==========================================================================
 *  Scenario 2 - maritime picture in sea clutter.  Exercises MTI, the R^-3
 *  clutter law and surface-target detection near the clutter knee.
 * ========================================================================== */
static void pwr_sc_maritime(PWR_Engine* e)
{
    pwr_env(e, 3, 26.0, 0, 0);
    pwr_add_polar(e, "SHIP-01",  20.0,  6500.0, 8.0,  110.0,  7.0,
                  2000.0, PWR_SWERLING_1, PWR_CLASS_SURFACE);
    pwr_add_polar(e, "SHIP-02", 118.0, 11500.0, 6.0,  300.0, 11.0,
                  800.0,  PWR_SWERLING_1, PWR_CLASS_SURFACE);
    pwr_add_polar(e, "BOAT-03", 250.0,  4200.0, 3.0,   35.0, 14.0,
                  25.0,   PWR_SWERLING_3, PWR_CLASS_SURFACE);
    pwr_add_polar(e, "BUOY-04", 305.0,  8800.0, 2.0,    0.0,  0.0,
                  10.0,   PWR_SWERLING_0, PWR_CLASS_SURFACE);
}

/* ==========================================================================
 *  Scenario 3 - mixed air and surface picture, the nominal operating case.
 * ========================================================================== */
static void pwr_sc_mixed(PWR_Engine* e)
{
    pwr_env(e, 2, 20.0, 0, 0);
    pwr_add_polar(e, "AIR-01",   30.0, 20000.0, 4500.0, 210.0, 220.0,
                  8.0,    PWR_SWERLING_1, PWR_CLASS_AIR);
    pwr_add_polar(e, "AIR-02",  155.0, 15000.0, 2200.0,  20.0, 160.0,
                  4.0,    PWR_SWERLING_2, PWR_CLASS_AIR);
    pwr_add_polar(e, "HELO-03", 200.0,  9000.0,  600.0,  95.0,  55.0,
                  12.0,   PWR_SWERLING_3, PWR_CLASS_ROTARY);
    pwr_add_polar(e, "SHIP-04",  80.0,  7500.0,    8.0, 250.0,   9.0,
                  1500.0, PWR_SWERLING_1, PWR_CLASS_SURFACE);
    pwr_add_polar(e, "SHIP-05", 300.0, 13000.0,    8.0,  60.0,  12.0,
                  900.0,  PWR_SWERLING_1, PWR_CLASS_SURFACE);
    pwr_add_polar(e, "UAV-06",  340.0, 11000.0, 1200.0, 160.0,  38.0,
                  0.6,    PWR_SWERLING_3, PWR_CLASS_UAV);
}

/* ==========================================================================
 *  Scenario 4 - two targets separated by roughly one range resolution cell.
 *  This is the range-resolution and OS-CFAR multi-target check.
 * ========================================================================== */
static void pwr_sc_resolution(PWR_Engine* e)
{
    PWR_DerivedMetrics dm;
    double dr;
    pwr_env(e, 0, 0.0, 0, 0);
    (void)pwr_engine_get_metrics(e, &dm);
    dr = dm.range_resolution_m;
    /* Both members of the range pair open on the same bearing at the same
     * speed, so they stay 1.2 cells apart and share one Doppler bin - which is
     * what makes this a *range* resolution test.  The speed has to be non-zero:
     * a stationary pair lands in the zero-Doppler notch the default CFAR
     * censors and neither target is ever detected, so the pair could not
     * exercise the resolution it exists to check.  60 m/s is clear of the notch
     * and well inside the unambiguous interval. */
    pwr_add_polar(e, "PAIR-A", 90.0, 12000.0,          2000.0, 90.0, 60.0,
                  5.0, PWR_SWERLING_0, PWR_CLASS_AIR);
    pwr_add_polar(e, "PAIR-B", 90.0, 12000.0 + 1.2*dr, 2000.0, 90.0, 60.0,
                  5.0, PWR_SWERLING_0, PWR_CLASS_AIR);
    /* Same range, different Doppler: the range-Doppler separation check. */
    pwr_add_polar(e, "DOPP-C", 270.0, 12000.0, 2000.0,  90.0, 120.0,
                  5.0, PWR_SWERLING_0, PWR_CLASS_AIR);
    pwr_add_polar(e, "DOPP-D", 270.0, 12000.0, 2000.0, 270.0, 120.0,
                  5.0, PWR_SWERLING_0, PWR_CLASS_AIR);
}

/* ==========================================================================
 *  Scenario 5 - crossing tracks.  Stresses the data association: two targets
 *  pass through the same resolution cell on opposite courses.
 * ========================================================================== */
static void pwr_sc_crossing(PWR_Engine* e)
{
    pwr_env(e, 1, 14.0, 0, 0);
    /* All three reach (0, 12000) together at t = 93.3 s, which is the crossing
     * the association logic is under test at.
     *
     * CROSS-B closes head-on, so its radial rate is its whole speed, and that
     * makes it the one target in the set whose speed has to be chosen with the
     * Doppler ambiguity in mind: at 150 m/s it would sit within 3 m/s of the
     * first blind speed (lambda*PRF/2 = 147.4 m/s), fold onto the zero-Doppler
     * column and be deleted by the CFAR notch the default configuration
     * censors - permanently invisible, turning the three-way crossing into a
     * two-way one.  120 m/s folds clear of the notch; the start range is set
     * so the rendezvous time is unchanged. */
    pwr_add(e, "CROSS-A", -14000.0, 12000.0, 3000.0,  150.0,    0.0, 0.0,
            8.0, PWR_SWERLING_1, PWR_CLASS_AIR);
    pwr_add(e, "CROSS-B",      0.0, 23200.0, 3050.0,    0.0, -120.0, 0.0,
            8.0, PWR_SWERLING_1, PWR_CLASS_AIR);
    pwr_add(e, "CROSS-C",  14000.0, 12000.0, 2900.0, -150.0,    0.0, 0.0,
            8.0, PWR_SWERLING_1, PWR_CLASS_AIR);
}

/* ==========================================================================
 *  Scenario 6 - high-speed inbound.  The radial velocity exceeds the
 *  unambiguous interval, so the plot folds in Doppler: the ambiguity flag and
 *  the tracker's tolerance of a wrapped range rate are what is under test.
 * ========================================================================== */
static void pwr_sc_high_speed(PWR_Engine* e)
{
    pwr_env(e, 2, 18.0, 0, 0);
    pwr_add_polar(e, "MSL-01", 10.0, 22000.0, 1500.0, 190.0, 300.0,
                  0.3, PWR_SWERLING_0, PWR_CLASS_MISSILE);
    pwr_add_polar(e, "MSL-02", 12.0, 20000.0,  120.0, 192.0, 260.0,
                  0.15, PWR_SWERLING_2, PWR_CLASS_MISSILE);
    pwr_add_polar(e, "SHIP-03", 95.0, 5000.0,    8.0, 275.0,   6.0,
                  1200.0, PWR_SWERLING_1, PWR_CLASS_SURFACE);
}

/* ==========================================================================
 *  Scenario 7 - degraded environment: rain cell plus a noise jammer on a
 *  fixed bearing.  Verifies the CFAR's clutter-edge behaviour and shows the
 *  jamming strobe on the PPI.
 * ========================================================================== */
static void pwr_sc_ecm(PWR_Engine* e)
{
    PWR_SimEnvironment env;
    pwr_env(e, 4, 28.0, 1, 1);
    (void)pwr_engine_get_environment(e, &env);
    env.jammer_azimuth_deg    = 135.0;
    env.jammer_power_db       = 22.0;
    env.rain_rate_mmph        = 12.0;
    env.rain_extent_km        = 10.0;
    env.enable_multipath      = 1;
    (void)pwr_engine_set_environment(e, &env);

    pwr_add_polar(e, "AIR-01",  135.0, 17000.0, 3000.0, 315.0, 200.0,
                  10.0,   PWR_SWERLING_1, PWR_CLASS_AIR);
    pwr_add_polar(e, "AIR-02",   60.0, 12000.0, 1200.0, 240.0, 140.0,
                  5.0,    PWR_SWERLING_1, PWR_CLASS_AIR);
    pwr_add_polar(e, "SHIP-03", 190.0,  9000.0,    8.0,  10.0,  10.0,
                  1800.0, PWR_SWERLING_1, PWR_CLASS_SURFACE);
}

/* ==========================================================================
 *  Scenario 8 - detection-range walk.  A single Swerling 1 target opens from
 *  close range to the horizon so the operator can read the actual detection
 *  range off the PPI and compare it with the radar equation.
 * ========================================================================== */
static void pwr_sc_range_walk(PWR_Engine* e)
{
    pwr_env(e, 1, 12.0, 0, 0);
    /* Altitude is deliberately low.  pwr_add_polar() takes a *ground* range,
     * and the elevation pattern is a Gaussian pinned to the horizon with no
     * tilt and no cosecant-squared fill, so a target that starts 2 km out at
     * airway altitude sits above 50 degrees elevation and is rejected by the
     * pattern - it would only appear several seconds later, at a range set by
     * the beam edge rather than by the radar equation, which is exactly the
     * comparison this scenario exists to make.  At 200 m the walk begins at
     * 5 degrees elevation and stays inside the beam all the way out. */
    pwr_add_polar(e, "WALK-01", 0.0, 2000.0, 200.0, 0.0, 200.0,
                  1.0, PWR_SWERLING_1, PWR_CLASS_AIR);
    pwr_add_polar(e, "WALK-02", 180.0, 2000.0, 200.0, 180.0, 200.0,
                  10.0, PWR_SWERLING_1, PWR_CLASS_AIR);
}

/* ==========================================================================
 *  Registry
 * ========================================================================== */
static const PWR_ScenarioDef g_scenarios[] = {
    { "00  Noise only (Pfa calibration)",        pwr_sc_noise_only },
    { "01  Single inbound aircraft (SW1)",       pwr_sc_single_air },
    { "02  Maritime picture in sea clutter",     pwr_sc_maritime   },
    { "03  Mixed air and surface picture",       pwr_sc_mixed      },
    { "04  Resolution pair (range + Doppler)",   pwr_sc_resolution },
    { "05  Crossing tracks (association)",       pwr_sc_crossing   },
    { "06  High-speed inbound (Doppler fold)",   pwr_sc_high_speed },
    { "07  Rain plus noise jammer (ECM)",        pwr_sc_ecm        },
    { "08  Detection-range walk",                pwr_sc_range_walk }
};
#define PWR_SCENARIO_COUNT \
    (uint32_t)(sizeof(g_scenarios) / sizeof(g_scenarios[0]))

PWR_EXPORT(const char*) pwr_scenario_name(uint32_t index)
{
    return (index < PWR_SCENARIO_COUNT) ? g_scenarios[index].name : NULL;
}

PWR_EXPORT(PWR_Status) pwr_engine_load_scenario(PWR_Engine* eng, uint32_t index)
{
    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    if (index >= PWR_SCENARIO_COUNT) { return PWR_ERR_OUT_OF_RANGE; }

    (void)pwr_engine_target_clear(eng);
    g_scenarios[index].build(eng);
    (void)pwr_engine_reset(eng);
    pwr_log(eng, PWR_LOG_INFO, "loaded scenario %u: %s",
            index, g_scenarios[index].name);
    return PWR_STATUS_OK;
}
