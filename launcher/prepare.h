// Windowless setup for other launchers (RecompLauncher's "prepare" step):
//
//   JurassicTheHunted.exe --prepare --game <disc image (.iso) or extracted folder>
//
// Installs the game data into the "game" folder next to the program, exactly as the GAME ISO tab
// does (same checks, same installer), so jurassichunted.exe then finds it by itself. An extracted
// folder (with default.xex) is checked the same way and copied. Opens no window; prints
//   progress <0..1> <text>
// lines on stdout and exits with 0 on success, 2 for bad arguments, 3 when the copy isn't the
// right game or version, 4 when installing failed (disk full, write error).

#pragma once

#include <filesystem>

namespace jth::launcher {

int RunPrepare(const std::filesystem::path& game, const std::filesystem::path& exe_dir);

}  // namespace jth::launcher
