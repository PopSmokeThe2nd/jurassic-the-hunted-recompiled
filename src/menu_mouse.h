// Mouse in the game's menus, and skipping videos/cutscenes with X or a click.
//
// Menus: the menu engine still has the pointer code the Wii/PC versions used (hover focuses the
// element under the pointer); nothing feeds it on Xbox 360, so menu_mouse.cpp does. A left click
// on an element accepts it, a right click goes back, the wheel moves up/down.
//
// Skipping: videos already skip on A/Start and in-game cutscenes on A (the "SkipCutscene" action).
// X and a left click are added by answering the game's action queries ("Accept" on video pages,
// "SkipCutscene" everywhere outside menus).

#pragma once

namespace jth::menu_mouse {

// Window events (UI thread), only while the mouse isn't captured for gameplay.
// x/y: client position in physical pixels; width/height: client size.
void OnPointerMove(int x, int y, int width, int height);
void OnClick(bool right);
void OnWheel(int direction);  // +1 = up, -1 = down

// True while a menu page is shown (mouse buttons shouldn't also act as fire/aim there).
bool InMenu();

}  // namespace jth::menu_mouse
