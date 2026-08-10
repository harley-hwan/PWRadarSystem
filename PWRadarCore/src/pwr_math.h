/* Numeric primitives shared by the whole chain: complex arithmetic, decibel
 * helpers, amplitude tapers, a reproducible RNG with the distributions the
 * clutter and target models need, order statistics, and the small dense linear
 * algebra the Kalman filter uses.
 */
#ifndef PWRADAR_PWR_MATH_H
#define PWRADAR_PWR_MATH_H

#include <math.h>
#include <stdint.h>

#include "pwradar/pwr_types.h"
#include "pwr_platform.h"

/* --------------------------------------------------------------------------
 *  Scalar helpers
 * ------------------------------------------------------------------------ */
#define PWR_DB_FLOOR        (-200.0f)   /* value substituted for log10(0)    */
#define PWR_EPS_F           1.0e-30f
#define PWR_EPS_D           1.0e-300

static PWR_INLINE double pwr_clampd(double v, double lo, double hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static PWR_INLINE float pwr_clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static PWR_INLINE int32_t pwr_clampi(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static PWR_INLINE uint32_t pwr_maxu(uint32_t a, uint32_t b) { return (a > b) ? a : b; }
static PWR_INLINE uint32_t pwr_minu(uint32_t a, uint32_t b) { return (a < b) ? a : b; }

static PWR_INLINE uint32_t pwr_clampu32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
static PWR_INLINE int32_t  pwr_maxi(int32_t a, int32_t b)   { return (a > b) ? a : b; }
static PWR_INLINE int32_t  pwr_mini(int32_t a, int32_t b)    { return (a < b) ? a : b; }
static PWR_INLINE double   pwr_maxd(double a, double b)      { return (a > b) ? a : b; }
static PWR_INLINE double   pwr_mind(double a, double b)      { return (a < b) ? a : b; }

/** 10*log10 with a hard floor so the display never receives -inf/NaN. */
static PWR_INLINE float pwr_pow_to_db(float power)
{
    if (!(power > PWR_EPS_F)) { return PWR_DB_FLOOR; }
    return 10.0f * log10f(power);
}

/** 20*log10 of a magnitude, same flooring behaviour. */
static PWR_INLINE float pwr_mag_to_db(float mag)
{
    if (!(mag > PWR_EPS_F)) { return PWR_DB_FLOOR; }
    return 20.0f * log10f(mag);
}

static PWR_INLINE double pwr_db_to_pow(double db) { return pow(10.0, 0.1 * db); }
static PWR_INLINE double pwr_db_to_amp(double db) { return pow(10.0, 0.05 * db); }

static PWR_INLINE double pwr_deg_to_rad(double d) { return d * (PWR_PI / 180.0); }
static PWR_INLINE double pwr_rad_to_deg(double r) { return r * (180.0 / PWR_PI); }

/** Wraps an angle into [0, 360). */
static PWR_INLINE double pwr_wrap360(double deg)
{
    double v = fmod(deg, 360.0);
    if (v < 0.0) { v += 360.0; }
    return v;
}

/** Wraps an angle into (-180, 180]. */
static PWR_INLINE double pwr_wrap180(double deg)
{
    double v = pwr_wrap360(deg + 180.0) - 180.0;
    return (v <= -180.0) ? 180.0 : v;
}

/** Shortest signed angular difference a-b, in (-180, 180]. */
static PWR_INLINE double pwr_angle_diff(double a, double b)
{
    return pwr_wrap180(a - b);
}

/* --------------------------------------------------------------------------
 *  Complex arithmetic (all inline, no aliasing surprises)
 * ------------------------------------------------------------------------ */
static PWR_INLINE PWR_Complex pwr_c(pwr_real re, pwr_real im)
{
    PWR_Complex z; z.re = re; z.im = im; return z;
}

static PWR_INLINE PWR_Complex pwr_cadd(PWR_Complex a, PWR_Complex b)
{
    return pwr_c(a.re + b.re, a.im + b.im);
}

static PWR_INLINE PWR_Complex pwr_csub(PWR_Complex a, PWR_Complex b)
{
    return pwr_c(a.re - b.re, a.im - b.im);
}

static PWR_INLINE PWR_Complex pwr_cmul(PWR_Complex a, PWR_Complex b)
{
    return pwr_c(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re);
}

/** a * conj(b) - the kernel of matched filtering and correlation. */
static PWR_INLINE PWR_Complex pwr_cmul_conj(PWR_Complex a, PWR_Complex b)
{
    return pwr_c(a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im);
}

static PWR_INLINE PWR_Complex pwr_cscale(PWR_Complex a, pwr_real s)
{
    return pwr_c(a.re * s, a.im * s);
}

static PWR_INLINE PWR_Complex pwr_cconj(PWR_Complex a)
{
    return pwr_c(a.re, -a.im);
}

static PWR_INLINE pwr_real pwr_cabs2(PWR_Complex a)
{
    return a.re * a.re + a.im * a.im;
}

static PWR_INLINE pwr_real pwr_cabs(PWR_Complex a)
{
    return sqrtf(pwr_cabs2(a));
}

/** exp(j*theta) */
static PWR_INLINE PWR_Complex pwr_cexpj(double theta)
{
    return pwr_c((pwr_real)cos(theta), (pwr_real)sin(theta));
}

/* --------------------------------------------------------------------------
 *  Amplitude tapers
 * ------------------------------------------------------------------------ */

/** Fills dst[0..n-1] with the requested window.  Symmetric definition
 *  (denominator n-1) which is the convention MATLAB's window functions use
 *  with the default 'symmetric' flag. */
PWR_Status pwr_window_generate(PWR_WindowType type, pwr_real* dst, uint32_t n);

/** Coherent gain sum(w)/n - the loss in peak signal amplitude. */
double pwr_window_coherent_gain(const pwr_real* w, uint32_t n);

/** Equivalent noise bandwidth n*sum(w^2)/sum(w)^2 - the SNR loss factor. */
double pwr_window_enbw(const pwr_real* w, uint32_t n);

/* --------------------------------------------------------------------------
 *  Random numbers - PCG32, chosen for its tiny state, excellent statistical
 *  quality and exact reproducibility across compilers (no libc dependency).
 * ------------------------------------------------------------------------ */
typedef struct PWR_Rng
{
    uint64_t state;
    uint64_t inc;       /* must be odd */
    double   spare;     /* cached second Box-Muller deviate */
    int32_t  has_spare;
    int32_t  _pad0;
} PWR_Rng;

void     pwr_rng_seed(PWR_Rng* r, uint64_t seed, uint64_t stream);
uint32_t pwr_rng_u32(PWR_Rng* r);

/** Uniform in [0,1). 24 significant bits, never exactly 1.0. */
double   pwr_rng_uniform(PWR_Rng* r);

/** Uniform in (0,1] - safe as a log() argument. */
double   pwr_rng_uniform_pos(PWR_Rng* r);

/** Standard normal, mean 0, sigma 1 (polar Box-Muller, cached spare). */
double   pwr_rng_normal(PWR_Rng* r);

/** Exponential with unit mean - the power of a Rayleigh (Swerling 1/2) echo. */
double   pwr_rng_exponential(PWR_Rng* r);

/** Chi-square with 2k degrees of freedom normalised to unit mean.  k == 1
 *  reproduces the exponential, k == 2 the Swerling 3/4 distribution. */
double   pwr_rng_chi2_norm(PWR_Rng* r, uint32_t k);

/** Zero-mean circular complex Gaussian with total power @p power
 *  (i.e. E[|z|^2] == power). */
PWR_Complex pwr_rng_cgauss(PWR_Rng* r, double power);

/** Random unit-modulus phasor. */
PWR_Complex pwr_rng_phasor(PWR_Rng* r);

/* --------------------------------------------------------------------------
 *  Order statistics (OS-CFAR / trimmed mean)
 * ------------------------------------------------------------------------ */

/** In-place ascending sort of a small float array (insertion sort: the CFAR
 *  windows are 8..64 cells where insertion sort beats quicksort). */
void  pwr_sort_f32(pwr_real* a, uint32_t n);

/** Returns the k-th smallest element (k is 0-based) using quickselect.
 *  @p a is permuted. */
pwr_real pwr_select_kth_f32(pwr_real* a, uint32_t n, uint32_t k);

/* --------------------------------------------------------------------------
 *  Small dense linear algebra for the tracker (4-state CV Kalman filter)
 * ------------------------------------------------------------------------ */

/** C = A*B for square matrices of order n (n <= 4). */
void pwr_mat_mul(const double* A, const double* B, double* C, uint32_t n);

/** C = A*B' */
void pwr_mat_mul_t(const double* A, const double* B, double* C, uint32_t n);

/** In-place 2x2 inverse.  Returns PWR_ERR_NUMERIC on a singular matrix. */
PWR_Status pwr_mat2_inv(const double* A, double* out);

/** Symmetrises M in place: M = (M + M')/2.  Guards against covariance drift. */
void pwr_mat_symmetrise(double* M, uint32_t n);

/** Quadratic form d' * Ainv * d for n == 2. */
double pwr_mahalanobis2(const double* Ainv, const double* d);

/* --------------------------------------------------------------------------
 *  Misc numeric utilities
 * ------------------------------------------------------------------------ */

/** Smallest power of two >= v. */
uint32_t pwr_pow2_ceil(uint32_t v);

/** 1 when v is a power of two and non-zero. */
static PWR_INLINE int pwr_is_pow2(uint32_t v)
{
    return (v != 0u) && ((v & (v - 1u)) == 0u);
}

/** Parabolic (3-point) interpolation of a peak located at index 1 of
 *  {ym1, y0, yp1}.  Returns the sub-sample offset in [-0.5, +0.5]. */
static PWR_INLINE double pwr_parabolic_peak(double ym1, double y0, double yp1)
{
    const double denom = (ym1 - 2.0 * y0 + yp1);
    if (fabs(denom) < 1e-12) { return 0.0; }
    return pwr_clampd(0.5 * (ym1 - yp1) / denom, -0.5, 0.5);
}

/** Inverse complementary error function, used to convert a design Pfa into a
 *  Gaussian threshold when a square-law detector is not assumed. */
double pwr_erfc_inv(double p);

/** Modified Bessel function of the first kind, order 0 (Kaiser window). */
double pwr_bessel_i0(double x);

/** Exponentially weighted moving average update, tau in samples. */
static PWR_INLINE double pwr_ewma(double prev, double sample, double alpha)
{
    return prev + alpha * (sample - prev);
}

#endif /* PWRADAR_PWR_MATH_H */
