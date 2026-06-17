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
// SCOPE — the float FMAC family (mode 0x110: Status+MAC flags), zero-regression:
//   * JIT'd here (all reuse the single mVUsetupMacroOp/mVUendMacroOp driver):
//       ADD/SUB/MUL/MADD/MSUB  + their i and x/y/z/w broadcast forms,
//       ADDA/SUBA/MULA/MADDA/MSUBA (ACC accumulator, SPECIAL2) + their i/x/y/z/w,
//       OPMULA/OPMSUB (outer-product cross-product helpers).
//     The broadcast (x/y/z/w), I-reg (i) and ACC-accumulator (A) forms are trivial
//     parameterisations of the same FMAC emitters, so they come for free. MADD/MSUB
//     read ACC, which the emitter loads from vuRegs[0].ACC memory (flushed across the
//     macro boundary) — so the load-from-memory/flush-to-memory driver stays correct.
//   * Every OTHER COP2 op stays on the existing interpreter fallback in aR5900.cpp
//     (recEmitInterpInline) — byte-for-byte unchanged. NOT yet ported:
//       - MAX/MINI (mode 0x0, no flags) and the integer ALU ops (IADD/IAND/IOR,
//         mode 0x104) — need a no-flag driver variant;
//       - Q-reg forms (ADDq/MULq/…, DIV/SQRT/RSQRT, mode bit 0x1/0x2) — need the
//         xmmPQ latency reg loaded from VI[REG_Q], which the EE block hasn't set up;
//       - ITOF/FTOI/ABS/CLIP, transfers (CFC2/QMFC2/…), branches (BC2*).
//   * Flags ARE emulated to match the interpreter exactly: the family updates the MAC
//     flag and the (non-sticky) Status flag (mode 0x10 in x86). We always do the flag
//     update (no vuFlagHack EE-analysis path — the ARM64 EE rec has no g_pCurInstInfo,
//     and the interpreter always updates flags).
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

// x86 setupMacroOp/endMacroOp mode bits (mirrors pcsx2/x86/microVU_Macro.inl):
//   0x01 -> reads Q reg      (Q-forms: ADDq/MULq/...; not yet JIT'd)
//   0x02 -> writes Q reg     (DIV/SQRT/RSQRT — next slice)
//   0x04 -> needs analysis pass (run pass 0 then 1; none of our families use it)
//   0x08 -> writes CLIP flag (CLIP)
//   0x10 -> updates Status + MAC flags (the FMAC family)
static constexpr int MVU_MODE_NOFLAG = 0x00;
static constexpr int MVU_MODE_FMAC   = 0x10;
static constexpr int MVU_MODE_CLIP   = 0x08;
static constexpr int MVU_MODE_QDIV   = 0x12; // Status (0x10) + writes Q (0x02): DIV/SQRT/RSQRT
static constexpr int MVU_MODE_QFMAC  = 0x11; // Status (0x10) + reads Q (0x01): ADDq/MULq/MADDq/...

