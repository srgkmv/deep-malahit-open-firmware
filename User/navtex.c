/*
 * NAVTEX (SITOR-B). Ver navtex.h para el porque de cada pieza.
 */
#include "navtex.h"
#include "nco.h"
#include <string.h>

#ifdef __GNUC__
#define TCMRAM_BSS __attribute__((section(".tcmram")))
#else
#define TCMRAM_BSS
#endif

/* ==========================================================================
 * EL ALFABETO CCIR 476
 * ==========================================================================
 *
 * Indexado por el codigo de 7 bits con el PRIMER BIT RECIBIDO ARRIBA (B1 en
 * el bit 6). Ese orden no es una suposicion: la norma define el relleno
 * "alfa" como 0001111 y la peticion de repeticion "beta" como 1100110, y
 * leidos asi salen 0x0F y 0x66, que son justo las dos casillas de control de
 * esta tabla. Leidos al reves saldrian 0x78 y 0x66, y 0x78 es el retorno de
 * carro: la tabla no cuadraria.
 *
 * Solo 35 de las 128 casillas llevan algo, las que tienen exactamente cuatro
 * unos. Las demas estan a 0 y eso ES la deteccion de error.
 */
static const char k_ltrs[128] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,'J', 0,0,0,'F', 0,'C','K',0,
    0,0,0,0,0,0,0,'W', 0,0,0,'Y', 0,'P','Q',0,
    0,0,0,0,0,'G',0,0, 0,'M','X',0, 'V',0,0,0,
    0,0,0,0,0,0,0,'A', 0,0,0,'S', 0,'I','U',0,
    0,0,0,'D', 0,'R','E',0, 0,'N',0,0, ' ',0,0,0,
    0,0,0,'Z', 0,'L',0,0, 0,'H',0,0, '\n',0,0,0,
    0,'O','B',0, 'T',0,0,0, '\r',0,0,0, 0,0,0,0
};

static const char k_figs[128] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,'\'', 0,0,0,'!', 0,':','(',0,
    0,0,0,0,0,0,0,'2', 0,0,0,'6', 0,'0','1',0,
    0,0,0,0,0,'&',0,0, 0,'.','/',0, ';',0,0,0,
    0,0,0,0,0,0,0,'-', 0,0,0,0,   0,'8','7',0,
    0,0,0,'$', 0,'4','3',0, 0,',',0,0, ' ',0,0,0,
    0,0,0,'"', 0,')',0,0, 0,'#',0,0, '\n',0,0,0,
    0,'9','?',0, '5',0,0,0, '\r',0,0,0, 0,0,0,0
};

uint8_t navtex_codigo_valido(uint8_t cod)
{
    uint8_t n = 0U, i;
    if (cod > 0x7FU) { return 0U; }
    for (i = 0U; i < 7U; i++) { if (cod & (1U << i)) { n++; } }
    return (uint8_t)(n == 4U);
}

char navtex_cod_a_car(uint8_t cod, uint8_t figuras)
{
    if (cod > 0x7FU) { return 0; }
    return figuras ? k_figs[cod] : k_ltrs[cod];
}

uint8_t navtex_car_a_cod(char c, uint8_t figuras)
{
    const char *t = figuras ? k_figs : k_ltrs;
    uint16_t i;
    for (i = 0U; i < 128U; i++) {
        if (t[i] == c && c != 0) { return (uint8_t)i; }
    }
    return 0xFFU;
}

/* ==========================================================================
 * ESTADO
 * ========================================================================== */

/* La media movil se hace sobre UN BIT. A 12 kHz y 100 baudios son 120
 * muestras; el tope deja sitio de sobra por si algun dia cambia la tasa. */
#define SPB_MAX   192U

/* Cinco posiciones de caracter, que son los 35 bits que la norma separa las
 * dos copias del mismo caracter. */
#define DIV_SALTO 5U

/* Cuantos bits se miran antes de decidir donde empieza el caracter. 7 x 32:
 * suficiente para que el desplazamiento bueno se despegue del 27% que sale
 * por azar, y solo dos segundos y pico de espera. */
#define BUSCA_BITS 224U

/*
 * LOS 42 BITS QUE HACEN FALTA PARA VER UNA PAREJA - 02/10/2026.
 *
 * Era cierto, y el fallo estaba en como se elegia el desplazamiento.
 * Contar codigos validos es un criterio ROMO: el desplazamiento bueno saca
 * el 100%, pero el de al lado saca el 70-75% el solo. Al correr un bit, un
 * codigo de cuatro unos SIGUE teniendo cuatro unos siempre que el bit que
 * sale valga igual que el que entra, o sea la mitad de las veces. La regla
 * que habia -"que saque mas del doble que el segundo"- pedia que el segundo
 * bajara del 50%, y eso solo pasa durante la señal de fase del principio,
 * que es lo unico que probaba el banco. Entrando a mitad de emision, el
 * buscador encontraba el sitio bueno con 32 de 32 y lo rechazaba.
 *
 * El criterio AFILADO es la repeticion: en el SITOR-B cada caracter se manda
 * dos veces, separadas cinco posiciones. En el desplazamiento bueno la mitad
 * de los caracteres son identicos al de cinco antes; en cualquier otro no
 * coinciden nunca, porque dos codigos sueltos coinciden una vez entre 128.
 * 50% contra 0% en vez de 100% contra 75%.
 *
 * Y para mirarlo no hace falta un historial por desplazamiento: cinco
 * caracteres son 35 bits, asi que con los ultimos 42 bits en un entero se
 * tiene a la vez el caracter de ahora (bits 0..6) y el de cinco antes EN SU
 * MISMO DESPLAZAMIENTO (bits 35..41), sea cual sea ese desplazamiento. Son
 * ocho bytes en vez de treinta y cinco.
 *
 * Los dos criterios conviven porque cubren casos distintos: durante la señal
 * de fase no hay NINGUNA pareja -relleno y peticion se alternan, y cinco es
 * impar, asi que nunca caen iguales- y ahi el unico que decide es el conteo
 * de codigos validos, que entonces si despega limpio. En cuanto empieza el
 * texto manda la repeticion.
 */
#define PAR_MIN  6U    /* parejas minimas para fiarse (de ~16 posibles) */

static uint8_t s_on;
static float   s_fs;

/* --- 1. los dos tonos --- */
static nco_t   s_nco_m, s_nco_e;
static float   s_centro_hz = NAVTEX_CENTRO_HZ;
static uint8_t s_invertido;

static TCMRAM_BSS float s_mi[SPB_MAX], s_mq[SPB_MAX];
static TCMRAM_BSS float s_ei[SPB_MAX], s_eq[SPB_MAX];
static uint16_t s_hidx;
static uint16_t s_spb;             /* muestras por bit */
static float    s_smi, s_smq, s_sei, s_seq;   /* las cuatro sumas moviles */
static uint16_t s_llenado;         /* muestras metidas, hasta llenar la ventana */

/* --- 2. el reloj de bit --- */
#define SIM_KP      0.020f
#define SIM_KI      0.000010f
#define SIM_KI_TOPE 0.0050f
static uint32_t s_fase, s_inc, s_inc_nom;
static float    s_err_int;
static float    s_d_medio;     /* el discriminador en el borde (Gardner) */
static float    s_d_ant;       /* el del centro anterior */
static float    s_nivel;       /* AGC lento, para normalizar el error */

/* --- 3. sincronismo de caracter --- */
static uint8_t  s_reg;          /* los ultimos 7 bits */
static uint32_t s_bits;         /* bits recibidos desde el arranque */
/* Los dos cuentan como mucho BUSCA_BITS/7 = 32 por ventana, asi que un byte
 * sobra. Con uint16_t el enlazador se planto por ocho bytes al añadir los 42
 * bits de s_bits42, y esto los devuelve con creces. */
