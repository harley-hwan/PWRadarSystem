/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  File    : ui_ppi.c
 *  Language: ISO C17
 * ========================================================================== */
#include "ui_ppi.h"
#include "ui_plot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_PPI_OUTSIDE 0xFFFFu

static double ui_max_d(double a, double b) { return (a > b) ? a : b; }

/* ==========================================================================
 *  Life cycle
 * ========================================================================== */
void ui_ppi_init(UI_Ppi* p)
{
    memset(p, 0, sizeof(*p));
    p->range_max_m      = 24000.0;
    p->range_max_full_m = 24000.0;
    p->cmap             = UI_CMAP_PHOSPHOR;
    p->video_gain_db    = 0.0;
    p->show_video       = 1;
    p->show_rings       = 1;
    p->show_beam        = 1;
    p->show_detections  = 1;
    p->show_tracks      = 1;
    p->show_trails      = 1;
    p->show_truth       = 0;
    p->show_labels      = 1;
    p->show_gates       = 0;
    p->lut_range_max    = -1.0;
}

void ui_ppi_release(UI_Ppi* p)
{
    free(p->lut_az); p->lut_az = NULL;
    free(p->lut_rg); p->lut_rg = NULL;
    p->lut_w = 0; p->lut_h = 0;
}

void ui_ppi_layout(UI_Ppi* p, UI_Rect frame)
{
    const int32_t side = (frame.w < frame.h) ? frame.w : frame.h;
    p->frame = frame;
    p->box   = ui_rect(frame.x + (frame.w - side) / 2,
                       frame.y + (frame.h - side) / 2, side, side);
    p->cx     = (double)p->box.x + (double)side * 0.5;
    p->cy     = (double)p->box.y + (double)side * 0.5;
    p->radius = (double)side * 0.5 - 22.0;
    if (p->radius < 10.0) { p->radius = 10.0; }
}

/* ==========================================================================
 *  Polar mapping cache
 * ========================================================================== */
static void ui_ppi_build_lut(UI_Ppi* p, int32_t cells, int32_t bins,
                             double range_first_m, double range_step_m)
{
    const int32_t w = p->box.w;
    const int32_t h = p->box.h;
    const double  r_px = p->radius;
    int32_t x, y;

    if (w < 1 || h < 1 || cells < 1 || bins < 1) { return; }

    if (p->lut_w != w || p->lut_h != h)
    {
        free(p->lut_az);
        free(p->lut_rg);
        p->lut_az = (uint16_t*)malloc((size_t)w * (size_t)h * sizeof(uint16_t));
        p->lut_rg = (uint16_t*)malloc((size_t)w * (size_t)h * sizeof(uint16_t));
        p->lut_w = w;
        p->lut_h = h;
        if (p->lut_az == NULL || p->lut_rg == NULL)
        {
            ui_ppi_release(p);
            return;
        }
        p->lut_range_max = -1.0;
    }
    if (p->lut_az == NULL || p->lut_rg == NULL) { return; }

    if (p->lut_range_max == p->range_max_m && p->lut_cells == cells &&
        p->lut_bins == bins && p->lut_heading == p->heading_up_deg)
    {
        return;
    }
    p->lut_range_max = p->range_max_m;
    p->lut_cells     = cells;
    p->lut_bins      = bins;
    p->lut_heading   = p->heading_up_deg;

    for (y = 0; y < h; ++y)
    {
        const double dy = ((double)(p->box.y + y) + 0.5) - p->cy;
        uint16_t* az_row = &p->lut_az[(size_t)(uint32_t)y * (uint32_t)w];
        uint16_t* rg_row = &p->lut_rg[(size_t)(uint32_t)y * (uint32_t)w];
        for (x = 0; x < w; ++x)
        {
            const double dx = ((double)(p->box.x + x) + 0.5) - p->cx;
            const double d  = sqrt(dx * dx + dy * dy);
            double bearing, range_m;
            int32_t ai, ri;

            if (d > r_px)
            {
                az_row[x] = UI_PPI_OUTSIDE;
                rg_row[x] = UI_PPI_OUTSIDE;
                continue;
            }
            /* Screen up is north: bearing = atan2(east, north) with north
             * pointing at -y. */
            bearing = atan2(dx, -dy) * 180.0 / UI_PI + p->heading_up_deg;
            while (bearing < 0.0)    { bearing += 360.0; }
            while (bearing >= 360.0) { bearing -= 360.0; }
            range_m = d / r_px * p->range_max_m;

            ai = (int32_t)(bearing / 360.0 * (double)cells + 0.5);
            if (ai >= cells) { ai -= cells; }
            if (ai < 0) { ai = 0; }

            ri = (range_step_m > 0.0)
                ? (int32_t)((range_m - range_first_m) / range_step_m + 0.5) : 0;
            if (ri < 0 || ri >= bins)
            {
                az_row[x] = UI_PPI_OUTSIDE;
                rg_row[x] = UI_PPI_OUTSIDE;
                continue;
            }
            az_row[x] = (uint16_t)ai;
            rg_row[x] = (uint16_t)ri;
        }
    }
}

