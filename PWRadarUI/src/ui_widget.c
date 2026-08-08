/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  File    : ui_widget.c
 *  Language: ISO C17
 * ========================================================================== */
#include "ui_widget.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 *  Identity
 * ========================================================================== */
uint32_t ui_id(const char* label, int32_t index)
{
    uint32_t h = 2166136261u;
    if (label != NULL)
    {
        while (*label != '\0')
        {
            h ^= (uint32_t)(unsigned char)*label++;
            h *= 16777619u;
        }
    }
    h ^= (uint32_t)index + 0x9E3779B9u;
    h *= 16777619u;
    return (h == 0u) ? 1u : h;
}

/* ==========================================================================
 *  Frame plumbing
 * ========================================================================== */
void ui_ctx_init(UI_Context* c)
{
    memset(c, 0, sizeof(*c));
    c->cursor       = UI_CURSOR_ARROW;
    c->popup_sel    = -1;
    c->popup_hover  = -1;
    c->mouse_inside = 1;
}

void ui_ctx_event(UI_Context* c, const UI_Event* ev)
{
    switch (ev->type)
    {
    case UI_EV_MOUSE_MOVE:
        c->mouse_x = ev->x;
        c->mouse_y = ev->y;
        c->mods    = ev->mods;
        c->mouse_inside = 1;
        break;

    case UI_EV_MOUSE_LEAVE:
        c->mouse_inside = 0;
        break;

    case UI_EV_MOUSE_DOWN:
        if (ev->button >= 0 && ev->button < 3)
        {
            c->down[ev->button]    = 1;
            c->pressed[ev->button] = 1;
        }
        c->mouse_x = ev->x;
        c->mouse_y = ev->y;
        c->press_x = ev->x;
        c->press_y = ev->y;
        c->mods    = ev->mods;
        if (ev->wheel == 2) { c->double_click = 1; }
        break;

    case UI_EV_MOUSE_UP:
        if (ev->button >= 0 && ev->button < 3)
        {
            c->down[ev->button]     = 0;
            c->released[ev->button] = 1;
        }
        c->mouse_x = ev->x;
        c->mouse_y = ev->y;
        c->mods    = ev->mods;
        break;

    case UI_EV_WHEEL:
        c->wheel  += ev->wheel;
        c->mouse_x = ev->x;
        c->mouse_y = ev->y;
        c->mods    = ev->mods;
        break;

    case UI_EV_KEY_DOWN:
        if (c->n_keys < UI_MAX_KEY_QUEUE) { c->keys[c->n_keys++] = ev->key; }
        c->mods = ev->mods;
        break;

    case UI_EV_TEXT:
        if (c->n_text < UI_MAX_TEXT_QUEUE)
        {
            c->text[c->n_text++] = ev->codepoint;
        }
        break;

    default:
        break;
    }
}

/* Forward declaration: the popup is serviced at frame start, drawn at end. */
static void ui_popup_input(UI_Context* c);
static void ui_popup_draw(UI_Context* c);

void ui_frame_begin(UI_Context* c, uint32_t* px, int32_t w, int32_t h,
                    int32_t stride, double now_s)
{
    ui_canvas_bind(&c->canvas, px, w, h, stride);
    c->dt_s   = (c->time_s > 0.0) ? (now_s - c->time_s) : 0.0;
    c->time_s = now_s;

    c->mouse_dx = c->mouse_x - c->mouse_prev_x;
    c->mouse_dy = c->mouse_y - c->mouse_prev_y;

    c->hot            = c->hot_next;
    c->hot_next       = 0u;
    c->cursor         = UI_CURSOR_ARROW;
    c->mouse_captured = 0;
    c->popup_changed  = 0;
    c->popup_closed   = 0u;

    /* The drop-down is modal: it consumes the pointer before any widget can
     * claim it, using the geometry it was drawn with last frame. */
    if (c->popup_id != 0u) { ui_popup_input(c); }
}

void ui_frame_end(UI_Context* c)
{
    if (c->popup_id != 0u) { ui_popup_draw(c); }

    c->mouse_prev_x = c->mouse_x;
    c->mouse_prev_y = c->mouse_y;
    memset(c->pressed,  0, sizeof(c->pressed));
    memset(c->released, 0, sizeof(c->released));
    c->double_click = 0;
    c->wheel   = 0;
    c->n_keys  = 0;
    c->n_text  = 0;
    if (c->down[0] == 0 && c->down[1] == 0 && c->down[2] == 0) { c->active = 0u; }
}

int ui_popup_open(const UI_Context* c) { return (c->popup_id != 0u) ? 1 : 0; }

/* --------------------------------------------------------------------------
 *  Shared interaction helper
 * ------------------------------------------------------------------------ */
static int ui_hit(UI_Context* c, UI_Rect r)
{
    if (c->popup_id != 0u) { return 0; }
    if (c->mouse_inside == 0) { return 0; }
    if (!ui_rect_contains(ui_rect_intersect(r, c->canvas.clip),
                          c->mouse_x, c->mouse_y)) { return 0; }
    return 1;
}

/* Returns: 0 idle, 1 hovered, 2 held, 3 clicked (released inside). */
static int ui_behave(UI_Context* c, uint32_t id, UI_Rect r)
{
    const int inside = ui_hit(c, r);
    int result = 0;

    if (c->active == id)
    {
        c->mouse_captured = 1;
        result = 2;
        if (c->released[UI_MB_LEFT] != 0)
        {
            if (inside != 0) { result = 3; }
            c->active = 0u;
        }
    }
    else if (inside != 0 && c->active == 0u)
    {
        c->hot_next = id;
        result = 1;
        if (c->pressed[UI_MB_LEFT] != 0)
        {
            c->active = id;
            c->focus  = id;
            result = 2;
        }
    }
    return result;
}

static int ui_key_pressed(const UI_Context* c, int32_t key)
{
    int32_t i;
    for (i = 0; i < c->n_keys; ++i) { if (c->keys[i] == key) { return 1; } }
    return 0;
}

/* ==========================================================================
 *  Chrome
 * ========================================================================== */
