#include "button_prompts.h"

#include "game_files.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>  // Dear ImGui's copy of stb_truetype (keycap and button labels)

REXCVAR_DEFINE_STRING(button_prompts, "xbox", "Controls",
                      "Button icons in the game's prompts: xbox (original), playstation, or "
                      "keyboard (your PC keys)")
    .allowed({"xbox", "playstation", "keyboard"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace jth::prompts {
namespace {

namespace fs = std::filesystem;

enum class Style { kPlayStation, kKeyboard };
enum class Button { kA, kB, kX, kY, kLB, kRB, kDpadUp, kRightStickX };

// One button inside a texture: the cell it occupies and which button it shows. The new image
// fills the area the original button covered (its alpha bounding box), so it lines up exactly
// where the game expects it.
struct Cell {
  int x, y, w, h;
  Button button;
  bool keep_aspect = false;  // fit inside instead of filling (shoulder buttons)
};
struct TextureEdit {
  const char* path;  // relative to the game data root
  int w, h;          // expected size; anything else is left alone
  std::vector<Cell> cells;
};

const std::vector<TextureEdit>& Edits() {
  static const std::vector<TextureEdit> edits = {
      // Small A B X Y (prompts in menus and text); the rest of the sheet is kept.
      {"gui\\textures\\icons.dds", 256, 256,
       {{0, 0, 64, 64, Button::kA}, {64, 0, 64, 64, Button::kB},
        {128, 0, 64, 64, Button::kX}, {192, 0, 64, 64, Button::kY}}},
      // Large A/B and X/Y, two of each.
      {"gui\\textures\\icons_big.dds", 256, 256,
       {{0, 0, 128, 128, Button::kA}, {128, 0, 128, 128, Button::kB},
        {0, 128, 128, 128, Button::kA}, {128, 128, 128, 128, Button::kB}}},
      {"gui\\textures\\icons_big_2.dds", 256, 256,
       {{0, 0, 128, 128, Button::kX}, {128, 0, 128, 128, Button::kY},
        {0, 128, 128, 128, Button::kX}, {128, 128, 128, 128, Button::kY}}},
      // RB above LB.
      {"gui\\textures\\icons\\xbox\\bumper.dds", 128, 128,
       {{0, 0, 128, 64, Button::kRB, true}, {0, 64, 128, 64, Button::kLB, true}}},
      // D-pad with "up" highlighted.
      {"gui\\textures\\icons\\xbox\\directional.dds", 64, 64, {{0, 0, 64, 64, Button::kDpadUp}}},
      {"gui\\textures\\icons\\directional.dds", 64, 64, {{0, 0, 64, 64, Button::kDpadUp}}},
      // "RS" right stick of the balancing prompt (fallen trees: "RS" between two arrows, which
      // are a separate texture). Named after the Wii's "tilt the remote" image.
      {"gui\\textures\\xbox\\controller_tilt1.dds", 256, 256,
       {{0, 0, 256, 256, Button::kRightStickX, true}}},
  };
  return edits;
}

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // RGBA8, straight alpha
  uint8_t* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
  const uint8_t* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

constexpr const char* kArrowLabel = "<arrow>";  // drawn as an up arrow

std::string FirstKey(const char* bind);

// Keyboard style: the first key bound to the pad button (pc_input.cpp maps each key to the same
// pad button everywhere, so the key shown always works).
std::string KeyLabel(Button b) {
  switch (b) {
    case Button::kA: return FirstKey("bind_jump");
    case Button::kB: return FirstKey("bind_crouch");
    case Button::kX: return FirstKey("bind_reload");
    case Button::kY: return FirstKey("bind_change_weapon");
    case Button::kLB: return FirstKey("bind_adrenaline");
    case Button::kRB: return FirstKey("bind_grenade");
    case Button::kDpadUp:
      // The game flips/rotates this one tile for all four D-pad directions, so draw an arrow
      // (which then reads as the matching arrow key) rather than a letter.
      return kArrowLabel;
    case Button::kRightStickX:
      // Balancing steers with the strafe keys (or the arrow keys / mouse; pc_input.cpp).
      return FirstKey("bind_move_left") + " / " + FirstKey("bind_move_right");
  }
  return "?";
}

// Display name of the first key in a bind setting ("C,LCtrl" -> "C", "LShift" -> "SHIFT").
std::string FirstKey(const char* bind) {
  std::string key = rex::cvar::GetFlagByName(bind);
  key = key.substr(0, key.find(','));
  key.erase(0, key.find_first_not_of(' '));
  key.erase(key.find_last_not_of(' ') + 1);
  static const std::pair<const char*, const char*> kNames[] = {
      {"space", "SPACE"}, {"lshift", "SHIFT"}, {"rshift", "SHIFT"}, {"lctrl", "CTRL"},
      {"rctrl", "CTRL"},  {"lalt", "ALT"},     {"ralt", "ALT"},     {"escape", "ESC"},
      {"return", "ENTER"}, {"enter", "ENTER"}, {"backspace", "BKSP"}, {"mouse4", "M4"},
      {"mouse5", "M5"},   {"capslock", "CAPS"}};
  for (auto& [from, to] : kNames)
    if (_stricmp(key.c_str(), from) == 0) return to;
  for (char& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
  return key.empty() ? "?" : key;
}

struct KeyFont {
  std::vector<uint8_t> data;
  stbtt_fontinfo info{};
  bool ok = false;
};

const KeyFont& GetKeyFont() {
  static const KeyFont font = [] {
    KeyFont f;
    char windir[MAX_PATH] = {};
    GetWindowsDirectoryA(windir, MAX_PATH);
    for (const char* name : {"segoeuib.ttf", "arialbd.ttf", "segoeui.ttf", "arial.ttf"}) {
      std::ifstream in(fs::path(windir) / "Fonts" / name, std::ios::binary);
      f.data.assign(std::istreambuf_iterator<char>(in), {});
      if (!f.data.empty() &&
          stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
        f.ok = true;
        break;
      }
    }
    return f;
  }();
  return font;
}

void DrawLabel(Image& out, const std::string& label, float max_size, float max_width);

// A keycap (rounded dark key with a light rim) with the label centered, w x h pixels.
// `roundness` is the corner radius as a share of the shorter side (0.5: a pill).
bool RenderKeycap(int w, int h, const std::string& label, Image& out, float roundness = 0.24f) {
  const KeyFont& font = GetKeyFont();
  if (!font.ok || w < 8 || h < 8) return false;
  out = Image{w, h, std::vector<uint8_t>(size_t(w) * h * 4, 0)};
  const float m = float(std::min(w, h)), rim = std::max(1.5f, m * 0.075f);
  const float radius = std::min(m * roundness, m * 0.5f - 1.0f);
  const float cx = w * 0.5f, cy = h * 0.5f, hx = w * 0.5f - 1.0f, hy = h * 0.5f - 1.0f;
  auto sd = [&](float px, float py) {  // signed distance to the rounded rectangle
    const float qx = std::fabs(px - cx) - (hx - radius), qy = std::fabs(py - cy) - (hy - radius);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
  };
  constexpr int kSs = 4;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      int outer = 0, inner = 0;
      for (int j = 0; j < kSs; ++j)
        for (int i = 0; i < kSs; ++i) {
          const float d = sd(x + (i + 0.5f) / kSs, y + (j + 0.5f) / kSs);
          outer += d <= 0;
          inner += d <= -rim;
        }
      if (!outer) continue;
      const float t = float(y) / h, fi = float(inner) / outer;
      const float fill[3] = {72 - 34 * t, 76 - 36 * t, 70 - 34 * t};  // dark grey, lighter on top
      const float edge[3] = {205, 210, 200};
      uint8_t* p = out.at(x, y);
      for (int k = 0; k < 3; ++k) p[k] = uint8_t(fill[k] * fi + edge[k] * (1 - fi));
      p[3] = uint8_t(255 * outer / (kSs * kSs));
    }

  if (label == kArrowLabel) {  // up arrow: triangle head + stem, 4x4 supersampled
    auto inside = [&](float px, float py) {
      const float u = (px - cx) / m, v = (py - cy) / m;  // -0.5..0.5
      const bool head = v >= -0.28f && v <= 0.0f && std::fabs(u) <= (v + 0.28f) * 0.9f;
      const bool stem = v > -0.02f && v <= 0.26f && std::fabs(u) <= 0.075f;
      return head || stem;
    };
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int a = 0;
        for (int j = 0; j < kSs; ++j)
          for (int i = 0; i < kSs; ++i) a += inside(x + (i + 0.5f) / kSs, y + (j + 0.5f) / kSs);
        if (!a) continue;
        a = a * 255 / (kSs * kSs);
        uint8_t* p = out.at(x, y);
        for (int k = 0; k < 3; ++k) p[k] = uint8_t((248 * a + p[k] * (255 - a)) / 255);
      }
    return true;
  }

  DrawLabel(out, label, h * (label.size() == 1 ? 0.62f : 0.42f), w * 0.76f);
  return true;
}

// White text, as large as fits (starting at font size max_size, no wider than max_width),
// centered on the image with its capital height.
void DrawLabel(Image& out, const std::string& label, float max_size, float max_width) {
  const KeyFont& font = GetKeyFont();
  if (!font.ok) return;
  const int w = out.w, h = out.h;
  const float cx = w * 0.5f, cy = h * 0.5f;
  const stbtt_fontinfo* f = &font.info;
  int hx0, hy0, hx1, hy1;
  stbtt_GetCodepointBox(f, 'H', &hx0, &hy0, &hx1, &hy1);
  float size = max_size;
  float scale = 0, width = 0;
  for (; size > 6; size -= 1.0f) {
    scale = stbtt_ScaleForPixelHeight(f, size);
    width = 0;
    for (size_t i = 0; i < label.size(); ++i) {
      int adv, lsb;
      stbtt_GetCodepointHMetrics(f, label[i], &adv, &lsb);
      width += adv * scale;
      if (i + 1 < label.size()) width += stbtt_GetCodepointKernAdvance(f, label[i], label[i + 1]) * scale;
    }
    if (width <= max_width) break;
  }
  const float baseline = cy + hy1 * scale * 0.5f;
  float pen = cx - width * 0.5f;
  for (size_t i = 0; i < label.size(); ++i) {
    int gw, gh, gx, gy;
    const float shift = pen - std::floor(pen);
    unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(f, scale, scale, shift, 0, label[i], &gw,
                                                         &gh, &gx, &gy);
    const int ox = int(std::floor(pen)) + gx, oy = int(std::lround(baseline)) + gy;
    for (int y = 0; y < gh; ++y)
      for (int x = 0; x < gw; ++x) {
        const int tx = ox + x, ty = oy + y;
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
        const int a = bmp[y * gw + x];
        uint8_t* p = out.at(tx, ty);
        for (int k = 0; k < 3; ++k) p[k] = uint8_t((248 * a + p[k] * (255 - a)) / 255);
      }
    stbtt_FreeBitmap(bmp, nullptr);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(f, label[i], &adv, &lsb);
    pen += adv * scale;
    if (i + 1 < label.size()) pen += stbtt_GetCodepointKernAdvance(f, label[i], label[i + 1]) * scale;
  }
}

// ---- PlayStation symbols ---------------------------------------------------------------------
// Drawn here (artwork made for this project, no image files): dark round buttons with a light rim
// and the coloured symbol, L1 / R1 as pills, the D-pad's four arrow pads with "up" lit, and the
// right stick with an R. Drawn at the size they're shown, 4x4 supersampled.

struct Rgb {
  float r, g, b;
};

// Paints every pixel that `inside` covers (4x4 samples per pixel) with color(x, y), over what's
// already there.
template <class Inside, class Color>
void Paint(Image& img, Inside inside, Color color) {
  constexpr int kSs = 4;
  for (int y = 0; y < img.h; ++y)
    for (int x = 0; x < img.w; ++x) {
      int n = 0;
      for (int j = 0; j < kSs; ++j)
        for (int i = 0; i < kSs; ++i) n += inside(x + (i + 0.5f) / kSs, y + (j + 0.5f) / kSs) ? 1 : 0;
      if (!n) continue;
      const float a = float(n) / (kSs * kSs);
      const Rgb c = color(x + 0.5f, y + 0.5f);
      uint8_t* d = img.at(x, y);
      const float da = d[3] / 255.0f, out_a = a + da * (1 - a);
      const float src[3] = {c.r, c.g, c.b};
      for (int k = 0; k < 3; ++k)
        d[k] = uint8_t(std::clamp((src[k] * a + d[k] * da * (1 - a)) / out_a, 0.0f, 255.0f) + 0.5f);
      d[3] = uint8_t(std::clamp(out_a * 255.0f + 0.5f, 0.0f, 255.0f));
    }
}

float SegmentDistance(float px, float py, float ax, float ay, float bx, float by) {
  const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
  const float t = std::clamp((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
  return std::hypot(wx - vx * t, wy - vy * t);
}

// Signed distance to an equilateral triangle pointing up, centred on its centroid (y down).
float TriangleDistance(float px, float py, float half_side) {
  const float k = std::sqrt(3.0f);
  float x = std::fabs(px) - half_side, y = -py + half_side / k;
  if (x + k * y > 0) {
    const float nx = (x - k * y) / 2, ny = (-k * x - y) / 2;
    x = nx, y = ny;
  }
  x -= std::clamp(x, -2 * half_side, 0.0f);
  return -std::hypot(x, y) * (y < 0 ? -1.0f : 1.0f);
}

// Dark round base with a light rim, a little lighter at the top (like the keycaps).
void RoundBase(Image& img, float cx, float cy, float r) {
  const float rim = std::max(1.5f, r * 0.085f);
  Paint(img, [&](float x, float y) { return std::hypot(x - cx, y - cy) <= r; },
        [](float, float) { return Rgb{205, 210, 200}; });
  Paint(img, [&](float x, float y) { return std::hypot(x - cx, y - cy) <= r - rim; },
        [&](float, float y) {
          const float t = std::clamp((y - (cy - r)) / (2 * r), 0.0f, 1.0f);
          return Rgb{64 - 30 * t, 68 - 32 * t, 64 - 30 * t};
        });
}

void FaceButton(Image& img, Button b) {
  const float cx = img.w * 0.5f, cy = img.h * 0.5f, r = std::min(img.w, img.h) * 0.5f - 1.0f;
  RoundBase(img, cx, cy, r);
  const float s = r * 0.42f, half = std::max(1.0f, r * 0.075f);  // symbol size, half the stroke
  switch (b) {
    case Button::kA:  // cross, blue
      Paint(img, [&](float x, float y) {
              return std::min(SegmentDistance(x, y, cx - s, cy - s, cx + s, cy + s),
                              SegmentDistance(x, y, cx - s, cy + s, cx + s, cy - s)) <= half;
            },
            [](float, float) { return Rgb{126, 178, 240}; });
      break;
    case Button::kB:  // circle, red
      Paint(img, [&](float x, float y) { return std::fabs(std::hypot(x - cx, y - cy) - s) <= half; },
            [](float, float) { return Rgb{242, 96, 98}; });
      break;
    case Button::kX:  // square, pink
      Paint(img, [&](float x, float y) {
              const float d = std::max(std::fabs(x - cx), std::fabs(y - cy));
              return std::fabs(d - s * 0.86f) <= half;
            },
            [](float, float) { return Rgb{232, 138, 214}; });
      break;
    case Button::kY:  // triangle, green
      Paint(img, [&](float x, float y) {
              return std::fabs(TriangleDistance(x - cx, y - cy - s * 0.1f, s * 1.02f)) <= half;
            },
            [](float, float) { return Rgb{70, 214, 166}; });
      break;
    default:
      break;
  }
}

// The D-pad's four arrow pads, pointing at the centre; "up" lit. The game turns this one tile for
// all four directions.
void Dpad(Image& img) {
  const float cx = img.w * 0.5f, cy = img.h * 0.5f, r = std::min(img.w, img.h) * 0.5f - 1.0f;
  RoundBase(img, cx, cy, r);
  // One pad (pointing down at the centre), in units of r: a rectangle with a pointed inner end.
  static constexpr float kPad[5][2] = {{-0.24f, -0.82f}, {0.24f, -0.82f}, {0.24f, -0.34f},
                                       {0.0f, -0.12f},   {-0.24f, -0.34f}};
  for (int dir = 0; dir < 4; ++dir) {
    // Rotate by 0, 90, 180, 270 degrees: up, right, down, left.
    const float c = dir == 0 ? 1.0f : dir == 2 ? -1.0f : 0.0f;
    const float sn = dir == 1 ? 1.0f : dir == 3 ? -1.0f : 0.0f;
    float pts[5][2];
    for (int i = 0; i < 5; ++i) {
      const float x = kPad[i][0] * r, y = kPad[i][1] * r;
      pts[i][0] = cx + x * c - y * sn;
      pts[i][1] = cy + x * sn + y * c;
    }
    auto inside = [&](float x, float y) {  // convex polygon, either winding
      bool pos = false, neg = false;
      for (int i = 0; i < 5; ++i) {
        const float* a = pts[i];
        const float* b = pts[(i + 1) % 5];
        const float cross = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
        pos |= cross > 0, neg |= cross < 0;
      }
      return !(pos && neg);
    };
    const Rgb lit{246, 246, 240}, dim{118, 122, 116};
    Paint(img, inside, [&](float, float) { return dir == 0 ? lit : dim; });
  }
}

// The right stick from above: a raised cap inside a ring, with an R.
void RightStick(Image& img) {
  const float cx = img.w * 0.5f, cy = img.h * 0.5f, r = std::min(img.w, img.h) * 0.5f - 1.0f;
  RoundBase(img, cx, cy, r);
  const float cap = r * 0.72f;
  Paint(img, [&](float x, float y) { return std::hypot(x - cx, y - cy) <= cap; },
        [&](float, float y) {
          const float t = std::clamp((y - (cy - cap)) / (2 * cap), 0.0f, 1.0f);
          return Rgb{92 - 34 * t, 96 - 36 * t, 92 - 34 * t};
        });
  Paint(img, [&](float x, float y) {
          return std::fabs(std::hypot(x - cx, y - cy) - cap) <= std::max(0.75f, r * 0.03f);
        },
        [](float, float) { return Rgb{150, 154, 146}; });
  DrawLabel(img, "R", cap * 1.15f, cap * 1.2f);
}

// The PlayStation version of a button, w x h pixels.
bool RenderPlayStation(Button b, int w, int h, Image& out) {
  if (w < 8 || h < 8) return false;
  out = Image{w, h, std::vector<uint8_t>(size_t(w) * h * 4, 0)};
  switch (b) {
    case Button::kA:
    case Button::kB:
    case Button::kX:
    case Button::kY:
      FaceButton(out, b);
      return true;
    case Button::kLB:
      return RenderKeycap(w, h, "L1", out, 0.5f);
    case Button::kRB:
      return RenderKeycap(w, h, "R1", out, 0.5f);
    case Button::kDpadUp:
      Dpad(out);
      return true;
    case Button::kRightStickX:
      RightStick(out);
      return true;
  }
  return false;
}

// ---- DXT5 (BC3) ------------------------------------------------------------------------------

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

void Rgb565(uint16_t c, uint8_t out[3]) {
  const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  out[0] = uint8_t(r << 3 | r >> 2);
  out[1] = uint8_t(g << 2 | g >> 4);
  out[2] = uint8_t(b << 3 | b >> 2);
}

uint16_t To565(const int c[3]) {
  auto q = [](int v, int max) { return std::clamp((v * max + 127) / 255, 0, max); };
  return uint16_t(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}

void DecodeBlock(const uint8_t* b, Image& img, int bx, int by) {
  uint8_t a[8] = {b[0], b[1]};
  if (a[0] > a[1])
    for (int k = 1; k < 7; ++k) a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1]) / 7);
  else {
    for (int k = 1; k < 5; ++k) a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1]) / 5);
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t abits = 0;
  for (int i = 0; i < 6; ++i) abits |= uint64_t(b[2 + i]) << (8 * i);
  uint8_t pal[4][3];
  Rgb565(uint16_t(b[8] | b[9] << 8), pal[0]);
  Rgb565(uint16_t(b[10] | b[11] << 8), pal[1]);
  for (int k = 0; k < 3; ++k) {
    pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
    pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
  }
  const uint32_t bits = Le32(b + 12);
  for (int i = 0; i < 16; ++i) {
    uint8_t* p = img.at(bx + (i & 3), by + (i >> 2));
    std::memcpy(p, pal[(bits >> (2 * i)) & 3], 3);
    p[3] = a[(abits >> (3 * i)) & 7];
  }
}

// Encodes one 4x4 block (alpha: 8-step ramp between the block's min and max).
void EncodeBlock(const Image& img, int bx, int by, uint8_t* out) {
  const uint8_t* px[16];
  for (int i = 0; i < 16; ++i) px[i] = img.at(bx + (i & 3), by + (i >> 2));

  int amax = 0, amin = 255;
  for (auto* p : px) amax = std::max<int>(amax, p[3]), amin = std::min<int>(amin, p[3]);
  out[0] = uint8_t(amax);
  out[1] = uint8_t(amin);
  uint64_t abits = 0;
  if (amax != amin)
    for (int i = 0; i < 16; ++i) {
      // 0 = amin .. 7 = amax; code 0 is a0 (amax), 1 is a1 (amin), 2..7 step from a0 to a1.
      const int t = int(std::lround((px[i][3] - amin) * 7.0 / (amax - amin)));
      const int code = t == 7 ? 0 : t == 0 ? 1 : 8 - t;
      abits |= uint64_t(code) << (3 * i);
    }
  for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(abits >> (8 * i));

  // Colour (BC3 colour blocks are always 4-colour): endpoints from the principal axis of the
  // visible pixels, then a least-squares refit for the chosen indices; keep whichever is better.
  float w[16], sum_w = 0, mean[3] = {};
  for (int i = 0; i < 16; ++i) {
    w[i] = px[i][3] > 8 ? 1.0f : 0.0f;  // invisible pixels don't matter
    sum_w += w[i];
    for (int k = 0; k < 3; ++k) mean[k] += w[i] * px[i][k];
  }
  if (sum_w == 0) {
    std::memset(out + 8, 0, 8);
    return;
  }
  for (float& m : mean) m /= sum_w;
  float cov[6] = {};  // rr rg rb gg gb bb
  for (int i = 0; i < 16; ++i) {
    const float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
    cov[0] += w[i] * d[0] * d[0], cov[1] += w[i] * d[0] * d[1], cov[2] += w[i] * d[0] * d[2];
    cov[3] += w[i] * d[1] * d[1], cov[4] += w[i] * d[1] * d[2], cov[5] += w[i] * d[2] * d[2];
  }
  float axis[3] = {1, 1, 1};
  for (int it = 0; it < 8; ++it) {
    const float a[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                        cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                        cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
    const float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (len < 1e-6f) break;
    for (int k = 0; k < 3; ++k) axis[k] = a[k] / len;
  }
  float tmin = 1e9f, tmax = -1e9f;
  for (int i = 0; i < 16; ++i)
    if (w[i] > 0) {
      const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] +
                      (px[i][2] - mean[2]) * axis[2];
      tmin = std::min(tmin, t), tmax = std::max(tmax, t);
    }

  // Encodes with the given endpoints; returns the error and fills bits / per-pixel weights.
  auto encode = [&](const float e0[3], const float e1[3], uint16_t& c0, uint16_t& c1,
                    uint32_t& bits, int sel[16]) {
    int i0[3], i1[3];
    for (int k = 0; k < 3; ++k) i0[k] = int(std::lround(e0[k])), i1[k] = int(std::lround(e1[k]));
    c0 = To565(i0);
    c1 = To565(i1);
    uint8_t pal[4][3];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    bits = 0;
    int64_t err = 0;
    for (int i = 0; i < 16; ++i) {
      int best = 0, best_d = INT32_MAX;
      for (int c = 0; c < 4; ++c) {
        int d = 0;
        for (int k = 0; k < 3; ++k) d += (px[i][k] - pal[c][k]) * (px[i][k] - pal[c][k]);
        if (d < best_d) best = c, best_d = d;
      }
      sel[i] = best;
      bits |= uint32_t(best) << (2 * i);
      if (w[i] > 0) err += best_d;
    }
    return err;
  };
  float e0[3], e1[3];
  for (int k = 0; k < 3; ++k)
    e0[k] = std::clamp(mean[k] + axis[k] * tmax, 0.0f, 255.0f),
    e1[k] = std::clamp(mean[k] + axis[k] * tmin, 0.0f, 255.0f);
  uint16_t c0, c1;
  uint32_t bits;
  int sel[16];
  int64_t err = encode(e0, e1, c0, c1, bits, sel);

  // Least squares: pixel = a * e0 + b * e1 with (a, b) per index (1,0) (0,1) (2/3,1/3) (1/3,2/3).
  static constexpr float kA[4] = {1, 0, 2.0f / 3, 1.0f / 3};
  float aa = 0, ab = 0, bb = 0, sa[3] = {}, sb[3] = {};
  for (int i = 0; i < 16; ++i) {
    if (w[i] == 0) continue;
    const float a = kA[sel[i]], b = 1 - a;
    aa += a * a, ab += a * b, bb += b * b;
    for (int k = 0; k < 3; ++k) sa[k] += a * px[i][k], sb[k] += b * px[i][k];
  }
  const float det = aa * bb - ab * ab;
  if (std::fabs(det) > 1e-6f) {
    float r0[3], r1[3];
    for (int k = 0; k < 3; ++k)
      r0[k] = std::clamp((sa[k] * bb - sb[k] * ab) / det, 0.0f, 255.0f),
      r1[k] = std::clamp((sb[k] * aa - sa[k] * ab) / det, 0.0f, 255.0f);
    uint16_t d0, d1;
    uint32_t dbits;
    int dsel[16];
    if (encode(r0, r1, d0, d1, dbits, dsel) < err) c0 = d0, c1 = d1, bits = dbits;
  }
  out[8] = uint8_t(c0), out[9] = uint8_t(c0 >> 8), out[10] = uint8_t(c1), out[11] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) out[12 + i] = uint8_t(bits >> (8 * i));
}