/* ==========================================================================
 *  Geometry helpers
 * ========================================================================== */
static void ui_ppi_polar_to_px(const UI_Ppi* p, double range_m, double bearing_deg,
                               double* px, double* py)
{
    const double a = (bearing_deg - p->heading_up_deg) * UI_PI / 180.0;
    const double r = (p->range_max_m > 0.0)
        ? (range_m / p->range_max_m * p->radius) : 0.0;
    *px = p->cx + r * sin(a);
    *py = p->cy - r * cos(a);
}

static void ui_ppi_enu_to_px(const UI_Ppi* p, double east_m, double north_m,
                             double* px, double* py)
{
    const double range   = sqrt(east_m * east_m + north_m * north_m);
    const double bearing = atan2(east_m, north_m) * 180.0 / UI_PI;
    ui_ppi_polar_to_px(p, range, bearing, px, py);
}

UI_Color ui_track_colour(int32_t state)
{
    switch (state)
    {
    case PWR_TRACK_CONFIRMED: return UI_C_TRACK_CONF;
    case PWR_TRACK_COASTING:  return UI_C_TRACK_COAST;
    case PWR_TRACK_TENTATIVE: return UI_C_TRACK_TENT;
    default:                  return UI_C_TEXT_DIM;
    }
}

/* ==========================================================================
 *  Input
 * ========================================================================== */
int ui_ppi_input(UI_Context* c, UI_Ppi* p, uint32_t id, const PWR_Frame* f)
{
    const int inside = (ui_popup_open(c) == 0) && (c->mouse_inside != 0) &&
                       ui_rect_contains(p->frame, c->mouse_x, c->mouse_y);
    const double dx = ((double)c->mouse_x + 0.5) - p->cx;
    const double dy = ((double)c->mouse_y + 0.5) - p->cy;
    const double d  = sqrt(dx * dx + dy * dy);
    int changed = 0;

    p->cursor_on = 0;
    if (inside == 0) { return 0; }

    c->hot_next = id;
    c->cursor   = UI_CURSOR_CROSS;

    if (d <= p->radius)
    {
        double b = atan2(dx, -dy) * 180.0 / UI_PI + p->heading_up_deg;
        while (b < 0.0)    { b += 360.0; }
        while (b >= 360.0) { b -= 360.0; }
        p->cursor_on           = 1;
        p->cursor_bearing_deg  = b;
        p->cursor_range_m      = d / p->radius * p->range_max_m;
    }

    /* ---- range scale ---------------------------------------------------- */
    if (c->wheel != 0)
    {
        p->range_max_m *= pow(0.8, (double)c->wheel);
        if (p->range_max_m < 500.0) { p->range_max_m = 500.0; }
        if (p->range_max_m > p->range_max_full_m * 1.05)
        {
            p->range_max_m = p->range_max_full_m;
        }
        c->wheel = 0;
    }
    if (c->double_click != 0)
    {
        p->range_max_m = p->range_max_full_m;
        p->selected_track = 0;
        changed = 1;
    }

    /* ---- track selection ------------------------------------------------ */
    if (c->pressed[UI_MB_LEFT] != 0 && f != NULL && p->show_tracks != 0)
    {
        double best = 18.0;                  /* pick radius in pixels */
        int32_t best_id = 0;
        uint32_t i;
        for (i = 0u; i < f->track_count; ++i)
        {
            double tx, ty, dd;
            ui_ppi_enu_to_px(p, f->tracks[i].x_m, f->tracks[i].y_m, &tx, &ty);
            dd = sqrt((tx - (double)c->mouse_x) * (tx - (double)c->mouse_x) +
                      (ty - (double)c->mouse_y) * (ty - (double)c->mouse_y));
            if (dd < best) { best = dd; best_id = (int32_t)f->tracks[i].id; }
        }
        if (best_id != p->selected_track)
        {
            p->selected_track = best_id;
            changed = 1;
        }
    }
    return changed;
}

