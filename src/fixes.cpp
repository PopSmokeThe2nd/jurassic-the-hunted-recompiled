// Hand-written replacements for functions the recompiler translates incorrectly.
//
// A recompiled function is a weak alias to __imp__sub_XXXXXXXX; defining a strong symbol with
// the same name (REX_HOOK_RAW) replaces it everywhere. Each replacement here mirrors the original
// PowerPC instruction for instruction, except for the construct the code generator gets wrong.

#include "jurassichunted_pch.h"  // generated: REX_LOAD_*/REX_STORE_*/REX_CALL_INDIRECT_FUNC

#include <rex/hook.h>

// sub_8236F3C8: stores f1 at this+12, then tail-calls a handler from an 8-byte-stride table at
// 0x827264D8 indexed by the byte at this+24 (with r4 <- r5, r5 <- this).
//
//   lbz r11,24(r3) ; lis r10,-32142 ; stfs f1,12(r3) ; mr r4,r5 ; rotlwi r9,r11,3
//   addi r8,r10,25816 ; mr r5,r3 ; lwzx r7,r9,r8 ; mtctr r7 ; bctr
//
// The code generator mistakes the final bctr for a switch jump table and switches on r8 (the
// table's address) instead of performing an indirect call, so every call ended in the
// "switch case out of range" trap (illegal instruction). First hit when loading Survivor mode.
// The SDK's indirect_calls hint is parsed but not used by v0.10.1, hence this override.
REX_HOOK_RAW(sub_8236F3C8) {
  PPCRegister temp{};
  ctx.r11.u64 = REX_LOAD_U8(ctx.r3.u32 + 24);
  ctx.r10.s64 = -2106458112;  // lis r10,-32142 -> 0x82720000
  ctx.fpscr.disableFlushMode();
  temp.f32 = float(ctx.f1.f64);
  REX_STORE_U32(ctx.r3.u32 + 12, temp.u32);
  ctx.r4.u64 = ctx.r5.u64;
  ctx.r9.u64 = __builtin_rotateleft32(ctx.r11.u32, 3);
  ctx.r8.s64 = ctx.r10.s64 + 25816;  // 0x827264D8
  ctx.r5.u64 = ctx.r3.u64;
  ctx.r7.u64 = REX_LOAD_U32(ctx.r9.u32 + ctx.r8.u32);
  ctx.ctr.u64 = ctx.r7.u64;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bctr: tail call, the callee returns to our caller
}
