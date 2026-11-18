#include "PreRTS.h"
#include "Common/AgentGameInternal.h"
#include "AgentFileReader.h"
#include "Common/AgentBridge.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Money.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Shell.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace GeneralsAgent
{
namespace
{
using namespace Detail;

struct SessionState
{
    std::string action, request, status = "idle", error;
    std::vector<std::string> arguments;
    bool pending = false;
    bool pauseWhenReady = false;
    bool waitForReady = false;
    bool expectingReplay = false;
    unsigned advanceUntil = 0;
    std::chrono::steady_clock::time_point deadline;
    std::string result;
    std::string lastMatchResult = "unknown";
    unsigned lastMatchFrame = 0;
    bool wasInMatch = false;
};
SessionState* s_session = nullptr;

SessionState& session()
{
    if (!s_session)
        s_session = new SessionState;
    return *s_session;
}

bool integer(const std::string& text, unsigned maximum, unsigned& result)
{
    if (text.empty() || text.size() > 10)
        return false;
    result = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9' || result > (maximum - unsigned(c - '0')) / 10)
            return false;
        result = result * 10 + unsigned(c - '0');
    }
    return result <= maximum;
}

bool saveLeaf(const std::string& input, std::string& leaf)
{
    leaf = input;
    if (leaf.size() > 4 && leaf.substr(leaf.size() - 4) == ".sav")
        leaf.resize(leaf.size() - 4);
    if (leaf.empty() || leaf.size() > 80)
        return false;
    for (char c : leaf)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    leaf += ".sav";
    return true;
}

bool multiplayerOrReplay()
{
    return TheGameLogic && (TheGameLogic->isInMultiplayerGame() ||
        TheGameLogic->isInReplayGame() || (TheRecorder && TheRecorder->isPlaybackMode()));
}

bool multiplayerMatch()
{
    return TheGameLogic && TheGameLogic->isInMultiplayerGame() && !TheGameLogic->isInReplayGame();
}

void rememberMatchResult()
{
    SessionState& state = session();
    if (TheRecorder && TheRecorder->isPlaybackMode()) return;
    if (matchReady())
    {
        Player* local = localPlayer();
        state.lastMatchFrame = TheGameLogic->getFrame();
        state.wasInMatch = true;
        state.lastMatchResult = "in_progress";
        if (TheVictoryConditions && TheVictoryConditions->hasAchievedVictory(local))
            state.lastMatchResult = "victory";
        else if (local->isPlayerDead() || (TheVictoryConditions &&
            (TheVictoryConditions->hasBeenDefeated(local) || TheVictoryConditions->isLocalDefeat())))
            state.lastMatchResult = "defeat";
        else if (local->isPlayerObserver()) state.lastMatchResult = "observing";
    }
    else if (state.wasInMatch)
    {
        state.wasInMatch = false;
        if (state.lastMatchResult == "in_progress" || state.lastMatchResult == "observing")
            state.lastMatchResult = "ended";
    }
}

// GeneralsX @feature Keep replay operations inside the game's replay directory and preserve existing recordings.
bool replayLeaf(const std::string& input, std::string& leaf)
{
    leaf = input;
    if (leaf.size() > 4 && _stricmp(leaf.substr(leaf.size() - 4).c_str(), ".rep") == 0)
        leaf.resize(leaf.size() - 4);
    if (leaf.empty() || leaf.size() > 80) return false;
    for (char c : leaf)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    const char* devices[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* device : devices) if (_stricmp(leaf.c_str(), device) == 0) return false;
    leaf += ".rep";
    return true;
}

struct ReplayFile
{
    AgentFileReader reader;
    unsigned start = 0, end = 0, frames = 0;
    unsigned long long bytes = 0;
    std::string error;
    bool finalized() const { return start && start <= 0x7fffffffU && end >= start && end <= 0x7fffffffU && frames; }
};

std::string replayPath(const std::string& leaf)
{
    return std::string(RecorderClass::getReplayDir().str()) + leaf;
}

std::string automaticReplayName()
{
    return std::string(RecorderClass::getLastReplayFileName().str()) + ".rep";
}

bool openReplay(const std::string& leaf, ReplayFile& file, bool requireClosed)
{
    if (!file.reader.open(replayPath(leaf), requireClosed, file.error))
        return false;
    file.bytes = file.reader.size();
    unsigned char header[18];
    std::size_t read = 0;
    if (file.bytes <= sizeof(header) || file.bytes > 256ULL * 1024 * 1024 ||
        !file.reader.readPrefix(header, sizeof(header), read) || read != sizeof(header) ||
        std::memcmp(header, "GENREP", 6) != 0)
    {
        file.error = "invalid_replay";
        return false;
    }
    std::memcpy(&file.start, header + 6, 4);
    std::memcpy(&file.end, header + 10, 4);
    std::memcpy(&file.frames, header + 14, 4);
    return true;
}

std::string replayJson(const std::string& name, const ReplayFile& file)
{
    auto out = jsonStream();
    out << "{\"name\":" << quote(name) << ",\"bytes\":" << file.bytes
        << ",\"start_time\":" << file.start << ",\"end_time\":" << file.end
        << ",\"frame_count\":" << file.frames << ",\"finalized\":" << file.finalized() << '}';
    return out.str();
}

std::string replayStatus(const std::string& id)
{
    if (!TheRecorder || !TheGlobalData) return failure(id, "not_ready", "The replay system is not initialized.");
    rememberMatchResult();
    const bool playing = TheRecorder->isPlaybackMode();
    const bool recording = matchReady() && !playing && TheRecorder->getMode() == RECORDERMODETYPE_RECORD;
    const std::string name = automaticReplayName();
    ReplayFile file;
    const bool valid = openReplay(name, file, !recording);
    auto out = jsonStream();
    out << "{\"recording\":" << recording << ",\"playing\":" << playing
        << ",\"playback_name\":" << (playing ? quote(TheRecorder->getCurrentReplayFilename().str()) : "null")
        << ",\"playback_frame_count\":" << (playing ? TheRecorder->getPlaybackFrameCount() : 0)
        << ",\"automatic_name\":" << quote(name) << ",\"can_save\":"
        << (valid && file.finalized() && !recording && !playing && !matchReady())
        << ",\"last_replay\":" << (valid ? replayJson(name, file) : "null")
        << ",\"last_match_result\":" << quote(session().lastMatchResult)
        << ",\"last_match_frame\":" << session().lastMatchFrame
        << ",\"error\":" << (valid ? "null" : quote(file.error)) << '}';
    return success(id, out.str());
}

std::string listReplays(const std::string& id)
{
    if (!TheRecorder || !TheGlobalData) return failure(id, "not_ready", "The replay system is not initialized.");
    auto out = jsonStream();
    out << "{\"replays\":[";
    unsigned count = 0;
    bool truncated = false;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(replayPath(""), ec), end; !ec && it != end; it.increment(ec))
    {
        const auto name = it->path().filename().u8string();
        std::string leaf(name.begin(), name.end());
        if (leaf.size() <= 4 || _stricmp(leaf.c_str() + leaf.size() - 4, ".rep") != 0 ||
            !replayLeaf(leaf, leaf)) continue;
        if (count == 512) { truncated = true; break; }
        ReplayFile file;
        if (!openReplay(leaf, file, false)) continue;
        if (count++) out << ',';
        out << replayJson(leaf, file);
    }
    out << "],\"truncated\":" << truncated << '}';
    return success(id, out.str());
}

