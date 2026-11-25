// Jurassic: The Hunted Recompiled - launcher.
//
// Graphics / Cheats / Game ISO settings over the title art, then starts jurassichunted.exe.
// Settings are written to jurassichunted.toml next to the game (the same file the game reads).
//
// Command line (mainly for testing):
//   --tab graphics|controls|cheats|iso   open on a tab
//   --game-dir <dir>             use/install game data at <dir> instead of searching
//   --install-iso <file.iso>     start installing from that ISO immediately

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <cwctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "iso_install.h"
#include "prepare.h"
#include "settings_file.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// ---- layout (logical pixels; multiplied by the DPI scale) ----
constexpr float kWinW = 1280, kWinH = 720;
constexpr float kLogoW = 460;
// Kept low and short so the pterosaurs in the middle band of the art stay visible above it.
constexpr float kPanelX = 190, kPanelY = 232, kPanelW = 900, kPanelH = 470;
constexpr float kFooterH = 104;  // divider + status line + PLAY, at the bottom of the panel

// ---- colors ----
const ImVec4 kGreen(0.55f, 0.76f, 0.08f, 1.0f);
const ImVec4 kGreenHover(0.64f, 0.85f, 0.16f, 1.0f);
const ImVec4 kGreenActive(0.45f, 0.64f, 0.05f, 1.0f);
const ImVec4 kOrange(0.95f, 0.47f, 0.14f, 1.0f);
const ImVec4 kText(0.93f, 0.95f, 0.88f, 1.0f);
const ImVec4 kMuted(0.66f, 0.71f, 0.60f, 1.0f);

// ---- D3D ----
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_rtv;
UINT g_resize_w = 0, g_resize_h = 0;
float g_scale = 1.0f;

struct Texture {
  ComPtr<ID3D11ShaderResourceView> srv;
  int w = 0, h = 0;
  ImTextureRef ref() const { return ImTextureRef((ImTextureID)(intptr_t)srv.Get()); }
};

void CreateRenderTarget() {
  ComPtr<ID3D11Texture2D> back;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  g_device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv);
}

bool CreateDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                             2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got,
                                             &g_ctx);
  if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU driver: software fallback
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                       D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_ctx);
  if (FAILED(hr)) return false;
  CreateRenderTarget();
  return true;
}

// Decodes an image from the RCDATA resource `resource_id` embedded in the exe (launcher.rc), or
// from `fallback` on disk if the resource is missing.
Texture LoadTexture(LPCWSTR resource, const fs::path& fallback) {
  Texture t;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wic))))
    return t;
  ComPtr<IWICStream> stream;
  if (HRSRC res = FindResourceW(nullptr, resource, RT_RCDATA)) {
    HGLOBAL h = LoadResource(nullptr, res);
    void* data = h ? LockResource(h) : nullptr;
    DWORD size = SizeofResource(nullptr, res);
    if (data && size && SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(data), size)))
      wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec);
  }
  if (!dec && (fallback.empty() ||
                FAILED(wic->CreateDecoderFromFilename(fallback.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &dec))))
    return t;
  if (FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                              nullptr, 0, WICBitmapPaletteTypeCustom)))
    return t;
  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  std::vector<uint8_t> px(size_t(w) * h * 4);
  if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) return t;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {px.data(), w * 4, 0};
  ComPtr<ID3D11Texture2D> tex;
  if (FAILED(g_device->CreateTexture2D(&td, &init, &tex))) return t;
  g_device->CreateShaderResourceView(tex.Get(), nullptr, &t.srv);
  t.w = int(w);
  t.h = int(h);
  return t;
}

std::string Utf8(const fs::path& p) {
  auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

fs::path ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return fs::path(std::wstring(buf, n)).parent_path();
}

int DisplayRefreshHz() {
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    return int(dm.dmDisplayFrequency);
  return 60;
}

std::pair<int, int> DisplaySize() {
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
    return {int(dm.dmPelsWidth), int(dm.dmPelsHeight)};
  return {1920, 1080};
}

