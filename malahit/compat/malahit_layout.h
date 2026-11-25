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
 *   header         0..63  (64)          0..33  (34)
 *   status        64..103 (40)          34..52  (19)
 *   spectrum     104..343 (240)        53..191 (139)
 *     trace      108..315 (208)        57..173 (117)
 *     ruler      318..343  (26)       176..191  (16)
 *   waterfall    348..419  (72)       194..285  (92)
 *   actions      427..476  (50)       290..319  (30)
 *
 *   34 + 19 + 139 + 2 + 92 + 4 + 30 = 320 exactly
 *
 * THE STATUS BAND IS 19 AND NOT 16, and the three rows came off the
 * trace, one at a time, with someone looking at the panel between
 * each. At 16 the chips filled the band exactly - top border hard
 * against the divider, bottom against the spectrum - and there is no
 * amount of moving the TEXT inside a box that fixes a box with no
 * slack in it. Two rows of air above the chips and one below.
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
#define UI_STATUS_H          19
#endif

#ifndef SPC_Y
#define SPC_Y                53
#endif
#ifndef SPC_H
#define SPC_H               139
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
/*
 * THE RDS STRIP - 22/11/2026, room made for it on the owner's word: the
 * clock and the battery stacked in one 32 px column (below), the MEM chip
 * pulled up against it, and the mode and band pills' side air cut from
 * 13/11 px a side to 5 (HDR_MODO_PAD, HDR_BANDA_PAD - upstream's numbers
 * made #ifndef for it). What is left between the band pill and MEM is
 * the strip: upstream's two lines, the station's name over the
 * radiotext, here in font_ui_10b / font_ui_10 (line box 13):
 *
 *     name     rows  2..14      radiotext  rows 18..30
 *
 * inside rows 1..32 - the divider is row 33. The deepsdr-ui render
 * `cab_wfm` measures it at the widest the header gets, 104.300,00 WFM FM.
 */
#define UI_TOP_RDS_Y          1
#define UI_TOP_RDS_H         32
#define UI_TOP_RDS_NOM_DY     1
#define UI_TOP_RDS_MAR_DY    17
#define UI_TOP_RDS_X1_M       6     /* air after the band pill */
#define UI_TOP_RDS_X2   (UI_TOP_MEM_X - 6)
#define UI_TOP_SAM_X        302
#define UI_TOP_SAM_Y         14
#define UI_TOP_SAM_W         54
#define UI_TOP_SAM_RX       300
#define UI_TOP_SAM_RY        14
#define UI_TOP_SAM_RW        56
#define UI_TOP_SAM_RH        10
#define UI_TOP_MEM_X        406     /* 406..439: 5 px of air to the battery at 445 */
#define UI_TOP_MEM_Y          8
#define UI_TOP_MEM_W         34     /* "MEM" in font_ui_11 and 4 px a side */
#define UI_TOP_MEM_H         18
#define UI_TOP_HORA_RX      442     /* the clock and battery column */
#define UI_TOP_HORA_RY        0

/* --- status band internals: 40 rows become 16.
 *     The bar and readout widths live further down, with the rest of
 *     the type-driven numbers. --- */
#define ST_KNOB_W           150
#define ST_CHIP_R           474     /* 794 -> same 6 px from the right */
#define ST_CHIP_GAP           5
#define ST_MOVIL_Y1           0
#define ST_MOVIL_Y2          19

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
 * THE FREQUENCY READOUT, AND WHY THE HEADER IS 34 ROWS.
 *
 * font_num_20 at 21 px made the primary readout of a receiver look
 * like a label. font_num_44 at 46 px does not fit a 320-row panel
 * alongside anything else. There was nothing in between, so
 * font_num_31 was generated with upstream's own tools/gen_assets.py -
 * a NEW file in this port's directory, with no upstream font touched.
 *
 * Its LINE BOX is 37 rows, which a 26-row header plainly cannot hold.
 * What makes 34 enough is that THE FACE IS DIGITS ONLY: ascent 29,
 * descent 8, and those 8 rows are empty, so the glyphs live inside the
 * ascent and the line can be placed at y = 0 without the box below it
 * mattering. The underline of the tuning digit then sits at row 31,
 * still inside the header.
 *
 * The 8 rows came out of the trace, which drops from 128 to 120. That
 * is the trade asked for, and it is the right way round: the tuned
 * frequency is the one thing on this screen that gets read at a glance
 * from across a room.
 */
