/*
 * LAS PANTALLAS DE MENU DEL MALAHIT, A 480 x 320 - 21/11/2026.
 *
 * Paso 9 del port (malahit/PORTING.md): ui_cfg.c -modos, bandas- y
 * ui_grid.c -pasos- con la geometria de malahit/compat/malahit_layout.h y
 * las caras de ui_top (MALAHIT_FONTS_TOP), que es exactamente como los
 * compila la radio. Se pintan ENCIMA de la pantalla principal entera
 * -principal.c, incluido aqui con su main() renombrado- para verlas en su
 * sitio: entre la banda de estado y la barra de acciones.
 *
 * Y se mide lo que no se ve: que ningun texto se salga de su celda. En
 * ui_grid lo cuenta el propio modulo (ui_grid_recortes()); en ui_cfg no hay
 * contador, asi que se mide aqui con las mismas caras.
 *
 *   make malahit-menus   -> out/malahit_{modos,bandas,pasos}.ppm y .png
 */
#define main principal_main
#include "principal.c"
#undef main

#include "ui_cfg.h"
#include "ui_grid.h"
#include "bandas_tabla.h"
#include <string.h>

/* Las caras con las que se pintan, las mismas que en ui_cfg.c y
 * ui_grid.c bajo MALAHIT_FONTS_TOP. */
extern const gfx2_font_t font_ui_11;

static int s_fallos;

static void cabe(const char *que, const char *txt, const gfx2_font_t *f,
                 int16_t hueco)
{
    int16_t w;

    if (!txt) { return; }
    w = gfx2_text_w(txt, f);
    if (w > hueco) {
        printf("  SE SALE  %-8s \"%s\" %d px en %d\n", que, txt, w, hueco);
        s_fallos++;
    }
}

static void escribe(const char *nombre)
{
    char ruta[64];

    snprintf(ruta, sizeof ruta, "out/malahit_%s.ppm", nombre);
    lcd_sim_write_ppm(ruta);
    printf("  -> %s\n", ruta);
}

/* La pantalla principal de fondo, sin escribir su ppm encima del nuestro. */
static void fondo_principal(void)
{
    char *argv[] = { "principal", 0 };
    principal_main(1, argv);
}

static void modos(void)
{
    static const struct { const char *n, *d; } k[] = {
        /* La lista de malahit/bringup/src/menus.c, k_modos[]: sin WFM, y
         * AM es "Envolvente" porque "Amplitud modulada" no cabe. */
        { "AM",  "Envolvente" },
        { "SAM", "AM síncrona" },
        { "USB", "Banda lateral alta" },
        { "LSB", "Banda lateral baja" },
        { "NFM", "FM estrecha" },
    };
    ui_cfg_state_t st;
    uint8_t i;

    memset(&st, 0, sizeof st);
    st.cat[0] = "Analógico";
    st.cats = 1; st.cat_sel = 0; st.denso = 1;
    st.n = (uint8_t)(sizeof k / sizeof k[0]);
    st.marcada = 0; st.cursor = 1; st.pressed = -1;
    for (i = 0; i < st.n; i++) {
        st.cel[i].nombre = k[i].n;
        st.cel[i].valor  = k[i].d;
        st.cel[i].extra  = "";
        cabe("modo", k[i].n, &font_ui_11, UIC_DCELL_W - 2);
        cabe("desc", k[i].d, &font_ui_10, UIC_DCELL_W - 2);
    }
    for (i = 0; i < st.cats; i++) {
        cabe("categ", st.cat[i], &font_ui_11,
             UIC_SIDE_W - UIC_CAT_TXT_X - UIC_CAT_TXT_R);
    }
    fondo_principal();
    ui_cfg_draw(&st);
    escribe("modos");
}

/*
 * LA MISMA REGLA QUE LA RADIO (src/menus.c, rango_cabe()): un rango que no
 * cabe pierde la unidad. Aqui se aplica y se mide lo que queda, que es lo
 * que se va a pintar.
 */
static char s_rng[64][24];

