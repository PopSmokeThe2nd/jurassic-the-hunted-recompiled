// Developer tool: run Lua code in the game's own script state (test command "lua <code>").
//
// The engine's script manager is the object at [0x8274D6FC]; its lua_State is at +48 (see
// sub_82193800, which runs a script file with luaL_loadbuffer + lua_pcall on that state).
// Queued code runs on the game thread from the GUI update, outside any running script; results
// and errors go to the log ("lua: ..."). For testing levels: trigger script events, inspect state.

#pragma once

#include <cstdint>
#include <string>

#include <rex/ppc/context.h>

namespace jth::devlua {

// Any thread. Runs at the next GUI update.
void Queue(std::string code);

// Game thread (GUI update hook): runs the queued code. Leaves `ctx` as it found it.
void RunPending(PPCContext& ctx, uint8_t* base);

// Game thread, outside any running script (e.g. the GUI update hook): runs `code` right away.
// Returns whether it ran without error. quiet: log nothing (errors still reach the Lua error
// log from luaD_throw). Leaves `ctx` as it found it.
bool RunNow(PPCContext& ctx, uint8_t* base, const std::string& code, bool quiet);

}  // namespace jth::devlua

// Mid-function hook at the start of luaD_throw (r3 = lua_State, r4 = status): logs the error
// message (config/default.toml).
void JthLuaThrow(PPCRegister& r3, PPCRegister& r4);

// Mid-function hook in luaL_loadbuffer (r4 = code, r5 = length, r6 = chunk name): with
// --log_lua_chunks=<text>, logs chunks whose code contains <text> ("*" = every chunk).
void JthLuaLoad(PPCRegister& r4, PPCRegister& r5, PPCRegister& r6);