#define HDR_FREQ_Y            0
/*
 * THE FREQUENCY TO THE EDGE, AND "kHz" UP TO THE MODE CHIP - 22/11/2026,
 * the owner's last pixels for the RDS strip. font_num_31's digits carry
 * 3 px of bearing, so a frequency at x = 3 puts its ink at x = 6 - the
 * S-meter bar's own left edge (ST_BAR_X), right under it. "kHz" is 18 px
 * (measured, the cab_* render prints it); 6 px after the digits as
 * before, and 4 px of air to the mode chip instead of 10:
 * HDR_CHIP_GAP = 6 + 18 + 4. The frequency's touch zone ends where the
 * chip begins (HDR_FREQ_HIT_M), where upstream's 12 had it 4 px into it.
 */
#define HDR_FREQ_X            3
#define HDR_UNIT_GAP          6
#define HDR_FREQ_HIT_M        4
#define HDR_UND_Y            31
#define HDR_UND_H             2

/* --- header chips: AM, band. 18 tall, text centred: (18-14)/2 = 2 --- */
#define HDR_CHIP_Y            8
#define HDR_CHIP_H           18
#define HDR_CHIP_TXT_Y       10     /* font_ui_11: (18-14)/2 = 2 */
#define HDR_UNIT_Y           18
#define HDR_CLK_Y             1     /* font_ui_11: line box 1..14 */
#define HDR_MEM_TXT_PAD       2     /* MEM draws in font_ui_18b, which this
                                     * port maps to font_ui_11: line box 14
                                     * in a chip of 18, so (18-14)/2 = 2.
                                     * It was 4 here, computed against the
                                     * 8 px face the STATUS chips use - the
                                     * wrong font for this one label. */
#define HDR_CHIP_GAP         28     /* 6 de aire + el ancho de "kHz" (18) + 4 */
#define HDR_BAND_GAP          4
#define HDR_MODO_PAD         10     /* 5 a side: was upstream's 13 */
#define HDR_BANDA_PAD        10     /* 5 a side: was upstream's 11 */
/*
 * THE CLOCK AND THE BATTERY ARE STACKED AGAIN - 22/11/2026, as upstream
 * has them, the clock over the battery, so the width they took side by
 * side goes to the RDS strip. 34 rows hold two 14-row lines: the clock
 * 1..14, the battery 18..31. What follows is the side-by-side layout of
 * 02/11/2026, kept for the measurements in it.
 *
 * (02/11/2026: THE CLOCK AND THE BATTERY SHARE A ROW. Upstream stacks
 * them in a 64-row header; 34 rows, it was thought then, cannot stack
 * anything, so they went side by side.)
 *
 * AND UNTIL TODAY THE BATTERY WAS SIMPLY NOT THERE. Its x was 790
 * minus its own width, hard-coded for an 800 px panel, so on this one
 * it was drawn at 740 and clipped away in silence - which is why it is
 * missing from every render of this layout made before now. The
 * constants that place it are new upstream and default to the old
 * numbers; see HDR_BAT_* in ui_top.c.
 *
 * The widths are measured, not guessed. "23:41" in the face this port
 * maps font_num_20 to is 29 px, so a 32 px box holds the worst case
 * with room; the battery needs its number inside, "100%" in the 8 px
 * face, which is 22 px plus 4 of border.
 *
 *     battery  409..438  + 3 px of nipple = 441
 *     clock    446..477
 *
 * Five pixels between them and two to the edge. The MEM chip ends at
 * 395, so there are 13 px of air on the other side as well.
 */
#define HDR_CLK_W            32
#define HDR_CLK_X    (GFX2_W - 34)              /* 446..477, right-aligned */
#define HDR_BAT_W            30
#define HDR_BAT_H            14
#define HDR_BAT_X           445                 /* 445..474 + 3 of nipple */
#define HDR_BAT_Y            18
/* 6 and not the derived 5: the box is 14 rows, and only an even nub
 * can sit exactly in the middle of an even box. At 5 it lands one row
 * low, which is visible on a pill this small - it is what the 20-row
 * default gets for free and this one does not. */
#define HDR_BAT_NIB_H         6

/* --- status chips: NCO, ATT, BW, AGC and the knob pill.
 *     16 tall in a 16-row band, text centred: (16-14)/2 = 1 --- */
