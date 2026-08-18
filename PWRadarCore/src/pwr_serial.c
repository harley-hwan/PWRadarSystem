/* Configuration and scenarios as flat text.
 *
 * Everything that describes a run - the radar, the environment and the target
 * list - is already a plain aggregate of fixed-width scalars, frozen by
 * PWR_ABI_VERSION. That makes serialisation a field table rather than a
 * format: one row per member, and the reader and the writer walk the same
 * table, so a member can never be written and not read back.
 *
 * The library deliberately does no file I/O. It converts between structs and a
 * caller-owned character buffer; opening files is the application's business,
 * which keeps the library free of stdio dependencies on the target side and
 * lets a caller keep a scenario in memory, in a resource, or over a socket.
 *
 * Reading is tolerant of unknown keys so a file written by a later build still
 * loads, and every value that arrives is passed through pwr_config_clamp() and
 * pwr_config_validate() before it can reach an engine - a hand-edited file is
 * untrusted input like any other.
 */
#include "pwr_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PWR_SERIAL_FORMAT_VERSION 1

/* ==========================================================================
 *  Field tables
 * ========================================================================== */
typedef enum PWR_FieldKind
{
    PWR_FK_D = 0,       /* double  */
    PWR_FK_I32,
    PWR_FK_U32,
    PWR_FK_U64,
    PWR_FK_STR          /* char[PWR_LABEL_LEN] */
} PWR_FieldKind;

typedef struct PWR_Field
{
    const char* name;
    uint32_t    offset;
    int32_t     kind;
} PWR_Field;

#define PWR_FD(st, m)        { #m, (uint32_t)offsetof(st, m), PWR_FK_D }
#define PWR_FI(st, m)        { #m, (uint32_t)offsetof(st, m), PWR_FK_I32 }
#define PWR_FU(st, m)        { #m, (uint32_t)offsetof(st, m), PWR_FK_U32 }
#define PWR_FQ(st, m)        { #m, (uint32_t)offsetof(st, m), PWR_FK_U64 }
#define PWR_FS(st, m)        { #m, (uint32_t)offsetof(st, m), PWR_FK_STR }
/* Nested section member, written with a dotted name. */
#define PWR_FND(sub, m)      { #sub "." #m, \
    (uint32_t)(offsetof(PWR_RadarConfig, sub) + offsetof(PWR_##sub##Config, m)), \
    PWR_FK_D }
#define PWR_FNI(sub, m)      { #sub "." #m, \
    (uint32_t)(offsetof(PWR_RadarConfig, sub) + offsetof(PWR_##sub##Config, m)), \
    PWR_FK_I32 }

/* The nested-member macros key on the struct naming convention: the member
 * `cfar` has type PWR_CfarConfig, `cluster` PWR_ClusterConfig and `tracker`
 * PWR_TrackerConfig.  Spelled out where the token pasting cannot reach. */
#define PWR_F_CFAR_D(m)    { "cfar." #m, \
    (uint32_t)(offsetof(PWR_RadarConfig, cfar) + offsetof(PWR_CfarConfig, m)), PWR_FK_D }
#define PWR_F_CFAR_I(m)    { "cfar." #m, \
    (uint32_t)(offsetof(PWR_RadarConfig, cfar) + offsetof(PWR_CfarConfig, m)), PWR_FK_I32 }
#define PWR_F_CLUS_D(m)    { "cluster." #m, \
    (uint32_t)(offsetof(PWR_RadarConfig, cluster) + offsetof(PWR_ClusterConfig, m)), PWR_FK_D }
#define PWR_F_CLUS_I(m)    { "cluster." #m, \
    (uint32_t)(offsetof(PWR_RadarConfig, cluster) + offsetof(PWR_ClusterConfig, m)), PWR_FK_I32 }
#define PWR_F_TRK_D(m)     { "tracker." #m, \
    (uint32_t)(offsetof(PWR_RadarConfig, tracker) + offsetof(PWR_TrackerConfig, m)), PWR_FK_D }
#define PWR_F_TRK_I(m)     { "tracker." #m, \
    (uint32_t)(offsetof(PWR_RadarConfig, tracker) + offsetof(PWR_TrackerConfig, m)), PWR_FK_I32 }

