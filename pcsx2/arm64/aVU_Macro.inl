// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// ARM64 microVU — COP2 *macro mode* driver (Phase 7.9, STEP 1).
//
// ARM64 counterpart to the macro-op half of pcsx2/x86/microVU_Macro.inl
// (setupMacroOp / endMacroOp / REC_COP2_mVU0). On the EE/R5900 thread, COP2
// (primary opcode 0x12) float ops are executed by *reusing* the VU0 microVU
// op emitters (mVU_ADD, mVU_SUB, …) against a freshly-reset register allocator
// and a one-instruction IR — exactly like the x86 rec's "macro mode".
//
// STEP-1 SCOPE (deliberately minimal, zero-regression):
//   * Only the VADD/VSUB arithmetic family is JIT'd here:
//       VADD, VADDi, VADDx/y/z/w, VADDA, VADDAi, VADDAx/y/z/w
//       VSUB, VSUBi, VSUBx/y/z/w, VSUBA, VSUBAi, VSUBAx/y/z/w
//     (The broadcast (x/y/z/w), I-reg (i) and ACC-accumulator (A) forms are all
//      trivial parameterisations of the same mVU_FMACa emitter, so they come for
//      free.) Every OTHER COP2 op stays on the existing interpreter fallback in
//      aR5900.cpp (recEmitInterpInline) — byte-for-byte unchanged.
//   * The Q-reg forms (VADDq/VSUBq, mode bit 0x1) are NOT included: they need the
//      xmmPQ latency reg loaded from VI[REG_Q], which the EE block hasn't set up.
//      They keep using the interpreter fallback.
//   * Flags ARE emulated to match the interpreter exactly. The VADD/VSUB family
//      updates the MAC flag and the (non-sticky) Status flag (mode 0x10 in x86).
//      We always do the flag update (no vuFlagHack EE-analysis path — the ARM64
//      EE rec has no g_pCurInstInfo, and the interpreter always updates flags).
//
// --------------------------------------------------------------------------------
// The x19 dual-role conflict and how it's resolved
// --------------------------------------------------------------------------------
// EE rec:   x19 = RESTATEPTR = &cpuRegs   (pinned for the whole EE block).
// microVU:  x19 = RVUSTATE   = &vuRegs[index]  (base for all VF/VI/ACC/I access).
//
// In macro mode we run a VU0 op emitter from inside an EE block, so x19 must point
// at &vuRegs[0] for the duration of the macro op, then be restored to &cpuRegs.
//
// This is safe and cheap because, by the time COP2 dispatch reaches here, the EE
// rec has already flushed *and killed* its entire GPR cache (recCacheFlushAll +
// recCacheKillAll in recTranslateOpOptimized, run before recTranslateOp). So no
// guest EE GPR lives in a host register at this point — all EE state is in cpuRegs
// memory, addressed via x19. The macro op only touches:
//   * vuRegs[0] state (VF/VI/ACC/I/flags) — addressed via x19 := &vuRegs[0];
//   * the microVU scratch/flag regs (w9/w10 gprT1/T2, x16/x17 scratch, w23-w26
//     gprF0-3, NEON v0-v24/v29-v31) — none of which alias the EE's pinned
//     x20 (fastmem) / x21 (vtlb). We load gprF0 from the Status flag at setup, so
//     we don't depend on its prior contents.
// Therefore: save x19 -> set x19=&vuRegs[0] -> run macro op (regAlloc reset, emit,
// flush all results back to vuRegs memory) -> restore x19=&cpuRegs. EE state in
// cpuRegs memory is untouched; correctness follows.
//
// The regAlloc is reset before and flushed+reset after each macro op, so nothing
// is cached across the macro-op boundary (matching the x86 "load from memory, op,
// store back" minimal scheme requested for step 1 — here it's the microVU regAlloc
// driven for a single instruction, which naturally loads operands from
// vuRegs[0].VF[] and flushes results back via flushAll()).

