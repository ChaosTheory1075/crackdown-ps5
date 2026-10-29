#!/usr/bin/env python3
"""The PC recomp's overrides with only the codegen hooks the PS5 build ports.

    filter_overrides.py <overrides.toml> <hook source>... <out overrides.toml>

[functions] (analysis fixes) stay as they are. A [[midasm_hook]] whose function
the hook sources don't define is left out, so codegen emits the original
instructions there (the Xbox 360 behaviour): the PC-only hooks (frame pacing,
mouse look, the quit menu, ...) don't reach the PS5 code at all.
"""
import re
import sys
import tomllib

src, ported_srcs, out = sys.argv[1], sys.argv[2:-1], sys.argv[-1]
text = open(src, encoding="utf-8").read()
ported = set()
for ported_src in ported_srcs:
    ported |= set(re.findall(r"^(?:void|bool) (crackdown_\w+)\(", open(ported_src, encoding="utf-8").read(), re.M))

head, *blocks = re.split(r"(?m)^(?=\[\[midasm_hook\]\]\s*$)", text)
kept, dropped = [], []
for b in blocks:
    name = re.search(r'(?m)^name\s*=\s*"([^"]+)"', b).group(1)
    (kept if name in ported else dropped).append((name, b))
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write(head + "".join(b for _, b in kept))
tomllib.load(open(out, "rb"))  # still valid TOML

missing = ported - {n for n, _ in kept}
print(f"{len(blocks)} hooks: {len(kept)} kept ({', '.join(sorted({n for n, _ in kept}))}), {len(dropped)} left out")
if missing:
    sys.exit(f"filter_overrides: ported hooks not in the overrides: {sorted(missing)}")