static const PWR_Field g_radar_fields[] = {
    PWR_FD(PWR_RadarConfig, carrier_hz),
    PWR_FD(PWR_RadarConfig, bandwidth_hz),
    PWR_FD(PWR_RadarConfig, pulse_width_s),
    PWR_FD(PWR_RadarConfig, sample_rate_hz),
    PWR_FD(PWR_RadarConfig, prf_hz),
    PWR_FD(PWR_RadarConfig, peak_power_w),
    PWR_FD(PWR_RadarConfig, duty_limit),
    PWR_FD(PWR_RadarConfig, tx_gain_db),
    PWR_FD(PWR_RadarConfig, rx_gain_db),
    PWR_FD(PWR_RadarConfig, noise_figure_db),
    PWR_FD(PWR_RadarConfig, system_loss_db),
    PWR_FD(PWR_RadarConfig, receiver_bandwidth_hz),
    PWR_FD(PWR_RadarConfig, azimuth_beamwidth_deg),
    PWR_FD(PWR_RadarConfig, elevation_beamwidth_deg),
    PWR_FD(PWR_RadarConfig, elevation_tilt_deg),
    PWR_FD(PWR_RadarConfig, elevation_csc2_deg),
    PWR_FD(PWR_RadarConfig, sidelobe_level_db),
    PWR_FD(PWR_RadarConfig, antenna_height_m),
    PWR_FD(PWR_RadarConfig, scan_rate_rpm),
    PWR_FD(PWR_RadarConfig, range_start_m),
    PWR_FD(PWR_RadarConfig, range_span_m),
    PWR_FU(PWR_RadarConfig, pulses_per_cpi),
    PWR_FU(PWR_RadarConfig, doppler_bins),
    PWR_FU(PWR_RadarConfig, range_bins),
    PWR_FI(PWR_RadarConfig, range_window),
    PWR_FI(PWR_RadarConfig, doppler_window),
    PWR_FI(PWR_RadarConfig, mti_mode),
    PWR_FI(PWR_RadarConfig, enable_pulse_compression),
    PWR_FI(PWR_RadarConfig, enable_doppler_processing),
    PWR_FI(PWR_RadarConfig, enable_stc),
    PWR_FD(PWR_RadarConfig, stc_range_m),

    PWR_F_CFAR_D(pfa),
    PWR_F_CFAR_D(extra_threshold_db),
    PWR_F_CFAR_I(type),
    PWR_F_CFAR_I(guard_range),
    PWR_F_CFAR_I(guard_doppler),
    PWR_F_CFAR_I(train_range),
    PWR_F_CFAR_I(train_doppler),
    PWR_F_CFAR_I(os_rank),
    PWR_F_CFAR_I(trim_low),
    PWR_F_CFAR_I(trim_high),
    PWR_F_CFAR_I(censor_zero_doppler),
    PWR_F_CFAR_I(zero_doppler_guard),
    PWR_F_CFAR_I(peak_selection),

    PWR_F_CLUS_D(min_snr_db),
    PWR_F_CLUS_I(enable),
    PWR_F_CLUS_I(range_tolerance),
    PWR_F_CLUS_I(doppler_tolerance),
    PWR_F_CLUS_I(min_cells),
    PWR_F_CLUS_I(max_cells),

    PWR_F_TRK_D(process_noise_accel),
    PWR_F_TRK_D(meas_sigma_range_m),
    PWR_F_TRK_D(meas_sigma_azimuth_deg),
    PWR_F_TRK_D(meas_sigma_velocity_mps),
    PWR_F_TRK_D(gate_sigma),
    PWR_F_TRK_D(gate_max_range_m),
    PWR_F_TRK_D(init_velocity_sigma),
    PWR_F_TRK_D(max_speed_mps),
    PWR_F_TRK_D(min_speed_for_course),
    PWR_F_TRK_I(enable),
    PWR_F_TRK_I(assoc_mode),
    PWR_F_TRK_I(confirm_m),
    PWR_F_TRK_I(confirm_n),
    PWR_F_TRK_I(delete_misses),
    PWR_F_TRK_I(coast_misses),
    PWR_F_TRK_I(use_doppler_in_gate),
    PWR_F_TRK_I(init_inhibit_m),

    PWR_FU(PWR_RadarConfig, ppi_azimuth_cells),
    PWR_FU(PWR_RadarConfig, rti_rows),
    PWR_FD(PWR_RadarConfig, ppi_persistence_s),
    PWR_FD(PWR_RadarConfig, time_scale),
    PWR_FI(PWR_RadarConfig, max_cpi_per_second),
    PWR_FI(PWR_RadarConfig, worker_thread),
    PWR_FI(PWR_RadarConfig, deterministic)
};

