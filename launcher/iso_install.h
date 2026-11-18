// Reading the Xbox 360 disc image (XDVDFS) and installing its files as the PC version's game data.
//
// The game's code was recompiled to native x64 at build time from one exact default.xex, so an
// ISO is only usable if its default.xex is byte-identical (checked by SHA-256). Installing copies
// the levels, sounds, videos and textures out of the image into a "game" folder.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace jth::launcher {

constexpr uint32_t kTitleId = 0x41560870;  // Jurassic: The Hunted (USA)
// SHA-256 of the default.xex this build was recompiled from.
constexpr const char* kExpectedXexSha256 =
    "315D2AFA6CDD8319597E1B035DFB22B69C4CAA9B50F466548E282EBB6B6213BA";

struct IsoInfo {
  bool readable = false;       // valid Xbox 360 disc image with a default.xex
  bool right_game = false;     // title ID matches
  bool exact_version = false;  // default.xex matches this build exactly
  std::string error;           // why it can't be used (empty if ok)
  uint64_t data_bytes = 0;     // total size of the files that will be installed
  size_t file_count = 0;
  bool usable() const { return readable && right_game && exact_version; }
};
IsoInfo InspectIso(const std::filesystem::path& iso);

struct GameDataStatus {
  bool found = false;
  bool exact_version = false;
  std::filesystem::path dir;
};
// Looks for <dir>/game/default.xex in start_dir and each parent (the same search the game does).
GameDataStatus FindGameData(const std::filesystem::path& start_dir);
GameDataStatus CheckGameDataDir(const std::filesystem::path& game_dir);

// Copies all files from the ISO into dest_game_dir on a background thread. Files are written to
// "<dest>.partial" and moved into place only after everything succeeded.
class Installer {
 public:
  struct Snapshot {
    bool running = false, finished = false, ok = false, cancelled = false;
    uint64_t done_bytes = 0, total_bytes = 0;
    size_t files_done = 0, files_total = 0;
    double mb_per_s = 0;
    std::string message, current_file;
  };

  ~Installer();
  bool Start(const std::filesystem::path& iso, const std::filesystem::path& dest_game_dir);
  void Cancel() { cancel_ = true; }
  Snapshot Get() const;

 private:
  void Run(std::filesystem::path iso, std::filesystem::path dest);
  void SetMessage(std::string m, std::string current = {});

  std::thread thread_;
  std::atomic<bool> running_{false}, finished_{false}, ok_{false}, cancel_{false};
  std::atomic<uint64_t> done_bytes_{0}, total_bytes_{0};
  std::atomic<size_t> files_done_{0}, files_total_{0};
  std::atomic<int64_t> start_ms_{0};
  mutable std::mutex mutex_;
  std::string message_, current_file_;
};

}  // namespace jth::launcher
