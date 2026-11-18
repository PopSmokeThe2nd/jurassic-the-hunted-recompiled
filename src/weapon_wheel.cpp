#include "weapon_wheel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "pc_input.h"
#include "weapon_art.h"

REXCVAR_DEFINE_BOOL(weapon_wheel, true, "Controls",
                    "Hold the change-weapon key for a weapon wheel; number keys 1-9 pick a weapon")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(log_weapon_wheel, false, "Debug",
                    "Log the inventory's weapon slots and each weapon switch (developer tool)");
REXCVAR_DEFINE_INT32(weapon_wheel_direction, 0, "Debug",
                     "Weapon wheel switching: 0 = shorter way round, 1 = next weapon only, "
                     "-1 = previous weapon only (testing)")
    .range(-1, 1)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(double, mouse_sensitivity);  // defined in pc_input.cpp

namespace jth::wheel {
namespace {

constexpr uint32_t kVtPlayerController = 0x820153B4;  // csControllerActorPlayer
constexpr uint32_t kVtActor = 0x82009C8C;
constexpr uint32_t kVtInventory = 0x820067F4;  // wsInventory
constexpr uint32_t kItemTypeFirearm = 49;
constexpr int kMaxSlots = 9;
constexpr double kPi = 3.14159265358979323846;
// Pointer: mouse counts times mouse_sensitivity, kept within this radius. The direction picks the
// weapon once the pointer is at least kDeadZone of the way out, and a highlighted weapon only
// gives way once the pointer is kStickiness past the edge of its sector (no flicker at edges).
constexpr float kCursorRadius = 300.0f;
constexpr float kDeadZone = 0.35f;
constexpr double kStickiness = 8.0 * kPi / 180.0;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Guest {
  const uint8_t* base;
  uint32_t u32(uint32_t a) const {
    const uint8_t* p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  static bool heap_ptr(uint32_t p) { return p >= 0x40000000 && p < 0x80000000 && !(p & 3); }
  // Heap or the executable's data.
  static bool data_ptr(uint32_t p) {
    return (p >= 0x40000000 && p < 0x80000000) || (p >= 0x82000000 && p < 0x82800000);
  }
  // A printable, NUL-terminated string at p ("" if not one).
  std::string str(uint32_t p) const {
    std::string s;
    if (!data_ptr(p)) return s;
    for (uint32_t i = 0; i < 160; ++i) {
      const char c = char(base[p + i]);
      if (!c) return s;
      if (c < 0x20 || c > 0x7E) return {};
      s += c;
    }
    return {};
  }
};

struct Entry {
  uint32_t item = 0;
  std::string key;  // folder under models\weapons, e.g. "sho_mossberg" (picture lookup)
  std::string name;
  int loaded = -1, capacity = -1;  // firearms: rounds in the magazine
};

// Written by the game thread; read by the UI and input threads.
std::mutex g_mutex;
std::vector<Entry> g_entries;  // carried weapons in slot order
std::atomic<uint32_t> g_current{0};
std::atomic<int64_t> g_last_frame_ns{0};

// Wheel state: open flag and highlight (any thread), pointer (mouse thread). "Pending" is the
// change-weapon key held but not yet long enough to open the wheel: mouse movement then already
// aims the pointer (and still turns the camera, in case the press turns out to be a tap).
std::atomic<bool> g_open{false};
std::atomic<bool> g_pending{false};
std::atomic<int> g_highlight{-1};
std::mutex g_cursor_mutex;
float g_cursor_x = 0, g_cursor_y = 0;

// Requested switch (input thread sets, game thread drives).
std::atomic<uint32_t> g_target{0};
std::atomic<int64_t> g_target_ns{0};
std::atomic<bool> g_pulse_ok{false};
std::atomic<int> g_dir{1};  // +1 next, -1 previous
int64_t g_last_pulse_ns = 0;  // game thread
uint32_t g_item_at_pulse = 0;
int g_pulses = 0;
bool g_prev_dir_pending = false;  // the last press was "previous"
enum class PrevState { kUnknown, kWorks, kBroken };
PrevState g_prev_state = PrevState::kUnknown;

// The weapon's folder, e.g. "models\weapons\sho_mossberg\mossberg.xml" -> "sho_mossberg".
std::string WeaponKey(std::string path) {
  for (char& c : path) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  if (size_t w = path.find("weapons/"); w != std::string::npos) {
    const std::string folder = path.substr(w + 8);
    return folder.substr(0, folder.find('/'));
  }
  return {};
}

// Readable weapon name, e.g. "models/weapons/sho_mossberg/mossberg.xml" -> "Mossberg shotgun".
std::string DisplayName(std::string path) {
  const std::string folder = WeaponKey(path);
  for (char& c : path) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  static const std::map<std::string, std::string> kNames = {
      {"ass_bar", "BAR"},
      {"ass_m249", "M249"},
      {"ass_scar", "SCAR"},
      {"pis_colt1911", "Colt 1911"},
      {"pis_glock18c", "Glock 18C"},
      {"pis_smith", "Smith & Wesson revolver"},
      {"plasmagun", "Plasma gun"},
      {"rif_coltrifle", "Colt rifle"},
      {"rpg7", "RPG-7"},
      {"sho_future", "Combat shotgun"},
      {"sho_mossberg", "Mossberg shotgun"},
      {"sil_crossbow", "Crossbow"},
      {"sni_whitworth", "Whitworth rifle"},
      {"kel_tec_rfb", "Kel-Tec RFB"},
  };
  if (auto it = kNames.find(folder); it != kNames.end()) return it->second;
  // Unknown: the file name without extension, e.g. "bow".
  std::string stem = path.substr(path.find_last_of('/') + 1);
  stem = stem.substr(0, stem.find('.'));
  for (char& c : stem)
    if (c == '_') c = ' ';
  if (!stem.empty()) stem[0] = char(std::toupper(static_cast<unsigned char>(stem[0])));
  return stem.empty() ? std::string("Weapon") : stem;
}

// The item's definition file path: a "models..." string the item points to. The descriptor at
// +480 holds it at +12 (as AddWeapon's "No slot available for weapon %s" uses it); otherwise
// the item's own fields are searched.
std::string ItemPath(const Guest& g, uint32_t item, int* found_at) {
  *found_at = -1;
  if (const uint32_t desc = g.u32(item + 480); Guest::heap_ptr(desc)) {
    std::string s = g.str(g.u32(desc + 12));
    if (s.size() > 6 && _strnicmp(s.c_str(), "models", 6) == 0) {
      *found_at = 480;
      return s;
    }
  }
  for (uint32_t off = 4; off < 0x400; off += 4) {
    std::string s = g.str(g.u32(item + off));
    if (s.size() > 6 && _strnicmp(s.c_str(), "models", 6) == 0) {
      *found_at = int(off);
      return s;
    }
  }
  return {};
}

int IndexOf(const std::vector<Entry>& entries, uint32_t item) {
  for (size_t i = 0; i < entries.size(); ++i)
    if (entries[i].item == item) return int(i);
  return -1;
}

// ----- pictures of your own (UI thread) -----
// weapon_images\<key>.png next to the game replaces the built-in silhouette, e.g.
// weapon_images\sho_mossberg.png. Loaded on first use; "no file" is remembered too.
rex::ui::ImmediateDrawer* g_immediate = nullptr;
std::filesystem::path g_images_dir;
std::map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> g_pictures;

rex::ui::ImmediateTexture* CustomPicture(const std::string& key) {
  if (key.empty() || !g_immediate) return nullptr;
  auto it = g_pictures.find(key);
  if (it == g_pictures.end()) {
    std::unique_ptr<rex::ui::ImmediateTexture> tex;
    const std::filesystem::path file = g_images_dir / (key + ".png");
    std::error_code ec;
    if (std::filesystem::is_regular_file(file, ec)) {
      std::ifstream in(file, std::ios::binary);
      const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
      int w = 0, h = 0;
      const std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
      if (!rgba.empty() && w > 0 && h > 0 && w <= 4096 && h <= 4096)
        tex = g_immediate->CreateTexture(uint32_t(w), uint32_t(h),
                                         rex::ui::ImmediateTextureFilter::kLinear, false,
                                         rgba.data());
      if (tex)
        REXLOG_INFO("Weapon wheel: using picture {} ({} x {})", file.string(), w, h);
      else
        REXLOG_WARN("Weapon wheel: couldn't read picture {} (PNG up to 4096 x 4096)", file.string());
    }
    it = g_pictures.emplace(key, std::move(tex)).first;
  }
  return it->second.get();
}

// ----- the wheel itself (UI thread) -----
class WheelDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit WheelDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  ~WheelDialog() override;
  using ImGuiDialog::Close;

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::vector<Entry> entries;
    {
      std::lock_guard lock(g_mutex);
      entries = g_entries;
    }
    const uint32_t current = g_current.load();
    const int n = int(entries.size());
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##jth_weapon_wheel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(w * 0.5f, h * 0.5f);
    const float outer = h * 0.33f, inner = h * 0.115f;
    const float font = ImGui::GetFontSize() * std::max(1.4f, h / 420.0f);
    const float small_font = font * 0.78f;
    ImFont* f = ImGui::GetFont();
    auto text_centered = [&](ImVec2 at, float size, ImU32 col, const std::string& s) {
      const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str());
      dl->AddText(f, size, ImVec2(at.x - sz.x * 0.5f + 1, at.y - sz.y * 0.5f + 1),
                  IM_COL32(0, 0, 0, 200), s.c_str());
      dl->AddText(f, size, ImVec2(at.x - sz.x * 0.5f, at.y - sz.y * 0.5f), col, s.c_str());
    };

