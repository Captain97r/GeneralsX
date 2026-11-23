/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>

#include "GameNetwork/LANAPI.h"

#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "GameClient/MapUtil.h"
#include "GameNetwork/LANAPICallbacks.h"
#endif

TEST(LANSimulationRate, RoomProfileRoundTrip)
{
	LANMessage message{};
	message.messageType = LANMessage::localJoinRequest;
	message.setSimulationRate(30);
	EXPECT_EQ(30, message.advertisedSimulationRate(sizeof(message)));
	EXPECT_EQ(0, message.advertisedSimulationRate(sizeof(message) - 1));
#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
	EXPECT_EQ(LANMessage::MSG_REQUEST_JOIN_RUNTIME, message.messageType);
	message.setSimulationRate(60);
	EXPECT_EQ(60, message.advertisedSimulationRate(sizeof(message)));
	message.setSimulationRate(45);
	EXPECT_EQ(0, message.advertisedSimulationRate(sizeof(message)));
	message.simulationProfile = 0x36304802; // Previous fixed-rate Zero Hour profile.
	EXPECT_EQ(0, message.advertisedSimulationRate(sizeof(message)));
#else
	EXPECT_EQ(LANMessage::MSG_REQUEST_JOIN, message.messageType);
#endif
}

#ifdef RTS_EXPERIMENTAL_60HZ_SIMULATION
namespace {
struct SavedLobbyGlobals
{
	GlobalData *data = TheWritableGlobalData;
	GameState *gameState = TheGameState;
	MapCache *maps = TheMapCache;
	LANAPI *lan = TheLAN;
	~SavedLobbyGlobals()
	{
		TheWritableGlobalData = data;
		TheGameState = gameState;
		TheMapCache = maps;
		TheLAN = lan;
	}
};

class LobbyTransport : public Transport
{
public:
	LANMessage packets[LANMessage::MSG_REQUEST_JOIN_RUNTIME + 1]{};
	Bool queueSend(UnsignedInt, UnsignedShort, const UnsignedByte *data, Int length) override
	{
		if (length != sizeof(LANMessage))
			return false;
		LANMessage message;
		memcpy(&message, data, sizeof(message));
		packets[message.messageType] = message;
		return true;
	}
};

class LobbyPeer : public LANAPI
{
public:
	static constexpr UnsignedInt hostIP = 0xc0000201, guestIP = 0xc0000202;
	Int starts = 0, optionUpdates = 0;
	LobbyPeer(UnsignedInt ip, Int rate)
	{
		TheLAN = this;
		delete m_transport;
		m_transport = new LobbyTransport;
		m_localIP = ip;
		m_inLobby = false;
		m_currentGame = new LANGameInfo;
		addGame(m_currentGame);
		m_currentGame->enterGame();
		m_currentGame->setSeed(123);
		m_currentGame->setMap("Maps\\Test\\Test.map");
		m_currentGame->setName(L"Test");
		m_currentGame->getLANSlot(0)->setState(SLOT_PLAYER, L"host", hostIP);
		m_currentGame->getLANSlot(1)->setState(SLOT_PLAYER, L"guest", guestIP);
		m_currentGame->setSimulationRate(rate);
	}
	LANMessage sent(LANMessage::Type type) const { return static_cast<LobbyTransport *>(m_transport)->packets[type]; }
	void OnGameStart() override { ++starts; }
	void OnGameOptions(UnsignedInt, Int, AsciiString) override { ++optionUpdates; }
	void receive(LANMessage message, UnsignedInt from)
	{
		TheLAN = this;
		const Int rate = message.advertisedSimulationRate(sizeof(message));
		switch (message.messageType)
		{
		case LANMessage::MSG_GAME_OPTIONS: handleGameOptions(&message, from, rate); break;
		case LANMessage::MSG_SET_ACCEPT: handleSetAccept(&message, from, rate); break;
		case LANMessage::MSG_GAME_START: handleGameStart(&message, from, rate); break;
		default: FAIL() << "Unexpected lobby packet";
		}
	}
};
}

TEST(LANSimulationRate, GuestUsesHostRateForReadyAndStart)
{
	SavedLobbyGlobals restoreOnExit;
	GlobalData data;
	TheWritableGlobalData = &data;
	GameState gameState;
	TheGameState = &gameState;
	MapCache maps;
	TheMapCache = &maps;
	for (const Int hostRate : {30, 60})
	{
		SCOPED_TRACE(hostRate);
		const Int guestRate = hostRate == 30 ? 60 : 30;
		LobbyPeer host(LobbyPeer::hostIP, hostRate), guest(LobbyPeer::guestIP, guestRate);
		LANGameInfo *hostRoom = host.GetMyGame(), *guestRoom = guest.GetMyGame();
		ASSERT_TRUE(guestRoom->adoptSimulationRate(hostRate, hostRoom->getSimulationStamp(), true));
		EXPECT_EQ(hostRate, guestRoom->getSimulationRate());
		guest.RequestAccept();
		LANMessage ready = guest.sent(LANMessage::MSG_SET_ACCEPT);
		EXPECT_EQ(hostRate, ready.advertisedSimulationRate(sizeof(ready)));
		LANMessage staleReady = ready;
		++staleReady.Accept.simulation.seed;
		host.receive(staleReady, LobbyPeer::guestIP);
		EXPECT_FALSE(hostRoom->areRemotePlayersReady());
		host.RequestGameStart();
		EXPECT_EQ(0, host.starts);
		host.receive(ready, LobbyPeer::guestIP);
		EXPECT_TRUE(hostRoom->areRemotePlayersReady());
		host.RequestGameStart();
		const LANMessage start = host.sent(LANMessage::MSG_GAME_START);
		LANMessage staleStart = start;
		++staleStart.Start.simulation.seed;
		guest.receive(staleStart, LobbyPeer::hostIP);
		EXPECT_EQ(0, guest.starts);
		guest.receive(start, LobbyPeer::hostIP);
		EXPECT_EQ(1, guest.starts);
		// Hosts built before the selector moved can still publish a newer room revision.
		LANMessage options{};
		options.messageType = LANMessage::MSG_GAME_OPTIONS;
		options.setSimulationRate(guestRate);
		options.GameOptions.simulation = hostRoom->getSimulationStamp();
		++options.GameOptions.simulation.revision;
		guest.receive(options, LobbyPeer::hostIP);
		EXPECT_EQ(guestRate, guestRoom->getSimulationRate());
		guest.receive(start, LobbyPeer::hostIP);
		EXPECT_EQ(1, guest.starts);
		const Int updates = guest.optionUpdates;
		--options.GameOptions.simulation.revision;
		guest.receive(options, LobbyPeer::hostIP);
		EXPECT_EQ(updates, guest.optionUpdates);
	}
}

TEST(LANSimulationRate, CommandBuffersFollowSuccessiveRooms)
{
	for (const Int rate : {30, 60, 30})
	{
		configureNetworkTiming(rate);
		EXPECT_EQ(4 * rate / 30, MIN_RUNAHEAD);
		EXPECT_EQ(128 * rate / 30, MAX_FRAMES_AHEAD);
		EXPECT_EQ(2 * (MAX_FRAMES_AHEAD + 1), FRAME_DATA_LENGTH);
		EXPECT_EQ(MAX_FRAMES_AHEAD / 2 + 1, FRAMES_TO_KEEP);
	}
}
#endif