/*
 * ONE ROW LOWER THAN THE ARITHMETIC SAYS - 03/11/2026, read off the
 * panel.
 *
 * font_ui_10 has a line box of 13 in a chip of 16, so centring gives
 * (16 - 13) / 2 = 1.5, and integer division rounds it DOWN to 1: one
 * row above the line box and two below. Every label in this band is
 * capitals and digits with no descender, so the bottom row of that
 * line box is always empty and the text reads high by a whole pixel.
 *
 * The half pixel has to go somewhere and it goes down, which is the
 * side the eye measures from here. Not visible in the render at any
 * sane zoom; plainly visible on the glass.
 *
 * STB_SU_Y moves the S-units text with them. It is a separate
 * constant precisely so it can: it used to share STB_CHIP_Y with the
 * knob pill, and the pill is exactly as tall as the band and cannot
 * move at all.
 */
#define STB_CHIP_Y            2
#define STB_CHIP_H           16
#define STB_CHIP_PAD          2     /* font_ui_10: (16-13)/2, rounded up */
#define STB_TXT_Y             4
#define STB_SU_Y              3     /* font_ui_11, line box 14 */

/* Horizontal air halved. With the 8 px face the old 9/16/6 left the
 * chips looking stretched: far more space beside the text than above
 * and below it. All three have to move together or chip_w() and chip()
 * stop agreeing about how wide a chip is. */
#define STB_CHIP_PADX         4
#define STB_CHIP_W_PAD        8
#define STB_CHIP_VGAP         3
#define STB_DOT_Y             6
#define STB_DOT_D             8

/* --- S-meter, a third narrower: the row has four chips to fit --- */
#define STB_METER_Y           6
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

/*
 * THE MENU SCREENS - step 9, 21/11/2026: ui_grid.c (the step screen)
 * and ui_cfg.c (modes, bands, settings), upstream's two menu engines.
 *
 * They cover what upstream's cover: the spectrum and the waterfall,
 * between the status band and the action bar - 53..288 here, 104..421
 * there - so the header, the S-meter and the six buttons stay in view.
 *
 * The type is ui_top's set (reglas.mk, MALAHIT_FONTS_TOP): titles in
 * font_ui_11 (line box 14), data in font_ui_10 / 10b (13). Every offset
 * below is that arithmetic, and the comment beside it says which.
 */
#define UIG_Y                53
#define UIG_H               236     /* 53..288, the action bar at 290 */
#define UIG_HDR_H            18
#define UIG_MARGIN            5
#define UIG_GAP               4
#define UIG_CELL_W          114     /* 5 + 4*114 + 3*4 + 5 = 478 */
#define UIG_CELL_H           59
#define UIG_GRID_Y   (UIG_Y + UIG_HDR_H + 3)            /* 74 */
#define UIG_NAV_H            22
#define UIG_NAV_Y    (UIG_Y + UIG_H - UIG_NAV_H - 2)    /* 265 */
#define UIG_CELL_R            6
#define UIG_L3_Y1             5     /* 14 + 13 + 13 = 40 in 59 */
#define UIG_L3_Y2            22
#define UIG_L3_Y3            38
#define UIG_L2_Y1            16     /* 14 + 13 = 27, centred in 59 */
#define UIG_L2_Y2            32
#define UIG_NAV_R             5
#define UIG_NAV_TXT_Y         4     /* (22 - 13) / 2 */
#define UIG_TIT_Y             2     /* (18 - 14) / 2 */
#define UIG_PAG_Y             3

#define UIC_SIDE_X            5
#define UIC_SIDE_W           84     /* "Radiodifusión" in font_ui_11, and air */
#define UIC_CAT_GAP           6
#define UIC_GRID_X  (UIC_SIDE_X + UIC_SIDE_W + 6)       /* 95 */
#define UIC_GRID_Y  (UIG_Y + 4)                         /* 57 */
#define UIC_CELL_W          122     /* 3 x 122 + 2 x 6 = 378 <= 480-95-5 */
#define UIC_CELL_H           72     /* 3 x 72 + 2 x 6 + 8 = 236 */
#define UIC_GAP               6
#define UIC_DCELL_W          92     /* 4 x 92 + 3 x 4 = 380: "18.068-18.168 kHz" is 89 */
#define UIC_DCELL_H          54     /* 4 x 54 + 3 x 4 + 8 = 236 */
#define UIC_DGAP              4
#define UIC_CAT_R             6
#define UIC_CEL_R             6
#define UIC_BAR_X             5
#define UIC_BAR_W             4
#define UIC_BAR_H            22
#define UIC_CAT_TXT_X        12
#define UIC_CAT_TXT_R         6
#define UIC_CAT_TXT_DY        7     /* half of font_ui_11's 14 */
#define UIC_L3_Y1             4     /* 14 + 13 + 13 = 40 in 54 */
#define UIC_L3_Y2            20
#define UIC_L3_Y3            35
#define UIC_NOM_X             8
#define UIC_NOM_Y             8
#define UIC_VAL_DY           26     /* the value's 14 rows end 12 above the bottom */

