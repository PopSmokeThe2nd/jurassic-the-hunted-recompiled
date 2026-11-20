#include "pc_input.h"

#include "cheats.h"
#include "menu_mouse.h"
#include "pc_options.h"
#include "weapon_wheel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vKey);
// GetForegroundWindow, WindowFromPoint, ClipCursor, SetCursor, ...: windows.h (via the SDK
// headers).

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(pc_controls, true, "Controls", "Enable native keyboard and mouse controls");
REXCVAR_DEFINE_BOOL(dev_balance_skip_walk, false, "Debug",
                    "Balancing: skip the automatic walk onto the beam (testing tool)");
REXCVAR_DEFINE_DOUBLE(mouse_sensitivity, 1.0, "Controls",
                      "Mouse look sensitivity (1.0 = 0.05 degrees per mouse count)");

// Layout follows the game's "Layout #1" controller scheme.
REXCVAR_DEFINE_STRING(bind_move_forward, "W", "Controls/Keybinds", "Move forward");
REXCVAR_DEFINE_STRING(bind_move_back, "S", "Controls/Keybinds", "Move back");
REXCVAR_DEFINE_STRING(bind_move_left, "A", "Controls/Keybinds", "Strafe left");
REXCVAR_DEFINE_STRING(bind_move_right, "D", "Controls/Keybinds", "Strafe right");
REXCVAR_DEFINE_STRING(bind_fire, "LMB", "Controls/Keybinds", "Fire (RT)");
REXCVAR_DEFINE_STRING(bind_aim, "RMB", "Controls/Keybinds", "Aim, hold (LT)");
REXCVAR_DEFINE_STRING(bind_jump, "Space", "Controls/Keybinds", "Jump (A)");
REXCVAR_DEFINE_STRING(bind_crouch, "C,LCtrl", "Controls/Keybinds", "Crouch (B)");
REXCVAR_DEFINE_STRING(bind_reload, "R", "Controls/Keybinds", "Reload (X)");
REXCVAR_DEFINE_STRING(bind_change_weapon, "Q", "Controls/Keybinds",
                      "Change weapon (Y); the mouse wheel also changes weapon");
REXCVAR_DEFINE_STRING(bind_grenade, "G", "Controls/Keybinds", "Throw grenade (RB)");
REXCVAR_DEFINE_STRING(bind_adrenaline, "F", "Controls/Keybinds", "Adrenaline (LB)");
REXCVAR_DEFINE_STRING(bind_sprint, "LShift", "Controls/Keybinds", "Sprint (left stick click)");
REXCVAR_DEFINE_STRING(bind_melee, "V,Mouse4", "Controls/Keybinds", "Melee (right stick click)");
REXCVAR_DEFINE_STRING(bind_interact, "E", "Controls/Keybinds", "Interact (D-pad)");
REXCVAR_DEFINE_STRING(bind_objectives, "Tab", "Controls/Keybinds", "Objectives (Back)");
REXCVAR_DEFINE_STRING(bind_pause, "Escape", "Controls/Keybinds",
                      "Pause in game (Start); back out of menus (B)");
REXCVAR_DEFINE_STRING(bind_menu_accept, "Enter", "Controls/Keybinds", "Menu accept (A)");
REXCVAR_DEFINE_STRING(bind_menu_back, "Backspace", "Controls/Keybinds", "Menu back (B)");
REXCVAR_DEFINE_STRING(bind_menu_start, "P", "Controls/Keybinds", "Start button");
REXCVAR_DEFINE_STRING(bind_dpad_up, "Up", "Controls/Keybinds", "D-pad up");
REXCVAR_DEFINE_STRING(bind_dpad_down, "Down", "Controls/Keybinds", "D-pad down");
REXCVAR_DEFINE_STRING(bind_dpad_left, "Left", "Controls/Keybinds", "D-pad left");
REXCVAR_DEFINE_STRING(bind_dpad_right, "Right", "Controls/Keybinds", "D-pad right");

