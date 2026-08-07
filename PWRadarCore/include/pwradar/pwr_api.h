/* ==========================================================================
 *  PWRadarSystem - PW Radar Detection System
 *  ------------------------------------------------------------------------
 *  File     : pwr_api.h
 *  Module   : PWRadarCore (public)
 *  Purpose  : The complete exported surface of PWRadarCore.dll / libpwradar.
 *
 *  Threading contract
 *  ------------------
 *    * pwr_engine_create / _destroy            : caller-serialised.
 *    * pwr_engine_frame_acquire / _release     : safe from one consumer
 *                                                thread concurrently with the
 *                                                internal worker.
 *    * every other pwr_engine_* setter         : internally locked, safe from
 *                                                any thread.
 *
 *  Build-time switches
 *  -------------------
 *    PWR_BUILD_SHARED   defined while compiling the library as a shared object
 *    PWR_BUILD_STATIC   defined to build/consume a static library
 *    (neither)          consuming the shared object -> dllimport on Windows
 *
 *  Language : ISO C17
 * ========================================================================== */
#ifndef PWRADAR_PWR_API_H
#define PWRADAR_PWR_API_H

#include "pwr_types.h"

/* --------------------------------------------------------------------------
 *  Linkage / calling convention
 * ------------------------------------------------------------------------ */
#if defined(_WIN32)
#  if defined(PWR_BUILD_STATIC)
#    define PWR_API
#  elif defined(PWR_BUILD_SHARED)
#    define PWR_API __declspec(dllexport)
#  else
#    define PWR_API __declspec(dllimport)
#  endif
#  define PWR_CALL  __cdecl
#else
#  if defined(PWR_BUILD_SHARED) && (defined(__GNUC__) || defined(__clang__))
#    define PWR_API __attribute__((visibility("default")))
#  else
#    define PWR_API
#  endif
#  define PWR_CALL
#endif

#define PWR_EXPORT(rettype) PWR_API rettype PWR_CALL

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  1.  Library identification
 * ========================================================================== */

/** Returns the ABI version the binary was built with. Compare against
 *  PWR_ABI_VERSION before doing anything else. */
PWR_EXPORT(uint32_t)    pwr_abi_version(void);

/** Human readable version, e.g. "1.0.0 (MSVC 19.40 x64 C17)". */
PWR_EXPORT(const char*) pwr_version_string(void);

/** Textual name of a status code, never NULL. */
PWR_EXPORT(const char*) pwr_status_string(PWR_Status status);

/** Textual names for the public enumerations (for populating UI combos). */
PWR_EXPORT(const char*) pwr_window_name(PWR_WindowType w);
PWR_EXPORT(const char*) pwr_mti_name(PWR_MtiMode m);
PWR_EXPORT(const char*) pwr_cfar_name(PWR_CfarType c);
PWR_EXPORT(const char*) pwr_swerling_name(PWR_Swerling s);
PWR_EXPORT(const char*) pwr_class_name(PWR_TargetClass c);
PWR_EXPORT(const char*) pwr_track_state_name(PWR_TrackState s);

/* ==========================================================================
 *  2.  Configuration helpers (pure functions, no engine required)
 * ========================================================================== */

/** Fills @p cfg with a validated, self-consistent S-band maritime/air
 *  surveillance configuration.  Always succeeds for a non-NULL pointer. */
PWR_EXPORT(PWR_Status) pwr_config_default(PWR_RadarConfig* cfg);

/** Fills @p env with a calm-sea, noise-only environment. */
PWR_EXPORT(PWR_Status) pwr_sim_environment_default(PWR_SimEnvironment* env);

/** Validates @p cfg.  On failure a human readable reason is written into
 *  @p err (may be NULL).  @p err_cap should be >= PWR_ERRMSG_LEN. */
PWR_EXPORT(PWR_Status) pwr_config_validate(const PWR_RadarConfig* cfg,
                                           char*  err,
                                           size_t err_cap);

/** Computes every derived radar metric from @p cfg without allocating. */
PWR_EXPORT(PWR_Status) pwr_config_derive(const PWR_RadarConfig* cfg,
                                         PWR_DerivedMetrics*    out);

/** Clamps every member of @p cfg into its legal domain, in place. */
PWR_EXPORT(PWR_Status) pwr_config_clamp(PWR_RadarConfig* cfg);