std::string copyFinalReplay(const std::string& leaf)
{
    const std::string original = automaticReplayName();
    ReplayFile source;
    if (!openReplay(original, source, true)) throw std::runtime_error(source.error);
    if (!source.finalized()) throw std::runtime_error("Replay has no finalized end timestamp and frame count.");
    if (!source.reader.copyToNewFile(replayPath(leaf)))
        throw std::runtime_error("Could not copy the replay to a new file.");
    return replayJson(leaf, source);
}

// Reject truncated/overlong header strings before the legacy Recorder parser reads them.
bool completeReplayHeader(ReplayFile& file)
{
    unsigned char data[16384];
    std::size_t count = 0;
    if (!file.reader.readPrefix(data, sizeof(data), count)) return false;
    size_t offset = 18 + (MAX_SLOTS + 2) * sizeof(Bool);
    auto string = [&](unsigned width)
    {
        for (unsigned length = 0; length < 1023; ++length)
        {
            if (offset + width > count) return false;
            const bool terminator = data[offset] == 0 && (width == 1 || data[offset + 1] == 0);
            offset += width;
            if (terminator) return true;
        }
        return false;
    };
    if (!string(sizeof(WideChar))) return false;
    offset += 8 * sizeof(std::uint16_t); // Eight 16-bit date/time fields.
    if (!string(sizeof(WideChar)) || !string(sizeof(WideChar))) return false;
    offset += 3 * sizeof(unsigned);
    if (!string(1) || !string(1)) return false;
    return offset + 5 * sizeof(unsigned) <= count; // Four setup values and the first command frame.
}

