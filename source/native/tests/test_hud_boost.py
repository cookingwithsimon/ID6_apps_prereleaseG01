"""Run the real hud-boost worker against a loopback stand-in cluster (SOME/IP 0xB006) and gateway (HSFZ).

Checks the frames it sends: our own start, the boost text and bar picture, the length fields, and
the factory clear at the end. No car needed."""
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

BIN = str(Path(__file__).resolve().parents[1] / "build/hud-boost")
AMBIENT = 0x6589            # DID 4201, 101.54 kPa * 256
BOOST = 180 * 128           # DID 4205, 180 kPa * 128 absolute -> 11.4 psi gauge
# drive mode the stand-in body controller reports: (seconds since connect, D611 value)
MODES = [(1, 4), (2, 1), (3, 2), (4, 3)]
MARK = "Jan 01 00:26:42    4 20003     2 pid 262204: MARKER "
NOISE = "Jan 01 00:23:10    1 32768     0 dev-ioamp-router McBSP_0_RX"
# lines from Simon's route start and cancel (2026-11-18)
ROUTE_START = [MARK + "attach TDA on guidance viewer:GUIDING LABELS", MARK + "RouteGuidanceStarted",
               MARK + "MapViewer: API call --> enable route: routeId 2094",
               MARK + "GuidanceNewRoute 2094, quality 1, state:0, for guided trip 1",
               MARK + "MapViewer: API call --> enable route: routeId 2098",
               MARK + "MapViewer: API call --> disable route: routeId 2094",
               MARK + "attach BJV on guidance viewer:GUIDING LABELS for man id:1"]
ROUTE_END = [MARK + "attach EMPTY on guidance viewer:GUIDING LABELS for man id:0",
             MARK + "MapViewer: API call --> disable route: routeId 2098"]
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
    """Takes connections one after another (the worker reconnects after handing the HUD back)
    until the test closes the listening socket."""
    while True:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            return
        cluster_conn(conn, frames)


def cluster_conn(conn, frames):
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
    started = time.monotonic()
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
            if body[1] == 0x40:                                                         # BDC drive mode
                elapsed = time.monotonic() - started
                mode = next((m for until, m in MODES if elapsed < until), MODES[-1][1])
                reply = bytes([0x40, 0xF4, 0x62, body[3], body[4], mode])
                conn.sendall(struct.pack(">IH", len(reply), 1) + reply)
                continue
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


def fake_sloginfo(folder, steps):
    """A stand-in `sloginfo -w`: steps are (seconds to wait first, lines to print)."""
    path = os.path.join(folder, "sloginfo")
    with open(path, "w") as f:
        f.write("#!/bin/sh\n")
        for wait, lines in steps:
            if wait:
                f.write("sleep %s\n" % wait)
            for line in lines:
                f.write("echo '%s'\n" % line)
        f.write("exec sleep 60\n")
    os.chmod(path, 0o755)
    return path


class HudBoostTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.quiet_log = fake_sloginfo(self.tmp.name, [(0, [NOISE] + ROUTE_START + ROUTE_END)])

    def tearDown(self):
        self.tmp.cleanup()

    def run_worker(self, *args, sloginfo=None):
        frames, reads = [], []
        csrv, cport = listener()
        gsrv, gport = listener()
        threads = [threading.Thread(target=cluster, args=(csrv, frames)),
                   threading.Thread(target=gateway, args=(gsrv, reads))]
        for t in threads:
            t.start()
        out = subprocess.run([BIN, "--kombi", "127.0.0.1", "--kombi-port", str(cport),
                              "--gateway", "127.0.0.1", "--gateway-port", str(gport),
                              "--sloginfo", sloginfo or self.quiet_log] + list(args),
                             capture_output=True, text=True, timeout=20)
        for s in (csrv, gsrv):
            try:
                s.close()
            except OSError:
                pass
        threads[0].join(5)
        return out, frames, reads

    def test_live_boost(self):
        out, frames, reads = self.run_worker("--seconds", "4")
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
        out, frames, reads = self.run_worker("--demo", "--seconds", "3", "--start", "1")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(reads, [])
        self.assertEqual([f[1] for f in frames][6:13], [3, 0xE, 0xA, 0xA, 0xC, 4, 9])

    def test_watch_mode(self):
        out, frames, reads = self.run_worker("--watch-mode", "--seconds", "5")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(frames, [])                     # nothing sent to the cluster
        for body in reads:
            self.assertEqual(body[:5], b"\xf4\x40\x22\xd6\x11")
        lines = [l for l in out.stdout.splitlines() if l.startswith("drive mode:")]
        self.assertEqual(lines[:4], ["drive mode: raw 04  COMFORT", "drive mode: raw 01  SPORT",
                                     "drive mode: raw 02  SPORT+", "drive mode: raw 03  SPORT Individual"])


    def test_auto_follows_sport_modes(self):
        global MODES
        saved = MODES
        MODES = [(1.5, 4), (4.5, 1), (99, 4)]      # COMFORT, then SPORT for 3 s, then COMFORT
        try:
            out, frames, reads = self.run_worker("--auto", "--seconds", "7")
        finally:
            MODES = saved
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("Drive mode: SPORT (1)", out.stdout)
        self.assertIn("HUD cleared (not a sport mode).", out.stdout)
        methods = [f[1] for f in frames]
        clear = [0x0009, 0x0009, 0x0009, 0x000C, 0x0004, 0x0009]
        # nothing before SPORT; then clear + start + updates; then the clear when COMFORT returns, and nothing after
        self.assertEqual(methods[:6], clear)
        self.assertEqual(methods[-6:], clear)
        self.assertIn(3, methods)
        texts = [text_of(f[4]) for f in frames if f[1] == 3 and f[4][4:9] == bytes.fromhex("0300000003")]
        self.assertIn("BOOST 11.4 psi", texts)
        self.assertTrue(any(body[1] == 0x40 for body in reads))

    def test_watch_nav(self):
        log = fake_sloginfo(self.tmp.name, [(0, [NOISE] + ROUTE_START + ROUTE_END), (2, ROUTE_START), (2, ROUTE_END)])
        out = subprocess.run([BIN, "--watch-nav", "--sloginfo", log, "--seconds", "6"],
                             capture_output=True, text=True, timeout=20)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        states = [l for l in out.stdout.splitlines() if l.startswith("Factory nav:")]
        # the replayed route that already ended does not count; then the live start and cancel
        self.assertEqual(states, ["Factory nav: not guiding", "Factory nav: guiding", "Factory nav: not guiding"])

    def test_nav_stands_aside(self):
        # guidance from 2.5 s to 4.5 s; HUD back 3 s after it ends
        log = fake_sloginfo(self.tmp.name, [(0, [NOISE] + ROUTE_START + ROUTE_END), (2.5, ROUTE_START), (2, ROUTE_END)])
        out, frames, reads = self.run_worker("--demo", "--seconds", "10", sloginfo=log)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        lines = out.stdout.splitlines()
        self.assertIn("HUD cleared (factory nav is guiding).", lines)
        self.assertEqual(sum(1 for l in lines if l.startswith("HUD started")), 2, out.stdout)
        clear = [0x0009, 0x0009, 0x0009, 0x000C, 0x0004, 0x0009]
        self.assertEqual([f[1] for f in frames][-6:], clear)

    def test_no_sloginfo_no_hud(self):
        out, frames, reads = self.run_worker("--demo", "--seconds", "3", sloginfo="/nonexistent/sloginfo")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("Cannot start /nonexistent/sloginfo -w", out.stdout)
        self.assertEqual(frames, [])


if __name__ == "__main__":
    unittest.main()
