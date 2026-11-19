#!/usr/bin/env python3
"""
Verify the AM envelope measurement BEFORE it goes on the radio, and emit
the one table it needs.

WHY THE ENVELOPE AND NOT THE SPECTRUM. The question is whether the AM
envelope is distorted, and the sidebands that would show it sit at
+/-1000, +/-2000, +/-3000 Hz either side of a carrier whose ABSOLUTE
frequency is unknown to about 125 Hz - the generator and the radio do
not share a reference. Measuring them in the spectrum therefore needs
the carrier found first, and then a non-coherent DFT at seven places.

The envelope |I + jQ| removes the carrier entirely. What is left is
DC + a 1000 Hz tone + harmonics, at frequencies the GENERATOR sets
exactly. 1000 Hz at Fs = 192000 is one cycle in exactly 192 samples, so
a 192-entry table measures 1000, 2000 and 3000 Hz coherently with one
stride each - no window, no leakage, no scalloping, and no dependence on
where the carrier landed.

  depth      m = 2 |X(1000)| / |X(0)|
  distortion |X(2000)| / |X(1000)|, which is zero for real AM
"""
import math
import os

FS = 192000
F_MOD = 1000
TBL = FS // F_MOD              # 192 samples per cycle of the modulation
assert TBL * F_MOD == FS, "the modulation must divide the sample rate"

SIN = [math.sin(2.0 * math.pi * k / TBL) for k in range(TBL)]
COS = [math.cos(2.0 * math.pi * k / TBL) for k in range(TBL)]


