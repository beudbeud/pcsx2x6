// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// preserve_most ABI shims for the arm64 vtlb dispatchers.
//
// The EE recompiler keeps guest values live in x9-x15 across its vtlb slow
// paths and emits no spill/reload around the call, so the dispatchers must
// preserve those registers. clang expresses that with
// __attribute__((preserve_most)); GCC has no equivalent and silently ignores
// the attribute, which would corrupt the EE pins at every MMIO access.
//
// This file provides the same guarantee without compiler support: each shim
// saves x9-x15, calls the real dispatcher through a plain AAPCS call, restores
// them, and returns. Arguments and return values are untouched (x0-x8 and the
// vector registers are never written), so the shim is ABI-transparent to the
// caller — vtlb_memRead128 still returns r128 in q0 and vtlb_memWrite128 still
// takes it in q0.
//
// LLVM's AArch64 preserve_most is CSR_AArch64_AAPCS plus x9-x15, so saving
// exactly x9-x15 on top of a normal call reproduces it.
//
// Only compiled when the attribute is unavailable; on clang the emitter calls
// the dispatchers directly and this file is empty.

#include "vtlb.h"

#if defined(ARCH_ARM64) && !VTLB_HAS_PRESERVE_MOST

// Unmangled entry points for the shims to branch to. Thin wrappers rather than
// naming the C++ template instantiations from asm, which would hard-code
// mangled symbols.
extern "C" {
mem8_t  vtlb_pmt_memRead8(u32 mem)   { return vtlb_memRead<mem8_t>(mem); }
mem16_t vtlb_pmt_memRead16(u32 mem)  { return vtlb_memRead<mem16_t>(mem); }
mem32_t vtlb_pmt_memRead32(u32 mem)  { return vtlb_memRead<mem32_t>(mem); }
mem64_t vtlb_pmt_memRead64(u32 mem)  { return vtlb_memRead<mem64_t>(mem); }
RETURNS_R128 vtlb_pmt_memRead128(u32 mem) { return vtlb_memRead128(mem); }

void vtlb_pmt_memWrite8(u32 mem, mem8_t v)   { vtlb_memWrite<mem8_t>(mem, v); }
void vtlb_pmt_memWrite16(u32 mem, mem16_t v) { vtlb_memWrite<mem16_t>(mem, v); }
void vtlb_pmt_memWrite32(u32 mem, mem32_t v) { vtlb_memWrite<mem32_t>(mem, v); }
void vtlb_pmt_memWrite64(u32 mem, mem64_t v) { vtlb_memWrite<mem64_t>(mem, v); }
void TAKES_R128 vtlb_pmt_memWrite128(u32 mem, r128 v) { vtlb_memWrite128(mem, v); }
}

// 64 bytes keeps sp 16-byte aligned; x30 rides along because the shim itself
// makes a call. A longjmp out of a dispatcher (Cpu->CancelInstruction) skips
// the restores, exactly as it would skip clang's preserve_most epilogue — the
// JIT frame is abandoned in that case, so no live value is lost either way.
#define VTLB_PM_SHIM(name, target) \
	__asm__( \
		".text\n" \
		".balign 4\n" \
		".globl " #name "\n" \
		".hidden " #name "\n" \
		".type " #name ", %function\n" \
		#name ":\n" \
		"	stp x9,  x10, [sp, #-64]!\n" \
		"	stp x11, x12, [sp, #16]\n" \
		"	stp x13, x14, [sp, #32]\n" \
		"	stp x15, x30, [sp, #48]\n" \
		"	bl  " #target "\n" \
		"	ldp x15, x30, [sp, #48]\n" \
		"	ldp x13, x14, [sp, #32]\n" \
		"	ldp x11, x12, [sp, #16]\n" \
		"	ldp x9,  x10, [sp], #64\n" \
		"	ret\n" \
		".size " #name ", .-" #name "\n")

VTLB_PM_SHIM(vtlb_pm_memRead8,    vtlb_pmt_memRead8);
VTLB_PM_SHIM(vtlb_pm_memRead16,   vtlb_pmt_memRead16);
VTLB_PM_SHIM(vtlb_pm_memRead32,   vtlb_pmt_memRead32);
VTLB_PM_SHIM(vtlb_pm_memRead64,   vtlb_pmt_memRead64);
VTLB_PM_SHIM(vtlb_pm_memRead128,  vtlb_pmt_memRead128);
VTLB_PM_SHIM(vtlb_pm_memWrite8,   vtlb_pmt_memWrite8);
VTLB_PM_SHIM(vtlb_pm_memWrite16,  vtlb_pmt_memWrite16);
VTLB_PM_SHIM(vtlb_pm_memWrite32,  vtlb_pmt_memWrite32);
VTLB_PM_SHIM(vtlb_pm_memWrite64,  vtlb_pmt_memWrite64);
VTLB_PM_SHIM(vtlb_pm_memWrite128, vtlb_pmt_memWrite128);

#endif
