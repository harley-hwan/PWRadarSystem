/* Internal declarations shared by the signal-processing chain. None of this is
 * exported.
 *
 * Data-cube shapes:
 *   rx      [pulse][fast-time sample]   n_pulses  x n_samples
 *   pc      [pulse][range bin]          n_pulses  x n_range
 *   slow    [range bin][doppler bin]    n_range   x n_doppler  (padded)
 *   rd_pow  [doppler bin][range bin]    n_doppler x n_range    (display)
 *
 * The double transpose is deliberate: every FFT then runs over a contiguous
 * row, and the final layout is what both the imagesc display and the
 * range-axis CFAR window want.
 */
#ifndef PWRADAR_PWR_CORE_H
#define PWRADAR_PWR_CORE_H

#include "pwradar/pwr_api.h"
#include "pwr_platform.h"
#include "pwr_math.h"
#include "pwr_fft.h"

/* ==========================================================================
 *  1.  Waveform / matched filter
 * ========================================================================== */
typedef struct PWR_Waveform
{
    PWR_Complex* tx;              /* [n_tx] complex baseband LFM chirp        */
    PWR_Complex* mf_spectrum;     /* [n_fft] equalised, weighted, normalised  */
    pwr_real*    range_window;    /* [n_win] spectral taper prototype         */
    uint32_t     n_win;           /* taper prototype length                   */
    uint32_t     n_tx;            /* samples in the uncompressed pulse        */
    uint32_t     n_fft;           /* fast-time FFT size                       */
    /* Measured, not assumed.  noise_gain is the amplitude gain the filter
     * applies to unit-variance white input once it has been normalised for a
     * unit compressed peak, so sigma_pc == noise_gain exactly.               */
    double       noise_gain;
    double       mismatch_loss_db;/* SNR cost of the equalisation vs matched  */
    double       sidelobe_db;     /* measured peak sidelobe of the response   */
    double       mainlobe_bins;   /* measured -3 dB width in output samples   */
} PWR_Waveform;

PWR_Status pwr_waveform_build(PWR_Waveform* wf,
                              const PWR_RadarConfig* cfg,
                              const PWR_DerivedMetrics* dm,
                              const PWR_FftPlan* fast_plan);
void       pwr_waveform_release(PWR_Waveform* wf);

/* ==========================================================================
 *  2.  Simulator
 * ========================================================================== */
typedef struct PWR_SimTargetState
{
    double   x, y, z;             /* propagated position                      */
    /* Propagated velocity.  It lives here rather than in PWR_SimTarget because
     * acceleration and rate of turn make it a state, not a definition: the
     * target struct keeps the launch condition and this keeps where the target
     * has got to. */
    double   vx, vy, vz;
    double   amp_scale;           /* current Swerling fluctuation             */
    double   next_scan_update_s;  /* scan-to-scan fluctuation bookkeeping     */
    int32_t  active;
    int32_t  _pad0;
} PWR_SimTargetState;

typedef struct PWR_Simulator
{
    PWR_SimEnvironment env;
    PWR_SimTarget      targets[PWR_MAX_SIM_TARGETS];
    PWR_SimTargetState state[PWR_MAX_SIM_TARGETS];
    uint32_t           target_count;
    int32_t            next_auto_id;
    PWR_Rng            rng;
    PWR_Rng            rng_clutter;

    /* Persistent sea-clutter phasors: correlated pulse-to-pulse with a
     * Gaussian spectrum of width env.clutter_spread_hz, decorrelated
     * range-cell to range-cell.  One phasor per range cell.               */
    PWR_Complex*       clutter_state;      /* [n_range] one per range gate  */
    pwr_real*          clutter_power;      /* [n_range] mean power, linear   */
    /* Compound-K texture: a slowly varying, unit-mean Gamma modulation of the
     * Gaussian speckle, redrawn once a scan because sea texture decorrelates
     * in seconds while the speckle decorrelates in milliseconds.  Unity
     * everywhere while the shape parameter leaves the field Rayleigh. */
    pwr_real*          clutter_texture;    /* [n_range]                      */
    double             texture_next_s;
    uint32_t           clutter_cells;
    double             clutter_rho;        /* pulse-to-pulse correlation    */
    /* Running phase of the bulk clutter Doppler, carried across CPIs so the
     * drift is continuous rather than restarting every batch. */
    double             clutter_phase;
    /* Spectral width in force this CPI: land and sea differ by more than an
     * order of magnitude, and which one applies depends on where the beam is
     * pointing. */
    double             clutter_spread_now_hz;
} PWR_Simulator;

