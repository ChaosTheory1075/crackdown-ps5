// Crackdown - ReXGlue recompiled app (see crackdown_app.h).
#include "crackdown_app.h"

#include <memory>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

#include "crackdown_init.h"
#include "ps5/cd_install.h"
#include "ps5/cd_trophies_game.h"
#include "ps5/crackdown_menu.h"

namespace fs = std::filesystem;

namespace {

// The app's own folder: /app0 on the PS5, beside the program elsewhere.
fs::path AppRoot(const rex::PathConfig& defaults) {
#if REX_PLATFORM_PS5
  (void)defaults;
  return fs::path("/app0");
#else
  return defaults.config_path.parent_path();
#endif
}

rex::PathConfig ResolvePaths(const rex::PathConfig& defaults);

}  // namespace

std::unique_ptr<rex::ui::WindowedApp> CrackdownApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<CrackdownApp>(new CrackdownApp(ctx, "crackdown", PPCImageConfig));
}

std::optional<rex::PathConfig> CrackdownApp::OnFinalizePaths(const rex::PathConfig& defaults,
                                                             std::function<void(rex::PathConfig)> resume) {
#if REX_PLATFORM_PS5
  // The menu first (ps5/crackdown_menu.cpp); PLAY carries on with the start.
  cdps5::ShowMenu(imgui_drawer(), immediate_drawer(), AppRoot(defaults),
                  [defaults, resume]() { resume(ResolvePaths(defaults)); });
  return std::nullopt;
#else
  (void)resume;
  return ResolvePaths(defaults);
#endif
}

void CrackdownApp::OnConfigureFonts(ImFontAtlas* atlas) {
#if REX_PLATFORM_PS5
  cdps5::ConfigureMenuFonts(atlas);
#else
  (void)atlas;
#endif
}

std::unique_ptr<rex::ui::AchievementNotificationDialog> CrackdownApp::CreateAchievementNotificationDialog() {
#if REX_PLATFORM_PS5
  // (an unlock as a PS5 trophy: ps5/cd_trophies_game.h)
  return std::make_unique<cdps5::TrophyPopup>(imgui_drawer(), immediate_drawer(), runtime());
#else
  return rex::ReXApp::CreateAchievementNotificationDialog();
#endif
}

namespace {

// The game's folders: game/, its update/, dlc/ and saves/.
rex::PathConfig ResolvePaths(const rex::PathConfig& defaults) {
  auto paths = defaults;
  const fs::path root = AppRoot(defaults);
  std::error_code ec;
  if (paths.game_data_root.empty()) paths.game_data_root = root / "game";
  if (paths.update_data_root.empty() && fs::is_directory(paths.game_data_root / "update", ec))
    paths.update_data_root = paths.game_data_root / "update";
  if (paths.marketplace_content_root.empty()) paths.marketplace_content_root = root / "dlc";
  if (paths.saved_game_root.empty()) paths.saved_game_root = root / "saves";
  fs::create_directories(paths.saved_game_root, ec);
  fs::create_directories(paths.marketplace_content_root, ec);

  // The disc names it Default.xex (and the update Default.xexp); the PS5's own drive tells
  // case apart: both names, so a copy of the disc as it is starts too.
  for (const char* name : {"default.xex", "default.xexp"}) {
    const fs::path lower = paths.game_data_root / name;
    std::string upper_name = name;
    upper_name[0] = 'D';
    const fs::path upper = paths.game_data_root / upper_name;
    if (!fs::exists(lower, ec) && fs::exists(upper, ec)) fs::copy_file(upper, lower, ec);
    if (!fs::exists(upper, ec) && fs::exists(lower, ec)) fs::copy_file(lower, upper, ec);
    ec.clear();
  }
  REXLOG_INFO("Crackdown: game {}, update {}, dlc {}, saves {}", paths.game_data_root.string(),
              paths.update_data_root.string(), paths.marketplace_content_root.string(),
              paths.saved_game_root.string());
  if (!fs::exists(paths.game_data_root / "default.xex", ec))
    REXLOG_ERROR("Crackdown: no default.xex in {} (copy the disc's files there, see README.txt)",
                 paths.game_data_root.string());
  // The code is Title Update 6's: without its patch the base XEX would run against it.
  if (!fs::exists(paths.game_data_root / "default.xexp", ec) &&
      !fs::exists(paths.game_data_root / "Default.xexp", ec))
    REXLOG_ERROR("Crackdown: no default.xexp in {} (Title Update 6's Default.xexp)",
                 paths.game_data_root.string());
  return paths;
}

}  // namespace

void CrackdownApp::OnPreSetup(rex::RuntimeConfig& config) {
  // The generic Xenos GPU (Xenia's Vulkan backend): no game-specific renderer.
  if (!config.graphics && config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  // As the PC recomp's crackdown.toml: the game reads resolved render targets on the CPU.
  rex::cvar::SetFlagByName("readback_resolve", "fast");
  // Copy back only resolves up to 256 KB (crackdown.toml's): the large ones aren't read on the CPU
  // and copying them cost most of the frame (ci/fw_perf_fixes.py adds the cvar to the fork).
  rex::cvar::SetFlagByName("readback_resolve_max_bytes", "262144");
}

void CrackdownApp::OnPostSetup() {
  // The title's mount task links cache: to \Device\cache0 (the Xbox 360's HDD cache partition,
  // where it keeps copies of streamed data); without the device each open of it fails. As the PC
  // recomp's cache_device.cpp: a folder in the user data.
  if (!runtime()) return;
#if REX_PLATFORM_PS5
  // The achievements' list and pictures, for the menu's Trophies page.
  cdps5::WriteTrophyCatalog(runtime(), runtime()->user_data_root());
  // Saves imported from the menu, into the runtime's profile (made on its first start).
  if (auto* ks = runtime()->kernel_state(); ks && ks->user_profile() && ks->user_profile()->xuid())
    cdps5::install::AdoptImportedSaves(fs::path("/app0/saves"), ks->user_profile()->xuid());
#endif
  const fs::path host_path = runtime()->user_data_root() / "4D5307DC" / "cache";
  std::error_code ec;
  fs::create_directories(host_path, ec);
  auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\cache0", host_path,
                                                                  /*read_only=*/false,
                                                                  /*allow_share_delete=*/true);
  if (ec || !device->Initialize() || !runtime()->file_system()->RegisterDevice(std::move(device))) {
    REXLOG_ERROR("Crackdown: cache: device not mounted at {}", host_path.string());
    return;
  }
  REXLOG_INFO("Crackdown: cache: device at {}", host_path.string());
}
