/* Plot-to-track association and Kalman filtering.
 *
 * Four-state constant-velocity filter in a local East-North-Up frame:
 *   X = [x y vx vy]'          metres, metres/second
 *   F = [I dt*I ; 0 I]
 *   Q = discrete white-noise-acceleration, driven by sigma_a
 *   H = [I 0]                 Cartesian position measurement
 *
 * A polar detection (R, Az) is converted to Cartesian and its covariance
 * transported through the Jacobian
 *
 *   J = [sin Az   R cos Az ; cos Az  -R sin Az]
 *
 * so down-range and cross-range uncertainty stay correctly shaped - the
 * characteristic "banana" gate of a long-range radar. The covariance update
 * uses the Joseph form, which stays symmetric and positive definite over long
 * coasting intervals where the plain (I-KH)P form is known to lose both.
 *
 * Association cost is normalised innovation squared plus ln det(S), the
 * standard global nearest-neighbour metric; pairs failing the gate are marked
 * infeasible. The rectangular assignment is then solved exactly by the
 * Jonker-Volgenant shortest-augmenting-path algorithm.
 *
 * A track is only expected to be seen while the beam is on it. Every CPI all
 * tracks are predicted forward, but only tracks the beam can plausibly have
 * illuminated take part in association and accrue a hit or a miss - without
 * that, a 2.5 s scan period would coast every track to death within a fraction
 * of a revolution.
 */
#include "pwr_core.h"

#include <string.h>

#define PWR_ASSOC_REJECT   1.0e17
#define PWR_TRAIL_PERIOD_S 0.25

/* Retirement is a two-step affair: the rule that kills a track moves it to
 * PWR_TRACK_TERMINATED, it is published once in that state so a consumer sees
 * an explicit end-of-track rather than a report that merely stops arriving,
 * and the slot is released at the top of the next update.  A terminated track
 * is no longer live: it neither predicts, associates, books attempts nor
 * inhibits initiation. */
static int pwr_track_is_live(const PWR_TrackInternal* tk)
{
    return (tk->state != PWR_TRACK_FREE &&
            tk->state != PWR_TRACK_TERMINATED) ? 1 : 0;
}

static void pwr_track_retire(PWR_Tracker* tr, PWR_TrackInternal* tk)
{
    if (tk->state == PWR_TRACK_FREE || tk->state == PWR_TRACK_TERMINATED)
    {
        return;
    }
    tk->state = PWR_TRACK_TERMINATED;
    ++tr->deleted_total;
}

/* ==========================================================================
 *  Life cycle
 * ========================================================================== */
PWR_Status pwr_tracker_init(PWR_Tracker* t)
{
    if (t == NULL) { return PWR_ERR_NULL_POINTER; }
    pwr_tracker_release(t);

    t->cost       = PWR_ALLOC_ARRAY(double,  (size_t)PWR_MAX_TRACKS * PWR_MAX_DETECTIONS);
    t->row_assign = PWR_ALLOC_ARRAY(int32_t, PWR_MAX_TRACKS);
    t->col_assign = PWR_ALLOC_ARRAY(int32_t, PWR_MAX_DETECTIONS);
    t->dual_u     = PWR_ALLOC_ARRAY(double,  PWR_ASSOC_DIM + 2u);
    t->dual_v     = PWR_ALLOC_ARRAY(double,  PWR_ASSOC_DIM + 2u);
    t->jv_minv    = PWR_ALLOC_ARRAY(double,  PWR_ASSOC_DIM + 2u);
    t->jv_way     = PWR_ALLOC_ARRAY(int32_t, PWR_ASSOC_DIM + 2u);
    t->jv_pcol    = PWR_ALLOC_ARRAY(int32_t, PWR_ASSOC_DIM + 2u);
    t->jv_used    = PWR_ALLOC_ARRAY(uint8_t, PWR_ASSOC_DIM + 2u);
    t->cand       = PWR_ALLOC_ARRAY(int32_t, PWR_MAX_TRACKS);

    if (t->cost == NULL || t->row_assign == NULL || t->col_assign == NULL ||
        t->dual_u == NULL || t->dual_v == NULL || t->jv_minv == NULL ||
        t->jv_way == NULL || t->jv_pcol == NULL || t->jv_used == NULL ||
        t->cand == NULL)
    {
        pwr_tracker_release(t);
        return PWR_ERR_OUT_OF_MEMORY;
    }
    pwr_tracker_reset(t);
    return PWR_STATUS_OK;
}

void pwr_tracker_release(PWR_Tracker* t)
{
    if (t == NULL) { return; }
    PWR_FREE(t->cost);
    PWR_FREE(t->row_assign);
    PWR_FREE(t->col_assign);
    PWR_FREE(t->dual_u);
    PWR_FREE(t->dual_v);
    PWR_FREE(t->jv_minv);
    PWR_FREE(t->jv_way);
    PWR_FREE(t->jv_pcol);
    PWR_FREE(t->jv_used);
    PWR_FREE(t->cand);
}

