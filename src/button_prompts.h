// In-game button prompts: optionally show PlayStation button icons instead of the Xbox ones.
//
// The game draws its prompts ("A Accept", tutorial hints, the HUD's LB/RB and D-pad) from a few
// DDS textures (gui\textures\icons*.dds, icons\xbox\bumper.dds, icons\...\directional.dds; copies
// sit both loose in game\Gui\Textures and inside startup.xds). With button_prompts =
// "playstation", Setup() builds PlayStation versions of them in memory from the installed loose
// originals (only the button cells are redrawn, with the images in assets/glyphs embedded in the
// exe) and has game_files.cpp swap them in as the game reads those files. Game files are never
// modified.

#pragma once

#include <filesystem>

namespace jth::prompts {

void Setup(const std::filesystem::path& game_root);

}  // namespace jth::prompts