std::optional<fs::path> PickIsoFile(HWND owner) {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dlg))))
    return std::nullopt;
  const COMDLG_FILTERSPEC filters[] = {{L"Xbox 360 disc image (*.iso)", L"*.iso"},
                                       {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  dlg->SetTitle(L"Select your Jurassic: The Hunted (Xbox 360) disc image");
  if (FAILED(dlg->Show(owner))) return std::nullopt;
  ComPtr<IShellItem> item;
  PWSTR path = nullptr;
  if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
    return std::nullopt;
  fs::path p(path);
  CoTaskMemFree(path);
  return p;
}

// ---------------------------------------------------------------------------------------------
// Settings shared with the game (jurassichunted.toml)
// ---------------------------------------------------------------------------------------------
const char* kResValues[] = {"auto", "720p", "1440p", "4k"};
const char* kResLabels[] = {"Auto (match my display)", "720p  (Xbox 360 original)",
                            "1440p  (2x)", "4K  (3x, 3840 x 2160)"};
const int kResHeights[] = {0, 720, 1440, 2160};
// (fsr2/fsr3 were offered once; the game treats them as FSR 1, and so does Load below.)
const char* kUpsValues[] = {"fsr", "cas", "bilinear"};
const char* kUpsLabels[] = {"AMD FSR 1", "AMD CAS (sharpening only)", "Off (bilinear)"};
constexpr int kUpsCount = 3;
const int kFpsValues[] = {30, 60, 120, 144, 165, 185, 240};
const char* kFpsLabels[] = {"30 FPS  (original)", "60 FPS", "120 FPS", "144 FPS",
                            "165 FPS", "185 FPS", "240 FPS"};
constexpr int kFpsCount = 7;
// FXAA on the rendered image (the game's MSAA can't be changed; see graphics_settings.h).
const char* kAaValues[] = {"off", "fxaa", "fxaa_high"};
const char* kAaLabels[] = {"Off", "FXAA", "FXAA high"};
constexpr int kAaCount = 3;
const char* kPromptValues[] = {"xbox", "playstation", "keyboard"};
const char* kPromptLabels[] = {"Xbox", "PlayStation", "Keyboard & mouse"};
constexpr int kPromptCount = 3;

// Display mode (the SDK's fullscreen / fullscreen_exclusive) and window size (window_width /
// window_height, 0 = automatic).
const char* kDisplayLabels[] = {"Borderless", "Fullscreen (exclusive)", "Windowed"};
constexpr int kDisplayCount = 3;
constexpr int kWinSizes[][2] = {{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
const char* kWinSizeLabels[] = {"Automatic", "1280 x 720", "1600 x 900", "1920 x 1080",
                                "2560 x 1440"};
constexpr int kWinSizeCount = 5;

// Super speed cheat (cheat_super_speed: the player's movement speed multiplier).
constexpr double kSpeedValues[] = {1.0, 1.5, 2.0, 2.5, 3.0, 4.0};
const char* kSpeedLabels[] = {"Off", "1.5x", "2x", "2.5x", "3x", "4x"};
constexpr int kSpeedCount = 6;

struct Settings {
  int res = 0, aa = 0, upscaler = 0, fov = 86, fps = 0, prompts = 0;
  int display = 0, win_size = 0;  // kDisplayLabels, kWinSizes
  bool vsync = false, weapon_wheel = true, show_fps = false, hide_hud = false;
  bool toggle_sprint = false;
  bool god = false, ammo = false, adren = false, grenades = false, one_hit = false;
  int speed = 0;  // kSpeedValues

  void Load(const fs::path& file) {
    auto kv = jth::settings::ReadTopLevel(file);
    auto index_of = [&](const char* key, const char* const* values, int n, int def) {
      auto it = kv.find(key);
      if (it != kv.end())
        for (int i = 0; i < n; ++i)
          if (it->second == values[i]) return i;
      return def;
    };
    res = index_of("internal_resolution", kResValues, 4, 0);
    aa = index_of("antialiasing", kAaValues, kAaCount, 0);
    upscaler = index_of("upscaler", kUpsValues, kUpsCount, 0);
    prompts = index_of("button_prompts", kPromptValues, kPromptCount, 0);
    if (auto it = kv.find("fov"); it != kv.end()) fov = std::clamp(std::atoi(it->second.c_str()), 60, 120);
    if (auto it = kv.find("framerate"); it != kv.end()) {
      int v = std::atoi(it->second.c_str());  // nearest offered value
      for (int i = 0; i < kFpsCount; ++i)
        if (std::abs(kFpsValues[i] - v) < std::abs(kFpsValues[fps] - v)) fps = i;
    }
    auto flag = [&](const char* key) {
      auto it = kv.find(key);
      return it != kv.end() && it->second == "true";
    };
    vsync = flag("vertical_sync");
    show_fps = flag("show_fps");
    hide_hud = flag("hide_hud");
    toggle_sprint = flag("toggle_sprint");
    {
      auto it = kv.find("fullscreen");
      const bool full = it == kv.end() || it->second != "false";  // the SDK starts full screen
      display = !full ? 2 : flag("fullscreen_exclusive") ? 1 : 0;
      auto num = [&](const char* key) {
        auto v = kv.find(key);
        return v == kv.end() ? 0 : std::atoi(v->second.c_str());
      };
      const int ww = num("window_width"), wh = num("window_height");
      win_size = 0;
      for (int i = 1; i < kWinSizeCount; ++i)
        if (ww == kWinSizes[i][0] && wh == kWinSizes[i][1]) win_size = i;
    }
    if (auto it = kv.find("weapon_wheel"); it != kv.end()) weapon_wheel = it->second != "false";
    god = flag("cheat_god_mode");
    ammo = flag("cheat_infinite_ammo");
    adren = flag("cheat_infinite_adrenaline");
    grenades = flag("cheat_infinite_grenades");
    one_hit = flag("cheat_one_hit_kill");
    if (auto it = kv.find("cheat_super_speed"); it != kv.end()) {
      const double v = std::atof(it->second.c_str());  // nearest offered value
      for (int i = 0; i < kSpeedCount; ++i)
        if (std::abs(kSpeedValues[i] - v) < std::abs(kSpeedValues[speed] - v)) speed = i;
    }
  }

  bool Save(const fs::path& file) const {
    using jth::settings::Quote;
    return jth::settings::UpsertTopLevel(
        file, {{"internal_resolution", Quote(kResValues[res])},
               {"antialiasing", Quote(kAaValues[aa])},
               {"upscaler", Quote(kUpsValues[upscaler])},
               {"fov", std::to_string(fov)},
               {"framerate", std::to_string(kFpsValues[fps])},
               {"vertical_sync", vsync ? "true" : "false"},
               {"show_fps", show_fps ? "true" : "false"},
               {"hide_hud", hide_hud ? "true" : "false"},
               {"toggle_sprint", toggle_sprint ? "true" : "false"},
               {"fullscreen", display == 2 ? "false" : "true"},
               {"fullscreen_exclusive", display == 1 ? "true" : "false"},
               {"window_width", std::to_string(kWinSizes[win_size][0])},
               {"window_height", std::to_string(kWinSizes[win_size][1])},
               {"button_prompts", Quote(kPromptValues[prompts])},
               {"weapon_wheel", weapon_wheel ? "true" : "false"},
               {"cheat_god_mode", god ? "true" : "false"},
               {"cheat_infinite_ammo", ammo ? "true" : "false"},
               {"cheat_infinite_adrenaline", adren ? "true" : "false"},
               {"cheat_infinite_grenades", grenades ? "true" : "false"},
               {"cheat_one_hit_kill", one_hit ? "true" : "false"},
               {"cheat_super_speed", SpeedText(kSpeedValues[speed])}});
  }

  static std::string SpeedText(double v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
  }
};

// ---------------------------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------------------------
enum class Tab { Graphics, Controls, Cheats, Iso };

struct App {
  HWND hwnd = nullptr;
  fs::path exe_dir, config_file, game_exe, game_dir_override;
  Texture background, logo;
  ImFont* body = nullptr;
  ImFont* heading = nullptr;
  ImFont* button = nullptr;
  Settings settings;
  std::map<std::string, std::string> binds;  // [bind] table of jurassichunted.toml
  Tab tab = Tab::Graphics;
  jth::launcher::GameDataStatus game;
  jth::launcher::Installer installer;
  bool install_was_running = false;
  fs::path picked_iso;
  jth::launcher::IsoInfo picked_info;
  std::string save_error;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  bool want_quit = false;

  fs::path InstallTarget() const {
    if (!game_dir_override.empty()) return game_dir_override;
    if (game.found) return game.dir;
    return exe_dir / "game";
  }
  void RefreshGame() {
    game = game_dir_override.empty() ? jth::launcher::FindGameData(exe_dir)
                                     : jth::launcher::CheckGameDataDir(game_dir_override);
  }
  void SaveSettings() {
    save_error = settings.Save(config_file) ? "" : "Couldn't save settings to " + Utf8(config_file);
  }
};
App g_app;

float S(float v) { return v * g_scale; }
ImVec2 S(float x, float y) { return ImVec2(x * g_scale, y * g_scale); }

void ApplyStyle() {
  ImGuiStyle& st = ImGui::GetStyle();
  st = ImGuiStyle();
  ImGui::StyleColorsDark(&st);
  st.WindowRounding = 0;
  st.ChildRounding = 12;
  st.FrameRounding = 6;
  st.GrabRounding = 6;
  st.PopupRounding = 6;
  st.FramePadding = ImVec2(12, 8);
  st.ItemSpacing = ImVec2(10, 10);
  st.ChildBorderSize = 1;
  st.GrabMinSize = 18;
  ImVec4* c = st.Colors;
  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kMuted;
  c[ImGuiCol_ChildBg] = ImVec4(0.035f, 0.05f, 0.025f, 0.78f);
  c[ImGuiCol_Border] = ImVec4(0.55f, 0.76f, 0.08f, 0.35f);
  c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.08f, 0.04f, 0.97f);
  c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.16f, 0.09f, 0.95f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.24f, 0.12f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.30f, 0.14f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.16f, 0.22f, 0.10f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.36f, 0.12f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.45f, 0.10f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.30f, 0.42f, 0.08f, 0.9f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.38f, 0.52f, 0.10f, 1.0f);
  c[ImGuiCol_HeaderActive] = kGreenActive;
  c[ImGuiCol_CheckMark] = kGreen;
  c[ImGuiCol_SliderGrab] = kGreen;
  c[ImGuiCol_SliderGrabActive] = kGreenHover;
  c[ImGuiCol_PlotHistogram] = kGreen;
  c[ImGuiCol_Separator] = ImVec4(0.55f, 0.76f, 0.08f, 0.25f);
  st.ScaleAllSizes(g_scale);
  st.FontScaleDpi = g_scale;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------
void DrawBackground(ImDrawList* dl, ImVec2 size, float t) {
  dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(8, 12, 6, 255));
  const Texture& bg = g_app.background;
  if (bg.srv) {
    // Cover the window and pan slowly across the (very wide) art, so both pterosaurs drift by.
    float img_aspect = float(bg.w) / float(bg.h), view_aspect = size.x / size.y;
    float uw = std::min(1.0f, view_aspect / img_aspect);
    float phase = 0.5f - 0.5f * std::cos(t * 2.0f * 3.14159265f / 90.0f);  // 90 s round trip
    float u0 = (1.0f - uw) * phase;
    dl->AddImage(bg.ref(), ImVec2(0, 0), size, ImVec2(u0, 0), ImVec2(u0 + uw, 1));
  }
  // Darken toward the bottom so the menu reads well, and vignette the sides.
  const ImU32 clear = IM_COL32(0, 0, 0, 0), dark = IM_COL32(0, 0, 0, 170);
  dl->AddRectFilledMultiColor(ImVec2(0, size.y * 0.35f), size, clear, clear, dark, dark);
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x * 0.18f, size.y), IM_COL32(0, 0, 0, 110),
                              clear, clear, IM_COL32(0, 0, 0, 110));
  dl->AddRectFilledMultiColor(ImVec2(size.x * 0.82f, 0), size, clear, IM_COL32(0, 0, 0, 110),
                              IM_COL32(0, 0, 0, 110), clear);
}

