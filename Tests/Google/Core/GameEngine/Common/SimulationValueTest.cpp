/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include <limits>

#include "Common/SimulationValue.h"

TEST(SimulationValue, PreviouslyLoadedTemplatesFollowMatchClock)
{
	const Int previousRate = getSimulationRate();
	prepareSimulationRateForGame(30);
	SimulationFrameValue<Int> shotDelay;
	shotDelay.setMilliseconds(250.0f);
	SimulationRateValue<Real> speed;
	speed.setPerSecond(300.0f);
	SimulationAccelerationValue<Real> acceleration;
	acceleration.setPerSecond(3600.0f);

	auto overrideDelay = shotDelay;
	overrideDelay.setMilliseconds(50.0f);
	for (const Int rate : {30, 60, 30})
	{
		prepareSimulationRateForGame(rate);
		EXPECT_EQ(rate == 30 ? 8 : 15, static_cast<Int>(shotDelay));
		EXPECT_EQ(rate == 30 ? 2 : 3, static_cast<Int>(overrideDelay));
		EXPECT_FLOAT_EQ(300.0f, static_cast<Real>(speed) * rate);
		EXPECT_FLOAT_EQ(3600.0f, static_cast<Real>(acceleration) * rate * rate);
	}
	prepareSimulationRateForGame(previousRate);
}

TEST(SimulationValue, FrameDefaultsPreserveSentinelsAndWideDurations)
{
	const Int previousRate = getSimulationRate();
	prepareSimulationRateForGame(30);
	const auto maximum = (std::numeric_limits<UnsignedShort>::max)();
	const SimulationFrameValue<Int> disabled(-1);
	const SimulationFrameValue<UnsignedShort> unbounded(maximum);
	const SimulationFrameValue<UnsignedShort> longDelay(40000);

	prepareSimulationRateForGame(60);
	EXPECT_EQ(-1, static_cast<Int>(disabled));
	EXPECT_EQ(maximum, static_cast<UnsignedShort>(unbounded));
	EXPECT_EQ(maximum, static_cast<UnsignedShort>(longDelay));
	const auto copiedDelay = longDelay;
	const SimulationFrameValue<Int> nativeTicks(15);

	prepareSimulationRateForGame(30);
	EXPECT_EQ(-1, static_cast<Int>(disabled));
	EXPECT_EQ(maximum, static_cast<UnsignedShort>(unbounded));
	EXPECT_EQ(40000, static_cast<UnsignedShort>(copiedDelay));
	EXPECT_EQ(8, static_cast<Int>(nativeTicks));
	prepareSimulationRateForGame(60);
	EXPECT_EQ(15, static_cast<Int>(nativeTicks));
	prepareSimulationRateForGame(previousRate);
}