/*
 * ui_det.c - one continuous setting, "-" and "+" either side of its
 * value: squelch, NR strength, smoothing. Upstream's are the largest
 * targets of its whole UI, 170 x 130; these keep the proportion at
 * 112 x 84 (17 x 13 mm on this glass). The value is font_num_31 - the
 * header's digits, same character set as upstream's font_num_44 -
 * through MALAHIT_FONTS_MENU in reglas.mk.
 *
 *   title  59..72   buttons 79..162   presets 175..210   foot 260..285
 */
#define UID_BTN_W           112
#define UID_BTN_H            84
#define UID_BTN_Y   (UIG_Y + 26)                        /* 79 */
#define UID_BTN_X0            8
#define UID_BTN_X1  (GFX2_W - UID_BTN_W - 8)            /* 360 */
#define UID_BTN_HIT_M         6     /* touch 73..168: the presets start at 171 */
#define UID_BTN_R             8
#define UID_SIGNO_L          36
#define UID_SIGNO_G           8
#define UID_VAL_GAP           6
#define UID_VAL_FILL_DY       6
#define UID_VAL_FILL_H       72
#define UID_VAL_TXT_DY       10     /* font_num_31: line 37, digits in the top 29 */
#define UID_VAL_PIE_DY       60
#define UID_TIT_DY            6
#define UID_PRE_Y   (UID_BTN_Y + UID_BTN_H + 12)        /* 175 */
#define UID_PRE_H            36
#define UID_PRE_GAP           6
#define UID_PIE_H            26
#define UID_PIE_Y   (UIG_Y + UIG_H - UID_PIE_H - 3)     /* 260 */
#define UID_PIE_M             8
#define UID_PIE_R             6
#define UID_PRE_R             6

/*
 * ui_kbd.c - the frequency keypad, opened by a tap on the frequency.
 * Upstream's is a reading strip over a 4 x 4 grid whose COLUMNS are
 * exactly ui_grid's, so nothing jumps sideways between the two screens;
 * here too: 5 + 4 x 114 + 3 x 4 + 5. Same area as the other menus.
 *
 * The strip is the arithmetic of its two faces: title and hint in
 * font_ui_10 (line box 13) stacked on the left, 3..15 and 17..29; the
 * typed number in font_num_31 on the right, whose digits' INK is rows
 * 6..28 of its line - so the line goes at 0 and the digits stand 59..81
 * beside the left block's 58..80, measured on the render. 34 rows, the
 * divider on the last.
 *
 * The keys get the rest: 90 + 4 x 46 + 3 x 4 = 286, three rows of air
 * above (the divider at 86) and three below (the action bar at 290).
 * 114 x 46 is 17 x 7 mm on this glass - upstream's are 192 x 59.
 *
 *   strip 53..86   keys 90..135  140..185  190..235  240..285
 */
#define UIK_Y            UIG_Y                          /* 53 */
#define UIK_H            UIG_H                          /* 236 */
#define UIK_READ_H           34
#define UIK_MARGIN            5
#define UIK_GAP               4
#define UIK_CELL_W          114     /* 5 + 4*114 + 3*4 + 5 = 478, as ui_grid */
#define UIK_CELL_H           46
#define UIK_GRID_Y  (UIK_Y + UIK_READ_H + 3)            /* 90 */
#define UIK_CELL_R            6
#define UIK_DIG_DY            6     /* font_num_31: ink 6..28 of the line, 23 rows: 12 above, 11 below */
#define UIK_ROT_DY           16     /* font_ui_11: (46 - 14) / 2 */
#define UIK_TXT_PAD           4
#define UIK_TIT_DY            3
#define UIK_PISTA_DY         17
#define UIK_LEC_DY            0     /* digits' ink 59..81, beside the title and hint's 58..80 */
#define UIK_UNI_DY           18     /* font_ui_11's baseline on the digits' */
#define UIK_UNI_GAP           6

