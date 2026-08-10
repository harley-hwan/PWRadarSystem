#include "pwr_math.h"

#include <string.h>

/* ==========================================================================
 *  Amplitude tapers
 * ========================================================================== */

/* Taylor n-bar weighting, computed from the closed-form Fbar coefficients.
 * nbar = 4 is the usual choice for pulse compression sidelobe control. */
static void pwr_window_taylor(pwr_real* dst, uint32_t n, double sll_db, uint32_t nbar)
{
    const double A  = acosh(pow(10.0, sll_db / 20.0)) / PWR_PI;
    const double A2 = A * A;
    double Fm[8];
    uint32_t m, i, k;

    if (nbar > 8u) { nbar = 8u; }
    if (nbar < 2u) { nbar = 2u; }

    /* sigma^2 scaling factor per Taylor's derivation. */
    {
        const double nb = (double)nbar;
        const double sigma2 = (nb * nb) / (A2 + (nb - 0.5) * (nb - 0.5));

        for (m = 1u; m <= nbar - 1u; ++m)
        {
            const double dm = (double)m;
            double num = 1.0, den = 1.0;
            for (k = 1u; k <= nbar - 1u; ++k)
            {
                const double dk = (double)k;
                num *= 1.0 - (dm * dm) / (sigma2 * (A2 + (dk - 0.5) * (dk - 0.5)));
                if (k != m)
                {
                    den *= 1.0 - (dm * dm) / (dk * dk);
                }
            }
            /* The k==m term of the denominator product is handled by the
             * standard (-1)^(m+1) / (2 * prod) closed form below. */
            Fm[m - 1u] = ((m & 1u) ? 1.0 : -1.0) * num / (2.0 * den);
        }
    }

    {
        double peak = 0.0;
        for (i = 0u; i < n; ++i)
        {
            const double x = (n > 1u) ? ((double)i / (double)(n - 1u) - 0.5) : 0.0;
            double w = 1.0;
            for (m = 1u; m <= nbar - 1u; ++m)
            {
                w += 2.0 * Fm[m - 1u] * cos(PWR_TWO_PI * (double)m * x);
            }
            if (w < 0.0) { w = 0.0; }
            dst[i] = (pwr_real)w;
            if (w > peak) { peak = w; }
        }
        /* Normalise to unit peak, matching the convention every other taper
         * here uses so that the coherent gain figures stay comparable. */
        if (peak > 0.0)
        {
            const pwr_real inv = (pwr_real)(1.0 / peak);
            for (i = 0u; i < n; ++i) { dst[i] *= inv; }
        }
    }
}

/* Dolph-Chebyshev via the frequency-domain definition, evaluated with the
 * inverse DFT of the Chebyshev polynomial samples. */
static void pwr_window_chebyshev(pwr_real* dst, uint32_t n, double sll_db)
{
    const double r    = pow(10.0, sll_db / 20.0);
    const double x0   = cosh(acosh(r) / (double)(n - 1u));
    const double N    = (double)n;
    uint32_t i, k;
    double peak = 0.0;

    if (n < 2u) { dst[0] = 1.0f; return; }

    for (i = 0u; i < n; ++i)
    {
        double sum = 0.0;
        for (k = 0u; k < n; ++k)
        {
            const double xk = x0 * cos(PWR_PI * ((double)k + 0.5) / N);
            double Tk;
            /* Chebyshev polynomial T_{n-1}(xk) with the standard branch split. */
            if (xk > 1.0)        { Tk = cosh((double)(n - 1u) * acosh(xk)); }
            else if (xk < -1.0)  { Tk = (((n - 1u) & 1u) ? -1.0 : 1.0) *
                                        cosh((double)(n - 1u) * acosh(-xk)); }
            else                 { Tk = cos((double)(n - 1u) * acos(xk)); }
            sum += Tk * cos(PWR_TWO_PI * (double)k * ((double)i / N -
                                                      0.5 + 0.5 / N));
        }
        dst[i] = (pwr_real)sum;
        if (fabs(sum) > peak) { peak = fabs(sum); }
    }
    if (peak > 0.0)
    {
        const pwr_real inv = (pwr_real)(1.0 / peak);
        for (i = 0u; i < n; ++i) { dst[i] *= inv; }
    }
}

