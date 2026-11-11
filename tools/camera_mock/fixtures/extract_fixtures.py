#!/usr/bin/env python3
"""Extract the camera mock's fixtures from real captures.

The mock answers the hub with what a real camera sent, not with what the hub's
own code would build (#122). This script reads pcaps of the TrailCam Go app
talking to a camera of the TC100 family, reassembles the camera's ARTEMIS
responses and writes them, still encrypted and Base64-encoded exactly as
captured, to core/capture_fixtures.cpp.

The captures are those of https://github.com/pwnb0t/trailcam-automation
(directory pcap/, a PH970S camera, firmware 2.3.6.5):

    git clone --depth 1 https://github.com/pwnb0t/trailcam-automation
    tools/camera_mock/fixtures/extract_fixtures.py trailcam-automation/pcap

Only the Python standard library and `cryptography` (to tell the responses
apart by their decrypted cmdId) are needed.
"""

import argparse
import base64
import json
import pathlib
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

COMMAND_KEY = b"xs38nul7cqf7m1va"
CAMERA_PORT = 40611
OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "core" / "capture_fixtures.cpp"

# (fixture name, pcap file, cmdId, pageNo or None)
WANTED = [
    ("kLoginResponse", "trailcam_2-1-connect.pcap", 0, None),
    ("kDeviceInfoResponse", "trailcam_2-1-connect.pcap", 512, None),
    ("kMediaListPage0Response", "trailcam_2-1-connect.pcap", 768, 0),
    ("kMediaListPage1Response", "trailcam_2-3-view-photo.pcap", 768, 1),
]


def decrypt(b64):
    raw = base64.b64decode(b64)
    decryptor = Cipher(algorithms.AES(COMMAND_KEY), modes.CBC(bytes(16))).decryptor()
    plain = decryptor.update(raw) + decryptor.finalize()
    return plain.split(b"\0")[0].decode()


def pcap_records(path):
    data = path.read_bytes()
    magic, = struct.unpack("<I", data[:4])
    if magic not in (0xA1B2C3D4, 0xA1B23C4D):
        raise SystemExit(f"{path}: not a little-endian pcap file")
    link, = struct.unpack("<I", data[20:24])
    offset = 24
    while offset + 16 <= len(data):
        _, _, incl, _ = struct.unpack("<IIII", data[offset:offset + 16])
        offset += 16
        yield link, data[offset:offset + incl]
        offset += incl


def udp_datagrams(path):
    """(source port, destination port, payload) of each IPv4 UDP datagram."""
    for link, frame in pcap_records(path):
        if link == 1:  # Ethernet
            frame = frame[14:]
        elif link == 113:  # Linux cooked capture
            frame = frame[16:]
        elif link != 101:  # raw IP
            continue
        if len(frame) < 20 or frame[0] >> 4 != 4 or frame[9] != 17:
            continue
        ihl = (frame[0] & 0x0F) * 4
        sport, dport, length = struct.unpack(">HHH", frame[ihl:ihl + 6])
        yield sport, dport, frame[ihl + 8:ihl + length]


def camera_messages(path):
    """ARTEMIS messages sent by the camera: (msg_type, app_seq, b64, padding)."""
    received = {}
    pending = set()  # indices of first fragments not complete yet
    for sport, _, payload in udp_datagrams(path):
        if sport != CAMERA_PORT or len(payload) < 8 or payload[0] != 0xF1:
            continue
        if payload[1] == 0x42 and payload[4:8] == b"LBCS":
            received = {}  # a new session numbers its DATA from 0 again
            pending = set()
            continue
        if payload[1] != 0xD0 or payload[4] != 0xD1 or payload[5] != 0:
            continue
        index, = struct.unpack(">H", payload[6:8])
        if index in received:
            continue  # retransmission
        received[index] = payload[8:]
        if received[index].startswith(b"ARTEMIS\0") and len(received[index]) >= 20:
            pending.add(index)
        for first in sorted(pending):
            body = received[first]
            msg_type, app_seq, length = struct.unpack("<III", body[8:20])
            message = bytearray(body[20:])
            next_index = first + 1
            while len(message) < length and next_index in received:
                message += received[next_index]
                next_index += 1
            if len(message) < length:
                continue
            pending.discard(first)
            message = bytes(message[:length])
            b64 = message.split(b"\0")[0].decode()
            yield msg_type, app_seq, b64, length - len(b64), first


def find(path, cmd_id, page):
    seen = set()
    for msg_type, app_seq, b64, padding, index in camera_messages(path):
        if msg_type != 3 or (app_seq, index) in seen:
            continue
        seen.add((app_seq, index))
        obj = json.loads(decrypt(b64))
        if obj.get("cmdId") != cmd_id:
            continue
        if page is not None and obj.get("pageNo") != page:
            continue
        return app_seq, b64, padding, obj
    raise SystemExit(f"{path.name}: no response with cmdId {cmd_id} page {page}")


def literal(text, indent="    "):
    chunk = 64
    return "\n".join(f'{indent}"{text[i:i + chunk]}"' for i in range(0, len(text), chunk))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pcap_dir", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path, default=OUTPUT)
    args = parser.parse_args()

    out = [
        "// Generated by tools/camera_mock/fixtures/extract_fixtures.py from the",
        "// captures of https://github.com/pwnb0t/trailcam-automation (pcap/,",
        "// PH970S, firmware 2.3.6.5). Do not edit; run the script again.",
        "//",
        "// Each body is the camera's ARTEMIS response exactly as captured:",
        "// Base64(AES-128-CBC(JSON)), followed on the wire by `padding` zero",
        "// bytes (the NUL and the camera's zero padding, counted in PayloadLen).",
        "// clang-format off",
        "",
        '#include "capture_fixtures.hpp"',
        "",
        "namespace camera_mock::fixtures",
        "{",
        "",
    ]
    for name, pcap, cmd_id, page in WANTED:
        app_seq, b64, padding, obj = find(args.pcap_dir / pcap, cmd_id, page)
        summary = f"cmdId {cmd_id}" + (f", pageNo {page}, {obj.get('cnt')} entries" if page is not None else "")
        out += [
            f"// {pcap}, AppSeq {app_seq:#x}: {summary}.",
            f"const CapturedResponse {name} = {{",
            literal(b64) + ",",
            f"    {padding},",
            "};",
            "",
        ]
        print(f"{name}: {summary}, {len(b64)} Base64 chars, padding {padding}", file=sys.stderr)
    out += ["}  // namespace camera_mock::fixtures", ""]
    args.output.write_text("\n".join(out))
    print(f"wrote {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
