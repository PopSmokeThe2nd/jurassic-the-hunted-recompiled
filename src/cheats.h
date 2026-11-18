// Cheats, toggled in the F2 PC Options panel or via settings:
//
//   cheat_god_mode            player can't be hurt (Invincible flag + health kept full)
//   cheat_infinite_ammo       current firearm's magazine stays full (no reloading)
//   cheat_infinite_adrenaline adrenaline meter stays full
//   cheat_infinite_grenades   grenades topped back up to 5 (Lua: Inventory:AddBullet)
//   cheat_one_hit_kill        damage the player deals is lethal (hook in the damage method)
//
// Applied once per frame from the player's look update (JthMouseLook), on the game thread that
// runs player logic. Every object is checked against its known vtable before anything is written.
//
// Object layout (found by reverse engineering, see README):
//   csControllerActorPlayer (vtable 0x820153B4)  +0x1C Actor*, +1072 gpManager*
//   Actor (vtable 0x82009C8C)                    +228 bool Invincible, +0x158 ActorProperties*,
//                                                +0x198 wsInventory*
//   ActorProperties[_SPBase]                     +4 float max health, +8 float health
//   wsInventory (vtable 0x820067F4)              +152 current item
//   firearm item (type +16 == 49)                +612 magazine capacity, +616 rounds loaded
//   gpManager (vtable 0x8202C14C)                +4 -> {count, capacity, items[]} incl. gpAdrenalin
//   gpAdrenalin (vtable 0x82034164)              +24 float meter; max at global 0x827121D8

#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace jth::cheats {

// controller = guest address of the player's csControllerActorPlayer (r31 in the look update).
void ApplyCheats(uint32_t controller);

// Game thread, from the GUI update (outside any running script): cheats done through the game's
// Lua API (infinite grenades: Inventory:GetBulletCount / AddBullet, twice a second).
void OnGuiUpdate(PPCContext& ctx, uint8_t* base);

}  // namespace jth::cheats

// Mid-function hook in ActorProperties_SPBase's apply-damage (sub_82368F98 @ 0x823692E4):
// r27 = attacker, r29 = the victim's ActorProperties, f30 = the final damage.
void JthDamage(PPCRegister& r27, PPCRegister& r29, PPCRegister& f30);
// Same for the plain ActorProperties apply-damage (sub_822D7510 @ 0x822D7520): r3 = victim's
// properties, r4 = damage info.
void JthDamageBase(PPCRegister& r3, PPCRegister& r4);
// Animal::Damage (sub_822EF018 @ 0x822EF114): r3 = the animal (health at +412), r4 = damage info.
void JthDamageAnimal(PPCRegister& r3, PPCRegister& r4);