namespace {

// x86 setupMacroOp/endMacroOp mode bits we care about for the VADD/VSUB family:
//   0x10 -> updates Status + MAC flags (all VADD/VSUB[A][i/x/y/z/w] use this)
//   0x01 -> reads Q reg     (NOT used by the family we JIT — see scope note)
//   0x02 -> writes Q reg    (NOT used)
//   0x04 -> needs analysis pass (NOT used by the FMACa family)
//   0x08 -> writes CLIP     (NOT used)
static constexpr int MVU_MACRO_FLAGS = 0x10;

// Repoint x19 (RESTATEPTR -> RVUSTATE) to &vuRegs[0] and prime the microVU state
// for a single macro op. Mirrors x86 setupMacroOp(0x10, ...).
static void mVUsetupMacroOp(u32 code)
{
	microVU& mVU = microVU0;

	// Fresh register allocator state — nothing is cached across macro ops.
	mVU.regAlloc->reset();

	mVU.cop2            = 1;
	mVU.prog.IRinfo.curPC = 0;
	mVU.code           = code;
	std::memset(&mVU.prog.IRinfo.info[0], 0, sizeof(mVU.prog.IRinfo.info[0]));

	// Status flag: write/lastWrite both reference flag instance 0 (gprF0); the
	// family updates the non-sticky O/U/S/Z bits. (x86: mode & 0x10 branch.)
	mVU.prog.IRinfo.info[0].sFlag.doFlag      = true;
	mVU.prog.IRinfo.info[0].sFlag.doNonSticky = true;
	mVU.prog.IRinfo.info[0].sFlag.write       = 0;
	mVU.prog.IRinfo.info[0].sFlag.lastWrite   = 0;

	// MAC flag: write instance 0xff (>= 4) => mVUallocMFLAGb stores straight to
	// vuRegs[0].VI[REG_MAC_FLAG] (the macroVU path), matching the interpreter.
	mVU.prog.IRinfo.info[0].mFlag.doFlag = true;
	mVU.prog.IRinfo.info[0].mFlag.write  = 0xff;

	// Point RVUSTATE (x19) at &vuRegs[0] for the macro op. The EE rec's
	// RESTATEPTR(&cpuRegs) is restored in mVUendMacroOp.
	armMoveAddressToReg(RVUSTATE, &::vuRegs[0]);

	// The interpreter keeps vuRegs[0].VI[REG_STATUS_FLAG] *normalized*. The mVU
	// flag pipeline works on a *denormalized* status flag kept in gprF0. Load &
	// denormalize it here (x86 setupMacroOp: mVUallocSFLAGd with the
	// DENORMALIZE_STATUS path, which on this port is unconditional since we always
	// arrive with a normalized in-memory flag).
	mVUallocSFLAGd(&::vuRegs[0].VI[REG_STATUS_FLAG].UL, gprF0, gprT1, gprT2);
}

// Flush the macro op's results back to vuRegs[0] memory, re-normalize the Status
// flag, and restore x19 = &cpuRegs for the rest of the EE block. Mirrors x86
// endMacroOp(0x10, ...).
static void mVUendMacroOp()
{
	microVU& mVU = microVU0;

	// Write back every cached VF/VI/ACC result to vuRegs[0] memory. (x86 used the
	// dropped flushPartialForCOP2; the equivalent here is a full flushAll, which
	// is correct — nothing must remain live across the macro-op boundary.)
	mVU.regAlloc->flushAll();

	// Re-normalize the Status flag from gprF0 back into vuRegs[0] memory, so the
	// interpreter (and the next macro op's denormalize) see the canonical form.
	// (x86 endMacroOp: mVUallocSFLAGc + store, the NORMALIZE_STATUS path.)
	mVUallocSFLAGc(gprT1, gprF0, 0);
	armMoveAddressToReg(RSCRATCHADDR, &::vuRegs[0].VI[REG_STATUS_FLAG].UL);
	armAsm->Str(gprT1, a64::MemOperand(RSCRATCHADDR));

	mVU.cop2 = 0;
	mVU.regAlloc->reset();

	// Restore x19 = &cpuRegs for the remainder of the EE block. (x19 is RVUSTATE
	// here / RESTATEPTR in the EE rec — the same physical register; aR5900.h isn't
	// in this TU, so address it via the RVUSTATE alias.)
	armMoveAddressToReg(RVUSTATE, &cpuRegs);
}

// Drive one FMACa-family macro op: setup, run the emitter's pass-2 (recompile)
// against microVU0, tear down. The VADD/VSUB family is mode 0x10 (no analysis
// pass, bit 0x04 clear), so we only run recPass == 1 — exactly like x86
// REC_COP2_mVU0's `else` branch.
#define MVU_MACRO_FMAC(name, emitter)                 \
	void recCOP2_##name(u32 code)                     \
	{                                                 \
		mVUsetupMacroOp(code);                        \
		emitter(microVU0, /*recPass=*/1);             \
		mVUendMacroOp();                              \
	}

} // anonymous namespace

