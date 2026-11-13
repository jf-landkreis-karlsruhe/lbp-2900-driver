#!/usr/bin/env python3
"""
Fake Canon LBP2900 for testing the capt backend without hardware.

Listens on a Unix socket, answers CAPT commands, decodes the Hi-SCoA print
data with an independent decoder and writes every received page as PBM.

  sim_printer.py SOCKET OUTDIR [--no-paper-once] [--small-buffer]

Run the backend against it with DEVICE_URI=capt-sim:SOCKET.
"""
import os
import socket
import struct
import sys
import threading
import time

BAND_LINES = 70


class BitReader:
    def __init__(self, data):
        self.data = bytes(b ^ 0x43 for b in data)
        self.pos = 0

    def bit(self):
        byte = self.data[self.pos >> 3]
        b = (byte >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return b

    def bits(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v

    def align32(self):
        # the encoder always pads with 1..32 one-bits
        self.pos = (self.pos // 32 + 1) * 32


def decode_band(br, line_size, nlines):
    """Decode one Hi-SCoA band starting at br.pos. Returns bytearray."""
    total = line_size * nlines
    out = bytearray()
    origin = [line_size, 0, line_size - 7, 1, 0, 4]
    mru = []
    extra = 0

    def copy_len():
        if br.bit() == 0:
            if br.bit() == 0:
                return 1
            return 2 if br.bit() == 1 else 3
        k = 1
        while k < 6 and br.bit() == 1:
            k += 1
        if k == 6:
            return 0
        nb = k + 1
        r = br.bits(nb)
        return (1 << nb) | (~r & ((1 << nb) - 1))

    def do_copy(cmd):
        nonlocal extra
        length = extra + copy_len()
        extra = 0
        d = origin[cmd]
        if d == 0 or len(out) < d:
            raise ValueError(f"bad copy cmd {cmd} dist {d} at {len(out)}")
        for _ in range(length):
            out.append(out[-d])
        if cmd == 2:
            origin[0], origin[2] = origin[2], origin[0]
        elif cmd == 5:
            origin[3], origin[5] = origin[5], origin[3]

    def literal(b):
        mru.insert(0, b)
        del mru[16:]
        out.append(b)

    while True:
        if br.bit() == 0:
            do_copy(0)
            continue
        if br.bit() == 0:                     # 10 iiii : MRU
            i = 15 - br.bits(4)
            b = mru.pop(i)
            mru.insert(0, b)
            out.append(b)
            continue
        if br.bit() == 0:                     # 110x
            if br.bit() == 0:
                do_copy(2)
            else:
                literal(br.bits(8))
            continue
        if br.bit() == 0:                     # 1110
            do_copy(3)
            continue
        if br.bit() == 0:                     # 11110
            raise ValueError("cmd 4 not expected")
        if br.bit() == 0:                     # 111110
            do_copy(5)
            continue
        sub = br.bits(2)                      # 111111xx
        if sub == 0:                          # long-length prefix
            nb = br.bits(2)
            r = br.bits(nb)
            val = (1 << nb) | (~r & ((1 << nb) - 1))
            extra = val * 128
        elif sub == 1:
            literal(0)
        elif sub == 2:                        # end of band
            eob = br.bits(2)
            br.align32()
            break
        else:
            raise ValueError("unknown code 0xFF")
    if len(out) != total:
        raise ValueError(f"band decoded to {len(out)} bytes, expected {total}")
    return out, eob


class Protocol(Exception):
    pass


class Printer:
    """Models the parts of the printer the driver must respect:
    basic/extended/input status flags, FIFO back-pressure (BufLevel),
    "input status changed" requests that must be serviced, paper-out."""

    def __init__(self, outdir, no_paper_once=False, small_buffer=False):
        self.outdir = outdir
        self.lock = threading.Lock()
        self.offline = True          # printer starts offline (needs go-online sequence)
        self.xchg = False
        self.need_input = True       # printer asks for GetInputStatus at start
        self.need_input_age = 0      # data chunks received while the request is pending
        self.dec = self.prn = self.out = self.done = self.rcv = 0
        self.busy_until = 0.0
        self.no_paper = False
        self.no_paper_once = no_paper_once
        self.button = False
        self.input_ready = True
        self.data = bytearray()
        self.params = None
        self.pages = []
        self.log = []
        self.cap = 60_000 if small_buffer else 2_000_000
        self.buffered = 0            # bytes in the FIFO not yet consumed by the engine
        self.engine_on = False
        self.max_buffered = 0
        self.streamed = False
        self.page_ended = True
        self.stale = False
        self.polls = 0

    # ---- status
    def b1(self):
        v = 0
        if time.time() < self.busy_until:
            v |= 0x80
        if self.offline:
            v |= 0x10
        if self.no_paper:
            v |= 0x12
        return v

    def b2(self):
        return (0x01 if self.xchg else 0) | (0x02 if self.need_input else 0)

    def buflevel(self):
        free = max(0, self.cap - self.buffered)
        return min(15, free * 15 // self.cap)

    def basic(self):
        self.polls += 1
        if self.polls % 37 == 0:     # real printers assert this periodically
            self.need_input = True
        return bytes([self.b1(), self.b2(), 0, 0, self.buflevel(), 0, 0, 0])

    def xstatus(self):
        p = bytearray(40)
        p[0], p[1], p[4] = self.b1(), self.b2(), self.buflevel()
        p[8] = 0x20 if self.button else 0                     # aux
        p[9] = 0x40 if self.no_paper else 0                    # cnt: print rejected
        p[10] = 0x00 if self.no_paper else 0x80                # pap
        struct.pack_into("<HHHH", p, 14, self.dec, self.prn, self.out, self.done)
        struct.pack_into("<H", p, 34, self.rcv)
        self.xchg = False
        self.button = False
        return bytes(p)

    def input_status(self):
        self.need_input = False
        self.need_input_age = 0
        flags = 0xD0 if self.input_ready else 0x80
        return bytes([0, 0x10, flags, 0, 0, 0x02 if self.input_ready else 0,
                      0xb0, 0x09, 0xb3, 0x0d, 0x96, 0, 0xfd, 1, 0, 0, 0, 0])

    def later(self, delay, fn):
        def run():
            time.sleep(delay)
            with self.lock:
                fn()
                self.xchg = True
        threading.Thread(target=run, daemon=True).start()

    def drain(self):
        # engine consumes the FIFO once started
        def run():
            while True:
                time.sleep(0.05)
                with self.lock:
                    if not self.engine_on:
                        return
                    self.buffered = max(0, self.buffered - 20_000)
        threading.Thread(target=run, daemon=True).start()

    # ---- commands
    def handle(self, cmd, payload):
        self.log.append(cmd)
        if cmd == 0xE0A0:
            return self.basic()
        if cmd == 0xA0A8:
            return self.xstatus()
        if cmd == 0xA0A1:
            return self.input_status()
        if cmd == 0xA1A1:
            return bytes.fromhex("000b312a0101f0ff4000040041000100")
        if cmd == 0xA2A0:
            return b"\x00\x00\x2a\x00\x00\x00\x00\x00"   # job id 42
        if cmd in (0xE0A3, 0xE0A2, 0xE0A4):
            self.busy_until = time.time() + 0.5
            if self.input_ready:
                self.no_paper = False
            return b"\x00\x00"
        if cmd == 0xE0A5:                       # GoOnline: resets page counters
            if self.no_paper:
                raise Protocol("GoOnline while paper-out error not cleared")
            self.offline = False
            self.dec = self.prn = self.out = self.done = self.rcv = 0
            self.stale = False
            self.buffered = 0
            self.engine_on = False
            return b"\x00\x00"
        if cmd == 0xE0A6:                       # GoOffline
            self.offline = True
            return b"\x00\x00"
        if self.offline and cmd in (0xD0A9, 0xC0A0):
            raise Protocol(f"{cmd:04X} sent while printer offline")
        if cmd == 0xD0A9:
            if self.stale:
                raise Protocol("page started without resetting the printer after the previous job "
                               "(real printer: data underrun, blank page)")
            self.parse_params(payload)
            self.dec += 1
            self.xchg = True
            self.data = bytearray()
            self.engine_on = False
            self.page_ended = False
            return None
        if cmd == 0xC0A0:
            self.chunks = getattr(self, "chunks", 0) + 1
            if self.chunks % 10 == 0:          # printer raises the request mid-page
                self.need_input = True
            if self.need_input:
                self.need_input_age += 1
                if self.need_input_age > 2:
                    raise Protocol("driver ignores the input-status request while sending data "
                                   "(a real printer stops accepting data here)")
            self.buffered += len(payload)
            self.max_buffered = max(self.max_buffered, self.buffered)
            if self.buffered > self.cap:
                raise Protocol(f"FIFO overflow: {self.buffered} > {self.cap} bytes")
            self.data += payload
            return None
        if cmd == 0xC0A4:
            self.page_ended = True
            self.finish_page()
            return None
        if cmd == 0xE0A7:
            page = struct.unpack("<H", payload[:2])[0]
            if page != self.dec:
                raise Protocol(f"StartPrint for page {page}, decoding {self.dec}")
            if not self.page_ended:
                self.streamed = True             # engine started before end of page data
            self.busy_until = time.time() + 0.3
            self.engine_on = True
            self.drain()
            if self.no_paper_once:
                self.no_paper_once = False

                def jam():
                    self.no_paper = True
                    self.input_ready = False
                    self.engine_on = False
                    self.need_input = True
                    self.offline = True
                    lost, _ = self.pages.pop()       # this sheet never gets printed
                    os.rename(lost, lost + ".lost")

                    def press():
                        self.input_ready = True
                        self.button = True
                        self.need_input = True
                    self.later(3.0, press)
                self.later(0.5, jam)
                return b"\x00\x00"

            def printed():
                if self.no_paper:
                    return
                self.prn += 1
                self.out += 1
                self.done += 1
            self.later(1.0, printed)
            return b"\x00\x00"
        if cmd == 0xE1A2:
            return payload
        if cmd in (0xE1A1, 0xA3A2, 0xE0A9):
            return b"\x00\x00"
        raise Protocol(f"unknown command {cmd:04X}")

    def parse_params(self, payload):
        pos = 0
        while pos < len(payload):
            c, ln = struct.unpack_from("<HH", payload, pos)
            body = payload[pos + 4:pos + ln]
            if c == 0xD0A0:
                assert len(body) == 40, len(body)
                (m1, m2, ls, nl, pw, ph) = struct.unpack_from("<6H", body, 22)
                self.params = dict(size=body[4], media=body[12], adapt=body[13], save=body[19],
                                   margin1=m1, margin2=m2, line_size=ls, lines=nl,
                                   width=pw, height=ph, fm=body[36])
            elif c == 0xD0A4:
                assert body == bytes([1, 4, 1, 1, 0, 0xF9, 0, 0]), body.hex()
            pos += ln
        assert pos == len(payload), "bad D0A9 container"

    def finish_page(self):
        p = self.params
        ls, nl = p["line_size"], p["lines"]
        br = BitReader(self.data)
        img = bytearray()
        y = 0
        while y < nl:
            n = min(BAND_LINES, nl - y)
            band, eob = decode_band(br, ls, n)
            last = y + n >= nl
            if eob != (1 if last else 0):
                raise Protocol(f"band at line {y}: end-of-band type {eob}, expected {1 if last else 0} "
                               "(missing end-of-page leaves the decoder dirty -> underrun on next page)")
            img += band
            y += n
        assert br.pos // 8 == len(self.data), f"{len(self.data) - br.pos // 8} trailing bytes"
        idx = len(self.pages) + 1
        path = os.path.join(self.outdir, f"page{idx:03d}.pbm")
        with open(path, "wb") as f:
            f.write(b"P4\n%d %d\n" % (ls * 8, nl))
            f.write(img)
        self.pages.append((path, dict(p)))
        print(f"sim: page {idx} received: {p} -> {path}", flush=True)

        def received():
            self.rcv += 1
        self.later(0.3, received)


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def serve(sock_path, outdir, no_paper_once, small_buffer, jobs=1):
    os.makedirs(outdir, exist_ok=True)
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    srv.listen(1)
    print("sim: listening", flush=True)
    pr = Printer(outdir, no_paper_once, small_buffer)
    rc = 0
    for job in range(1, jobs + 1):
        conn, _ = srv.accept()
        pr.log = []
        try:
            while True:
                hdr = recv_exact(conn, 4)
                cmd, ln = struct.unpack("<HH", hdr)
                payload = recv_exact(conn, ln - 4)
                with pr.lock:
                    reply = pr.handle(cmd, payload)
                if reply is not None:
                    conn.sendall(struct.pack("<HH", cmd, len(reply) + 4) + reply)
        except EOFError:
            pass
        except Protocol as e:
            print(f"sim: job {job}: PROTOCOL VIOLATION: {e}", flush=True)
            conn.close()
            return 1
        conn.close()
        seq = " ".join(f"{c:04X}" for c in pr.log if c not in (0xE0A0, 0xA0A8, 0xA0A1, 0xC0A0))
        ended = 0xE0A9 in pr.log
        print(f"sim: job {job}: command sequence: {seq}", flush=True)
        print(f"sim: job {job} properly ended: {ended}; max FIFO fill {pr.max_buffered} of {pr.cap} bytes; "
              f"streaming used: {pr.streamed}", flush=True)
        if not ended:
            rc = 1
        if not pr.offline:
            pr.stale = True      # left online: leftover decoder state
    print(f"sim: {len(pr.pages)} page(s) total", flush=True)
    return rc


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    jobs = 1
    for a in sys.argv:
        if a.startswith("--jobs="):
            jobs = int(a.split("=", 1)[1])
    sys.exit(serve(sys.argv[1], sys.argv[2], "--no-paper-once" in sys.argv,
                   "--small-buffer" in sys.argv, jobs))
