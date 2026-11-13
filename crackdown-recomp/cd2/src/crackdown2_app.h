// Crackdown 2 - ReXGlue recompiled app: cd2.bin, the program Crackdown: ReActivated's menu starts for
// Crackdown 2 (sceSystemServiceLoadExec, as Saints Row: ReInstated's sr2.bin).
//
// On the PS5 the app folder is /app0: Crackdown 2's files in /app0/game2 (the disc's files, with Title
// Update 5's Default.xexp beside default.xex and its other files in /app0/game2/update), its DLC
// packages in /app0/dlc2, saves in /app0/saves (beside Crackdown 1's: the title id's folder keeps them
// apart). No menu of its own: it applies the menu's settings (menu.ini) and starts the game.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <rex/rex_app.h>

class Crackdown2App : public rex::ReXApp {
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
  // The menu's UPSCALING and SHARPNESS, to the presenter (it read them before the settings were applied).
  void ApplyUpscaling();
};
