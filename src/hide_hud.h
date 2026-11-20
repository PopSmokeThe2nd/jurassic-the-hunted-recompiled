// hide_hud: hides the game's HUD (crosshair, ammo, compass, objectives, messages, subtitles and
// button prompts) for a clean view. F2 panel or settings; applies live.
//
// The HUD is the GUI page "hud" (gui/hud.xml), loaded twice: once by the player's HUD object
// (ActorHud_SPBase, at ActorProperties +196, page at +4: status bar, compass, objectives,
// messages, subtitles...) and once in the GUI manager (crosshair). Both are shown and hidden with
// GuiPage::SetVisible (sub_8211CC48: r3 = page, r4 = visible, r5 = keep the fade going, r6;
// visible flag at page +22, name at +4). A hook at its start (after the prologue, 0x8211CC58)
// notes what the game asks for each "hud" page and keeps it hidden while hide_hud is on; once per
// frame, hud pages still showing are hidden, and turning the option off shows them again if the
// game wants them shown.

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace jth::hud {

// Game thread, once per frame (GUI manager update, menu_mouse.cpp).
void OnGuiUpdate(PPCContext& ctx, uint8_t* base);

}  // namespace jth::hud

// Mid-function hook at the start of GuiPage::SetVisible (sub_8211CC48 @ 0x8211CC58): r3 = the
// page, r4 = the requested visibility.
void JthGuiPageSetVisible(PPCRegister& r3, PPCRegister& r4);