void validatePlayback(const std::string& leaf, ReplayFile& file)
{
    if (!openReplay(leaf, file, true)) throw std::runtime_error(file.error);
    if (!file.finalized() || !completeReplayHeader(file))
        throw std::runtime_error("Replay is incomplete or has an invalid header.");
    RecorderClass::ReplayHeader header;
    if (!TheRecorder->readReplayHeader(header, AsciiString(leaf.c_str()), FALSE))
        throw std::runtime_error("The engine rejected the replay header.");
    if (!RecorderClass::replayMatchesGameVersion(header))
        throw std::runtime_error("Replay version or game-data checksum does not match this executable.");
    ReplayGameInfo info;
    if (!ParseAsciiStringToGameInfo(&info, header.gameOptions))
        throw std::runtime_error("Replay game options are invalid.");
    const MapMetaData* map = TheMapCache ? TheMapCache->findMap(info.getMap()) : nullptr;
    if (!map || !map->m_doesExist) throw std::runtime_error("The replay map is not installed.");
}

int factionIndex(const std::string& name)
{
    if (!ThePlayerTemplateStore)
        return -1;
    for (int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
    {
        const PlayerTemplate* type = ThePlayerTemplateStore->getNthPlayerTemplate(i);
        if (type && type->getName().str() == name && type->getStartingBuilding().isNotEmpty())
            return i;
    }
    return -1;
}

std::string sessionJson()
{
    const SessionState& state = session();
    auto out = jsonStream();
    out << "{\"status\":" << quote(state.status) << ",\"action\":" << quote(state.action)
        << ",\"request_id\":" << quote(state.request) << ",\"pending\":" << state.pending
        << ",\"error\":" << (state.error.empty() ? "null" : quote(state.error))
        << ",\"result\":" << (state.result.empty() ? "null" : state.result)
        << ",\"frame\":" << (TheGameLogic ? TheGameLogic->getFrame() : 0)
        << ",\"paused\":" << bool(TheGameLogic && TheGameLogic->isGamePaused())
        << ",\"advance_until_frame\":" << state.advanceUntil << '}';
    return out.str();
}

void closePauseMenu()
{
    if (TheInGameUI && TheInGameUI->isQuitMenuVisible())
        destroyQuitMenu();
}

std::string queueOperation(const std::string& id, const std::string& action,
    const std::vector<std::string>& args)
{
    SessionState& state = session();
    if (state.pending || state.status == "loading")
        return failure(id, "session_busy", "A session operation is already pending; inspect get_session.");
    if (state.advanceUntil)
        return failure(id, "game_advancing", "Wait for advance_game to finish or pause explicitly before a session operation.");
    state.action = action;
    state.request = id;
    state.arguments = args;
    state.error.clear();
    state.result.clear();
    state.pending = true;
    state.pauseWhenReady = false;
    state.waitForReady = false;
    state.expectingReplay = false;
    state.advanceUntil = 0;
    state.status = "queued";
    return success(id, sessionJson());
}

std::string listMaps(const std::string& id)
{
    if (!TheMapCache || !ThePlayerTemplateStore)
        return failure(id, "not_ready", "Map and faction definitions are not loaded yet.");
    auto out = jsonStream();
    out << "{\"maps\":[";
    unsigned count = 0;
    bool truncated = false;
    for (const auto& item : *TheMapCache)
    {
        const MapMetaData& map = item.second;
        if (!map.m_isMultiplayer || map.m_numPlayers < 2 || !map.m_doesExist)
            continue;
        if (count == 512) { truncated = true; break; }
        if (count++) out << ',';
        out << "{\"name\":" << quote(item.first.str()) << ",\"players\":" << map.m_numPlayers
            << ",\"official\":" << bool(map.m_isOfficial) << '}';
    }
    out << "],\"truncated\":" << truncated << ",\"factions\":[";
    bool first = true;
    for (int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
    {
        const PlayerTemplate* type = ThePlayerTemplateStore->getNthPlayerTemplate(i);
        if (!type || type->getStartingBuilding().isEmpty()) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"name\":" << quote(type->getName().str()) << ",\"side\":"
            << quote(type->getSide().str()) << ",\"starting_building\":"
            << quote(type->getStartingBuilding().str()) << '}';
    }
    out << "]}";
    return success(id, out.str());
}

std::string listSaves(const std::string& id)
{
    if (!TheGameState)
        return failure(id, "not_ready", "Save system is not initialized.");
    auto out = jsonStream();
    out << "{\"saves\":[";
    unsigned count = 0;
    bool truncated = false;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(TheGameState->getSaveDirectory().str(), ec), end;
         !ec && it != end; it.increment(ec))
    {
        const auto name = it->path().filename().u8string();
        std::string leaf(name.begin(), name.end());
        if (!leaf.ends_with(".sav") || !saveLeaf(leaf, leaf)) continue;
        std::error_code sizeError;
        if (!std::filesystem::is_regular_file(it->symlink_status(sizeError))) continue;
        const auto bytes = it->file_size(sizeError);
        if (sizeError) continue;
        if (count == 512) { truncated = true; break; }
        if (count++) out << ',';
        out << "{\"name\":" << quote(leaf) << ",\"bytes\":" << bytes << '}';
    }
    out << "],\"truncated\":" << truncated << '}';
    return success(id, out.str());
}

void startSkirmish(const std::vector<std::string>& args)
{
    ResetObservationMatch();
    if (TheGameLogic->isInGame())
        TheGameLogic->clearGameData(FALSE);
    const AsciiString map(args[0].c_str());
    const MapMetaData* metadata = TheMapCache->findMap(map);
    if (!metadata)
        throw std::runtime_error("Map disappeared before start.");
    if (!TheSkirmishGameInfo)
        TheSkirmishGameInfo = NEW SkirmishGameInfo;
    TheSkirmishGameInfo->init();
    TheSkirmishGameInfo->enterGame();
    TheSkirmishGameInfo->clearSlotList();
    TheSkirmishGameInfo->setLocalIP(0);
    GameSlot human;
    human.setState(SLOT_PLAYER, UnicodeString(L"MCP Player"), 0);
    human.setPlayerTemplate(factionIndex(args[1]));
    human.setColor(0);
    human.setStartPos(0);
    human.setTeamNumber(-1);
    TheSkirmishGameInfo->setSlot(0, human);
    GameSlot opponent;
    opponent.setState(args[3] == "easy" ? SLOT_EASY_AI : args[3] == "medium" ? SLOT_MED_AI : SLOT_BRUTAL_AI);
    opponent.setPlayerTemplate(factionIndex(args[2]));
    opponent.setColor(1);
    opponent.setStartPos(1);
    opponent.setTeamNumber(-1);
    TheSkirmishGameInfo->setSlot(1, opponent);
    unsigned cashValue = 0;
    integer(args[4], 1000000, cashValue);
    Money money;
    money.setStartingCash(cashValue);
    TheSkirmishGameInfo->setStartingCash(money);
    TheSkirmishGameInfo->setUseStats(FALSE);
    TheSkirmishGameInfo->setSuperweaponRestriction(0);
    TheSkirmishGameInfo->setSeed(timeGetTime());
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
}
}