PWR_Status pwr_window_generate(PWR_WindowType type, pwr_real* dst, uint32_t n)
{
    uint32_t i;
    double denom;

    if (dst == NULL) { return PWR_ERR_NULL_POINTER; }
    if (n == 0u)     { return PWR_ERR_INVALID_ARGUMENT; }
    if (n == 1u)     { dst[0] = 1.0f; return PWR_STATUS_OK; }

    denom = (double)(n - 1u);

    switch (type)
    {
    case PWR_WIN_RECTANGULAR:
        for (i = 0u; i < n; ++i) { dst[i] = 1.0f; }
        break;

    case PWR_WIN_HANN:
        for (i = 0u; i < n; ++i)
        {
            dst[i] = (pwr_real)(0.5 - 0.5 * cos(PWR_TWO_PI * (double)i / denom));
        }
        break;

    case PWR_WIN_HAMMING:
        for (i = 0u; i < n; ++i)
        {
            dst[i] = (pwr_real)(0.54 - 0.46 * cos(PWR_TWO_PI * (double)i / denom));
        }
        break;

    case PWR_WIN_BLACKMAN:
        for (i = 0u; i < n; ++i)
        {
            const double t = PWR_TWO_PI * (double)i / denom;
            dst[i] = (pwr_real)(0.42 - 0.5 * cos(t) + 0.08 * cos(2.0 * t));
        }
        break;

    case PWR_WIN_BLACKMAN_HARRIS:
        for (i = 0u; i < n; ++i)
        {
            const double t = PWR_TWO_PI * (double)i / denom;
            dst[i] = (pwr_real)(0.35875
                              - 0.48829 * cos(t)
                              + 0.14128 * cos(2.0 * t)
                              - 0.01168 * cos(3.0 * t));
        }
        break;

    case PWR_WIN_TAYLOR_35DB:
        pwr_window_taylor(dst, n, 35.0, 4u);
        break;

    case PWR_WIN_TAYLOR_50DB:
        pwr_window_taylor(dst, n, 50.0, 5u);
        break;

    case PWR_WIN_KAISER_B6:
        {
            const double beta = 6.0;
            const double i0b  = pwr_bessel_i0(beta);
            for (i = 0u; i < n; ++i)
            {
                const double r = 2.0 * (double)i / denom - 1.0;
                const double a = 1.0 - r * r;
                dst[i] = (pwr_real)(pwr_bessel_i0(beta * sqrt(a > 0.0 ? a : 0.0)) / i0b);
            }
        }
        break;

    case PWR_WIN_CHEBYSHEV_60DB:
        pwr_window_chebyshev(dst, n, 60.0);
        break;

    default:
        return PWR_ERR_INVALID_ARGUMENT;
    }

    /* An amplitude taper is non-negative by definition.  The three-term
     * cosine families evaluate to a few ulp below zero at the end points in
     * single precision, which would otherwise show up as a phase reversal in
     * the matched filter; clamp them away. */
    for (i = 0u; i < n; ++i)
    {
        if (!(dst[i] > 0.0f)) { dst[i] = 0.0f; }
    }
    return PWR_STATUS_OK;
}

double pwr_window_coherent_gain(const pwr_real* w, uint32_t n)
{
    double s = 0.0;
    uint32_t i;
    if (w == NULL || n == 0u) { return 1.0; }
    for (i = 0u; i < n; ++i) { s += (double)w[i]; }
    return s / (double)n;
}

double pwr_window_enbw(const pwr_real* w, uint32_t n)
{
    double s1 = 0.0, s2 = 0.0;
    uint32_t i;
    if (w == NULL || n == 0u) { return 1.0; }
    for (i = 0u; i < n; ++i)
    {
        s1 += (double)w[i];
        s2 += (double)w[i] * (double)w[i];
    }
    if (s1 == 0.0) { return 1.0; }
    return (double)n * s2 / (s1 * s1);
}

/* ==========================================================================
 *  PCG32
 * ========================================================================== */
#define PWR_PCG_MULT  6364136223846793005ULL