// Repoint x19 (RESTATEPTR -> RVUSTATE) to &vuRegs[0] and prime the microVU state
// for a single macro op. `mode` selects which flag plumbing to emit (see bits above).
// No-flag ops (mode 0) skip all of it — they don't touch any flag instance, so
// leaving gprF0/the flag pipeline untouched matches the interpreter exactly.
static void mVUsetupMacroOp(u32 code, int mode)
{
	microVU& mVU = microVU0;

	// Fresh register allocator state — nothing is cached across macro ops.
	mVU.regAlloc->reset();

	mVU.cop2            = 1;
	mVU.prog.IRinfo.curPC = 0;
	mVU.code           = code;
	std::memset(&mVU.prog.IRinfo.info[0], 0, sizeof(mVU.prog.IRinfo.info[0]));

	if (mode & 0x10)
	{
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
	}

	if (mode & 0x08)
	{
		// CLIP: read the previous clip flag and write the new one. Instance 0xff (>= 4)
		// makes mVUallocCFLAGa/b read/write vuRegs[0].VI[REG_CLIP_FLAG] in memory
		// directly (the macroVU path) — so no driver-side load/store is needed.
		mVU.prog.IRinfo.info[0].cFlag.write     = 0xff;
		mVU.prog.IRinfo.info[0].cFlag.lastWrite = 0xff;
	}

	// Point RVUSTATE (x19) at &vuRegs[0] for the macro op. The EE rec's
	// RESTATEPTR(&cpuRegs) is restored in mVUendMacroOp.
	armMoveAddressToReg(RVUSTATE, &::vuRegs[0]);

	if (mode & 0x01)
	{
		// Q-form FMAC (ADDq/MULq/...) reads the Q register: load vuRegs[0].VI[REG_Q]
		// into mVU_xmmPQ lane 0 (the lane getQreg(readQ=0) broadcasts from). Ldr S
		// zero-extends [127:32], matching x86 setupMacroOp's xMOVSSZX(xmmPQ, VI[REG_Q]).
		armMoveAddressToReg(RSCRATCHADDR, &::vuRegs[0].VI[REG_Q].UL);
		armAsm->Ldr(mVU_xmmPQ.S(), a64::MemOperand(RSCRATCHADDR));
	}

	if (mode & 0x10)
	{
		// The interpreter keeps vuRegs[0].VI[REG_STATUS_FLAG] *normalized*. The mVU
		// flag pipeline works on a *denormalized* status flag kept in gprF0. Load &
		// denormalize it here (x86 setupMacroOp: mVUallocSFLAGd with the
		// DENORMALIZE_STATUS path, which on this port is unconditional since we always
		// arrive with a normalized in-memory flag).
		mVUallocSFLAGd(&::vuRegs[0].VI[REG_STATUS_FLAG].UL, gprF0, gprT1, gprT2);
	}
}

// Flush the macro op's results back to vuRegs[0] memory, re-normalize the Status
// flag (mode 0x10 only), and restore x19 = &cpuRegs for the rest of the EE block.
// Mirrors x86 endMacroOp. (CLIP needs no teardown — its flag went straight to memory.)
static void mVUendMacroOp(u32 code, int mode)
{
	microVU& mVU = microVU0;

	// Write back every cached VF/VI/ACC result to vuRegs[0] memory. (x86 used the
	// dropped flushPartialForCOP2; the equivalent here is a full flushAll, which
	// is correct — nothing must remain live across the macro-op boundary.)
	mVU.regAlloc->flushAll();

	if (mode & 0x02)
	{
		// DIV/SQRT/RSQRT wrote the result into Q-lane 0 of mVU_xmmPQ (writeQreg with
		// instance 0). Store that lane back to vuRegs[0].VI[REG_Q] (x86 endMacroOp:
		// xMOVSS(ptr32[&vu0Regs.VI[REG_Q].UL], xmmPQ)). The div-by-zero/invalid flags
		// were already merged into gprF0 by the emitter's own `if (mVU.cop2)` branch.
		armMoveAddressToReg(RSCRATCHADDR, &::vuRegs[0].VI[REG_Q].UL);
		armAsm->Str(mVU_xmmPQ.S(), a64::MemOperand(RSCRATCHADDR));
	}

	if (mode & 0x10)
	{
		// Re-normalize the Status flag from gprF0 back into vuRegs[0] memory, so the
		// interpreter (and the next macro op's denormalize) see the canonical form.
		// (x86 endMacroOp: mVUallocSFLAGc + store, the NORMALIZE_STATUS path.)
		mVUallocSFLAGc(gprT1, gprF0, 0);
		armMoveAddressToReg(RSCRATCHADDR, &::vuRegs[0].VI[REG_STATUS_FLAG].UL);
		armAsm->Str(gprT1, a64::MemOperand(RSCRATCHADDR));
	}

	mVU.cop2 = 0;
	mVU.regAlloc->reset();

	// Restore x19 = &cpuRegs for the remainder of the EE block. (x19 is RVUSTATE
	// here / RESTATEPTR in the EE rec — the same physical register; aR5900.h isn't
	// in this TU, so address it via the RVUSTATE alias.)
	armMoveAddressToReg(RVUSTATE, &cpuRegs);
}

