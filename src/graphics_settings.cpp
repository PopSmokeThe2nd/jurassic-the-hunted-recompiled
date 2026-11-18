#include "graphics_settings.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

// Windows last: its macros must not leak into the SDK headers above.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>

REXCVAR_DEFINE_STRING(internal_resolution, "auto", "Graphics",
                      "3D rendering resolution: auto (match display), 720p (native), "
                      "1440p (2x), 4k (3x)")
    .allowed({"auto", "720p", "1440p", "4k"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(upscaler, "fsr", "Graphics",
                      "Scaling to the window: fsr (AMD FSR 1), cas (AMD CAS), bilinear "
                      "(fsr2 / fsr3 are accepted but use FSR 1)")
    .allowed({"fsr", "cas", "bilinear", "fsr2", "fsr3"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(upscaler_sharpness, 0.9, "Graphics",
                      "Sharpening strength 0..1: FSR RCAS sharpness (1 = sharpest) or CAS extra "
                      "sharpness")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(antialiasing, "off", "Graphics",
                      "Anti-aliasing of the 3D image: off, fxaa, fxaa_high (FXAA before "
                      "upscaling; the game has no usable MSAA)")
    .allowed({"off", "fxaa", "fxaa_high"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(hdr_max_brightening, 2.0, "Graphics",
                      "Upper limit for how far the game's HDR auto-exposure may brighten the "
                      "image (0 = the level's own value). Fixes the white-out in The Lost "
                      "Castaways, whose level allows 16x")
    .range(0.0, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace jth::gfx {
namespace {

// Our anti-aliasing setting -> SDK swap_post_effect.
const char* SwapPostEffect(const std::string& mode) {
  if (mode == "fxaa") return "fxaa";
  if (mode == "fxaa_high") return "fxaa_extreme";
  return "none";
}

bool UserSet(const char* name) {
  return rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault;
}

// Height of the primary display in physical pixels (independent of DPI scaling).
uint32_t DisplayHeight() {
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
    return mode.dmPelsHeight;
  return 1080;
}

// Maps our sharpness (0..1) onto the SDK's FSR / CAS options.
void ApplySharpness(double s) {
  s = std::clamp(s, 0.0, 1.0);
  char fsr[32], cas[32];
  std::snprintf(fsr, sizeof(fsr), "%.3f", 2.0 * (1.0 - s));  // RCAS: stops of sharpness reduction
  std::snprintf(cas, sizeof(cas), "%.3f", s);
  rex::cvar::SetFlagByName("present_fsr_sharpness_reduction", fsr);
  rex::cvar::SetFlagByName("present_cas_additional_sharpness", cas);
}

std::string Normalized(std::string effect) {
  if (effect == "fsr2" || effect == "fsr3") return "fsr";
  return effect;
}

}  // namespace

void SetUpscaler(const std::string& upscaler) {
  const std::string effect = Normalized(upscaler);
  rex::cvar::SetFlagByName("upscaler", effect);
  if (!rex::cvar::SetFlagByName("present_effect", effect))
    REXLOG_WARN("Graphics: upscaler '{}' unavailable in this build", effect);
  else
    REXLOG_INFO("Graphics: upscaler {} (live)", effect);
}

void SetSharpness(double sharpness) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", std::clamp(sharpness, 0.0, 1.0));
  rex::cvar::SetFlagByName("upscaler_sharpness", buf);
  ApplySharpness(sharpness);
}

void SetAntiAliasing(const std::string& mode) {
  rex::cvar::SetFlagByName("antialiasing", mode);
  // The patched SDK passes the change straight to the running GPU command processor.
  if (rex::cvar::SetFlagByName("swap_post_effect", SwapPostEffect(mode)))
    REXLOG_INFO("Graphics: anti-aliasing {} (live)", mode);
  else
    REXLOG_WARN("Graphics: anti-aliasing '{}' unavailable in this build", mode);
}

std::string InternalResolutionText() {
  const int scale = std::max(1, std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()));
  return std::to_string(1280 * scale) + " x " + std::to_string(720 * scale);
}

void ApplyGraphicsSettings() {
  // Internal resolution -> SDK resolution_scale (integer multiple of the console's 1280x720).
  if (!UserSet("resolution_scale") && !UserSet("draw_resolution_scale_x") &&
      !UserSet("draw_resolution_scale_y")) {
    std::string res = REXCVAR_GET(internal_resolution);
    int scale = 1;
    if (res == "4k") {
      scale = 3;
    } else if (res == "1440p") {
      scale = 2;
    } else if (res == "auto") {
      uint32_t h = DisplayHeight();
      scale = h >= 2160 ? 3 : h >= 1440 ? 2 : 1;
    }
    // resolution_scale belongs to the GPU plugin (rexgpu-xenos.dll), which is loaded after this
    // runs, so setting it directly is rejected (the setting doesn't exist yet). Hand it over
    // through the environment instead: when a late-loaded module registers a setting, the SDK
    // applies REX_<NAME> from the environment (cvar.cpp, late registration). A --resolution_scale
    // on the command line still wins.
    const std::string value = std::to_string(scale);
    if (!rex::cvar::SetFlagByName("resolution_scale", value))
      SetEnvironmentVariableA("REX_RESOLUTION_SCALE", value.c_str());
    // At 4K the GPU is the limit (the game renders with 4x MSAA, so 3x scale = 9x the samples) and
    // the image is supersampled anyway: use the game's own texture filtering instead of the SDK's
    // forced 4x anisotropic (measured +8% at 4K on an RTX 3080). Same plugin-setting handover.
    if (scale >= 3 && !UserSet("anisotropic_override") &&
        !rex::cvar::SetFlagByName("anisotropic_override", "-1"))
      SetEnvironmentVariableA("REX_ANISOTROPIC_OVERRIDE", "-1");
    REXLOG_INFO("Graphics: internal resolution {} -> {}x{} ({}x)", res, 1280 * scale, 720 * scale,
                scale);
  }

  // Anti-aliasing -> SDK swap_post_effect (a GPU plugin setting: same handover as above).
  if (!UserSet("swap_post_effect")) {
    const std::string mode = REXCVAR_GET(antialiasing);
    const char* effect = SwapPostEffect(mode);
    if (!rex::cvar::SetFlagByName("swap_post_effect", effect))
      SetEnvironmentVariableA("REX_SWAP_POST_EFFECT", effect);
    REXLOG_INFO("Graphics: anti-aliasing {}", mode);
  }

  // Upscaler -> SDK present_effect.
  if (!UserSet("present_effect")) {
    std::string effect = REXCVAR_GET(upscaler);
    // The SDK's FSR 2/3 path is temporal and needs real depth/motion vectors; this game provides
    // none, so it synthesizes them and the result is blocky and smeared. Use FSR 1 instead.
    if (effect == "fsr2" || effect == "fsr3") {
      REXLOG_WARN("Graphics: upscaler {} doesn't work with this game; using FSR 1", effect);
      effect = "fsr";
    }
    if (!UserSet("present_fsr_sharpness_reduction") && !UserSet("present_cas_additional_sharpness"))
      ApplySharpness(REXCVAR_GET(upscaler_sharpness));
    if (!rex::cvar::SetFlagByName("present_effect", effect)) {
      REXLOG_WARN("Graphics: upscaler '{}' unavailable in this build, using bilinear", effect);
      rex::cvar::SetFlagByName("present_effect", "bilinear");
    } else {
      REXLOG_INFO("Graphics: upscaler {}", effect);
    }
  }
}

}  // namespace jth::gfx

// The renderer's HDR parameter setters (sub_8213E3E8 sets the target values, sub_8213E468 the
// current ones; f1..f5 = avg intensity, max darkening, max brightening, blur power, rays power).
// The level's RenderParam zones feed them every frame (sub_822B6xxx), as does Lua
// Renderer:SetHDRParam. The auto-exposure measures the scene through a GPU -> CPU readback that
// the recompiled renderer doesn't provide, so the exposure always sits at the brightening limit:
// harmless where levels allow 1.3-1.5x, a white-out in L08a (The Lost Castaways, 16x).
void JthHdrParams(PPCRegister& f3) {
  const double cap = REXCVAR_GET(hdr_max_brightening);
  if (cap > 0.0 && f3.f64 > cap) f3.f64 = cap;
}
