#include "framerate.h"

#include "pc_input.h"  // RunOnUiThread

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

REXCVAR_DEFINE_INT32(framerate, 30, "Graphics",
                     "Frame rate limit: 30 (original), 60, 120, 144, 165, 185, 240")
    .range(30, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);  // live from the F2 panel (SetFramerate)

REXCVAR_DEFINE_BOOL(vertical_sync, false, "Graphics",
                    "Vsync: show each frame on the display's refresh (no tearing; the frame rate "
                    "can't exceed the refresh rate)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(show_fps, false, "Graphics",
                    "Show the game's frame rate in the top left corner")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
std::atomic<float> g_measured_fps{0};  // game frames per second, updated twice a second

// The SDK's guest "vsync" (two 60 Hz vblanks per frame = the 30 fps cap) goes off above 30 and back
// on at 30, unless it was set by hand (settings file or command line). Checked on the first call,
// before this code has changed it.
bool ApplyCap(int fps) {
  static const bool by_hand = rex::cvar::GetFlagSource("vsync") != rex::cvar::Source::kDefault;
  if (by_hand) return rex::cvar::GetFlagByName("vsync") == (fps > 30 ? "false" : "true");
  return rex::cvar::SetFlagByName("vsync", fps > 30 ? "false" : "true");
}
}  // namespace

namespace jth::fps {

void ApplyFramerateSettings() {
  // Vsync is the host swap chain's (SDK patch: present_vsync in d3d12_presenter.cpp). The SDK's
  // own "vsync" setting is unrelated: it's the emulated console's 60 Hz vblank.
  if (rex::cvar::GetFlagSource("present_vsync") == rex::cvar::Source::kDefault) {
    const bool on = REXCVAR_GET(vertical_sync);
    if (rex::cvar::SetFlagByName("present_vsync", on ? "true" : "false"))
      REXLOG_INFO("Vsync: {}", on ? "on" : "off");
    else
      REXLOG_WARN("Vsync: not supported by this SDK build");
  }

  // Live changes (F2 panel, test scripts): lift or restore the 30 fps cap to match. Deferred to the
  // UI thread, outside the setting's own change notification.
  rex::cvar::RegisterChangeCallback("framerate", [](std::string_view, std::string_view value) {
    const int new_fps = std::clamp(std::atoi(std::string(value).c_str()), 30, 240);
    jth::pc::RunOnUiThread([new_fps] {
      if (ApplyCap(new_fps))
        REXLOG_INFO("Frame rate: {} fps", new_fps);
      else
        REXLOG_WARN("Frame rate: couldn't change the 30 fps cap (vsync set by hand)");
    });
  });

  const int fps = REXCVAR_GET(framerate);
  if (fps <= 30) return;  // the game's own pacing
  // Remove the 2-vblank cap; JthFramePace limits the rate instead.
  if (ApplyCap(fps))
    REXLOG_INFO("Frame rate: limited to {} fps", fps);
  else
    REXLOG_WARN("Frame rate: couldn't lift the 30 fps cap (vsync setting unavailable)");
}

void SetFramerate(int fps) {
  // The change notification registered above applies the cap.
  rex::cvar::SetFlagByName("framerate", std::to_string(std::clamp(fps, 30, 240)));
}

float MeasuredFps() { return g_measured_fps.load(std::memory_order_relaxed); }

// ----- FPS counter (show_fps): a small ImGui overlay, top left -----
namespace {
class FpsDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  ~FpsDialog() override;
  using ImGuiDialog::Close;
  // Drawn with each game frame; no extra repaints (they'd make the window present far more often
  // than the game draws).
  bool WantsContinuousRepaint() override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", MeasuredFps());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // About 2.6% of the screen height (37 px at 1440p), on a dark backing.
    const float size = std::max(16.0f, io.DisplaySize.y * 0.026f);
    const ImVec2 at(size * 0.5f, size * 0.35f);
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float pad = size * 0.25f;
    dl->AddRectFilled(ImVec2(at.x - pad, at.y - pad * 0.5f),
                      ImVec2(at.x + ts.x + pad, at.y + ts.y + pad * 0.5f), IM_COL32(0, 0, 0, 140),
                      pad);
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 220), text);
    dl->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 235, 120, 255), text);
  }
};

rex::ui::ImGuiDrawer* g_drawer = nullptr;  // UI thread only
FpsDialog* g_counter = nullptr;

FpsDialog::~FpsDialog() {
  if (g_counter == this) g_counter = nullptr;
}
}  // namespace

void CreateFpsCounter(rex::ui::ImGuiDrawer* drawer) {
  g_drawer = drawer;
  if (REXCVAR_GET(show_fps)) ShowFpsCounter(true);
}

void DestroyFpsCounter() {
  delete g_counter;  // also clears g_counter
  g_drawer = nullptr;
}

void ShowFpsCounter(bool show) {
  rex::cvar::SetFlagByName("show_fps", show ? "true" : "false");
  if (show && !g_counter && g_drawer) {
    g_counter = new FpsDialog(g_drawer);
  } else if (!show && g_counter) {
    g_counter->Close();  // deleted by the SDK after its next draw
    g_counter = nullptr;
  }
}

}  // namespace jth::fps

namespace {

int64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

int64_t QpcFrequency() {
  static const int64_t f = [] {
    LARGE_INTEGER t;
    QueryPerformanceFrequency(&t);
    return t.QuadPart;
  }();
  return f;
}

}  // namespace

// Called on the game's render thread right before every VdSwap (frame present).
void JthFramePace() {
  {
    // Debug aid: the game's real frame rate (overlays such as RTSS count host presents instead).
    static thread_local int64_t window_start = 0, frames = 0;
    const int64_t t = Qpc();
    if (!window_start) window_start = t;
    ++frames;
    static thread_local int64_t counter_start = 0, counter_frames = 0;  // FPS counter
    if (!counter_start) counter_start = t;
    ++counter_frames;
    if (t - counter_start >= QpcFrequency() / 2) {
      g_measured_fps.store(float(double(counter_frames) * QpcFrequency() / (t - counter_start)),
                           std::memory_order_relaxed);
      counter_start = t;
      counter_frames = 0;
    }
    if (t - window_start >= 5 * QpcFrequency()) {
      REXLOG_DEBUG("Frame rate: {:.1f} fps", double(frames) * QpcFrequency() / (t - window_start));
      window_start = t;
      frames = 0;
    }
  }
  const int fps = REXCVAR_GET(framerate);
  if (fps <= 30) return;

  static thread_local HANDLE timer = [] {
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    return t ? t : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }();
  static thread_local int64_t next = 0;

  const int64_t freq = QpcFrequency();
  const int64_t period = freq / fps;
  int64_t now = Qpc();
  // First frame, or we fell far behind (loading, a hitch): start pacing from now rather than
  // rushing out a burst of frames to catch up.
  if (next == 0 || now - next > 4 * period) next = now;

  // Sleep until ~0.4 ms before the deadline with the high-resolution timer, then spin the rest.
  const int64_t spin_ticks = freq * 4 / 10000;
  if (next - now > spin_ticks && timer) {
    LARGE_INTEGER due;
    due.QuadPart = -((next - now - spin_ticks) * 10000000 / freq);  // relative, 100 ns units
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer, INFINITE);
  }
  while ((now = Qpc()) < next) YieldProcessor();

  next += period;
}