/* ==========================================================================
 *  Drawing
 * ========================================================================== */
static void ui_ppi_draw_video(UI_Canvas* cv, UI_Ppi* p, const PWR_Frame* f)
{
    const UI_Color* pal = ui_colormap((UI_ColormapId)p->cmap);
    const UI_Rect q = ui_rect_intersect(p->box, cv->clip);
    const int32_t gain = (int32_t)(p->video_gain_db * 4.0);   /* ~4 LSB per dB */
    int32_t x, y;

    if (p->lut_az == NULL || f == NULL || f->ppi_video == NULL) { return; }
    if (ui_rect_empty(q)) { return; }

    for (y = q.y; y < q.y + q.h; ++y)
    {
        const int32_t ly = y - p->box.y;
        const uint16_t* az_row = &p->lut_az[(size_t)(uint32_t)ly * (uint32_t)p->lut_w];
        const uint16_t* rg_row = &p->lut_rg[(size_t)(uint32_t)ly * (uint32_t)p->lut_w];
        uint32_t* drow = &cv->px[ui_row_at(cv, y)];
        for (x = q.x; x < q.x + q.w; ++x)
        {
            const int32_t lx = x - p->box.x;
            const uint16_t a = az_row[lx];
            if (a == UI_PPI_OUTSIDE) { continue; }
            {
                const uint16_t r = rg_row[lx];
                int32_t v = (int32_t)f->ppi_video[(size_t)a * f->range_bins + r] + gain;
                if (v < 0) { v = 0; } else if (v > 255) { v = 255; }
                drow[x] = pal[v];
            }
        }
    }
}

