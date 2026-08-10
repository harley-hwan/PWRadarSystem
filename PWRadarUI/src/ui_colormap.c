#include "ui_colormap.h"

#include <string.h>

typedef struct UI_CmapStop
{
    double  t;
    uint8_t r, g, b;
} UI_CmapStop;

/* --------------------------------------------------------------------------
 *  Control points.  Values are the published anchors of each map, sampled
 *  densely enough that piecewise-linear interpolation is visually
 *  indistinguishable from the original.
 * ------------------------------------------------------------------------ */
static const UI_CmapStop k_parula[] = {
    { 0.000,  53,  42, 135 }, { 0.125,  15,  92, 221 },
    { 0.250,   5, 125, 205 }, { 0.375,  24, 149, 174 },
    { 0.500,  85, 165, 130 }, { 0.625, 158, 174,  91 },
    { 0.750, 220, 182,  63 }, { 0.875, 251, 205,  40 },
    { 1.000, 249, 251,  14 }
};

static const UI_CmapStop k_jet[] = {
    { 0.000,   0,   0, 143 }, { 0.125,   0,   0, 255 },
    { 0.250,   0, 127, 255 }, { 0.375,   0, 255, 255 },
    { 0.500, 127, 255, 127 }, { 0.625, 255, 255,   0 },
    { 0.750, 255, 127,   0 }, { 0.875, 255,   0,   0 },
    { 1.000, 127,   0,   0 }
};

static const UI_CmapStop k_turbo[] = {
    { 0.000,  48,  18,  59 }, { 0.100,  70, 107, 227 },
    { 0.200,  62, 155, 254 }, { 0.300,  24, 214, 203 },
    { 0.400,  70, 248, 131 }, { 0.500, 162, 252,  60 },
    { 0.600, 225, 220,  55 }, { 0.700, 254, 163,  49 },
    { 0.800, 239,  89,  17 }, { 0.900, 189,  32,   4 },
    { 1.000, 122,   4,   3 }
};

static const UI_CmapStop k_viridis[] = {
    { 0.000,  68,   1,  84 }, { 0.125,  71,  44, 122 },
    { 0.250,  59,  81, 139 }, { 0.375,  44, 113, 142 },
    { 0.500,  33, 144, 141 }, { 0.625,  39, 173, 129 },
    { 0.750,  92, 200,  99 }, { 0.875, 170, 220,  50 },
    { 1.000, 253, 231,  37 }
};

static const UI_CmapStop k_hot[] = {
    { 0.000,   0,   0,   0 }, { 0.375, 255,   0,   0 },
    { 0.750, 255, 255,   0 }, { 1.000, 255, 255, 255 }
};

static const UI_CmapStop k_gray[] = {
    { 0.000,   0,   0,   0 }, { 1.000, 255, 255, 255 }
};

static const UI_CmapStop k_bone[] = {
    { 0.000,   0,   0,   0 }, { 0.375,  81,  81, 113 },
    { 0.750, 166, 198, 198 }, { 1.000, 255, 255, 255 }
};

static const UI_CmapStop k_cool[] = {
    { 0.000,   0, 255, 255 }, { 1.000, 255,   0, 255 }
};

/* P7 long-persistence radar phosphor: near-black through deep green to a
 * white-hot core, which is what a real PPI tube looks like. */
static const UI_CmapStop k_phosphor[] = {
    { 0.000,   2,   6,   4 }, { 0.150,   6,  32,  16 },
    { 0.350,  16,  86,  38 }, { 0.600,  40, 168,  72 },
    { 0.820, 118, 232, 132 }, { 1.000, 224, 255, 228 }
};

static const UI_CmapStop k_amber[] = {
    { 0.000,   4,   3,   2 }, { 0.150,  36,  20,   4 },
    { 0.350,  92,  50,   6 }, { 0.600, 168, 102,  14 },
    { 0.820, 232, 170,  58 }, { 1.000, 255, 236, 196 }
};

typedef struct UI_CmapDef
{
    const char*        name;
    const UI_CmapStop* stops;
    uint32_t           count;
} UI_CmapDef;

#define UI_CMAP_DEF(nm, arr) { nm, arr, (uint32_t)(sizeof(arr) / sizeof((arr)[0])) }

static const UI_CmapDef k_defs[UI_CMAP_COUNT] = {
    UI_CMAP_DEF("parula",   k_parula),
    UI_CMAP_DEF("jet",      k_jet),
    UI_CMAP_DEF("turbo",    k_turbo),
    UI_CMAP_DEF("viridis",  k_viridis),
    UI_CMAP_DEF("hot",      k_hot),
    UI_CMAP_DEF("gray",     k_gray),
    UI_CMAP_DEF("bone",     k_bone),
    UI_CMAP_DEF("cool",     k_cool),
    UI_CMAP_DEF("phosphor", k_phosphor),
    UI_CMAP_DEF("amber",    k_amber)
};

static UI_Color g_tables[UI_CMAP_COUNT][256];
static int      g_ready = 0;

static void ui_build_one(UI_Color* dst, const UI_CmapStop* stops, uint32_t n)
{
    uint32_t i;
    for (i = 0u; i < 256u; ++i)
    {
        const double t = (double)i / 255.0;
        uint32_t k = 0u;
        double u;
        while (k + 2u < n && stops[k + 1u].t < t) { ++k; }
        {
            const UI_CmapStop* a = &stops[k];
            const UI_CmapStop* b = &stops[k + 1u];
            const double dt = b->t - a->t;
            u = (dt > 1e-12) ? ((t - a->t) / dt) : 0.0;
            if (u < 0.0) { u = 0.0; }
            if (u > 1.0) { u = 1.0; }
            dst[i] = UI_RGB((int)((double)a->r + ((double)b->r - (double)a->r) * u + 0.5),
                            (int)((double)a->g + ((double)b->g - (double)a->g) * u + 0.5),
                            (int)((double)a->b + ((double)b->b - (double)a->b) * u + 0.5));
        }
    }
}

void ui_colormap_init(void)
{
    int i;
    if (g_ready != 0) { return; }
    for (i = 0; i < UI_CMAP_COUNT; ++i)
    {
        ui_build_one(g_tables[i], k_defs[i].stops, k_defs[i].count);
    }
    g_ready = 1;
}

const UI_Color* ui_colormap(UI_ColormapId id)
{
    ui_colormap_init();
    if ((int)id < 0 || (int)id >= UI_CMAP_COUNT) { id = UI_CMAP_PARULA; }
    return g_tables[id];
}

const char* ui_colormap_name(UI_ColormapId id)
{
    if ((int)id < 0 || (int)id >= UI_CMAP_COUNT) { return "?"; }
    return k_defs[id].name;
}

UI_Color ui_colormap_sample(UI_ColormapId id, double t)
{
    int32_t k;
    const UI_Color* tab = ui_colormap(id);
    if (t <= 0.0) { return tab[0]; }
    if (t >= 1.0) { return tab[255]; }
    k = (int32_t)(t * 255.0 + 0.5);
    return tab[k];
}
