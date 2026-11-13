#!/usr/bin/env bash
# The PS5 build (Ubuntu 24.04; WSL works), from your own default.xex and Title
# Update 6 Default.xexp: codegen, compile, link and package the app folder.
#
#   ci/local_build.sh <work dir> <repo> <framework setup.sh'd> <ps5-plume-triangle built> \
#                     <default.xex> <default.xexp> [<app assets> [<Crackdown 2 default.xex> <its TU5 Default.xexp>]]
#
# <app assets>: the user's own dashboard art and music (kept out of git), laid
# out as the app folder - sce_sys/{icon0.png,pic0.dds,pic1.dds,snd0.at9},
# hub/{background.jpg,logo.png,music/} - copied over the built app folder.
#
# The work dir keeps its build trees and ccache between runs; the framework is
# synced back to pristine each time by content (the patch scripts expect a clean tree). Start it from
# a Windows process that stays open (WSL stops the VM once no session holds it).
#
# CD_PTBR_BIN (default /root/cdtext/ptbr/ptbr.bin): Crackdown's Brazilian Portuguese text, made from your own
# game files by Recomps/ptbr/crackdown (build.py, build_sub.py, build_ui.py, pack.py); incbin'd into the app,
# which writes game/Local/pt-BR from it (cd_ptbr_install.cpp). Missing: English only.
set -euo pipefail
work=$1 repo=$2 fw_src=$3 tri=$4 xex=$5 xexp=$6 assets=${7:-} xex2=${8:-} xexp2=${9:-}
# Crackdown 2 (cd2.bin): its disc's default.xex and Title Update 5's Default.xexp
XEX2_SHA256=8959c449e382661d29b8ea479c685f506cd5bd3eca40b3d73a51cc10f281069f
XEXP2_SHA256=a5e49fc1ff7f6f65e6f95a7b5c4b9392bb9e38674d1237ac0e2f0149d05929d7
XEX_SHA256=7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a
XEXP_SHA256=a0d4fe06d6a4a2fd96d18f5de426f761fe1b8cce5d5bc7f981d738c3519c819e
CD_PC_REPO=https://github.com/saulob/crackdown-xbox360-recompiled
CD_PC_COMMIT=33ab1b632e38cb5c1c39a3d56c479537b9e0d185
TITLE_ID=PPSA53070
export CCACHE_DIR=$work/ccache

mkdir -p "$work"
cd "$work"
rm -rf out && mkdir -p out
echo "== framework"
# Into the persistent tree by content, WITHOUT the source's mtimes (no -t): a file the patches changed last time (or a
# patch since removed) is rewritten with a new mtime, so ninja recompiles it; unchanged files keep theirs. "cp -a"
# kept fw-clean's old mtimes, and a file put back to its pristine state looked older than its object: stale objects
# (ABI mismatches and a SpirvShaderTranslator crash, both games, 11-05).
mkdir -p fw
rsync -rlpgoD --checksum --delete "$fw_src/" fw/
git -C fw rev-parse HEAD | tee fw-commit.txt
python3 "$repo/ci/patch_framework.py" fw > out/patch_framework.txt
M=fw/glue/rexglue-sdk-main/crackdown-recomp
rm -rf $M && cp -r "$repo/crackdown-recomp" $M

echo "== PC recomp and the perf fixes"
if [ ! -d cd ]; then
  git init -q cd
  git -C cd remote add origin "$CD_PC_REPO"
fi
if [ "$(git -C cd rev-parse HEAD 2>/dev/null)" != "$CD_PC_COMMIT" ]; then
  git -C cd fetch -q --depth 1 origin "$CD_PC_COMMIT"
  git -C cd checkout -q FETCH_HEAD