/* ==========================================================================
 *  3.  Detector / clustering products
 * ========================================================================== */
typedef struct PWR_CfarWork
{
    uint8_t*  hit;                /* [n_doppler*n_range] 1 == above threshold */
    pwr_real* threshold;          /* [n_doppler*n_range] linear power         */
    /* Local estimate of the interference mean in each cell, unbiased across
     * the estimator families.  Plot SNR is measured against this rather than
     * against a global noise floor, so it stays honest under the MTI filter's
     * Doppler-shaped noise, the STC ramp, transmit eclipsing and clutter. */
    pwr_real* noise_est;          /* [n_doppler*n_range] linear power         */
    double*   integral;           /* [n_doppler*(n_range+1)] per-row prefix sums */
    /* Sliding Doppler-window sums for the CA family: win_psum is the prefix
     * row summed over all 2*Hd+1 window rows, gband_psum the same over the
     * 2*Gd+1 guard-band rows.  Maintained incrementally as the cell-under-
     * test row advances, making the reference sum O(1) per cell. */
    double*   win_psum;           /* [n_range+1]                              */
    double*   gband_psum;         /* [n_range+1]                              */
    pwr_real* train_scratch;      /* [max_train_cells] for OS / TM            */
    uint32_t  train_capacity;
    /* Threshold-multiplier memo, indexed by reference-cell count.  Two of the
     * five families solve their exact false-alarm expression by bisection, so
     * this must persist across CPIs rather than being rebuilt per run: the
     * count takes only a handful of distinct values, varying solely at the map
     * edges, and a change to any parameter alpha was solved for clears it. */
    double*   alpha_tab;          /* [alpha_cap]                              */
    double*   bias_tab;           /* [alpha_cap] mean-unbiasing divisor        */
    uint8_t*  alpha_set;          /* [alpha_cap]                              */
    double*   so_scratch;         /* [2*alpha_cap] GO / SO weight series       */
    uint32_t  alpha_cap;
    double    tuned_pfa;
    int32_t   tuned_type;
    int32_t   tuned_os_rank;
    int32_t   tuned_trim_low;
    int32_t   tuned_trim_high;
    int32_t   tuned_valid;
    uint32_t  cells_tested;
    uint32_t  cells_hit;
} PWR_CfarWork;

/* One plot under construction across the CPIs of a beam dwell.  A strong
 * target stays above threshold for several consecutive CPIs while the beam
 * sweeps across it; folding those per-CPI hits into a single power-weighted
 * plot (azimuth beam splitting) is what keeps one target from producing a
 * string of plots spread over degrees of azimuth. */
typedef struct PWR_DwellPlot
{
    PWR_Detection peak;           /* strongest contributing per-CPI plot      */
    double   peak_w;              /* its linear-SNR weight                    */
    double   w_sum;               /* total linear-SNR weight                  */
    double   waz_e, waz_n;        /* weighted azimuth unit vector (wrap safe) */
    double   wrange_sum;          /* weighted range sum [m]                   */
    double   wvel_sum;            /* weighted radial velocity sum             */
    uint32_t cpi_count;           /* contributing CPIs                        */
    uint32_t last_cpi;            /* CPI index of the newest contribution     */
    uint32_t cells;               /* summed cell count                        */
    int32_t  active;
} PWR_DwellPlot;

/* ==========================================================================
 *  4.  Tracker
 * ========================================================================== */
typedef struct PWR_TrackInternal
{
    double  X[4];                 /* state [x, y, vx, vy] in ENU metres       */
    double  P[16];                /* 4x4 covariance, row major                */
    double  last_predict_time_s;
    double  last_meas_time_s;
    uint32_t id;
    uint32_t hits;
    uint32_t misses;
    uint32_t consecutive_misses;
    uint32_t update_attempts;
    uint32_t history_bits;        /* sliding M-of-N hit history               */
    int32_t  state;               /* PWR_TrackState                           */
    int32_t  target_class;
    int32_t  dwell_state;         /* PWR_DWELL_*, refreshed every CPI         */
    /* Rotating-antenna scan bookkeeping: a plot association raises
     * hit_this_scan; the M-of-N attempt is booked once per revolution, when
     * the beam has swept past the track, and consumes the flag. */
    int32_t  hit_this_scan;
    double   last_attempt_time_s;
    float    snr_db;
    float    innovation_norm;
    double   first_time_s;
    double   last_update_time_s;
    double   radial_velocity_mps;
    PWR_TrackPoint trail[PWR_TRACK_TRAIL_LEN];
    uint32_t trail_count;
    uint32_t trail_head;
    double   trail_last_time_s;
} PWR_TrackInternal;

