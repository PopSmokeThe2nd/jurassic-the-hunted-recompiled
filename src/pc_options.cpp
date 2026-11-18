#include "pc_options.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

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
REXCVAR_DECLARE(std::string, upscaler);     // defined in graphics_settings.cpp
REXCVAR_DECLARE(double, upscaler_sharpness);
REXCVAR_DECLARE(std::string, antialiasing);
REXCVAR_DECLARE(double, hdr_max_brightening);  // defined in graphics_settings.cpp
REXCVAR_DECLARE(bool, weapon_wheel);            // defined in weapon_wheel.cpp

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

// The dialog exists only while the panel is open: while any ImGui dialog is registered, the SDK
// repaints the window continuously (hundreds of presents per second even at a 30 fps cap).
class PcOptionsDialog final : public rex::ui::ImGuiDialog {
 public:
  PcOptionsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
      : ImGuiDialog(drawer), config_path_(std::move(config_path)) {
    g_panel_open.store(true);
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
      if (ImGui::Checkbox("Weapon wheel (hold the change-weapon key; 1-9 pick a weapon)", &wheel)) {
        rex::cvar::SetFlagByName("weapon_wheel", wheel ? "true" : "false");
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

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("F2 closes this panel. Changes are saved right away.");
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
    char sens[32], sharp[32], hdr[32];
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
             {"hdr_max_brightening", hdr},
             {"weapon_wheel", flag(REXCVAR_GET(weapon_wheel))},
             {"cheat_god_mode", flag(REXCVAR_GET(cheat_god_mode))},
             {"cheat_infinite_ammo", flag(REXCVAR_GET(cheat_infinite_ammo))},
             {"cheat_infinite_adrenaline", flag(REXCVAR_GET(cheat_infinite_adrenaline))},
             {"cheat_infinite_grenades", flag(REXCVAR_GET(cheat_infinite_grenades))},
             {"cheat_one_hit_kill", flag(REXCVAR_GET(cheat_one_hit_kill))}}))
      REXLOG_DEBUG("PC options saved to {}", config_path_.string());
    else
      REXLOG_WARN("PC options: couldn't save to {}", config_path_.string());
  }

  std::filesystem::path config_path_;
  bool dirty_ = false;
};

// UI thread only. Non-owning: an open dialog deletes itself when closed.
PcOptionsDialog* g_dialog = nullptr;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
std::filesystem::path g_config_path;

PcOptionsDialog::~PcOptionsDialog() {
  if (g_dialog == this) g_dialog = nullptr;
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
