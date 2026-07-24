/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-License-Identifier: GPL-3.0+
 *
 * preserve_most ABI shims for the arm64 vtlb dispatchers.
 *
 * The EE recompiler keeps guest values live in x9-x15 across its vtlb slow
 * paths and emits no spill/reload around the call, so the dispatchers must
 * preserve those registers. clang expresses that with
 * __attribute__((preserve_most)); GCC has no equivalent and silently ignores
 * the attribute, which would corrupt the EE pins at every MMIO access.
 *
 * Each shim below gives the same guarantee without compiler support: save
 * x9-x15, call the dispatcher through a normal AAPCS call, restore, return.
 * LLVM's AArch64 preserve_most is CSR_AArch64_AAPCS plus x9-x15, so saving
 * exactly x9-x15 on top of a normal call reproduces it.
 *
 * x0-x8 and the vector registers are never touched, so this is ABI
 * transparent: vtlb_memRead128 still returns r128 in q0 and vtlb_memWrite128
 * still takes it in q0.
 *
 * This lives in a .S rather than a top-level __asm__ block in the .cpp because
 * symbols defined in file-scope inline asm are invisible to GCC's LTO symbol
 * resolution — with LTO_PCSX2_CORE they resolve to undefined at link time.
 * Assembly TUs are never handed to the LTO plugin, so their symbols are always
 * real. The vtlb_pmt_* targets are the extern "C" thunks in the sibling .cpp.
 *
 * Only assembled when the attribute is unavailable; on clang the emitter calls
 * the dispatchers directly and this file produces nothing.
 */

/* clang is the only compiler implementing preserve_most, so "not clang" is the
 * same condition as !VTLB_HAS_PRESERVE_MOST in vtlb.h. It cannot be spelled the
 * same way here — GCC's .S preprocessor has no __has_attribute — so the sibling
 * .cpp static-asserts that the two conditions still agree. If they ever drift,
 * that #error fires instead of the link silently losing the shims or their
 * targets. */
#if defined(__aarch64__) && !defined(__clang__)

.macro VTLB_PM_SHIM name, target
	.text
	.balign 4
	.globl \name
	.hidden \name
	.type \name, %function
\name:
	stp x9,  x10, [sp, #-64]!
	stp x11, x12, [sp, #16]
	stp x13, x14, [sp, #32]
	stp x15, x30, [sp, #48]
	bl  \target
	ldp x15, x30, [sp, #48]
	ldp x13, x14, [sp, #32]
	ldp x11, x12, [sp, #16]
	ldp x9,  x10, [sp], #64
	ret
	.size \name, .-\name
.endm

	VTLB_PM_SHIM vtlb_pm_memRead8,    vtlb_pmt_memRead8
	VTLB_PM_SHIM vtlb_pm_memRead16,   vtlb_pmt_memRead16
	VTLB_PM_SHIM vtlb_pm_memRead32,   vtlb_pmt_memRead32
	VTLB_PM_SHIM vtlb_pm_memRead64,   vtlb_pmt_memRead64
	VTLB_PM_SHIM vtlb_pm_memRead128,  vtlb_pmt_memRead128
	VTLB_PM_SHIM vtlb_pm_memWrite8,   vtlb_pmt_memWrite8
	VTLB_PM_SHIM vtlb_pm_memWrite16,  vtlb_pmt_memWrite16
	VTLB_PM_SHIM vtlb_pm_memWrite32,  vtlb_pmt_memWrite32
	VTLB_PM_SHIM vtlb_pm_memWrite64,  vtlb_pmt_memWrite64
	VTLB_PM_SHIM vtlb_pm_memWrite128, vtlb_pmt_memWrite128

#endif

/* Not an executable stack. */
#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
