#!/usr/bin/env python3
"""
Read the retune test's FINE dumps (main.c, RETUNE_TEST 2) out of a
console log and say what a click is made of.

Each dump is 768 entries of 32 samples (167 us) across one retune:

  F rel lr li cr ci a0 a1

  rel     entries from the one the synthesiser was written in
  lr li   raw I/Q mean over 32 samples - the 0 Hz term: leakage plus
          the tuner's DC offset (the station cancels: 32 samples is two
          periods of 12 kHz)
  cr ci   after leakage subtraction and rotation, before the channel
          filter: the station's complex envelope
  a0 a1   the codec's output, every 16th sample (12 kHz)

What it prints per retune, and why each one:

  leak step     how far the 0 Hz term moved, and how long am.c's 1.6 Hz
                estimate takes to follow - a DC step it has not followed
                yet is still in the signal, rotated to 12 kHz
  carrier       amplitude dip and phase jump at the retune - a PLL that
                relocks with a phase step makes the channel filter's
                output pass through a smaller vector on the way
  click         the audio with the 1 kHz tone notched out: whatever is
                left near the retune is the click, in peak and in time

  python3 clickdump.py <console log> [--png out.png]
"""
import sys

import numpy as np

SUB = 32
FS = 192000
T_ENTRY_MS = SUB * 1000.0 / FS          # 0.1667 ms
FS_AUDIO = FS / 16                      # a0/a1: every 16th sample


def dumps(path):
    """Yield (index, rows) for each FINE dump in the log."""
    cur, idx = None, None
    with open(path, "rb") as fh:
        for raw in fh:
            line = raw.decode("ascii", "replace").strip()
            if line.startswith("FINE") and "begin" in line:
                idx = int(line.split()[1])
                cur = []
            elif line.startswith("FINE") and "end" in line:
                if cur:
                    yield idx, np.array(cur, dtype=float)
                cur = None
            elif cur is not None and line.startswith("F "):
                parts = line.split()
                if len(parts) == 8:
                    try:
                        cur.append([float(p) for p in parts[1:]])
                    except ValueError:
                        pass             # a garbled line; skip it


def notch_1k(x):
    """Remove the 1 kHz test tone and its harmonics by a least-squares fit
    of a few sinusoids over the PRE-retune audio, carried across."""
    t = np.arange(len(x)) / FS_AUDIO
    cols = []
    for h in (1, 2, 3):
        cols += [np.cos(2 * np.pi * 1000 * h * t),
                 np.sin(2 * np.pi * 1000 * h * t)]
    return np.column_stack(cols)


