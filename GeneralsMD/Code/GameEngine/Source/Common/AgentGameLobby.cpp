#include "PreRTS.h"
#include "Common/AgentGameInternal.h"
#include "Common/GlobalData.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "GameClient/MapUtil.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Shell.h"
#include "GameClient/WindowLayout.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GUIUtil.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include <cstring>
#include <set>

extern Bool LANbuttonPushed;
extern Bool LANisShuttingDown;
extern Bool LanLobbyMenuIsEntering();
extern void StartPressed();

namespace GeneralsAgent
{
namespace
{
using namespace Detail;

// GeneralsX @feature Drive LAN setup through the normal shell and LAN API lifecycle.
const char* const lobbyScreen = "Menus/LanLobbyMenu.wnd";
const char* const optionsScreen = "Menus/LanGameOptionsMenu.wnd";

struct LobbyOverrides
{
    UnsignedInt localIP;
    char name[g_lanPlayerNameLength + 1];
};
LobbyOverrides s_lobbyOverrides = {};

bool integer(const std::string& value, int minimum, int maximum, int& result)
{
    if (value.empty() || value.size() > 11) return false;
    bool negative = value[0] == '-';
    size_t index = negative ? 1 : 0;
    if (index == value.size()) return false;
    long long parsed = 0;
    for (; index < value.size(); ++index)
    {
        if (value[index] < '0' || value[index] > '9') return false;
        parsed = parsed * 10 + value[index] - '0';
        if (parsed > 2147483648LL) return false;
    }
    if (negative) parsed = -parsed;
    if (parsed < minimum || parsed > maximum) return false;
    result = int(parsed);
    return true;
}

std::string unicodeJson(const UnicodeString& value)
{
    static const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    const WideChar* text = value.str();
    for (size_t i = 0; text && text[i]; ++i)
    {
        unsigned c = static_cast<unsigned short>(text[i]);
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') result += static_cast<char>(c);
        else
        {
            result += "\\u";
            for (int shift = 12; shift >= 0; shift -= 4) result += hex[(c >> shift) & 15];
        }
    }
    return result + '"';
}

std::string ipString(UnsignedInt ip)
{
    auto out = jsonStream();
    out << ((ip >> 24) & 255) << '.' << ((ip >> 16) & 255) << '.' << ((ip >> 8) & 255) << '.' << (ip & 255);
    return out.str();
}

bool parseIP(const std::string& text, UnsignedInt& address)
{
    size_t start = 0;
    address = 0;
    for (int part = 0; part < 4; ++part)
    {
        size_t end = text.find('.', start);
        if ((part < 3 && end == std::string::npos) || (part == 3 && end != std::string::npos)) return false;
        if (end == std::string::npos) end = text.size();
        int octet;
        if (!integer(text.substr(start, end - start), 0, 255, octet)) return false;
        address = (address << 8) | octet;
        start = end + 1;
    }
    return address != 0 && address != 0xffffffff;
}

std::string screen()
{
    return TheShell && TheShell->top() ? TheShell->top()->getFilename().str() : "";
}

bool transitioning()
{
    return !TheShell || TheShell->isLayoutChangePending() || !TheShell->isAnimFinished() ||
        ((screen() == lobbyScreen || screen() == optionsScreen) && (LANbuttonPushed || LANisShuttingDown)) ||
        (screen() == lobbyScreen && LanLobbyMenuIsEntering());
}

const char* setupRestriction(bool requireGame)
{
    if (!TheShell || !TheShell->isShellActive() || !TheGameLogic ||
        (TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()))
        return "LAN setup is only available in the shell, outside an active match.";
    if (!TheLAN || screen() != (requireGame ? optionsScreen : lobbyScreen))
        return requireGame ? "Enter a LAN game setup first." : "Enter the LAN lobby first.";
    if (transitioning() || TheLAN->hasPendingAction()) return "Wait for the current lobby transition or request to finish.";
    if (requireGame && (!TheLAN->GetMyGame() || TheLAN->GetMyGame()->isGameInProgress()))
        return "No configurable LAN game is active.";
    if (requireGame && (mapSelectLayout || !AreSlotListUpdatesEnabled()))
        return "Close the map selection dialog and wait for setup to finish.";
    if (TheLAN->isGameStartCountdownActive()) return "The game start countdown is active.";
    return nullptr;
}

const char* stateName(SlotState state)
{
    switch (state)
    {
    case SLOT_OPEN: return "open";
    case SLOT_CLOSED: return "closed";
    case SLOT_EASY_AI: return "easy";
    case SLOT_MED_AI: return "medium";
    case SLOT_BRUTAL_AI: return "brutal";
    case SLOT_PLAYER: return "human";
    default: return "unknown";
    }
}

std::string factionName(int index)
{
    if (index == PLAYERTEMPLATE_RANDOM) return "random";
    if (index == PLAYERTEMPLATE_OBSERVER) return "observer";
    if (!ThePlayerTemplateStore || index < 0 || index >= ThePlayerTemplateStore->getPlayerTemplateCount()) return "";
    return ThePlayerTemplateStore->getNthPlayerTemplate(index)->getName().str();
}

std::string gameJson(LANGameInfo* game)
{
    if (!game) return "null";
    auto out = jsonStream();
    out << "{\"name\":" << unicodeJson(game->getName()) << ",\"host_ip\":" << quote(ipString(game->getHostIP()))
        << ",\"map\":" << quote(game->getMap().str()) << ",\"in_progress\":" << bool(game->isGameInProgress())
        << ",\"local_slot\":" << game->getLocalSlotNum() << ",\"starting_cash\":" << game->getStartingCash().countMoney()
        << ",\"limit_superweapons\":" << bool(game->getSuperweaponRestriction()) << ",\"slots\":[";
    for (int i = 0; i < MAX_SLOTS; ++i)
    {
        const LANGameSlot* slot = game->getConstLANSlot(i);
        // Keep random lobby choices private after the match resolves them.
        bool original = game->isGameInProgress() && slot->hasSavedOriginalSetup();
        int faction = original ? slot->getOriginalPlayerTemplate() : slot->getPlayerTemplate();
        int color = original ? slot->getOriginalColor() : slot->getColor();
        int position = original ? slot->getOriginalStartPos() : slot->getStartPos();
        if (i) out << ',';
        out << "{\"slot\":" << i << ",\"state\":" << quote(stateName(slot->getState()))
            << ",\"name\":" << unicodeJson(slot->getName()) << ",\"ip\":" << quote(ipString(slot->getIP()))
            << ",\"faction\":" << quote(factionName(faction)) << ",\"faction_index\":" << faction
            << ",\"color\":" << color << ",\"team\":" << slot->getTeamNumber()
            << ",\"start_position\":" << position << ",\"accepted\":" << bool(slot->isAccepted())
            << ",\"has_map\":" << bool(slot->hasMap()) << ",\"local\":" << (i == game->getLocalSlotNum()) << '}';
    }
    out << "]}";
    return out.str();
}

std::string getLobby(const std::string& id)
{
    auto out = jsonStream();
    out << "{\"available\":" << bool(TheLAN) << ",\"screen\":" << quote(screen())
        << ",\"transitioning\":" << transitioning() << ",\"pending\":" << bool(TheLAN && TheLAN->hasPendingAction())
        << ",\"countdown\":" << bool(TheLAN && TheLAN->isGameStartCountdownActive())
        << ",\"local_name\":" << (TheLAN ? unicodeJson(TheLAN->GetMyName()) : "\"\"")
        << ",\"local_ip\":" << quote(TheLAN ? ipString(TheLAN->GetLocalIP()) : "")
        << ",\"is_host\":" << bool(TheLAN && TheLAN->AmIHost())
        << ",\"current_game\":" << gameJson(TheLAN ? TheLAN->GetMyGame() : nullptr) << ",\"games\":[";
    unsigned count = 0;
    if (TheLAN)
        for (; count < 64; ++count)
        {
            LANGameInfo* game = TheLAN->LookupGameByListOffset(count);
            if (!game) break;
            if (count) out << ',';
            out << gameJson(game);
        }
    out << "],\"truncated\":" << bool(TheLAN && count == 64 && TheLAN->LookupGameByListOffset(64))
        << ",\"color_count\":" << (TheMultiplayerSettings ? TheMultiplayerSettings->getNumColors() : 0)
        << ",\"starting_cash_choices\":[";
    bool first = true;
    if (TheMultiplayerSettings)
        for (const Money& money : TheMultiplayerSettings->getStartingMoneyList())
        {
            if (!first) out << ',';
            first = false;
            out << money.countMoney();
        }
    out << "]}";
    return success(id, out.str());
}

std::string queued(const std::string& id, const std::string& method)
{
    return success(id, "{\"status\":\"queued\",\"action\":" + quote(method) + "}");
}

std::string enterLAN(const std::string& id, const std::vector<std::string>& args)
{
    if (!args.empty() && args.size() != 2) return failure(id, "invalid_arguments", "Expected optional player_name and local_ip.");
    if (!TheWindowManager || TheWindowManager->isDummy())
        return failure(id, "unsupported_headless", "LAN setup requires normal menu callbacks; launch the game without -headless. MCP does not require screenshots or visual input.");
    if (!TheShell || !TheShell->isShellActive() || !TheGameLogic ||
        (TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()) ||
        screen() != "Menus/MainMenu.wnd" || transitioning() || !TheWritableGlobalData)
        return failure(id, "invalid_state", "Enter LAN from the idle main menu.");
    UnsignedInt localIP = 0;
    if (!args.empty())
    {
        if (args[0].size() > g_lanPlayerNameLength) return failure(id, "invalid_name", "Player name must contain at most 12 printable ASCII characters.");
        for (unsigned char c : args[0])
            if (c < 32 || c > 126 || c == ',' || c == ':' || c == ';')
                return failure(id, "invalid_name", "Player name must be printable ASCII without commas, colons or semicolons.");
        if (!args[0].empty() && (args[0].front() == ' ' || args[0].back() == ' '))
            return failure(id, "invalid_name", "Player name cannot start or end with a space.");
        if (!args[1].empty())
        {
            if (!parseIP(args[1], localIP)) return failure(id, "invalid_ip", "Expected a local IPv4 address.");
            bool local = (localIP >> 24) == 127;
            IPEnumeration interfaces;
            for (EnumeratedIP* item = interfaces.getAddresses(); item; item = item->getNext())
                if (item->getIP() == localIP) local = true;
            if (!local) return failure(id, "invalid_ip", "The address is not assigned to a local interface.");
        }
    }
    // Menu initialization applies these only to this LAN session, including returns from game setup.
    LobbyOverrides previous = s_lobbyOverrides;
    s_lobbyOverrides = {};
    s_lobbyOverrides.localIP = localIP;
    if (!args.empty()) std::memcpy(s_lobbyOverrides.name, args[0].c_str(), args[0].size());
    try
    {
        // GeneralsX @bugfix Shell::push can accept a transition for a later frame.
        if (!TheShell->push(lobbyScreen, TRUE))
        {
            s_lobbyOverrides = previous;
            return failure(id, "enter_failed", "The shell rejected the LAN lobby transition.");
        }
    }
    catch (...)
    {
        s_lobbyOverrides = previous;
        throw;
    }
    return queued(id, "enter_lan");
}

std::string startGame(const std::string& id)
{
    LANGameInfo* game = TheLAN->GetMyGame();
    if (!game->amIHost()) return failure(id, "not_host", "Only the host can start the game.");
    const MapMetaData* map = TheMapCache ? TheMapCache->findMap(game->getMap()) : nullptr;
    if (!map || !map->m_doesExist) return failure(id, "invalid_map", "The host map is unavailable.");
    unsigned players = 0, humans = 0, unteamed = 0;
    std::set<int> teams;
    for (int i = 0; i < MAX_SLOTS; ++i)
    {
        GameSlot* slot = game->getSlot(i);
        if (slot->isHuman() && i != game->getLocalSlotNum() && !slot->isAccepted())
            return failure(id, "not_ready", "Every remote human player must accept the current options.");
        if (slot->isHuman() && !slot->hasMap() && map->m_isOfficial)
            return failure(id, "missing_map", "A human player is missing the official map.");
        if (!slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER) continue;
        ++players;
        if (slot->isHuman()) ++humans;
        if (slot->getTeamNumber() < 0) ++unteamed;
        else teams.insert(slot->getTeamNumber());
    }
    if (players > unsigned(map->m_numPlayers)) return failure(id, "too_many_players", "The selected map has too few start positions.");
    int minimum = TheGlobalData->m_netMinPlayers;
    if (players < unsigned(minimum) || (minimum && !humans)) return failure(id, "too_few_players", "The game requires more participating players.");
    if (unteamed + teams.size() < unsigned(minimum)) return failure(id, "too_few_teams", "The game requires more opposing teams.");
    // Keep the stock countdown, open-slot closure, map checks and normal start path.
    StartPressed();
    return queued(id, "lan_start");
}

std::string setSlot(const std::string& id, const std::vector<std::string>& args)
{
    if (args.size() != 6) return failure(id, "invalid_arguments", "Expected slot, state, faction, color, team, start_position.");
    if (!TheMultiplayerSettings || !ThePlayerTemplateStore || !TheMapCache) return failure(id, "not_ready", "Game definitions are not loaded.");
    int index, color, team, position;
    if (!integer(args[0], 0, MAX_SLOTS - 1, index) || !integer(args[3], -1, TheMultiplayerSettings->getNumColors() - 1, color) ||
        !integer(args[4], -1, MAX_SLOTS / 2 - 1, team) || !integer(args[5], -1, MAX_SLOTS - 1, position))
        return failure(id, "invalid_arguments", "Invalid slot, color, team or start position.");
    LANGameInfo* game = TheLAN->GetMyGame();
    LANGameSlot* slot = game->getLANSlot(index);
    SlotState state = slot->getState();
    if (args[1] == "open") state = SLOT_OPEN;
    else if (args[1] == "closed") state = SLOT_CLOSED;
    else if (args[1] == "easy") state = SLOT_EASY_AI;
    else if (args[1] == "medium") state = SLOT_MED_AI;
    else if (args[1] == "brutal") state = SLOT_BRUTAL_AI;
    else if (args[1] != "keep") return failure(id, "invalid_state", "State must be keep, open, closed, easy, medium or brutal.");
    bool local = index == game->getLocalSlotNum();
    if (slot->isHuman() && !local) return failure(id, "not_owned", "Another human player's slot cannot be changed.");
    if ((!game->amIHost() && !local) || (local && state != slot->getState()))
        return failure(id, "not_owned", "Only the host can change non-human slot states; preserve your human slot.");
    int faction = slot->getPlayerTemplate();
    if (args[2] == "random") faction = PLAYERTEMPLATE_RANDOM;
    else if (args[2] == "observer") faction = PLAYERTEMPLATE_OBSERVER;
    else if (!args[2].empty())
    {
        faction = -3;
        for (int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
        {
            const PlayerTemplate* type = ThePlayerTemplateStore->getNthPlayerTemplate(i);
            if (args[2] == type->getName().str() && type->getStartingBuilding().isNotEmpty() && (!game->oldFactionsOnly() || type->isOldFaction())) faction = i;
        }
        if (faction == -3) return failure(id, "invalid_faction", "Choose a loaded playable faction, random or observer.");
    }
    bool occupied = state == SLOT_PLAYER || (state >= SLOT_EASY_AI && state <= SLOT_BRUTAL_AI);
    if (faction == PLAYERTEMPLATE_OBSERVER)
    {
        if (occupied && state != SLOT_PLAYER) return failure(id, "invalid_faction", "AI players cannot be observers.");
        if (color != -1 || team != -1 || position != -1) return failure(id, "invalid_arguments", "Observers require random color, team and start position (-1).");
    }
    const MapMetaData* map = TheMapCache->findMap(game->getMap());
    if (position >= 0 && (!map || position >= map->m_numPlayers)) return failure(id, "invalid_position", "Start position is outside the selected map.");
    if (occupied)
        for (int i = 0; i < MAX_SLOTS; ++i)
        {
            GameSlot* other = game->getSlot(i);
            if (i == index || !other->isOccupied()) continue;
            if (color >= 0 && other->getColor() == color) return failure(id, "color_taken", "That color is assigned to another player.");
            if (position >= 0 && other->getStartPos() == position) return failure(id, "position_taken", "That start position is assigned to another player.");
        }
    if (game->amIHost())
    {
        if (state != slot->getState()) slot->setState(state);
        slot->setPlayerTemplate(faction);
        slot->setColor(occupied ? color : -1);
        slot->setTeamNumber(occupied ? team : -1);
        slot->setStartPos(occupied ? position : -1);
        game->resetAccepted();
        TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
        lanUpdateSlotList();
    }
    else
    {
        if (slot->isAccepted()) return failure(id, "already_accepted", "Wait for the host to change options before changing your accepted setup.");
        const char* keys[] = {"PlayerTemplate", "Color", "Team", "StartPos"};
        int values[] = {faction, color, team, position};
        for (int i = 0; i < 4; ++i)
        {
            AsciiString option;
            option.format("%s=%d", keys[i], values[i]);
            TheLAN->RequestGameOptions(option, true);
        }
    }
    return queued(id, "lan_set_slot");
}

std::string setOptions(const std::string& id, const std::vector<std::string>& args)
{
    if (args.size() != 3) return failure(id, "invalid_arguments", "Expected map, starting_cash and limit_superweapons (0 or 1).");
    LANGameInfo* game = TheLAN->GetMyGame();
    if (!game->amIHost()) return failure(id, "not_host", "Only the host can change game options.");
    int cash, limit;
    if (!integer(args[1], 0, 2147483647, cash) || !integer(args[2], 0, 1, limit))
        return failure(id, "invalid_arguments", "Invalid starting cash or superweapon limit.");
    const MapMetaData* map = TheMapCache ? TheMapCache->findMap(args[0].c_str()) : nullptr;
    if (!map || !map->m_doesExist || !map->m_isMultiplayer || map->m_numPlayers < 2)
        return failure(id, "invalid_map", "Choose an installed multiplayer map from list_maps.");
    bool validCash = false;
    if (TheMultiplayerSettings)
        for (const Money& option : TheMultiplayerSettings->getStartingMoneyList())
            if (option.countMoney() == unsigned(cash)) validCash = true;
    if (!validCash) return failure(id, "invalid_cash", "Choose a loaded starting_cash_choices value from get_lobby.");
    bool changedMap = game->getMap().compareNoCase(args[0].c_str()) != 0;
    Money money;
    money.deposit(cash, FALSE, FALSE);
    game->setStartingCash(money);
    game->setSuperweaponRestriction(limit);
    if (changedMap)
    {
        game->setMap(args[0].c_str());
        game->getSlot(0)->setMapAvailability(true);
        game->setMapCRC(map->m_CRC);
        game->setMapSize(map->m_filesize);
        game->resetStartSpots();
        game->adjustSlotsForMap();
    }
    game->resetAccepted();
    updateGameOptions();
    lanUpdateSlotList();
    TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
    TheLAN->RequestGameAnnounce();
    return queued(id, "lan_set_options");
}
}

bool ApplyLobbyOverrides(UnsignedInt& localIP, UnicodeString& playerName)
{
    if (s_lobbyOverrides.localIP) localIP = s_lobbyOverrides.localIP;
    if (s_lobbyOverrides.name[0]) playerName.translate(s_lobbyOverrides.name);
    return s_lobbyOverrides.localIP != 0;
}

bool HasLobbyNameOverride()
{
    return s_lobbyOverrides.name[0] != '\0';
}

void ClearLobbyOverrides()
{
    s_lobbyOverrides = {};
}

std::string ExecuteLobby(const std::string& id, const std::string& method, const std::vector<std::string>& args)
{
    if (method == "enter_lan") return enterLAN(id, args);
    bool known = method == "get_lobby" || method == "lan_host" || method == "lan_join" || method == "lan_accept" ||
        method == "lan_start" || method == "lan_leave" || method == "lan_set_slot" || method == "lan_set_options";
    if (!known) return std::string();
    if (method != "lan_join" && method != "lan_set_slot" && method != "lan_set_options" && !args.empty())
        return failure(id, "invalid_arguments", "This method takes no arguments.");
    if (method == "get_lobby") return getLobby(id);
    bool requireGame = method != "lan_host" && method != "lan_join" && !(method == "lan_leave" && screen() == lobbyScreen);
    const char* restriction = setupRestriction(requireGame);
    if (restriction) return failure(id, "invalid_state", restriction);
    if (method == "lan_set_slot") return setSlot(id, args);
    if (method == "lan_set_options") return setOptions(id, args);
    if (method == "lan_start") return startGame(id);
    if (method == "lan_host") TheLAN->RequestGameCreate(L"", FALSE);
    else if (method == "lan_join")
    {
        UnsignedInt address;
        int direct = 0;
        if ((args.size() != 1 && args.size() != 2) || !parseIP(args[0], address) ||
            (args.size() == 2 && !integer(args[1], 0, 1, direct)))
            return failure(id, "invalid_arguments", "Expected host IPv4 address and optional direct flag (0 or 1).");
        if (address == TheLAN->GetLocalIP() || (address >> 24) == 0 || (address >> 24) >= 224 || (address & 255) == 255)
            return failure(id, "invalid_ip", "Choose a unicast host address other than this client.");
        if (direct)
        {
            TheLAN->RequestGameJoinDirectConnect(address);
            return queued(id, method);
        }
        LANGameInfo* game = TheLAN->LookupGameByHost(address);
        if (!game || game->isGameInProgress()) return failure(id, "game_unavailable", "The discovered host is unavailable or already playing.");
        bool open = false;
        for (int i = 0; i < MAX_SLOTS; ++i) if (game->getSlot(i)->isOpen()) open = true;
        if (!open) return failure(id, "game_full", "The discovered game has no open slot.");
        TheLAN->RequestGameJoin(game);
    }
    else if (method == "lan_accept")
    {
        LANGameInfo* game = TheLAN->GetMyGame();
        int local = game->getLocalSlotNum();
        if (local < 0) return failure(id, "invalid_state", "The local player has no slot.");
        if (game->amIHost()) return failure(id, "not_client", "The host accepts when issuing lan_start.");
        const MapMetaData* map = TheMapCache ? TheMapCache->findMap(game->getMap()) : nullptr;
        bool transfer = map ? !map->m_isOfficial : WouldMapTransfer(game->getMap());
        if (!game->getSlot(local)->hasMap() && !transfer) return failure(id, "missing_map", "The map is unavailable and cannot transfer.");
        if (game->getSlot(local)->isAccepted()) return success(id, "{\"status\":\"already_accepted\"}");
        TheLAN->RequestAccept();
    }
    else if (method == "lan_leave")
    {
        if (requireGame) TheLAN->RequestGameLeave();
        else
        {
            LANbuttonPushed = true;
            TheShell->pop();
            ClearLobbyOverrides();
            delete TheLAN;
            TheLAN = nullptr;
        }
    }
    return queued(id, method);
}
}
