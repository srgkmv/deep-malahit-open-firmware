/*
 * THE 480 x 320 LAYOUT, IN ONE PLACE.
 *
 * Force-included ahead of upstream's UI headers, whose layout constants
 * are #ifndef-guarded for exactly this. The same file is used by the
 * firmware build and by the deepsdr-ui host render, so the geometry on
 * the radio and the geometry in the PNG cannot drift apart - which is
 * the whole reason to lay this out on a PC first.
 *
 * ---------------------------------------------------------------
 *
 * THE VERTICAL BUDGET IS THE WHOLE PROBLEM. Width is nearly free:
 * ui_top.c, spec_chrome.c and ui_act.c turned out to be parameterised
 * on GFX2_W already - the only numbers 800 and 480 appear in, in all
 * three files, are comments. PORTING.md section 3.1 called the layout
 * "hand-fitted, not parameterised", and for main.c that is true; for
 * the gfx2-era UI modules it is no longer.
 *
 * Height is not free. 480 rows become 320, so a third of the screen
 * has to come out of somewhere, and the fonts do not shrink with it.
 *
 *            upstream 800x480        here 480x320
 *   header         0..63  (64)          0..25  (26)
 *   status        64..103 (40)          26..41  (16)
 *   spectrum     104..343 (240)        42..191 (150)
 *     trace      108..315 (208)        46..173 (128)
 *     ruler      318..343  (26)       176..191  (16)
 *   waterfall    348..419  (72)       194..289  (96)
 *   actions      427..476  (50)       294..319  (26)
 *
 *   26 + 16 + 150 + 2 + 96 + 4 + 26 = 320 exactly
 *
 * EVERYTHING HALVED, AND THE TRACE AND WATERFALL GOT THE PROCEEDS.
 *
 * The first fit kept upstream's font sizes and spent the whole budget
 * on chrome: a 44-row header, a 28-row status band and a 48-row action
 * bar left 92 rows of trace and 72 of waterfall. On a 320-row panel
 * that is more than a third of the screen spent on labels.
 *
 * Halving the chrome - and the type in it - buys 36 rows for the trace
 * and 24 for the waterfall, both of which are the reason the radio has
 * a screen at all. The trace now has MORE rows than the first fit gave
 * it on a panel a third shorter than upstream's.
 *
 * The cost is legibility, deliberately accepted to find out where the
 * limit is. The faces are remapped in reglas.mk and in the deepsdr-ui
 * target, not here, because that is a -D and not a layout constant.
 */
#ifndef MALAHIT_LAYOUT_H
#define MALAHIT_LAYOUT_H

#define GFX2_W              480
#define GFX2_H              320
#define GFX_SCREEN_WIDTH    480
#define GFX_SCREEN_HEIGHT   320

#ifndef UI_TOP_H
#define UI_TOP_H             34
#endif
#ifndef UI_STATUS_Y
#define UI_STATUS_Y          34
#endif
#ifndef UI_STATUS_H
#define UI_STATUS_H          16
#endif

#ifndef SPC_Y
#define SPC_Y                50
#endif
#ifndef SPC_H
#define SPC_H               142
#endif
#ifndef SPC_RULER_H
#define SPC_RULER_H          16
#endif

/*
 * The gutter narrows from 40 to 34. It carries the dB labels in the
 * 11 px face, which needs about 28 px for "-120" plus a tick, so 34 is
 * the smallest that still reads. Upstream's comment on this is worth
 * keeping in mind: losing trace width to the gutter is what makes a
 * spectrum something you can measure with rather than only look at.
 */
#define SPC_GUT_W            34

/*
 * TRACE WIDTH MUST BE DIVISIBLE BY FOUR, and upstream says why: the
 * low-IF demodulation point falls at exactly +Fs/4, so SPC_TRACE_W/4
 * has to be a whole number of pixels or the marker sits off the signal
 * it marks. 480 - 34 of gutter - 6 of right margin = 440, and 440/4 is
 * 110 exactly.
 */