void Hint(const char* fmt, ...) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  va_list args;
  va_start(args, fmt);
  ImGui::TextWrappedV(fmt, args);
  va_end(args);
  ImGui::PopStyleColor();
}

void Label(const char* text) {
  ImGui::PushFont(g_app.heading, 20.0f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
}

bool TabButton(const char* label, bool active, float width) {
  ImGui::PushFont(g_app.heading, 21.0f);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));
  ImGui::PushStyleColor(ImGuiCol_Text, active ? kGreenHover : kMuted);
  bool pressed = ImGui::Button(label, ImVec2(width, S(44)));
  ImGui::PopStyleColor(4);
  ImGui::PopFont();
  if (active) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + S(18), b.y - S(3)),
                                              ImVec2(b.x - S(18), b.y),
                                              ImGui::GetColorU32(kOrange), S(2));
  }
  return pressed;
}

void DrawGraphicsTab() {
  auto& s = g_app.settings;
  const float col2 = S(240), w2 = S(600);
  auto [dw, dh] = DisplaySize();
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(12, 5));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, S(10, 6));
  auto hint_line = [&](const char* fmt, auto... args) {
    ImGui::SetCursorPosX(col2);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(4));
    Hint(fmt, args...);
  };

  // Internal resolution and anti-aliasing share a row.
  Label("Internal resolution");
  ImGui::SameLine(col2);
  const float res_w = S(320);
  ImGui::SetNextItemWidth(res_w);
  if (ImGui::Combo("##res", &s.res, kResLabels, 4)) g_app.SaveSettings();
  ImGui::SameLine(col2 + res_w + S(22));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Anti-aliasing");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(col2 + w2 - ImGui::GetCursorPosX());
  if (ImGui::Combo("##aa", &s.aa, kAaLabels, kAaCount)) g_app.SaveSettings();
  int internal_h = kResHeights[s.res];
  if (s.res == 0) internal_h = dh >= 2160 ? 2160 : dh >= 1440 ? 1440 : 720;
  hint_line("3D scene drawn at %d x %d (display %d x %d). RTX 3080: 1440p ~105 fps, 4K ~55 fps.",
            internal_h * 16 / 9, internal_h, dw, dh);
  if (s.aa != 0)
    hint_line("FXAA smooths jagged edges at almost no cost (~0.4 ms per frame at 4K). Also in F2.");
  else if (internal_h > dh)
    hint_line("Rendering above your display already smooths edges (supersampling).");

  Label("Upscaling");
  ImGui::SameLine(col2);
  ImGui::SetNextItemWidth(w2);
  if (ImGui::Combo("##ups", &s.upscaler, kUpsLabels, kUpsCount)) g_app.SaveSettings();
  if (s.upscaler == 2)
    hint_line(internal_h > dh ? "Plain bilinear scaling (softer when scaling down; FSR is sharper)."
                              : "Plain bilinear scaling.");
  else if (s.upscaler == 1)
    hint_line("Sharpens the final image at any resolution.");
  else if (internal_h < dh)
    hint_line("FSR sharpens and upscales %d x %d to %d x %d.", internal_h * 16 / 9, internal_h,
              dw, dh);
  else if (internal_h > dh)
    hint_line("Rendered above your display's resolution and scaled down to it: sharpest image.");
  else
    hint_line("Nothing to upscale: already rendering at your display's resolution.");

  Label("Field of view");
  ImGui::SameLine(col2);
  ImGui::SetNextItemWidth(w2 - S(110));
  if (ImGui::SliderInt("##fov", &s.fov, 60, 120, "%d\xC2\xB0")) g_app.SaveSettings();
  ImGui::SameLine();
  if (ImGui::Button("Default", ImVec2(S(100), 0))) {
    s.fov = 86;
    g_app.SaveSettings();
  }
  hint_line("Game default 86\xC2\xB0. Also adjustable in game with F2.");

  // Frame rate, with Vsync and the FPS counter on the same row.
  Label("Frame rate");
  ImGui::SameLine(col2);
  ImGui::SetNextItemWidth(res_w);
  if (ImGui::Combo("##fps", &s.fps, kFpsLabels, kFpsCount)) g_app.SaveSettings();
  ImGui::SameLine(col2 + res_w + S(22));
  if (ImGui::Checkbox("Vsync", &s.vsync)) g_app.SaveSettings();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", "On: no tearing; frames wait for your display's refresh.\nOff: lowest "
                            "input delay; fast motion may tear.");
  ImGui::SameLine(0, S(28));
  if (ImGui::Checkbox("Show FPS", &s.show_fps)) g_app.SaveSettings();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The game's frame rate in the top left corner. Also in game with F2.");
  ImGui::SameLine(0, S(28));
  if (ImGui::Checkbox("Hide HUD", &s.hide_hud)) g_app.SaveSettings();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", "Just the game: no crosshair, ammo, messages, subtitles or button "
                            "prompts.\nAlso in game with F2.");
  const int hz = DisplayRefreshHz();
  if (s.fps == 0)
    hint_line("The game's original frame rate.");
  else if (kFpsValues[s.fps] > hz && s.vsync)
    hint_line("Vsync caps it at your display's %d Hz.", hz);
  else if (kFpsValues[s.fps] > hz)
    hint_line("Above your display's %d Hz: extra frames lower input delay but aren't all shown.",
              hz);
  else
    hint_line("Game speed stays correct at any frame rate. Your display: %d Hz.", hz);

  // Display mode, with the window size (windowed only) on the same row.
  Label("Display mode");
  ImGui::SameLine(col2);
  ImGui::SetNextItemWidth(res_w);
  if (ImGui::Combo("##display", &s.display, kDisplayLabels, kDisplayCount)) g_app.SaveSettings();
  ImGui::SameLine(col2 + res_w + S(22));
  ImGui::AlignTextToFramePadding();
  ImGui::BeginDisabled(s.display != 2);
  ImGui::TextUnformatted("Window size");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(col2 + w2 - ImGui::GetCursorPosX());
  if (ImGui::Combo("##winsize", &s.win_size, kWinSizeLabels, kWinSizeCount)) g_app.SaveSettings();
  ImGui::EndDisabled();
  if (s.display == 0)
    hint_line("Fills your screen; switching to other apps is instant. Also in game with F2.");
  else if (s.display == 1)
    hint_line("Takes over the display; switching to other apps is slower. Also in game with F2.");
  else
    hint_line("A window you can move around, at the size picked. Also in game with F2.");
  ImGui::PopStyleVar(2);
}

