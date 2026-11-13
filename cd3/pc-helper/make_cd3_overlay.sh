#!/usr/bin/env bash
# Crackdown 3's Prospero Win runtime overlay (run in WSL on our build PC): what our installer writes over a stock
# Prospero Win (PPSA99995/win/wine/lib/wine/...), so it has what Crackdown 3 played with on 2026-11-11.
#   make_cd3_overlay.sh OUT          (OUT = .../Crackdown 3/install-data)
# - x86_64-windows: wine-uwp's PE build (the UWP app model: windows.*, winexboxauth, combase, ...) with today's fixes,
#   shell32 (item without a listable parent), then pw-merge's own PE modules on top (kernelbase LoadPackagedLibrary,
#   ntdll/win32u/winevulkan/xinput... that must match the unix side).
# - x86_64-unix: pw-merge's 14 signed PRX (ntdll preload + view blocks, ws2_32 DNS fallback, ...), WRAPPED: the
#   console decrypts a signed file for any reader, so a copy made by reading it on the console would be the plain
#   ELF it refuses (0x80020008). Each is stored as "CD3PRXW1" + the signed bytes and unwrapped by the installer.
set -euo pipefail
OUT=$1
DIST=/root/cd3/wine-uwp-dist/lib/wine/x86_64-windows
PE=/root/cd3/pw-ps5work/pe/x86_64-windows
PRX=/root/cd3/pw-ps5work/prx/sce_module
SHELL32=/root/cd3/wine-uwp-build/dlls/shell32/x86_64-windows/shell32.dll
XBA=/root/cd3/wine-uwp-build/dlls/winexboxauth/x86_64-windows/winexboxauth.dll
O=$OUT/overlay
rm -rf "$O"
mkdir -p "$O/x86_64-windows" "$O/x86_64-unix"
cp -a "$DIST/." "$O/x86_64-windows/"
cp -f "$SHELL32" "$O/x86_64-windows/shell32.dll"
cp -f "$XBA" "$O/x86_64-windows/winexboxauth.dll"
cp -f "$PE/." "$O/x86_64-windows/" 2>/dev/null || cp -f "$PE"/* "$O/x86_64-windows/"
for f in "$PRX"/*.prx; do
  n=$(basename "$f")
  [ "$(head -c4 "$f" | xxd -p)" = 5414f5ee ] || { echo "not a signed PRX: $f" >&2; exit 1; }
  { printf 'CD3PRXW1'; cat "$f"; } > "$O/x86_64-unix/$n.w"
done
( cd "$O" && find . -type f -printf '%P\t%s\n' | sort ) > "$OUT/overlay-files.tsv"
echo "overlay: $(find "$O/x86_64-windows" -type f | wc -l) PE files, $(ls "$O/x86_64-unix" | wc -l) PRX, $(du -sh "$O" | cut -f1)"
