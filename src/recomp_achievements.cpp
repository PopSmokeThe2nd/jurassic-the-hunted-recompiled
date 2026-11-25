#include "recomp_achievements.h"

#include "version.h"

#include <rl_achievements.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(log_achievements, false, "Debug",
                    "Log what is sent to RecompLauncher's Recomp Achievements (developer tool)");
REXCVAR_DEFINE_INT32(debug_unlock_achievement, 0, "Debug",
                     "Unlock this achievement id now, as the game would (developer tool for "
                     "testing; use a throwaway --user_data_root)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Cheats (cheats.cpp) and the developer level list (game_files.cpp): any of them on makes the
// session's unlocks Casual.
REXCVAR_DECLARE(bool, cheat_god_mode);
REXCVAR_DECLARE(bool, cheat_infinite_ammo);
REXCVAR_DECLARE(bool, cheat_infinite_adrenaline);
REXCVAR_DECLARE(bool, cheat_infinite_grenades);
REXCVAR_DECLARE(bool, cheat_one_hit_kill);
REXCVAR_DECLARE(double, cheat_super_speed);
REXCVAR_DECLARE(bool, dev_level_list);

namespace {

constexpr const char* kRecompId = "jurassic-the-hunted-recompiled";  // RecompLauncher catalog id
constexpr uint32_t kXContentSavedGame = 1;
constexpr uint32_t kDispositionOpened = 2;

bool g_started = false;       // game thread
bool g_connected = false;     // game thread
bool g_sent_cheats = false;   // game thread
bool g_sent_debug = false;    // game thread
int g_last_quiet = -1;        // game thread

std::mutex g_map_mutex;
std::string g_pending_map;  // set on the loading thread, sent from the game thread

bool Log() { return REXCVAR_GET(log_achievements); }

bool AnyCheat() {
  return REXCVAR_GET(cheat_god_mode) || REXCVAR_GET(cheat_infinite_ammo) ||
         REXCVAR_GET(cheat_infinite_adrenaline) || REXCVAR_GET(cheat_infinite_grenades) ||
         REXCVAR_GET(cheat_one_hit_kill) || REXCVAR_GET(cheat_super_speed) > 1.0;
}

// The chapters, as the game's level list (scripts/maps.xml) and menus name them.
std::string ChapterName(const std::string& map) {
  struct Chapter {
    const char* file;  // maps\<dir>\<file>.cew, lower case
    int number;
    const char* name;
  };
  static const Chapter kChapters[] = {
      {"l02.cew", 1, "Code: Whiskey-Tango-Foxtrot"},
      {"l04.cew", 2, "Far from Shore, Far from Home"},
      {"l15.cew", 3, "Sinking Feeling"},
      {"l07.cew", 4, "Vortex Zero"},
      {"l13.cew", 5, "Out of the Frying Pan"},
      {"l08a.cew", 6, "The Lost Castaways"},
      {"l16.cew", 7, "Rise Above"},
      {"l16_b.cew", 8, "The Tyrant King"},
      {"l10.cew", 9, "U-1226"},
      {"l03.cew", 10, "Special Delivery"},
      {"l03_b.cew", 11, "Enter: \"Spike\""},
      {"l08b.cew", 12, "Escaping Extinction"},
      {"l17.cew", 13, "The Final Piece"},
      {"l08c.cew", 14, "No Time Like the Present"},
  };
  std::string m = map;
  std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const size_t slash = m.find_last_of("\\/");
  const std::string file = slash == std::string::npos ? m : m.substr(slash + 1);
  if (file.rfind("survival", 0) == 0) return "Survival";
  for (const Chapter& c : kChapters)
    if (file == c.file) return "Chapter " + std::to_string(c.number) + ": " + c.name;
  return {};
}

void Start() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  const auto list = kernel->achievements().ListAchievements();
  if (list.empty()) return;  // the title's achievements aren't loaded yet
  g_started = true;

  rl_ach_set_info(kRecompId, JTH_VERSION);
  // The names, descriptions and gamerscore ReXGlue read from the XEX (the launcher can't).
  for (const auto& a : list)
    rl_ach_native_define(a.id, a.label.c_str(), a.description.c_str(), a.gamerscore);
  g_connected = rl_ach_init("x360", nullptr, nullptr) != 0;
  REXLOG_INFO("Recomp Achievements: {} ({} achievements)",
              g_connected ? "connected to RecompLauncher" : "not started from RecompLauncher",
              list.size());
  if (!g_connected) return;

  // Unlocks from before this session (or before the launcher saw this game): sent once; the
  // launcher keeps them as earlier, so they can't jump the leaderboards.
  int earlier = 0;
  for (const auto& a : list)
    if (kernel->achievements().IsUnlocked(a.id)) {
      rl_ach_native_earlier(a.id);
      ++earlier;
    }
  if (Log()) REXLOG_INFO("Recomp Achievements: {} earlier unlock(s) sent", earlier);

  kernel->achievements().RegisterUnlockCallback([](const rex::system::AchievementEvent& e) {
    rl_ach_native(e.achievement.id);  // the library is thread-safe
    if (REXCVAR_GET(log_achievements))
      REXLOG_INFO("Recomp Achievements: unlock {} '{}'", e.achievement.id, e.achievement.label);
  });
}

}  // namespace

