// Mouse in the game's menus, and X / click to skip videos and cutscenes. See menu_mouse.h.
//
// Menu engine layout (guest, big-endian), worked out from the code and runtime dumps:
//   GUI manager (`this` of sub_82126880, once per frame on the game thread)
//     +184 current page (null in gameplay)   +292/+296 pointer x/y in 1280x720 GUI units (float)
//     +307 $GUI.menu_mouse_enabled (already 1)  +308 $GUI.menu_input_enabled
//   page  +4 name (char*)  +22 active  +120 focused element  +124 modal popup
//   sub_8211FA70(page, f1=x, f2=y): pointer moved -> focuses the element under the pointer
//       (among the focused element's siblings, so the button under the cursor in a menu list)
//   sub_8211D3E8(page, f1=x, f2=y): hit test, returns the element under the pointer or 0
//   sub_821BC8A0(mapper, action name, mode): how pressed an input action is (f1); action names
//       are the string literals below, bound to buttons in config\key_binding.txt.

#include "menu_mouse.h"

#include "cheats.h"
#include "dev_lua.h"
#include "pc_options.h"
#include "jurassichunted_funcs.h"
#include "pc_input.h"  // generated: guest function declarations (+ pch macros)
#include "weapon_wheel.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(menu_mouse_debug, false, "Debug", "Log menu pointer hits");
REXCVAR_DEFINE_BOOL(log_input_actions, false, "Debug",
                    "Log which game code queries which input action (and when one first reports "
                    "a value), to find what a game mode reads");

