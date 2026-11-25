#include "cheats.h"

#include "dev_lua.h"
#include "pc_input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(cheat_god_mode, false, "Cheats", "Player can't be hurt")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cheat_infinite_ammo, false, "Cheats",
                    "Current firearm's magazine stays full (no reloading)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cheat_infinite_adrenaline, false, "Cheats", "Adrenaline meter stays full")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cheat_one_hit_kill, false, "Cheats",
                    "Anything the player hurts dies from one hit (the player's own damage only)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cheat_infinite_grenades, false, "Cheats",
                    "Grenades are topped back up to 5 after each throw")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(cheat_super_speed, 1.0, "Cheats",
                      "Player movement speed multiplier: 1 = normal, up to 4 (walking, running "
                      "and sprinting)")
    .range(1.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(log_damage, false, "Debug", "Log every damage calculation (developer tool)");
REXCVAR_DEFINE_BOOL(log_player_pos, false, "Debug",
                    "Log the player's position four times a second, e.g. to measure movement "
                    "speed (developer tool)");
REXCVAR_DEFINE_BOOL(log_ammo, false, "Debug",
                    "Log the weapon in hand's ammo fields whenever they change (developer tool)");

namespace jth::cheats {
namespace {

constexpr uint32_t kVtPlayerController = 0x820153B4;  // csControllerActorPlayer
constexpr uint32_t kVtActor = 0x82009C8C;             // Actor
constexpr uint32_t kVtActorProperties = 0x8202280C;
constexpr uint32_t kVtActorPropertiesSP = 0x8202FB1C;  // ActorProperties_SPBase
constexpr uint32_t kVtInventory = 0x820067F4;          // wsInventory
constexpr uint32_t kVtAdrenalin = 0x82034164;          // gpAdrenalin
constexpr uint32_t kVtGpManager = 0x8202C14C;          // gpManager
constexpr uint32_t kVtModeReloadPlasmaGun = 0x82030BCC;  // the plasma gun's recharge
constexpr uint32_t kAdrenalinMaxAddr = 0x827121D8;
constexpr uint32_t kItemTypeFirearm = 49;

struct Guest {
  uint8_t* base;
  uint32_t u32(uint32_t a) const {
    const uint8_t* p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  void set_u32(uint32_t a, uint32_t v) const {
    uint8_t* p = base + a;
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  }
  float f32(uint32_t a) const {
    uint32_t v = u32(a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  void set_f32(uint32_t a, float f) const {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    set_u32(a, v);
  }
  // Heap objects live in 0x40000000-0x7FFFFFFF; anything else isn't a valid object pointer.
  static bool heap_ptr(uint32_t p) { return p >= 0x40000000 && p < 0x80000000 && !(p & 3); }
};

bool g_god_applied = false;
uint32_t g_god_actor = 0;
std::atomic<uint32_t> g_player_actor{0};  // the player's Actor, refreshed every frame
std::atomic<float> g_sprint_stamina{-1.0f};  // ActorProperties +16, or -1 when unknown

}  // namespace

float SprintStamina() { return g_sprint_stamina.load(std::memory_order_relaxed); }

void ApplyCheats(uint32_t controller) {
  Guest g{rex::system::kernel_memory()->virtual_membase()};
  if (!Guest::heap_ptr(controller) || g.u32(controller) != kVtPlayerController) return;
  const uint32_t actor = g.u32(controller + 0x1C);
  if (!Guest::heap_ptr(actor) || g.u32(actor) != kVtActor) return;
  g_player_actor.store(actor, std::memory_order_relaxed);

  // Sprint stamina (ActorProperties +16): drains at SprintStaminaDecaySpeed (4/s, from ~30) while
  // the player sprints. Toggle sprint (pad_filter.cpp) watches it to see when the game's sprint
  // ends.
  {
    const uint32_t props = g.u32(actor + 0x158);
    const bool ok = Guest::heap_ptr(props) &&
                    (g.u32(props) == kVtActorProperties || g.u32(props) == kVtActorPropertiesSP);
    g_sprint_stamina.store(ok ? g.f32(props + 16) : -1.0f, std::memory_order_relaxed);
  }

  if (REXCVAR_GET(log_player_pos)) {  // Actor +0x50/+0x54/+0x58, four times a second
    static auto last_pos = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_pos >= std::chrono::milliseconds(250)) {
      last_pos = now;
      REXLOG_INFO("Player position: {:.2f} {:.2f} {:.2f} (sprint stamina {:.2f})",
                  g.f32(actor + 0x50), g.f32(actor + 0x54), g.f32(actor + 0x58),
                  g_sprint_stamina.load(std::memory_order_relaxed));
    }
  }
  if (REXCVAR_GET(log_ammo)) {
    // The item in hand: type, and the words around its magazine fields (+612 capacity, +616
    // loaded), logged when anything there changes.
    const uint32_t inv = g.u32(actor + 0x198);
    const uint32_t item = Guest::heap_ptr(inv) ? g.u32(inv + 152) : 0;
    if (Guest::heap_ptr(item)) {
      static uint32_t last_item = 0;
      static std::string last;
      std::string words;
      for (uint32_t off = 592; off < 660; off += 4) words += fmt::format(" +{}={:X}", off, g.u32(item + off));
      if (item != last_item || words != last) {
        last_item = item;
        last = words;
        REXLOG_INFO("ammo: item {:08X} vtable {:08X} type {}{}", item, g.u32(item), g.u32(item + 16), words);
      }
    }
  }

  const bool god = REXCVAR_GET(cheat_god_mode);
  const bool ammo = REXCVAR_GET(cheat_infinite_ammo);
  const bool adren = REXCVAR_GET(cheat_infinite_adrenaline);
  if (!god && !ammo && !adren && !g_god_applied) return;

  // God mode: the game's own Invincible flag (what scripts use for set pieces) plus full health
  // as a backstop for damage that ignores the flag (falls, scripted kills of the player).
  if (god) {
    g.base[actor + 228] = 1;
    g_god_applied = true;
    g_god_actor = actor;
    const uint32_t props = g.u32(actor + 0x158);
    if (Guest::heap_ptr(props)) {
      const uint32_t vt = g.u32(props);
      if (vt == kVtActorProperties || vt == kVtActorPropertiesSP) {
        const float max_health = g.f32(props + 4);
        const float health = g.f32(props + 8);
        if (max_health > 0.0f && health < max_health) {
          REXLOG_DEBUG("God mode: health {:.1f} -> {:.1f}", health, max_health);
          g.set_f32(props + 8, max_health);
        }
      }
    }
  } else if (g_god_applied) {
    if (g_god_actor == actor) g.base[actor + 228] = 0;
    g_god_applied = false;
  }

  // Infinite ammo: keep the current firearm's magazine full.
  if (ammo) {
    const uint32_t inv = g.u32(actor + 0x198);
    if (Guest::heap_ptr(inv) && g.u32(inv) == kVtInventory) {
      const uint32_t item = g.u32(inv + 152);
      if (Guest::heap_ptr(item) && g.u32(item + 16) == kItemTypeFirearm) {
        const uint32_t capacity = g.u32(item + 612);
        if (capacity > 0 && capacity < 1000 && g.u32(item + 616) < capacity)
          g.set_u32(item + 616, capacity);
      }
    }
  }

  // Infinite adrenaline: keep gpAdrenalin's meter at its maximum. gpManager+4 points to a small
  // array {u32 count, u32 capacity, gp process* items[]}; gpAdrenalin is normally item 0.
  if (adren) {
    const uint32_t mgr = g.u32(controller + 1072);
    if (Guest::heap_ptr(mgr) && g.u32(mgr) == kVtGpManager) {
      const uint32_t arr = g.u32(mgr + 4);
      if (Guest::heap_ptr(arr)) {
        const uint32_t count = g.u32(arr);
        for (uint32_t i = 0; i < count && i < 16; ++i) {
          const uint32_t proc = g.u32(arr + 8 + 4 * i);
          if (Guest::heap_ptr(proc) && g.u32(proc) == kVtAdrenalin) {
            static int n = 0;
            if (++n % 60 == 1)
              REXLOG_DEBUG("Adrenaline: state {} meter {:.2f} / {:.2f}", g.u32(proc + 8),
                           g.f32(proc + 24), g.f32(kAdrenalinMaxAddr));
            g.set_f32(proc + 24, g.f32(kAdrenalinMaxAddr));
            break;
          }
        }
      }
    }
  }
}

void OnGuiUpdate(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cheat_infinite_grenades) || !jth::pc::GameplayLookActive()) return;
  static auto last = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::milliseconds(500)) return;
  last = now;
  // Grenades are ammo ("bullets") of the frag grenade, as the game's weapons.lua adds them.
  // Topped up to 5, what the HUD shows when full.
  static const std::string kTopUp =
      "local a = world and world:GetPlayerActor() "
      "if a then "
      "local inv = a.Inventory "
      "local g = 'models/weapons/grenade_frag_d/grenade_frag_d.xml' "
      "local n = inv:GetBulletCount(g) "
      "if n < 5 then inv:AddBullet(g, 5 - n) end "
      "end";
  jth::devlua::RunNow(ctx, base, kTopUp, true);
}

}  // namespace jth::cheats

