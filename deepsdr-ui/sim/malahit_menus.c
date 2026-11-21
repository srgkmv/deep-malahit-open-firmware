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
#include "temas_tabla.h"
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
        /* La lista de malahit/bringup/src/menus.c, k_modos[]; AM es
         * "Envolvente" porque "Amplitud modulada" no cabe. WFM desde el
         * 21/11/2026. */
        { "AM",  "Envolvente" },
        { "SAM", "AM síncrona" },
        { "USB", "Banda lateral alta" },
        { "LSB", "Banda lateral baja" },
        { "NFM", "FM estrecha" },
        { "WFM", "FM ancha" },
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

/* Lo que la radio sintoniza: RADIO_TUNE_MIN_HZ..RADIO_TUNE_MAX_HZ de su
 * src/menus.h - 100 kHz a 180 MHz desde el 21/11/2026. */
static int sintoniza(int k) { return banda[k].hz >= 100000UL && banda[k].hz <= 180000000UL; }

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
    /* paso_tabla[] es TUNE_STEPS_LISTA de main.c, generada en
     * bandas_tabla.h: la misma lista que usa la radio. */
    ui_grid_state_t st;
    uint8_t i;

    memset(&st, 0, sizeof st);
    st.titulo = "Paso de sintonía";
    st.pagina = 0; st.paginas = 1;
    st.n = (uint8_t)N_PASOS;
    for (i = 0; i < st.n; i++) {
        st.cel[i].l1 = paso_tabla[i].rotulo;
        st.cel[i].l2 = paso_tabla[i].desc;
    }
    st.pressed = -1; st.cursor = 4; st.marcada = 3;
    st.nav[0] = "Anterior"; st.nav[1] = "Siguiente";
    st.nav_on[0] = 0; st.nav_on[1] = 0;
    fondo_principal();
    ui_grid_draw(&st);
    escribe("pasos");
}

/*
 * AJUSTES: la rejilla 3 x 3 de ui_cfg, el nombre arriba en pequeño y el
 * valor abajo en grande. El valor se pinta con gfx2_text() desde x + 8 y
 * NO se recorta, asi que uno largo invade la celda de al lado: se miden
 * los que mas varian - los nombres de los 18 temas y de todas las paletas
 * del waterfall - contra lo que queda en la celda.
 */
static void ajustes(void)
{
    ui_cfg_state_t st;
    int i, pal_n = spectrum_palette_count();
    int16_t hueco = (int16_t)(UIC_CELL_W - UIC_NOM_X - 4);

    for (i = 0; i < SIM_TEMA_COUNT; i++) {
        cabe("tema", k_sim_temas[i].nombre, &font_ui_11, hueco);
    }
    for (i = 0; i < pal_n; i++) {
        cabe("paleta", spectrum_palette_nombre((uint8_t)i), &font_ui_11, hueco);
    }

    memset(&st, 0, sizeof st);
    st.cat[0] = "Radio"; st.cat[1] = "Pantalla"; st.cat[2] = "Espectro";
    st.cats = 3; st.cat_sel = 1; st.denso = 0;
    st.marcada = 0xFF; st.cursor = 0; st.pressed = -1;
    st.n = 4;
    st.cel[0].nombre = "Tema";    st.cel[0].valor = k_sim_temas[1].nombre;
    st.cel[1].nombre = "Paleta";  st.cel[1].valor = spectrum_palette_nombre(0);
    st.cel[2].nombre = "Estilo";  st.cel[2].valor = "Relleno";
    st.cel[3].nombre = "Volumen"; st.cel[3].valor = "-46 dB";
    fondo_principal();
    ui_cfg_draw(&st);
    escribe("ajustes");
}

/*
 * DETALLE: ui_det.c, el "-" y el "+" a los lados del valor. El valor va en
 * font_num_31 (la de la frecuencia: MALAHIT_FONTS_TOP la pone en lugar de
 * font_num_44) y la linea de debajo en font_ui_10; se miden los textos que
 * pone src/menus.c contra el hueco entre los dos botones.
 */
#include "ui_det.h"
extern const gfx2_font_t font_num_31;

