/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "Common/GameCommon.h"
#include "WWMath/wwmath.h"

#include <limits>

// GeneralsX @feature Codex 22/11/2026 Limit automatic zero-delay fire between retail ticks.
// A salvo can fire repeatedly on the same tick. Positive cooldowns and explicit reloads have their own deadlines.
inline bool weaponIsBetweenZeroDelayShots(UnsignedInt frame, UnsignedInt lastShot, UnsignedInt readyFrame)
{
	return lastShot != 0 && readyFrame == lastShot && frame != lastShot &&
		frame - lastShot < static_cast<UnsignedInt>(logicFramesFromBaseFrames(1));
}

// GeneralsX @bugfix Codex 23/11/2026 Apply rate-of-fire bonuses before converting the retail reload or shot deadline.
inline Int weaponCooldownInLogicFrames(Int retailFrames, Real rateOfFire)
{
	const Int longestDelay = (std::numeric_limits<Int>::max)();
	if (!(rateOfFire > 0.0f))
		return longestDelay;
	if (retailFrames <= 0)
		return 0;
	const Real duration = retailFrames / rateOfFire;
	if (duration >= static_cast<Real>(longestDelay / logicFramesFromBaseFrames(1)))
		return longestDelay;
	return logicFramesFromBaseFrames(static_cast<Int>(WWMath::Floorf(duration)));
}
