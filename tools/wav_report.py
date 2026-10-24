"""Pure-Python WAV check (no numpy): level, clipping, band energy, transients.

    python tools/wav_report.py recording.wav [other.wav ...]
"""
import array
import math
import sys
import wave

BANDS = [("<50 Hz rumble/handling", None, 50), ("50-150 Hz heart/rumble", 50, 150),
         ("150-600 Hz breath core", 150, 600), ("600-1200 Hz breath high", 600, 1200),
         ("1.2-2.5 kHz crackle/wheeze", 1200, 2500), (">2.5 kHz hiss", 2500, None)]


def biquad(kind, f0, fs, q=0.7071):
    w = 2 * math.pi * f0 / fs
    a = math.sin(w) / (2 * q)
    c = math.cos(w)
    if kind == "lp":
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]
    else:
        b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    a0 = 1 + a
    return [x / a0 for x in b], [-2 * c / a0, (1 - a) / a0]


def run(coefs, x):
    (b0, b1, b2), (a1, a2) = coefs
    y = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def band(x, lo, hi, fs):
    y = x
    if lo:
        y = run(biquad("hp", lo, fs), run(biquad("hp", lo, fs), y))
    if hi:
        y = run(biquad("lp", hi, fs), run(biquad("lp", hi, fs), y))
    return y


def rms(v):
    return math.sqrt(sum(s * s for s in v) / len(v)) if v else 0.0


def db(v, ref=32768.0):
    return 20 * math.log10(v / ref) if v > 0 else -999


def report(path, skip=0.3):
    with wave.open(path, "rb") as w:
        ch, sw, fs, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    # Every format is scaled to 16-bit full scale so levels are comparable.
    if sw == 2:
        data = array.array("h", raw)
        if sys.byteorder != "little":
            data.byteswap()
        x = [float(v) for v in data[::ch]]
    elif sw in (3, 4):
        step = sw * ch
        scale = 2.0 ** (8 * sw - 16)
        x = [int.from_bytes(raw[i:i + sw], "little", signed=True) / scale
             for i in range(0, len(raw) - step + 1, step)]
    else:
        raise SystemExit(f"{path}: unsupported sample width {sw}")
    # Opening the serial port resets the board; drop that click from the stats.
    x = x[int(skip * fs):]
    print(f"== {path}")
    print(f"format: {ch} ch, {8 * sw} bit, {fs} Hz, {len(x) / fs:.2f} s analysed (first {skip:g} s skipped)")
    mean = sum(x) / len(x)
    peak = max(abs(v) for v in x)
    total = rms(x)
    knee = sum(1 for v in x if abs(v) >= 16000)
    ceil = sum(1 for v in x if abs(v) >= 27990)
    print(f"DC {mean:.1f}  RMS {total:.0f} ({db(total):.1f} dBFS)  peak {peak:.0f} ({db(peak):.1f} dBFS)  "
          f"crest {db(peak, total):.1f} dB")
    print(f"samples >=16000 (soft limiter): {knee} ({100 * knee / len(x):.2f}%)   at ceiling 28000: {ceil}")
    zc = sum(1 for a, b in zip(x, x[1:]) if (a < 0) != (b < 0))
    print(f"zero-crossing rate: {zc / (len(x) / fs):.0f} /s")

    print("band energy (share of total power):")
    powers = []
    for name, lo, hi in BANDS:
        p = rms(band(x, lo, hi, fs)) ** 2
        powers.append((name, p))
    tp = sum(p for _, p in powers) or 1
    for name, p in powers:
        print(f"  {name:<28} {100 * p / tp:5.1f}%   {db(math.sqrt(p)):6.1f} dBFS")

    frame = fs // 20  # 50 ms
    frames = [rms(x[i:i + frame]) for i in range(0, len(x) - frame + 1, frame)]
    srt = sorted(frames)
    med = srt[len(srt) // 2]
    p10, p90 = srt[len(srt) // 10], srt[9 * len(srt) // 10]
    loud = [i for i, f in enumerate(frames) if f > 4 * med]
    print(f"50 ms frame RMS: p10 {p10:.0f}  median {med:.0f}  p90 {p90:.0f}  max {srt[-1]:.0f}  "
          f"(p90/p10 {db(p90, p10):.1f} dB)")
    print(f"transient frames (>4x median): {len(loud)} of {len(frames)}"
          + (f", at s: {', '.join(f'{i * 0.05:.2f}' for i in loud[:15])}" if loud else ""))
    # breath band envelope over time (150-1200 Hz), 0.5 s resolution
    b = band(x, 150, 1200, fs)
    half = fs // 2
    env = [rms(b[i:i + half]) for i in range(0, len(b) - half + 1, half)]
    print("breath-band (150-1200 Hz) RMS per 0.5 s:")
    print("  " + " ".join(f"{e:.0f}" for e in env))
    print()


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("wav", nargs="+")
    parser.add_argument("--skip", type=float, default=0.3,
                        help="seconds dropped from the start (board reset click), default 0.3")
    args = parser.parse_args()
    for p in args.wav:
        report(p, args.skip)
