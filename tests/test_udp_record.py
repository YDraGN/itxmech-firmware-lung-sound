"""UDP reassembly and concealment in tools/udp_record.py."""
from array import array
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import udp_record as udp  # noqa: E402


def packet(seq, value):
    return b"LSP1" + struct.pack("<I", seq) + array("h", [value] * udp.BLOCK).tobytes()


class ReassemblerTest(unittest.TestCase):
    def test_in_order_stream_is_concatenated(self):
        r = udp.Reassembler()
        for seq in range(3):
            self.assertTrue(r.add(packet(seq, seq + 1)))
        self.assertEqual(len(r.pcm), 3 * udp.BLOCK)
        self.assertEqual((r.lost, r.late), (0, 0))

    def test_lost_packet_is_filled_with_faded_repeat(self):
        r = udp.Reassembler()
        r.add(packet(0, 1000))
        r.add(packet(2, 500))  # seq 1 lost
        self.assertEqual(len(r.pcm), 3 * udp.BLOCK)  # duration preserved
        self.assertEqual(r.lost, 1)
        gap = r.pcm[udp.BLOCK:2 * udp.BLOCK]
        self.assertEqual(gap[0], 1000)
        self.assertLess(gap[-1], 1000)
        self.assertGreater(gap[-1], 0)
        self.assertEqual(r.pcm[2 * udp.BLOCK], 500)

    def test_long_outage_fades_to_silence(self):
        r = udp.Reassembler()
        r.add(packet(0, -3000))
        r.add(packet(10, 1))
        filled = r.pcm[udp.BLOCK:10 * udp.BLOCK]
        self.assertTrue(all(v == 0 for v in filled[udp.FADE:]))
        self.assertTrue(all(v <= 0 for v in filled))  # negative audio fades toward 0

    def test_late_duplicate_dropped_and_restart_resyncs(self):
        r = udp.Reassembler()
        for seq in (5000, 5001, 5000):
            r.add(packet(seq, 7))
        self.assertEqual(r.late, 1)
        r.add(packet(0, 9))  # device rebooted
        self.assertEqual(len(r.pcm), 3 * udp.BLOCK)
        self.assertEqual(r.lost, 0)

    def test_garbage_ignored(self):
        r = udp.Reassembler()
        self.assertFalse(r.add(b"hello"))
        self.assertFalse(r.add(b"XXXX" + bytes(8)))

    def test_matches_firmware_rounding(self):
        c = udp.GapConcealer()
        c.push([7, 9])
        self.assertEqual(c.fill(4), [7, 8, 6, 8])  # same values as tests/host
        c = udp.GapConcealer()
        c.push([-7])
        self.assertEqual(c.fill(1), [-7])
        self.assertEqual(c.fill(1), [-6])  # truncation toward zero, like C++


if __name__ == "__main__":
    unittest.main()
