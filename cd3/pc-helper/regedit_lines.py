#!/usr/bin/env python3
"""Set values in one key of a Wine .reg file (user.reg / system.reg), keeping its line endings.

    regedit_lines.py FILE 'KEY' 'NAME=VALUE' ...      (KEY as written in the file, e.g. Software\\\\Wine\\\\DllOverrides)

A value line already there is replaced; a missing one is added at the key's end. Values are written as given
(e.g. "native" or str(2):"..."), names are quoted.
"""
import sys

path, key = sys.argv[1], sys.argv[2].replace("/", "\\\\")
pairs = [a.split("=", 1) for a in sys.argv[3:]]
raw = open(path, "rb").read()
nl = "\r\n" if b"\r\n" in raw else "\n"
lines = raw.decode("utf-8").split(nl)
head = "[" + key + "]"
start = next((i for i, l in enumerate(lines) if l.startswith(head + " ") or l == head), None)
if start is None:
    raise SystemExit(f"{path}: no key {head}")
end = start + 1
while end < len(lines) and lines[end].strip() != "":
    end += 1
for name, value in pairs:
    q = '"' + name + '"='
    for i in range(start + 1, end):
        if lines[i].lower().startswith(q.lower()):
            lines[i] = q + value
            break
    else:
        lines.insert(end, q + value)
        end += 1
open(path, "wb").write(nl.join(lines).encode("utf-8"))
print(f"{path}: {head} set {', '.join(n for n, _ in pairs)}")
