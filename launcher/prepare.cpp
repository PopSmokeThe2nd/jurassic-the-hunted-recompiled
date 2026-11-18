#include "prepare.h"

#include "iso_install.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

namespace jth::launcher {
namespace {

namespace fs = std::filesystem;

constexpr int kOk = 0, kBadArgs = 2, kWrongCopy = 3, kInstallFailed = 4;

std::string U8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// "progress <0..1> <text>" lines on stdout. The launcher has no console of its own: stdout is the
// pipe the calling launcher reads, or (run by hand from a terminal) that terminal.
class Out {
 public:
  Out() {
    h_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!h_ || h_ == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
      h_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
  }
  void Line(double fraction, std::string text) {
    std::replace(text.begin(), text.end(), '\r', ' ');
    std::replace(text.begin(), text.end(), '\n', ' ');
    char num[16];
    std::snprintf(num, sizeof(num), "%.3f", std::clamp(fraction, 0.0, 1.0));
    const std::string line = std::string("progress ") + num + " " + text + "\n";
    DWORD wrote = 0;
    if (h_ && h_ != INVALID_HANDLE_VALUE)
      WriteFile(h_, line.data(), DWORD(line.size()), &wrote, nullptr);
    last_ms_ = NowMs();
  }
  // At most four lines a second.
  void Throttled(double fraction, const std::string& text) {
    if (NowMs() - last_ms_ >= 250) Line(fraction, text);
  }

 private:
  HANDLE h_ = nullptr;
  int64_t last_ms_ = 0;
};

std::string Mb(uint64_t bytes) { return std::to_string(bytes >> 20) + " MB"; }

// Lowercase absolute form, for "is one folder inside the other" (Windows paths ignore case).
std::wstring Key(const fs::path& p) {
  std::error_code ec;
  std::wstring s = fs::weakly_canonical(fs::absolute(p, ec), ec).wstring();
  for (wchar_t& c : s) c = wchar_t(std::towlower(c));
  while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  return s;
}
bool Inside(const fs::path& inner, const fs::path& outer) {
  const std::wstring a = Key(inner), b = Key(outer);
  return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && (a[b.size()] == L'\\');
}

int FromIso(const fs::path& iso, const fs::path& dest, Out& out) {
  out.Line(0, "Checking your disc image");
  const IsoInfo info = InspectIso(iso);
  if (!info.usable()) {
    out.Line(0, info.error);
    return kWrongCopy;
  }
  Installer installer;  // the GAME ISO tab's own installer
  if (!installer.Start(iso, dest)) {
    out.Line(0, "Couldn't start installing.");
    return kInstallFailed;
  }
  for (;;) {
    const Installer::Snapshot s = installer.Get();
    const double f = s.total_bytes ? double(s.done_bytes) / double(s.total_bytes) : 0.0;
    if (s.finished) {
      if (s.ok) {
        out.Line(1, "Game data installed");
        return kOk;
      }
      out.Line(f, s.message);
      return kInstallFailed;
    }
    out.Throttled(f * 0.99, "Installing game data, " + Mb(s.done_bytes) + " of " +
                                Mb(s.total_bytes));
    Sleep(100);
  }
}

struct CopyProgress {
  Out* out;
  uint64_t base, total;
};
DWORD CALLBACK OnCopyProgress(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER,
                              DWORD, DWORD, HANDLE, HANDLE, LPVOID data) {
  auto* p = static_cast<CopyProgress*>(data);
  const uint64_t now = p->base + uint64_t(done.QuadPart);
  p->out->Throttled(p->total ? double(now) / double(p->total) * 0.99 : 0.0,
                    "Copying game data, " + Mb(now) + " of " + Mb(p->total));
  return PROGRESS_CONTINUE;
}

// An extracted disc folder: the same checks as an ISO, then a copy (written to game.partial and
// moved into place at the end, like the installer).
int FromFolder(const fs::path& src, const fs::path& dest, Out& out) {
  out.Line(0, "Checking your game folder");
  const GameDataStatus st = CheckGameDataDir(src);
  if (!st.found) {
    out.Line(0, "This folder has no default.xex. Pick the extracted disc's folder, or the disc "
                "image (.iso).");
    return kWrongCopy;
  }
  if (!st.exact_version) {
    out.Line(0, "This is Jurassic: The Hunted, but a different release than the one this PC "
                "version was built from (Xbox 360, USA). Its code wouldn't match.");
    return kWrongCopy;
  }
  std::error_code ec;
  if (fs::exists(dest, ec) && fs::equivalent(src, dest, ec)) {
    out.Line(1, "Game data already in place");
    return kOk;
  }
  if (Inside(src, dest) || Inside(dest, src)) {
    out.Line(0, "Pick a folder outside the program's own folder.");
    return kBadArgs;
  }

  std::vector<std::pair<fs::path, uint64_t>> files;  // relative path, size
  uint64_t total = 0;
  for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const uint64_t size = it->file_size(ec);
    files.emplace_back(fs::relative(it->path(), src, ec), size);
    total += size;
  }
  if (ec) {
    out.Line(0, "Couldn't read " + U8(src) + ": " + ec.message());
    return kInstallFailed;
  }
  ULARGE_INTEGER free_bytes{};
  fs::create_directories(dest.parent_path(), ec);
  if (GetDiskFreeSpaceExW(dest.parent_path().c_str(), &free_bytes, nullptr, nullptr) &&
      free_bytes.QuadPart < total + (256ull << 20)) {
    out.Line(0, "Not enough free disk space: " + Mb(total) + " needed.");
    return kInstallFailed;
  }

  fs::path partial = dest;
  partial += ".partial";
  fs::remove_all(partial, ec);
  uint64_t done = 0;
  for (const auto& [rel, size] : files) {
    const fs::path to = partial / rel;
    fs::create_directories(to.parent_path(), ec);
    CopyProgress progress{&out, done, total};
    if (!CopyFileExW((src / rel).c_str(), to.c_str(), OnCopyProgress, &progress, nullptr, 0)) {
      fs::remove_all(partial, ec);
      out.Line(total ? double(done) / double(total) : 0.0,
               "Copying " + U8(rel) + " failed (disk full, or the file can't be read).");
      return kInstallFailed;
    }
    done += size;
  }
  out.Line(0.99, "Verifying");
  if (!CheckGameDataDir(partial).exact_version) {
    fs::remove_all(partial, ec);
    out.Line(0.99, "Verification failed: the copied game executable doesn't match.");
    return kInstallFailed;
  }
  if (fs::exists(dest, ec)) fs::remove_all(dest, ec);  // set up again: replace the old copy
  fs::rename(partial, dest, ec);
  if (ec) {
    out.Line(0.99, "Couldn't move the game data into place: " + ec.message());
    return kInstallFailed;
  }
  out.Line(1, "Game data installed");
  return kOk;
}

}  // namespace

int RunPrepare(const fs::path& game, const fs::path& exe_dir) {
  Out out;
  std::error_code ec;
  if (game.empty()) {
    out.Line(0, "No game given. Use --prepare --game <disc image (.iso) or extracted folder>.");
    return kBadArgs;
  }
  const fs::path dest = exe_dir / "game";
  if (fs::is_directory(game, ec)) return FromFolder(game, dest, out);
  if (!fs::is_regular_file(game, ec)) {
    out.Line(0, "Can't find " + U8(game) + ".");
    return kBadArgs;
  }
  return FromIso(game, dest, out);
}

}  // namespace jth::launcher