void pwr_tracker_reset(PWR_Tracker* t)
{
    uint32_t i;
    if (t == NULL) { return; }
    for (i = 0u; i < PWR_MAX_TRACKS; ++i)
    {
        memset(&t->tracks[i], 0, sizeof(t->tracks[i]));
        t->tracks[i].state = PWR_TRACK_FREE;
    }
    t->next_id       = 1u;
    t->created_total = 0u;
    t->deleted_total = 0u;
    t->cost_rows     = 0u;
    t->cost_cols     = 0u;
    t->last_beam_az_deg = 0.0;
}

/* ==========================================================================
 *  Rectangular linear assignment - Jonker-Volgenant shortest augmenting path
 * ==========================================================================
 *  Solves min sum cost[i][row_assign[i]] with every row matched to a distinct
 *  column, assuming rows <= cols.  O(rows^2 * cols).  The wrapper below
 *  transposes when rows > cols so the caller never has to care.
 * ------------------------------------------------------------------------ */
void pwr_assign_jv(const double* cost, uint32_t n, uint32_t m,
                   uint32_t stride,
                   int32_t* row_assign,
                   double* u, double* v, double* minv,
                   int32_t* way, int32_t* p, uint8_t* used)
{
    uint32_t i, j;

    if (cost == NULL || row_assign == NULL || n == 0u || m == 0u || n > m)
    {
        if (row_assign != NULL)
        {
            for (i = 0u; i < n; ++i) { row_assign[i] = -1; }
        }
        return;
    }

    for (j = 0u; j <= m; ++j) { v[j] = 0.0; p[j] = 0; way[j] = 0; }
    for (i = 0u; i <= n; ++i) { u[i] = 0.0; }

    for (i = 1u; i <= n; ++i)
    {
        uint32_t j0 = 0u;
        p[0] = (int32_t)i;
        for (j = 0u; j <= m; ++j) { minv[j] = PWR_ASSOC_INFEASIBLE * 10.0; used[j] = 0u; }

        do
        {
            const uint32_t i0 = (uint32_t)p[j0];
            double delta = PWR_ASSOC_INFEASIBLE * 10.0;
            uint32_t j1 = 0u;

            used[j0] = 1u;
            for (j = 1u; j <= m; ++j)
            {
                if (used[j] != 0u) { continue; }
                {
                    const double cur = cost[(size_t)(i0 - 1u) * stride + (j - 1u)]
                                     - u[i0] - v[j];
                    if (cur < minv[j]) { minv[j] = cur; way[j] = (int32_t)j0; }
                    if (minv[j] < delta) { delta = minv[j]; j1 = j; }
                }
            }
            if (j1 == 0u) { break; }   /* no reachable column: unmatched row  */

            for (j = 0u; j <= m; ++j)
            {
                if (used[j] != 0u)
                {
                    u[(uint32_t)p[j]] += delta;
                    v[j]              -= delta;
                }
                else
                {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);

        if (p[j0] == 0)
        {
            /* Augment along the alternating path. */
            while (j0 != 0u)
            {
                const uint32_t j1 = (uint32_t)way[j0];
                p[j0] = p[j1];
                j0 = j1;
            }
        }
    }

    for (i = 0u; i < n; ++i) { row_assign[i] = -1; }
    for (j = 1u; j <= m; ++j)
    {
        if (p[j] > 0 && (uint32_t)p[j] <= n)
        {
            row_assign[(uint32_t)p[j] - 1u] = (int32_t)(j - 1u);
        }
    }
}

/* ==========================================================================
 *  Kalman primitives
 * ========================================================================== */
static void pwr_kf_predict(PWR_TrackInternal* tk, double dt, double sigma_a)
{
    const double dt2 = dt * dt;
    const double dt3 = dt2 * dt;
    const double dt4 = dt2 * dt2;
    const double q   = sigma_a * sigma_a;
    double F[16], T[16], P2[16], Q[16];
    uint32_t i;

    if (dt <= 0.0) { return; }

    /* X = F X */
    tk->X[0] += tk->X[2] * dt;
    tk->X[1] += tk->X[3] * dt;

    memset(F, 0, sizeof(F));
    F[0] = 1.0; F[2]  = dt;
    F[5] = 1.0; F[7]  = dt;
    F[10] = 1.0;
    F[15] = 1.0;

    memset(Q, 0, sizeof(Q));
    Q[0]  = q * dt4 / 4.0;  Q[2]  = q * dt3 / 2.0;
    Q[5]  = q * dt4 / 4.0;  Q[7]  = q * dt3 / 2.0;
    Q[8]  = q * dt3 / 2.0;  Q[10] = q * dt2;
    Q[13] = q * dt3 / 2.0;  Q[15] = q * dt2;

    pwr_mat_mul(F, tk->P, T, 4u);        /* T  = F P    */
    pwr_mat_mul_t(T, F, P2, 4u);         /* P2 = F P F' */
    for (i = 0u; i < 16u; ++i) { tk->P[i] = P2[i] + Q[i]; }
    pwr_mat_symmetrise(tk->P, 4u);
}

/* Cartesian measurement covariance from the polar accuracies. */
static void pwr_meas_cov(double range_m, double az_deg,
                         double sig_r, double sig_az_deg, double* R2)
{
    const double az   = pwr_deg_to_rad(az_deg);
    const double s    = sin(az);
    const double c    = cos(az);
    const double sr2  = sig_r * sig_r;
    const double sc2  = (range_m * pwr_deg_to_rad(sig_az_deg)) *
                        (range_m * pwr_deg_to_rad(sig_az_deg));
    R2[0] = sr2 * s * s + sc2 * c * c;
    R2[1] = (sr2 - sc2) * s * c;
    R2[2] = R2[1];
    R2[3] = sr2 * c * c + sc2 * s * s;
}

/* Returns the normalised innovation squared, or a negative value on a
 * numerically singular innovation covariance. */
static double pwr_kf_gate(const PWR_TrackInternal* tk, const double* z,
                          const double* R2, double* out_Sinv, double* out_logdet)
{
    double S[4], Sinv[4], d[2];
    S[0] = tk->P[0]  + R2[0];
    S[1] = tk->P[1]  + R2[1];
    S[2] = tk->P[4]  + R2[2];
    S[3] = tk->P[5]  + R2[3];
    if (pwr_mat2_inv(S, Sinv) != PWR_STATUS_OK) { return -1.0; }
    d[0] = z[0] - tk->X[0];
    d[1] = z[1] - tk->X[1];
    if (out_Sinv != NULL) { memcpy(out_Sinv, Sinv, sizeof(Sinv)); }
    if (out_logdet != NULL)
    {
        const double det = S[0] * S[3] - S[1] * S[2];
        *out_logdet = (det > 0.0) ? log(det) : 0.0;
    }
    return pwr_mahalanobis2(Sinv, d);
}

static void pwr_kf_update(PWR_TrackInternal* tk, const double* z, const double* R2)
{
    double S[4], Sinv[4], K[8], d[2], IKH[16], T[16], P2[16], KR[8];
    uint32_t i, j, k;

    S[0] = tk->P[0] + R2[0];
    S[1] = tk->P[1] + R2[1];
    S[2] = tk->P[4] + R2[2];
    S[3] = tk->P[5] + R2[3];
    if (pwr_mat2_inv(S, Sinv) != PWR_STATUS_OK) { return; }

    /* K = P H' Sinv,  H = [I 0]  ->  P H' is the first two columns of P. */
    for (i = 0u; i < 4u; ++i)
    {
        const double p0 = tk->P[i * 4u + 0u];
        const double p1 = tk->P[i * 4u + 1u];
        K[i * 2u + 0u] = p0 * Sinv[0] + p1 * Sinv[2];
        K[i * 2u + 1u] = p0 * Sinv[1] + p1 * Sinv[3];
    }

    d[0] = z[0] - tk->X[0];
    d[1] = z[1] - tk->X[1];
    for (i = 0u; i < 4u; ++i)
    {
        tk->X[i] += K[i * 2u + 0u] * d[0] + K[i * 2u + 1u] * d[1];
    }

    /* Joseph form: P = (I-KH) P (I-KH)' + K R K' */
    memset(IKH, 0, sizeof(IKH));
    for (i = 0u; i < 4u; ++i) { IKH[i * 4u + i] = 1.0; }
    for (i = 0u; i < 4u; ++i)
    {
        IKH[i * 4u + 0u] -= K[i * 2u + 0u];
        IKH[i * 4u + 1u] -= K[i * 2u + 1u];
    }
    pwr_mat_mul(IKH, tk->P, T, 4u);
    pwr_mat_mul_t(T, IKH, P2, 4u);

    /* KR = K * R2 (4x2 * 2x2) */
    for (i = 0u; i < 4u; ++i)
    {
        KR[i * 2u + 0u] = K[i * 2u + 0u] * R2[0] + K[i * 2u + 1u] * R2[2];
        KR[i * 2u + 1u] = K[i * 2u + 0u] * R2[1] + K[i * 2u + 1u] * R2[3];
    }
    for (i = 0u; i < 4u; ++i)
    {
        for (j = 0u; j < 4u; ++j)
        {
            double acc = P2[i * 4u + j];
            for (k = 0u; k < 2u; ++k)
            {
                acc += KR[i * 2u + k] * K[j * 2u + k];
            }
            tk->P[i * 4u + j] = acc;
        }
    }
    pwr_mat_symmetrise(tk->P, 4u);
}

/* ==========================================================================
 *  Classification heuristic (kinematic only)
 * ========================================================================== */
static int32_t pwr_classify(double speed_mps)
{
    if (speed_mps < 18.0)  { return PWR_CLASS_SURFACE; }
    if (speed_mps < 60.0)  { return PWR_CLASS_ROTARY;  }
    if (speed_mps < 120.0) { return PWR_CLASS_UAV;     }
    if (speed_mps < 400.0) { return PWR_CLASS_AIR;     }
    return PWR_CLASS_MISSILE;
}

static uint32_t pwr_popcount32(uint32_t v)
{
    v = v - ((v >> 1u) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2u) & 0x33333333u);
    v = (v + (v >> 4u)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24u;
}

/* Books one M-of-N attempt - per CPI while staring, per scan while rotating -
 * and applies the confirmation / termination rules.  The caller is expected
 * to have done the hit-side bookkeeping (KF update, hit counters) already. */
static void pwr_track_book_attempt(PWR_Tracker* tr, PWR_TrackInternal* tk,
                                   const PWR_TrackerConfig* cfg, int hit)
{
    ++tk->update_attempts;
    tk->history_bits <<= 1u;
    if (hit != 0)
    {
        tk->history_bits |= 1u;
    }
    else
    {
        if (tk->dwell_state == PWR_DWELL_IDLE)
        {
            tk->dwell_state = PWR_DWELL_MISS;
        }
        ++tk->misses;
        ++tk->consecutive_misses;
    }
    {
        const uint32_t mask = (cfg->confirm_n >= 32)
            ? 0xFFFFFFFFu : ((1u << (uint32_t)cfg->confirm_n) - 1u);
        const uint32_t recent = pwr_popcount32(tk->history_bits & mask);

        if (tk->state == PWR_TRACK_TENTATIVE &&
            recent >= (uint32_t)cfg->confirm_m)
        {
            tk->state = PWR_TRACK_CONFIRMED;
        }
        if (tk->consecutive_misses >= (uint32_t)cfg->delete_misses ||
            (tk->state == PWR_TRACK_TENTATIVE &&
             tk->consecutive_misses >= (uint32_t)cfg->coast_misses &&
             tk->hits < 2u))
        {
            pwr_track_retire(tr, tk);
        }
        else if (tk->state == PWR_TRACK_CONFIRMED &&
                 tk->consecutive_misses >= (uint32_t)cfg->coast_misses)
        {
            tk->state = PWR_TRACK_COASTING;
        }
    }
}

static void pwr_trail_push(PWR_TrackInternal* tk, double time_s)
{
    if (tk->trail_count > 0u && (time_s - tk->trail_last_time_s) < PWR_TRAIL_PERIOD_S)
    {
        /* Keep the newest point fresh without growing the trail. */
        tk->trail[tk->trail_head].x_m = (float)tk->X[0];
        tk->trail[tk->trail_head].y_m = (float)tk->X[1];
        return;
    }
    tk->trail_head = (tk->trail_count == 0u)
                   ? 0u
                   : ((tk->trail_head + 1u) % PWR_TRACK_TRAIL_LEN);
    tk->trail[tk->trail_head].x_m = (float)tk->X[0];
    tk->trail[tk->trail_head].y_m = (float)tk->X[1];
    if (tk->trail_count < PWR_TRACK_TRAIL_LEN) { ++tk->trail_count; }
    tk->trail_last_time_s = time_s;
}

/* ==========================================================================
 *  Track initiation
 * ========================================================================== */
/* Returns the new track id, or 0 when the track file is full. */
static uint32_t pwr_track_init_from_detection(PWR_Tracker* tr,
                                              const PWR_Detection* det,
                                              const PWR_TrackerConfig* cfg,
                                              double time_s)
{
    uint32_t slot = PWR_MAX_TRACKS;
    uint32_t i;
    PWR_TrackInternal* tk;
    double R2[4];
    const double az = pwr_deg_to_rad(det->azimuth_deg);

    for (i = 0u; i < PWR_MAX_TRACKS; ++i)
    {
        if (tr->tracks[i].state == PWR_TRACK_FREE) { slot = i; break; }
    }
    if (slot == PWR_MAX_TRACKS) { return 0u; }

    tk = &tr->tracks[slot];
    memset(tk, 0, sizeof(*tk));

    tk->X[0] = det->range_m * sin(az);
    tk->X[1] = det->range_m * cos(az);
    /* Seed the velocity with the measured range rate projected on the line of
     * sight: it is the only velocity information a single plot carries. */
    tk->X[2] = det->radial_velocity_mps * sin(az);
    tk->X[3] = det->radial_velocity_mps * cos(az);

    pwr_meas_cov(det->range_m, det->azimuth_deg,
                 cfg->meas_sigma_range_m, cfg->meas_sigma_azimuth_deg, R2);
    tk->P[0]  = R2[0];  tk->P[1]  = R2[1];
    tk->P[4]  = R2[2];  tk->P[5]  = R2[3];
    tk->P[10] = cfg->init_velocity_sigma * cfg->init_velocity_sigma;
    tk->P[15] = cfg->init_velocity_sigma * cfg->init_velocity_sigma;

    tk->id                  = tr->next_id++;
    tk->state               = PWR_TRACK_TENTATIVE;
    tk->hits                = 1u;
    tk->update_attempts     = 1u;
    tk->history_bits        = 1u;
    tk->dwell_state         = PWR_DWELL_HIT;    /* born from this dwell's plot */
    tk->hit_this_scan       = 0;    /* the birth attempt is already booked     */
    tk->last_attempt_time_s = time_s;
    tk->snr_db              = (float)det->snr_db;
    tk->first_time_s        = time_s;
    tk->last_meas_time_s    = time_s;
    tk->last_update_time_s  = time_s;
    tk->last_predict_time_s = time_s;
    tk->radial_velocity_mps = det->radial_velocity_mps;
    tk->target_class        = PWR_CLASS_UNKNOWN;
    pwr_trail_push(tk, time_s);
    ++tr->created_total;
    return tk->id;
}

/* ==========================================================================
 *  Per-CPI tracker update
 * ========================================================================== */
void pwr_tracker_update(struct PWR_Engine* e)
{
    PWR_Tracker* const tr = &e->tracker;
    const PWR_TrackerConfig* const cfg = &e->cfg.tracker;
    const double now = e->scenario_time_s;
    const double beam = e->beam_azimuth_deg;
    /* Illuminated arc: the mainlobe plus a margin for prediction error. */
    const double arc = 0.5 * e->cfg.azimuth_beamwidth_deg * 1.6 +
                       0.5 * e->dm.cpi_duration_s * 6.0 * e->cfg.scan_rate_rpm;
    /* Zero when the antenna is staring, which disables the scan-based
     * staleness rule and leaves the per-CPI M-of-N logic in sole charge. */
    const double scan_period = e->dm.scan_period_s;
    /* Rotating antenna: plots are dwell-centroided and emitted after the
     * beam has left the target, so candidacy keys on the plots themselves
     * and the M-of-N attempt is booked once per revolution (step 5b).
     * Staring: plots flow per CPI and the classic per-CPI logic stays in
     * charge. */
    const int rotating = (scan_period > 0.0) ? 1 : 0;
    uint32_t n_cand = 0u;
    uint32_t i, c;

    /* ---- 0. release the slots of tracks retired on the previous update ---
     *  They were held one extra publication so the consumer saw an explicit
     *  PWR_TRACK_TERMINATED report; from here the slot is reusable. */
    for (i = 0u; i < PWR_MAX_TRACKS; ++i)
    {
        if (tr->tracks[i].state == PWR_TRACK_TERMINATED)
        {
            tr->tracks[i].state = PWR_TRACK_FREE;
        }
    }

    if (cfg->enable == 0)
    {
        for (i = 0u; i < PWR_MAX_TRACKS; ++i)
        {
            pwr_track_retire(tr, &tr->tracks[i]);
        }
        return;
    }

    /* ---- 1. predict every live track to the current time ---------------- */
    for (i = 0u; i < PWR_MAX_TRACKS; ++i)
    {
        PWR_TrackInternal* tk = &tr->tracks[i];
        double dt;
        if (pwr_track_is_live(tk) == 0) { continue; }
        tk->dwell_state = PWR_DWELL_IDLE;
        dt = now - tk->last_predict_time_s;
        if (dt > 0.0)
        {
            pwr_kf_predict(tk, dt, cfg->process_noise_accel);
            tk->last_predict_time_s = now;
        }
        /* Kinematic plausibility guard: a diverged filter is terminated
         * rather than allowed to poison the association. */
        {
            const double sp = sqrt(tk->X[2] * tk->X[2] + tk->X[3] * tk->X[3]);
            if (!(sp <= cfg->max_speed_mps * 2.0) ||
                !(tk->P[0] < 1.0e12) || !(tk->P[5] < 1.0e12))
            {
                pwr_track_retire(tr, tk);
                continue;
            }
        }
        /* ---- 2. scan-based staleness ------------------------------------
         *  With a rotating antenna a track only accrues hits and misses while
         *  the beam is on it, so the M-of-N counters alone can never retire a
         *  track that the beam has left for good - for instance a track seeded
         *  by a single false alarm.  Elapsed time since the last real
         *  measurement, expressed in scan periods, closes that gap. */
        if (scan_period > 0.0)
        {
            const double missed = (now - tk->last_meas_time_s) / scan_period;
            /* With per-scan attempt booking the M-of-N counters retire dead
             * tracks by themselves; this stays as a backstop with a longer
             * fuse rather than pre-empting the operator's delete setting. */
            const double kill_confirmed =
                pwr_maxd(2.0, (double)cfg->delete_misses + 2.0);
            if (tk->state == PWR_TRACK_TENTATIVE && missed > 1.5)
            {
                pwr_track_retire(tr, tk);
                continue;
            }
            if (missed > kill_confirmed)
            {
                pwr_track_retire(tr, tk);
                continue;
            }
            if (tk->state == PWR_TRACK_CONFIRMED && missed > 1.0)
            {
                tk->state = PWR_TRACK_COASTING;
            }
        }

        /* ---- 3. build the association candidate list -------------------- */
        {
            const double az = pwr_wrap360(pwr_rad_to_deg(atan2(tk->X[0], tk->X[1])));
            int in = 0;
            if (rotating == 0)
            {
                in = (fabs(pwr_angle_diff(az, beam)) <= arc) ? 1 : 0;
            }
            else
            {
                /* Metric pre-gate against every emitted plot, mirroring the
                 * rectangular pre-gate of the cost matrix.  A fixed azimuth
                 * window here would be tighter than the statistical gate and
                 * would cut fast crossing targets off from their own tracks:
                 * a radially seeded track predicts a frozen azimuth, and at
                 * 2 km a 150 m/s crossing target moves ten degrees per scan
                 * while still being only 375 m from the prediction. */
                uint32_t d;
                for (d = 0u; d < e->detection_count && in == 0; ++d)
                {
                    const PWR_Detection* dd = &e->detections[d];
                    const double azr = pwr_deg_to_rad(dd->azimuth_deg);
                    if (fabs(dd->range_m * sin(azr) - tk->X[0])
                            <= cfg->gate_max_range_m &&
                        fabs(dd->range_m * cos(azr) - tk->X[1])
                            <= cfg->gate_max_range_m)
                    {
                        in = 1;
                    }
                }
            }
            if (in != 0 && n_cand < PWR_MAX_TRACKS)
            {
                tr->cand[n_cand++] = (int32_t)i;
            }
        }
    }

    /* ---- 4. cost matrix ------------------------------------------------- */
    {
        const uint32_t nd = pwr_minu(e->detection_count, PWR_MAX_DETECTIONS);
        const double gate2 = cfg->gate_sigma * cfg->gate_sigma;
        double R2[4];

        tr->cost_rows = n_cand;
        tr->cost_cols = nd;

        for (c = 0u; c < nd; ++c) { tr->col_assign[c] = -1; }
        for (i = 0u; i < n_cand; ++i) { tr->row_assign[i] = -1; }

        if (n_cand > 0u && nd > 0u)
        {
            for (i = 0u; i < n_cand; ++i)
            {
                PWR_TrackInternal* tk = &tr->tracks[tr->cand[i]];
                for (c = 0u; c < nd; ++c)
                {
                    const PWR_Detection* det = &e->detections[c];
                    const double az = pwr_deg_to_rad(det->azimuth_deg);
                    double z[2], d2, logdet = 0.0;
                    double cost = PWR_ASSOC_INFEASIBLE;

                    z[0] = det->range_m * sin(az);
                    z[1] = det->range_m * cos(az);

                    /* Cheap rectangular pre-gate before the quadratic form. */
                    if (fabs(z[0] - tk->X[0]) <= cfg->gate_max_range_m &&
                        fabs(z[1] - tk->X[1]) <= cfg->gate_max_range_m)
                    {
                        pwr_meas_cov(det->range_m, det->azimuth_deg,
                                     cfg->meas_sigma_range_m,
                                     cfg->meas_sigma_azimuth_deg, R2);
                        d2 = pwr_kf_gate(tk, z, R2, NULL, &logdet);
                        if (d2 >= 0.0 && d2 <= gate2)
                        {
                            int ok = 1;
                            if (cfg->use_doppler_in_gate != 0)
                            {
                                /* Predicted range rate along the plot's line
                                 * of sight versus the measured one.  The
                                 * measurement arrives folded into the
                                 * unambiguous interval, so the comparison is
                                 * taken modulo 2*Vua: without the fold, a
                                 * converged track on a target whose true
                                 * range rate crosses the ambiguity boundary
                                 * rejects every subsequent plot and starves. */
                                const double ux = sin(az), uy = cos(az);
                                const double pred = tk->X[2] * ux + tk->X[3] * uy;
                                const double vua2 =
                                    2.0 * e->dm.unambiguous_velocity_mps;
                                const double lim = cfg->gate_sigma *
                                    (cfg->meas_sigma_velocity_mps +
                                     sqrt(pwr_maxd(tk->P[10] + tk->P[15], 0.0)));
                                double dv = pred - det->radial_velocity_mps;
                                if (vua2 > 0.0)
                                {
                                    dv -= vua2 * floor(dv / vua2 + 0.5);
                                }
                                if (fabs(dv) > lim) { ok = 0; }
                            }
                            if (ok != 0) { cost = d2 + logdet; }
                        }
                    }
                    tr->cost[(size_t)i * nd + c] = cost;
                }
            }

            /* The exact solver requires rows <= cols.  When there are more
             * in-beam tracks than plots the greedy solver takes over: at these
             * sizes (a handful of each) the two agree almost always, and the
             * difference costs at most one deferred association. */
            if (cfg->assoc_mode == PWR_ASSOC_GLOBAL && n_cand <= nd)
            {
                pwr_assign_jv(tr->cost, n_cand, nd, nd, tr->row_assign,
                              tr->dual_u, tr->dual_v, tr->jv_minv,
                              tr->jv_way, tr->jv_pcol, tr->jv_used);
            }
            else
            {
                /* Greedy nearest neighbour: repeatedly take the globally
                 * cheapest feasible pair. */
                for (;;)
                {
                    double best = PWR_ASSOC_REJECT;
                    uint32_t bi = 0u, bj = 0u;
                    int found = 0;
                    for (i = 0u; i < n_cand; ++i)
                    {
                        if (tr->row_assign[i] >= 0) { continue; }
                        for (c = 0u; c < nd; ++c)
                        {
                            if (tr->col_assign[c] >= 0) { continue; }
                            if (tr->cost[(size_t)i * nd + c] < best)
                            {
                                best = tr->cost[(size_t)i * nd + c];
                                bi = i; bj = c; found = 1;
                            }
                        }
                    }
                    if (found == 0) { break; }
                    tr->row_assign[bi] = (int32_t)bj;
                    tr->col_assign[bj] = (int32_t)bi;
                }
            }

            /* Drop assignments that landed on an infeasible pair and rebuild
             * the column map from the (possibly JV-produced) row map. */
            for (c = 0u; c < nd; ++c) { tr->col_assign[c] = -1; }
            for (i = 0u; i < n_cand; ++i)
            {
                const int32_t j = tr->row_assign[i];
                if (j < 0) { continue; }
                if (tr->cost[(size_t)i * nd + (uint32_t)j] >= PWR_ASSOC_REJECT)
                {
                    tr->row_assign[i] = -1;
                    continue;
                }
                tr->col_assign[j] = (int32_t)i;
            }
        }

        /* ---- 5. plot association bookkeeping ----------------------------- */
        for (i = 0u; i < n_cand; ++i)
        {
            PWR_TrackInternal* tk = &tr->tracks[tr->cand[i]];
            const int32_t j = (nd > 0u) ? tr->row_assign[i] : -1;

            if (j >= 0)
            {
                const PWR_Detection* det = &e->detections[j];
                const double az = pwr_deg_to_rad(det->azimuth_deg);
                double z[2];
                double d2, Sinv[4];

                z[0] = det->range_m * sin(az);
                z[1] = det->range_m * cos(az);
                pwr_meas_cov(det->range_m, det->azimuth_deg,
                             cfg->meas_sigma_range_m,
                             cfg->meas_sigma_azimuth_deg, R2);
                d2 = pwr_kf_gate(tk, z, R2, Sinv, NULL);
                tk->innovation_norm = (float)((d2 > 0.0) ? sqrt(d2) : 0.0);
                pwr_kf_update(tk, z, R2);

                /* Label the consumed plot so the console can show which
                 * detection updated which track this dwell. */
                e->detections[j].assoc_track_id = (int32_t)tk->id;
                tk->dwell_state = PWR_DWELL_HIT;

                if (rotating != 0 &&
                    now - tk->last_attempt_time_s < 0.5 * scan_period &&
                    (tk->history_bits & 1u) == 0u)
                {
                    /* This revolution's attempt was already booked as a miss:
                     * the crossing fired on a lagging prediction before the
                     * dwell plot could be emitted.  Repair the books instead
                     * of losing a real hit to the second-crossing dedupe. */
                    tk->history_bits |= 1u;
                    if (tk->misses > 0u)             { --tk->misses; }
                    if (tk->consecutive_misses > 0u) { --tk->consecutive_misses; }
                    {
                        const uint32_t mask = (cfg->confirm_n >= 32)
                            ? 0xFFFFFFFFu
                            : ((1u << (uint32_t)cfg->confirm_n) - 1u);
                        if (tk->state == PWR_TRACK_TENTATIVE &&
                            pwr_popcount32(tk->history_bits & mask) >=
                                (uint32_t)cfg->confirm_m)
                        {
                            tk->state = PWR_TRACK_CONFIRMED;
                        }
                    }
                }
                else
                {
                    tk->hit_this_scan = 1;
                }

                ++tk->hits;
                tk->consecutive_misses = 0u;
                tk->snr_db = (float)(0.7 * (double)tk->snr_db +
                                     0.3 * (double)det->snr_db);
                tk->last_meas_time_s   = now;
                tk->last_update_time_s = now;
                tk->radial_velocity_mps = det->radial_velocity_mps;
                if (tk->state == PWR_TRACK_COASTING)
                {
                    tk->state = PWR_TRACK_CONFIRMED;
                }
            }

            if (rotating == 0)
            {
                /* Staring antenna: every CPI is an independent dwell, so the
                 * M-of-N attempt is booked right here, as it always was. */
                pwr_track_book_attempt(tr, tk, cfg, (j >= 0) ? 1 : 0);
                tk->hit_this_scan = 0;
            }

            if (pwr_track_is_live(tk) != 0)
            {
                const double sp = sqrt(tk->X[2] * tk->X[2] + tk->X[3] * tk->X[3]);
                tk->target_class = pwr_classify(sp);
                pwr_trail_push(tk, now);
            }
        }

        /* ---- 5b. per-scan attempt booking (rotating antenna) --------------
         *  Dwell-merged plots arrive only after the beam has left the target,
         *  so an attempt cannot be judged per CPI of illumination.  Instead
         *  one attempt is booked per revolution, at the moment the boresight
         *  sweeps LAG degrees past the track's azimuth - by which time the
         *  dwell plot, if the target produced one, has been emitted and
         *  associated (raising hit_this_scan). */
        if (rotating != 0)
        {
            const double deg_per_cpi = 360.0 * e->dm.cpi_duration_s / scan_period;
            const double lag   = 1.6 * e->cfg.azimuth_beamwidth_deg +
                                 2.0 * deg_per_cpi;
            const double prev  = tr->last_beam_az_deg;
            const double sweep = pwr_wrap360(beam - prev);

            for (i = 0u; i < PWR_MAX_TRACKS; ++i)
            {
                PWR_TrackInternal* tk = &tr->tracks[i];
                double gate_az, d1;
                int crossed, overdue;
                if (pwr_track_is_live(tk) == 0) { continue; }
                gate_az = pwr_wrap360(
                    pwr_rad_to_deg(atan2(tk->X[0], tk->X[1])) + lag);
                d1 = pwr_wrap360(gate_az - prev);
                crossed = (d1 > 1e-9 && d1 <= sweep) ? 1 : 0;
                /* The geometric crossing can be hopped over by a gate moving
                 * against the rotation (retrograde apparent motion) or by a
                 * KF azimuth jump; a time-based catch-up guarantees roughly
                 * one attempt per revolution regardless. */
                overdue = (now - tk->last_attempt_time_s > 1.25 * scan_period)
                        ? 1 : 0;
                if (crossed == 0 && overdue == 0) { continue; }
                if (crossed != 0 &&
                    now - tk->last_attempt_time_s < 0.5 * scan_period)
                {
                    /* Second crossing of the same revolution (initiation, a
                     * repair, or a KF azimuth jump): already on the books. */
                    tk->hit_this_scan = 0;
                    continue;
                }
                pwr_track_book_attempt(tr, tk, cfg,
                                       (tk->hit_this_scan != 0) ? 1 : 0);
                tk->hit_this_scan       = 0;
                tk->last_attempt_time_s = now;
                if (tk->state != PWR_TRACK_FREE)
                {
                    const double sp = sqrt(tk->X[2] * tk->X[2] +
                                           tk->X[3] * tk->X[3]);
                    tk->target_class = pwr_classify(sp);
                    pwr_trail_push(tk, now);
                }
            }
            tr->last_beam_az_deg = beam;
        }

        /* ---- 6. initiate tracks from unassigned plots --------------------
         *  A plot inside the initiation-inhibit radius of any live track is
         *  discarded rather than promoted.  A strong target routinely yields
         *  more than one plot per dwell (sidelobe residue, an extended
         *  scatterer, a split cluster) and only one of them can be assigned;
         *  without this guard every extra plot would seed a duplicate track. */
        {
            const double inhibit = (double)cfg->init_inhibit_m;
            const double inhibit2 = inhibit * inhibit;
            for (c = 0u; c < nd; ++c)
            {
                const PWR_Detection* det = &e->detections[c];
                double zx, zy;
                int blocked = 0;

                if (tr->col_assign[c] >= 0) { continue; }
                if (det->snr_db < e->cfg.cluster.min_snr_db) { continue; }

                zx = det->range_m * sin(pwr_deg_to_rad(det->azimuth_deg));
                zy = det->range_m * cos(pwr_deg_to_rad(det->azimuth_deg));
                if (inhibit > 0.0)
                {
                    for (i = 0u; i < PWR_MAX_TRACKS; ++i)
                    {
                        const PWR_TrackInternal* tk = &tr->tracks[i];
                        double dx, dy;
                        if (pwr_track_is_live(tk) == 0) { continue; }
                        dx = zx - tk->X[0];
                        dy = zy - tk->X[1];
                        if (dx * dx + dy * dy < inhibit2) { blocked = 1; break; }
                    }
                }
                if (blocked == 0)
                {
                    const uint32_t nid =
                        pwr_track_init_from_detection(tr, det, cfg, now);
                    if (nid != 0u)
                    {
                        e->detections[c].assoc_track_id = (int32_t)nid;
                    }
                }
            }
        }
    }
}
