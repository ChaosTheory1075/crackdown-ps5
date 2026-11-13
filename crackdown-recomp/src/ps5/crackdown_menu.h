// Crackdown PS5 - the menu the app opens on (crackdown_menu.cpp): its pages
// HOME (PLAY), CONTENT, SAVES, SETTINGS (graphics, controller, game, experimental, audio), TROPHIES
// (the achievements, earned and not: cd_trophies.h) and CREDITS, over
// hub/background.jpg with the menu song (hub/music/) playing.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <cstdint>
#include <filesystem>
#include <functional>

struct ImFontAtlas;
struct ImFont;
namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace cdps5 {

// The menu's fonts (from OnConfigureFonts), and two of them for the trophy popup.
void ConfigureMenuFonts(ImFontAtlas* atlas);
ImFont* MenuBodyFont();
ImFont* MenuItemFont();

// Crackdown 2's program (cd2.bin): menu.ini's settings applied, without the menu.
void ApplySettings(const std::filesystem::path& app);

// Shows the menu; `play` runs (on the UI thread) on PLAY, its settings applied
// (the resolution's cvars) and saved in app/menu.ini.
void ShowMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const std::filesystem::path& app,
              std::function<void()> play);

}  // namespace cdps5

#endif  // REX_PLATFORM_PS5