static void ui_ppi_draw_rings(UI_Canvas* cv, const UI_Ppi* p)
{
    UI_Ticks t;
    int32_t i;
    char buf[24];

    /* Range rings on nice 1-2-5 steps, exactly like the axes tick engine so the
     * PPI and the A-scope always agree on what a "nice" range is. */
    ui_ticks_linear(&t, 0.0, p->range_max_m * 1e-3, 5);
    for (i = 0; i < t.count; ++i)
    {
        const double rkm = t.pos[i];
        const double rr  = rkm * 1000.0 / p->range_max_m * p->radius;
        if (rr < 6.0 || rr > p->radius + 0.5) { continue; }
        ui_frame_circle(cv, p->cx, p->cy, rr, UI_C_PPI_RING, 1.0);
        if (p->show_labels != 0)
        {
            (void)snprintf(buf, sizeof(buf), "%g km", rkm);
            (void)ui_text(cv, UI_FONT_SMALL, (int32_t)(p->cx + 3.0),
                          (int32_t)(p->cy - rr - 3.0), UI_C_PPI_RING, buf);
        }
    }
    ui_frame_circle(cv, p->cx, p->cy, p->radius, UI_C_AXIS, 1.4);

    /* Bearing spokes every 30 degrees, labelled every 30. */
    for (i = 0; i < 12; ++i)
    {
        const double b = (double)i * 30.0;
        double x0, y0, x1, y1;
        ui_ppi_polar_to_px(p, p->range_max_m * 0.06, b, &x0, &y0);
        ui_ppi_polar_to_px(p, p->range_max_m,        b, &x1, &y1);
        ui_line(cv, x0, y0, x1, y1, UI_C_PPI_SPOKE, 1.0);
        if (p->show_labels != 0)
        {
            double lx, ly;
            ui_ppi_polar_to_px(p, p->range_max_m * 1.075, b, &lx, &ly);
            (void)snprintf(buf, sizeof(buf), "%03d", (int)b);
            (void)ui_text_aligned(cv, UI_FONT_SMALL, (int32_t)lx,
                                  (int32_t)ly + ui_font_ascent(UI_FONT_SMALL) / 2,
                                  UI_C_TEXT_FAINT, UI_ALIGN_CENTRE, buf);
        }
    }
    /* Cross hairs through the origin. */
    ui_hline(cv, (int32_t)(p->cx - p->radius), (int32_t)(p->cx + p->radius),
             (int32_t)p->cy, UI_C_PPI_SPOKE);
    ui_vline(cv, (int32_t)p->cx, (int32_t)(p->cy - p->radius),
             (int32_t)(p->cy + p->radius), UI_C_PPI_SPOKE);
}

static void ui_ppi_draw_beam(UI_Canvas* cv, const UI_Ppi* p, const PWR_Frame* f)
{
    /* A ring of discrete rays reads as a fan of spokes, which is not what a
     * scope looks like.  The sweep is drawn instead as a filled wedge whose
     * alpha decays behind the leading edge - the afterglow - with one bright
     * radial at the current boresight. */
    const int32_t steps = 14;
    const double  trail_deg = 16.0;
    float xs[3], ys[3];
    int32_t i;
    double x1, y1;

    if (f == NULL) { return; }

    for (i = 0; i < steps; ++i)
    {
        const double b0 = f->beam_azimuth_deg - trail_deg * (double)i / (double)steps;
        const double b1 = f->beam_azimuth_deg -
                          trail_deg * (double)(i + 1) / (double)steps;
        const double fade = exp(-3.0 * (double)i / (double)steps);
        const uint8_t alpha = (uint8_t)(56.0 * fade);
        double ax, ay, bx, by;
        if (alpha == 0u) { continue; }
        ui_ppi_polar_to_px(p, p->range_max_m, b0, &ax, &ay);
        ui_ppi_polar_to_px(p, p->range_max_m, b1, &bx, &by);
        xs[0] = (float)p->cx; ys[0] = (float)p->cy;
        xs[1] = (float)ax;    ys[1] = (float)ay;
        xs[2] = (float)bx;    ys[2] = (float)by;
        ui_fill_poly(cv, xs, ys, 3, ui_color_alpha(UI_C_BEAM, alpha));
    }
    ui_ppi_polar_to_px(p, p->range_max_m, f->beam_azimuth_deg, &x1, &y1);
    ui_line(cv, p->cx, p->cy, x1, y1, ui_color_alpha(UI_C_BEAM, 210), 1.6);
}

static void ui_ppi_draw_detections(UI_Canvas* cv, const UI_Ppi* p,
                                   const PWR_Frame* f)
{
    uint32_t i;
    if (f == NULL) { return; }
    for (i = 0u; i < f->detection_count; ++i)
    {
        const PWR_Detection* d = &f->detections[i];
        double x, y;
        if (d->range_m > p->range_max_m) { continue; }
        ui_ppi_polar_to_px(p, d->range_m, d->azimuth_deg, &x, &y);
        ui_marker(cv, UI_MARKER_PLUS, x, y, 9.0,
                  ui_color_alpha(UI_C_DETECTION, 220), UI_RGBA(0, 0, 0, 0), 1.3);
    }
}

