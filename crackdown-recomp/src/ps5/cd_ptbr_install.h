// Crackdown PS5 - Crackdown's text in Brazilian Portuguese, installed by the app.
//
// The disc has no Portuguese, but the title's text folder table lists 15, pt-BR (locale_ps5.cpp); the menu
// gives the game that folder once game/Local/pt-BR exists (crackdown_menu.cpp PortugueseTextInstalled).
// This writes it at the menu's start, only when it's missing: game/Local/pt-BR (en-US's folder with the
// Portuguese GameStrings.str, Subtitle.xzp and XUIFrontEnd/Scene.xzp in place of the English ones) and
// game/update/Local/pt-BR (Title Update 6's GameStrings_DLC.str in Portuguese). The Portuguese files come
// from a blob made from the user's own game files (Recomps/ptbr/crackdown: build.py, build_sub.py,
// build_ui.py, pack.py -> CD_PTBR_BIN); a build without it installs nothing (English text). Nothing else is
// touched: no other locale, nor game2/, saves/ or UserData/.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <filesystem>

namespace cdps5 {

// <app>: the app's folder (game/ in it). Writes the pt-BR folders that are missing; leaves existing ones.
void InstallPortugueseText(const std::filesystem::path& app);

}  // namespace cdps5

#endif  // REX_PLATFORM_PS5
