/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "Common/GameCommon.h"

// GeneralsX @bugfix Captain97r 26/11/2026 WorldBuilder stores the recruitment window in 30 Hz frames.
constexpr bool teamRecruitmentExpired(UnsignedInt elapsed, Int idleFrames)
{
	return idleFrames > 0 && elapsed > static_cast<UnsignedInt>(idleFrames) * logicFramesFromBaseFrames(1);
}