// ---- Drawing ---------------------------------------------------------------------------------

struct Box {
  float x0, y0, x1, y1;
};

// Area the original button covers (alpha bounding box) inside a cell.
Box ButtonArea(const Image& img, const Cell& c) {
  int x0 = c.x + c.w, y0 = c.y + c.h, x1 = c.x, y1 = c.y;
  for (int y = c.y; y < c.y + c.h; ++y)
    for (int x = c.x; x < c.x + c.w; ++x)
      if (img.at(x, y)[3] > 16) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
  if (x0 >= x1 || y0 >= y1) return {c.x + 2.0f, c.y + 2.0f, c.x + c.w - 2.0f, c.y + c.h - 2.0f};
  return {float(x0), float(y0), float(x1), float(y1)};
}

// Bilinear sample of src at (u, v) in texels, premultiplied.
void Sample(const Image& src, float u, float v, float out[4]) {
  u = std::clamp(u, 0.0f, float(src.w - 1));
  v = std::clamp(v, 0.0f, float(src.h - 1));
  const int x0 = int(u), y0 = int(v), x1 = std::min(x0 + 1, src.w - 1), y1 = std::min(y0 + 1, src.h - 1);
  const float fx = u - x0, fy = v - y0;
  const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
  const uint8_t* t[4] = {src.at(x0, y0), src.at(x1, y0), src.at(x0, y1), src.at(x1, y1)};
  for (int k = 0; k < 4; ++k) out[k] = 0;
  for (int i = 0; i < 4; ++i) {
    const float a = t[i][3] / 255.0f * w[i];
    for (int k = 0; k < 3; ++k) out[k] += t[i][k] * a;
    out[3] += a;
  }
}

