#!/usr/bin/env python3
"""Drives the serial console of the firmware (hub_console, "TCH" protocol).

Runs commands, follows the log and copies files from the SD card over the
console port (UART or USB-Serial-JTAG). Needs pyserial (part of the ESP-IDF
Python environment). The diagnostic commands (heap, ls, cat, rm, sdbench)
need CONFIG_HUB_CONSOLE_DIAGNOSTICS.

  dongle_console.py run status
  dongle_console.py run scan --follow 300 --until "TCH EVENT done"
  dongle_console.py run rm -r /sdcard/<camera>
  dongle_console.py get logs/log-00003.txt ./log-00003.txt
  dongle_console.py get-tree <camera> ./sdcard-copy
  dongle_console.py log 60

Output lines of the firmware are printed as they arrive; "TCH DATA" lines are
hidden. A command's reply is every "TCH" line up to "TCH DONE" (asynchronous
"TCH EVENT" lines aside); it failed if a "TCH ERR" line came. The port is
opened without changing DTR and RTS, so the chip is not reset.
"""

import argparse
import base64
import os
import re
import sys
import time
import zlib

import serial

DEFAULT_PORT = os.environ.get("ESPPORT", "/dev/ttyACM0")


class Console:
    def __init__(self, port, echo=True):
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 115200
        self.serial.timeout = 0.2
        # Two readers would split the output between them.
        self.serial.exclusive = True
        # The USB-Serial-JTAG port resets the chip when RTS is set while DTR
        # is not. Opening the port sets both at once; pyserial would then
        # clear DTR before RTS, so both stay set (the default).
        self.serial.open()
        self.pending = b""
        self.echo = echo

    def lines(self, deadline):
        """Yields received lines (without line ending) until the deadline."""
        while time.monotonic() < deadline:
            chunk = self.serial.read(4096)
            if not chunk:
                continue
            self.pending += chunk
            *complete, self.pending = self.pending.split(b"\n")
            for raw in complete:
                line = raw.rstrip(b"\r").decode("utf-8", "replace")
                if self.echo and not line.startswith("TCH DATA "):
                    print(line, flush=True)
                yield line

    def send(self, command):
        self.serial.write(command.encode() + b"\n")
        self.serial.flush()

    def replies(self, command, timeout):
        """Sends a command and yields its reply lines, without "TCH ", up to
        "TCH DONE"."""
        self.send(command)
        for line in self.lines(time.monotonic() + timeout):
            if not line.startswith("TCH ") or line.startswith("TCH EVENT "):
                continue
            reply = line[4:]
            if reply.strip() == "DONE":
                return
            yield reply
        raise RuntimeError("no answer to %r" % command)

    def run(self, command, timeout=30.0):
        """Runs a command; returns (ok, reply lines)."""
        try:
            output = list(self.replies(command, timeout))
        except RuntimeError as error:
            return False, [str(error)]
        return not any(line.startswith("ERR") for line in output), output

    def read_range(self, path, offset, length, timeout):
        """Returns ({offset: bytes}, file size, sent range, crc) of one cat."""
        command = "cat %s %d" % (path, offset)
        if length >= 0:
            command += " %d" % length
        chunks = {}
        size = None
        end = None
        for line in self.replies(command, timeout):
            if line.startswith("FILE "):
                size = int(line.split()[1])
            elif line.startswith("DATA "):
                parts = line.split(" ")
                if len(parts) != 3:
                    continue
                try:
                    chunks[int(parts[1])] = base64.b64decode(parts[2],
                                                             validate=True)
                except ValueError:
                    pass  # a damaged line; requested again as a gap
            elif line.startswith("CRC "):
                _, start, sent, crc = line.split()
                end = (int(start), int(sent), int(crc, 16))
            elif line.startswith("ERR") and size is None:
                raise RuntimeError("%s: %s" % (path, line))
        return chunks, size, end

    def get(self, path, timeout_per_mb=60.0, attempts=5):
        """Copies a file; requests lost lines again and checks the CRC."""
        chunks, size, end = self.read_range(path, 0, -1,
                                            30 + timeout_per_mb)
        if size is None:
            raise RuntimeError("%s: no TCH FILE line" % path)
        for _ in range(attempts):
            gaps = find_gaps(chunks, size)
            if not gaps:
                break
            for gap_start, gap_length in gaps:
                more, _, _ = self.read_range(
                    path, gap_start, gap_length,
                    30 + timeout_per_mb * gap_length / 1e6)
                chunks.update(more)
        if find_gaps(chunks, size):
            raise RuntimeError("%s: lines still missing" % path)
        data = b"".join(chunks[o] for o in sorted(chunks))[:size]
        if end is not None and end[1] == size and \
                zlib.crc32(data) != end[2]:
            raise RuntimeError("%s: CRC mismatch" % path)
        return data


