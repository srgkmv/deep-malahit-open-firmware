#include "encoder.h"
#include "gd32f4xx.h"
#include "debug_uart.h"

/*
 * WHICH PINS, AND WHY THEY ARE OVERRIDABLE - 05/11/2026.
 *
 * These were six fixed defines, which is right for one board and wrong
 * for two. The Malahit DSP-1 port (malahit/) wires the same encoder to
 * PC0/PC1 with its button on PA1, and it compiles THIS FILE rather than
 * a copy of it, so the three pins arrive from its build instead. With no
 * -D the DEEPSDR 101 gets exactly what it always got.
 */
#ifndef ENC_A_PORT
#define ENC_A_PORT   GPIOD
#endif
#ifndef ENC_A_PIN
#define ENC_A_PIN    GPIO_PIN_13
#endif
#ifndef ENC_B_PORT
#define ENC_B_PORT   GPIOD
#endif
#ifndef ENC_B_PIN
#define ENC_B_PIN    GPIO_PIN_12
#endif
#ifndef ENC_BTN_PORT
#define ENC_BTN_PORT GPIOC
#endif
#ifndef ENC_BTN_PIN
#define ENC_BTN_PIN  GPIO_PIN_9
#endif

/*
 * WHICH WAY THE BUTTON GOES, which is a property of the board and not of
 * the decoder.
 *
 * On the DEEPSDR 101 the button drives Vcc when pressed and rests at 0 V,
 * so pressed = SET and the line wants an internal pull-DOWN - the
 * opposite of the A/B channels, and surprising enough that encoder.h
 * says so twice. A board whose switch goes to ground instead sets
 * ENC_BTN_PRESSED to 0 and gets a pull-up, and nothing else in this file
 * changes: the debounce integrator, the hold timer and the short/long
 * classification are all written in terms of "the debounced state is a
 * press", not in terms of a voltage.
 */
#ifndef ENC_BTN_PRESSED
#define ENC_BTN_PRESSED  1
#endif

#define ENC_BTN_PUPD     (ENC_BTN_PRESSED ? GPIO_PUPD_PULLDOWN \
                                          : GPIO_PUPD_PULLUP)

/* Confirmed inverted on real hardware, 31/07/2026: tuning went
 * backwards with ENCODER_DIRECTION=1 (i.e. A leading B is actually
 * counter-clockwise on this knob, opposite of what was assumed when
 * this flag was first added - see encoder.h's encoder_take_delta()
 * comment). Flipped here rather than swapping the A/B wires.
 *
 * 30/09/2026: esto es el sentido de ESTA placa, no el de todas. El usuario
 * estreno otra unidad y giraba al reves que la suya de siempre: donde una
 * subia la frecuencia al girar en sentido horario, la otra la bajaba. O
 * sea que el cableado del mando varia entre placas y una constante de
 * compilacion no puede acertar en las dos.
 *
 * Asi que esto se queda como el sentido POR DEFECTO de la placa, y encima
 * va s_invertido, que el usuario pone desde Ajustes -> Equipo -> Mando y se
 * guarda en CONFIG.CSV. Uno es de fabrica y el otro es tuyo. */
#define ENCODER_DIRECTION -1

/* 0 = como venga la placa, 1 = al reves. Ver encoder_invertido_pon(). */
static uint8_t s_invertido;

/* Quarter-steps per detent of the mechanical encoder. The common
 * EC11-style part gives one full quadrature cycle (4 transitions) per
 * detent; if this encoder turns out to be a 2-per-detent type, set
 * this to 2. */
#define QUARTER_STEPS_PER_DETENT 4

#define BTN_DEBOUNCE_MS 20U

/* Held this long (or more) before release = LONG press instead of
 * SHORT - see encoder.h's comment on why classification happens on
 * release. 600ms comfortably clears an intentional-but-quick click
 * (even a deliberately slow one) while still feeling immediate for a
 * "hold to exit" gesture. */
#define BTN_LONG_PRESS_MS 600U

/*
 * Full-quadrature transition table, indexed by (prev_state << 2) |
 * new_state, where state = (A << 1) | B. Valid Gray transitions map
 * to +/-1, everything else (no change, or an "impossible" two-bit
 * jump from noise) maps to 0.
 */
static const int8_t k_qdec[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0,
};

