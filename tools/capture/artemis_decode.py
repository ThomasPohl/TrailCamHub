#!/usr/bin/env python3
"""Decrypts ARTEMIS commands from a pcap/pcapng capture of the camera's Wi-Fi.

Usage:
    python3 artemis_decode.py capture.pcap              # everything but heartbeats (525)
    python3 artemis_decode.py capture.pcap --all        # including heartbeats
    python3 artemis_decode.py capture.pcap --cmd 513    # only these cmdIds
    python3 artemis_decode.py capture.pcap --json out.jsonl

Python standard library only. AES through the `cryptography` package when it is
installed, otherwise through the `openssl` command (preinstalled on macOS).
Captures and output contain the camera's screen lock code (and the capture its
Wi-Fi traffic): do not commit them or attach them to issues.
"""
import argparse
import base64
import datetime
import json
import struct
import subprocess
import sys

CAM_IP = bytes([192, 168, 43, 1])
KEY = b"xs38nul7cqf7m1va"
IV = bytes(16)


# ---------------------------------------------------------------- AES
def _aes_decrypt(data: bytes) -> bytes:
    data = data[: len(data) - len(data) % 16]
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        d = Cipher(algorithms.AES(KEY), modes.CBC(IV)).decryptor()
        return d.update(data) + d.finalize()
    except ImportError:
        return subprocess.run(
            ["openssl", "enc", "-d", "-aes-128-cbc", "-nopad",
             "-K", KEY.hex(), "-iv", IV.hex()],
            input=data, capture_output=True, check=True).stdout


def _strip_padding(b: bytes) -> bytes:
    if b and 1 <= b[-1] <= 16 and b.endswith(bytes([b[-1]]) * b[-1]):
        b = b[: -b[-1]]
    return b.rstrip(b"\x00")


