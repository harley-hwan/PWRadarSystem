/* ==========================================================================
 *  PWRadarSystem - PWRadarCore (internal)
 *  ------------------------------------------------------------------------
 *  File    : pwr_waveform.c
 *  Purpose : Linear-FM transmit waveform and the range-compression filter.
 *
 *  Waveform
 *  --------
 *      s(t) = exp( j * pi * K * t^2 ),  K = B / Tp,  |t| <= Tp/2
 *  sampled as   tx[m] = s( (m - (Ntx-1)/2) / fs ),  m = 0 .. Ntx-1
 *
 *  Compression filter
 *  ------------------
 *  Compression is a correlation, realised in the frequency domain, so that the
 *  output index equals the echo delay in samples exactly and
 *          range(i) = c * i / (2*fs)
 *  needs no group-delay correction anywhere else in the chain.
 *
 *  Weighting is applied in the *frequency* domain with spectral equalisation:
 *
 *      H(f) = conj(TX(f)) * W(f) / ( |TX(f)|^2 + eps )
 *
 *  rather than by tapering the time-domain replica.  This matters, and it is
 *  what real pulse compressors do.  Tapering the replica in time relies on the
 *  stationary-phase equivalence between time and frequency across the chirp,
 *  whose error scales as 1/sqrt(Tp*B).  At the time-bandwidth products a
 *  surveillance radar actually uses (100 is typical) the residual Fresnel
 *  ripple of the LFM spectrum leaves a flat pedestal of paired echoes near
 *  -40 dBc spread over the whole +/-Tp support - far above the level the
 *  chosen taper nominally promises, and quite strong enough to raise phantom
 *  detections a full uncompressed pulse length either side of a large ship.
 *  Equalising |TX(f)| removes the ripple, so the achieved sidelobe level is
 *  the designed one.  eps caps the equalisation dynamic range and hence the
 *  noise amplification; the resulting mismatch loss is measured and reported.
 *
 *  Everything the rest of the chain needs is *measured* here rather than
 *  assumed: the filter is normalised so a unit-amplitude echo produces a unit
 *  compressed peak, and noise_gain is then the exact amplitude gain applied to
 *  unit-variance white input, i.e. the sigma_pc the dB calibration uses.
 *
 *  Language: ISO C17
 * ========================================================================== */
#include "pwr_core.h"

#include <string.h>

/* Equalisation floor as a fraction of the peak |TX(f)|^2.  0.02 keeps the
 * mismatch loss around a tenth of a dB while still flattening the in-band
 * ripple by better than 15 dB. */
#define PWR_EQ_EPSILON      0.02
/* Length of the spectral taper prototype that is interpolated across the band. */
#define PWR_TAPER_POINTS    2048u

/* Linear interpolation of the taper prototype at u in [-1, +1]. */
static double pwr_taper_at(const pwr_real* w, uint32_t n, double u)
{
    double x;
    uint32_t i0;
    double fr;
    if (u < -1.0 || u > 1.0) { return 0.0; }
    x   = 0.5 * (u + 1.0) * (double)(n - 1u);
    i0  = (uint32_t)x;
    if (i0 >= n - 1u) { return (double)w[n - 1u]; }
    fr = x - (double)i0;
    return (1.0 - fr) * (double)w[i0] + fr * (double)w[i0 + 1u];
}

