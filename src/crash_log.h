// Crash reporting: when the game dies from an unhandled exception (access violation, the guest's
// KeBugCheck breakpoint, ...), write what happened to the log - exception code, faulting module +
// offset, fault address, thread, last map loaded - flush it, and save a minidump next to the logs
// (logs\crash_<date>_<time>.dmp). Without this a hard crash left nothing in the log, and debug
// lines still buffered were lost.

#pragma once

#include <filesystem>
#include <string>

namespace jth::crash {

// Call once at startup.
void Install(const std::filesystem::path& logs_dir);

// The map the game is loading or playing (shown in crash reports).
void SetCurrentMap(const std::string& map);

}  // namespace jth::crash
