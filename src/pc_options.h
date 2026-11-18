// Field of view, and the in-game "PC Options" panel (F2) with sliders for FOV and mouse
// sensitivity.
//
//   fov   horizontal field of view in degrees at 16:9 (game default 86, range 60-120)
//
// The game builds its projection in Camera::Update (sub_82260948), passing a horizontal FOV in
// radians to the matrix builder sub_822640F0. JthCameraFov (mid-asm hook at 0x82260A98, see
// config/default.toml) rescales that value for the player's camera in tangent space, so
// scripted FOV changes keep their proportions. Cutscenes are left as authored. Aiming zoom is
// applied elsewhere and still works on top. The first-person weapon is drawn with the same
// projection (one Camera::Update per frame), so it also widens: smaller gun at high FOV.

#pragma once

#include <filesystem>

#include <rex/ppc/context.h>

namespace rex::ui {
class ImGuiDrawer;
}

namespace jth::opts {

// Creates the panel and binds F2 to show/hide it. config_path is where changes are saved.
void CreatePcOptions(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path);
void DestroyPcOptions();

// True while the panel is open (keyboard/mouse game input is paused and the cursor is freed).
bool PcOptionsOpen();

}  // namespace jth::opts

void JthCameraFov(PPCRegister& f1, PPCRegister& r31);