std::string ExecuteSession(const std::string& id, const std::string& method,
    const std::vector<std::string>& args)
{
    using namespace Detail;
    const bool noArgs = method == "get_capabilities" || method == "get_session" ||
        method == "list_maps" || method == "list_saves" || method == "end_match" ||
        method == "get_replay_status" || method == "list_replays";
    const bool oneArg = method == "set_paused" || method == "advance_game" ||
        method == "save_game" || method == "load_game" || method == "save_replay" || method == "play_replay";
    if (!noArgs && !oneArg && method != "start_skirmish")
        return "";
    if (args.size() != (noArgs ? 0 : oneArg ? 1 : 5))
        return failure(id, "invalid_argument", "Incorrect number of session arguments.");
    if (method == "get_capabilities")
    {
        auto out = jsonStream();
        out << "{\"version\":\"0.3.0\",\"engine\":\"GeneralsX\"";
        auto methods = [&out](const char* group, std::initializer_list<const char*> names)
        {
            out << ',' << quote(group) << ":[";
            bool first = true;
            for (const char* name : names)
            {
                if (!first) out << ',';
                first = false;
                out << quote(name);
            }
            out << ']';
        };
        methods("observations", {"get_state", "get_actions", "get_world", "get_object", "get_map",
            "get_events", "get_players", "get_player_actions", "get_snapshot", "get_unit_profile", "check_order"});
        methods("commands", {"train_unit", "attack_move", "unit_order", "build", "build_line",
            "check_build", "find_build_sites", "use_command", "cancel_production", "purchase_science",
            "set_rally_point", "batch_orders"});
        methods("session", {"get_session", "list_maps", "start_skirmish", "set_paused", "advance_game",
            "list_saves", "save_game", "load_game", "end_match", "set_control_mode",
            "get_replay_status", "list_replays", "save_replay", "play_replay"});
        methods("lobby", {"get_lobby", "enter_lan", "lan_host", "lan_join", "lan_accept", "lan_start",
            "lan_leave", "lan_set_slot", "lan_set_options"});
        out << ",\"visibility\":\"local_player_fog_and_stealth\",\"orders\":\"normal_game_message_pipeline\","
            "\"session_control\":\"singleplayer_only\",\"requires_images\":false,"
            "\"control_mode\":" << quote(IsRealtimeControl() ? "realtime" : "stepped")
            << ",\"allowed_stepping\":" << !IsRealtimeControl() << ',' <<
            "\"headless_skirmish\":true,\"headless_lan_lobby\":false,\"headless\":"
            << bool(TheGlobalData && TheGlobalData->m_headless) << '}';
        return success(id, out.str());
    }
    if (method == "get_session") return success(id, sessionJson());
    if (method == "list_maps") return listMaps(id);
    if (method == "list_saves") return listSaves(id);
    if (method == "get_replay_status") return replayStatus(id);
    if (method == "list_replays") return listReplays(id);
    if (!TheGameLogic || !TheMessageStream || !TheGameState)
        return failure(id, "not_ready", "Game session systems are not initialized.");
    const bool replayControl = method == "end_match" || method == "set_paused" || method == "advance_game";
    if (multiplayerMatch() || (multiplayerOrReplay() && !replayControl))
        return failure(id, "singleplayer_only", "This session control is unavailable during multiplayer or replay.");
    if (TheLAN && (method == "start_skirmish" || method == "load_game" || method == "play_replay"))
        return failure(id, "leave_lan_first", "Leave the LAN menus before starting or loading a single-player match.");
    if (session().pending || session().status == "loading")
        return failure(id, "session_busy", "Inspect get_session until the pending operation completes.");
    if (method == "save_replay" || method == "play_replay")
    {
        if (!TheRecorder || !TheGlobalData)
            return failure(id, "not_ready", "The replay system is not initialized.");
        if (matchReady())
            return failure(id, "match_active", "End the current match before saving or playing a replay.");
        std::string leaf;
        if (!replayLeaf(args[0], leaf))
            return failure(id, "invalid_argument", "Use 1-80 ASCII letters, digits, underscores, or hyphens, optionally ending in .rep; device names are reserved.");
        ReplayFile file;
        if (!openReplay(method == "save_replay" ? automaticReplayName() : leaf, file, true))
            return failure(id, file.error.c_str(), "The requested replay is unavailable or still open for writing.");
        if (!file.finalized())
            return failure(id, "replay_incomplete", "Replay end timestamp and frame count must be finalized before saving or playback.");
        if (method == "save_replay" && GetFileAttributesA(replayPath(leaf).c_str()) != INVALID_FILE_ATTRIBUTES)
            return failure(id, "replay_exists", "Choose a new name; existing replay files are not overwritten.");
        return queueOperation(id, method, {leaf});
    }
    if (method == "start_skirmish")
    {
        if (matchReady())
            return failure(id, "match_active", "End or save the current match before starting another.");
        if (!TheMapCache || !ThePlayerTemplateStore || args[0].empty() || args[0].size() > 240)
            return failure(id, "invalid_map", "Choose a map returned by list_maps.");
        const MapMetaData* map = TheMapCache->findMap(AsciiString(args[0].c_str()));
        if (!map || !map->m_doesExist || !map->m_isMultiplayer || map->m_numPlayers < 2)
            return failure(id, "invalid_map", "A cached skirmish map with at least two starts is required.");
        if (factionIndex(args[1]) < 0 || factionIndex(args[2]) < 0)
            return failure(id, "invalid_faction", "Choose playable faction names returned by list_maps.");
        unsigned cash;
        if ((args[3] != "easy" && args[3] != "medium" && args[3] != "hard") || !integer(args[4], 1000000, cash))
            return failure(id, "invalid_argument", "Use easy/medium/hard and starting money from 0 to 1000000.");
        return queueOperation(id, method, args);
    }
    if (method == "load_game" || method == "save_game")
    {
        std::string leaf;
        if (!saveLeaf(args[0], leaf))
            return failure(id, "invalid_argument", "Use a simple save name with 1-80 letters, digits, underscores, or hyphens, optionally ending in .sav.");
        const bool exists = TheGameState->doesSaveGameExist(AsciiString(leaf.c_str()));
        if (method == "save_game" && exists)
            return failure(id, "save_exists", "Choose a new name; existing saves are not overwritten.");
        if (method == "load_game" && !exists)
            return failure(id, "save_not_found", "The requested save does not exist in the game save directory.");
        if (method == "save_game" && !matchReady())
            return failure(id, "not_in_match", "An initialized single-player match is required for saving.");
        return queueOperation(id, method, {leaf});
    }
    if (!matchReady())
        return failure(id, "not_in_match", "An initialized single-player match is required.");
    if (method == "end_match")
        return queueOperation(id, method, {});
    if (method == "set_paused")
    {
        if (args[0] != "0" && args[0] != "1")
            return failure(id, "invalid_argument", "paused must be 0 or 1.");
        if (args[0] == "1" && IsRealtimeControl())
            return failure(id, "realtime_control", "Real-time control does not pause the simulation.");
        closePauseMenu();
        session().advanceUntil = 0;
        TheGameLogic->setGamePaused(args[0] == "1", FALSE, FALSE);
        session().status = "completed";
    }
    else
    {
        if (IsRealtimeControl())
            return failure(id, "realtime_control", "Real-time control does not advance a fixed number of frames.");
        unsigned frames;
        if (!integer(args[0], 1800, frames) || !frames)
            return failure(id, "invalid_argument", "Advance 1 to 1800 simulation frames.");
        if (session().advanceUntil)
            return failure(id, "game_advancing", "A frame advance is already running.");
        closePauseMenu();
        session().advanceUntil = TheGameLogic->getFrame() + frames;
        TheGameLogic->setGamePaused(FALSE, FALSE, FALSE);
        session().status = "advancing";
    }
    session().action = method;
    session().request = id;
    session().error.clear();
    session().result.clear();
    return success(id, sessionJson());
}