# ---------------------------------------------------------------- pcap
def _packets(path):
    """Yields (timestamp_seconds, raw_frame) for pcap and pcapng."""
    with open(path, "rb") as f:
        buf = f.read()
    magic = buf[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        le = magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1")
        nano = magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d")
        e = "<" if le else ">"
        off = 24
        while off + 16 <= len(buf):
            sec, frac, incl, _ = struct.unpack(e + "IIII", buf[off:off + 16])
            off += 16
            yield sec + frac / (1e9 if nano else 1e6), buf[off:off + incl]
            off += incl
    elif magic == b"\x0a\x0d\x0d\x0a":
        off, e, tsres = 0, "<", {}
        iface = 0
        while off + 12 <= len(buf):
            btype = struct.unpack(e + "I", buf[off:off + 4])[0]
            if btype == 0x0A0D0D0A:
                e = "<" if buf[off + 8:off + 12] == b"\x4d\x3c\x2b\x1a" else ">"
                iface = 0
            blen = struct.unpack(e + "I", buf[off + 4:off + 8])[0]
            body = buf[off + 8:off + blen - 4]
            if btype == 1:  # Interface Description
                res = 1e-6
                o = 8
                while o + 4 <= len(body):
                    code, ln = struct.unpack(e + "HH", body[o:o + 4])
                    if code == 0:
                        break
                    if code == 9 and ln >= 1:
                        v = body[o + 4]
                        res = 2.0 ** -(v & 0x7F) if v & 0x80 else 10.0 ** -v
                    o += 4 + ((ln + 3) & ~3)
                tsres[iface] = res
                iface += 1
            elif btype == 6:  # Enhanced Packet
                ifid, hi, lo, cap = struct.unpack(e + "IIII", body[:16])
                ts = ((hi << 32) | lo) * tsres.get(ifid, 1e-6)
                yield ts, body[20:20 + cap]
            elif btype == 3:  # Simple Packet
                yield 0.0, body[4:]
            if blen < 12:
                break
            off += blen
    else:
        sys.exit("Unknown file format (neither pcap nor pcapng).")


def _udp(raw):
    """Finds the IPv4/UDP packet to or from the camera in a frame (any link type, rvi0/PKTAP too)."""
    start = 0
    while True:
        i = raw.find(b"\x45", start)
        if i < 0 or i + 28 > len(raw):
            return None
        start = i + 1
        if raw[i + 9] != 17:  # UDP
            continue
        src, dst = raw[i + 12:i + 16], raw[i + 16:i + 20]
        if CAM_IP not in (src, dst):
            continue
        u = i + 20
        ulen = struct.unpack(">H", raw[u + 4:u + 6])[0]
        payload = raw[u + 8:u + ulen]
        sport, dport = struct.unpack(">HH", raw[u:u + 4])
        return src == CAM_IP, sport, dport, payload


# ---------------------------------------------------------------- ARTEMIS
def decode(path, show_all=False, only=None, jsonl=None):
    state = {}  # message being reassembled per (direction, channel)
    seen = {True: set(), False: set()}
    binary = {}  # packets that are not ARTEMIS JSON per (direction, channel)
    out = open(jsonl, "w") if jsonl else None
    count = 0

    def emit(ts, from_cam, msg, ch):
        nonlocal count
        hdr = msg["data"][:20]
        mtype, seq, plen = struct.unpack("<III", hdr[8:20])
        b64 = msg["data"][20:20 + plen].split(b"\x00", 1)[0]
        try:
            text = _strip_padding(_aes_decrypt(base64.b64decode(b64, validate=True))).decode("utf-8")
        except Exception:  # noqa: BLE001
            # Not Base64/JSON, e.g. the live picture (258 … 259). Only count it.
            binary[(from_cam, ch)] = binary.get((from_cam, ch), 0) + 1
            return
        try:
            obj = json.loads(text)
            cmd = obj.get("cmdId") if isinstance(obj, dict) else None
        except ValueError:
            obj, cmd = None, None
        if cmd == 525 and not show_all:
            return
        if only and cmd not in only:
            return
        t = datetime.datetime.fromtimestamp(msg["ts"]).strftime("%H:%M:%S.%f")[:-3]
        direction = "camera -> app" if from_cam else "app -> camera"
        print(f"{t}  {direction}  {'Resp' if mtype == 3 else 'Req '}  seq={seq:#x}  cmdId={cmd}"
              + (f"  channel {ch}" if ch else ""))
        print("   " + (json.dumps(obj, ensure_ascii=False) if obj is not None else text))
        if out:
            out.write(json.dumps({"time": t, "dir": "cam->app" if from_cam else "app->cam", "channel": ch,
                                  "msgType": mtype, "seq": seq, "cmdId": cmd,
                                  "json": obj if obj is not None else text},
                                 ensure_ascii=False) + "\n")
        count += 1

    for ts, raw in _packets(path):
        p = _udp(raw)
        if not p:
            continue
        from_cam, _, _, pl = p
        if len(pl) < 8 or pl[0] != 0xF1 or pl[1] != 0xD0 or pl[4] != 0xD1:
            continue
        idx = struct.unpack(">H", pl[6:8])[0]
        key = (pl[5], idx)
        if key in seen[from_cam]:
            continue  # retransmission
        seen[from_cam].add(key)
        data = pl[8:]
        sk = (from_cam, pl[5])
        if data.startswith(b"ARTEMIS\x00") and len(data) >= 20:
            plen = struct.unpack("<I", data[16:20])[0]
            state[sk] = {"ts": ts, "data": data, "need": 20 + plen}
        elif state.get(sk) is not None:
            state[sk]["data"] += data
        else:
            binary[sk] = binary.get(sk, 0) + 1  # continuation without a start, e.g. video data
            continue
        m = state[sk]
        if len(m["data"]) >= m["need"]:
            emit(ts, from_cam, m, pl[5])
            state[sk] = None

    if out:
        out.close()
    print(f"\n{count} messages printed.", file=sys.stderr)
    for sk, m in state.items():
        if m is not None:
            binary[sk] = binary.get(sk, 0) + 1  # never completed
    for (fc, ch), n in sorted(binary.items()):
        print(f"Skipped (not JSON, e.g. live picture): {n} packets "
              f"{'camera -> app' if fc else 'app -> camera'}, channel {ch}", file=sys.stderr)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap")
    ap.add_argument("--all", action="store_true", help="also show heartbeats (525)")
    ap.add_argument("--cmd", type=int, action="append", help="only this cmdId (repeatable)")
    ap.add_argument("--json", metavar="FILE", help="also write JSON Lines")
    a = ap.parse_args()
    decode(a.pcap, a.all, set(a.cmd) if a.cmd else None, a.json)
