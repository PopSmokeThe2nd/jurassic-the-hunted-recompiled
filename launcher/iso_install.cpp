#include "iso_install.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <vector>

namespace jth::launcher {
namespace {

constexpr uint64_t kSector = 2048;
constexpr uint64_t kPartitions[] = {0x0, 0xFD90000, 0x2080000, 0x18300000};  // XSF, XGD2, XGD1, XGD3
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

struct Entry {
  std::string path;  // "Maps/L02/l02.xds"
  uint32_t sector = 0;
  uint32_t size = 0;
};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Rejects names that could escape the destination folder or aren't valid file names.
bool SafeName(const std::string& n) {
  if (n.empty() || n == "." || n == "..") return false;
  for (unsigned char c : n)
    if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|')
      return false;
  return true;
}

class Iso {
 public:
  ~Iso() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
  }

  bool Open(const std::filesystem::path& path, std::string& err) {
    h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                     FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
      err = "Couldn't open the file.";
      return false;
    }
    for (uint64_t p : kPartitions) {
      char magic[20];
      if (ReadAt(p + 32 * kSector, magic, 20) && std::memcmp(magic, kMagic, 20) == 0) {
        partition_ = p;
        uint8_t vd[8];
        if (!ReadAt(p + 32 * kSector + 20, vd, 8)) break;
        root_sector_ = uint32_t(vd[0]) | (uint32_t(vd[1]) << 8) | (uint32_t(vd[2]) << 16) |
                       (uint32_t(vd[3]) << 24);
        root_size_ = uint32_t(vd[4]) | (uint32_t(vd[5]) << 8) | (uint32_t(vd[6]) << 16) |
                     (uint32_t(vd[7]) << 24);
        return true;
      }
    }
    err = "This isn't an Xbox 360 disc image.";
    return false;
  }

  bool ReadAt(uint64_t offset, void* buf, uint32_t len) {
    LARGE_INTEGER li;
    li.QuadPart = int64_t(offset);
    if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h_, buf, len, &got, nullptr) && got == len;
  }

  bool ReadFileData(const Entry& e, uint64_t pos, void* buf, uint32_t len) {
    return ReadAt(partition_ + uint64_t(e.sector) * kSector + pos, buf, len);
  }

  bool List(std::vector<Entry>& out, std::string& err) {
    struct Dir {
      uint32_t sector, size;
      std::string prefix;
      int depth;
    };
    std::vector<Dir> dirs{{root_sector_, root_size_, "", 0}};
    std::set<uint32_t> seen_dirs;
    while (!dirs.empty()) {
      Dir d = dirs.back();
      dirs.pop_back();
      if (d.depth > 32 || d.size == 0 || d.size > (16u << 20) || !seen_dirs.insert(d.sector).second)
        continue;
      std::vector<uint8_t> data(d.size);
      if (!ReadAt(partition_ + uint64_t(d.sector) * kSector, data.data(), d.size)) {
        err = "The disc image is truncated or damaged.";
        return false;
      }
      std::vector<uint32_t> stack{0};
      std::set<uint32_t> seen;
      while (!stack.empty()) {
        uint32_t off = stack.back();
        stack.pop_back();
        if (off + 14 > data.size() || !seen.insert(off).second) continue;
        const uint8_t* p = data.data() + off;
        uint16_t left = uint16_t(p[0] | (p[1] << 8));
        uint16_t right = uint16_t(p[2] | (p[3] << 8));
        if (left == 0xFFFF && right == 0xFFFF) continue;  // padding
        uint32_t start = uint32_t(p[4]) | (uint32_t(p[5]) << 8) | (uint32_t(p[6]) << 16) |
                         (uint32_t(p[7]) << 24);
        uint32_t size = uint32_t(p[8]) | (uint32_t(p[9]) << 8) | (uint32_t(p[10]) << 16) |
                        (uint32_t(p[11]) << 24);
        uint8_t attr = p[12], name_len = p[13];
        if (off + 14u + name_len > data.size()) continue;
        std::string name(reinterpret_cast<const char*>(p + 14), name_len);
        if (left) stack.push_back(uint32_t(left) * 4);
        if (right) stack.push_back(uint32_t(right) * 4);
        if (!SafeName(name)) continue;
        if (attr & 0x10) {
          dirs.push_back({start, size, d.prefix + name + "/", d.depth + 1});
        } else {
          out.push_back({d.prefix + name, start, size});
          if (out.size() > 200000) {
            err = "The disc image's file table is damaged.";
            return false;
          }
        }
      }
    }
    if (out.empty()) {
      err = "The disc image contains no files.";
      return false;
    }
    return true;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
  uint64_t partition_ = 0;
  uint32_t root_sector_ = 0, root_size_ = 0;
};

