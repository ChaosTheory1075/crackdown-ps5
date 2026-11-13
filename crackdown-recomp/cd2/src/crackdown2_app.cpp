// Crackdown 2 - ReXGlue recompiled app (cd2.bin, see crackdown2_app.h). The menu's own pieces are shared
// with Crackdown 1's program (crackdown-recomp/src/ps5): its settings, the installer's save adoption, the
// trophies and their pop-up, the FPS counter, the quick menu (L3 + R3) with Crackdown 2's own cheats
// (cd2_cheats.h). Crackdown 1's game hooks (frame pacing, the PC recomp's XAudio fixes and cheats, the DualSense
// triggers) are bound to Title Update 6's addresses and aren't in this program; its PlayStation button prompts
// are its own (cd2_prompts.h).
#include "crackdown2_app.h"

#include <chrono>
#include <memory>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/presenter.h>
#include <spdlog/spdlog.h>

#include "cd2_cheats.h"
#include "cd2_prompts.h"
#include "crackdown2_init.h"
#include "ps5/cd_install.h"
#include "ps5/cd_overlay.h"
#include "ps5/cd_quick_menu.h"
#include "ps5/cd_trophies_game.h"
#include "ps5/crackdown_menu.h"

REXCVAR_DECLARE(std::string, present_effect);  // (the presenter's, ui/presenter.cpp)
REXCVAR_DECLARE(double, present_fsr_sharpness_reduction);
REXCVAR_DECLARE(bool, crackdown_quick_menu);  // (ps5/cd_quick_menu.cpp)
REXCVAR_DECLARE(bool, cd2_no_msaa);           // (cd2_features.cpp)

namespace cd2 {
void StartLightBar();  // cd2_features.cpp
}

namespace fs = std::filesystem;

namespace {

#if REX_PLATFORM_PS5
std::unique_ptr<cdps5::FpsOverlay> g_fps_overlay;
std::unique_ptr<cdps5::QuickMenu> g_quick_menu;
#endif

// The app's own folder: /app0 on the PS5, beside the program elsewhere.
fs::path AppRoot(const rex::PathConfig& defaults) {
#if REX_PLATFORM_PS5
  (void)defaults;
  return fs::path("/app0");
#else
  return defaults.config_path.parent_path();
#endif
}

// Crackdown 2's folders: game2/, its update/, dlc2/ and saves/ (shared with Crackdown 1's).
rex::PathConfig ResolvePaths(const rex::PathConfig& defaults) {
  auto paths = defaults;
  const fs::path root = AppRoot(defaults);
  std::error_code ec;
  if (paths.game_data_root.empty()) paths.game_data_root = root / "game2";
  if (paths.update_data_root.empty() && fs::is_directory(paths.game_data_root / "update", ec))
    paths.update_data_root = paths.game_data_root / "update";
  if (paths.marketplace_content_root.empty()) paths.marketplace_content_root = root / "dlc2";
  if (paths.saved_game_root.empty()) paths.saved_game_root = root / "saves";
  fs::create_directories(paths.saved_game_root, ec);
  fs::create_directories(paths.marketplace_content_root, ec);
  REXLOG_INFO("Crackdown 2: game {}, update {}, dlc {}, saves {}", paths.game_data_root.string(),
              paths.update_data_root.string(), paths.marketplace_content_root.string(),
              paths.saved_game_root.string());
  // (the installer writes both names: the PS5's drive tells case apart)
  if (!fs::exists(paths.game_data_root / "default.xex", ec) && !fs::exists(paths.game_data_root / "Default.xex", ec))
    REXLOG_ERROR("Crackdown 2: no default.xex in {} (install the disc from the menu)", paths.game_data_root.string());
  // The code is Title Update 5's: without its patch the base XEX would run against it.
  if (!fs::exists(paths.game_data_root / "default.xexp", ec) && !fs::exists(paths.game_data_root / "Default.xexp", ec))
    REXLOG_ERROR("Crackdown 2: no default.xexp in {} (Title Update 5's Default.xexp)", paths.game_data_root.string());
  return paths;
}

}  // namespace

std::unique_ptr<rex::ui::WindowedApp> Crackdown2App::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<Crackdown2App>(new Crackdown2App(ctx, "crackdown2", PPCImageConfig));
}

