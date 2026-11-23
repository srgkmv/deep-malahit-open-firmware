#!/usr/bin/env python3
"""
Test signals for the decoders, sent by a HackRF into the radio - 22/11/2026.

    tools/hackrf_gen.py cw --text "CQ DE R1TEST K" --wpm 20 --snr 12 --out f.iq
    tools/hackrf_gen.py rtty --text "CQ DE R1TEST K" --baud 50 --shift 450 --out r.iq
    tools/hackrf_gen.py psk31 --text "CQ CQ de R1TEST k" --out p.iq
    tools/hackrf_gen.py navtex --snr 10 --out n.iq
    tools/hackrf_gen.py dsc --snr 10 --out d.iq
    tools/hackrf_gen.py wefax --snr 20 --lines 200 --out w.iq
    tools/hackrf_gen.py sstv --sstv-mode scottie1 --snr 20 --out s.iq
    tools/hackrf_gen.py hell --snr 15 --out h.iq
    tools/hackrf_gen.py ale --call R1TEST --snr 10 --out a.iq
    tools/hackrf_gen.py carrier --seconds 10 --out c.iq
    hackrf_transfer -t f.iq -f <carrier Hz - 200000> -s 2000000 -a 0 -x 47

THE BENCH (PORTING.md step 42): HackRF -> 10 dB -> 40 dB -> the radio's
antenna input, by cable, never an antenna. The HackRF's amplifier stays
OFF (-a 0); the level is the TX gain, -x 0..47 in 1 dB steps, which with
the 50 dB of pads is about -97..-50 dBm at the radio. `lsusb` shows
1d50:6089 when it is there; hackrf_info says more.

THE SIGNAL sits at +offset (200 kHz) from the HackRF's centre, so the
HackRF's own LO leakage, at the centre, is far outside the 192 kHz the
radio sees. So tune the HackRF to (wanted carrier - 200 kHz). It is built
at 48 kHz complex, the carrier at 0 Hz, then interpolated to the
HackRF's rate and mixed up, and scaled to the 8-bit DAC's full range -
the level is set with -x and the pads, never by a quieter file.

NOISE, --snr: complex white noise, the SNR defined as upstream's host
benches define it - the carrier's power against the noise in 500 Hz -
and limited to +/- noise-bw/2 around the carrier. What lies outside the
radio's audio passband reaches no decoder, and leaving it in would spend
the DAC's 8 bits on noise and push the carrier towards the radio's own
floor. The line printed says where the carrier ended up in the DAC; at
-x 47 a clean file is ~31 dB over this radio's floor at ATT 59, so keep
the carrier more than ~10 dB above it or the radio's noise adds to yours.

--audio writes instead what the radio's USB branch should hand the
decoder - the carrier at +tone Hz, 300-4000 Hz kept, the real part, f32
at 12 kHz - which upstream's sim/cwaire and friends take: the same
signal through the same decoder on the host, to tell the radio's chain
from the decoder.

The two references differ: this HackRF and this radio by ~9 ppm at
14 MHz, a 700 Hz CW pitch heard at 575-640 Hz. The decoders follow it;
a test that must not depend on it can measure it first with `carrier`.

THE TABLES ARE THIS FILE'S OWN, written from the ITU recommendations by
hand - deliberately NOT read from the decoders, so an error in a
decoder's table cannot pass its own test.
"""
import argparse
import numpy as np
from scipy.signal import resample_poly

FS_LO = 48000      # the baseband's rate, complex

# ITU-R M.1677
MORSE = {
    'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.',
    'G': '--.', 'H': '....', 'I': '..', 'J': '.---', 'K': '-.-', 'L': '.-..',
    'M': '--', 'N': '-.', 'O': '---', 'P': '.--.', 'Q': '--.-', 'R': '.-.',
    'S': '...', 'T': '-', 'U': '..-', 'V': '...-', 'W': '.--', 'X': '-..-',
    'Y': '-.--', 'Z': '--..',
    '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-',
    '5': '.....', '6': '-....', '7': '--...', '8': '---..', '9': '----.',
    '/': '-..-.', '?': '..--..', '.': '.-.-.-', ',': '--..--', '=': '-...-',
}

# ITU-T S.2 (ITA2), the code's value with the FIRST bit sent as bit 0.
# Only what ITA2 and the US teletype agree on: letters, digits and
# - ? : ( ) . , / - the rest differ between the two (' = ; and the bell).
ITA2_LTRS = {
    'E': 0x01, 'A': 0x03, 'S': 0x05, 'I': 0x06, 'U': 0x07, 'D': 0x09,
    'R': 0x0A, 'J': 0x0B, 'N': 0x0C, 'F': 0x0D, 'C': 0x0E, 'K': 0x0F,
    'T': 0x10, 'Z': 0x11, 'L': 0x12, 'W': 0x13, 'H': 0x14, 'Y': 0x15,
    'P': 0x16, 'Q': 0x17, 'O': 0x18, 'B': 0x19, 'G': 0x1A, 'M': 0x1C,
    'X': 0x1D, 'V': 0x1E,
}
ITA2_FIGS = {
    '3': 0x01, '-': 0x03, '8': 0x06, '7': 0x07, '4': 0x0A, ',': 0x0C,
    ':': 0x0E, '(': 0x0F, '5': 0x10, ')': 0x12, '2': 0x13, '6': 0x15,
    '0': 0x16, '1': 0x17, '9': 0x18, '?': 0x19, '.': 0x1C, '/': 0x1D,
}
ITA2_LF, ITA2_SPACE, ITA2_CR, ITA2_FIGS_SHIFT, ITA2_LTRS_SHIFT = 0x02, 0x04, 0x08, 0x1B, 0x1F