void Cheat(const char* label, bool* v, const char* desc, bool experimental) {
  ImGui::PushFont(g_app.heading, 20.0f);
  if (ImGui::Checkbox(label, v)) g_app.SaveSettings();
  ImGui::PopFont();
  ImGui::SameLine(S(280));
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextUnformatted(desc);
  ImGui::PopStyleColor();
  if (experimental) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextUnformatted("(experimental)");
    ImGui::PopStyleColor();
  }
}


// ---------------------------------------------------------------------------------------------
// Controls tab
// ---------------------------------------------------------------------------------------------
// Button glyphs are drawn in code unless an image is embedded under the glyph's id as an RCDATA
// resource (launcher.rc), e.g. xbox_a, ps_cross, key_space: then that image is used instead.
const Texture* GlyphImage(const std::string& id) {
  static std::map<std::string, Texture> cache;
  auto it = cache.find(id);
  if (it == cache.end()) {
    std::wstring name(id.begin(), id.end());
    for (auto& ch : name) ch = wchar_t(towupper(ch));
    Texture t;
    if (FindResourceW(nullptr, name.c_str(), RT_RCDATA)) t = LoadTexture(name.c_str(), {});
    it = cache.emplace(id, std::move(t)).first;
  }
  return it->second.srv ? &it->second : nullptr;
}

ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

void CenteredText(ImDrawList* dl, ImVec2 c, float size, ImU32 col, const char* text,
                  ImFont* font = nullptr) {
  font = font ? font : g_app.heading;
  ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, text);
}

// A rounded "pill" with a label (shoulder buttons, triggers, small system buttons).
float Pill(ImDrawList* dl, ImVec2 p, float h, const char* text, ImU32 fill, ImU32 ink,
           float min_w = 0) {
  float size = h * 0.55f;
  float w = std::max(min_w, g_app.heading->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x + h * 0.6f);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), fill, h * 0.3f);
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), Rgb(255, 255, 255, 40), h * 0.3f);
  CenteredText(dl, ImVec2(p.x + w * 0.5f, p.y + h * 0.5f), size, ink, text);
  return w;
}

