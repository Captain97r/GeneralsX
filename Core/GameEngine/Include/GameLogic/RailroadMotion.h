/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "Common/GameCommon.h"

// GeneralsX @bugfix Captain97r 26/11/2026 Update train controls in authored frames and travel in native frames.
namespace RailroadMotion
{
	constexpr bool isControlFrame(UnsignedInt frame)
	{
		return frame % logicFramesFromBaseFrames(1) == 0;
	}

	inline Real accelerate(Real speed, Real direction, Real acceleration, Real speedMax)
	{
		speed += 0.02f * direction;
		speed *= acceleration;
		if (speed > speedMax)
			return speedMax;
		if (speed < -speedMax)
			return -speedMax;
		return speed;
	}
}
