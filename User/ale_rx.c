#include "ale_rx.h"
#include "ale_golay.h"
#include <string.h>
#include <math.h>
#ifdef ALE_AFC_TRAZA
#include <stdio.h>
#endif

/* ==========================================================================
 * NUMEROS DEL ESTANDAR
 * ========================================================================== */
#define ALE_TONOS      8U
#define ALE_SPS        96U      /* muestras por simbolo a 12 kHz */
#define ALE_FASES      4U       /* fases de simbolo que se prueban */
#define ALE_PASO       (ALE_SPS / ALE_FASES)   /* 24 */
#define ALE_BITS_PAL   49U      /* bits de una palabra ya con FEC */
#define ALE_SIM_PAL    49U      /* simbolos que ocupa (147 bits / 3) */
#define ALE_ANILLO     (ALE_BITS_PAL * 3U)     /* 147 */

/*
 * TONO -> SIMBOLO, en codigo Gray. Ver la cabecera de ale_rx.h: el orden
 * NO es lineal, y ponerlo lineal da un decodificador que no acierta una.
 */
static const uint8_t k_gray[ALE_TONOS] = { 0U, 1U, 3U, 2U, 6U, 7U, 5U, 4U };

/* Los ocho tonos, en hercios. */
static const float k_hz[ALE_TONOS] = {
    750.0f, 1000.0f, 1250.0f, 1500.0f, 1750.0f, 2000.0f, 2250.0f, 2500.0f
};

/*
 * UMBRALES DE ENGANCHE, los mismos que usa el decodificador de referencia
 * (LinuxALE, modem.h) porque estan probados en el aire:
 *
 *   MAL_VOTOS  de los 48 bits votados, cuantos pueden salir NO unanimes.
 *              25 de 48 es mas de la mitad, y aun asi vale: con 2 de 3, un
 *              bit no unanime sigue decidiendose bien mientras solo una de
 *              las tres copias este mal.
 *   ERR_SINC   al BUSCAR el principio de palabra se exige que las dos
 *              mitades Golay salgan casi limpias (<= 1 error corregido).
 *              Es mas duro que los 3 que el codigo aguanta, y a proposito:
 *              enganchar en una fase equivocada que "casi" cuadra cuesta
 *              una palabra entera de recuperarse.
 *   ERR_SIGUE  ya enganchado se admite todo lo que el Golay pueda arreglar.
 */
#define MAL_VOTOS  25U
#define ERR_SINC    1U
#define ERR_SIGUE   3U

/*
 * EL GOLAY NO BASTA PARA ENCONTRAR EL PRINCIPIO DE PALABRA, Y ESTO ES LO
 * MENOS OBVIO DE TODO EL MODO. 27/09/2026.
 *
 * La idea natural es: pruebo las 49 fases y me quedo con la que decodifica
 * limpio por Golay. No funciona, y no funciona por una propiedad del
 * codigo: el Golay (24,12) de ALE es un ciclico [23,12] extendido, y en un
 * codigo CICLICO la rotacion de una palabra valida es OTRA palabra valida.
 *
 * Y el entrelazado convierte una rotacion de DOS bits del flujo en una
 * rotacion de UN bit de cada una de las dos mitades. O sea que la fase
 * correcta y la fase correcta mas dos bits decodifican las DOS limpiamente,
 * sin un solo error corregido, y no hay forma de distinguirlas mirando el
 * FEC.
 *
 * Lo enseno el banco a la primera: con la palabra descolocada 24 muestras
 * salian cuatro palabras perfectas con 48 votos unanimes... y valian
 * 0x58B05A en vez de 0xB160B4, que es exactamente lo mismo desplazado un
 * bit. Decodificador impecable, resultado inservible, y ni un aviso.
 *
 * LO QUE SI LO DISTINGUE es el contenido, y el estandar lo dice: los
 * criterios de aceptacion de palabra (FED-STD-1045A 5.2.4.3.3) no son dos
 * sino siete, y entre ellos estan "preambulo admisible" y "bits de
 * caracter admisibles". Una palabra desplazada un bit da basura en los tres
 * caracteres -en el caso de arriba, 'b', '`' y 'Z'- y las minusculas y los
 * signos raros NO existen en el alfabeto de ALE.
 *
 * Asi que para ENGANCHAR en frio se exige ademas:
 *   - preambulo de los que abren una direccion (TO, TWAS, FROM, TIS). Los
 *     de extension (DATA, REP) engancharian la rejilla desplazada una
 *     palabra entera, y CMD no tiene alfabeto con el que filtrar.
 *   - los tres caracteres, del alfabeto basico de 38 (A-Z, 0-9, '@', '?').
 *
 * Ya enganchado se afloja: cualquier preambulo y cualquier caracter que
 * exista en ALE, porque entonces la rejilla la sujeta el reloj y no el
 * contenido.
 */

/*
 * El alfabeto: 0 = no existe en ALE, 1 = solo en el juego ampliado de 64,
 * 2 = tambien en el basico de 38 (A-Z, 0-9, '@', '?').
 */
static const uint8_t k_alfa[128] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,   /* 0x00 */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,   /* 0x10 */
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,   /* 0x20  espacio y signos */
    2,2,2,2,2,2,2,2, 2,2,1,1,1,1,1,2,   /* 0x30  0-9 ... '?' */
    2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,   /* 0x40  '@' y A-O */
    2,2,2,2,2,2,2,2, 2,2,2,1,1,1,1,1,   /* 0x50  P-Z y [\]^_ */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,   /* 0x60  minusculas: NO */
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0    /* 0x70 */
};

/*
 * Preambulos que pueden ABRIR una direccion. El resto (DATA=0, THRU=1,
 * CMD=6, REP=7) solo valen en medio de una secuencia.
 */