namespace {
// Shared by the apply-damage hooks: returns the damage to apply (one-hit kill) and logs.
// victim = the actor or animal hit; health = its current health (for the log).
double OnDamage(const char* where, uint32_t victim, float health, uint32_t attacker,
                double damage) {
  using namespace jth::cheats;
  const uint32_t player = g_player_actor.load(std::memory_order_relaxed);
  double out = damage;
  // Only damage the player deals, to anyone but the player: allies' and barricades' damage from
  // creatures, and damage to the player, are untouched.
  if (REXCVAR_GET(cheat_one_hit_kill) && player && attacker == player && victim != player &&
      damage > 0.0)
    out = std::max(damage, 1.0e6);
  if (REXCVAR_GET(log_damage))
    REXLOG_INFO("damage ({}) {:.1f}{} to {:08X}{} (health {:.1f}) by {:08X}{}", where, damage,
                out != damage ? " -> one hit kill" : "", victim,
                victim == player ? " (player)" : "", health, attacker,
                attacker == player ? " (player)" : "");
  return out;
}

bool DamageHooksActive() {
  return REXCVAR_GET(cheat_one_hit_kill) || REXCVAR_GET(log_damage);
}

// Who caused the damage: info+92 is a bullet (type 16, its shooter at +212) or the attacker.
uint32_t Attacker(const jth::cheats::Guest& g, uint32_t info) {
  const uint32_t src = g.u32(info + 92);
  if (!jth::cheats::Guest::heap_ptr(src)) return 0;
  return g.u32(src + 16) == 16 ? g.u32(src + 212) : src;
}
}  // namespace

