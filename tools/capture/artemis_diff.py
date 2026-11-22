#!/usr/bin/env python3
"""Compares 512/513 messages across JSON Lines files written by artemis_decode.py.

Usage:
    python3 artemis_diff.py run1.jsonl run2.jsonl ...
    python3 artemis_diff.py *.jsonl --cmd 512 --cmd 513

Prints every request in full and, for every response, only the fields that
changed since the previous response with the same cmdId. Fields that change all
the time are skipped with --ignore (default: token, utcTime).
"""
import argparse
import json


def flat(o, prefix=""):
    if isinstance(o, dict):
        for k, v in o.items():
            yield from flat(v, f"{prefix}.{k}" if prefix else k)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            yield from flat(v, f"{prefix}[{i}]")
    else:
        yield prefix, o


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--cmd", type=int, action="append", help="cmdIds (default: 512 and 513)")
    ap.add_argument("--ignore", action="append", default=["token", "utcTime"],
                    help="field names that do not count as a change")
    a = ap.parse_args()
    cmds = set(a.cmd or [512, 513])
    last = {}
    for fn in a.files:
        print(f"===== {fn}")
        with open(fn, encoding="utf-8") as f:
            for line in f:
                m = json.loads(line)
                if m.get("cmdId") not in cmds or not isinstance(m.get("json"), dict):
                    continue
                cmd, req = m["cmdId"], m["dir"] == "app->cam"
                head = f"{m['time']}  {'app -> camera' if req else 'camera -> app'}  cmdId={cmd}"
                if req:
                    print(f"{head}  REQUEST\n   {json.dumps(m['json'], ensure_ascii=False)}")
                    continue
                cur = dict(flat(m["json"]))
                prev = last.get(cmd)
                last[cmd] = cur
                if prev is None:
                    print(f"{head}  first response, {len(cur)} fields:")
                    for k, v in cur.items():
                        print(f"   {k} = {v!r}")
                    continue
                ch = [(k, prev.get(k, "<missing>"), cur.get(k, "<missing>"))
                      for k in sorted(set(prev) | set(cur))
                      if prev.get(k, "<missing>") != cur.get(k, "<missing>")
                      and k.split(".")[-1] not in a.ignore]
                print(f"{head}  {len(ch) or 'no'} change(s)")
                for k, o, n in ch:
                    print(f"   {k}: {o!r} -> {n!r}")


if __name__ == "__main__":
    main()