static volatile int32_t s_qsteps    = 0; /* accumulated quarter-steps */
static volatile uint8_t s_presses   = 0; /* pending debounced SHORT presses */
static volatile uint8_t s_long_presses = 0; /* pending debounced LONG presses */
static uint8_t          s_ab_prev   = 0; /* last sampled (A<<1)|B     */
static uint8_t          s_btn_integ = 0; /* debounce integrator, ms   */
static uint8_t          s_btn_state = 0; /* debounced press, 0 = idle, 1 = pressed */
static uint16_t         s_btn_held_ms = 0; /* ms held since the current debounced press started; only meaningful while s_btn_state==1 */
/*
 * 1 = la pulsacion que hay en curso YA se ha usado para otra cosa (girar
 * el mando con el boton apretado), asi que al soltar no se reporta ni
 * pulsacion corta ni larga.
 *
 * Por que hace falta: el gesto "aprieta y gira" del firmware original
 * termina siempre en una suelta, y esa suelta, sin esto, dispararia ademas
 * lo que signifique la pulsacion corta - cambiar el paso de sintonia, que
 * es justo lo que el gesto acaba de estar ajustando. Se marca desde el
 * bucle principal (encoder_consume_press) en cuanto llega el primer detente
 * con el boton apretado, y se limpia sola en el siguiente flanco de
 * pulsacion.
 */
static volatile uint8_t s_btn_consumed = 0;

static uint8_t ab_read(void)
{
    uint8_t a = (gpio_input_bit_get(ENC_A_PORT, ENC_A_PIN) == SET) ? 1U : 0U;
    uint8_t b = (gpio_input_bit_get(ENC_B_PORT, ENC_B_PIN) == SET) ? 1U : 0U;
    return (uint8_t)((a << 1) | b);
}

/*
 * WHEN THE BUTTON IS NOT A PIN AT ALL - 05/11/2026.
 *
 * ENC_BTN_READ names a function the board provides, and if it is defined
 * that function is the whole of the reading: no port, no pin, no pull.
 * The Malahit DSP-1 is why - both of its encoder buttons sit on ONE
 * analog line through a resistor ladder, so one of the two presses puts
 * it at 1.95 V, which on a 3.3 V input is neither a low nor a high. No
 * pull and no polarity can rescue that; it takes the ADC and a
 * comparison against two thresholds.
 *
 * Everything below this line stays exactly as it was, which is the
 * reason for doing it here rather than in the board's own code: the
 * debounce integrator, the hold timer, the short/long classification on
 * release and the "pressed and turned" consumption are all written in
 * terms of "the debounced state is a press". None of them cares where
 * the press came from, and a board that reads its button differently
 * should not need a second copy of any of it.
 *
 * With no -D the DEEPSDR 101 compiles the GPIO reader below, unchanged.
 */
#ifdef ENC_BTN_READ
extern uint8_t ENC_BTN_READ(void);

static uint8_t btn_read(void)
{
    return ENC_BTN_READ() ? 1U : 0U;
}
#else
/* 1 = pressed, whichever voltage that is on this board. Everything below
 * is written in terms of this and not of a level - see ENC_BTN_PRESSED. */
static uint8_t btn_read(void)
{
    uint8_t lvl = (gpio_input_bit_get(ENC_BTN_PORT, ENC_BTN_PIN) == SET)
                      ? 1U : 0U;

    return (lvl == (uint8_t)ENC_BTN_PRESSED) ? 1U : 0U;
}
#endif

void encoder_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);

    gpio_mode_set(ENC_A_PORT, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, ENC_A_PIN);
    gpio_mode_set(ENC_B_PORT, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, ENC_B_PIN);
#ifndef ENC_BTN_READ
    /* The button's pull is the one that holds it at its IDLE level, which
     * depends on which rail the switch goes to - see ENC_BTN_PRESSED.
     *
     * NOT TOUCHED when the board reads its button some other way, and
     * this guard is not tidiness. ENC_BTN_PORT and ENC_BTN_PIN still
     * have their defaults, so without it this line would configure
     * whatever pin those happen to name - on the Malahit that is PC9,
     * which carries the touch controller's I2C data, and a pull-up
     * written onto a bus by an encoder driver is a fault nobody would
     * look for here. */
    gpio_mode_set(ENC_BTN_PORT, GPIO_MODE_INPUT, ENC_BTN_PUPD, ENC_BTN_PIN);
#endif

    s_ab_prev = ab_read();
    s_btn_state = btn_read();
    s_btn_integ = 0;

    debug_print("encoder: A/B and button configured\n");
    debug_print_dec("encoder: initial AB state", s_ab_prev);
    debug_print_dec("encoder: initial button level (0=idle)", s_btn_state);
}

