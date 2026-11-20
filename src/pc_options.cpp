#include "pc_options.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/input/touchpad.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "framerate.h"
#include "graphics_settings.h"
#include "pc_input.h"
#include "settings_file.h"

REXCVAR_DEFINE_INT32(fov, 86, "Graphics",
                     "Horizontal field of view in degrees at 16:9 (game default 86)")
    .range(60, 120)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(weapon_fov_fixed, true, "Graphics",
                    "Keep the first-person weapon at its original size when the FOV is changed")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pc_options_toggle_test, 0, "Debug",
                     "Each change toggles the PC Options panel like F2 (automated tests)");

REXCVAR_DECLARE(double, mouse_sensitivity);  // defined in pc_input.cpp
REXCVAR_DECLARE(bool, cheat_god_mode);       // defined in cheats.cpp
REXCVAR_DECLARE(bool, cheat_infinite_ammo);
REXCVAR_DECLARE(bool, cheat_infinite_adrenaline);
REXCVAR_DECLARE(bool, cheat_infinite_grenades);  // defined in cheats.cpp
REXCVAR_DECLARE(bool, cheat_one_hit_kill);
REXCVAR_DECLARE(double, cheat_super_speed);

namespace {
// Super speed choices (cheat_super_speed, 1-4).
constexpr double kSpeeds[] = {1.0, 1.5, 2.0, 2.5, 3.0, 4.0};
const char* kSpeedLabels[] = {"Off (normal)", "1.5x", "2x", "2.5x", "3x", "4x"};
constexpr int kSpeedCount = 6;
}  // namespace
REXCVAR_DECLARE(std::string, upscaler);     // defined in graphics_settings.cpp
REXCVAR_DECLARE(double, upscaler_sharpness);
REXCVAR_DECLARE(std::string, antialiasing);
REXCVAR_DECLARE(int32_t, framerate);  // defined in framerate.cpp
REXCVAR_DECLARE(bool, show_fps);
REXCVAR_DECLARE(double, hdr_max_brightening);  // defined in graphics_settings.cpp
REXCVAR_DECLARE(bool, weapon_wheel);            // defined in weapon_wheel.cpp
REXCVAR_DECLARE(bool, toggle_sprint);           // defined in pad_filter.cpp
REXCVAR_DECLARE(bool, hide_hud);                // defined in hide_hud.cpp

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kGameDefaultFov = 86;
// Widest horizontal FOV the engine renders without gaps at the screen edges (see JthCameraFov).
constexpr double kMaxFovRad = 126.0 * kPi / 180.0;
// Guest globals: game/world object (its +108 is the active player camera) and the default
// camera FOV (70 degrees horizontal at 4:3, which the game widens to ~86 degrees at 16:9).
constexpr uint32_t kWorldPtrAddr = 0x8274DC68;
constexpr uint32_t kDefaultFovAddr = 0x827193D0;

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
float LoadBeFloat(const uint8_t* p) {
  uint32_t v = LoadBe32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

std::atomic<bool> g_panel_open{false};
rex::input::InputSystem* g_input = nullptr;  // set once at startup
using rex::X_RESULT;                         // X_ERROR_SUCCESS

// The controller as ImGui gamepad navigation: D-pad / left stick move, A (cross) activates, B
// (circle) backs out, LB / RB tweak faster.
void FeedGamepad(ImGuiIO& io, const rex::input::X_INPUT_GAMEPAD& g) {
  using namespace rex::input;
  auto key = [&](ImGuiKey k, uint16_t bit) { io.AddKeyEvent(k, (g.buttons & bit) != 0); };
  key(ImGuiKey_GamepadFaceDown, X_INPUT_GAMEPAD_A);
  key(ImGuiKey_GamepadFaceRight, X_INPUT_GAMEPAD_B);
  key(ImGuiKey_GamepadFaceLeft, X_INPUT_GAMEPAD_X);
  key(ImGuiKey_GamepadFaceUp, X_INPUT_GAMEPAD_Y);
  key(ImGuiKey_GamepadDpadUp, X_INPUT_GAMEPAD_DPAD_UP);
  key(ImGuiKey_GamepadDpadDown, X_INPUT_GAMEPAD_DPAD_DOWN);
  key(ImGuiKey_GamepadDpadLeft, X_INPUT_GAMEPAD_DPAD_LEFT);
  key(ImGuiKey_GamepadDpadRight, X_INPUT_GAMEPAD_DPAD_RIGHT);
  key(ImGuiKey_GamepadL1, X_INPUT_GAMEPAD_LEFT_SHOULDER);
  key(ImGuiKey_GamepadR1, X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  key(ImGuiKey_GamepadStart, X_INPUT_GAMEPAD_START);
  key(ImGuiKey_GamepadBack, X_INPUT_GAMEPAD_BACK);
  auto stick = [&](ImGuiKey neg, ImGuiKey pos, int16_t raw) {
    constexpr float kDead = 0.3f;
    const float v = std::clamp(raw / 32767.0f, -1.0f, 1.0f);
    const float a = std::max(0.0f, (std::fabs(v) - kDead) / (1 - kDead));
    io.AddKeyAnalogEvent(pos, v > kDead, v > 0 ? a : 0.0f);
    io.AddKeyAnalogEvent(neg, v < -kDead, v < 0 ? a : 0.0f);
  };
  stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, g.thumb_lx);
  stick(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, g.thumb_ly);  // XInput: up is +
}

// The dialog exists only while the panel is open: while any ImGui dialog is registered, the SDK
// repaints the window continuously (hundreds of presents per second even at a 30 fps cap).
class PcOptionsDialog final : public rex::ui::ImGuiDialog {
 public:
  PcOptionsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
      : ImGuiDialog(drawer), config_path_(std::move(config_path)) {
    g_panel_open.store(true);
    if (g_input) {
      g_input->AddUIInputBlocker();  // the game reads a neutral pad while the panel is open
      blocking_ = true;
    }
  }
  ~PcOptionsDialog() override;

  // Saves, then closes; deleted by the SDK right after the next Draw (see ImGuiDialog::Draw).
  void RequestClose() {
    Save();
    Close();
  }

 protected:
  void OnClose() override {
    g_panel_open.store(false);
    if (dirty_) Save();  // e.g. a slider still being dragged
  }

  void OnDraw(ImGuiIO& io) override {
    if (g_input) {
      rex::input::X_INPUT_STATE pad{};
      if (g_input->GetStateForUI(0, &pad) == X_ERROR_SUCCESS) FeedGamepad(io, pad.gamepad);
      io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
      io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("PC Options##jth", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      // The SDK's ImGui font is sized for 720p; scale the panel with the display.
      ImGui::SetWindowFontScale(std::max(1.0f, io.DisplaySize.y / 800.0f));
      const float slider_w = ImGui::GetFontSize() * 22.0f;
      int fov = REXCVAR_GET(fov);
      ImGui::TextUnformatted("Field of view (horizontal degrees)");
      ImGui::SetNextItemWidth(slider_w);
      if (ImGui::SliderInt("##fov", &fov, 60, 120, "%d")) {
        rex::cvar::SetFlagByName("fov", std::to_string(fov));
        dirty_ = true;
      }
      SaveWhenEditEnds();
      ImGui::SameLine();
      if (ImGui::Button("Default##fov")) {
        rex::cvar::SetFlagByName("fov", std::to_string(kGameDefaultFov));
        Save();
      }
      ImGui::TextDisabled("Game default: %d. Cutscenes keep their original framing.",
                          kGameDefaultFov);
      bool fixed = REXCVAR_GET(weapon_fov_fixed);
      if (ImGui::Checkbox("Keep weapon size fixed", &fixed)) {
        rex::cvar::SetFlagByName("weapon_fov_fixed", fixed ? "true" : "false");
        Save();
      }

      ImGui::Spacing();
      float sens = float(REXCVAR_GET(mouse_sensitivity));
      ImGui::TextUnformatted("Mouse sensitivity");
      ImGui::SetNextItemWidth(slider_w);
      if (ImGui::SliderFloat("##sens", &sens, 0.1f, 5.0f, "%.2f",
                             ImGuiSliderFlags_Logarithmic)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", sens);
        rex::cvar::SetFlagByName("mouse_sensitivity", buf);
        dirty_ = true;
      }
      SaveWhenEditEnds();
      ImGui::SameLine();
      if (ImGui::Button("Default##sens")) {
        rex::cvar::SetFlagByName("mouse_sensitivity", "1.0");
        Save();
      }
      bool wheel = REXCVAR_GET(weapon_wheel);
      if (ImGui::Checkbox("Weapon wheel (hold Q, or Y / triangle; 1-9 pick a weapon)", &wheel)) {
        rex::cvar::SetFlagByName("weapon_wheel", wheel ? "true" : "false");
        Save();
      }
      bool toggle_sprint = REXCVAR_GET(toggle_sprint);
      if (ImGui::Checkbox("Toggle sprint (press once; stops when you stop moving)",
                          &toggle_sprint)) {
        rex::cvar::SetFlagByName("toggle_sprint", toggle_sprint ? "true" : "false");
        Save();
      }

      // Upscaling: applied live by the (patched) presenter.
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextUnformatted("Upscaling");
      static const char* kEffects[] = {"fsr", "cas", "bilinear"};
      static const char* kEffectLabels[] = {"AMD FSR 1", "AMD CAS (sharpening only)",
                                            "Off (bilinear)"};
      std::string current = REXCVAR_GET(upscaler);
      int effect = 0;
      for (int i = 0; i < 3; ++i)
        if (current == kEffects[i]) effect = i;
      ImGui::SetNextItemWidth(slider_w);
      if (ImGui::Combo("##upscaler", &effect, kEffectLabels, 3)) {
        jth::gfx::SetUpscaler(kEffects[effect]);
        Save();
      }
      if (effect != 2) {
        float sharp = float(REXCVAR_GET(upscaler_sharpness)) * 100.0f;
        ImGui::TextUnformatted("Sharpness");
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat("##sharp", &sharp, 0.0f, 100.0f, "%.0f%%")) {
          jth::gfx::SetSharpness(sharp / 100.0f);
          dirty_ = true;
        }
        SaveWhenEditEnds();
        ImGui::SameLine();
        if (ImGui::Button("Default##sharp")) {
          jth::gfx::SetSharpness(0.9);
          Save();
        }
      }
      // Anti-aliasing: FXAA on the rendered image, switched live by the (patched) SDK.
      ImGui::TextUnformatted("Anti-aliasing");
      static const char* kAaModes[] = {"off", "fxaa", "fxaa_high"};
      static const char* kAaLabels[] = {"Off", "FXAA", "FXAA (high quality)"};
      const std::string current_aa = REXCVAR_GET(antialiasing);
      int aa = 0;
      for (int i = 0; i < 3; ++i)
        if (current_aa == kAaModes[i]) aa = i;
      ImGui::SetNextItemWidth(slider_w);
      if (ImGui::Combo("##antialiasing", &aa, kAaLabels, 3)) {
        jth::gfx::SetAntiAliasing(kAaModes[aa]);
        Save();
      }
      // Frame rate limit and FPS counter, live (framerate.cpp).
      ImGui::TextUnformatted("Frame rate");
      static const int kFps[] = {30, 60, 120, 144, 165, 185, 240};
      static const char* kFpsLabels[] = {"30 (original)", "60", "120", "144", "165", "185", "240"};
      int fps_index = 0;
      for (int i = 0; i < 7; ++i)
        if (REXCVAR_GET(framerate) == kFps[i]) fps_index = i;
      ImGui::SetNextItemWidth(slider_w);
      if (ImGui::Combo("##framerate", &fps_index, kFpsLabels, 7)) {
        jth::fps::SetFramerate(kFps[fps_index]);
        Save();
      }
      ImGui::SameLine();
      bool show_fps = REXCVAR_GET(show_fps);
      if (ImGui::Checkbox("Show FPS counter", &show_fps)) {
        jth::fps::ShowFpsCounter(show_fps);
        Save();
      }
      bool hide_hud = REXCVAR_GET(hide_hud);
      if (ImGui::Checkbox("Hide HUD (crosshair, ammo, messages, subtitles, button prompts)",
                          &hide_hud)) {
        rex::cvar::SetFlagByName("hide_hud", hide_hud ? "true" : "false");
        Save();
      }
      // Display mode: the SDK's window settings, which it applies live.
      ImGui::TextUnformatted("Display mode");
      static const char* kModes[] = {"Borderless", "Fullscreen (exclusive)", "Windowed"};
      const bool full = rex::cvar::GetFlagByName("fullscreen") == "true";
      const bool exclusive = rex::cvar::GetFlagByName("fullscreen_exclusive") == "true";
      int mode = !full ? 2 : exclusive ? 1 : 0;
      ImGui::SetNextItemWidth(slider_w);
      if (ImGui::Combo("##display", &mode, kModes, 3)) {
        rex::cvar::SetFlagByName("fullscreen_exclusive", mode == 1 ? "true" : "false");
        rex::cvar::SetFlagByName("fullscreen", mode == 2 ? "false" : "true");
        Save();
      }
      if (mode == 2) {
        static const int kSizes[][2] = {{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
        static const char* kSizeLabels[] = {"Automatic", "1280 x 720", "1600 x 900", "1920 x 1080",
                                            "2560 x 1440"};
        const int ww = std::atoi(rex::cvar::GetFlagByName("window_width").c_str());
        const int wh = std::atoi(rex::cvar::GetFlagByName("window_height").c_str());
        int size = 0;
        for (int i = 1; i < 5; ++i)
          if (ww == kSizes[i][0] && wh == kSizes[i][1]) size = i;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (ImGui::Combo("##window_size", &size, kSizeLabels, 5)) {
          rex::cvar::SetFlagByName("window_width", std::to_string(kSizes[size][0]));
          rex::cvar::SetFlagByName("window_height", std::to_string(kSizes[size][1]));
          Save();
        }
      }
      ImGui::TextDisabled("3D scene rendered at %s. Internal resolution is set in the launcher;",
                          jth::gfx::InternalResolutionText().c_str());
      ImGui::TextDisabled("above your display's, it also smooths edges (supersampling).");
      // HDR auto-exposure limit (see graphics_settings.cpp): 0 = each level's own value.
      float hdr = float(REXCVAR_GET(hdr_max_brightening));
      ImGui::TextUnformatted("Auto-exposure brightening limit");
      ImGui::SetNextItemWidth(slider_w);
      if (ImGui::SliderFloat("##hdr", &hdr, 0.0f, 4.0f, hdr <= 0.0f ? "Off (level's own)" : "%.1fx")) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", hdr < 0.05f ? 0.0f : hdr);
        rex::cvar::SetFlagByName("hdr_max_brightening", buf);
        dirty_ = true;
      }
      SaveWhenEditEnds();
      ImGui::SameLine();
      if (ImGui::Button("Default##hdr")) {
        rex::cvar::SetFlagByName("hdr_max_brightening", "2.0");
        Save();
      }
      ImGui::TextDisabled("Lower = darker bright scenes. Fixes The Lost Castaways' white glare.");

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextUnformatted("Cheats");
      auto cheat_box = [this](const char* label, const char* cvar, bool value) {
        bool v = value;
        if (ImGui::Checkbox(label, &v)) {
          rex::cvar::SetFlagByName(cvar, v ? "true" : "false");
          Save();
        }
      };
      cheat_box("God mode", "cheat_god_mode", REXCVAR_GET(cheat_god_mode));
      cheat_box("Infinite ammo (no reloading)", "cheat_infinite_ammo",
                REXCVAR_GET(cheat_infinite_ammo));
      cheat_box("Infinite adrenaline", "cheat_infinite_adrenaline",
                REXCVAR_GET(cheat_infinite_adrenaline));
      cheat_box("Infinite grenades", "cheat_infinite_grenades",
                REXCVAR_GET(cheat_infinite_grenades));
      cheat_box("One hit kill", "cheat_one_hit_kill", REXCVAR_GET(cheat_one_hit_kill));
      // Super speed: the player's movement speed multiplier (cheats.cpp, JthMoveSpeed).
      ImGui::TextUnformatted("Super speed");
      ImGui::SameLine();
      int speed = 0;
      for (int i = 0; i < kSpeedCount; ++i)
        if (std::abs(REXCVAR_GET(cheat_super_speed) - kSpeeds[i]) < 0.01) speed = i;
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
      if (ImGui::Combo("##super_speed", &speed, kSpeedLabels, kSpeedCount)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", kSpeeds[speed]);
        rex::cvar::SetFlagByName("cheat_super_speed", buf);
        Save();
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("F2 (or a PlayStation controller's touchpad) closes this panel.");
      ImGui::TextDisabled("Controller: D-pad to move, A / cross to change. Saved right away.");
    }
    ImGui::End();
    if (!open) Close();
  }

 private:
  // Sliders: save once the drag (or keyboard/gamepad edit) is finished, not every frame.
  void SaveWhenEditEnds() {
    if (ImGui::IsItemDeactivatedAfterEdit()) Save();
  }

  // Writes everything the panel controls (the launcher reads the same keys).
  void Save() {
    using jth::settings::Quote;
    char sens[32], sharp[32], hdr[32], speed[32];
    std::snprintf(speed, sizeof(speed), "%.2f", REXCVAR_GET(cheat_super_speed));
    std::snprintf(sens, sizeof(sens), "%.3f", REXCVAR_GET(mouse_sensitivity));
    std::snprintf(sharp, sizeof(sharp), "%.2f", REXCVAR_GET(upscaler_sharpness));
    std::snprintf(hdr, sizeof(hdr), "%.2f", REXCVAR_GET(hdr_max_brightening));
    auto flag = [](bool b) { return std::string(b ? "true" : "false"); };
    dirty_ = false;
    if (jth::settings::UpsertTopLevel(
            config_path_,
            {{"fov", std::to_string(REXCVAR_GET(fov))},
             {"weapon_fov_fixed", flag(REXCVAR_GET(weapon_fov_fixed))},
             {"mouse_sensitivity", sens},
             {"upscaler", Quote(REXCVAR_GET(upscaler))},
             {"upscaler_sharpness", sharp},
             {"antialiasing", Quote(REXCVAR_GET(antialiasing))},
             {"framerate", std::to_string(REXCVAR_GET(framerate))},
             {"show_fps", flag(REXCVAR_GET(show_fps))},
             {"hide_hud", flag(REXCVAR_GET(hide_hud))},
             {"toggle_sprint", flag(REXCVAR_GET(toggle_sprint))},
             {"fullscreen", rex::cvar::GetFlagByName("fullscreen")},
             {"fullscreen_exclusive", rex::cvar::GetFlagByName("fullscreen_exclusive")},
             {"window_width", rex::cvar::GetFlagByName("window_width")},
             {"window_height", rex::cvar::GetFlagByName("window_height")},
             {"hdr_max_brightening", hdr},
             {"weapon_wheel", flag(REXCVAR_GET(weapon_wheel))},
             {"cheat_god_mode", flag(REXCVAR_GET(cheat_god_mode))},
             {"cheat_infinite_ammo", flag(REXCVAR_GET(cheat_infinite_ammo))},
             {"cheat_infinite_adrenaline", flag(REXCVAR_GET(cheat_infinite_adrenaline))},
             {"cheat_infinite_grenades", flag(REXCVAR_GET(cheat_infinite_grenades))},
             {"cheat_one_hit_kill", flag(REXCVAR_GET(cheat_one_hit_kill))},
             {"cheat_super_speed", speed}}))
      REXLOG_DEBUG("PC options saved to {}", config_path_.string());
    else
      REXLOG_WARN("PC options: couldn't save to {}", config_path_.string());
  }

  std::filesystem::path config_path_;
  bool dirty_ = false;
  bool blocking_ = false;  // holds the SDK's UI input blocker
};

// UI thread only. Non-owning: an open dialog deletes itself when closed.
PcOptionsDialog* g_dialog = nullptr;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
std::filesystem::path g_config_path;

PcOptionsDialog::~PcOptionsDialog() {
  if (g_dialog == this) g_dialog = nullptr;
  if (blocking_ && g_input) g_input->RemoveUIInputBlocker();
}

}  // namespace

namespace jth::opts {

// UI thread.
void TogglePanel() {
  REXLOG_DEBUG("PC options panel: {}", g_dialog ? "close" : "open");
  if (g_dialog)
    g_dialog->RequestClose();
  else if (g_drawer)
    g_dialog = new PcOptionsDialog(g_drawer, g_config_path);
}

void CreatePcOptions(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path) {
  g_drawer = drawer;
  g_config_path = std::move(config_path);
  rex::ui::RegisterBind("bind_pc_options", "F2", "Toggle PC Options (FOV, mouse sensitivity)",
                        [] { TogglePanel(); });
  rex::cvar::RegisterChangeCallback("pc_options_toggle_test", [](std::string_view, std::string_view) {
    jth::pc::RunOnUiThread([] { TogglePanel(); });
  });
}

void DestroyPcOptions() {
  rex::ui::UnregisterBind("bind_pc_options");
  delete g_dialog;  // also clears g_dialog
  g_drawer = nullptr;
}

bool PcOptionsOpen() {
  return g_panel_open.load(std::memory_order_relaxed);
}

void SetInputSystem(rex::input::InputSystem* input) { g_input = input; }

void PollTouchpad() {
  static uint32_t last = rex::input::TouchpadClickCount();
  const uint32_t now = rex::input::TouchpadClickCount();
  if (now == last) return;
  last = now;
  jth::pc::RunOnUiThread([] { TogglePanel(); });
}

}  // namespace jth::opts

// ---------------------------------------------------------------------------------------------
// FOV hook (guest render thread), before Camera::Update calls the projection builder.
// f1 = horizontal FOV in radians, r31 = the camera being updated.
// ---------------------------------------------------------------------------------------------
namespace {
// The first-person weapon is drawn with the world's projection, in its own near depth range. The
// (patched) SDK scales the viewport of those draws by viewmodel_xy_scale; with
// tan(our FOV / 2) / tan(game FOV / 2) the weapon looks exactly as the game would draw it (its own
// aim zoom and sprint widening included), whatever the FOV slider says.
double g_viewmodel_scale = 1.0;  // guest render thread only
void SetViewmodelScale(double scale) {
  if (!REXCVAR_GET(weapon_fov_fixed)) scale = 1.0;
  if (std::fabs(scale - g_viewmodel_scale) < 1e-4) return;
  g_viewmodel_scale = scale;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.5f", scale);
  rex::cvar::SetFlagByName("viewmodel_xy_scale", buf);
}
}  // namespace

void JthCameraFov(PPCRegister& f1, PPCRegister& r31) {
  if (!jth::pc::GameplayLookActive()) {  // menus, cutscenes: no first-person weapon scaling
    SetViewmodelScale(1.0);
    return;
  }
  const uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  const uint32_t world = LoadBe32(base + kWorldPtrAddr);
  if (!world || LoadBe32(base + world + 108) != r31.u32) return;  // player camera only

  const int fov = REXCVAR_GET(fov);
  if (fov == kGameDefaultFov) {
    SetViewmodelScale(1.0);
    return;
  }

  // The game's normal widescreen FOV: 70 degrees at 4:3, widened to 16:9 ("Hor+").
  const double base_43 = LoadBeFloat(base + kDefaultFovAddr);
  const double base_169 = 2.0 * std::atan(std::tan(base_43 * 0.5) * 4.0 / 3.0);
  const double k = std::tan(fov * kPi / 360.0) / std::tan(base_169 * 0.5);
  // Sprinting widens the game's FOV by ~12 degrees, which the scaling above carries along. The
  // engine stops drawing scenery beyond a view slope of 2.0 (~126.9 degrees wide), so anything
  // wider shows black bars at the screen edges; keep the final FOV just under that.
  const double game_fov = f1.f64;
  f1.f64 = std::min(2.0 * std::atan(std::tan(game_fov * 0.5) * k), kMaxFovRad);
  SetViewmodelScale(std::tan(f1.f64 * 0.5) / std::tan(game_fov * 0.5));
}
