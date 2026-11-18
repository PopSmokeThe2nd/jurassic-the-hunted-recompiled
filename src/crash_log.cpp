// Crash reporting. See crash_log.h.

#include "crash_log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/logging/api.h>

REXCVAR_DEFINE_BOOL(debug_crash_test, false, "Debug",
                    "Setting this to true crashes the game on purpose (tests the crash report)");

// Windows last: its macros must not leak into the SDK headers above.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace jth::crash {
namespace {

std::filesystem::path g_logs_dir;
std::mutex g_map_mutex;
char g_map[260] = "(none)";
std::atomic<bool> g_reported{false};

const char* Describe(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_BREAKPOINT: return "breakpoint (guest panic / bug check)";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000409: return "stack buffer overrun / fast fail";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
  }
}

void WriteMinidump(EXCEPTION_POINTERS* info) {
  if (g_logs_dir.empty()) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf(name, 64, L"crash_%04u%02u%02u_%02u%02u%02u.dmp", t.wYear, t.wMonth, t.wDay, t.wHour,
           t.wMinute, t.wSecond);
  const std::filesystem::path path = g_logs_dir / name;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION mei = {};
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = info;
  mei.ClientPointers = FALSE;
  // Threads, stacks and module list; no full heap (the guest memory alone is gigabytes).
  const auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
  const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &mei,
                                    nullptr, nullptr);
  CloseHandle(file);
  if (ok)
    REXLOG_CRITICAL("Crash dump written to {}", path.string());
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
  if (g_reported.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD* rec = info ? info->ExceptionRecord : nullptr;
  const DWORD code = rec ? rec->ExceptionCode : 0;
  void* where = rec ? rec->ExceptionAddress : nullptr;

  char module[MAX_PATH] = "?";
  uintptr_t offset = 0;
  HMODULE mod = nullptr;
  if (where && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCSTR>(where), &mod) &&
      mod) {
    char full[MAX_PATH];
    if (GetModuleFileNameA(mod, full, MAX_PATH)) {
      const char* slash = std::strrchr(full, '\\');
      std::snprintf(module, sizeof(module), "%s", slash ? slash + 1 : full);
    }
    offset = reinterpret_cast<uintptr_t>(where) - reinterpret_cast<uintptr_t>(mod);
  }
  char extra[96] = "";
  if (rec && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
      rec->NumberParameters >= 2) {
    const char* op = rec->ExceptionInformation[0] == 0   ? "reading"
                     : rec->ExceptionInformation[0] == 8 ? "executing"
                                                         : "writing";
    std::snprintf(extra, sizeof(extra), ", %s 0x%llX", op,
                  static_cast<unsigned long long>(rec->ExceptionInformation[1]));
  }
  char map[260];
  {
    std::lock_guard lock(g_map_mutex);
    std::snprintf(map, sizeof(map), "%s", g_map);
  }
  REXLOG_CRITICAL("CRASH: {} (0x{:08X}) at {}+0x{:X}{} on thread {}; map {}", Describe(code), code,
                  module, offset, extra, GetCurrentThreadId(), map);
  rex::FlushLogging();
  WriteMinidump(info);
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Install(const std::filesystem::path& logs_dir) {
  g_logs_dir = logs_dir;
  SetUnhandledExceptionFilter(OnUnhandledException);
  rex::cvar::RegisterChangeCallback("debug_crash_test", [](std::string_view, std::string_view v) {
    if (v == "true" || v == "1") *static_cast<volatile int*>(nullptr) = 0;
  });
}

void SetCurrentMap(const std::string& map) {
  {
    std::lock_guard lock(g_map_mutex);
    std::snprintf(g_map, sizeof(g_map), "%s", map.c_str());
  }
  REXLOG_INFO("Map: {}", map);
}

}  // namespace jth::crash