fi
python3 "$repo/ci/fw_perf_fixes.py" fw cd | tee out/fw_perf_fixes.txt
# GPU-thread speed work from Saints Row / Max Payne 3 (shaders and pipelines off the GPU thread, pipeline cache, ...)
python3 "$repo/ci/fw_speed_fixes.py" fw | tee out/fw_speed_fixes.txt
python3 "$repo/ci/fw_texture_fixes.py" fw | tee out/fw_texture_fixes.txt
python3 "$repo/ci/fw_ring_fixes.py" fw | tee out/fw_ring_fixes.txt
python3 "$repo/ci/fw_submit_thread.py" fw | tee out/fw_submit_thread.txt
python3 "$repo/ci/fw_draw_skip.py" fw | tee out/fw_draw_skip.txt
python3 "$repo/ci/fw_xma_fixes.py" fw | tee out/fw_xma_fixes.txt
python3 "$repo/ci/fw_tiling_fixes.py" fw | tee out/fw_tiling_fixes.txt
python3 "$repo/ci/fw_readback_fixes.py" fw | tee out/fw_readback_fixes.txt
python3 "$repo/ci/fw_fence_fixes.py" fw | tee out/fw_fence_fixes.txt
python3 "$repo/ci/fw_frame_sync.py" fw | tee out/fw_frame_sync.txt
python3 "$repo/ci/fw_orb_fix.py" fw | tee out/fw_orb_fix.txt
python3 "$repo/ci/fw_tile_replay.py" fw | tee out/fw_tile_replay.txt
python3 "$repo/ci/fw_draw_worker.py" fw | tee out/fw_draw_worker.txt
python3 "$repo/ci/fw_tiling_log.py" fw | tee out/fw_tiling_log.txt
python3 "$repo/ci/fw_one_tile_scissor.py" fw | tee out/fw_one_tile_scissor.txt
# (marker hunt diagnostics, done 2026-11-12: off, they logged every capture frame and dumped textures)
# python3 "$repo/ci/fw_marker_log.py" fw | tee out/fw_marker_log.txt
# (marker hunt diagnostics, done 2026-11-12: off, they logged every capture frame and dumped textures)
# python3 "$repo/ci/fw_marker_log2.py" fw | tee out/fw_marker_log2.txt
python3 "$repo/ci/fw_odepth_float24.py" fw | tee out/fw_odepth_float24.txt
python3 "$repo/ci/fw_lane_fence.py" fw | tee out/fw_lane_fence.txt
python3 "$repo/ci/fw_scratch_now.py" fw | tee out/fw_scratch_now.txt
python3 "$repo/ci/fw_release_fixes.py" fw | tee out/fw_release_fixes.txt
mkdir -p $M/config
cp cd/config/tu6/discovered-functions.toml $M/config/
python3 "$repo/ci/filter_overrides.py" cd/config/tu6/overrides.toml "$repo/crackdown-recomp/src/hooks_ps5.cpp" \
  "$repo/crackdown-recomp/src/frame_pacing_ps5.cpp" $M/config/overrides.toml > out/hooks.txt
# The PS5's own Jump Height hook (cheats_ps5.cpp): right after Title Update 6's jump launch works
# out the height (fmuls f30,f1,f31 at 0x8231EA64), before r30 is loaded.
cat >> $M/config/overrides.toml <<'TOML'

[[midasm_hook]]
address = 0x8231EA68
name = "crackdown_jump_height"
registers = ["f30", "r11"]
after_instruction = false
TOML
cp cd/src/massive_server_guard.h cd/src/visibility_grid_guard.h cd/src/audio_pcm_firewall.h \
  cd/src/guest_memory.h cd/src/frame_timing.h cd/src/character_step_fixes.h \
  cd/src/vehicle_action_step.h cd/src/audio_wave_timeout.h cd/src/xaudio_barrier.h \
  cd/src/font_atlas.h cd/src/font_atlas.cpp \
  cd/src/cheats.h cd/src/cheats.cpp cd/src/cheat_hooks.h cd/src/cheat_hooks.cpp cd/src/guest_hooks.h \
  cd/src/guest_layout.h cd/src/orb_map.h cd/src/orb_map.cpp cd/src/orb_map_rules.h cd/src/aspect_ratio.h $M/src/
