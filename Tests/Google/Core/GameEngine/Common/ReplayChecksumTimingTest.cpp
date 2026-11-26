/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include "Common/CRCDebug.h"
#include "GameNetwork/GameInfo.h"

TEST(ReplayChecksumTiming, NormalDefaultsHaveTheRetailPeriodInGameTime)
{
	EXPECT_EQ(100 * LOGICFRAMES_PER_SECOND, REPLAY_CRC_INTERVAL * BaseFps);
#if defined(DEBUG_CRC) && !RETAIL_COMPATIBLE_NETWORKING
	EXPECT_EQ(1, NET_CRC_INTERVAL);
#else
	EXPECT_EQ(100 * LOGICFRAMES_PER_SECOND, NET_CRC_INTERVAL * BaseFps);
#endif
}

TEST(ReplayChecksumTiming, ExplicitIntervalsAreInSimulationTicks)
{
	EXPECT_EQ(1, GameInfo::clampCRCInterval(1));
	EXPECT_EQ(37, GameInfo::clampCRCInterval(37));
	EXPECT_EQ(100, GameInfo::clampCRCInterval(100));
	EXPECT_EQ(GameInfo::defaultCRCInterval, GameInfo::clampCRCInterval(GameInfo::defaultCRCInterval + 1));
	EXPECT_EQ(GameInfo::defaultCRCInterval, GameInfo::clampCRCInterval(0));
}