    // Dim the scene a little behind the wheel.
    dl->AddCircleFilled(c, outer * 1.08f, IM_COL32(0, 0, 0, 90), 96);
    const int hl = g_highlight.load();
    if (n == 0) {
      text_centered(c, font, IM_COL32(230, 230, 230, 255), "No weapons");
    }
    const float step = n ? float(2.0 * kPi / n) : 0.0f;
    for (int i = 0; i < n; ++i) {
      // Sector i is centered on angle -90 degrees + i * step (top, then clockwise).
      const float mid = float(-kPi / 2) + i * step;
      const float a0 = mid - step * 0.5f + 0.012f, a1 = mid + step * 0.5f - 0.012f;
      const bool lit = i == hl;
      const bool held = entries[i].item == current;
      const ImU32 fill = lit ? IM_COL32(205, 160, 60, 210)
                             : held ? IM_COL32(70, 70, 70, 190) : IM_COL32(25, 25, 25, 175);
      const int segs = std::max(8, int(64 / std::max(1, n)));
      for (int k = 0; k < segs; ++k) {
        const float t0 = a0 + (a1 - a0) * k / segs, t1 = a0 + (a1 - a0) * (k + 1) / segs;
        dl->AddQuadFilled(ImVec2(c.x + std::cos(t0) * inner, c.y + std::sin(t0) * inner),
                          ImVec2(c.x + std::cos(t0) * outer, c.y + std::sin(t0) * outer),
                          ImVec2(c.x + std::cos(t1) * outer, c.y + std::sin(t1) * outer),
                          ImVec2(c.x + std::cos(t1) * inner, c.y + std::sin(t1) * inner), fill);
      }
      // Picture, name, and "slot  rounds  (in hand)" stacked at the middle of the sector. The
      // picture is as wide as the sector allows there.
      const float r = (inner + outer) * 0.5f;
      const ImVec2 at(c.x + std::cos(mid) * r, c.y + std::sin(mid) * r);
      const float chord = n > 1 ? 2.0f * r * std::sin(step * 0.5f) : 2.0f * r;
      const float pic_w = std::min(outer * 0.62f, chord * 0.72f), pic_h = pic_w * 0.4f;
      const float block = pic_h + font * 0.25f + font + small_font;
      const float top = at.y - block * 0.5f;
      const ImU32 col = lit ? IM_COL32(20, 20, 20, 255) : IM_COL32(235, 235, 235, 255);
      const ImVec2 pmin(at.x - pic_w * 0.5f, top), pmax(at.x + pic_w * 0.5f, top + pic_h);
      if (rex::ui::ImmediateTexture* tex = CustomPicture(entries[i].key)) {
        // A picture of your own (weapon_images\<key>.png), aspect-fitted.
        const float s = std::min(pic_w / tex->width, pic_h / tex->height);
        const ImVec2 sz(tex->width * s, tex->height * s);
        const ImVec2 o(at.x - sz.x * 0.5f, top + (pic_h - sz.y) * 0.5f);
        dl->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<uintptr_t>(tex))), o,
                     ImVec2(o.x + sz.x, o.y + sz.y));
      } else {
        DrawWeaponArt(dl, entries[i].key, pmin, pmax,
                      lit ? IM_COL32(30, 26, 18, 255) : IM_COL32(225, 228, 220, 255),
                      lit ? IM_COL32(255, 235, 180, 110) : IM_COL32(0, 0, 0, 150));
      }
      text_centered(ImVec2(at.x, top + pic_h + font * 0.25f + font * 0.5f), font, col,
                    entries[i].name);
      std::string sub = std::to_string(i + 1);
      if (entries[i].capacity > 0)
        sub += "    " + std::to_string(entries[i].loaded) + " / " + std::to_string(entries[i].capacity);
      if (held) sub += "    (in hand)";
      text_centered(ImVec2(at.x, top + pic_h + font * 1.25f + small_font * 0.5f), small_font,
                    lit ? IM_COL32(40, 40, 40, 255) : IM_COL32(190, 190, 190, 255), sub);
    }
    // Center: the highlighted weapon, and how to use the wheel.
    if (hl >= 0 && hl < n) text_centered(ImVec2(c.x, c.y - font * 0.5f), font,
                                         IM_COL32(255, 220, 140, 255), entries[hl].name);
    text_centered(ImVec2(c.x, c.y + font * 0.7f), small_font * 0.9f, IM_COL32(200, 200, 200, 230),
                  "Release to switch");
    text_centered(ImVec2(c.x, c.y + font * 1.5f), small_font * 0.9f, IM_COL32(200, 200, 200, 230),
                  "Right-click: cancel");
    // Pointer (mouse direction).
    float cx, cy;
    {
      std::lock_guard lock(g_cursor_mutex);
      cx = g_cursor_x;
      cy = g_cursor_y;
    }
    const float len = std::sqrt(cx * cx + cy * cy);
    if (len > 1.0f) {
      const float d = std::min(len, kCursorRadius) / kCursorRadius * inner * 0.9f;
      dl->AddCircleFilled(ImVec2(c.x + cx / len * d, c.y + cy / len * d), h * 0.008f,
                          IM_COL32(255, 220, 140, 230), 16);
    }
    ImGui::End();
  }

};

