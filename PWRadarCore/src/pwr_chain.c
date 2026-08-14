/* The coherent chain: pulse compression -> MTI -> Doppler filter bank ->
 * display products.
 *
 * The simulator injects thermal noise with variance exactly 1.0 per complex
 * sample and this chain carries that reference forward, so the published
 * range-Doppler map is already in dB of SNR: the noise floor sits at 0 dB and
 * a target reading +18 dB really has 18 dB post-integration. Nothing
 * downstream has to rescale, and the display doubles as an SNR meter.
 *
 * Two constants hold that up:
 *   sigma_pc     = sqrt(ENBW(range window) / Ntx)     after compression
 *   doppler_norm = 1 / (sigma_pc * sqrt(sum w_d^2))   after the FFT
 */
#include "pwr_core.h"

#include <string.h>

/* ==========================================================================
 *  1.  Pulse compression
 * ==========================================================================
 *  Frequency-domain correlation.  Output index == echo delay in samples, so
 *  bin i maps to range_first_m + i * range_step_m with no offset correction.
 * ------------------------------------------------------------------------ */
void pwr_chain_pulse_compress(struct PWR_Engine* e)
{
    const uint32_t n_pulses  = e->n_pulses;
    const uint32_t n_samples = e->n_samples;
    const uint32_t n_range   = e->n_range;
    const uint32_t n_fft     = e->n_fast_fft;
    const uint32_t decim     = e->range_decim;
    const uint32_t offset    = e->range_offset;
    uint32_t p, i;

    for (p = 0u; p < n_pulses; ++p)
    {
        const PWR_Complex* PWR_RESTRICT src = &e->rx[(size_t)p * n_samples];
        PWR_Complex*       PWR_RESTRICT dst = &e->pc[(size_t)p * n_range];

        if (e->cfg.enable_pulse_compression != 0)
        {
            PWR_Complex* PWR_RESTRICT w = e->fast_scratch;
            /* Only the gated window can reach a displayed bin; the rest of
             * the transform stays zero padding for the circular wrap. */
            const uint32_t n_copy = e->fast_copy;

            memcpy(w, src, (size_t)n_copy * sizeof(PWR_Complex));
            memset(w + n_copy, 0,
                   (size_t)(n_fft - n_copy) * sizeof(PWR_Complex));

            pwr_fft_run(e->plan_fast, w);
            for (i = 0u; i < n_fft; ++i)
            {
                w[i] = pwr_cmul(w[i], e->wf.mf_spectrum[i]);
            }
            pwr_ifft_run(e->plan_fast, w);

            for (i = 0u; i < n_range; ++i)
            {
                dst[i] = w[offset + i * decim];
            }
        }
        else
        {
            /* Bypass path: raw video, decimated only.  sigma_pc is set to 1
             * in this mode so the dB scale stays consistent. */
            for (i = 0u; i < n_range; ++i)
            {
                dst[i] = src[offset + i * decim];
            }
        }

        /* Sensitivity time control is deliberately NOT applied here.  An
         * analogue STC ramp sits ahead of the receiver, so it attenuates the
         * echo and the surface clutter arriving from a gate but never the
         * receiver's own thermal noise.  Applying it to the compressed cube
         * would hit the noise as well - and, because distributed clutter is
         * injected after this stage, would miss the clutter entirely, which
         * is the one signal the control exists to suppress.  The ramp is
         * charged at the two places the arriving energy is synthesised:
         * pwr_sim_generate_cpi() for echoes and pwr_sim_add_clutter() for the
         * distributed field. */
    }
}

/* ==========================================================================
 *  1b.  Raw-video trace
 * ==========================================================================
 *  Incoherent average across the pulse dimension - the pre-MTI "raw video"
 *  A-scope trace.  It is a separate pass rather than a few lines inside the
 *  compression loop because it has to run *after* pwr_sim_add_clutter(): the
 *  distributed clutter field is injected into the compressed cube, so a trace
 *  accumulated during compression would show none of the clutter, which is the
 *  one thing that trace is read for - it is what the MTI comparison on the
 *  A-scope is against.
 * ------------------------------------------------------------------------ */
void pwr_chain_raw_profile(struct PWR_Engine* e)
{
    const uint32_t n_pulses = e->n_pulses;
    const uint32_t n_range  = e->n_range;
    const double   inv_np   = 1.0 / (double)n_pulses;
    const double   inv_var  = 1.0 / (e->sigma_pc * e->sigma_pc);
    uint32_t p, i;

    memset(e->profile_raw, 0, (size_t)n_range * sizeof(pwr_real));
    for (p = 0u; p < n_pulses; ++p)
    {
        const PWR_Complex* PWR_RESTRICT src = &e->pc[(size_t)p * n_range];
        for (i = 0u; i < n_range; ++i)
        {
            e->profile_raw[i] += (pwr_real)(pwr_cabs2(src[i]) * inv_np * inv_var);
        }
    }
}