static void ui_ppi_draw_covariance(UI_Canvas* cv, const UI_Ppi* p,
                                   const PWR_Track* t, UI_Color col)
{
    /* 2-sigma error ellipse from the position covariance, drawn as a polygon of
     * 24 points after the eigen-decomposition of the 2x2 block. */
    const double a = t->pos_cov[0], b = t->pos_cov[1], d = t->pos_cov[3];
    const double tr = a + d;
    const double det = a * d - b * b;
    double disc, l1, l2, th;
    float xs[24], ys[24];
    int32_t i;

    if (!(det > 0.0) || !(tr > 0.0)) { return; }
    disc = sqrt(ui_max_d(0.25 * tr * tr - det, 0.0));
    l1 = 0.5 * tr + disc;
    l2 = 0.5 * tr - disc;
    if (l2 < 0.0) { l2 = 0.0; }
    th = 0.5 * atan2(2.0 * b, a - d);

    for (i = 0; i < 24; ++i)
    {
        const double u = (double)i / 24.0 * 2.0 * UI_PI;
        const double e = 2.0 * sqrt(l1) * cos(u);
        const double n = 2.0 * sqrt(l2) * sin(u);
        const double ex = t->x_m + e * cos(th) - n * sin(th);
        const double ny = t->y_m + e * sin(th) + n * cos(th);
        double px, py;
        ui_ppi_enu_to_px(p, ex, ny, &px, &py);
        xs[i] = (float)px;
        ys[i] = (float)py;
    }
    for (i = 0; i < 24; ++i)
    {
        const int32_t j = (i + 1) % 24;
        ui_line(cv, xs[i], ys[i], xs[j], ys[j], ui_color_alpha(col, 110), 1.0);
    }
}