// Drive one macro op: setup, run the emitter's pass-2 (recompile) against microVU0,
// tear down. None of these families set bit 0x04, so we only run recPass == 1
// (x86 REC_COP2_mVU0's `else` branch).
#define MVU_MACRO_OP(name, emitter, mode)        \
	void recCOP2_##name(u32 code)                \
	{                                            \
		mVUsetupMacroOp(code, (mode));           \
		emitter(microVU0, /*recPass=*/1);        \
		mVUendMacroOp(code, (mode));             \
	}

#define MVU_MACRO_FMAC(name, emitter)   MVU_MACRO_OP(name, emitter, MVU_MODE_FMAC)
#define MVU_MACRO_NOFLAG(name, emitter) MVU_MACRO_OP(name, emitter, MVU_MODE_NOFLAG)
#define MVU_MACRO_CLIP(name, emitter)   MVU_MACRO_OP(name, emitter, MVU_MODE_CLIP)
#define MVU_MACRO_QDIV(name, emitter)   MVU_MACRO_OP(name, emitter, MVU_MODE_QDIV)
#define MVU_MACRO_QFMAC(name, emitter)  MVU_MACRO_OP(name, emitter, MVU_MODE_QFMAC)

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

// VMUL family (Upper, SPECIAL1) — same mode 0x110 (Status+MAC), reuses the driver.
MVU_MACRO_FMAC(VMUL,   mVU_MUL)
MVU_MACRO_FMAC(VMULi,  mVU_MULi)
MVU_MACRO_FMAC(VMULx,  mVU_MULx)
MVU_MACRO_FMAC(VMULy,  mVU_MULy)
MVU_MACRO_FMAC(VMULz,  mVU_MULz)
MVU_MACRO_FMAC(VMULw,  mVU_MULw)
// VMULA accumulator family (Upper, SPECIAL2)
MVU_MACRO_FMAC(VMULA,  mVU_MULA)
MVU_MACRO_FMAC(VMULAi, mVU_MULAi)
MVU_MACRO_FMAC(VMULAx, mVU_MULAx)
MVU_MACRO_FMAC(VMULAy, mVU_MULAy)
MVU_MACRO_FMAC(VMULAz, mVU_MULAz)
MVU_MACRO_FMAC(VMULAw, mVU_MULAw)

// VMADD family (Upper, SPECIAL1) — multiply-add against ACC; the emitter loads ACC
// from vuRegs[0].ACC (flushed to memory across the macro boundary), so the same
// load-from-memory/flush-to-memory driver is correct.
MVU_MACRO_FMAC(VMADD,   mVU_MADD)
MVU_MACRO_FMAC(VMADDi,  mVU_MADDi)
MVU_MACRO_FMAC(VMADDx,  mVU_MADDx)
MVU_MACRO_FMAC(VMADDy,  mVU_MADDy)
MVU_MACRO_FMAC(VMADDz,  mVU_MADDz)
MVU_MACRO_FMAC(VMADDw,  mVU_MADDw)
// VMADDA accumulator family (Upper, SPECIAL2)
MVU_MACRO_FMAC(VMADDA,  mVU_MADDA)
MVU_MACRO_FMAC(VMADDAi, mVU_MADDAi)
MVU_MACRO_FMAC(VMADDAx, mVU_MADDAx)
MVU_MACRO_FMAC(VMADDAy, mVU_MADDAy)
MVU_MACRO_FMAC(VMADDAz, mVU_MADDAz)
MVU_MACRO_FMAC(VMADDAw, mVU_MADDAw)