namespace jth::pc {
namespace {

using Clock = std::chrono::steady_clock;
using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

constexpr DeviceId kPcDevice = static_cast<DeviceId>(0x4A54484B);  // "JTHK"
constexpr float kDegreesPerCount = 0.05f;
constexpr auto kGameplayTimeout = std::chrono::milliseconds(250);
constexpr int64_t kGameplayTimeoutNs = 250'000'000;
// Guest address of the game's "Invert Look" option byte (read in sub_821C7EB0).
constexpr uint32_t kGameInvertLookAddr = 0x8274D6CB;

// ----- state shared between the UI thread, the input driver and the camera hook -----
std::mutex g_mouse_mutex;
float g_mouse_dx = 0, g_mouse_dy = 0;
std::atomic<int64_t> g_last_look_ns{0};
// Balancing (see OnBalanceFrame): when the balance update last ran, the steering keys held
// (-1 left, +1 right), and the mouse movement collected for the current frame.
std::atomic<int64_t> g_last_balance_ns{0};
std::atomic<int> g_balance_keys{0};
std::atomic<float> g_balance_mouse_dx{0};
// Set while the right stick we report is a stand-in for mouse movement (so tutorial / "is the
// player looking" checks see look input); the camera hook then drops the stick-driven turn.
std::atomic<bool> g_synthetic_look{false};
// When the mouse last moved, and in which direction (for the stand-in stick).
std::atomic<int64_t> g_last_motion_ns{0};
std::atomic<int> g_last_motion_dir{0};  // 1 right, 2 left, 3 down, 4 up
// The game polls input at ~30 Hz; the window must span a few polls or the stand-in stick
// flickers, which the tutorial does not count as looking.
constexpr int64_t kMotionWindowNs = 150'000'000;

void NoteMotion(float dx, float dy);
std::array<std::atomic<bool>, 256> g_test_keys{};
std::atomic<int> g_wheel_steps{0};
std::atomic<uint16_t> g_kb_buttons{0};  // the buttons the keyboard reported last (KeyboardButtons)
std::atomic<rex::ui::Window*> g_window{nullptr};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

struct KeyName {
  const char* name;
  int vk;
};
constexpr KeyName kKeyNames[] = {
    {"LMB", 0x01},       {"RMB", 0x02},      {"MMB", 0x04},       {"Mouse4", 0x05},
    {"Mouse5", 0x06},    {"Backspace", 0x08}, {"Tab", 0x09},      {"Enter", 0x0D},
    {"Return", 0x0D},    {"Shift", 0x10},    {"Ctrl", 0x11},      {"Control", 0x11},
    {"Alt", 0x12},       {"CapsLock", 0x14}, {"Escape", 0x1B},    {"Esc", 0x1B},
    {"Space", 0x20},     {"PageUp", 0x21},   {"PageDown", 0x22},  {"End", 0x23},
    {"Home", 0x24},      {"Left", 0x25},     {"Up", 0x26},        {"Right", 0x27},
    {"Down", 0x28},      {"Insert", 0x2D},   {"Delete", 0x2E},    {"LShift", 0xA0},
    {"RShift", 0xA1},    {"LCtrl", 0xA2},    {"RCtrl", 0xA3},     {"LAlt", 0xA4},
    {"RAlt", 0xA5},      {"Semicolon", 0xBA}, {"Equals", 0xBB},   {"Comma", 0xBC},
    {"Minus", 0xBD},     {"Period", 0xBE},   {"Slash", 0xBF},     {"Grave", 0xC0},
    {"Tilde", 0xC0},     {"LBracket", 0xDB}, {"Backslash", 0xDC}, {"RBracket", 0xDD},
    {"Quote", 0xDE},
};

bool IEquals(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

std::vector<int> ParseBind(const std::string& spec, const char* action) {
  std::vector<int> keys;
  size_t start = 0;
  while (start <= spec.size()) {
    size_t comma = spec.find(',', start);
    std::string_view token(spec.data() + start,
                           (comma == std::string::npos ? spec.size() : comma) - start);
    while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
    while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
    if (!token.empty()) {
      if (int vk = KeyFromName(token))
        keys.push_back(vk);
      else
        REXLOG_WARN("PC controls: unknown key '{}' for {}", token, action);
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return keys;
}

bool KeyDown(int vk, bool use_os) {
  if (g_test_keys[vk & 0xFF].load(std::memory_order_relaxed)) return true;
  return use_os && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// What keeps the game from having the keyboard and mouse right now. Checked every poll as well as
// relying on the window's focus events: the Windows key / Start menu must always free the mouse,
// and keys typed elsewhere must not reach the game (GetAsyncKeyState sees every key).
enum class Blocker { kNone, kBackground, kOverlay };

// kBackground: another process's window is the foreground window (Start menu, Alt+Tab, ...).
// kOverlay: a system overlay covers the game while the game stays the foreground window - the
// Xbox Game Bar (Win+G) works this way, so focus events never fire for it. Detected as a large
// window of another process at the center of the game window; click-through overlays (FPS
// counters, Discord) are skipped by WindowFromPoint. `other_pid` gets the blocking process.
Blocker InputBlocker(DWORD* other_pid = nullptr) {
  const DWORD self = GetCurrentProcessId();
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (fg) GetWindowThreadProcessId(fg, &pid);
  if (!fg || pid != self) {
    if (other_pid) *other_pid = pid;
    return Blocker::kBackground;
  }
  RECT client;
  if (!GetClientRect(fg, &client) || client.right <= 0 || client.bottom <= 0) return Blocker::kNone;
  POINT center{client.right / 2, client.bottom / 2};
  ClientToScreen(fg, &center);
  HWND hit = WindowFromPoint(center);
  HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
  if (!root) return Blocker::kNone;
  DWORD hit_pid = 0;
  GetWindowThreadProcessId(root, &hit_pid);
  RECT r;
  if (hit_pid != self && GetWindowRect(root, &r) &&
      (r.right - r.left) * 5 >= client.right * 4 && (r.bottom - r.top) * 5 >= client.bottom * 4) {
    if (other_pid) *other_pid = hit_pid;
    return Blocker::kOverlay;
  }
  return Blocker::kNone;
}

// Executable name of a process, for the log.
std::string ProcessName(DWORD pid) {
  std::string name;
  if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
      std::wstring_view path(buf, n);
      const size_t slash = path.find_last_of(L"\\/");
      for (wchar_t ch : path.substr(slash == std::wstring_view::npos ? 0 : slash + 1))
        name += ch < 128 ? char(ch) : '?';
    }
    CloseHandle(h);
  }
  return name.empty() ? "process " + std::to_string(pid) : name;
}

// Sets the OS cursor directly (SDL only does it while it believes the pointer is over the window).
// Only affects the pointer while Windows considers it over one of our windows.
void SetOsCursor(bool visible) {
  SetCursor(visible ? LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) : nullptr);  // IDC_ARROW
}


bool AnyDown(const std::vector<int>& keys, bool use_os) {
  for (int vk : keys)
    if (KeyDown(vk, use_os)) return true;
  return false;
}

// Like AnyDown, but ignores mouse buttons when skip_mouse is set.
bool AnyDownExceptMouse(const std::vector<int>& keys, bool use_os, bool skip_mouse) {
  for (int vk : keys) {
    const bool mouse = vk == 0x01 || vk == 0x02 || vk == 0x04 || vk == 0x05 || vk == 0x06;
    if (!(skip_mouse && mouse) && KeyDown(vk, use_os)) return true;
  }
  return false;
}

class PcInputDriver final : public rex::input::InputDriver,
                            public rex::ui::WindowInputListener,
                            public rex::ui::WindowListener {
 public:
  PcInputDriver() : InputDriver(nullptr, 0) {}
  ~PcInputDriver() override {
    if (window_) {
      window_->RemoveInputListener(this);
      window_->RemoveListener(this);
    }
  }

  X_STATUS Setup() override {
    forward_ = ParseBind(REXCVAR_GET(bind_move_forward), "bind_move_forward");
    back_ = ParseBind(REXCVAR_GET(bind_move_back), "bind_move_back");
    left_ = ParseBind(REXCVAR_GET(bind_move_left), "bind_move_left");
    right_ = ParseBind(REXCVAR_GET(bind_move_right), "bind_move_right");
    fire_ = ParseBind(REXCVAR_GET(bind_fire), "bind_fire");
    aim_ = ParseBind(REXCVAR_GET(bind_aim), "bind_aim");
    pause_ = ParseBind(REXCVAR_GET(bind_pause), "bind_pause");
    // Change weapon (Y) is handled in GetDeviceState: with the weapon wheel, a tap changes weapon
    // and holding opens the wheel.
    change_weapon_ = ParseBind(REXCVAR_GET(bind_change_weapon), "bind_change_weapon");
    auto add = [this](const std::string& spec, const char* name, uint16_t bit) {
      buttons_.push_back({ParseBind(spec, name), bit});
    };
    using namespace rex::input;
    add(REXCVAR_GET(bind_jump), "bind_jump", X_INPUT_GAMEPAD_A);
    add(REXCVAR_GET(bind_menu_accept), "bind_menu_accept", X_INPUT_GAMEPAD_A);
    add(REXCVAR_GET(bind_crouch), "bind_crouch", X_INPUT_GAMEPAD_B);
    add(REXCVAR_GET(bind_menu_back), "bind_menu_back", X_INPUT_GAMEPAD_B);
    add(REXCVAR_GET(bind_reload), "bind_reload", X_INPUT_GAMEPAD_X);
    add(REXCVAR_GET(bind_grenade), "bind_grenade", X_INPUT_GAMEPAD_RIGHT_SHOULDER);
    add(REXCVAR_GET(bind_adrenaline), "bind_adrenaline", X_INPUT_GAMEPAD_LEFT_SHOULDER);
    add(REXCVAR_GET(bind_sprint), "bind_sprint", X_INPUT_GAMEPAD_LEFT_THUMB);
    add(REXCVAR_GET(bind_melee), "bind_melee", X_INPUT_GAMEPAD_RIGHT_THUMB);
    add(REXCVAR_GET(bind_interact), "bind_interact", X_INPUT_GAMEPAD_DPAD_UP);
    add(REXCVAR_GET(bind_objectives), "bind_objectives", X_INPUT_GAMEPAD_BACK);
    add(REXCVAR_GET(bind_menu_start), "bind_menu_start", X_INPUT_GAMEPAD_START);
    add(REXCVAR_GET(bind_dpad_up), "bind_dpad_up", X_INPUT_GAMEPAD_DPAD_UP);
    add(REXCVAR_GET(bind_dpad_down), "bind_dpad_down", X_INPUT_GAMEPAD_DPAD_DOWN);
    add(REXCVAR_GET(bind_dpad_left), "bind_dpad_left", X_INPUT_GAMEPAD_DPAD_LEFT);
    add(REXCVAR_GET(bind_dpad_right), "bind_dpad_right", X_INPUT_GAMEPAD_DPAD_RIGHT);
    // Number keys pick a weapon (weapon wheel), unless bound to something else.
    for (int d = 0; d < 9; ++d) {
      const int vk = '1' + d;
      auto bound = [vk](const std::vector<int>& keys) {
        return std::find(keys.begin(), keys.end(), vk) != keys.end();
      };
      digit_free_[d] = !bound(forward_) && !bound(back_) && !bound(left_) && !bound(right_) &&
                       !bound(fire_) && !bound(aim_) && !bound(pause_) && !bound(change_weapon_);
      for (auto& [keys, bit] : buttons_) digit_free_[d] = digit_free_[d] && !bound(keys);
    }
    REXLOG_INFO("PC controls enabled (mouse sensitivity {:.2f})", REXCVAR_GET(mouse_sensitivity));
    return X_STATUS_SUCCESS;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kPcDevice;
    info.name = "Keyboard and mouse";
    info.synthetic = true;  // merges into player 1 alongside any controller
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kPcDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    // Window focused and not covered by a system overlay, no SDK overlay grabbing input, and our
    // PC Options panel closed.
    DWORD other_pid = 0;
    const Blocker blocker = InputBlocker(&other_pid);
    const bool foreground = blocker == Blocker::kNone;
    const bool active = foreground && is_active() && !jth::opts::PcOptionsOpen();
    const bool gameplay = GameplayLookActive();
    if (blocker != last_blocker_) {
      if (blocker == Blocker::kBackground)
        REXLOG_INFO("PC controls: {} took focus; mouse and keyboard released",
                    other_pid ? ProcessName(other_pid) : std::string("another window"));
      else if (blocker == Blocker::kOverlay)
        REXLOG_INFO("PC controls: an overlay ({}) is covering the game, e.g. the Xbox Game Bar; "
                    "mouse and keyboard released",
                    ProcessName(other_pid));
      else
        REXLOG_INFO("PC controls: game has focus again");
      last_blocker_ = blocker;
    }
    if (!foreground && was_foreground_.exchange(false) && window_)
      window_->app_context().CallInUIThreadDeferred([this] { ForceRelease(); });
    if (foreground) was_foreground_.store(true);
    UpdateCapture(active && gameplay);

    using namespace rex::input;
    uint16_t buttons = 0;
    for (auto& [keys, bit] : buttons_)
      if (AnyDown(keys, active)) buttons |= bit;
    // Weapon wheel (weapon_wheel.cpp): open while the wheel is shown, LMB picks the highlighted
    // weapon, RMB or Escape closes it; firing and aiming wait until that button is released.
    const bool lmb = KeyDown(VK_LBUTTON, active), rmb = KeyDown(VK_RBUTTON, active);
    const bool esc = AnyDown(pause_, active);
    bool wheel_open = jth::wheel::IsOpen();
    if (wheel_open && !gameplay) {
      jth::wheel::Cancel();
      wheel_open = false;
    }
    if (wheel_open) {
      if (lmb && !lmb_was_) {
        jth::wheel::Confirm();
        hold_fire_ = true;
      } else if ((rmb && !rmb_was_) || (esc && !esc_was_)) {
        jth::wheel::Cancel();
        hold_aim_ = rmb;
        hold_pause_ = esc;
      }
    }
    if (!lmb) hold_fire_ = false;
    if (!rmb) hold_aim_ = false;
    if (!esc) hold_pause_ = false;
    lmb_was_ = lmb;
    rmb_was_ = rmb;
    esc_was_ = esc;

    // Change weapon: in gameplay with the wheel enabled, a tap changes weapon when the key is
    // released (one Y press) and holding it opens the wheel; releasing then picks the weapon.
    // Elsewhere (menus) the key is simply Y.
    const bool cw = AnyDown(change_weapon_, active);
    if (gameplay && jth::wheel::Enabled()) {
      const int64_t now = NowNs();
      if (cw && !cw_was_) {
        cw_since_ = now;
        jth::wheel::BeginHold();
      }
      if (cw && !cw_wheel_ && now - cw_since_ >= kWheelHoldNs) {
        jth::wheel::Open();
        cw_wheel_ = true;
      }
      if (!cw && cw_was_) {
        if (cw_wheel_) {
          jth::wheel::Confirm();  // no-op if a click already picked or cancelled
        } else {
          jth::wheel::EndHold();
          g_wheel_steps.fetch_add(1);  // tap: next weapon, as Y
        }
        cw_wheel_ = false;
      }
      // Number keys: the n-th weapon carried.
      for (int d = 0; d < 9; ++d) {
        const bool down = digit_free_[d] && KeyDown('1' + d, active);
        if (down && !digit_was_[d]) jth::wheel::SelectSlot(d);
        digit_was_[d] = down;
      }
    } else {
      if (cw) buttons |= X_INPUT_GAMEPAD_Y;
      if (cw_was_ && !cw_wheel_) jth::wheel::EndHold();
      cw_wheel_ = false;
    }
    cw_was_ = cw;
    wheel_open = jth::wheel::IsOpen();

    // Escape pauses during play (Start; B would crouch). Elsewhere it sends Start + B: Start
    // skips cutscenes and passes the title screen, B backs out of menus.
    if (esc && !wheel_open && !hold_pause_)
      buttons |= gameplay ? X_INPUT_GAMEPAD_START : (X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_B);

    // Mouse wheel: one weapon change per notch, held for a few polls so the game sees it.
    if (wheel_hold_ > 0) {
      buttons |= X_INPUT_GAMEPAD_Y;
      --wheel_hold_;
    } else if (wheel_gap_ > 0) {
      --wheel_gap_;
    } else if (g_wheel_steps.load() > 0) {
      g_wheel_steps.fetch_sub(1);
      wheel_hold_ = 3;
      wheel_gap_ = 3;
      buttons |= X_INPUT_GAMEPAD_Y;
    }

    int lx = 0, ly = 0;
    if (AnyDown(forward_, active)) ly += 32767;
    if (AnyDown(back_, active)) ly -= 32767;
    if (AnyDown(right_, active)) lx += 32767;
    if (AnyDown(left_, active)) lx -= 32767;
    // Balance steering: strafe keys and the left/right arrow keys (D-pad).
    {
      const bool steer_right = lx > 0 || (buttons & X_INPUT_GAMEPAD_DPAD_RIGHT);
      const bool steer_left = lx < 0 || (buttons & X_INPUT_GAMEPAD_DPAD_LEFT);
      g_balance_keys.store(int(steer_right) - int(steer_left), std::memory_order_relaxed);
    }
    const bool balancing =
        NowNs() - g_last_balance_ns.load(std::memory_order_relaxed) < kGameplayTimeoutNs;

    if (lx && ly) {  // keep diagonal speed equal to straight speed
      lx = lx * 23170 / 32767;
      ly = ly * 23170 / 32767;
    }

    // While the mouse is moving in gameplay, also report a right-stick deflection in the same
    // direction. Scripted events like the "look around" tutorial step only watch the stick.
    // (Not while balancing: the mouse steers the balance directly there.)
    int rx = 0, ry = 0;
    if (gameplay && !balancing &&
        NowNs() - g_last_motion_ns.load(std::memory_order_relaxed) < kMotionWindowNs) {
      constexpr int kDeflect = 20000;
      switch (g_last_motion_dir.load(std::memory_order_relaxed)) {
        case 1: rx = kDeflect; break;
        case 2: rx = -kDeflect; break;
        case 3: ry = -kDeflect; break;
        case 4: ry = kDeflect; break;
      }
    }
    const bool synthetic = rx != 0 || ry != 0;
    if (g_synthetic_look.exchange(synthetic, std::memory_order_relaxed) != synthetic)
      REXLOG_DEBUG("PC controls: stand-in right stick {} (rx={}, ry={})", synthetic ? "on" : "off",
                   rx, ry);

    std::memset(out, 0, sizeof(*out));
    out->gamepad.buttons = buttons;
    g_kb_buttons.store(buttons, std::memory_order_relaxed);
    // In menus the mouse buttons click (menu_mouse.cpp) instead of firing/aiming.
    const bool menu = jth::menu_mouse::InMenu();
    out->gamepad.left_trigger =
        !wheel_open && !hold_aim_ && AnyDownExceptMouse(aim_, active, menu) ? 255 : 0;
    out->gamepad.right_trigger =
        !wheel_open && !hold_fire_ && AnyDownExceptMouse(fire_, active, menu) ? 255 : 0;
    out->gamepad.thumb_lx = static_cast<int16_t>(lx);
    out->gamepad.thumb_ly = static_cast<int16_t>(ly);
    out->gamepad.thumb_rx = static_cast<int16_t>(rx);
    out->gamepad.thumb_ry = static_cast<int16_t>(ry);

    uint64_t sig = (uint64_t(buttons) << 48) ^ (uint64_t(uint16_t(lx)) << 32) ^
                   (uint64_t(uint16_t(ly)) << 16) ^ (uint64_t(out->gamepad.left_trigger) << 8) ^
                   out->gamepad.right_trigger ^ (uint64_t(uint16_t(rx)) << 24) ^
                   (uint64_t(uint16_t(ry)) << 40);
    if (sig != last_sig_) {
      last_sig_ = sig;
      ++packet_;
    }
    out->packet_number = packet_;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (id != kPcDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kPcDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kPcDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  void OnWindowAvailable(rex::ui::Window* window) override {
    window_ = window;
    g_window.store(window);
    if (window_) {
      window_->AddInputListener(this, 0);
      window_->AddListener(this);
    }
  }

  // ----- window events (UI thread) -----
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!captured_) {
      // Free pointer (menus, cutscenes): drives the menu cursor.
      if (window_)
        jth::menu_mouse::OnPointerMove(e.x(), e.y(), int(window_->GetActualPhysicalWidth()),
                                       int(window_->GetActualPhysicalHeight()));
      return;
    }
    AddMouseDelta(e.dx(), e.dy());
  }

  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (captured_ || !window_ || !window_->HasFocus() || jth::opts::PcOptionsOpen()) return;
    if (e.button() == rex::ui::MouseEvent::Button::kLeft) jth::menu_mouse::OnClick(false);
    if (e.button() == rex::ui::MouseEvent::Button::kRight) jth::menu_mouse::OnClick(true);
  }

  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (e.scroll_y() == 0) return;
    if (captured_)
      g_wheel_steps.fetch_add(1);  // change weapon
    else if (!jth::opts::PcOptionsOpen())
      jth::menu_mouse::OnWheel(e.scroll_y() > 0 ? 1 : -1);
  }