/* ==========================================================================
 *  2.  MTI - binomial delay-line cancellers across the pulse dimension
 * ==========================================================================
 *  A canceller of order K consumes K pulses, therefore only N-K pulses remain
 *  coherently usable.  They are written back starting at index 0 and
 *  e->n_pulses_valid is reduced accordingly, so the Doppler stage always sees
 *  a contiguous, fully valid slow-time record - no zero-fill discontinuity.
 *
 *  Coefficients are normalised by 1/sqrt(sum c_i^2) so that the noise floor,
 *  and hence the whole dB calibration, is unchanged by enabling MTI.
 * ------------------------------------------------------------------------ */
void pwr_chain_mti(struct PWR_Engine* e)
{
    const uint32_t n_pulses = e->n_pulses;
    const uint32_t n_range  = e->n_range;
    static const double coeff_tab[5][4] = {
        { 1.0,  0.0,  0.0,  0.0 },  /* unused (OFF)        */
        { 1.0,  0.0,  0.0,  0.0 },  /* unused (DC removal) */
        { 1.0, -1.0,  0.0,  0.0 },  /* 2-pulse             */
        { 1.0, -2.0,  1.0,  0.0 },  /* 3-pulse             */
        { 1.0, -3.0,  3.0, -1.0 }   /* 4-pulse             */
    };
    uint32_t p, i, order;
    double   norm;

    e->n_pulses_valid = n_pulses;
    e->mti_offset     = 0u;

    switch (e->cfg.mti_mode)
    {
    case PWR_MTI_OFF:
        return;

    case PWR_MTI_DC_REMOVAL:
        /* Remove the slow-time mean: a zero-Doppler notch that costs no
         * pulses.  Noise gain is (1 - 1/N), close enough to unity that the
         * calibration is left untouched. */
        for (i = 0u; i < n_range; ++i)
        {
            PWR_Complex mean = pwr_c(0.0f, 0.0f);
            for (p = 0u; p < n_pulses; ++p)
            {
                mean = pwr_cadd(mean, e->pc[(size_t)p * n_range + i]);
            }
            mean = pwr_cscale(mean, (pwr_real)(1.0 / (double)n_pulses));
            for (p = 0u; p < n_pulses; ++p)
            {
                PWR_Complex* z = &e->pc[(size_t)p * n_range + i];
                *z = pwr_csub(*z, mean);
            }
        }
        return;

    case PWR_MTI_TWO_PULSE:   order = 1u; break;
    case PWR_MTI_THREE_PULSE: order = 2u; break;
    case PWR_MTI_FOUR_PULSE:  order = 3u; break;
    default:                  return;
    }

    if (n_pulses <= order) { return; }

    {
        const double* c = coeff_tab[order + 1u];
        double s2 = 0.0;
        uint32_t t;
        for (t = 0u; t <= order; ++t) { s2 += c[t] * c[t]; }
        norm = (s2 > 0.0) ? (1.0 / sqrt(s2)) : 1.0;

        /* Forward sweep: y[p-order] = sum_t c[t] * x[p-t].  Because the output
         * index is always strictly below the lowest input index still needed,
         * the transform is safe in place. */
        for (p = order; p < n_pulses; ++p)
        {
            PWR_Complex* PWR_RESTRICT out = &e->pc[(size_t)(p - order) * n_range];
            for (i = 0u; i < n_range; ++i)
            {
                double ar = 0.0, ai = 0.0;
                for (t = 0u; t <= order; ++t)
                {
                    const PWR_Complex z = e->pc[(size_t)(p - t) * n_range + i];
                    ar += c[t] * (double)z.re;
                    ai += c[t] * (double)z.im;
                }
                out[i] = pwr_c((pwr_real)(ar * norm), (pwr_real)(ai * norm));
            }
        }
    }
    e->n_pulses_valid = n_pulses - order;
}

/* ==========================================================================
 *  3.  Doppler filter bank
 * ==========================================================================
 *  Slow-time transpose, amplitude taper, FFT, then a transposing write into
 *  the display-ordered power map.
 *
 *  Axis order.  The published Doppler axis increases with range rate, so
 *  column 0 is the fastest closing target.  Output row j therefore reads the
 *  signed Doppler index m = n/2 - 1 - j, i.e. FFT bin
 *          k = (n/2 - 1 - j + n) mod n
 *  and the corresponding range rate is
 *          rdot(j) = lambda * PRF / (2n) * (j - n/2 + 1).
 * ------------------------------------------------------------------------ */
