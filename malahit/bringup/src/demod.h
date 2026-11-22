#ifndef MALAHIT_DEMOD_H
#define MALAHIT_DEMOD_H

#include <stdint.h>

/*
 * UPSTREAM'S DEMODULATOR ON THIS RADIO - step 8, 21/11/2026.
 *
 * ../../User/demod_am.c is the hub of every mode upstream has - AM, SAM,
 * USB, LSB, NFM - and the reason step 7 wrote its own am.c instead was a
 * count: its #include closure is 106 files and 72 126 lines. The
 * closure that LINKS is much smaller, and upstream's own host bench
 * (deepsdr-ui `make demod`) had already shown it: demod_am.c, ten DSP
 * modules, and the decoders it hands audio to switched off. The census
 * on this port's flags, 21/11/2026: four compile errors on one spot (the
 * speaker pin, PB7 there and PE1 here) and 55 undefined symbols, every
 * one of them the same set that bench stubs. src/decoders_off.c is that
 * set for this radio.
 *
 * WHAT THIS FILE DOES is the plumbing between the two block shapes:
 *
 *   this port   one SAI half = 8192 frames at 192 kHz, 42.7 ms, and the
 *               hook writes the transmit half of the same index
 *   upstream    demod_am_process_raw() takes 512 frames at 192 kHz
 *               (rf_doble: halved to 96 kHz inside, doubled again on the
 *               way out), and hands 512 stereo frames to
 *               gd32_i2s_stream_write_half()
 *
 * so demod_block() calls upstream 16 times a half and points the sink
 * at the matching 512 frames of the transmit half each time.
 *
 * am.c STAYS, and is still what boots: the two are A/B'd in one run,
 * the way every speed and sound decision on this port has been made
 * since the panel DMA (WORKING.md). From the display loop, over SWD:
 *
 *   swd-call.sh build/bringup.elf demod_path 1     upstream's demod_am
 *   swd-call.sh build/bringup.elf demod_path 0     this port's am.c
 *   swd-call.sh build/bringup.elf demod_mode 2     0 AM 1 USB 2 LSB
 *                                                  3 NFM 5 SAM
 *   swd-call.sh build/bringup.elf demod_report     what it costs
 *
 * WFM (mode 4) is refused: it is a different hook upstream, at a
 * different block size, and nothing here is wired for it yet.
 */

/* Once, before the hook can be switched: upstream's init and the rates
 * this port runs at. Touches no hook - am.c keeps running. */
void demod_init(void);

/* The receive-block hook for sai1_set_block_hook(). */
void demod_block(const int16_t *rx, int16_t *tx, uint32_t frames);

/* 1 = upstream's demod_am in the block interrupt, 0 = am.c. */
void demod_path(int upstream);
int  demod_path_get(void);

/* upstream's demod_mode_t, as an int so swd-call can pass it. Returns
 * the mode now in force. */
int  demod_mode(int mode);

/* The Morse decoder on (1) or off (0), with no block running through the
 * switch - cw_set_enabled() clears the state cw_process() works on, and
 * the block interrupt preempts the caller. */
void demod_cw(int on);

/* RTTY: 0 off, 1 RTTY-L's polarity (config.h's mark and space), 2
 * RTTY-U's (swapped) - upstream's RTTY_VARIANT_NORMAL / _INVERTED. */
void demod_rtty(int variante);

/* PSK31 on (1) or off (0), started from zero each time. */
void demod_psk31(int on);

/* NAVTEX on (1) or off (0), started from zero each time. */
void demod_navtex(int on);

/* DSC on (1) or off (0), started from zero each time. */
void demod_dsc(int on);

/* WEFAX on (1) or off (0). */
void demod_wefax(int on);

/* SSTV on (1) or off (0), for both of its entries. */
void demod_sstv(int on);

/* Feld-Hell on (1) or off (0), from zero each time. */
void demod_hell(int on);

/* JTTY on (1) or off (0). */
void demod_jtty(int on);

/* The bench's recorder of what PSK31 is fed: 1 arms it (2.5 s, g_graba). */
void demod_graba(int on);

/* Hold the block interrupt off (1) while a decoder is reconfigured from
 * the display loop, and let it run again (0): a block that lands plays
 * silence and is counted, as during a mode change. Microseconds. */
void demod_quieto(int on);

/* The digital mix in hertz: tuned frequency minus the LO, upstream's
 * definition (demod_am.h). The dial sits AM_IF_HZ above the LO, so
 * +12000 - the sign is a prediction from the I/Q order, and this is the
 * switch for the bench to settle it. */
void demod_mix(int hz);

/* Cost and health, on the console. */
void demod_report(void);

#endif /* MALAHIT_DEMOD_H */
