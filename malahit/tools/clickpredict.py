#!/usr/bin/env python3
"""
The retune click that is LEFT - and whether predicting the modulation
across the hold beats fading it out.

clickmodel.py designed the repair that runs (am.c, "THE RETUNE CLICK"):
around a synthesiser write the envelope is held at the carrier's level
and only the modulation is faded, 1 ms down, 1.7 ms out, 1 ms back.
That removed the spike - its metric, the residual ABOVE 2 kHz, fell to
-20 dB - and left a hole in the modulation 3.7 ms long. On a steady
1 kHz tone a hole is a dull click, and the owner hears it on every
retune with a signal on the dial, 10 Hz steps included (21/11/2026).
A dip in a 1 kHz tone puts its energy around 1 kHz, below 2 kHz, where
clickmodel.py does not look.

So this measures the residual from the tone over the WHOLE audio band,
after the DC blocker and the 200 Hz - 4 kHz band-pass, and compares:

  none      the chain as it was before any repair
  hold      what runs: carrier frozen from the DC blocker's state,
            modulation faded (clickmodel.py's causal hold, exactly)
  predict   the same window, but what fills it is the envelope's own
            continuation: the pitch period found by autocorrelation on
            the history BEFORE the window, and the last period repeated
            - packet-loss concealment, as speech codecs do it. Causal:
            nothing from after the window start is used.

Every capture also gets the >2 kHz figure, so a repair that brings the
spike back cannot hide.

  python3 clickpredict.py <console log with RAW dumps> [--png out.png]
"""
import os
import sys

import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clickmodel as cm  # noqa: E402

FS = cm.FS
MIN_AFTER = 1700        # samples after the write a capture must have


def gain(rel, a, b, ramp):
    """The fade: 1 -> 0 over `ramp` before a, 0 over [a, b), 0 -> 1 over
    `ramp` after b. Raised cosine, as am.c's hold_gain()."""
    lo, hi = a - ramp, b + ramp
    g = np.ones(len(rel))
    r = rel.astype(float)
    dn = (r >= lo) & (r < a)
    g[dn] = 0.5 * (1 + np.cos(np.pi * (r[dn] - lo) / ramp))
    g[(r >= a) & (r < b)] = 0.0
    up = (r >= b) & (r < hi)
    g[up] = 0.5 * (1 - np.cos(np.pi * (r[up] - b) / ramp))
    return g


def pitch(h, pmin=96, pmax=2400, win=768):
    """Period of the history's tail by normalised autocorrelation: the
    last `win` samples against the same length `p` earlier. The
    SMALLEST lag within 90 % of the best wins, so a 1 kHz tone gives
    192 and not 384 or 576 - a multiple repeats more history than it
    needs to, and anything that drifts drifts further."""
    h = h - h.mean()
    x = h[-win:]
    ex = np.dot(x, x)
    lags = list(range(pmin, min(pmax, len(h) - win) + 1))
    r = []
    for p in lags:
        y = h[-win - p:-p]
        r.append(np.dot(x, y) / np.sqrt(ex * np.dot(y, y) + 1e-12))
    r = np.array(r)
    best = r.max()
    # PEAKS only. Near its maximum a sinusoid's autocorrelation falls
    # off as a cosine - 0.91 at 179 samples for a 192-sample period -
    # so "the first lag within 90 % of the best" lands on the leading
    # slope of the right peak, not on the peak. It did, at 179.
    for k in range(1, len(r) - 1):
        if r[k] >= r[k - 1] and r[k] >= r[k + 1] and r[k] >= 0.9 * best:
            return lags[k], r[k]
    k = int(np.argmax(r))
    return lags[k], r[k]


REAL_FROM = None        # rel of the first REAL sample (the rest is lead-in)

# The firmware's search, exactly as am.c does it: a coarse pass on the
# history taken every DEC-th sample (the envelope is band-limited well
# under 24 kHz, so plain sub-sampling is enough to find a period), then
# a full-rate pass within +-DEC of the coarse answer.
DEC, CWIN, CLAG_LO, CLAG_HI, FWIN, R_MIN = 8, 96, 12, 300, 512, 0.5


