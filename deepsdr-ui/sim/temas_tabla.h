/* GENERADO por tools/gen_temas.py - no editar a mano. */
#ifndef TEMAS_TABLA_H
#define TEMAS_TABLA_H
#include "palette.h"
#include "spectrum.h"

typedef struct { const char *nombre; const char *desc;
                 const palette_t *pal; spectrum_palette_t wf;
                 const char *nombre_en; const char *desc_en; } sim_tema_t;

static const sim_tema_t k_sim_temas[] = {
    { "Oscura", "La de siempre, con el waterfall clásico", &k_pal_oscura, SPECTRUM_PALETTE_CLASSIC, "Dark", "The usual one, classic waterfall" },
    { "Contrastada", "Negro real y saltos mayores, para pleno sol", &k_pal_contraste, SPECTRUM_PALETTE_TURBO, "High contrast", "True black, wider steps, for full sun" },
    { "Ámbar", "Grises cálidos, sin azul: para la noche", &k_pal_ambar, SPECTRUM_PALETTE_INFERNO, "Amber", "Warm greys, no blue: for night use" },
    { "Fría", "Grises azulados, para luz de día", &k_pal_fria, SPECTRUM_PALETTE_VIRIDIS, "Cool", "Bluish greys, for daylight" },
    { "Oliva", "Verde oliva con tinta negra, tema claro", &k_pal_oliva, SPECTRUM_PALETTE_SMOKE, "Olive", "Olive green, black ink, light theme" },
    { "Naranja", "Naranja fuerte con tinta negra, tema claro", &k_pal_naranja, SPECTRUM_PALETTE_FIRE, "Orange", "Strong orange, black ink, light theme" },
    { "Fósforo", "Verde fósforo sobre negro, como un terminal", &k_pal_fosforo, SPECTRUM_PALETTE_PHOSPHOR, "Phosphor", "Phosphor green on black, like a terminal" },
    { "Chemistry", "Papel blanco con tinta negra, para pantallas flojas", &k_pal_chemistry, SPECTRUM_PALETTE_SMOKE, "Chemistry", "Paper white, black ink, for weak displays" },
    { "Lino", "Blanco roto cálido, tema claro", &k_pal_lino, SPECTRUM_PALETTE_SMOKE, "Linen", "Warm off-white, light theme" },
    { "Marfil", "Crema amarillento, tema claro", &k_pal_marfil, SPECTRUM_PALETTE_SMOKE, "Ivory", "Yellowish cream, light theme" },
    { "Arena", "Beige tostado, tema claro", &k_pal_arena, SPECTRUM_PALETTE_SMOKE, "Sand", "Toasted beige, light theme" },
    { "Pergamino", "Crema envejecido, tema claro", &k_pal_pergamino, SPECTRUM_PALETTE_SMOKE, "Parchment", "Aged cream, light theme" },
    { "Niebla", "Gris azulado muy claro, tema claro", &k_pal_niebla, SPECTRUM_PALETTE_SMOKE, "Mist", "Pale bluish grey, light theme" },
    { "Almendra", "Crema rosado, tema claro", &k_pal_almendra, SPECTRUM_PALETTE_SMOKE, "Almond", "Pinkish cream, light theme" },
    { "Menta", "Verde muy claro, tema claro", &k_pal_menta, SPECTRUM_PALETTE_SMOKE, "Mint", "Pale green, light theme" },
    { "Cielo", "Azul muy claro, tema claro", &k_pal_cielo, SPECTRUM_PALETTE_SMOKE, "Sky", "Pale blue, light theme" },
    { "Trigo", "Dorado pálido, tema claro", &k_pal_trigo, SPECTRUM_PALETTE_SMOKE, "Wheat", "Pale gold, light theme" },
    { "Porcelana", "Blanco azulado, tema claro", &k_pal_porcelana, SPECTRUM_PALETTE_SMOKE, "Porcelain", "Bluish white, light theme" },
};
#define SIM_TEMA_COUNT (int)(sizeof(k_sim_temas)/sizeof(k_sim_temas[0]))
#endif