#define SPC_TRACE_W         440

#ifndef SPC_WF_PANEL_Y
#define SPC_WF_PANEL_Y      192
#endif
#ifndef SPC_WF_Y
#define SPC_WF_Y            194
#endif
#ifndef SPC_WF_ROWS
#define SPC_WF_ROWS          92
#endif

/*
 * The action bar, and the arithmetic upstream wrote out as
 * "2 + 6*126 + 5*8 + 2 = 800 exacto". The same identity has to hold at
 * 480 or the last button is clipped:
 *
 *     2 + 6*76 + 5*4 + 2 = 480 exacto
 *
 * Six buttons kept rather than four. Narrower is worse to hit with a
 * finger, but dropping two means deciding which two actions the radio
 * loses, and that is a decision for whoever uses it rather than for
 * whoever is fitting the pixels.
 */
#ifndef UI_ACT_Y
#define UI_ACT_Y            290
#endif
#ifndef UI_ACT_H
#define UI_ACT_H             30
#endif
#define UI_ACT_N              6
#define UI_ACT_MARGIN         2
#define UI_ACT_GAP            4
#define UI_ACT_BTN_W         76

/*
 * INSIDE THE HEADER AND THE STATUS BAND.
 *
 * This is the part the first attempt got wrong, and the render showed
 * it plainly: shrinking the TYPE without moving the CONTAINERS left the
 * badges, pills and buttons drawn at their old positions and heights,
 * so a 26-row header clipped elements laid out for 64.
 *
 * It also corrects something said too confidently earlier. The number
 * 800 really does not appear in ui_top.c - but ST_CHIP_R is 794,
 * UI_TOP_HORA_RX is 716 and UI_TOP_MEM_X is 640, which are the same
 * hand-fitting expressed as absolute coordinates. Of 27 positional
 * constants, 7 assume the width. PORTING.md section 3.1 was right
 * about the header after all; what is genuinely parameterised is the
 * panel widths, not the element positions inside them.
 *
 * X positions that are RIGHT-ANCHORED keep their distance from the
 * right edge rather than being scaled, because that is what they mean:
 * 800-716 = 84 becomes 480-84 = 396. Scaling them by 0.6 instead would
 * move the clock away from the corner it belongs in.
 */

/* --- header internals: 64 rows become 26 --- */
#define UI_TOP_RDS_Y          4
#define UI_TOP_RDS_H         18
#define UI_TOP_RDS_X2       300     /* 620 -> same 180 px from the right */
#define UI_TOP_SAM_X        302
#define UI_TOP_SAM_Y         14
#define UI_TOP_SAM_W         54
#define UI_TOP_SAM_RX       300
#define UI_TOP_SAM_RY        14
#define UI_TOP_SAM_RW        56
#define UI_TOP_SAM_RH        10
#define UI_TOP_MEM_X        352
#define UI_TOP_MEM_Y          8
#define UI_TOP_MEM_W         44
#define UI_TOP_MEM_H         18
#define UI_TOP_HORA_RX      396     /* 716 -> same 84 px from the right */
#define UI_TOP_HORA_RY        0

/* --- status band internals: 40 rows become 16.
 *     The bar and readout widths live further down, with the rest of
 *     the type-driven numbers. --- */
#define ST_KNOB_W           150
#define ST_CHIP_R           474     /* 794 -> same 6 px from the right */
#define ST_CHIP_GAP           5
#define ST_MOVIL_Y1           0
#define ST_MOVIL_Y2          16

/*
 * SIZED FOR THE TYPE, NOT SCALED FROM UPSTREAM.
 *
 * The derived defaults in ui_top.c and ui_act.c scale every offset by
 * the band height, which is right when the TYPE scales with it. Here it
 * does not: the faces stop at font_ui_11, so a chip still needs room
 * for an 11 px face whatever the band does. Scaling gave chips half the
 * height they need, with the text sitting low inside them.
 *
 * So these are computed from the face instead. font_ui_11 has
 * ascent 11, descent 3, LINE HEIGHT 14 - that 14 is the number that
 * matters, because gfx2_text() lays out a line box and not a glyph.
 * A chip of height H centres its text at (H - 14) / 2.
 *
 * It is also why the action bar grew from 26 rows to 30: two lines of
 * 14 are 28, and 26 could not hold them. They were not badly centred,
 * they did not fit, and four rows came out of the waterfall to pay for
 * it.
 */