static void ui_ppi_draw_tracks(UI_Canvas* cv, const UI_Ppi* p, const PWR_Frame* f)
{
    uint32_t i;
    char buf[64];
    if (f == NULL) { return; }

    for (i = 0u; i < f->track_count; ++i)
    {
        const PWR_Track* t = &f->tracks[i];
        const UI_Color col = ui_track_colour(t->state);
        const int sel = ((int32_t)t->id == p->selected_track) ? 1 : 0;
        /* A tentative track is very often a false alarm that will be retired
         * within a scan.  It is still shown - suppressing it would hide the
         * detector's behaviour, which is the whole point of the console - but as
         * a bare dot with no label, leader or trail, so a picture with dozens of
         * them stays readable and the confirmed tracks stay dominant. */
        const int quiet = (t->state == PWR_TRACK_TENTATIVE && sel == 0) ? 1 : 0;
        double x, y;

        if (t->range_m > p->range_max_m * 1.02) { continue; }
        ui_ppi_enu_to_px(p, t->x_m, t->y_m, &x, &y);

        if (quiet != 0)
        {
            ui_marker(cv, UI_MARKER_DOT, x, y, 4.0,
                      ui_color_alpha(col, 190), UI_RGBA(0, 0, 0, 0), 1.0);
            continue;
        }

        /* ---- history trail ---------------------------------------------- */
        if (p->show_trails != 0 && t->trail_count > 1u)
        {
            uint32_t k;
            double px = 0.0, py = 0.0;
            int first = 1;
            for (k = 0u; k < t->trail_count; ++k)
            {
                /* Walk the ring from oldest to newest. */
                const uint32_t idx = (t->trail_head + PWR_TRACK_TRAIL_LEN + 1u + k -
                                      t->trail_count) % PWR_TRACK_TRAIL_LEN;
                double tx, ty;
                ui_ppi_enu_to_px(p, (double)t->trail[idx].x_m,
                                 (double)t->trail[idx].y_m, &tx, &ty);
                if (first == 0)
                {
                    const uint8_t a = (uint8_t)(30.0 + 120.0 * (double)k /
                                                (double)t->trail_count);
                    ui_line(cv, px, py, tx, ty, ui_color_alpha(col, a), 1.2);
                }
                px = tx; py = ty; first = 0;
            }
        }

        /* ---- symbol ----------------------------------------------------- */
        switch (t->target_class)
        {
        case PWR_CLASS_SURFACE:
            ui_marker(cv, UI_MARKER_SQUARE, x, y, sel ? 13.0 : 10.0,
                      col, UI_RGBA(0, 0, 0, 0), 1.6);
            break;
        case PWR_CLASS_MISSILE:
            ui_marker(cv, UI_MARKER_DIAMOND, x, y, sel ? 15.0 : 12.0,
                      col, ui_color_alpha(col, 60), 1.6);
            break;
        case PWR_CLASS_ROTARY:
        case PWR_CLASS_UAV:
            ui_marker(cv, UI_MARKER_TRIANGLE_UP, x, y, sel ? 14.0 : 11.0,
                      col, UI_RGBA(0, 0, 0, 0), 1.6);
            break;
        case PWR_CLASS_AIR:
        default:
            ui_marker(cv, UI_MARKER_CIRCLE, x, y, sel ? 14.0 : 11.0,
                      col, UI_RGBA(0, 0, 0, 0), 1.6);
            break;
        }
        if (t->state == PWR_TRACK_COASTING)
        {
            /* A coasting track is drawn with a dashed ring so the operator can
             * see at a glance that it is dead reckoning. */
            ui_arc(cv, x, y, 9.0, 0.0, UI_PI * 0.5, col, 1.2);
            ui_arc(cv, x, y, 9.0, UI_PI, UI_PI * 1.5, col, 1.2);
        }
        if (sel != 0)
        {
            ui_frame_circle(cv, x, y, 17.0, ui_color_alpha(UI_C_FOCUS, 200), 1.4);
        }

        /* ---- velocity leader (one minute of travel) --------------------- */
        if (t->speed_mps > 0.5 && t->state != PWR_TRACK_TENTATIVE)
        {
            double lx, ly;
            ui_ppi_enu_to_px(p, t->x_m + t->vx_mps * 60.0,
                             t->y_m + t->vy_mps * 60.0, &lx, &ly);
            ui_line(cv, x, y, lx, ly, ui_color_alpha(col, 190), 1.4);
        }

        if (p->show_gates != 0) { ui_ppi_draw_covariance(cv, p, t, col); }

        /* ---- label ------------------------------------------------------ */
        if (p->show_labels != 0)
        {
            (void)snprintf(buf, sizeof(buf), "T%03u", t->id);
            (void)ui_text(cv, UI_FONT_SMALL, (int32_t)x + 12, (int32_t)y - 4,
                          col, buf);
            if (sel != 0)
            {
                (void)snprintf(buf, sizeof(buf), "%.1f km  %.0f\xC2\xB0  %.0f m/s",
                               t->range_m * 1e-3, t->course_deg, t->speed_mps);
                (void)ui_text(cv, UI_FONT_SMALL, (int32_t)x + 12,
                              (int32_t)y + ui_font_line_height(UI_FONT_SMALL) - 4,
                              ui_color_alpha(col, 200), buf);
            }
        }
    }
}