typedef struct PWR_Tracker
{
    PWR_TrackInternal tracks[PWR_MAX_TRACKS];
    uint32_t          next_id;
    uint32_t          created_total;
    uint32_t          deleted_total;
    /* Assignment work area: cost matrix and the shortest-augmenting-path
     * (Jonker-Volgenant) scratch.  Sized once at construction time so the
     * hot path never allocates.                                            */
    double*  cost;                /* [PWR_MAX_TRACKS * PWR_MAX_DETECTIONS]   */
    int32_t* row_assign;          /* [PWR_MAX_TRACKS]                        */
    int32_t* col_assign;          /* [PWR_MAX_DETECTIONS]                    */
    double*  dual_u;              /* [PWR_ASSOC_DIM + 1]                     */
    double*  dual_v;              /* [PWR_ASSOC_DIM + 1]                     */
    double*  jv_minv;             /* [PWR_ASSOC_DIM + 1]                     */
    int32_t* jv_way;              /* [PWR_ASSOC_DIM + 1]                     */
    int32_t* jv_pcol;             /* [PWR_ASSOC_DIM + 1]                     */
    uint8_t* jv_used;             /* [PWR_ASSOC_DIM + 1]                     */
    /* In-beam candidate list for this CPI. */
    int32_t* cand;                /* [PWR_MAX_TRACKS]                        */
    uint32_t cost_rows, cost_cols;
    /* Boresight at the previous CPI, for detecting when the beam crosses a
     * track's azimuth (the per-scan M-of-N attempt boundary). */
    double   last_beam_az_deg;
} PWR_Tracker;

#define PWR_ASSOC_DIM \
    ((PWR_MAX_TRACKS > PWR_MAX_DETECTIONS) ? PWR_MAX_TRACKS : PWR_MAX_DETECTIONS)

/* ==========================================================================
 *  5.  Published frame storage (one of three rotating buffers)
 * ========================================================================== */
typedef struct PWR_FrameStore
{
    pwr_real*      range_profile_db;
    pwr_real*      range_profile_raw_db;
    pwr_real*      cfar_threshold_db;
    pwr_real*      doppler_spectrum_db;
    pwr_real*      rd_map_db;
    uint8_t*       ppi_video;
    pwr_real*      rti_db;
    PWR_Detection* detections;
    PWR_Track*     tracks;
    PWR_SimTarget* truth;
    PWR_Frame      header;        /* pointers above are wired into this      */
    volatile int32_t refcount;    /* >0 while a consumer holds the buffer    */
} PWR_FrameStore;

#define PWR_FRAME_SLOTS 3u

/* Section flags for the hot-setter mailbox (PWR_Engine::pending_flags). */
#define PWR_PENDING_CFAR        (1u << 0)
#define PWR_PENDING_CLUSTER     (1u << 1)
#define PWR_PENDING_TRACKER     (1u << 2)
#define PWR_PENDING_ENV         (1u << 3)
#define PWR_PENDING_TIME_SCALE  (1u << 4)
#define PWR_PENDING_SCAN_RATE   (1u << 5)
#define PWR_PENDING_STC         (1u << 6)

/* ==========================================================================
 *  6.  The engine
 * ========================================================================== */
struct PWR_Engine
{
    /* ---- configuration -------------------------------------------------- */
    PWR_RadarConfig     cfg;
    PWR_DerivedMetrics  dm;

