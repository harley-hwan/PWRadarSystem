/* Everything that crosses the DLL boundary.
 *
 * These layouts are part of PWR_ABI_VERSION. When changing them:
 *   - order members largest-alignment-first, so MSVC x64 and GCC/Clang x86-64
 *     agree on the layout
 *   - no bitfields, no bool, no enum-typed members (store enums as int32_t)
 *   - fixed-width integer types only
 */
#ifndef PWRADAR_PWR_TYPES_H
#define PWRADAR_PWR_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "pwr_status.h"
#include "pwr_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  0.  Compile-time limits (part of the ABI)
 * ========================================================================== */
#define PWR_MAX_SIM_TARGETS         64u
#define PWR_MAX_DETECTIONS          512u
#define PWR_MAX_TRACKS              128u
#define PWR_TRACK_TRAIL_LEN         64u     /* PPI history trail points     */
#define PWR_LABEL_LEN               32u
#define PWR_ERRMSG_LEN              256u

#define PWR_MIN_RANGE_BINS          64u
#define PWR_MAX_RANGE_BINS          4096u
#define PWR_MIN_PULSES_PER_CPI      2u
#define PWR_MAX_PULSES_PER_CPI      256u
#define PWR_MAX_DOPPLER_BINS        512u
#define PWR_MAX_PPI_AZ_CELLS        2048u
#define PWR_MAX_RTI_ROWS            512u

/* Physical constants used consistently across the whole library. */
#define PWR_C_LIGHT                 299792458.0     /* m/s                  */
#define PWR_BOLTZMANN               1.380649e-23    /* J/K                  */
#define PWR_T0_KELVIN               290.0           /* reference temperature */
#define PWR_PI                      3.14159265358979323846
#define PWR_TWO_PI                  6.28318530717958647692

/* ==========================================================================
 *  1.  Elementary numeric types
 * ========================================================================== */

/* Single precision is used for all bulk signal buffers: it halves memory
 * bandwidth (the dominant cost of the FFT stages) while keeping >120 dB of
 * usable dynamic range, which is far beyond any real receiver's ENOB.      */
typedef float pwr_real;

/* MSVC does not implement C99 `_Complex` arithmetic in C mode, therefore the
 * library carries its own interleaved complex type.  Layout is identical to
 * `float _Complex` so buffers may be reinterpreted on POSIX if desired.    */
typedef struct PWR_Complex
{
    pwr_real re;
    pwr_real im;
} PWR_Complex;

/* ==========================================================================
 *  2.  Enumerations
 * ========================================================================== */

/** Amplitude taper applied before an FFT. */
typedef enum PWR_WindowType
{
    PWR_WIN_RECTANGULAR   = 0,
    PWR_WIN_HANN          = 1,
    PWR_WIN_HAMMING       = 2,
    PWR_WIN_BLACKMAN      = 3,
    PWR_WIN_BLACKMAN_HARRIS = 4,
    PWR_WIN_TAYLOR_35DB   = 5,
    PWR_WIN_TAYLOR_50DB   = 6,
    PWR_WIN_KAISER_B6     = 7,
    PWR_WIN_CHEBYSHEV_60DB = 8,
    PWR_WIN_COUNT
} PWR_WindowType;

/** Moving-target-indication pre-filter applied across the pulse dimension. */
typedef enum PWR_MtiMode
{
    PWR_MTI_OFF           = 0,
    PWR_MTI_DC_REMOVAL    = 1,  /* subtract slow-time mean                  */
    PWR_MTI_TWO_PULSE     = 2,  /* single delay-line canceller  [1 -1]      */
    PWR_MTI_THREE_PULSE   = 3,  /* double canceller             [1 -2 1]    */
    PWR_MTI_FOUR_PULSE    = 4,  /* triple canceller             [1 -3 3 -1] */
    PWR_MTI_COUNT
} PWR_MtiMode;

/** Constant-false-alarm-rate estimator family. */
typedef enum PWR_CfarType
{
    PWR_CFAR_CA           = 0,  /* cell averaging                            */
    PWR_CFAR_GOCA         = 1,  /* greatest-of  (clutter edge robust)        */
    PWR_CFAR_SOCA         = 2,  /* smallest-of  (multi-target robust)        */
    PWR_CFAR_OS           = 3,  /* ordered statistic (k-th ranked cell)      */
    PWR_CFAR_TM           = 4,  /* trimmed mean                              */
    PWR_CFAR_COUNT
} PWR_CfarType;