// csControllerActor's movement step (sub_82241E10) at 0x82242054, right after it loads the
// controller's input speed modifier (+264, what the script command scSetInputSpeedModifier sets)
// into f0; the movement input (+76..+84) is multiplied by it next. r31 = the controller, which
// for the player is csControllerActorPlayer. Super speed scales the player's modifier.
void JthMoveSpeed(PPCRegister& r31, PPCRegister& f0) {
  using namespace jth::cheats;
  const double speed = REXCVAR_GET(cheat_super_speed);
  if (!(speed > 1.0)) return;
  Guest g{rex::system::kernel_memory()->virtual_membase()};
  if (!Guest::heap_ptr(r31.u32) || g.u32(r31.u32) != kVtPlayerController) return;
  f0.f64 *= std::min(speed, 4.0);
}

// wsModeReloadPlasmaGun's update (sub_82373A50) after the prologue: r31 = the mode (+4 its item,
// +64 bullet_load_time, +68 charge in seconds, +72 full charge). Each shot spends one load time of
// charge and the update sets the loaded rounds to charge / load time, so for infinite ammo the
// charge of the player's plasma gun is kept full.
void JthPlasmaCharge(PPCRegister& r31) {
  using namespace jth::cheats;
  const bool log = REXCVAR_GET(log_ammo);
  if (!REXCVAR_GET(cheat_infinite_ammo) && !log) return;
  Guest g{rex::system::kernel_memory()->virtual_membase()};
  const uint32_t mode = r31.u32;
  if (!Guest::heap_ptr(mode) || g.u32(mode) != kVtModeReloadPlasmaGun) return;
  if (log) {
    static uint32_t last_mode = 0;
    if (mode != last_mode) {
      last_mode = mode;
      REXLOG_INFO("ammo: plasma recharge {:08X} item {:08X} load time {:.2f} charge {:.2f} / {:.2f}",
                  mode, g.u32(mode + 4), g.f32(mode + 64), g.f32(mode + 68), g.f32(mode + 72));
    }
  }
  if (!REXCVAR_GET(cheat_infinite_ammo)) return;
  const uint32_t actor = g_player_actor.load(std::memory_order_relaxed);
  if (!Guest::heap_ptr(actor)) return;
  const uint32_t inv = g.u32(actor + 0x198);
  if (!Guest::heap_ptr(inv) || g.u32(inv) != kVtInventory) return;
  if (g.u32(inv + 152) != g.u32(mode + 4)) return;  // only the gun in the player's hands
  const float full = g.f32(mode + 72), charge = g.f32(mode + 68);
  if (full > 0.0f && full < 10000.0f && charge < full) {
    if (REXCVAR_GET(log_ammo)) REXLOG_INFO("ammo: plasma charge {:.2f} -> {:.2f}", charge, full);
    g.set_f32(mode + 68, full);
  }
}