static uint8_t abre_direccion(uint8_t pre)
{
    return (uint8_t)((pre == 2U) || (pre == 3U) ||
                     (pre == 4U) || (pre == 5U));
}

static uint8_t texto_vale(uint32_t pal, uint8_t minimo)
{
    uint8_t i;

    for (i = 0U; i < 3U; i++) {
        uint8_t c = (uint8_t)((pal >> (14U - (7U * i))) & 0x7FU);

        if (k_alfa[c] < minimo) { return 0U; }
    }
    return 1U;
}

/* ==========================================================================
 * ESTADO
 * ========================================================================== */
static uint8_t  s_on;

/* Goertzel: coeficientes y la ventana deslizante. */
static float    s_coef[ALE_TONOS];
static float    s_hist[ALE_SPS];
static uint16_t s_hi;           /* donde escribe la ventana */
static uint16_t s_lleno;

/* Fase de simbolo: cuatro candidatas, se elige la de mas energia. */
static float    s_fase_e[ALE_FASES];
static uint8_t  s_fase;             /* la elegida */
static uint8_t  s_sim_f[ALE_FASES]; /* el simbolo que ve cada fase */
static uint16_t s_cuenta;           /* muestras dentro del simbolo, 0..95 */

/* Anillo de bits: 147, uno por byte para que el acceso sea directo. */
static uint8_t  s_bits[ALE_ANILLO];
static uint16_t s_pos;          /* donde entra el siguiente bit */

/*
 * REVISION A FONDO DEL 08/10/2026: LO QUE CRUZA CON LA INTERRUPCION DE
 * AUDIO, VOLATILE.
 *
 * ale_rx_mete() corre en la interrupcion y es quien escribe la palabra
 * recien sacada (el anillo de palabras, ver ALE_PAL_N), el estado del
 * enganche y los contadores se escriben en la interrupcion y se leen desde
 * ale_rx_saca() y desde ale_rx_info(). Sin volatile el compilador puede
 * cachear los indices del anillo y la palabra no sale nunca.
 *
 * Ojo: volatile NO hace atomica una secuencia de dos accesos, y por eso
 * esto dejo de ser una casilla unica con bandera el 09/10/2026 -ver el
 * comentario de ALE_PAL_N-. volatile y anillo son dos cosas distintas y
 * hacen falta las dos.
 */
/* Sincronismo de palabra. */
static volatile uint8_t  s_eng; /* 1 = enganchado */
static uint16_t s_pos_pal;      /* la posicion del anillo donde empieza */
static uint16_t s_hasta;        /* simbolos que faltan para la siguiente */

/* Salida: una palabra pendiente basta - salen a 2,5 por segundo. */
/*
 * UN ANILLO DE CUATRO, Y NO UNA CASILLA - 09/10/2026.
 *
 * Esto era un buzon de una sola casilla con bandera, y la bandera se bajaba
 * DESPUES de leer los dos campos. Si la interrupcion entregaba una palabra
 * entre la lectura del dato y la de la calidad, pasaban las dos cosas malas
 * a la vez: salia la palabra W1 con la CALIDAD DE W2 al lado -un numero que
 * no es de esa palabra-, y W2 se perdia entera porque el bucle principal
 * bajaba la bandera justo despues. Y s_info.palabras ya la habia contado,
 * asi que el contador de la pantalla se quedaba para siempre diciendo una
 * palabra mas de las que hay en la lista.
 *
 * El volatile de la cabecera de este fichero no cierra esa ventana: no es
 * un problema de cacheo, es que leer dos campos y bajar una bandera no es
 * una sola operacion.
 *
 * Con un anillo -el dato se guarda y DESPUES se avanza la cabeza; se lee de
 * la cola y DESPUES se avanza- no hay ventana: es el mismo patron que ya
 * usan ais_rx.c, rtty.c, psk31.c, navtex.c y seis mas. Cuatro casillas
 * sobran: la interrupcion entrega como mucho una palabra cada 8 ms y el
 * bucle principal pasa cada 16.
 */
#define ALE_PAL_N 4U
static volatile uint32_t s_pal_ring[ALE_PAL_N];
static volatile uint8_t  s_pal_cal_ring[ALE_PAL_N];
static volatile uint8_t  s_pal_cab;   /* escribe la interrupcion */
static volatile uint8_t  s_pal_col;   /* lee el bucle principal */

static volatile ale_rx_info_t s_info;
static float    s_nivel;