void UpdateSession()
{
    SessionState& state = session();
    rememberMatchResult();
    if (IsRealtimeControl() && state.advanceUntil)
    {
        state.advanceUntil = 0;
        state.status = "failed";
        state.error = "Control mode changed to real-time during a frame advance; the simulation continues running.";
    }
    if ((multiplayerMatch() || (TheRecorder && TheRecorder->isPlaybackMode() &&
        state.waitForReady && !state.expectingReplay)) && (state.advanceUntil || state.waitForReady))
    {
        state.advanceUntil = 0;
        state.pauseWhenReady = false;
        state.waitForReady = false;
        state.status = "failed";
        state.error = "Session changed to multiplayer or replay during deferred control.";
    }
    if (state.advanceUntil && !matchReady())
    {
        state.advanceUntil = 0;
        state.status = "failed";
        state.error = "Match ended during the frame advance.";
    }
    if (state.advanceUntil && matchReady() && TheGameLogic->getFrame() >= state.advanceUntil)
    {
        TheGameLogic->setGamePaused(TRUE, FALSE, FALSE);
        state.advanceUntil = 0;
        state.status = "completed";
    }
    if (state.waitForReady)
    {
        if (matchReady())
        {
            if (state.pauseWhenReady && !IsRealtimeControl())
                TheGameLogic->setGamePaused(TRUE, FALSE, FALSE);
            state.pauseWhenReady = false;
            state.waitForReady = false;
            state.status = "completed";
        }
        else if (std::chrono::steady_clock::now() > state.deadline)
        {
            state.pauseWhenReady = false;
            state.waitForReady = false;
            state.status = "failed";
            state.error = "Match did not become ready within 90 seconds.";
        }
        return;
    }
    // Loading/saving can be slow. First deliver the queued acknowledgement and
    // let the requesting client close its pipe; report completion via get_session.
    if (!state.pending || !IsBridgeIdle()) return;
    state.pending = false;
    state.status = "running";
    bool loadingStarted = false;
    try
    {
        if (multiplayerMatch() || (multiplayerOrReplay() && state.action != "end_match"))
            throw std::runtime_error("Session changed to multiplayer or replay before the operation ran.");
        if (state.action == "start_skirmish")
        {
            if (matchReady()) throw std::runtime_error("A match started before the queued operation ran.");
            startSkirmish(state.arguments);
            state.waitForReady = true;
            state.pauseWhenReady = !IsRealtimeControl();
            state.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            state.status = "loading";
        }
        else if (state.action == "save_game")
        {
            const AsciiString leaf(state.arguments[0].c_str());
            if (!matchReady() || TheGameState->doesSaveGameExist(leaf))
                throw std::runtime_error("The match ended or the save name already exists.");
            UnicodeString description;
            description.translate(leaf);
            SaveResult result = TheGameState->saveGame(leaf, description, SAVE_FILE_TYPE_NORMAL);
            if (result.saveCode != SC_OK) throw std::runtime_error("The engine could not save this match.");
            state.status = "completed";
        }
        else if (state.action == "save_replay")
        {
            if (matchReady()) throw std::runtime_error("A match started before the replay could be saved.");
            state.result = copyFinalReplay(state.arguments[0]);
            state.status = "completed";
        }
        else if (state.action == "play_replay")
        {
            if (matchReady()) throw std::runtime_error("A match started before replay playback could begin.");
            ReplayFile file;
            validatePlayback(state.arguments[0], file);
            closePauseMenu();
            ResetObservationMatch();
            loadingStarted = true;
            if (!TheRecorder->playbackFile(AsciiString(state.arguments[0].c_str())))
                throw std::runtime_error("The engine could not begin replay playback.");
            state.result = replayJson(state.arguments[0], file);
            state.expectingReplay = true;
            state.waitForReady = true;
            state.pauseWhenReady = !IsRealtimeControl();
            state.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            state.status = "loading";
        }
        else if (state.action == "load_game")
        {
            AvailableGameInfo info;
            info.filename = state.arguments[0].c_str();
            info.next = info.prev = nullptr;
            TheGameState->getSaveGameInfoFromFile(info.filename, &info.saveGameInfo);
            closePauseMenu();
            if (matchReady() && TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
                TheRecorder->stopRecording();
            if (!matchReady()) TheGameLogic->prepareNewGame(GAME_SINGLE_PLAYER, DIFFICULTY_NORMAL, 0);
            loadingStarted = true;
            ResetObservationMatch();
            if (TheGameState->loadGame(info) != SC_OK)
                throw std::runtime_error("The engine rejected the save file.");
            state.waitForReady = true;
            state.pauseWhenReady = !IsRealtimeControl();
            state.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            state.status = "loading";
        }
        else if (state.action == "end_match")
        {
            closePauseMenu();
            // GeneralsX @bugfix Finalize the replay while the original match frame still exists.
            // Recorder reset only closes the file; it does not write the final timestamp/frame count.
            if (TheRecorder)
            {
                if (TheRecorder->isPlaybackMode()) TheRecorder->stopPlayback();
                else if (TheRecorder->getMode() == RECORDERMODETYPE_RECORD) TheRecorder->stopRecording();
            }
            rememberMatchResult();
            ResetObservationMatch();
            TheGameLogic->setGamePaused(FALSE);
            TheGameLogic->clearGameData(FALSE);
            TheGameEngine->reset();
            if (TheShell) TheShell->showShell(TRUE);
            rememberMatchResult();
            state.status = "completed";
        }
    }
    catch (const std::exception& error)
    {
        state.error = error.what();
        state.status = "failed";
    }
    catch (...)
    {
        state.error = "The game engine failed the session operation.";
        state.status = "failed";
    }
    if (loadingStarted && state.status == "failed")
    {
        // Match the normal load menu's recovery after a partial engine reset.
        if (TheGameLogic->isInGame()) TheGameLogic->clearGameData(FALSE);
        TheGameEngine->reset();
        if (TheShell) TheShell->showShell(TRUE);
        state.pauseWhenReady = false;
        state.waitForReady = false;
        state.expectingReplay = false;
    }
}

void ShutdownSession()
{
    delete s_session;
    s_session = nullptr;
}
}