void ui_panel(UI_Context* c, UI_Rect r)
{
    ui_fill_rect(&c->canvas, r, UI_C_PANEL);
    ui_frame_rect(&c->canvas, r, UI_C_BORDER);
}

UI_Rect ui_group(UI_Context* c, UI_Rect r, const char* title)
{
    const int32_t hdr = UI_M_HEADER_H;
    ui_fill_round_rect(&c->canvas, r, UI_M_RADIUS, UI_C_PANEL);
    ui_fill_gradient_v(&c->canvas, ui_rect(r.x + 1, r.y + 1, r.w - 2, hdr - 1),
                       UI_C_PANEL_HI, UI_C_PANEL);
    ui_hline(&c->canvas, r.x + 1, r.x + r.w - 2, r.y + hdr, UI_C_BORDER);
    ui_frame_round_rect(&c->canvas, r, UI_M_RADIUS, UI_C_BORDER);
    if (title != NULL)
    {
        ui_text(&c->canvas, UI_FONT_BOLD, r.x + UI_M_PAD,
                r.y + (hdr + ui_font_ascent(UI_FONT_BOLD)) / 2 - 1,
                UI_C_TEXT, title);
    }
    return ui_rect(r.x + UI_M_PAD, r.y + hdr + UI_M_GAP,
                   r.w - 2 * UI_M_PAD,
                   (r.h - hdr - UI_M_GAP - UI_M_PAD > 0)
                       ? (r.h - hdr - UI_M_GAP - UI_M_PAD) : 0);
}

void ui_label(UI_Context* c, UI_Rect r, const char* s)
{
    (void)ui_text_in_rect(&c->canvas, UI_FONT_BODY, r, 0, UI_C_TEXT,
                          UI_ALIGN_LEFT, s);
}

void ui_label_dim(UI_Context* c, UI_Rect r, const char* s)
{
    (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL, r, 0, UI_C_TEXT_DIM,
                          UI_ALIGN_LEFT, s);
}

void ui_readout(UI_Context* c, UI_Rect r, const char* label, const char* value,
                UI_Color value_col)
{
    (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL, r, 0, UI_C_TEXT_DIM,
                          UI_ALIGN_LEFT, label);
    (void)ui_text_in_rect(&c->canvas, UI_FONT_MONO, r, 0, value_col,
                          UI_ALIGN_RIGHT, value);
}

void ui_separator(UI_Context* c, UI_Rect r)
{
    ui_hline(&c->canvas, r.x, r.x + r.w - 1, r.y + r.h / 2, UI_C_BORDER);
}

/* ==========================================================================
 *  Buttons
 * ========================================================================== */
static void ui_draw_button_face(UI_Context* c, UI_Rect r, int state,
                                UI_Color on_top, UI_Color on_bot, int is_on)
{
    UI_Color top, bot;
    if (state == 2)
    {
        top = UI_C_CTRL_DOWN; bot = UI_C_CTRL_DOWN;
    }
    else if (is_on != 0)
    {
        top = (state == 1) ? UI_C_CTRL_ON_HOT : on_top;
        bot = (state == 1) ? UI_C_CTRL_ON_HOT : on_bot;
    }
    else if (state == 1)
    {
        top = UI_C_CTRL_HOT_TOP; bot = UI_C_CTRL_HOT_BOT;
    }
    else
    {
        top = UI_C_CTRL_TOP; bot = UI_C_CTRL_BOT;
    }
    ui_fill_round_rect(&c->canvas, r, UI_M_RADIUS, bot);
    ui_fill_gradient_v(&c->canvas, ui_rect_inset(r, 1, 1), top, bot);
    ui_frame_round_rect(&c->canvas, r, UI_M_RADIUS,
                        (state != 0) ? UI_C_BORDER_HI : UI_C_BORDER);
}

int ui_button(UI_Context* c, uint32_t id, UI_Rect r, const char* label)
{
    const int state = ui_behave(c, id, r);
    ui_draw_button_face(c, r, state, UI_C_CTRL_ON, UI_C_CTRL_ON, 0);
    if (state == 1) { c->cursor = UI_CURSOR_HAND; }
    (void)ui_text_in_rect(&c->canvas, UI_FONT_BODY, r, 0, UI_C_TEXT,
                          UI_ALIGN_CENTRE, label);
    return (state == 3) ? 1 : 0;
}

int ui_button_accent(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
                     UI_Color accent)
{
    const int state = ui_behave(c, id, r);
    const UI_Color hot = ui_color_scale(accent, 1.18);
    ui_draw_button_face(c, r, state, (state == 1) ? hot : accent,
                        (state == 1) ? hot : ui_color_scale(accent, 0.86), 1);
    if (state == 1) { c->cursor = UI_CURSOR_HAND; }
    (void)ui_text_in_rect(&c->canvas, UI_FONT_BOLD, r, 0, UI_C_TEXT_ON_ACCENT,
                          UI_ALIGN_CENTRE, label);
    return (state == 3) ? 1 : 0;
}

int ui_toggle(UI_Context* c, uint32_t id, UI_Rect r, const char* label, int32_t* on)
{
    const int state = ui_behave(c, id, r);
    int changed = 0;
    if (state == 3) { *on = (*on != 0) ? 0 : 1; changed = 1; }
    ui_draw_button_face(c, r, (state == 2) ? 2 : state,
                        UI_C_CTRL_ON, ui_color_scale(UI_C_CTRL_ON, 0.85), *on);
    if (state == 1) { c->cursor = UI_CURSOR_HAND; }
    (void)ui_text_in_rect(&c->canvas, UI_FONT_BODY, r, 0,
                          (*on != 0) ? UI_RGB(0xF0, 0xF6, 0xFF) : UI_C_TEXT,
                          UI_ALIGN_CENTRE, label);
    return changed;
}

