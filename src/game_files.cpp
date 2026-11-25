#include "game_files.h"

#include "crash_log.h"
#include "recomp_achievements.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_STRING(dump_game_files, "", "Debug",
                      "Comma-separated game paths to save as they are read (developer tool)");
REXCVAR_DEFINE_STRING(dump_dir, "", "Debug", "Folder for --dump_game_files");
REXCVAR_DEFINE_BOOL(log_game_files, false, "Debug",
                    "Log every file the game opens, including files inside its .xds archives "
                    "(developer tool, e.g. to find paths for --dump_game_files)");
REXCVAR_DEFINE_BOOL(dev_level_list, false, "Debug",
                    "LEVEL SELECTION opens the game's developer list of every map (all levels, "
                    "locked or not; developer/testing tool)");

namespace jth::files {
namespace {

struct Replacement {
  std::vector<uint8_t> original, replacement;
};
// Path -> possible (original, replacement) pairs; a request can resolve to more than one file (the
// game tries an "xbox\" variant first). Built before the game runs; read-only afterwards.
std::unordered_map<std::string, std::vector<Replacement>> g_replacements;

// Same-length find/replace patches for small text files (see AddPatch).
struct Patch {
  std::string find, replace;
};
std::unordered_map<std::string, std::vector<Patch>> g_patches;

struct Stream {
  std::string path;
  const std::vector<Replacement>* replace = nullptr;
  const std::vector<Patch>* patches = nullptr;
  bool dump = false;
  uint32_t pos = 0;           // bytes read so far (the game reads these files front to back)
  std::vector<uint8_t> data;  // dump only
};
std::mutex g_mutex;
std::map<uint32_t, Stream> g_streams;  // guest stream address -> tracked file

// The read in progress on this thread (between JthStreamRead and JthStreamReadDone).
struct PendingRead {
  uint32_t stream = 0, buf = 0, size = 0;
};
thread_local PendingRead t_read;

std::string Normalize(std::string s) {
  for (char& c : s) c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
  for (std::string_view drive : {"d:\\", "game:\\"})
    if (s.starts_with(drive)) s.erase(0, drive.size());
  return s;
}

const std::set<std::string>& DumpList() {
  static const std::set<std::string> list = [] {
    std::set<std::string> out;
    const std::string s = REXCVAR_GET(dump_game_files);
    size_t start = 0;
    while (start < s.size()) {
      size_t comma = s.find(',', start);
      std::string item = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      item.erase(0, item.find_first_not_of(' '));
      item.erase(item.find_last_not_of(' ') + 1);
      if (!item.empty()) out.insert(Normalize(item));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    return out;
  }();
  return list;
}

bool Active() {
  return !g_replacements.empty() || !g_patches.empty() || !DumpList().empty() ||
         REXCVAR_GET(log_game_files);
}

uint8_t* Guest(uint32_t addr) { return rex::system::kernel_memory()->virtual_membase() + addr; }

void SaveDump(const Stream& s) {
  const std::filesystem::path out = std::filesystem::path(REXCVAR_GET(dump_dir)) / s.path;
  std::error_code ec;
  std::filesystem::create_directories(out.parent_path(), ec);
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(s.data.data()), std::streamsize(s.data.size()));
  REXLOG_INFO("dump: {} ({} bytes) -> {}", s.path, s.data.size(), out.string());
}

}  // namespace

void AddReplacement(const std::string& path, std::vector<uint8_t> original,
                    std::vector<uint8_t> replacement) {
  g_replacements[Normalize(path)].push_back({std::move(original), std::move(replacement)});
}

void AddPatch(const std::string& path, std::string find, std::string replace) {
  if (find.size() != replace.size() || find.empty()) {
    REXLOG_ERROR("game files: patch for {} must keep the length", path);
    return;
  }
  g_patches[Normalize(path)].push_back({std::move(find), std::move(replace)});
}

void SetupDevOptions() {
  if (REXCVAR_GET(dev_level_list)) {
    // The main menu's LEVEL SELECTION opens page "maplist_2" instead of "maplist_scroll": the
    // developer table of every map in maps\maplist order, which loads the chosen row directly.
    // (The game's own hidden "LEVEL SELECTION DEBUG" button does the same.)
    AddPatch("gui\\mainmenu.xml", "$GUI.page=\"maplist_scroll\";", "$GUI.page=\"maplist_2\";     ");
    REXLOG_INFO("dev_level_list: LEVEL SELECTION opens the developer map list");
  }
}

}  // namespace jth::files