void pwr_chain_doppler(struct PWR_Engine* e)
{
    const uint32_t n_range   = e->n_range;
    const uint32_t n_dop     = e->n_doppler;
    const uint32_t n_valid   = e->n_pulses_valid;
    const double   gamma     = e->doppler_norm;
    uint32_t r, m, j;

    if (e->cfg.enable_doppler_processing == 0 || n_valid < 2u)
    {
        /* Non-coherent fallback: replicate the incoherent pulse average across
         * every Doppler row so the map and the A-scope stay usable. */
        const double inv_var = 1.0 / (e->sigma_pc * e->sigma_pc);
        const double inv_np  = 1.0 / (double)pwr_maxu(n_valid, 1u);
        for (r = 0u; r < n_range; ++r)
        {
            double acc = 0.0;
            for (m = 0u; m < n_valid; ++m)
            {
                acc += (double)pwr_cabs2(e->pc[(size_t)m * n_range + r]);
            }
            acc *= inv_np * inv_var;
            for (j = 0u; j < n_dop; ++j)
            {
                e->rd_pow[(size_t)j * n_range + r] = (pwr_real)acc;
            }
        }
        return;
    }

    /* ---- transpose + taper + zero pad ---------------------------------- */
    for (r = 0u; r < n_range; ++r)
    {
        PWR_Complex* PWR_RESTRICT row = &e->slow[(size_t)r * n_dop];
        for (m = 0u; m < n_valid; ++m)
        {
            row[m] = pwr_cscale(e->pc[(size_t)m * n_range + r],
                                e->doppler_window[m]);
        }
        if (n_dop > n_valid)
        {
            memset(row + n_valid, 0,
                   (size_t)(n_dop - n_valid) * sizeof(PWR_Complex));
        }
    }

    /* ---- FFT per range gate -------------------------------------------- */
    for (r = 0u; r < n_range; ++r)
    {
        pwr_fft_run(e->plan_slow, &e->slow[(size_t)r * n_dop]);
    }

    /* ---- transposing write into the display-ordered power map ----------- */
    {
        const double g2 = gamma * gamma;
        const int32_t nd = (int32_t)n_dop;
        for (j = 0u; j < n_dop; ++j)
        {
            const int32_t k = (nd / 2 - 1 - (int32_t)j + nd) % nd;
            pwr_real* PWR_RESTRICT out = &e->rd_pow[(size_t)j * n_range];
            for (r = 0u; r < n_range; ++r)
            {
                const PWR_Complex z = e->slow[(size_t)r * n_dop + (uint32_t)k];
                out[r] = (pwr_real)((double)pwr_cabs2(z) * g2);
            }
        }
    }
}

/* ==========================================================================
 *  4.  Display products
 * ========================================================================== */

/* Robust noise-floor estimate: median of a strided subsample of the map,
 * which is insensitive to the targets and to clutter ridges. */
static double pwr_estimate_noise_floor(struct PWR_Engine* e)
{
    const uint32_t n_range = e->n_range;
    const uint32_t n_dop   = e->n_doppler;
    const uint32_t total   = n_range * n_dop;
    const uint32_t want    = 2048u;
    const uint32_t stride  = pwr_maxu(1u, total / want);
    uint32_t count = 0u, i;
    pwr_real* buf = e->cfar.train_scratch;

    if (buf == NULL || e->cfar.train_capacity < 16u) { return 1.0; }

    for (i = 0u; i < total && count < e->cfar.train_capacity; i += stride)
    {
        buf[count++] = e->rd_pow[i];
    }
    if (count < 8u) { return 1.0; }
    /* The cell powers are exponentially distributed, whose median is
     * ln(2) times the mean.  Dividing by ln(2) turns the (target- and
     * clutter-immune) median into an unbiased mean estimate, which is what the
     * dB calibration is referenced to - so a clean map reads 0.0 dB. */
    return (double)pwr_select_kth_f32(buf, count, count / 2u) / 0.6931471805599453;
}

void pwr_chain_products(struct PWR_Engine* e)
{
    const uint32_t n_range = e->n_range;
    const uint32_t n_dop   = e->n_doppler;
    uint32_t r, j;

    /* ---- range profile: peak over Doppler (classic max-hold A-scope) ---- */
    for (r = 0u; r < n_range; ++r)
    {
        e->profile_pow[r]    = 0.0f;
        e->profile_peak_j[r] = 0u;
    }
    for (j = 0u; j < n_dop; ++j)
    {
        const pwr_real* PWR_RESTRICT row = &e->rd_pow[(size_t)j * n_range];
        for (r = 0u; r < n_range; ++r)
        {
            if (row[r] > e->profile_pow[r])
            {
                e->profile_pow[r] = row[r];
                /* Argmax row for the CFAR threshold trace: the trace reports
                 * the threshold in the cell the A-scope actually shows. */
                e->profile_peak_j[r] = j;
            }
        }
    }

    /* ---- dB conversion of the whole map -------------------------------- */
    {
        const uint32_t total = n_range * n_dop;
        uint32_t i;
        for (i = 0u; i < total; ++i)
        {
            e->rd_db[i] = pwr_pow_to_db(e->rd_pow[i]);
        }
    }

    e->stats.measured_noise_floor_db =
        (double)pwr_pow_to_db((pwr_real)pwr_estimate_noise_floor(e));
}