int ui_checkbox(UI_Context* c, uint32_t id, UI_Rect r, const char* label, int32_t* on)
{
    const int32_t box = 14;
    const UI_Rect br = ui_rect(r.x, r.y + (r.h - box) / 2, box, box);
    const int state = ui_behave(c, id, r);
    int changed = 0;

    if (state == 3 || (c->focus == id && ui_key_pressed(c, UI_KEY_SPACE)))
    {
        *on = (*on != 0) ? 0 : 1;
        changed = 1;
    }
    if (state == 1) { c->cursor = UI_CURSOR_HAND; }

    ui_fill_round_rect(&c->canvas, br, 2.0,
                       (*on != 0) ? UI_C_CTRL_ON : UI_C_EDIT_BG);
    ui_frame_round_rect(&c->canvas, br, 2.0,
                        (state != 0) ? UI_C_BORDER_HI : UI_C_BORDER);
    if (*on != 0)
    {
        /* Tick mark drawn as two antialiased strokes. */
        const double x0 = br.x + 3.0, y0 = br.y + box * 0.55;
        const double x1 = br.x + box * 0.42, y1 = br.y + box - 4.0;
        const double x2 = br.x + box - 3.0, y2 = br.y + 3.5;
        ui_line(&c->canvas, x0, y0, x1, y1, UI_RGB(0xF2, 0xF8, 0xFF), 1.9);
        ui_line(&c->canvas, x1, y1, x2, y2, UI_RGB(0xF2, 0xF8, 0xFF), 1.9);
    }
    if (label != NULL)
    {
        (void)ui_text_in_rect(&c->canvas, UI_FONT_BODY,
                              ui_rect(r.x + box + 7, r.y, r.w - box - 7, r.h),
                              0, UI_C_TEXT, UI_ALIGN_LEFT, label);
    }
    return changed;
}

int ui_radio(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
             int32_t* sel, int32_t value)
{
    const int state = ui_behave(c, id, r);
    const double cx = (double)r.x + 8.0;
    const double cy = (double)r.y + (double)r.h * 0.5;
    int changed = 0;

    if (state == 3 && *sel != value) { *sel = value; changed = 1; }
    if (state == 1) { c->cursor = UI_CURSOR_HAND; }

    ui_fill_circle(&c->canvas, cx, cy, 7.0, UI_C_EDIT_BG);
    ui_frame_circle(&c->canvas, cx, cy, 7.0,
                    (state != 0) ? UI_C_BORDER_HI : UI_C_BORDER, 1.0);
    if (*sel == value) { ui_fill_circle(&c->canvas, cx, cy, 3.6, UI_C_FOCUS); }
    if (label != NULL)
    {
        (void)ui_text_in_rect(&c->canvas, UI_FONT_BODY,
                              ui_rect(r.x + 20, r.y, r.w - 20, r.h),
                              0, UI_C_TEXT, UI_ALIGN_LEFT, label);
    }
    return changed;
}

/* ==========================================================================
 *  Sliders
 * ========================================================================== */
static double ui_slider_to_norm(double v, double lo, double hi, int log_scale)
{
    if (log_scale != 0 && lo > 0.0 && hi > 0.0)
    {
        if (v <= lo) { return 0.0; }
        if (v >= hi) { return 1.0; }
        return log(v / lo) / log(hi / lo);
    }
    if (hi <= lo) { return 0.0; }
    return (v - lo) / (hi - lo);
}

static double ui_slider_from_norm(double t, double lo, double hi, int log_scale)
{
    if (t < 0.0) { t = 0.0; }
    if (t > 1.0) { t = 1.0; }
    if (log_scale != 0 && lo > 0.0 && hi > 0.0)
    {
        return lo * pow(hi / lo, t);
    }
    return lo + (hi - lo) * t;
}

int ui_slider(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
              double* v, double lo, double hi, const char* fmt, int log_scale)
{
    /* The label and value gutters are capped as a fraction of the control, so a
     * slider dropped into a narrow toolbar still gets a usable track instead of
     * being squeezed to a few pixels by fixed insets. */
    const int32_t lab_w = (label != NULL)
        ? ((UI_M_LABEL_W < r.w / 3) ? UI_M_LABEL_W : r.w / 3) : 0;
    const int32_t val_w = (72 < r.w / 4) ? 72 : r.w / 4;
    const UI_Rect track = ui_rect(r.x + lab_w, r.y + r.h / 2 - 3,
                                  r.w - lab_w - val_w, 6);
    const int state = ui_behave(c, id, ui_rect(track.x - 8, r.y,
                                               track.w + 16, r.h));
    int changed = 0;
    double t;
    char buf[48];

    if (state >= 2 && track.w > 1)
    {
        const double nt = (double)(c->mouse_x - track.x) / (double)track.w;
        const double nv = ui_slider_from_norm(nt, lo, hi, log_scale);
        if (nv != *v) { *v = nv; changed = 1; }
    }
    if (c->focus == id)
    {
        const double step = (hi - lo) / 100.0;
        if (ui_key_pressed(c, UI_KEY_LEFT))
        {
            *v = (log_scale != 0)
                ? ui_slider_from_norm(ui_slider_to_norm(*v, lo, hi, 1) - 0.01,
                                      lo, hi, 1)
                : (*v - step);
            changed = 1;
        }
        if (ui_key_pressed(c, UI_KEY_RIGHT))
        {
            *v = (log_scale != 0)
                ? ui_slider_from_norm(ui_slider_to_norm(*v, lo, hi, 1) + 0.01,
                                      lo, hi, 1)
                : (*v + step);
            changed = 1;
        }
    }
    if (*v < lo) { *v = lo; }
    if (*v > hi) { *v = hi; }
    t = ui_slider_to_norm(*v, lo, hi, log_scale);

    if (state == 1 || state == 2) { c->cursor = UI_CURSOR_HAND; }

    if (label != NULL)
    {
        (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL,
                              ui_rect(r.x, r.y, lab_w - 4, r.h), 0,
                              UI_C_TEXT_DIM, UI_ALIGN_LEFT, label);
    }
    ui_fill_round_rect(&c->canvas, track, 3.0, UI_C_TRACK_GROOVE);
    if (t > 0.0)
    {
        ui_fill_round_rect(&c->canvas,
                           ui_rect(track.x, track.y,
                                   (int32_t)((double)track.w * t + 0.5), track.h),
                           3.0, UI_C_SLIDER_FILL);
    }
    {
        const double hx = (double)track.x + (double)track.w * t;
        const double hy = (double)track.y + 3.0;
        ui_fill_circle(&c->canvas, hx, hy, (state != 0) ? 7.0 : 6.0,
                       (state == 2) ? UI_C_FOCUS : UI_C_BORDER_HI);
        ui_fill_circle(&c->canvas, hx, hy, (state != 0) ? 4.6 : 4.0,
                       UI_RGB(0xE8, 0xEE, 0xF6));
    }
    (void)snprintf(buf, sizeof(buf), (fmt != NULL) ? fmt : "%.3g", *v);
    (void)ui_text_in_rect(&c->canvas, UI_FONT_MONO,
                          ui_rect(r.x + r.w - val_w, r.y, val_w, r.h), 0,
                          UI_C_TEXT, UI_ALIGN_RIGHT, buf);
    return changed;
}