// UI thread only.
rex::ui::ImGuiDrawer* g_drawer = nullptr;
WheelDialog* g_dialog = nullptr;

WheelDialog::~WheelDialog() {
  if (g_dialog == this) g_dialog = nullptr;
}

void ShowDialog(bool show) {
  jth::pc::RunOnUiThread([show] {
    if (show && !g_dialog && g_drawer && g_open.load()) {
      g_dialog = new WheelDialog(g_drawer);
    } else if (!show && g_dialog) {
      g_dialog->Close();  // deleted by the SDK after its next draw
      g_dialog = nullptr;
    }
  });
}

void RequestSwitch(uint32_t item, const char* how) {
  if (!item) return;
  if (item == g_current.load()) {
    g_target.store(0);
    return;
  }
  g_target_ns.store(NowNs());
  g_target.store(item);
  if (REXCVAR_GET(log_weapon_wheel)) REXLOG_INFO("Weapon wheel: switch to {:08X} ({})", item, how);
}

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
            std::filesystem::path images_dir) {
  g_drawer = drawer;
  g_immediate = immediate;
  g_images_dir = std::move(images_dir);
}

void Destroy() {
  delete g_dialog;  // also clears g_dialog
  g_pictures.clear();  // before the drawer that made them goes away
  g_immediate = nullptr;
  g_drawer = nullptr;
}