  void OnLostFocus(rex::ui::UISetupEvent&) override {
    capture_wanted_.store(false);
    ForceRelease();
  }

  void OnGotFocus(rex::ui::UISetupEvent&) override { ApplyCapture(); }

  void OnClosing(rex::ui::UIEvent&) override {
    if (window_) {
      window_->RemoveInputListener(this);
      window_->RemoveListener(this);
      window_ = nullptr;
    }
  }

 private:
  // Called from the guest input thread; the actual capture change runs on the UI thread.
  void UpdateCapture(bool want) {
    if (capture_wanted_.exchange(want) == want || !window_) return;
    window_->app_context().CallInUIThreadDeferred([this] { ApplyCapture(); });
  }

  void ApplyCapture() {
    if (!window_) return;
    bool want =
        capture_wanted_.load() && window_->HasFocus() && InputBlocker() == Blocker::kNone;
    if (want == captured_) {
      if (!want) window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      return;
    }
    captured_ = want;
    if (want) {
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      // Order matters (SDL 3): relative mode first, capture second. Relative mode marks the pointer
      // as over the window and delivers motion only while that holds; enabling it after a capture
      // made SDL drop the capture again, and dropping a capture clears "pointer over the window"
      // whenever Windows hasn't re-tracked the pointer since it last left - exactly the case after
      // it was over the Start menu or the Game Bar. Mouse look then got no motion at all and the
      // arrow stayed visible in the middle of the screen until the next focus change. In relative
      // mode SDL doesn't capture at all, so the request below only keeps the SDK's count.
      // The warp (through SDL, no motion delta) also hands the OS pointer back to the window.
      int32_t center_x = 0, center_y = 0;
      window_->WarpMouseToCenter(center_x, center_y);
      if (!window_->SetRelativeMouseMode(true))
        REXLOG_WARN("PC controls: relative mouse mode unavailable");
      window_->CaptureMouse();
      SetOsCursor(false);
      REXLOG_INFO("PC controls: mouse captured for gameplay");
    } else {
      window_->ReleaseMouse();  // before leaving relative mode, see above
      window_->SetRelativeMouseMode(false);
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      SetOsCursor(true);
      REXLOG_INFO("PC controls: mouse released (menu or cutscene)");
    }
    std::lock_guard lock(g_mouse_mutex);
    g_mouse_dx = g_mouse_dy = 0;  // no jump when capture starts or ends
  }