int ui_slider_int(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
                  int32_t* v, int32_t lo, int32_t hi, const char* fmt)
{
    /* Reports a change only when the *rounded* value moves, so a drag that
     * stays within one integer step never spams the caller's reconfigure
     * path even though the underlying slider changed every frame. */
    double d = (double)*v;
    int32_t nv;
    (void)ui_slider(c, id, r, label, &d, (double)lo, (double)hi,
                    (fmt != NULL) ? fmt : "%.0f", 0);
    nv = (int32_t)floor(d + 0.5);
    if (nv != *v) { *v = nv; return 1; }
    return 0;
}

/* ==========================================================================
 *  Numeric edit field
 * ========================================================================== */
static void ui_edit_start(UI_Context* c, uint32_t id, const char* text)
{
    c->edit_id  = id;
    c->edit_bad = 0;
    (void)snprintf(c->edit, sizeof(c->edit), "%s", (text != NULL) ? text : "");
    c->edit_len   = (int32_t)strlen(c->edit);
    c->edit_caret = c->edit_len;
}

static int ui_edit_parse(const char* s, double* out)
{
    char* end = NULL;
    double v;
    if (s == NULL || s[0] == '\0') { return 0; }
    v = strtod(s, &end);
    if (end == s) { return 0; }
    while (*end == ' ' || *end == '\t') { ++end; }
    if (*end != '\0') { return 0; }
    if (!(v == v)) { return 0; }                    /* reject NaN */
    *out = v;
    return 1;
}

int ui_edit_double(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
                   double* v, double lo, double hi, const char* fmt)
{
    const int32_t lab_w = (label != NULL)
        ? ((UI_M_LABEL_W < r.w / 2) ? UI_M_LABEL_W : r.w / 2) : 0;
    const UI_Rect box = ui_rect(r.x + lab_w, r.y + (r.h - UI_M_CTRL_H) / 2,
                                r.w - lab_w, UI_M_CTRL_H);
    const int inside = ui_hit(c, box);
    const int editing = (c->edit_id == id) ? 1 : 0;
    int changed = 0;
    char shown[UI_EDIT_CAP];

    if (inside != 0)
    {
        c->hot_next = id;
        c->cursor   = UI_CURSOR_TEXT;
        if (c->pressed[UI_MB_LEFT] != 0 && editing == 0)
        {
            char cur[48];
            (void)snprintf(cur, sizeof(cur), (fmt != NULL) ? fmt : "%.6g", *v);
            ui_edit_start(c, id, cur);
            c->focus = id;
        }
    }
    else if (c->pressed[UI_MB_LEFT] != 0 && editing != 0)
    {
        /* Clicking away commits, which is what a spreadsheet-trained operator
         * expects; an unparsable value is discarded. */
        double parsed;
        if (ui_edit_parse(c->edit, &parsed) != 0)
        {
            if (parsed < lo) { parsed = lo; }
            if (parsed > hi) { parsed = hi; }
            if (parsed != *v) { *v = parsed; changed = 1; }
        }
        c->edit_id = 0u;
    }

    if (editing != 0)
    {
        int32_t i;
        for (i = 0; i < c->n_text; ++i)
        {
            const uint32_t cp = c->text[i];
            if (c->edit_len + 1 < (int32_t)sizeof(c->edit) && cp < 128u &&
                (((cp >= '0') && (cp <= '9')) || cp == '.' || cp == '-' ||
                 cp == '+' || cp == 'e' || cp == 'E'))
            {
                memmove(&c->edit[c->edit_caret + 1], &c->edit[c->edit_caret],
                        (size_t)(c->edit_len - c->edit_caret + 1));
                c->edit[c->edit_caret] = (char)cp;
                ++c->edit_caret;
                ++c->edit_len;
            }
        }
        for (i = 0; i < c->n_keys; ++i)
        {
            switch (c->keys[i])
            {
            case UI_KEY_BACKSPACE:
                if (c->edit_caret > 0)
                {
                    memmove(&c->edit[c->edit_caret - 1], &c->edit[c->edit_caret],
                            (size_t)(c->edit_len - c->edit_caret + 1));
                    --c->edit_caret;
                    --c->edit_len;
                }
                break;
            case UI_KEY_DELETE:
                if (c->edit_caret < c->edit_len)
                {
                    memmove(&c->edit[c->edit_caret], &c->edit[c->edit_caret + 1],
                            (size_t)(c->edit_len - c->edit_caret));
                    --c->edit_len;
                }
                break;
            case UI_KEY_LEFT:  if (c->edit_caret > 0) { --c->edit_caret; } break;
            case UI_KEY_RIGHT: if (c->edit_caret < c->edit_len) { ++c->edit_caret; } break;
            case UI_KEY_HOME:  c->edit_caret = 0; break;
            case UI_KEY_END:   c->edit_caret = c->edit_len; break;
            case UI_KEY_ESCAPE: c->edit_id = 0u; break;
            case UI_KEY_ENTER:
            {
                double parsed;
                if (ui_edit_parse(c->edit, &parsed) != 0)
                {
                    if (parsed < lo) { parsed = lo; }
                    if (parsed > hi) { parsed = hi; }
                    if (parsed != *v) { *v = parsed; changed = 1; }
                    c->edit_id = 0u;
                }
                else { c->edit_bad = 1; }
                break;
            }
            default: break;
            }
        }
        {
            double dummy;
            c->edit_bad = (c->edit_len > 0 && ui_edit_parse(c->edit, &dummy) == 0)
                        ? 1 : 0;
        }
    }

    if (label != NULL)
    {
        (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL,
                              ui_rect(r.x, r.y, lab_w - 4, r.h), 0,
                              UI_C_TEXT_DIM, UI_ALIGN_LEFT, label);
    }
    ui_fill_round_rect(&c->canvas, box, 2.0,
                       (editing != 0 && c->edit_bad != 0) ? UI_C_EDIT_BG_BAD
                                                          : UI_C_EDIT_BG);
    ui_frame_round_rect(&c->canvas, box, 2.0,
                        (editing != 0) ? UI_C_FOCUS
                                       : ((inside != 0) ? UI_C_BORDER_HI : UI_C_BORDER));

    if (editing != 0)
    {
        (void)snprintf(shown, sizeof(shown), "%s", c->edit);
    }
    else
    {
        (void)snprintf(shown, sizeof(shown), (fmt != NULL) ? fmt : "%.6g", *v);
    }
    {
        const int32_t base = box.y + (box.h + ui_font_ascent(UI_FONT_MONO) -
                                      ui_font_descent(UI_FONT_MONO)) / 2;
        ui_clip_push(&c->canvas, ui_rect_inset(box, 2, 1));
        (void)ui_text(&c->canvas, UI_FONT_MONO, box.x + 5, base, UI_C_TEXT, shown);
        if (editing != 0)
        {
            char head[UI_EDIT_CAP];
            int32_t cw;
            (void)snprintf(head, sizeof(head), "%.*s", (int)c->edit_caret, c->edit);
            cw = ui_text_width(UI_FONT_MONO, head);
            /* Blink at 1.6 Hz, the rate that reads as "waiting for input"
             * without being distracting on a display an operator watches. */
            if (fmod(c->time_s * 1.6, 1.0) < 0.6)
            {
                ui_vline(&c->canvas, box.x + 5 + cw, box.y + 3,
                         box.y + box.h - 4, UI_C_TEXT);
            }
        }
        ui_clip_pop(&c->canvas);
    }
    return changed;
}