// VMSUB family (Upper, SPECIAL1) — multiply-subtract against ACC.
MVU_MACRO_FMAC(VMSUB,   mVU_MSUB)
MVU_MACRO_FMAC(VMSUBi,  mVU_MSUBi)
MVU_MACRO_FMAC(VMSUBx,  mVU_MSUBx)
MVU_MACRO_FMAC(VMSUBy,  mVU_MSUBy)
MVU_MACRO_FMAC(VMSUBz,  mVU_MSUBz)
MVU_MACRO_FMAC(VMSUBw,  mVU_MSUBw)
// VMSUBA accumulator family (Upper, SPECIAL2)
MVU_MACRO_FMAC(VMSUBA,  mVU_MSUBA)
MVU_MACRO_FMAC(VMSUBAi, mVU_MSUBAi)
MVU_MACRO_FMAC(VMSUBAx, mVU_MSUBAx)
MVU_MACRO_FMAC(VMSUBAy, mVU_MSUBAy)
MVU_MACRO_FMAC(VMSUBAz, mVU_MSUBAz)
MVU_MACRO_FMAC(VMSUBAw, mVU_MSUBAw)

// Outer-product accumulate (cross-product helpers) — mode 0x110, fixed operand
// fields handled inside the emitter.
MVU_MACRO_FMAC(VOPMSUB, mVU_OPMSUB) // SPECIAL1
MVU_MACRO_FMAC(VOPMULA, mVU_OPMULA) // SPECIAL2

// --- mode 0x0 (no flag side effects) -------------------------------------------
// Integer<->float conversions, the dominant remaining COP2 fallback in 3D scenes
// (fixed-point geometry constantly converts via ITOF/FTOI).
MVU_MACRO_NOFLAG(VITOF0,  mVU_ITOF0)   // SPECIAL2
MVU_MACRO_NOFLAG(VITOF4,  mVU_ITOF4)
MVU_MACRO_NOFLAG(VITOF12, mVU_ITOF12)
MVU_MACRO_NOFLAG(VITOF15, mVU_ITOF15)
MVU_MACRO_NOFLAG(VFTOI0,  mVU_FTOI0)
MVU_MACRO_NOFLAG(VFTOI4,  mVU_FTOI4)
MVU_MACRO_NOFLAG(VFTOI12, mVU_FTOI12)
MVU_MACRO_NOFLAG(VFTOI15, mVU_FTOI15)
MVU_MACRO_NOFLAG(VABS,    mVU_ABS)     // SPECIAL2

// MAX / MINI (clamping) — no flags. SPECIAL1.
MVU_MACRO_NOFLAG(VMAX,    mVU_MAX)
MVU_MACRO_NOFLAG(VMAXi,   mVU_MAXi)
MVU_MACRO_NOFLAG(VMAXx,   mVU_MAXx)
MVU_MACRO_NOFLAG(VMAXy,   mVU_MAXy)
MVU_MACRO_NOFLAG(VMAXz,   mVU_MAXz)
MVU_MACRO_NOFLAG(VMAXw,   mVU_MAXw)
MVU_MACRO_NOFLAG(VMINI,   mVU_MINI)
MVU_MACRO_NOFLAG(VMINIi,  mVU_MINIi)
MVU_MACRO_NOFLAG(VMINIx,  mVU_MINIx)
MVU_MACRO_NOFLAG(VMINIy,  mVU_MINIy)
MVU_MACRO_NOFLAG(VMINIz,  mVU_MINIz)
MVU_MACRO_NOFLAG(VMINIw,  mVU_MINIw)

// MOVE / MR32 (VF<->VF copy / rotate) — no flags. SPECIAL2 (Lower emitters).
MVU_MACRO_NOFLAG(VMOVE,   mVU_MOVE)
MVU_MACRO_NOFLAG(VMR32,   mVU_MR32)

// --- mode 0x08 (writes the CLIP flag) ------------------------------------------
// CLIP feeds frustum/backface culling — heavy in 3D titles. mVU_CLIP reads the
// previous clip flag and writes the new one; with cFlag instance 0xff both go
// straight to vuRegs[0].VI[REG_CLIP_FLAG] (macroVU path), so no teardown is needed.
MVU_MACRO_CLIP(VCLIP, mVU_CLIP) // SPECIAL2