static uint8_t  s_punt[7];      /* codigos validos por desplazamiento */
static uint8_t  s_pares[7];     /* y parejas que cuadran, por desplazamiento */
static uint64_t s_bits42;       /* los ultimos 42 bits: ver PAR_MIN */
static uint16_t s_punt_n;       /* bits contados en esa busqueda */
static uint8_t  s_desp;         /* el desplazamiento elegido, 0..6 */
static uint8_t  s_tiene_desp;
static uint16_t s_vig_ok, s_vig_n;   /* vigilancia del enganche, ver bit_nuevo() */
#define RECOLOCA_MARGEN 4U           /* ver "enganchado, se sigue mirando" en bit_nuevo() */
static uint16_t s_recolocados;       /* cuantas veces ha pasado a otro desplazamiento */

/* --- 4. diversidad temporal --- */
/*
 * Historial de caracteres recibidos, en anillo. No es solo para emparejar
 * cada caracter con su copia de cinco posiciones antes: es para PODER VOLVER
 * ATRAS.
 *
 * Mientras no se sabe cual de las dos paridades lleva las repeticiones no se
 * puede emitir nada, y averiguarlo cuesta una docena de caracteres. Sin
 * historial, esa docena se pierde - y justo esos son el "ZCZC" y la cabecera
 * que dice quien emite y de que va el mensaje, o sea lo unico que hay que
 * leer si solo se va a leer una cosa. Con 64 caracteres de historial (cuatro
 * segundos y medio) se puede rebobinar y sacarlos cuando por fin se sabe.
 */
#define HIST_N 64U   /* potencia de dos: el modulo sale gratis */
static uint8_t  s_hist[HIST_N];
static uint32_t s_car_n;                  /* caracteres desde el enganche */
static uint16_t s_par_cuad[2];            /* parejas que cuadran, por paridad */
static uint8_t  s_paridad;
static uint8_t  s_tiene_par;

/* --- 5. salida --- */
#define RING_N 256U
static char     s_ring[RING_N];
static uint16_t s_ring_cab, s_ring_col;
static uint8_t  s_figuras;

/* --- 6. cuentas y cabecera --- */
static navtex_info_t s_info;
static uint8_t  s_zczc;        /* cuantas letras de "ZCZC" llevamos vistas */
static uint8_t  s_cab_n;       /* cuantas de la cabecera llevamos capturadas */
static uint8_t  s_cab_plazo;   /* caracteres que lleva esperando esa cabecera */
static uint8_t  s_nnnn;
static uint8_t  s_en_mensaje;

/* ==========================================================================
 * ARRANQUE
 * ========================================================================== */
/*
 * EL CENTRO DE LOS TONOS, MEDIDO - 02/10/2026.
 *
 * Los dos filtros de tono solo cogen la señal si el centro esta a menos de
 * unos 60 Hz del de verdad. Medido en la grabacion de 8424 kHz: engancha
 * entre 1050 y 1175 Hz y fuera de ahi no, ni un caracter.
 *
 * Y nadie ponia ese centro: navtex_set_centro_hz() no se llamaba desde
 * NINGUN sitio del firmware, asi que la radio escuchaba siempre en 1700 Hz.
 * Los tonos del usuario caian en 1108 -las listas dan la frecuencia CENTRO
 * para recibir en FSK, y en banda lateral hay que bajar el dial 1,7 kHz para
 * que caigan ahi-, o sea 592 Hz fuera. Con 60 Hz de margen y un paso de
 * sintonia de 500 Hz, acertar a mano es loteria.
 *
 * LO PRIMERO QUE PROBE fue ir moviendo el centro de cien en cien hasta que
 * enganchara. No vale: al centro bueno hay que darle cinco o seis segundos
 * -el lazo de reloj tiene que asentarse y las parejas tienen que acumularse-
 * y multiplicado por quince sitios son mas de setenta segundos de vuelta. En
 * la grabacion de Oostende el buscador pasaba por el centro bueno mientras
 * todavia estaba asentandose, lo descartaba, y se iba a dar la vuelta.
 *
 * El centro no hay que buscarlo: SE MIDE. Una FSK de dos tonos equiprobables
 * cruza el cero, de media, a la frecuencia de en medio de los dos, asi que
 * contar cruces por cero durante un segundo lo da directamente y sin una
 * sola multiplicacion. Contra las dos grabaciones:
 *
 *     Oostende  2204 Hz de verdad -> 2223 medido  (+19)
 *     8424 kHz  1108 Hz de verdad -> 1136 medido  (+28)
 *
 * Los dos dentro del margen de 60, y estables (5 Hz de dispersion en la
 * limpia, 77 en la del movil). Un segundo en vez de setenta.
 *
 * El sesgo de +20 o +30 Hz es real y conocido: lo mete el ruido de banda
 * ancha que hay por encima de los tonos, que añade cruces. No se corrige
 * porque cabe de sobra en el margen, y corregir un sesgo medido en dos
 * grabaciones seria ajustar a dos puntos.
 *
 * PERO LA MEDIDA NO MANDA DE ENTRADA, y esto lo enseño el banco: con ruido
 * de banda ancha encima -hasta tres veces la señal, que es la prueba 3- los
 * cruces los pone el ruido y la cuenta se va. Donde el centro del ajuste ya
 * era bueno, la medida lo estropeaba.
 *
 * Asi que primero manda el ajuste. Solo cuando una ventana de busqueda
 * entera -2,2 s- pasa sin enganchar se hace caso a la medida, que es justo
 * el caso del usuario: en 1700 Hz no hay nada que enganchar porque los tonos
 * estan en 1108. Lo que funciona no se toca, y lo que no funciona se mide.
 *
 * Con la grabacion de 8424 kHz y el centro por omision, de punta a punta:
 * engancha y saca "TON CHARTH TON HNOMENON ETHNON KAI TO DIETHNES" con cero
 * caracteres rotos, sin que nadie toque el ajuste de centro.
 *
 * Mientras no hay enganche se vuelve a medir cada segundo. En cuanto
 * engancha se deja quieto: una medida nueva con la señal desvaneciendose
 * movería el centro justo cuando mas falta hace que no se mueva.
 */
#define VUELTAS_ANTES_DE_MEDIR 1U
#define CRUCES_VENT 12000U   /* un segundo a 12 kHz */
#define CENTRO_MIN   700.0f
#define CENTRO_MAX  2800.0f

static uint16_t s_cruces;      /* cruces por cero en la ventana en curso */
static uint16_t s_cruces_n;    /* muestras contadas */
static uint8_t  s_signo_ant;   /* el signo de la muestra anterior */
static float    s_centro_med;  /* lo ultimo que se midio, 0 = todavia nada */
static uint8_t  s_vueltas;     /* ventanas de busqueda gastadas sin enganchar */

/* El centro medido en el espectro: ver navtex_espectro(), mas abajo. */
static uint8_t          s_esp_on;   /* 1 = alguien da espectro: los cruces callan */
static uint8_t          s_fijo;     /* 1 = el centro lo ha puesto el operador */
static volatile uint8_t s_pide;     /* el bucle pide un centro a la interrupcion */
static volatile float   s_pide_hz;

static void tonos_fija(void)
{
    float mitad = NAVTEX_SHIFT_HZ * 0.5f;
    float c = (s_centro_med > 0.0f) ? s_centro_med : s_centro_hz;
    float m = s_invertido ? (c - mitad) : (c + mitad);
    float e = s_invertido ? (c + mitad) : (c - mitad);

    nco_freq(&s_nco_m, m, s_fs);
    nco_freq(&s_nco_e, e, s_fs);
}

/* Un centro medido pasa a mandar, y el reloj de bit, que venia siguiendo
 * otra cosa, empieza limpio. Desde la interrupcion: navtex_process().
 *
 * SALVO QUE SEA UNA CORRECCION - 22/11/2026. Si el centro se mueve menos
 * de lo que aguantan los filtros (+-60 Hz), lo que el reloj venia
 * siguiendo ERA la señal, mal centrada pero ella, y ponerlo a cero tira
 * los segundos que lleva buscando. Solo se retocan los tonos. Para la
 * medida del espectro (`corrige`); los cruces, como estaban. */
static void centro_aplica(float f, uint8_t corrige)
{
    float ant = (s_centro_med > 0.0f) ? s_centro_med : s_centro_hz;
    float d = f - ant;

    s_centro_med = f;
    tonos_fija();
    if (d < 0.0f) { d = -d; }
    if (corrige && d < 60.0f) { return; }
    s_err_int = 0.0f;
    s_inc = s_inc_nom;
    memset(s_punt, 0, sizeof s_punt);
    memset(s_pares, 0, sizeof s_pares);
    s_punt_n = 0U;
}