/** Swerling fluctuation model for the target radar cross-section. */
typedef enum PWR_Swerling
{
    PWR_SWERLING_0        = 0,  /* non-fluctuating (a.k.a. Swerling V)       */
    PWR_SWERLING_1        = 1,  /* scan-to-scan, Rayleigh                    */
    PWR_SWERLING_2        = 2,  /* pulse-to-pulse, Rayleigh                  */
    PWR_SWERLING_3        = 3,  /* scan-to-scan, chi-square 4 dof            */
    PWR_SWERLING_4        = 4,  /* pulse-to-pulse, chi-square 4 dof          */
    PWR_SWERLING_COUNT
} PWR_Swerling;

/** Coarse platform classification used for symbology and gating. */
typedef enum PWR_TargetClass
{
    PWR_CLASS_UNKNOWN     = 0,
    PWR_CLASS_SURFACE     = 1,  /* ship / small boat                        */
    PWR_CLASS_AIR         = 2,  /* fixed wing aircraft                      */
    PWR_CLASS_ROTARY      = 3,  /* helicopter                               */
    PWR_CLASS_MISSILE     = 4,  /* high-speed inbound                       */
    PWR_CLASS_UAV         = 5,
    PWR_CLASS_CLUTTER     = 6,
    PWR_CLASS_COUNT
} PWR_TargetClass;

/** Track life-cycle state (standard M/N confirmation logic).
 *
 *  PWR_TRACK_TERMINATED is published for exactly one frame when a track is
 *  retired, then its slot is released.  That single report is the end-of-track
 *  notification: a consumer that keeps its own track file must delete on it
 *  rather than infer death from a track number that stops appearing. */
typedef enum PWR_TrackState
{
    PWR_TRACK_FREE        = 0,
    PWR_TRACK_TENTATIVE   = 1,
    PWR_TRACK_CONFIRMED   = 2,
    PWR_TRACK_COASTING    = 3,
    PWR_TRACK_TERMINATED  = 4,
    PWR_TRACK_STATE_COUNT
} PWR_TrackState;

/** Engine run state. */
typedef enum PWR_RunState
{
    PWR_RUN_STOPPED       = 0,
    PWR_RUN_RUNNING       = 1,
    PWR_RUN_PAUSED        = 2,
    PWR_RUN_FAULTED       = 3
} PWR_RunState;

/** Diagnostic severity for the log callback. */
typedef enum PWR_LogLevel
{
    PWR_LOG_TRACE = 0,
    PWR_LOG_DEBUG = 1,
    PWR_LOG_INFO  = 2,
    PWR_LOG_WARN  = 3,
    PWR_LOG_ERROR = 4
} PWR_LogLevel;

/** Data-association strategy used by the tracker. */
typedef enum PWR_AssocMode
{
    PWR_ASSOC_NEAREST     = 0,  /* greedy nearest neighbour                 */
    PWR_ASSOC_GLOBAL      = 1,  /* global nearest neighbour (Jonker-Volgenant) */
    PWR_ASSOC_COUNT
} PWR_AssocMode;

/* ==========================================================================
 *  3.  Configuration structures
 * ========================================================================== */

/** CFAR detector configuration (applies to the 2-D range/Doppler map). */
typedef struct PWR_CfarConfig
{
    double   pfa;                   /* design false-alarm probability        */
    double   extra_threshold_db;    /* operator bias on top of computed T    */
    int32_t  type;                  /* PWR_CfarType                          */
    int32_t  guard_range;           /* guard cells each side, range axis      */
    int32_t  guard_doppler;         /* guard cells each side, Doppler axis    */
    int32_t  train_range;           /* training cells each side, range axis    */
    int32_t  train_doppler;         /* training cells each side, Doppler axis  */
    int32_t  os_rank;               /* k for OS-CFAR (1..N), 0 => 0.75*N      */
    int32_t  trim_low;              /* TM-CFAR: cells trimmed from the bottom  */
    int32_t  trim_high;             /* TM-CFAR: cells trimmed from the top     */
    int32_t  censor_zero_doppler;   /* 1 => suppress the DC Doppler column     */
    int32_t  zero_doppler_guard;    /* half-width of the suppressed notch      */
    /* Peak selection: a cell only declares a detection if it is also the
     * strongest cell inside its own guard region.  This is what keeps a single
     * strong target from producing a cross of plots on its range and Doppler
     * sidelobes, and every operational processor does it.                     */
    int32_t  peak_selection;
} PWR_CfarConfig;