static const char *rango_cabe(int k, const char *r)
{
    size_t n = strlen(r);

    if (gfx2_text_w(r, &font_ui_10) <= (int16_t)(UIC_DCELL_W - 2)) { return r; }
    if (n >= sizeof s_rng[0]) { n = sizeof s_rng[0] - 1; }
    memcpy(s_rng[k], r, n);
    s_rng[k][n] = '\0';
    if (n > 4 && (!strcmp(&s_rng[k][n - 4], " kHz") || !strcmp(&s_rng[k][n - 4], " MHz"))) {
        s_rng[k][n - 4] = '\0';
    }
    return s_rng[k];
}

/* Lo que la radio sintoniza: TUNE_MIN_HZ..TUNE_MAX_HZ de su main.c. */
static int sintoniza(int k) { return banda[k].hz >= 100000UL && banda[k].hz <= 30000000UL; }

static void bandas(void)
{
    ui_cfg_state_t st;
    uint8_t i, n = 0;
    int k;

    memset(&st, 0, sizeof st);
    for (i = 0; i < 3; i++) {
        st.cat[i] = familia_nombre[i];          /* VHF/UHF: nada por debajo de 30 MHz */
        cabe("categ", familia_nombre[i], &font_ui_11,
             UIC_SIDE_W - UIC_CAT_TXT_X - UIC_CAT_TXT_R);
    }
    st.cats = 3; st.cat_sel = 1; st.denso = 1;
    st.marcada = 0xFF; st.cursor = -1; st.pressed = -1;

    /* Se miden TODAS las que la radio ofrece; se pinta una familia. */
    for (k = 0; k < N_BANDAS && k < 64; k++) {
        const char *r;

        if (!sintoniza(k)) { continue; }
        r = rango_cabe(k, banda[k].rango);
        cabe("banda", banda[k].nombre, &font_ui_11, UIC_DCELL_W - 2);
        cabe("rango", r, &font_ui_10, UIC_DCELL_W - 2);
        cabe("modo", banda[k].modo, &font_ui_10b, UIC_DCELL_W - 2);
        if (banda[k].familia == 1 && n < UIC_CELLS) {
            st.cel[n].nombre = banda[k].nombre;
            st.cel[n].valor  = r;
            st.cel[n].extra  = banda[k].modo;
            if (!strcmp(banda[k].nombre, "41 m")) { st.marcada = n; }
            n++;
        }
    }
    st.n = n;
    fondo_principal();
    ui_cfg_draw(&st);
    escribe("bandas");
}

static void pasos(void)
{
    static const struct { const char *et, *desc; } k[] = {
        { "10 Hz",    "Batido y deriva" },
        { "100 Hz",   "Ajuste fino" },
        { "500 Hz",   "Buscar en SSB" },
        { "1 kHz",    "SSB" },
        { "5 kHz",    "Onda corta" },
        { "9 kHz",    "Onda media" },
        { "10 kHz",   "CB y OM americana" },
        { "12,5 kHz", "Marina y 2 m" },
        { "25 kHz",   "Aeronáutica" },
        { "100 kHz",  "FM comercial" },
        { "1 MHz",    "Saltos grandes" },
    };
    ui_grid_state_t st;
    uint8_t i;

    memset(&st, 0, sizeof st);
    st.titulo = "Paso de sintonía";
    st.pagina = 0; st.paginas = 1;
    st.n = (uint8_t)(sizeof k / sizeof k[0]);
    for (i = 0; i < st.n; i++) {
        st.cel[i].l1 = k[i].et;
        st.cel[i].l2 = k[i].desc;
    }
    st.pressed = -1; st.cursor = 4; st.marcada = 3;
    st.nav[0] = "Anterior"; st.nav[1] = "Siguiente";
    st.nav_on[0] = 0; st.nav_on[1] = 0;
    fondo_principal();
    ui_grid_draw(&st);
    escribe("pasos");
}

int main(void)
{
    printf("menus del Malahit, 480 x 320\n");
    modos();
    bandas();
    pasos();
    if (ui_grid_recortes()) {
        printf("  ui_grid: %u renglones recortados\n",
               (unsigned)ui_grid_recortes());
        s_fallos++;
    }
    printf(s_fallos ? "\n%d FALLOS\n" : "\nTODO CABE (%d fallos)\n", s_fallos);
    return s_fallos ? 1 : 0;
}