/*
 * Una muestra mas para la cuenta de cruces. Cuando la ventana se llena, si
 * no hay enganche, el resultado pasa a ser el centro.
 *
 * Y SOLO SI NADIE DA ESPECTRO NI HA PUESTO EL CENTRO A MANO - 22/11/2026.
 * Ver navtex_espectro(): los cruces miden el centro del RUIDO en cuanto la
 * señal no lo tapa de sobra, y entonces mueven los filtros a donde no hay
 * nada. Se quedan para quien no tenga espectro que dar (el banco de las
 * grabaciones, sim/navtextest.c, que los prueba asi).
 */
static void cruces_mete(float x)
{
    uint8_t sg = (x >= 0.0f) ? 1U : 0U;

    if (sg != s_signo_ant) { s_cruces++; }
    s_signo_ant = sg;
    s_cruces_n++;
    if (s_cruces_n < CRUCES_VENT) { return; }

    if (!s_tiene_desp && s_vueltas >= VUELTAS_ANTES_DE_MEDIR && !s_esp_on && !s_fijo) {
        float f = (float)s_cruces * s_fs / (2.0f * (float)CRUCES_VENT);
        float d = f - s_centro_med;

        if (d < 0.0f) { d = -d; }
        /*
         * SOLO SI CAMBIA DE VERDAD. La medida se repite cada segundo y baila
         * unas decenas de hercios; aplicarla cada vez volvia a poner el reloj
         * de bit a cero una vez por segundo y asi no se asienta nunca -la
         * señal del usuario no enganchaba por esto, no por el centro-. Cuarenta
         * hercios es menos que el margen de los filtros, o sea que mientras
         * baile por debajo de eso no hay nada que mover.
         */
        if (f >= CENTRO_MIN && f <= CENTRO_MAX
            && (s_centro_med <= 0.0f || d > 40.0f)) {
            centro_aplica(f, 0U);
        }
    }
    s_cruces = 0U;
    s_cruces_n = 0U;
}

/* ==========================================================================
 * EL CENTRO, MEDIDO EN EL ESPECTRO - 22/11/2026, en el port del Malahit.
 * ==========================================================================
 *
 * LOS CRUCES POR CERO MIDEN EL CENTRO DEL RUIDO. Lo decia el comentario de
 * arriba -"con ruido de banda ancha encima los cruces los pone el ruido"- y
 * lo que no se habia medido es cuanto ruido hace falta. Muy poco: el ruido
 * de toda la banda de audio pesa en la cuenta como la señal, y en una
 * banda lateral de 4 kHz eso son 9 dB mas que en los 500 Hz donde se suele
 * dar la relacion señal/ruido. Medido en el Malahit, con un HackRF dando
 * un NAVTEX de verdad en 4209,5 kHz y el filtro de 0,3-4,0 kHz:
 *
 *     sin señal          el centro se va a 2300-2350 Hz (el del ruido)
 *     SNR 15 dB/500 Hz   vuelve hasta 1741 (la señal en ~1720): engancha
 *     SNR  8 dB/500 Hz   se queda en 2020-2100: NO engancha, ni una letra
 *
 * Y en el banco del ordenador, la señal en 1100 Hz con SNR 10: medida en
 * 2245, nada. El decodificador, puesto en su centro, lee limpio hasta SNR 3;
 * lo que se perdia entre 3 y 15 dB era culpa de donde escuchaba.
 *
 * LO QUE NO SE CONFUNDE CON EL RUIDO ES LA FORMA. Un NAVTEX en el espectro
 * son dos lomos separados 170 Hz, y el ruido es llano. El osciloscopio de
 * sintonia ya hace ese espectro (rtty_scope.c, 23,4 Hz por casilla, un
 * cuadro cada 43 ms) para pintarlo, asi que el bucle principal se lo pasa
 * a navtex_espectro(), que lo promedia ~1,4 s y busca la pareja:
 *
 *   - cada lomo (+-45 Hz alrededor de c-85 y c+85) contra el fondo de SU
 *     lado de fuera (de 90 a 180 Hz mas alla). Hacia fuera y no entre los
 *     dos, porque entre los dos esta la falda de los lomos. Y por separado:
 *     junto al corte del filtro de banda lateral el fondo de un lado cae en
 *     la banda eliminada, y un "fondo" medio entre los dos lados daria por
 *     buena una pareja hecha solo de ruido junto al corte. Por lados, el
 *     lomo que cae junto al corte pasa, pero el otro no;
 *   - la nota es el PEOR de los dos cocientes, asi que una portadora sola
 *     -un lomo, no dos- no pasa;
 *   - pasa con el doble (3 dB): la media de 32 cuadros deja el ruido en
 *     un +-10 % y un NAVTEX con SNR 0 dB en 500 Hz pone sus lomos a mas de
 *     2,5 veces el fondo.
 *
 * Y LAS REGLAS DE ARRIBA, LAS MISMAS: con la señal enganchada no se mueve
 * nada (un desvanecimiento moveria el centro justo cuando no debe), lo que
 * ya esta bien no se toca (25 Hz de tolerancia, menos que los +-60 que
 * aguantan los filtros) y el operador manda (navtex_set_centro_hz()). Lo
 * nuevo es lo que pasa cuando no hay señal: NADA. Sin pareja no se mide, y
 * el centro se queda donde estaba en vez de irse al del ruido.
 *
 * El bucle no toca los filtros: deja el centro en s_pide y la interrupcion
 * lo aplica al principio de su siguiente bloque.
 */
#define ESP_N          176U     /* casillas promediadas, 0..4125 Hz a 23,4 */
#define ESP_ALFA       0.03125f /* media exponencial de ~32 cuadros, 1,4 s */
#define ESP_MIN_CUAD   24U      /* cuadros antes de fiarse de la media */
#define ESP_CADA       6U       /* se busca cada seis cuadros nuevos */
#define ESP_LOMO_HZ    45.0f    /* media anchura de cada lomo */
#define ESP_FONDO0_HZ  90.0f    /* el fondo de cada lado, de 90... */
#define ESP_FONDO1_HZ  180.0f   /* ...a 180 Hz por fuera de su lomo */
#define ESP_UMBRAL     2.0f     /* cada lomo, el doble que su fondo */
#define ESP_MUEVE_HZ   25.0f    /* lo que tiene que discrepar para moverlo */
#define ESP_ACUERDO_HZ 15.0f    /* y lo que pueden discrepar las medidas */
#define ESP_CERCA_HZ   150.0f   /* navtex_centro_cerca(): cuanto mira */

static float    s_esp[ESP_N];
static float    s_esp_hz;       /* Hz por casilla de lo que llega */
static uint16_t s_esp_n;        /* casillas que valen */
static uint16_t s_esp_cuad;     /* cuadros promediados */
static uint32_t s_esp_visto;    /* el numero del ultimo cuadro metido */
static uint8_t  s_esp_visto_ok;
static uint8_t  s_esp_acuerdo;  /* busquedas seguidas que han dado lo mismo */
static float    s_esp_cand;     /* lo que dio la ultima */
static float    s_esp_ult;      /* lo ultimo encontrado, 0 = ninguna pareja */
static float    s_esp_nota;     /* y su nota */

static void esp_olvida(void)
{
    s_esp_cuad = 0U;
    s_esp_on = 0U;
    s_esp_visto_ok = 0U;
    s_esp_acuerdo = 0U;
    s_esp_cand = 0.0f;
    s_esp_ult = 0.0f;
    s_esp_nota = 0.0f;
}

/*
 * La media por casilla del espectro promediado entre f0 y f1 Hz, contando
 * de cada casilla la PARTE que cae dentro (la casilla k cubre de k-1/2 a
 * k+1/2). Con casillas enteras la energia de una ventana sube a saltos de
 * una casilla al correrla, el maximo se queda en el escalon y el centro
 * salia en dos sitios a 23 Hz uno de otro; asi la curva es continua.
 */