    /* ---- dimensions (cached from cfg/dm to keep the hot path branch-free) */
    uint32_t            n_pulses;
    uint32_t            n_samples;      /* fast-time samples per PRI          */
    uint32_t            n_range;        /* displayed / processed range bins    */
    uint32_t            n_doppler;
    uint32_t            n_fast_fft;
    uint32_t            range_offset;   /* first processed fast-time sample    */
    uint32_t            range_decim;    /* fast-time decimation into bins      */
    /* Fast-time samples per pulse that can reach a displayed bin (gate window
     * plus one pulse).  The simulator synthesises receiver output and the
     * compressor copies input only up to here; the FFT's remaining bins are
     * zero padding that absorbs the circular wrap. */
    uint32_t            fast_copy;
    uint32_t            ppi_cells;
    uint32_t            rti_rows;

    /* ---- FFT plans ------------------------------------------------------ */
    PWR_FftPlan*        plan_fast;
    PWR_FftPlan*        plan_slow;

    /* ---- waveform ------------------------------------------------------- */
    PWR_Waveform        wf;
    pwr_real*           doppler_window;     /* [n_pulses] (n_pulses_valid used) */
    double              doppler_win_cg;     /* coherent gain                  */
    double              doppler_win_enbw;

    /* ---- calibration ----------------------------------------------------
     *  sigma_pc      : post-pulse-compression noise amplitude (input noise
     *                  variance is exactly 1.0 per complex sample).
     *  doppler_norm  : scale applied to the Doppler FFT output so that the
     *                  published range-Doppler map is calibrated directly in
     *                  dB of signal-to-noise ratio (0 dB == noise floor).
     * ------------------------------------------------------------------- */
    double              sigma_pc;
    double              doppler_norm;
    uint32_t            n_pulses_valid;     /* pulses surviving the MTI filter */
    uint32_t            mti_offset;         /* first valid pulse index         */

    /* ---- published axis definition (single source of truth) ------------- */
    double              axis_range_first_m;
    double              axis_range_step_m;
    double              axis_vel_first_mps;
    double              axis_vel_step_mps;

    /* ---- data cube ------------------------------------------------------ */
    PWR_Complex*        rx;         /* [n_pulses * n_samples]                 */
    PWR_Complex*        pc;         /* [n_pulses * n_range]                   */
    PWR_Complex*        slow;       /* [n_range  * n_doppler]                 */
    PWR_Complex*        fast_scratch; /* [n_fast_fft] per-pulse FFT workspace  */
    pwr_real*           rd_pow;     /* [n_doppler * n_range] linear power     */
    pwr_real*           rd_db;      /* [n_doppler * n_range] dB               */
    pwr_real*           profile_pow;/* [n_range] max over doppler             */
    uint32_t*           profile_peak_j; /* [n_range] argmax Doppler row of the
                                     * profile, recorded by products so the
                                     * CFAR threshold trace needs no rescan  */
    pwr_real*           profile_raw;/* [n_range] pre-MTI incoherent sum       */
    pwr_real*           thresh_prof;/* [n_range] threshold at profile peak    */
    pwr_real*           stc_gain;   /* [n_range] sensitivity time control     */

    /* ---- display accumulators -------------------------------------------
     *  The PPI afterglow is accumulated in Q8 fixed point (65535 == 255.0
     *  display levels) rather than in the published 8-bit video.  With 8-bit
     *  storage the per-CPI decay factor rounds down by up to a whole level, so
     *  the afterglow would fall linearly at one level per CPI - about 230 levels
     *  per revolution - and wipe the picture within a single scan no matter what
     *  time constant the operator selected.  Eight fraction bits reduce that
     *  quantisation loss to under a quarter of a level per revolution.
     * ------------------------------------------------------------------- */
    uint16_t*           ppi_accum;  /* [ppi_cells * n_range] Q8               */
    pwr_real*           rti;        /* [rti_rows * n_range] ring buffer       */
    uint32_t            rti_head;

    /* ---- detector ------------------------------------------------------- */
    PWR_CfarWork        cfar;
    PWR_Detection       detections[PWR_MAX_DETECTIONS];
    uint32_t            detection_count;
    int32_t*            cluster_label;  /* [n_doppler*n_range]                */
    int32_t*            cluster_stack;  /* [n_doppler*n_range] flood-fill      */

    /* ---- dwell plot centroiding (rotating antenna only) ------------------ */
    PWR_DwellPlot       dwell[PWR_MAX_DETECTIONS];
    PWR_Detection       dwell_emit[PWR_MAX_DETECTIONS];

    /* ---- tracker -------------------------------------------------------- */
    PWR_Tracker         tracker;

