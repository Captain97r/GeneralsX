/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include "Common/FramePacer.h"

namespace
{
class OfflinePacer : public FramePacer
{
public:
	Int advance(Real seconds, Bool fastMode = false)
	{
		m_updateTime = seconds;
		return takeOfflineLogicFrames(fastMode);
	}
};
} // namespace

TEST(FramePacerTiming, OfflineClockTracksWallTimeAtDifferentRenderRates)
{
	for (Int tickRate : {30, 60})
	{
		for (Int renderRate : {30, 45, 60, 120})
		{
			SCOPED_TRACE(::testing::Message() << tickRate << " Hz at " << renderRate << " FPS");
			OfflinePacer pacer;
			pacer.configureGameLogicRate(tickRate);
			pacer.setFramesPerSecondLimit(renderRate);
			Int frames = 0;
			for (Int i = 0; i < renderRate * 10; ++i)
				frames += pacer.advance(1.0f / renderRate);
			EXPECT_EQ(frames, 10 * tickRate);
		}
	}
}

TEST(FramePacerTiming, PausedAndFastModesDiscardDebt)
{
	OfflinePacer pacer;
	pacer.configureGameLogicRate(60);
	EXPECT_EQ(pacer.advance(1.0f / 120), 0);
	pacer.setGameHalted(true);
	EXPECT_EQ(pacer.advance(10.0f), 0);
	pacer.setGameHalted(false);
	EXPECT_EQ(pacer.advance(1.0f / 120), 0);
	EXPECT_EQ(pacer.advance(1.0f / 120), 1);
	pacer.setTimeFrozen(true); // Frozen worlds still run script countdowns.
	EXPECT_EQ(pacer.advance(1.0f / 30), 2);
	pacer.setTimeFrozen(false);
	EXPECT_EQ(pacer.advance(1.0f / 120), 0);
	EXPECT_EQ(pacer.advance(2.0f, true), 1);
	EXPECT_EQ(pacer.advance(1.0f / 120), 0);
	pacer.enableLogicTimeScale(false);
	EXPECT_EQ(pacer.advance(2.0f), 1);
}

TEST(FramePacerTiming, CatchUpIsBoundedAndNewWorldsDiscardTheFraction)
{
	OfflinePacer pacer;
	pacer.configureGameLogicRate(60);
	EXPECT_EQ(pacer.advance(5.0f), 8);
	EXPECT_EQ(pacer.advance(1.0f / 60), 1);
	EXPECT_EQ(pacer.advance(1.0f / 120), 0);
	pacer.configureGameLogicRate(30);
	EXPECT_EQ(pacer.advance(1.0f / 60), 0);
	EXPECT_EQ(pacer.advance(1.0f / 60), 1);
}

TEST(FramePacerTiming, SpeedHotkeysRespectNativeRateAtDifferentRenderCaps)
{
	const Int previousRate = getSimulationRate();
	prepareSimulationRateForGame(60);
	for (Int renderRate : {30, 60, 120})
	{
		OfflinePacer pacer;
		pacer.setFramesPerSecondLimit(renderRate);
		pacer.configureGameLogicRate(60);
		for (Int rate : {60, 55, 60, 65})
		{
			if (rate != pacer.getLogicTimeScaleFps())
				pacer.changeOfflineLogicTimeScale(rate > pacer.getLogicTimeScaleFps() ? FpsValueChange_Increase : FpsValueChange_Decrease);
			EXPECT_TRUE(pacer.isLogicTimeScaleEnabled());
			EXPECT_EQ(rate, pacer.getLogicTimeScaleFps());
			Int ticks = 0;
			for (Int render = 0; render < renderRate; ++render)
				ticks += pacer.advance(1.0f / renderRate);
			EXPECT_EQ(ticks, rate);
		}
	}
	OfflinePacer pacer;
	pacer.setFramesPerSecondLimit(72);
	pacer.configureGameLogicRate(70);
	pacer.changeOfflineLogicTimeScale(FpsValueChange_Increase);
	EXPECT_FALSE(pacer.isLogicTimeScaleEnabled());
	pacer.changeOfflineLogicTimeScale(FpsValueChange_Decrease);
	EXPECT_TRUE(pacer.isLogicTimeScaleEnabled());
	EXPECT_EQ(pacer.getLogicTimeScaleFps(), 70);
	pacer.setFramesPerSecondLimit(30);
	pacer.configureGameLogicRate(240);
	pacer.changeOfflineLogicTimeScale(FpsValueChange_Increase);
	EXPECT_TRUE(pacer.isLogicTimeScaleEnabled());
	EXPECT_EQ(pacer.getLogicTimeScaleFps(), 240);
	pacer.configureGameLogicRate(1);
	pacer.changeOfflineLogicTimeScale(FpsValueChange_Decrease);
	EXPECT_EQ(pacer.getLogicTimeScaleFps(), 5);
	prepareSimulationRateForGame(previousRate);
}