using namespace jth::files;

void JthOpenFile(PPCRegister& r3, PPCRegister& r4) {
  if (!r4.u32) return;
  const char* s = reinterpret_cast<const char*>(Guest(r4.u32));
  // A level's main file (maps\<level>\<level>.cew): note it for the log and crash reports.
  if (const size_t n = strnlen(s, 260); n > 4 && _strnicmp(s + n - 4, ".cew", 4) == 0) {
    const std::string map = Normalize(std::string(s, n));
    jth::crash::SetCurrentMap(map);
    jth::ach::OnMapLoaded(map);  // RecompLauncher's "now playing"
  }
  if (!Active()) return;
  const std::string key = Normalize(std::string(s, strnlen(s, 260)));
  if (REXCVAR_GET(log_game_files)) REXLOG_INFO("game file: {}", key);
  auto rep = g_replacements.find(key);
  auto patch = g_patches.find(key);
  const bool dump = DumpList().count(key) && !REXCVAR_GET(dump_dir).empty();
  std::lock_guard lock(g_mutex);
  if (rep == g_replacements.end() && patch == g_patches.end() && !dump) {
    g_streams.erase(r3.u32);  // stream object reused for another file
    return;
  }
  Stream stream;
  stream.path = key;
  stream.replace = rep != g_replacements.end() ? &rep->second : nullptr;
  stream.patches = patch != g_patches.end() ? &patch->second : nullptr;
  stream.dump = dump;
  g_streams[r3.u32] = std::move(stream);
}

void JthStreamRead(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  t_read = {};
  if (!Active()) return;
  std::lock_guard lock(g_mutex);
  if (g_streams.count(r3.u32)) t_read = {r3.u32, r4.u32, r5.u32};
}

void JthStreamReadDone(PPCRegister& r3) {
  if (!t_read.stream) return;
  const PendingRead read = t_read;
  t_read = {};
  const int32_t got = static_cast<int32_t>(r3.u32);
  std::lock_guard lock(g_mutex);
  auto it = g_streams.find(read.stream);
  if (it == g_streams.end() || got <= 0) return;
  Stream& s = it->second;
  const uint32_t n = std::min<uint32_t>(uint32_t(got), read.size);
  uint8_t* buf = Guest(read.buf);
  if (s.replace)
    for (const Replacement& r : *s.replace)
      if (size_t(s.pos) + n <= r.original.size() && size_t(s.pos) + n <= r.replacement.size() &&
          std::memcmp(buf, r.original.data() + s.pos, n) == 0) {
        std::memcpy(buf, r.replacement.data() + s.pos, n);
        REXLOG_DEBUG("Replaced {} bytes {}-{} as they were read", s.path, s.pos, s.pos + n);
        break;
      }
  if (s.patches)
    for (const Patch& p : *s.patches) {
      char* text = reinterpret_cast<char*>(buf);
      for (char* at = std::search(text, text + n, p.find.begin(), p.find.end()); at != text + n;
           at = std::search(at + p.find.size(), text + n, p.find.begin(), p.find.end())) {
        std::memcpy(at, p.replace.data(), p.replace.size());
        REXLOG_DEBUG("Patched {} at {}: {}", s.path, s.pos + uint32_t(at - text), p.replace);
      }
    }
  if (s.dump) {
    if (s.data.size() < size_t(s.pos) + n) s.data.resize(size_t(s.pos) + n);
    std::memcpy(s.data.data() + s.pos, buf, n);  // what the game sees
  }
  s.pos += n;
}

void JthStreamClose(PPCRegister& r3) {
  if (!Active()) return;
  std::lock_guard lock(g_mutex);
  auto it = g_streams.find(r3.u32);
  if (it == g_streams.end()) return;
  if (it->second.dump) SaveDump(it->second);
  g_streams.erase(it);
}