def ncorr(x, y):
    return np.dot(x, y) / np.sqrt(np.dot(x, x) * np.dot(y, y) + 1e-12)


def pitch_fw(h):
    """Returns (period in samples, r) or (0, r) when r < R_MIN - the
    firmware then falls back to the hold."""
    h = h - h[-FWIN:].mean()
    hd = h[::-1][::DEC][::-1]                    # every DEC-th, ending last
    x = hd[-CWIN:]
    r = np.array([ncorr(x, hd[-CWIN - L:-L])
                  for L in range(CLAG_LO, CLAG_HI + 1)])
    best = r.max()
    L = CLAG_LO + int(np.argmax(r))
    for k in range(1, len(r) - 1):
        if r[k] >= r[k - 1] and r[k] >= r[k + 1] and r[k] >= 0.9 * best:
            L = CLAG_LO + k
            break
    xf = h[-FWIN:]
    bp, br = 0, -2.0
    for p in range(L * DEC - DEC, L * DEC + DEC + 1):
        c = ncorr(xf, h[-FWIN - p:-p])
        if c > br:
            bp, br = p, c
    return (bp, br) if br >= R_MIN else (0, br)


def predict_fw(rel, env, a, b, ramp):
    lo, hi = a - ramp, b + ramp
    n0 = int(np.argmax(rel >= lo))
    n1 = int(np.argmax(rel >= hi))
    p, c = pitch_fw(env[:n0])
    if p == 0:
        return hold_env(rel, env, a, b, ramp), 0, c
    h = env[:n0]
    xp = np.array([h[n0 - p + (k % p)] for k in range(n1 - n0)])
    out = env.copy()
    g = gain(rel[n0:n1], a, b, ramp)
    out[n0:n1] = g * env[n0:n1] + (1 - g) * xp
    return out, p, c


def predict_env(rel, env, a, b, ramp, win=512):
    """Fill [a - ramp, b + ramp) with the envelope's own continuation,
    crossfaded with the live envelope by gain(). Returns the new
    envelope and the period used. The period is searched in REAL history
    only - the lead-in front() puts in front is a tiling, and a tiling
    of a carrier a few hertz off the dial has seams a pitch search
    finds instead of the tone."""
    lo, hi = a - ramp, b + ramp
    n0 = int(np.argmax(rel >= lo))
    n1 = int(np.argmax(rel >= hi))
    h = env[:n0]
    real = int(np.count_nonzero((rel >= REAL_FROM) & (rel < lo)))
    p, c = pitch(h, pmax=min(2400, real - win), win=win)
    buf = list(h[-p:])                       # one period, then repeat
    xp = np.array([buf[k % p] for k in range(n1 - n0)])
    out = env.copy()
    g = gain(rel[n0:n1], a, b, ramp)
    out[n0:n1] = g * env[n0:n1] + (1 - g) * xp
    return out, p, c


