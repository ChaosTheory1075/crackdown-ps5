# dis.py <missN dir> <fn hex>... [-n N]: each function's first N instructions (default 14)
import sys, bisect
from capstone import *
M = sys.argv[1]; args = sys.argv[2:]; N = 14
if "-n" in args: i = args.index("-n"); N = int(args[i + 1]); del args[i:i + 2]
d = open(M + "/image.bin", "rb").read(); B = 0x82000000
funcs = sorted(int(x, 16) for x in open(M + "/funcs.txt").read().split())
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
for a in args:
    f = int(a, 16); k = bisect.bisect_right(funcs, f); end = funcs[k] if k < len(funcs) else f + 0x400
    ins = list(md.disasm(d[f - B:min(end, f + 4 * N) - B], f))
    print(f"{f:08X} ({end-f:x}): " + " | ".join(f"{i.mnemonic} {i.op_str}" for i in ins))