# G3PLX's Varicode for PSK31, the printable part and CR/LF, written out
# here by hand - checked against upstream's two copies (psk31.c and
# psk31_varicode.txt) only AFTER being written, as a third opinion.
VARICODE = {
    ' ': '1', '\n': '11101', '\r': '11111',
    'a': '1011', 'b': '1011111', 'c': '101111', 'd': '101101', 'e': '11',
    'f': '111101', 'g': '1011011', 'h': '101011', 'i': '1101', 'j': '111101011',
    'k': '10111111', 'l': '11011', 'm': '111011', 'n': '1111', 'o': '111',
    'p': '111111', 'q': '110111111', 'r': '10101', 's': '10111', 't': '101',
    'u': '110111', 'v': '1111011', 'w': '1101011', 'x': '11011111',
    'y': '1011101', 'z': '111010101',
    'A': '1111101', 'B': '11101011', 'C': '10101101', 'D': '10110101',
    'E': '1110111', 'F': '11011011', 'G': '11111101', 'H': '101010101',
    'I': '1111111', 'J': '111111101', 'K': '101111101', 'L': '11010111',
    'M': '10111011', 'N': '11011101', 'O': '10101011', 'P': '11010101',
    'Q': '111011101', 'R': '10101111', 'S': '1101111', 'T': '1101101',
    'U': '101010111', 'V': '110110101', 'W': '101011101', 'X': '101110101',
    'Y': '101111011', 'Z': '1010101101',
    '0': '10110111', '1': '10111101', '2': '11101101', '3': '11111111',
    '4': '101110111', '5': '101011011', '6': '101101011', '7': '110101101',
    '8': '110101011', '9': '110110111',
    '.': '1010111', ',': '1110101', '?': '1010101111', '/': '110101111',
    '-': '110101', ':': '11110101', '=': '1010101',
}

# CCIR 476 (ITU-R M.476/M.625), SITOR's 7-unit code: every character four B
# and three Y, written here in the order SENT, B1 first, B the HIGHER radio
# frequency. The three control signals are the recommendation's (alpha
# BBBBYYY, beta BBYYBBY, signal repetition YBBYYBB) and fix the bit order;
# the letters and figures could not be written from the recommendation
# independently in this session - they were checked against upstream's
# navtex.c table, which upstream proved on the air (an Oostende recording,
# 02/10/2026). The independent test of them is a real station on 518/490
# kHz. Only what NAVTEX traffic uses: letters, digits, - ? : ( ) . , / '
CCIR_LTRS = {
    'A': 'BBBYYYB', 'B': 'YBYYBBB', 'C': 'BYBBBYY', 'D': 'BBYYBYB',
    'E': 'YBBYBYB', 'F': 'BBYBBYY', 'G': 'BYBYBBY', 'H': 'BYYBYBB',
    'I': 'BYBBYYB', 'J': 'BBBYBYY', 'K': 'YBBBBYY', 'L': 'BYBYYBB',
    'M': 'BYYBBBY', 'N': 'BYYBBYB', 'O': 'BYYYBBB', 'P': 'BYBBYBY',
    'Q': 'YBBBYBY', 'R': 'BYBYBYB', 'S': 'BBYBYYB', 'T': 'YYBYBBB',
    'U': 'YBBBYYB', 'V': 'YYBBBBY', 'W': 'BBBYYBY', 'X': 'YBYBBBY',
    'Y': 'BBYBYBY', 'Z': 'BBYYYBB',
}
CCIR_FIGS = {   # the same codes as the letters above them, in figures
    '-': 'BBBYYYB', '?': 'YBYYBBB', ':': 'BYBBBYY', '8': 'BYBBYYB',
    "'": 'BBBYBYY', '(': 'YBBBBYY', ')': 'BYBYYBB', '.': 'BYYBBBY',
    ',': 'BYYBBYB', '9': 'BYYYBBB', '0': 'BYBBYBY', '1': 'YBBBYBY',
    '4': 'BYBYBYB', '3': 'YBBYBYB', '5': 'YYBYBBB', '7': 'YBBBYYB',
    '2': 'BBBYYBY', '/': 'YBYBBBY', '6': 'BBYBYBY',
}
CCIR_SPACE, CCIR_CR, CCIR_LF = 'YYBBBYB', 'YYYBBBB', 'YYBBYBB'
CCIR_LTRS_SHIFT, CCIR_FIGS_SHIFT = 'YBYBBYB', 'YBBYBBY'
CCIR_ALPHA, CCIR_BETA, CCIR_RQ = 'BBBBYYY', 'BBYYBBY', 'YBBYYBB'


def raised_edges(key, rise_ms):
    """Raised-cosine edges: a hard key makes clicks across the band."""
    n = max(1, int(FS_LO * rise_ms / 1000.0))
    w = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    k = np.convolve(key, w / w.sum(), mode='same')
    return np.clip(k, 0.0, 1.0)


def cw_baseband(a):
    """Keyed carrier at 0 Hz. PARIS timing: a dot is 1.2/wpm s."""
    dot = 1.2 / a.wpm
    units = []                                  # (key, dots)
    for wi, word in enumerate(a.text.upper().split()):
        if wi:
            units.append((0, 7))
        for ci, ch in enumerate(word):
            if ci:
                units.append((0, 3))
            for ei, el in enumerate(MORSE[ch]):
                if ei:
                    units.append((0, 1))
                units.append((1, 1 if el == '.' else 3))
    key = np.concatenate([np.full(int(round(n * dot * FS_LO)), on, dtype=np.float64)
                          for on, n in units])
    gap = np.zeros(int(a.pause * FS_LO))
    key = np.concatenate([gap] + [np.concatenate([key, gap])] * a.repeat)
    return raised_edges(key, 5.0).astype(np.complex128), repr(a.text)


def ita2_codes(text):
    """The code stream: LTRS first, a shift when the case changes, \n as
    CR LF - and after a space or a line end the next character's shift
    again, whichever it is: decoders that "unshift on space" are back in
    letters there and the rest are not, and an extra shift is right for
    both."""
    out, figs = [ITA2_LTRS_SHIFT, ITA2_LTRS_SHIFT], False
    for ch in text.upper():
        if ch == '\n':
            out += [ITA2_CR, ITA2_LF]
            figs = None
        elif ch == ' ':
            out.append(ITA2_SPACE)
            figs = None                         # unknown: say it again
        elif ch in ITA2_LTRS:
            if figs is not False:
                out.append(ITA2_LTRS_SHIFT)
                figs = False
            out.append(ITA2_LTRS[ch])
        elif ch in ITA2_FIGS:
            if figs is not True:
                out.append(ITA2_FIGS_SHIFT)
                figs = True
            out.append(ITA2_FIGS[ch])
        else:
            raise SystemExit(f'rtty: {ch!r} is not in this file\'s ITA2 table')
    return out