// --- mode 0x12 (Status + writes Q) ---------------------------------------------
// DIV/SQRT/RSQRT: perspective divide + vector normalisation — heavy in 3D. The
// result lands in Q (mVU_xmmPQ lane 0, stored to VI[REG_Q] by the driver), and the
// div-by-zero/invalid status bits are merged into gprF0 by the emitters' own
// `if (mVU.cop2)` branch (cop2 is set by the driver). No analysis pass needed:
// the zeroed IRinfo gives writeQ instance 0 (lane 0), which the driver stores.
MVU_MACRO_QDIV(VDIV,   mVU_DIV)   // SPECIAL2
MVU_MACRO_QDIV(VSQRT,  mVU_SQRT)  // SPECIAL2
MVU_MACRO_QDIV(VRSQRT, mVU_RSQRT) // SPECIAL2

// --- mode 0x11 (Status + reads Q) ----------------------------------------------
// Q-form FMAC: the broadcast operand is the Q register (the result of a prior DIV).
// The driver loads VI[REG_Q] into mVU_xmmPQ lane 0; the emitters read it via
// getQreg(readQ=0). No Q write-back (they only read Q).
MVU_MACRO_QFMAC(VADDq,  mVU_ADDq)   // SPECIAL1
MVU_MACRO_QFMAC(VSUBq,  mVU_SUBq)
MVU_MACRO_QFMAC(VMULq,  mVU_MULq)
MVU_MACRO_QFMAC(VMADDq, mVU_MADDq)
MVU_MACRO_QFMAC(VMSUBq, mVU_MSUBq)
MVU_MACRO_QFMAC(VADDAq,  mVU_ADDAq)  // SPECIAL2 (accumulator)
MVU_MACRO_QFMAC(VSUBAq,  mVU_SUBAq)
MVU_MACRO_QFMAC(VMULAq,  mVU_MULAq)
MVU_MACRO_QFMAC(VMADDAq, mVU_MADDAq)
MVU_MACRO_QFMAC(VMSUBAq, mVU_MSUBAq)

#undef MVU_MACRO_FMAC
#undef MVU_MACRO_NOFLAG
#undef MVU_MACRO_CLIP
#undef MVU_MACRO_QDIV
#undef MVU_MACRO_QFMAC
#undef MVU_MACRO_OP

