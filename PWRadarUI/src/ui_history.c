/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  File    : ui_history.c
 *  Language: ISO C17
 * ========================================================================== */
#include "ui_history.h"

#include <stdlib.h>
#include <string.h>

#define UI_PATHSET_INITIAL  16
#define UI_PATH_INITIAL     64

/* ==========================================================================
 *  Path sets
 * ========================================================================== */
void ui_pathset_init(UI_PathSet* s)
{
    memset(s, 0, sizeof(*s));
}

void ui_pathset_release(UI_PathSet* s)
{
    int32_t i;
    if (s == NULL) { return; }
    for (i = 0; i < s->count; ++i) { free(s->paths[i].pts); }
    free(s->paths);
    memset(s, 0, sizeof(*s));
}

void ui_pathset_clear(UI_PathSet* s)
{
    int32_t i;
    if (s == NULL) { return; }
    for (i = 0; i < s->count; ++i) { free(s->paths[i].pts); }
    s->count = 0;                   /* the paths array itself is reused */
}

void ui_pathset_begin_frame(UI_PathSet* s)
{
    int32_t i;
    for (i = 0; i < s->count; ++i) { s->paths[i].alive = 0; }
}

static UI_Path* ui_pathset_get(UI_PathSet* s, int32_t key)
{
    int32_t i;
    for (i = 0; i < s->count; ++i)
    {
        if (s->paths[i].key == key) { return &s->paths[i]; }
    }
    if (s->count == s->cap)
    {
        const int32_t ncap = (s->cap > 0) ? (s->cap * 2) : UI_PATHSET_INITIAL;
        UI_Path* np = (UI_Path*)realloc(s->paths,
                                        (size_t)ncap * sizeof(UI_Path));
        if (np == NULL) { return NULL; }
        s->paths = np;
        s->cap   = ncap;
    }
    {
        UI_Path* p = &s->paths[s->count++];
        memset(p, 0, sizeof(*p));
        p->key = key;
        return p;
    }
}

void ui_pathset_point(UI_PathSet* s, int32_t key, int32_t cls, int32_t state,
                      float x_m, float y_m, double t_s, double min_step_m)
{
    UI_Path* p = ui_pathset_get(s, key);
    if (p == NULL) { return; }
    p->cls   = cls;
    p->state = state;
    p->alive = 1;

    if (p->count > 0)
    {
        UI_PathPoint* tip = &p->pts[p->count - 1];
        const double dx = (double)x_m - (double)tip->x_m;
        const double dy = (double)y_m - (double)tip->y_m;
        if (dx * dx + dy * dy < min_step_m * min_step_m)
        {
            /* Not far enough to matter: keep the tip current instead. */
            tip->x_m = x_m;
            tip->y_m = y_m;
            tip->t_s = t_s;
            return;
        }
    }
    if (p->count == p->cap)
    {
        if (p->cap >= UI_PATH_MAX_POINTS)
        {
            const int32_t drop = p->cap / 4;
            memmove(p->pts, p->pts + drop,
                    (size_t)(p->count - drop) * sizeof(UI_PathPoint));
            p->count -= drop;
        }
        else
        {
            const int32_t ncap = (p->cap > 0) ? (p->cap * 2) : UI_PATH_INITIAL;
            UI_PathPoint* np = (UI_PathPoint*)realloc(
                p->pts, (size_t)ncap * sizeof(UI_PathPoint));
            if (np == NULL) { return; }
            p->pts = np;
            p->cap = ncap;
        }
    }
    p->pts[p->count].x_m = x_m;
    p->pts[p->count].y_m = y_m;
    p->pts[p->count].t_s = t_s;
    ++p->count;
}

void ui_pathset_prune(UI_PathSet* s, double now_s, double retain_s)
{
    const double cutoff = now_s - retain_s;
    int32_t i = 0;
    while (i < s->count)
    {
        UI_Path* p = &s->paths[i];
        int32_t k = 0;
        while (k < p->count && p->pts[k].t_s < cutoff) { ++k; }
        if (k > 0)
        {
            memmove(p->pts, p->pts + k,
                    (size_t)(p->count - k) * sizeof(UI_PathPoint));
            p->count -= k;
        }
        if (p->count == 0 && p->alive == 0)
        {
            free(p->pts);
            s->paths[i] = s->paths[s->count - 1];
            --s->count;
            continue;                /* re-examine the swapped-in slot */
        }
        ++i;
    }
}

/* ==========================================================================
 *  Plot memory
 * ========================================================================== */
void ui_plot_history_init(UI_PlotHistory* h)
{
    memset(h, 0, sizeof(*h));
}

void ui_plot_history_release(UI_PlotHistory* h)
{
    if (h == NULL) { return; }
    free(h->marks);
    memset(h, 0, sizeof(*h));
}

void ui_plot_history_clear(UI_PlotHistory* h)
{
    if (h != NULL) { h->count = 0; }
}

void ui_plot_history_push(UI_PlotHistory* h, float x_m, float y_m, double t_s,
                          float snr_db, int32_t assoc_track_id)
{
    if (h->marks == NULL)
    {
        h->marks = (UI_PlotMark*)malloc((size_t)UI_PLOT_HIST_CAP *
                                        sizeof(UI_PlotMark));
        if (h->marks == NULL) { return; }
        h->cap   = UI_PLOT_HIST_CAP;
        h->count = 0;
    }
    if (h->count == h->cap)
    {
        const int32_t drop = h->cap / 4;
        memmove(h->marks, h->marks + drop,
                (size_t)(h->count - drop) * sizeof(UI_PlotMark));
        h->count -= drop;
    }
    h->marks[h->count].x_m            = x_m;
    h->marks[h->count].y_m            = y_m;
    h->marks[h->count].t_s            = t_s;
    h->marks[h->count].snr_db         = snr_db;
    h->marks[h->count].assoc_track_id = assoc_track_id;
    ++h->count;
}

void ui_plot_history_prune(UI_PlotHistory* h, double now_s, double retain_s)
{
    const double cutoff = now_s - retain_s;
    int32_t k = 0;
    if (h == NULL || h->count == 0) { return; }
    while (k < h->count && h->marks[k].t_s < cutoff) { ++k; }
    if (k > 0)
    {
        memmove(h->marks, h->marks + k,
                (size_t)(h->count - k) * sizeof(UI_PlotMark));
        h->count -= k;
    }
}
