#!/usr/bin/env python3
"""
Test signals for the decoders, sent by a HackRF into the radio - 22/11/2026.

    tools/hackrf_gen.py cw --text "CQ DE R1TEST K" --wpm 20 --snr 12 --out f.iq
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
    """What the radio's USB branch hands its decoders: f32, 12 kHz."""
    n = np.arange(len(x))
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
                       help='the carrier\'s audio frequency for --audio')

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

    a = ap.parse_args()
    x, what = a.build(a)
    x = add_noise(x, a)
    if a.audio:
        write_audio(x, a)
    else:
        write_iq(x, a, what)


if __name__ == '__main__':
    main()