def hold_env(rel, env, a, b, ramp):
    """The firmware's hold, as clickmodel.chain(causal=...) does it."""
    lo = a - ramp
    n0 = int(np.argmax(rel >= lo))
    off = env[:cm.LEAD // 2].mean()
    y = signal.lfilter([1, -1], [1, -0.9995], env[:n0] - off)
    c0 = (env[n0 - 1] - off) - y[-1] + off
    g = gain(rel, a, b, ramp)
    return c0 + g * (env - c0)


def audio(env):
    off = env[:cm.LEAD // 2].mean()
    dcb = signal.lfilter([1, -1], [1, -0.9995], env - off)
    return signal.sosfilt(cm.SOS_AF, dcb)


def metrics(rel, aud, fit_hi):
    """Residual from the tone (fitted late, where it has settled):
    whole band peak and RMS over [-256, 900), and >2 kHz peak, all in
    dB re the tone's amplitude."""
    res, tone = cm.tone_residual(rel, aud, 960, fit_hi)
    w = (rel >= -256) & (rel < 900)
    pk = np.abs(res[w]).max()
    rms = np.sqrt(np.mean(res[w] ** 2))
    hf = signal.sosfilt(cm.SOS_HP2K, res)
    hpk = np.abs(hf[(rel >= -64) & (rel < 768)]).max()
    floor = np.sqrt(np.mean(res[(rel >= -1400) & (rel < -400)] ** 2))
    db = lambda v: 20 * np.log10(v / tone)  # noqa: E731
    return db(pk), db(rms), db(hpk), db(floor), res


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    variants = [
        ("hold   0..320 r192", "hold", (0, 320, 192)),
        ("predict 0..320 r192", "pred", (0, 320, 192)),
        ("predict 0..320 r64", "pred", (0, 320, 64)),
        ("predict 32..448 r64", "pred", (32, 448, 64)),
        ("predict 0..512 r128", "pred", (0, 512, 128)),
        ("FIRMWARE 0..320 r192", "fw", (0, 320, 192)),
    ]
    rows, plots = [], []
    for idx, d in cm.raws(sys.argv[1]):
        if d[:, 0].max() < MIN_AFTER:
            print(f"retune {idx}: {len(d)} frames, only {int(d[:, 0].max())}"
                  f" after the write - too short, skipped")
            continue
        global REAL_FROM
        REAL_FROM = int(d[0, 0])
        rel, z = cm.front(d)
        cm._REL = rel
        fit_hi = min(1920, int(rel.max()))
        yi = signal.sosfilt(cm.SOS_CH, z.real)
        yq = signal.sosfilt(cm.SOS_CH, z.imag)
        env = np.hypot(yi, yq)
        res = {}
        res["none"] = metrics(rel, audio(env), fit_hi)
        extra = ""
        for name, kind, w in variants:
            if kind == "hold":
                e = hold_env(rel, env, *w)
            elif kind == "fw":
                e, p, c = predict_fw(rel, env, *w)
                extra += f"; firmware search {p} r {c:.3f}"
            else:
                e, p, c = predict_env(rel, env, *w)
                extra = f"period {p} ({FS / p:.0f} Hz), r {c:.3f}"
            res[name] = metrics(rel, audio(e), fit_hi)
        print(f"retune {idx}: {len(d)} frames; {extra}")
        print(f"  {'':22s} {'peak':>7s} {'rms':>7s} {'>2kHz':>7s}"
              f"   (dB re tone; quiet floor {res['none'][3]:+.1f} rms)")
        for k, v in res.items():
            print(f"  {k:22s} {v[0]:+7.1f} {v[1]:+7.1f} {v[2]:+7.1f}")
        rows.append(res)
        plots.append((idx, rel, res))
    if rows:
        print("\nmean over the captures (dB re tone):")
        for k in rows[0]:
            m = np.mean([[r[k][0], r[k][1], r[k][2]] for r in rows], axis=0)
            print(f"  {k:22s} peak {m[0]:+6.1f}  rms {m[1]:+6.1f}"
                  f"  >2kHz {m[2]:+6.1f}")
    if "--png" in sys.argv and plots:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(len(plots), 1, figsize=(10, 2.6 * len(plots)),
                               squeeze=False)
        for r, (idx, rel, res) in enumerate(plots):
            s = (rel > -400) & (rel < 1200)
            for k in ("none", "hold   0..320 r192", "predict 0..320 r192"):
                ax[r][0].plot(rel[s], res[k][4][s], lw=0.7, label=k)
            ax[r][0].axvline(0, color="k", lw=0.5, ls=":")
            ax[r][0].set_title(f"retune {idx}: audio minus the tone")
            ax[r][0].legend(fontsize=7)
        ax[-1][0].set_xlabel("samples from the write (192 kHz)")
        fig.tight_layout()
        p = sys.argv[sys.argv.index("--png") + 1]
        fig.savefig(p, dpi=90)
        print(f"plot: {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