// EE-rec dispatch: route the SPECIAL1/SPECIAL2 funct of a COP2 op to one of the
// JIT'd macro ops above. Returns true if the op was emitted natively, false if it
// must stay on the interpreter fallback (Q-forms, DIV/SQRT/RSQRT, CLIP, the integer
// ALU ops, CALLMS, and anything else not in the FMAC / no-flag families below).
//
// COP2 op layout (matches x86 microVU_Macro.inl recCOP2SPECIAL1t/2t indexing):
//   SPECIAL1 (rs==0x10..0x1f i.e. cpuRegs.code bit26-set group): index = funct(0..63)
//   SPECIAL2 (SPECIAL1 funct 0x3c-0x3f): index = (code & 3) | ((code >> 4) & 0x7c)
bool recCOP2_TryMacroFMAC(u32 code)
{
	const u32 funct = code & 0x3f;

	// SPECIAL2 escape: SPECIAL1 funct 0x3c..0x3f route to the SPECIAL2 table.
	if (funct >= 0x3c)
	{
		const u32 idx2 = (code & 0x3) | ((code >> 4) & 0x7c);
		switch (idx2)
		{
			// recCOP2SPECIAL2t indices (see x86 table):
			case 0x00: recCOP2_VADDAx(code);  return true;
			case 0x01: recCOP2_VADDAy(code);  return true;
			case 0x02: recCOP2_VADDAz(code);  return true;
			case 0x03: recCOP2_VADDAw(code);  return true;
			case 0x04: recCOP2_VSUBAx(code);  return true;
			case 0x05: recCOP2_VSUBAy(code);  return true;
			case 0x06: recCOP2_VSUBAz(code);  return true;
			case 0x07: recCOP2_VSUBAw(code);  return true;
			case 0x08: recCOP2_VMADDAx(code); return true;
			case 0x09: recCOP2_VMADDAy(code); return true;
			case 0x0a: recCOP2_VMADDAz(code); return true;
			case 0x0b: recCOP2_VMADDAw(code); return true;
			case 0x0c: recCOP2_VMSUBAx(code); return true;
			case 0x0d: recCOP2_VMSUBAy(code); return true;
			case 0x0e: recCOP2_VMSUBAz(code); return true;
			case 0x0f: recCOP2_VMSUBAw(code); return true;
			case 0x10: recCOP2_VITOF0(code);  return true; // ITOF (no flags)
			case 0x11: recCOP2_VITOF4(code);  return true;
			case 0x12: recCOP2_VITOF12(code); return true;
			case 0x13: recCOP2_VITOF15(code); return true;
			case 0x14: recCOP2_VFTOI0(code);  return true; // FTOI (no flags)
			case 0x15: recCOP2_VFTOI4(code);  return true;
			case 0x16: recCOP2_VFTOI12(code); return true;
			case 0x17: recCOP2_VFTOI15(code); return true;
			case 0x18: recCOP2_VMULAx(code);  return true;
			case 0x19: recCOP2_VMULAy(code);  return true;
			case 0x1a: recCOP2_VMULAz(code);  return true;
			case 0x1b: recCOP2_VMULAw(code);  return true;
			case 0x1c: recCOP2_VMULAq(code);  return true; // MULAq (reads Q)
			case 0x1d: recCOP2_VABS(code);    return true; // ABS (no flags)
			case 0x1e: recCOP2_VMULAi(code);  return true; // MULAi
			case 0x1f: recCOP2_VCLIP(code);   return true; // CLIP (writes clip flag)
			case 0x20: recCOP2_VADDAq(code);  return true; // ADDAq (reads Q)
			case 0x21: recCOP2_VMADDAq(code); return true; // MADDAq (reads Q)
			case 0x22: recCOP2_VADDAi(code);  return true; // ADDAi
			case 0x23: recCOP2_VMADDAi(code); return true; // MADDAi
			case 0x24: recCOP2_VSUBAq(code);  return true; // SUBAq (reads Q)
			case 0x25: recCOP2_VMSUBAq(code); return true; // MSUBAq (reads Q)
			case 0x26: recCOP2_VSUBAi(code);  return true; // SUBAi
			case 0x27: recCOP2_VMSUBAi(code); return true; // MSUBAi
			case 0x28: recCOP2_VADDA(code);   return true; // ADDA
			case 0x29: recCOP2_VMADDA(code);  return true; // MADDA
			case 0x2a: recCOP2_VMULA(code);   return true; // MULA
			case 0x2c: recCOP2_VSUBA(code);   return true; // SUBA
			case 0x2d: recCOP2_VMSUBA(code);  return true; // MSUBA
			case 0x2e: recCOP2_VOPMULA(code); return true; // OPMULA
			case 0x2f: return true;                        // VNOP — no operation, emit nothing
			case 0x30: recCOP2_VMOVE(code);   return true; // MOVE (no flags)
			case 0x31: recCOP2_VMR32(code);   return true; // MR32 (no flags)
			case 0x38: recCOP2_VDIV(code);    return true; // DIV (Q + status)
			case 0x39: recCOP2_VSQRT(code);   return true; // SQRT (Q + status)
			case 0x3a: recCOP2_VRSQRT(code);  return true; // RSQRT (Q + status)
			case 0x3b: return true;                        // VWAITQ — Q is resolved synchronously
			                                               // by the CpuVU0 interpreter (DIV/SQRT/RSQRT
			                                               // write Q immediately, no latency pipeline),
			                                               // so there is nothing to wait for: the
			                                               // interpreter body _vuWAITQ() is empty.
			                                               // Emit nothing instead of an interp call —
			                                               // VWAITQ is the dominant COP2 fallback in
			                                               // VU0-macro geometry (after every divide).
			default: return false;
		}
	}

	// SPECIAL1 table (recCOP2SPECIAL1t indices = funct directly).
	switch (funct)
	{
		case 0x00: recCOP2_VADDx(code);  return true;
		case 0x01: recCOP2_VADDy(code);  return true;
		case 0x02: recCOP2_VADDz(code);  return true;
		case 0x03: recCOP2_VADDw(code);  return true;
		case 0x04: recCOP2_VSUBx(code);  return true;
		case 0x05: recCOP2_VSUBy(code);  return true;
		case 0x06: recCOP2_VSUBz(code);  return true;
		case 0x07: recCOP2_VSUBw(code);  return true;
		case 0x08: recCOP2_VMADDx(code); return true;
		case 0x09: recCOP2_VMADDy(code); return true;
		case 0x0a: recCOP2_VMADDz(code); return true;
		case 0x0b: recCOP2_VMADDw(code); return true;
		case 0x0c: recCOP2_VMSUBx(code); return true;
		case 0x0d: recCOP2_VMSUBy(code); return true;
		case 0x0e: recCOP2_VMSUBz(code); return true;
		case 0x0f: recCOP2_VMSUBw(code); return true;
		case 0x10: recCOP2_VMAXx(code);  return true; // MAX (no flags)
		case 0x11: recCOP2_VMAXy(code);  return true;
		case 0x12: recCOP2_VMAXz(code);  return true;
		case 0x13: recCOP2_VMAXw(code);  return true;
		case 0x14: recCOP2_VMINIx(code); return true; // MINI (no flags)
		case 0x15: recCOP2_VMINIy(code); return true;
		case 0x16: recCOP2_VMINIz(code); return true;
		case 0x17: recCOP2_VMINIw(code); return true;
		case 0x18: recCOP2_VMULx(code);  return true;
		case 0x19: recCOP2_VMULy(code);  return true;
		case 0x1a: recCOP2_VMULz(code);  return true;
		case 0x1b: recCOP2_VMULw(code);  return true;
		case 0x1c: recCOP2_VMULq(code);  return true; // MULq (reads Q)
		case 0x1d: recCOP2_VMAXi(code);  return true; // MAXi (no flags)
		case 0x1e: recCOP2_VMULi(code);  return true; // MULi
		case 0x1f: recCOP2_VMINIi(code); return true; // MINIi (no flags)
		case 0x20: recCOP2_VADDq(code);  return true; // ADDq (reads Q)
		case 0x21: recCOP2_VMADDq(code); return true; // MADDq (reads Q)
		case 0x22: recCOP2_VADDi(code);  return true; // ADDi
		case 0x23: recCOP2_VMADDi(code); return true; // MADDi
		case 0x24: recCOP2_VSUBq(code);  return true; // SUBq (reads Q)
		case 0x25: recCOP2_VMSUBq(code); return true; // MSUBq (reads Q)
		case 0x26: recCOP2_VSUBi(code);  return true; // SUBi
		case 0x27: recCOP2_VMSUBi(code); return true; // MSUBi
		case 0x28: recCOP2_VADD(code);   return true; // ADD
		case 0x29: recCOP2_VMADD(code);  return true; // MADD
		case 0x2a: recCOP2_VMUL(code);   return true; // MUL
		case 0x2b: recCOP2_VMAX(code);   return true; // MAX (no flags)
		case 0x2c: recCOP2_VSUB(code);   return true; // SUB
		case 0x2d: recCOP2_VMSUB(code);  return true; // MSUB
		case 0x2e: recCOP2_VOPMSUB(code);return true; // OPMSUB
		case 0x2f: recCOP2_VMINI(code);  return true; // MINI (no flags)
		default: return false;
	}
}
