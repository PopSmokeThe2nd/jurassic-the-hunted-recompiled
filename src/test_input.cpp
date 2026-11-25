#include "test_input.h"

#include "dev_lua.h"

#include "menu_mouse.h"

#include "pc_input.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(test_input_file, "", "Debug",
                      "Path to a scripted-input command file (testing only; see src/test_input.h)");

namespace jth {
namespace {

using Clock = std::chrono::steady_clock;
using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

constexpr DeviceId kScriptedDevice = static_cast<DeviceId>(0x4A544849);  // "JTHI"

struct Step {
  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint8_t lt = 0, rt = 0;
  int ms = 0;
  std::vector<int> vks;     // PC keys held for the step (key command, e.g. lshift+w)
  // key ... mouse <dx> <dy> <after ms>: mouse movement while the keys are held (weapon wheel)
  float hold_mdx = 0, hold_mdy = 0;
  int hold_mouse_ms = -1;
  float mdx = 0, mdy = 0;   // mouse movement applied when the step starts (mouse command)
  float px = -1, py = -1;   // menu pointer position in 1280x720 GUI units (pointer command)
  int click = 0;            // 1 = left, 2 = right (click command)
  int wheel = 0;            // +1 up / -1 down (wheel command)
  std::string lua;          // Lua code run when the step starts (lua command)
  std::string set_name, set_value;  // setting changed when the step starts (setq command)
  std::string label;
};

uint16_t ParseButtons(const std::string& spec, bool& ok) {
  static const std::pair<const char*, uint16_t> kNames[] = {
      {"a", rex::input::X_INPUT_GAMEPAD_A},
      {"b", rex::input::X_INPUT_GAMEPAD_B},
      {"x", rex::input::X_INPUT_GAMEPAD_X},
      {"y", rex::input::X_INPUT_GAMEPAD_Y},
      {"start", rex::input::X_INPUT_GAMEPAD_START},
      {"back", rex::input::X_INPUT_GAMEPAD_BACK},
      {"lb", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"rb", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"ls", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},
      {"rs", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"up", rex::input::X_INPUT_GAMEPAD_DPAD_UP},
      {"down", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
      {"left", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
      {"right", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"guide", rex::input::X_INPUT_GAMEPAD_GUIDE},
  };
  uint16_t mask = 0;
  ok = true;
  std::stringstream ss(spec);
  std::string part;
  while (std::getline(ss, part, '+')) {
    bool found = false;
    for (auto& [name, bit] : kNames) {
      if (part == name) {
        mask |= bit;
        found = true;
      }
    }
    ok = ok && found;
  }
  return mask;
}

int16_t Axis(double v) {
  v = std::clamp(v, -1.0, 1.0);
  return static_cast<int16_t>(v * 32767.0);
}

class ScriptedInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptedInputDriver(std::filesystem::path file)
      : InputDriver(nullptr, 0), file_(std::move(file)) {}

  X_STATUS Setup() override {
    REXLOG_INFO("Scripted input: watching {}", file_.string());
    return X_STATUS_SUCCESS;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kScriptedDevice;
    info.name = "Scripted test input";
    info.synthetic = true;  // synthetic devices merge into player 1
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kScriptedDevice)
      return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(mutex_);
    PollFile();
    Advance();
    std::memset(out, 0, sizeof(*out));
    if (active_) {
      out->gamepad.buttons = current_.buttons;
      out->gamepad.left_trigger = current_.lt;
      out->gamepad.right_trigger = current_.rt;
      out->gamepad.thumb_lx = current_.lx;
      out->gamepad.thumb_ly = current_.ly;
      out->gamepad.thumb_rx = current_.rx;
      out->gamepad.thumb_ry = current_.ry;
    }
    out->packet_number = packet_;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (id != kScriptedDevice)
      return X_ERROR_DEVICE_NOT_CONNECTED;
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
    return id == kScriptedDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kScriptedDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  void PollFile() {
    auto now = Clock::now();
    if (now - last_poll_ < std::chrono::milliseconds(50))
      return;
    last_poll_ = now;

    std::error_code ec;
    auto size = std::filesystem::file_size(file_, ec);
    if (ec)
      return;
    if (size < offset_)
      offset_ = 0;  // file was truncated/rewritten: start over
    if (size == offset_)
      return;

    std::ifstream in(file_, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset_));
    std::string chunk(size - offset_, '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    chunk.resize(static_cast<size_t>(in.gcount()));
    // Only consume complete lines.
    auto last_nl = chunk.rfind('\n');
    if (last_nl == std::string::npos)
      return;
    offset_ += last_nl + 1;
    std::stringstream lines(chunk.substr(0, last_nl + 1));
    std::string line;
    while (std::getline(lines, line))
      ParseLine(line);
  }

  void ParseLine(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    // lua <code>: the rest of the line exactly as written (case matters, '#' is Lua's length
    // operator), run in sequence when the step is reached.
    {
      const size_t start = line.find_first_not_of(" \t");
      if (start != std::string::npos && line.size() > start + 4 &&
          _strnicmp(line.c_str() + start, "lua ", 4) == 0) {
        Step s;
        s.lua = line.substr(start + 4);
        s.lua.erase(0, s.lua.find_first_not_of(" \t"));
        s.label = "lua " + s.lua;
        if (!s.lua.empty()) queue_.push_back(std::move(s));
        return;
      }
    }
    if (auto hash = line.find('#'); hash != std::string::npos)
      line.resize(hash);
    std::transform(line.begin(), line.end(), line.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::stringstream ss(line);
    std::string cmd;
    if (!(ss >> cmd))
      return;

    Step s;
    s.label = line;
    bool ok = true;
    if (cmd == "wait") {
      ok = static_cast<bool>(ss >> s.ms);
    } else if (cmd == "tap" || cmd == "hold") {
      std::string spec;
      ss >> spec;
      s.buttons = ParseButtons(spec, ok);
      int ms = 0;
      s.ms = (ss >> ms) ? ms : (cmd == "tap" ? 120 : 0);
      ok = ok && s.ms > 0;
      if (ok && cmd == "tap") {
        queue_.push_back(s);
        s = Step{};
        s.ms = 150;
        s.label = "(release)";
      }
    } else if (cmd == "stick") {
      std::string which;
      double x = 0, y = 0;
      ok = static_cast<bool>(ss >> which >> x >> y >> s.ms) && (which == "l" || which == "r");
      (which == "l" ? s.lx : s.rx) = Axis(x);
      (which == "l" ? s.ly : s.ry) = Axis(y);
      // Optional buttons held at the same time, e.g. "stick l 1 0 800 y"; "lt" / "rt" pull a
      // trigger fully ("stick l 0 1 800 rt+ls").
      std::string spec;
      if (ok && ss >> spec) {
        std::string rest, part;
        std::stringstream parts(spec);
        while (std::getline(parts, part, '+')) {
          if (part == "lt") s.lt = 255;
          else if (part == "rt") s.rt = 255;
          else rest += (rest.empty() ? "" : "+") + part;
        }
        if (!rest.empty()) s.buttons = ParseButtons(rest, ok);
      }
    } else if (cmd == "key") {
      std::string names, name;
      ss >> names;
      std::stringstream parts(names);
      while (std::getline(parts, name, '+')) {
        int vk = jth::pc::KeyFromName(name);
        if (!vk) ok = false;
        s.vks.push_back(vk);
      }
      int ms = 0;
      s.ms = (ss >> ms) ? ms : 120;
      std::string word;
      if (ss >> word && word == "mouse")
        ok = ok && static_cast<bool>(ss >> s.hold_mdx >> s.hold_mdy >> s.hold_mouse_ms);
      ok = ok && !s.vks.empty() && s.ms > 0;
      if (ok) {
        queue_.push_back(s);
        s = Step{};
        s.ms = 150;
        s.label = "(release)";
      }
    } else if (cmd == "set") {  // change a setting immediately, e.g. "set fov 110"
      std::string name, value;
      ok = static_cast<bool>(ss >> name >> value);
      if (ok) {
        bool applied = rex::cvar::SetFlagByName(name, value);
        REXLOG_INFO("Scripted input: set {} = {} ({})", name, value, applied ? "ok" : "rejected");
      }
      return;
    } else if (cmd == "setq") {  // change a setting in sequence, when this step is reached
      ok = static_cast<bool>(ss >> s.set_name >> s.set_value);
      s.ms = 1;
    } else if (cmd == "mouse") {
      ok = static_cast<bool>(ss >> s.mdx >> s.mdy);
      int ms = 0;
      s.ms = (ss >> ms) ? ms : 60;
    } else if (cmd == "pointer") {  // pointer <x> <y> [ms]: menu cursor, 1280x720 GUI units
      ok = static_cast<bool>(ss >> s.px >> s.py);
      int ms = 0;
      s.ms = (ss >> ms) ? ms : 60;
    } else if (cmd == "click") {  // click [left|right] [ms]
      std::string which = "left";
      ss >> which;
      s.click = which == "right" ? 2 : 1;
      int ms = 0;
      s.ms = (ss >> ms) ? ms : 60;
    } else if (cmd == "wheel") {  // wheel <up|down> [ms]
      std::string which;
      ok = static_cast<bool>(ss >> which) && (which == "up" || which == "down");
      s.wheel = which == "up" ? 1 : -1;
      int ms = 0;
      s.ms = (ss >> ms) ? ms : 60;
    } else if (cmd == "trigger") {
      std::string which;
      double v = 0;
      ok = static_cast<bool>(ss >> which >> v >> s.ms) && (which == "l" || which == "r");
      (which == "l" ? s.lt : s.rt) = static_cast<uint8_t>(std::clamp(v, 0.0, 1.0) * 255.0);
    } else {
      ok = false;
    }
    if (!ok) {
      REXLOG_WARN("Scripted input: can't parse '{}'", line);
      return;
    }
    queue_.push_back(std::move(s));
  }

  void Advance() {
    auto now = Clock::now();
    while (true) {
      if (active_ && current_.hold_mouse_ms >= 0 &&
          now - started_ >= std::chrono::milliseconds(current_.hold_mouse_ms)) {
        jth::pc::AddMouseDelta(current_.hold_mdx, current_.hold_mdy);
        current_.hold_mouse_ms = -1;
      }
      if (active_ && now - started_ < std::chrono::milliseconds(current_.ms))
        return;
      if (active_)
        for (int vk : current_.vks) jth::pc::SetTestKey(vk, false);
      if (queue_.empty()) {
        if (active_) {
          active_ = false;
          ++packet_;
        }
        return;
      }
      current_ = std::move(queue_.front());
      queue_.pop_front();
      started_ = now;
      active_ = true;
      ++packet_;
      for (int vk : current_.vks) jth::pc::SetTestKey(vk, true);
      if (current_.mdx != 0 || current_.mdy != 0)
        jth::pc::AddMouseDelta(current_.mdx, current_.mdy);
      if (current_.px >= 0)
        jth::menu_mouse::OnPointerMove(int(current_.px), int(current_.py), 1280, 720);
      if (current_.click) jth::menu_mouse::OnClick(current_.click == 2);
      if (current_.wheel) jth::menu_mouse::OnWheel(current_.wheel);
      if (!current_.lua.empty()) jth::devlua::Queue(current_.lua);
      if (!current_.set_name.empty()) {
        const bool applied = rex::cvar::SetFlagByName(current_.set_name, current_.set_value);
        REXLOG_INFO("Scripted input: setq {} = {} ({})", current_.set_name, current_.set_value,
                    applied ? "ok" : "rejected");
      }
      if (current_.label != "(release)")
        REXLOG_INFO("Scripted input: {}", current_.label);
    }
  }

  std::filesystem::path file_;
  std::mutex mutex_;
  uintmax_t offset_ = 0;
  Clock::time_point last_poll_{};
  std::deque<Step> queue_;
  Step current_;
  bool active_ = false;
  Clock::time_point started_{};
  // Start high so the merged packet number (max over devices) changes when we do.
  uint32_t packet_ = 0x40000000;
};

}  // namespace

std::unique_ptr<rex::input::InputDriver> CreateScriptedInputDriver() {
  std::string file = REXCVAR_GET(test_input_file);
  if (file.empty())
    return nullptr;
  auto driver = std::make_unique<ScriptedInputDriver>(file);
  driver->Setup();
  return driver;
}

}  // namespace jth
