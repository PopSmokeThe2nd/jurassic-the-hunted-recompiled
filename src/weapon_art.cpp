#include "weapon_art.h"

#include <algorithm>
#include <cfloat>
#include <map>
#include <vector>

namespace jth::wheel {
namespace {

// A silhouette: convex polygons in a ~100 x 40 frame (y down, muzzle to the right). Overlapping
// parts are fine: everything is filled with one color.
using Poly = std::vector<ImVec2>;
using Art = std::vector<Poly>;

Poly R(float x0, float y0, float x1, float y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }

const std::map<std::string, Art>& Arts() {
  static const std::map<std::string, Art> arts = {
      // Pistols
      {"pis_colt1911",
       {R(20, 6, 92, 17), R(86, 3, 89, 6), R(22, 4, 27, 6), R(40, 17, 90, 21),
        {{22, 17}, {42, 17}, {36, 40}, {14, 40}}, R(54, 21, 58, 29), R(40, 26, 58, 29),
        R(46, 21, 48, 26), {{14, 6}, {20, 6}, {20, 12}, {16, 12}}}},
      {"pis_glock18c",
       {R(18, 6, 94, 17), R(88, 4, 91, 6), R(20, 4, 25, 6), R(36, 17, 90, 22),
        {{20, 17}, {42, 17}, {38, 40}, {16, 40}}, {{17, 38}, {38, 38}, {37, 45}, {17, 45}},
        R(54, 22, 58, 30), R(40, 27, 58, 30), R(46, 22, 48, 27)}},
      {"pis_smith",
       {R(48, 8, 98, 15), R(48, 15, 92, 18), R(93, 5, 96, 8), {{24, 6}, {50, 6}, {50, 22}, {30, 24}},
        R(30, 8, 50, 21), {{22, 18}, {34, 18}, {30, 40}, {14, 38}},
        {{18, 4}, {24, 4}, {26, 10}, {22, 10}}, R(34, 22, 46, 25), R(44, 18, 47, 25)}},
      // Shotguns
      {"sho_mossberg",
       {R(36, 10, 99, 14), R(40, 15, 90, 18), R(56, 13, 78, 20), R(24, 8, 44, 19),
        R(28, 19, 38, 22), {{0, 13}, {26, 9}, {26, 19}, {4, 27}, {0, 26}}}},
      {"sho_future",
       {R(46, 11, 98, 14), R(44, 8, 84, 18), R(18, 7, 48, 20), R(20, 5, 44, 7),
        {{40, 20}, {50, 20}, {52, 32}, {42, 32}}, {{24, 20}, {32, 20}, {29, 33}, {21, 33}},
        R(2, 9, 20, 17), R(0, 7, 5, 22)}},
      // Rifles and machine guns
      {"ass_scar",
       {R(80, 11, 99, 13), R(94, 10, 99, 14), R(54, 8, 82, 17), R(28, 7, 56, 16),
        R(30, 16, 56, 20), R(30, 5, 80, 7), {{44, 20}, {52, 20}, {55, 33}, {47, 33}},
        {{32, 20}, {38, 20}, {35, 31}, {29, 31}}, R(6, 8, 28, 15), R(2, 7, 7, 21)}},
      {"ass_bar",
       {R(72, 11, 99, 13), R(95, 10, 99, 14), R(56, 14, 86, 16), R(44, 10, 72, 17),
        R(26, 8, 48, 18), R(40, 18, 52, 28), R(30, 18, 40, 21),
        {{0, 12}, {28, 9}, {28, 18}, {4, 26}, {0, 25}}}},
      {"ass_m249",
       {R(74, 11, 99, 13), R(58, 9, 84, 15), R(24, 6, 62, 17), R(28, 4, 56, 6), R(40, 1, 54, 4),
        R(36, 17, 54, 30), {{78, 15}, {80, 15}, {86, 31}, {84, 31}},
        {{76, 15}, {78, 15}, {72, 31}, {70, 31}}, {{26, 17}, {32, 17}, {29, 28}, {23, 28}},
        R(4, 8, 24, 16), R(0, 6, 5, 20)}},
      {"rif_coltrifle",
       {R(46, 10, 99, 13), R(50, 14, 90, 15), R(48, 13, 78, 17), R(32, 7, 48, 17),
        R(26, 8, 34, 18), {{24, 4}, {29, 4}, {30, 8}, {26, 8}}, R(30, 18, 40, 21),
        {{0, 14}, {27, 9}, {27, 18}, {3, 26}, {0, 25}}}},
      {"sni_whitworth",
       {R(30, 11, 99, 13), R(22, 12, 84, 16), R(28, 5, 74, 8), R(68, 4, 74, 9), R(28, 4, 32, 9),
        R(36, 8, 38, 11), R(64, 8, 66, 11), R(36, 16, 92, 17), R(26, 16, 34, 19),
        {{0, 13}, {24, 10}, {24, 17}, {3, 25}, {0, 24}}}},
      {"kel_tec_rfb",
       {R(70, 11, 99, 13), R(20, 7, 74, 19), R(26, 5, 66, 7), {{40, 19}, {48, 19}, {50, 30}, {42, 30}},
        {{52, 19}, {58, 19}, {56, 30}, {50, 30}}, R(0, 8, 22, 21)}},
      // Others
      {"sil_crossbow",
       {R(8, 14, 72, 18), {{0, 13}, {10, 12}, {10, 20}, {2, 24}, {0, 23}},
        {{66, 1}, {70, 1}, {74, 14}, {70, 14}}, {{70, 18}, {74, 18}, {70, 31}, {66, 31}},
        R(68, 12, 76, 20), {{67, 2}, {68, 2}, {52, 15}, {51, 15}},
        {{51, 17}, {52, 17}, {68, 30}, {67, 30}}, R(30, 8, 52, 11),
        {{30, 18}, {36, 18}, {34, 28}, {28, 28}}, R(36, 18, 44, 21)}},
      {"rpg7",
       {R(8, 13, 86, 17), R(34, 11, 60, 19), {{82, 8}, {94, 8}, {100, 15}, {94, 22}, {82, 22}},
        R(76, 14, 84, 16), {{0, 10}, {10, 12}, {10, 18}, {0, 20}},
        {{36, 19}, {42, 19}, {40, 30}, {34, 30}}, {{52, 19}, {58, 19}, {56, 30}, {50, 30}},
        R(52, 7, 56, 11)}},
      {"plasmagun",
       {R(18, 8, 78, 20), {{30, 4}, {60, 4}, {64, 8}, {26, 8}}, R(78, 11, 92, 17),
        R(82, 8, 85, 20), R(88, 9, 90, 19), R(40, 20, 54, 28),
        {{24, 20}, {32, 20}, {29, 32}, {21, 32}}, {{6, 10}, {18, 8}, {18, 20}, {6, 18}}}},
  };
  return arts;
}

const Art& ArtFor(const std::string& key) {
  const auto& arts = Arts();
  if (auto it = arts.find(key); it != arts.end()) return it->second;
  // A similar weapon by class prefix.
  const char* fallback = "ass_scar";
  if (key.starts_with("pis_")) fallback = "pis_colt1911";
  else if (key.starts_with("sho_")) fallback = "sho_mossberg";
  else if (key.starts_with("sni_")) fallback = "sni_whitworth";
  else if (key.starts_with("rif_")) fallback = "rif_coltrifle";
  return arts.at(fallback);
}

// ImGui's anti-aliased fill wants clockwise (screen space) polygons.
void FillConvex(ImDrawList* dl, std::vector<ImVec2>& pts, ImU32 col) {
  float area = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    const ImVec2& a = pts[i];
    const ImVec2& b = pts[(i + 1) % pts.size()];
    area += a.x * b.y - b.x * a.y;
  }
  if (area < 0) std::reverse(pts.begin(), pts.end());
  dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col);
}

}  // namespace