/*
 * THE RADIO CLOCK - ui_hora.c, step 56 (24/11/2026). Upstream's screen in
 * the menus' place, 53..289, its literals #ifndef in ui_hora.h/.c with
 * upstream's values. Faces: MALAHIT_FONTS_MENU (title font_ui_11, state
 * font_ui_10b, detail font_ui_10, the time font_num_31, all 13-14 px but
 * the time's 37).
 *
 *   title 57   state 73   detail 87   bars 105..119, 133..147
 *   time and date centred in 147..253   buttons 253..285
 *
 * Four buttons of 112: 4 + 4*112 + 3*8 + 4 = 480. A station's label
 * ("DCF77 - 77,5 kHz, Alemania") splits at its " - " into two lines of
 * 13 px, which upstream's boton() does when the small face is too wide.
 */
#define UIH_Y               UIG_Y                       /* 53 */
#define UIH_H               UIG_H                       /* 236 */
#define UIH_W               480
#define UIH_BTN_H            32
#define UIH_BTN_W           112
#define UIH_BTN_AIRE          4
#define UIH_BARRA_X          24
#define UIH_BARRA_H          14
#define UIH_BARRA1_DY        52
#define UIH_BARRA_GAP        14
#define UIH_ROT_W            44
#define UIH_ROT_DY            1
#define UIH_ROT_PAD           6
#define UIH_BARRA_R           4
#define UIH_TIT_DY            4
#define UIH_EST_DY           20
#define UIH_DET_DY           34
#define UIH_BTN_R             6
#define UIH_BTN_TXT           8
#define UIH_BTN_MARGEN        4
#define UIH_HIT_AIRE          4     /* 249..289: the action bar starts at 290 */

/*
 * THE DECODERS' PANEL - ui_digi.c, step 42 (22/11/2026). Upstream's
 * arrangement, the owner's choice: in a text mode the spectrum and the
 * waterfall give their place to the tone scope on top and the text
 * below, the panel's bottom where the waterfall's was (upstream's
 * RTTY_TEXT_PANEL_Y). Upstream's eight rows, each in the menus' faces
 * (font_ui_14b -> font_ui_10b, line 13) with a pixel of air; the header
 * in proportion, 22 for its 30.
 *
 *   scope 53..149 (97 rows)   gap 150..151   header 152..173
 *   rows 174..285 (8 x 14)    - to the waterfall's last row, 285
 *
 * Across the header, upstream's partition of 800 px scaled to 480 and
 * kept EXACT, because ui_digi.c asserts it: the fourth button's ceiling
 * plus the fifth plus the chip, with their gaps, end at the right margin
 * (4 + 64 + 6 + 174 + 6 + 68 + 6 + 56 + 6 + 86 + 4 = 480).
 */
#define UDG_ROWS              8
#define UDG_HDR_H            22
#define UDG_LINE_H           14
#define UDG_PAD_X             4
#define UDG_BTN_X             4
#define UDG_BTN_W            64
#define UDG_BTN_H            18
#define UDG_BTN_DY            2     /* (22 - 18) / 2 */
#define UDG_TXT_DY            2     /* font_ui_10b's 13 in 18 */
#define UDG_BTN2_GAP          6
#define UDG_BTN2_W           86
#define UDG_CHIP_RES         86
#define UDG_BTN5_W           56
#define UDG_CHIP_SIN5_MAX   (UDG_CHIP_RES + UDG_BTN2_GAP + UDG_BTN5_W)
#define UDG_BAR_W           174
#define UDG_BAR_AIRE          6
#define UDG_BTN3_MIN_W       42
#define UDG_BTN4_MIN_W       54
#define UDG_BTN4_MAX_W       68
#define UDG_D2_BTN3_W        64
#define UDG_D2_BTN4_W        68
#define UDG_D2_BTN5_W        56
#define UDG_D2_BAR_MIN      120     /* 196 here; upstream's floor is 200 of its 800 */
#define UDG_COL_RESERVA      36

#define DIGI_SCOPE_Y        SPC_Y                                   /* 53 */
#define DIGI_PANEL_H        (UDG_HDR_H + UDG_ROWS * UDG_LINE_H)     /* 134 */
#define DIGI_PANEL_Y        (SPC_WF_Y + SPC_WF_ROWS - DIGI_PANEL_H) /* 152 */
#define DIGI_SCOPE_GAP        2
#define DIGI_SCOPE_H        (DIGI_PANEL_Y - DIGI_SCOPE_Y - DIGI_SCOPE_GAP)  /* 97 */
/* RTTY's header: the bar cut short so the shift button fits after it, as
 * upstream's 170 of its 300 (digi_sync()) - 100 of 174 here, which leaves
 * that button 68 px (UDG_BTN3_MIN_W 42). */
