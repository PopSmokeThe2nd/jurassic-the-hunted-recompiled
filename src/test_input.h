// Scripted controller input for automated testing.
//
// When launched with --test_input_file <path>, a synthetic controller is merged into player 1's
// input. The file is watched while the game runs; each new line is a command:
//
//   tap <buttons> [ms]          press then release (default 120 ms down, 150 ms up)
//   hold <buttons> <ms>         press for <ms>
//   stick <l|r> <x> <y> <ms>    move a stick; x/y in -1..1 (y up is positive)
//   trigger <l|r> <0..1> <ms>   pull a trigger
//   wait <ms>                   neutral input
//   key <name>[+name...] [ms]   hold PC keys (default 120 ms), e.g. "key space", "key lshift+w 3000"
//       ... mouse <dx> <dy> <after ms>   also move the mouse that long into the hold, e.g.
//       "key q 2000 mouse 0 -80 600" (weapon wheel: hold Q, point up)
//   mouse <dx> <dy> [ms]        move the mouse by (dx, dy) counts, then wait (default 60 ms)
//   set <setting> <value>       change a setting right away (not queued), e.g. "set fov 110"
//   lua <code>                  run Lua in the game's script state (in sequence; the rest of the line
//                               as written); the result or error is logged ("lua: ..."), see dev_lua.h
//   pointer <x> <y> [ms]        move the menu cursor (1280x720 GUI units)
//   click [left|right] [ms]     mouse click (menus, videos, cutscenes)
//   wheel <up|down> [ms]        mouse wheel notch
//
// <buttons> is one or more of a b x y start back lb rb ls rs up down left right guide joined
// with '+', e.g. "tap a", "hold lb+rb 500". Lines starting with '#' are ignored.

#pragma once

#include <memory>

#include <rex/input/input_driver.h>

namespace jth {

// The scripted test controller, or nullptr if --test_input_file was not given.
std::unique_ptr<rex::input::InputDriver> CreateScriptedInputDriver();

}  // namespace jth
