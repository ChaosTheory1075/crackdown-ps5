// Crackdown - ReXGlue recompiled app.
//
// On the PS5 the app folder is /app0 (the homebrew folder): the game's files
// in /app0/game (the disc's files, with Title Update 6's Default.xexp beside
// default.xex), the update's other files in /app0/game/update, DLC packages
// in /app0/dlc and saves in /app0/saves. It opens on the menu
// (ps5/crackdown_menu.h).

#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <rex/rex_app.h>

class CrackdownApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override;
};