/* ==========================================================================
 *  Drop-down
 * ========================================================================== */
static void ui_popup_input(UI_Context* c)
{
    const int32_t rh = UI_M_ROW_H;
    int close = 0;

    c->popup_hover = -1;

    if (c->wheel != 0 && c->popup_count * rh > c->popup_rect.h)
    {
        c->popup_scroll -= c->wheel * rh * 2;
        if (c->popup_scroll < 0) { c->popup_scroll = 0; }
        if (c->popup_scroll > c->popup_count * rh - c->popup_rect.h)
        {
            c->popup_scroll = c->popup_count * rh - c->popup_rect.h;
        }
        c->wheel = 0;
    }

    if (ui_rect_contains(c->popup_rect, c->mouse_x, c->mouse_y))
    {
        const int32_t row = (c->mouse_y - c->popup_rect.y + c->popup_scroll) / rh;
        if (row >= 0 && row < c->popup_count) { c->popup_hover = row; }
        if (c->pressed[UI_MB_LEFT] != 0 && c->popup_hover >= 0)
        {
            if (c->popup_sel != c->popup_hover)
            {
                c->popup_sel     = c->popup_hover;
                c->popup_changed = 1;
            }
            close = 1;
        }
    }
    else if (c->pressed[UI_MB_LEFT] != 0)
    {
        close = 1;
    }

    if (ui_key_pressed(c, UI_KEY_ESCAPE)) { close = 1; }
    /* Enter confirms and closes.  It reports a change only when the hovered
     * row differs from the selection, so confirming the current value stays
     * a no-op for the caller (no spurious scenario reload, for instance). */
    if (ui_key_pressed(c, UI_KEY_ENTER))
    {
        if (c->popup_hover >= 0 && c->popup_sel != c->popup_hover)
        {
            c->popup_sel     = c->popup_hover;
            c->popup_changed = 1;
        }
        close = 1;
    }
    if (ui_key_pressed(c, UI_KEY_DOWN) && c->popup_sel + 1 < c->popup_count)
    {
        ++c->popup_sel;
        c->popup_changed = 1;
    }
    if (ui_key_pressed(c, UI_KEY_UP) && c->popup_sel > 0)
    {
        --c->popup_sel;
        c->popup_changed = 1;
    }

    /* The pointer belongs to the popup for the rest of the frame. */
    c->pressed[UI_MB_LEFT]  = 0;
    c->released[UI_MB_LEFT] = 0;
    c->n_keys = 0;

    if (close != 0)
    {
        c->popup_closed = c->popup_id;
        c->popup_id     = 0u;
        c->popup_items  = NULL;
    }
}

static void ui_popup_draw(UI_Context* c)
{
    const int32_t rh = UI_M_ROW_H;
    int32_t i;

    ui_clip_reset(&c->canvas);
    /* Drop shadow, then the list surface. */
    ui_fill_round_rect(&c->canvas,
                       ui_rect(c->popup_rect.x + 2, c->popup_rect.y + 3,
                               c->popup_rect.w, c->popup_rect.h),
                       UI_M_RADIUS, UI_RGBA(0, 0, 0, 110));
    ui_fill_round_rect(&c->canvas, c->popup_rect, UI_M_RADIUS, UI_C_PANEL_HI);
    ui_frame_round_rect(&c->canvas, c->popup_rect, UI_M_RADIUS, UI_C_BORDER_HI);

    ui_clip_push(&c->canvas, ui_rect_inset(c->popup_rect, 1, 1));
    for (i = 0; i < c->popup_count; ++i)
    {
        const UI_Rect row = ui_rect(c->popup_rect.x + 1,
                                    c->popup_rect.y + i * rh - c->popup_scroll,
                                    c->popup_rect.w - 2, rh);
        const char* txt;
        if (row.y + rh < c->popup_rect.y || row.y > c->popup_rect.y + c->popup_rect.h)
        {
            continue;
        }
        if (i == c->popup_hover)
        {
            ui_fill_rect(&c->canvas, row, UI_C_SELECTION);
        }
        else if (i == c->popup_sel)
        {
            ui_fill_rect(&c->canvas, row, UI_C_PANEL_LO);
        }
        txt = (c->popup_items != NULL) ? c->popup_items(c->popup_user, i) : "";
        (void)ui_text_in_rect(&c->canvas, UI_FONT_BODY, row, UI_M_PAD,
                              UI_C_TEXT, UI_ALIGN_LEFT,
                              (txt != NULL) ? txt : "");
        if (i == c->popup_sel)
        {
            (void)ui_text_in_rect(&c->canvas, UI_FONT_BODY, row, UI_M_PAD,
                                  UI_C_FOCUS, UI_ALIGN_RIGHT, "\xE2\x97\x8F");
        }
    }
    ui_clip_pop(&c->canvas);
}

