/* CFAR detection on the range-Doppler map, and the conversion of the resulting
 * cell mask into plot reports.
 *
 * Reference window: Hr = guard_range + train_range, Hd = guard_doppler +
 * train_doppler. Training cells are inside the outer rectangle and outside the
 * guard rectangle. The range axis clamps at the map edges; the Doppler axis
 * wraps, which is physically right because the spectrum is periodic in the PRF.
 *
 * CA/GOCA/SOCA use the full 2-D window, evaluated from per-row prefix sums
 * along range. OS/TM use a 1-D window along range only: a full 2-D ordered
 * statistic would need hundreds of samples ranked per cell, which no real-time
 * processor does, and range is where OS earns its keep anyway (closely spaced
 * targets).
 *
 * Threshold multipliers, square-law detector, exponential clutter+noise power:
 *   CA   alpha = N * (Pfa^(-1/N) - 1)
 *   OS   Pfa = prod_{i<k} (N-i)/(N-i+alpha), solved by bisection - exact for
 *        the k-th ranked cell
 *   TM   CA form on the count left after trimming
 *   GO   CA form on the half window at Pfa/2 (standard approximation)
 *   SO   CA form on the half window at Pfa (optimistic)
 */
#include "pwr_core.h"

#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 *  Threshold multipliers
 * ========================================================================== */
static double pwr_alpha_ca(uint32_t n, double pfa)
{
    if (n == 0u) { return 1.0e12; }
    return (double)n * (pow(pfa, -1.0 / (double)n) - 1.0);
}

static double pwr_os_pfa(double alpha, uint32_t n, uint32_t k)
{
    double p = 1.0;
    uint32_t i;
    for (i = 0u; i < k; ++i)
    {
        const double m = (double)(n - i);
        p *= m / (m + alpha);
    }
    return p;
}

static double pwr_alpha_os(uint32_t n, uint32_t k, double pfa)
{
    double lo = 0.0, hi = 1.0e6;
    int it;
    if (n == 0u || k == 0u || k > n) { return 1.0e12; }
    /* pwr_os_pfa is strictly decreasing in alpha, so plain bisection is both
     * safe and fast enough (40 iterations => 1e-6 relative). */
    while (pwr_os_pfa(hi, n, k) > pfa && hi < 1.0e12) { hi *= 4.0; }
    for (it = 0; it < 60; ++it)
    {
        const double mid = 0.5 * (lo + hi);
        if (pwr_os_pfa(mid, n, k) > pfa) { lo = mid; } else { hi = mid; }
    }
    return 0.5 * (lo + hi);
}

/* ==========================================================================
 *  Allocation
 * ========================================================================== */