static const PWR_Field g_env_fields[] = {
    PWR_FD(PWR_SimEnvironment, sea_state),
    PWR_FD(PWR_SimEnvironment, clutter_to_noise_db),
    PWR_FD(PWR_SimEnvironment, clutter_spread_hz),
    PWR_FD(PWR_SimEnvironment, rain_rate_mmph),
    PWR_FD(PWR_SimEnvironment, rain_extent_km),
    PWR_FD(PWR_SimEnvironment, refraction_k),
    PWR_FD(PWR_SimEnvironment, clutter_mean_doppler_hz),
    PWR_FD(PWR_SimEnvironment, sea_shape_nu),
    PWR_FD(PWR_SimEnvironment, land_clutter_to_noise_db),
    PWR_FD(PWR_SimEnvironment, land_spread_hz),
    PWR_FD(PWR_SimEnvironment, land_bearing_deg),
    PWR_FD(PWR_SimEnvironment, land_width_deg),
    PWR_FD(PWR_SimEnvironment, land_range_min_m),
    PWR_FD(PWR_SimEnvironment, jammer_azimuth_deg),
    PWR_FD(PWR_SimEnvironment, jammer_power_db),
    PWR_FD(PWR_SimEnvironment, jammer_bandwidth_frac),
    PWR_FQ(PWR_SimEnvironment, rng_seed),
    PWR_FI(PWR_SimEnvironment, enable_thermal_noise),
    PWR_FI(PWR_SimEnvironment, enable_sea_clutter),
    PWR_FI(PWR_SimEnvironment, enable_rain),
    PWR_FI(PWR_SimEnvironment, enable_jammer),
    PWR_FI(PWR_SimEnvironment, enable_multipath),
    PWR_FI(PWR_SimEnvironment, enable_eclipsing),
    PWR_FI(PWR_SimEnvironment, enable_range_ambiguity)
};

static const PWR_Field g_target_fields[] = {
    PWR_FS(PWR_SimTarget, label),
    PWR_FD(PWR_SimTarget, x_m),
    PWR_FD(PWR_SimTarget, y_m),
    PWR_FD(PWR_SimTarget, z_m),
    PWR_FD(PWR_SimTarget, vx_mps),
    PWR_FD(PWR_SimTarget, vy_mps),
    PWR_FD(PWR_SimTarget, vz_mps),
    PWR_FD(PWR_SimTarget, rcs_m2),
    PWR_FD(PWR_SimTarget, spawn_time_s),
    PWR_FD(PWR_SimTarget, lifetime_s),
    PWR_FD(PWR_SimTarget, accel_mps2),
    PWR_FD(PWR_SimTarget, turn_rate_dps),
    PWR_FD(PWR_SimTarget, length_m),
    PWR_FI(PWR_SimTarget, scatterers),
    PWR_FI(PWR_SimTarget, id),
    PWR_FI(PWR_SimTarget, swerling),
    PWR_FI(PWR_SimTarget, target_class),
    PWR_FI(PWR_SimTarget, enabled)
};

#define PWR_NFIELDS(t) (uint32_t)(sizeof(t) / sizeof((t)[0]))

/* ==========================================================================
 *  Writer
 * ========================================================================== */
typedef struct PWR_TextOut
{
    char*  buf;
    size_t cap;
    size_t len;         /* bytes that would be written, capped or not */
} PWR_TextOut;

static void pwr_ser_put(PWR_TextOut* o, const char* fmt, ...)
{
    va_list ap;
    int n;
    char tmp[256];

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) { return; }
    if (o->buf != NULL && o->len + (size_t)n < o->cap)
    {
        memcpy(o->buf + o->len, tmp, (size_t)n);
    }
    o->len += (size_t)n;
}

/* Shortest representation that still reads back bit-identical, so a saved and
 * reloaded configuration compares equal to the original - which matters
 * because the engine's reconfiguration path decides whether to reallocate by
 * comparing derived doubles exactly. */
static void pwr_ser_double(PWR_TextOut* o, const char* key, double v)
{
    char t[48];
    (void)snprintf(t, sizeof(t), "%.15g", v);
    if (strtod(t, NULL) != v) { (void)snprintf(t, sizeof(t), "%.17g", v); }
    pwr_ser_put(o, "%s = %s\n", key, t);
}

static void pwr_ser_fields(PWR_TextOut* o, const void* base,
                           const PWR_Field* f, uint32_t n)
{
    const char* p = (const char*)base;
    uint32_t i;
    for (i = 0u; i < n; ++i)
    {
        const void* m = (const void*)(p + f[i].offset);
        switch (f[i].kind)
        {
        case PWR_FK_D:   pwr_ser_double(o, f[i].name, *(const double*)m); break;
        case PWR_FK_I32: pwr_ser_put(o, "%s = %d\n", f[i].name, *(const int32_t*)m); break;
        case PWR_FK_U32: pwr_ser_put(o, "%s = %u\n", f[i].name, *(const uint32_t*)m); break;
        case PWR_FK_U64: pwr_ser_put(o, "%s = %llu\n", f[i].name,
                                     (unsigned long long)*(const uint64_t*)m); break;
        case PWR_FK_STR: pwr_ser_put(o, "%s = %s\n", f[i].name, (const char*)m); break;
        default: break;
        }
    }
}