void pwr_rng_seed(PWR_Rng* r, uint64_t seed, uint64_t stream)
{
    if (r == NULL) { return; }
    r->state     = 0u;
    r->inc       = (stream << 1u) | 1u;
    r->has_spare = 0;
    r->spare     = 0.0;
    (void)pwr_rng_u32(r);
    r->state += seed;
    (void)pwr_rng_u32(r);
}

uint32_t pwr_rng_u32(PWR_Rng* r)
{
    const uint64_t old = r->state;
    uint32_t xorshifted, rot;
    r->state = old * PWR_PCG_MULT + r->inc;
    xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    rot        = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

double pwr_rng_uniform(PWR_Rng* r)
{
    /* 53-bit mantissa from two draws keeps the tail behaviour of the normal
     * generator clean. */
    const uint64_t hi = (uint64_t)(pwr_rng_u32(r) >> 5u);   /* 27 bits */
    const uint64_t lo = (uint64_t)(pwr_rng_u32(r) >> 6u);   /* 26 bits */
    return (double)((hi << 26u) | lo) * (1.0 / 9007199254740992.0);
}

double pwr_rng_uniform_pos(PWR_Rng* r)
{
    double u = pwr_rng_uniform(r);
    return (u <= 0.0) ? (1.0 / 9007199254740992.0) : u;
}

double pwr_rng_normal(PWR_Rng* r)
{
    double u, v, s;
    if (r->has_spare)
    {
        r->has_spare = 0;
        return r->spare;
    }
    do
    {
        u = 2.0 * pwr_rng_uniform(r) - 1.0;
        v = 2.0 * pwr_rng_uniform(r) - 1.0;
        s = u * u + v * v;
    } while (s >= 1.0 || s <= 0.0);
    s = sqrt(-2.0 * log(s) / s);
    r->spare     = v * s;
    r->has_spare = 1;
    return u * s;
}

double pwr_rng_exponential(PWR_Rng* r)
{
    return -log(pwr_rng_uniform_pos(r));
}

double pwr_rng_chi2_norm(PWR_Rng* r, uint32_t k)
{
    double acc = 0.0;
    uint32_t i;
    if (k == 0u) { return 1.0; }
    for (i = 0u; i < k; ++i) { acc += pwr_rng_exponential(r); }
    return acc / (double)k;
}

PWR_Complex pwr_rng_cgauss(PWR_Rng* r, double power)
{
    /* Split the requested total power equally between the I and Q channels. */
    const double sigma = sqrt(pwr_maxd(power, 0.0) * 0.5);
    return pwr_c((pwr_real)(sigma * pwr_rng_normal(r)),
                 (pwr_real)(sigma * pwr_rng_normal(r)));
}

PWR_Complex pwr_rng_phasor(PWR_Rng* r)
{
    return pwr_cexpj(PWR_TWO_PI * pwr_rng_uniform(r));
}

/* ==========================================================================
 *  Order statistics
 * ========================================================================== */
void pwr_sort_f32(pwr_real* a, uint32_t n)
{
    uint32_t i;
    if (a == NULL || n < 2u) { return; }
    for (i = 1u; i < n; ++i)
    {
        const pwr_real key = a[i];
        uint32_t j = i;
        while (j > 0u && a[j - 1u] > key)
        {
            a[j] = a[j - 1u];
            --j;
        }
        a[j] = key;
    }
}

pwr_real pwr_select_kth_f32(pwr_real* a, uint32_t n, uint32_t k)
{
    uint32_t lo = 0u, hi;
    if (a == NULL || n == 0u) { return 0.0f; }
    if (k >= n) { k = n - 1u; }
    hi = n - 1u;

    while (lo < hi)
    {
        /* Median-of-three pivot for robustness against sorted input. */
        const uint32_t mid = lo + ((hi - lo) >> 1u);
        pwr_real pivot;
        uint32_t i = lo, j = hi;

        if (a[mid] < a[lo]) { const pwr_real t = a[mid]; a[mid] = a[lo]; a[lo] = t; }
        if (a[hi] < a[lo])  { const pwr_real t = a[hi];  a[hi]  = a[lo]; a[lo] = t; }
        if (a[hi] < a[mid]) { const pwr_real t = a[hi];  a[hi]  = a[mid]; a[mid] = t; }
        pivot = a[mid];

        while (i <= j)
        {
            while (a[i] < pivot) { ++i; }
            while (a[j] > pivot) { if (j == 0u) { break; } --j; }
            if (i <= j)
            {
                const pwr_real t = a[i]; a[i] = a[j]; a[j] = t;
                ++i;
                if (j == 0u) { break; }
                --j;
            }
        }
        if (k <= j)      { hi = j; }
        else if (k >= i) { lo = i; }
        else             { return a[k]; }
    }
    return a[k];
}

/* ==========================================================================
 *  Small dense linear algebra
 * ========================================================================== */
void pwr_mat_mul(const double* A, const double* B, double* C, uint32_t n)
{
    uint32_t i, j, k;
    for (i = 0u; i < n; ++i)
    {
        for (j = 0u; j < n; ++j)
        {
            double s = 0.0;
            for (k = 0u; k < n; ++k) { s += A[i * n + k] * B[k * n + j]; }
            C[i * n + j] = s;
        }
    }
}

void pwr_mat_mul_t(const double* A, const double* B, double* C, uint32_t n)
{
    uint32_t i, j, k;
    for (i = 0u; i < n; ++i)
    {
        for (j = 0u; j < n; ++j)
        {
            double s = 0.0;
            for (k = 0u; k < n; ++k) { s += A[i * n + k] * B[j * n + k]; }
            C[i * n + j] = s;
        }
    }
}

PWR_Status pwr_mat2_inv(const double* A, double* out)
{
    const double det = A[0] * A[3] - A[1] * A[2];
    double inv;
    if (fabs(det) < 1e-18) { return PWR_ERR_NUMERIC; }
    inv = 1.0 / det;
    out[0] =  A[3] * inv;
    out[1] = -A[1] * inv;
    out[2] = -A[2] * inv;
    out[3] =  A[0] * inv;
    return PWR_STATUS_OK;
}

void pwr_mat_symmetrise(double* M, uint32_t n)
{
    uint32_t i, j;
    for (i = 0u; i < n; ++i)
    {
        for (j = i + 1u; j < n; ++j)
        {
            const double m = 0.5 * (M[i * n + j] + M[j * n + i]);
            M[i * n + j] = m;
            M[j * n + i] = m;
        }
    }
}

double pwr_mahalanobis2(const double* Ainv, const double* d)
{
    const double t0 = Ainv[0] * d[0] + Ainv[1] * d[1];
    const double t1 = Ainv[2] * d[0] + Ainv[3] * d[1];
    return d[0] * t0 + d[1] * t1;
}

/* ==========================================================================
 *  Misc numerics
 * ========================================================================== */
uint32_t pwr_pow2_ceil(uint32_t v)
{
    if (v <= 1u) { return 1u; }
    --v;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16;
    return v + 1u;
}

double pwr_bessel_i0(double x)
{
    /* Abramowitz & Stegun 9.8.1 / 9.8.2 - better than 2e-7 relative. */
    const double ax = fabs(x);
    if (ax < 3.75)
    {
        const double y = (x / 3.75) * (x / 3.75);
        return 1.0 + y * (3.5156229 + y * (3.0899424 + y * (1.2067492 +
               y * (0.2659732 + y * (0.0360768 + y * 0.0045813)))));
    }
    else
    {
        const double y = 3.75 / ax;
        return (exp(ax) / sqrt(ax)) *
               (0.39894228 + y * (0.01328592 + y * (0.00225319 +
                y * (-0.00157565 + y * (0.00916281 + y * (-0.02057706 +
                y * (0.02635537 + y * (-0.01647633 + y * 0.00392377))))))));
    }
}

double pwr_erfc_inv(double p)
{
    /* Newton refinement of a rational initial guess; p in (0,2). */
    double x, err, t;
    int i;
    if (p <= 0.0) { return  INFINITY; }
    if (p >= 2.0) { return -INFINITY; }

    t = (p < 1.0) ? p : (2.0 - p);
    t = sqrt(-2.0 * log(0.5 * t));
    x = -0.70711 * ((2.30753 + t * 0.27061) / (1.0 + t * (0.99229 + t * 0.04481)) - t);
    for (i = 0; i < 3; ++i)
    {
        err = erfc(x) - p;
        x  += err / (1.12837916709551257 * exp(-x * x) - x * err);
    }
    return (p < 1.0) ? x : -x;
}