def rtty_baseband(a):
    """Continuous-phase FSK round 0 Hz: mark +shift/2, space -shift/2 -
    mark the HIGHER radio frequency, the usual convention; --reverse
    swaps them. Each character: a start bit (space), five bits first-
    bit-first, a stop of `stop` bits (mark). Mark between characters
    and around the message, as a teletype idles."""
    bits = []                                   # (level, length in bits)
    for c in ita2_codes(a.text):
        bits.append((0, 1.0))
        bits += [((c >> k) & 1, 1.0) for k in range(5)]
        bits.append((1, a.stop))
    t_bit = FS_LO / a.baud
    lead = int(a.pause * FS_LO)
    level = []
    for lvl, nb in bits:
        level.append(np.full(int(round(nb * t_bit)), lvl, dtype=np.float64))
    one = np.concatenate(level)
    idle = np.ones(lead)
    lvl = np.concatenate([idle] + [np.concatenate([one, idle])] * a.repeat)
    if a.reverse:
        lvl = 1.0 - lvl
    f = (lvl - 0.5) * a.shift                   # +shift/2 mark, -shift/2 space
    ph = 2 * np.pi * np.cumsum(f) / FS_LO
    x = np.exp(1j * ph)
    # silence before and after, keyed in and out softly
    key = np.concatenate([np.zeros(int(0.5 * FS_LO)), np.ones(len(x)),
                          np.zeros(int(0.5 * FS_LO))])
    x = np.concatenate([np.zeros(int(0.5 * FS_LO)), x, np.zeros(int(0.5 * FS_LO))])
    return x * raised_edges(key, 5.0), (f'{a.text!r}, {a.baud} Bd, {a.shift:.0f} Hz '
                                        f'{"reversed" if a.reverse else "normal"}')


def psk31_baseband(a):
    """BPSK31: 31.25 symbols a second; a 0 is a phase reversal, a 1 none.
    Each character's Varicode, then 00. A preamble of reversals (idle) and
    a postamble of unmodulated carrier, as the programs send. Each symbol
    a cos^2 pulse two symbols long, so the envelope stays flat between
    equal symbols and goes through zero across a reversal - G3PLX's
    shaping, 31 Hz wide."""
    bits = '0' * int(a.preamble * 31.25)
    for ch in a.text:
        if ch not in VARICODE:
            raise SystemExit(f'psk31: {ch!r} is not in this file\'s Varicode table')
        bits += VARICODE[ch] + '00'
    bits += '1' * int(a.postamble * 31.25)
    ph, p = [], 1.0
    for b in bits:
        if b == '0':
            p = -p
        ph.append(p)
    T = FS_LO / 31.25                            # 1536 samples
    n = int(len(ph) * T)
    x = np.zeros(n + int(2 * T))
    t = np.arange(int(2 * T)) - T                # the pulse, -T..T
    pulse = np.cos(np.pi * t / (2 * T)) ** 2
    for k, v in enumerate(ph):
        i0 = int(k * T)
        x[i0:i0 + len(pulse)] += v * pulse
    x = x[int(T):]                               # the first pulse's lead-in
    gap = np.zeros(int(a.pause * FS_LO))
    x = np.concatenate([gap] + [np.concatenate([x, gap])] * a.repeat)
    return x.astype(np.complex128), f'{a.text!r}, BPSK31'


def ccir_codes(text):
    """The text as CCIR 476 codes: LETTERS first, a shift where the case
    changes, \\n as CR LF - and after a line end the shift again whatever
    it is, since NAVTEX receivers (upstream's too) go back to letters
    there."""
    out, figs = [CCIR_LTRS_SHIFT], False
    for ch in text.upper():
        if ch == '\n':
            out += [CCIR_CR, CCIR_LF]
            figs = None
        elif ch == ' ':
            out.append(CCIR_SPACE)
        elif ch in CCIR_LTRS:
            if figs is not False:
                out.append(CCIR_LTRS_SHIFT)
                figs = False
            out.append(CCIR_LTRS[ch])
        elif ch in CCIR_FIGS:
            if figs is not True:
                out.append(CCIR_FIGS_SHIFT)
                figs = True
            out.append(CCIR_FIGS[ch])
        else:
            raise SystemExit(f'navtex: {ch!r} is not in this file\'s CCIR 476 table')
    return out


def navtex_baseband(a):
    """SITOR-B (ITU-R M.476 FEC, M.625 mode B), as NAVTEX sends it (ITU-R
    M.540): 100 Bd, continuous-phase FSK +-85 Hz round 0 Hz, B the higher
    frequency (--reverse swaps them). Character positions of 70 ms; each
    character twice - the first copy (DX) at an even position, the
    repeat (RX) five positions (350 ms) later, at an odd one. Before the
    text, `phasing` seconds of alpha in DX and signal repetition in RX;
    after it, alpha in DX (end of emission) while the RX copies run out.
    No silence inside: the station never stops keying."""
    codes = ccir_codes(a.text)
    nph = int(round(a.phasing / 0.07)) & ~1        # even: DX on the evens
    total = nph + 2 * len(codes) + 5 + 6
    pos = [CCIR_ALPHA if (i & 1) == 0 else CCIR_RQ for i in range(total)]
    for i, c in enumerate(codes):
        pos[nph + 2 * i] = c
        pos[nph + 2 * i + 5] = c
    for i in range(nph + 2 * len(codes), total, 2):
        pos[i] = CCIR_ALPHA                         # end of emission, DX
    for c in pos:
        assert len(c) == 7 and c.count('B') == 4, c
    one = np.array([1.0 if b == 'B' else 0.0 for c in pos for b in c])
    if a.reverse:
        one = 1.0 - one
    t_bit = FS_LO / 100.0                           # 480 samples
    lvl = np.repeat(one, int(t_bit))
    lvl = np.concatenate([np.concatenate([lvl, np.full(int(a.pause * FS_LO), -1.0)])
                          for _ in range(a.repeat)])
    key = (lvl >= 0.0).astype(np.float64)
    f = (np.clip(lvl, 0.0, 1.0) - 0.5) * 170.0     # +85 B, -85 Y
    ph = 2 * np.pi * np.cumsum(f) / FS_LO
    x = np.exp(1j * ph)
    lead = np.zeros(int(a.lead * FS_LO))
    x = np.concatenate([lead, x])
    key = np.concatenate([np.zeros(len(lead)), key])
    return x * raised_edges(key, 5.0), (f'{a.text!r}, SITOR-B, {len(codes)} codes, '
                                        f'{nph * 0.07:.1f} s phasing, '
                                        f'{"reversed" if a.reverse else "normal"}')


