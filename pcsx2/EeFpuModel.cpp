// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The EE FPU / VU FMAC arithmetic, in bits (u32 -> u32), ported verbatim from
// armsx2 (author pstef). This is the SAME numeric model armsx2 states in FPU.cpp,
// but pulled into its own translation unit so the VU can read it WITHOUT dragging
// in the EE COP1 recompiler's FPR-word relocation (FPRreg::UD/SetWord/EeFpuFormat)
// — that coupling only ever existed in the EE FPU opcodes and the upstream test
// harness, never in the model itself. See docs/VU-ACCURACY-STANDALONE.md.
//
// Every function here is pure integer/double math over raw single-precision words;
// none touches fpuRegs / FCR31. The EE FPU interpreter opcodes stay in FPU.cpp with
// our own register representation (FPRreg::UL) untouched.

#include "EeFpuModel.h"

#include "common/Pcsx2Defs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// The ends of the EE's representable range (0x7FFFFFFF == (2 - 2^-23) * 2^128,
// one binade above IEEE single's max; 0x00800000 == 2^-126, smallest normal).
static constexpr double kEeFpuMax = 0x1.fffffep+128;
static constexpr double kEeMinNormal = 0x1p-126;

static u32 floatToBits(float f)
{
	u32 bits;
	std::memcpy(&bits, &f, sizeof(bits));
	return bits;
}

