// Weapon wheel (PC): hold the change-weapon key (Q) to open a radial menu of the weapons you
// carry (picture, name, rounds), point at one with the mouse, and release (or click) to switch to
// it. A quick tap still switches to the next weapon, and the number keys 1-9 pick a weapon
// directly. On a controller, holding Y / triangle opens it and the left stick points
// (pad_filter.cpp).
//
//   weapon_wheel      enable the wheel and the number keys (default true)
//
// The game's controls only have "next weapon" (Y). The switch is done by pressing the game's own
// ChangeNext / ChangePrev actions (sub_821BC8A0 hook in menu_mouse.cpp; ChangePrev is queried
// but bound to nothing) until the chosen weapon is in hand, so the game's switch animation,
// sounds and rules (e.g. weapons without ammo) are unchanged.
//
// Inventory layout (wsInventory, vtable 0x820067F4, at Actor+0x198), from AddWeapon
// (sub_8213C3D0):
//   +44  -> {u32 count, u32 capacity, wsItem* slots[]}   weapon slots, null when empty
//   +152 wsItem* current item

#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace jth::wheel {

// images_dir: optional pictures of your own, <images_dir>\<weapon folder>.png (e.g.
// sho_mossberg.png), shown instead of the built-in silhouettes (weapon_art.cpp).
void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
            std::filesystem::path images_dir);
void Destroy();

bool Enabled();

// Game thread, once per frame from the player's look update: reads the inventory and drives a
// requested switch.
void OnPlayerFrame(uint32_t controller);

// Input thread. BeginHold when the change-weapon key goes down (the mouse starts aiming the
// pointer), EndHold if it comes back up as a tap, Open once it has been held long enough.
void BeginHold();
void EndHold();
// by_pad: opened with a controller (Y / triangle held, pad_filter.cpp): the left stick points
// (SetStick) and the hints name the controller's buttons.
void Open(bool by_pad = false);
// Controller: the left stick's direction (x right, y up, -1..1) points at a weapon.
void SetStick(float x, float y);
void Confirm();  // switch to the highlighted weapon and close
void Cancel();
bool IsOpen();
void SelectSlot(int index);  // number keys: 0-based position among the carried weapons

// Mouse movement: while Tracking(), it aims the wheel's pointer; once the wheel is open it no
// longer turns the camera.
bool Tracking();
void AddCursor(float dx, float dy);

// Game thread, from the input-action hook: true to report ChangeNext (prev = false) or ChangePrev
// (prev = true) as pressed for this query.
bool ConsumeChange(bool prev);

}  // namespace jth::wheel