static void ui_ppi_draw_truth(UI_Canvas* cv, const UI_Ppi* p, const PWR_Frame* f)
{
    uint32_t i;
    char buf[40];
    if (f == NULL) { return; }
    for (i = 0u; i < f->truth_count; ++i)
    {
        const PWR_SimTarget* t = &f->truth[i];
        const double range = sqrt(t->x_m * t->x_m + t->y_m * t->y_m);
        double x, y;
        if (t->enabled == 0) { continue; }
        if (range > p->range_max_m) { continue; }
        ui_ppi_enu_to_px(p, t->x_m, t->y_m, &x, &y);
        ui_marker(cv, UI_MARKER_CROSS, x, y, 11.0,
                  ui_color_alpha(UI_C_TRUTH, 190), UI_RGBA(0, 0, 0, 0), 1.2);
        if (p->show_labels != 0 && t->label[0] != '\0')
        {
            (void)snprintf(buf, sizeof(buf), "%s", t->label);
            (void)ui_text(cv, UI_FONT_SMALL, (int32_t)x + 10, (int32_t)y + 12,
                          ui_color_alpha(UI_C_TRUTH, 170), buf);
        }
    }
}

void ui_ppi_draw(UI_Canvas* cv, UI_Ppi* p, const PWR_Frame* f)
{
    ui_clip_push(cv, p->frame);
    ui_fill_rect(cv, p->frame, UI_C_PLOT_BG);

    if (f != NULL && p->show_video != 0)
    {
        ui_ppi_build_lut(p, (int32_t)f->ppi_az_cells, (int32_t)f->range_bins,
                         f->range_first_m, f->range_step_m);
        ui_ppi_draw_video(cv, p, f);
    }
    if (p->show_rings != 0)      { ui_ppi_draw_rings(cv, p); }
    if (p->show_beam != 0)       { ui_ppi_draw_beam(cv, p, f); }
    if (p->show_truth != 0)      { ui_ppi_draw_truth(cv, p, f); }
    if (p->show_detections != 0) { ui_ppi_draw_detections(cv, p, f); }
    if (p->show_tracks != 0)     { ui_ppi_draw_tracks(cv, p, f); }

    /* ---- cursor readout ------------------------------------------------- */
    if (p->cursor_on != 0)
    {
        char buf[64];
        double x, y;
        int32_t tw;
        ui_ppi_polar_to_px(p, p->cursor_range_m, p->cursor_bearing_deg, &x, &y);
        ui_line(cv, p->cx, p->cy, x, y, ui_color_alpha(UI_C_CURSOR, 90), 1.0);
        ui_frame_circle(cv, x, y, 5.0, UI_C_CURSOR, 1.2);
        (void)snprintf(buf, sizeof(buf), "%.2f km / %05.1f\xC2\xB0",
                       p->cursor_range_m * 1e-3, p->cursor_bearing_deg);
        tw = ui_text_width(UI_FONT_MONO, buf);
        ui_fill_round_rect(cv, ui_rect(p->frame.x + 6,
                                       p->frame.y + p->frame.h - 24,
                                       tw + 10, 19), 2.0,
                           UI_RGBA(0x10, 0x14, 0x1A, 215));
        (void)ui_text(cv, UI_FONT_MONO, p->frame.x + 11,
                      p->frame.y + p->frame.h - 24 + 3 +
                      ui_font_ascent(UI_FONT_MONO), UI_C_CURSOR, buf);
    }

    /* ---- corner annotations --------------------------------------------- */
    if (f != NULL)
    {
        char buf[80];
        (void)snprintf(buf, sizeof(buf), "PPI   scale %.0f km   %s",
                       p->range_max_m * 1e-3,
                       ui_colormap_name((UI_ColormapId)p->cmap));
        (void)ui_text(cv, UI_FONT_SMALL, p->frame.x + 6,
                      p->frame.y + ui_font_ascent(UI_FONT_SMALL) + 4,
                      UI_C_TEXT_DIM, buf);
        (void)snprintf(buf, sizeof(buf), "AZ %06.2f\xC2\xB0   scan %llu",
                       f->beam_azimuth_deg,
                       (unsigned long long)f->stats.scan_count);
        (void)ui_text_aligned(cv, UI_FONT_MONO,
                              p->frame.x + p->frame.w - 6,
                              p->frame.y + ui_font_ascent(UI_FONT_MONO) + 4,
                              UI_C_BEAM, UI_ALIGN_RIGHT, buf);
    }
    ui_clip_pop(cv);
}
