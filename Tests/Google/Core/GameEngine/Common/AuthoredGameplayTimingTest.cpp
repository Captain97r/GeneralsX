/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>
#include <limits>

#include "GameLogic/RailroadMotion.h"
#include "GameLogic/TeamRecruitment.h"

#if RTS_ZEROHOUR
#include "Common/ThingTemplate.h"
#include "GameClient/RadiusDecal.h"
#include "GameLogic/Module/UpdateModule.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/SpectreGunshipUpdate.h"
#endif

TEST(AuthoredGameplayTiming, RecruitmentDeadlineIncludesItsLastFrame)
{
	const UnsignedInt second = LOGICFRAMES_PER_SECOND;
	EXPECT_FALSE(teamRecruitmentExpired(second - 1, 30));
	EXPECT_FALSE(teamRecruitmentExpired(second, 30));
	EXPECT_TRUE(teamRecruitmentExpired(second + 1, 30));
	EXPECT_FALSE(teamRecruitmentExpired(second + 1, 0));
	EXPECT_FALSE(teamRecruitmentExpired(second + 1, -1));
	const Int longest = (std::numeric_limits<Int>::max)();
	const UnsignedInt last = static_cast<UnsignedInt>(longest) * logicFramesFromBaseFrames(1);
	EXPECT_FALSE(teamRecruitmentExpired(last, longest));
	EXPECT_TRUE(teamRecruitmentExpired(last + 1, longest));
}

TEST(AuthoredGameplayTiming, TrainSpeedAndTrackDistanceFollowTheAuthoredCurve)
{
	for (Real direction : {-1.0f, 1.0f})
	{
		Real speed = 0.0f, retailSpeed = 0.0f;
		Real distance = 0.0f, retailDistance = 0.0f;
		for (Int frame = 0; frame < 2 * LOGICFRAMES_PER_SECOND; ++frame)
		{
			if (RailroadMotion::isControlFrame(frame))
				speed = RailroadMotion::accelerate(speed, direction, 1.05f, 1.5f);
			distance += logicRateFromBaseRate(speed);
			if ((frame + 1) % logicFramesFromBaseFrames(1) == 0)
			{
				retailSpeed = (retailSpeed + direction * 0.02f) * 1.05f;
				if (direction * retailSpeed > 1.5f)
					retailSpeed = direction * 1.5f;
				retailDistance += retailSpeed;
				EXPECT_FLOAT_EQ(retailSpeed, speed);
				EXPECT_NEAR(retailDistance, distance, 1e-4f);
			}
		}
	}
}

#if RTS_ZEROHOUR
TEST(AuthoredGameplayTiming, SpectreTargetSweepsAtTheConfiguredWorldSpeed)
{
	SpectreGunshipUpdateModuleData settings;
	for (Real increment : {settings.m_strafingIncrement, 6.0f})
	{
		settings.m_strafingIncrement = increment;
		Coord3D aim = {0.0f, 0.0f, 0.0f};
		const Coord3D target = {30.0f * increment + 1.0f, 0.0f, 0.0f};
		for (Int tick = 0; tick < LOGICFRAMES_PER_SECOND; ++tick)
			EXPECT_FALSE(settings.advanceGattlingTarget(aim, target));
		EXPECT_FLOAT_EQ(target.x - 1.0f, aim.x);
		EXPECT_TRUE(settings.advanceGattlingTarget(aim, target));
		EXPECT_FLOAT_EQ(target.x, aim.x);
	}
}
#endif