    /* ---- simulator ------------------------------------------------------ */
    PWR_Simulator       sim;

    /* ---- scan state ----------------------------------------------------- */
    double              scenario_time_s;
    double              beam_azimuth_deg;
    double              last_scan_wrap_deg;
    uint32_t            cursor_range_bin;
    /* CPIs since the PPI afterglow decay was last applied; the full-grid
     * decay pass is batched (see pwr_display_update_ppi). */
    uint32_t            ppi_decay_pending;

    /* ---- published frames ----------------------------------------------- */
    PWR_FrameStore      frames[PWR_FRAME_SLOTS];
    volatile int32_t    publish_index;   /* newest complete slot, -1 == none  */
    volatile int32_t    write_index;
    int32_t             held_index;      /* slot currently handed out         */
    PWR_Mutex           frame_lock;

    /* ---- threading / control --------------------------------------------
     *  Lock roles:
     *    ctrl_lock     guards run_state / quit_flag and backs ctrl_cond,
     *                  which is where the worker parks while stopped/paused.
     *    proc_lock     is held by the worker for the whole duration of one
     *                  CPI and by the full reconfiguration path.
     *    pending_lock  is a leaf lock guarding the hot-setter mailbox below;
     *                  it is only ever held for a struct copy.
     *  Worker order:  ctrl -> (release) -> proc -> pending.
     *  Mutator order: proc -> (release) -> ctrl, or pending alone.
     *  pending_lock is never taken before another lock, so the one nesting
     *  direction (proc -> pending) can never deadlock.
     * ------------------------------------------------------------------- */
    PWR_Thread*         worker;
    PWR_Mutex           ctrl_lock;
    PWR_Mutex           proc_lock;
    PWR_Cond            ctrl_cond;
    volatile int32_t    run_state;       /* PWR_RunState                      */
    volatile int32_t    quit_flag;
    volatile int32_t    worker_alive;
    /* Fairness gate for proc_lock.  Plain mutexes hand no ownership to
     * waiters: a saturated worker (load factor > 1) re-acquires proc_lock
     * within nanoseconds of releasing it, so a blocking mutator can starve
     * for seconds behind back-to-back CPIs.  Mutators announce themselves
     * here and the worker yields between CPIs while anyone is queued.  See
     * pwr_engine_lock_proc_fair(). */
    volatile int32_t    mutator_waiting;

    /* ---- hot-setter mailbox ---------------------------------------------
     *  Setters must never wait out a CPI: at high load the worker holds
     *  proc_lock almost continuously, so a blocking setter would stall a UI
     *  thread for tens of milliseconds per slider notch.  Instead a setter
     *  validates, deposits the new section here and returns; the worker
     *  folds the flagged sections into cfg at the top of the next CPI.  When
     *  the engine is idle the setter applies immediately via trylock. */
    PWR_Mutex           pending_lock;
    PWR_CfarConfig      pending_cfar;
    PWR_ClusterConfig   pending_cluster;
    PWR_TrackerConfig   pending_tracker;
    PWR_SimEnvironment  pending_env;
    double              pending_time_scale;
    double              pending_scan_rpm;
    double              pending_stc_range_m;
    int32_t             pending_stc_enable;
    uint32_t            pending_flags;   /* PWR_PENDING_* bitmask             */

    /* ---- diagnostics ---------------------------------------------------- */
    PWR_Stats           stats;
    PWR_LogFn           log_fn;
    void*               log_user;
    int32_t             log_level;
    char                err[PWR_ERRMSG_LEN];
    double              wall_origin_s;
    double              last_cpi_wall_s;
    double              rate_ewma;
};

/* ==========================================================================
 *  7.  Internal entry points (implemented across the pwr_*.c files)
 * ========================================================================== */

/* --- logging ------------------------------------------------------------- */
void pwr_log(struct PWR_Engine* e, PWR_LogLevel lvl, const char* fmt, ...);
void pwr_set_error(struct PWR_Engine* e, const char* fmt, ...);

/* --- sensitivity time control -------------------------------------------- */
/** Amplitude gain the STC attenuator applies to energy arriving from
 *  @p range_m; unity while the control is off.  The per-bin ramp in
 *  PWR_Engine::stc_gain is built from this, so echo and clutter always see the
 *  same law. */
double pwr_stc_gain_at(const struct PWR_Engine* e, double range_m);

