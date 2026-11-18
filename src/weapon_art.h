// Weapon pictures for the weapon wheel: side-view silhouettes drawn with ImGui shapes (original
// artwork made for this project; the game itself ships no weapon pictures). Keyed by the weapon's
// folder under models\weapons, e.g. "sho_mossberg".

#pragma once

#include <string>

#include <imgui.h>

namespace jth::wheel {

// Draws the weapon's silhouette, aspect-fitted and centered in [min, max]. Unknown weapons get a
// generic one for their class (pis_ / sho_ / sni_ / ass_ ...).
void DrawWeaponArt(ImDrawList* dl, const std::string& key, ImVec2 min, ImVec2 max, ImU32 color,
                   ImU32 shadow);

}  // namespace jth::wheel