/** Classic radar range equation: single-pulse SNR in dB for a given RCS and
 *  range.  Useful for the UI's link-budget readout. */
PWR_EXPORT(double)     pwr_snr_single_pulse_db(const PWR_RadarConfig* cfg,
                                               double rcs_m2,
                                               double range_m);

/** Maximum detection range for a required single-pulse SNR. */
PWR_EXPORT(double)     pwr_max_range_for_snr(const PWR_RadarConfig* cfg,
                                             double rcs_m2,
                                             double required_snr_db);

/* ==========================================================================
 *  3.  Engine life cycle
 * ========================================================================== */

/** Creates an engine.  @p cfg may be NULL to accept the defaults.
 *  The engine is created in PWR_RUN_STOPPED state. */
PWR_EXPORT(PWR_Status) pwr_engine_create(const PWR_RadarConfig* cfg,
                                         PWR_Engine**           out_engine);

/** Stops the worker, releases every resource.  Safe with NULL. */
PWR_EXPORT(void)       pwr_engine_destroy(PWR_Engine* eng);

/** Installs a diagnostic sink.  Pass fn == NULL to detach. */
PWR_EXPORT(PWR_Status) pwr_engine_set_log(PWR_Engine* eng,
                                          PWR_LogFn   fn,
                                          void*       user,
                                          PWR_LogLevel min_level);

/** Last failure detail for this engine (never NULL). */
PWR_EXPORT(const char*) pwr_engine_last_error(const PWR_Engine* eng);

/* ==========================================================================
 *  4.  Execution control
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_engine_start(PWR_Engine* eng);
PWR_EXPORT(PWR_Status) pwr_engine_stop(PWR_Engine* eng);
PWR_EXPORT(PWR_Status) pwr_engine_pause(PWR_Engine* eng);
PWR_EXPORT(PWR_Status) pwr_engine_resume(PWR_Engine* eng);

/** Resets scenario time, clears tracks, PPI and RTI history. Configuration
 *  and target list are preserved. */
PWR_EXPORT(PWR_Status) pwr_engine_reset(PWR_Engine* eng);

/** Processes exactly @p cpi_count coherent processing intervals on the
 *  calling thread.  Valid while stopped or paused, and it is the only way to
 *  advance an engine created with cfg->worker_thread == 0. */
PWR_EXPORT(PWR_Status) pwr_engine_step(PWR_Engine* eng, uint32_t cpi_count);

PWR_EXPORT(PWR_RunState) pwr_engine_run_state(const PWR_Engine* eng);

/* ==========================================================================
 *  5.  Runtime reconfiguration
 * ========================================================================== */

/** Applies a whole new configuration.  Buffers are reallocated only if a
 *  dimension actually changed, so this is cheap enough to call from a slider
 *  callback.  On validation failure nothing is modified. */
PWR_EXPORT(PWR_Status) pwr_engine_reconfigure(PWR_Engine*            eng,
                                              const PWR_RadarConfig* cfg,
                                              char*                  err,
                                              size_t                 err_cap);

/** Snapshot of the configuration currently in force. */
PWR_EXPORT(PWR_Status) pwr_engine_get_config(const PWR_Engine* eng,
                                             PWR_RadarConfig*  out);

PWR_EXPORT(PWR_Status) pwr_engine_get_metrics(const PWR_Engine*   eng,
                                              PWR_DerivedMetrics* out);

/* Fine-grained hot setters: none of these resize a buffer. */
PWR_EXPORT(PWR_Status) pwr_engine_set_cfar(PWR_Engine* eng,
                                           const PWR_CfarConfig* cfar);
PWR_EXPORT(PWR_Status) pwr_engine_set_cluster(PWR_Engine* eng,
                                              const PWR_ClusterConfig* cl);
PWR_EXPORT(PWR_Status) pwr_engine_set_tracker(PWR_Engine* eng,
                                              const PWR_TrackerConfig* tk);
PWR_EXPORT(PWR_Status) pwr_engine_set_mti(PWR_Engine* eng, PWR_MtiMode mode);
PWR_EXPORT(PWR_Status) pwr_engine_set_windows(PWR_Engine* eng,
                                              PWR_WindowType range_win,
                                              PWR_WindowType doppler_win);
PWR_EXPORT(PWR_Status) pwr_engine_set_scan_rate(PWR_Engine* eng, double rpm);
PWR_EXPORT(PWR_Status) pwr_engine_set_time_scale(PWR_Engine* eng, double scale);
PWR_EXPORT(PWR_Status) pwr_engine_set_stc(PWR_Engine* eng,
                                          int32_t enable, double range_m);

