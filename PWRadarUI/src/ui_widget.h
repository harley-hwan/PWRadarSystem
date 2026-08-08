/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_widget.h
 *  Purpose : The control set: buttons, toggles, check boxes, radio groups,
 *            sliders, numeric edit fields, drop-downs, tab bars, scroll bars,
 *            draggable splitters, group panels and a sortable data table.
 *
 *  Model
 *  -----
 *  Immediate mode with stable identities.  A widget is a function call that
 *  both draws and returns its interaction result, while the small amount of
 *  state that must persist between frames - which widget is hot, which is
 *  active, which has keyboard focus, the text being edited, the open drop-down
 *  - lives in UI_Context keyed by a hashed identity.  For a console whose
 *  layout is computed from the live radar configuration every frame this is far
 *  less machinery than a retained widget tree, and it cannot get out of sync
 *  with the data it displays.
 *
 *  A drop-down needs to paint over widgets declared after it, so its input is
 *  handled at the top of the frame from the previous frame's geometry and its
 *  painting is deferred to ui_frame_end().  That is the only ordering subtlety
 *  in the whole layer.
 *
 *  Language: ISO C17
 * ========================================================================== */
#ifndef PWRADAR_UI_WIDGET_H
#define PWRADAR_UI_WIDGET_H

#include <stddef.h>
#include <stdint.h>

#include "ui_gfx.h"
#include "ui_platform.h"
#include "ui_theme.h"

#define UI_MAX_TEXT_QUEUE   16
#define UI_MAX_KEY_QUEUE    16
#define UI_EDIT_CAP         64
#define UI_POPUP_MAX_ROWS   40

typedef const char* (*UI_ItemFn)(void* user, int32_t index);

typedef struct UI_Context
{
    UI_Canvas canvas;

    /* ---- pointer -------------------------------------------------------- */
    int32_t   mouse_x, mouse_y;
    int32_t   mouse_prev_x, mouse_prev_y;
    int32_t   mouse_dx, mouse_dy;
    int32_t   press_x, press_y;
    int       down[3];
    int       pressed[3];
    int       released[3];
    int       double_click;
    int32_t   wheel;
    uint32_t  mods;
    int       mouse_inside;
    int       mouse_captured;      /* set when a widget owns the pointer      */

    /* ---- keyboard ------------------------------------------------------- */
    int32_t   keys[UI_MAX_KEY_QUEUE];
    int32_t   n_keys;
    uint32_t  text[UI_MAX_TEXT_QUEUE];
    int32_t   n_text;

    /* ---- interaction ---------------------------------------------------- */
    uint32_t  hot;
    uint32_t  active;
    uint32_t  focus;
    uint32_t  hot_next;
    UI_Cursor cursor;

    /* ---- text editing --------------------------------------------------- */
    char      edit[UI_EDIT_CAP];
    int32_t   edit_len;
    int32_t   edit_caret;
    uint32_t  edit_id;
    int       edit_bad;

    /* ---- deferred drop-down --------------------------------------------- */
    uint32_t  popup_id;
    /* Owner of the popup on the frame it closed.  A selection lands on the
     * close frame, after popup_id is already cleared, so the owning combo is
     * remembered by identity - never by geometry, which would confuse two
     * widgets that happen to share a position. */
    uint32_t  popup_closed;
    UI_Rect   popup_rect;
    int32_t   popup_count;
    /* The live selection is stored HERE, never behind a caller pointer: the
     * `sel` a combo passes usually points at panel stack that is only valid
     * within the frame that passed it, so the popup keeps the value and the
     * combo commits it through the current frame's fresh pointer. */
    int32_t   popup_sel;
    int32_t   popup_hover;
    int32_t   popup_scroll;
    UI_ItemFn popup_items;
    void*     popup_user;
    int       popup_changed;

    /* ---- timing --------------------------------------------------------- */
    double    time_s;
    double    dt_s;
} UI_Context;

/* --------------------------------------------------------------------------
 *  Identity
 * ------------------------------------------------------------------------ */

/** FNV-1a over the label, mixed with @p index so a loop can generate distinct
 *  identities from one literal. */
uint32_t ui_id(const char* label, int32_t index);

/* --------------------------------------------------------------------------
 *  Frame
 * ------------------------------------------------------------------------ */
void ui_ctx_init(UI_Context* c);

/** Feeds one platform event into the context.  Call for every event before
 *  ui_frame_begin(). */
void ui_ctx_event(UI_Context* c, const UI_Event* ev);

void ui_frame_begin(UI_Context* c, uint32_t* px, int32_t w, int32_t h,
                    int32_t stride, double now_s);
/** Paints any open drop-down and clears the per-frame input state. */
void ui_frame_end(UI_Context* c);

/** True when a modal drop-down is swallowing input this frame. */
int  ui_popup_open(const UI_Context* c);

