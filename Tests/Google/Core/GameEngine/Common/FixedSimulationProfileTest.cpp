/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include "Common/SimulationFileProfile.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/networkutil.h"

TEST(FixedSimulationProfile, StockAndExperimentalFilesUseDifferentClocks)
{
	using namespace SimulationFileProfile;
	ASSERT_EQ(6, signatureLength);
	EXPECT_TRUE(acceptsReplay(replaySignature, signatureLength));
	EXPECT_FALSE(acceptsReplay(replaySignature, signatureLength - 1));
	EXPECT_FALSE(acceptsReplay("GX60RP", signatureLength));
	EXPECT_FALSE(acceptsReplay("GX60R2", signatureLength));
	EXPECT_FALSE(acceptsReplay("GX60Z3", signatureLength));
	EXPECT_FALSE(acceptsReplay("GX60G3", signatureLength));
	EXPECT_TRUE(acceptsSave(saveVersion));
	EXPECT_FALSE(acceptsSave(0x60));
	EXPECT_FALSE(acceptsSave(0x61));
#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
	EXPECT_FALSE(acceptsReplay("GENREP", signatureLength));
	EXPECT_FALSE(acceptsSave(1));
	EXPECT_FALSE(acceptsSave(2));
#else
	EXPECT_STREQ("GENREP", replaySignature);
	EXPECT_EQ(2, saveVersion);
	EXPECT_TRUE(acceptsSave(1));
	EXPECT_FALSE(acceptsReplay("GX60Z4", signatureLength));
	EXPECT_FALSE(acceptsReplay("GX60G4", signatureLength));
	EXPECT_FALSE(acceptsSave(0x62));
	EXPECT_FALSE(acceptsSave(0x63));
#endif
}

TEST(FixedSimulationProfile, NetworkNegotiationRespectsTheEffectiveRenderCap)
{
	for (Int storedCap : {0, 30, 50, 56, 60, 120})
	{
		SCOPED_TRACE(storedCap);
		EXPECT_EQ(LOGICFRAMES_PER_SECOND, clampNetworkFrameRate(144, storedCap));
		EXPECT_EQ(20, clampNetworkFrameRate(20, storedCap));
	}
	EXPECT_EQ(LOGICFRAMES_PER_SECOND == BaseFps ? 20 : 60, clampNetworkFrameRate(144, 20));
}

TEST(FixedSimulationProfile, LANJoinUsesMatchingOpcodeAndPacketLayout)
{
	LANMessage packet{};
#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
	EXPECT_EQ(475u, sizeof(packet));
	EXPECT_EQ(LANMessage::MSG_REQUEST_JOIN_60HZ, LANMessage::localJoinRequest);
	EXPECT_FALSE(packet.matchesSimulationProfile(sizeof(packet)));
	for (uint32_t olderProfile : {0x5A483604u, 0x474E3603u})
	{
		packet.simulationProfile = olderProfile;
		EXPECT_FALSE(packet.matchesSimulationProfile(sizeof(packet)));
	}
	packet.simulationProfile = LANMessage::native60Profile;
#else
	EXPECT_EQ(471u, sizeof(packet));
	EXPECT_EQ(LANMessage::MSG_REQUEST_JOIN, LANMessage::localJoinRequest);
#endif
	EXPECT_TRUE(packet.matchesSimulationProfile(sizeof(packet)));
	EXPECT_FALSE(packet.matchesSimulationProfile(sizeof(packet) - 1));
	EXPECT_FALSE(packet.matchesSimulationProfile(sizeof(packet) + 1));
}