  // UI thread: free the mouse unconditionally (focus lost, e.g. the Windows key) - relative mode
  // off, capture released, cursor shown, no cursor clip left behind. Done through both SDL and
  // Windows directly, so nothing depends on SDL's idea of where the pointer is.
  void ForceRelease() {
    if (!window_) return;
    if (captured_) window_->ReleaseMouse();  // before leaving relative mode, see ApplyCapture
    window_->SetRelativeMouseMode(false);
    window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    ReleaseCapture();
    ClipCursor(nullptr);
    SetOsCursor(true);
    // Cursor display counter back to >= 0 (it's per thread; nothing should have lowered it, but a
    // negative count would hide the pointer over our window regardless of the cursor shape).
    int count = ShowCursor(TRUE);
    while (count < 0) count = ShowCursor(TRUE);
    if (count > 0) ShowCursor(FALSE);
    captured_ = false;
    std::lock_guard lock(g_mouse_mutex);
    g_mouse_dx = g_mouse_dy = 0;
  }

  rex::ui::Window* window_ = nullptr;
  std::atomic<bool> was_foreground_{true};
  Blocker last_blocker_ = Blocker::kNone;  // input thread only (for the log)
  std::atomic<bool> capture_wanted_{false};
  bool captured_ = false;  // UI thread only

