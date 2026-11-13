# What's missing before a first PS5 boot test

> Historical (October 2026): the plan written before Crackdown first booted on the PS5. Everything below
> has since been done. Crackdown is built on the gta4-ps5 runtime with the generic Xenos renderer, not GTA IV's
> `gta4_native`. Kept for the record; it is not the current state.

State after the first pass: inputs verified and encrypted, CI builds the
recompiler, generates the base and TU6 guests, and builds the PS5 toolchain.
Nothing PS5-side compiles Crackdown yet.

CI results (`codegen` run 40447889668, `ps5-toolchain` run 40449677994):

- rexglue 0.10.0 + the PC recomp's patches: builds with clang 20.
- Base guest: 361 files, 185 MB, 40 s. TU6 guest: the loader logs
  `XEX patch applied successfully: base version: 0.0.0.1, new version: 0.0.6.1`,
  377 files, 194 MB. Both log a few `Unexpected float16_4 pack instruction`
  notes (8260xxxx / 8267xxxx), worth checking against the PC recomp's log.
- PS5 toolchain: all seven outputs (prospero-lld, RADV, ps5-native-tool, app
  CRT, C++ runtime, libSceAgc stubs, libc.prx), after the Meson >= 1.4 fix.

## The deciding fact: two ReXGlue versions, two GPU paths

| | Crackdown PC recomp | gta4-ps5 runtime |
|---|---|---|
| ReXGlue | upstream **v0.10.0** (`f5337cd`) + 100 own patches (`patches/rexglue-sdk`, 3 touch codegen: 0005, 0052, 0095) | fork of **0.9.0** with PS5 support |
| GPU | generic Xenos emulation (`rexgpu-xenos`, Vulkan on Linux) | on PS5 **only** the GTA IV-specific `gta4_native` renderer is built (`src/graphics/CMakeLists.txt`: `if(NOT PS5) add_library(rexgpu-xenos …)`) |
| Size of change | 0.9.0 → 0.10.0 upstream: 126 src files, about +7.1k/−6.1k lines (codegen: 18 files) | 0.9.0 → fork: 496 src files, +137k lines (mostly `gta4_native`, `gta4_metal`, hub) |

Both use ReXGlue, so the PS5 platform layer (threads, memory, file system,
`input/ps5`, `audio/ps5`, the Vulkan/RADV surface, kernel bits, about 78 files
with PS5 code) is reusable. The GTA IV renderer is not: Crackdown needs the
generic Xenos command processor running on RADV.

## Blocking work, in order

1. **One SDK for both.** The `probe-gta4` job (run 40451949627) shows the
   gta4-ps5 fork's 0.9.0 codegen **accepts the PC recomp's manifests and
   configs unchanged** (only `sdk_version` rewritten): base 93 files / 182 MB,
   TU6 97 files / 190 MB, same `float16_4` notes, exit 0. So the fastest route
   to a first boot is: fork codegen + fork runtime, and port the PC recomp's
   host code (`src/`, hooks, midasm overrides) down to the fork's 0.9.0 APIs
   where they differ; a few of its runtime patches matter for correctness
   (e.g. 0104 `--xex_patch`, 0052 `vrsqrtefp`, 0095 fused multiply-add, 0005
   `vpkd3d128`: the last three change codegen, so check the probe output
   against them). Longer term: merge upstream's 0.9.0 → 0.10.0 delta (about
   7k lines) into the fork so the PC recomp's patches apply as they are.
2. **Generic Xenos Vulkan backend on PS5.** Drop the `if(NOT PS5)` gate,
   link `rexgpu-xenos` statically (the PS5 build is one static program; make
   `gpu_plugin_loader.cpp` call it directly, as it does `gta4_native`), and
   get its SPIR-V shader translation and pipelines working on RADV (the PC
   recomp's `.linux` patches are the Vulkan set: 0013, 0014, 0038–0040,
   0043, 0046, 0049, 0053, 0058). Expect the most debugging here.
   The Max Payne 3 port is doing exactly this for the 0.9.0 fork in
   Max Payne 3 port's `ci/patch_framework.py` (static `rexgpu-xenos`
   on PS5, `gta4_native` left out, host fixes): reuse it rather than redo it.
3. **PS5 CMake target for Crackdown** (`ps5/CMakeLists.txt` like gta4-ps5's):
   generated `default` + `tu6` guests, the PC recomp's `src/` minus Windows
   and launcher code (TRG launcher, D3D12, SDL window/mouse, updater), with
   `cmake/crackdown_guests.cmake`'s symbol renaming. Build only the TU6 guest
   first (one guest halves the link and code size).
4. **`crackdown_ps5_app.cpp`**: where the game and the hub meet, adapted from
   `gta4_ps5_app.cpp` (103 lines). Guest selection (`CrackdownApp::Create` /
   `SelectGuest`) and the save-generation swap from the PC recomp, minus its
   relaunch-on-guest-change (on PS5 the hub picks before boot).
5. **Hub/installer for Crackdown** (from `src/ps5/hub`, 7.5k lines with
   `game/`): installs from an extracted disc folder or ISO under `install/`,
   then the TU6 package and the two DLC packages (STFS reader already in
   ReXGlue: `stfs_container_device`). Replace GTA IV's episode detection with
   title ID `4D5307DC`, the TU6 xexp SHA-256 and the two DLC names. Reuse the
   PC recomp's `content_install.*`, `content_state.*`, `content_catalog.h`.
   Free-For-All needs TU6.
6. **Packaging**: `ps5/tools/ps5_package.sh` with title ID **PPSA53070**,
   Crackdown icon/art in `sce_sys`, installer payload. Deploy to
   `/mnt/ext1/homebrew/PPSA53070` over FTP 2121 with curl.
7. **CI job `ps5-build`**: toolchain (cached) + generated code artifact +
   the above, producing the app folder as a private artifact.

## Can wait until after first boot

- Trophies: map the PC recomp's offline achievements (`achievements_*`) onto
  `trophies_game.cpp`.
- Saves: 360 save import (`save_import.cpp`) with Crackdown's save layout
  (`save_compat.*` knows versions 4/5/6).
- Hub polish: Crackdown art, menu music, language strings, `GameStrings_DLC.str`.
- Performance: the PC recomp's GPU/command-processor patches are tuned for
  desktop; FSR/frame-gen from gta4-ps5 is tied to `gta4_native` and won't
  carry over.
- Keyboard/mouse, launcher, cheats overlay, high-FPS/ultrawide options.