int ui_combo(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
             int32_t* sel, int32_t count, UI_ItemFn items, void* user)
{
    const int32_t lab_w = (label != NULL)
        ? ((UI_M_LABEL_W < r.w / 2) ? UI_M_LABEL_W : r.w / 2) : 0;
    const UI_Rect box = ui_rect(r.x + lab_w, r.y + (r.h - UI_M_CTRL_H) / 2,
                                r.w - lab_w, UI_M_CTRL_H);
    const int state = ui_behave(c, id, box);
    const char* txt;
    int changed = 0;

    if (state == 3 && c->popup_id == 0u)
    {
        const int32_t rh = UI_M_ROW_H;
        int32_t want = count * rh + 2;
        int32_t maxh = (count > UI_POPUP_MAX_ROWS)
            ? (UI_POPUP_MAX_ROWS * rh + 2) : want;
        int32_t y = box.y + box.h + 2;
        if (maxh > c->canvas.h - 16) { maxh = c->canvas.h - 16; }
        if (y + maxh > c->canvas.h - 4)
        {
            /* Not enough room below: flip above the field. */
            y = box.y - maxh - 2;
            if (y < 4) { y = 4; }
        }
        c->popup_id     = id;
        c->popup_rect   = ui_rect(box.x, y, box.w, maxh);
        c->popup_count  = count;
        c->popup_items  = items;
        c->popup_user   = user;
        c->popup_sel    = *sel;
        c->popup_hover  = *sel;
        c->popup_scroll = 0;
        if (*sel * rh > maxh - rh) { c->popup_scroll = *sel * rh - maxh / 2; }
        if (c->popup_scroll < 0) { c->popup_scroll = 0; }
    }
    /* Commit through THIS frame's pointer, both while the popup is open
     * (arrow keys) and on the frame it closed; the `sel` captured when the
     * popup opened pointed at panel stack that died with that frame. */
    if (c->popup_changed != 0 &&
        (c->popup_id == id || c->popup_closed == id) &&
        c->popup_sel >= 0 && c->popup_sel < count && *sel != c->popup_sel)
    {
        *sel = c->popup_sel;
        changed = 1;
    }

    if (label != NULL)
    {
        (void)ui_text_in_rect(&c->canvas, UI_FONT_SMALL,
                              ui_rect(r.x, r.y, lab_w - 4, r.h), 0,
                              UI_C_TEXT_DIM, UI_ALIGN_LEFT, label);
    }
    ui_draw_button_face(c, box, (c->popup_id == id) ? 2 : state,
                        UI_C_CTRL_ON, UI_C_CTRL_ON, 0);
    if (state == 1) { c->cursor = UI_CURSOR_HAND; }

    txt = (items != NULL && *sel >= 0 && *sel < count) ? items(user, *sel) : "";
    ui_clip_push(&c->canvas, ui_rect(box.x, box.y, box.w - 18, box.h));
    (void)ui_text_in_rect(&c->canvas, UI_FONT_BODY, box, UI_M_PAD - 2,
                          UI_C_TEXT, UI_ALIGN_LEFT, (txt != NULL) ? txt : "");
    ui_clip_pop(&c->canvas);
    {
        /* Chevron. */
        const double cx = (double)(box.x + box.w - 11);
        const double cy = (double)box.y + (double)box.h * 0.5;
        ui_line(&c->canvas, cx - 4.0, cy - 2.0, cx, cy + 2.5, UI_C_TEXT_DIM, 1.6);
        ui_line(&c->canvas, cx, cy + 2.5, cx + 4.0, cy - 2.0, UI_C_TEXT_DIM, 1.6);
    }
    return changed;
}

/* ==========================================================================
 *  Tabs
 * ========================================================================== */
int ui_tabs(UI_Context* c, uint32_t id, UI_Rect r, int32_t* sel,
            const char* const* labels, int32_t count)
{
    int32_t i, x = r.x;
    int changed = 0;
    if (count <= 0) { return 0; }

    ui_fill_rect(&c->canvas, r, UI_C_PANEL_LO);
    ui_hline(&c->canvas, r.x, r.x + r.w - 1, r.y + r.h - 1, UI_C_BORDER);

    for (i = 0; i < count; ++i)
    {
        const int32_t w = ui_text_width(UI_FONT_BODY, labels[i]) + 2 * UI_M_PAD + 6;
        const UI_Rect tr = ui_rect(x, r.y, w, r.h);
        const uint32_t tid = ui_id("##tab", (int32_t)(id ^ (uint32_t)i));
        const int state = ui_behave(c, tid, tr);
        const int on = (*sel == i) ? 1 : 0;

        if (state == 3 && on == 0) { *sel = i; changed = 1; }
        if (state == 1) { c->cursor = UI_CURSOR_HAND; }

        if (on != 0)
        {
            ui_fill_rect(&c->canvas, tr, UI_C_PANEL);
            ui_fill_rect(&c->canvas, ui_rect(tr.x, tr.y, tr.w, 2), UI_C_FOCUS);
            ui_vline(&c->canvas, tr.x, tr.y, tr.y + tr.h - 1, UI_C_BORDER);
            ui_vline(&c->canvas, tr.x + tr.w - 1, tr.y, tr.y + tr.h - 1, UI_C_BORDER);
            ui_hline(&c->canvas, tr.x + 1, tr.x + tr.w - 2, tr.y + tr.h - 1,
                     UI_C_PANEL);
        }
        else if (state == 1)
        {
            ui_fill_rect(&c->canvas, ui_rect_inset(tr, 0, 2), UI_C_PANEL_HI);
        }
        (void)ui_text_in_rect(&c->canvas, on ? UI_FONT_BOLD : UI_FONT_BODY, tr, 0,
                              on ? UI_C_TEXT : UI_C_TEXT_DIM,
                              UI_ALIGN_CENTRE, labels[i]);
        x += w;
    }
    return changed;
}