/* --- simulator ---------------------------------------------------------- */
PWR_Status pwr_sim_init(PWR_Simulator* s, const PWR_RadarConfig* cfg,
                        uint32_t n_cells);
void       pwr_sim_release(PWR_Simulator* s);
void       pwr_sim_reset(PWR_Simulator* s, const PWR_RadarConfig* cfg);

/** Integrates every active target forward by @p dt seconds. */
void       pwr_sim_advance(PWR_Simulator* s, double dt, double now_s);

/** Total clutter spectral width: internal motion and antenna scan modulation
 *  in quadrature.  Exposed so the console can show what the rotation rate is
 *  costing the MTI. */
double     pwr_sim_clutter_spread_hz(const PWR_RadarConfig* cfg,
                                     double internal_hz);

/** Recomputes the pulse-to-pulse clutter correlation from the current
 *  environment and geometry.  Must be called whenever the spectral width, the
 *  PRF, the beamwidth or the scan rate moves. */
void       pwr_sim_update_clutter_rho(PWR_Simulator* s,
                                      const PWR_RadarConfig* cfg);

/** Generates one CPI of complex baseband receiver output into e->rx. */
void       pwr_sim_generate_cpi(struct PWR_Engine* e);

/** Adds distributed (surface / volume) clutter to the compressed data cube.
 *  Must run after pwr_chain_pulse_compress() and before pwr_chain_mti(). */
void       pwr_sim_add_clutter(struct PWR_Engine* e);

/* --- signal chain ------------------------------------------------------- */
void       pwr_chain_pulse_compress(struct PWR_Engine* e);

/** Pre-MTI incoherent average for the "raw video" A-scope trace.  Must run
 *  after pwr_sim_add_clutter(), or the trace shows none of the clutter. */
void       pwr_chain_raw_profile(struct PWR_Engine* e);

void       pwr_chain_mti(struct PWR_Engine* e);
void       pwr_chain_doppler(struct PWR_Engine* e);
void       pwr_chain_products(struct PWR_Engine* e);

/* --- detector ----------------------------------------------------------- */
PWR_Status pwr_cfar_alloc(PWR_CfarWork* w, uint32_t n_doppler, uint32_t n_range,
                          const PWR_CfarConfig* cfg);
void       pwr_cfar_release(PWR_CfarWork* w);
void       pwr_cfar_run(struct PWR_Engine* e);
void       pwr_cluster_run(struct PWR_Engine* e);
void       pwr_detections_sort(PWR_Detection* d, uint32_t n);

/** Folds this CPI's per-CPI plots into dwell plots and replaces
 *  e->detections with the dwells the beam has finished sweeping past.
 *  No-op while the antenna is staring (a dwell would never end). */
void       pwr_plots_dwell_merge(struct PWR_Engine* e);

/* --- tracker ------------------------------------------------------------ */
PWR_Status pwr_tracker_init(PWR_Tracker* t);
void       pwr_tracker_release(PWR_Tracker* t);
void       pwr_tracker_reset(PWR_Tracker* t);
void       pwr_tracker_update(struct PWR_Engine* e);

/** Global nearest-neighbour assignment (Jonker-Volgenant shortest augmenting
 *  path).  @p cost is rows x cols row-major and PWR_ASSOC_INFEASIBLE marks a
 *  forbidden pair.  Requires rows <= cols.  On return row_assign[i] holds the
 *  matched column or -1.  Every scratch array must hold at least cols+2
 *  elements; the routine allocates nothing and keeps no static state, so it
 *  is re-entrant. */
#define PWR_ASSOC_INFEASIBLE 1.0e18
void pwr_assign_jv(const double* cost, uint32_t rows, uint32_t cols,
                   uint32_t stride,
                   int32_t* row_assign,
                   double* u, double* v, double* minv,
                   int32_t* way, int32_t* p, uint8_t* used);

/* --- display feeds ------------------------------------------------------ */
void pwr_display_update_ppi(struct PWR_Engine* e);
void pwr_display_update_rti(struct PWR_Engine* e);

/* --- frame publication -------------------------------------------------- */
PWR_Status pwr_frames_alloc(struct PWR_Engine* e);
void       pwr_frames_release(struct PWR_Engine* e);
void       pwr_frame_publish(struct PWR_Engine* e);

#endif /* PWRADAR_PWR_CORE_H */