#define DIGI_RTTY_BARRA_W   100

/* JTTY's table (step 50): Time, Offset, Qual., Message, Country, in 480 px
 * of font_ui_10b - upstream's { 8, 62, 132, 176, 672 } are for 800. The
 * country right-aligned against the edge (a negative x), as upstream does
 * with FT8's last two columns; the message has 120..390. */
#define UDG_COLS_JTTY { 4, 38, 90, 120, -476 }

/* ALE's table (step 51): Callsign, Type, kHz, Qual., N - upstream's
 * { 8, 280, 400, 520, -792 } for 800 px, scaled to 480. Measured in
 * font_ui_10b: fifteen 'W's (the standard's longest address) 150 px in
 * 164, "TWAS" 29 in 72, "180.000" 39 in 72; N right-aligned, "65535" 30
 * in the reserve of 36. */
#define UDG_COLS_ALE { 4, 168, 240, 312, -476 }

/* STANAG's identifier table (step 52) - and IDENT's, which shares it:
 * Period, Signal, Quality. Upstream's { 8, 120, 680 } are for 800 px.
 * Measured in font_ui_10b: "106,67 ms" 50 px in 68, "STANAG 4538 (ALE 3G)"
 * 114 in 268, and the loop instrument's third field, "999 ms polls, 999
 * drawing", 127 in 136. */
#define UDG_COLS_IDENT { 4, 72, 340 }

/* AIS's table (step 55): MMSI, Name, Position, Speed, Course, N - upstream's
 * { 8, 110, 420, 580, 650, -792 } for 800 px reserve the absolute worst
 * name, twenty 'W's; here that is 200 px of font_ui_10b and does not fit
 * beside the rest. Measured: "224123456" 54 px in 54, "43,371N 179,999W"
 * 93 in 94, "102,3" 27 in 28; the name has 130 - "ATLANTIC NAVIGATOR II"
 * 122 fits, twenty 'W's are cut. N right-aligned, the channel in its title. */
#define UDG_COLS_AIS { 4, 62, 196, 294, 326, -476 }

/*
 * THE QTH SCREEN - ui_qth.c, step 57 (25/11/2026). Upstream's six boxes,
 * one per character of the locator, each offering only its own alphabet,
 * in the menus' place 53..289; its literals #ifndef in ui_qth.h/.c with
 * upstream's values. Faces: font_ui_18b kept for the letters, the title,
 * the coordinates and the buttons (reglas.mk: ui_qth.o's own set), its
 * font_ui_14 as font_ui_10 for the line on the right and the hints.
 *
 *   title 57   boxes 93..155   coordinates 173   buttons 249..285
 *
 * Six boxes of 64: 28 + 6*64 + 5*8 + 28 = 480. Four buttons of 110:
 * 8 + 4*110 + 3*8 + 8 = 480.
 */
#define UIQ_Y               UIG_Y                       /* 53 */
#define UIQ_H               UIG_H                       /* 236 */
#define UIQ_W               480
#define UIQ_PAD               8
#define UIQ_CAJA_W           64
#define UIQ_CAJA_H           62
#define UIQ_CAJA_GAP          8
#define UIQ_CAJA_DY          40
#define UIQ_BTN_H            36
#define UIQ_BTN_W           110
#define UIQ_BTN_GAP           8
#define UIQ_BTN_FONDO         4     /* 249..285: the action bar starts at 290 */
#define UIQ_TIT_DY            4
#define UIQ_PIE_DY            8
#define UIQ_LETRA_DY         12
#define UIQ_PISTA_DY         18
#define UIQ_COORD_DY         18
#define UIQ_BTN_TXT_DY        8
#define UIQ_BTN_R             6
#define UIQ_CAJA_R            8
#define UIQ_HIT_CAJA_AIRE    12
#define UIQ_HIT_BTN_AIRE      4

/* FT8's table (step 57): Time, Hz, dB, DT, Message, km, Country - upstream's
 * { 8, 110, 210, 290, 380, -660, -792 } for 800 px, measured in font_ui_10b
 * (deepsdr-ui `make malahit-menus`, digi_ft8()). */
#define UDG_COLS_FT8 { 4, 36, 64, 88, 116, -392, -476 }

#endif /* MALAHIT_LAYOUT_H */