void DrawWeaponArt(ImDrawList* dl, const std::string& key, ImVec2 min, ImVec2 max, ImU32 color,
                   ImU32 shadow) {
  const Art& art = ArtFor(key);
  float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
  for (const Poly& p : art)
    for (const ImVec2& v : p) {
      x0 = std::min(x0, v.x), y0 = std::min(y0, v.y);
      x1 = std::max(x1, v.x), y1 = std::max(y1, v.y);
    }
  const float bw = x1 - x0, bh = y1 - y0;
  if (bw <= 0 || bh <= 0) return;
  const float s = std::min((max.x - min.x) / bw, (max.y - min.y) / bh);
  const float ox = (min.x + max.x) * 0.5f - (x0 + bw * 0.5f) * s;
  const float oy = (min.y + max.y) * 0.5f - (y0 + bh * 0.5f) * s;
  const float drop = std::max(1.0f, s * 1.2f);
  std::vector<ImVec2> pts;
  for (int pass = 0; pass < 2; ++pass) {  // soft shadow, then the silhouette
    const float d = pass == 0 ? drop : 0.0f;
    for (const Poly& p : art) {
      pts.clear();
      for (const ImVec2& v : p) pts.push_back(ImVec2(ox + v.x * s + d, oy + v.y * s + d));
      FillConvex(dl, pts, pass == 0 ? shadow : color);
    }
  }
}

}  // namespace jth::wheel
