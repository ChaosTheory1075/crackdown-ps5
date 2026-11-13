#!/usr/bin/env python3
"""Crackdown 3 for PS5 - the PC helper: takes the Windows 11 files Crackdown 3 needs out of Microsoft's own
Windows 11 ISO, on your PC, and puts them in one folder you copy into the Crackdown app's install/ folder.

    python cd3_windows_files.py --download                 (downloads Microsoft's Windows 11 evaluation ISO, 7 GB)
    python cd3_windows_files.py --iso Win11_eval.iso       (an ISO you already have: it must be that same image)
    options: --out FOLDER (default: "Crackdown 3 Windows files" beside this script)  --keep-iso

Needs Python 3.8+ and 7-Zip (https://www.7-zip.org, the normal 64-bit install). Nothing of Microsoft's comes with
the Crackdown app: these files come from Microsoft's server to your PC and stay yours. The game itself you dump with
UWPDumper from your own copy (Store / Game Pass) and copy into install/ as well.

Every file is checked against the list the console was tested with (ms-files.tsv beside this script); a different
Windows image is refused, since the console setup is made for exactly this one.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ISO_URL = "https://go.microsoft.com/fwlink/?linkid=2334167"   # Microsoft Evaluation Center, Windows 11 Enterprise
ISO_SIZE = 7092807680
ISO_SHA256 = "a61adeab895ef5a4db436e0a7011c92a2ff17bb0357f58b13bbc4062e535e7b9"   # 25H2 build 26200.6584 en-US
MARKER = "cd3-windows.txt"


def say(msg):
    print(msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def seven_zip():
    for p in (shutil.which("7z"), r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe"):
        if p and os.path.exists(p):
            return p
    sys.exit("7-Zip isn't installed. Get it from https://www.7-zip.org and run this again.")


def download(dest):
    part = dest + ".part"
    done = os.path.getsize(part) if os.path.exists(part) else 0
    req = urllib.request.Request(ISO_URL, headers={"User-Agent": "Crackdown3-PS5-helper",
                                                   **({"Range": f"bytes={done}-"} if done else {})})
    say("Downloading the Windows 11 evaluation ISO from Microsoft (7 GB; it continues if stopped)...")
    with urllib.request.urlopen(req, timeout=60) as r, open(part, "ab" if done and r.status == 206 else "wb") as f:
        if r.status != 206:
            done = 0
        last = time.time()
        while chunk := r.read(1 << 20):
            f.write(chunk)
            done += len(chunk)
            if time.time() - last > 2:
                say(f"  {done / 1e9:.2f} of {ISO_SIZE / 1e9:.2f} GB")
                last = time.time()
    os.replace(part, dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--iso", help="Microsoft's Windows 11 evaluation ISO (25H2, 26200.6584, en-US)")
    g.add_argument("--download", action="store_true", help="download it from Microsoft first")
    ap.add_argument("--out", default=os.path.join(HERE, "Crackdown 3 Windows files"))
    ap.add_argument("--keep-iso", action="store_true", help="keep the downloaded ISO")
    ap.add_argument("--wim", help=argparse.SUPPRESS)  # an already extracted install.wim (tests)
    args = ap.parse_args()

    rows = []
    with open(os.path.join(HERE, "ms-files.tsv"), encoding="utf-8") as f:
        for line in f:
            target, src, size, digest = line.rstrip("\n").split("\t")
            rows.append((target, src, int(size), digest))
    wanted = {}
    for _, src, size, digest in rows:
        wanted[src] = (size, digest)
    say(f"{len(wanted)} Windows files to take out of the ISO.")

    sz = seven_zip()
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    work = tempfile.mkdtemp(prefix="cd3-helper-", dir=os.path.dirname(os.path.abspath(args.out)))
    iso = args.iso
    try:
        if args.download:
            iso = os.path.join(work if not args.keep_iso else os.path.dirname(os.path.abspath(args.out)),
                               "Win11_Enterprise_eval.iso")
            if not os.path.exists(iso):
                download(iso)
        wim = args.wim
        if not wim:
            if not os.path.exists(iso):
                sys.exit(f"No such file: {iso}")
            if os.path.getsize(iso) != ISO_SIZE:
                sys.exit("This isn't the Windows 11 evaluation ISO Crackdown 3 is set up for (its size differs). "
                         "Use --download to get the right one from Microsoft.")
            say("Checking the ISO (a minute)...")
            if sha256(iso) != ISO_SHA256:
                sys.exit("This ISO isn't the exact Windows 11 evaluation image Crackdown 3 is set up for. "
                         "Use --download to get it from Microsoft.")
            say("Unpacking the Windows image from the ISO (a few minutes)...")
            subprocess.run([sz, "e", "-y", f"-o{work}", iso, "sources/install.wim"], check=True,
                           stdout=subprocess.DEVNULL)
            wim = os.path.join(work, "install.wim")
        # the image's file list: the first edition folder ("1/...") holds a full Windows
        out = subprocess.run([sz, "l", "-slt", wim], check=True, capture_output=True, text=True,
                             errors="replace").stdout
        names = [l[7:] for l in out.splitlines() if l.startswith("Path = ")]
        prefix = next((n.split("\\")[0] + "\\" for n in names if n.lower().endswith("\\windows\\system32\\ntdll.dll")),
                      "")
        by_lower = {n[len(prefix):].replace("\\", "/").lower(): n for n in names if n.startswith(prefix)}
        missing = [s for s in wanted if s.lower() not in by_lower]
        if missing:
            sys.exit(f"{len(missing)} needed files aren't in this image (first: {missing[0]}). Wrong ISO?")
        listfile = os.path.join(work, "list.txt")
        with open(listfile, "w", encoding="utf-8") as f:
            for s in wanted:
                f.write(by_lower[s.lower()] + "\n")
        say("Taking the files out...")
        stage = os.path.join(work, "x")
        subprocess.run([sz, "x", "-y", f"-o{stage}", wim, f"@{listfile}"], check=True, stdout=subprocess.DEVNULL)

        if os.path.exists(args.out):
            shutil.rmtree(args.out)
        bad = 0
        for s, (size, digest) in wanted.items():
            src = os.path.join(stage, by_lower[s.lower()])
            dst = os.path.join(args.out, *s.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.move(src, dst)
            if os.path.getsize(dst) != size or sha256(dst) != digest:
                bad += 1
                say(f"  differs: {s}")
        if bad:
            sys.exit(f"{bad} files differ from the tested ones; the console setup can't use this image.")
        shutil.copy(os.path.join(HERE, "ms-files.tsv"), os.path.join(args.out, "ms-files.tsv"))
        with open(os.path.join(args.out, MARKER), "w", encoding="utf-8") as f:
            f.write("Crackdown 3 for PS5: Windows 11 files (25H2 26200.6584 evaluation), "
                    f"{len(wanted)} files, all checked.\n")
        say(f"Done: {args.out}\nCopy that whole folder into the Crackdown app's install/ folder on the PS5, "
            "next to your Crackdown 3 dump.")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