void encoder_tick(void)
{
    uint8_t ab;
    uint8_t btn_raw;

    /* --- quadrature --- */
    ab = ab_read();
    if (ab != s_ab_prev) {
        s_qsteps += k_qdec[(uint8_t)((s_ab_prev << 2) | ab)];
        s_ab_prev = ab;
    }

    /* --- button, integrating debounce ---
     * The integrator ramps toward the raw level and the debounced
     * state only flips once it saturates: a press must hold LOW for
     * BTN_DEBOUNCE_MS consecutive ms to register, and likewise for
     * release. Bounce shorter than that just wiggles the counter.
     *
     * Classification (SHORT vs LONG) happens on the RELEASE edge, not
     * the press edge - see encoder.h's comment for why. While held,
     * s_btn_held_ms just counts ms since the debounced press started
     * (saturating rather than wrapping on an implausibly long hold);
     * on the debounced release it's compared against
     * BTN_LONG_PRESS_MS to decide which counter to bump. */
    btn_raw = btn_read();
    if (btn_raw == s_btn_state) {
        s_btn_integ = 0;
        if (s_btn_state == 1U && s_btn_held_ms < 0xFFFFU) {
            s_btn_held_ms++;
        }
    } else {
        s_btn_integ++;
        if (s_btn_integ >= BTN_DEBOUNCE_MS) {
            s_btn_state = btn_raw;
            s_btn_integ = 0;
            if (s_btn_state == 1U) { /* 0->1 = a press started */
                s_btn_held_ms = 0;
                s_btn_consumed = 0U; /* pulsacion nueva, aun sin usar */
            } else { /* 1->0 = released - classify now that the hold time is known */
                if (s_btn_consumed) {
                    /* se gasto girando: ni corta ni larga - ver
                     * s_btn_consumed */
                } else if (s_btn_held_ms >= BTN_LONG_PRESS_MS) {
                    s_long_presses++;
                } else {
                    s_presses++;
                }
            }
        }
    }
}

uint8_t encoder_button_down(void)
{
    return s_btn_state;
}

void encoder_consume_press(void)
{
    s_btn_consumed = 1U;
}

int32_t encoder_take_delta(void)
{
    int32_t q;
    int32_t detents;
    uint32_t primask;

    primask = __get_PRIMASK();
    __disable_irq();
    q = s_qsteps;
    /* Keep the sub-detent remainder for next time; hand out only
     * whole detents. C99 division truncates toward zero, which is
     * exactly what we want for both signs here. */
    detents  = q / QUARTER_STEPS_PER_DETENT;
    s_qsteps = q % QUARTER_STEPS_PER_DETENT;
    __set_PRIMASK(primask);

    /* El ajuste del usuario se aplica AQUI, en el unico sitio por donde
     * salen los pasos del mando de verdad. Los pasos INYECTADOS -los
     * botones "-" y "+" de la pantalla de detalle, ver s_inject_detents en
     * main.c- no pasan por aqui y no deben invertirse: esos ya vienen en
     * "mas" y "menos", no en "horario" y "antihorario".
     *
     * Comprobado en la placa el 30/09/2026, que es lo unico que vale aqui:
     * con "Mando" = "Invertido", girando en sentido horario la frecuencia
     * sube, y los botones "-" y "+" de detalle siguen bajando y subiendo en
     * las DOS posiciones del ajuste. El banco no puede ver esto: mide el
     * signo que sale de esta funcion, no hacia donde gira un mando fisico. */
    return detents * ENCODER_DIRECTION * (s_invertido ? -1 : 1);
}

void encoder_invertido_pon(uint8_t v)
{
    s_invertido = (uint8_t)(v ? 1U : 0U);
}

uint8_t encoder_invertido(void)
{
    return s_invertido;
}

uint8_t encoder_take_press(void)
{
    uint8_t p = 0;
    uint32_t primask;

    primask = __get_PRIMASK();
    __disable_irq();
    if (s_presses > 0U) {
        s_presses--;
        p = 1;
    }
    __set_PRIMASK(primask);

    return p;
}

uint8_t encoder_take_long_press(void)
{
    uint8_t p = 0;
    uint32_t primask;

    primask = __get_PRIMASK();
    __disable_irq();
    if (s_long_presses > 0U) {
        s_long_presses--;
        p = 1;
    }
    __set_PRIMASK(primask);

    return p;
}