// Draws one controller glyph at p with height h; returns its width.
float PadGlyph(ImDrawList* dl, ImVec2 p, float h, const std::string& id) {
  if (const Texture* img = GlyphImage(id)) {
    float w = h * float(img->w) / float(img->h);
    dl->AddImage(img->ref(), p, ImVec2(p.x + w, p.y + h));
    // The small system buttons are just shapes; name them.
    static const std::pair<const char*, const char*> kNames[] = {
        {"xbox_back", "Back"}, {"xbox_start", "Start"}, {"ps_share", "Share"},
        {"ps_options", "Options"}, {"ps_select", "Select"}, {"ps_start", "Start"}};
    for (auto& [gid, name] : kNames) {
      if (id != gid) continue;
      const float size = h * 0.62f;
      dl->AddText(g_app.heading, size, ImVec2(p.x + w + h * 0.2f, p.y + (h - size) * 0.5f),
                  ImGui::GetColorU32(kText), name);
      w += h * 0.2f + g_app.heading->CalcTextSizeA(size, FLT_MAX, 0.0f, name).x;
    }
    return w;
  }
  const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
  const float r = h * 0.5f;
  const ImU32 dark = Rgb(38, 42, 36), white = Rgb(245, 245, 240);
  auto face = [&](ImU32 fill, ImU32 ink, const char* t) {
    dl->AddCircleFilled(c, r, fill);
    CenteredText(dl, c, h * 0.62f, ink, t);
    return h;
  };
  auto ring = [&](ImU32 ink, const char* t, bool pressed) {
    dl->AddCircleFilled(c, r, dark);
    dl->AddCircle(c, r - 1, pressed ? Rgb(170, 200, 90) : Rgb(150, 155, 145), 0, h * 0.09f);
    CenteredText(dl, ImVec2(c.x, c.y - (pressed ? h * 0.08f : 0)),
                 h * (std::strlen(t) > 1 ? 0.4f : 0.5f), ink, t);
    if (pressed)  // "click": a small down chevron
      dl->AddTriangleFilled(ImVec2(c.x - h * 0.12f, c.y + h * 0.2f),
                            ImVec2(c.x + h * 0.12f, c.y + h * 0.2f), ImVec2(c.x, c.y + h * 0.33f),
                            Rgb(170, 200, 90));
    return h;
  };
  auto dpad = [&]() {
    const float a = h * 0.16f, b = h * 0.46f;
    dl->AddRectFilled(ImVec2(c.x - a, c.y - b), ImVec2(c.x + a, c.y + b), Rgb(200, 205, 195), 2);
    dl->AddRectFilled(ImVec2(c.x - b, c.y - a), ImVec2(c.x + b, c.y + a), Rgb(200, 205, 195), 2);
    return h;
  };
  // Xbox
  if (id == "xbox_a") return face(Rgb(70, 180, 60), white, "A");
  if (id == "xbox_b") return face(Rgb(220, 60, 55), white, "B");
  if (id == "xbox_x") return face(Rgb(40, 130, 225), white, "X");
  if (id == "xbox_y") return face(Rgb(240, 195, 20), Rgb(30, 30, 30), "Y");
  if (id == "xbox_lb") return Pill(dl, p, h, "LB", dark, white, h * 1.5f);
  if (id == "xbox_rb") return Pill(dl, p, h, "RB", dark, white, h * 1.5f);
  if (id == "xbox_lt") return Pill(dl, p, h, "LT", dark, white, h * 1.5f);
  if (id == "xbox_rt") return Pill(dl, p, h, "RT", dark, white, h * 1.5f);
  if (id == "xbox_ls") return ring(white, "L", false);
  if (id == "xbox_rs") return ring(white, "R", false);
  if (id == "xbox_ls_click") return ring(white, "L", true);
  if (id == "xbox_rs_click") return ring(white, "R", true);
  if (id == "xbox_dpad_up" || id == "ps_dpad_up") return dpad();
  if (id == "xbox_back") return Pill(dl, p, h, "Back", dark, white);
  if (id == "xbox_start") return Pill(dl, p, h, "Start", dark, white);
  // PlayStation
  if (id == "ps_cross" || id == "ps_circle" || id == "ps_square" || id == "ps_triangle") {
    dl->AddCircleFilled(c, r, dark);
    const float k = h * 0.24f, t = h * 0.09f;
    if (id == "ps_cross") {
      dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), Rgb(125, 180, 235), t);
      dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), Rgb(125, 180, 235), t);
    } else if (id == "ps_circle") {
      dl->AddCircle(c, k * 1.05f, Rgb(240, 105, 105), 0, t);
    } else if (id == "ps_square") {
      dl->AddRect(ImVec2(c.x - k * 0.95f, c.y - k * 0.95f), ImVec2(c.x + k * 0.95f, c.y + k * 0.95f),
                  Rgb(225, 150, 200), 0, 0, t);
    } else {
      dl->AddTriangle(ImVec2(c.x, c.y - k * 1.1f), ImVec2(c.x + k * 1.1f, c.y + k * 0.8f),
                      ImVec2(c.x - k * 1.1f, c.y + k * 0.8f), Rgb(65, 210, 165), t);
    }
    return h;
  }
  if (id == "ps_l1") return Pill(dl, p, h, "L1", dark, white, h * 1.5f);
  if (id == "ps_r1") return Pill(dl, p, h, "R1", dark, white, h * 1.5f);
  if (id == "ps_l2") return Pill(dl, p, h, "L2", dark, white, h * 1.5f);
  if (id == "ps_r2") return Pill(dl, p, h, "R2", dark, white, h * 1.5f);
  if (id == "ps_ls") return ring(white, "L", false);
  if (id == "ps_rs") return ring(white, "R", false);
  if (id == "ps_l3") return ring(white, "L3", true);
  if (id == "ps_r3") return ring(white, "R3", true);
  if (id == "ps_share") return Pill(dl, p, h, "Share", dark, white);
  if (id == "ps_options") return Pill(dl, p, h, "Options", dark, white);
  return 0;
}

std::string KeyLabel(std::string k) {
  static const std::pair<const char*, const char*> kNames[] = {
      {"LMB", "Left click"}, {"RMB", "Right click"}, {"MMB", "Middle click"},
      {"Mouse4", "Mouse 4"}, {"Mouse5", "Mouse 5"},  {"LShift", "L Shift"},
      {"RShift", "R Shift"}, {"LCtrl", "L Ctrl"},    {"RCtrl", "R Ctrl"},
      {"LAlt", "L Alt"},     {"RAlt", "R Alt"},      {"Escape", "Esc"},
      {"Return", "Enter"},
  };
  for (auto& [from, to] : kNames)
    if (_stricmp(k.c_str(), from) == 0) return to;
  return k;
}

