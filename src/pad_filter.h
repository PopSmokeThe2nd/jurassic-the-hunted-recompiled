// Controller extras, done by rewriting the controller state the game reads (SDK patch:
// InputSystem::SetGuestStateFilter), on the thread that polls the pad:
//
//  - Weapon wheel on a controller (with weapon_wheel on): holding Y / triangle opens the wheel
//    (weapon_wheel.cpp), the left stick points at a weapon and releasing the button switches to
//    it; B / circle cancels. A quick tap still changes to the next weapon, as before. While the
//    button is down the game doesn't see Y, and while the wheel is open it doesn't see the left
//    stick (the player stands still).
//  - toggle_sprint: one press of sprint (left stick click, or Shift) keeps sprinting; the game
//    sees the button held meanwhile. It ends as letting go of the button would: when you press
//    it again, stop or stop going forward, fire, aim, reload, crouch, melee, throw a grenade or
//    use adrenaline, or when the game's own sprint ends (stamina stops draining).
//
// Only in gameplay (jth::pc::GameplayLookActive); menus and cutscenes get the pad unchanged.

#pragma once

namespace rex::input {
class InputSystem;
}

namespace jth::pad {

void Install(rex::input::InputSystem* input);

}  // namespace jth::pad
