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
 * them, POLLED, both directions in lockstep off the one frame clock.
 *
 * This was the only transport until the DMA section below existed, and
 * it is kept because it is the one that establishes frame alignment in
 * hardware and because every measurement in BOARD.md section 6 was taken
 * through it. The tests in main.c still use it, unchanged, so their
 * numbers stay comparable across this step.
 *
 * It costs what it always cost: the transmit FIFO is eight words - 83 us
 * at 48 kHz - so anything that blocks this loop for longer is an audible
 * gap. A console line at 115200 baud is thirty times longer. That is
 * what sai1_dma_start() is for.
 *
 * tone may be NULL for silence, rx may be NULL to just play.
 * Returns the number of frames captured, which is rx_frames unless the
 * bounded wait gave up - a short count is the signal that the clocks
 * stopped.
 *
 * IF THE DMA TRANSPORT IS RUNNING, THIS STOPS IT. Both cannot own DR:
 * one CPU write into a FIFO a DMA stream is also filling is one word too
 * many, and from then on left is right. sai1_mode_switches() counts how
 * often that happened, because a run that keeps switching transports is
 * a bug in the caller and should be visible rather than merely safe.
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
 * Overrun/underrun events. This is the stability criterion for the Fs
 * sweep: the clocks can be wrong in ways that still produce data, but a
 * codec or a bus that cannot keep up shows here.
 *
 * TWO DIFFERENT MEASUREMENTS UNDER ONE NAME, and the difference matters
 * when a number is quoted. On the polled path these are counted inside
 * the transfer loop, so they are close to a count of events. On the DMA
 * path there is no interrupt, so OVRUDR is read at each
 * sai1_rx_half()/sai1_tx_write_half() call and the count is "calls that
 * found the flag set" - the flag is sticky, so nothing is missed, but
 * five underruns inside one frame read as one. Which is enough for the
 * question being asked of it: zero or not zero.
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

/* ==================================================================== */
/* THE DMA TRANSPORT                                                    */
/* ==================================================================== */

/*
 * WHY DMA, AND WHY IT IS NOT ABOUT CPU LOAD.
 *
 * At 192 kHz a stereo frame is 2500 CPU cycles and four half-words, so
 * the polled loop above is nowhere near its limit and never was. What it
 * cannot do is be LATE. The transmit FIFO is eight words deep - four
 * frames, 21 us at 192 kHz - and anything that holds the CPU for longer
 * than that empties it:
 *
 *   a console line at 115200 baud    ~9 000 us    one click per line
 *   the waterfall blit over SPI     ~40 000 us    the whole frame silent
 *   the trace redraw                ~17 000 us
 *
 * So the panel and the audio could not coexist, and the symptom is not
 * "slow" but "audibly broken", which is a different bug and gets blamed
 * on the codec. With the transfers on DMA the deadline belongs to a
 * state machine that does not care what the CPU is doing.
 *
 * NO MPU REGIONS, AND NO CACHE MAINTENANCE, BECAUSE THE D-CACHE IS OFF.
 * clock.c's SystemInit() and the reset handler both leave DC clear
 * (WORKING.md has why), so there are no dirty lines for a DMA write to
 * miss and no stale lines for a DMA read to hide behind. That is the
 * whole reason this step is small. Anything that later turns the data
 * cache on owes these two buffers a non-cacheable MPU region, and that
 * is the step where the H7's classic "works, except sometimes" arrives -
 * see PORTING.md section 3.4.
 *
 * THE RINGS LIVE IN AXI SRAM, which DMA1 can reach and DTCM is not.
 * Everything in this image is linked into AXI SRAM at 0x24000000
 * (stm32h743_ram.ld says why), and DMA1 gets there through the D2-to-D1
 * bridge. The trap worth knowing is the one that does NOT apply here:
 * DTCM at 0x20000000 is CPU-local and no DMA controller can see it, so a
 * buffer that drifts there produces a transfer error and nothing else.
 * sai1_dma_errors() is how that would be noticed.
 *
 * NO INTERRUPT, DELIBERATELY. The consumer is main()'s display loop at
 * about ten frames a second and the producer is a circular stream; which
 * half is finished can simply be read out of NDTR, which is what
 * upstream's own gd32_i2s_stream_write_half() does on the DEEPSDR. An
 * interrupt buys nothing until there is a demodulator that must run once
 * per block - and when that arrives it also needs a named vector, which
 * startup_stm32h743.S currently does not have for any DMA stream (all
 * 150 external vectors point at Default_Handler). Writing the ISR is
 * step 8's work; the transport is this step's.
 */

