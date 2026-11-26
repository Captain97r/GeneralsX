/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include "Common/FramePacer.h"
#include "GameClient/VisualTiming.h"

namespace
{
class VisualPacer : public FramePacer
{
public:
	void setElapsed(Real seconds) { m_updateTime = seconds; }
	Int cameraSteps(double &remainder) const
	{
		return takeScriptedCameraFrames(getActualLogicTimeScaleOverFpsRatio(IgnoreFrozenTime), remainder);
	}
};
}

TEST(VisualTiming, BeaconAndRiverAdvanceByThirtyAuthoredFramesInOneSecond)
{
	VisualPacer pacer;
	for (const Int fps : {30, 45, 60, 120})
	{
		SCOPED_TRACE(fps);
		pacer.setElapsed(1.0f / fps);
		Real beacon = 0.0f;
		Real river = 0.0f;
		for (Int frame = 0; frame < fps; ++frame)
		{
			const Real step = pacer.getActualLogicTimeScaleOverFpsRatio();
			beacon += logicRateFromBaseRate(0.25f) * step;
			river += 0.002f * logicRateFromBaseRate(step);
		}
		EXPECT_NEAR(7.5f, beacon, 0.0001f);
		EXPECT_NEAR(0.06f, river, 0.000001f);
	}
	pacer.setTimeFrozen(true);
	EXPECT_FLOAT_EQ(0.0f, pacer.getActualLogicTimeScaleOverFpsRatio());
}

TEST(VisualTiming, ShakeChangesPhaseOncePerThirtyHertzRender)
{
	const Int ticks = logicFramesFromBaseFrames(1);
	for (Int render = 0; render < BaseFps; ++render)
	{
		Int advances = 0;
		for (Int tick = render * ticks; tick < (render + 1) * ticks; ++tick)
			if (isCameraShakeFrame(tick))
				++advances;
		EXPECT_EQ(1, advances);
	}
}

#if defined(RTS_EXPERIMENTAL_60HZ_SIMULATION)
TEST(VisualTiming, ScriptedCameraConsumesElapsedFramesWhileTheWorldIsFrozen)
{
	VisualPacer pacer;
	pacer.setTimeFrozen(true);
	for (const Int fps : {30, 45, 60, 120})
	{
		SCOPED_TRACE(fps);
		pacer.setElapsed(1.0f / fps);
		double remainder = 0.0;
		Int steps = 0;
		for (Int frame = 0; frame < fps; ++frame)
			steps += pacer.cameraSteps(remainder);
		EXPECT_EQ(LOGICFRAMES_PER_SECOND, steps);
	}

	double remainder = 0.0;
	pacer.setElapsed(0.0f);
	EXPECT_EQ(0, pacer.cameraSteps(remainder));
	pacer.setElapsed(0.5f / LOGICFRAMES_PER_SECOND);
	EXPECT_EQ(0, pacer.cameraSteps(remainder));
	pacer.setGameHalted(true);
	EXPECT_EQ(0, pacer.cameraSteps(remainder));
	pacer.setGameHalted(false);
	EXPECT_EQ(1, pacer.cameraSteps(remainder));
	pacer.setElapsed(3.0f / LOGICFRAMES_PER_SECOND);
	EXPECT_EQ(3, pacer.cameraSteps(remainder));
}
#endif