  std::vector<int> forward_, back_, left_, right_, fire_, aim_, pause_, change_weapon_;
  // Weapon wheel / change weapon (input thread).
  static constexpr int64_t kWheelHoldNs = 220'000'000;  // longer than this: a hold, not a tap
  bool cw_was_ = false, cw_wheel_ = false;
  int64_t cw_since_ = 0;
  bool lmb_was_ = false, rmb_was_ = false, esc_was_ = false;
  bool hold_fire_ = false, hold_aim_ = false, hold_pause_ = false;
  std::array<bool, 9> digit_free_{}, digit_was_{};
  std::vector<std::pair<std::vector<int>, uint16_t>> buttons_;
  int wheel_hold_ = 0, wheel_gap_ = 0;
  uint64_t last_sig_ = ~0ull;
  uint32_t packet_ = 0x20000000;
};

}  // namespace

int KeyFromName(std::string_view name) {
  if (name.size() == 1) {
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
  }
  if ((name[0] == 'F' || name[0] == 'f') && name.size() <= 3 && name.size() >= 2) {
    int n = std::atoi(std::string(name.substr(1)).c_str());
    if (n >= 1 && n <= 12) return 0x70 + n - 1;
  }
  if (name.size() == 7 && IEquals(name.substr(0, 6), "Numpad") && std::isdigit(name[6]))
    return 0x60 + (name[6] - '0');
  for (auto& k : kKeyNames)
    if (IEquals(name, k.name)) return k.vk;
  return 0;
}

