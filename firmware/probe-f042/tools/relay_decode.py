#!/usr/bin/env python3
"""relay_decode.py — decode the P5a probe relay stream from the ST-LINK VCP.

Mirrors firmware/probe-f042/src/relay.c. Frame (01-debug-link-protocol.md §3):

    FLAG | TYPE SEQ LEN  ts[4 LE] data[N]  CRC8 | FLAG
    0x7E |  \\______ covered by CRC8 (poly 0x07) ______/  | 0x7E
    SLIP stuffing inside the frame: 0x7E->0x7D 0x5E, 0x7D->0x7D 0x5D.

Live:     ./relay_decode.py /dev/ttyACM0            (needs pyserial)
From file:./relay_decode.py --file capture.bin
Self-test:./relay_decode.py --selftest             (no hardware/deps)

This is a P5a bench bring-up helper; the canonical decoder is the single-source
cdl_proto codec (P1) used by debug-monitor (P4).
"""
import argparse
import sys

FLAG, ESC, ESC_FLAG, ESC_ESC = 0x7E, 0x7D, 0x5E, 0x5D
FRAME_TYPE = 0xF0     # timestamped data
STATUS_TYPE = 0xF1    # cumulative dropped-byte count


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


class Deframer:
    """Feed bytes; yields decoded frames as dicts. Resyncs on FLAG."""

    def __init__(self):
        self._buf = bytearray()
        self._in = False
        self._esc = False
        self._bad = False           # invalid escape seen in the current frame

    def feed(self, chunk: bytes):
        for b in chunk:
            if b == FLAG:
                if self._in and self._buf:
                    # A FLAG arriving mid-escape (ESC then FLAG) is a dangling
                    # escape — the frame is truncated, not a valid body.
                    if self._bad or self._esc:
                        yield {"error": "escape", "raw": bytes(self._buf).hex()}
                    else:
                        frame = self._decode(bytes(self._buf))
                        if frame is not None:
                            yield frame
                self._buf.clear()
                self._in = True
                self._esc = False
                self._bad = False
            elif not self._in:
                continue
            elif b == ESC:
                self._esc = True
            elif self._esc:
                if b == ESC_FLAG:
                    self._buf.append(FLAG)
                elif b == ESC_ESC:
                    self._buf.append(ESC)
                else:
                    self._bad = True            # malformed escape -> drop frame
                self._esc = False
            else:
                self._buf.append(b)

    @staticmethod
    def _decode(inner: bytes):
        if len(inner) < 5:                              # too short
            return {"error": "malformed", "raw": inner.hex()}
        body, crc = inner[:-1], inner[-1]
        if crc8(body) != crc:
            return {"error": "crc", "raw": inner.hex()}
        typ, seq, length = body[0], body[1], body[2]
        if length + 3 != len(body) or length < 4:
            return {"error": "len", "raw": inner.hex()}
        if typ == STATUS_TYPE:
            return {"type": typ, "seq": seq, "dropped": int.from_bytes(body[3:7], "little")}
        ts = int.from_bytes(body[3:7], "little")
        return {"type": typ, "seq": seq, "ts_us": ts, "data": bytes(body[7:])}


def _render(f) -> str:
    if "error" in f:
        return f"  !! {f['error']:8s} raw={f['raw']}"
    if f["type"] == STATUS_TYPE:
        return f"  ** STATUS seq={f['seq']:3d}  dropped={f['dropped']}"
    tag = "" if f["type"] == FRAME_TYPE else f" type=0x{f['type']:02X}"
    return (f"[{f['ts_us']:>10d} us] seq={f['seq']:3d}{tag}  "
            f"{f['data'].hex(' ') or '(empty)'}")


def _encode(seq: int, ts: int, data: bytes) -> bytes:
    """Reference encoder (test only) — matches relay.c."""
    inner = bytes([FRAME_TYPE, seq, 4 + len(data)]) + ts.to_bytes(4, "little") + data
    inner += bytes([crc8(inner)])
    out = bytearray([FLAG])
    for b in inner:
        out += bytes([ESC, ESC_FLAG]) if b == FLAG else \
               bytes([ESC, ESC_ESC]) if b == ESC else bytes([b])
    out.append(FLAG)
    return bytes(out)


def _check(cond: bool, msg: str):
    if not cond:                          # explicit (asserts are stripped under -O)
        raise AssertionError(msg)


def selftest() -> int:
    d = Deframer()
    payloads = [b"\x01\x7e\x7d\xaa", b"", bytes(range(10))]
    stream = b"".join(_encode(i, 1000 + i, p) for i, p in enumerate(payloads))
    # split stream across chunk boundaries to exercise the state machine
    got = []
    for i in range(0, len(stream), 3):
        got.extend(d.feed(stream[i:i + 3]))
    _check(len(got) == len(payloads), f"frame count {len(got)} != {len(payloads)}")
    for i, (f, p) in enumerate(zip(got, payloads)):
        _check("error" not in f, f"frame {i}: {f}")
        _check(f["seq"] == i and f["ts_us"] == 1000 + i and f["data"] == p,
               f"frame {i} mismatch: {f}")
    # corrupted CRC -> reported as error, doesn't desync the next frame
    bad = bytearray(_encode(9, 5, b"\xde\xad")); bad[-2] ^= 0xFF
    out = list(d.feed(bytes(bad) + _encode(10, 6, b"\xbe\xef")))
    _check(any("error" in f for f in out) and any(f.get("seq") == 10 for f in out), str(out))
    # dangling escape (body then ESC then FLAG) -> escape error, resyncs cleanly
    out = list(d.feed(b"\x7e\xaa\xbb\x7d\x7e" + _encode(11, 7, b"\x01")))
    _check(any(f.get("error") == "escape" for f in out), f"dangling escape: {out}")
    _check(any(f.get("seq") == 11 for f in out), f"resync after dangling escape: {out}")
    print("relay_decode selftest: ALL PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Decode the P5a probe VCP relay stream.")
    ap.add_argument("port", nargs="?", help="serial port, e.g. /dev/ttyACM0")
    # Matches PROBE_VCP_BAUD in src/probe_pins.h (P5a ST-LINK VCP). For the P5b
    # native USB-CDC link the host ignores the baud, so any value works there.
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--file", help="decode a captured binary file instead of a port")
    ap.add_argument("--selftest", action="store_true", help="run offline self-test")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    d = Deframer()
    if args.file:
        with open(args.file, "rb") as fh:
            for f in d.feed(fh.read()):
                print(_render(f))
        return 0
    if not args.port:
        ap.error("give a serial port, --file, or --selftest")
    try:
        import serial  # pyserial, only needed for live capture
    except ImportError:
        print("pyserial not installed: pip install pyserial (or use --file)", file=sys.stderr)
        return 2
    with serial.Serial(args.port, args.baud, timeout=1) as s:
        print(f"# reading {args.port} @ {args.baud} (Ctrl-C to stop)", file=sys.stderr)
        try:
            while True:
                chunk = s.read(256)
                if chunk:
                    for f in d.feed(chunk):
                        print(_render(f), flush=True)
        except KeyboardInterrupt:
            return 0


if __name__ == "__main__":
    raise SystemExit(main())
