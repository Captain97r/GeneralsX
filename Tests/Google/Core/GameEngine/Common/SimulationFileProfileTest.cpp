/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include "Common/SimulationFileProfile.h"

TEST(SimulationFileProfile, ReplaySignatureSelectsRecordedClock)
{
	using namespace SimulationFileProfile;
	EXPECT_STREQ("GENREP", replaySignature(30));
	EXPECT_EQ(30, replayRate("GENREP"));
	EXPECT_EQ(0, replayRate("BROKEN"));
	EXPECT_EQ(0, replayRate("GX60RP"));
	EXPECT_STREQ("GX60R2", replaySignature(60));
	EXPECT_EQ(60, replayRate("GX60R2"));
}

TEST(SimulationFileProfile, SaveMetadataAcceptsOnlySupportedVersions)
{
	using namespace SimulationFileProfile;
	EXPECT_EQ(30, saveRate(1));
	EXPECT_EQ(30, saveRate(saveVersion(30)));
	EXPECT_EQ(0, saveRate(0));
	EXPECT_EQ(0, saveRate(3));
	EXPECT_EQ(0, saveRate(0x60));
	EXPECT_EQ(60, saveRate(saveVersion(60)));
}
