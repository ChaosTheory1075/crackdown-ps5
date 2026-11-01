// Crackdown PS5 - PlayStation button prompts.
//
// The game draws every button prompt from its font: the 40 characters U+0190.. of
// Local\<language>\Crackdown_Font.dds/.abc are the Xbox 360's buttons (A B X Y, START, BACK, the
// bumpers, triggers, sticks and D-pad). Before the title starts, a copy of the language's font is
// built with its atlas at twice the size and those glyphs replaced by the PlayStation ones of
// /app0/glyphs/ps_button_glyphs.dds (tools/make_ps_button_glyphs.py), and the game's Local\<language>
// folder is pointed at it, as the PC recomp's glyph overlay does: the game files stay as they are.
// The copy is kept in UserData/4D5307DC/glyphs/<language> and built again only when its inputs
// change. Two menus (the Agency's upgrades, the dossiers) show A and B from pictures of their own
// (XUIFrontEnd\agencyNode.xzp, dossier.xzp): those two packages are rewritten with Cross and Circle,
// the originals kept beside them as .xbox and put back when the setting is off.
#pragma once

#include <filesystem>
#include <string_view>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace cdps5::glyphs {

// The font of `language` (a game\Local folder name). Any failure leaves the game's own glyphs.
void InstallFont(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& game_root,
                 const std::filesystem::path& user_data_root, std::string_view language);

// The menus' button pictures: PlayStation ones when `playstation`, else the originals back.
void SetMenuPictures(const std::filesystem::path& game_root, bool playstation);

}  // namespace cdps5::glyphs