/* ==========================================================================
 *  Scroll bar
 * ========================================================================== */
int ui_scrollbar(UI_Context* c, uint32_t id, UI_Rect r, int32_t* offset,
                 int32_t view, int32_t content)
{
    const int32_t max_off = (content > view) ? (content - view) : 0;
    int32_t thumb_h, thumb_y;
    int state, changed = 0;

    ui_fill_rect(&c->canvas, r, UI_C_TRACK_GROOVE);
    if (max_off <= 0) { *offset = 0; return 0; }

    thumb_h = (int32_t)((double)r.h * (double)view / (double)content);
    if (thumb_h < 18) { thumb_h = 18; }
    if (thumb_h > r.h) { thumb_h = r.h; }
    thumb_y = r.y + (int32_t)((double)(r.h - thumb_h) *
                              ((double)*offset / (double)max_off));

    state = ui_behave(c, id, r);
    if (state >= 2)
    {
        const int32_t track = r.h - thumb_h;
        if (track > 0)
        {
            const double t = (double)(c->mouse_y - r.y - thumb_h / 2) / (double)track;
            int32_t nv = (int32_t)((t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t)) *
                                   (double)max_off + 0.5);
            if (nv != *offset) { *offset = nv; changed = 1; }
            thumb_y = r.y + (int32_t)((double)track *
                                      ((double)*offset / (double)max_off));
        }
    }
    ui_fill_round_rect(&c->canvas, ui_rect(r.x + 1, thumb_y, r.w - 2, thumb_h),
                       3.0, (state != 0) ? UI_C_BORDER_HI : UI_C_BORDER);
    return changed;
}

/* ==========================================================================
 *  Splitters
 * ========================================================================== */
int ui_splitter_v(UI_Context* c, uint32_t id, UI_Rect r, int32_t total_px,
                  double* frac, double lo, double hi)
{
    /* The hit area is widened past the visible grip so the splitter is easy to
     * grab without making the chrome heavy. */
    const int state = ui_behave(c, id, ui_rect(r.x - 2, r.y, r.w + 4, r.h));
    int changed = 0;

    if (state == 1 || state == 2) { c->cursor = UI_CURSOR_SIZE_WE; }
    if (state == 2 && c->mouse_dx != 0 && total_px > 1)
    {
        double nf = *frac + (double)c->mouse_dx / (double)total_px;
        if (nf < lo) { nf = lo; }
        if (nf > hi) { nf = hi; }
        if (nf != *frac) { *frac = nf; changed = 1; }
    }
    ui_fill_rect(&c->canvas, r,
                 (state != 0) ? UI_C_SPLITTER_HOT : UI_C_SPLITTER);
    {
        /* Grip dots. */
        const int32_t cx = r.x + r.w / 2;
        const int32_t cy = r.y + r.h / 2;
        int32_t k;
        for (k = -2; k <= 2; ++k)
        {
            ui_fill_rect(&c->canvas, ui_rect(cx - 1, cy + k * 5, 2, 2),
                         (state != 0) ? UI_C_TEXT : UI_C_TEXT_FAINT);
        }
    }
    return changed;
}

int ui_splitter_h(UI_Context* c, uint32_t id, UI_Rect r, int32_t total_px,
                  double* frac, double lo, double hi)
{
    const int state = ui_behave(c, id, ui_rect(r.x, r.y - 2, r.w, r.h + 4));
    int changed = 0;

    if (state == 1 || state == 2) { c->cursor = UI_CURSOR_SIZE_NS; }
    if (state == 2 && c->mouse_dy != 0 && total_px > 1)
    {
        double nf = *frac + (double)c->mouse_dy / (double)total_px;
        if (nf < lo) { nf = lo; }
        if (nf > hi) { nf = hi; }
        if (nf != *frac) { *frac = nf; changed = 1; }
    }
    ui_fill_rect(&c->canvas, r,
                 (state != 0) ? UI_C_SPLITTER_HOT : UI_C_SPLITTER);
    {
        const int32_t cx = r.x + r.w / 2;
        const int32_t cy = r.y + r.h / 2;
        int32_t k;
        for (k = -2; k <= 2; ++k)
        {
            ui_fill_rect(&c->canvas, ui_rect(cx + k * 5, cy - 1, 2, 2),
                         (state != 0) ? UI_C_TEXT : UI_C_TEXT_FAINT);
        }
    }
    return changed;
}

/* ==========================================================================
 *  Table
 * ========================================================================== */