/** Selects which range gate feeds PWR_Frame::doppler_spectrum_db. */
PWR_EXPORT(PWR_Status) pwr_engine_set_cursor_range_bin(PWR_Engine* eng,
                                                       uint32_t bin);

/* ==========================================================================
 *  6.  Scenario management
 * ========================================================================== */
PWR_EXPORT(PWR_Status) pwr_engine_set_environment(PWR_Engine* eng,
                                                  const PWR_SimEnvironment* env);
PWR_EXPORT(PWR_Status) pwr_engine_get_environment(const PWR_Engine* eng,
                                                  PWR_SimEnvironment* out);

/** Adds a target.  If tgt->id is 0 an id is assigned and written back. */
PWR_EXPORT(PWR_Status) pwr_engine_target_add(PWR_Engine* eng, PWR_SimTarget* tgt);
PWR_EXPORT(PWR_Status) pwr_engine_target_update(PWR_Engine* eng,
                                                const PWR_SimTarget* tgt);
PWR_EXPORT(PWR_Status) pwr_engine_target_remove(PWR_Engine* eng, int32_t id);
PWR_EXPORT(PWR_Status) pwr_engine_target_clear(PWR_Engine* eng);
PWR_EXPORT(uint32_t)   pwr_engine_target_count(const PWR_Engine* eng);
PWR_EXPORT(PWR_Status) pwr_engine_target_at(const PWR_Engine* eng,
                                            uint32_t index,
                                            PWR_SimTarget* out);

/** Loads a canned verification scenario.  @p index selects the scenario;
 *  pwr_scenario_name() enumerates them (returns NULL past the end). */
PWR_EXPORT(const char*) pwr_scenario_name(uint32_t index);
PWR_EXPORT(PWR_Status)  pwr_engine_load_scenario(PWR_Engine* eng, uint32_t index);

/* ==========================================================================
 *  7.  Frame consumption
 * ========================================================================== */

/** Acquires the newest complete frame.  Returns PWR_STATUS_NO_DATA if the
 *  engine has not published anything yet.  Must be paired with
 *  pwr_engine_frame_release(). */
PWR_EXPORT(PWR_Status) pwr_engine_frame_acquire(PWR_Engine* eng,
                                                PWR_Frame*  out_frame);

/** Releases the frame acquired by the calling thread. */
PWR_EXPORT(PWR_Status) pwr_engine_frame_release(PWR_Engine* eng);

/** Cheap sequence probe: lets the UI skip a redraw when nothing changed. */
PWR_EXPORT(uint64_t)   pwr_engine_frame_sequence(const PWR_Engine* eng);

PWR_EXPORT(PWR_Status) pwr_engine_get_stats(const PWR_Engine* eng,
                                            PWR_Stats* out);

/* ==========================================================================
 *  8.  Utility exposed for the presentation layer
 * ========================================================================== */

/** Monotonic high-resolution seconds; identical clock the engine uses. */
PWR_EXPORT(double)     pwr_time_now_s(void);

/** Sleeps at least @p seconds, yielding the CPU. */
PWR_EXPORT(void)       pwr_sleep_s(double seconds);

/** Window coefficient generator, exposed so the UI can plot tapers. */
PWR_EXPORT(PWR_Status) pwr_window_fill(PWR_WindowType type,
                                       pwr_real* dst, uint32_t n);

/** In-place complex FFT / IFFT, n must be a power of two.  Exposed so the UI
 *  can run its own spectral tooling without a second FFT implementation. */
PWR_EXPORT(PWR_Status) pwr_fft_forward(PWR_Complex* data, uint32_t n);
PWR_EXPORT(PWR_Status) pwr_fft_inverse(PWR_Complex* data, uint32_t n);

/** Smallest power of two >= v (v <= 2^31). */
PWR_EXPORT(uint32_t)   pwr_next_pow2(uint32_t v);

/** Runs the built-in numerical self-test suite (FFT round-trip, pulse
 *  compression peak position, CFAR Pfa, Kalman consistency).  Returns PWR_STATUS_OK
 *  when every case passes; a report is written to @p report when non-NULL. */
PWR_EXPORT(PWR_Status) pwr_self_test(char* report, size_t report_cap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PWRADAR_PWR_API_H */