PWR_Status pwr_cfar_alloc(PWR_CfarWork* w, uint32_t n_doppler, uint32_t n_range,
                          const PWR_CfarConfig* cfg)
{
    uint32_t train_cap;

    if (w == NULL || cfg == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_cfar_release(w);

    /* Enough room for the widest 1-D window plus the noise-floor subsample. */
    train_cap = (uint32_t)(2 * (cfg->train_range + cfg->guard_range) + 4);
    train_cap = pwr_maxu(train_cap, 4096u);

    w->hit           = PWR_ALLOC_ARRAY(uint8_t,  (size_t)n_doppler * n_range);
    w->threshold     = PWR_ALLOC_ARRAY(pwr_real, (size_t)n_doppler * n_range);
    w->integral      = PWR_ALLOC_ARRAY(double,   (size_t)n_doppler * (n_range + 1u));
    w->win_psum      = PWR_ALLOC_ARRAY(double,   (size_t)n_range + 1u);
    w->gband_psum    = PWR_ALLOC_ARRAY(double,   (size_t)n_range + 1u);
    w->train_scratch = PWR_ALLOC_ARRAY(pwr_real, train_cap);
    if (w->hit == NULL || w->threshold == NULL || w->integral == NULL ||
        w->win_psum == NULL || w->gband_psum == NULL ||
        w->train_scratch == NULL)
    {
        pwr_cfar_release(w);
        return PWR_ERR_OUT_OF_MEMORY;
    }
    w->train_capacity = train_cap;
    return PWR_STATUS_OK;
}

void pwr_cfar_release(PWR_CfarWork* w)
{
    if (w == NULL) { return; }
    PWR_FREE(w->hit);
    PWR_FREE(w->threshold);
    PWR_FREE(w->integral);
    PWR_FREE(w->win_psum);
    PWR_FREE(w->gband_psum);
    PWR_FREE(w->train_scratch);
    w->train_capacity = 0u;
    w->cells_tested   = 0u;
    w->cells_hit      = 0u;
}

/* ==========================================================================
 *  Detector
 * ==========================================================================
 *  Performance notes
 *  -----------------
 *   * The estimator family is dispatched once per Doppler row, not once per
 *     cell, and the wrapped Doppler row indices are computed once per row.
 *   * alpha depends only on the reference count, which takes only a handful of
 *     distinct values (it varies solely at the range edges), so it is memoised
 *     instead of calling pow() 64k times per CPI.
 * ------------------------------------------------------------------------ */
#define PWR_ALPHA_TAB_MAX 1024u
#define PWR_CFAR_MAX_HD   32

typedef struct PWR_AlphaCache
{
    double   pfa;
    double   val[PWR_ALPHA_TAB_MAX];
    uint8_t  set[PWR_ALPHA_TAB_MAX];
} PWR_AlphaCache;

static double pwr_alpha_cached(PWR_AlphaCache* ac, uint32_t n)
{
    if (n >= PWR_ALPHA_TAB_MAX) { return pwr_alpha_ca(n, ac->pfa); }
    if (ac->set[n] == 0u)
    {
        ac->val[n] = pwr_alpha_ca(n, ac->pfa);
        ac->set[n] = 1u;
    }
    return ac->val[n];
}

/* Local-maximum test inside the guard region.  Operates on the untouched power
 * map, so the result does not depend on the scan order. */
static void pwr_cfar_peak_select(struct PWR_Engine* e)
{
    const uint32_t nr = e->n_range;
    const uint32_t nd = e->n_doppler;
    const int32_t  Gr = pwr_maxi(1, e->cfg.cfar.guard_range);
    const int32_t  Gd = pwr_maxi(1, e->cfg.cfar.guard_doppler);
    uint32_t j, r;
    uint32_t removed = 0u;

    for (j = 0u; j < nd; ++j)
    {
        uint8_t* PWR_RESTRICT hit = &e->cfar.hit[(size_t)j * nr];
        for (r = 0u; r < nr; ++r)
        {
            const pwr_real cut = e->rd_pow[(size_t)j * nr + r];
            int32_t dj;
            int keep = 1;
            if (hit[r] == 0u) { continue; }
            for (dj = -Gd; dj <= Gd && keep != 0; ++dj)
            {
                const int32_t jj = (((int32_t)j + dj) % (int32_t)nd +
                                    (int32_t)nd) % (int32_t)nd;
                const pwr_real* PWR_RESTRICT row = &e->rd_pow[(size_t)jj * nr];
                int32_t dr;
                for (dr = -Gr; dr <= Gr; ++dr)
                {
                    const int32_t rr = (int32_t)r + dr;
                    if (rr < 0 || rr >= (int32_t)nr) { continue; }
                    if (dj == 0 && dr == 0) { continue; }
                    if (row[rr] > cut) { keep = 0; break; }
                }
            }
            if (keep == 0) { hit[r] = 0u; ++removed; }
        }
    }
    if (removed <= e->cfar.cells_hit) { e->cfar.cells_hit -= removed; }
}

void pwr_cfar_run(struct PWR_Engine* e)
{
    const PWR_CfarConfig* const cf = &e->cfg.cfar;
    const uint32_t nr = e->n_range;
    const uint32_t nd = e->n_doppler;
    const int32_t  Gr = cf->guard_range;
    const int32_t  Gd = cf->guard_doppler;
    const int32_t  Hr = cf->guard_range + cf->train_range;
    /* guard_doppler and train_doppler are each clamped to 16 by
     * pwr_config_clamp(), so Hd never exceeds PWR_CFAR_MAX_HD. */
    const int32_t  Hd = pwr_mini(cf->guard_doppler + cf->train_doppler,
                                 (int32_t)PWR_CFAR_MAX_HD);
    const double   bias = pwr_db_to_pow(cf->extra_threshold_db);
    const int32_t  zero_row = (int32_t)nd / 2 - 1;
    double* const  psum = e->cfar.integral;
    double* const  wsum = e->cfar.win_psum;
    double* const  gsum = e->cfar.gband_psum;
    const int      ca_family = (cf->type == PWR_CFAR_CA ||
                                cf->type == PWR_CFAR_GOCA ||
                                cf->type == PWR_CFAR_SOCA) ? 1 : 0;
    PWR_AlphaCache ac;
    uint32_t j, r;

    e->cfar.cells_tested = 0u;
    e->cfar.cells_hit    = 0u;
    memset(e->cfar.hit, 0, (size_t)nd * nr * sizeof(uint8_t));

    ac.pfa = cf->pfa;
    memset(ac.set, 0, sizeof(ac.set));

    /* ---- per-row prefix sums along range ------------------------------- */
    for (j = 0u; j < nd; ++j)
    {
        const pwr_real* PWR_RESTRICT row = &e->rd_pow[(size_t)j * nr];
        double* PWR_RESTRICT ps = &psum[(size_t)j * (nr + 1u)];
        double acc = 0.0;
        ps[0] = 0.0;
        for (r = 0u; r < nr; ++r)
        {
            acc += (double)row[r];
            ps[r + 1u] = acc;
        }
    }

    /* ---- one Doppler row at a time -------------------------------------- */
    for (j = 0u; j < nd; ++j)
    {
        const pwr_real* PWR_RESTRICT row = &e->rd_pow[(size_t)j * nr];
        uint8_t*  PWR_RESTRICT hit = &e->cfar.hit[(size_t)j * nr];
        pwr_real* PWR_RESTRICT thr = &e->cfar.threshold[(size_t)j * nr];
        const int32_t censor = (cf->censor_zero_doppler != 0 &&
                                labs((long)((int32_t)j - zero_row)) <=
                                    (long)cf->zero_doppler_guard) ? 1 : 0;

        /* Advance the sliding window sums.  Adding and subtracting whole
         * prefix rows costs O(nr) per Doppler row where the old per-cell row
         * walk cost O(Hd) per cell; the double-precision drift across nd
         * incremental slides is a few ulp, far below anything a statistical
         * threshold can feel. */
        if (ca_family != 0)
        {
            uint32_t rr;
            if (j == 0u)
            {
                int32_t dj;
                memset(wsum, 0, (size_t)(nr + 1u) * sizeof(double));
                memset(gsum, 0, (size_t)(nr + 1u) * sizeof(double));
                for (dj = -Hd; dj <= Hd; ++dj)
                {
                    const int32_t jj = ((dj % (int32_t)nd) + (int32_t)nd) %
                                       (int32_t)nd;
                    const double* PWR_RESTRICT ps = &psum[(size_t)jj * (nr + 1u)];
                    const int in_g = (dj >= -Gd && dj <= Gd) ? 1 : 0;
                    for (rr = 0u; rr <= nr; ++rr)
                    {
                        wsum[rr] += ps[rr];
                        if (in_g != 0) { gsum[rr] += ps[rr]; }
                    }
                }
            }
            else
            {
                const int32_t aw = (((int32_t)j + Hd) % (int32_t)nd +
                                    (int32_t)nd) % (int32_t)nd;
                const int32_t sw = (((int32_t)j - 1 - Hd) % (int32_t)nd +
                                    (int32_t)nd) % (int32_t)nd;
                const int32_t ag = (((int32_t)j + Gd) % (int32_t)nd +
                                    (int32_t)nd) % (int32_t)nd;
                const int32_t sg = (((int32_t)j - 1 - Gd) % (int32_t)nd +
                                    (int32_t)nd) % (int32_t)nd;
                const double* PWR_RESTRICT paw = &psum[(size_t)aw * (nr + 1u)];
                const double* PWR_RESTRICT psw = &psum[(size_t)sw * (nr + 1u)];
                const double* PWR_RESTRICT pag = &psum[(size_t)ag * (nr + 1u)];
                const double* PWR_RESTRICT psg = &psum[(size_t)sg * (nr + 1u)];
                for (rr = 0u; rr <= nr; ++rr)
                {
                    wsum[rr] += paw[rr] - psw[rr];
                    gsum[rr] += pag[rr] - psg[rr];
                }
            }
        }

        switch (cf->type)
        {
        /* ---------------- two-dimensional cell averaging ----------------- */
        case PWR_CFAR_CA:
        case PWR_CFAR_GOCA:
        case PWR_CFAR_SOCA:
        {
            const int32_t n_rows = 2 * Hd + 1;
            const int32_t n_mid  = n_rows - (2 * Gd + 1);
            for (r = 0u; r < nr; ++r)
            {
                const int32_t r_lo = pwr_maxi(0, (int32_t)r - Hr);
                const int32_t r_hi = pwr_mini((int32_t)nr - 1, (int32_t)r + Hr);
                const int32_t g_lo = pwr_maxi(0, (int32_t)r - Gr);
                const int32_t g_hi = pwr_mini((int32_t)nr - 1, (int32_t)r + Gr);
                double sum_lead = 0.0, sum_lag = 0.0;
                uint32_t n_lead = 0u, n_lag = 0u, n_ref;
                double noise_est, alpha, threshold;

                /* Same arithmetic as the old per-row walk, regrouped through
                 * the combined window rows: the lead/lag spans are identical
                 * for every window row, and the guard-band middle span is the
                 * window sum minus the guard-band sum, halved exactly as the
                 * per-row 0.5 splits summed to.  The integer counts replicate
                 * the per-row floor division bit for bit. */
                if (g_lo > r_lo)
                {
                    sum_lag = wsum[g_lo] - wsum[r_lo];
                    n_lag   = (uint32_t)n_rows * (uint32_t)(g_lo - r_lo);
                }
                if (r_hi > g_hi)
                {
                    sum_lead = wsum[r_hi + 1] - wsum[g_hi + 1];
                    n_lead   = (uint32_t)n_rows * (uint32_t)(r_hi - g_hi);
                }
                if (n_mid > 0 && g_hi >= g_lo)
                {
                    const double sp = (wsum[g_hi + 1] - wsum[g_lo]) -
                                      (gsum[g_hi + 1] - gsum[g_lo]);
                    const uint32_t cc = (uint32_t)(g_hi - g_lo + 1);
                    sum_lag  += 0.5 * sp;
                    sum_lead += 0.5 * sp;
                    n_lag    += (uint32_t)n_mid * (cc / 2u);
                    n_lead   += (uint32_t)n_mid * (cc - cc / 2u);
                }

                if (cf->type == PWR_CFAR_CA)
                {
                    n_ref = n_lead + n_lag;
                    if (n_ref == 0u) { thr[r] = (pwr_real)1.0e30; continue; }
                    noise_est = (sum_lead + sum_lag) / (double)n_ref;
                    alpha     = pwr_alpha_cached(&ac, n_ref);
                }
                else
                {
                    const double m_lead = (n_lead > 0u) ? sum_lead / (double)n_lead : 0.0;
                    const double m_lag  = (n_lag  > 0u) ? sum_lag  / (double)n_lag  : 0.0;
                    n_ref = pwr_maxu(pwr_maxu(n_lead, n_lag), 1u);
                    if (cf->type == PWR_CFAR_GOCA)
                    {
                        noise_est = pwr_maxd(m_lead, m_lag);
                        alpha     = pwr_alpha_ca(n_ref, cf->pfa * 0.5);
                    }
                    else
                    {
                        noise_est = (n_lead > 0u && n_lag > 0u)
                            ? pwr_mind(m_lead, m_lag) : pwr_maxd(m_lead, m_lag);
                        alpha     = pwr_alpha_cached(&ac, n_ref);
                    }
                }

                threshold = noise_est * alpha * bias;
                thr[r]    = (pwr_real)threshold;
                /* A censored cell receives no detection test, so it must not
                 * enter the measured-Pfa denominator either - counting it
                 * would bias stats.measured_pfa low by the censored fraction. */
                if (censor == 0)
                {
                    ++e->cfar.cells_tested;
                    if ((double)row[r] > threshold)
                    {
                        hit[r] = 1u;
                        ++e->cfar.cells_hit;
                    }
                }
            }
        }
            break;

        /* ---------------- ordered statistic ------------------------------ */
        case PWR_CFAR_OS:
            for (r = 0u; r < nr; ++r)
            {
                pwr_real* buf = e->cfar.train_scratch;
                uint32_t cnt = 0u, k;
                double noise_est, alpha, threshold;
                int32_t dr;

                for (dr = -Hr; dr <= Hr; ++dr)
                {
                    const int32_t rr = (int32_t)r + dr;
                    if (dr >= -Gr && dr <= Gr) { continue; }
                    if (rr < 0 || rr >= (int32_t)nr) { continue; }
                    if (cnt < e->cfar.train_capacity) { buf[cnt++] = row[rr]; }
                }
                if (cnt == 0u) { thr[r] = (pwr_real)1.0e30; continue; }

                k = (cf->os_rank > 0) ? (uint32_t)cf->os_rank
                                      : (uint32_t)((double)cnt * 0.75 + 0.5);
                if (k == 0u) { k = 1u; }
                if (k > cnt) { k = cnt; }
                noise_est = (double)pwr_select_kth_f32(buf, cnt, k - 1u);
                alpha     = pwr_alpha_os(cnt, k, cf->pfa);
                threshold = noise_est * alpha * bias;
                thr[r]    = (pwr_real)threshold;
                if (censor == 0)
                {
                    ++e->cfar.cells_tested;
                    if ((double)row[r] > threshold)
                    {
                        hit[r] = 1u;
                        ++e->cfar.cells_hit;
                    }
                }
            }
            break;

        /* ---------------- trimmed mean ----------------------------------- */
        case PWR_CFAR_TM:
        default:
            for (r = 0u; r < nr; ++r)
            {
                pwr_real* buf = e->cfar.train_scratch;
                uint32_t cnt = 0u, lo, hi, i, used = 0u;
                double acc = 0.0, noise_est, alpha, threshold;
                int32_t dr;

                for (dr = -Hr; dr <= Hr; ++dr)
                {
                    const int32_t rr = (int32_t)r + dr;
                    if (dr >= -Gr && dr <= Gr) { continue; }
                    if (rr < 0 || rr >= (int32_t)nr) { continue; }
                    if (cnt < e->cfar.train_capacity) { buf[cnt++] = row[rr]; }
                }
                if (cnt == 0u) { thr[r] = (pwr_real)1.0e30; continue; }

                lo = (uint32_t)pwr_maxi(0, cf->trim_low);
                hi = (uint32_t)pwr_maxi(0, cf->trim_high);
                if (lo + hi >= cnt) { lo = 0u; hi = 0u; }
                pwr_sort_f32(buf, cnt);
                for (i = lo; i + hi < cnt; ++i) { acc += (double)buf[i]; ++used; }
                if (used == 0u) { thr[r] = (pwr_real)1.0e30; continue; }
                noise_est = acc / (double)used;
                alpha     = pwr_alpha_cached(&ac, used);
                threshold = noise_est * alpha * bias;
                thr[r]    = (pwr_real)threshold;
                if (censor == 0)
                {
                    ++e->cfar.cells_tested;
                    if ((double)row[r] > threshold)
                    {
                        hit[r] = 1u;
                        ++e->cfar.cells_hit;
                    }
                }
            }
            break;
        }
    }

    if (cf->peak_selection != 0) { pwr_cfar_peak_select(e); }

    /* ---- threshold trace that accompanies the A-scope ------------------- */
    for (r = 0u; r < nr; ++r)
    {
        /* Report the threshold in the Doppler bin where the profile peaked -
         * the cell the A-scope is actually showing.  pwr_chain_products()
         * recorded that argmax while it built the profile, so no rescan of
         * the map is needed here. */
        e->thresh_prof[r] = pwr_pow_to_db(
            e->cfar.threshold[(size_t)e->profile_peak_j[r] * nr + r]);
    }
}

/* ==========================================================================
 *  Clustering
 * ==========================================================================
 *  Connected-component labelling of the hit mask with a configurable
 *  connectivity radius, then power-weighted centroiding with parabolic
 *  sub-bin peak interpolation on the dB surface.
 * ------------------------------------------------------------------------ */
typedef struct PWR_ClusterAcc
{
    double  w_sum;
    double  wr_sum;
    double  wd_sum;
    pwr_real peak_pow;
    uint32_t peak_r;
    uint32_t peak_j;
    uint32_t cells;
} PWR_ClusterAcc;

static int pwr_detection_cmp_desc(const PWR_Detection* a, const PWR_Detection* b)
{
    return (a->snr_db > b->snr_db) ? -1 : ((a->snr_db < b->snr_db) ? 1 : 0);
}

void pwr_detections_sort(PWR_Detection* d, uint32_t n)
{
    uint32_t i;
    for (i = 1u; i < n; ++i)
    {
        const PWR_Detection key = d[i];
        uint32_t j = i;
        while (j > 0u && pwr_detection_cmp_desc(&key, &d[j - 1u]) < 0)
        {
            d[j] = d[j - 1u];
            --j;
        }
        d[j] = key;
    }
}

void pwr_cluster_run(struct PWR_Engine* e)
{
    const uint32_t nr = e->n_range;
    const uint32_t nd = e->n_doppler;
    const PWR_ClusterConfig* const cc = &e->cfg.cluster;
    const int32_t tol_r = cc->range_tolerance;
    const int32_t tol_d = cc->doppler_tolerance;
    const double  noise_db = e->stats.measured_noise_floor_db;
    const uint32_t total = nr * nd;
    uint32_t idx, sp;

    e->detection_count = 0u;
    if (e->cluster_label == NULL) { return; }
    memset(e->cluster_label, 0, (size_t)total * sizeof(int32_t));

    for (idx = 0u; idx < total; ++idx)
    {
        PWR_ClusterAcc acc;
        uint32_t seed_j;

        if (e->cfar.hit[idx] == 0u || e->cluster_label[idx] != 0) { continue; }

        seed_j = idx / nr;
        memset(&acc, 0, sizeof(acc));
        acc.peak_pow = -1.0f;

        /* ---- flood fill ------------------------------------------------- */
        sp = 0u;
        e->cluster_stack[sp++] = (int32_t)idx;
        e->cluster_label[idx]  = 1;

        while (sp > 0u)
        {
            const uint32_t cur = (uint32_t)e->cluster_stack[--sp];
            const uint32_t cj  = cur / nr;
            const uint32_t cr  = cur % nr;
            const pwr_real pw  = e->rd_pow[cur];
            int32_t dj, dr;

            acc.cells  += 1u;
            acc.w_sum  += (double)pw;
            acc.wr_sum += (double)pw * (double)cr;
            /* Doppler centroid is accumulated relative to the seed row so
             * that a cluster straddling the wrap point still centroids
             * correctly; it is folded back below. */
            acc.wd_sum += (double)pw *
                (double)(((int32_t)cj - (int32_t)seed_j + (int32_t)nd +
                          (int32_t)nd / 2) % (int32_t)nd - (int32_t)nd / 2);
            if (pw > acc.peak_pow)
            {
                acc.peak_pow = pw;
                acc.peak_r   = cr;
                acc.peak_j   = cj;
            }

            if (cc->enable == 0) { continue; }

            for (dj = -tol_d; dj <= tol_d; ++dj)
            {
                const int32_t jj = (((int32_t)cj + dj) % (int32_t)nd +
                                    (int32_t)nd) % (int32_t)nd;
                for (dr = -tol_r; dr <= tol_r; ++dr)
                {
                    const int32_t rr = (int32_t)cr + dr;
                    uint32_t nidx;
                    if (rr < 0 || rr >= (int32_t)nr) { continue; }
                    nidx = (uint32_t)jj * nr + (uint32_t)rr;
                    if (e->cfar.hit[nidx] == 0u) { continue; }
                    if (e->cluster_label[nidx] != 0) { continue; }
                    e->cluster_label[nidx] = 1;
                    if (sp < total) { e->cluster_stack[sp++] = (int32_t)nidx; }
                }
            }
        }

        /* ---- accept or reject ------------------------------------------- */
        if (acc.cells < (uint32_t)cc->min_cells) { continue; }
        if (acc.cells > (uint32_t)cc->max_cells) { continue; }
        if (!(acc.w_sum > 0.0)) { continue; }

        {
            const double peak_db = (double)pwr_pow_to_db(acc.peak_pow);
            const double snr_db  = peak_db - noise_db;
            double cen_r, cen_d;
            PWR_Detection* det;

            if (snr_db < cc->min_snr_db) { continue; }
            if (e->detection_count >= PWR_MAX_DETECTIONS) { break; }

            /* Power-weighted centroid, refined by a parabolic fit on the dB
             * surface when the peak is interior. */
            cen_r = acc.wr_sum / acc.w_sum;
            cen_d = (double)seed_j + acc.wd_sum / acc.w_sum;

            if (acc.peak_r > 0u && acc.peak_r + 1u < nr)
            {
                const pwr_real* row = &e->rd_db[(size_t)acc.peak_j * nr];
                cen_r = (double)acc.peak_r +
                        pwr_parabolic_peak((double)row[acc.peak_r - 1u],
                                           (double)row[acc.peak_r],
                                           (double)row[acc.peak_r + 1u]);
            }
            {
                const int32_t jm = (((int32_t)acc.peak_j - 1 + (int32_t)nd) %
                                    (int32_t)nd);
                const int32_t jp = (((int32_t)acc.peak_j + 1) % (int32_t)nd);
                cen_d = (double)acc.peak_j +
                        pwr_parabolic_peak(
                            (double)e->rd_db[(size_t)jm * nr + acc.peak_r],
                            (double)e->rd_db[(size_t)acc.peak_j * nr + acc.peak_r],
                            (double)e->rd_db[(size_t)jp * nr + acc.peak_r]);
            }

            det = &e->detections[e->detection_count++];
            memset(det, 0, sizeof(*det));
            det->range_bin   = acc.peak_r;
            det->doppler_bin = acc.peak_j;
            det->cell_count  = acc.cells;
            det->centroid_range_bin   = cen_r;
            det->centroid_doppler_bin = cen_d;
            det->range_m = e->axis_range_first_m +
                           cen_r * e->axis_range_step_m;
            det->radial_velocity_mps = e->axis_vel_first_mps +
                                       cen_d * e->axis_vel_step_mps;
            det->azimuth_deg  = pwr_wrap360(e->beam_azimuth_deg);
            det->time_s       = e->scenario_time_s;
            det->amplitude_db = peak_db;
            det->snr_db       = snr_db;
            det->threshold_db = (double)pwr_pow_to_db(
                e->cfar.threshold[(size_t)acc.peak_j * nr + acc.peak_r]);
            det->ambiguous = (det->range_m > e->dm.unambiguous_range_m ||
                              fabs(det->radial_velocity_mps) >
                                  e->dm.unambiguous_velocity_mps) ? 1 : 0;
        }
    }

    pwr_detections_sort(e->detections, e->detection_count);
}

/* ==========================================================================
 *  Dwell-level plot centroiding (azimuth beam splitting)
 * ==========================================================================
 *  The per-CPI plots above carry the beam boresight as their azimuth, and a
 *  strong target stays above threshold for several consecutive CPIs while
 *  the beam sweeps across it.  Left unmerged, one target therefore produces
 *  a string of plots spread over degrees of azimuth every scan: the leftover
 *  plots escape the initiation-inhibit radius and seed duplicate tracks, and
 *  the CPI-to-CPI azimuth stepping injects a bogus tangential velocity into
 *  any track that follows the string.
 *
 *  This pass does what an operational plot extractor does: per-CPI hits that
 *  agree in range and radial velocity across consecutive CPIs accumulate
 *  into one dwell plot, and when the beam moves past (one CPI with no new
 *  contribution) the plot is closed and emitted with power-weighted
 *  centroids.  The azimuth centroid across the two-way beam shape is the
 *  classic beam-splitting estimator, accurate to a fraction of a beamwidth.
 *
 *  The range/velocity gates are deliberately tight - about one output cell -
 *  so genuinely resolved targets (the scenario-4 resolution pair at 1.2
 *  range cells, or a Doppler-separated pair in the same gate) are never
 *  merged; same-CPI plots are never merged with each other for the same
 *  reason.  While the antenna is staring a dwell has no natural end, so the
 *  pass is bypassed and plots flow through per CPI exactly as before.
 * ------------------------------------------------------------------------ */
void pwr_plots_dwell_merge(struct PWR_Engine* e)
{
    const uint32_t cpi   = (uint32_t)e->stats.cpi_count;
    const double   r_tol = 1.25 * e->axis_range_step_m;
    const double   v_tol = 1.5 * fabs(e->axis_vel_step_mps);
    uint32_t i, k, n_out = 0u;

    if (e->dm.scan_period_s <= 0.0) { return; }      /* staring: no dwell end */

    /* ---- 1. fold this CPI's plots into the open dwell plots ------------- */
    for (i = 0u; i < e->detection_count; ++i)
    {
        const PWR_Detection* d = &e->detections[i];
        const double w  = pwr_db_to_pow(d->snr_db);
        const double az = pwr_deg_to_rad(d->azimuth_deg);
        PWR_DwellPlot* best = NULL;
        double best_dr = r_tol;

        for (k = 0u; k < PWR_MAX_DETECTIONS; ++k)
        {
            PWR_DwellPlot* dp = &e->dwell[k];
            double dr, dv;
            /* Only a dwell fed in the immediately preceding CPI may grow:
             * same-CPI plots are distinct targets by construction, and a
             * gap means the beam has left. */
            if (dp->active == 0 || dp->last_cpi == cpi ||
                (cpi - dp->last_cpi) > 1u) { continue; }
            dr = fabs(d->range_m - dp->wrange_sum / dp->w_sum);
            dv = fabs(d->radial_velocity_mps - dp->wvel_sum / dp->w_sum);
            if (dr <= best_dr && dv <= v_tol)
            {
                best    = dp;
                best_dr = dr;
            }
        }
        if (best == NULL)
        {
            for (k = 0u; k < PWR_MAX_DETECTIONS; ++k)
            {
                if (e->dwell[k].active == 0) { best = &e->dwell[k]; break; }
            }
            if (best == NULL) { continue; }          /* table full: drop */
            memset(best, 0, sizeof(*best));
            best->active = 1;
            best->peak_w = -1.0;
        }
        best->w_sum      += w;
        best->waz_e      += w * sin(az);
        best->waz_n      += w * cos(az);
        best->wrange_sum += w * d->range_m;
        best->wvel_sum   += w * d->radial_velocity_mps;
        best->cells      += d->cell_count;
        best->cpi_count  += 1u;
        best->last_cpi    = cpi;
        if (w > best->peak_w)
        {
            best->peak   = *d;
            best->peak_w = w;
        }
    }

    /* ---- 2. close and emit every dwell the beam has moved past ---------- */
    for (k = 0u; k < PWR_MAX_DETECTIONS; ++k)
    {
        PWR_DwellPlot* dp = &e->dwell[k];
        if (dp->active == 0 || dp->last_cpi == cpi) { continue; }
        /* A gap of exactly one CPI is the normal end of a dwell.  Anything
         * older can only mean the merge was bypassed in between (scan rate
         * toggled through zero): that dwell is stale and must be dropped,
         * or it would re-emit a plot the staring CPI already published. */
        if ((cpi - dp->last_cpi) == 1u && dp->w_sum > 0.0 &&
            n_out < PWR_MAX_DETECTIONS)
        {
            PWR_Detection* out = &e->dwell_emit[n_out++];
            *out = dp->peak;
            out->range_m             = dp->wrange_sum / dp->w_sum;
            out->radial_velocity_mps = dp->wvel_sum / dp->w_sum;
            out->azimuth_deg         = pwr_wrap360(
                pwr_rad_to_deg(atan2(dp->waz_e, dp->waz_n)));
            out->cell_count          = dp->cells;
            out->assoc_track_id      = 0;
        }
        /* Older leftovers (mode switch while dwells were open) are stale
         * and dropped rather than emitted. */
        dp->active = 0;
    }

    /* ---- 3. the tracker and the frame consume the closed dwells --------- */
    memcpy(e->detections, e->dwell_emit, (size_t)n_out * sizeof(PWR_Detection));
    e->detection_count = n_out;
    pwr_detections_sort(e->detections, n_out);
}