def analyse(idx, d):
    rel = d[:, 0]
    lr, li, cr, ci = d[:, 1], d[:, 2], d[:, 3], d[:, 4]
    a = np.empty(2 * len(d))
    a[0::2], a[1::2] = d[:, 5], d[:, 6]
    t_ms = rel * T_ENTRY_MS
    pre = rel < -2
    post_late = rel > rel.max() - 128              # last ~21 ms

    leak = lr + 1j * li
    l0 = leak[pre].mean()
    l1 = leak[post_late].mean()
    step = abs(l1 - l0)

    car = cr + 1j * ci
    amp = np.abs(car)
    a_pre = np.median(amp[pre])
    near = (rel >= -2) & (rel <= 60)               # retune .. +10 ms
    a_min = amp[near].min() if near.any() else np.nan
    t_min = t_ms[near][np.argmin(amp[near])] if near.any() else np.nan

    # Phase: unwrap, and compare the step across the retune with the
    # increment the carrier's own offset predicts on each side.
    ph = np.unwrap(np.angle(car))
    dph = np.diff(ph)
    inc_pre = np.median(dph[pre[1:]])
    inc_post = np.median(dph[(rel[1:] > 60)])
    win = (rel[1:] >= -2) & (rel[1:] <= 60)
    excess = dph[win] - np.where(rel[1:][win] < 0, inc_pre, inc_post)
    jump = np.degrees(excess.sum()) if win.any() else np.nan

    # The click: audio with the tone fitted on the pre-retune part and
    # subtracted everywhere.
    t_a = np.repeat(t_ms, 2) + np.tile([0.0, T_ENTRY_MS / 2], len(d))
    A = notch_1k(a)
    pre_a = np.repeat(pre, 2)
    coef, *_ = np.linalg.lstsq(A[pre_a], a[pre_a], rcond=None)
    res = a - A @ coef
    noise = np.sqrt(np.mean(res[pre_a] ** 2))
    win_a = (t_a >= -1) & (t_a <= 20)
    k = np.argmax(np.abs(res[win_a]))
    click_pk = np.abs(res[win_a])[k]
    click_t = t_a[win_a][k]
    over = np.abs(res) > 4 * noise
    span = t_a[over & win_a]
    dur = (span.max() - span.min()) if span.size else 0.0
    tone = np.hypot(coef[0], coef[1])

    print(f"\nretune {idx}: retune at entry 0 (t = 0 ms)")
    print(f"  leak    before {abs(l0):7.1f} at {np.degrees(np.angle(l0)):+6.1f} deg"
          f"   after {abs(l1):7.1f} at {np.degrees(np.angle(l1)):+6.1f} deg"
          f"   STEP {step:7.1f}")
    print(f"  carrier amplitude {a_pre:7.1f} before, min {a_min:7.1f} "
          f"({20*np.log10(max(a_min,1e-9)/a_pre):+5.1f} dB) at {t_min:+6.2f} ms")
    print(f"          phase increment {np.degrees(inc_pre):+6.1f} -> "
          f"{np.degrees(inc_post):+6.1f} deg/entry, EXCESS across the "
          f"retune {jump:+7.1f} deg")
    print(f"  audio   tone {tone:7.0f}, residual rms before {noise:6.1f}")
    print(f"  CLICK   peak residual {click_pk:7.0f} "
          f"({20*np.log10(max(click_pk,1e-9)/tone):+5.1f} dB re tone) at "
          f"{click_t:+6.2f} ms, above 4x noise for {dur:5.2f} ms")

    # The AUDIBLE part: the residual above 2 kHz. A smooth dip in a 1 kHz
    # tone, or the AGC's level change after it, puts almost nothing up
    # there; a spike puts most of itself there. Same metric as
    # clickmodel.py, on the radio's real output instead of a model's.
    from scipy import signal as _sig
    hp = _sig.butter(4, 2000, btype="high", fs=FS_AUDIO, output="sos")
    hf = _sig.sosfilt(hp, res)
    floor = np.abs(hf[(t_a > -40) & (t_a < -10)]).max()
    win_h = (t_a >= -2) & (t_a <= 10)
    hpk = np.abs(hf[win_h]).max()
    print(f"  AUDIBLE residual above 2 kHz: {hpk:7.0f} "
          f"({20*np.log10(max(hpk,1e-9)/tone):+5.1f} dB re tone), "
          f"floor before {floor:5.0f} ({20*np.log10(max(floor,1e-9)/tone):+5.1f} dB)")
    return dict(t=t_ms, leak=leak, amp=amp, ph=ph, t_a=t_a, a=a, res=res,
                idx=idx)


def plot(results, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    n = len(results)
    fig, ax = plt.subplots(4, n, figsize=(4.2 * n, 9), squeeze=False,
                           sharex="col")
    for c, r in enumerate(results):
        sel = (r["t"] > -15) & (r["t"] < 40)
        sela = (r["t_a"] > -15) & (r["t_a"] < 40)
        ax[0][c].plot(r["t"][sel], np.abs(r["leak"][sel]))
        ax[0][c].set_title(f"retune {r['idx']}")
        ax[0][c].set_ylabel("|leak|")
        ax[1][c].plot(r["t"][sel], r["amp"][sel])
        ax[1][c].set_ylabel("|carrier|")
        ax[2][c].plot(r["t_a"][sela], r["a"][sela], lw=0.7)
        ax[2][c].set_ylabel("audio out")
        ax[3][c].plot(r["t_a"][sela], r["res"][sela], lw=0.7, color="C3")
        ax[3][c].set_ylabel("audio - tone")
        ax[3][c].set_xlabel("ms from the synthesiser write")
        for row in ax:
            row[c].axvline(0, color="k", lw=0.5, ls=":")
    fig.tight_layout()
    fig.savefig(out, dpi=90)
    print(f"\nplot: {out}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    results = [analyse(i, d) for i, d in dumps(sys.argv[1])]
    if not results:
        print("no FINE dumps in that log")
        return 1
    if "--png" in sys.argv:
        plot(results, sys.argv[sys.argv.index("--png") + 1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