// Draws src scaled into box (4x4 supersampled) over dst.
void DrawScaled(Image& dst, const Image& src, Box box, bool keep_aspect) {
  if (keep_aspect) {
    const float bw = box.x1 - box.x0, bh = box.y1 - box.y0;
    const float s = std::min(bw / src.w, bh / src.h), gw = src.w * s, gh = src.h * s;
    box = {box.x0 + (bw - gw) / 2, box.y0 + (bh - gh) / 2, box.x0 + (bw + gw) / 2, box.y0 + (bh + gh) / 2};
  }
  const float sx = src.w / (box.x1 - box.x0), sy = src.h / (box.y1 - box.y0);
  constexpr int kSs = 4;
  for (int y = int(box.y0); y < int(std::ceil(box.y1)); ++y)
    for (int x = int(box.x0); x < int(std::ceil(box.x1)); ++x) {
      float acc[4] = {};
      for (int j = 0; j < kSs; ++j)
        for (int i = 0; i < kSs; ++i) {
          const float px = x + (i + 0.5f) / kSs, py = y + (j + 0.5f) / kSs;
          if (px < box.x0 || px > box.x1 || py < box.y0 || py > box.y1) continue;
          float s[4];
          Sample(src, (px - box.x0) * sx - 0.5f, (py - box.y0) * sy - 0.5f, s);
          for (int k = 0; k < 4; ++k) acc[k] += s[k];
        }
      const float a = acc[3] / (kSs * kSs);
      if (a <= 0) continue;
      uint8_t* d = dst.at(x, y);
      const float da = d[3] / 255.0f, out_a = a + da * (1 - a);
      for (int k = 0; k < 3; ++k) {
        const float c = acc[k] / (kSs * kSs);  // premultiplied
        d[k] = uint8_t(std::clamp((c + d[k] * da * (1 - a)) / out_a, 0.0f, 255.0f) + 0.5f);
      }
      d[3] = uint8_t(std::clamp(out_a * 255.0f + 0.5f, 0.0f, 255.0f));
    }
}