static float esp_media(float f0, float f1)
{
    float   x0 = f0 / s_esp_hz, x1 = f1 / s_esp_hz, sum = 0.0f;
    int32_t k, k0, k1;

    if (x0 < 0.5f) { x0 = 0.5f; }
    if (x1 > (float)s_esp_n - 0.5f) { x1 = (float)s_esp_n - 0.5f; }
    if (x1 <= x0) { return 0.0f; }
    k0 = (int32_t)(x0 + 0.5f);
    k1 = (int32_t)(x1 + 0.5f);
    if (k1 > (int32_t)s_esp_n - 1) { k1 = (int32_t)s_esp_n - 1; }
    for (k = k0; k <= k1; k++) {
        float a = (float)k - 0.5f, b = (float)k + 0.5f;
        if (a < x0) { a = x0; }
        if (b > x1) { b = x1; }
        if (b > a) { sum += s_esp[k] * (b - a); }
    }
    return sum / (x1 - x0);
}

/* Lo que se parece a una pareja de NAVTEX lo que hay centrado en c: el
 * peor de los dos lomos contra el fondo de su lado. */
static float esp_nota(float c)
{
    float m  = NAVTEX_SHIFT_HZ * 0.5f;
    float lo = esp_media(c - m - ESP_LOMO_HZ, c - m + ESP_LOMO_HZ);
    float hi = esp_media(c + m - ESP_LOMO_HZ, c + m + ESP_LOMO_HZ);
    float fl = esp_media(c - m - ESP_FONDO1_HZ, c - m - ESP_FONDO0_HZ);
    float fh = esp_media(c + m + ESP_FONDO0_HZ, c + m + ESP_FONDO1_HZ);
    float a, b;

    if (fl <= 0.0f || fh <= 0.0f) { return 0.0f; }
    a = lo / fl;
    b = hi / fh;
    return (a < b) ? a : b;
}

/*
 * DONDE ESTA, QUE NO ES LO MISMO QUE SI ESTA. La nota de arriba decide si
 * hay una pareja, y es mala para decir donde: corriendo c hacia arriba, el
 * fondo de la derecha se aleja de su lomo (su cociente sube) y el de la
 * izquierda se acerca al suyo (baja), y el peor de los dos apenas cambia en
 * +-35 Hz. Medido en el banco con los tonos en 1672: la mejor nota salia
 * en 1672 o en 1708 segun el cuadro, y en la radio el centro saltaba entre
 * los dos, y cada salto volvia a poner el reloj de bit a cero.
 *
 * El sitio lo da la ENERGIA de los dos lomos juntos, que es simetrica: con
 * c corrido, lo que gana una ventana lo pierde la otra, y el maximo cae en
 * el centro de verdad. Se busca a un cuarto de casilla (5,9 Hz) a +-60 Hz
 * de donde la nota encontro la pareja, y se afina con una parabola por los
 * tres puntos de alrededor del maximo.
 */
static float esp_energia(float c)
{
    float m = NAVTEX_SHIFT_HZ * 0.5f;

    return esp_media(c - m - ESP_LOMO_HZ, c - m + ESP_LOMO_HZ)
         + esp_media(c + m - ESP_LOMO_HZ, c + m + ESP_LOMO_HZ);
}

static float esp_situa(float c0)
{
    float paso = s_esp_hz * 0.25f, c, cm = c0, em = -1.0f, e0, e1, den, d;

    for (c = c0 - 60.0f; c <= c0 + 60.0f; c += paso) {
        float e = esp_energia(c);
        if (e > em) { em = e; cm = c; }
    }
    e0 = esp_energia(cm - paso);
    e1 = esp_energia(cm + paso);
    den = e0 - 2.0f * em + e1;
    if (den < 0.0f) {
        d = 0.5f * (e0 - e1) / den;
        if (d > -1.0f && d < 1.0f) { cm += d * paso; }
    }
    return cm;
}

/* La mejor pareja entre f0 y f1 (sus centros), y su nota. */
static float esp_busca(float f0, float f1, float *nota)
{
    float c, paso = s_esp_hz * 0.5f, mejor = 0.0f, cm = 0.0f;

    for (c = f0; c <= f1; c += paso) {
        float p = esp_nota(c);
        if (p > mejor) { mejor = p; cm = c; }
    }
    *nota = mejor;
    return (mejor > 0.0f) ? esp_situa(cm) : 0.0f;
}

void navtex_espectro(const float *pot, uint16_t n, float hz_casilla, uint32_t cuadro)
{
    uint16_t k;
    float    sum = 0.0f, inv, nota, c, d, ahora;

    if (!s_on || !pot || n < 16U || hz_casilla <= 0.0f) { return; }
    s_esp_on = 1U;
    if (s_esp_visto_ok && cuadro == s_esp_visto) { return; }   /* ese ya esta */
    /*
     * EL PRIMER CUADRO QUE SE VE NO SE PROMEDIA: es el que ya estaba hecho
     * al arrancar, o sea de antes - del modo anterior, de otro dial. Visto
     * en el banco: un tono que habia en el cuadro viejo se quedo en la
     * media diez segundos y el centro se fue a buscarlo en puro ruido.
     */
    if (!s_esp_visto_ok) {
        s_esp_visto = cuadro;
        s_esp_visto_ok = 1U;
        return;
    }
    s_esp_visto = cuadro;
    if (n > ESP_N) { n = ESP_N; }
    if (n != s_esp_n || hz_casilla != s_esp_hz) {
        s_esp_n = n;
        s_esp_hz = hz_casilla;
        s_esp_cuad = 0U;
    }

    /* Cada cuadro, a su propia media antes de promediar: un
     * desvanecimiento entero no es forma del espectro. */
    for (k = 1U; k < n; k++) { sum += pot[k]; }
    if (sum <= 0.0f) { return; }
    inv = (float)(n - 1U) / sum;
    /* Los primeros 32, media de verdad (1/1, 1/2, 1/3...); despues, la
     * exponencial. Arrancar la exponencial con el primer cuadro entero le
     * deja la mitad del peso veinte cuadros despues. */
    {
        float a = (s_esp_cuad < 32U) ? 1.0f / (float)(s_esp_cuad + 1U) : ESP_ALFA;
        for (k = 0U; k < n; k++) { s_esp[k] += a * (pot[k] * inv - s_esp[k]); }
    }
    if (s_esp_cuad < 0xFFFFU) { s_esp_cuad++; }
    if (s_esp_cuad < ESP_MIN_CUAD || (s_esp_cuad % ESP_CADA) != 0U) { return; }

    c = esp_busca(CENTRO_MIN, CENTRO_MAX, &nota);
    s_esp_nota = nota;
    if (nota < ESP_UMBRAL || c <= 0.0f) {
        s_esp_ult = 0.0f;
        s_esp_acuerdo = 0U;
        return;
    }
    s_esp_ult = c;

    /* Tres busquedas seguidas a menos de 15 Hz unas de otras, ~0,8 s. */
    d = c - s_esp_cand;
    if (d < 0.0f) { d = -d; }
    s_esp_acuerdo = (s_esp_acuerdo > 0U && d <= ESP_ACUERDO_HZ)
                  ? (uint8_t)((s_esp_acuerdo < 255U) ? s_esp_acuerdo + 1U : 255U) : 1U;
    s_esp_cand = c;
    if (s_esp_acuerdo < 3U || s_fijo || s_tiene_desp || s_pide) { return; }

    ahora = (s_centro_med > 0.0f) ? s_centro_med : s_centro_hz;
    d = c - ahora;
    if (d < 0.0f) { d = -d; }
    if (d <= ESP_MUEVE_HZ) { return; }
    s_pide_hz = c;
    s_pide = 1U;
}

float navtex_centro_cerca(float hz)
{
    float nota = 0.0f, c;

    if (!s_on || s_esp_cuad < ESP_MIN_CUAD || s_esp_hz <= 0.0f) { return hz; }
    c = esp_busca(hz - ESP_CERCA_HZ, hz + ESP_CERCA_HZ, &nota);
    return (c > 0.0f && nota >= 1.6f) ? c : hz;     /* el ruido llega a 1,5 */
}

