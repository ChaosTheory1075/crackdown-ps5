// Crackdown 2's PlayStation button prompts (crackdown_ps_buttons): the UI packs patched into the user data and the
// game pointed at them (cd2_prompts.cpp; the rewriting itself: cd2_prompts_core.h).
#pragma once

#include <filesystem>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace cd2::prompts {

// Before the title starts (OnPostSetup). Any failure leaves the game's own buttons.
void Install(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& game_root,
             const std::filesystem::path& user_data_root);

}  // namespace cd2::prompts