def env_dft(env, harm):
    """Coherent DFT of a real signal at harm * F_MOD. harm 0 is the mean."""
    n = (len(env) // TBL) * TBL          # whole cycles only, or it leaks
    if n == 0:
        return 0.0
    if harm == 0:
        return abs(sum(env[:n]) / n)
    re = im = 0.0
    for k in range(n):
        idx = (k * harm) % TBL
        re += env[k] * COS[idx]
        im -= env[k] * SIN[idx]
    return 2.0 * math.hypot(re, im) / n   # x2: a real tone splits its
                                          # energy between +f and -f


def measure(iq_i, iq_q):
    env = [math.hypot(i, q) for i, q in zip(iq_i, iq_q)]
    dc = env_dft(env, 0)
    h = [env_dft(env, k) for k in (1, 2, 3)]
    depth = (h[0] / dc) if dc > 0 else 0.0
    return depth, dc, h


def make_am(depth, n, carrier_off_hz=19875.0, amp=8000.0, clip=None,
            lsb=0.0, dc_i=0.0, dc_q=0.0, gain_db=0.0, phase_deg=0.0):
    """A downconverted AM signal, with the impairments this board has."""
    i_s, q_s = [], []
    g = 10.0 ** (gain_db / 20.0)
    pe = math.radians(phase_deg)
    for k in range(n):
        t = k / FS
        # The generator builds amplitude * (1 + m cos) and then the DAC
        # clips it if it does not fit. Clipping happens to the ENVELOPE,
        # at the generator, before anything is received.
        a = amp * (1.0 + depth * math.cos(2.0 * math.pi * F_MOD * t))
        if clip is not None and a > clip:
            a = clip
        ph = 2.0 * math.pi * carrier_off_hz * t
        i = a * math.cos(ph)
        q = g * a * math.sin(ph + pe)      # I/Q imbalance, if any
        if lsb > 0.0:                      # DAC quantisation
            i = round(i / lsb) * lsb
            q = round(q / lsb) * lsb
        i_s.append(i + dc_i)               # the mixer's standing offset
        q_s.append(q + dc_q)
    return i_s, q_s


def db(x):
    return -99.0 if x <= 0 else 20.0 * math.log10(x)


N = 7680                                   # 40 whole cycles of 1000 Hz
print(f"{'case':34s} {'depth':>7s} {'2nd':>8s} {'3rd':>8s}")
print("-" * 62)

for name, kw in [
    ("clean AM, 70 %",              dict(depth=0.70)),
    ("clean AM, 50 %",              dict(depth=0.50)),
    ("clean AM, 30 %",              dict(depth=0.30)),
    ("70 %, envelope clipped 10 %", dict(depth=0.70, clip=8000 * 1.53)),
    ("70 %, envelope clipped 25 %", dict(depth=0.70, clip=8000 * 1.27)),
    ("70 %, envelope clipped hard",  dict(depth=0.70, clip=8000 * 1.05)),
    ("70 %, DAC in ~10 LSBs",       dict(depth=0.70, lsb=8000 * 0.17)),
]:
    i_s, q_s = make_am(n=N, **kw)
    d, dc, h = measure(i_s, q_s)
    print(f"{name:34s} {d*100:6.1f}% {db(h[1]/h[0]):7.1f}dB "
          f"{db(h[2]/h[0]):7.1f}dB")

print()
print("Reading it: depth should come back as it was set, and the 2nd")
print("and 3rd harmonics of a REAL AM envelope are absent. Any energy")
print("there is distortion, and clipping the envelope is what puts it")
print("there - the harder the clip, the higher the harmonics.")
print()
print("A DAC working in ten levels shows as a depth that is still about")
print("right with harmonics well up: quantisation is not clipping, and")
print("the two are told apart by WHICH harmonic dominates and by")
print("whether the depth still reads correctly.")

# ---------------------------------------------------------------------
# THE INSTRUMENT'S OWN ERRORS, which cost a wrong conclusion once.
#
# The first measurement on the radio read 62.5 % depth with a 3rd
# harmonic at -26.6 dB, from a generator set to 70 %, and that reads
# exactly like a generator distorting. It was this measurement: |I+jQ|
# is not linear, so the mixer's standing DC offset beats against the
# rotating carrier, flattens the modulation and manufactures harmonics
# of it. The firmware subtracts the DC first now - the same correction
# upstream makes in demod_am.c as s_rf_dc_i, before its NCO mix.
# ---------------------------------------------------------------------
print()
print("=" * 62)
print("THE INSTRUMENT'S OWN ERRORS - why the firmware removes DC first")
print("=" * 62)
print(f"{'impairment':34s} {'depth':>7s} {'2nd':>8s} {'3rd':>8s}")
print("-" * 62)

AMP = 6371.0        # the envelope this board measured
for name, kw in [
    ("none",                          dict(depth=0.70)),
    ("DC +400/-80  (6 % of carrier)", dict(depth=0.70, dc_i=400, dc_q=-80)),
    ("DC +1705/-332 (26 %, MEASURED)", dict(depth=0.70, dc_i=1705,
                                            dc_q=-332)),
    ("DC +2500/-500 (39 %)",          dict(depth=0.70, dc_i=2500,
                                           dc_q=-500)),
    ("I/Q gain 1.0 dB",               dict(depth=0.70, gain_db=1.0)),
    ("I/Q phase 4 deg",               dict(depth=0.70, phase_deg=4.0)),
]:
    i_s, q_s = make_am(n=N, amp=AMP, **kw)
    d, dc, h = measure(i_s, q_s)
    print(f"{name:34s} {d*100:6.1f}% {db(h[1]/h[0]):7.1f}dB "
          f"{db(h[2]/h[0]):7.1f}dB")

print()
print("The DC rows are why the subtraction is not optional: at the 26 %")
print("this board actually has, a PERFECT 70 % signal reads 66 % with a")
print("2nd harmonic at -38 dB, which is most of what was first blamed")
print("on the generator.")
print()
print("The I/Q rows are the control: imbalance leaves the depth exactly")
print("right. Its wobble is at twice the CARRIER rate, tens of kHz from")
print("1000 Hz, so the coherent DFT never sees it - which means a depth")
print("error can never be blamed on I/Q balance, and that is worth")
print("knowing before someone tries.")

# ---- the table, for the firmware -----------------------------------
#
# The path is relative to THIS FILE, not to the shell's directory. Said
# plainly because the first version wrote "env_tbl.h" and left a second,
# identical, untracked copy in tools/ every time it was run from there -
# a generated file in two places is a generated file that will one day
# disagree with itself.
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "compat", "env_tbl.h")
with open(OUT, "w") as f:
    f.write("/* GENERATED by envdft.py - one cycle of 1000 Hz at 192 kHz.\n"
            " * 192 points, so 1000/2000/3000 Hz are all coherent with a\n"
            " * stride. Written from a host computation rather than from a\n"
            " * hand-rolled approximation: see trap 10 in WORKING.md, where\n"
            " * a polynomial recalled from memory was exact at every power\n"
            " * of two and 1.5 bits wrong in between. */\n")
    f.write(f"#define ENV_TBL_N {TBL}\n\n")
    f.write("static const float env_sin[ENV_TBL_N] = {\n")
    for r in range(0, TBL, 6):
        f.write("    " + ", ".join(f"{v: .9f}f" for v in SIN[r:r+6]) + ",\n")
    f.write("};\n\nstatic const float env_cos[ENV_TBL_N] = {\n")
    for r in range(0, TBL, 6):
        f.write("    " + ", ".join(f"{v: .9f}f" for v in COS[r:r+6]) + ",\n")
    f.write("};\n")

err = max(abs(SIN[k] - math.sin(2*math.pi*k/TBL)) for k in range(TBL))
print(f"\ntable written, max error {err:.2e} (float32 holds ~1e-7)")