/** Detection clustering (converts contiguous CFAR hits into one plot). */
typedef struct PWR_ClusterConfig
{
    double   min_snr_db;            /* reject clusters weaker than this       */
    int32_t  enable;                /* 1 => cluster, 0 => raw cells           */
    int32_t  range_tolerance;        /* connectivity radius, range bins        */
    int32_t  doppler_tolerance;      /* connectivity radius, Doppler bins      */
    int32_t  min_cells;             /* minimum cells for a valid cluster      */
    int32_t  max_cells;             /* clusters larger than this are clutter  */
    int32_t  _pad0;
} PWR_ClusterConfig;

/** Kalman tracker configuration. */
typedef struct PWR_TrackerConfig
{
    double   process_noise_accel;   /* sigma_a  [m/s^2]  (CV model driver)    */
    double   meas_sigma_range_m;    /* range measurement 1-sigma              */
    double   meas_sigma_azimuth_deg;/* azimuth measurement 1-sigma            */
    double   meas_sigma_velocity_mps;/* Doppler measurement 1-sigma           */
    double   gate_sigma;            /* validation gate in Mahalanobis sigmas  */
    double   gate_max_range_m;      /* hard positional gate ceiling           */
    double   init_velocity_sigma;   /* initial velocity uncertainty [m/s]     */
    double   max_speed_mps;         /* kinematic plausibility limit           */
    double   min_speed_for_course;  /* below this, course is held             */
    int32_t  enable;                /* 1 => run tracker                        */
    int32_t  assoc_mode;            /* PWR_AssocMode                           */
    /* M-of-N unit: one update attempt is booked per antenna revolution on a
     * rotating radar (plots are dwell-merged, one per target per scan) and
     * per CPI when the antenna is staring. */
    int32_t  confirm_m;             /* confirm on M hits ...                  */
    int32_t  confirm_n;             /* ... within the last N update attempts  */
    int32_t  delete_misses;         /* terminate after this many consecutive  */
    int32_t  coast_misses;          /* enter COASTING after this many         */
    int32_t  use_doppler_in_gate;   /* include radial velocity in the gate    */
    /* Initiation inhibit radius [m]: a plot this close to an existing track
     * never starts a new one.  Without it, a target that yields two plots in
     * one dwell spawns a duplicate track on every scan.                       */
    int32_t  init_inhibit_m;
} PWR_TrackerConfig;

/**
 *  Complete radar configuration.  Everything the engine needs to build its
 *  processing chain.  Obtain a valid starting point from pwr_config_default()
 *  and then modify individual members.
 */