def find_gaps(chunks, size):
    gaps = []
    position = 0
    for offset in sorted(chunks):
        if offset > position:
            gaps.append((position, offset - position))
        position = max(position, offset + len(chunks[offset]))
    if position < size:
        gaps.append((position, size - position))
    return gaps


def list_dir(console, path):
    ok, output = console.run("ls " + path)
    if not ok:
        raise RuntimeError("ls %s: %s" % (path, output[-1:]))
    entries = []
    for line in output:
        match = re.match(r"INFO ([df]) (\d+) (-?\d+) (.+)$", line)
        if match:
            entries.append((match.group(1) == "d", int(match.group(2)),
                            match.group(4)))
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", default=DEFAULT_PORT)
    sub = parser.add_subparsers(dest="action", required=True)

    run = sub.add_parser("run", help="run a console command")
    run.add_argument("command", nargs=argparse.REMAINDER)
    run.add_argument("--timeout", type=float, default=30)
    run.add_argument("--follow", type=float, default=0,
                     help="then print the log for this many seconds")
    run.add_argument("--until", help="stop following at a matching line")

    log = sub.add_parser("log", help="print the log")
    log.add_argument("seconds", type=float)
    log.add_argument("--until", help="stop at a matching line")

    get = sub.add_parser("get", help="copy a file from the SD card")
    get.add_argument("path")
    get.add_argument("local")

    tree = sub.add_parser("get-tree", help="copy a directory tree")
    tree.add_argument("path")
    tree.add_argument("local")

    args = parser.parse_args()
    # --follow/--until may follow the command words.
    if args.action == "run":
        words = args.command
        for flag in ("--follow", "--until", "--timeout"):
            if flag in words:
                index = words.index(flag)
                value = words[index + 1]
                setattr(args, flag[2:],
                        value if flag == "--until" else float(value))
                del words[index:index + 2]
        args.command = " ".join(words)

    console = Console(args.port, echo=args.action in ("run", "log"))
    if args.action == "run":
        ok, _ = console.run(args.command, args.timeout)
        if args.follow:
            follow(console, args.follow, args.until)
        sys.exit(0 if ok else 1)
    elif args.action == "log":
        follow(console, args.seconds, args.until)
    elif args.action == "get":
        data = console.get(args.path)
        with open(args.local, "wb") as file:
            file.write(data)
        print("%s: %d bytes" % (args.local, len(data)))
    elif args.action == "get-tree":
        get_tree(console, args.path.strip("/"), args.local)


def follow(console, seconds, until):
    pattern = re.compile(until) if until else None
    for line in console.lines(time.monotonic() + seconds):
        if pattern and pattern.search(line):
            return


def get_tree(console, path, local):
    os.makedirs(local, exist_ok=True)
    for is_dir, size, name in list_dir(console, path):
        remote = path + "/" + name if path else name
        target = os.path.join(local, name)
        if is_dir:
            get_tree(console, remote, target)
        else:
            started = time.monotonic()
            data = console.get(remote)
            with open(target, "wb") as file:
                file.write(data)
            elapsed = time.monotonic() - started
            print("%s: %d bytes, %.0f KB/s" %
                  (remote, len(data), len(data) / 1024 / max(elapsed, 1e-3)),
                  flush=True)


if __name__ == "__main__":
    main()
