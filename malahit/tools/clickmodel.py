#!/usr/bin/env python3
"""
am.c's chain, on the RAW samples the retune test captured around a
synthesiser write (main.c RETUNE_TEST 2, the `R rel q i` lines), with
and without a repair of the glitch - so the repair is designed on the
real glitch and judged by the same chain that will run it.

The filters are upstream's own coefficients, printed by compiling
User/audiofil.c on the host (audiofil_disena(0, 4000) for the channel,
(200, 4000) for the audio), not stand-ins.

What it reports per retune:

  glitch    where the station's complex envelope departs from itself:
            sample-level |z| and phase after the leakage subtraction and
            the rotation, i.e. exactly what the channel filter is fed
  click     the channel-filtered envelope minus a fitted AM tone, peak
            in the 3 ms after the write: the thing the DC blocker and
            the audio band-pass turn into the click
  repaired  the same, with the I/Q bridged across the glitch window

  python3 clickmodel.py <console log> [--png out.png]
          [--win A B]   bridge samples [A, B) relative to the write

WHAT THIS DOES NOT SEE (21/11/2026): its audible metric is the residual
ABOVE 2 kHz, built to catch the spike - and blind to the hole the hold
then left in the modulation, whose energy sits around the tone. The
hole was the dull click still heard; clickpredict.py measures the whole
band, and clickaudio.py the codec's real output.
"""
import sys

import numpy as np
from scipy import signal

FS = 192000
DC_K = 0.000052

# audiofil_disena(0, 4000, 192000): {b0 b1 b2 a1 a2} x 3, CMSIS DF1 sign
CHANNEL = [0.00381723512, 0.00763447024, 0.00381723512, 1.76950443, -0.78477335,
           0.0040740571, 0.0081481142, 0.0040740571, 1.888556, -0.904852152,
           1, 0, 0, 0, 0]
AUDIO = [0.00381723512, 0.00763447024, 0.00381723512, 1.76950443, -0.78477335,
         0.0040740571, 0.0081481142, 0.0040740571, 1.888556, -0.904852152,
         0.995382726, -1.99076545, 0.995382726, 1.99074411, -0.990786731]


def sos(c):
    """CMSIS DF1 {b0 b1 b2 a1 a2} with y += a1*y1 + a2*y2 -> scipy sos."""
    out = []
    for s in range(0, len(c), 5):
        b0, b1, b2, a1, a2 = c[s:s + 5]
        out.append([b0, b1, b2, 1.0, -a1, -a2])
    return np.array(out)


SOS_CH, SOS_AF = sos(CHANNEL), sos(AUDIO)


def raws(path):
    cur, idx = None, None
    with open(path, "rb") as fh:
        for raw in fh:
            line = raw.decode("ascii", "replace").strip()
            if line.startswith("RAW") and "begin" in line:
                idx, cur = int(line.split()[1]), []
            elif line.startswith("RAW") and "end" in line:
                if cur:
                    yield idx, np.array(cur, dtype=float)
                cur = None
            elif cur is not None and line.startswith("R "):
                p = line.split()
                if len(p) == 4:
                    try:
                        cur.append([float(x) for x in p[1:]])
                    except ValueError:
                        pass


LEAD = 40 * 192         # 40 ms of lead-in, see front()