void navtex_espectro_dbg(float *pareja_hz, float *nota)
{
    if (pareja_hz) { *pareja_hz = s_esp_ult; }
    if (nota)      { *nota = s_esp_nota; }
}

void navtex_start(float fs_hz)
{
    uint32_t spb;

    if (fs_hz <= 0.0f) { return; }
    s_fs = fs_hz;

    spb = (uint32_t)(fs_hz / NAVTEX_BAUD + 0.5f);
    if (spb < 8U)       { spb = 8U; }
    if (spb > SPB_MAX)  { spb = SPB_MAX; }
    s_spb = (uint16_t)spb;

    nco_init();
    nco_fase_cero(&s_nco_m);
    nco_fase_cero(&s_nco_e);
    s_centro_med = 0.0f;    /* ANTES de tonos_fija(): lo usa para el centro */
    s_cruces = 0U; s_cruces_n = 0U; s_signo_ant = 1U; s_vueltas = 0U;
    s_fijo = 0U;            /* lo que puso el operador valia para la emisora de antes */
    s_pide = 0U;
    esp_olvida();
    tonos_fija();

    memset(s_mi, 0, sizeof s_mi); memset(s_mq, 0, sizeof s_mq);
    memset(s_ei, 0, sizeof s_ei); memset(s_eq, 0, sizeof s_eq);
    s_hidx = 0U; s_llenado = 0U;
    s_smi = s_smq = s_sei = s_seq = 0.0f;

    s_fase = 0U;
    s_inc_nom = (uint32_t)((double)NAVTEX_BAUD / (double)fs_hz * 4294967296.0 + 0.5);
    s_inc = s_inc_nom;
    s_err_int = 0.0f;
    s_d_medio = s_d_ant = 0.0f;
    s_nivel = 1e-6f;

    s_reg = 0U; s_bits = 0UL; s_bits42 = 0ULL;
    memset(s_punt, 0, sizeof s_punt);
    memset(s_pares, 0, sizeof s_pares);
    s_punt_n = 0U; s_desp = 0U; s_tiene_desp = 0U;
    s_vig_ok = s_vig_n = 0U;
    s_recolocados = 0U;

    memset(s_hist, 0, sizeof s_hist);
    s_car_n = 0UL;
    s_par_cuad[0] = s_par_cuad[1] = 0U;
    s_paridad = 0U; s_tiene_par = 0U;

    s_ring_cab = s_ring_col = 0U;
    s_figuras = 0U;
    s_zczc = 0U; s_cab_n = 0U; s_cab_plazo = 0U; s_nnnn = 0U; s_en_mensaje = 0U;

    memset(&s_info, 0, sizeof s_info);
    s_on = 1U;
}

void navtex_stop(void)     { s_on = 0U; }
uint8_t navtex_activo(void){ return s_on; }

void navtex_set_centro_hz(float hz)
{
    if (hz < 300.0f || hz > 3000.0f) { return; }
    s_centro_hz = hz;
    s_centro_med = 0.0f;   /* si lo mueve el operador, manda el operador */
    s_vueltas = 0U;
    /*
     * Y MANDA HASTA QUE SE VUELVA A ENTRAR EN EL MODO - 22/11/2026. El
     * comentario de arriba lo prometia y no se cumplia: s_vueltas a cero
     * solo le daba una ventana de busqueda, 2,2 s, y si en ese tiempo no
     * enganchaba, los cruces por cero volvian a mover el centro a donde
     * dijera el ruido. Ahora ni los cruces ni el espectro lo tocan hasta
     * el proximo navtex_start().
     */
    s_fijo = 1U;
    if (s_on) { tonos_fija(); }
}
float navtex_get_centro_hz(void) { return s_centro_hz; }

void navtex_set_invertido(uint8_t inv)
{
    s_invertido = inv ? 1U : 0U;
    if (s_on) { tonos_fija(); }
}
uint8_t navtex_get_invertido(void) { return s_invertido; }

/* ==========================================================================
 * SALIDA
 * ========================================================================== */
static void ring_push(char c)
{
    uint16_t sig = (uint16_t)((s_ring_cab + 1U) % RING_N);
    if (sig == s_ring_col) { return; }   /* lleno: se tira, no se pisa */
    s_ring[s_ring_cab] = c;
    s_ring_cab = sig;
}

uint8_t navtex_get_char(char *out)
{
    if (s_ring_col == s_ring_cab) { return 0U; }
    if (out) { *out = s_ring[s_ring_col]; }
    s_ring_col = (uint16_t)((s_ring_col + 1U) % RING_N);
    return 1U;
}

/*
 * LA CABECERA. Un mensaje empieza con "ZCZC" y cuatro caracteres que dicen
 * quien emite (B1), de que va (B2) y un numero de mensaje (B3B4); termina con
 * "NNNN". Se sigue con dos contadores tontos en vez de un buffer porque lo
 * unico que hay que reconocer son dos palabras fijas.
 */
static void mira_cabecera(char c)
{
    static const char zc[4] = { 'Z', 'C', 'Z', 'C' };

    /*
     * REVISION A FONDO DEL 08/10/2026: LA CABECERA, CON PLAZO.
     *
     * Mientras s_cab_n estaba entre 1 y 4 esta funcion RETORNABA SIEMPRE,
     * y los espacios no consumian nada. O sea que un ZCZC seguido de
     * espacios -o de nada, porque la señal se fuera justo ahi- dejaba el
     * reconocedor atascado en este trozo para siempre: no se veian nuevos
     * ZCZC, no se veia el NNNN de cierre y s_en_mensaje se quedaba en 1
     * hasta que alguien apagara y encendiera el modo. Y en NAVTEX eso es
     * lo normal, no lo raro: las emisoras se turnan por horas y el final
     * de la emision llega en medio de cualquier cosa.
     *
     * La cabecera son CUATRO caracteres seguidos detras del ZCZC, asi que
     * si en doce no han llegado los cuatro es que no va a llegar ninguno.
     * Se suelta el trozo, se deja la cabecera sin marcar -que es la
     * verdad- y se vuelve a mirar ZCZC y NNNN como siempre.
     */
    if (s_cab_n > 0U && s_cab_n <= 4U) {
        if (c != ' ') {
            s_info.cab[s_cab_n - 1U] = c;
            s_cab_n++;
            if (s_cab_n > 4U) {
                s_info.cab[4] = '\0';
                s_info.tiene_cab = 1U;
                s_cab_n = 0U;
            }
            return;
        }
        s_cab_plazo++;
        if (s_cab_plazo < 12U) { return; }
        s_cab_n = 0U;          /* se acabo el plazo: esto no es cabecera */
    }
    s_cab_plazo = 0U;

    s_zczc = (c == zc[s_zczc]) ? (uint8_t)(s_zczc + 1U) : (uint8_t)(c == 'Z' ? 1U : 0U);
    if (s_zczc >= 4U) {
        s_zczc = 0U;
        s_cab_n = 1U;
        s_cab_plazo = 0U;
        s_en_mensaje = 1U;
        /*
         * Y LA CABECERA DEJA DE VALER AQUI MISMO - 09/10/2026.
         *
         * tiene_cab se subia al completar los cuatro caracteres y no se
         * bajaba NUNCA mas. Como los cuatro llegan de uno en uno -unos
         * 140 ms cada uno a 100 baudios-, durante medio segundo largo
         * cab[] era una MEZCLA de la cabecera anterior y la nueva, y la
         * pantalla la leia asi diecisiete veces: con "FA01" antes y
         * "GB02" llegando, se lee "GB01". Eso no es un dato viejo, es un
         * identificador QUE NO HA EXISTIDO NUNCA en el aire -otra
         * estacion, otro tipo de mensaje y el numero de serie de un
         * tercero-, y en NAVTEX la letra de estacion es justo lo que se
         * mira para saber si el aviso es de tu zona.
         *
         * Bajandola aqui, mientras se captura la nueva se sigue viendo la
         * anterior: viejo pero cierto, que es lo unico aceptable de las
         * dos cosas.
         */
        s_info.tiene_cab = 0U;
        return;
    }

    if (s_en_mensaje) {
        s_nnnn = (c == 'N') ? (uint8_t)(s_nnnn + 1U) : 0U;
        if (s_nnnn >= 4U) {
            s_nnnn = 0U;
            s_en_mensaje = 0U;
            s_info.mensajes++;
        }
    }
}