/*
 * Ring geometry, in stereo frames per half. Two halves each, so the
 * buffers are twice this and four times it in half-words.
 *
 * RX is sized for main.c's CAP_FRAMES, which is 8192 - 42.7 ms at
 * 192 kHz.
 *
 * TX NOW MATCHES IT, AND THAT IS WHAT MAKES AUDIO POSSIBLE. It used to
 * be 1024 frames, chosen as a small output latency back when the only
 * thing it ever carried was a test tone written from the display loop.
 * A demodulator changes the question: audio is produced by the RX
 * block interrupt, one receive half at a time, so a transmit half that
 * holds less than a receive half cannot take what a block produces and
 * the two would have to be re-synchronised every block - which is the
 * shape of problem that ends in clicks nobody can reproduce.
 *
 * Equal halves make it arithmetic instead: one receive half in, one
 * transmit half out, locked to the same DMA. The cost is 56 kB of
 * AXISRAM and an output latency of 42.7 ms, which on a receiver is
 * below noticing - it is the delay of a metre of air.
 */
#define SAI1_RX_HALF_MAX    8192U
#define SAI1_TX_HALF_MAX    8192U

/*
 * THE RECEIVE BLOCK HOOK - the one piece this transport was missing.
 *
 * Called from the DMA's own interrupt every time a receive half fills,
 * with a pointer to that half and its size in stereo frames. It is where
 * a demodulator belongs: it runs at block rate (42.7 ms here) rather
 * than at the display's frame rate, which is 90 ms and cannot feed an
 * audio stream at all.
 *
 * It also retires the hazard sai1_tx_due() was always going to have.
 * That mechanism asks "is a transmit half free" from whatever loop
 * happens to be running, and a producer more than one half late simply
 * misses it. A hook driven by the DMA cannot be late: it is the DMA
 * saying so.
 *
 * IT RUNS IN INTERRUPT CONTEXT, below SysTick, so the 1 kHz tick that
 * samples the ADC and the encoders still preempts it. Whatever it does
 * must be bounded, must not print, and must not wait.
 *
 * Null by default, and a null hook is not an error - it is this
 * transport doing exactly what it did before the hook existed.
 *
 * IT IS HANDED BOTH HALVES, receive and transmit, and that is not a
 * convenience. The two rings are the same size and are clocked by the
 * same frame clock - block A transmits what block B receives, in step -
 * so the transmit half that was being played while this receive half
 * was filling is exactly the one that is now free for 42.7 ms. Handing
 * it over removes the staging buffer a demodulator would otherwise
 * need, which at this block size is 32 kB of a part that is already at
 * 89 % of its RAM, and removes the question "which half is safe to
 * write" from every caller that will ever exist.
 */
typedef void (*sai1_block_hook_t)(const int16_t *rx_half, int16_t *tx_half,
                                  uint32_t frames);

void sai1_set_block_hook(sai1_block_hook_t fn);

/* How many block interrupts have arrived, and how many found the hook
 * still running from last time - which is the number that says the
 * demodulator is too slow for the block rate. */
extern volatile uint32_t sai1_blocks;
extern volatile uint32_t sai1_block_overruns;

/*
 * Arm both directions and start the clocks.
 *
 * rx_frames and tx_frames are the half sizes, in stereo frames, each at
 * most the maximum above. The transmit ring starts SILENT rather than
 * holding whatever the last run left - an audio buffer that comes up
 * playing the previous image's samples is a click at best and, over SWD,
 * is a different image's samples.
 *
 * Returns SAI1_OK, or SAI1_ERR_DMA_SIZE for an impossible half size.
 */
int sai1_dma_start(uint32_t rx_frames, uint32_t tx_frames);

void sai1_dma_stop(void);
int  sai1_dma_running(void);

/* Frames into the receive half the DMA is filling now - with
 * sai1_blocks, which half that is. For pinning an event (a retune) to
 * the sample it happened at. */
uint32_t sai1_rx_frame_pos(void);

