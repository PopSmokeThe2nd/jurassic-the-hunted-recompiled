#include "hide_hud.h"

#include "jurassichunted_funcs.h"  // generated: guest function declarations

#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(hide_hud, false, "Graphics",
                    "Hide the HUD (crosshair, ammo, compass, objectives, messages, subtitles and "
                    "button prompts)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kVtGuiPage = 0x82005ADC;

uint32_t Be32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool HeapPtr(uint32_t p) { return p >= 0x40000000 && p < 0x80000000 && !(p & 3); }

// A live GuiPage named "hud".
bool IsHudPage(const uint8_t* base, uint32_t page) {
  if (!HeapPtr(page) || Be32(base, page) != kVtGuiPage) return false;
  const uint32_t name = Be32(base, page + 4);
  if (!HeapPtr(name) && !(name >= 0x82000000 && name < 0x82800000)) return false;
  return base[name] == 'h' && base[name + 1] == 'u' && base[name + 2] == 'd' && !base[name + 3];
}

struct PageState {
  bool wanted = false;  // what the game last asked for
  bool hidden = false;  // we're keeping it hidden
};

std::mutex g_mutex;
std::map<uint32_t, PageState> g_pages;  // the "hud" pages seen
bool g_internal = false;                // our own SetVisible call (game thread)

}  // namespace

void JthGuiPageSetVisible(PPCRegister& r3, PPCRegister& r4) {
  const uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  const uint32_t page = r3.u32;
  if (g_internal || !IsHudPage(base, page)) return;
  std::lock_guard lock(g_mutex);
  PageState& s = g_pages[page];
  s.wanted = (r4.u32 & 0xFF) != 0;
  if (s.wanted && REXCVAR_GET(hide_hud)) {
    r4.u64 = 0;
    s.hidden = true;
  }
}

namespace jth::hud {

void OnGuiUpdate(PPCContext& ctx, uint8_t* base) {
  const bool hide = REXCVAR_GET(hide_hud);
  std::lock_guard lock(g_mutex);
  for (auto it = g_pages.begin(); it != g_pages.end();) {
    const uint32_t page = it->first;
    PageState& s = it->second;
    if (!IsHudPage(base, page)) {  // freed with its level
      it = g_pages.erase(it);
      continue;
    }
    const bool visible = base[page + 22] != 0;
    int want = -1;  // -1: leave it, 0: hide, 1: show
    if (hide && visible) {
      want = 0;
      s.hidden = true;
    } else if (!hide && s.hidden) {
      s.hidden = false;
      if (s.wanted && !visible) want = 1;
    }
    if (want >= 0) {
      // GuiPage::SetVisible(page, visible, keep fading = 0, 0), as the game calls it.
      const PPCContext saved = ctx;
      ctx.r3.u64 = page;
      ctx.r4.u64 = uint64_t(want);
      ctx.r5.u64 = 0;
      ctx.r6.u64 = 0;
      g_internal = true;
      sub_8211CC48(ctx, base);
      g_internal = false;
      ctx = saved;
      REXLOG_DEBUG("HUD: page {:08X} {}", page, want ? "shown again" : "hidden");
    }
    ++it;
  }
}

}  // namespace jth::hud
