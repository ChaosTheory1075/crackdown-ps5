# Names the addresses in a PS5 log's profiler and stack lines ("ps5 prof:",
# "ps5 stack:"): offsets from rex::ps5diag::NowMs + 0x10000000, the top bit
# marking a library call's caller. symbols.txt.gz is ps5-build's (llvm-nm -C -n).
#
#   py -3 tools/ps5_prof_symbols.py symbols.txt.gz crackdown_001.log [--lines N]
import bisect
import gzip
import re
import sys

syms_path, log_path = sys.argv[1], sys.argv[2]
max_lines = int(sys.argv[sys.argv.index("--lines") + 1]) if "--lines" in sys.argv else 200

addrs, names = [], []
anchor = None
with gzip.open(syms_path, "rt", encoding="utf-8", errors="replace") as f:
    for line in f:
        parts = line.rstrip("\n").split(" ", 2)
        if len(parts) < 3 or parts[1].lower() not in "tw":
            continue
        a = int(parts[0], 16)
        addrs.append(a)
        names.append(parts[2])
        if parts[2] == "rex::ps5diag::NowMs()":
            anchor = a
if anchor is None:
    sys.exit("rex::ps5diag::NowMs() isn't in the symbols")


def name(word):
    lib = word & 0x80000000
    a = (word & 0x7FFFFFFF) - 0x10000000 + anchor
    i = bisect.bisect_right(addrs, a) - 1
    n = f"{names[i]}+{a - addrs[i]:X}" if i >= 0 else "?"
    return ("lib<-" if lib else "") + n


hexword = re.compile(r"\b([0-9A-F]{7,8})\b")
shown = 0
with open(log_path, encoding="utf-8", errors="replace") as f:
    for line in f:
        if "ps5 prof: tid" not in line and "ps5 stack: tid" not in line:
            continue
        head, _, body = line.partition("part 1:" if "part 1:" in line else "|")
        print(head.split("] ")[-1].strip())
        for m in body.split():
            word, _, count = m.partition(":")
            parts = word.split("/")
            if not all(hexword.fullmatch(p) for p in parts):
                continue
            print(f"    {count or '':>6} " + "  <  ".join(name(int(p, 16)) for p in parts))
        shown += 1
        if shown >= max_lines:
            break