static void detalle(void)
{
    static const char *const valores[] = { "-10 dB", "60 dB", "4080", "-60 dB", "95" };
    static const char *const pies[] = {
        "abajo del todo: abierto siempre", "encendida (botón Ruido)",
        "apagada: se enciende con Ruido",
        "reducción de ganancia del sintonizador",
        "% promediado entre fotogramas" };
    int16_t hueco = (int16_t)(UID_BTN_X1 - UID_VAL_GAP
                              - (UID_BTN_X0 + UID_BTN_W + UID_VAL_GAP));
    ui_det_state_t st;
    unsigned i;

    for (i = 0; i < sizeof valores / sizeof valores[0]; i++) {
        cabe("valor", valores[i], &font_num_31, hueco);
    }
    for (i = 0; i < sizeof pies / sizeof pies[0]; i++) {
        cabe("pie", pies[i], &font_ui_10, hueco);
    }
    cabe("titulo", "Reducción de ruido", &font_ui_11, GFX2_W);
    /* Suavizado's cell on the settings grid: name and "95 %". */
    cabe("ajuste", "Suavizado", &font_ui_11, (int16_t)(UIC_CELL_W - 2 * UIC_NOM_X));
    cabe("ajuste", "95 %", &font_ui_10, (int16_t)(UIC_CELL_W - 2 * UIC_NOM_X));

    memset(&st, 0, sizeof st);
    st.titulo = "Reducción de ruido";
    st.valor  = "48";
    st.pie    = pies[2];
    st.pressed = -1; st.preset_on = -1;
    fondo_principal();
    ui_det_draw(&st);
    escribe("detalle");
}

/*
 * TECLADO: ui_kbd.c, el de la frecuencia - lo que abre un toque en la
 * frecuencia de la cabecera. Los rotulos son los de upstream
 * (k_kbd_freq_rot[] en su main.c, los que copia src/menus.c). Se mide:
 * cada rotulo contra su tecla; la lectura mas larga que se puede teclear
 * -nueve cifras y la coma- contra lo que le deja la pista a su izquierda,
 * porque las dos comparten filas; y que el centro de cada tecla la
 * devuelve a ella y la franja de arriba no responde.
 */
#include "ui_kbd.h"

static void teclado(void)
{
    static const char *const rot[UIK_KEYS] = {
        "1", "2", "3", "Borrar",
        "4", "5", "6", "Vaciar",
        "7", "8", "9", "Volver",
        ",", "0", "kHz", "MHz",
    };
    static const uint8_t tip[UIK_KEYS] = {
        UIK_DIGITO, UIK_DIGITO, UIK_DIGITO, UIK_BORRA,
        UIK_DIGITO, UIK_DIGITO, UIK_DIGITO, UIK_BORRA,
        UIK_DIGITO, UIK_DIGITO, UIK_DIGITO, UIK_VUELVE,
        UIK_DIGITO, UIK_DIGITO, UIK_ACEPTA, UIK_ACEPTA,
    };
    const char *titulo = "Frecuencia", *pista = "teclea y elige la unidad";
    ui_kbd_state_t st;
    uint8_t i;

    for (i = 0U; i < UIK_KEYS; i++) {
        cabe("tecla", rot[i], tip[i] == UIK_DIGITO ? &font_num_31 : &font_ui_11,
             (int16_t)(UIK_CELL_W - 8));
    }
    {
        int16_t izq = gfx2_text_w(pista, &font_ui_10);
        int16_t t   = gfx2_text_w(titulo, &font_ui_10);

        if (t > izq) { izq = t; }
        cabe("lectura", "999999999,", &font_num_31,
             (int16_t)(GFX2_W - 2 * (UIK_MARGIN + UIK_TXT_PAD) - izq - 8));
    }
    if (UIK_GRID_Y + UIK_ROWS * UIK_CELL_H + (UIK_ROWS - 1) * UIK_GAP > UIK_Y + UIK_H) {
        printf("  SE SALE  teclado: la rejilla pasa de la zona de menu\n");
        s_fallos++;
    }
    for (i = 0U; i < UIK_KEYS; i++) {
        int cx = UIK_MARGIN + (i % UIK_COLS) * (UIK_CELL_W + UIK_GAP) + UIK_CELL_W / 2;
        int cy = UIK_GRID_Y + (i / UIK_COLS) * (UIK_CELL_H + UIK_GAP) + UIK_CELL_H / 2;

        if (ui_kbd_hit((uint16_t)cx, (uint16_t)cy) != (int8_t)i) {
            printf("  TOQUE    la tecla %u no responde en su centro\n", (unsigned)i);
            s_fallos++;
        }
    }
    if (ui_kbd_hit(GFX2_W / 2, (uint16_t)(UIK_Y + UIK_READ_H / 2)) != UIK_HIT_NONE) {
        printf("  TOQUE    la franja de la lectura responde como tecla\n");
        s_fallos++;
    }

    memset(&st, 0, sizeof st);
    st.titulo  = titulo;
    st.pista   = pista;
    st.lectura = "14,074";
    st.pressed = 6;                     /* el "6", con el dedo encima */
    st.cursor  = -1;
    for (i = 0U; i < UIK_KEYS; i++) { st.tecla[i] = rot[i]; st.tipo[i] = tip[i]; }
    fondo_principal();
    ui_kbd_draw(&st);
    escribe("teclado");

    /* Vacio, como se abre: las rayas. */
    st.lectura = "";
    st.pressed = -1;
    fondo_principal();
    ui_kbd_draw(&st);
    escribe("teclado_vacio");
}