/* Un caracter ya decidido (el bueno de las dos copias) se traduce y sale. */
/*
 * VOLVER A LETRAS AL SALTAR DE RENGLON - 03/10/2026.
 *
 * Aquello no era ruido: eran las mismas letras impresas por la columna de
 * las cifras. Se habia perdido un cambio a letras -los dos ejemplares, el
 * suyo y su copia- y de ahi no se salia: emite() solo volvia a letras si
 * llegaba limpio otro codigo de letras, y mientras tanto el mensaje entero
 * sale en cifras. Un solo caracter roto, y lo que viene detras ilegible.
 *
 * El remedio es el de siempre en los teletipos: un salto de renglon vuelve a
 * letras. Un mensaje NAVTEX va por renglones y todos empiezan en letras, asi
 * que el salto es una marca de sincronismo que la señal trae gratis.
 *
 * LO QUE CUESTA, dicho claro: una tabla de numeros repartida en varios
 * renglones necesita que la emisora repita el cambio a cifras en cada uno.
 * Lo hacen, precisamente porque todos los receptores hacen esto; pero si una
 * no lo hiciera, sus numeros saldrian como letras. Se cambia un fallo que
 * estropea el resto del mensaje por otro que, como mucho, estropea un
 * renglon y solo en una emisora que se salte la costumbre.
 */
static void emite(uint8_t cod)
{
    char c;

    if (cod == NAVTEX_COD_LTRS) { s_figuras = 0U; return; }
    if (cod == NAVTEX_COD_FIGS) { s_figuras = 1U; return; }
    if (cod == NAVTEX_COD_ALFA || cod == NAVTEX_COD_REP) { return; }

    c = navtex_cod_a_car(cod, s_figuras);
    if (c == 0) { return; }
    if (c == '\r' || c == '\n') { s_figuras = 0U; }
    ring_push(c);
    mira_cabecera(c);
}

/* ==========================================================================
 * 4. DIVERSIDAD TEMPORAL
 * ==========================================================================
 *
 * Llega un caracter nuevo. Si esta en una posicion de REPETICION, su pareja
 * es la que llego cinco posiciones antes, y entre las dos sale una:
 *
 *   - la primera copia, si es un codigo valido (es la que llego antes, asi
 *     que es la que menos ha tenido que esperar a nada);
 *   - si no, la segunda;
 *   - si ninguna vale, un interrogante, que es lo honesto: hubo un caracter
 *     ahi y no sabemos cual.
 */
static void par_emite(uint32_t n)
{
    uint8_t dx = s_hist[(n - DIV_SALTO) & (HIST_N - 1U)];
    uint8_t rx = s_hist[n & (HIST_N - 1U)];

    if (navtex_codigo_valido(dx)) {
        s_info.car_ok++;
        emite(dx);
    } else if (navtex_codigo_valido(rx)) {
        s_info.car_ok++;
        s_info.rescatados++;
        emite(rx);
    } else {
        s_info.car_mal++;
        ring_push('?');
    }
}

static void caracter_nuevo(uint8_t cod)
{
    s_car_n++;
    s_hist[s_car_n & (HIST_N - 1U)] = cod;
    if (s_car_n <= DIV_SALTO) { return; }

    /*
     * QUE PARIDAD LLEVA LAS REPETICIONES. No se sabe de antemano, asi que se
     * mide: se cuenta, por separado para las posiciones pares y las impares,
     * cuantas veces el caracter de ahora coincide con el de hace cinco. En la
     * paridad buena coinciden casi siempre; en la otra se estan comparando
     * caracteres que no tienen nada que ver.
     *
     * Durante la señal de fase que precede al mensaje no coincide ninguna de
     * las dos -son relleno y peticion de repeticion alternandose-, asi que
     * esto no decide nada hasta que empieza el texto. Por eso no hay ventana
     * ni reinicio: se acumula y se decide en cuanto una gana claro.
     */
    if (!s_tiene_par) {
        uint8_t p = (uint8_t)(s_car_n & 1U);
        uint8_t dx = s_hist[(s_car_n - DIV_SALTO) & (HIST_N - 1U)];

        if (dx == cod && navtex_codigo_valido(dx)) { s_par_cuad[p]++; }

        {
            uint8_t a = (s_par_cuad[0] >= s_par_cuad[1]) ? 0U : 1U;
            uint16_t mejor = s_par_cuad[a];
            uint16_t peor  = s_par_cuad[a ^ 1U];

            /* Que gane CLARAMENTE: ocho parejas y el triple que la otra.
             * Equivocarse aqui no da basura -las dos copias llevan el mismo
             * texto- pero deja la segunda copia emparejada con un caracter
             * que no es el suyo, o sea que el rescate pasaria a inventar. */
            if (mejor >= 8U && mejor > (uint16_t)(peor * 3U + 2U)) {
                uint32_t n, desde;

                s_paridad = a;
                s_tiene_par = 1U;

                /* Rebobinar: sacar todo lo que el historial todavia alcanza.
                 * El limite es que la pareja de cinco atras siga dentro. */
                desde = (s_car_n > (uint32_t)(HIST_N - DIV_SALTO))
                      ? (s_car_n - (uint32_t)(HIST_N - DIV_SALTO) + DIV_SALTO + 1U)
                      : (DIV_SALTO + 1U);
                for (n = desde; n <= s_car_n; n++) {
                    if ((uint8_t)(n & 1U) == s_paridad) { par_emite(n); }
                }
            }
        }
        return;
    }

    if ((uint8_t)(s_car_n & 1U) != s_paridad) { return; }
    par_emite(s_car_n);
}

/* ==========================================================================
 * 3. SINCRONISMO DE CARACTER
 * ==========================================================================
 *
 * Un bit nuevo entra en el registro. Con los ultimos 7 bits se puede formar
 * un caracter para UN desplazamiento concreto: el que le toca a este bit.
 * Contando por desplazamiento cuantos salen validos, el bueno se despega
 * solo. No hay que buscar ninguna marca en la señal porque no la hay.
 */
/*
 * EL ORDEN DE LOS BITS DENTRO DEL CARACTER - 02/10/2026.
 *
 * Una vez arreglado el enganche, el decodificador cogia la señal y sacaba
 * codigos TODOS validos -calidad 0,99, cero malos- y aun asi el texto no se
 * leia. Eso solo lo explica una cosa: dar la vuelta a los siete bits de un
 * codigo de cuatro unos deja un codigo de cuatro unos, o sea que el filtro
 * de validez no nota la diferencia. Se estaban leyendo caracteres buenos
 * pero equivocados.
 *
 * Invirtiendo el orden salio el texto a la primera:
 *
 *     ZCZC  CQ DE OST QTC LIST 30/0?/01 13:34  OST QTC LIST IN FEC MODE:
 *
 * OST es Oostende Radio. Se lee entero.
 *
 * POR QUE EL BANCO NO LO COGIO, que es lo que de verdad hay que aprender de
 * esto. El banco fabrica la señal con navtex_car_a_cod(), o sea con la MISMA
 * tabla con la que luego la lee, y presumia de ello en su cabecera ("asi no
 * puede haber dos tablas que se separen"). Lo que no puede haber es dos
 * tablas que se separen ENTRE ELLAS; de separarse las dos a la vez del aire
 * no protege, y es exactamente lo que pasaba. Por eso desde hoy el banco
 * lleva ademas una grabacion de verdad, sim/muestras/sitorb_tfc.mp3, y exige
 * que de ahi salga "CQ DE OST QTC LIST". Eso no se puede falsear desde
 * dentro.
 */
static uint8_t vuelve7(uint8_t c)
{
    uint8_t r = 0U, i;
    for (i = 0U; i < 7U; i++) {
        if (c & (uint8_t)(1U << i)) { r |= (uint8_t)(1U << (6U - i)); }
    }
    return r;
}