# The orb map's markers drawn with the game's frames, not every vblank (a repaint every vblank queues
# repeats ahead of the game's frames on the PS5).
python3 - "$M/src/orb_map.cpp" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
old = "  explicit OrbMapOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}\n"
assert t.count(old) == 1, "orb_map.cpp changed"
open(p, "w").write(t.replace(old, old + "  bool WantsContinuousRepaint() const override { return false; }  // [ps5]\n"))
PY
# The minimap's orb dots drawn for the frame that is on screen (rex/cd_frame_sync.h, ci/fw_frame_sync.py):
# the dots are worked out on the game's thread, and the GPU thread shows frames a few behind it on the
# PS5, so the newest dots slid against the turning minimap (the user: "the green orbs are not locked in
# where they are and vary with the camera", 2026-11-11).
python3 - "$M/src/orb_map.cpp" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
def rep(old, new):
    global t
    assert t.count(old) == 1, "orb_map.cpp changed: " + old[:60]
    t = t.replace(old, new)
rep('#include "settings.h"\n', '#include "settings.h"\n\n#include <deque>\n\n#include <rex/cd_frame_sync.h>  // [ps5]\n')
rep("  Clock::time_point time;\n};\n",
    "  Clock::time_point time;\n  uint32_t swap = 0;  // [ps5] the game frame (its VdSwap number) the dots are for\n};\n")
rep("Frame g_minimap;  // guarded by g_minimap_mutex\n",
    "Frame g_minimap;  // guarded by g_minimap_mutex\n"
    "std::deque<Frame> g_minimap_history;  // [ps5] the last frames' dots, oldest first, guarded by g_minimap_mutex\n")
rep("  frame.time = Clock::now();\n  std::lock_guard lock(g_minimap_mutex);\n  g_minimap = std::move(frame);\n",
    "  frame.time = Clock::now();\n"
    "  frame.swap = rex::cd_frame_sync::cpu_swaps.load(std::memory_order_relaxed) + 1;  // [ps5] the frame being built\n"
    "  std::lock_guard lock(g_minimap_mutex);\n"
    "  g_minimap_history.push_back(frame);  // [ps5]\n"
    "  while (g_minimap_history.size() > 32) {\n    g_minimap_history.pop_front();\n  }\n"
    "  g_minimap = std::move(frame);\n")
rep("        minimap = g_minimap;\n",
    "        minimap = g_minimap;\n"
    "        // [ps5] the dots of the frame on screen, not of the newest one the game has worked out\n"
    "        const uint32_t shown = rex::cd_frame_sync::gpu_swaps.load(std::memory_order_relaxed);\n"
    "        for (auto it = g_minimap_history.rbegin(); it != g_minimap_history.rend(); ++it) {\n"
    "          if (int32_t(shown - it->swap) >= 0) {\n            minimap = *it;\n            break;\n          }\n        }\n")
open(p, "w").write(t)
PY
# Jump Height works on Title Update 6 here (cheats_ps5.cpp's codegen hook), so the cheat is offered.
python3 - "$M/src/guest_layout.h" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
old = "    .jump_height_cheat = false,\n"
assert t.count(old) == 1, "guest_layout.h changed"
open(p, "w").write(t.replace(old, "    .jump_height_cheat = true,  // [ps5]\n"))
PY
# (std::hypot is libc's hypotf, which no PS5 build has imported yet: an import the console lacks
# stops the app from starting at all)
python3 - "$M/src/orb_map_rules.h" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
old = "const float distance = std::hypot(x - centre.x, z - centre.z);"
assert t.count(old) == 1, "orb_map_rules.h changed"
open(p, "w").write(t.replace(old, "const float distance = std::sqrt((x - centre.x) * (x - centre.x) + "
                                  "(z - centre.z) * (z - centre.z));  // [ps5]"))
PY

echo "== rexglue CLI (host)"
ccache -M 16G > /dev/null
cmake -S fw/glue/rexglue-sdk-main -B build-rexglue -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 \
  -DCMAKE_CXX_STANDARD=23 -DCD_NO_GTA4_NATIVE=ON \
  -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_GTA4_RECOMP=OFF \
  -DREXGLUE_BUILD_TESTS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > out/rexglue-configure.txt
ninja -C build-rexglue rexglue > out/rexglue-build.txt
rexglue=$(realpath "$(find build-rexglue fw/glue/rexglue-sdk-main/out -name rexglue -type f -perm -u+x | head -1)")
test -n "$rexglue"

