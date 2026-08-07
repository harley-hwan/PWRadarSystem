/* ==========================================================================
 *  PWRadarSystem - PWRadarUI
 *  ------------------------------------------------------------------------
 *  File    : ui_theme.h
 *  Purpose : One place for every colour and metric the console uses, so the
 *            whole surface stays visually consistent and can be re-skinned by
 *            editing a single header.
 *
 *  The palette is a dark instrumentation scheme: a near-black desk, cool grey
 *  chrome, and saturated accents reserved for meaning (amber = tentative,
 *  green = confirmed, cyan = detections, red = alarms).  Data ink is never
 *  spent on decoration.
 *
 *  Language: ISO C17
 * ========================================================================== */
#ifndef PWRADAR_UI_THEME_H
#define PWRADAR_UI_THEME_H

#include "ui_gfx.h"

/* ---- surfaces ----------------------------------------------------------- */
#define UI_C_DESK           UI_RGB(0x10, 0x12, 0x16)
#define UI_C_PANEL          UI_RGB(0x1A, 0x1D, 0x23)
#define UI_C_PANEL_HI       UI_RGB(0x22, 0x26, 0x2E)
#define UI_C_PANEL_LO       UI_RGB(0x15, 0x18, 0x1D)
#define UI_C_PLOT_BG        UI_RGB(0x0A, 0x0C, 0x10)
#define UI_C_BORDER         UI_RGB(0x30, 0x36, 0x40)
#define UI_C_BORDER_HI      UI_RGB(0x48, 0x50, 0x5C)
#define UI_C_SPLITTER       UI_RGB(0x26, 0x2B, 0x33)
#define UI_C_SPLITTER_HOT   UI_RGB(0x3E, 0x8E, 0xD0)

/* ---- text --------------------------------------------------------------- */
#define UI_C_TEXT           UI_RGB(0xD6, 0xDC, 0xE4)
#define UI_C_TEXT_DIM       UI_RGB(0x8A, 0x93, 0xA0)
#define UI_C_TEXT_FAINT     UI_RGB(0x5C, 0x64, 0x70)
#define UI_C_TEXT_ON_ACCENT UI_RGB(0x08, 0x0A, 0x0D)

/* ---- controls ----------------------------------------------------------- */
#define UI_C_CTRL_TOP       UI_RGB(0x2C, 0x32, 0x3B)
#define UI_C_CTRL_BOT       UI_RGB(0x21, 0x26, 0x2D)
#define UI_C_CTRL_HOT_TOP   UI_RGB(0x37, 0x3E, 0x4A)
#define UI_C_CTRL_HOT_BOT   UI_RGB(0x2A, 0x30, 0x39)
#define UI_C_CTRL_DOWN      UI_RGB(0x18, 0x1C, 0x22)
#define UI_C_CTRL_ON        UI_RGB(0x1E, 0x6F, 0xA8)
#define UI_C_CTRL_ON_HOT    UI_RGB(0x2A, 0x86, 0xC4)
#define UI_C_FOCUS          UI_RGB(0x4F, 0xA8, 0xE8)
#define UI_C_TRACK_GROOVE   UI_RGB(0x0E, 0x11, 0x15)
#define UI_C_SLIDER_FILL    UI_RGB(0x2C, 0x7E, 0xB8)
#define UI_C_EDIT_BG        UI_RGB(0x0C, 0x0E, 0x12)
#define UI_C_EDIT_BG_BAD    UI_RGB(0x2A, 0x12, 0x14)
#define UI_C_SELECTION      UI_RGB(0x1C, 0x4C, 0x74)

/* ---- axes and data ----------------------------------------------------- */
#define UI_C_AXIS           UI_RGB(0x6A, 0x74, 0x82)
#define UI_C_GRID           UI_RGB(0x24, 0x2A, 0x33)
#define UI_C_GRID_MINOR     UI_RGB(0x1A, 0x1E, 0x25)
#define UI_C_CURSOR         UI_RGB(0xE8, 0xC8, 0x4A)

#define UI_C_SERIES_0       UI_RGB(0x4F, 0xC3, 0xF7)   /* processed video      */
#define UI_C_SERIES_1       UI_RGB(0x9E, 0xA7, 0xB3)   /* raw video            */
#define UI_C_SERIES_2       UI_RGB(0xFF, 0x6E, 0x5A)   /* CFAR threshold       */
#define UI_C_SERIES_3       UI_RGB(0x8BC, 0x34, 0x0A)
#define UI_C_SERIES_4       UI_RGB(0xB3, 0x9D, 0xDB)

/* ---- radar symbology --------------------------------------------------- */
#define UI_C_DETECTION      UI_RGB(0x40, 0xE0, 0xD0)
#define UI_C_TRACK_TENT     UI_RGB(0xF0, 0xB0, 0x40)
#define UI_C_TRACK_CONF     UI_RGB(0x5C, 0xE0, 0x70)
#define UI_C_TRACK_COAST    UI_RGB(0xE0, 0x70, 0x50)
#define UI_C_TRUTH          UI_RGB(0x70, 0x80, 0xE8)
#define UI_C_BEAM           UI_RGB(0x38, 0xE0, 0x70)
#define UI_C_PPI_RING       UI_RGB(0x2E, 0x4A, 0x38)
#define UI_C_PPI_SPOKE      UI_RGB(0x28, 0x3E, 0x30)
#define UI_C_ALARM          UI_RGB(0xE8, 0x4A, 0x40)
#define UI_C_OK             UI_RGB(0x50, 0xC8, 0x78)
#define UI_C_WARN           UI_RGB(0xE8, 0xB0, 0x40)

/* ---- metrics ----------------------------------------------------------- */
#define UI_M_PAD            8
#define UI_M_GAP            6
#define UI_M_ROW_H          24
#define UI_M_CTRL_H         22
#define UI_M_HEADER_H       26
#define UI_M_TOOLBAR_H      38
#define UI_M_STATUS_H       24
#define UI_M_SPLITTER       6
#define UI_M_RADIUS         3.0
#define UI_M_LABEL_W        104

#endif /* PWRADAR_UI_THEME_H */
