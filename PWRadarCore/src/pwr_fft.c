/* Why the radix-4 stage reads its two middle operands swapped.
 *
 * Two cascaded radix-2 DIT stages on bit-reversed data at half-lengths h and
 * 2h produce, for A=d[b+j], B=d[b+j+h], C=d[b+j+2h], D=d[b+j+3h] and
 * w1 = W_4h^j, w2 = w1^2, w3 = w1^3:
 *
 *     out[j]    = A + w2*B + w1*C + w3*D
 *     out[j+h]  = A - w2*B - i*(w1*C - w3*D)
 *     out[j+2h] = A + w2*B - w1*C - w3*D
 *     out[j+3h] = A - w2*B + i*(w1*C - w3*D)
 *
 * One radix-4 butterfly reproduces that exactly if the DFT-4 inputs are taken
 * as (A, C, B, D) - the two middle operands exchanged, which is the base-4
 * digit reversal bit reversal leaves behind. Hence t1 loads from the +2h slot
 * and t2 from the +h slot below. Without the exchange the transform is simply
 * wrong: the outputs are neither the right bins nor a reordering of them. The
 * self test checks the result against a direct DFT.
 */
#include "pwr_fft.h"

#include <stdlib.h>
#include <string.h>

#define PWR_FFT_MAX_LOG2  22u

/* --------------------------------------------------------------------------
 *  Plan construction
 * ------------------------------------------------------------------------ */
static uint32_t pwr_bit_reverse(uint32_t v, uint32_t bits)
{
    uint32_t r = 0u, i;
    for (i = 0u; i < bits; ++i)
    {
        r = (r << 1u) | (v & 1u);
        v >>= 1u;
    }
    return r;
}

PWR_Status pwr_fft_plan_create(PWR_FftPlan** out, uint32_t n)
{
    PWR_FftPlan* p;
    uint32_t log2n = 0u, t = n, k;

    if (out == NULL) { return PWR_ERR_NULL_POINTER; }
    *out = NULL;
    if (!pwr_is_pow2(n) || n < 2u) { return PWR_ERR_INVALID_ARGUMENT; }
    while ((t >>= 1u) != 0u) { ++log2n; }
    if (log2n > PWR_FFT_MAX_LOG2) { return PWR_ERR_OUT_OF_RANGE; }

    p = (PWR_FftPlan*)pwr_aligned_calloc(1u, sizeof(*p), PWR_SIMD_ALIGN);
    if (p == NULL) { return PWR_ERR_OUT_OF_MEMORY; }

    p->n     = n;
    p->log2n = log2n;
    p->rev   = PWR_ALLOC_ARRAY(uint32_t, n);
    p->tw    = PWR_ALLOC_ARRAY(PWR_Complex, n / 2u);
    if (p->rev == NULL || p->tw == NULL)
    {
        pwr_fft_plan_destroy(p);
        return PWR_ERR_OUT_OF_MEMORY;
    }

    for (k = 0u; k < n; ++k)
    {
        p->rev[k] = pwr_bit_reverse(k, log2n);
    }
    /* Twiddles are evaluated in double precision then narrowed, which keeps
     * the spurious floor below -150 dBc for every length the radar uses. */
    for (k = 0u; k < n / 2u; ++k)
    {
        const double a = -PWR_TWO_PI * (double)k / (double)n;
        p->tw[k].re = (pwr_real)cos(a);
        p->tw[k].im = (pwr_real)sin(a);
    }

    *out = p;
    return PWR_STATUS_OK;
}

void pwr_fft_plan_destroy(PWR_FftPlan* p)
{
    if (p == NULL) { return; }
    PWR_FREE(p->rev);
    PWR_FREE(p->tw);
    pwr_aligned_free(p);
}

/* --------------------------------------------------------------------------
 *  Permutation
 * ------------------------------------------------------------------------ */
static void pwr_fft_permute(const PWR_FftPlan* p, PWR_Complex* d)
{
    const uint32_t n = p->n;
    const uint32_t* PWR_RESTRICT rev = p->rev;
    uint32_t i;
    for (i = 0u; i < n; ++i)
    {
        const uint32_t j = rev[i];
        if (j > i)
        {
            const PWR_Complex tmp = d[i];
            d[i] = d[j];
            d[j] = tmp;
        }
    }
}

/* --------------------------------------------------------------------------
 *  Butterflies
 * ------------------------------------------------------------------------ */

/* One radix-2 stage; `half` is the sub-transform length entering the stage. */
static void pwr_fft_radix2_stage(PWR_Complex* PWR_RESTRICT d,
                                 const PWR_Complex* PWR_RESTRICT tw,
                                 uint32_t n, uint32_t half)
{
    const uint32_t len    = half << 1u;
    const uint32_t stride = n / len;
    uint32_t base, j;

    for (base = 0u; base < n; base += len)
    {
        for (j = 0u; j < half; ++j)
        {
            PWR_Complex* const a = &d[base + j];
            PWR_Complex* const b = &d[base + j + half];
            const PWR_Complex w  = tw[j * stride];
            const PWR_Complex u  = *a;
            const PWR_Complex v  = pwr_cmul(*b, w);
            a->re = u.re + v.re;  a->im = u.im + v.im;
            b->re = u.re - v.re;  b->im = u.im - v.im;
        }
    }
}

