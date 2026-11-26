/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "Common/GameCommon.h"
#include "Lib/BaseFunctions.h"
#include <limits>

// GeneralsX @feature Codex 22/11/2026 Limit automatic zero-delay fire between retail ticks.
// A salvo can fire repeatedly on the same tick. Positive cooldowns and explicit reloads have their own deadlines.
constexpr bool weaponIsBetweenZeroDelayShots(UnsignedInt frame, UnsignedInt lastShot, UnsignedInt readyFrame)
{
	return lastShot != 0 && readyFrame == lastShot && frame != lastShot &&
		frame - lastShot < static_cast<UnsignedInt>(logicFramesFromBaseFrames(1));
}

// GeneralsX @bugfix Codex 26/11/2026 Apply rate-of-fire bonuses on the authored tick, then scale the deadline.
inline Int weaponCooldownInLogicFrames(Int baseFrames, Real rateOfFire)
{
#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
	const Int longest = (std::numeric_limits<Int>::max)();
	if (!(rateOfFire > 0.0f) || baseFrames / rateOfFire >= static_cast<Real>(longest / logicFramesFromBaseFrames(1)))
		return longest;
	if (baseFrames <= 0)
		return 0;
#endif
	return logicFramesFromBaseFrames(REAL_TO_INT_FLOOR(baseFrames / rateOfFire));
}