std::optional<rex::PathConfig> Crackdown2App::OnFinalizePaths(const rex::PathConfig& defaults,
                                                              std::function<void(rex::PathConfig)> resume) {
  (void)resume;
#if REX_PLATFORM_PS5
  // Started by the menu (eboot.bin's): its settings, and on with the game.
  REXLOG_INFO("Crackdown 2: started by the menu");
  cdps5::ApplySettings(AppRoot(defaults));
  ApplyUpscaling();
  // FRAME RATE 60 (crackdown2_frame_rate, cd2_features.cpp): the game waits two vblanks a frame, so its vblanks run
  // free and cd2_features.cpp paces the frames to 60 with the measured game clock (the PC recomp's vsync=false +
  // fps_cap=60 + sim_game_clock). 30: the shipped path. A vsync set in the config wins. Here, not in OnPreSetup:
  // that runs before this (rex_app.cpp SetupPresentation, then OnFinalizePaths), when crackdown2_frame_rate was
  // still its default 30, so 60 never turned vsync off and the game stayed at 30 (cd2.bin gets no launch args).
  // vsync is read at every vblank (graphics_system.cpp), so setting it now takes effect.
  if (rex::cvar::GetFlagByName("crackdown2_frame_rate") == "60" && !rex::cvar::HasNonDefaultValue("vsync"))
    rex::cvar::SetFlagByName("vsync", "false");
  REXLOG_INFO("Crackdown 2: frame rate {}, vsync {}", rex::cvar::GetFlagByName("crackdown2_frame_rate"),
              rex::cvar::GetFlagByName("vsync"));
#endif
  return ResolvePaths(defaults);
}

// The menu's UPSCALING and SHARPNESS (present_effect, present_fsr_sharpness_reduction): the window's
// presenter read them when it started, so it gets them here (as Crackdown 1's program).
void Crackdown2App::ApplyUpscaling() {
#if REX_PLATFORM_PS5
  if (rex::ui::Presenter* presenter = graphics_system() ? graphics_system()->presenter() : nullptr) {
    using Config = rex::ui::Presenter::GuestOutputPaintConfig;
    Config config = presenter->GetGuestOutputPaintConfigFromUIThread();
    config.SetEffect(REXCVAR_GET(present_effect) == "fsr" ? Config::Effect::kFsr : Config::Effect::kBilinear);
    config.SetFsrSharpnessReduction(float(REXCVAR_GET(present_fsr_sharpness_reduction)));
    presenter->SetGuestOutputPaintConfigFromUIThread(config);
  }
#endif
}

void Crackdown2App::OnConfigureFonts(ImFontAtlas* atlas) {
#if REX_PLATFORM_PS5
  cdps5::ConfigureMenuFonts(atlas);  // (the trophy pop-up's and the FPS counter's)
#else
  (void)atlas;
#endif
}

std::unique_ptr<rex::ui::AchievementNotificationDialog> Crackdown2App::CreateAchievementNotificationDialog() {
#if REX_PLATFORM_PS5
  return std::make_unique<cdps5::TrophyPopup>(imgui_drawer(), immediate_drawer(), runtime());
#else
  return rex::ReXApp::CreateAchievementNotificationDialog();
#endif
}

REXCVAR_DEFINE_BOOL(cd_profile, false, "Crackdown",
                    "One profiling run: the CPU sampler on (its report every 30 s in the log)");