/*
 * EN INGLES - 21/11/2026: el port habla los dos idiomas de upstream
 * (idioma.c), y el ingles es mas largo justo donde menos sitio hay. Se
 * miden los textos ingleses de src/menus.c y src/main.c contra los mismos
 * huecos que los espanoles de arriba, y se pintan dos pantallas en ingles
 * para mirarlas. Las cadenas son las de esos dos ficheros, copiadas: si
 * se cambia una alli, se cambia aqui.
 */
#include "idioma.h"
extern const gfx2_font_t font_ui_10b;

/* src/menus.c, paso_l2(): upstream's English for the 10 kHz step, "CB and
 * MW in the Americas", is 136 px in a 114 px cell, so the port says it
 * shorter. Same rule as AM's "Envolvente". */
static const char *paso_en(int k)
{
    return paso_tabla[k].hz == 10000UL ? "CB, MW Americas" : paso_tabla[k].desc_en;
}

static void ingles(void)
{
    static const char *const modos_en[] = {
        "Envelope", "Synchronous AM", "Upper sideband", "Lower sideband",
        "Narrow FM", "Wide FM" };
    static const char *const paginas_en[] = { "Radio", "Display", "Spectrum", "Device" };
    static const char *const nombres_en[] = {
        "AGC", "Bandwidth", "Notch", "Squelch", "Noise red.", "Blanker", "Preamp",
        "Attenuator", "Theme", "Palette", "Style", "Volume", "Smoothing", "Bridge",
        "Trace", "Language" };
    static const char *const valores_en[] = {
        "No AGC", "Slow", "Medium", "Fast", "Gentle", "Normal", "Strong", "Maximum",
        "Off", "On", "Filled", "Line", "Outline", "Joined", "Loose", "White",
        "Palette", "English", "Español", "-60 dB", "0-4,0k", "15 k", "95 %" };
    static const char *const titulos_en[] = {
        "Squelch", "Noise reduction", "Volume", "Attenuator", "Smoothing" };
    static const char *const pies_en[] = {
        "at the bottom: always open", "on (Noise button)",
        "off: the Noise button turns it on", "the tuner's gain reduction",
        "% averaged between frames" };
    static const char *const teclas_en[] = { "Delete", "Clear", "Back", "kHz", "MHz" };
    static const char *const barra_en[] = {
        "Bands", "Mode", "Step", "Volume", "Noise", "Settings", "on", "off" };
    int16_t hueco_aj  = (int16_t)(UIC_CELL_W - UIC_NOM_X - 4);
    int16_t hueco_cat = (int16_t)(UIC_SIDE_W - UIC_CAT_TXT_X - UIC_CAT_TXT_R);
    int16_t hueco_det = (int16_t)(UID_BTN_X1 - UID_VAL_GAP
                                  - (UID_BTN_X0 + UID_BTN_W + UID_VAL_GAP));
    ui_cfg_state_t st;
    unsigned i;
    int k;

    idioma_pon(IDIOMA_EN);

    for (i = 0; i < sizeof modos_en / sizeof modos_en[0]; i++) {
        cabe("desc en", modos_en[i], &font_ui_10, UIC_DCELL_W - 2);
    }
    cabe("categ en", "Analogue", &font_ui_11, hueco_cat);
    for (i = 0; i < sizeof familia_nombre_en / sizeof familia_nombre_en[0]; i++) {
        cabe("categ en", familia_nombre_en[i], &font_ui_11, hueco_cat);
    }
    for (k = 0; k < N_BANDAS; k++) {
        if (!sintoniza(k)) { continue; }
        cabe("banda en", banda[k].nombre_en, &font_ui_11, UIC_DCELL_W - 2);
    }
    for (i = 0; i < sizeof paginas_en / sizeof paginas_en[0]; i++) {
        cabe("pagina en", paginas_en[i], &font_ui_11, hueco_cat);
    }
    for (i = 0; i < sizeof nombres_en / sizeof nombres_en[0]; i++) {
        cabe("ajuste en", nombres_en[i], &font_ui_10, hueco_aj);
    }
    for (i = 0; i < sizeof valores_en / sizeof valores_en[0]; i++) {
        cabe("valor en", valores_en[i], &font_ui_11, hueco_aj);
    }
    for (k = 0; k < SIM_TEMA_COUNT; k++) {
        cabe("tema en", k_sim_temas[k].nombre_en, &font_ui_11, hueco_aj);
    }
    for (k = 0; k < spectrum_palette_count(); k++) {
        cabe("paleta en", spectrum_palette_nombre((uint8_t)k), &font_ui_11, hueco_aj);
    }
    for (i = 0; i < sizeof titulos_en / sizeof titulos_en[0]; i++) {
        cabe("titulo en", titulos_en[i], &font_ui_11, GFX2_W);
    }
    for (i = 0; i < sizeof pies_en / sizeof pies_en[0]; i++) {
        cabe("pie en", pies_en[i], &font_ui_10, hueco_det);
    }
    for (i = 0; i < sizeof teclas_en / sizeof teclas_en[0]; i++) {
        cabe("tecla en", teclas_en[i], &font_ui_11, (int16_t)(UIK_CELL_W - 8));
    }
    {
        int16_t izq = gfx2_text_w("type it and pick the unit", &font_ui_10);

        cabe("lectura en", "999999999,", &font_num_31,
             (int16_t)(GFX2_W - 2 * (UIK_MARGIN + UIK_TXT_PAD) - izq - 8));
    }
    for (i = 0; i < sizeof barra_en / sizeof barra_en[0]; i++) {
        cabe("barra en", barra_en[i], &font_ui_11, (int16_t)(UI_ACT_BTN_W - 4));
    }

    /* Los pasos en ingles: ui_grid cuenta sus propios recortes, una vez
     * por franja de dibujo; aqui se dice cual. */
    for (k = 0; k < N_PASOS; k++) {
        cabe("paso en", paso_en(k), &font_ui_10b, UIG_CELL_W);
    }
    {
        ui_grid_state_t g;

        memset(&g, 0, sizeof g);
        g.titulo = "Tuning step";
        g.pagina = 0; g.paginas = 1;
        g.n = (uint8_t)N_PASOS;
        for (i = 0; i < g.n; i++) {
            g.cel[i].l1 = paso_tabla[i].rotulo;
            g.cel[i].l2 = paso_en((int)i);
        }
        g.pressed = -1; g.cursor = -1; g.marcada = 3;
        g.nav[0] = "Previous"; g.nav[1] = "Next";
        fondo_principal();
        ui_grid_draw(&g);
        escribe("pasos_en");
    }

    /* Ajustes, la pagina nueva - Device, con Language - para mirarla. */
    memset(&st, 0, sizeof st);
    for (i = 0; i < 4; i++) { st.cat[i] = paginas_en[i]; }
    st.cats = 4; st.cat_sel = 3; st.denso = 0;
    st.marcada = 0xFF; st.cursor = -1; st.pressed = -1;
    st.n = 1;
    st.cel[0].nombre = "Language"; st.cel[0].valor = "English";
    fondo_principal();
    ui_cfg_draw(&st);
    escribe("ajustes_en");

    idioma_pon(IDIOMA_ES);
}

int main(void)
{
    printf("menus del Malahit, 480 x 320\n");
    modos();
    bandas();
    pasos();
    ajustes();
    detalle();
    teclado();
    ingles();
    if (ui_grid_recortes()) {
        printf("  ui_grid: %u renglones recortados\n",
               (unsigned)ui_grid_recortes());
        s_fallos++;
    }
    printf(s_fallos ? "\n%d FALLOS\n" : "\nTODO CABE (%d fallos)\n", s_fallos);
    return s_fallos ? 1 : 0;
}