/*
 * Hand over the most recently FINISHED receive half.
 *
 * frames must equal the rx_frames the transport was armed with: the
 * caller says how long its buffer is and this refuses rather than
 * guessing. That rule is upstream's, and it is written in blood - the
 * same length contract broke twice on the DEEPSDR because it was implied
 * by two constants in two files instead of passed (gd32_i2s.h has the
 * two dates).
 *
 * The block is contiguous and correctly aligned - slot 0 at even
 * indices - because a synchronous slave receiver starts on the frame its
 * master generates, so the first half-word the stream ever writes is
 * slot 0 and the parity is fixed by construction.
 *
 * WHAT IT DOES NOT PROMISE IS RECENCY. The half handed over ended
 * somewhere between now and one half-time ago (42.7 ms at the default
 * size), and the stream never stops, so samples keep arriving into the
 * other half while this one is read. That is the right trade for a
 * display and the WRONG one for "change a register, then measure": a
 * block fetched immediately after a gain change can be entirely from
 * before it. The tests in main.c run on the polled path for exactly this
 * reason.
 *
 * Returns frames, or 0 if the transport is not running, the length does
 * not match, or the copy was overtaken by the writer twice in a row.
 */
uint32_t sai1_rx_half(int16_t *dst, uint32_t frames);

/*
 * Put one half's worth of audio into whichever transmit half is NOT
 * being played, derived from the live DMA position.
 *
 * frames must equal the tx_frames armed, for the reason above.
 *
 * Nothing has to call this. With the ring left as it is the transmitter
 * keeps sending whatever is in it, which after sai1_dma_start() is
 * silence - so an image with no audio source still holds the bus
 * correctly and still proves the transport by never underrunning.
 *
 * THE HALF CHOICE HAS NO MEMORY, which is upstream's own hard-won
 * observation: if the call lands near a half boundary it can pick the
 * same half twice and the other one then repeats. That is inaudible for
 * a tone whose period divides the half and a comb of harmonics for
 * anything else, so it is counted - sai1_tx_repeats() - rather than
 * hoped about.
 */
int sai1_tx_write_half(const int16_t *stereo_frames, uint32_t frames);

/*
 * 1 when there is a half waiting to be filled: the one that is not
 * playing is no longer the one the last sai1_tx_write_half() wrote.
 *
 * This is the POLLED STAND-IN for the half-transfer interrupt, and it
 * has the limitation that comes with polling: a caller that is more than
 * a whole half late finds the free half back to the one it wrote last,
 * reads "nothing due", and the ring quietly repeats. That cannot be
 * fixed without the interrupt, which is step 8's - and until there is a
 * demodulator producing blocks, nothing is late. Said out loud here
 * because the failure is silent and sounds like a comb of harmonics
 * rather than like a missed deadline.
 */
int sai1_tx_due(void);

/*
 * How far into the playing half the last write landed, in half-words,
 * lowest and highest seen. Upstream's phase-margin diagnostic: both low
 * means the producer is comfortable, both high means it is arriving at
 * the end of its budget, and a rising max is the warning before a
 * repeat.
 */
uint32_t sai1_tx_margin_min(void);
uint32_t sai1_tx_margin_max(void);
uint32_t sai1_tx_repeats(void);
uint32_t sai1_tx_half_words(void);

/*
 * WHICH DMAMUX CHANNELS HAD TO BE TAKEN AWAY FROM SOMEBODY ELSE, as a
 * bitmask of channel numbers - 0..7 are DMA1's streams, 8..15 are
 * DMA2's.
 *
 * Not a curiosity. On this radio it reads 0x2200 on the first arm:
 * channels 9 and 13, which are DMA2 streams 1 and 5, which are the
 * FACTORY FIRMWARE's own SAI transport, still enabled and still pointed
 * at its buffers in D2 SRAM because nothing about loading an image over
 * SWD stops a DMA controller. Sharing a request line costs exactly half
 * the stream, silently - see the long comment in sai1.c.
 *
 * A zero here on a board that has just run the factory firmware is more
 * suspicious than a non-zero one.
 */
uint32_t sai1_mux_released(void);

/*
 * Transfer errors, FIFO errors and direct-mode errors on either stream,
 * and whether a stream has stopped itself. Zero is the only acceptable
 * value: a transfer error on this part means a bus address the DMA
 * cannot reach, which is a bug in where the buffer was linked, not a
 * passing condition.
 */
uint32_t sai1_dma_errors(void);

