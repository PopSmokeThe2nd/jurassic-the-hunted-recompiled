// Hooks on the game's own file layer (streams), used to:
//  - replace a game file's contents as the game reads it (button_prompts.cpp), whether the file
//    comes from the disc folder or from inside a compressed .xds archive;
//  - save files as they are read (developer tool: --dump_game_files=<paths> --dump_dir=<folder>).
//
// The game opens a stream with sub_82130F08 (r3 = stream, r4 = path relative to the disc, e.g.
// "gui\textures\icons.dds"), reads with sub_82130378 (r3 = stream, r4 = buffer, r5 = size; it
// returns through 0x8213044C for disc files and 0x821304A8 for archive files with r3 = bytes read)
// and closes with sub_82130598 or the destructor sub_82130AB8.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

namespace jth::files {

// Serve `replacement` instead of `original` for `path` (lower case, backslashes). Each read is
// swapped only if the bytes the game read match `original` at that offset, so a different file
// version is left alone. Call before the game starts.
void AddReplacement(const std::string& path, std::vector<uint8_t> original,
                    std::vector<uint8_t> replacement);

// Replace every occurrence of `find` with `replace` (same length) in `path` as it's read. Meant for
// the game's small text files (XML pages, scripts), which it reads in one go; a match split across
// two reads would be missed. Call before the game starts.
void AddPatch(const std::string& path, std::string find, std::string replace);

// Developer options that patch game files (dev_level_list). Call before the game starts.
void SetupDevOptions();

}  // namespace jth::files

void JthOpenFile(PPCRegister& r3, PPCRegister& r4);
void JthStreamRead(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5);
void JthStreamReadDone(PPCRegister& r3);
void JthStreamClose(PPCRegister& r3);
