/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cmath>
#include <gtest/gtest.h>

#include "GameLogic/PhysicsTimeStep.h"
#include "GameLogic/WeaponTiming.h"
#include "Common/FramePacer.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioRequest.h"

TEST(SimulationTiming, ModUnitsUseWallClockSeconds)
{
	const float steps = LOGICFRAMES_PER_SECONDS_REAL;
	EXPECT_FLOAT_EQ(ConvertDurationFromMsecsToFrames(10000.0f), steps * 10.0f); // oil derrick
	EXPECT_FLOAT_EQ(ConvertDurationFromMsecsToFrames(100.0f), steps * 0.1f); // poison damage
	EXPECT_FLOAT_EQ(ConvertVelocityInSecsToFrames(300.0f) * steps, 300.0f);
	EXPECT_FLOAT_EQ(ConvertAccelerationInSecsToFrames(450.0f) * steps * steps, 450.0f);
	EXPECT_FLOAT_EQ(logicFramesFromBaseFrames(3) / steps, 0.1f); // unit path request debounce
}

TEST(SimulationTiming, DragRetainsRetailVelocityOverOneSecond)
{
	const float authoredFractions[] = { -1.0f, 0.0f, 0.00001f, 0.01f, 0.15f, 0.65f, 0.8f, 0.99f, 3.0f };
	for (const float baseFriction : authoredFractions)
	{
		SCOPED_TRACE(baseFriction);
		for (const float minimum : { 0.0f, 0.01f })
		{
			const float stepLoss = physicsFrictionForStep(logicRateFromBaseRate(baseFriction), minimum, 0.99f);
			float remaining = 1.0f;
			for (int i = 0; i < LOGICFRAMES_PER_SECOND; ++i)
				remaining *= 1.0f - stepLoss;
			const float clamped = std::fmin(0.99f, std::fmax(minimum, baseFriction));
			EXPECT_NEAR(remaining, std::pow(1.0f - clamped, BaseFps), 1e-5f);
		}
	}
}

TEST(SimulationTiming, AutomaticFireKeepsSalvosAndExplicitDeadlines)
{
	const UnsignedInt fired = 100;
	const UnsignedInt nextRetailTick = fired + logicFramesFromBaseFrames(1);
	EXPECT_FALSE(weaponIsBetweenZeroDelayShots(fired, fired, fired)); // same-tick salvo
	EXPECT_FALSE(weaponIsBetweenZeroDelayShots(1, 0, 0)); // new weapon
	EXPECT_FALSE(weaponIsBetweenZeroDelayShots(fired + 1, fired, fired + 1)); // explicit cooldown or reload
	EXPECT_FALSE(weaponIsBetweenZeroDelayShots(nextRetailTick, fired, fired));
	for (UnsignedInt frame = fired + 1; frame < nextRetailTick; ++frame)
		EXPECT_TRUE(weaponIsBetweenZeroDelayShots(frame, fired, fired));
}

TEST(SimulationTiming, WeaponBonusesPreserveRetailShotAndReloadCadence)
{
	const Int ticks = logicFramesFromBaseFrames(1);
	const Int fiftyMilliseconds = static_cast<Int>(std::ceil(ConvertDurationFromMsecsToFrames(50.0f)));
	EXPECT_EQ(2, baseFramesFromLogicDuration(fiftyMilliseconds));
	EXPECT_EQ(2 * ticks, weaponCooldownInLogicFrames(baseFramesFromLogicDuration(fiftyMilliseconds), 1.0f));
	EXPECT_EQ(ticks, weaponCooldownInLogicFrames(baseFramesFromLogicDuration(fiftyMilliseconds), 1.5f));
	EXPECT_EQ(3 * ticks, weaponCooldownInLogicFrames(5, 1.5f));
}

#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
TEST(SimulationTiming, OfflineSimulationTracksWallTimeAtDifferentRenderRates)
{
	class OfflinePacer : public FramePacer
	{
	public:
		Int advance(Real seconds)
		{
			m_updateTime = seconds;
			return takeOfflineLogicFrames(false);
		}
	};
	for (Int renderFps : {30, 45, 60, 120})
	{
		OfflinePacer pacer;
		pacer.configureGameLogicRate(LOGICFRAMES_PER_SECOND);
		Int frames = 0;
		for (Int i = 0; i < renderFps * 10; ++i)
			frames += pacer.advance(1.0f / renderFps);
		EXPECT_EQ(LOGICFRAMES_PER_SECOND * 10, frames);
		pacer.setGameHalted(true);
		EXPECT_EQ(0, pacer.advance(10.0f));
		pacer.setGameHalted(false);
		EXPECT_EQ(0, pacer.advance(-1.0f));
		pacer.setTimeFrozen(true); // Script countdowns still run while the world is frozen.
		EXPECT_EQ(1, pacer.advance(1.0f / LOGICFRAMES_PER_SECOND));
	}
}
#endif

TEST(SimulationTiming, SoundLoopsKeepTheRetailImmediateRestartThreshold)
{
	EXPECT_FALSE(AudioRequest::shouldQueueLoop(20.0f));
	EXPECT_TRUE(AudioRequest::shouldQueueLoop(34.0f));
}

#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
TEST(SimulationTiming, DelayedAudioUsesTimeSinceEnqueueAtEveryRenderRate)
{
	for (UnsignedInt fps : {30u, 60u, 120u})
	{
		SCOPED_TRACE(fps);
		AudioRequest *request = newInstance(AudioRequest);
		request->m_pendingEvent.Assign_No_Add_Ref(newInstance(DynamicAudioEventRTS));
		request->m_pendingEvent->decrementDelay(-1000.0f);
		request->m_queuedAtMs = 0xffffff00u; // The millisecond counter wraps during this delay.
		for (UnsignedInt frame = 0; frame < fps; ++frame)
			EXPECT_FALSE(request->isReadyToProcess(request->m_queuedAtMs + frame * 1000 / fps));
		EXPECT_TRUE(request->m_requiresCheckForSample);
		EXPECT_TRUE(request->isReadyToProcess(request->m_queuedAtMs + 1000));
		EXPECT_TRUE(request->isReadyToProcess(request->m_queuedAtMs + 1400));
		deleteInstance(request);
	}
}
#endif