// Draws keycaps for a binding like "C,LCtrl" ("C" / "L Ctrl"); returns the width used.
float KeyCaps(ImDrawList* dl, ImVec2 p, float h, const std::string& spec) {
  float x = p.x;
  size_t start = 0;
  bool first = true;
  while (start <= spec.size()) {
    size_t comma = spec.find(',', start);
    std::string key = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    key.erase(0, key.find_first_not_of(' '));
    key.erase(key.find_last_not_of(' ') + 1);
    if (!key.empty()) {
      if (!first) {
        CenteredText(dl, ImVec2(x + h * 0.3f, p.y + h * 0.5f), h * 0.6f, Rgb(160, 170, 150), "/");
        x += h * 0.6f;
      }
      first = false;
      std::string lower = key;
      for (auto& ch : lower) ch = char(tolower((unsigned char)ch));
      if (const Texture* img = GlyphImage("key_" + lower)) {
        float w = h * float(img->w) / float(img->h);
        dl->AddImage(img->ref(), ImVec2(x, p.y), ImVec2(x + w, p.y + h));
        x += w;
      } else {
        x += Pill(dl, ImVec2(x, p.y), h, KeyLabel(key).c_str(), Rgb(58, 64, 52), Rgb(240, 240, 232),
                  h);
      }
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return x - p.x;
}

std::string Bind(const char* name, const char* def) {
  if (auto it = g_app.binds.find(name); it != g_app.binds.end() && !it->second.empty())
    return it->second;
  return def;
}

void DrawControlsTab() {
  struct Row {
    const char* action;
    std::string pc;
    const char *xbox, *ps;
  };
  const std::string move = Bind("move_forward", "W") + "," + Bind("move_left", "A") + "," +
                           Bind("move_back", "S") + "," + Bind("move_right", "D");
  const Row rows[] = {
      {"Move", move, "xbox_ls", "ps_ls"},
      {"Look", "Mouse", "xbox_rs", "ps_rs"},
      {"Fire", Bind("fire", "LMB"), "xbox_rt", "ps_r2"},
      {"Aim (hold)", Bind("aim", "RMB"), "xbox_lt", "ps_l2"},
      {"Jump", Bind("jump", "Space"), "xbox_a", "ps_cross"},
      {"Crouch", Bind("crouch", "C,LCtrl"), "xbox_b", "ps_circle"},
      {"Reload", Bind("reload", "R"), "xbox_x", "ps_square"},
      {"Switch weapon", Bind("change_weapon", "Q") + ",Wheel", "xbox_y", "ps_triangle"},
      {"Throw grenade", Bind("grenade", "G"), "xbox_rb", "ps_r1"},
      {"Adrenaline", Bind("adrenaline", "F"), "xbox_lb", "ps_l1"},
      {"Sprint", Bind("sprint", "LShift"), "xbox_ls_click", "ps_l3"},
      {"Melee", Bind("melee", "V,Mouse4"), "xbox_rs_click", "ps_r3"},
      {"Interact", Bind("interact", "E"), "xbox_dpad_up", "ps_dpad_up"},
      {"Objectives", Bind("objectives", "Tab"), "xbox_back", "ps_share"},
      {"Pause / skip", Bind("pause", "Escape"), "xbox_start", "ps_options"},
      {"PC options", "F2", "", ""},
  };
  constexpr int kPerColumn = 8;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float total_w = ImGui::GetContentRegionAvail().x;
  const float half = total_w * 0.5f, gap = S(14);
  const float row_h = S(25), glyph_h = S(22), head_h = S(20);
  // Column offsets inside each half.
  const float col_pc = S(122), col_xb = half - S(168), col_ps = half - S(98);
  const ImU32 muted = ImGui::GetColorU32(kMuted), text = ImGui::GetColorU32(kText);
  const ImU32 chosen = ImGui::GetColorU32(kGreenHover);  // the set the game shows
  auto& s = g_app.settings;

  for (int side = 0; side < 2; ++side) {
    const float x0 = o.x + side * (half + gap * 0.5f);
    dl->AddText(g_app.heading, S(14), ImVec2(x0 + col_pc, o.y), s.prompts == 2 ? chosen : muted,
                "KEYBOARD & MOUSE");
    dl->AddText(g_app.heading, S(14), ImVec2(x0 + col_xb, o.y), s.prompts == 0 ? chosen : muted,
                "XBOX");
    dl->AddText(g_app.heading, S(14), ImVec2(x0 + col_ps, o.y), s.prompts == 1 ? chosen : muted,
                "PLAYSTATION");
  }
  dl->AddLine(ImVec2(o.x + half + gap * 0.25f - S(6), o.y + S(4)),
              ImVec2(o.x + half + gap * 0.25f - S(6), o.y + head_h + kPerColumn * row_h),
              Rgb(140, 190, 30, 60), 1.0f);
  for (int i = 0; i < int(std::size(rows)); ++i) {
    const Row& r = rows[i];
    const float x0 = o.x + (i / kPerColumn) * (half + gap * 0.5f);
    const float y = o.y + head_h + (i % kPerColumn) * row_h;
    if (i % kPerColumn)
      dl->AddLine(ImVec2(x0, y), ImVec2(x0 + half - gap, y), Rgb(255, 255, 255, 14));
    const float gy = y + (row_h - glyph_h) * 0.5f;
    dl->AddText(g_app.heading, S(17), ImVec2(x0, y + (row_h - S(17)) * 0.5f), text, r.action);
    KeyCaps(dl, ImVec2(x0 + col_pc, gy), glyph_h, r.pc);
    if (*r.xbox) PadGlyph(dl, ImVec2(x0 + col_xb, gy), glyph_h, r.xbox);
    if (*r.ps) PadGlyph(dl, ImVec2(x0 + col_ps, gy), glyph_h, r.ps);
  }
  ImGui::Dummy(ImVec2(total_w, head_h + kPerColumn * row_h));

  // Which icons the game itself shows in its button prompts ("A Accept", tutorials, HUD).
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, S(12, 4));
  Label("In-game button icons");
  ImGui::SameLine(S(240));
  ImGui::SetNextItemWidth(S(200));
  if (ImGui::Combo("##prompts", &s.prompts, kPromptLabels, kPromptCount)) g_app.SaveSettings();
  // (Keyboard & mouse shows the keys bound above; the mouse also works in menus.)
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Menu and tutorial prompts. Applies when the game starts.");

  // Weapon wheel and toggle sprint on the same row.
  ImGui::SameLine(S(240) + S(200) + S(30));
  if (ImGui::Checkbox("Weapon wheel", &s.weapon_wheel)) g_app.SaveSettings();
  const std::string cw = Bind("change_weapon", "Q");
  const std::string key = KeyLabel(cw.substr(0, cw.find(',')));
  if (ImGui::IsItemHovered()) {
    const std::string tip =
        s.weapon_wheel ? "Hold " + key + " and point with the mouse, or hold Y / triangle and point "
                         "with the left stick.\nA tap still switches to the next weapon; 1-9 pick "
                         "a weapon."
                       : "Off: " + key + " / Y / triangle switch to the next weapon.";
    ImGui::SetTooltip("%s", tip.c_str());
  }
  ImGui::SameLine(0, S(28));
  if (ImGui::Checkbox("Toggle sprint", &s.toggle_sprint)) g_app.SaveSettings();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", "Press sprint (left stick click or Shift) once instead of holding it.\n"
                            "It stops when you stop or turn off course, shoot, aim, reload or "
                            "crouch,\nor press it again.");
  ImGui::PopStyleVar();
}

void DrawCheatsTab() {
  auto& s = g_app.settings;
  // Slightly tighter rows so all six cheats and the hint fit without scrolling.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, S(4)));
  Cheat("God mode", &s.god, "You can't be hurt.", false);
  Cheat("Infinite ammo", &s.ammo, "The magazine never runs out; no reloading.", false);
  Cheat("Infinite adrenaline", &s.adren, "The slow-motion meter stays full.", false);
  Cheat("Infinite grenades", &s.grenades, "Grenades are topped back up to 5 after each throw.",
        false);
  Cheat("One hit kill", &s.one_hit, "Anything you hurt dies from one hit.", false);
  // Super speed: a choice of multipliers rather than on / off. The name lines up with the
  // checkbox labels above; the choice sits between it and the description column.
  ImGui::PushFont(g_app.heading, 20.0f);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFrameHeight() +
                       ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Super speed");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(280) - ImGui::GetCursorPosX() - S(8));
  if (ImGui::Combo("##super_speed", &s.speed, kSpeedLabels, kSpeedCount)) g_app.SaveSettings();
  ImGui::PopFont();
  ImGui::SameLine(S(280));
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextUnformatted("Walk, run and sprint faster.");
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  Hint("Cheats can also be switched on and off in game with F2.");
}

std::string Gb(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
  return buf;
}

void StatusDot(const ImVec4& col) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + S(6), p.y + h * 0.5f + S(1)), S(5),
                                              ImGui::GetColorU32(col));
  ImGui::Dummy(ImVec2(S(16), h));
  ImGui::SameLine();
}