/* ==========================================================================
 * EL CAF - 23/11/2026, port del Malahit
 * ==========================================================================
 *
 * SOLO CON ALE_CAF=1, y por defecto NO: el DEEPSDR tiene la flash llena a
 * unos cientos de bytes y esto son 1,5 kB, asi que alli se compila como
 * estaba -los ocho bins fijos- y el port del Malahit lo enciende en su
 * reglas.mk; los bancos de deepsdr-ui tambien.
 *
 * POR QUE. Los ocho Goertzel miran bins FIJOS: la ventana de 96 muestras
 * tiene 125 Hz de lobulo y los tonos estan a 250, asi que un tono que llega
 * corrido sigue cayendo en su bin hasta +-80 Hz -medido en el banco, y en
 * la radio contra el HackRF- y a +-90 deja de salir nada. Las ppm del
 * cristal de una radio son eso a 14 MHz; a 28 el doble. Las estaciones de
 * ALE estan en canales exactos, pero quien escucha no.
 *
 * QUE HACE: mover las frecuencias de los ocho Goertzel, no la radio. El
 * Goertzel se calcula entero sobre la ventana cada vez (mide()), asi que
 * cambiar sus coeficientes entre dos medidas no deja nada a medias.
 *
 * DOS PIEZAS:
 *
 *   EL DISCRIMINADOR. En cada simbolo, en la fase elegida -la ventana
 *   cuadrada con el simbolo-, dos Goertzel mas a +-62,5 Hz del tono que ha
 *   ganado. Para un tono puro en una ventana rectangular el cociente de sus
 *   modulos da el error EXACTO mientras quede a menos de medio bin:
 *
 *       e = 62,5 * (|X+| - |X-|) / (|X+| + |X-|)   Hz
 *
 *   (los dos son sinc(a -+ 1/2), y sin(pi(a-1/2)) = -cos(pi a) =
 *   -sin(pi(a+1/2)): el cociente se queda en |a+1/2| / |a-1/2|.) Mas alla
 *   de medio bin sigue dando el signo. Cada tono lleva la media de su
 *   error, y cada simbolo mueve el CAF un 8 % de la MEDIANA de los ocho:
 *   una portadora pegada a un tono no se lo lleva.
 *
 *   LOS BORDES. El discriminador lleva el tono al bin MAS CERCANO, y con un
 *   error de mas de 125 Hz el mas cercano es el del vecino: el CAF se queda
 *   quieto y feliz 250 Hz al lado, con siete de los ocho tonos en bins de
 *   la rejilla -y ni una palabra, porque cada simbolo sale corrido un
 *   puesto. Eso se ve en el borde: el tono que sobra cae 250 Hz por debajo
 *   del primero (o por encima del ultimo), donde un CAF en su sitio no ve
 *   mas que ruido. Dos Goertzel mas en esos dos sitios, promediados, y
 *   cuando uno de ellos tiene claramente mas que el otro y que los bins sin
 *   tono, el CAF salta 250 Hz hacia el. Con eso agarra +-400 Hz.
 *
 *   LA BUSQUEDA. Con el tono a 75-125 Hz de su bin ninguna de las dos de
 *   arriba arranca: el discriminador lleva un tono al bin y no sabe a cual
 *   si esta a medio camino, y ahi los simbolos dejan de ser claros - un
 *   trozo de tono de media ventana tiene el lobulo el doble de ancho y
 *   llega mas a ese bin que el tono entero, asi que la eleccion de fase de
 *   simbolo se va a ventanas a caballo entre dos simbolos, que reparten la
 *   energia. Medido: a +-100 y +-150 Hz, ni una palabra y el CAF en cero.
 *   Asi que cada simbolo mide, ademas de la rejilla de ahora, UNA de cuatro
 *   rejillas mas -a +-62,5 y +-125 Hz de ella, por turno: ningun error
 *   queda a mas de 31 Hz de alguna-, y cuando una de ellas tiene senal y
 *   bastante mas energia que la de ahora durante 16 simbolos seguidos, el
 *   CAF se va a ella. El discriminador y los bordes terminan el trabajo.
 *
 *   CADA REJILLA EN SU MEJOR FASE. Medirlas todas en la fase que ha elegido
 *   la de ahora no sirve: esa fase es justo la ventana a caballo, que
 *   reparte cada tono en dos bins de cualquier rejilla, y las cuatro salian
 *   con la misma energia (trazado: 1,4-1,8 mil las cuatro, con el tono a
 *   100 Hz). Asi que la rejilla que toca se mide en las CUATRO fases del
 *   simbolo, como la de ahora, y de cada una se queda, simbolo a simbolo,
 *   la fase en la que su tono ganador sale mas fuerte: la buena, cuadrada
 *   con el simbolo, recoge el tono entero en un bin. Cuesta 32 Goertzel
 *   por simbolo, lo mismo que el decodificador.
 *
 * QUE ES "HAY SENAL". La energia de cada uno de los ocho tonos, en cada
 * una de las cinco rejillas, promediada unos 100 simbolos (0,8 s); el suelo
 * es la media por tono de la rejilla mas pobre. Una rejilla tiene senal
 * cuando SEIS DE SUS OCHO TONOS estan un 30 % por encima del suelo Y a no
 * menos de una quinta parte del mas fuerte: ALE usa los ocho por igual, y
 * una portadora, un RTTY o un PSK encienden uno o dos. Lo segundo hace
 * falta: una portadora fuerte enciende todos los bins con sus lobulos
 * laterales -una a SNR 30 en 1690 Hz se llevaba el CAF 47 Hz-, pero el
 * suyo queda veinte o cincuenta veces por encima de los demas. Se
 * mira la energia y no si cada simbolo es claro, porque la energia no
 * necesita la ventana cuadrada con el simbolo; y porque "claro" -el tono
 * ganador seis veces por encima de la media de los otros siete- lo es el
 * diez por ciento de los simbolos de puro ruido, y seis de cada ocho
 * claros salian en el ruido cada pocos minutos. Seis tonos encendidos de
 * ocho promediados, nunca: el CAF se queda donde lo dejo la ultima
 * estacion, que es lo que se quiere - lo que mide es sobre todo el error de
 * la propia radio en esta frecuencia, y vale para la siguiente estacion
 * del mismo canal.
 */
#ifndef ALE_CAF
#define ALE_CAF 0
#endif
#if ALE_CAF
#define AFC_DELTA     62.5f     /* medio bin: el discriminador */
#define AFC_GANANCIA  0.08f
#define AFC_MAX       400.0f
#define AFC_CLARO     6.0f      /* el ganador contra la media de los otros */
#define AFC_ALIAS     2.5f      /* un borde contra el otro y contra el ruido */
#define AFC_MIN_N     48U       /* simbolos con senal antes de juzgar los bordes */
#define AFC_HIPOTESIS 4U
#define AFC_A0        (1.0f / 48.0f)  /* la rejilla de ahora: cada simbolo */
#define AFC_A1        (1.0f / 12.0f)  /* cada otra: uno de cada cuatro, el mismo tiempo */
#ifndef AFC_SOBRE
#define AFC_SOBRE     1.3f      /* un tono encendido, contra el suelo */
#endif
#define AFC_ENCENDIDOS 6U       /* tonos encendidos de ocho: hay senal */
#define AFC_PAREJO    0.2f      /* y no a menos de esto del mas fuerte */
#ifndef AFC_MEJOR
#define AFC_MEJOR     1.25f     /* otra rejilla, contra la de ahora */
#endif
#define AFC_MIN_EVAL  96U       /* simbolos desde la ultima puesta a cero */
#ifndef AFC_SEGUIDOS
#define AFC_SEGUIDOS  16U       /* simbolos seguidos ganando la misma, para irse */
#endif