typedef struct PWR_RadarConfig
{
    /* ---- transmitter / waveform ------------------------------------------ */
    double   carrier_hz;            /* RF centre frequency                    */
    double   bandwidth_hz;          /* LFM sweep bandwidth                    */
    double   pulse_width_s;         /* uncompressed pulse duration            */
    double   sample_rate_hz;        /* complex baseband sampling rate         */
    double   prf_hz;                /* pulse repetition frequency             */
    double   peak_power_w;          /* transmitter peak power                 */
    double   duty_limit;            /* max allowed pulse_width * prf          */

    /* ---- antenna / RF budget -------------------------------------------- */
    double   tx_gain_db;
    double   rx_gain_db;
    double   noise_figure_db;
    double   system_loss_db;
    double   receiver_bandwidth_hz; /* 0 => use sample_rate_hz                */
    double   azimuth_beamwidth_deg;
    double   elevation_beamwidth_deg;
    double   antenna_height_m;
    double   scan_rate_rpm;         /* mechanical rotation, 0 => staring      */

    /* ---- signal processing ---------------------------------------------- */
    double   range_start_m;         /* first displayed range gate             */
    double   range_span_m;          /* displayed range extent                 */
    uint32_t pulses_per_cpi;        /* coherent processing interval length    */
    uint32_t doppler_bins;          /* Doppler FFT size (>= pulses_per_cpi)   */
    uint32_t range_bins;            /* 0 => derive from range_span/resolution */
    int32_t  range_window;          /* PWR_WindowType, pulse-compression taper */
    int32_t  doppler_window;        /* PWR_WindowType, slow-time taper         */
    int32_t  mti_mode;              /* PWR_MtiMode                             */
    int32_t  enable_pulse_compression;
    int32_t  enable_doppler_processing;
    /* Sensitivity time control: an R^2 amplitude ramp (R^4 in power) that
     * attenuates the near-in gates, modelled where a real STC attenuator sits
     * - ahead of the receiver, so it acts on echo and clutter but not on
     * thermal noise.  It therefore preserves signal-to-clutter ratio and
     * charges the near-in signal-to-noise ratio. */
    int32_t  enable_stc;
    double   stc_range_m;           /* range at which the ramp reaches unity  */

    PWR_CfarConfig    cfar;
    PWR_ClusterConfig cluster;
    PWR_TrackerConfig tracker;

    /* ---- display feeds -------------------------------------------------- */
    uint32_t ppi_azimuth_cells;     /* angular resolution of the PPI buffer    */
    uint32_t rti_rows;              /* waterfall history depth                 */
    double   ppi_persistence_s;     /* video afterglow decay time constant    */

    /* ---- real-time execution ------------------------------------------- */
    double   time_scale;            /* 1.0 => real time, <1 => slow motion    */
    int32_t  max_cpi_per_second;    /* throttle (0 => as fast as the clock)   */
    int32_t  worker_thread;         /* 1 => internal thread, 0 => manual step */
    int32_t  deterministic;         /* 1 => fixed RNG stream, repeatable      */
    int32_t  _pad0;
} PWR_RadarConfig;

/* ==========================================================================
 *  4.  Derived (read-only) radar metrics
 * ========================================================================== */
typedef struct PWR_DerivedMetrics
{
    double   wavelength_m;
    double   range_resolution_m;        /* c / (2B)                           */
    double   compressed_pulse_width_s;  /* 1 / B                              */
    double   time_bandwidth_product;    /* Tp * B  == compression gain        */
    double   pulse_compression_gain_db;
    double   coherent_integration_gain_db;
    double   unambiguous_range_m;       /* c / (2*PRF)                        */
    double   unambiguous_velocity_mps;  /* +/- lambda*PRF/4                   */
    double   velocity_resolution_mps;
    double   blind_range_m;             /* c*Tp/2                             */
    double   cpi_duration_s;
    double   duty_cycle;
    double   average_power_w;
    double   noise_power_w;             /* kT0 * B * F                        */
    double   noise_power_dbm;
    double   range_bin_spacing_m;
    double   azimuth_cell_deg;
    double   scan_period_s;
    double   pulses_per_beamwidth;
    uint32_t range_bins;
    uint32_t samples_per_pri;
    uint32_t fast_time_fft_size;
    uint32_t doppler_bins;
} PWR_DerivedMetrics;

/* ==========================================================================
 *  5.  Simulation scenario
 * ========================================================================== */

/** One synthetic target in the scenario. */
typedef struct PWR_SimTarget
{
    double   x_m;                   /* East  (ENU, radar at origin)           */
    double   y_m;                   /* North                                  */
    double   z_m;                   /* Up                                     */
    double   vx_mps;
    double   vy_mps;
    double   vz_mps;
    double   rcs_m2;                /* mean radar cross-section               */
    double   spawn_time_s;          /* becomes active at this scenario time   */
    double   lifetime_s;            /* <=0 => forever                         */
    int32_t  id;                    /* caller supplied, unique                */
    int32_t  swerling;              /* PWR_Swerling                            */
    int32_t  target_class;          /* PWR_TargetClass                         */
    int32_t  enabled;
    char     label[PWR_LABEL_LEN];
} PWR_SimTarget;