// ActorProperties_SPBase apply-damage (sub_82368F98) at 0x823692E4: r27 = attacker (or 0),
// r29 = victim's ActorProperties (+4 max health, +8 health, +192 its actor), f30 = final damage.
void JthDamage(PPCRegister& r27, PPCRegister& r29, PPCRegister& f30) {
  using namespace jth::cheats;
  if (!DamageHooksActive()) return;
  Guest g{rex::system::kernel_memory()->virtual_membase()};
  const uint32_t props = r29.u32;
  if (!Guest::heap_ptr(props)) return;
  f30.f64 = OnDamage("actor", g.u32(props + 192), g.f32(props + 8), r27.u32, f30.f64);
}

// ActorProperties apply-damage (sub_822D7510) after the prologue: r3 = victim's properties,
// r4 = damage info (+24 damage).
void JthDamageBase(PPCRegister& r3, PPCRegister& r4) {
  using namespace jth::cheats;
  if (!DamageHooksActive()) return;
  Guest g{rex::system::kernel_memory()->virtual_membase()};
  const uint32_t props = r3.u32, info = r4.u32;
  if (!Guest::heap_ptr(props) || !Guest::heap_ptr(info)) return;
  const double damage = g.f32(info + 24);
  const double out =
      OnDamage("actor-base", g.u32(props + 192), g.f32(props + 8), Attacker(g, info), damage);
  if (out != damage) g.set_f32(info + 24, float(out));
}

// Animal::Damage (sub_822EF018) before it subtracts info+24 from the animal's health (+412):
// r3 = the animal, r4 = damage info.
void JthDamageAnimal(PPCRegister& r3, PPCRegister& r4) {
  using namespace jth::cheats;
  if (!DamageHooksActive()) return;
  Guest g{rex::system::kernel_memory()->virtual_membase()};
  const uint32_t animal = r3.u32, info = r4.u32;
  if (!Guest::heap_ptr(animal) || !Guest::heap_ptr(info)) return;
  const double damage = g.f32(info + 24);
  const double out = OnDamage("animal", animal, g.f32(animal + 412), Attacker(g, info), damage);
  if (out != damage) g.set_f32(info + 24, float(out));
}