/*
 * THE FREQUENCY READOUT, AND WHY THE HEADER IS 40 ROWS.
 *
 * font_num_20 at 21 px made the primary readout of a receiver look
 * like a label. font_num_44 at 46 px does not fit a 320-row panel
 * alongside anything else. There was nothing in between, so
 * font_num_38 was generated with upstream's own tools/gen_assets.py -
 * a NEW file in this port's directory, with no upstream font touched.
 *
 * Its line box is 45 rows, which a 26-row header plainly cannot hold.
 * What makes 40 enough is that THE FACE IS DIGITS ONLY: its 9 rows of
 * descent are empty, so the glyphs live in the ascent and sit at rows
 * 8..36 when the line is placed at y = 0.
 *
 * The 14 rows came out of the trace, which drops from 128 to 114.
 * That is the trade asked for, and it is the right way round: the
 * tuned frequency is the one thing on this screen that gets read at a
 * glance from across a room.
 */
#define HDR_FREQ_Y            0
#define HDR_UND_Y            31
#define HDR_UND_H             2

/* --- header chips: AM, band. 18 tall, text centred: (18-14)/2 = 2 --- */
#define HDR_CHIP_Y            8
#define HDR_CHIP_H           18
#define HDR_CHIP_TXT_Y       10     /* font_ui_11: (18-14)/2 = 2 */
#define HDR_UNIT_Y           18
#define HDR_CLK_Y            10
#define HDR_MEM_TXT_PAD       2     /* MEM draws in font_ui_18b, which this
                                     * port maps to font_ui_11: line box 14
                                     * in a chip of 18, so (18-14)/2 = 2.
                                     * It was 4 here, computed against the
                                     * 8 px face the STATUS chips use - the
                                     * wrong font for this one label. */
#define HDR_CHIP_GAP         34     /* 6 de aire + el ancho de "kHz" + 8 */
#define HDR_BAND_GAP          6
#define HDR_CLK_W            60
#define HDR_CLK_X    (GFX2_W - 64)

/* --- status chips: NCO, ATT, BW, AGC and the knob pill.
 *     16 tall in a 16-row band, text centred: (16-14)/2 = 1 --- */
#define STB_CHIP_Y            0
#define STB_CHIP_H           16
#define STB_CHIP_PAD          3     /* font_ui_8: (16-10)/2 = 3 */
#define STB_TXT_Y             3

/* Horizontal air halved. With the 8 px face the old 9/16/6 left the
 * chips looking stretched: far more space beside the text than above
 * and below it. All three have to move together or chip_w() and chip()
 * stop agreeing about how wide a chip is. */
#define STB_CHIP_PADX         4
#define STB_CHIP_W_PAD        8
#define STB_CHIP_VGAP         3
#define STB_DOT_Y             4
#define STB_DOT_D             8

/* --- S-meter, a third narrower: the row has four chips to fit --- */
#define STB_METER_Y           4
#define STB_METER_H           8
#define ST_BAR_X              6
#define ST_BAR_W             48
#define ST_READ_X            58
#define ST_READ_W            60

/* --- ruler: ticks halved, so the numbers can come up and sit closer.
 *     A 16-row ruler cannot spare 7 px for a tick AND 14 for a line. --- */
#define SPC_TICK_H            2
#define SPC_TICK_DEMOD_H      3
#define SPC_RULER_TXT_Y       4

/* --- action bar: two 14-row lines in 30, centred --- */
#define ACT_NAME_Y            1
#define ACT_VALUE_Y          15
#define ACT_SOLO_Y            8

#endif /* MALAHIT_LAYOUT_H */
