#ifndef MALAHIT_SAI1_H
#define MALAHIT_SAI1_H

#include <stdint.h>

/*
 * SAI1 as the I2S MASTER, which is the opposite of the DEEPSDR 101.
 *
 * On this board SAI1_CODEC_MCLK is an MCU OUTPUT on PE2, so the STM32
 * supplies the codec's master clock and the NAU8822 is the slave. The
 * DEEPSDR wiring has it the other way round - the AIC3204 is master and
 * the GD32 follows - so none of the upstream I2S setup transfers.
 *
 *   PE2  SAI1_CK1   AF6   MCLK out, 256 x Fs
 *   PE3  SAI1_SD_B  AF6   ADCOUT  - codec -> MCU, the I/Q pair
 *   PE4  SAI1_FS_A  AF6   FS  out
 *   PE5  SAI1_SCK_A AF6   BCLK out, 32 x Fs
 *   PE6  SAI1_SD_A  AF6   DACIN   - MCU -> codec, the audio out
 *
 * Block A is the master transmitter and owns the clocks; block B is a
 * slave receiver synchronous with A, so both directions run off one
 * frame clock. That is the textbook full-duplex pair and it is what the
 * pin assignment was clearly chosen for.
 *
 * 16-bit data in 16-bit slots, two slots, 32 bits a frame, standard I2S.
 * 16 bits because the whole upstream DSP tree is int16_t - gd32_i2s.c
 * runs I2S_FRAMEFORMAT_DT16B_CH16B and every buffer from sdr_rx.c down is
 * int16_t - and matching it keeps step 8 a transplant rather than a
 * rewrite. The codec's converters are nominally 24-bit but their SNR is
 * around 95 dB, so 16 bits is close to all they actually deliver.
 */

/*
 * WHY MCLK FORCES THE WHOLE CLOCK TREE TO BE RECOMPUTED.
 *
 * The NAU8822 derives everything from IMCLK, which it insists is exactly
 * 256 x Fs (BOARD.md section 6). With NODIV = 0 and OSR = 0 the SAI
 * generates MCLK = 256 x Fs from its kernel clock as
 *
 *     sai1_ker_ck = 256 x Fs x MCKDIV,     MCKDIV = 1..63
 *
 * and generates FS and BCLK from the same divider chain, which also
 * satisfies the codec's other requirement - that FS, BCLK and IMCLK all
 * come from one reference.
 *
 * So the kernel clock has to be an exact multiple of 256 x Fs. It cannot
 * be pll1_q_ck (240 MHz, the SPI3 clock), so PLL2 is dedicated to the
 * audio chain and retuned for every sample rate.
 *
 * THE ARITHMETIC DOES NOT COME OUT EVEN, AND IT CANNOT.
 *
 *   192 kHz needs MCLK = 49.152 MHz = 6144/125 MHz
 *   160 kHz needs MCLK = 40.96  MHz = 1024/25  MHz
 *
 * Both carry a factor of 5^3 in the denominator against a 22 MHz
 * reference. A PLL output is HSE/M x (N + FRACN/8192) / P, and 8192 is a
 * power of two, so the only place a factor of five can come from is M or
 * P. Working it through: an exact 49.152 MHz needs M x P = 1375 x k,
 * which forces N above 3000 against a hardware maximum of 512. Exact
 * 160 kHz fails the same way.
 *
 * NEITHER OF THE TWO CANDIDATE SAMPLE RATES IS EXACTLY SYNTHESISABLE ON
 * THIS BOARD. That sounds worse than it is. Because MCKDIV and P between
 * them offer thousands of places for the fractional divider to work, the
 * solver lands within a few hundred parts per BILLION - 192 kHz comes
 * out at 191999.988 Hz, 160 kHz at 160000.07 Hz. X1 itself is a
 * commercial oscillator module whose own tolerance measured 1500 ppm in
 * step 1, four orders of magnitude larger. The residual division error
 * is not the thing that will ever be wrong with this receiver.
 *
 * (Which is why the error below is reported in ppb. In ppm every rate
 * rounds to zero, and a column of zeroes says nothing about whether the
 * arithmetic was done at all.)
 *
 * The divider M is fixed at 2, giving an 11 MHz phase comparison
 * frequency rather than the 2 MHz PLL1 has to live with. For an SDR that
 * is not cosmetic: MCLK jitter lands directly on the I/Q sampling
 * instants, so it is the receiver's own noise floor.
 */
#define SAI1_PLL2_DIVM      2U
#define SAI1_PLL2_REF_HZ    11000000U      /* HSE_HZ / SAI1_PLL2_DIVM */

/*
 * The VCO has to stay inside 192..960 MHz (wide range, PLL2VCOSEL = 0),
 * and a higher VCO gives finer fractional resolution, so the solver
 * prefers one. The kernel clock ceiling is a self-imposed 200 MHz:
 * nothing here needs more, and SAI1 sits on APB2 in D2.
 */
#define SAI1_VCO_MIN_HZ     192000000U
#define SAI1_VCO_MAX_HZ     960000000U
#define SAI1_KER_MAX_HZ     200000000U

/*
 * What the solver decided, so main() can print the plan BEFORE applying
 * it. A clock configuration that is going to be wrong should say so
 * while the console still works, not after the codec has gone quiet.
 */
