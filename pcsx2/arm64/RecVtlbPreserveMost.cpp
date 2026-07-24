// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Call targets for the preserve_most ABI shims in RecVtlbPreserveMost.S.
// See that file for what the shims do and why they exist.
//
// These are unmangled wrappers so the assembly can name them directly instead
// of hard-coding Itanium-mangled template symbols. `used` is required: the only
// references to them live in an assembly TU, which the LTO plugin never sees,
// so without it LTO would consider them unreachable and discard them.

#include "vtlb.h"

#ifdef ARCH_ARM64
// RecVtlbPreserveMost.S gates the shims on !defined(__clang__) because GCC's .S
// preprocessor cannot evaluate __has_attribute. Both sides must stay in sync:
// shims without targets, or targets without shims, is an undefined symbol at
// link time — and with LTO a confusing one. Catch the drift here instead.
#if defined(__clang__) && !VTLB_HAS_PRESERVE_MOST
#error "clang without preserve_most: the .S emits no shims but the emitter calls them"
#endif
#if !defined(__clang__) && VTLB_HAS_PRESERVE_MOST
#error "non-clang compiler with preserve_most: the .S emits shims with no targets"
#endif
#endif

#if defined(ARCH_ARM64) && !VTLB_HAS_PRESERVE_MOST

#define VTLB_PM_TARGET __attribute__((used, visibility("default")))

extern "C" {

VTLB_PM_TARGET mem8_t  vtlb_pmt_memRead8(u32 mem)  { return vtlb_memRead<mem8_t>(mem); }
VTLB_PM_TARGET mem16_t vtlb_pmt_memRead16(u32 mem) { return vtlb_memRead<mem16_t>(mem); }
VTLB_PM_TARGET mem32_t vtlb_pmt_memRead32(u32 mem) { return vtlb_memRead<mem32_t>(mem); }
VTLB_PM_TARGET mem64_t vtlb_pmt_memRead64(u32 mem) { return vtlb_memRead<mem64_t>(mem); }
VTLB_PM_TARGET RETURNS_R128 vtlb_pmt_memRead128(u32 mem) { return vtlb_memRead128(mem); }

VTLB_PM_TARGET void vtlb_pmt_memWrite8(u32 mem, mem8_t v)   { vtlb_memWrite<mem8_t>(mem, v); }
VTLB_PM_TARGET void vtlb_pmt_memWrite16(u32 mem, mem16_t v) { vtlb_memWrite<mem16_t>(mem, v); }
VTLB_PM_TARGET void vtlb_pmt_memWrite32(u32 mem, mem32_t v) { vtlb_memWrite<mem32_t>(mem, v); }
VTLB_PM_TARGET void vtlb_pmt_memWrite64(u32 mem, mem64_t v) { vtlb_memWrite<mem64_t>(mem, v); }
VTLB_PM_TARGET void TAKES_R128 vtlb_pmt_memWrite128(u32 mem, r128 v) { vtlb_memWrite128(mem, v); }

}

#endif