/* +125 y -125 son la misma rejilla salvo en los bordes - y los bordes son
 * justo lo que decide cual de las dos tiene los ocho tonos. */
static const float k_hip[AFC_HIPOTESIS] = { 62.5f, 125.0f, -62.5f, -125.0f };

static float    s_fs = 12000.0f;
static float    s_afc;              /* Hz que se han movido los tonos */
static float    s_afc_puesto;       /* el que tienen los coeficientes */
static float    s_coef_mas[ALE_TONOS];
static float    s_coef_menos[ALE_TONOS];
static float    s_coef_borde[2];
static float    s_coef_hip[AFC_HIPOTESIS][ALE_TONOS];
static float    s_e[AFC_HIPOTESIS + 1U][ALE_TONOS];  /* 0 = la de ahora */
static uint16_t s_eval;             /* simbolos promediados desde el cero */
static uint8_t  s_hip;              /* la que toca medir */
static float    s_m0[ALE_TONOS];    /* este simbolo, la de ahora en su mejor fase */
static float    s_mh[ALE_TONOS];    /* y la que toca, en la suya */
static float    s_m0_max, s_mh_max;
static uint8_t  s_senal;            /* la de ahora tiene senal: el CAF se mueve */
static uint8_t  s_gana, s_gana_n;   /* la que gana, y cuantos simbolos seguidos */
static float    s_err[ALE_TONOS];   /* error medio de cada tono contra la rejilla, Hz */
static uint8_t  s_err_hay;          /* bit t: el tono t ya tiene error medido */
static float    s_b_bajo, s_b_alto, s_b_ruido;
static uint16_t s_afc_n;
static volatile uint8_t s_afc_cero_pedido;  /* lo pide el bucle, lo hace mide() */
static void afc_a_cero(void);
#endif /* ALE_CAF */

/* ==========================================================================
 * ARRANQUE
 * ========================================================================== */
void ale_rx_start(float fs_hz)
{
    uint8_t t;
    float fs = (fs_hz > 1.0f) ? fs_hz : 12000.0f;

    memset(s_hist, 0, sizeof s_hist);
    memset(s_bits, 0, sizeof s_bits);
    memset(s_fase_e, 0, sizeof s_fase_e);
    memset(s_sim_f, 0, sizeof s_sim_f);
    /* El cast quita el volatile para memset: ver el comentario de la
     * declaracion. Aqui no hay interrupcion de por medio -esto corre al
     * arrancar el modo, antes de que entre audio-. */
    memset((void *)&s_info, 0, sizeof s_info);
    s_hi = 0U; s_lleno = 0U; s_cuenta = 0U; s_fase = 0U;
    s_pos = 0U; s_eng = 0U; s_pos_pal = 0U; s_hasta = 0U;
    s_pal_cab = 0U; s_pal_col = 0U; s_nivel = 0.0f;

    /*
     * El coeficiente de Goertzel de cada tono: 2*cos(2*pi*f/fs). Los ocho
     * tonos caen en bins exactos de la ventana de 96 muestras, asi que no
     * hay fuga entre ellos y no hace falta ventana de suavizado.
     */
    for (t = 0U; t < ALE_TONOS; t++) {
        s_coef[t] = 2.0f * cosf(2.0f * 3.14159265358979f * k_hz[t] / fs);
    }
#if ALE_CAF
    s_fs = fs;
    afc_a_cero();
#endif
    s_on = 1U;
}

#if ALE_CAF
/* Los coeficientes de los ocho tonos, de sus dos discriminadores y de los
 * dos bordes, con el CAF de ahora. Con s_afc a cero, s_coef[] sale
 * identico al de ale_rx_start(): la misma expresion con el mismo numero. */
static float coef(float hz)
{
    return 2.0f * cosf(2.0f * 3.14159265358979f * hz / s_fs);
}

static void afc_coefs(void)
{
    uint8_t t, h;

    for (t = 0U; t < ALE_TONOS; t++) {
        float f = k_hz[t] + s_afc;

        s_coef[t]       = coef(f);
        s_coef_mas[t]   = coef(f + AFC_DELTA);
        s_coef_menos[t] = coef(f - AFC_DELTA);
    }
    s_coef_borde[0] = coef(k_hz[0] + s_afc - 250.0f);
    s_coef_borde[1] = coef(k_hz[ALE_TONOS - 1U] + s_afc + 250.0f);
    for (h = 0U; h < AFC_HIPOTESIS; h++) {
        for (t = 0U; t < ALE_TONOS; t++) {
            s_coef_hip[h][t] = coef(k_hz[t] + s_afc + k_hip[h]);
        }
    }
    s_afc_puesto = s_afc;
}

static void afc_bordes_cero(void)
{
    s_b_bajo = 0.0f; s_b_alto = 0.0f; s_b_ruido = 0.0f;
    s_afc_n = 0U;
}

static void afc_registros_cero(void)
{
    memset(s_e, 0, sizeof s_e);
    memset(s_err, 0, sizeof s_err);
    s_err_hay = 0U;
    s_eval = 0U;
    s_senal = 0U;
    s_gana = 0U;
    s_gana_n = 0U;
    s_m0_max = -1.0f;
    s_mh_max = -1.0f;
}

static void afc_a_cero(void)
{
    s_afc = 0.0f;
    afc_registros_cero();
    afc_bordes_cero();
    afc_coefs();
    s_info.afc_hz = 0;
    s_afc_cero_pedido = 0U;
}