static float bitsToFloat(u32 bits)
{
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

/*	The EE value of a raw FPR word, exactly, as a double.

	The only way an operand enters this file: every arithmetic op, every flag
	decision and every compare reads through here. Here 0x7F800000 is 2^128, an
	ordinary number, and 0x7FFFFFFF is the largest one; a double holds the whole
	EE range, so nothing is rewritten on the way in.

	fpuDouble()/fpuOperandBits(), which this replaced, folded exponent 255 to
	+-0x7F7FFFFF to fit a host single, so the op ran on a different operand than
	the one it was given; 368 of the 1147 captured cases touch the top binade.
	The arithmetic stopped reading them in ccae642180 and 4ce2b543cb, the
	compares last.

	Denormal operands flush to signed zero. The EE has none, and U is raised only
	when the result computed from the flushed operands is nonzero and below the
	smallest normal -- "mul 1.0, MIN_DENORM" returns +0 with FCR31 untouched
	(autocases_fpuovf.h).
*/
static double eeToDouble(u32 f)
{
	const u32 exp = (f >> 23) & 0xFF;
	u64 bits = static_cast<u64>(f & 0x80000000u) << 32;
	if (exp != 0)
	{
		bits |= (static_cast<u64>(exp) + (1023 - 127)) << 52;
		bits |= static_cast<u64>(f & 0x007FFFFFu) << 29;
	}
	double d;
	std::memcpy(&d, &bits, sizeof(d));
	return d;
}

/*	Whether a result saturates, decided after the rounding and not on the exact
	value. The unit normalises and rounds to 24 bits before anything looks at
	the exponent field, so a result can exceed kEeFpuMax and still come back on
	it: 0x7FFFFFFF + 2^104 is 2^129 - 2^104, which needs 25 significant bits and
	chops to 0x7FFFFFFF. One exponent higher the sum is 2^129, which does not
	fit however it is rounded.

	Above 2^129 no rounding brings a value back, which also keeps the scaled
	cast below in float range.

	The rounding is the (float) cast's, so the ambient FPCR decides it here as
	it does everywhere else in this file -- under round-to-nearest the same
	2^129 - 2^104 is a tie that goes to 2^129 and does saturate.
*/
static bool eeRoundsOutOfRange(double exact)
{
	const double mag = std::fabs(exact);
	if (mag <= kEeFpuMax)
		return false;
	if (mag >= 0x1p129)
		return true;
	const u32 w = floatToBits(static_cast<float>(exact * 0x1p-4));
	return ((w >> 23) & 0xFFu) + 4u > 255u;
}

/*	Underflow: a result strictly below 2^-126 and not zero is not always
	flushed. The add/sub family leaves the mantissa bits where normalisation put
	them and forces the exponent field to 0; MUL and DIV clear them and return
	signed zero. That is the raw output of an adder with no denormal path:
	`exact` is a double 1.m * 2^E, and what comes out is m's top 23 bits, i.e.
	bits [51:29] of the double, with the exponent thrown away. It is not the
	arithmetic answer, only its bits, and no rounding mode produces it.

	The console rows this reproduces, how they were sampled and how they rule
	out flushing and the true denormal value, are in
	tests/ctest/core/recompilers/ee_fpu_underflow_console_tests.cpp.
*/
static u32 eeRoundToSingle(double exact, bool addsub = false)
{
	const double mag = std::fabs(exact);

	if (eeRoundsOutOfRange(exact))
		return (std::signbit(exact) ? 0x80000000u : 0u) | 0x7FFFFFFFu;

	if (mag < kEeMinNormal)
	{
		/*	Ahead of the (float) cast, so the answer does not depend on the
			ambient FPCR having FZ set and nothing can round up out of the
			region -- the console returns +0 for a product of 2^-126 - 2^-150,
			which is nearer 2^-126 than to zero. */
		const u32 sign = std::signbit(exact) ? 0x80000000u : 0u;
		if (!addsub || exact == 0.0)
			return sign;

		u64 bits;
		std::memcpy(&bits, &exact, sizeof(bits));
		return sign | static_cast<u32>((bits >> 29) & 0x7FFFFFu);
	}

	if (mag >= 0x1p126)
	{
		/*	Scale down by 2^4, round there, then add the 4 exponents back. The
			scaled exponent field is at most 251, so the +4 cannot carry into
			the sign, and the >= 2^126 floor keeps the scaled value normal, so
			nothing is flushed on the way through. */
		return floatToBits(static_cast<float>(exact * 0x1p-4)) + (4u << 23);
	}

	return floatToBits(static_cast<float>(exact));
}

/*	The EE FPU's adder carries no guard bits to the right of the mantissa. A
	compliant adder shifts the smaller operand right into extra bits it keeps for
	the rounding decision; whatever shifts past this one's mantissa is gone.
	Subtraction -- and addition of unlike signs -- can then renormalise left and
	pull the hole up into the result, landing one ULP toward zero from the IEEE
	answer:

	    sub.s  0x00800000, 0x3F000000  ->  console BF000000, plain IEEE BEFFFFFF

	The model is the exponent difference, which is how far the smaller operand
	gets shifted: it loses its low (|diff| - 1) mantissa bits, and past 24 it has
	nothing left but its sign. |diff| <= 1 masks nothing.

	Ported from x86 FPU_ADD_SUB (x86/iFPU.cpp) and, on arm64, fpuEmitGuardedAddSub
	(iFPU-arm64.cpp, the single-precision fast path) and FPU_ADD_SUB_D
	(iFPUd-arm64.cpp, the Full-clamp DOUBLE path).

	Both recompilers gate the masking on CHECK_FPU_GUARDED, the fpuGuardedAddSub
	INI bool, so an EE-FPU-heavy title can buy back one op per ADD.S/SUB.S. The
	interpreter does not read it: its target is the console, not the recompiler's
	speed. With fpuGuardedAddSub=false the engines therefore disagree on exactly
	these cases, which EeRecFpuGuardBit.GuardOffDivergesFromInterpreterByDesign
	pins.

	The console rows this reproduces, with their corpus ordinals, are tabulated in
	tests/ctest/core/recompilers/ee_fpu_guarded_addsub_console_tests.cpp.
*/
static void fpuGuardMask(u32& a, u32& b)
{
	const s32 diff = (s32)((a >> 23) & 0xFF) - (s32)((b >> 23) & 0xFF);

	if (diff >= 25)
		b &= 0x80000000;
	else if (diff >= 2)
		b &= 0xffffffffu << (diff - 1);
	else if (diff <= -25)
		a &= 0x80000000;
	else if (diff <= -2)
		a &= 0xffffffffu << (-diff - 1);
}

/*	The EE's adder: mask the guard bits away, add exactly, round once.

	The mask is what makes the add exact. Within 24 exponents the sum needs 48
	bits of the double's 53; beyond that the mask has already reduced the
	smaller operand to +-0. So eeRoundToSingle() below is the only rounding, as
	on the hardware.

	Subtraction is addition of the negated operand, as IEEE defines it: that gets
	the zero signs right, including for a masked +-0.

	This returns the sum rather than the rounded word because the flags come off
	it too: what the adder produced is what FCR31 reports, so its callers round
	it for the destination and hand this same value to raiseOrClearOU() instead
	of recomputing a second sum from the unmasked operands. Where the mask
	erases an operand the two differ, and the console follows this one -- see
	ee_fpu_ou_rounding_console_tests.cpp. */
static double eeGuardedSum(u32 a, u32 b, bool issub)
{
	fpuGuardMask(a, b);
	if (issub)
		b ^= 0x80000000;
	return eeToDouble(a) + eeToDouble(b);
}

/*	The partial remainder, in the redundant form the recurrence carries it in.
	No step propagates a carry across the width of the operand, which is why the
	selector below cannot see the true remainder. */
struct EeSrtRemainder
{
	u32 sum, carry;
};

/*	The digit, in the form the recurrence consumes it: one all-ones mask per
	sign, both zero for the digit 0. Both set never happens.

	Every use of it inside the step is a select. As -1/0/+1 those are two
	data-dependent branches per step, on a digit sequence nothing predicts, and
	the mispredicts cost more than the step does. */
struct EeSrtDigitMask
{
	u32 plus, minus;
};

/*	Back to -1/0/+1, for the quotient and the root. Nothing the next digit
	depends on reads it. */
static __fi u32 eeSrtDigitValue(EeSrtDigitMask d)
{
	return d.minus - d.plus;
}

static __fi EeSrtRemainder eeSrtCarrySave(u32 a, u32 b, u32 c)
{
	const u32 u = a ^ b;
	const u32 h = (a & b) | (u & c);
	return {u ^ c, h << 1};
}

/*	The selection function: which of -1, 0, +1 the next digit takes.

	It assimilates the redundant remainder only partially -- the carry word is
	added in above bit 23 while the low 24 bits of the sum are OR-ed back rather
	than added -- so it decides on something other than the remainder, and picks a
	different digit from the one an exact comparison would. The thresholds are
	+2^23 and -2^24, with the binary point between bits 24 and 25; that asymmetry
	is what biases the unit toward truncation. The digit set is redundant, so the
	last digit can still be -1 and the result reach T+1 on rows a truncation could
	never reach.

	Its two comparisons are already the masks the next step selects with, so they
	are what it returns. */
static __fi EeSrtDigitMask eeSrtDigit(EeSrtRemainder r)
{
	constexpr u32 mask = (1u << 24) - 1u;
	const s32 estimate = (s32)(((r.sum & ~mask) + r.carry) | (r.sum & mask));
	return {(u32)0 - (u32)(estimate >= (1 << 23)),
			(u32)0 - (u32)(estimate < (s32)(~0u << 24))};
}

/*	On a zero digit the next digit is selected from the un-recompressed pair
	while the state advances with the recompressed one, so selection and state
	see different splittings of the same value. Drop the distinction and the
	model stops reproducing silicon. */
static __fi EeSrtRemainder eeSrtSelect(EeSrtRemainder cur, EeSrtRemainder next, EeSrtDigitMask d)
{
	const u32 m = d.plus | d.minus;
	return {(cur.sum & ~m) | (next.sum & m), (cur.carry & ~m) | (next.carry & m)};
}

/*	The digits the unit does not have to run.

	The recurrence returns T or T+1, T being the truncated quotient of the two
	significands, and on a large share of operands the remainder alone says
	which:

	    lt  = ma < mb              does the quotient need a shift
	    num = ma << (23 + lt)
	    T   = num / mb             the truncated 24-bit significand
	    rem = num - T*mb           0 <= rem < mb
	    u   = mb - rem             how far the exact quotient sits below T+1

	    u > cap  =>  T       cap = 2^22 on A>=B, max(2^23, mb-2^22) on A<B

	Square root is the same shape with the root in place of the quotient: with X
	the placed radicand, R = floor(sqrt(X)) and rem = X - R*R, u is 2R + 1 - rem
	and the cap is 2^23. So one integer division answers 44% of arbitrary
	divides and one integer square root 65% of square roots without a digit
	being run; the rest fall through to the recurrence.

	The caps came out of console captures, not out of the recurrence, so this is
	only ever a shortcut: what it returns has to be what the recurrence would
	have returned. The implication is one-way -- rows below the cap are not
	settled and must fall through -- so a narrowed cap costs only digits, while a
	widened one can change results. EeFpuDivUnitExhaustive and
	EeFpuDivUnitConsole hold the rows it fires on against the console. */
static __fi u32 eeDivideCap(u32 mb, u32 lt)
{
	return lt ? ((mb > (3u << 22)) ? mb - (1u << 22) : (1u << 23)) : (1u << 22);
}

/*	The quotient of two 24-bit significands as 25 digits, weights 2^24 down to
	2^0. A positive digit subtracts the divisor as ~divisor with the +1 fed into
	the carry word, which the selector then sees -- that is one of the places
	the estimate and the state come apart. The value returned is 25 bits when
	sma >= smb and 24 bits when it is not; the caller normalises, and the digit
	that falls off the bottom there is simply dropped. */
static u32 eeDivideSignificand(u32 sma, u32 smb)
{
	{
		const u32 lt = (sma < smb) ? 1u : 0u;
		const u64 num = (u64)sma << (23 + lt);
		const u32 T = (u32)(num / smb);
		const u32 rem = (u32)(num - (u64)T * smb);
		if ((smb - rem) > eeDivideCap(smb, lt))
		{
			// Only the 24 bits the caller keeps are the answer. On A>=B the
			// recurrence's own last digit is as often 1 as 0 and is dropped
			// there, so this arm supplies a 0 for it rather than reproducing
			// it -- do not compare the two below that bit.
			return lt ? T : (T << 1);
		}
	}

	const u32 divisor = smb << 2;
	const u32 ndivisor = ~divisor;
	EeSrtRemainder rem = {sma << 2, 0};
	u32 quotient = 0;
	EeSrtDigitMask digit = {~0u, 0}; // +1

	for (int i = 0; i < 24; ++i)
	{
		quotient = (quotient << 1) + eeSrtDigitValue(digit);
		const u32 addend = (ndivisor & digit.plus) | (divisor & digit.minus);
		// subtracting the mask is the +1 that goes with ~divisor
		const EeSrtRemainder cur = {rem.sum, rem.carry - digit.plus};
		const EeSrtRemainder next = eeSrtCarrySave(cur.sum, cur.carry, addend);
		digit = eeSrtDigit(eeSrtSelect(cur, next, digit));
		rem.sum = next.sum << 1;
		rem.carry = next.carry << 1;
	}
	return (quotient << 1) + eeSrtDigitValue(digit);
}

EEFPU_MODEL_CALL u32 eeDivide(u32 a, u32 b)
{
	const s32 ea = (s32)((a >> 23) & 0xFF);
	const s32 eb = (s32)((b >> 23) & 0xFF);
	const u32 sign = (a ^ b) & 0x80000000u;

	if (ea == 0)
		return sign; // zero dividend (denormals are zero), sign from both operands

	// Exponent 255 is an ordinary binade on this FPU, so every finite operand
	// reaches here and the hidden bit is always present. The divisor is already
	// known nonzero: that is a flag question the callers answer first.
	u32 quotient = eeDivideSignificand(0x800000u | (a & 0x7FFFFFu), 0x800000u | (b & 0x7FFFFFu));
	s32 e = ea - eb + 126;
	if (quotient >= (1u << 24))
	{
		quotient >>= 1;
		++e;
	}

	// No carry out of the significand is possible below this point -- there is
	// no rounding step left that could walk the quotient into the next binade.
	if (e > 255)
		return sign | 0x7FFFFFFFu; // the EE's maximum, not FLT_MAX
	if (e < 1)
		return sign; // the EE has no denormals to underflow into
	return sign | ((u32)e << 23) | (quotient & 0x7FFFFFu);
}

/*	floor(sqrt(x)) for x below 2^48, exactly. The double is a seed only: x
	converts to it exactly and its square root is correctly rounded, so the true
	root is within one of the truncation under any host rounding mode, and the
	two corrections run unconditionally. */
static u32 eeISqrt48(u64 x)
{
	u64 r = (u64)std::sqrt((double)x);
	while (r > 0 && r * r > x)
		--r;
	while ((r + 1) * (r + 1) <= x)
		++r;
	return (u32)r;
}

static u32 eeSqrtSignificand(u32 m)
{
	{
		// The radicand in the law's frame, which places it 22 bits further up
		// than the recurrence does. See the comment above eeDivideCap().
		const u64 x = (u64)m << 22;
		const u32 root = eeISqrt48(x);
		if ((2ull * root + 1ull - (x - (u64)root * root)) > (1u << 23))
			return root;
	}

	EeSrtRemainder rem = {m, 0};
	u32 root = 0;
	EeSrtDigitMask digit = {~0u, 0}; // +1

	for (int i = 0; i < 24; ++i)
	{
		const u32 w = 1u << (24 - i);
		const u32 base_plus = root + w, base_minus = root - w;
		const u32 root_plus = base_plus + w, root_minus = base_minus - w;
		const u32 addend = (~base_plus & digit.plus) | (base_minus & digit.minus);
		const u32 any = digit.plus | digit.minus;
		root = (root_plus & digit.plus) | (root_minus & digit.minus) | (root & ~any);
		const EeSrtRemainder cur = {rem.sum, rem.carry - digit.plus};
		const EeSrtRemainder next = eeSrtCarrySave(cur.sum, cur.carry, addend);
		digit = eeSrtDigit(eeSrtSelect(cur, next, digit));
		rem.sum = next.sum << 1;
		rem.carry = next.carry << 1;
	}
	// The last digit carries weight 2^1, below the root's least significant
	// bit, so it only reaches the result by borrowing out of it.
	root += eeSrtDigitValue(digit) << 1;
	return (root >> 2) & 0xFFFFFFu;
}

EEFPU_MODEL_CALL u32 eeSqrtBits(u32 t)
{
	const u32 E = (t >> 23) & 0xFFu;
	if (E == 0)
		return 0; // +/-0 and the denormals: the EE drops the sign here, and so
		          // do both recompilers (they take |Ft| first). See
		          // EeRecFpu.SqrtSOfNegativeZeroIsPositiveZero.

	const u32 m = (0x800000u | (t & 0x7FFFFFu)) << ((E & 1u) ? 1 : 2);
	return (((E + 127u) >> 1) << 23) | (eeSqrtSignificand(m) & 0x7FFFFFu);
}

/*	The 3-bit window that selects what digit `bit` of b contributes: 0 and 7
	select zero, 1 and 2 select +a, 3 selects +2a, 4 selects -2a, 5 and 6
	select -a. */
static u32 eeBoothWindow(u32 b, u32 bit)
{
	return (bit ? b >> (bit * 2 - 1) : b << 1) & 7;
}

/*	That digit's partial product of a. 32-bit on purpose: no column above 31 can
	reach a decision taken at column 15, and letting the shift overflow is what
	discards them. A negative digit is left as a one's complement here -- the
	`+1` that would complete the negation is eeBoothCorrection() below. */
static u32 eeBoothPartial(u32 a, u32 b, u32 bit)
{
	const u32 window = eeBoothWindow(b, bit);
	a <<= bit * 2;
	a += (window == 3 || window == 4) ? a : 0;
	if (window >= 4 && window <= 6)
		a ^= 0u - (1u << (bit * 2));
	return (window >= 1 && window <= 6) ? a : 0;
}

/*	The `+1` a negative digit owes, at that digit's own weight. Digits 0..4
	never receive theirs -- their columns are not built, which is the whole
	defect -- so only 5..7 get one. */
static u32 eeBoothCorrection(u32 b, u32 bit)
{
	const u32 window = eeBoothWindow(b, bit);
	return (window >= 4 && window <= 6) ? (1u << (bit * 2)) : 0;
}

/*	One 3:2 carry-save row: returns the sum bits, writes the carry bits. */
static u32 eeCarrySaveAdd(u32 a, u32 b, u32 c, u32& carry)
{
	const u32 u = a ^ b;
	carry = ((u & c) | (a & b)) << 1;
	return u ^ c;
}

/*	The 48-bit significand product as the console's array computes it: the exact
	product, less 2^15 where the truncated low columns come up short there. The
	masks are the columns silicon does not build. */
static u64 eeMulArray(u32 a, u32 b)
{
	const u64 full = static_cast<u64>(a) * static_cast<u64>(b);

	const u32 p0 = eeBoothPartial(a, b, 0);
	const u32 p1 = eeBoothPartial(a, b, 1);
	const u32 p2 = eeBoothPartial(a, b, 2);
	const u32 p3 = eeBoothPartial(a, b, 3);
	const u32 p4 = eeBoothPartial(a, b, 4);
	const u32 p5 = eeBoothPartial(a, b, 5);
	const u32 p6 = eeBoothPartial(a, b, 6);
	const u32 p7 = eeBoothPartial(a, b, 7);

	/*	The tree below is four carry-save levels deep and each lifts a bit by one
		column, so nothing under bit 11 can reach the decision at column 15.
		Digit 4's mask is exactly that boundary -- widening it changes no output,
		narrowing it by one does. Digit 5's sits one higher because its bits 10
		and 11 do not travel through the tree; they are re-injected below. */
	u32 carry0, carry1, carry2, carry3, carry4, carry5;
	const u32 sum0 = eeCarrySaveAdd(p1, p2, p3, carry0);
	const u32 sum1 = eeCarrySaveAdd(p4 & ~0x7ffu, p5 & ~0xfffu, p6, carry1);

	// Digit 5's two surviving product bits, and the corrections digits 5 and 6
	// still receive, ride on rows they did not originate in.
	const u32 hi1 = carry1 | eeBoothCorrection(b, 6) | (p5 & 0x800);
	const u32 row7 = p7 | ((p5 & 0x400) + eeBoothCorrection(b, 5));

	const u32 sum2 = eeCarrySaveAdd(p0, sum0, carry0, carry2);
	const u32 sum3 = eeCarrySaveAdd(row7, sum1, hi1, carry3);
	const u32 sum4 = eeCarrySaveAdd(carry2, sum3, carry3, carry4);
	const u32 sum5 = eeCarrySaveAdd(sum2, sum4, carry4, carry5);

	const u32 lo = sum5 & ~0x7fffu;
	const u32 hi = (carry5 + eeBoothCorrection(b, 7)) & ~0x7fffu;
	return full - (((lo + hi) ^ full) & 0x8000);
}

bool eeMulOneUlpLow(u32 fs, u32 ft)
{
	if ((fs & 0x7F800000) == 0 || (ft & 0x7F800000) == 0)
		return false; // a zero operand (denormals are zero): the product is zero

	const u32 a = 0x800000u | (fs & 0x7FFFFF);
	const u32 b = 0x800000u | (ft & 0x7FFFFF);
	const u64 prod = static_cast<u64>(a) * static_cast<u64>(b); // exact in 64
	const int k = (prod >> 47) ? 24 : 23;
	if ((prod & ((1ull << k) - 1u)) >= 0x8000u)
		return false; // the tail below the ULP absorbs the whole borrow

	return (prod >> k) != (eeMulArray(a, b) >> k);
}

/*	eeRoundToSingle() for a product, plus the multiplier defect. */
static u32 eeMulRound(u32 fs, u32 ft, double exact)
{
	const u32 w = eeRoundToSingle(exact);

	if (std::fabs(exact) > kEeFpuMax) // saturated: never measured, leave it
		return w;
	if ((w & 0x7F800000) == 0) // flushed to zero
		return w;
	if ((w & 0x7FFFFFFF) == 0x00800000) // a decrement would leave the normals
		return w;

	return eeMulOneUlpLow(fs, ft) ? w - 1u : w;
}


namespace EeFpuModel
{

// O and U as raiseOrClearOU() reads them for FCR31: O after the rounding, so a
// value past 0x7FFFFFFF that chops back onto it does not raise it, and U off
// the exact value, the only place a flushed result is still distinguishable
// from a zero.
static Result MakeResult(double exact, u32 bits)
{
	Result s;
	s.bits = bits;
	s.overflow = eeRoundsOutOfRange(exact);
	s.underflow = !s.overflow && exact != 0.0 && std::fabs(exact) < kEeMinNormal;
	return s;
}

Result AddSub(u32 a, u32 b, bool issub)
{
	const double sum = eeGuardedSum(a, b, issub);
	return MakeResult(sum, eeRoundToSingle(sum, true));
}

Result Mul(u32 fs, u32 ft)
{
	const double product = eeToDouble(fs) * eeToDouble(ft);
	return MakeResult(product, eeMulRound(fs, ft, product));
}

Accumulate MulAccumulate(u32 acc, u32 fs, u32 ft, bool issub)
{
	const Result product = Mul(fs, ft);
	if (product.overflow)
	{
		Result result = product;
		result.bits ^= issub ? 0x80000000u : 0u;
		return {product, result};
	}
	return {product, AddSub(acc, product.bits, issub)};
}

EEFPU_MODEL_CALL u32 Divide(u32 a, u32 b)
{
	return eeDivide(a, b);
}

EEFPU_MODEL_CALL u32 SqrtBits(u32 t)
{
	return eeSqrtBits(t);
}

EEFPU_MODEL_CALL u32 RecipSqrt(u32 a, u32 t)
{
	return eeDivide(a, eeSqrtBits(t));
}
} // namespace EeFpuModel