class Sha256 {
 public:
  Sha256() {
    BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (alg_) BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
  }
  ~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  void Update(const void* data, size_t len) {
    if (hash_) BCryptHashData(hash_, (PUCHAR)data, ULONG(len), 0);
  }
  std::string HexDigest() {
    uint8_t digest[32] = {};
    if (hash_) BCryptFinishHash(hash_, digest, 32, 0);
    static const char* kHex = "0123456789ABCDEF";
    std::string s;
    for (uint8_t b : digest) {
      s += kHex[b >> 4];
      s += kHex[b & 15];
    }
    return s;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// Title ID from an XEX2 header (execution info optional header 0x00040006).
uint32_t XexTitleId(const std::vector<uint8_t>& x) {
  if (x.size() < 0x18 || std::memcmp(x.data(), "XEX2", 4) != 0) return 0;
  uint32_t n = Be32(&x[0x14]);
  for (uint32_t i = 0; i < n && 0x18 + 8 * (i + 1) <= x.size(); ++i) {
    uint32_t key = Be32(&x[0x18 + 8 * i]), val = Be32(&x[0x1C + 8 * i]);
    if (key == 0x00040006 && val + 16 <= x.size()) return Be32(&x[val + 12]);
  }
  return 0;
}

std::string HashFile(const std::filesystem::path& p) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  Sha256 sha;
  std::vector<uint8_t> buf(1 << 20);
  DWORD got = 0;
  while (ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr) && got) sha.Update(buf.data(), got);
  CloseHandle(h);
  return sha.HexDigest();
}

const Entry* FindEntry(const std::vector<Entry>& files, const char* path) {
  for (auto& e : files)
    if (_stricmp(e.path.c_str(), path) == 0) return &e;
  return nullptr;
}

}  // namespace

IsoInfo InspectIso(const std::filesystem::path& iso_path) {
  IsoInfo info;
  Iso iso;
  std::vector<Entry> files;
  if (!iso.Open(iso_path, info.error) || !iso.List(files, info.error)) return info;
  const Entry* xex = FindEntry(files, "default.xex");
  if (!xex || xex->size < 0x100 || xex->size > (64u << 20)) {
    info.error = "This disc image has no game executable (default.xex).";
    return info;
  }
  info.readable = true;
  info.file_count = files.size();
  for (auto& e : files) info.data_bytes += e.size;

  std::vector<uint8_t> data(xex->size);
  if (!iso.ReadFileData(*xex, 0, data.data(), xex->size)) {
    info.error = "The disc image is truncated or damaged.";
    return info;
  }
  info.right_game = XexTitleId(data) == kTitleId;
  if (!info.right_game) {
    info.error = "This is a different game, not Jurassic: The Hunted.";
    return info;
  }
  Sha256 sha;
  sha.Update(data.data(), data.size());
  info.exact_version = sha.HexDigest() == kExpectedXexSha256;
  if (!info.exact_version)
    info.error =
        "This is Jurassic: The Hunted, but a different release than the one this PC version was "
        "built from (Xbox 360, USA). Its code wouldn't match.";
  return info;
}

GameDataStatus CheckGameDataDir(const std::filesystem::path& game_dir) {
  GameDataStatus s;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(game_dir / "default.xex", ec)) return s;
  s.found = true;
  s.dir = game_dir;
  s.exact_version = HashFile(game_dir / "default.xex") == kExpectedXexSha256;
  return s;
}

GameDataStatus FindGameData(const std::filesystem::path& start_dir) {
  for (auto dir = start_dir; !dir.empty(); dir = dir.parent_path()) {
    auto s = CheckGameDataDir(dir / "game");
    if (s.found) return s;
    if (dir == dir.parent_path()) break;
  }
  return {};
}

// ---------------------------------------------------------------------------------------------

Installer::~Installer() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
}