PWR_Status pwr_waveform_build(PWR_Waveform* wf,
                              const PWR_RadarConfig* cfg,
                              const PWR_DerivedMetrics* dm,
                              const PWR_FftPlan* fast_plan)
{
    uint32_t m, n_tx, n_fft;
    double   K, fs, centre, maxp, eps, g_sig, sum_h2;
    PWR_Complex* txs = NULL;      /* FFT(tx), kept for the measurements */
    PWR_Complex* tmp = NULL;

    if (wf == NULL || cfg == NULL || dm == NULL || fast_plan == NULL)
    {
        return PWR_ERR_NULL_POINTER;
    }
    pwr_waveform_release(wf);

    fs   = cfg->sample_rate_hz;
    n_tx = (uint32_t)(cfg->pulse_width_s * fs + 0.5);
    if (n_tx < 2u) { n_tx = 2u; }
    n_fft = fast_plan->n;
    if (n_tx >= n_fft) { return PWR_ERR_INVALID_ARGUMENT; }

    wf->n_tx  = n_tx;
    wf->n_fft = n_fft;
    wf->n_win = PWR_TAPER_POINTS;
    wf->tx           = PWR_ALLOC_ARRAY(PWR_Complex, n_tx);
    wf->range_window = PWR_ALLOC_ARRAY(pwr_real,    PWR_TAPER_POINTS);
    wf->mf_spectrum  = PWR_ALLOC_ARRAY(PWR_Complex, n_fft);
    txs              = PWR_ALLOC_ARRAY(PWR_Complex, n_fft);
    tmp              = PWR_ALLOC_ARRAY(PWR_Complex, n_fft);
    if (wf->tx == NULL || wf->range_window == NULL ||
        wf->mf_spectrum == NULL || txs == NULL || tmp == NULL)
    {
        PWR_FREE(txs);
        PWR_FREE(tmp);
        pwr_waveform_release(wf);
        return PWR_ERR_OUT_OF_MEMORY;
    }

    /* ---- 1. chirp ------------------------------------------------------- */
    K      = cfg->bandwidth_hz / cfg->pulse_width_s;
    centre = 0.5 * (double)(n_tx - 1u);
    for (m = 0u; m < n_tx; ++m)
    {
        const double t = ((double)m - centre) / fs;
        wf->tx[m] = pwr_cexpj(PWR_PI * K * t * t);
    }

    /* ---- 2. spectral taper prototype ------------------------------------ */
    if (pwr_window_generate((PWR_WindowType)cfg->range_window,
                            wf->range_window, PWR_TAPER_POINTS) != PWR_STATUS_OK)
    {
        PWR_FREE(txs);
        PWR_FREE(tmp);
        pwr_waveform_release(wf);
        return PWR_ERR_INVALID_ARGUMENT;
    }

    /* ---- 3. transmit spectrum ------------------------------------------- */
    memset(txs, 0, (size_t)n_fft * sizeof(PWR_Complex));
    for (m = 0u; m < n_tx; ++m) { txs[m] = wf->tx[m]; }
    pwr_fft_run(fast_plan, txs);

    maxp = 0.0;
    for (m = 0u; m < n_fft; ++m)
    {
        const double p = (double)pwr_cabs2(txs[m]);
        if (p > maxp) { maxp = p; }
    }
    eps = PWR_EQ_EPSILON * pwr_maxd(maxp, 1.0e-30);

    /* ---- 4. equalised, weighted compression filter ---------------------- */
    for (m = 0u; m < n_fft; ++m)
    {
        /* Signed baseband frequency of bin m. */
        const double f = (m <= n_fft / 2u)
            ? ((double)m * fs / (double)n_fft)
            : (((double)m - (double)n_fft) * fs / (double)n_fft);
        const double u = 2.0 * f / cfg->bandwidth_hz;
        const double w = pwr_taper_at(wf->range_window, PWR_TAPER_POINTS, u);
        const double p = (double)pwr_cabs2(txs[m]);
        const double g = (w > 0.0) ? (w / (p + eps)) : 0.0;
        wf->mf_spectrum[m].re = (pwr_real)( (double)txs[m].re * g);
        wf->mf_spectrum[m].im = (pwr_real)(-(double)txs[m].im * g);
    }

    /* ---- 5. normalise for a unit compressed peak ------------------------ */
    for (m = 0u; m < n_fft; ++m)
    {
        tmp[m] = pwr_cmul(txs[m], wf->mf_spectrum[m]);
    }
    pwr_ifft_run(fast_plan, tmp);
    g_sig = (double)pwr_cabs(tmp[0]);
    if (!(g_sig > 0.0))
    {
        PWR_FREE(txs);
        PWR_FREE(tmp);
        pwr_waveform_release(wf);
        return PWR_ERR_NUMERIC;
    }
    for (m = 0u; m < n_fft; ++m)
    {
        wf->mf_spectrum[m] = pwr_cscale(wf->mf_spectrum[m],
                                        (pwr_real)(1.0 / g_sig));
    }

    /* ---- 6. measured noise gain: sigma_out = sigma_in * sqrt(sum |h|^2) --
     *  With the forward transform unnormalised and the inverse carrying 1/N,
     *  Parseval gives sum_m |h[m]|^2 = (1/N) sum_k |H[k]|^2.               */
    sum_h2 = 0.0;
    for (m = 0u; m < n_fft; ++m)
    {
        sum_h2 += (double)pwr_cabs2(wf->mf_spectrum[m]);
    }
    wf->noise_gain = sqrt(sum_h2 / (double)n_fft);

    /* Reference: an ideal matched filter weighted with the same taper would
     * reach sqrt(ENBW/Ntx).  The difference is the equalisation cost.      */
    {
        double enbw = pwr_window_enbw(wf->range_window, PWR_TAPER_POINTS);
        const double ideal = sqrt(enbw / (double)n_tx);
        wf->mismatch_loss_db = (ideal > 0.0 && wf->noise_gain > 0.0)
            ? 20.0 * log10(wf->noise_gain / ideal) : 0.0;
    }

    /* ---- 7. measured compressed response ------------------------------- */
    for (m = 0u; m < n_fft; ++m)
    {
        tmp[m] = pwr_cmul(txs[m], wf->mf_spectrum[m]);
    }
    pwr_ifft_run(fast_plan, tmp);
    {
        const double peak = (double)pwr_cabs(tmp[0]);
        const double half = peak * 0.70794578;      /* -3 dB in amplitude */
        double side = 0.0;
        uint32_t i, null_at = 1u;

        /* Walk out of the mainlobe until the response first rises again: that
         * is the first null, and everything past it is sidelobe. */
        while (null_at + 1u < n_fft / 2u &&
               (double)pwr_cabs(tmp[null_at + 1u]) < (double)pwr_cabs(tmp[null_at]))
        {
            ++null_at;
        }
        for (i = null_at; i < n_fft - null_at; ++i)
        {
            const double a = (double)pwr_cabs(tmp[i]);
            if (a > side) { side = a; }
        }
        wf->sidelobe_db = (peak > 0.0 && side > 0.0)
                        ? 20.0 * log10(side / peak) : -120.0;

        wf->mainlobe_bins = 1.0;
        for (i = 1u; i < n_fft / 2u; ++i)
        {
            if ((double)pwr_cabs(tmp[i]) < half)
            {
                wf->mainlobe_bins = 2.0 * (double)i;
                break;
            }
        }
    }

    PWR_FREE(txs);
    PWR_FREE(tmp);
    return PWR_STATUS_OK;
}

void pwr_waveform_release(PWR_Waveform* wf)
{
    if (wf == NULL) { return; }
    PWR_FREE(wf->tx);
    PWR_FREE(wf->mf_spectrum);
    PWR_FREE(wf->range_window);
    wf->n_tx  = 0u;
    wf->n_fft = 0u;
    wf->n_win = 0u;
    wf->noise_gain       = 1.0;
    wf->mismatch_loss_db = 0.0;
    wf->sidelobe_db      = 0.0;
    wf->mainlobe_bins    = 0.0;
}
