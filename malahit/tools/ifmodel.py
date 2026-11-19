#!/usr/bin/env python3
"""
A host model of bringup/src/am.c's AM chain, to ask where the carrier
may sit relative to the local oscillator before the audio breaks.

WHY THIS EXISTS. For one session the firmware ran zero IF: tuner on the
dial, and the oscillator's leakage removed by subtracting a one-pole DC
estimate from I and Q. At zero IF the AM carrier is that DC term too,
and the bench could not show it - the generator sits a repeatable
2 250 Hz off the dial. This model can, and it showed it before the
build was ever loaded. WORKING.md, "Forward again, to a fixed 12 kHz".

THE CHAIN, as in am_block():

  z + leakage -> DC estimate subtracted (DC_K) -> [rotate by +AM_IF_HZ]
  -> channel low-pass 4 kHz on I and Q -> |z| -> DC blocker 0.9995
  -> audio band-pass 200..4000 Hz       (the AGC is a gain; omitted)

The two filters are Butterworth stand-ins for upstream's audiofil
design. The rotation and the probe use the same 16-entry table and the
same formulas as am.c, and the station sits on the NEGATIVE side of
z = I + jQ, which is what am_probe() measured on the radio.

  python3 ifmodel.py            the fixed 12 kHz offset (what runs)
  python3 ifmodel.py --zero-if  tuner on the dial, no rotation
"""
import sys

import numpy as np
from scipy import signal

FS = 192000
IF_HZ = 12000
FM = 1000.0          # modulating tone
M = 0.70             # depth, as the bench generator is set
DC_K = 0.000052      # am.c's leakage estimate, ~1.6 Hz
LEAK = 2.4           # leakage / carrier, am_probe(): DC 1712 against 715
T = 6.0              # seconds simulated
TAIL = 3.0           # seconds measured at the end, after the estimate settles

ZERO_IF = "--zero-if" in sys.argv[1:]

n = int(FS * T)
t = np.arange(n) / FS
k = np.arange(n)
ROT_C = np.cos(np.pi * (k % 16) / 8)
ROT_S = np.sin(np.pi * (k % 16) / 8)

B_CH, A_CH = signal.butter(4, 4000, fs=FS)
B_AF, A_AF = signal.butter(2, [200, 4000], btype="band", fs=FS)


def baseband(err_hz):
    """The station, err_hz off the dial, plus the leakage at DC."""
    where = err_hz if ZERO_IF else -IF_HZ + err_hz
    env = 1.0 + M * np.cos(2 * np.pi * FM * t)
    return env * np.exp(1j * (2 * np.pi * where * t + 0.3)) \
        + LEAK * np.exp(1j * 1.1)


def chain(err_hz):
    z = baseband(err_hz)
    i, q = z.real, z.imag

    est = [DC_K], [1, -(1 - DC_K)]
    i = i - signal.lfilter(*est, i)
    q = q - signal.lfilter(*est, q)

    if not ZERO_IF:                         # (i + jq)(c + js)
        i, q = i * ROT_C - q * ROT_S, i * ROT_S + q * ROT_C

    env = np.hypot(signal.lfilter(B_CH, A_CH, i),
                   signal.lfilter(B_CH, A_CH, q))
    env = signal.lfilter([1, -1], [1, -0.9995], env)
    aud = signal.lfilter(B_AF, A_AF, env)

    seg = aud[n - int(FS * TAIL):]
    seg = seg[: (len(seg) // 192) * 192]    # whole cycles of 1 kHz
    tt = np.arange(len(seg)) / FS

    def amp(h):
        return 2 * abs(np.mean(seg * np.exp(-2j * np.pi * h * FM * tt)))

    return amp(1), amp(2), amp(3)


def probe(err_hz):
    """am_probe(), the same formulas: leakage, station side, mirror."""
    z = baseband(err_hz)
    i, q = z.real, z.imag
    dc = np.hypot(i.sum(), q.sum()) / n
    st = np.hypot(np.sum(i * ROT_C - q * ROT_S),
                  np.sum(i * ROT_S + q * ROT_C)) / n
    mi = np.hypot(np.sum(i * ROT_C + q * ROT_S),
                  np.sum(q * ROT_C - i * ROT_S)) / n
    return dc, st, mi


def main():
    print("zero IF, tuner on the dial" if ZERO_IF
          else f"tuner {IF_HZ} Hz below the dial, rotated back")
    ref = chain(2250.0)[0]
    print(f"{'carrier-dial':>12} {'1 kHz':>9} {'H2':>9} {'H3':>9}")
    for d in [0.0, 0.25, 0.5, 1.0, 2.0, 10.0, 100.0, 2250.0]:
        a1, a2, a3 = chain(d)
        rel = lambda x: 20 * np.log10(max(x, 1e-15) / max(a1, 1e-15))
        print(f"{d:>10.2f}Hz {20 * np.log10(max(a1, 1e-15) / ref):>+8.1f}dB"
              f" {rel(a2):>+8.1f}dB {rel(a3):>+8.1f}dB")
    if not ZERO_IF:
        dc, st, mi = probe(0.0)
        print(f"probe: leak DC {dc:.2f}  station {st:.2f}  mirror {mi:.4f}"
              f"   (carrier 1.0, leakage {LEAK})")


if __name__ == "__main__":
    main()
