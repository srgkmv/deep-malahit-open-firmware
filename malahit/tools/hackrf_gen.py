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
    tools/hackrf_gen.py audio --in s.f32 --tone 1800 --snr 15 --out s.iq
    tools/hackrf_gen.py ais --out s.iq                       (AIS, 161.975 MHz)
    tools/hackrf_gen.py ax25 --fm 3000 --out a.iq            (APRS, NFM)
    tools/hackrf_gen.py ax25 --hf --lsb --snr 15 --out p.iq  (PKT300, LSB)
    tools/hackrf_gen.py carrier --seconds 10 --out c.iq
    tools/hackrf_gen.py ft8 --slots 4 --out f.iq             (FT8, from the dial)
    tools/hackrf_gen.py wspr --cycles 3 --out w.iq           (WSPR, from the dial)
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


# AX.25 (the AX.25 2.2 specification) over Bell 202 AFSK, written from the
# specifications and not from upstream's ax25.c:
#   - a UI frame: destination, source and path addresses, each six
#     characters shifted left one bit and space-padded, then the SSID byte
#     0b011SSSSx with x = 1 on the last address; control 0x03, PID 0xF0, the
#     information field;
#   - the FCS, CRC-16/X.25 (polynomial 0x1021 reflected, initial 0xFFFF,
#     complemented), sent low byte first; every byte least significant bit
#     first;
#   - bit stuffing: a 0 after five 1s in a row, never inside a flag;
#   - flags 0x7E before (the TXDELAY) and after;
#   - NRZI: a 0 changes the tone, a 1 keeps it - so which tone is which
#     does not matter, and an LSB receiver's mirror does not either;
#   - AFSK with continuous phase: 1200/2200 Hz at 1200 Bd (APRS, Bell 202),
#     or 1600/1800 Hz at 300 Bd with --hf (HF packet, PKT300).
def ax25_crc(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def ax25_address(call, last):
    name, _, ssid = call.upper().partition('-')
    name = (name + '      ')[:6]
    out = [ord(c) << 1 for c in name]
    out.append(0x60 | ((int(ssid or 0) & 15) << 1) | (1 if last else 0))
    return out


def ax25_frame(dest, src, path, info):
    calls = [dest, src] + [p for p in path.split(',') if p]
    body = []
    for i, c in enumerate(calls):
        body += ax25_address(c, i == len(calls) - 1)
    body += [0x03, 0xF0] + list(info.encode('ascii'))
    crc = ax25_crc(body)
    return body + [crc & 0xFF, crc >> 8]


def ax25_bits(frame, flags_before, flags_after):
    flag = [0, 1, 1, 1, 1, 1, 1, 0]            # 0x7E, LSB first
    bits = flag * flags_before
    ones = 0
    for b in frame:
        for k in range(8):
            v = (b >> k) & 1
            bits.append(v)
            ones = ones + 1 if v else 0
            if ones == 5:
                bits.append(0)
                ones = 0
    return bits + flag * flags_after


def ax25_baseband(a):
    """AX.25 UI frames as AFSK: --frames of them, --pause s apart, each
    with --txdelay flags before. The tones round 0 Hz, --tone (their middle)
    being the centre; with --fm the audio goes out as narrow FM (APRS),
    with --lsb as LSB (PKT300)."""
    baud, f0, f1 = (300.0, 1600.0, 1800.0) if a.hf else (1200.0, 1200.0, 2200.0)
    info = a.info
    out = [np.full(int(a.lead * FS_LO), np.nan)]
    sps = FS_LO / baud
    for k in range(a.frames):
        fr = ax25_frame(a.dest, a.src, a.path, info.replace('#', str(k + 1)))
        bits = ax25_bits(fr, a.txdelay, 3)
        tone, seq = 0, []
        for v in bits:
            if v == 0:
                tone ^= 1                       # NRZI
            seq.append(f1 if tone else f0)
        n = int(len(seq) * sps)
        f = np.array(seq)[(np.arange(n) / sps).astype(int)] - a.tone
        out += [f, np.full(int(a.pause * FS_LO), np.nan)]
    f = np.concatenate(out)
    key = (~np.isnan(f)).astype(np.float64)
    f = np.nan_to_num(f, nan=0.0)
    x = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
    return x * raised_edges(key, 2.0), (f'AX.25 {"PKT300" if a.hf else "APRS"} {a.src}>{a.dest} '
                                        f'x{a.frames}: {info!r}')


# AIS (ITU-R M.1371), written from the recommendation and not from
# upstream's ais_*.c:
#   - message 1, the position report, 168 bits: type, repeat, MMSI, status,
#     rate of turn (-128 = none), speed x10, accuracy, longitude and
#     latitude in 1/10000 minute, course x10, heading, second, manoeuvre,
#     spare, RAIM, radio state; and message 5, 424 bits: MMSI, version,
#     IMO, callsign (7) and name (20) in six-bit ASCII ('@'..'_' are 0..31,
#     ' '..'?' 32..63), type, dimensions, EPFD, ETA, draught, destination;
#   - the bits packed most significant first into bytes, and the bytes
#     sent LEAST significant bit first, as HDLC sends them; the FCS is
#     CRC-16/X.25 over those bytes, low byte first;
#   - bit stuffing, NRZI (0 changes, 1 keeps), a 24-bit training sequence
#     of alternating bits, flags 0x7E either side;
#   - GMSK: BT 0.4, 9600 bps, h 0.5, so +-2400 Hz.
AIS_6BIT = '@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !"#$%&\'()*+,-./0123456789:;<=>?'


def ais_bits(fields):
    out = []
    for v, n in fields:
        out += [(int(v) >> (n - 1 - i)) & 1 for i in range(n)]
    return out


def ais_text(t, n):
    t = (t.upper() + '@' * n)[:n]
    return [(AIS_6BIT.index(c), 6) for c in t]


def ais_msg1(mmsi, lat, lon, sog, cog, hdg):
    return ais_bits([(1, 6), (0, 2), (mmsi, 30), (0, 4), (0x80, 8), (round(sog * 10), 10),
                     (1, 1), (round(lon * 600000) & 0xFFFFFFF, 28),
                     (round(lat * 600000) & 0x7FFFFFF, 27), (round(cog * 10), 12),
                     (hdg, 9), (30, 6), (0, 2), (0, 3), (0, 1), (0, 19)])


def ais_msg5(mmsi, call, name, dest):
    f = [(5, 6), (0, 2), (mmsi, 30), (0, 2), (9123456, 30)] + ais_text(call, 7) \
        + ais_text(name, 20) + [(70, 8), (100, 9), (50, 9), (10, 6), (10, 6), (1, 4),
                                (11, 4), (24, 5), (12, 5), (0, 6), (65, 8)] \
        + ais_text(dest, 20) + [(0, 1), (0, 1)]
    return ais_bits(f)


def ais_frame_bits(msg):
    assert len(msg) % 8 == 0
    data = [int(''.join(str(b) for b in msg[i:i + 8]), 2) for i in range(0, len(msg), 8)]
    crc = ax25_crc(data)
    data += [crc & 0xFF, crc >> 8]
    flag = [0, 1, 1, 1, 1, 1, 1, 0]
    bits, ones = [0, 1] * 12 + flag, 0
    for b in data:
        for k in range(8):                      # least significant first
            v = (b >> k) & 1
            bits.append(v)
            ones = ones + 1 if v else 0
            if ones == 5:
                bits.append(0)
                ones = 0
    return bits + flag + [0, 0, 0, 0]


def ais_baseband(a):
    """AIS bursts: message 1 and message 5 of one ship, --repeat times,
    --pause s apart, GMSK at +-2400 Hz round 0 Hz."""
    msgs = [ais_msg1(a.mmsi, a.lat, a.lon, a.sog, a.cog, a.hdg),
            ais_msg5(a.mmsi, a.call, a.name, a.dest)]
    sps = FS_LO // 9600                        # 5
    from scipy.special import erf
    bt = 0.4
    t = (np.arange(-2 * sps, 2 * sps + 1)) / sps
    c = 2 * np.pi * bt / np.sqrt(np.log(2))
    g = 0.5 * (erf(c * (t + 0.5) / np.sqrt(2)) - erf(c * (t - 0.5) / np.sqrt(2)))
    g /= g.sum()
    out = [np.full(int(a.lead * FS_LO), np.nan)]
    for _ in range(a.repeat):
        for m in msgs:
            bits = ais_frame_bits(m)
            lvl, nrz = 1, []
            for v in bits:
                if v == 0:
                    lvl = -lvl                 # NRZI
                nrz.append(lvl)
            x = np.repeat(np.array(nrz, dtype=np.float64), sps)
            f = np.convolve(x, g * sps, mode='same') / sps * 2400.0
            out += [f, np.full(int(a.pause * FS_LO), np.nan)]
    f = np.concatenate(out)
    key = (~np.isnan(f)).astype(np.float64)
    f = np.nan_to_num(f, nan=0.0)
    x = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
    return x * raised_edges(key, 0.5), (f'AIS {a.mmsi} {a.name!r} {a.lat:.4f},{a.lon:.4f} '
                                        f'x{a.repeat}')


def audio_baseband(a):
    """Any 12 kHz f32 audio file - what a sideband receiver should hand its
    decoders, written by a host tool (deepsdr-ui/out/stanaggenera, ...) -
    sent as USB: the file's positive frequencies, --tone of them at the
    centre (0 Hz here, so the dial is the HackRF's centre + 200 kHz -
    --tone). Its power where it is not silent is 1, the carrier's of the
    other modes, so --snr means the same thing."""
    x = np.fromfile(a.input, dtype=np.float32).astype(np.float64)
    x = resample_poly(x, FS_LO // 12000, 1)
    X = np.fft.fft(x)
    f = np.fft.fftfreq(len(x), 1.0 / FS_LO)
    X[f < 0] = 0
    X[f > 0] *= 2
    z = np.fft.ifft(X) * np.exp(-2j * np.pi * a.tone * np.arange(len(x)) / FS_LO)
    vivo = np.abs(z) > 1e-4 * np.max(np.abs(z))
    z = z / np.sqrt(np.mean(np.abs(z[vivo]) ** 2))
    return z, f'audio {a.input}, {len(x) / FS_LO:.1f} s, {a.tone:.0f} Hz at the centre'


def carrier_baseband(a):
    return np.ones(int(a.seconds * FS_LO), dtype=np.complex128), 'carrier'


# FT8 (WSJT-X 2.x, "The FT4 and FT8 Communication Protocols", QEX July/August
# 2020, and WSJT-X's packjt77.f90 / genft8.f90 / gen_ft8wave.f90), written from
# the protocol's description, not from ft8_lib's packer or upstream's
# sim/ft8_gen.h (which uses ft8_lib's ftx_message_encode()):
#   - 77 bits, type i3=1 "standard": c28 call1, r1, c28 call2, r1, R1, g15,
#     i3 - the calls six characters with the digit third (" K1ABC"), after
#     the special tokens and the 22-bit hashes (NTOKENS + MAX22 + n), the
#     grid j1*1800 + j2*100 + j3*10 + j4, a report MAXGRID4 + 35 + dB;
#   - a CRC-14 (poly 0x2757 and the leading 1) over the 77 bits and 5 zeros;
#   - the (174,91) LDPC: the 91 bits then 83 parity bits, each the parity of
#     a row of the GENERATOR matrix and the 91 bits. The matrix is the
#     protocol's (WSJT-X's ldpc_174_91_c_generator.f90); it is read here
#     from Ft8Lib/ft8/constants.c, where the decoder does NOT use it - ft8_lib
#     decodes with the parity CHECKS (kFTX_LDPC_Nm), and every codeword made
#     here is checked against those before it is sent;
#   - 3 bits a symbol, Gray coded (0 1 3 2 5 6 4 7), 58 data symbols, the
#     Costas array 3 1 4 0 6 5 2 at symbols 0, 36 and 72: 79 symbols of
#     0.16 s, tones 6.25 Hz apart, GFSK with BT 2.0, the first and last
#     symbols ramped over an eighth of a symbol;
#   - sent from 0.5 s after the 15 s slot's edge (DT 0 in WSJT-X).
FT8_COSTAS = [3, 1, 4, 0, 6, 5, 2]
FT8_GRAY = [0, 1, 3, 2, 5, 6, 4, 7]
FT8_NTOKENS = 2063592
FT8_MAX22 = 4194304
FT8_MAXGRID4 = 32400
FT8_A1 = ' 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'
FT8_A2 = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'
FT8_A3 = '0123456789'
FT8_A4 = ' ABCDEFGHIJKLMNOPQRSTUVWXYZ'


def ft8_c28(w):
    """A callsign, or one of the tokens, as its 28 bits."""
    if w == 'DE':
        return 0
    if w == 'QRZ':
        return 1
    if w == 'CQ':
        return 2
    c = w
    if len(c) >= 3 and c[2].isdigit():
        pass
    elif len(c) >= 2 and c[1].isdigit():
        c = ' ' + c
    c = (c + '      ')[:6]
    if (len(w) > 6 or not c[2].isdigit() or any(ch not in FT8_A4 for ch in c[3:])
            or c[0] not in FT8_A1 or c[1] not in FT8_A2):
        raise SystemExit(f'ft8: "{w}" is not a standard callsign (the digit second or third, '
                         'at most three letters after it) - this generator packs only those')
    n = FT8_A1.index(c[0])
    n = n * 36 + FT8_A2.index(c[1])
    n = n * 10 + FT8_A3.index(c[2])
    n = n * 27 + FT8_A4.index(c[3])
    n = n * 27 + FT8_A4.index(c[4])
    n = n * 27 + FT8_A4.index(c[5])
    return FT8_NTOKENS + FT8_MAX22 + n


def ft8_pack77(text):
    """A standard message, 'CALL1 CALL2 X' or 'CQ CALL2 GRID', as 77 bits
    (a list of 0/1, the first sent first)."""
    w = text.upper().split()
    if len(w) == 2:
        w.append('')
    if len(w) != 3:
        raise SystemExit(f'ft8: "{text}": three words, the third a grid, a report, RRR, RR73 or 73')
    n28a, n28b = ft8_c28(w[0]), ft8_c28(w[1])
    g, ir = w[2], 0
    if g == '':
        g15 = FT8_MAXGRID4 + 1
    elif g == 'RRR':
        g15 = FT8_MAXGRID4 + 2
    elif g == 'RR73':
        g15 = FT8_MAXGRID4 + 3
    elif g == '73':
        g15 = FT8_MAXGRID4 + 4
    elif len(g) == 4 and 'A' <= g[0] <= 'R' and 'A' <= g[1] <= 'R' and g[2:].isdigit():
        g15 = ((ord(g[0]) - 65) * 18 + (ord(g[1]) - 65)) * 100 + int(g[2:])
    else:
        if g.startswith('R') and len(g) > 1:
            ir, g = 1, g[1:]
        r = int(g)
        if -50 <= r <= -31:
            r += 101
        g15 = FT8_MAXGRID4 + r + 35
    bits = []
    for v, n in ((n28a, 28), (0, 1), (n28b, 28), (0, 1), (ir, 1), (g15, 15), (1, 3)):
        bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]
    return bits


def ft8_crc14(bits):
    """CRC-14 over the bits given, MSB first, init 0 - poly x^14 + 0x2757."""
    r = 0
    for b in bits:
        top = ((r >> 13) & 1) ^ b
        r = (r << 1) & 0x3FFF
        if top:
            r ^= 0x2757
    return [(r >> (13 - i)) & 1 for i in range(14)]


def ft8_tables():
    """The generator rows and the parity checks out of Ft8Lib/ft8/constants.c."""
    import os
    import re
    here = os.path.dirname(os.path.abspath(__file__))
    src = open(os.path.join(here, '..', '..', 'Ft8Lib', 'ft8', 'constants.c')).read()

    def block(name):
        i = src.index(name)
        return src[src.index('{', i):src.index('};', i)]
    gen = []
    for row in re.findall(r'\{([^{}]*)\}', block('kFTX_LDPC_generator')):
        by = [int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]+', row)]
        gen.append([(by[j // 8] >> (7 - j % 8)) & 1 for j in range(91)])
    nm = []
    for row in re.findall(r'\{([^{}]*)\}', block('kFTX_LDPC_Nm')):
        nm.append([int(v) for v in re.findall(r'\d+', row) if int(v) > 0])
    assert len(gen) == 83 and len(nm) == 83, 'constants.c: not the tables expected'
    return gen, nm


def ft8_tones(text, tabs):
    gen, nm = tabs
    a91 = ft8_pack77(text)
    a91 = a91 + ft8_crc14(a91 + [0] * 5)
    cw = a91 + [sum(g * m for g, m in zip(row, a91)) & 1 for row in gen]
    for chk in nm:                       # 1-based bit numbers
        assert sum(cw[k - 1] for k in chk) % 2 == 0, 'ft8: a codeword fails a parity check'
    data = [FT8_GRAY[cw[3 * i] * 4 + cw[3 * i + 1] * 2 + cw[3 * i + 2]] for i in range(58)]
    return FT8_COSTAS + data[:29] + FT8_COSTAS + data[29:] + FT8_COSTAS


def ft8_wave(tones, f0):
    """gen_ft8wave's: the GFSK frequency of 79 symbols at FS_LO, complex,
    tone 0 at f0 Hz, unit amplitude, ramped at both ends."""
    from scipy.special import erf
    sps = int(FS_LO * 0.16)              # 7680
    t = (np.arange(3 * sps) - 1.5 * sps) / sps
    c = np.pi * np.sqrt(2.0 / np.log(2.0))
    pulse = 0.5 * (erf(c * 2.0 * (t + 0.5)) - erf(c * 2.0 * (t - 0.5)))
    n = len(tones)
    df = np.zeros((n + 2) * sps)
    for j, tn in enumerate(tones):
        df[j * sps:j * sps + 3 * sps] += tn * pulse
    df[0:2 * sps] += tones[0] * pulse[sps:3 * sps]
    df[n * sps:(n + 2) * sps] += tones[-1] * pulse[0:2 * sps]
    f = f0 + 6.25 * df[sps:sps + n * sps]
    x = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
    r = sps // 8
    x[:r] *= (1 - np.cos(np.pi * np.arange(r) / r)) / 2
    x[-r:] *= (1 + np.cos(np.pi * np.arange(r) / r)) / 2
    return x


def ft8_slots(a):
    """--slots slots of 15 s, each with every --msg at its --hz, from 0.5 s +
    --dt after the edge; --lead s before the first edge. The dial (USB) is
    at 0 Hz. Noise from 200 to 3800 Hz with the SNR of WSJT-X: the signal
    against the noise in 2500 Hz, one figure per message (--snr)."""
    import datetime as dt
    tabs = ft8_tables()
    msgs = a.msg or ['CQ YL2ABC KO26', 'R1ABC YL2ABC -12', 'YL2ABC OH2XYZ RR73']
    hz = a.hz or [600.0 + 500.0 * i for i in range(len(msgs))]
    snr = a.snr if a.snr else [-5.0]
    if len(hz) < len(msgs) or (len(snr) != 1 and len(snr) < len(msgs)):
        raise SystemExit('ft8: one --hz (and one --snr, or a single one) per --msg')
    n = int((a.lead + 15.0 * a.slots) * FS_LO)
    x = np.zeros(n, dtype=np.complex128)
    for s_ in range(a.slots):
        t0 = int((a.lead + 15.0 * s_ + 0.5 + a.dt) * FS_LO)
        for i, m in enumerate(msgs):
            if a.alternate and (s_ + i) % 2:
                continue
            w = ft8_wave(ft8_tones(m, tabs), hz[i])
            amp = np.sqrt(10 ** ((snr[i] if len(snr) > 1 else snr[0]) / 10.0))
            x[t0:t0 + len(w)] += amp * w[:max(0, n - t0)]
    if not a.clean:
        rng = np.random.default_rng(a.seed)
        sigma2 = FS_LO / 2500.0          # noise power 1 in 2500 Hz
        w = np.sqrt(sigma2 / 2) * (rng.standard_normal(n) + 1j * rng.standard_normal(n))
        W = np.fft.fft(w)
        f = np.fft.fftfreq(n, 1.0 / FS_LO)
        W[(f < 200.0) | (f > 3800.0)] = 0
        x = x + np.fft.ifft(W)
    now = dt.datetime.now(dt.timezone.utc)
    edge = now.replace(microsecond=0) + dt.timedelta(seconds=30)
    edge += dt.timedelta(seconds=(15 - edge.second % 15) % 15)
    what = ', '.join(f'"{m}" {hz[i]:.0f} Hz' for i, m in enumerate(msgs))
    what = f'FT8 {a.slots} slots: {what}; SNR {snr} dB in 2500 Hz'
    return x, edge, what


# WSPR (K1JT; G4JNT's "The WSPR Coding Process", 2009), written from that
# description, not from upstream's wspr_msg.c:
#   - the call as six characters with the digit third (" K1ABC"), 0-9 A-Z
#     and space as 0..36: N = c0; *36 + c1; *10 + c2; then *27 + (c - 10)
#     for the last three (space 26) - 28 bits;
#   - the locator and the power: M1 = (179 - 10*L0 - L2) * 180 + 10*L1 + L3
#     (letters from A, digits), M = M1 * 128 + dBm + 64 - 22 bits;
#   - those 50 bits and 31 zeros through the K=32 rate-1/2 convolutional
#     code, polynomials 0xF2D05351 and 0xE4613C47: 162 bits;
#   - interleaved by bit-reversed 8-bit addresses (those under 162);
#   - each symbol sync + 2 * data, the 162-bit sync vector below - written
#     here from memory and found equal, bit for bit, to upstream's
#     k_wspr_sincro, two sources for one table;
#   - 4-FSK, continuous phase, 12000/8192 Hz apart, 8192/12000 s a symbol
#     (110.6 s), from 1 s after an even minute.
WSPR_SYNC = [
    1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1,
    1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1,
    1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1,
    0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0,
    1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0]
WSPR_DF = 12000.0 / 8192.0
WSPR_TS = 8192.0 / 12000.0


def wspr_symbols(call, grid, dbm):
    c = call.upper()
    if not (len(c) >= 3 and c[2].isdigit()):
        c = ' ' + c
    c = (c + '      ')[:6]

    def v(ch):
        if ch.isdigit():
            return ord(ch) - 48
        if 'A' <= ch <= 'Z':
            return ord(ch) - 55
        return 36
    if len(call) > 6 or not c[2].isdigit() or any(not ('A' <= ch <= 'Z' or ch == ' ') for ch in c[3:]):
        raise SystemExit(f'wspr: "{call}" is not a type-1 callsign (digit 2nd or 3rd, '
                         'letters after it, six at most)')
    n = v(c[0])
    n = n * 36 + v(c[1])
    n = n * 10 + v(c[2])
    for ch in c[3:]:
        n = n * 27 + (v(ch) - 10)
    g = grid.upper()
    m1 = (179 - 10 * (ord(g[0]) - 65) - int(g[2])) * 180 + 10 * (ord(g[1]) - 65) + int(g[3])
    m = m1 * 128 + dbm + 64
    bits = [(n >> (27 - i)) & 1 for i in range(28)] + [(m >> (21 - i)) & 1 for i in range(22)]
    bits += [0] * 31
    reg, conv = 0, []
    for b in bits:
        reg = ((reg << 1) | b) & 0xFFFFFFFF
        conv.append(bin(reg & 0xF2D05351).count('1') & 1)
        conv.append(bin(reg & 0xE4613C47).count('1') & 1)
    inter = [0] * 162
    p = 0
    for i in range(256):
        j = int(f'{i:08b}'[::-1], 2)
        if j < 162:
            inter[j] = conv[p]
            p += 1
    return [WSPR_SYNC[i] + 2 * inter[i] for i in range(162)]


def wspr_cycles(a):
    """--cycles 2-minute cycles, one beacon each (--msg 'CALL GRID dBm',
    repeated, used in turn), from 1 s + --dt after each even minute, its
    centre at --hz of audio over the dial (0 Hz). Noise from 200 to 3800 Hz,
    the SNR of WSJT-X: the beacon against the noise in 2500 Hz."""
    import datetime as dt
    msgs = a.msg or ['YL2ABC KO26 37']
    n = int((a.lead + 120.0 * a.cycles) * FS_LO)
    x = np.zeros(n, dtype=np.complex128)
    sps = WSPR_TS * FS_LO                       # 32768 samples a symbol
    for k in range(a.cycles):
        call, grid, dbm = msgs[k % len(msgs)].split()
        sym = wspr_symbols(call, grid, int(dbm))
        t0 = int((a.lead + 120.0 * k + 1.0 + a.dt) * FS_LO)
        idx = np.arange(int(162 * sps))
        f = a.hz - 1.5 * WSPR_DF + WSPR_DF * np.array(sym)[(idx / sps).astype(int)]
        w = np.exp(1j * 2 * np.pi * np.cumsum(f) / FS_LO)
        snrs = a.snr or [-20.0]
        amp = np.sqrt(10 ** (snrs[k % len(snrs)] / 10.0))
        e = min(n, t0 + len(w))
        x[t0:e] += amp * w[:e - t0]
    if not a.clean:
        rng = np.random.default_rng(a.seed)
        sigma2 = FS_LO / 2500.0
        w = np.sqrt(sigma2 / 2) * (rng.standard_normal(n) + 1j * rng.standard_normal(n))
        W = np.fft.fft(w)
        f = np.fft.fftfreq(n, 1.0 / FS_LO)
        W[(f < 200.0) | (f > 3800.0)] = 0
        x = x + np.fft.ifft(W)
    now = dt.datetime.now(dt.timezone.utc)
    edge = now.replace(second=0, microsecond=0) + dt.timedelta(minutes=1)
    if edge.minute % 2:
        edge += dt.timedelta(minutes=1)
    what = f'WSPR {a.cycles} cycles: {msgs} at {a.hz:.1f} Hz, SNR {a.snr} dB in 2500 Hz'
    return x, edge, what


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


def wwv_frame(t):
    """The 60 symbols of WWV's time code for the minute that BEGINS at t
    (UTC), NIST SP 432 (the IRIG-H variant on 100 Hz): 0, 1, 2 = marker,
    3 = second 0's hole. Bits are BCD, least significant first, each digit
    in its own group: year units 4-7, minutes 10-13/15-17, hours
    20-23/25-26, day of year 30-33/35-38/40-41, year tens 51-54; DST 2 and
    55, leap second 3, DUT1 50/56-58 (all 0 here); markers at 9, 19, ...,
    59."""
    s = [0] * 60
    s[0] = 3
    for k in (9, 19, 29, 39, 49, 59):
        s[k] = 2

    def bcd(v, pos):
        for i, p in enumerate(pos):
            s[p] = (v >> i) & 1
    y = t.year % 100
    doy = t.timetuple().tm_yday
    bcd(y % 10, (4, 5, 6, 7))
    bcd(t.minute % 10, (10, 11, 12, 13))
    bcd(t.minute // 10, (15, 16, 17))
    bcd(t.hour % 10, (20, 21, 22, 23))
    bcd(t.hour // 10, (25, 26))
    bcd(doy % 10, (30, 31, 32, 33))
    bcd((doy // 10) % 10, (35, 36, 37, 38))
    bcd(doy // 100, (40, 41))
    bcd(y // 10, (51, 52, 53, 54))
    return s


def wwv_envelope(a):
    """WWV's AM envelope at FS_LO: the 100 Hz subcarrier at 18 % for 170,
    470 or 770 ms from 30 ms into the second and at 3 % the rest, none in
    second 0; the 5 ms tick of 1000 Hz at the top of each second (none at
    29 and 59, as the station does); the 500 Hz tone at 50 % if --tone500.
    --lead seconds of ticks and the low subcarrier before the first minute,
    which starts at --start (UTC, HH:MM; the next minute plus one by
    default)."""
    import datetime as dt
    now = dt.datetime.now(dt.timezone.utc).replace(second=0, microsecond=0)
    if a.start:
        hh, mm = (int(v) for v in a.start.split(':'))
        t0 = now.replace(hour=hh, minute=mm)
        if t0 <= now:
            t0 += dt.timedelta(days=1)
    else:
        t0 = now + dt.timedelta(minutes=2)
    n_s = FS_LO
    syms = [-1] * int(a.lead)
    for m in range(a.minutes):
        syms += wwv_frame(t0 + dt.timedelta(minutes=m))
    env = np.empty(len(syms) * n_s, dtype=np.float32)
    tt = np.arange(n_s) / FS_LO
    for k, sym in enumerate(syms):
        sec = (k - int(a.lead)) % 60
        ms = tt * 1000.0
        m100 = np.full(n_s, 0.03)
        if sym in (0, 1, 2):
            alto = {0: 170.0, 1: 470.0, 2: 770.0}[sym]
            m100[(ms >= 30.0) & (ms < 30.0 + alto)] = 0.18
        if sym == 3:
            m100[:] = 0.0
        tabs = (k * n_s + np.arange(n_s)) / FS_LO
        e = 1.0 + m100 * np.sin(2 * np.pi * 100.0 * tabs)
        if a.tone500:
            e += 0.5 * np.sin(2 * np.pi * 500.0 * tabs)
        if sec not in (29, 59):
            tick = ms < 5.0
            e[tick] += 1.0 * np.sin(2 * np.pi * 1000.0 * tabs[tick])
        env[k * n_s:(k + 1) * n_s] = e
    first = t0.strftime('%Y-%m-%d %H:%M UTC')
    return env, t0, (f'WWV time code, {a.minutes} min from {first} (day {t0.timetuple().tm_yday}), '
                     f'{a.lead:.0f} s lead')


def write_am_long(env, a, what):
    """A long AM signal straight to int8 I/Q, in pieces: the envelope (real,
    at FS_LO) interpolated linearly to the HackRF's rate - its content is
    under 1.5 kHz, so the images at multiples of 48 kHz fall in sinc
    nulls - and moved to --offset with a phase-continuous oscillator. All
    of it at once would be gigabytes."""
    up = a.rate // FS_LO
    assert up * FS_LO == a.rate, 'the rate must be a multiple of 48 kHz'
    scale = 110.0 / float(np.max(np.abs(env)))
    w = 2 * np.pi * a.offset / a.rate
    chunk = FS_LO                       # a second of envelope at a time
    ph0 = 0.0
    with open(a.out, 'wb') as f:
        for i in range(0, len(env), chunk):
            seg = env[i:i + chunk + 1]
            if len(seg) < 2:
                break
            n_out = (len(seg) - 1) * up
            xi = np.arange(n_out) / up
            y = np.interp(xi, np.arange(len(seg)), seg) * scale
            ph = ph0 + w * np.arange(n_out)
            iq = np.empty(2 * n_out, dtype=np.int8)
            iq[0::2] = np.clip(np.round(y * np.cos(ph)), -127, 127)
            iq[1::2] = np.clip(np.round(y * np.sin(ph)), -127, 127)
            iq.tofile(f)
            ph0 = (ph0 + w * n_out) % (2 * np.pi)
    print(f'{a.out}: {len(env) / FS_LO:.1f} s at {a.rate} Hz, {what}, '
          f'offset {a.offset:.0f} Hz')


def write_iq_long(x, a, what):
    """A long complex baseband (at FS_LO) straight to int8 I/Q, in pieces, as
    write_am_long(): interpolated linearly to the HackRF's rate (a multiple
    of 48 kHz; the content is a few kHz wide, so the images at multiples of
    48 kHz fall in the sinc's nulls) and moved to --offset with a
    phase-continuous oscillator. Full scale is the file's peak."""
    up = a.rate // FS_LO
    assert up * FS_LO == a.rate, 'the rate must be a multiple of 48 kHz'
    scale = 110.0 / float(np.max(np.abs(np.concatenate([x.real, x.imag]))))
    w = 2 * np.pi * a.offset / a.rate
    chunk = FS_LO
    ph0 = 0.0
    with open(a.out, 'wb') as f:
        for i in range(0, len(x), chunk):
            seg = x[i:i + chunk + 1]
            if len(seg) < 2:
                break
            n_out = (len(seg) - 1) * up
            xi = np.arange(n_out) / up
            k = np.arange(len(seg))
            y = (np.interp(xi, k, seg.real) + 1j * np.interp(xi, k, seg.imag)) * scale
            y = y * np.exp(1j * (ph0 + w * np.arange(n_out)))
            iq = np.empty(2 * n_out, dtype=np.int8)
            iq[0::2] = np.clip(np.round(y.real), -127, 127)
            iq[1::2] = np.clip(np.round(y.imag), -127, 127)
            iq.tofile(f)
            ph0 = (ph0 + w * n_out) % (2 * np.pi)
    print(f'{a.out}: {len(x) / FS_LO:.1f} s at {a.rate} Hz, {what}, offset {a.offset:.0f} Hz')


RDS_OFS = {'A': 0x0FC, 'B': 0x198, 'C': 0x168, "C'": 0x350, 'D': 0x1B4}


def rds_block(data, ofs):
    """16 bits of data and the 10-bit check (IEC 62106: g(x) = x^10 + x^8 +
    x^7 + x^5 + x^4 + x^3 + 1) plus the block's offset word, MSB first."""
    reg = data << 10
    for i in range(25, 9, -1):
        if reg & (1 << i):
            reg ^= 0x5B9 << (i - 10)
    chk = (reg & 0x3FF) ^ RDS_OFS[ofs]
    w = (data << 10) | chk
    return [(w >> (25 - k)) & 1 for k in range(26)]


def rds_group(a_, b_, c_, d_):
    return rds_block(a_, 'A') + rds_block(b_, 'B') + rds_block(c_, 'C') + rds_block(d_, 'D')


def rds_g0a(pi, ps, seg, tp=0, pty=0):
    b_ = (0 << 12) | (0 << 11) | (tp << 10) | (pty << 5) | (0 << 4) | (1 << 3) | (0 << 2) | seg
    d_ = (ord(ps[2 * seg]) << 8) | ord(ps[2 * seg + 1])
    return rds_group(pi, b_, 0xE0CD, d_)              # AF: none (224), filler (205)


def rds_g4a(pi, t, half_hours, tp=0, pty=0):
    """Clock-time: MJD, UTC hour and minute, the local offset in half hours
    - the time of the minute that begins where this group ends."""
    import datetime as dt
    mjd = (t.date() - dt.date(1858, 11, 17)).days
    b_ = (4 << 12) | (0 << 11) | (tp << 10) | (pty << 5) | ((mjd >> 15) & 3)
    c_ = ((mjd & 0x7FFF) << 1) | ((t.hour >> 4) & 1)
    sign = 1 if half_hours < 0 else 0
    d_ = ((t.hour & 0xF) << 12) | (t.minute << 6) | (sign << 5) | (abs(half_hours) & 0x1F)
    return rds_group(pi, b_, c_, d_)


def rds_long(a):
    """Wide FM with a 19 kHz pilot and RDS at 57 kHz, no audio, straight to
    int8 I/Q in one-second pieces: 0A groups with the PS, and a 4A
    clock-time group placed so it ENDS at each minute's edge, which the
    standard asks to within 0.1 s (and which rds.c's rds_hora_atraso_ms()
    counts from). The bits differentially coded, biphase, at 1187.5 bit/s,
    shaped at 152 kHz (128 samples a bit) and interpolated up."""
    import datetime as dt
    from scipy.signal import firwin, lfilter
    now = dt.datetime.now(dt.timezone.utc).replace(second=0, microsecond=0)
    t0 = now + dt.timedelta(minutes=2)
    if a.start:
        hh, mm = (int(v) for v in a.start.split(':'))
        t0 = now.replace(hour=hh, minute=mm)
        if t0 <= now:
            t0 += dt.timedelta(days=1)
    bps = 1187.5
    total_s = a.lead + 60.0 * a.minutes + 2.0
    n_grp = int(total_s * bps / 104) + 1
    kind = ['0A'] * n_grp
    when = {}
    for i in range(a.minutes + 1):
        edge = a.lead + 60.0 * i
        g = int(round(edge * bps / 104.0)) - 1      # the group that ends nearest the edge
        if 0 <= g < n_grp:
            kind[g] = '4A'
            when[g] = t0 + dt.timedelta(minutes=i)
    bits, seg = [], 0
    ps = (a.ps + ' ' * 8)[:8]
    for g in range(n_grp):
        if kind[g] == '4A':
            bits += rds_g4a(a.pi, when[g], a.half_hours)
        else:
            bits += rds_g0a(a.pi, ps, seg)
            seg = (seg + 1) & 3
    e, prev = [], 0
    for b in bits:                                   # differential coding
        prev ^= b
        e.append(prev)
    sps = 128
    fs_b = bps * sps                                 # 152 kHz
    half = np.concatenate([np.ones(sps // 2), -np.ones(sps // 2)])
    bb = np.concatenate([(1.0 if v else -1.0) * half for v in e])
    bb = lfilter(firwin(301, 2600.0, fs=fs_b), 1.0, bb)
    bb = np.roll(bb, -150)                           # the FIR's delay out
    bb /= np.max(np.abs(bb))
    w_off = 2 * np.pi * a.offset / a.rate
    ph = 0.0
    n_tot = int(total_s * a.rate)
    with open(a.out, 'wb') as f:
        for i0 in range(0, n_tot, a.rate):
            n = np.arange(i0, min(i0 + a.rate, n_tot))
            t = n / a.rate
            rds = np.interp(t * fs_b, np.arange(len(bb)), bb, right=0.0)
            m = a.dev_pilot * np.cos(2 * np.pi * 19000.0 * t) \
                + a.dev_rds * rds * np.cos(2 * np.pi * 57000.0 * t) \
                + a.dev_audio * np.sin(2 * np.pi * 1000.0 * t)
            inc = 2 * np.pi * m / a.rate + w_off
            phase = ph + np.cumsum(inc)
            ph = float(phase[-1]) % (2 * np.pi)
            iq = np.empty(2 * len(n), dtype=np.int8)
            iq[0::2] = np.round(110.0 * np.cos(phase)).astype(np.int8)
            iq[1::2] = np.round(110.0 * np.sin(phase)).astype(np.int8)
            iq.tofile(f)
    first = t0.strftime('%Y-%m-%d %H:%M UTC')
    print(f'{a.out}: {total_s:.1f} s at {a.rate} Hz, WFM + RDS PI {a.pi:04X} PS "{ps}", '
          f'4A from {first}, local offset {a.half_hours / 2:+.1f} h, offset {a.offset:.0f} Hz')
    print(f'start the transfer {a.lead:.0f} s before {t0.strftime("%H:%M:%S")} UTC')


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

    p = sub.add_parser('ax25', help='AX.25 UI frames: APRS (1200 Bd AFSK), or PKT300 with --hf')
    common(p)
    p.add_argument('--hf', action='store_true', help='300 Bd, 1600/1800 Hz (HF packet)')
    p.add_argument('--src', default='R1TEST-9')
    p.add_argument('--dest', default='APRS')
    p.add_argument('--path', default='WIDE1-1')
    p.add_argument('--info', default='!5545.00N/03737.00E-Malahit test #')
    p.add_argument('--frames', type=int, default=4)
    p.add_argument('--txdelay', type=int, default=40, help='flags before each frame')
    p.add_argument('--pause', type=float, default=2.0)
    p.add_argument('--lead', type=float, default=1.0)
    p.set_defaults(build=ax25_baseband, tone=1700.0)

    p = sub.add_parser('ais', help='AIS: message 1 and 5 of a ship, GMSK 9600 bps')
    common(p)
    p.add_argument('--mmsi', type=int, default=273123450)
    p.add_argument('--name', default='MALAHIT TEST')
    p.add_argument('--call', default='R1TEST')
    p.add_argument('--dest', default='SANKT PETERBURG')
    p.add_argument('--lat', type=float, default=59.9311)
    p.add_argument('--lon', type=float, default=30.3609)
    p.add_argument('--sog', type=float, default=12.3)
    p.add_argument('--cog', type=float, default=271.5)
    p.add_argument('--hdg', type=int, default=270)
    p.add_argument('--repeat', type=int, default=4)
    p.add_argument('--pause', type=float, default=1.0)
    p.add_argument('--lead', type=float, default=1.0)
    p.set_defaults(build=ais_baseband, tone=0.0)

    p = sub.add_parser('wwv', help='WWV time code (NIST SP 432) as AM, minutes long')
    p.add_argument('--out', required=True)
    p.add_argument('--offset', type=float, default=200e3)
    p.add_argument('--rate', type=int, default=1_968_000,
                   help='a multiple of 48 kHz (default 1.968 MHz)')
    p.add_argument('--minutes', type=int, default=4)
    p.add_argument('--lead', type=float, default=5.0,
                   help='seconds of ticks before the first minute (default 5)')
    p.add_argument('--start', default=None, help='UTC HH:MM of the first minute')
    p.add_argument('--tone500', action='store_true', help='add the 500 Hz tone at 50 %%')
    p.set_defaults(build=None)

    p = sub.add_parser('rds', help='wide FM with RDS: PS (0A) and the clock-time (4A)')
    p.add_argument('--out', required=True)
    p.add_argument('--offset', type=float, default=200e3)
    p.add_argument('--rate', type=int, default=2_016_000)
    p.add_argument('--minutes', type=int, default=3)
    p.add_argument('--lead', type=float, default=5.0)
    p.add_argument('--start', default=None, help='UTC HH:MM of the first minute edge after the lead')
    p.add_argument('--pi', type=lambda v: int(v, 16), default=0xC0DE)
    p.add_argument('--ps', default='MALAHIT')
    p.add_argument('--half-hours', type=int, default=4, help='local offset, half hours (+4 = +2:00)')
    p.add_argument('--dev-pilot', type=float, default=6750.0)
    p.add_argument('--dev-rds', type=float, default=2500.0)
    p.add_argument('--dev-audio', type=float, default=50000.0,
                   help='a 1 kHz mono tone: a station is never quite silent, and '
                        "upstream's WFM takes an unswept carrier for the LO's leakage")
    p.set_defaults(build=None)

    p = sub.add_parser('ft8', help='FT8 slots, the dial at 0 Hz (USB): --msg/--hz/--snr each')
    p.add_argument('--out', required=True)
    p.add_argument('--offset', type=float, default=200e3)
    p.add_argument('--rate', type=int, default=2_016_000,
                   help='a multiple of 48 kHz, 2 MHz or more for the HackRF (default 2.016 MHz): '
                        'minutes long, written in pieces')
    p.add_argument('--msg', action='append', help="'CQ YL2ABC KO26', 'R1ABC YL2ABC -12', ... (repeat)")
    p.add_argument('--hz', type=float, action='append', help='its audio frequency, tone 0 (repeat)')
    p.add_argument('--snr', type=float, action='append',
                   help='dB in 2500 Hz, WSJT-X\'s (one, or one per --msg; default -5)')
    p.add_argument('--slots', type=int, default=4)
    p.add_argument('--alternate', action='store_true',
                   help='each message every other slot, as stations work')
    p.add_argument('--dt', type=float, default=0.0, help='s late against the slot (DT)')
    p.add_argument('--lead', type=float, default=5.0, help='s before the first slot edge')
    p.add_argument('--clean', action='store_true', help='no noise')
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--audio', action='store_true',
                   help='write the 12 kHz f32 audio of the USB branch, not I/Q')
    p.add_argument('--level', type=float, default=1.0,
                   help='--audio: multiply by this (the radio\'s scale)')
    p.set_defaults(build=None)

    p = sub.add_parser('wspr', help="WSPR beacons, the dial at 0 Hz (USB), one a 2-minute cycle")
    p.add_argument('--out', required=True)
    p.add_argument('--offset', type=float, default=200e3)
    p.add_argument('--rate', type=int, default=2_016_000,
                   help='a multiple of 48 kHz, 2 MHz or more (default 2.016 MHz)')
    p.add_argument('--msg', action='append', help="'YL2ABC KO26 37' - call, locator, dBm (repeat: in turn)")
    p.add_argument('--hz', type=float, default=1500.0, help='the beacon\'s centre, audio Hz')
    p.add_argument('--snr', type=float, action='append',
                   help='dB in 2500 Hz, WSJT-X\'s (default -20); repeat: one a cycle, in turn')
    p.add_argument('--cycles', type=int, default=2)
    p.add_argument('--dt', type=float, default=0.0, help='s late against the even minute + 1 s')
    p.add_argument('--lead', type=float, default=5.0, help='s before the first even minute')
    p.add_argument('--clean', action='store_true')
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--audio', action='store_true')
    p.add_argument('--level', type=float, default=1.0)
    p.set_defaults(build=None)

    p = sub.add_parser('audio', help='any 12 kHz f32 audio file, sent as USB')
    common(p)
    p.add_argument('--in', dest='input', required=True, help='the f32 file at 12 kHz')
    p.set_defaults(build=audio_baseband, tone=1800.0)

    a = ap.parse_args()
    if a.mode == 'rds':
        rds_long(a)
        return
    if a.mode in ('ft8', 'wspr'):
        x, edge, what = ft8_slots(a) if a.mode == 'ft8' else wspr_cycles(a)
        if a.audio:
            r = resample_poly(np.real(x), 1, 4).astype(np.float32) * np.float32(a.level)
            r.tofile(a.out)
            print(f'{a.out}: {len(r) / 12000:.1f} s of audio at 12 kHz, {what}; '
                  f'the first slot edge {a.lead:.1f} s in')
        else:
            if a.rate % FS_LO:
                a.snr = None
                write_iq(x, a, what)        # resample_poly, the whole file in memory
            else:
                write_iq_long(x, a, what)
            print(f'start the transfer {a.lead:.1f} s before '
                  + ('a slot edge (:00 :15 :30 :45)' if a.mode == 'ft8' else 'an EVEN minute')
                  + f', e.g. {(edge).strftime("%H:%M:%S")} UTC')
        return
    if a.mode == 'wwv':
        env, t0, what = wwv_envelope(a)
        write_am_long(env, a, what)
        print(f'start the transfer {a.lead:.0f} s before {t0.strftime("%H:%M:%S")} UTC')
        return
    x, what = a.build(a)
    if a.fm > 0.0:
        # the audio an SSB receiver would hear, as the modulation of an FM
        # carrier at 0 Hz: phase = 2 pi dev * integral(audio / peak)
        n = np.arange(len(x))
        aud = np.real(x * np.exp(2j * np.pi * a.tone * n / FS_LO))
        aud = aud / max(1e-9, np.max(np.abs(aud)))
        x = np.exp(1j * 2 * np.pi * a.fm * np.cumsum(aud) / FS_LO)
        what += f', NFM +-{a.fm:.0f} Hz'
    if a.lsb and not a.audio and a.fm <= 0.0:
        # sent as LSB: the receiver hears the spectrum mirrored, so the
        # baseband goes out mirrored, and the dial is the HackRF's centre +
        # offset + --tone
        x = np.conj(x)
        what += ', LSB'
    x = add_noise(x, a)
    if a.audio:
        write_audio(x, a)
    else:
        write_iq(x, a, what)


if __name__ == '__main__':
    main()