// Builds the PlayStation version of one texture from the installed original. Returns false if
// the installed file isn't the expected DXT5 texture (it's then left alone).
bool BuildTexture(Style style, const TextureEdit& edit, const fs::path& game_root,
                  std::vector<uint8_t>& original, std::vector<uint8_t>& dds) {
  std::ifstream in(game_root / edit.path, std::ios::binary);
  original.assign(std::istreambuf_iterator<char>(in), {});
  dds = original;
  const size_t blocks = size_t(edit.w / 4) * (edit.h / 4);
  if (dds.size() < 128 + blocks * 16 || std::memcmp(dds.data(), "DDS ", 4) != 0 ||
      Le32(&dds[12]) != uint32_t(edit.h) || Le32(&dds[16]) != uint32_t(edit.w) ||
      std::memcmp(&dds[84], "DXT5", 4) != 0) {
    REXLOG_WARN("Button prompts: {} isn't the expected texture; keeping the original", edit.path);
    return false;
  }
  uint8_t* body = dds.data() + 128;
  Image img{edit.w, edit.h, std::vector<uint8_t>(size_t(edit.w) * edit.h * 4)};
  for (int by = 0; by < edit.h; by += 4)
    for (int bx = 0; bx < edit.w; bx += 4)
      DecodeBlock(body + ((by / 4) * (edit.w / 4) + bx / 4) * 16, img, bx, by);

  for (const Cell& c : edit.cells) {
    Box area = ButtonArea(img, c);
    Image glyph;
    bool keep_aspect = c.keep_aspect;
    if (style == Style::kPlayStation) {
      // Drawn at the size it's shown: round buttons in a square, L1 / R1 as pills at most about
      // twice as wide as they're high.
      const float aw = area.x1 - area.x0, ah = area.y1 - area.y0;
      const bool pill = c.button == Button::kLB || c.button == Button::kRB;
      const float gw = pill ? std::min(aw, ah * 2.0f) : std::min(aw, ah);
      const float gh = pill ? ah : std::min(aw, ah);
      area = {area.x0 + (aw - gw) * 0.5f, area.y0 + (ah - gh) * 0.5f, area.x0 + (aw + gw) * 0.5f,
              area.y0 + (ah + gh) * 0.5f};
      area = {std::floor(area.x0), std::floor(area.y0), std::ceil(area.x1), std::ceil(area.y1)};
      if (!RenderPlayStation(c.button, int(area.x1 - area.x0), int(area.y1 - area.y0), glyph)) {
        REXLOG_WARN("Button prompts: couldn't draw the PlayStation icons (no system font)");
        return false;
      }
      keep_aspect = false;
    } else {
      // Keycaps are drawn at their final size (crisp text); shoulder-button cells are very wide,
      // so cap those at a key-like width.
      const float aw = area.x1 - area.x0, ah = area.y1 - area.y0;
      const float kw = c.keep_aspect ? std::min(aw, ah * 1.6f) : aw;
      area = {area.x0 + (aw - kw) * 0.5f, area.y0, area.x0 + (aw + kw) * 0.5f, area.y1};
      area = {std::floor(area.x0), std::floor(area.y0), std::ceil(area.x1), std::ceil(area.y1)};
      if (!RenderKeycap(int(area.x1 - area.x0), int(area.y1 - area.y0), KeyLabel(c.button), glyph)) {
        REXLOG_WARN("Button prompts: couldn't draw keyboard icons (no system font)");
        return false;
      }
      keep_aspect = false;
    }
    for (int y = c.y; y < c.y + c.h; ++y) std::memset(img.at(c.x, y), 0, size_t(c.w) * 4);
    DrawScaled(img, glyph, area, keep_aspect);
    for (int by = c.y; by < c.y + c.h; by += 4)
      for (int bx = c.x; bx < c.x + c.w; bx += 4)
        EncodeBlock(img, bx, by, body + ((by / 4) * (edit.w / 4) + bx / 4) * 16);
  }
  return true;
}

}  // namespace

void Setup(const fs::path& game_root) {
  const std::string choice = REXCVAR_GET(button_prompts);
  if ((choice != "playstation" && choice != "keyboard") || game_root.empty()) return;
  const Style style = choice == "keyboard" ? Style::kKeyboard : Style::kPlayStation;
  int built = 0;
  for (const TextureEdit& edit : Edits()) {
    std::vector<uint8_t> original, ps;
    if (!BuildTexture(style, edit, game_root, original, ps)) continue;
    ++built;
    // The game asks for "gui\textures\icons\bumper.dds" and gets "icons\xbox\bumper.dds" (its
    // platform variant), so register the file under both names.
    std::string path = edit.path;
    if (size_t x = path.find("\\xbox\\"); x != std::string::npos) {
      std::string generic = path;
      files::AddReplacement(generic.erase(x, 5), original, ps);
    }
    files::AddReplacement(path, std::move(original), std::move(ps));
  }
  if (built)
    REXLOG_INFO("Button prompts: {} icons ({} textures)",
                style == Style::kKeyboard ? "keyboard" : "PlayStation", built);
}

}  // namespace jth::prompts
