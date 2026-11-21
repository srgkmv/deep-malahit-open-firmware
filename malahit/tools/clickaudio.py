#!/usr/bin/env python3
"""
The retune click in what the CODEC got - am.c's output after the AGC,
dumped by the retune test (main.c RETUNE_TEST 2, the `AUD n begin` blocks
and their `A rel v` lines), one dump per move, each labelled with the
repair the firmware used for it: PREDICT or HOLD (am.h, "PREDICTED, NOT
FADED"). The test interleaves them, P H H P, so the two are compared on
the same signal, the same minute, both directions of move.

Per dump: a tone of 1 kHz with its 2nd and 3rd harmonics fitted on the
LATE part, well after the retune, and the residual from it

  peak, rms   over [-256, 900) samples from the write - the whole audio
              band, which is where a hole in a 1 kHz tone puts its energy
  >2 kHz      the same residual high-passed, peak - where a spike lives
  floor       rms of the residual well BEFORE the write, where nothing
              happened: no repair can beat it

all in dB re the tone's amplitude.

  python3 clickaudio.py <console log> [--png out.png]
"""
import sys

import numpy as np
from scipy import signal

FS = 192000
HP2K = signal.butter(4, 2000, btype="high", fs=FS, output="sos")


def dumps(path):
    cur, idx, mode = None, None, None
    with open(path, "rb") as fh:
        for raw in fh:
            line = raw.decode("ascii", "replace").strip()
            if line.startswith("AUD") and "begin" in line:
                idx = int(line.split()[1].rstrip(":"))
                mode = "PREDICT" if "PREDICT" in line else "HOLD"
                cur = []
            elif line.startswith("AUD") and "end" in line:
                if cur:
                    yield idx, mode, np.array(cur, dtype=float)
                cur = None
            elif cur is not None and line.startswith("A "):
                p = line.split()
                if len(p) == 3:
                    try:
                        cur.append([float(p[1]), float(p[2])])
                    except ValueError:
                        pass


def residual(rel, y, lo, hi):
    t = rel / FS
    cols = [np.ones_like(t)]
    for h in (1, 2, 3):
        cols += [np.cos(2 * np.pi * 1000 * h * t),
                 np.sin(2 * np.pi * 1000 * h * t)]
    A = np.column_stack(cols)
    m = (rel >= lo) & (rel < hi)
    c, *_ = np.linalg.lstsq(A[m], y[m], rcond=None)
    return y - A @ c, abs(c[1] + 1j * c[2])


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rows = {"PREDICT": [], "HOLD": []}
    plots = []
    for idx, mode, d in dumps(sys.argv[1]):
        rel, y = d[:, 0], d[:, 1]
        res, tone = residual(rel, y, 1000, min(2500, int(rel.max())))
        db = lambda v: 20 * np.log10(max(v, 1e-9) / tone)  # noqa: E731
        w = (rel >= -256) & (rel < 900)
        pk = db(np.abs(res[w]).max())
        rms = db(np.sqrt(np.mean(res[w] ** 2)))
        hf = db(np.abs(signal.sosfilt(HP2K, res)[w]).max())
        fl = db(np.sqrt(np.mean(res[(rel >= -1400) & (rel < -400)] ** 2)))
        rows[mode].append((pk, rms, hf, fl))
        plots.append((idx, mode, rel, res, tone))
        print(f"move {idx}: {mode:7s} tone {tone:6.0f}  peak {pk:+6.1f}  "
              f"rms {rms:+6.1f}  >2kHz {hf:+6.1f}   floor {fl:+6.1f}")
    print()
    for mode, r in rows.items():
        if r:
            m = np.mean(r, axis=0)
            print(f"mean {mode:7s} ({len(r)}): peak {m[0]:+6.1f}  rms "
                  f"{m[1]:+6.1f}  >2kHz {m[2]:+6.1f}   floor {m[3]:+6.1f}")
    if "--png" in sys.argv and plots:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(len(plots), 1, figsize=(10, 1.9 * len(plots)),
                               squeeze=False, sharex=True)
        for r, (idx, mode, rel, res, tone) in enumerate(plots):
            s = (rel > -600) & (rel < 1400)
            ax[r][0].plot(rel[s], res[s] / tone, lw=0.7)
            ax[r][0].set_ylim(-1.2, 1.2)
            ax[r][0].axvline(0, color="k", lw=0.5, ls=":")
            ax[r][0].set_ylabel(f"{idx} {mode}", fontsize=8)
        ax[-1][0].set_xlabel("samples from the write (192 kHz); "
                             "audio minus the tone, re the tone")
        fig.tight_layout()
        p = sys.argv[sys.argv.index("--png") + 1]
        fig.savefig(p, dpi=90)
        print(f"plot: {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
