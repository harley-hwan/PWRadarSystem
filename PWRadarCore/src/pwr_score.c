/* Truth-versus-track scoring.
 *
 * Every active simulated target is paired with the nearest published track
 * inside a fixed gate, and the per-target bookkeeping follows from that: how
 * much of the target's life a track held it (completeness), how far the track
 * sat from the truth (RMS error), and how long the first hold took
 * (time-to-track).  The same pass yields the two whole-picture counts a track
 * file is judged on - confirmed tracks that no target claims (spurious) and
 * extra confirmed tracks sitting on a target another track already holds
 * (redundant, which is the split-track failure the initiation-inhibit radius
 * exists to prevent).
 *
 * Pairing is greedy rather than optimal on purpose. A scorer that solved the
 * assignment problem would flatter the tracker exactly where it is weakest:
 * when two tracks have swapped targets, greedy pairing charges the swap and an
 * optimal assignment hides it.
 *
 * The scorer is driven by published frames alone and holds no reference to the
 * engine, so a recording could be scored the same way. It integrates on the
 * frame's own scenario clock rather than on a caller-supplied step, which is
 * what makes a repeated frame free and a paused engine cost nothing.
 */
#include "pwr_core.h"

#include <string.h>

/* Generous next to the 0.5 deg azimuth accuracy the tracker is configured for:
 * at 20 km a beamwidth is 560 m across, so a pairing radius smaller than this
 * would score a correctly held target as lost whenever it crossed a gate. */
#define PWR_SCORE_DEFAULT_GATE_M   1500.0
/* A frame further ahead than this is a discontinuity (a scenario reload or a
 * long pause), not elapsed track time, and must not be integrated. */
#define PWR_SCORE_MAX_STEP_S       2.0

static void pwr_score_clear_slot(PWR_TargetScore* t, int32_t id)
{
    memset(t, 0, sizeof(*t));
    t->truth_id      = id;
    t->first_seen_s  = -1.0;
    t->first_track_s = -1.0;
    t->err_now_m     = -1.0;
}

PWR_EXPORT(PWR_Status) pwr_scorer_init(PWR_Scorer* s, double gate_m)
{
    uint32_t i;
    if (s == NULL) { return PWR_ERR_NULL_POINTER; }
    memset(s, 0, sizeof(*s));
    s->gate_m = (gate_m > 0.0) ? gate_m : PWR_SCORE_DEFAULT_GATE_M;
    for (i = 0u; i < PWR_MAX_SIM_TARGETS; ++i)
    {
        pwr_score_clear_slot(&s->targets[i], 0);
    }
    return PWR_STATUS_OK;
}

/* Existing slot for this target id, or a fresh one.  NULL when the table is
 * full, which cannot happen while the truth list is capped at the same size. */
static PWR_TargetScore* pwr_score_slot(PWR_Scorer* s, int32_t id)
{
    uint32_t i;
    for (i = 0u; i < PWR_MAX_SIM_TARGETS; ++i)
    {
        if (s->targets[i].truth_id == id) { return &s->targets[i]; }
    }
    for (i = 0u; i < PWR_MAX_SIM_TARGETS; ++i)
    {
        if (s->targets[i].truth_id == 0)
        {
            pwr_score_clear_slot(&s->targets[i], id);
            return &s->targets[i];
        }
    }
    return NULL;
}