echo "== codegen"
mkdir -p $M/assets
cp "$xex" $M/assets/default.xex
cp "$xexp" $M/assets/Default.xexp
echo "$XEX_SHA256  $M/assets/default.xex" | sha256sum -c
echo "$XEXP_SHA256  $M/assets/Default.xexp" | sha256sum -c
cp $M/assets/Default.xexp $M/assets/default.xexp
"$rexglue" --log-level info -f codegen $M/crackdown_manifest.toml > out/codegen.txt 2>&1 || { tail -80 out/codegen.txt; exit 1; }
grep -q "XEX patch applied successfully" out/codegen.txt || { echo "Title Update 6 was not applied"; exit 1; }
rm -rf $M/assets
echo "$(ls $M/generated | wc -l) generated files"

if [ -n "$xex2" ]; then
  echo "== codegen (Crackdown 2)"
  # (kept between builds under a key of its inputs: the codegen takes minutes)
  echo "$XEX2_SHA256  $xex2" | sha256sum -c
  echo "$XEXP2_SHA256  $xexp2" | sha256sum -c
  key=$(cat $M/cd2/crackdown2_manifest.toml $M/cd2/cd2_config.toml "$rexglue" | sha256sum | cut -c1-16)
  if [ ! -f "$work/cd2gen/$key/generated/sources.cmake" ]; then
    rm -rf "$work/cd2gen" && mkdir -p "$work/cd2gen/$key" $M/cd2/assets
    cp "$xex2" $M/cd2/assets/default.xex
    cp "$xexp2" $M/cd2/assets/Default.xexp
    cp $M/cd2/assets/Default.xexp $M/cd2/assets/default.xexp
    (cd $M/cd2 && REX_DCBZ128=1 "$rexglue" --log-level info -f codegen crackdown2_manifest.toml) > out/codegen2.txt 2>&1 ||
      { tail -80 out/codegen2.txt; exit 1; }
    grep -q "XEX patch applied successfully" out/codegen2.txt || { echo "Title Update 5 was not applied"; exit 1; }
    rm -rf $M/cd2/assets
    mv $M/cd2/generated "$work/cd2gen/$key/generated"
  fi
  rm -rf $M/cd2/generated && cp -r "$work/cd2gen/$key/generated" $M/cd2/generated
  echo "$(ls $M/cd2/generated | wc -l) generated files (Crackdown 2, $key)"
fi

echo "== PS5 program"
SDK=$tri/.deps/native/ps5-payload-sdk
cmake -S fw/ps5 -B ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$work/fw/ps5/cmake/ps5-toolchain.cmake -DPS5_PAYLOAD_SDK=$SDK \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCD_NO_GTA4_NATIVE=ON -DREXGLUE_ENABLE_FIDELITYFX=OFF   -DCD_PTBR_BIN="${CD_PTBR_BIN:-/root/cdtext/ptbr/ptbr.bin}" \
  -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
  -DSDL_OSS=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
  -DSDL_SNDIO=OFF -DSDL_HIDAPI=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF \
  -DSDL_VULKAN=OFF -DSDL_DUMMYVIDEO=ON -DSDL_DUMMYAUDIO=ON -DSDL_OFFSCREEN=OFF > out/configure.txt 2>&1
ninja -C ps5 -k 0 crackdown > out/build.txt 2>&1 || { grep -E "error|FAILED" out/build.txt | head -40; exit 1; }
if [ -n "$xex2" ]; then
  ninja -C ps5 -k 0 crackdown2 > out/build2.txt 2>&1 || { grep -E "error|FAILED" out/build2.txt | head -40; exit 1; }
fi

echo "== package"
bash "$repo/ci/ps5_package_crackdown.sh" ps5 "$tri" fw "$TITLE_ID" 2>&1 | tee out/package.txt
if [ -n "$assets" ]; then
  echo "== the app's art and music from $assets"
  cp -r "$assets"/. "ps5/ps5-app/$TITLE_ID/"
fi
elf=ps5/ps5-link/build/llvm-pie.elf
llvm-nm-19 -C -n --defined-only "$elf" | gzip -9 > out/symbols.txt.gz
sha256sum "$elf" | tee out/elf-sha256.txt
echo "== done: $work/ps5/ps5-app/$TITLE_ID"
