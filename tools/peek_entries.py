# peek.py <missN dir> <base hex>: the candidates' first instructions, and a verdict: "fn" (a prologue,
# or reads no r14-r31 before writing them) or "label" (reads a callee-saved register first: a switch label)
import struct, sys, re
from capstone import *
from capstone.ppc import *
M = sys.argv[1]
d = open(M + "/image.bin", "rb").read(); B = 0x82000000
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN); md.detail = True
out = []
for line in open(M + "/missing2.txt"):
    m = re.match(r"\s*([0-9A-F]{7,8}) (data|code)", line)
    if not m: continue
    a = int(m.group(1).rjust(8, "8"), 16)
    ins = list(md.disasm(d[a - B:a - B + 40], a))
    text = " | ".join(f"{i.mnemonic} {i.op_str}" for i in ins[:6])
    verdict = "fn"
    if ins and not (ins[0].mnemonic in ("mflr", "stwu", "stdu") or (ins[0].mnemonic == "bl")):
        written = set()
        for i in ins[:10]:
            regs = re.findall(r"r(\d+)", i.op_str)
            mn = i.mnemonic
            if mn.startswith(("st", "cmp", "tw", "mt", "b")) or not regs:
                reads, writes = set(regs), set()
            else:
                reads, writes = set(regs[1:]), {regs[0]}
            if mn.startswith("st") and i.op_str.endswith("(r1)") and regs:
                reads.discard(regs[0])  # (a save of the register isn't a use)
            if any(str(n) in reads and str(n) not in written for n in range(14, 32)):
                verdict = "label"; break
            written |= writes
            if i.mnemonic in ("blr", "b", "bctr"): break
    out.append((a, m.group(2), verdict, text))
for a, k, v, t in out:
    print(f"{a:08X} {k:4} {v:5} {t}")
