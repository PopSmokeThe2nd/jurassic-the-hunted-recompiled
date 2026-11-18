// Developer tool: run Lua code in the game's script state. See dev_lua.h.
//
// Lua 5.1 API in the game (found from the base library's registration table: "loadstring" ->
// luaB_loadstring 0x8260A180, "pcall" -> luaB_pcall 0x8260A658, and what those call):
//   luaL_loadbuffer sub_82604C30 (L, text, len, chunkname)   lua_pcall sub_82603CC0 (L, nargs,
//   nresults, errfunc)   lua_gettop sub_82602A68   lua_settop sub_82602A80   lua_tolstring
//   sub_82603048 (L, index, len*)

#include "dev_lua.h"

#include "jurassichunted_funcs.h"  // generated: guest function declarations

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_STRING(log_lua_chunks, "", "Debug",
                      "Log Lua chunks the game compiles whose code contains this text (* = all), "
                      "with their names (developer tool)");

namespace {

constexpr uint32_t kScriptManager = 0x8274D6FC;  // global: the engine's script manager
constexpr uint32_t kStateOffset = 48;            // its lua_State*

std::mutex g_mutex;
std::deque<std::string> g_pending;

uint32_t Be32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::string GuestString(const uint8_t* base, uint32_t p) {
  std::string s;
  if (p < 0x10000) return s;
  for (uint32_t i = 0; i < 2000 && base[p + i]; ++i) s += char(base[p + i]);
  return s;
}

}  // namespace

namespace jth::devlua {

void Queue(std::string code) {
  std::lock_guard lock(g_mutex);
  g_pending.push_back(std::move(code));
}

void RunPending(PPCContext& ctx, uint8_t* base) {
  std::deque<std::string> work;
  {
    std::lock_guard lock(g_mutex);
    if (g_pending.empty()) return;
    work.swap(g_pending);
  }
  for (const std::string& code : work)
    if (!RunNow(ctx, base, code, false)) break;
}

bool RunNow(PPCContext& ctx, uint8_t* base, const std::string& code, bool quiet) {
  const uint32_t manager = Be32(base, kScriptManager);
  const uint32_t L = manager ? Be32(base, manager + kStateOffset) : 0;
  if (!L) {
    if (!quiet) REXLOG_WARN("lua: the game's script state doesn't exist yet");
    return false;
  }
  const PPCContext saved = ctx;
  {
    // Code text and chunk name go on the guest stack below the current frame; the calls below
    // use the space under that.
    static constexpr char kChunk[] = "=dev_lua";
    const uint32_t len = uint32_t(code.size());
    const uint32_t frame = (saved.r1.u32 - len - sizeof(kChunk) - 512) & ~15u;
    const uint32_t text = frame + 256;
    const uint32_t chunk = text + len + 1;
    std::memcpy(base + text, code.data(), len);
    base[text + len] = 0;
    std::memcpy(base + chunk, kChunk, sizeof(kChunk));
    ctx.r1.u64 = frame;

    auto gettop = [&] {
      ctx.r3.u64 = L;
      sub_82602A68(ctx, base);
      return int32_t(ctx.r3.u32);
    };
    auto tostring = [&](int32_t index) {
      ctx.r3.u64 = L;
      ctx.r4.u64 = uint64_t(int64_t(index));
      ctx.r5.u64 = 0;
      sub_82603048(ctx, base);
      return GuestString(base, ctx.r3.u32);
    };

    const int32_t top = gettop();
    ctx.r3.u64 = L;
    ctx.r4.u64 = text;
    ctx.r5.u64 = len;
    ctx.r6.u64 = chunk;
    sub_82604C30(ctx, base);  // luaL_loadbuffer
    int32_t status = int32_t(ctx.r3.u32);
    const bool compiled = status == 0;
    if (compiled) {
      ctx.r3.u64 = L;
      ctx.r4.u64 = 0;
      ctx.r5.u64 = uint64_t(int64_t(-1));  // LUA_MULTRET
      ctx.r6.u64 = 0;
      sub_82603CC0(ctx, base);  // lua_pcall
      status = int32_t(ctx.r3.u32);
    }
    const int32_t now = gettop();
    if (status != 0) {
      if (!quiet)
        REXLOG_WARN("lua: {} error in '{}': {}", compiled ? "runtime" : "syntax", code,
                    tostring(-1));
    } else if (quiet) {
    } else if (now > top) {
      for (int32_t i = top + 1; i <= now; ++i)
        REXLOG_INFO("lua: '{}' -> [{}] {}", code, i - top, tostring(i));
    } else {
      REXLOG_INFO("lua: '{}' ok", code);
    }
    ctx.r3.u64 = L;
    ctx.r4.u64 = uint64_t(int64_t(top));
    sub_82602A80(ctx, base);  // lua_settop: drop results / error
    ctx = saved;
    return status == 0;
  }
}

}  // namespace jth::devlua

void JthLuaLoad(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
  const std::string filter = REXCVAR_GET(log_lua_chunks);
  if (filter.empty()) return;
  const uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  const uint32_t len = r5.u32;
  if (r4.u32 < 0x10000 || len > (64u << 20)) return;
  const std::string_view code(reinterpret_cast<const char*>(base + r4.u32), len);
  if (filter != "*" && code.find(filter) == std::string_view::npos) return;
  // Binary chunks start with "\x1bLua"; text chunks: show the first line.
  std::string first = code.substr(0, 4) == "\x1bLua" ? std::string("(compiled)")
                                                      : std::string(code.substr(0, code.find('\n')));
  if (first.size() > 80) first.resize(80);
  REXLOG_INFO("Lua chunk '{}' ({} bytes): {}", GuestString(base, r6.u32), len, first);
}

// Lua 5.1 layout (32-bit guest): lua_State +8 top (TValue*); TValue = 8-byte value + 4-byte type
// tag (+8), 16 bytes; type 4 = string; TString: +12 length, characters from +16.
void JthLuaThrow(PPCRegister& r3, PPCRegister& r4) {
  static std::atomic<int> logged{0};
  static std::mutex mutex;
  static std::string last;
  constexpr int kMaxLogged = 200;
  if (logged.load(std::memory_order_relaxed) >= kMaxLogged) return;
  uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  const uint32_t L = r3.u32;
  if (L < 0x10000) return;
  const uint32_t top = Be32(base, L + 8);
  std::string message = "(not a string)";
  if (top >= 0x10000 + 16) {
    const uint32_t value = top - 16;
    if (Be32(base, value + 8) == 4 /* LUA_TSTRING */) {
      const uint32_t ts = Be32(base, value);
      const uint32_t len = ts >= 0x10000 ? Be32(base, ts + 12) : 0;
      if (len && len < 4096) message.assign(reinterpret_cast<const char*>(base + ts + 16), len);
    }
  }
  std::lock_guard lock(mutex);
  if (message == last) return;  // the same error every frame would flood the log
  last = message;
  const int n = logged.fetch_add(1, std::memory_order_relaxed) + 1;
  REXLOG_WARN("Lua error (status {}): {}{}", int32_t(r4.u32), message,
              n == kMaxLogged ? " (no more Lua errors logged this session)" : "");
}