/* Desde el bucle principal: los coeficientes los lee la interrupcion de
 * audio, asi que no se tocan desde aqui. Se pide, y la siguiente medida de
 * mide() lo hace antes de usarlos. Parado, ale_rx_start() lo hara igual. */
void ale_rx_afc_cero(void)
{
    s_afc_cero_pedido = 1U;
}
#else
void ale_rx_afc_cero(void) { }
#endif /* ALE_CAF */

void ale_rx_stop(void)      { s_on = 0U; }
uint8_t ale_rx_activo(void) { return s_on; }

void ale_rx_info(ale_rx_info_t *out)
{
    if (out == (ale_rx_info_t *)0) { return; }
    *out = s_info;
    out->enganchado = s_eng;
    out->nivel = (uint8_t)((s_nivel > 99.0f) ? 99.0f : s_nivel);
}

void ale_rx_cuentas_cero(void)
{
    s_info.palabras = 0U;
    s_info.enganches = 0U;
}

uint8_t ale_rx_saca(uint32_t *palabra, uint8_t *calidad)
{
    uint8_t col = s_pal_col;

    if ((palabra == (uint32_t *)0) || (col == s_pal_cab)) { return 0U; }
    /* Se lee de la cola... */
    *palabra = s_pal_ring[col];
    if (calidad != (uint8_t *)0) { *calidad = s_pal_cal_ring[col]; }
    /* ...y SOLO DESPUES se suelta la casilla. Mientras no se avance, la
     * interrupcion no puede pisarla. */
    s_pal_col = (uint8_t)((col + 1U) % ALE_PAL_N);
    return 1U;
}

/* ==========================================================================
 * LA PRUEBA DE UNA FASE DE PALABRA
 * ==========================================================================
 *
 * Toma 147 bits del anillo empezando en `p`, vota 2 de 3, desentrelaza,
 * decodifica las dos mitades Golay y dice si cuadra.
 */
static uint8_t prueba(uint16_t p, uint8_t err_max, uint8_t frio,
                      uint32_t *pal, uint8_t *cal)
{
    uint8_t  v[48];
    uint8_t  i, unanimes = 0U, ea = 0U, eb = 0U;
    uint32_t a = 0U, b = 0U;
    uint16_t da, db;

    /*
     * VOTACION 2 DE 3, y el bit 48 (el de relleno, S49) NO se vota: vale
     * cero siempre y meterlo falsearia la cuenta de unanimidad, que es la
     * medida de calidad que decide el enganche.
     */
    for (i = 0U; i < 48U; i++) {
        uint8_t n = (uint8_t)(s_bits[(p + i) % ALE_ANILLO] +
                              s_bits[(p + i + 49U) % ALE_ANILLO] +
                              s_bits[(p + i + 98U) % ALE_ANILLO]);

        v[i] = (uint8_t)(n >= 2U);
        if ((n == 0U) || (n == 3U)) { unanimes++; }
    }
    if ((48U - unanimes) > MAL_VOTOS) { return 0U; }

    /*
     * DESENTRELAZADO: los bits pares son de la mitad A y los impares de la
     * B, y dentro de cada una van de mas a menos peso. Es un intercalado
     * alterno, nada mas - pero al reves no decodifica nada.
     */
    for (i = 0U; i < 24U; i++) {
        a |= ((uint32_t)v[2U * i]      << (23U - i));
        b |= ((uint32_t)v[2U * i + 1U] << (23U - i));
    }

    /*
     * Y LOS DOCE BITS DE PARIDAD DE LA MITAD B VIAJAN INVERTIDOS. Lo dice
     * el estandar (5.2.3.3) y sirve para que el receptor distinga una
     * mitad de la otra. Sin deshacerlo, la mitad B nunca decodifica y la A
     * si: el sintoma es "decodifica la mitad de cada palabra", que no se
     * parece en nada a su causa.
     */
    b ^= 0x00000FFFUL;

    da = ale_golay_decodifica(a, &ea);
    db = ale_golay_decodifica(b, &eb);

    if ((ea == ALE_GOLAY_NO) || (eb == ALE_GOLAY_NO)) { return 0U; }
    if ((ea > err_max) || (eb > err_max)) { return 0U; }

    {
        uint32_t w = ((uint32_t)da << 12) | (uint32_t)db;

        /*
         * Y AQUI ES DONDE SE TIRA LA FASE DESPLAZADA. Ver el comentario
         * largo de arriba: el Golay solo no distingue la fase buena de la
         * buena mas dos bits.
         */
        if (frio) {
            if (!abre_direccion((uint8_t)((w >> 21) & 7U))) { return 0U; }
            if (!texto_vale(w, 2U)) { return 0U; }
        } else {
            if (!texto_vale(w, 1U)) { return 0U; }
        }

        *pal = w;
    }
    *cal = unanimes;
    return 1U;
}

static void entrega(uint32_t pal, uint8_t cal)
{
    uint8_t sig = (uint8_t)((s_pal_cab + 1U) % ALE_PAL_N);

    if (sig == s_pal_col) {
        /* Lleno: se tira la nueva y NO se cuenta, para que el contador de
         * la pantalla siga cuadrando con lo que se ve en la lista. Con
         * cuatro casillas y una palabra cada 8 ms esto no deberia pasar
         * nunca; si pasa, es que el bucle principal esta muy atascado y el
         * problema es ese, no esta palabra. */
        return;
    }
    /* Primero el dato... */
    s_pal_ring[s_pal_cab]     = pal;
    s_pal_cal_ring[s_pal_cab] = cal;
    /* ...y SOLO DESPUES se publica. */
    s_pal_cab = sig;
    s_info.palabras++;
    s_info.calidad = cal;
}

/* ==========================================================================
 * UN SIMBOLO NUEVO
 * ========================================================================== */