typedef struct {
    uint32_t fs_target;     /* what was asked for                        */
    uint32_t divn;          /* PLL2 N, integer part                      */
    uint32_t fracn;         /* PLL2 FRACN, 0..8191; 0 means integer mode */
    uint32_t divp;          /* PLL2 P                                    */
    uint32_t mckdiv;        /* SAI MCKDIV                                */
    uint32_t vco_hz;        /* achieved                                  */
    uint32_t ker_hz;        /* achieved                                  */
    uint32_t mclk_hz;       /* achieved, = 256 x fs_actual               */
    uint32_t fs_hz;         /* achieved, rounded                         */
    int32_t  err_ppb;       /* achieved against target, signed, in ppb   */
    int      ok;            /* 0 if no configuration was found at all    */
} sai1_clock_t;

/*
 * Work out the best PLL2 and MCKDIV for a sample rate. Pure arithmetic,
 * touches no register, so it is safe to call for rates that will never
 * be used - which is exactly what the Fs sweep does.
 */
void sai1_solve(uint32_t fs_hz, sai1_clock_t *out);

/*
 * Pins, RCC, block A and block B. Does not start the clocks.
 * Returns 0, or SAI1_ERR_* below.
 */
int sai1_init(void);

/*
 * Retune to a sample rate: stop the blocks, re-lock PLL2, rewrite
 * MCKDIV, start again. The codec sees MCLK stop and restart, so its
 * registers survive but its converters need a moment to settle.
 */
int sai1_set_fs(uint32_t fs_hz);

/* What sai1_set_fs() actually achieved. */
const sai1_clock_t *sai1_clock(void);

void sai1_start(void);
void sai1_stop(void);

/*
 * Receive timing on block B: the clock edge it samples on, and whether
 * it expects data one bit after the frame edge.
 *
 * These two bits exist as a knob because the documentation and ST's own
 * HAL disagree about the first one, and the hardware settled it. RM0433
 * describes CKSTR = 0 as "received signals are sampled on the rising
 * edge", which is what I2S wants; HAL_SAI sets CKSTR for every receiver,
 * which says the opposite. Reading one of them and believing it is a
 * coin toss.
 *
 * Getting it wrong does not look like a timing error. It looks like a
 * railed converter: sampling on the same edge the codec changes its
 * data returns the PREVIOUS bit, so every sample arrives shifted one
 * place, the sign bit is whatever the last bit of the previous slot
 * happened to be, and a quiet input reading -310 comes out alternating
 * between +32613 and -155. The statistics then say "mean 16384, rms
 * 16384", which is arithmetically indistinguishable from an overloaded
 * input and sends you looking in the wrong half of the board.
 *
 * ckstr 0 leaves SAI_xCR1.CKSTR clear, 1 sets it.
 * fsoff 1 is I2S (data one bit after the FS edge), 0 is left-justified.
 */
void sai1_set_rx_timing(int ckstr, int fsoff);

/*
 * Play tone_frames stereo frames cyclically while capturing rx_frames of
 * them, polled, both directions in lockstep off the one frame clock.
 *
 * Polled rather than DMA on purpose. At 192 kHz a frame is 2500 CPU
 * cycles and there are four words to move, so the loop is nowhere near
 * the limit - and polling keeps the D-cache question out of step 4
 * entirely. The H7's cache against DMA is the classic "works, except
 * sometimes" failure (PORTING.md section 3.4); it belongs in a step that
 * is about DMA, with the MPU regions that make it safe, not smuggled
 * into the step that is about the codec.
 *
 * tone may be NULL for silence, rx may be NULL to just play.
 * Returns the number of frames captured, which is rx_frames unless the
 * bounded wait gave up - a short count is the signal that the clocks
 * stopped.
 */
uint32_t sai1_stream(const int16_t *tone, uint32_t tone_frames,
                     int16_t *rx, uint32_t rx_frames);

/*
 * Play frames without restarting anything, keeping the tone's phase
 * across calls.
 *
 * sai1_stream() stops and restarts the blocks so that frame alignment is
 * established rather than assumed, which is right for a measurement and
 * wrong for a tone: a restart every few tens of milliseconds is audible
 * as a click at exactly the repetition rate of the calls, and a clicking
 * tone is indistinguishable from a tone being generated badly.
 *
 * So this one just feeds the transmitter and keeps the receive FIFO
 * drained. The phase accumulator is kept here rather than by the caller
 * so that consecutive calls join seamlessly.
 */
void sai1_play(const int16_t *tone, uint32_t tone_frames, uint32_t frames);

/*
 * Overrun/underrun events seen during the last sai1_stream(). This is
 * the stability criterion for the Fs sweep: the clocks can be wrong in
 * ways that still produce data, but a codec or a bus that cannot keep up
 * shows here.
 */
uint32_t sai1_tx_underruns(void);
uint32_t sai1_rx_overruns(void);

/*
 * The actual frame rate, measured over the last sai1_stream() against
 * the CPU clock rather than calculated from the dividers. Step 1 proved
 * the clock tree this way - SysTick against a debugger pause - and it is
 * the only kind of evidence that catches a divider that the hardware
 * quietly refused.
 */
uint32_t sai1_measured_fs(void);

/*
 * SAI_xSR for both blocks as they stood at the end of the last stream.
 * Worth having when a capture comes up short: FLVL says whether the
 * receive FIFO was empty because nothing arrived, and the flag bits say
 * whether the block had faulted or simply stopped.
 */
uint32_t sai1_sr_a(void);
uint32_t sai1_sr_b(void);

enum {
    SAI1_OK              =  0,
    SAI1_ERR_NO_CLOCK    = -1,  /* no PLL2 configuration for that rate */
    SAI1_ERR_PLL2        = -2,  /* PLL2 never locked                   */
    SAI1_ERR_NOT_READY   = -3   /* sai1_init() not called              */
};

#endif /* MALAHIT_SAI1_H */
