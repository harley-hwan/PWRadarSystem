/* The colour maps a radar console needs, in the MATLAB vocabulary an engineer
 * already has, plus two specific to this domain (phosphor, amber).
 *
 * Each map is a 256-entry lookup built once at start-up by piecewise-linear
 * interpolation of published control points, so the image blitter's inner loop
 * is a single indexed fetch.
 */
#ifndef PWRADAR_UI_COLORMAP_H
#define PWRADAR_UI_COLORMAP_H

#include "ui_gfx.h"

typedef enum UI_ColormapId
{
    UI_CMAP_PARULA = 0,     /* MATLAB default since R2014b                    */
    UI_CMAP_JET,            /* the classic radar/spectrogram map              */
    UI_CMAP_TURBO,          /* rainbow with even perceptual spacing           */
    UI_CMAP_VIRIDIS,        /* perceptually uniform, colour-blind safe        */
    UI_CMAP_HOT,            /* black-red-yellow-white                        */
    UI_CMAP_GRAY,
    UI_CMAP_BONE,
    UI_CMAP_COOL,
    UI_CMAP_PHOSPHOR,       /* P7 radar-scope green, for the PPI              */
    UI_CMAP_AMBER,          /* amber scope, the other traditional PPI tube     */
    UI_CMAP_COUNT
} UI_ColormapId;

/** Builds every table.  Call once before any drawing. */
void ui_colormap_init(void);

/** 256-entry table for @p id.  Never NULL for a valid id. */
const UI_Color* ui_colormap(UI_ColormapId id);

/** Human readable name for a combo box. */
const char* ui_colormap_name(UI_ColormapId id);

/** Samples a map at t in [0,1]. */
UI_Color ui_colormap_sample(UI_ColormapId id, double t);

#endif /* PWRADAR_UI_COLORMAP_H */