/* ==========================================================================
 *  5.  PPI and RTI accumulators
 * ========================================================================== */
void pwr_display_update_ppi(struct PWR_Engine* e)
{
    const uint32_t n_range = e->n_range;
    const uint32_t cells   = e->ppi_cells;
    const double   az_step = 360.0 / (double)cells;
    const double   half_bw = 0.5 * e->cfg.azimuth_beamwidth_deg;
    /* Paint every azimuth cell the beam actually illuminated this CPI: the
     * mainlobe half-width, and never fewer than one cell. */
    const int32_t  span    = pwr_maxi(0, (int32_t)(half_bw / az_step));
    const int32_t  centre  = (int32_t)(pwr_wrap360(e->beam_azimuth_deg) / az_step + 0.5);
    /* Video is mapped over a fixed 60 dB window above the noise floor, which is
     * what a real PPI's logarithmic receiver does. */
    const double   lo_db   = -6.0;
    const double   scale   = 65535.0 / 60.0;
    int32_t k;

    if (e->ppi_accum == NULL) { return; }

    /* Exponential persistence in Q16.  The decay pass touches every cell of
     * the accumulator, so it is batched to every fourth CPI - but only when
     * the persistence constant spans at least 32 CPIs, which bounds one
     * batched step (and the transient over- or under-brightness a cell can
     * carry either side of it) to a fraction of a decibel.  Shorter
     * constants decay every CPI, where the pass is cheap relative to the
     * CPI budget anyway. */
    if (e->cfg.ppi_persistence_s > 0.0)
    {
        const uint32_t batch =
            (e->cfg.ppi_persistence_s >= 32.0 * e->dm.cpi_duration_s) ? 4u : 1u;
        ++e->ppi_decay_pending;
        if (e->ppi_decay_pending >= batch)
        {
            const double decay = exp(-(double)e->ppi_decay_pending *
                                     e->dm.cpi_duration_s /
                                     e->cfg.ppi_persistence_s);
            const uint32_t f = (uint32_t)pwr_clampd(decay * 65536.0 + 0.5,
                                                    0.0, 65536.0);
            const size_t total = (size_t)cells * n_range;
            size_t i;
            for (i = 0u; i < total; ++i)
            {
                e->ppi_accum[i] = (uint16_t)(((uint32_t)e->ppi_accum[i] * f) >> 16u);
            }
            e->ppi_decay_pending = 0u;
        }
    }

    for (k = -span; k <= span; ++k)
    {
        const int32_t idx = (int32_t)(((centre + k) % (int32_t)cells + (int32_t)cells)
                                     % (int32_t)cells);
        uint16_t* PWR_RESTRICT dst = &e->ppi_accum[(size_t)idx * n_range];
        /* Pattern weighting across the painted arc keeps the beam shape. */
        const double off = (double)k * az_step;
        const double wgt = exp(-2.7726 * (off / e->cfg.azimuth_beamwidth_deg) *
                                        (off / e->cfg.azimuth_beamwidth_deg));
        uint32_t r;
        for (r = 0u; r < n_range; ++r)
        {
            const double db = (double)pwr_pow_to_db(e->profile_pow[r]);
            double v = (db - lo_db) * scale * wgt;
            uint16_t u16;
            if (v < 0.0)          { v = 0.0; }
            else if (v > 65535.0) { v = 65535.0; }
            u16 = (uint16_t)(v + 0.5);
            if (u16 > dst[r]) { dst[r] = u16; }
        }
    }
}

void pwr_display_update_rti(struct PWR_Engine* e)
{
    const uint32_t n_range = e->n_range;
    if (e->rti == NULL || e->rti_rows == 0u) { return; }
    e->rti_head = (e->rti_head + 1u) % e->rti_rows;
    {
        pwr_real* PWR_RESTRICT row = &e->rti[(size_t)e->rti_head * n_range];
        uint32_t r;
        for (r = 0u; r < n_range; ++r)
        {
            row[r] = pwr_pow_to_db(e->profile_pow[r]);
        }
    }
}
