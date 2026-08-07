/* ==========================================================================
 *  PWRadarSystem - PWRadarCore (internal)
 *  File    : pwr_fft.h
 *  Purpose : Self-contained power-of-two complex FFT.
 *
 *            Implementation notes
 *            --------------------
 *            * Decimation-in-time Cooley-Tukey, radix-4 for the even part of
 *              log2(n) with a single radix-2 stage when log2(n) is odd. This
 *              yields ~25% fewer real multiplies than pure radix-2.
 *            * Twiddle factors and the digit-reversal permutation are
 *              precomputed once per plan; the transform itself performs no
 *              allocation and no trigonometry.
 *            * A plan is immutable after creation, therefore several worker
 *              threads may share one plan concurrently as long as each uses
 *              its own data buffer.
 *  Language: ISO C17
 * ========================================================================== */
#ifndef PWRADAR_PWR_FFT_H
#define PWRADAR_PWR_FFT_H

#include "pwradar/pwr_types.h"
#include "pwr_math.h"

typedef struct PWR_FftPlan
{
    uint32_t     n;             /* transform length, power of two            */
    uint32_t     log2n;
    uint32_t*    rev;           /* [n]   bit-reversal permutation            */
    PWR_Complex* tw;            /* [n/2] e^(-j*2*pi*k/n), k = 0..n/2-1       */
} PWR_FftPlan;

/** Creates a plan for length @p n (must be a power of two, 2 <= n <= 2^22). */
PWR_Status pwr_fft_plan_create(PWR_FftPlan** out, uint32_t n);
void       pwr_fft_plan_destroy(PWR_FftPlan* p);

/** In-place forward transform: X[k] = sum x[m] e^(-j2*pi*km/n). */
void pwr_fft_run(const PWR_FftPlan* p, PWR_Complex* data);

/** In-place inverse transform, normalised by 1/n. */
void pwr_ifft_run(const PWR_FftPlan* p, PWR_Complex* data);

/** Rotates a spectrum so that DC lands in the centre (MATLAB fftshift).
 *  @p n must be even; works in place with a single temporary swap. */
void pwr_fftshift(PWR_Complex* data, uint32_t n);

/** fftshift for a real array (used on the dB-scaled Doppler axis). */
void pwr_fftshift_real(pwr_real* data, uint32_t n);

#endif /* PWRADAR_PWR_FFT_H */