namespace {

// Action name literals the game passes to sub_821BC8A0.
constexpr uint32_t kActionAccept = 0x82018E7C;
constexpr uint32_t kActionBack = 0x82018E68;
constexpr uint32_t kActionUp = 0x82018EE4;
constexpr uint32_t kActionDown = 0x82018EC8;
constexpr uint32_t kActionXKey = 0x82018E54;
constexpr uint32_t kActionSkipCutscene = 0x82018E04;
constexpr uint32_t kActionLookHorizontal = 0x820154F8;
constexpr uint32_t kActionChangeNext = 0x82015160;
constexpr uint32_t kActionChangePrev = 0x82015148;
// Return addresses of the player controller's ChangeNext / ChangePrev queries (sub_821BDxxx,
// command flags 8 / 16).
constexpr uint32_t kChangeNextCaller = 0x821BE00C;
constexpr uint32_t kChangePrevCaller = 0x821BE044;

enum class PageKind : int { kNone, kMenu, kVideo };

// Written by the UI thread.
std::atomic<uint64_t> g_pointer{0};  // x, y as float bits
std::atomic<uint32_t> g_pointer_seq{0};
std::atomic<int> g_left_clicks{0}, g_right_clicks{0}, g_wheel{0};

// Written by the game thread (GUI update), read by the action query and the input driver.
std::atomic<PageKind> g_kind{PageKind::kNone};
std::atomic<bool> g_accept{false}, g_back{false}, g_up{false}, g_down{false}, g_skip{false};

uint32_t Be32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void StoreBeF32(uint8_t* base, uint32_t addr, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
bool Plausible(uint32_t p) { return p >= 0x30000000 && p < 0xE0000000; }

std::string PageName(const uint8_t* base, uint32_t page) {
  std::string name;
  const uint32_t p = Plausible(page) ? Be32(base, page + 4) : 0;
  for (uint32_t i = 0; Plausible(p) && i < 40 && base[p + i] >= 0x20 && base[p + i] < 0x7F; ++i)
    name += char(base[p + i]);
  return name;
}

PageKind Classify(const std::string& name, uint32_t page) {
  if (!page || name == "hud") return PageKind::kNone;  // gameplay or an in-game cutscene
  if (name.starts_with("video_") || name.starts_with("legal_screen")) return PageKind::kVideo;
  return PageKind::kMenu;
}

}  // namespace

namespace jth::menu_mouse {

void OnPointerMove(int x, int y, int width, int height) {
  if (width <= 0 || height <= 0) return;
  // The 1280x720 image is letterboxed into the window.
  const float scale = std::min(width / 1280.0f, height / 720.0f);
  const float gx = (x - (width - 1280.0f * scale) * 0.5f) / scale;
  const float gy = (y - (height - 720.0f * scale) * 0.5f) / scale;
  uint32_t bx, by;
  std::memcpy(&bx, &gx, 4);
  std::memcpy(&by, &gy, 4);
  g_pointer.store(uint64_t(bx) << 32 | by, std::memory_order_relaxed);
  g_pointer_seq.fetch_add(1, std::memory_order_release);
}

void OnClick(bool right) { (right ? g_right_clicks : g_left_clicks).fetch_add(1); }

void OnWheel(int direction) { g_wheel.fetch_add(direction > 0 ? 1 : -1); }

bool InMenu() { return g_kind.load(std::memory_order_relaxed) == PageKind::kMenu; }

}  // namespace jth::menu_mouse

// GUI manager update (r3 = GUI manager, f1 = dt): processes queued input events, then updates
// the pages. Once per frame on the game thread.
REX_HOOK_RAW(sub_82126880) {
  const uint32_t gui = ctx.r3.u32;
  __imp__sub_82126880(ctx, base);
  jth::devlua::RunPending(ctx, base);  // developer tool: queued Lua (test command "lua")
  jth::cheats::OnGuiUpdate(ctx, base);  // infinite grenades
  jth::opts::PollTouchpad();             // PlayStation touchpad: F2 panel

  const uint32_t page = Be32(base, gui + 184);
  static uint32_t last_page = ~0u;
  const std::string name = PageName(base, page);
  const PageKind kind = Classify(name, page);
  if (page != last_page) {
    last_page = page;
    REXLOG_DEBUG("Menu page: '{}'", name);
  }
  g_kind.store(kind, std::memory_order_relaxed);

  const int left = g_left_clicks.exchange(0), right = g_right_clicks.exchange(0);
  const int wheel = g_wheel.exchange(0);
  if (kind == PageKind::kVideo) {
    if (left) g_accept = true;
    return;
  }
  if (kind == PageKind::kNone) {
    if (left) g_skip = true;  // the mouse is only free here outside gameplay: cutscenes
    return;
  }

  // Menu page: hover, click, wheel.
  const uint64_t packed = g_pointer.load(std::memory_order_relaxed);
  float x, y;
  const uint32_t bx = uint32_t(packed >> 32), by = uint32_t(packed);
  std::memcpy(&x, &bx, 4);
  std::memcpy(&y, &by, 4);
  static uint32_t last_seq = 0;
  const uint32_t seq = g_pointer_seq.load(std::memory_order_acquire);
  if (base[gui + 308] /* menu input enabled */ && Plausible(page)) {
    if (seq != last_seq) {
      last_seq = seq;
      StoreBeF32(base, gui + 292, x);
      StoreBeF32(base, gui + 296, y);
      ctx.r3.u64 = page;
      ctx.f1.f64 = x;
      ctx.f2.f64 = y;
      sub_8211FA70(ctx, base);  // pointer moved: focus the element under it
      if (REXCVAR_GET(menu_mouse_debug)) {
        const uint32_t f = Be32(base, page + 120);
        auto rf = [&](uint32_t a) {
          uint32_t v = Be32(base, a);
          float r;
          std::memcpy(&r, &v, 4);
          return r;
        };
        REXLOG_INFO("menu_mouse: pointer ({:.0f},{:.0f}) focus {:08X} rect ({:.0f},{:.0f})-({:.0f},{:.0f})",
                    x, y, f, Plausible(f) ? rf(f + 316) : 0.f, Plausible(f) ? rf(f + 320) : 0.f,
                    Plausible(f) ? rf(f + 324) : 0.f, Plausible(f) ? rf(f + 328) : 0.f);
      }
    }
    if (left) {
      ctx.r3.u64 = page;
      ctx.f1.f64 = x;
      ctx.f2.f64 = y;
      sub_8211D3E8(ctx, base);  // only accept when the click is on an element
      if (ctx.r3.u32) g_accept = true;
    }
  }
  if (right) g_back = true;
  if (wheel > 0) g_up = true;
  if (wheel < 0) g_down = true;
}

namespace {
thread_local bool t_in_balance_update = false;
}  // namespace

// Balancing (fallen trees and beams, e.g. L04 "Far from Shore, Far from Home"). While the player
// controller's +841 flag is set (csControllerActorPlayer:StartBalance), its look update
// (sub_821C7EB0) calls this instead of the normal look code: first an automatic walk onto the
// beam (+840), then the balance itself, steered with LookHorizontal (right stick X; the prompt
// shows "RS <- ->"). On PC, A/D, the arrow keys and the mouse steer it (sub_821BC8A0 below).
REX_HOOK_RAW(sub_821C26A8) {
  jth::pc::OnBalanceFrame(ctx.r3.u32);  // r3 = the player's csControllerActorPlayer
  t_in_balance_update = true;
  __imp__sub_821C26A8(ctx, base);
  t_in_balance_update = false;
}

// How pressed an input action is (r3 = input mapper, r4 = action name, r5 = mode; result f1).
REX_HOOK_RAW(sub_821BC8A0) {
  const uint32_t mapper = ctx.r3.u32, action = ctx.r4.u32, mode = ctx.r5.u32;
  const uint32_t caller = uint32_t(ctx.lr);  // return address: which game code is asking
  __imp__sub_821BC8A0(ctx, base);
  if (t_in_balance_update && action == kActionLookHorizontal) {
    // Axis units: +-1000 at full stick. Added to a controller's stick, if any.
    const double before = ctx.f1.f64;
    ctx.f1.f64 = std::clamp(before + jth::pc::BalanceSteer(), -1000.0, 1000.0);
    static double last_logged = 0.0;
    if ((ctx.f1.f64 != 0.0) != (last_logged != 0.0)) {
      last_logged = ctx.f1.f64;
      REXLOG_DEBUG("Balance steering {:.0f} (stick {:.0f}) from {:08X}", ctx.f1.f64, before, caller);
    }
    return;
  }
  if (REXCVAR_GET(log_input_actions)) {
    // Each (caller, action) pair once, and again whenever it first reports a non-zero value.
    static std::mutex mutex;
    static std::set<uint64_t> seen, active;
    const uint64_t key = uint64_t(caller) << 32 | action;
    std::lock_guard lock(mutex);
    const bool first = seen.insert(key).second;
    const bool now_active = ctx.f1.f64 != 0.0 && active.insert(key).second;
    if (first || now_active) {
      std::string name;
      for (uint32_t i = 0; i < 40 && Plausible(action) && base[action + i]; ++i)
        name += char(base[action + i]);
      REXLOG_INFO("input action: '{}' (mode {}) from {:08X} = {:.3f}{}", name, mode, caller,
                  ctx.f1.f64, first ? "" : " (first non-zero)");
    }
  }
  if (ctx.f1.f64 != 0.0) return;

  auto x_pressed = [&] {
    ctx.r3.u64 = mapper;
    ctx.r4.u64 = kActionXKey;
    ctx.r5.u64 = mode;
    __imp__sub_821BC8A0(ctx, base);
    const bool pressed = ctx.f1.f64 != 0.0;
    ctx.f1.f64 = 0.0;
    return pressed;
  };
  const PageKind kind = g_kind.load(std::memory_order_relaxed);
  bool on = false;
  switch (action) {
    case kActionAccept:
      on = g_accept.exchange(false) || (kind == PageKind::kVideo && x_pressed());
      break;
    case kActionBack:
      on = g_back.exchange(false);
      break;
    case kActionUp:
      on = g_up.exchange(false);
      break;
    case kActionDown:
      on = g_down.exchange(false);
      break;
    case kActionSkipCutscene:
      // Outside menus only; harmless when no cutscene runs (A = Jump fires it all the time).
      on = g_skip.exchange(false) || (kind == PageKind::kNone && x_pressed());
      break;
    case kActionChangeNext:
      // Weapon wheel / number keys: one "next weapon" press at a time until the chosen weapon
      // is in hand (weapon_wheel.cpp).
      on = caller == kChangeNextCaller && jth::wheel::ConsumeChange(false);
      break;
    case kActionChangePrev:
      on = caller == kChangePrevCaller && jth::wheel::ConsumeChange(true);
      break;
  }
  if (on) ctx.f1.f64 = 1.0;
}
