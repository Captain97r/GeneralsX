/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include "Common/SimulationFileProfile.h"

TEST(SimulationFileProfile, ReplaySignatureSelectsRecordedClock)
{
	using namespace SimulationFileProfile;
	EXPECT_STREQ("GENREP", replaySignature(30));
	EXPECT_EQ(30, replayRate("GENREP"));
	EXPECT_EQ(0, replayRate("BROKEN"));
#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
	EXPECT_STREQ("GX60RP", replaySignature(60));
	EXPECT_EQ(60, replayRate("GX60RP"));
#else
	EXPECT_EQ(0, replayRate("GX60RP"));
#endif
}

TEST(SimulationFileProfile, SaveMetadataAcceptsOnlySupportedVersions)
{
	using namespace SimulationFileProfile;
	EXPECT_EQ(30, saveRate(1));
	EXPECT_EQ(30, saveRate(saveVersion(30)));
	EXPECT_EQ(0, saveRate(3));
	EXPECT_EQ(0, saveRate(0x61));
#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
	EXPECT_EQ(60, saveRate(saveVersion(60)));
#else
	EXPECT_EQ(0, saveRate(0x60));
#endif
}
