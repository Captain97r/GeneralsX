/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>
#include <memory>

#include "GameNetwork/LANAPI.h"
#include "GameNetwork/NetworkInterface.h"

TEST(FixedLANProfile, AcceptsOnlyTheMatchingEditionAndRules)
{
	LANMessage packet{};
	packet.simulationProfile = LANMessage::native60Profile;
	EXPECT_TRUE(packet.matchesSimulationProfile(sizeof(packet)));
	EXPECT_FALSE(packet.matchesSimulationProfile(sizeof(packet) - 1));
	EXPECT_FALSE(packet.matchesSimulationProfile(sizeof(packet) + 1));

	// Previous experimental builds and the other edition simulate different rules.
	const uint32_t incompatibleProfiles[] = {
		0, 0x36304801, 0x36304802,
#if RTS_ZEROHOUR
		0x474E3602
#else
		0x5A483603
#endif
	};
	for (uint32_t incompatible : incompatibleProfiles)
	{
		packet.simulationProfile = incompatible;
		EXPECT_FALSE(packet.matchesSimulationProfile(sizeof(packet)));
	}
}

TEST(FixedLANProfile, OnlineAndLANSelectTheirOwnClocks)
{
	const std::unique_ptr<NetworkInterface> online(NetworkInterface::createNetwork());
	const std::unique_ptr<NetworkInterface> lan(NetworkInterface::createNetwork(NativeFps));
	EXPECT_EQ(30, online->getNativeSimulationRate());
	EXPECT_EQ(60, lan->getNativeSimulationRate());
}