void DrawIsoTab() {
  auto snap = g_app.installer.Get();
  Hint("The game's code is already converted to PC. This copies the levels, sounds and videos "
       "(about 3 GB) out of your Xbox 360 disc image and checks it's the exact USA release.");
  ImGui::Spacing();

  if (snap.running) {
    ImGui::PushFont(g_app.heading, 22.0f);
    ImGui::TextUnformatted(snap.message.c_str());
    ImGui::PopFont();
    float frac = snap.total_bytes ? float(double(snap.done_bytes) / double(snap.total_bytes)) : 0;
    char overlay[96];
    std::snprintf(overlay, sizeof(overlay), "%s / %s   (%.0f MB/s)", Gb(snap.done_bytes).c_str(),
                  Gb(snap.total_bytes).c_str(), snap.mb_per_s);
    ImGui::ProgressBar(frac, ImVec2(-1, S(30)), overlay);
    Hint("%zu / %zu files   %s", snap.files_done, snap.files_total, snap.current_file.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(S(140), 0))) g_app.installer.Cancel();
    return;
  }

  if (snap.finished && !snap.ok) {
    ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
    ImGui::TextWrapped("%s", snap.message.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }

  // Current game data.
  ImGui::PushFont(g_app.heading, 21.0f);
  if (g_app.game.found && g_app.game.exact_version) {
    StatusDot(kGreen);
    ImGui::TextUnformatted("Game data installed and verified");
  } else if (g_app.game.found) {
    StatusDot(kOrange);
    ImGui::TextUnformatted("Game data found, but it's not the USA release this build needs");
  } else {
    StatusDot(kOrange);
    ImGui::TextUnformatted("No game data installed yet");
  }
  ImGui::PopFont();
  if (g_app.game.found) Hint("%s", Utf8(g_app.game.dir).c_str());
  ImGui::Spacing();

  if (ImGui::Button(g_app.game.found ? "Reinstall from ISO..." : "Select ISO...",
                    ImVec2(S(230), S(40)))) {
    if (auto p = PickIsoFile(g_app.hwnd)) {
      g_app.picked_iso = *p;
      SetCursor(LoadCursor(nullptr, IDC_WAIT));
      g_app.picked_info = jth::launcher::InspectIso(*p);
    }
  }

  if (!g_app.picked_iso.empty()) {
    ImGui::Spacing();
    const auto& info = g_app.picked_info;
    ImGui::TextWrapped("%s", Utf8(g_app.picked_iso.filename()).c_str());
    if (info.usable()) {
      StatusDot(kGreen);
      ImGui::Text("Verified: Jurassic: The Hunted (Xbox 360, USA)  -  %zu files, %s",
                  info.file_count, Gb(info.data_bytes).c_str());
      Hint("Installs to %s%s", Utf8(g_app.InstallTarget()).c_str(),
           g_app.game.found ? "  (replaces the current game data)" : "");
      ImGui::PushStyleColor(ImGuiCol_Button, kGreen);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGreenHover);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGreenActive);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.07f, 0.02f, 1));
      ImGui::PushFont(g_app.heading, 21.0f);
      if (ImGui::Button("Install", ImVec2(S(160), S(40))))
        g_app.installer.Start(g_app.picked_iso, g_app.InstallTarget());
      ImGui::PopFont();
      ImGui::PopStyleColor(4);
    } else {
      StatusDot(kOrange);
      ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
      ImGui::TextWrapped("%s", info.error.c_str());
      ImGui::PopStyleColor();
    }
  }
}

