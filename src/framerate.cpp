#include "framerate.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

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
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vertical_sync, false, "Graphics",
                    "Vsync: show each frame on the display's refresh (no tearing; the frame rate "
                    "can't exceed the refresh rate)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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

  const int fps = REXCVAR_GET(framerate);
  if (fps <= 30) return;  // the game's own pacing
  // Remove the 2-vblank cap; JthFramePace limits the rate instead.
  bool uncapped = rex::cvar::GetFlagSource("vsync") != rex::cvar::Source::kDefault
                      ? rex::cvar::GetFlagByName("vsync") == "false"
                      : rex::cvar::SetFlagByName("vsync", "false");
  if (uncapped)
    REXLOG_INFO("Frame rate: limited to {} fps", fps);
  else
    REXLOG_WARN("Frame rate: couldn't lift the 30 fps cap (vsync setting unavailable)");
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