static void simbolo(uint8_t sim)
{
    uint8_t i;

    s_info.simbolos++;

    /* Tres bits, de mas a menos peso. */
    for (i = 0U; i < 3U; i++) {
        s_bits[s_pos] = (uint8_t)((sim >> (2U - i)) & 1U);
        s_pos++;
        if (s_pos >= ALE_ANILLO) { s_pos = 0U; }
    }

    if (s_eng) {
        /*
         * Ya enganchado: no se busca nada. Se entrega una palabra cada 49
         * simbolos exactos y, si deja de cuadrar, se vuelve a buscar.
         */
        if (s_hasta > 0U) { s_hasta--; return; }
        s_hasta = (uint16_t)(ALE_SIM_PAL - 1U);
        {
            uint32_t pal = 0U;
            uint8_t  cal = 0U;

            if (prueba(s_pos_pal, ERR_SIGUE, 0U, &pal, &cal)) {
                entrega(pal, cal);
            }
            else { s_eng = 0U; }
        }
        return;
    }

    /*
     * BUSCANDO. Se prueba UNA fase por simbolo, la que toca ahora, y como
     * el anillo avanza tres bits por simbolo, en 49 simbolos se han
     * barrido las 49 posiciones posibles. Probar las 49 en cada simbolo
     * seria 49 veces mas caro para llegar al mismo sitio medio segundo
     * antes.
     *
     * Y son 49 y no 147: el flujo empieza en frontera de simbolo, asi que
     * el principio de palabra siempre cae en un multiplo de tres bits.
     */
    {
        uint32_t pal = 0U;
        uint8_t  cal = 0U;

        if (prueba(s_pos, ERR_SINC, 1U, &pal, &cal)) {
            s_eng = 1U;
            s_pos_pal = s_pos;
            s_hasta = (uint16_t)(ALE_SIM_PAL - 1U);
            s_info.enganches++;
            entrega(pal, cal);
        }
    }
}

/* ==========================================================================
 * LA ENTRADA
 * ========================================================================== */
#if ALE_CAF
/* Un Goertzel sobre la ventana, en dos tramos como los de mide(). */
static float goertzel(float c)
{
    float s0, s1 = 0.0f, s2 = 0.0f;
    uint16_t k;

    for (k = s_hi; k < ALE_SPS; k++) { s0 = s_hist[k] + c * s1 - s2; s2 = s1; s1 = s0; }
    for (k = 0U; k < s_hi; k++)      { s0 = s_hist[k] + c * s1 - s2; s2 = s1; s1 = s0; }
    return (s1 * s1) + (s2 * s2) - (c * s1 * s2);
}

/*
 * Un paso del CAF, con las ocho potencias de la fase elegida y el tono que
 * ha ganado. Ver la cabecera del CAF, arriba.
 */
/* Las potencias de una rejilla: 1 si el simbolo es claro. `otros_out`
 * (puede ser NULL) recibe la media de los siete que no ganan. */
static uint8_t claro(const float *pt, uint8_t mejor, float *otros_out)
{
    float   otros = 0.0f;
    uint8_t t;

    for (t = 0U; t < ALE_TONOS; t++) { if (t != mejor) { otros += pt[t]; } }
    otros *= (1.0f / 7.0f);
    if (otros_out != (float *)0) { *otros_out = otros; }
    return (uint8_t)((pt[mejor] >= AFC_CLARO * otros) ? 1U : 0U);
}

/* El CAF en el sitio, con lo que haya movido: los coeficientes si hace
 * falta, y el numero para ale_rx_info(). */
static void afc_aplica(void)
{
    if (s_afc >  AFC_MAX) { s_afc =  AFC_MAX; }
    if (s_afc < -AFC_MAX) { s_afc = -AFC_MAX; }
    if (fabsf(s_afc - s_afc_puesto) >= 1.0f) { afc_coefs(); }
    s_info.afc_hz = (int16_t)((s_afc >= 0.0f) ? (s_afc + 0.5f) : (s_afc - 0.5f));
}

/*
 * En CADA medida (las cuatro fases): la rejilla que toca, y la mejor fase
 * de este simbolo de ella y de la de ahora. Al cerrar el periodo, los
 * promedios, si hay senal, y si otra rejilla la tiene mejor.
 */