/* ==========================================================================
 *  Reader
 * ========================================================================== */
static void pwr_ser_trim(const char** b, const char** e)
{
    while (*b < *e && (**b == ' ' || **b == '\t')) { ++(*b); }
    while (*e > *b)
    {
        const char c = *(*e - 1);
        if (c != ' ' && c != '\t' && c != '\r') { break; }
        --(*e);
    }
}

static int pwr_ser_eq(const char* b, const char* e, const char* name)
{
    const size_t n = (size_t)(e - b);
    return (strlen(name) == n && strncmp(b, name, n) == 0) ? 1 : 0;
}

/* Assigns one key/value into a struct through a field table.  Returns 1 when
 * the key was recognised. */
static int pwr_ser_assign(void* base, const PWR_Field* f, uint32_t n,
                          const char* kb, const char* ke,
                          const char* vb, const char* ve)
{
    char* p = (char*)base;
    char  val[128];
    size_t vn = (size_t)(ve - vb);
    uint32_t i;

    if (vn >= sizeof(val)) { vn = sizeof(val) - 1u; }
    memcpy(val, vb, vn);
    val[vn] = '\0';

    for (i = 0u; i < n; ++i)
    {
        void* m;
        if (pwr_ser_eq(kb, ke, f[i].name) == 0) { continue; }
        m = (void*)(p + f[i].offset);
        switch (f[i].kind)
        {
        case PWR_FK_D:   *(double*)m   = strtod(val, NULL); break;
        case PWR_FK_I32: *(int32_t*)m  = (int32_t)strtol(val, NULL, 10); break;
        case PWR_FK_U32: *(uint32_t*)m = (uint32_t)strtoul(val, NULL, 10); break;
        case PWR_FK_U64: *(uint64_t*)m = (uint64_t)strtoull(val, NULL, 10); break;
        case PWR_FK_STR:
            (void)snprintf((char*)m, PWR_LABEL_LEN, "%s", val);
            break;
        default: break;
        }
        return 1;
    }
    return 0;
}