void LaunchGame() {
  std::wstring cmd = L"\"" + g_app.game_exe.wstring() + L"\" --game_data_root \"" +
                     g_app.game.dir.wstring() + L"\"";
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (CreateProcessW(g_app.game_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                     g_app.exe_dir.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_app.want_quit = true;
  } else {
    MessageBoxW(g_app.hwnd, L"Couldn't start jurassichunted.exe.", L"Jurassic: The Hunted",
                MB_ICONERROR);
  }
}

void DrawUI() {
  ImGuiIO& io = ImGui::GetIO();
  float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_app.start).count();

  // Re-check the game data once an install finishes.
  auto snap = g_app.installer.Get();
  if (g_app.install_was_running && !snap.running) {
    g_app.RefreshGame();
    if (snap.ok) g_app.picked_iso.clear();
  }
  g_app.install_was_running = snap.running;

  DrawBackground(ImGui::GetBackgroundDrawList(), io.DisplaySize, t);

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  // Logo, top middle.
  if (g_app.logo.srv) {
    float w = S(kLogoW), h = w * g_app.logo.h / g_app.logo.w;
    ImGui::GetWindowDrawList()->AddImage(g_app.logo.ref(), ImVec2((io.DisplaySize.x - w) * 0.5f, S(18)),
                                         ImVec2((io.DisplaySize.x + w) * 0.5f, S(18) + h));
  } else {
    // Built without the game's art (launcher/assets isn't in the repository): a plain title.
    const char* title = "JURASSIC: THE HUNTED";
    const float size = S(54);
    const ImVec2 ts = g_app.heading->CalcTextSizeA(size, FLT_MAX, 0.0f, title);
    ImGui::GetWindowDrawList()->AddText(g_app.heading, size,
                                        ImVec2((io.DisplaySize.x - ts.x) * 0.5f, S(70)),
                                        ImGui::GetColorU32(kText), title);
  }

  // Menu panel.
  ImGui::SetCursorPos(S(kPanelX, kPanelY));
  ImGui::BeginChild("##panel", S(kPanelW, kPanelH),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  {
    const float tab_w = (S(kPanelW) - ImGui::GetStyle().WindowPadding.x * 2 - S(30)) / 4;
    if (TabButton("GRAPHICS", g_app.tab == Tab::Graphics, tab_w)) g_app.tab = Tab::Graphics;
    ImGui::SameLine();
    if (TabButton("CONTROLS", g_app.tab == Tab::Controls, tab_w)) g_app.tab = Tab::Controls;
    ImGui::SameLine();
    if (TabButton("CHEATS", g_app.tab == Tab::Cheats, tab_w)) g_app.tab = Tab::Cheats;
    ImGui::SameLine();
    if (TabButton("GAME ISO", g_app.tab == Tab::Iso, tab_w)) g_app.tab = Tab::Iso;
    ImGui::Separator();

    // NavFlattened: keyboard navigation (Tab / arrows) flows between the tabs, settings and PLAY.
    ImGui::BeginChild("##content", ImVec2(0, ImGui::GetContentRegionAvail().y - S(kFooterH)),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);  // scrolls if a tab overflows
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::PushFont(g_app.body, 17.0f);
    switch (g_app.tab) {
      case Tab::Graphics: DrawGraphicsTab(); break;
      case Tab::Controls: DrawControlsTab(); break;
      case Tab::Cheats: DrawCheatsTab(); break;
      case Tab::Iso: DrawIsoTab(); break;
    }
    ImGui::PopFont();
    ImGui::EndChild();

    // Footer: a divider, then the status line and PLAY, centred together in the space below it.
    ImGui::Separator();
    const bool installing = snap.running;
    const bool have_exe = fs::exists(g_app.game_exe);
    const bool can_play = g_app.game.found && g_app.game.exact_version && have_exe && !installing;
    const ImVec2 play_size = S(220, 60);
    const float row_top = ImGui::GetCursorPosY();
    const float play_y = row_top + std::max(0.0f, (ImGui::GetContentRegionAvail().y - play_size.y) * 0.5f);
    const float play_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - play_size.x;
    ImGui::PushFont(g_app.body, 17.0f);
    ImGui::SetCursorPosY(play_y + (play_size.y - ImGui::GetTextLineHeight()) * 0.5f);
    if (!g_app.save_error.empty()) {
      StatusDot(kOrange);
      ImGui::TextUnformatted(g_app.save_error.c_str());
    } else if (can_play) {
      StatusDot(kGreen);
      ImGui::TextUnformatted("Ready to play. Settings are saved automatically.");
    } else if (installing) {
      StatusDot(kOrange);
      ImGui::TextUnformatted("Installing game data...");
    } else if (!have_exe) {
      StatusDot(kOrange);
      ImGui::TextUnformatted("jurassichunted.exe is missing next to the launcher.");
    } else {
      StatusDot(kOrange);
      ImGui::TextUnformatted("Install your game data in the GAME ISO tab to play.");
    }
    ImGui::PopFont();

    ImGui::SetCursorPos(ImVec2(play_x, play_y));
    ImGui::BeginDisabled(!can_play);
    ImGui::PushStyleColor(ImGuiCol_Button, kGreen);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kGreenHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kGreenActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.04f, 0.07f, 0.02f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    ImGui::PushFont(g_app.heading, 30.0f);
    if (ImGui::Button("PLAY", play_size)) LaunchGame();
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
  }
  ImGui::EndChild();
  ImGui::End();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) {
        g_resize_w = LOWORD(lp);
        g_resize_h = HIWORD(lp);
      }
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<RECT*>(lp);
      g_scale = HIWORD(wp) / 96.0f;
      ApplyStyle();
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // no Alt menu
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ImGui_ImplWin32_EnableDpiAwareness();

  // Arguments.
  std::wstring tab_arg, install_iso, game_arg;
  bool prepare = false;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
      std::wstring a = argv[i];
      auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
      if (a == L"--tab") tab_arg = next();
      else if (a == L"--game-dir") g_app.game_dir_override = next();
      else if (a == L"--install-iso") install_iso = next();
      else if (a == L"--prepare") prepare = true;
      else if (a == L"--game") game_arg = next();
    }
    LocalFree(argv);
  }

  // Windowless setup for other launchers (launcher/prepare.h): install and exit, no window.
  if (prepare) return jth::launcher::RunPrepare(game_arg, ExeDir());

  g_app.exe_dir = ExeDir();
  g_app.config_file = g_app.exe_dir / "jurassichunted.toml";
  g_app.game_exe = g_app.exe_dir / "jurassichunted.exe";
  g_app.settings.Load(g_app.config_file);
  g_app.binds = jth::settings::ReadTable(g_app.config_file, "bind");
  g_app.RefreshGame();
  if (tab_arg == L"cheats") g_app.tab = Tab::Cheats;
  else if (tab_arg == L"controls") g_app.tab = Tab::Controls;
  else if (tab_arg == L"iso") g_app.tab = Tab::Iso;
  else if (!g_app.game.found) g_app.tab = Tab::Iso;  // first run: start where it's needed

  // Window: fixed size, scaled for DPI, centered on the primary monitor's work area.
  POINT origin = {0, 0};
  HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  g_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  const RECT work = mi.rcWork;
  g_scale = std::min(g_scale, std::min((work.right - work.left) * 0.95f / kWinW,
                                       (work.bottom - work.top) * 0.95f / kWinH));
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT rc = {0, 0, LONG(kWinW * g_scale), LONG(kWinH * g_scale)};
  AdjustWindowRectEx(&rc, style, FALSE, 0);
  const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;

  WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, nullptr,
                    LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"JthLauncher", nullptr};
  wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXICON),
                                           GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
  wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"Jurassic: The Hunted", style,
                            work.left + (work.right - work.left - ww) / 2,
                            work.top + (work.bottom - work.top - wh) / 2, ww, wh, nullptr,
                            nullptr, inst, nullptr);
  g_app.hwnd = hwnd;
  if (!CreateDevice(hwnd)) {
    MessageBoxW(hwnd, L"Couldn't start Direct3D 11.", L"Jurassic: The Hunted", MB_ICONERROR);
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Tab / arrows / Space / Enter
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_device.Get(), g_ctx.Get());

  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  const fs::path fonts = fs::path(windir) / "Fonts";
  auto add_font = [&](const char* file) -> ImFont* {
    fs::path p = fonts / file;
    return fs::exists(p) ? io.Fonts->AddFontFromFileTTF(Utf8(p).c_str(), 18.0f) : nullptr;
  };
  g_app.body = add_font("segoeui.ttf");
  g_app.heading = add_font("bahnschrift.ttf");
  if (!g_app.body) g_app.body = io.Fonts->AddFontDefault();
  if (!g_app.heading) g_app.heading = g_app.body;
  io.FontDefault = g_app.body;
  ApplyStyle();

  const fs::path assets = g_app.exe_dir / "launcher";  // only used if the resources are missing
  g_app.background = LoadTexture(MAKEINTRESOURCEW(101), assets / "background.jpg");
  g_app.logo = LoadTexture(MAKEINTRESOURCEW(102), assets / "logo.png");

  if (!install_iso.empty()) {
    g_app.tab = Tab::Iso;
    g_app.picked_iso = install_iso;
    g_app.picked_info = jth::launcher::InspectIso(install_iso);
    if (g_app.picked_info.usable()) g_app.installer.Start(install_iso, g_app.InstallTarget());
  }

  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    if (IsIconic(hwnd)) {
      Sleep(16);
      continue;
    }
    if (g_resize_w && g_resize_h) {
      g_rtv.Reset();
      g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
      g_resize_w = g_resize_h = 0;
      CreateRenderTarget();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    const float clear[4] = {0, 0, 0, 1};
    g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
    g_ctx->ClearRenderTargetView(g_rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);

    if (g_app.want_quit) DestroyWindow(hwnd);
  }

  g_app.installer.Cancel();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  CoUninitialize();
  return 0;
}