PWR_EXPORT(PWR_Status) pwr_scorer_update(PWR_Scorer* s, const PWR_Frame* f)
{
    uint8_t  used[PWR_MAX_TRACKS];
    double   dt;
    uint32_t i, k;

    if (s == NULL || f == NULL) { return PWR_ERR_NULL_POINTER; }
    if (f->truth == NULL || f->tracks == NULL) { return PWR_ERR_INVALID_STATE; }

    /* Re-scoring the same frame would double-count every integral. */
    if (s->started != 0 && f->sequence == s->last_sequence)
    {
        return PWR_STATUS_NO_DATA;
    }

    /* Elapsed scenario time since the last scored frame.  A backward or
     * implausibly large step means the engine was reset or the scenario
     * reloaded, so the run restarts rather than accumulating across the seam. */
    dt = (s->started != 0) ? (f->time_s - s->last_time_s) : 0.0;
    if (dt < 0.0 || dt > PWR_SCORE_MAX_STEP_S)
    {
        const double gate = s->gate_m;
        (void)pwr_scorer_init(s, gate);
        dt = 0.0;
    }
    s->started       = 1;
    s->last_time_s   = f->time_s;
    s->last_sequence = f->sequence;

    memset(used, 0, sizeof(used));
    memset(&s->summary, 0, sizeof(s->summary));

    /* Reclaim slots whose target no longer exists: the published truth list
     * always carries every current target, enabled or not, so absence from it
     * means the target list was cleared or a new scenario was loaded. */
    for (i = 0u; i < PWR_MAX_SIM_TARGETS; ++i)
    {
        int present = 0;
        if (s->targets[i].truth_id == 0) { continue; }
        for (k = 0u; k < f->truth_count; ++k)
        {
            if (f->truth[k].id == s->targets[i].truth_id) { present = 1; break; }
        }
        if (present == 0) { pwr_score_clear_slot(&s->targets[i], 0); }
    }

    for (i = 0u; i < f->truth_count; ++i)
    {
        const PWR_SimTarget* tg = &f->truth[i];
        PWR_TargetScore* sc = pwr_score_slot(s, tg->id);
        double best = s->gate_m;
        int32_t bi = -1;

        if (sc == NULL) { continue; }
        memcpy(sc->label, tg->label, sizeof(sc->label));
        sc->label[PWR_LABEL_LEN - 1u] = '\0';
        sc->active = tg->enabled;
        if (tg->enabled == 0)
        {
            sc->paired_track_id = 0;
            sc->err_now_m       = -1.0;
            continue;
        }
        ++s->summary.truth_active;
        if (sc->first_seen_s < 0.0) { sc->first_seen_s = f->time_s; }
        sc->time_active_s += dt;

        /* Only a declared track counts as holding the target.  A tentative
         * track is internal state, not an output: it has not passed the M-of-N
         * test, it carries no symbology on the display and it is retired
         * within about one revisit.  Pairing against it would report a target
         * as tracked from the first plot and make both completeness and
         * time-to-track meaningless as acceptance figures.  A coasting track
         * does count - it was declared and is being predicted through a
         * missed dwell, which is exactly the case the measure exists to
         * reward. */
        for (k = 0u; k < f->track_count; ++k)
        {
            double dx, dy, d;
            if (used[k] != 0u) { continue; }
            if (f->tracks[k].state != PWR_TRACK_CONFIRMED &&
                f->tracks[k].state != PWR_TRACK_COASTING) { continue; }
            dx = f->tracks[k].x_m - tg->x_m;
            dy = f->tracks[k].y_m - tg->y_m;
            d  = sqrt(dx * dx + dy * dy);
            if (d < best) { best = d; bi = (int32_t)k; }
        }

        if (bi < 0)
        {
            sc->paired_track_id = 0;
            sc->err_now_m       = -1.0;
        }
        else
        {
            used[bi] = 1u;
            sc->paired_track_id = (int32_t)f->tracks[bi].id;
            sc->err_now_m       = best;
            sc->err_sum2       += best * best;
            ++sc->err_n;
            sc->err_rms_m       = sqrt(sc->err_sum2 / (double)sc->err_n);
            sc->time_tracked_s += dt;
            if (sc->first_track_s < 0.0) { sc->first_track_s = f->time_s; }
            ++s->summary.truth_tracked;

            /* Any further confirmed track inside the same gate is holding a
             * target that is already held. */
            for (k = 0u; k < f->track_count; ++k)
            {
                double dx, dy;
                if (used[k] != 0u) { continue; }
                if (f->tracks[k].state != PWR_TRACK_CONFIRMED) { continue; }
                dx = f->tracks[k].x_m - tg->x_m;
                dy = f->tracks[k].y_m - tg->y_m;
                if (sqrt(dx * dx + dy * dy) < s->gate_m)
                {
                    used[k] = 1u;
                    ++s->summary.redundant;
                }
            }
        }
        sc->completeness = (sc->time_active_s > 0.0)
            ? pwr_clampd(sc->time_tracked_s / sc->time_active_s, 0.0, 1.0)
            : 0.0;
    }

    for (k = 0u; k < f->track_count; ++k)
    {
        if (used[k] == 0u && f->tracks[k].state == PWR_TRACK_CONFIRMED)
        {
            ++s->summary.spurious;
        }
    }
    return PWR_STATUS_OK;
}