void Crackdown2App::OnPreSetup(rex::RuntimeConfig& config) {
  // As Crackdown 1's program: the generic Xenos GPU, the CPU reading back small resolves only, three
  // pipeline creation threads, frames with a pipeline still building shown, no stall reports (their
  // SIGPROF sampling froze loads: memory ps5-hang-reports-freeze).
  if (!config.graphics && config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  // Resolves read back a frame late ("fast"), as the Crackdown 2 PC recomp ships it (its docs/playbook.md: the
  // game's sunlight exposure reads a resolve back; host_tuning.cpp's game default). "full" (2026-11-05..11-12, for
  // orbs that slid with "fast") made every D3D callback a round trip through the GPU: with readbacks pending, the
  // command stream's scratch write-backs were kept until the GPU finished (fw_fence_fixes.py) and the very next
  // WAIT_REG_MEMs waited for them, ~15 ms of a 30 ms frame (crackdown2_027/028). The orb/marker fault turned out to
  // be the depth test (fw_odepth_float24.py), not readback timing. A config file can still set "full".
  if (!rex::cvar::HasNonDefaultValue("readback_resolve")) rex::cvar::SetFlagByName("readback_resolve", "fast");
  // (the config file can raise it: "cd readback cap" log lines list what it skips)
  if (!rex::cvar::HasNonDefaultValue("readback_resolve_max_bytes"))
    rex::cvar::SetFlagByName("readback_resolve_max_bytes", "262144");
  if (!rex::cvar::HasNonDefaultValue("vulkan_pipeline_creation_threads"))
    rex::cvar::SetLaunchFlag("vulkan_pipeline_creation_threads", "3");
  rex::cvar::SetLaunchFlag("vulkan_async_skip_incomplete_frames", "false");
  if (!rex::cvar::HasNonDefaultValue("ps5_hang_reports")) rex::cvar::SetLaunchFlag("ps5_hang_reports", "false");
  // No CPU sampler in the user's build (SIGPROF to every thread, the game's too, every 10 ms, and a report
  // every 30 s): off unless a toml turns it on, as The Darkness and MGR (2026-11-06).
  // cd_profile = true in crackdown2.toml turns the sampler on for one run (a toml "ps5_cpu_sampler = true" can't:
  // true is that cvar's default, so the line above took it for unset and switched it off; crackdown2_024).
  if (REXCVAR_GET(cd_profile)) rex::cvar::SetLaunchFlag("ps5_cpu_sampler", "true");
  else if (!rex::cvar::HasNonDefaultValue("ps5_cpu_sampler")) rex::cvar::SetLaunchFlag("ps5_cpu_sampler", "false");
  // No CPU readback of the shaders' memory exports (Xenia Canary's default): Crackdown 2's particles
  // and effects export from the GPU for the GPU, and copying every export back stalled the GPU queue
  // mid-frame: 7-10 fps whenever many effects were in view (crackdown2_006.log, main thread 5 ms).
  if (!rex::cvar::HasNonDefaultValue("readback_memexport")) rex::cvar::SetLaunchFlag("readback_memexport", "false");
  // The submit thread (ci/fw_submit_thread.py): each submission's recorded Vulkan commands replayed and
  // submitted on CPU 5 (the lane's twin, empty) while the GPU thread goes on with the next draws. That work was
  // 5-11 ms of the GPU thread's heavy frames (crackdown2_017 "cd perf" submit), and the GPU thread sets
  // Crackdown 2's frame rate. Proof (2026-11-06, Extras/tmp/replay/compare.sh): Stranglehold's and Driver SF's
  // traces through this framework give byte-identical pictures with it off and on. A toml can turn it off.
  if (!rex::cvar::HasNonDefaultValue("cd_submit_thread")) rex::cvar::SetLaunchFlag("cd_submit_thread", "true");
  // The game's fences written when the GPU gets there (ci/fw_fence_fixes.py, cd_fence_deferred, with the batched
  // readback it needs, ci/fw_readback_fixes.py): crackdown2_017's "GPU fences", 10-13 ms of the GPU thread's frame in
  // play, were 400-550 drains in 10 s at EVENT_WRITE_SHD (330-400 ms a second), the GPU thread idle until the GPU had
  // done everything. Now the fence is kept with its readbacks and written once that submission's GPU fence signals.
  // Batched readback ran in play in crackdown2_017. Proof for the fences (2026-11-07, Extras/tmp/replay/
  // compare_fence.sh): Driver SF's trace keeps fences with readbacks and gives a byte-identical picture off and on.
  // The 10 s "cd copy settings" line says what a run had. A toml can turn either off.
  if (!rex::cvar::HasNonDefaultValue("cd_readback_batched")) rex::cvar::SetLaunchFlag("cd_readback_batched", "true");
  if (!rex::cvar::HasNonDefaultValue("cd_fence_deferred")) rex::cvar::SetLaunchFlag("cd_fence_deferred", "true");
  // A texture descriptor set written earlier this frame with the same contents is bound again instead of a new
  // one (ci/fw_tile_replay.py): 91% of a frame's ~2,280 sets repeat (the second tile replays the first).
  // Same contents = same draw: identical picture on the PC trace replay (md5), 7.7 -> 6.1 us a draw there.
  if (!rex::cvar::HasNonDefaultValue("cd_texture_set_reuse")) rex::cvar::SetLaunchFlag("cd_texture_set_reuse", "true");
  // A fence or scratch write-back the game sees is stored by the GPU lane after its pending copies instead of the
  // GPU thread draining the lane first (ci/fw_lane_fence.py): crackdown2_024 had the GPU thread waiting on the lane
  // 1-2.5 s in every 10 s, mostly at EVENT_WRITE_SHD and the scratch write-back. Same order of stores, so the
  // game sees the same values no earlier. Proof (2026-11-11, Extras/tmp/replay/compare_lanefence.sh): Crackdown 2's,
  // Stranglehold's and Driver SF's traces give byte-identical pictures off, on, and on with the lane slowed
  // (22-52 fences a replay went through the lane). The 10 s "cd lane fence writes" line counts them.
  // On since 2.0.0: the user's console ran it (crackdown2_030, 2026-11-13: no fault, the EVENT_WRITE_SHD lane waits
  // ~1 s -> 0.6-0.7 s in 10 s). A toml can turn it off.
  if (!rex::cvar::HasNonDefaultValue("cd_lane_fence_write")) rex::cvar::SetLaunchFlag("cd_lane_fence_write", "true");
  // The predicated-tiling fixes and the lane hash fix the user's crackdown2.toml has had on since 2026-11-06/07
  // (Canary e1f4a0d00 / 6579cc2a4: the window offset in the EDRAM bases, the negative viewport origin; the GPU lane's
  // hash race): on by default since 2.0.0, every console run since then had them.
  for (const char* on : {"cd_window_offset_in_edram", "cd_viewport_negative_origin", "cd_lane_hash_fix"})
    if (!rex::cvar::HasNonDefaultValue(on)) rex::cvar::SetLaunchFlag(on, "true");
  // Draws on two cores (ci/fw_draw_worker.py, Saints Row's draw worker ported; Extras/CD2-DRAW-WORKER.md): each
  // draw's own state, constants, descriptor sets and draw command recorded on CPU 5 (the submit thread moves to
  // CPU 1). Identical pictures on three PC trace replays (md5), but on the console (crackdown2_023, 2026-11-11) it
  // took the city from 14-22 fps to 3.5-11: the worker topped out at ~35k draws a second and the GPU thread waited
  // on it. OFF; the cvar stays for later work.
  // (cd_lane_wait_on_submit stays off: with the lane slowed, a PC trace replay hung in a page-fault loop with it on,
  // 2026-11-07. The "gpu lane waits by call site" line says where the GPU thread waits for the lane.)
  // One tile (cd2_no_msaa, cd2_features.cpp): the game's baked tile-0 scissor snippets (1280x484) would clip the
  // whole frame to rows 0-484; the framework widens exactly that header to 1280x720 (ci/fw_one_tile_scissor.py).
  // PC proof: Canary trace replayed through our renderer, Extras/CD2-NO-MSAA.md.
  if (REXCVAR_GET(cd2_no_msaa)) rex::cvar::SetLaunchFlag("cd_one_tile_scissor", "true");
  // (FRAME RATE 60's vsync: OnFinalizePaths, once the menu's settings are applied; they aren't yet here)
}

void Crackdown2App::OnPostSetup() {
  // The log written out every second (the app flushes on critical lines only, so a run the system
  // killed left an empty log: crackdown2_002.log). Not every line: with a warning on every effect draw
  // that held the game at 2 fps (crackdown2_005.log). Crash lines go straight to the file anyway.
  spdlog::flush_every(std::chrono::seconds(1));
  REXLOG_INFO("Crackdown 2: readback_memexport={} readback_resolve={}", rex::cvar::GetFlagByName("readback_memexport"),
              rex::cvar::GetFlagByName("readback_resolve"));
  if (!runtime()) return;
#if REX_PLATFORM_PS5
  // The achievements' list and pictures, for the menu's Trophies page (UserData/crackdown2).
  cdps5::WriteTrophyCatalog(runtime(), runtime()->user_data_root());
  // Saves imported from the menu, into the runtime's profile.
  if (auto* ks = runtime()->kernel_state(); ks && ks->user_profile() && ks->user_profile()->xuid())
    cdps5::install::AdoptImportedSaves(fs::path("/app0/saves"), ks->user_profile()->xuid());
  // The quick menu (L3 + R3) and its cheats as saved (none with the menu's CHEAT MENU off).
  if (REXCVAR_GET(crackdown_quick_menu)) cd2::cheats::Load("/app0/cheats2.ini");
  g_fps_overlay = std::make_unique<cdps5::FpsOverlay>(imgui_drawer());
  g_quick_menu = std::make_unique<cdps5::QuickMenu>(imgui_drawer());
  // BUTTONS PLAYSTATION: the UI packs' button shapes swapped (cd2_prompts.h), before the title opens them
  cd2::prompts::Install(runtime()->file_system(), fs::path("/app0/game2"), runtime()->user_data_root());
  cd2::StartLightBar();  // (DUALSENSE FEATURES: cd2_features.cpp)
#endif
  // cache: -> \Device\cache0, as Crackdown 1 (the Xbox 360's HDD cache partition): a folder in the user data.
  const fs::path host_path = runtime()->user_data_root() / "4D5308BC" / "cache";
  std::error_code ec;
  fs::create_directories(host_path, ec);
  auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\cache0", host_path,
                                                                  /*read_only=*/false,
                                                                  /*allow_share_delete=*/true);
  if (ec || !device->Initialize() || !runtime()->file_system()->RegisterDevice(std::move(device))) {
    REXLOG_ERROR("Crackdown 2: cache: device not mounted at {}", host_path.string());
    return;
  }
  REXLOG_INFO("Crackdown 2: cache: device at {}", host_path.string());
}