bool Enabled() { return REXCVAR_GET(weapon_wheel); }

void OnPlayerFrame(uint32_t controller) {
  const int64_t now = NowNs();
  g_last_frame_ns.store(now, std::memory_order_relaxed);
  const Guest g{rex::system::kernel_memory()->virtual_membase()};
  if (!Guest::heap_ptr(controller) || g.u32(controller) != kVtPlayerController) return;
  const uint32_t actor = g.u32(controller + 0x1C);
  if (!Guest::heap_ptr(actor) || g.u32(actor) != kVtActor) return;
  const uint32_t inv = g.u32(actor + 0x198);
  if (!Guest::heap_ptr(inv) || g.u32(inv) != kVtInventory) return;
  const uint32_t current = g.u32(inv + 152);
  g_current.store(current);

  // Weapon slots. Names are looked up once per item.
  struct Known {
    std::string key, name;  // name empty: empty hands, left out of the wheel
  };
  static std::map<uint32_t, Known> names;
  static uint64_t last_sig = 0;
  std::vector<Entry> entries;
  uint64_t sig = current;
  const uint32_t arr = g.u32(inv + 44);
  if (Guest::heap_ptr(arr)) {
    const uint32_t count = std::min<uint32_t>(g.u32(arr), 16);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t item = g.u32(arr + 8 + 4 * i);
      if (!Guest::heap_ptr(item)) continue;
      Entry e;
      e.item = item;
      sig = sig * 1000003 ^ item;
      auto it = names.find(item);
      if (it == names.end()) {
        int at = -1;
        const std::string path = ItemPath(g, item, &at);
        Known k{WeaponKey(path), DisplayName(path)};
        if (k.key == "no_weapon") k.name.clear();
        it = names.emplace(item, std::move(k)).first;
        if (REXCVAR_GET(log_weapon_wheel))
          REXLOG_INFO("Weapon wheel: slot {} item {:08X} vtable {:08X} type {} '{}' (path at +{}) -> {}",
                      i, item, g.u32(item), g.u32(item + 16), path, at, it->second.name);
      }
      if (it->second.name.empty()) continue;
      e.key = it->second.key;
      e.name = it->second.name;
      if (g.u32(item + 16) == kItemTypeFirearm) {
        const uint32_t cap = g.u32(item + 612), loaded = g.u32(item + 616);
        if (cap > 0 && cap < 1000 && loaded <= 1000) {
          e.capacity = int(cap);
          e.loaded = int(loaded);
        }
      }
      entries.push_back(std::move(e));
      if (entries.size() == kMaxSlots) break;
    }
  }
  if (sig != last_sig) {
    last_sig = sig;
    if (REXCVAR_GET(log_weapon_wheel)) {
      std::string list;
      for (const Entry& e : entries)
        list += (list.empty() ? "" : ", ") + e.name + (e.item == current ? " (current)" : "");
      REXLOG_INFO("Weapon wheel: carrying {} weapon(s): {}; current item {:08X}", entries.size(),
                  list, current);
    }
  }
  if (names.size() > 64) names.clear();  // items come and go between levels
  {
    std::lock_guard lock(g_mutex);
    g_entries = entries;
  }

  // When the weapon in hand last changed (a switch's draw animation follows it).
  static uint32_t last_current = 0;
  static int64_t changed_ns = 0;
  if (current != last_current) {
    last_current = current;
    changed_ns = now;
  }

  // A requested switch: press the game's own "next" or "previous weapon" (one query at a time,
  // see ConsumeChange) until the chosen weapon is in hand, whichever way round is shorter (the
  // game cycles through the slots in order, as the wheel shows them). A press made while the
  // previous switch is still animating is dropped by the game, so each press waits until the
  // weapon in hand has changed and settled (or 2 s without a change: the press was dropped).
  const uint32_t target = g_target.load();
  if (!target) {
    g_pulses = 0;
    g_pulse_ok.store(false);
    return;
  }
  const bool carried = IndexOf(entries, target) >= 0;
  if (current == target || !carried) {
    if (REXCVAR_GET(log_weapon_wheel))
      REXLOG_INFO("Weapon wheel: {} after {} press(es)",
                  current == target ? "switched" : "weapon no longer carried", g_pulses);
    uint32_t expected = target;  // a newer request stays
    g_target.compare_exchange_strong(expected, 0u);
    g_pulse_ok.store(false);
    return;
  }
  if (now - g_target_ns.load() > 8'000'000'000 || g_pulses > int(entries.size()) * 2 + 2) {
    REXLOG_WARN("Weapon wheel: couldn't switch to the chosen weapon ({} presses)", g_pulses);
    g_target.store(0);
    g_pulse_ok.store(false);
    return;
  }
  const int n = int(entries.size());
  const int ci = IndexOf(entries, current), ti = IndexOf(entries, target);
  const int forward = ci >= 0 ? (ti - ci + n) % n : 1, backward = ci >= 0 ? (ci - ti + n) % n : n;
  // "Previous" is only used once it has been seen to work (a press that changed the weapon).
  if (g_prev_dir_pending && current != g_item_at_pulse) g_prev_state = PrevState::kWorks;
  if (g_prev_dir_pending && now - g_last_pulse_ns > 2'000'000'000 && current == g_item_at_pulse &&
      g_prev_state == PrevState::kUnknown) {
    g_prev_state = PrevState::kBroken;
    REXLOG_WARN("Weapon wheel: \"previous weapon\" had no effect; using \"next\" only");
  }
  const int forced = REXCVAR_GET(weapon_wheel_direction);
  g_dir.store(forced ? forced
                     : (backward < forward && g_prev_state != PrevState::kBroken ? -1 : 1));
  const int64_t since = now - g_last_pulse_ns;
  const bool settled = current != g_item_at_pulse && now - changed_ns > 600'000'000;
  g_pulse_ok.store(g_pulses == 0 || (since > 250'000'000 && (settled || since > 2'000'000'000)));
}

bool ConsumeChange(bool prev) {
  if (!g_target.load() || prev != (g_dir.load() < 0) || !g_pulse_ok.exchange(false)) return false;
  g_last_pulse_ns = NowNs();
  g_item_at_pulse = g_current.load();
  g_prev_dir_pending = prev;
  ++g_pulses;
  if (REXCVAR_GET(log_weapon_wheel))
    REXLOG_INFO("Weapon wheel: press {} ({})", prev ? "previous" : "next", g_pulses);
  return true;
}

namespace {
// Which weapon the pointer (x, y) points at, or `hl` (the current highlight) while the pointer is
// near the center or still within `hl`'s sector plus the stickiness margin.
int PickSector(float x, float y, int n, int hl) {
  const float len = std::sqrt(x * x + y * y);
  if (n <= 0 || len < kCursorRadius * kDeadZone) return hl;
  // Angle clockwise from straight up, in [0, 2pi); sector i is centered on i * step.
  double a = std::atan2(double(y), double(x)) + kPi / 2;
  if (a < 0) a += 2 * kPi;
  const double step = 2 * kPi / n;
  const int pick = int(std::floor(a / step + 0.5)) % n;
  if (hl >= 0 && hl < n && pick != hl) {
    const double off = std::fabs(std::remainder(a - hl * step, 2 * kPi));
    if (off < step * 0.5 + kStickiness) return hl;
  }
  return pick;
}
}  // namespace

void BeginHold() {
  {
    std::lock_guard lock(g_cursor_mutex);
    g_cursor_x = g_cursor_y = 0;
  }
  g_pending.store(true);
}

void EndHold() { g_pending.store(false); }

bool Tracking() {
  return g_open.load(std::memory_order_relaxed) || g_pending.load(std::memory_order_relaxed);
}

void Open() {
  if (g_open.exchange(true)) return;
  g_pending.store(false);
  // Start on the weapon in hand, or where the mouse already points if it moved since the key
  // went down (a quick press-and-flick).
  int hl = -1, n = 0;
  {
    std::lock_guard lock(g_mutex);
    hl = IndexOf(g_entries, g_current.load());
    n = int(g_entries.size());
  }
  float x, y;
  {
    std::lock_guard lock(g_cursor_mutex);
    x = g_cursor_x;
    y = g_cursor_y;
  }
  g_highlight.store(PickSector(x, y, n, hl));
  ShowDialog(true);
}

void Confirm() {
  g_pending.store(false);
  if (!g_open.exchange(false)) return;
  const int hl = g_highlight.load();
  uint32_t item = 0;
  {
    std::lock_guard lock(g_mutex);
    if (hl >= 0 && hl < int(g_entries.size())) item = g_entries[hl].item;
  }
  RequestSwitch(item, "wheel");
  ShowDialog(false);
}

void Cancel() {
  g_pending.store(false);
  if (!g_open.exchange(false)) return;
  ShowDialog(false);
}

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

void SelectSlot(int index) {
  uint32_t item = 0;
  {
    std::lock_guard lock(g_mutex);
    if (index >= 0 && index < int(g_entries.size())) item = g_entries[index].item;
  }
  RequestSwitch(item, "number key");
}

void AddCursor(float dx, float dy) {
  // Mouse counts scaled by the mouse sensitivity (so the same hand movement works at any DPI /
  // sensitivity the camera feels right with), kept within kCursorRadius.
  const float k = float(std::clamp(REXCVAR_GET(mouse_sensitivity), 0.05, 20.0));
  float x, y;
  {
    std::lock_guard lock(g_cursor_mutex);
    g_cursor_x += dx * k;
    g_cursor_y += dy * k;
    const float len = std::sqrt(g_cursor_x * g_cursor_x + g_cursor_y * g_cursor_y);
    if (len > kCursorRadius) {
      g_cursor_x *= kCursorRadius / len;
      g_cursor_y *= kCursorRadius / len;
    }
    x = g_cursor_x;
    y = g_cursor_y;
  }
  if (!g_open.load()) return;  // still pending: Open() picks the starting sector
  int n;
  {
    std::lock_guard lock(g_mutex);
    n = int(g_entries.size());
  }
  g_highlight.store(PickSector(x, y, n, g_highlight.load()));
}

}  // namespace jth::wheel