/** Environment / interference model. */
typedef struct PWR_SimEnvironment
{
    double   sea_state;             /* Douglas sea state 0..6                 */
    /* The distributed-clutter field is a function of range only: there is no
     * land/sea partition, no coastline and no terrain masking, so no land
     * clutter parameter is offered.  Surface clutter is described entirely by
     * sea_state and clutter_to_noise_db below. */
    double   clutter_to_noise_db;   /* CNR at the first range gate            */
    double   clutter_spread_hz;     /* internal clutter motion spectral width */
    double   rain_rate_mmph;
    double   rain_extent_km;
    double   jammer_azimuth_deg;
    double   jammer_power_db;       /* broadband noise jamming, JNR           */
    double   jammer_bandwidth_frac; /* 0..1 of the receiver band              */
    uint64_t rng_seed;
    int32_t  enable_thermal_noise;
    int32_t  enable_sea_clutter;
    int32_t  enable_rain;
    int32_t  enable_jammer;
    int32_t  enable_multipath;
    int32_t  enable_eclipsing;      /* transmit blanking of near returns      */
    int32_t  enable_range_ambiguity;
    int32_t  _pad0;
} PWR_SimEnvironment;

/* ==========================================================================
 *  6.  Detection and track reports
 * ========================================================================== */
typedef struct PWR_Detection
{
    double   range_m;
    double   azimuth_deg;           /* power-weighted dwell centroid (beam
                                     * splitting); boresight while staring    */
    double   radial_velocity_mps;   /* positive == opening (receding)         */
    double   time_s;
    double   amplitude_db;          /* post-integration magnitude, dB         */
    double   snr_db;
    double   threshold_db;
    double   centroid_range_bin;    /* sub-bin interpolated                   */
    double   centroid_doppler_bin;
    uint32_t range_bin;
    uint32_t doppler_bin;
    uint32_t cell_count;
    int32_t  ambiguous;             /* 1 => beyond unambiguous range/velocity */
    /* Track that consumed this plot during data association in the same CPI,
     * 0 when the plot went unassociated.  Lets the console draw plot-to-track
     * association and label the plot table. */
    int32_t  assoc_track_id;
    int32_t  _pad0;
} PWR_Detection;

typedef struct PWR_TrackPoint
{
    float    x_m;
    float    y_m;
} PWR_TrackPoint;

/* Beam feedback for the current CPI, published per track so the console can
 * show the dwell-level hit/miss evidence the M-of-N logic accumulates. */
#define PWR_DWELL_IDLE  0           /* beam not on the predicted position     */
#define PWR_DWELL_MISS  1           /* illuminated, no plot associated        */
#define PWR_DWELL_HIT   2           /* illuminated and updated by a plot      */

typedef struct PWR_Track
{
    double   x_m, y_m;              /* filtered position, ENU                 */
    double   vx_mps, vy_mps;        /* filtered velocity                      */
    double   range_m, azimuth_deg;  /* polar restatement of the above         */
    double   speed_mps, course_deg; /* course is compass (0=N, 90=E)          */
    double   radial_velocity_mps;
    double   pos_cov[4];            /* row-major 2x2 position covariance      */
    double   first_time_s, last_time_s, last_update_time_s;
    double   quality;               /* 0..1 track score                       */
    float    snr_db;
    float    innovation_norm;       /* last normalised innovation             */
    uint32_t id;
    uint32_t hits, misses, consecutive_misses, update_attempts;
    int32_t  state;                 /* PWR_TrackState                          */
    int32_t  target_class;          /* PWR_TargetClass                         */
    /* Sliding dwell window, newest attempt in bit 0, 1 == hit.  The confirm
     * logic counts these bits over the last confirm_n attempts, so this is
     * the exact M-of-N evidence behind the track state. */
    uint32_t history_bits;
    int32_t  dwell_state;           /* PWR_DWELL_*, this CPI's beam feedback   */
    uint32_t trail_count;
    uint32_t trail_head;
    PWR_TrackPoint trail[PWR_TRACK_TRAIL_LEN];
} PWR_Track;

/* ==========================================================================
 *  7.  Runtime statistics
 * ========================================================================== */