/* ==========================================================================
 *  Public: configuration only
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_config_save(const PWR_RadarConfig* cfg,
                                       char* buf, size_t cap, size_t* out_len)
{
    PWR_TextOut o;
    if (cfg == NULL) { return PWR_ERR_NULL_POINTER; }
    o.buf = buf; o.cap = cap; o.len = 0u;

    pwr_ser_put(&o, "# PWRadarSystem configuration\n");
    pwr_ser_put(&o, "format = %d\n", PWR_SERIAL_FORMAT_VERSION);
    pwr_ser_put(&o, "abi = %d\n\n", PWR_ABI_VERSION);
    pwr_ser_put(&o, "[radar]\n");
    pwr_ser_fields(&o, cfg, g_radar_fields, PWR_NFIELDS(g_radar_fields));

    if (out_len != NULL) { *out_len = o.len; }
    if (buf == NULL || o.len >= cap) { return PWR_ERR_CAPACITY_EXCEEDED; }
    buf[o.len] = '\0';
    return PWR_STATUS_OK;
}

/* ==========================================================================
 *  Public: whole scenario
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_engine_scenario_save(const PWR_Engine* eng,
                                                char* buf, size_t cap,
                                                size_t* out_len)
{
    PWR_TextOut o;
    uint32_t i;

    if (eng == NULL) { return PWR_ERR_NULL_POINTER; }
    o.buf = buf; o.cap = cap; o.len = 0u;

    pwr_ser_put(&o, "# PWRadarSystem scenario\n");
    pwr_ser_put(&o, "format = %d\n", PWR_SERIAL_FORMAT_VERSION);
    pwr_ser_put(&o, "abi = %d\n\n", PWR_ABI_VERSION);

    pwr_ser_put(&o, "[radar]\n");
    pwr_ser_fields(&o, &eng->cfg, g_radar_fields, PWR_NFIELDS(g_radar_fields));

    pwr_ser_put(&o, "\n[environment]\n");
    pwr_ser_fields(&o, &eng->sim.env, g_env_fields, PWR_NFIELDS(g_env_fields));

    for (i = 0u; i < eng->sim.target_count; ++i)
    {
        pwr_ser_put(&o, "\n[target]\n");
        pwr_ser_fields(&o, &eng->sim.targets[i], g_target_fields,
                       PWR_NFIELDS(g_target_fields));
    }

    if (out_len != NULL) { *out_len = o.len; }
    if (buf == NULL || o.len >= cap) { return PWR_ERR_CAPACITY_EXCEEDED; }
    buf[o.len] = '\0';
    return PWR_STATUS_OK;
}

PWR_EXPORT(PWR_Status) pwr_engine_scenario_load(PWR_Engine* eng,
                                                const char* text,
                                                char* err, size_t err_cap)
{
    enum { SEC_NONE = 0, SEC_RADAR, SEC_ENV, SEC_TARGET };
    PWR_RadarConfig    cfg;
    PWR_SimEnvironment env;
    PWR_SimTarget      targets[PWR_MAX_SIM_TARGETS];
    PWR_SimTarget      cur;
    uint32_t           n_targets = 0u;
    int                section = SEC_NONE, have_target = 0;
    const char*        p;
    PWR_Status         st;

    if (eng == NULL || text == NULL) { return PWR_ERR_NULL_POINTER; }
    if (err != NULL && err_cap > 0u) { err[0] = '\0'; }

    /* Anything the file does not mention keeps the value it has now, so a
     * partial file is a patch rather than a truncation. */
    (void)pwr_engine_get_config(eng, &cfg);
    (void)pwr_engine_get_environment(eng, &env);
    memset(targets, 0, sizeof(targets));
    memset(&cur, 0, sizeof(cur));

    for (p = text; *p != '\0'; )
    {
        const char* lb = p;
        const char* le = p;
        const char* kb;
        const char* ke;
        const char* vb;
        const char* ve;
        const char* eq;

        while (*le != '\0' && *le != '\n') { ++le; }
        p = (*le == '\n') ? (le + 1) : le;
        pwr_ser_trim(&lb, &le);
        if (lb == le || *lb == '#' || *lb == ';') { continue; }

        if (*lb == '[')
        {
            const char* sb = lb + 1;
            const char* se = sb;
            while (se < le && *se != ']') { ++se; }
            if (pwr_ser_eq(sb, se, "radar"))       { section = SEC_RADAR; }
            else if (pwr_ser_eq(sb, se, "environment")) { section = SEC_ENV; }
            else if (pwr_ser_eq(sb, se, "target"))
            {
                if (have_target != 0 && n_targets < PWR_MAX_SIM_TARGETS)
                {
                    targets[n_targets++] = cur;
                }
                memset(&cur, 0, sizeof(cur));
                cur.enabled = 1;
                section = SEC_TARGET;
                have_target = 1;
            }
            else { section = SEC_NONE; }
            continue;
        }

        eq = lb;
        while (eq < le && *eq != '=') { ++eq; }
        if (eq == le) { continue; }
        kb = lb; ke = eq;
        vb = eq + 1; ve = le;
        pwr_ser_trim(&kb, &ke);
        pwr_ser_trim(&vb, &ve);
        if (kb == ke) { continue; }

        switch (section)
        {
        case SEC_RADAR:
            (void)pwr_ser_assign(&cfg, g_radar_fields,
                                 PWR_NFIELDS(g_radar_fields), kb, ke, vb, ve);
            break;
        case SEC_ENV:
            (void)pwr_ser_assign(&env, g_env_fields,
                                 PWR_NFIELDS(g_env_fields), kb, ke, vb, ve);
            break;
        case SEC_TARGET:
            (void)pwr_ser_assign(&cur, g_target_fields,
                                 PWR_NFIELDS(g_target_fields), kb, ke, vb, ve);
            break;
        default:
            break;      /* header keys and unknown sections are ignored */
        }
    }
    if (have_target != 0 && n_targets < PWR_MAX_SIM_TARGETS)
    {
        targets[n_targets++] = cur;
    }

    /* A file is untrusted input: clamp and validate before anything reaches
     * the engine, and leave the engine untouched if it does not survive. */
    (void)pwr_config_clamp(&cfg);
    st = pwr_config_validate(&cfg, err, err_cap);
    if (st != PWR_STATUS_OK) { return st; }

    st = pwr_engine_reconfigure(eng, &cfg, err, err_cap);
    if (st != PWR_STATUS_OK) { return st; }
    (void)pwr_engine_set_environment(eng, &env);
    (void)pwr_engine_target_clear(eng);
    {
        uint32_t i;
        for (i = 0u; i < n_targets; ++i)
        {
            PWR_SimTarget t = targets[i];
            (void)pwr_engine_target_add(eng, &t);
        }
    }
    (void)pwr_engine_reset(eng);
    return PWR_STATUS_OK;
}