std::unique_ptr<rex::input::InputDriver> CreatePcInputDriver() {
  if (!REXCVAR_GET(pc_controls)) return nullptr;
  auto driver = std::make_unique<PcInputDriver>();
  driver->Setup();
  return driver;
}

void AddMouseDelta(float dx, float dy) {
  if (dx == 0 && dy == 0) return;
  if (jth::wheel::IsOpen()) {  // the mouse points at a weapon instead of turning the camera
    jth::wheel::AddCursor(dx, dy);
    return;
  }
  // Change-weapon key held, wheel not open yet: aim the pointer too (a quick press-and-flick
  // selects), but keep turning the camera in case it's a tap.
  if (jth::wheel::Tracking()) jth::wheel::AddCursor(dx, dy);
  {
    std::lock_guard lock(g_mouse_mutex);
    g_mouse_dx += dx;
    g_mouse_dy += dy;
  }
  NoteMotion(dx, dy);
}

namespace {
void NoteMotion(float dx, float dy) {
  int dir = std::fabs(dx) >= std::fabs(dy) ? (dx > 0 ? 1 : 2) : (dy > 0 ? 3 : 4);
  g_last_motion_dir.store(dir, std::memory_order_relaxed);
  g_last_motion_ns.store(NowNs(), std::memory_order_relaxed);
}
}  // namespace