static void bit_nuevo(uint8_t bit)
{
    uint8_t d;

    s_reg = (uint8_t)(((s_reg << 1) | bit) & 0x7FU);
    s_bits42 = (uint64_t)((s_bits42 << 1) | (uint64_t)bit);
    s_bits++;
    if (s_bits < 7UL) { return; }

    d = (uint8_t)(s_bits % 7UL);

#ifdef NAVTEX_DBG
    { extern void navtex_dbg_bit(uint8_t bit, uint8_t reg, uint8_t d);
      navtex_dbg_bit(bit, s_reg, d); }
#endif
    if (!s_tiene_desp) {
        if (navtex_codigo_valido(s_reg)) { s_punt[d]++; }
        /* La pareja de cinco atras, que son 35 bits atras EN ESTE MISMO
         * desplazamiento. Hace falta que los 42 bits esten llenos. */
        if (s_bits >= 42UL) {
            uint8_t ant = (uint8_t)((s_bits42 >> 35) & 0x7FU);
            if (ant == s_reg && navtex_codigo_valido(s_reg)) { s_pares[d]++; }
        }
        s_punt_n++;

        if (s_punt_n >= BUSCA_BITS) {
            uint8_t i, mejor = 0U, pmej = 0U;
            uint8_t segundo = 0U, pseg = 0U;
            for (i = 1U; i < 7U; i++) {
                if (s_punt[i] > s_punt[mejor]) { mejor = i; }
            }
            for (i = 0U; i < 7U; i++) {
                if (i != mejor && s_punt[i] > segundo) { segundo = s_punt[i]; }
            }
            for (i = 1U; i < 7U; i++) {
                if (s_pares[i] > s_pares[pmej]) { pmej = i; }
            }
            for (i = 0U; i < 7U; i++) {
                if (i != pmej && s_pares[i] > pseg) { pseg = s_pares[i]; }
            }
            /* Cada desplazamiento ha visto BUSCA_BITS/7 = 32 caracteres. El
             * bueno tiene que sacar al menos 24 validos (75%) y doblar al
             * segundo; si no, es que todavia no hay señal que valga. */
#ifdef NAVTEX_DBG
            { extern void navtex_dbg_punt(const uint8_t *p, uint8_t mejor, uint8_t seg);
              extern void navtex_dbg_pares(const uint8_t *p);
              navtex_dbg_punt(s_punt, mejor, segundo); navtex_dbg_pares(s_pares); }
#endif
            /*
             * DOS CAMINOS PARA ENGANCHAR, y el que vale depende de donde se
             * haya caido dentro de la emision:
             *
             *   - por REPETICION, que es el bueno en cuanto hay texto. Le
             *     basta con ser el mayor ESTRICTO, y eso no es poco pedir:
             *     el desplazamiento de al lado hereda parte de las parejas
             *     -si dos caracteres son iguales, las ventanas corridas un
             *     bit tambien lo son mientras no se salgan- pero necesita
             *     que coincidan DOS caracteres seguidos donde el bueno solo
             *     necesita uno, asi que nunca puede pasarle. Medido en el
             *     aire: 19 contra 15 en la grabacion de 8424 kHz. Por eso
             *     aqui no vale un margen del triple; con el triple se
             *     rechazaba una señal que estaba perfectamente.
             *   - por CODIGOS VALIDOS, el de antes, que es el unico que
             *     funciona durante la señal de fase (ahi no hay ninguna
             *     pareja porque relleno y peticion se alternan). Se le exige
             *     ademas que el que gane por codigos sea el mismo que gana
             *     por parejas, o que no haya ninguna pareja en juego.
             */
            if (s_pares[pmej] >= PAR_MIN && s_pares[pmej] > pseg
                && s_punt[pmej] >= (BUSCA_BITS / 7U) * 3U / 4U) {
                s_desp = pmej;
                s_tiene_desp = 1U;
            } else if (s_punt[mejor] >= (BUSCA_BITS / 7U) * 3U / 4U
                       && (uint16_t)s_punt[mejor] > (uint16_t)segundo * 2U
                       && (s_pares[pmej] < PAR_MIN || pmej == mejor)) {
                s_desp = mejor;
                s_tiene_desp = 1U;
            }
            if (s_tiene_desp) {
                s_car_n = 0UL;
                s_par_cuad[0] = s_par_cuad[1] = 0U;
                s_tiene_par = 0U;
                s_vig_ok = s_vig_n = 0U;
            }
            memset(s_punt, 0, sizeof s_punt);
            memset(s_pares, 0, sizeof s_pares);
            s_punt_n = 0U;
            if (!s_tiene_desp && s_vueltas < 250U) { s_vueltas++; }
        }
        return;
    }

    /*
     * Y ENGANCHADO, SE SIGUE MIRANDO SI OTRO DESPLAZAMIENTO VA MEJOR -
     * 22/11/2026, port del Malahit.
     *
     * La vigilancia de abajo suelta el enganche cuando los codigos validos
     * caen a lo que da el azar. Se escribio para el reloj que resbala un
     * bit con un desvanecimiento, y ese caso NO lo ve: corrido un bit, un
     * texto da todavia el 45-70 % de codigos validos, por encima de su 37 %,
     * y el decodificador se queda en el sitio equivocado hasta el final del
     * mensaje, sacando basura. Medido en el banco (seccion 9 de
     * sim/navtextest.c: un bit que falta en el caracter 60 de 151 y lo demas
     * ilegible) y en la radio contra el HackRF (dos emisiones de diez
     * enganchadas con calidad 57-65 % y el mensaje entero en basura).
     *
     * Tampoco lo ven las repeticiones comparadas entre si, que fue lo
     * primero que probe: dos copias corridas el mismo bit comparten seis de
     * sus siete bits, y si las dos salen validas es que el septimo tambien
     * coincide - dos palabras de siete bits que se diferencian en uno no
     * pueden tener las dos cuatro unos. Corridas, cuando valen las dos,
     * coinciden SIEMPRE.
     *
     * Lo que si lo ve es la misma cuenta de la busqueda, la de los siete
     * desplazamientos a la vez: tras el resbalon, el de al lado pasa a
     * sacar las parejas y los codigos validos que sacaba el bueno. Asi que
     * enganchado se sigue contando, y al final de cada ventana (224 bits,
     * 2,2 s), si otro desplazamiento saca a la vez PAR_MIN parejas, cuatro
     * parejas mas que el puesto y cuatro codigos validos mas, se pasa a el.
     * Las dos cosas a la vez, para que el ruido de una recepcion mala no
     * mueva un enganche bueno; y sin parejas -la señal de fase, el ruido
     * entre emisiones- no se mueve nada. La paridad se vuelve a medir desde
     * cero, y el rebobinado la recupera: no se pierde lo que llego entre
     * medias.
     */
    if (navtex_codigo_valido(s_reg)) { s_punt[d]++; }
    if (s_bits >= 42UL) {
        uint8_t ant = (uint8_t)((s_bits42 >> 35) & 0x7FU);
        if (ant == s_reg && navtex_codigo_valido(s_reg)) { s_pares[d]++; }
    }
    if (++s_punt_n >= BUSCA_BITS) {
        uint8_t i, pmej = s_desp;

        for (i = 0U; i < 7U; i++) {
            if (s_pares[i] > s_pares[pmej]) { pmej = i; }
        }
        if (pmej != s_desp && s_pares[pmej] >= PAR_MIN
            && s_pares[pmej] >= (uint8_t)(s_pares[s_desp] + RECOLOCA_MARGEN)
            && s_punt[pmej] >= (uint8_t)(s_punt[s_desp] + RECOLOCA_MARGEN)) {
            s_desp = pmej;
            s_car_n = 0UL;
            s_par_cuad[0] = s_par_cuad[1] = 0U;
            s_tiene_par = 0U;
            s_vig_ok = s_vig_n = 0U;
            s_recolocados++;
        }
        memset(s_punt, 0, sizeof s_punt);
        memset(s_pares, 0, sizeof s_pares);
        s_punt_n = 0U;
    }

    if (d == s_desp) {
        uint8_t val = navtex_codigo_valido(s_reg);

        /* Calidad: la fraccion de codigos validos tal y como llegan, ANTES
         * de que la diversidad rescate nada. Es la medida de como esta la
         * recepcion, no de como de bien disimula el decodificador. */
        s_info.calidad = s_info.calidad * 0.98f + (val ? 0.02f : 0.0f);

        /*
         * VIGILANCIA DEL ENGANCHE. Una vez enganchado hay que poder
         * DESENGANCHARSE, y hasta que esto existio no se podia: si el reloj
         * de bit resbalaba un bit -un desvanecimiento basta- todo lo que
         * venia detras caia en el desplazamiento equivocado y el
         * decodificador se quedaba ahi para siempre, seguro de si mismo,
         * sacando interrogantes.
         *
         * El criterio no es "va mal", es "va como el azar": una cadena de
         * bits cualquiera da codigos validos el 27% de las veces, asi que
         * por debajo del 37% en 64 caracteres lo que hay ahi no es un
         * mensaje. Se vuelve a buscar desde cero.
         */
        s_vig_n++;
        if (val) { s_vig_ok++; }
        if (s_vig_n >= 64U) {
            if (s_vig_ok < 24U) {
                s_tiene_desp = 0U;
                s_tiene_par = 0U;
                s_par_cuad[0] = s_par_cuad[1] = 0U;
                memset(s_punt, 0, sizeof s_punt);
                memset(s_pares, 0, sizeof s_pares);
                s_punt_n = 0U;
            }
            s_vig_ok = s_vig_n = 0U;
        }

        caracter_nuevo(vuelve7(s_reg));
    }
}

