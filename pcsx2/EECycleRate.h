// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "Config.h"

// ponytail: ARMSX2's EE cycle rate governor (a dynamic selector patched into emitted charges) is not
// ported; this is the two-call surface microVU reads from it, answered from the static setting.
namespace EECycleRate
{
	/// The cycle rate selector the EE currently runs at (-3..3).
	inline s8 GetEffective() { return static_cast<s8>(EmuConfig.Speedhacks.EECycleRate); }
	inline void NoteVuReset(int /*vu_index*/) {}
} // namespace EECycleRate
