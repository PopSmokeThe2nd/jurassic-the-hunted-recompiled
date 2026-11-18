// Frame rate setting.
//
//   framerate   30 (original) | 60 | 120 | 144 | 165 | 185 | 240
//
// The game advances its world by the real time each frame took (verified: its clock runs at one
// game-second per real second at 30, 60 and ~200 fps), so higher frame rates don't speed it up.
// Its 30 fps cap comes from waiting for two vertical blanks per frame at the emulated 60 Hz.
// Above 30 we remove that cap (the SDK's guest "vsync" timer) and pace frames ourselves just
// before the game's VdSwap call (mid-asm hook JthFramePace at 0x824BA09C, config/default.toml).

#pragma once

namespace jth::fps {

// Call once the GPU plugin is loaded (ReXApp::OnPostSetup).
void ApplyFramerateSettings();

}  // namespace jth::fps

void JthFramePace();