typedef struct PWR_Stats
{
    double   scenario_time_s;
    double   wall_time_s;
    double   cpi_rate_hz;               /* achieved CPI throughput            */
    double   t_simulate_ms;             /* per-stage timings, EWMA smoothed   */
    double   t_pulse_compress_ms;
    double   t_mti_ms;
    double   t_doppler_ms;
    double   t_cfar_ms;
    double   t_cluster_ms;
    double   t_track_ms;
    double   t_total_ms;
    double   load_factor;               /* t_total / cpi_duration             */
    double   measured_noise_floor_db;
    double   measured_pfa;              /* detections / cells tested          */
    uint64_t cpi_count;
    uint64_t scan_count;
    uint64_t detection_total;
    uint64_t cells_tested_total;
    uint64_t frames_published;
    uint64_t frames_dropped;
    uint32_t detections_current;
    /* Plots lost at the PWR_MAX_DETECTIONS ceiling, cumulative.  Non-zero means
     * the plot table saturated and the reported picture is incomplete - the
     * detector kept the strongest plots and threw the rest away. */
    uint32_t detections_dropped;
    uint32_t tracks_active;
    uint32_t tracks_confirmed;
    uint32_t tracks_created_total;
    uint32_t tracks_deleted_total;
    int32_t  run_state;                 /* PWR_RunState                        */
    int32_t  _pad0;
} PWR_Stats;

/* ==========================================================================
 *  8.  Published frame (read-only snapshot handed to the presentation layer)
 * ==========================================================================
 *  Lifetime contract
 *  -----------------
 *  Pointers inside PWR_Frame remain valid only between a successful
 *  pwr_engine_frame_acquire() and the matching pwr_engine_frame_release().
 *  The engine keeps a triple buffer, so the producer never blocks and the
 *  consumer always sees a self-consistent, tear-free snapshot.
 * ------------------------------------------------------------------------ */
typedef struct PWR_Frame
{
    /* ---- identification ------------------------------------------------- */
    uint64_t             sequence;          /* monotonic CPI index            */
    double               time_s;            /* scenario timestamp             */
    double               beam_azimuth_deg;  /* boresight for this CPI         */
    double               beam_azimuth_start_deg;
    double               beam_azimuth_end_deg;

    /* ---- axis metadata (so the UI needs no radar knowledge) ------------- */
    uint32_t             range_bins;
    uint32_t             doppler_bins;
    uint32_t             ppi_az_cells;
    uint32_t             rti_rows;
    double               range_first_m;
    double               range_step_m;
    double               velocity_first_mps;
    double               velocity_step_mps;
    double               noise_floor_db;

    /* ---- 1-D products --------------------------------------------------- */
    const pwr_real*      range_profile_db;      /* [range_bins]  A-scope      */
    const pwr_real*      range_profile_raw_db;  /* [range_bins]  pre-MTI      */
    const pwr_real*      cfar_threshold_db;     /* [range_bins]              */
    const pwr_real*      doppler_spectrum_db;   /* [doppler_bins] @cursor bin */

    /* ---- 2-D products --------------------------------------------------- */
    const pwr_real*      rd_map_db;             /* [doppler_bins*range_bins]  */
    const uint8_t*       ppi_video;             /* [ppi_az_cells*range_bins]  */
    const pwr_real*      rti_db;                /* [rti_rows*range_bins] ring */
    uint32_t             rti_head;              /* newest row index           */
    uint32_t             cursor_range_bin;

    /* ---- reports -------------------------------------------------------- */
    const PWR_Detection* detections;
    uint32_t             detection_count;
    const PWR_Track*     tracks;
    uint32_t             track_count;

    /* ---- ground truth (verification overlay) ---------------------------- */
    const PWR_SimTarget* truth;
    uint32_t             truth_count;

    PWR_Stats            stats;
    PWR_DerivedMetrics   metrics;
} PWR_Frame;

/* ==========================================================================
 *  9.  Callbacks
 * ========================================================================== */
typedef void (*PWR_LogFn)(void* user, int32_t level, const char* message);

/** Opaque engine handle. */
typedef struct PWR_Engine PWR_Engine;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PWRADAR_PWR_TYPES_H */