static void afc_fase(const float *pt, float pmax, uint8_t fase)
{
    float   ph[ALE_TONOS], phmax = -1.0f, suelo, em[AFC_HIPOTESIS + 1U];
    uint8_t h, t, enc[AFC_HIPOTESIS + 1U], mejor_h;

    for (t = 0U; t < ALE_TONOS; t++) {
        ph[t] = goertzel(s_coef_hip[s_hip][t]);
        if (ph[t] > phmax) { phmax = ph[t]; }
    }
    if (pmax > s_m0_max)  { s_m0_max = pmax;  memcpy(s_m0, pt, sizeof s_m0); }
    if (phmax > s_mh_max) { s_mh_max = phmax; memcpy(s_mh, ph, sizeof s_mh); }
    if (fase != (ALE_FASES - 1U)) { return; }

    /* Cierra el simbolo: a los promedios, y la siguiente rejilla. */
    h = (uint8_t)(s_hip + 1U);
    for (t = 0U; t < ALE_TONOS; t++) {
        s_e[0][t] += AFC_A0 * (s_m0[t] - s_e[0][t]);
        s_e[h][t] += AFC_A1 * (s_mh[t] - s_e[h][t]);
    }
    s_m0_max = -1.0f;
    s_mh_max = -1.0f;
    s_hip = (uint8_t)((s_hip + 1U) % AFC_HIPOTESIS);
    if (s_eval < 0xFFFFU) { s_eval++; }
    if (s_eval < AFC_MIN_EVAL) { s_senal = 0U; return; }

    /* El suelo, la rejilla mas pobre por tono; los tonos encendidos de
     * cada una, y su energia. */
    suelo = -1.0f;
    for (h = 0U; h <= AFC_HIPOTESIS; h++) {
        em[h] = 0.0f;
        for (t = 0U; t < ALE_TONOS; t++) { em[h] += s_e[h][t]; }
        if ((suelo < 0.0f) || (em[h] < suelo)) { suelo = em[h]; }
    }
    suelo *= (1.0f / (float)ALE_TONOS);
    for (h = 0U; h <= AFC_HIPOTESIS; h++) {
        float mx = 0.0f;

        for (t = 0U; t < ALE_TONOS; t++) { if (s_e[h][t] > mx) { mx = s_e[h][t]; } }
        enc[h] = 0U;
        for (t = 0U; t < ALE_TONOS; t++) {
            if ((s_e[h][t] > AFC_SOBRE * suelo) && (s_e[h][t] >= AFC_PAREJO * mx)) { enc[h]++; }
        }
    }
#ifdef ALE_AFC_TRAZA
    {
        static uint32_t n;
        if ((++n % 32U) == 0U) {
            printf("afc %+6.1f suelo %.3g | em %.3g %.3g %.3g %.3g %.3g | enc %u %u %u %u %u\n",
                   (double)s_afc, (double)suelo, (double)em[0], (double)em[1], (double)em[2],
                   (double)em[3], (double)em[4], enc[0], enc[1], enc[2], enc[3], enc[4]);
        }
    }
#endif

    /* Otra rejilla con senal y bastante mas energia: alli. */
    mejor_h = 0U;
    for (h = 1U; h <= AFC_HIPOTESIS; h++) {
        if ((enc[h] >= AFC_ENCENDIDOS) && (em[h] > AFC_MEJOR * em[0])
            && (em[h] > em[mejor_h])) { mejor_h = h; }
    }
    if ((mejor_h == 0U) || (mejor_h != s_gana)) { s_gana_n = 0U; }
    s_gana = mejor_h;
    if ((mejor_h != 0U) && (s_gana_n < 0xFFU)) { s_gana_n++; }
    if ((mejor_h != 0U) && (s_gana_n >= AFC_SEGUIDOS)) {
        s_afc += k_hip[mejor_h - 1U];
        if (s_afc >  AFC_MAX) { s_afc -= 250.0f; }   /* la misma rejilla, */
        if (s_afc < -AFC_MAX) { s_afc += 250.0f; }   /* dentro del margen */
        afc_registros_cero();
        afc_bordes_cero();
        afc_aplica();
        return;
    }
    s_senal = (uint8_t)(enc[0] >= AFC_ENCENDIDOS);
}

/*
 * En la fase elegida, con senal en la rejilla de ahora: el discriminador
 * sobre el tono que ha ganado, si el simbolo es claro, y los bordes.
 */
static void afc_sigue(const float *pt, uint8_t mejor)
{
    float   otros, bajo, alto;

    if (!s_senal) { return; }
    if (claro(pt, mejor, &otros)) {
        float mm = sqrtf(goertzel(s_coef_mas[mejor]));
        float mn = sqrtf(goertzel(s_coef_menos[mejor]));

        if ((mm + mn) > 0.0f) {
            float o[ALE_TONOS], d, e = AFC_DELTA * (mm - mn) / (mm + mn);
            uint8_t i, j, n = 0U;

            /* El error de ESTE tono, a su media; y lo que se corrige es la
             * MEDIANA de los tonos que ya lo tienen medido (los que aun no,
             * en cero, la frenarian). Una portadora al lado de un tono
             * tuerce ese tono -con una a -6 dB a 50 Hz del bin, el CAF
             * acababa en la portadora- pero no la mediana. */
            if (s_err_hay & (1U << mejor)) { s_err[mejor] += 0.25f * (e - s_err[mejor]); }
            else { s_err[mejor] = e; s_err_hay = (uint8_t)(s_err_hay | (1U << mejor)); }
            for (i = 0U; i < ALE_TONOS; i++) {
                float v = s_err[i];

                if (!(s_err_hay & (1U << i))) { continue; }
                for (j = n; (j > 0U) && (o[j - 1U] > v); j--) { o[j] = o[j - 1U]; }
                o[j] = v;
                n++;
            }
            d = AFC_GANANCIA * (((n & 1U) != 0U) ? o[n / 2U]
                                                 : 0.5f * (o[n / 2U - 1U] + o[n / 2U]));
            s_afc += d;
            for (i = 0U; i < ALE_TONOS; i++) { s_err[i] -= d; }
        }
    }

    bajo = goertzel(s_coef_borde[0]);
    alto = goertzel(s_coef_borde[1]);
    s_b_bajo  += 0.05f * (bajo  - s_b_bajo);
    s_b_alto  += 0.05f * (alto  - s_b_alto);
    s_b_ruido += 0.05f * (otros - s_b_ruido);
    if (s_afc_n < 0xFFFFU) { s_afc_n++; }
    if (s_afc_n >= AFC_MIN_N) {
        if ((s_b_bajo > AFC_ALIAS * s_b_alto) && (s_b_bajo > AFC_ALIAS * s_b_ruido)) {
            s_afc -= 250.0f;            /* los tonos estan un puesto mas abajo */
            afc_bordes_cero();
            afc_registros_cero();       /* lo promediado era de otras rejillas */
        } else if ((s_b_alto > AFC_ALIAS * s_b_bajo) && (s_b_alto > AFC_ALIAS * s_b_ruido)) {
            s_afc += 250.0f;
            afc_bordes_cero();
            afc_registros_cero();
        }
    }

    afc_aplica();
}

#endif /* ALE_CAF */

