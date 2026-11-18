#include "PreRTS.h"

#if defined(RTS_ENABLE_MCP_BRIDGE)

#include "Common/AgentSkirmishTest.h"
#include "Common/AsciiString.h"
#include "Common/BuildAssistant.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Money.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/RandomValue.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameNetwork/GameInfo.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace rts
{
namespace
{
bool s_configured = false;
bool s_enabled = false;
bool s_started = false;
bool s_fixtureAdded = false;
bool s_expired = false;
bool s_sessionOnly = false;
// No owning global STL objects: CRT destructors run after the game allocator
// has shut down. These test settings must remain valid without heap storage.
char s_map[MAX_PATH] = {};
char s_userData[MAX_PATH + 1] = {};
unsigned s_timeoutSeconds = 120;
std::chrono::steady_clock::time_point s_startTime;

void failTest(const char* reason)
{
	std::fprintf(stderr, "Agent skirmish test: %s\n", reason);
	std::fflush(stderr);
	std::exit(2);
}

bool isAbsoluteDirectory(const char* path)
{
	if (!path)
		return false;
	const size_t length = std::strlen(path);
	if (length < 3 || length >= MAX_PATH)
		return false;
	std::error_code error;
	const std::filesystem::path directory(path);
	return directory.is_absolute() && std::filesystem::is_directory(directory, error);
}

void configureTest()
{
	if (s_configured)
		return;
	s_configured = true;
	const char* map = std::getenv("GENERALS_MCP_TEST_MAP");
	const char* sessionMode = std::getenv("GENERALS_MCP_TEST_SESSION");
	s_sessionOnly = sessionMode && std::strcmp(sessionMode, "1") == 0;
	if ((!map || !*map) && !s_sessionOnly)
		return;
	if (!map) map = "";
	const size_t mapLength = std::strlen(map);
	if (mapLength >= sizeof(s_map))
		failTest("GENERALS_MCP_TEST_MAP must be shorter than MAX_PATH");

	const char* pipe = std::getenv("GENERALS_MCP_PIPE");
	if (!pipe || !*pipe)
		failTest("GENERALS_MCP_PIPE is required with GENERALS_MCP_TEST_MAP");
	if (std::strlen(pipe) > 64)
		failTest("GENERALS_MCP_PIPE must be a valid bridge suffix");
	for (const char* c = pipe; *c; ++c)
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
			(*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.'))
			failTest("GENERALS_MCP_PIPE must be a valid bridge suffix");
	const char* directory = std::getenv("GENERALS_MCP_TEST_USER_DATA");
	if (!isAbsoluteDirectory(directory))
		failTest("GENERALS_MCP_TEST_USER_DATA must be an absolute existing directory");
	const char* timeout = std::getenv("GENERALS_MCP_TEST_TIMEOUT_SECONDS");
	if (timeout)
	{
		unsigned seconds = 0;
		for (const char* c = timeout; *c; ++c)
		{
			if (*c < '0' || *c > '9')
				failTest("GENERALS_MCP_TEST_TIMEOUT_SECONDS must be an integer from 1 to 120");
			seconds = seconds * 10 + static_cast<unsigned>(*c - '0');
			if (seconds > (s_sessionOnly ? 600u : 120u))
				failTest("test timeout exceeds the allowed maximum");
		}
		if (seconds == 0)
			failTest("GENERALS_MCP_TEST_TIMEOUT_SECONDS must be an integer from 1 to 120");
		s_timeoutSeconds = seconds;
	}

	const std::string fullPath = std::filesystem::path(directory).lexically_normal().make_preferred().string();
	const size_t length = fullPath.size();
	if (length == 0 || length >= MAX_PATH)
		failTest("cannot resolve the private user-data directory");
	std::memcpy(s_userData, fullPath.c_str(), length + 1);
	if (s_userData[length - 1] != '\\')
	{
		s_userData[length] = '\\';
		s_userData[length + 1] = '\0';
	}
	std::memcpy(s_map, map, mapLength + 1);
	for (size_t i = 0; i < mapLength; ++i)
		if (s_map[i] == '/')
			s_map[i] = '\\';

	// GlobalData resolves its user directory before the instance mutex is
	// acquired. Only this explicitly configured test process gets another ID.
	ClientInstance::setMultiInstance(true);
	ClientInstance::skipPrimaryInstance();
	s_enabled = true;
}

Int findFaction(const char* name)
{
	for (Int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
	{
		const PlayerTemplate* faction = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (faction && std::strcmp(faction->getName().str(), name) == 0 &&
			faction->getStartingBuilding().isNotEmpty())
			return i;
	}
	return -1;
}

void startTestGame()
{
	if (!TheGlobalData->m_headless)
		failTest("the test requires -headless");
	if (!TheMapCache || !ThePlayerTemplateStore || !TheMessageStream)
		failTest("game subsystems are not initialized");
	if (TheGameLogic->isInGame())
	{
		if (!TheGameLogic->isInShellGame())
			failTest("refusing to replace an existing match");
		TheGameLogic->clearGameData(FALSE);
	}

	const AsciiString map(s_map);
	const MapMetaData* metadata = TheMapCache->findMap(map);
	if (!metadata || !metadata->m_isMultiplayer || metadata->m_numPlayers < 2)
		failTest("the requested map is not a cached multiplayer map with two starts");
	const Int america = findFaction("FactionAmerica");
	const Int gla = findFaction("FactionGLA");
	if (america < 0 || gla < 0)
		failTest("the loaded mod lacks playable FactionAmerica or FactionGLA");

	if (!TheSkirmishGameInfo)
		TheSkirmishGameInfo = NEW SkirmishGameInfo;
	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->enterGame();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->setLocalIP(0);

	GameSlot human;
	human.setState(SLOT_PLAYER, UnicodeString(L"MCP Smoke Test"), 0);
	human.setPlayerTemplate(america);
	human.setColor(0);
	human.setStartPos(0);
	human.setTeamNumber(-1);
	TheSkirmishGameInfo->setSlot(0, human);
	GameSlot opponent;
	opponent.setState(SLOT_EASY_AI);
	opponent.setPlayerTemplate(gla);
	opponent.setColor(1);
	opponent.setStartPos(1);
	opponent.setTeamNumber(-1);
	TheSkirmishGameInfo->setSlot(1, opponent);

	Money cash;
	cash.setStartingCash(50000);
	TheSkirmishGameInfo->setStartingCash(cash);
	TheSkirmishGameInfo->setUseStats(FALSE);
	TheSkirmishGameInfo->setSuperweaponRestriction(0);
	TheSkirmishGameInfo->setSeed(12345);
	TheSkirmishGameInfo->setMap(map);
	TheSkirmishGameInfo->setMapCRC(metadata->m_CRC);
	TheSkirmishGameInfo->setMapSize(metadata->m_filesize);
	TheWritableGlobalData->m_mapName = map;
	TheSkirmishGameInfo->startGame(0);
	InitRandom(TheSkirmishGameInfo->getSeed());
	GameMessage* message = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	message->appendIntegerArgument(GAME_SKIRMISH);
	message->appendIntegerArgument(DIFFICULTY_NORMAL);
	message->appendIntegerArgument(0);
	message->appendIntegerArgument(30);
	std::fprintf(stderr, "Agent skirmish test: starting %s; deadline %u seconds\n", s_map, s_timeoutSeconds);
	std::fflush(stderr);
}

Object* addBuildingFixture(const ThingTemplate* building, Object* commandCenter, Player* player)
{
	const Int directions[8][2] = {
		{1, 0}, {-1, 0}, {0, 1}, {0, -1},
		{1, 1}, {-1, 1}, {1, -1}, {-1, -1}
	};
	const Real angle = building->getPlacementViewAngle();
	for (Int distance = 120; distance <= 480; distance += 60)
		for (Int direction = 0; direction < 8; ++direction)
		{
			Coord3D position = *commandCenter->getPosition();
			position.x += distance * directions[direction][0];
			position.y += distance * directions[direction][1];
			if (TheBuildAssistant->isLocationLegalToBuild(&position, building, angle,
				BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::NO_OBJECT_OVERLAP,
				nullptr, player) != LBC_OK)
				continue;

			// Existing script placement completes construction, registers the
			// building with its player/pathfinder, and runs creation callbacks.
			// This fixture exists only in the isolated opt-in test process.
			Object* fixture = TheBuildAssistant->buildObjectNow(nullptr, building, &position, angle, player);
			if (!fixture)
				continue;
			// Script-built structures leave these final state changes to their
			// caller (as AIPlayer::buildStructure does). Completion callbacks
			// already ran above; do not repeat them or account for power twice.
			fixture->clearStatus(MAKE_OBJECT_STATUS_MASK2(
				OBJECT_STATUS_UNDER_CONSTRUCTION, OBJECT_STATUS_RECONSTRUCTING));
			fixture->clearModelConditionFlags(MAKE_MODELCONDITION_MASK3(
				MODELCONDITION_AWAITING_CONSTRUCTION, MODELCONDITION_PARTIALLY_CONSTRUCTED,
				MODELCONDITION_ACTIVELY_BEING_CONSTRUCTED));
			fixture->setConstructionPercent(CONSTRUCTION_COMPLETE);
			fixture->updateUpgradeModules();
			fixture->handlePartitionCellMaintenance();
			std::fprintf(stderr, "Agent skirmish test: fixture %s object %u at %.1f, %.1f\n",
				building->getName().str(), static_cast<unsigned>(fixture->getID()), position.x, position.y);
			std::fflush(stderr);
			return fixture;
		}
	failTest("no legal fixture location near the local command center");
	return nullptr;
}

void addStartingFixtures()
{
	Player* player = ThePlayerList->getLocalPlayer();
	Object* commandCenter = nullptr;
	for (Object* object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
		if (object->getControllingPlayer() == player && object->isKindOf(KINDOF_COMMANDCENTER))
		{
			commandCenter = object;
			break;
		}
	if (!commandCenter)
		failTest("the loaded skirmish has no local command center for its fixture");

	const ThingTemplate* reactor = TheThingFactory->findTemplate("Cybr_AmericaPowerPlant", FALSE);
	if (!reactor)
		reactor = TheThingFactory->findTemplate("AmericaPowerPlant", FALSE);
	const ThingTemplate* barracks = TheThingFactory->findTemplate("Cybr_AmericaBarracks", FALSE);
	if (!barracks)
		barracks = TheThingFactory->findTemplate("AmericaBarracks", FALSE);
	if (!reactor || !reactor->isKindOf(KINDOF_STRUCTURE) ||
		!barracks || !barracks->isKindOf(KINDOF_STRUCTURE))
		failTest("the loaded mod lacks the USA power and barracks fixture templates");

	addBuildingFixture(reactor, commandCenter, player);
	Object* producer = addBuildingFixture(barracks, commandCenter, player);
	if (!producer->getProductionUpdateInterface())
		failTest("the barracks fixture has no production interface");
	s_fixtureAdded = true;
}
}

bool agentSkirmishTestUserData(AsciiString& directory)
{
	configureTest();
	if (!s_enabled)
		return false;
	directory = s_userData;
	return true;
}

void updateAgentSkirmishTest()
{
	configureTest();
	if (!s_enabled || s_expired)
		return;
	if (!s_started)
	{
		s_started = true;
		s_startTime = std::chrono::steady_clock::now();
		if (s_sessionOnly)
		{
			const char* windowed = std::getenv("GENERALS_MCP_TEST_WINDOWED");
			const bool explicitWindowedTest = windowed && std::strcmp(windowed, "1") == 0;
			if (!TheGlobalData->m_headless && !(explicitWindowedTest && TheGlobalData->m_windowed))
				failTest("session tests require -headless or explicit GENERALS_MCP_TEST_WINDOWED=1 with -win");
		}
		else
			startTestGame();
	}
	if (!s_sessionOnly && !s_fixtureAdded && TheGameLogic->isInSkirmishGame() && TheGameLogic->getFrame() >= 2)
		addStartingFixtures();
	if (std::chrono::steady_clock::now() - s_startTime >= std::chrono::seconds(s_timeoutSeconds))
	{
		s_expired = true;
		std::fprintf(stderr, "Agent skirmish test: deadline reached; exiting test process\n");
		std::fflush(stderr);
		TheGameEngine->setQuitting(TRUE);
	}
}
}

#endif
