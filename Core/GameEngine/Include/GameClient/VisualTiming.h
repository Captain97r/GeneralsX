/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "Common/GameCommon.h"

// GeneralsX @bugfix Codex 26/11/2026 Advance frame-based script easing in whole camera steps, retaining subframes.
inline Int takeScriptedCameraFrames(Real elapsedFrames, double &remainder)
{
	if (!(elapsedFrames > 0.0f))
		return 0;
	remainder += elapsedFrames;
	const Int steps = static_cast<Int>(remainder);
	remainder -= steps;
	return steps;
}

// GeneralsX @bugfix Codex 26/11/2026 Reverse and damp the camera shake once per authored frame.
constexpr bool isCameraShakeFrame(UnsignedInt frame)
{
	return frame % logicFramesFromBaseFrames(1) == 0;
}