static void mide(void)
{
    uint8_t  t, mejor = 0U;
    float    pmax = -1.0f;
    float    pt[ALE_TONOS];
    uint8_t  fase = (uint8_t)(s_cuenta / ALE_PASO);

#if ALE_CAF
    if (s_afc_cero_pedido) { afc_a_cero(); }
#endif

    /*
     * EN DOS TRAMOS SEGUIDOS, SIN MODULO - 05/10/2026.
     *
     * Esto recorria la ventana con `(s_hi + k) % ALE_SPS`, y ALE_SPS son 96:
     * no es potencia de dos, asi que cada uno de esos modulos es una
     * division entera de verdad. Ocho tonos por noventa y seis muestras son
     * 768 divisiones por llamada, y esto se llama cuatro veces por simbolo,
     * o sea quinientas veces por segundo: 384.000 divisiones al segundo
     * dentro de la interrupcion de audio.
     *
     * Un anillo recorrido entero desde un punto cualquiera son DOS TRAMOS
     * seguidos: del punto al final, y del principio al punto. Misma
     * secuencia exacta, mismas muestras en el mismo orden, cero divisiones.
     */
    for (t = 0U; t < ALE_TONOS; t++) {
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, p;
        const float c = s_coef[t];
        uint16_t k;

        for (k = s_hi; k < ALE_SPS; k++) {
            s0 = s_hist[k] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        for (k = 0U; k < s_hi; k++) {
            s0 = s_hist[k] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        p = (s1 * s1) + (s2 * s2) - (c * s1 * s2);
        pt[t] = p;
        if (p > pmax) { pmax = p; mejor = t; }
    }

    /*
     * LA FASE DE SIMBOLO SE ELIGE POR ENERGIA, y esto es lo unico que aqui
     * hace de recuperacion de reloj.
     *
     * La ventana de Goertzel cubre 96 muestras, que es un simbolo entero.
     * Si esta bien centrada, toda la energia cae en un tono; si esta a
     * caballo entre dos simbolos, se reparte entre dos y el pico baja. Asi
     * que midiendo en cuatro fases y quedandose con la que da el pico mas
     * alto se encuentra el centro del simbolo sin lazo ni interpolador.
     *
     * CUATRO FASES son 24 muestras de resolucion, o sea la cuarta parte de
     * un simbolo. ALE no exige mas: no hay deriva de reloj apreciable en
     * los 392 ms que dura una palabra.
     *
     * Y la fase SOLO se cambia mientras no hay enganche. Cambiarla a mitad
     * de palabra desplazaria los bits y tiraria la palabra entera.
     */
    s_fase_e[fase] += 0.02f * (pmax - s_fase_e[fase]);
    s_sim_f[fase] = k_gray[mejor];

    if (!s_eng) {
        uint8_t f, fm = 0U;

        for (f = 1U; f < ALE_FASES; f++) {
            if (s_fase_e[f] > s_fase_e[fm]) { fm = f; }
        }
        s_fase = fm;
    }

    s_nivel += 0.002f * ((pmax * 0.02f) - s_nivel);
    if (s_nivel < 0.0f) { s_nivel = 0.0f; }

    /*
     * UN SIMBOLO CADA NOVENTA Y SEIS MUESTRAS, SIEMPRE, Y NO "CUANDO TOCA
     * LA FASE BUENA". 27/09/2026.
     *
     * La primera version emitia el simbolo dentro del if de la fase
     * elegida, que parece lo natural y esta mal: cuando la eleccion de fase
     * cambia -y cambia sola mientras se busca, porque las medias de energia
     * todavia se estan asentando- el flujo de simbolos se salta uno o
     * emite dos, y eso desplaza el anillo de bits. A partir de ahi el
     * sincronismo de palabra busca en un flujo que ya no es el que se
     * emitio.
     *
     * El sintoma era finisimo: con la palabra descolocada 13 o 24 muestras
     * no decodificaba NADA, y con 0, 37, 48, 61, 72 u 85 decodificaba las
     * cuatro palabras. Ocho retardos probados, seis bien y dos a cero. Un
     * fallo asi en antena se lee como "esta frecuencia no entra".
     *
     * Ahora las cuatro fases miden siempre y cada una deja apuntado SU
     * simbolo; al cerrarse el periodo se emite el de la fase elegida. Una
     * emision por periodo, pase lo que pase con la eleccion, y cambiar de
     * fase ya no mueve el flujo: solo cambia de que ventana sale el valor.
     */
    if (fase == (ALE_FASES - 1U)) { simbolo(s_sim_f[s_fase]); }

    /* El CAF: la busqueda en cada medida, y el seguimiento una vez por
     * simbolo, en la fase elegida, que es la ventana cuadrada con el
     * simbolo. Despues de todo lo de arriba, para que lo que mueva valga
     * desde la medida siguiente y no a mitad de esta. */
#if ALE_CAF
    afc_fase(pt, pmax, fase);
    if (fase == s_fase) { afc_sigue(pt, mejor); }
#else
    (void)pt;
#endif
}

void ale_rx_mete(const float *audio, uint32_t n)
{
    uint32_t k;

    if (!s_on || (audio == (const float *)0)) { return; }

    for (k = 0U; k < n; k++) {
        s_hist[s_hi] = audio[k];
        s_hi++;
        if (s_hi >= ALE_SPS) { s_hi = 0U; }

        /*
         * El contador va modulo 96 -un simbolo entero- y no libre. Con un
         * contador libre de 16 bits, al dar la vuelta (65.536 no es
         * multiplo de 24) la numeracion de las cuatro fases se desplazaria,
         * y el decodificador se desengancharia solo cada cinco segundos y
         * medio sin que nada lo explicara.
         */
        s_cuenta++;
        if (s_cuenta >= ALE_SPS) { s_cuenta = 0U; }

        if (s_lleno < ALE_SPS) { s_lleno++; continue; }
        if ((s_cuenta % ALE_PASO) == 0U) { mide(); }
    }
}
