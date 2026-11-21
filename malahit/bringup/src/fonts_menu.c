/*
 * font_ui_10, font_ui_10b AND font_num_31, ONCE - 21/11/2026.
 *
 * compat/font_ui_10.h and _10b.h carry their glyphs as `static const`,
 * on purpose while ui_top.c was their only reader (tools/gen_fuentes.py
 * says so). The menu screens read them too - ui_cfg.c, ui_grid.c and
 * src/menus.c - and three more static copies cost 24 kB of AXI SRAM,
 * which took the RAM image from 472 to 496 kB of 512.
 *
 * So they are defined here once, with external linkage: `static` is
 * defined away around the two generated headers. The menu objects are
 * compiled with MALAHIT_FONTS_MENU (reglas.mk), which renames upstream's
 * faces WITHOUT -include-ing these headers, so upstream's own
 * `extern const gfx2_font_t font_ui_14;` becomes an extern declaration
 * of font_ui_10 - this one. ui_top.c keeps its private copies: it is the
 * verified path, and the two are the same data.
 */
#include "gfx2_font.h"

#define static
#include "font_ui_10.h"
#include "font_ui_10b.h"
#include "font_num_31.h"    /* ui_det.c's value, as font_num_44 */
#undef static
