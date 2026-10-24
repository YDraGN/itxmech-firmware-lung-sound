"""Record the stream_serial firmware (raw int16 LE PCM at 921600 baud) to a WAV.

    pip install pyserial
    python tools/serial_record.py --port COM11 --seconds 20 --out lung.wav
"""
import argparse
import time
import wave

import serial

SAMPLE_RATE = 16000


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", required=True, help="e.g. COM11 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--out", default=f"lung_serial_{int(time.time())}.wav")
    args = parser.parse_args()

    target = int(args.seconds * SAMPLE_RATE) * 2
    audio = bytearray()
    with serial.Serial(args.port, args.baud, timeout=2) as port:
        time.sleep(2)  # the board resets when the port opens; skip boot text
        port.reset_input_buffer()
        print(f"Recording {args.seconds:g} s from {args.port} ...")
        deadline = time.time() + args.seconds + 5
        while len(audio) < target and time.time() < deadline:
            audio.extend(port.read(min(4096, target - len(audio))))
    if len(audio) % 2:
        audio = audio[:-1]
    with wave.open(args.out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(bytes(audio))
    print(f"Saved {args.out}: {len(audio) / 2 / SAMPLE_RATE:.1f} s")
    if len(audio) < target:
        print("WARNING: fewer bytes than expected; check baud rate and USB cable.")


if __name__ == "__main__":
    main()