void SetTestKey(int vk, bool down) {
  g_test_keys[vk & 0xFF].store(down);
}

void RunOnUiThread(std::function<void()> fn) {
  if (rex::ui::Window* w = g_window.load()) w->app_context().CallInUIThreadDeferred(std::move(fn));
}

uint16_t KeyboardButtons() { return g_kb_buttons.load(std::memory_order_relaxed); }

bool GameplayLookActive() {
  return NowNs() - g_last_look_ns.load(std::memory_order_relaxed) <
         std::chrono::duration_cast<std::chrono::nanoseconds>(kGameplayTimeout).count();
}

void OnBalanceFrame(uint32_t controller) {
  const int64_t now = NowNs();
  // The normal look update (JthMouseLook) doesn't run while balancing: keep the gameplay state
  // (mouse capture, keys) and the cheats going from here instead.
  g_last_look_ns.store(now, std::memory_order_relaxed);
  const int64_t before = g_last_balance_ns.exchange(now, std::memory_order_relaxed);
  jth::cheats::ApplyCheats(controller);
  // Testing: finish the automatic walk onto the beam at once (+840 walking; the walk moves the
  // controller's position, +88..+96, to the target StartBalance stored at +792..+800).
  if (REXCVAR_GET(dev_balance_skip_walk)) {
    uint8_t* base = rex::system::kernel_memory()->virtual_membase();
    if (base[controller + 840]) std::memcpy(base + controller + 88, base + controller + 792, 12);
  }
  static int64_t last_log = 0;
  if (now - before > kGameplayTimeoutNs || now - last_log > 1'000'000'000) {
    last_log = now;
    const uint8_t* base = rex::system::kernel_memory()->virtual_membase();
    const uint8_t* p = base + controller + 824;
    const uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    float f824;
    std::memcpy(&f824, &bits, 4);
    REXLOG_DEBUG("Balancing: walking onto the beam {}, balancing {}, +824 {:.3f}",
                 base[controller + 840], base[controller + 841], f824);
  }
  float dx;
  {
    std::lock_guard lock(g_mouse_mutex);
    dx = g_mouse_dx;
    g_mouse_dx = g_mouse_dy = 0;  // not left for the camera to jump by after balancing
  }
  g_balance_mouse_dx.store(dx, std::memory_order_relaxed);
}