def dsc_symbol_bits(sym):
    """ITU-R M.493 section 2: ten bits a character - the seven of the
    value, least significant FIRST, then three that are the COUNT of B
    (0) elements among those seven, most significant first. Y = 1, B = 0."""
    info = [(sym >> k) & 1 for k in range(7)]
    zeros = info.count(0)
    return info + [(zeros >> 2) & 1, (zeros >> 1) & 1, zeros & 1]


def dsc_mmsi(mmsi):
    """Section 5: nine digits and a trailing 0, two digits a character."""
    d = (mmsi + '0')
    assert len(d) == 10 and d.isdigit(), mmsi
    return [int(d[i:i + 2]) for i in range(0, 10, 2)]


def dsc_call(a):
    """The DX characters of a call, format specifier to the closing EOS.
    Individual routine call (120), or all-ships (116), or a distress
    alert (112, synthetic - the decoder's only test of it, as upstream says)."""
    if a.format == 'individual':
        info = [120, 120] + dsc_mmsi(a.to) + [100] + dsc_mmsi(a.mmsi) \
             + [100, 126] + [126] * 6
        eos = 117
    elif a.format == 'allships':
        info = [116, 116] + [108] + dsc_mmsi(a.mmsi) + [100, 126] + [126] * 6
        eos = 127
    else:   # distress: self-ID, nature, position (4 x 126 unknown here), time, telecommand
        info = [112, 112] + dsc_mmsi(a.mmsi) + [105] + [99, 99, 99, 99, 99] \
             + [88, 88] + [100]
        eos = 127
    # Section 10: the ECC is the even parity of the seven information bits
    # of every information character, the format specifier counted ONCE
    # and the EOS once.
    ecc = 0
    for c in info[1:] + [eos]:
        ecc ^= c & 0x7F
    return info + [eos, ecc, eos, eos], ecc


def dsc_baseband(a):
    """HF DSC (ITU-R M.493): 100 Bd FSK +-85 Hz round 0 Hz, B (0) the
    HIGHER frequency, as F1B always is. A dot pattern (B and Y alternating),
    then DX and RX alternating: DX 125 six times while RX counts 111 down to
    104, then the call in DX; each RX position carrying the DX character of
    two pairs before (five positions, 500 ms). Silence before and after."""
    dx, ecc = dsc_call(a)
    dx_all = [125] * 6 + dx
    npairs = len(dx_all) + 2
    pos = []
    for n in range(npairs):
        d = dx_all[n] if n < len(dx_all) else 126   # nothing after: the RX run out
        if n < 8:
            r = 111 - n
        else:
            r = dx_all[n - 2]
        pos += [d, r]
    pos = pos[:2 * len(dx_all) + 4]                 # the last RX copies, then stop
    bits = [(k & 1) for k in range(a.dots)]         # B Y B Y ... (0 1 0 1)
    for sym in pos:
        bits += dsc_symbol_bits(sym)
    one = np.array(bits, dtype=np.float64)
    if a.reverse:
        one = 1.0 - one
    lvl = np.repeat(one, int(FS_LO / 100.0))
    gap = np.full(int(a.pause * FS_LO), -1.0)
    lvl = np.concatenate([np.full(int(a.lead * FS_LO), -1.0)]
                         + [np.concatenate([lvl, gap])] * a.repeat)
    key = (lvl >= 0.0).astype(np.float64)
    f = (0.5 - np.clip(lvl, 0.0, 1.0)) * 170.0     # 0 (B) +85, 1 (Y) -85
    x = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
    return x * raised_edges(key, 5.0), (f'DSC {a.format}, {len(dx)} DX characters, '
                                        f'ECC {ecc}, {a.dots} dots, '
                                        f'{"reversed" if a.reverse else "normal"}')


