// jurassichunted - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>

#include "button_prompts.h"
#include "crash_log.h"
#include "game_files.h"
#include "framerate.h"
#include "graphics_settings.h"
#include "pad_filter.h"
#include "pc_input.h"
#include "pc_options.h"
#include "recomp_achievements.h"
#include "test_input.h"
#include "weapon_wheel.h"

class JurassicHuntedApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<JurassicHuntedApp>(new JurassicHuntedApp(ctx, "jurassichunted",
        PPCImageConfig));
  }

  // If --game_data_root was not given, look for a "game" folder (the extracted disc)
  // next to the executable, then in each parent directory. This lets both a dev build
  // (out/build/<preset>/) and a packaged build find the user's game files.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    config_path_for_options_ = paths.config_path;
    std::error_code ec;
    if (paths.game_data_root.empty()) {
      for (auto dir = rex::filesystem::GetExecutableFolder(); !dir.empty();
           dir = dir.parent_path()) {
        auto candidate = dir / "game";
        if (std::filesystem::is_regular_file(candidate / "default.xex", ec)) {
          paths.game_data_root = candidate;
          break;
        }
        if (dir == dir.parent_path())
          break;
      }
    }

    // The SDK default is Documents\<app>, which is often OneDrive-redirected and protected by
    // Windows "Controlled folder access"; writes there fail and abort startup. Use
    // %LOCALAPPDATA%\JurassicTheHuntedRecomp instead unless the user overrode the path.
    auto sdk_default_user = rex::filesystem::GetUserFolder() / "jurassichunted";
    if (paths.user_data_root == sdk_default_user) {
      if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
        auto old_cache = sdk_default_user / "cache";
        paths.user_data_root = std::filesystem::path(local) / "JurassicTheHuntedRecomp";
        if (paths.cache_root == old_cache)
          paths.cache_root = paths.user_data_root / "cache";
      }
    }
    game_root_ = paths.game_data_root;
  }

  // F2: PC Options panel (FOV and mouse sensitivity sliders).
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    jth::opts::CreatePcOptions(drawer, config_path_for_options_);
    jth::fps::CreateFpsCounter(drawer);
    // Weapon wheel; pictures of your own go in a "weapon_images" folder next to the game.
    jth::wheel::Create(drawer, immediate_drawer(),
                       config_path_for_options_.parent_path() / "weapon_images");
  }
  void OnShutdown() override {
    jth::fps::DestroyFpsCounter();
    jth::wheel::Destroy();
    jth::opts::DestroyPcOptions();
  }

  // The SDK defines --perf_log_csv but never opens the file; do it here. Each row is one frame's
  // counters (frame_time_us etc.).
  void OnPostSetup() override {
    if (auto path = rex::cvar::GetFlagByName("perf_log_csv"); !path.empty())
      rex::perf::SetCsvLogPath(path);
    // After setup, because the "vsync" setting it changes lives in the GPU plugin, which only
    // registers its settings once loaded.
    jth::fps::ApplyFramerateSettings();
    REXLOG_INFO("Graphics: rendering at {} (resolution_scale {}), post effect {}",
                jth::gfx::InternalResolutionText(), rex::cvar::GetFlagByName("resolution_scale"),
                rex::cvar::GetFlagByName("swap_post_effect"));
    // PlayStation button icons, if chosen (built from the installed textures before the game runs).
    jth::prompts::Setup(game_root_);
    jth::files::SetupDevOptions();  // dev_level_list
    jth::ach::Install();            // Recomp Achievements: save-load tracking
  }

  // GPU emulation is a runtime-loaded plugin in ReXGlue and is off unless requested.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Log + minidump on a hard crash (logs\ next to the exe, where the SDK writes its logs).
    jth::crash::Install(rex::filesystem::GetExecutableFolder() / "logs");
    if (config.gpu_plugin.empty())
      config.gpu_plugin = "xenos";
    jth::gfx::ApplyGraphicsSettings();  // internal resolution + upscaler

    // Controllers (SDL), plus keyboard/mouse and, for testing, a scripted controller. All
    // synthetic devices merge into player 1, so any of them can be used at any time.
    config.input_factory = [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = rex::input::CreateDefaultInputSystem(tool_mode);
      jth::opts::SetInputSystem(input.get());  // F2 panel: controller navigation, input blocker
      if (!tool_mode) {
        jth::pad::Install(input.get());  // controller weapon wheel, toggle sprint
        if (auto pc = jth::pc::CreatePcInputDriver())
          input->AddDriver(std::move(pc));
        if (auto scripted = jth::CreateScriptedInputDriver())  // --test_input_file only
          input->AddDriver(std::move(scripted));
      }
      return input;
    };
  }

 private:
  std::filesystem::path config_path_for_options_;  // jurassichunted.toml next to the exe
  std::filesystem::path game_root_;
};