double BalanceSteer() {
  // The game's axis units: +-1000 at full stick. Keys ramp up to a full push over 0.25 s so a tap
  // is a small correction (a stick can be pushed partway; a key can't). Holding D at full tips
  // the player off a log within ~2 s, as holding the stick does. Mouse: a 25-count flick in one
  // frame is a full push at sensitivity 1 (scaled with mouse_sensitivity, like look).
  // Game thread only (one call per frame from the balance update).
  static int last_dir = 0;
  static int64_t since = 0;
  const int dir = g_balance_keys.load(std::memory_order_relaxed);
  const int64_t now = NowNs();
  if (dir != last_dir) {
    last_dir = dir;
    since = now;
  }
  constexpr double kRampNs = 250'000'000.0;
  const double keys = dir * 1000.0 * std::min(1.0, double(now - since) / kRampNs);
  const double mouse = g_balance_mouse_dx.exchange(0, std::memory_order_relaxed) * 40.0 *
                       REXCVAR_GET(mouse_sensitivity);
  return std::clamp(keys + mouse, -1000.0, 1000.0);
}

}  // namespace jth::pc

// ---------------------------------------------------------------------------------------------
// Camera hook (guest thread)
// ---------------------------------------------------------------------------------------------
namespace {
float LoadBeFloat(const uint8_t* p) {
  uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreBeFloat(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
}  // namespace

void JthMouseLook(PPCRegister& r1, PPCRegister& r31) {
  using namespace jth::pc;
  g_last_look_ns.store(NowNs(), std::memory_order_relaxed);

  // Cheats are applied here because this runs once per frame on the player-logic thread, with
  // r31 = the player's csControllerActorPlayer.
  jth::cheats::ApplyCheats(r31.u32);
  jth::wheel::OnPlayerFrame(r31.u32);

  float dx, dy;
  {
    std::lock_guard lock(g_mouse_mutex);
    dx = g_mouse_dx;
    dy = g_mouse_dy;
    g_mouse_dx = g_mouse_dy = 0;
  }
  const bool synthetic = g_synthetic_look.load(std::memory_order_relaxed);
  if (dx == 0 && dy == 0 && !synthetic) return;

  uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  const float rad_per_count =
      kDegreesPerCount * float(REXCVAR_GET(mouse_sensitivity)) * 3.14159265f / 180.0f;
  const bool game_invert = base[kGameInvertLookAddr] != 0;

  uint8_t* yaw = base + r1.u32 + 80;    // + = turn right
  uint8_t* pitch = base + r1.u32 + 84;  // + = look down
  float yaw_delta = synthetic ? 0.0f : LoadBeFloat(yaw);  // drop the stand-in stick's turn
  float pitch_delta = synthetic ? 0.0f : LoadBeFloat(pitch);
  StoreBeFloat(yaw, yaw_delta + dx * rad_per_count);
  StoreBeFloat(pitch, pitch_delta + (game_invert ? -dy : dy) * rad_per_count);
}