// VADD family (Upper, SPECIAL1)
MVU_MACRO_FMAC(VADD,   mVU_ADD)
MVU_MACRO_FMAC(VADDi,  mVU_ADDi)
MVU_MACRO_FMAC(VADDx,  mVU_ADDx)
MVU_MACRO_FMAC(VADDy,  mVU_ADDy)
MVU_MACRO_FMAC(VADDz,  mVU_ADDz)
MVU_MACRO_FMAC(VADDw,  mVU_ADDw)
// VADDA accumulator family (Upper, SPECIAL2)
MVU_MACRO_FMAC(VADDA,  mVU_ADDA)
MVU_MACRO_FMAC(VADDAi, mVU_ADDAi)
MVU_MACRO_FMAC(VADDAx, mVU_ADDAx)
MVU_MACRO_FMAC(VADDAy, mVU_ADDAy)
MVU_MACRO_FMAC(VADDAz, mVU_ADDAz)
MVU_MACRO_FMAC(VADDAw, mVU_ADDAw)

// VSUB family (Upper, SPECIAL1)
MVU_MACRO_FMAC(VSUB,   mVU_SUB)
MVU_MACRO_FMAC(VSUBi,  mVU_SUBi)
MVU_MACRO_FMAC(VSUBx,  mVU_SUBx)
MVU_MACRO_FMAC(VSUBy,  mVU_SUBy)
MVU_MACRO_FMAC(VSUBz,  mVU_SUBz)
MVU_MACRO_FMAC(VSUBw,  mVU_SUBw)
// VSUBA accumulator family (Upper, SPECIAL2)
MVU_MACRO_FMAC(VSUBA,  mVU_SUBA)
MVU_MACRO_FMAC(VSUBAi, mVU_SUBAi)
MVU_MACRO_FMAC(VSUBAx, mVU_SUBAx)
MVU_MACRO_FMAC(VSUBAy, mVU_SUBAy)
MVU_MACRO_FMAC(VSUBAz, mVU_SUBAz)
MVU_MACRO_FMAC(VSUBAw, mVU_SUBAw)

#undef MVU_MACRO_FMAC

// EE-rec dispatch: route the SPECIAL1/SPECIAL2 funct of a COP2 op to one of the
// JIT'd macro ops above. Returns true if the op was emitted natively, false if it
// must stay on the interpreter fallback (every op not in the VADD/VSUB family).
//
// COP2 op layout (matches x86 microVU_Macro.inl recCOP2SPECIAL1t/2t indexing):
//   SPECIAL1 (rs==0x10..0x1f i.e. cpuRegs.code bit26-set group): index = funct(0..63)
//   SPECIAL2 (SPECIAL1 funct 0x3c-0x3f): index = (code & 3) | ((code >> 4) & 0x7c)
bool recCOP2_TryMacroVADDSUB(u32 code)
{
	const u32 funct = code & 0x3f;

	// SPECIAL2 escape: SPECIAL1 funct 0x3c..0x3f route to the SPECIAL2 table.
	if (funct >= 0x3c)
	{
		const u32 idx2 = (code & 0x3) | ((code >> 4) & 0x7c);
		switch (idx2)
		{
			// recCOP2SPECIAL2t indices (see x86 table):
			case 0x00: recCOP2_VADDAx(code); return true;
			case 0x01: recCOP2_VADDAy(code); return true;
			case 0x02: recCOP2_VADDAz(code); return true;
			case 0x03: recCOP2_VADDAw(code); return true;
			case 0x04: recCOP2_VSUBAx(code); return true;
			case 0x05: recCOP2_VSUBAy(code); return true;
			case 0x06: recCOP2_VSUBAz(code); return true;
			case 0x07: recCOP2_VSUBAw(code); return true;
			case 0x22: recCOP2_VADDAi(code); return true; // ADDAi
			case 0x26: recCOP2_VSUBAi(code); return true; // SUBAi
			case 0x28: recCOP2_VADDA(code);  return true; // ADDA
			case 0x2c: recCOP2_VSUBA(code);  return true; // SUBA
			default: return false;
		}
	}

	// SPECIAL1 table (recCOP2SPECIAL1t indices = funct directly).
	switch (funct)
	{
		case 0x00: recCOP2_VADDx(code); return true;
		case 0x01: recCOP2_VADDy(code); return true;
		case 0x02: recCOP2_VADDz(code); return true;
		case 0x03: recCOP2_VADDw(code); return true;
		case 0x04: recCOP2_VSUBx(code); return true;
		case 0x05: recCOP2_VSUBy(code); return true;
		case 0x06: recCOP2_VSUBz(code); return true;
		case 0x07: recCOP2_VSUBw(code); return true;
		case 0x22: recCOP2_VADDi(code); return true; // ADDi
		case 0x26: recCOP2_VSUBi(code); return true; // SUBi
		case 0x28: recCOP2_VADD(code);  return true; // ADD
		case 0x2c: recCOP2_VSUB(code);  return true; // SUB
		default: return false;
	}
}
