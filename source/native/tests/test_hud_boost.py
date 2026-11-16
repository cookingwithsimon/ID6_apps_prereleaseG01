"""Run the real hud-boost worker against a loopback stand-in cluster (SOME/IP 0xB006) and gateway (HSFZ).

Checks the frames it sends: our own start, the boost text and bar picture, the length fields, and
the factory clear at the end. No car needed."""
import socket
import struct
import subprocess
import threading
import unittest
from pathlib import Path

BIN = str(Path(__file__).resolve().parents[1] / "build/hud-boost")
AMBIENT = 0x6589            # DID 4201, 101.54 kPa * 256
BOOST = 180 * 128           # DID 4205, 180 kPa * 128 absolute -> 11.4 psi gauge
CLEAR_FIRST = bytes.fromhex("000000c821000000210000002a0000000221000000210f000000ffffffff07")


def exact(conn, n):
    out = b""
    while len(out) < n:
        data = conn.recv(n - len(out))
        if not data:
            raise EOFError
        out += data
    return out


def listener():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(1)
    s.settimeout(5)
    return s, s.getsockname()[1]


def cluster(srv, frames):
    try:
        conn, _ = srv.accept()
    except OSError:
        return
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.sendall(struct.pack(">HHIHHBBBB", 0xFFFF, 0x8000, 8, 0xDEAD, 0xBEEF, 1, 1, 2, 0))   # magic cookie
    try:
        while True:
            svc, method, length, client, session, _, _, mtype, _ = struct.unpack(">HHIHHBBBB", exact(conn, 16))
            body = exact(conn, length - 8)
            frames.append((svc, method, client, mtype, body))
            # an unrelated event first, then the answer
            conn.sendall(struct.pack(">HHIHHBBBB", 0xB006, 0x8001, 12, 0, 0, 1, 1, 2, 0) + b"\1\2\3\4")
            conn.sendall(struct.pack(">HHIHHBBBB", svc, method, 8, client, session, 1, 1, 0x80, 0))
    except (EOFError, ConnectionError):
        pass
    finally:
        conn.close()


def gateway(srv, reads):
    try:
        conn, _ = srv.accept()
    except OSError:
        return
    try:
        while True:
            length, ctrl = struct.unpack(">IH", exact(conn, 6))
            body = exact(conn, length)
            if ctrl != 1:
                continue
            reads.append(body)
            conn.sendall(struct.pack(">IH", length, 2) + body)                          # ack echo
            if len(reads) == 2:
                conn.sendall(struct.pack(">IH", 2, 0x12) + b"\0\0")                      # alive check
            if len(reads) == 3:
                conn.sendall(struct.pack(">IH", 5, 1) + bytes([0x12, 0xF4, 0x7F, 0x22, 0x78]))   # pending
            value = {0x4201: AMBIENT, 0x4205: BOOST}[(body[3] << 8) | body[4]]
            reply = bytes([0x12, 0xF4, 0x62, body[3], body[4], value >> 8, value & 0xFF])
            conn.sendall(struct.pack(">IH", len(reply), 1) + reply)
    except (EOFError, ConnectionError):
        pass
    finally:
        conn.close()


def text_of(body):
    i = body.index(b"\xfe\xff", 17)
    return body[i + 2:-2].decode("utf-16-be")


def decode_icon(body):
    w, h, n = struct.unpack(">HHI", body[22:30])
    data, px, i = body[30:30 + n], [], 0
    while i < len(data):
        c = data[i]
        i += 1
        if c & 0x80:
            px += [data[i:i + 4]] * (c & 0x7F)
            i += 4
        else:
            px += [data[i + 4 * k:i + 4 * k + 4] for k in range(c)]
            i += 4 * c
    return w, h, px


class HudBoostTest(unittest.TestCase):
    def run_worker(self, *args):
        frames, reads = [], []
        csrv, cport = listener()
        gsrv, gport = listener()
        threads = [threading.Thread(target=cluster, args=(csrv, frames)),
                   threading.Thread(target=gateway, args=(gsrv, reads))]
        for t in threads:
            t.start()
        out = subprocess.run([BIN, "--kombi", "127.0.0.1", "--kombi-port", str(cport),
                              "--gateway", "127.0.0.1", "--gateway-port", str(gport)] + list(args),
                             capture_output=True, text=True, timeout=20)
        for s in (csrv, gsrv):
            try:
                s.close()
            except OSError:
                pass
        threads[0].join(5)
        return out, frames, reads

    def test_live_boost(self):
        out, frames, reads = self.run_worker("--seconds", "3")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertTrue(frames)
        for svc, method, client, mtype, body in frames:
            self.assertEqual((svc, client, mtype), (0xB006, 0x7E01, 0))
            self.assertEqual(body[:4], b"\0\0\0\xc8")
            if len(body) >= 17:
                self.assertEqual(struct.unpack(">I", body[9:13])[0], len(body) - 17)
        methods = [f[1] for f in frames]
        # the factory clear first and last
        self.assertEqual(frames[0][4][:len(CLEAR_FIRST)], CLEAR_FIRST)
        self.assertEqual(methods[-6:], [0x0009, 0x0009, 0x0009, 0x000C, 0x0004, 0x0009])
        self.assertEqual(frames[-6][4], frames[0][4])
        # our own start: idle state, destination info, then text/road/distance/turn/list/commit/state
        self.assertEqual(methods[6:15], [9, 9, 3, 0xE, 0xA, 0xA, 0xC, 4, 9])
        texts = [text_of(f[4]) for f in frames if f[1] == 3 and f[4][4:9] == bytes.fromhex("0300000003")]
        self.assertEqual(texts[0], "BOOST")
        self.assertIn("BOOST 11.4 psi", texts)
        icons = [f[4] for f in frames if f[1] == 3 and f[4][4:9] == bytes.fromhex("0100000001")]
        self.assertTrue(icons)
        w, h, px = decode_icon(icons[-1])
        self.assertEqual((w, h, len(px)), (52, 33, 52 * 33))
        orange = sum(1 for x in range(4, 48) if px[16 * 52 + x] == b"\xff\x00\x66\xff")
        self.assertEqual(orange, 25)          # 11.4 of 20 psi across 44 columns
        # only read-only UDS 0x22 of 4201 and 4205 to the DME
        for body in reads:
            self.assertEqual(body[:3], b"\xf4\x12\x22")
            self.assertIn(body[3:5], (b"\x42\x01", b"\x42\x05"))

    def test_demo_needs_no_gateway(self):
        out, frames, reads = self.run_worker("--demo", "--seconds", "2", "--start", "1")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(reads, [])
        self.assertEqual([f[1] for f in frames][6:13], [3, 0xE, 0xA, 0xA, 0xC, 4, 9])


if __name__ == "__main__":
    unittest.main()