bool Installer::Start(const std::filesystem::path& iso, const std::filesystem::path& dest) {
  if (running_) return false;
  if (thread_.joinable()) thread_.join();
  running_ = true;
  finished_ = ok_ = cancel_ = false;
  done_bytes_ = total_bytes_ = 0;
  files_done_ = files_total_ = 0;
  start_ms_ = NowMs();
  SetMessage("Reading disc image...");
  thread_ = std::thread([this, iso, dest] { Run(iso, dest); });
  return true;
}

void Installer::SetMessage(std::string m, std::string current) {
  std::lock_guard lock(mutex_);
  message_ = std::move(m);
  current_file_ = std::move(current);
}

Installer::Snapshot Installer::Get() const {
  Snapshot s;
  s.running = running_;
  s.finished = finished_;
  s.ok = ok_;
  s.cancelled = cancel_ && finished_ && !ok_;
  s.done_bytes = done_bytes_;
  s.total_bytes = total_bytes_;
  s.files_done = files_done_;
  s.files_total = files_total_;
  double secs = (NowMs() - start_ms_) / 1000.0;
  if (secs > 0.5) s.mb_per_s = done_bytes_ / 1048576.0 / secs;
  std::lock_guard lock(mutex_);
  s.message = message_;
  s.current_file = current_file_;
  return s;
}

void Installer::Run(std::filesystem::path iso_path, std::filesystem::path dest) {
  namespace fs = std::filesystem;
  auto fail = [&](std::string why) {
    SetMessage(std::move(why));
    finished_ = true;
    running_ = false;
  };

  IsoInfo info = InspectIso(iso_path);
  if (!info.usable()) return fail(info.error);

  Iso iso;
  std::vector<Entry> files;
  std::string err;
  if (!iso.Open(iso_path, err) || !iso.List(files, err)) return fail(err);
  // Read in disc order: much faster on hard drives and optical-style images.
  std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.sector < b.sector; });
  total_bytes_ = info.data_bytes;
  files_total_ = files.size();

  std::error_code ec;
  fs::path partial = dest;
  partial += ".partial";
  fs::create_directories(dest.parent_path(), ec);
  ULARGE_INTEGER free_bytes{};
  if (GetDiskFreeSpaceExW(dest.parent_path().c_str(), &free_bytes, nullptr, nullptr) &&
      free_bytes.QuadPart < info.data_bytes + (256ull << 20))
    return fail("Not enough free disk space: " + std::to_string(info.data_bytes >> 20) +
                " MB needed.");
  fs::remove_all(partial, ec);
  if (!fs::create_directories(partial, ec) && ec) return fail("Couldn't create " + partial.string());

  std::vector<uint8_t> buf(4 << 20);
  for (const Entry& e : files) {
    if (cancel_) break;
    fs::path out_path = partial / fs::path(e.path).make_preferred();
    fs::create_directories(out_path.parent_path(), ec);
    SetMessage("Installing game data...", e.path);
    HANDLE out = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
      fs::remove_all(partial, ec);
      return fail("Couldn't write " + out_path.string());
    }
    bool good = true;
    for (uint64_t pos = 0; pos < e.size && good && !cancel_;) {
      uint32_t n = uint32_t(std::min<uint64_t>(buf.size(), e.size - pos));
      DWORD wrote = 0;
      good = iso.ReadFileData(e, pos, buf.data(), n) &&
             WriteFile(out, buf.data(), n, &wrote, nullptr) && wrote == n;
      pos += n;
      done_bytes_ += n;
    }
    CloseHandle(out);
    if (!good) {
      fs::remove_all(partial, ec);
      return fail("Copying " + e.path + " failed (disc image damaged or disk full).");
    }
    ++files_done_;
  }
  if (cancel_) {
    SetMessage("Cleaning up...");
    fs::remove_all(partial, ec);
    return fail("Installation cancelled.");
  }

  SetMessage("Verifying...");
  if (HashFile(partial / "default.xex") != kExpectedXexSha256) {
    fs::remove_all(partial, ec);
    return fail("Verification failed: the copied game executable doesn't match.");
  }
  if (fs::exists(dest, ec)) fs::remove_all(dest, ec);  // reinstall: replace the old copy
  fs::rename(partial, dest, ec);
  if (ec) return fail("Couldn't move the game data into place: " + ec.message());

  ok_ = true;
  SetMessage("Game data installed.");
  finished_ = true;
  running_ = false;
}

}  // namespace jth::launcher