int ui_table(UI_Context* c, uint32_t id, UI_Rect r,
             const UI_TableColumn* cols, int32_t n_cols, int32_t n_rows,
             UI_TableState* st, UI_TableCellFn cell, void* user)
{
    const int32_t hdr_h = 22;
    const int32_t row_h = (st->row_h > 0) ? st->row_h : 20;
    const int32_t sb_w  = 12;
    UI_Rect head, body, sb;
    int32_t fixed = 0, flex_count = 0, flex_w, x, i, row;
    int32_t content = n_rows * row_h;
    int changed = 0;
    char buf[128];

    if (cols == NULL || n_cols <= 0) { return 0; }

    head = ui_rect(r.x, r.y, r.w, hdr_h);
    body = ui_rect(r.x, r.y + hdr_h, r.w - sb_w, r.h - hdr_h);
    sb   = ui_rect(r.x + r.w - sb_w, r.y + hdr_h, sb_w, r.h - hdr_h);

    for (i = 0; i < n_cols; ++i)
    {
        if (cols[i].width > 0) { fixed += cols[i].width; }
        else { ++flex_count; }
    }
    flex_w = (flex_count > 0) ? ((body.w - fixed) / flex_count) : 0;
    if (flex_w < 40) { flex_w = 40; }

    /* ---- background ----------------------------------------------------- */
    ui_fill_rect(&c->canvas, r, UI_C_PLOT_BG);
    ui_fill_gradient_v(&c->canvas, head, UI_C_PANEL_HI, UI_C_PANEL);
    ui_hline(&c->canvas, head.x, head.x + head.w - 1, head.y + head.h - 1,
             UI_C_BORDER);

    /* ---- scrolling ------------------------------------------------------ */
    if (ui_hit(c, body) != 0 && c->wheel != 0)
    {
        st->scroll -= c->wheel * row_h * 3;
        c->wheel = 0;
    }
    if (st->scroll > content - body.h) { st->scroll = content - body.h; }
    if (st->scroll < 0) { st->scroll = 0; }

    /* ---- header: click to sort ------------------------------------------ */
    x = head.x;
    for (i = 0; i < n_cols; ++i)
    {
        const int32_t w = (cols[i].width > 0) ? cols[i].width : flex_w;
        const UI_Rect hr = ui_rect(x, head.y, w, head.h);
        const uint32_t hid = ui_id("##thdr", (int32_t)(id ^ (uint32_t)(i * 131)));
        const int state = ui_behave(c, hid, hr);

        if (state == 3)
        {
            if (st->sort_col == i) { st->sort_dir = -st->sort_dir; }
            else { st->sort_col = i; st->sort_dir = 1; }
            if (st->sort_dir == 0) { st->sort_dir = 1; }
            changed = 1;
        }
        if (state == 1) { c->cursor = UI_CURSOR_HAND; }
        if (state != 0) { ui_fill_rect(&c->canvas, ui_rect_inset(hr, 0, 1),
                                      UI_C_PANEL_HI); }
        ui_clip_push(&c->canvas, hr);
        (void)ui_text_in_rect(&c->canvas, UI_FONT_BOLD, hr, 6, UI_C_TEXT_DIM,
                              cols[i].align, cols[i].title);
        ui_clip_pop(&c->canvas);
        if (st->sort_col == i && st->sort_dir != 0)
        {
            const double cx = (double)(x + w - 8);
            const double cy = (double)head.y + (double)head.h * 0.5;
            const double s  = (st->sort_dir > 0) ? -1.0 : 1.0;
            ui_line(&c->canvas, cx - 3.0, cy - s * 2.0, cx, cy + s * 2.5,
                    UI_C_FOCUS, 1.4);
            ui_line(&c->canvas, cx, cy + s * 2.5, cx + 3.0, cy - s * 2.0,
                    UI_C_FOCUS, 1.4);
        }
        if (i + 1 < n_cols)
        {
            ui_vline(&c->canvas, x + w - 1, head.y + 4, head.y + head.h - 5,
                     UI_C_BORDER);
        }
        x += w;
    }

    /* ---- rows ----------------------------------------------------------- */
    ui_clip_push(&c->canvas, body);
    {
        const int32_t first = st->scroll / row_h;
        const int32_t last  = (st->scroll + body.h) / row_h + 1;
        for (row = first; row <= last && row < n_rows; ++row)
        {
            const UI_Rect rr = ui_rect(body.x, body.y + row * row_h - st->scroll,
                                       body.w, row_h);
            const uint32_t rid = ui_id("##trow", (int32_t)(id ^ (uint32_t)(row * 7919)));
            const int state = ui_behave(c, rid, rr);

            if (row < 0) { continue; }
            if (state == 3 && st->selected != row) { st->selected = row; changed = 1; }
            if (st->selected == row)
            {
                ui_fill_rect(&c->canvas, rr, UI_C_SELECTION);
            }
            else if (state == 1)
            {
                ui_fill_rect(&c->canvas, rr, UI_C_PANEL_LO);
            }
            else if ((row & 1) != 0)
            {
                ui_fill_rect(&c->canvas, rr, UI_RGBA(0xFF, 0xFF, 0xFF, 8));
            }

            x = body.x;
            for (i = 0; i < n_cols; ++i)
            {
                const int32_t w = (cols[i].width > 0) ? cols[i].width : flex_w;
                UI_Color fg = UI_C_TEXT;
                buf[0] = '\0';
                if (cell != NULL) { cell(user, row, i, buf, sizeof(buf), &fg); }
                ui_clip_push(&c->canvas, ui_rect(x, rr.y, w, rr.h));
                (void)ui_text_in_rect(&c->canvas, cols[i].font,
                                      ui_rect(x, rr.y, w, rr.h), 6, fg,
                                      cols[i].align, buf);
                ui_clip_pop(&c->canvas);
                x += w;
            }
        }
    }
    ui_clip_pop(&c->canvas);

    if (ui_scrollbar(c, ui_id("##tsb", (int32_t)id), sb, &st->scroll,
                     body.h, content) != 0)
    {
        changed = 1;
    }
    ui_frame_rect(&c->canvas, r, UI_C_BORDER);
    return changed;
}

/* ==========================================================================
 *  Layout stack
 * ========================================================================== */
void ui_stack_begin(UI_Stack* s, UI_Rect area, int32_t gap)
{
    s->area = area;
    s->y    = area.y;
    s->gap  = gap;
}

UI_Rect ui_stack_row(UI_Stack* s, int32_t h)
{
    const UI_Rect r = ui_rect(s->area.x, s->y, s->area.w, h);
    s->y += h + s->gap;
    return r;
}

void ui_stack_row_split(UI_Stack* s, int32_t h, int32_t n, UI_Rect* out)
{
    const UI_Rect row = ui_stack_row(s, h);
    int32_t i;
    if (n < 1) { return; }
    for (i = 0; i < n; ++i)
    {
        const int32_t x0 = row.x + (row.w + UI_M_GAP) * i / n;
        const int32_t x1 = row.x + (row.w + UI_M_GAP) * (i + 1) / n - UI_M_GAP;
        out[i] = ui_rect(x0, row.y, (x1 > x0) ? (x1 - x0) : 1, h);
    }
}

int32_t ui_stack_remaining(const UI_Stack* s)
{
    const int32_t left = s->area.y + s->area.h - s->y;
    return (left > 0) ? left : 0;
}
