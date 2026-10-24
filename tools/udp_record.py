"""Record the stream_udp firmware to a WAV, repairing lost packets.

Each packet is  b"LSP1" | seq (u32 LE) | 512 x int16 LE PCM.
Missing sequence numbers are filled by repeating the last good audio with a
fade-out (same algorithm as src/dsp/GapConcealer.cpp), so the WAV keeps its
real duration and the loss is reported instead of silently shortening it.

    python tools/udp_record.py --seconds 20 --port 12345 --out lung.wav
"""
import argparse
from array import array
import socket
import struct
import sys
import time
import wave

SAMPLE_RATE = 16000
BLOCK = 512
MAGIC = b"LSP1"
HISTORY = 512
FADE = 2048


def _cdiv(a, b):
    """Integer division truncating toward zero, like C++."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


class GapConcealer:
    def __init__(self):
        self.history = []
        self.gap_pos = 0
        self.in_gap = False
        self.concealed = 0
        self.gaps = 0

    def push(self, samples):
        if not len(samples):
            return
        self.history = (list(self.history) + list(samples))[-HISTORY:]
        self.in_gap = False
        self.gap_pos = 0

    def fill(self, n):
        if n and not self.in_gap:
            self.in_gap = True
            self.gaps += 1
        out = []
        for _ in range(n):
            if not self.history or self.gap_pos >= FADE:
                out.append(0)
            else:
                src = self.history[self.gap_pos % len(self.history)]
                out.append(_cdiv(src * (FADE - self.gap_pos), FADE))
            self.gap_pos += 1
        self.concealed += n
        return out


class Reassembler:
    """Orders packets by sequence number and conceals the holes."""
    RESYNC = 1000  # a jump back this far means the device restarted

    def __init__(self):
        self.pcm = array("h")
        self.expected = None
        self.concealer = GapConcealer()
        self.received = 0
        self.lost = 0
        self.late = 0

    def add(self, packet):
        if len(packet) < 8 or packet[:4] != MAGIC or (len(packet) - 8) % 2:
            return False
        seq = struct.unpack_from("<I", packet, 4)[0]
        samples = array("h", packet[8:])
        if sys.byteorder != "little":
            samples.byteswap()
        if self.expected is not None and seq < self.expected:
            if self.expected - seq < self.RESYNC:
                self.late += 1  # duplicate or reordered: already concealed
                return True
            self.expected = seq
        if self.expected is not None and seq > self.expected:
            missing = seq - self.expected
            self.lost += missing
            self.pcm.extend(self.concealer.fill(missing * BLOCK))
        self.concealer.push(samples)
        self.pcm.extend(samples)
        self.received += 1
        self.expected = seq + 1
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=12345)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--out", default=f"lung_{int(time.time())}.wav")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(1.0)
    target = int(args.seconds * SAMPLE_RATE)
    stream = Reassembler()
    print(f"Listening on UDP {args.port}, recording {args.seconds:g} s ...")
    first = None
    try:
        while len(stream.pcm) < target:
            try:
                packet, _ = sock.recvfrom(4096)
            except socket.timeout:
                if first and time.time() - first > args.seconds + 5:
                    print("Stream stopped; saving what was received.")
                    break
                continue
            first = first or time.time()
            stream.add(packet)
    except KeyboardInterrupt:
        print("\nStopped by user.")
    finally:
        sock.close()

    pcm = stream.pcm[:target]
    data = array("h", pcm)
    if sys.byteorder != "little":
        data.byteswap()
    with wave.open(args.out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(data.tobytes())
    total = stream.received + stream.lost
    loss = 100.0 * stream.lost / total if total else 0.0
    peak = max((abs(v) for v in pcm), default=0)
    print(f"Saved {args.out}: {len(pcm) / SAMPLE_RATE:.1f} s, packets {stream.received} ok / "
          f"{stream.lost} lost ({loss:.2f}%) in {stream.concealer.gaps} gap(s), "
          f"{stream.late} late, peak {peak}")
    if loss > 2:
        print("WARNING: >2% loss. Move closer to the AP or use a less busy Wi-Fi channel.")


if __name__ == "__main__":
    main()