def wefax_chart(lines, width=1809):
    """A test chart, 0 black .. 1 white: a white page with a black border,
    a 100-pixel grid, circles, a grey wedge and the word R1TEST in big
    letters - shapes that show slant, scaling and the grey scale."""
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new('L', (width, lines), 255)
    d = ImageDraw.Draw(im)
    for x in range(0, width, 100):
        d.line([(x, 0), (x, lines)], fill=160, width=2)
    for y in range(0, lines, 100):
        d.line([(0, y), (width, y)], fill=160, width=2)
    d.rectangle([4, 4, width - 5, lines - 5], outline=0, width=8)
    for r in (60, 140, 220):
        d.ellipse([450 - r, lines // 2 - r, 450 + r, lines // 2 + r], outline=0, width=10)
    for k in range(10):                        # the grey wedge, 10 steps
        d.rectangle([900 + 80 * k, 40, 980 + 80 * k, 160], fill=int(255 * k / 9))
    try:
        f = ImageFont.load_default(size=180)
    except TypeError:
        f = ImageFont.load_default()
    d.text((900, lines // 2 - 60), 'R1TEST', fill=0, font=f)
    return np.asarray(im, dtype=np.float64) / 255.0


def wefax_baseband(a):
    """Radiofax (WMO / ITU-R, IOC 576): FM round 0 Hz, black -400 Hz and
    white +400 Hz (1500 and 2300 Hz in the audio of a dial 1.9 kHz under),
    at --lpm lines a minute, 1809 pixels a line. In order: the start signal
    (black and white alternating at 300 Hz, 5 s), phasing (--phasing s of
    black lines with a white pulse the first 5 % of each), the chart, the
    stop signal (450 Hz, 5 s) and 10 s of black."""
    t_line = 60.0 / a.lpm
    spl = int(round(t_line * FS_LO))
    W = 1809
    chart = wefax_chart(a.lines, W)
    pieces = [np.full(int(a.lead * FS_LO), np.nan)]
    n_apt = int(5.0 * FS_LO)
    t = np.arange(n_apt) / FS_LO
    pieces.append((np.sign(np.sin(2 * np.pi * 300.0 * t)) > 0).astype(np.float64))
    pulse = np.zeros(spl)
    pulse[:int(0.05 * spl)] = 1.0
    pieces.append(np.tile(pulse, int(round(a.phasing / t_line))))
    idx = (np.arange(spl) * W) // spl
    for row in chart:
        pieces.append(row[idx])
    t = np.arange(n_apt) / FS_LO
    pieces.append((np.sign(np.sin(2 * np.pi * 450.0 * t)) > 0).astype(np.float64))
    pieces.append(np.zeros(int(10.0 * FS_LO)))
    pieces.append(np.full(int(1.0 * FS_LO), np.nan))
    g = np.concatenate(pieces)
    key = (~np.isnan(g)).astype(np.float64)
    g = np.nan_to_num(g, nan=0.5)
    f = (g - 0.5) * 800.0
    if a.ppm:
        # the transmitter's line rate a little off: resample in time
        n = len(f)
        src = np.arange(n) * (1.0 + a.ppm * 1e-6)
        f = np.interp(src, np.arange(n), f)
    x = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
    return x * raised_edges(key, 5.0), (f'WEFAX {a.lpm} lpm, {a.lines} lines, '
                                        f'{a.phasing:.0f} s phasing, {a.ppm:+.0f} ppm')


def sstv_picture():
    """A 320 x 256 test picture: the eight colour bars, a grey ramp, a
    circle and R1TEST - colours, sharpness and geometry at a glance."""
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new('RGB', (320, 256), (0, 0, 0))
    d = ImageDraw.Draw(im)
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    for k, c in enumerate(bars):
        d.rectangle([k * 40, 0, k * 40 + 39, 99], fill=c)
    for x in range(320):
        g = int(255 * x / 319)
        d.line([(x, 100), (x, 129)], fill=(g, g, g))
    d.ellipse([20, 140, 130, 250], outline=(255, 255, 255), width=4)
    try:
        f = ImageFont.load_default(size=64)
    except TypeError:
        f = ImageFont.load_default()
    d.text((140, 160), 'R1TEST', fill=(255, 128, 0), font=f)
    return np.asarray(im, dtype=np.float64)


# The line plans, from the mode specifications (J. L. Barber, "Proposal for
# SSTV mode specifications", Dayton 2000): (Hz, ms) pieces, a colour scan
# written as ('G'|'B'|'R', ms).
SSTV_MODES = {
    'martin1': dict(vis=44, lines=256, line=[(1200, 4.862), (1500, 0.572),
                    ('G', 146.432), (1500, 0.572), ('B', 146.432), (1500, 0.572),
                    ('R', 146.432), (1500, 0.572)], start=[]),
    'scottie1': dict(vis=60, lines=256, line=[(1500, 1.5), ('G', 138.240),
                     (1500, 1.5), ('B', 138.240), (1200, 9.0), (1500, 1.5),
                     ('R', 138.240)], start=[(1200, 9.0)]),
}


def sstv_baseband(a):
    """SSTV as a tone whose frequency is the signal (1500 black, 2300
    white, 1200 sync), round 0 Hz = 1900 Hz of audio: the VIS header
    (leader 1900 300 ms, break 1200 10 ms, leader 300 ms, start bit 1200
    30 ms, seven bits least significant first - 1100 a 1, 1300 a 0 - even
    parity, stop bit 1200 30 ms), then the picture in the mode's plan."""
    m = SSTV_MODES[a.sstv_mode]
    pic = sstv_picture()
    seq = [(1900, 300.0), (1200, 10.0), (1900, 300.0), (1200, 30.0)]
    bits = [(m['vis'] >> k) & 1 for k in range(7)]
    seq += [(1100 if b else 1300, 30.0) for b in bits]
    seq += [(1100 if (sum(bits) & 1) else 1300, 30.0), (1200, 30.0)]
    f = [np.full(int(a.lead * FS_LO), np.nan)]
    for hz, ms in seq + m['start']:
        f.append(np.full(int(round(ms * FS_LO / 1000.0)), float(hz)))
    ch = {'R': 0, 'G': 1, 'B': 2}
    for y in range(m['lines']):
        for piece in m['line']:
            if isinstance(piece[0], str):
                n = int(round(piece[1] * FS_LO / 1000.0))
                px = (np.arange(n) * 320) // n
                f.append(1500.0 + 800.0 * pic[y, px, ch[piece[0]]] / 255.0)
            else:
                f.append(np.full(int(round(piece[1] * FS_LO / 1000.0)), float(piece[0])))
    f.append(np.full(int(a.pause * FS_LO), np.nan))
    f = np.concatenate(f)
    key = (~np.isnan(f)).astype(np.float64)
    f = np.nan_to_num(f, nan=1900.0) - 1900.0
    x = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
    return x * raised_edges(key, 2.0), (f'SSTV {a.sstv_mode} (VIS {m["vis"]}), '
                                        f'{len(f) / FS_LO:.0f} s')


# A 5 x 7 font for the test text, rows top to bottom - this file's own.
HELL_FONT = {
    'C': ['01110', '10001', '10000', '10000', '10000', '10001', '01110'],
    'Q': ['01110', '10001', '10001', '10001', '10101', '10010', '01101'],
    'D': ['11110', '10001', '10001', '10001', '10001', '10001', '11110'],
    'E': ['11111', '10000', '10000', '11110', '10000', '10000', '11111'],
    'R': ['11110', '10001', '10001', '11110', '10100', '10010', '10001'],
    'T': ['11111', '00100', '00100', '00100', '00100', '00100', '00100'],
    'S': ['01111', '10000', '10000', '01110', '00001', '00001', '11110'],
    'K': ['10001', '10010', '10100', '11000', '10100', '10010', '10001'],
    '1': ['00100', '01100', '00100', '00100', '00100', '00100', '01110'],
    ' ': ['00000'] * 7,
}


def hell_baseband(a):
    """Feld-Hell: the carrier keyed on and off, 245 points a second
    (122.5 Bd, a symbol a pair of points), a character 7 columns of 14
    points, left to right, each column scanned BOTTOM TO TOP. The first and
    last columns blank; the 5 x 7 glyph with each row doubled fills the 14.
    Edges a 2 ms raised cosine, as Hell transmitters shape them."""
    pts = []
    for ch in a.text.upper():
        if ch not in HELL_FONT:
            raise SystemExit(f'hell: {ch!r} is not in this file\'s font')
        g = HELL_FONT[ch]
        cols = [[0] * 14]
        for c in range(5):
            col = []
            for r in range(7):
                col += [int(g[r][c])] * 2      # rows top to bottom, doubled
            cols.append(col)
        cols.append([0] * 14)
        for col in cols:
            pts += col[::-1]                   # sent bottom to top
    pts = pts * a.repeat
    spp = FS_LO / 245.0
    n = int(len(pts) * spp)
    key = np.array(pts, dtype=np.float64)[(np.arange(n) / spp).astype(int)]
    lead = np.zeros(int(a.lead * FS_LO))
    key = np.concatenate([lead, key, lead])
    return raised_edges(key, 2.0).astype(np.complex128), (f'Feld-Hell {a.text!r} x{a.repeat}, '
                                                          f'{n / FS_LO:.1f} s')


# JTTY (WSJT-X 3.2's teletype mode): the six normative vectors, their 46
# data symbols as WSJT-X's own tbcc.f90 computes them (copied from
# deepsdr-ui/sim/jtty_oro.h, which is generated from the Fortran, not
# written by hand), and the 13-symbol sync. Each is a whole message (bit 34,
# the end flag, set) - the text is what a decoder must print.
JTTY_SYNC = [0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0]
JTTY_VECTORS = [
    ('CQ K1ABC CQ', '2032033023223100122300300333333201300310213223'),
    ('HELLO', '2323311103331220002310303011201011120221221220'),
    ('FN42', '3222230101133023330102222130322000010311110211'),
    ('123 EOM', '1123211212000002133212232230130322213331132103'),
    ('AGN?', '1212313320000000000000000002322000220003033020'),
    ('CA', '2030210012000211022113220112101121012121102112'),
]


def jtty_baseband(a):
    """JTTY: 4-GFSK (BT 1.0) at 31.25 Bd, tones 31.25 Hz apart round 0 Hz
    (-1.5 .. +1.5 spacings), a frame the 13 sync symbols and the 46 data
    symbols - 59 symbols, 1.888 s. Each of --messages (indices into the
    six vectors) sent as one frame, --gap s apart."""
    sps = int(FS_LO / 31.25)                       # 1536
    tones = []
    gaps = []
    for k in a.messages:
        tones.append(JTTY_SYNC + [int(c) for c in JTTY_VECTORS[k][1]])
    n_frame = 59 * sps
    t = (np.arange(-2.5 * sps, 2.5 * sps) + 0.5) / sps
    from scipy.special import erf
    bt = 1.0
    c = 2 * np.pi * bt / np.sqrt(np.log(2))
    pulse = 0.5 * (erf(c * (t + 0.5) / np.sqrt(2)) - erf(c * (t - 0.5) / np.sqrt(2)))
    out = [np.full(int(a.lead * FS_LO), np.nan)]
    for tn in tones:
        f = np.zeros(n_frame + len(pulse))
        for i, s_ in enumerate(tn):
            f[i * sps:i * sps + len(pulse)] += (s_ - 1.5) * 31.25 * pulse
        f = f[int(2.5 * sps):int(2.5 * sps) + n_frame]
        out += [f + a.offset_hz, np.full(int(a.gap * FS_LO), np.nan)]
    f = np.concatenate(out)
    key = (~np.isnan(f)).astype(np.float64)
    f = np.nan_to_num(f, nan=0.0)
    x = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
    texts = ' | '.join(JTTY_VECTORS[k][0] for k in a.messages)
    return x * raised_edges(key, 5.0), f'JTTY {texts}, {a.offset_hz:+.0f} Hz'


# ALE 2G, MIL-STD-188-141 appendix A (= FED-STD-1045A), written from the
# standard's own description, not from upstream's ale_*.c:
#   - 8-FSK, 125 Bd, tones 750..2500 Hz every 250, each symbol three bits,
#     the first bit sent the tribit's most significant;
#   - the tone of each tribit, the standard's table (a Gray code - the
#     tones in order are 000 001 011 010 110 111 101 100);
#   - a word is 24 bits: the preamble (3 bits) and three 7-bit characters,
#     the most significant bit of each first;
#   - each half of 12 bits through the extended Golay (24,12): the 12 data
#     bits, then the 11 check bits of the cyclic (23,12) code with
#     g(x) = x^11 + x^9 + x^7 + x^6 + x^5 + x + 1, then the overall parity;
#     the second half's 12 check bits INVERTED;
#   - the two codewords interleaved A1 B1 A2 B2 .. A24 B24, a stuff bit 0
#     after them (49 bits), and those 49 bits sent three times over - 147
#     bits, 49 symbols, 392 ms; the copies do not start on a symbol.
# What the Golay's convention could share with upstream's by mistake (the
# place of the overall parity, the order of the check bits) only a real
# station can settle: the soundings on HF are everywhere.
ALE_TONE_OF_TRIBIT = {0b000: 750, 0b001: 1000, 0b011: 1250, 0b010: 1500,
                      0b110: 1750, 0b111: 2000, 0b101: 2250, 0b100: 2500}
ALE_PRE = {'DATA': 0, 'THRU': 1, 'TO': 2, 'TWAS': 3, 'FROM': 4, 'TIS': 5,
           'CMD': 6, 'REP': 7}
ALE_GOLAY_G = 0b101011100011           # x^11+x^9+x^7+x^6+x^5+x+1


def ale_golay24(d12):
    """The extended Golay codeword of 12 data bits: data, 11 check bits,
    overall parity - 24 bits, the data in the top 12."""
    r = d12 << 11
    for i in range(22, 10, -1):         # the remainder of d(x) x^11 mod g(x)
        if r & (1 << i):
            r ^= ALE_GOLAY_G << (i - 11)
    c23 = (d12 << 11) | r
    return (c23 << 1) | (bin(c23).count('1') & 1)


def ale_word_tribits(pre, chars):
    """One ALE word, the preamble's name and three characters, to its 49
    tribits on the air."""
    assert len(chars) == 3
    w = ALE_PRE[pre] << 21
    for i, c in enumerate(chars):
        w |= (ord(c) & 0x7F) << (14 - 7 * i)
    A = ale_golay24(w >> 12)
    B = ale_golay24(w & 0xFFF) ^ 0xFFF
    bits = []
    for k in range(24):
        bits += [(A >> (23 - k)) & 1, (B >> (23 - k)) & 1]
    bits.append(0)                      # the stuff bit
    bits = bits * 3
    return [(bits[3 * i] << 2) | (bits[3 * i + 1] << 1) | bits[3 * i + 2]
            for i in range(49)]


def ale_address_words(first, addr):
    """An address after its opening preamble: three characters a word, '@'
    filling the last, the words after the first alternating DATA and REP."""
    addr = addr.upper()
    if len(addr) > 15:
        raise SystemExit('ale: an address is 15 characters at most')
    addr += '@' * (-len(addr) % 3)
    out = []
    for i in range(0, len(addr), 3):
        pre = first if i == 0 else ('DATA' if (i // 3) % 2 == 1 else 'REP')
        out.append((pre, addr[i:i + 3]))
    return out


def ale_baseband(a):
    """ALE soundings: each the station's address under TIS (or TWAS with
    --twas), --words times over without a gap (a scanning sound), and with
    --to a call: the called address under TO first. --sounds of them,
    --pause s apart. The tones round 0 Hz, 1625 Hz (the middle of 750..2500)
    being --tone, plus --offset-hz of mistuning."""
    seq = []
    if a.to:
        seq += ale_address_words('TO', a.to)
    seq += ale_address_words('TWAS' if a.twas else 'TIS', a.call)
    words = seq * a.words
    sps = int(FS_LO / 125)              # 384
    f = [np.full(int(a.lead * FS_LO), np.nan)]
    for _ in range(a.sounds):
        tr = []
        for pre, ch in words:
            tr += ale_word_tribits(pre, ch)
        tones = np.array([ALE_TONE_OF_TRIBIT[t] for t in tr], dtype=np.float64)
        f += [np.repeat(tones - 1625.0 + a.offset_hz, sps),
              np.full(int(a.pause * FS_LO), np.nan)]
    f = np.concatenate(f)
    key = (~np.isnan(f)).astype(np.float64)
    f = np.nan_to_num(f, nan=0.0)
    x = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
    txt = ' '.join(f'{p} {c}' for p, c in seq)
    return x * raised_edges(key, 2.0), (f'ALE [{txt}] x{a.words} words, {a.sounds} sounds, '
                                        f'{len(words) * 0.392:.1f} s each, '
                                        f'{a.offset_hz:+.0f} Hz')


def carrier_baseband(a):
    return np.ones(int(a.seconds * FS_LO), dtype=np.complex128), 'carrier'


def add_noise(x, a):
    """Complex white noise, `snr` dB below the carrier in 500 Hz, only
    within +/- noise_bw/2 of it. The carrier's power is 1."""
    if a.snr is None:
        return x
    rng = np.random.default_rng(a.seed)
    sigma2 = (1.0 / 10 ** (a.snr / 10.0)) * FS_LO / 500.0
    w = np.sqrt(sigma2 / 2) * (rng.standard_normal(len(x))
                               + 1j * rng.standard_normal(len(x)))
    W = np.fft.fft(w)
    f = np.fft.fftfreq(len(w), 1.0 / FS_LO)
    W[np.abs(f) > a.noise_bw / 2] = 0
    return x + np.fft.ifft(W)


def write_audio(x, a):
    """What the radio's sideband branch hands its decoders: f32, 12 kHz,
    the carrier (or the FSK's centre) at `tone`, mirrored in LSB."""
    n = np.arange(len(x))
    if a.lsb:
        x = np.conj(x)                          # LSB hears the spectrum mirrored
    z = x * np.exp(2j * np.pi * a.tone * n / FS_LO)
    Z = np.fft.fft(z)
    f = np.fft.fftfreq(len(z), 1.0 / FS_LO)
    Z[(f < 300) | (f > 4000)] = 0
    r = resample_poly(np.real(np.fft.ifft(Z)), 1, 4).astype(np.float32)
    r.tofile(a.out)
    print(f'{a.out}: {len(r) / 12000:.1f} s of audio at 12 kHz, '
          f'carrier at {a.tone:.0f} Hz, snr {a.snr}')


def write_iq(x, a, what):
    """int8 I/Q interleaved, for hackrf_transfer -t."""
    y = resample_poly(x, a.rate // 1000, FS_LO // 1000)
    n = np.arange(len(y))
    y = y * np.exp(2j * np.pi * a.offset * n / a.rate)
    peak = np.max(np.abs(np.concatenate([y.real, y.imag])))
    y = y * (110.0 / peak)                      # full scale, a little headroom
    car = 110.0 / peak                          # where the carrier ended up, LSB
    iq = np.empty(2 * len(y), dtype=np.int8)
    iq[0::2] = np.clip(np.round(y.real), -127, 127)
    iq[1::2] = np.clip(np.round(y.imag), -127, 127)
    iq.tofile(a.out)
    print(f'{a.out}: {len(y) / a.rate:.1f} s at {a.rate} Hz, {what}, '
          f'offset {a.offset:.0f} Hz, snr {a.snr}, carrier {car:.1f} LSB '
          f'({20 * np.log10(car / 110.0):+.1f} dB vs a clean file)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='mode', required=True)

    def common(p):
        p.add_argument('--out', required=True)
        p.add_argument('--offset', type=float, default=200e3,
                       help='Hz above the HackRF centre (default 200 kHz)')
        p.add_argument('--rate', type=int, default=2_000_000)
        p.add_argument('--snr', type=float, default=None,
                       help='dB, the carrier against the noise in 500 Hz; '
                            'omitted = no noise')
        p.add_argument('--noise-bw', type=float, default=8000.0,
                       help='Hz of noise around the carrier (default 8000)')
        p.add_argument('--seed', type=int, default=1)
        p.add_argument('--audio', action='store_true',
                       help='write the 12 kHz f32 audio of the USB branch, not I/Q')
        p.add_argument('--tone', type=float, default=700.0,
                       help='the carrier\'s (FSK: the centre\'s) audio frequency for --audio')
        p.add_argument('--lsb', action='store_true',
                       help='--audio as the LSB branch hears it (mirrored)')
        p.add_argument('--fm', type=float, default=0.0, metavar='DEV_HZ',
                       help='send the AUDIO (the signal at --tone) as narrow FM with '
                            'this peak deviation, for the radio\'s NFM modes; 0 = SSB')

    p = sub.add_parser('carrier', help='a steady carrier: levels, and the ppm')
    common(p)
    p.add_argument('--seconds', type=float, default=10.0)
    p.set_defaults(build=carrier_baseband)

    p = sub.add_parser('cw', help='Morse')
    common(p)
    p.add_argument('--text', default='VVV CQ CQ DE R1TEST R1TEST K')
    p.add_argument('--wpm', type=float, default=20.0)
    p.add_argument('--pause', type=float, default=2.0, help='s of silence around it')
    p.add_argument('--repeat', type=int, default=1)
    p.set_defaults(build=cw_baseband)

    p = sub.add_parser('rtty', help='radioteletype, ITA2 FSK')
    common(p)
    p.add_argument('--text', default='RYRYRY CQ CQ DE R1TEST R1TEST K')
    p.add_argument('--baud', type=float, default=50.0)
    p.add_argument('--shift', type=float, default=450.0, help='Hz, mark to space')
    p.add_argument('--stop', type=float, default=1.5, help='stop bits')
    p.add_argument('--reverse', action='store_true', help='mark the LOWER frequency')
    p.add_argument('--pause', type=float, default=1.0, help='s of idle mark around it')
    p.add_argument('--repeat', type=int, default=1)
    p.set_defaults(build=rtty_baseband)

    p = sub.add_parser('psk31', help='BPSK31, Varicode')
    common(p)
    p.add_argument('--text', default='CQ CQ de R1TEST R1TEST pse k')
    p.add_argument('--preamble', type=float, default=2.0, help='s of idle reversals')
    p.add_argument('--postamble', type=float, default=1.0, help='s of carrier')
    p.add_argument('--pause', type=float, default=1.0, help='s of silence around it')
    p.add_argument('--repeat', type=int, default=1)
    p.set_defaults(build=psk31_baseband)

    p = sub.add_parser('navtex', help='NAVTEX, SITOR-B FEC')
    common(p)
    p.add_argument('--text', default='ZCZC FA01\nSECURITE\nNAVAREA I 123/26\n'
                   'TEST MESSAGE DE R1TEST. GALE 8 SW, 55-12N 012-30E AT 0942 UTC.\n'
                   'NNNN\n')
    p.add_argument('--phasing', type=float, default=10.0,
                   help='s of phasing before the text (M.540: at least 10)')
    p.add_argument('--reverse', action='store_true', help='B the LOWER frequency')
    p.add_argument('--lead', type=float, default=0.5,
                   help='s of silence (only the noise, with --snr) before it')
    p.add_argument('--pause', type=float, default=3.0,
                   help='s of silence after each emission')
    p.add_argument('--repeat', type=int, default=1)
    p.set_defaults(build=navtex_baseband)

    p = sub.add_parser('dsc', help='HF DSC, ITU-R M.493')
    common(p)
    p.add_argument('--format', choices=['individual', 'allships', 'distress'],
                   default='individual')
    p.add_argument('--mmsi', default='273123450', help='who calls (9 digits)')
    p.add_argument('--to', default='002734417', help='whom (9 digits)')
    p.add_argument('--dots', type=int, default=200, help='bits of dot pattern')
    p.add_argument('--reverse', action='store_true', help='B the LOWER frequency')
    p.add_argument('--lead', type=float, default=1.0, help='s of silence before')
    p.add_argument('--pause', type=float, default=3.0, help='s of silence after each')
    p.add_argument('--repeat', type=int, default=1)
    p.set_defaults(build=dsc_baseband)

    p = sub.add_parser('wefax', help='radiofax, IOC 576')
    common(p)
    p.add_argument('--lpm', type=float, default=120.0)
    p.add_argument('--lines', type=int, default=300, help='lines of chart')
    p.add_argument('--phasing', type=float, default=30.0, help='s of phasing')
    p.add_argument('--ppm', type=float, default=0.0, help='line rate error, ppm')
    p.add_argument('--lead', type=float, default=1.0, help='s of silence before')
    p.set_defaults(build=wefax_baseband, tone=1900.0)

    p = sub.add_parser('sstv', help='slow-scan TV, Martin 1 or Scottie 1')
    common(p)
    p.add_argument('--sstv-mode', choices=sorted(SSTV_MODES), default='martin1')
    p.add_argument('--lead', type=float, default=1.0, help='s of silence before')
    p.add_argument('--pause', type=float, default=2.0, help='s of silence after')
    p.set_defaults(build=sstv_baseband, tone=1900.0)

    p = sub.add_parser('hell', help='Feld-Hell')
    common(p)
    p.add_argument('--text', default='CQ CQ DE R1TEST R1TEST K ')
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--lead', type=float, default=1.0, help='s of silence before and after')
    p.set_defaults(build=hell_baseband, tone=1000.0)

    p = sub.add_parser('jtty', help="WSJT-X's JTTY, the six normative messages")
    common(p)
    p.add_argument('--messages', type=int, nargs='+', default=[0, 1, 2, 3, 4, 5],
                   help='which of the six (0 CQ K1ABC CQ, 1 HELLO, 2 FN42, 3 123 EOM, 4 AGN?, 5 CA)')
    p.add_argument('--gap', type=float, default=3.0, help='s between frames')
    p.add_argument('--offset-hz', type=float, default=0.0, help='Hz off the centre')
    p.add_argument('--lead', type=float, default=1.0)
    p.set_defaults(build=jtty_baseband, tone=1500.0)

    p = sub.add_parser('ale', help='ALE 2G soundings, MIL-STD-188-141')
    common(p)
    p.add_argument('--call', default='R1TEST', help="the station's own address (15 at most)")
    p.add_argument('--to', default='', help='a called address, under TO before it')
    p.add_argument('--twas', action='store_true', help='TWAS instead of TIS')
    p.add_argument('--words', type=int, default=6, help='times the address is sent in a sound')
    p.add_argument('--sounds', type=int, default=3)
    p.add_argument('--pause', type=float, default=5.0, help='s between sounds')
    p.add_argument('--offset-hz', type=float, default=0.0, help='Hz of mistuning')
    p.add_argument('--lead', type=float, default=1.0)
    p.set_defaults(build=ale_baseband, tone=1625.0)

    a = ap.parse_args()
    x, what = a.build(a)
    if a.fm > 0.0:
        # the audio an SSB receiver would hear, as the modulation of an FM
        # carrier at 0 Hz: phase = 2 pi dev * integral(audio / peak)
        n = np.arange(len(x))
        aud = np.real(x * np.exp(2j * np.pi * a.tone * n / FS_LO))
        aud = aud / max(1e-9, np.max(np.abs(aud)))
        x = np.exp(1j * 2 * np.pi * a.fm * np.cumsum(aud) / FS_LO)
        what += f', NFM +-{a.fm:.0f} Hz'
    x = add_noise(x, a)
    if a.audio:
        write_audio(x, a)
    else:
        write_iq(x, a, what)


if __name__ == '__main__':
    main()
