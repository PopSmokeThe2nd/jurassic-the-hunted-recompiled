// Recomp Achievements (RecompLauncher): the game's own Xbox 360 achievements, passed on to the
// launcher, which shows its own unlock toasts and keeps profiles, "Beaten" / "Mastered" games and
// leaderboards. Library: third_party/rl-achievements (github.com/elripalda/RecompAchievements).
// Started without RecompLauncher (no RL_SESSION in the environment), every call does nothing.
//
// What this module sends (the library's ReXGlue bridge, done from app code):
//  - the achievement list ReXGlue read from the XEX (names, descriptions, gamerscore), once the
//    title's achievements are loaded;
//  - unlocks the player already had (sent once; the launcher keeps them as earlier / Casual);
//  - each new unlock, from the SDK's unlock callback;
//  - "save loaded" whenever the game opens an existing saved game (SDK patch:
//    KernelState::set_content_opened_callback), so unlocks right after a load aren't Ranked;
//  - "cheats" (Casual from then on) the first time a cheat is on, and "debug" for the developer
//    level list;
//  - a "now playing" line with the chapter being played.
// While the launcher asks for it ("quiet"), the SDK's own unlock toast is turned off (SDK patch:
// KernelState::set_show_achievement_toasts).
//
//   log_achievements   log what is sent to the launcher (developer tool)

#pragma once

#include <string>

namespace jth::ach {

// Once the kernel exists, before the game starts (save-load tracking).
void Install();

// Game thread, once per frame (GUI manager update).
void OnGuiUpdate();

// A level (.cew) started loading; maps\lXX\... (game_files.cpp).
void OnMapLoaded(const std::string& map);

}  // namespace jth::ach
