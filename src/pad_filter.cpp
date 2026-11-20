#include "pad_filter.h"

#include "pc_input.h"
#include "weapon_wheel.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(toggle_sprint, false, "Controls",
                    "Sprint toggles: press sprint (left stick click / Shift) once to run until "
                    "you stop moving or press it again")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(log_pad_filter, false, "Debug",
                    "Log the controller weapon wheel and toggle sprint decisions (developer tool)");

namespace {

using rex::input::X_INPUT_STATE;
constexpr uint16_t kY = rex::input::X_INPUT_GAMEPAD_Y;
constexpr uint16_t kB = rex::input::X_INPUT_GAMEPAD_B;
constexpr uint16_t kSprint = rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;

constexpr int64_t kWheelHoldNs = 220'000'000;  // Y held longer than this opens the wheel (as Q)
constexpr int64_t kTapPulseNs = 100'000'000;   // a tap is replayed as Y held this long
constexpr int64_t kStillNs = 300'000'000;      // toggle sprint ends after standing still this long
constexpr float kMovingStick = 0.3f;           // left stick past this counts as moving

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Y / triangle: kIdle -> kPending (down, the game doesn't see it) -> a tap (released early: Y is
// replayed briefly) or kOpen (the wheel). kSwallow hides Y until it's released (the wheel was
// cancelled or closed some other way); kPassThrough is a press that started outside gameplay.
enum class YState { kIdle, kPending, kOpen, kSwallow, kPassThrough };

struct Pad {
  YState y = YState::kIdle;
  int64_t y_since = 0;
  int64_t pulse_until = 0;
  bool hold_b = false;  // B cancelled the wheel: hidden until released
  bool b_was = false;
  // Toggle sprint.
  bool sprint_was = false;
  bool latched = false;
  bool hold_sprint = false;  // the press that ended a latched sprint: hidden until released
  int64_t still_since = 0;
};

std::mutex g_mutex;
std::array<Pad, 4> g_pads;

void Log(const char* what) {
  if (REXCVAR_GET(log_pad_filter)) REXLOG_INFO("Pad: {}", what);
}

void Filter(uint32_t user, X_INPUT_STATE* state) {
  if (user >= g_pads.size() || !state) return;
  std::lock_guard lock(g_mutex);
  Pad& p = g_pads[user];
  const int64_t now = NowNs();
  uint16_t buttons = state->gamepad.buttons;
  int lx = state->gamepad.thumb_lx, ly = state->gamepad.thumb_ly;
  const float stick_x = float(lx) / 32767.0f, stick_y = float(ly) / 32767.0f;
  const bool gameplay = jth::pc::GameplayLookActive();

  // ----- Weapon wheel on Y / triangle -----
  // The keyboard's own Y pulses (a Q tap, a mouse-wheel notch) pass straight through.
  const bool y = (buttons & kY) && !(jth::pc::KeyboardButtons() & kY);
  const bool b = (buttons & kB) != 0;
  if (!gameplay || !jth::wheel::Enabled()) {
    if (p.y == YState::kOpen) jth::wheel::Cancel();
    if (p.y == YState::kPending) jth::wheel::EndHold();
    p.y = y ? YState::kPassThrough : YState::kIdle;
    p.pulse_until = 0;
  } else {
    switch (p.y) {
      case YState::kIdle:
        if (y) {
          p.y = YState::kPending;
          p.y_since = now;
          jth::wheel::BeginHold();
        }
        break;
      case YState::kPassThrough:
        if (!y) p.y = YState::kIdle;
        break;
      case YState::kSwallow:
        if (!y) p.y = YState::kIdle;
        break;
      case YState::kPending:
        if (!y) {
          jth::wheel::EndHold();
          p.y = YState::kIdle;
          p.pulse_until = now + kTapPulseNs;  // a tap: next weapon, as before
          Log("Y tap: next weapon");
        } else if (now - p.y_since >= kWheelHoldNs && !jth::wheel::IsOpen()) {
          jth::wheel::Open(true);
          p.y = YState::kOpen;
          Log("Y held: wheel open");
        }
        break;
      case YState::kOpen:
        if (!jth::wheel::IsOpen()) {
          p.y = y ? YState::kSwallow : YState::kIdle;  // closed by the mouse / Escape
        } else if (!y) {
          jth::wheel::Confirm();
          p.y = YState::kIdle;
          Log("Y released: switch");
        } else if (b && !p.b_was) {
          jth::wheel::Cancel();
          p.hold_b = true;
          p.y = YState::kSwallow;
          Log("B: wheel cancelled");
        } else {
          jth::wheel::SetStick(stick_x, stick_y);
        }
        break;
    }
  }
  p.b_was = b;
  if (!b) p.hold_b = false;

  const bool hide_y =
      p.y == YState::kPending || p.y == YState::kOpen || p.y == YState::kSwallow;
  if (hide_y) buttons &= uint16_t(~kY);
  if (now < p.pulse_until) buttons |= kY;
  if (p.y == YState::kOpen || p.hold_b) buttons &= uint16_t(~kB);
  if (p.y == YState::kOpen) lx = ly = 0;  // the stick points at weapons instead of walking

  // ----- Toggle sprint -----
  const bool sprint = (buttons & kSprint) != 0;
  if (!REXCVAR_GET(toggle_sprint) || !gameplay) {
    p.latched = false;
    p.hold_sprint = false;
  } else {
    if (sprint && !p.sprint_was) {
      if (p.latched) {
        p.latched = false;
        p.hold_sprint = true;
        Log("sprint pressed again: stop");
      } else {
        p.latched = true;
        p.still_since = 0;
        Log("sprint on");
      }
    }
    if (!sprint) p.hold_sprint = false;
    if (p.latched) {
      if (std::hypot(stick_x, stick_y) < kMovingStick) {
        if (!p.still_since) p.still_since = now;
        if (now - p.still_since >= kStillNs) {
          p.latched = false;
          Log("stopped moving: sprint off");
        }
      } else {
        p.still_since = 0;
      }
    }
    if (p.latched)
      buttons |= kSprint;
    else if (p.hold_sprint)
      buttons &= uint16_t(~kSprint);
  }
  p.sprint_was = sprint;

  state->gamepad.buttons = buttons;
  state->gamepad.thumb_lx = int16_t(lx);
  state->gamepad.thumb_ly = int16_t(ly);
}

}  // namespace

namespace jth::pad {

void Install(rex::input::InputSystem* input) {
  if (input) input->SetGuestStateFilter(Filter);
}

}  // namespace jth::pad