/*
 * The flags themselves, sticky: bits 0..5 are the receive stream's
 * group as the hardware lays it out - FEIF bit 0, DMEIF bit 2, TEIF
 * bit 3 - and bits 8..13 the transmit stream's. Three different faults
 * share one counter and they need completely different work, so the
 * counter says how often and this says which.
 */
uint32_t sai1_dma_err_bits(void);

/*
 * The raw hardware, for the report that is printed after every arm.
 *
 * This exists because an arm can fail SILENTLY: a DMA stream whose
 * event flags are not clear accepts the write to EN and then has it
 * cleared again by hardware, so the stream does nothing, raises nothing
 * and the ring simply stays as it was left - which reads as a receiver
 * delivering digital silence. Nothing in a derived number distinguishes
 * that from a muted codec. Four registers do.
 */
/*
 * How many extra writes SAIEN needed before the hardware admitted to it.
 *
 * Zero would mean the readback loop in sai1_dma_start() is decoration.
 * It is not: without it, two arms in four came up with block B's SAIEN
 * clear and the receiver silently dead. See the comment at that loop.
 */
uint32_t sai1_en_retries(void);

/*
 * How many times an enable had to be done AGAIN because SAIEN did not
 * hold. Different fact from the retries above and they must be read
 * together: retries count writes the hardware would not accept at all,
 * relatches count writes it accepted and then threw away. The second
 * one is the real failure, and before it was looked for it presented as
 * a receiver delivering digital silence.
 */
uint32_t sai1_en_relatch(void);

/*
 * WHAT THE HARDWARE LOOKED LIKE 100 us AFTER THE LAST ARM.
 *
 * Taken inside sai1_dma_start(), before anything else in the program
 * runs, because that is the only window in which the question can be
 * answered: SAIEN on block B reads back as SET and is clear by the time
 * any caller looks at it. Nineteen frames in is late enough for the
 * first frame synchronisation and early enough that nothing else has
 * touched the peripheral.
 */
typedef struct {
    uint32_t cr1_a, cr1_b;      /* SAIEN is bit 16, DMAEN bit 17 */
    uint32_t sr_a,  sr_b;       /* OVRUDR 0, AFSDET 4, CNRDY 5, LFSDET 6 */
    uint32_t rx_cr, tx_cr;      /* EN is bit 0 */
    uint32_t rx_ndtr, tx_ndtr;
} sai1_arm_t;

const sai1_arm_t *sai1_arm_snapshot(void);

uint32_t sai1_rx_cr(void);
uint32_t sai1_tx_cr(void);
uint32_t sai1_rx_ndtr(void);
uint32_t sai1_tx_ndtr(void);
uint32_t sai1_dma_lisr(void);
int      sai1_dma_streams_live(void);

/*
 * Where the receive stream is in its ring, in half-words, and how long
 * the ring is. Together with a clock they are the only honest way to ask
 * the question "is the transport moving two words a frame, or one?" -
 * which is not a theoretical question: it was wrong once, and the
 * symptom was a spectrum at twice the right offset with an image almost
 * as strong as the carrier, because consecutive samples of ONE channel
 * read as an I/Q pair are a real signal sampled at half the rate.
 */
uint32_t sai1_rx_pos(void);
uint32_t sai1_rx_ring_words(void);

/* How often a polled call had to stop the DMA transport. See
 * sai1_stream(). Anything but zero after the tests is a caller bug. */
uint32_t sai1_mode_switches(void);

/* The receive block was overtaken by the writer mid-copy. At 42.7 ms a
 * half against a 30 us copy this should never move. */
uint32_t sai1_rx_torn(void);

/* A caller passed a length that was not the one armed. Should be zero;
 * it is the length contract above reporting that it caught something. */
uint32_t sai1_len_mismatches(void);

enum {
    SAI1_OK              =  0,
    SAI1_ERR_NO_CLOCK    = -1,  /* no PLL2 configuration for that rate */
    SAI1_ERR_PLL2        = -2,  /* PLL2 never locked                   */
    SAI1_ERR_NOT_READY   = -3,  /* sai1_init() not called              */
    SAI1_ERR_DMA_SIZE    = -4,  /* half size zero or over the maximum  */
    SAI1_ERR_SAIEN       = -5   /* SAIEN would not take - see block_enable */
};

#endif /* MALAHIT_SAI1_H */