/* --------------------------------------------------------------------------
 *  Chrome
 * ------------------------------------------------------------------------ */
void ui_panel(UI_Context* c, UI_Rect r);
/** Group box with a title bar; returns the client rectangle inside it. */
UI_Rect ui_group(UI_Context* c, UI_Rect r, const char* title);
void ui_label(UI_Context* c, UI_Rect r, const char* s);
void ui_label_dim(UI_Context* c, UI_Rect r, const char* s);
/** label on the left, value right-aligned - the standard readout row. */
void ui_readout(UI_Context* c, UI_Rect r, const char* label, const char* value,
                UI_Color value_col);
void ui_separator(UI_Context* c, UI_Rect r);

/* --------------------------------------------------------------------------
 *  Controls.  Each returns non-zero when the value changed this frame.
 * ------------------------------------------------------------------------ */
int ui_button(UI_Context* c, uint32_t id, UI_Rect r, const char* label);
int ui_button_accent(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
                     UI_Color accent);
int ui_toggle(UI_Context* c, uint32_t id, UI_Rect r, const char* label, int32_t* on);
int ui_checkbox(UI_Context* c, uint32_t id, UI_Rect r, const char* label, int32_t* on);
int ui_radio(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
             int32_t* sel, int32_t value);

/** Horizontal slider with an inline label and formatted value.  @p log_scale
 *  maps the handle position logarithmically, which is what a Pfa or a power
 *  control needs. */
int ui_slider(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
              double* v, double lo, double hi, const char* fmt, int log_scale);
int ui_slider_int(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
                  int32_t* v, int32_t lo, int32_t hi, const char* fmt);

/** Numeric text field.  Commits on Enter or focus loss, reverts on Escape, and
 *  shows a red field while the text does not parse. */
int ui_edit_double(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
                   double* v, double lo, double hi, const char* fmt);

/** Drop-down.  @p items is queried for display strings. */
int ui_combo(UI_Context* c, uint32_t id, UI_Rect r, const char* label,
             int32_t* sel, int32_t count, UI_ItemFn items, void* user);

int ui_tabs(UI_Context* c, uint32_t id, UI_Rect r, int32_t* sel,
            const char* const* labels, int32_t count);

/** Vertical scroll bar.  @p view and @p content are in pixels. */
int ui_scrollbar(UI_Context* c, uint32_t id, UI_Rect r, int32_t* offset,
                 int32_t view, int32_t content);

/** Draggable splitter.  @p handle is the grip strip, @p total_px the extent
 *  being divided, and @p frac the fraction assigned to the first pane, clamped
 *  to [lo, hi].  Returns 1 while the operator is dragging it. */
int ui_splitter_v(UI_Context* c, uint32_t id, UI_Rect handle, int32_t total_px,
                  double* frac, double lo, double hi);
int ui_splitter_h(UI_Context* c, uint32_t id, UI_Rect handle, int32_t total_px,
                  double* frac, double lo, double hi);

/* --------------------------------------------------------------------------
 *  Data table (MATLAB's uitable, minus the editing)
 * ------------------------------------------------------------------------ */
typedef struct UI_TableColumn
{
    const char* title;
    int32_t     width;          /* 0 => share the remaining width equally     */
    UI_Align    align;
    UI_FontId   font;
} UI_TableColumn;

typedef struct UI_TableState
{
    int32_t scroll;             /* pixels                                     */
    int32_t selected;           /* row index, -1 when nothing is selected     */
    int32_t sort_col;
    int32_t sort_dir;           /* +1 ascending, -1 descending, 0 unsorted    */
    int32_t row_h;
} UI_TableState;

/** Fills @p out with the text for one cell.  Set *fg to override the colour
 *  (leave it alone for the default). */
typedef void (*UI_TableCellFn)(void* user, int32_t row, int32_t col,
                               char* out, size_t cap, UI_Color* fg);

/** Draws the table and handles selection, scrolling and header sorting.
 *  Returns 1 when the selection or the sort order changed. */
int ui_table(UI_Context* c, uint32_t id, UI_Rect r,
             const UI_TableColumn* cols, int32_t n_cols, int32_t n_rows,
             UI_TableState* st, UI_TableCellFn cell, void* user);

/* --------------------------------------------------------------------------
 *  Layout helper: a vertical stack that hands out rows
 * ------------------------------------------------------------------------ */
typedef struct UI_Stack
{
    UI_Rect area;
    int32_t y;
    int32_t gap;
} UI_Stack;

void    ui_stack_begin(UI_Stack* s, UI_Rect area, int32_t gap);
UI_Rect ui_stack_row(UI_Stack* s, int32_t h);
/** Splits the next row into @p n equal cells, writing them into @p out. */
void    ui_stack_row_split(UI_Stack* s, int32_t h, int32_t n, UI_Rect* out);
int32_t ui_stack_remaining(const UI_Stack* s);

#endif /* PWRADAR_UI_WIDGET_H */