/* ==========================================================================
 * 2. EL RELOJ DE BIT
 * ========================================================================== */
static void discriminador(float d)
{
    uint32_t fase_ant = s_fase;

    s_fase += s_inc;

    /*
     * DONDE SE MIRA, Y POR QUE AHI Y NO EN EL "CENTRO".
     *
     * Esto costo una tarde. La media movil es CAUSAL: el valor de ahora
     * resume el ultimo bit de señal, no el bit que esta centrado en ahora.
     * O sea que lleva medio bit de retraso incorporado.
     *
     * La primera version muestreaba a mitad de la vuelta del acumulador,
     * pensando que eso era el centro del bit. Lo que caia ahi de verdad era
     * la mezcla de la segunda mitad de un bit con la primera del siguiente:
     * el PEOR punto posible. Se veia como un flujo de bits que se parecia al
     * bueno pero perdia uno cada dieciseis.
     *
     * Con el retraso de la media movil metido en la cuenta, los dos puntos
     * se intercambian:
     *
     *   - en la VUELTA del acumulador, la ventana cubre exactamente un bit:
     *     ahi se decide;
     *   - a MITAD de vuelta, la ventana esta a caballo de dos bits: ahi es
     *     donde hay que mirar para saber si el reloj va adelantado o
     *     atrasado, que es justo lo que pide Gardner.
     */
    if (fase_ant < 0x80000000UL && s_fase >= 0x80000000UL) {
        s_d_medio = d;
    }

    if (s_fase < fase_ant) {
        float e = s_d_medio * (d - s_d_ant);
        float den = s_nivel * s_nivel;

        /*
         * Detector de Gardner. Si el reloj va atrasado, la muestra de en
         * medio ya lleva el bit nuevo y el producto sale positivo; si va
         * adelantado, todavia lleva el viejo y sale negativo. Y si no hay
         * transicion, el segundo factor vale cero y el detector no dice
         * nada, que es lo correcto: no hay nada que medir.
         */
        if (den > 1e-12f) {
            e /= den;
            if (e > 1.0f)  { e = 1.0f; }
            if (e < -1.0f) { e = -1.0f; }

            s_err_int += SIM_KI * e;
            if (s_err_int > SIM_KI_TOPE)  { s_err_int = SIM_KI_TOPE; }
            if (s_err_int < -SIM_KI_TOPE) { s_err_int = -SIM_KI_TOPE; }

            /*
             * La fase, de golpe, pero sin dejar que se pase de cero hacia
             * atras. La correccion se aplica justo despues de la vuelta del
             * acumulador, o sea cuando vale casi nada, y una correccion
             * negativa un poco grande lo meteria por debajo de cero - que en
             * un entero sin signo es casi 2^32, o sea un bit entero perdido y
             * la alineacion de 7 bits al suelo. Es literalmente el mismo
             * fallo que costo encontrar en el RDS; ver rds.c.
             */
            {
                int32_t corr = (int32_t)(SIM_KP * e * 4294967296.0f * 0.01f);
                if (corr < 0 && (uint32_t)(-corr) > s_fase) {
                    corr = -(int32_t)s_fase;
                }
                s_fase = (uint32_t)((int32_t)s_fase + corr);
            }

            /* Y la frecuencia SIEMPRE calculada del nominal, nunca acumulada
             * encima de si misma: asi escrito al reves seria un doble
             * integrador. Otra vez la misma leccion del RDS. */
            s_inc = (uint32_t)((int32_t)s_inc_nom
                               + (int32_t)((float)s_inc_nom * s_err_int));
        }

        s_d_ant = d;
        bit_nuevo((uint8_t)(d > 0.0f));
    }
}

/* ==========================================================================
 * 1. LA ENTRADA
 * ========================================================================== */
void navtex_process(const float *audio, uint32_t n)
{
    uint32_t k;

    if (!s_on || !audio) { return; }

    /* Un centro que el bucle principal ha medido en el espectro: se aplica
     * aqui, entre dos muestras, y no alli, a mitad de una. */
    if (s_pide) {
        centro_aplica(s_pide_hz, 1U);
        s_pide = 0U;
    }

    for (k = 0U; k < n; k++) {
        float x = audio[k];
        float sn, cs, mi, mq, ei, eq, pm, pe;

        /* El centro de los tonos, medido sobre el audio crudo: ver
         * cruces_mete(). Cuesta una comparacion por muestra. */
        cruces_mete(x);

        /* Bajar los dos tonos a banda base. */
        nco_paso(&s_nco_m, &sn, &cs);
        mi = x * cs;  mq = -x * sn;
        nco_paso(&s_nco_e, &sn, &cs);
        ei = x * cs;  eq = -x * sn;

        /* Media movil rectangular de un bit: se suma lo que entra y se resta
         * lo que sale. Mantener las sumas cuesta cuatro sumas y cuatro
         * restas por muestra, y da el discriminador en CADA muestra - que es
         * lo que necesita el lazo de reloj para no ir a ciegas. */
        s_smi += mi - s_mi[s_hidx];  s_mi[s_hidx] = mi;
        s_smq += mq - s_mq[s_hidx];  s_mq[s_hidx] = mq;
        s_sei += ei - s_ei[s_hidx];  s_ei[s_hidx] = ei;
        s_seq += eq - s_eq[s_hidx];  s_eq[s_hidx] = eq;
        s_hidx++;
        if (s_hidx >= s_spb) { s_hidx = 0U; }

        if (s_llenado < s_spb) { s_llenado++; continue; }

        pm = s_smi * s_smi + s_smq * s_smq;
        pe = s_sei * s_sei + s_seq * s_seq;

        /* Nivel lento, para que las ganancias de los lazos no dependan de lo
         * fuerte que entre la señal. */
        s_nivel += 0.0005f * ((pm + pe) - s_nivel);
        if (s_nivel < 1e-12f) { s_nivel = 1e-12f; }

        discriminador(pm - pe);
    }
}

/* ==========================================================================
 * LO QUE SE PUEDE MIRAR DESDE FUERA
 * ========================================================================== */
void navtex_info(navtex_info_t *out)
{
    if (!out) { return; }
    *out = s_info;
    out->bit_sync = (uint8_t)(s_on && s_tiene_desp);
    out->desvio_hz = (int16_t)((s_centro_med > 0.0f)
                               ? (s_centro_med - s_centro_hz) : 0.0f);
    out->car_sync = (uint8_t)(s_on && s_tiene_desp && s_tiene_par);
}

uint16_t navtex_recolocados(void) { return s_recolocados; }

void navtex_lazos_dbg(float *reloj_ppm, float *nivel, uint8_t *desp_bit)
{
    if (reloj_ppm) { *reloj_ppm = s_err_int * 1.0e6f; }
    if (nivel)     { *nivel = s_nivel; }
    if (desp_bit)  { *desp_bit = s_desp; }
}
