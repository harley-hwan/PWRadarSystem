/* Embedded antialiased bitmap fonts.
 *
 * The glyph atlases in ui_font_data.c are produced offline by tools/gen_font.py
 * and checked in, so the console renders properly antialiased, correctly
 * advanced text while linking against nothing but the C runtime.
 */
#ifndef PWRADAR_UI_FONT_H
#define PWRADAR_UI_FONT_H

#include <stdint.h>

/* Glyph set: ASCII 32..126 plus the engineering symbols listed in the
 * generator.  Keep in step with tools/gen_font.py. */
#define UI_FONT_GLYPHS  117

typedef enum UI_FontId
{
    UI_FONT_SMALL = 0,      /* 11 px sans - axis tick labels, dense tables    */
    UI_FONT_BODY  = 1,      /* 13 px sans - the default UI face               */
    UI_FONT_BOLD  = 2,      /* 13 px sans bold - titles, headers, readouts    */
    UI_FONT_MONO  = 3,      /* 12 px mono - numeric columns that must align   */
    UI_FONT_COUNT = 4
} UI_FontId;

typedef struct UI_Glyph
{
    uint16_t atlas_x;       /* column of the glyph inside the atlas row       */
    uint8_t  w, h;          /* glyph bitmap extent                            */
    int8_t   bx;            /* horizontal bearing from the pen position       */
    int8_t   by;            /* vertical bearing from the baseline, up = -ve   */
    uint8_t  advance;       /* pen advance                                    */
} UI_Glyph;

typedef struct UI_Font
{
    const uint8_t*  atlas;      /* [atlas_w * atlas_h] 8-bit coverage         */
    const UI_Glyph* glyphs;     /* [count]                                    */
    const int32_t*  codepoints; /* [count] ascending within each block        */
    uint16_t        atlas_w;
    uint16_t        atlas_h;
    uint8_t         ascent;
    uint8_t         descent;
    uint8_t         line_height;
    uint16_t        count;
} UI_Font;

extern const UI_Font ui_fonts[UI_FONT_COUNT];

#endif /* PWRADAR_UI_FONT_H */