namespace jth::ach {

void Install() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  kernel->set_content_opened_callback([](uint32_t type, uint32_t disposition, uint32_t flags) {
    if (REXCVAR_GET(log_achievements))
      REXLOG_INFO("Recomp Achievements: content type {} {} (flags {:#x})", type,
                  disposition == kDispositionOpened ? "opened" : "created", flags);
    // An existing saved game opened to read it: a save load (creating or overwriting one is a
    // save, not a load).
    if (type == kXContentSavedGame && disposition == kDispositionOpened && (flags & 0xF) == 3) {
      rl_ach_save_loaded();
      if (REXCVAR_GET(log_achievements)) REXLOG_INFO("Recomp Achievements: save loaded");
    }
  });
}

void OnGuiUpdate() {
  if (!g_started) {
    Start();
    if (!g_started) return;
  }
  // Developer tool: unlock an achievement on request, through the same path as the game's own
  // XGIUserWriteAchievements (KernelState::UnlockAchievement).
  static int last_debug_unlock = 0;
  if (const int id = REXCVAR_GET(debug_unlock_achievement); id != last_debug_unlock) {
    last_debug_unlock = id;
    if (id > 0)
      if (auto* kernel = rex::system::kernel_state()) {
        REXLOG_INFO("Recomp Achievements: debug unlock of achievement {}", id);
        kernel->UnlockAchievement(uint32_t(id));
      }
  }
  if (!g_connected) return;
  rl_ach_frame();

  // RecompLauncher shows its own toast: hide ReXGlue's meanwhile.
  const int quiet = rl_ach_quiet_toasts();
  if (quiet != g_last_quiet) {
    g_last_quiet = quiet;
    if (auto* kernel = rex::system::kernel_state()) kernel->set_show_achievement_toasts(!quiet);
    if (Log()) REXLOG_INFO("Recomp Achievements: ReXGlue toasts {}", quiet ? "off" : "on");
  }

  if (!g_sent_cheats && AnyCheat()) {
    g_sent_cheats = true;
    rl_ach_casual("cheats");
    REXLOG_INFO("Recomp Achievements: a cheat is on; this session's unlocks count as Casual");
  }
  if (!g_sent_debug && REXCVAR_GET(dev_level_list)) {
    g_sent_debug = true;
    rl_ach_casual("debug");
    REXLOG_INFO("Recomp Achievements: developer level list on; unlocks count as Casual");
  }

  std::string map;
  {
    std::lock_guard lock(g_map_mutex);
    map.swap(g_pending_map);
  }
  if (!map.empty()) {
    const std::string chapter = ChapterName(map);
    if (!chapter.empty()) {
      rl_ach_presence(chapter.c_str());
      if (Log()) REXLOG_INFO("Recomp Achievements: now playing '{}'", chapter);
    }
  }
}

void OnMapLoaded(const std::string& map) {
  std::lock_guard lock(g_map_mutex);
  g_pending_map = map;
}

}  // namespace jth::ach
