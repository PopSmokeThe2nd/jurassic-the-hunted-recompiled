// Native PC controls: keyboard + mouse buttons drive a virtual controller, and mouse movement
// is applied straight to the camera (see JthMouseLook in pc_input.cpp and the [[midasm_hook]]
// in config/default.toml), so aiming is 1:1 with no thumbstick acceleration.
//
// Settings (command line --name=value, or jurassichunted.toml next to the exe):
//   pc_controls        enable keyboard/mouse (default true)
//   mouse_sensitivity  1.0 = 0.05 degrees per mouse count
//   bind_*             comma-separated key names per action, e.g. bind_jump = "Space"

#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <rex/input/input_driver.h>
#include <rex/ppc/context.h>

namespace jth::pc {

// The keyboard/mouse device (merged into player 1), or nullptr if pc_controls is off.
std::unique_ptr<rex::input::InputDriver> CreatePcInputDriver();

// Virtual-key code for a key name ("W", "Space", "LShift", "LMB", "Mouse4"...), or 0.
int KeyFromName(std::string_view name);

// Testing hooks used by the scripted input driver.
void AddMouseDelta(float dx, float dy);
void SetTestKey(int vk, bool down);

// True while the in-game camera is taking look input (i.e. not in a menu or cutscene).
bool GameplayLookActive();

// The controller buttons the keyboard/mouse reported in its latest state (e.g. the Y pulses of a
// Q tap or mouse-wheel step), so pad_filter.cpp can tell them from a real controller's.
uint16_t KeyboardButtons();

// Balancing (fallen trees / beams, e.g. L04): while the player controller's +841 flag is set
// (StartBalance), its update runs the balance code (sub_821C26A8) instead of the normal look,
// which steers with LookHorizontal (right stick X). Called once per frame from that update
// (menu_mouse.cpp hook): keeps gameplay state (mouse capture, cheats) and collects this frame's
// mouse movement for steering.
void OnBalanceFrame(uint32_t controller);
// Steering for the balance's LookHorizontal query, in the game's axis units (-1000..1000):
// A/D or the arrow keys (full), plus this frame's horizontal mouse movement.
double BalanceSteer();

// Runs fn on the window's UI thread (deferred). No-op before the window exists.
void RunOnUiThread(std::function<void()> fn);

}  // namespace jth::pc

// Mid-asm hook in the player's look update (sub_821C7EB0 @ 0x821C80CC): adds mouse movement to
// the frame's yaw/pitch deltas (stack floats at r1+80 / r1+84, radians) after the stick response
// curve and aim assist, but before zoom scaling, cutscene look-lock and the aiming slow-down.
void JthMouseLook(PPCRegister& r1, PPCRegister& r31);