def front(d):
    """Leakage subtracted (estimate started at the pre-write mean, as a
    settled one would be) and rotated by +12 kHz with am.c's table.

    WITH A LEAD-IN, because the capture holds only 1.3 ms before the
    write and the DC blocker and the 200 Hz high-pass need tens of
    milliseconds to settle: without it, every metric here peaked on the
    filters' own start-up and three repairs scored identically to the
    last digit. The bench signal makes a lead-in exact rather than
    invented: 1 kHz modulation is periodic in 192 samples at 192 kHz,
    and with the carrier on the dial or 1 kHz off it, so is z. So the
    first 192 pre-write samples are repeated 40 times in front."""
    rel, q, i = d[:, 0], d[:, 1], d[:, 2]
    z = i + 1j * q
    per = z[:192]
    z = np.concatenate([np.tile(per, LEAD // 192), z])
    rel = np.concatenate([np.arange(-LEAD, 0) + rel[0], rel])
    pre = rel < -16
    est0 = z[pre].mean()
    est = signal.lfilter([DC_K], [1, -(1 - DC_K)], z - est0) + est0
    z = z - est
    n = np.arange(len(z))
    return rel, z * np.exp(1j * np.pi * n / 8)


def bridge(rel, z, a, b):
    """Polar bridge over [a, b): amplitude linear and UNWRAPPED phase
    linear between means of 16 samples either side - so the channel
    filter sees the carrier swing round, not collapse through zero."""
    z = z.copy()
    w = (rel >= a) & (rel < b)
    if not w.any():
        return z
    k0, k1 = np.argmax(w), len(w) - np.argmax(w[::-1])   # [k0, k1)
    left = z[max(k0 - 16, 0):k0]
    right = z[k1:k1 + 16]
    zl, zr = left.mean(), right.mean()
    al, ar = abs(zl), abs(zr)
    pl = np.angle(zl)
    pr = pl + np.angle(zr / zl)                  # shortest way round
    t = (np.arange(k0, k1) - (k0 - 0.5)) / (k1 - k0 + 0.0)
    z[k0:k1] = (al + (ar - al) * t) * np.exp(1j * (pl + (pr - pl) * t))
    return z


def find_glitch(rel, z, search=(0, 256), k=4.0, margin=4):
    """The glitch's own extent: where the sample-to-sample change of the
    station's complex envelope exceeds k times its pre-write median.
    Clean signal is band-limited and moves little per sample; the
    synthesiser's relock is noise-like and moves a lot."""
    d = np.abs(np.diff(z, prepend=z[0]))
    ref = np.median(d[rel < -16])
    hot = (d > k * ref) & (rel >= search[0]) & (rel < search[1])
    if not hot.any():
        return None
    a, b = rel[hot].min() - margin, rel[hot].max() + 1 + margin
    return int(a), int(b)


def hermite(y0, s0, y1, s1, n):
    """Cubic Hermite from (y0, slope s0) to (y1, slope s1) over n+1
    intervals, at the n interior points; slopes per sample."""
    t = np.arange(1, n + 1) / (n + 1.0)
    L = n + 1.0
    h00 = 2 * t**3 - 3 * t**2 + 1
    h10 = t**3 - 2 * t**2 + t
    h01 = -2 * t**3 + 3 * t**2
    h11 = t**3 - t**2
    return h00 * y0 + h10 * L * s0 + h01 * y1 + h11 * L * s1


def end_fit(y, x):
    """Value at the inner end and slope, from a straight line fitted to
    a short run - robust to the noise a single-sample slope is not."""
    c = np.polyfit(x, y, 1)
    return c


def bridge_hermite(rel, z, a, b, run=16):
    """Bridge [a, b) with cubic Hermite in amplitude AND in unwrapped
    phase, end values and slopes from a straight-line fit to `run`
    samples either side. The phase slope at each end is the carrier's
    frequency offset on that side, so the bridge also carries the
    frequency change across."""
    z = z.copy()
    w = (rel >= a) & (rel < b)
    if not w.any():
        return z
    k0, k1 = np.argmax(w), len(w) - np.argmax(w[::-1])   # [k0, k1)
    if k0 < run or k1 + run > len(z):
        return z
    xl = np.arange(k0 - run, k0)
    xr = np.arange(k1, k1 + run)
    al = end_fit(np.abs(z[xl]), xl)
    ar = end_fit(np.abs(z[xr]), xr)
    pl_u = np.unwrap(np.angle(z[xl]))
    pr_u = np.unwrap(np.angle(z[xr]))
    pl = end_fit(pl_u, xl)
    pr = end_fit(pr_u, xr)
    y0a, y1a = np.polyval(al, k0 - 1), np.polyval(ar, k1)
    y0p, y1p = np.polyval(pl, k0 - 1), np.polyval(pr, k1)
    # The phase across the gap is not knowable mod 2 pi; take the end
    # phase nearest to where the left side's own slope would have gone.
    guess = y0p + pl[0] * (k1 - (k0 - 1))
    y1p = y1p + 2 * np.pi * np.round((guess - y1p) / (2 * np.pi))
    n = k1 - k0
    amp = hermite(y0a, al[0], y1a, ar[0], n)
    ph = hermite(y0p, pl[0], y1p, pr[0], n)
    z[k0:k1] = np.maximum(amp, 0.0) * np.exp(1j * ph)
    return z


def chain(z, hold=None, causal=None):
    """am.c from the channel filter on. `hold` = (a, b, ramp): over
    [a, b) samples from the write, the envelope is replaced by the
    CARRIER level, faded in and out over `ramp` samples either side -
    so the modulation dips smoothly and nothing the DC blocker or the
    AGC could latch on is ever made.

    `causal` = (a, b, ramp) does the same the way the firmware can: the
    carrier level is FROZEN at the start of the fade, taken from the DC
    blocker's own state (input minus output = its ~10 ms mean), and the
    fade back simply blends the live envelope in. Nothing from after the
    window is used - the hook cannot always see that far."""
    yi = signal.sosfilt(SOS_CH, z.real)
    yq = signal.sosfilt(SOS_CH, z.imag)
    env = np.hypot(yi, yq)
    if hold is not None:
        env = hold_env(env, *hold)
    off = env[:LEAD // 2].mean()
    if causal is not None:
        a, b, ramp = causal
        rel = _REL
        lo, hi = a - ramp, b + ramp
        n0 = int(np.argmax(rel >= lo))
        y = signal.lfilter([1, -1], [1, -0.9995], env[:n0] - off)
        c0 = (env[n0 - 1] - off) - y[-1] + off     # x1 - y1, in env units
        env = env.copy()
        sel = (rel >= lo) & (rel < hi)
        r = rel[sel].astype(float)
        g = np.ones_like(r)
        down = r < a
        g[down] = 0.5 * (1 + np.cos(np.pi * (r[down] - lo) / ramp))
        g[(r >= a) & (r < b)] = 0.0
        up = r >= b
        g[up] = 0.5 * (1 - np.cos(np.pi * (r[up] - b) / ramp))
        env[sel] = c0 + g * (env[sel] - c0)
    dcb = signal.lfilter([1, -1], [1, -0.9995], env - off)
    aud = signal.sosfilt(SOS_AF, dcb)
    return env, aud


_REL = None     # set per capture by main(); hold_env needs the time axis


def hold_env(env, a, b, ramp, span=192):
    """Carrier level = the envelope's mean over one modulation-free-ish
    stretch (span samples) just outside the faded region on each side,
    crossfaded across. g = raised cosine: 1 -> 0 over `ramp`, 0 over
    [a, b), 0 -> 1 over `ramp`."""
    rel = _REL
    env = env.copy()
    lo, hi = a - ramp, b + ramp
    m_pre = (rel >= lo - span) & (rel < lo)
    m_post = (rel >= hi) & (rel < hi + span)
    if not m_pre.any() or not m_post.any():
        return env
    c0, c1 = env[m_pre].mean(), env[m_post].mean()
    sel = (rel >= lo) & (rel < hi)
    r = rel[sel].astype(float)
    g = np.ones_like(r)
    down = r < a
    g[down] = 0.5 * (1 + np.cos(np.pi * (r[down] - lo) / ramp))
    g[(r >= a) & (r < b)] = 0.0
    up = r >= b
    g[up] = 0.5 * (1 - np.cos(np.pi * (r[up] - b) / ramp))
    w = (r - lo) / (hi - lo)
    carrier = c0 + (c1 - c0) * w
    env[sel] = carrier + g * (env[sel] - carrier)
    return env


SOS_HP2K = signal.butter(4, 2000, btype="high", fs=FS, output="sos")


def hf_click(rel, aud):
    """The audible part of a click: the audio's residual from the tone,
    above 2 kHz - where a smooth dip in a 1 kHz tone puts nothing and a
    spike puts most of itself. Peak in the 4 ms after the write."""
    res, tone = tone_residual(rel, aud, 960, 1920)
    hf = signal.sosfilt(SOS_HP2K, res)
    w = (rel >= -64) & (rel < 768)
    return np.abs(hf[w]).max(), tone


def tone_residual(rel, env, fit_lo, fit_hi):
    """Envelope minus A + B cos + C sin at 1 kHz (and 2 kHz), fitted on
    [fit_lo, fit_hi) samples relative to the write."""
    t = rel / FS
    cols = [np.ones_like(t)]
    for h in (1, 2):
        cols += [np.cos(2 * np.pi * 1000 * h * t), np.sin(2 * np.pi * 1000 * h * t)]
    A = np.column_stack(cols)
    m = (rel >= fit_lo) & (rel < fit_hi)
    c, *_ = np.linalg.lstsq(A[m], env[m], rcond=None)
    return env - A @ c, abs(c[1] + 1j * c[2])


def click_of(rel, env):
    """Peak of the envelope's departure from the tone in the 3 ms after
    the write, against the tone's own size. The tone is fitted on the
    LATE part (5..10 ms), where the new frequency has settled."""
    res, tone = tone_residual(rel, env, 960, 1920)
    w = (rel >= -32) & (rel < 576)
    pk = np.abs(res[w]).max()
    return pk, tone, res


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    win = None
    if "--win" in sys.argv:
        k = sys.argv.index("--win")
        win = (int(sys.argv[k + 1]), int(sys.argv[k + 2]))
    out = []
    for idx, d in raws(sys.argv[1]):
        if d[:, 0].max() < 1700:
            print(f"retune {idx}: only {int(d[:, 0].max())} samples after "
                  f"the write - too short for the tone fit, skipped")
            continue
        rel, z = front(d)
        a = np.abs(z)
        ph = np.unwrap(np.angle(z))
        pre = rel < -16
        a_pre = np.median(a[pre])
        # sample-level glitch: where |z| (smoothed over 8) falls below a
        # third of its pre-write median
        sm = np.convolve(a, np.ones(8) / 8, mode="same")
        low = (sm < a_pre / 3) & (rel > -64) & (rel < 1024)
        g0 = rel[low].min() if low.any() else None
        g1 = rel[low].max() if low.any() else None
        env0, _ = chain(z)
        pk0, tone, _ = click_of(rel, env0)
        line = (f"retune {idx}: {len(rel)} frames from {int(rel[0])}; "
                f"|z| below a third from {g0} to {g1} samples; "
                f"click {pk0:7.0f} vs tone {tone:6.0f} "
                f"({20*np.log10(pk0/tone):+5.1f} dB)")
        if win:
            env1, _ = chain(bridge(rel, z, *win))
            pk1, _, _ = click_of(rel, env1)
            line += (f"   bridged [{win[0]},{win[1]}): {pk1:7.0f} "
                     f"({20*np.log10(pk1/tone):+5.1f} dB)")
        g = find_glitch(rel, z)
        if g:
            env2, _ = chain(bridge_hermite(rel, z, *g))
            pk2, _, _ = click_of(rel, env2)
            line += (f"\n    glitch found [{g[0]},{g[1]}) = {g[1]-g[0]} samples;"
                     f" Hermite bridge: {pk2:7.0f} ({20*np.log10(pk2/tone):+5.1f} dB)")
        # The metric's own floor: the same residual far from the write,
        # where nothing happened - a repair cannot beat this.
        _, _, res = click_of(rel, env0)
        fl = np.abs(res[(rel >= 300) & (rel < 900)]).max()
        _, aud_f = chain(z)
        hres, ht = tone_residual(rel, aud_f, 960, 1920)
        hfl = np.abs(signal.sosfilt(SOS_HP2K, hres)[(rel >= -1200) & (rel < -200)]).max()
        line += f"\n    AUDIO >2 kHz floor before the write: {hfl:7.0f}"
        line += f"\n    metric floor (300..900 samples, no glitch there): {fl:7.0f}"

        # The audible comparison, on the AUDIO (after the DC blocker and
        # the band-pass): residual above 2 kHz.
        global _REL
        _REL = rel
        _, aud0 = chain(z)
        h0, t0 = hf_click(rel, aud0)
        line += (f"\n    AUDIO >2 kHz: as is {h0:7.0f} "
                 f"({20*np.log10(h0/t0):+5.1f} dB re tone)")
        if g:
            _, aud2 = chain(bridge_hermite(rel, z, *g))
            h2, _ = hf_click(rel, aud2)
            line += f"   hermite {h2:7.0f} ({20*np.log10(h2/t0):+5.1f})"
        for hold in ((16, 224, 96), (16, 288, 128), (0, 320, 192)):
            _, aud3 = chain(z, hold)
            h3, _ = hf_click(rel, aud3)
            _, aud4 = chain(z, causal=hold)
            h4, _ = hf_click(rel, aud4)
            line += (f"\n      hold [{hold[0]},{hold[1]}) ramp {hold[2]}: "
                     f"{h3:7.0f} ({20*np.log10(h3/t0):+5.1f} dB)"
                     f"   CAUSAL {h4:7.0f} ({20*np.log10(h4/t0):+5.1f} dB)")
        print(line)
        out.append((idx, rel, a, ph, env0, z))
    if "--png" in sys.argv and out:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(3, len(out), figsize=(4.2 * len(out), 7.5),
                               squeeze=False, sharex="col")
        for c, (idx, rel, a, ph, env0, z) in enumerate(out):
            s = (rel > -200) & (rel < 600)
            ax[0][c].plot(rel[s], a[s], lw=0.6)
            ax[0][c].set_title(f"retune {idx}")
            ax[0][c].set_ylabel("|z| rotated, raw")
            ax[1][c].plot(rel[s], np.degrees(ph[s] - ph[s][0]), lw=0.6)
            ax[1][c].set_ylabel("phase, deg")
            ax[2][c].plot(rel[s], env0[s], lw=0.8, label="as is")
            if win:
                env1, _ = chain(bridge(rel, z, *win))
                ax[2][c].plot(rel[s], env1[s], lw=0.8, label="bridged")
                ax[2][c].legend(fontsize=7)
            ax[2][c].set_ylabel("channel-filtered |z|")
            ax[2][c].set_xlabel("samples from the write (192 kHz)")
            for row in ax:
                row[c].axvline(0, color="k", lw=0.5, ls=":")
        fig.tight_layout()
        p = sys.argv[sys.argv.index("--png") + 1]
        fig.savefig(p, dpi=90)
        print(f"plot: {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
