// Frame rate setting.
//
//   framerate   30 (original) | 60 | 120 | 144 | 165 | 185 | 240 (also live in the F2 panel)
//
// The game advances its world by the real time each frame took (verified: its clock runs at one
// game-second per real second at 30, 60 and ~200 fps), so higher frame rates don't speed it up.
// Its 30 fps cap comes from waiting for two vertical blanks per frame at the emulated 60 Hz.
// Above 30 we remove that cap (the SDK's guest "vsync" timer) and pace frames ourselves just
// before the game's VdSwap call (mid-asm hook JthFramePace at 0x824BA09C, config/default.toml).

//   show_fps    the game's frame rate in the top left corner (an ImGui overlay)

#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace jth::fps {

// Call once the GPU plugin is loaded (ReXApp::OnPostSetup).
void ApplyFramerateSettings();

// Live changes (F2 panel, UI thread). SetFramerate also lifts or restores the 30 fps cap.
void SetFramerate(int fps);
void ShowFpsCounter(bool show);

// The FPS counter overlay (show_fps); UI thread.
void CreateFpsCounter(rex::ui::ImGuiDrawer* drawer);
void DestroyFpsCounter();

// Game frames per second, measured at each VdSwap.
float MeasuredFps();

}  // namespace jth::fps

void JthFramePace();