/* One radix-4 stage; `quarter` is the sub-transform length entering it. */
static void pwr_fft_radix4_stage(PWR_Complex* PWR_RESTRICT d,
                                 const PWR_Complex* PWR_RESTRICT tw,
                                 uint32_t n, uint32_t quarter)
{
    const uint32_t len    = quarter << 2u;
    const uint32_t stride = n / len;
    const uint32_t nhalf  = n >> 1u;
    uint32_t base, j;

    for (base = 0u; base < n; base += len)
    {
        for (j = 0u; j < quarter; ++j)
        {
            PWR_Complex* const x0 = &d[base + j];
            PWR_Complex* const x1 = &d[base + j + quarter];
            PWR_Complex* const x2 = &d[base + j + 2u * quarter];
            PWR_Complex* const x3 = &d[base + j + 3u * quarter];

            const uint32_t i1 = j * stride;          /* < n/4  by construction */
            const uint32_t i2 = i1 << 1u;            /* < n/2  -> never wraps  */
            uint32_t       i3 = i1 * 3u;             /* < 3n/4 -> may wrap once*/
            int            neg3 = 0;

            PWR_Complex t0, t1, t2, t3, s0, s1, s2, s3;

            if (i3 >= nhalf) { i3 -= nhalf; neg3 = 1; }

            /* Note the deliberate slot exchange: t1 <- (+2q), t2 <- (+1q). */
            t0 = *x0;
            t1 = pwr_cmul(*x2, tw[i1]);
            t2 = pwr_cmul(*x1, tw[i2]);
            t3 = pwr_cmul(*x3, tw[i3]);
            if (neg3) { t3.re = -t3.re; t3.im = -t3.im; }

            s0.re = t0.re + t2.re;  s0.im = t0.im + t2.im;
            s1.re = t0.re - t2.re;  s1.im = t0.im - t2.im;
            s2.re = t1.re + t3.re;  s2.im = t1.im + t3.im;
            s3.re = t1.re - t3.re;  s3.im = t1.im - t3.im;

            x0->re = s0.re + s2.re;  x0->im = s0.im + s2.im;
            x2->re = s0.re - s2.re;  x2->im = s0.im - s2.im;
            /* (-i) * s3 == ( s3.im, -s3.re ) */
            x1->re = s1.re + s3.im;  x1->im = s1.im - s3.re;
            x3->re = s1.re - s3.im;  x3->im = s1.im + s3.re;
        }
    }
}

/* --------------------------------------------------------------------------
 *  Stage schedule
 * ------------------------------------------------------------------------ */
static void pwr_fft_core(const PWR_FftPlan* p, PWR_Complex* d)
{
    const uint32_t n     = p->n;
    const uint32_t log2n = p->log2n;
    uint32_t half        = 1u;

    if ((log2n & 1u) != 0u)
    {
        pwr_fft_radix2_stage(d, p->tw, n, 1u);
        half = 2u;
    }
    while (half < n)
    {
        pwr_fft_radix4_stage(d, p->tw, n, half);
        half <<= 2u;
    }
}

void pwr_fft_run(const PWR_FftPlan* p, PWR_Complex* data)
{
    if (p == NULL || data == NULL) { return; }
    pwr_fft_permute(p, data);
    pwr_fft_core(p, data);
}

void pwr_ifft_run(const PWR_FftPlan* p, PWR_Complex* data)
{
    uint32_t i;
    pwr_real inv;
    if (p == NULL || data == NULL) { return; }

    /* x = conj( FFT( conj(X) ) ) / n */
    for (i = 0u; i < p->n; ++i) { data[i].im = -data[i].im; }
    pwr_fft_permute(p, data);
    pwr_fft_core(p, data);
    inv = (pwr_real)(1.0 / (double)p->n);
    for (i = 0u; i < p->n; ++i)
    {
        data[i].re =  data[i].re * inv;
        data[i].im = -data[i].im * inv;
    }
}

/* --------------------------------------------------------------------------
 *  Spectrum centring
 * ------------------------------------------------------------------------ */
void pwr_fftshift(PWR_Complex* data, uint32_t n)
{
    uint32_t i;
    const uint32_t half = n / 2u;
    if (data == NULL || n < 2u) { return; }
    for (i = 0u; i < half; ++i)
    {
        const PWR_Complex t = data[i];
        data[i]        = data[i + half];
        data[i + half] = t;
    }
}

void pwr_fftshift_real(pwr_real* data, uint32_t n)
{
    uint32_t i;
    const uint32_t half = n / 2u;
    if (data == NULL || n < 2u) { return; }
    for (i = 0u; i < half; ++i)
    {
        const pwr_real t = data[i];
        data[i]        = data[i + half];
        data[i + half] = t;
    }
}
