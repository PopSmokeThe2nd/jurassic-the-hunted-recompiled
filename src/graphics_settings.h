// High-level graphics settings, mapped onto the SDK's GPU / presenter options.
//
//   internal_resolution  auto | 720p | 1440p | 4k   (startup only)
//       The resolution the game's 3D scene is rendered at. The Xbox 360 renders at 1280x720;
//       1440p and 4k render at 2x and 3x that (2560x1440, 3840x2160). auto matches your display.
//   upscaler             fsr | cas | bilinear   (also changeable live from the F2 panel)
//       How the rendered image is scaled to the window. fsr = AMD FidelityFX Super Resolution 1
//       (edge-preserving scaling + RCAS sharpening); cas = AMD Contrast Adaptive Sharpening.
//       fsr2/fsr3 are accepted but use FSR 1 (the SDK's temporal path doesn't work here).
//   upscaler_sharpness   0..1 (default 0.9)
//       FSR: RCAS sharpness (1 = sharpest); CAS: additional sharpness.
//   antialiasing         off | fxaa | fxaa_high   (also changeable live from the F2 panel)
//       FXAA on the rendered 3D image, before upscaling (SDK swap_post_effect). The game itself
//       draws its main scene without MSAA, and MSAA can't be added: its 4x MSAA surfaces are a
//       half-resolution trick laid over the full-resolution depth buffer in EDRAM, and changing
//       their sample count breaks the image. A higher internal resolution is supersampling.
//
// The SDK's own options (resolution_scale, present_effect, ...) still work and take priority
// when set explicitly at startup.

#pragma once

#include <string>

namespace jth::gfx {

// Call before the GPU and presenter are created (ReXApp::OnPreSetup).
void ApplyGraphicsSettings();

// Live changes (F2 panel). Updates our setting and the SDK's presenter options, which the
// patched SDK applies immediately.
void SetUpscaler(const std::string& upscaler);  // fsr | cas | bilinear
void SetSharpness(double sharpness);            // 0..1
void SetAntiAliasing(const std::string& mode);  // off | fxaa | fxaa_high

// The 3D scene's render size, e.g. "3840 x 2160".
std::string InternalResolutionText();

}  // namespace jth::gfx
