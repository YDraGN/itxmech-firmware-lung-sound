"""Preview other MIC_HPF_HZ / MIC_LPF_HZ values on an existing 16-bit recording.

Applies an extra 4th-order Butterworth high-pass and/or low-pass (the same
sections as src/dsp/LungFilter.cpp) so settings can be compared by ear before
reflashing:

    python tools/refilter.py in.wav --hpf 50 --lpf 1200 --out in_hp50_lp1200.wav
"""
import argparse
import array
import math
import sys
import wave

BUTTER_Q = (0.54119610, 1.30656296)


def section(kind, f0, fs, q):
    w = 2 * math.pi * f0 / fs
    alpha, c = math.sin(w) / (2 * q), math.cos(w)
    a0 = 1 + alpha
    if kind == "lp":
        b = ((1 - c) / 2, 1 - c, (1 - c) / 2)
    else:
        b = ((1 + c) / 2, -(1 + c), (1 + c) / 2)
    return [v / a0 for v in b], (-2 * c / a0, (1 - alpha) / a0)


def run(x, coefs):
    (b0, b1, b2), (a1, a2) = coefs
    z1 = z2 = 0.0
    out = []
    for v in x:
        y = b0 * v + z1
        z1 = b1 * v - a1 * y + z2
        z2 = b2 * v - a2 * y
        out.append(y)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("wav")
    parser.add_argument("--hpf", type=float, help="extra high-pass corner, Hz")
    parser.add_argument("--lpf", type=float, help="extra low-pass corner, Hz")
    parser.add_argument("--gain", type=float, default=1.0)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    with wave.open(args.wav, "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            raise SystemExit("expects a 16-bit mono WAV from the device")
        fs = w.getframerate()
        data = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder != "little":
        data.byteswap()
    x = [float(v) for v in data]
    for kind, f0 in (("hp", args.hpf), ("lp", args.lpf)):
        if f0:
            for q in BUTTER_Q:
                x = run(x, section(kind, f0, fs, q))
    out = array.array("h", (max(-32768, min(32767, int(round(v * args.gain)))) for v in x))
    if sys.byteorder != "little":
        out.byteswap()
    with wave.open(args.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(out.tobytes())
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
