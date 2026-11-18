#include "PreRTS.h" // This must go first in EVERY cpp file in the GameEngine.

#include "Common/AgentGameAdapter.h"
#include "Common/AgentGameInternal.h"
#include "Common/BuildAssistant.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "Common/ThingTemplate.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

// GeneralsX @feature Keep observations and command validation on the game
// thread. Orders use the same selection/command messages as the normal UI.
namespace GeneralsAgent
{
namespace { bool s_realtimeControl = false; }
bool IsRealtimeControl() { return s_realtimeControl; }
namespace Detail
{
const size_t MAX_OBSERVED_OBJECTS = 512;
const size_t MAX_OBJECT_BYTES = 500 * 1024;
const size_t MAX_ATTACK_UNITS = 64;
const size_t MAX_QUEUE_ENTRIES = 32;

std::string quote(const std::string& value)
{
    static const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            result += '\\';
            result += static_cast<char>(c);
        }
        else if (c < 0x20 || c >= 0x7f)
        {
            // Engine template names are ASCII. Escaping non-ASCII bytes keeps
            // legacy mod text valid JSON even when it is not encoded as UTF-8.
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        }
        else
            result += static_cast<char>(c);
    }
    result += '"';
    return result;
}

std::ostringstream jsonStream()
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(9) << std::boolalpha;
    return stream;
}

std::string number(Real value)
{
    if (!std::isfinite(value))
        return "null";
    std::ostringstream stream = jsonStream();
    stream << value;
    return stream.str();
}

std::string success(const std::string& id, const std::string& result)
{
    return "{\"protocol\":1,\"request_id\":" + quote(id) +
        ",\"ok\":true,\"result\":" + result + "}";
}

std::string failure(const std::string& id, const char* code, const std::string& message)
{
    return "{\"protocol\":1,\"request_id\":" + quote(id) +
        ",\"ok\":false,\"error\":{\"code\":" + quote(code) +
        ",\"message\":" + quote(message) + "}}";
}

bool parseId(const std::string& text, ObjectID& id)
{
    if (text.empty() || text.size() > 10)
        return false;
    unsigned long value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9' || value > (INT_MAX - (c - '0')) / 10UL)
            return false;
        value = value * 10 + (c - '0');
    }
    if (value == 0)
        return false;
    id = static_cast<ObjectID>(value);
    return true;
}

bool parseCoordinate(const std::string& text, Real& value)
{
    if (text.empty() || text.size() > 64)
        return false;
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    stream >> std::noskipws >> value;
    return !stream.fail() && stream.eof() && std::isfinite(value);
}

Player* localPlayer()
{
    return ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
}

bool matchReady()
{
    return TheGameLogic && TheGameLogic->isInGame() &&
        !TheGameLogic->isInShellGame() && !TheGameLogic->isLoadingMap() &&
        !TheGameLogic->isLoadingSave() && !TheGameLogic->isClearingGameData() &&
        localPlayer();
}

const char* writeRestriction()
{
    if (!matchReady())
        return "not_in_match";
    if (TheGameLogic->isInReplayGame() || (TheRecorder && TheRecorder->isPlaybackMode()))
        return "replay_read_only";
    if (TheGameLogic->isGamePaused())
        return "game_paused";
    if (!TheMessageStream || !TheInGameUI || !TheTerrainLogic ||
        !TheControlBar || !TheBuildAssistant || localPlayer()->getPlayerType() != PLAYER_HUMAN ||
        localPlayer()->isPlayerObserver() || !localPlayer()->isPlayerActive())
        return "control_unavailable";
    if (!TheInGameUI->getInputEnabled())
        return "input_disabled";
    return nullptr;
}

Object* ownedObject(ObjectID id)
{
    Object* object = TheGameLogic->findObjectByID(id);
    if (!object || object->isDestroyed() || object->isEffectivelyDead() ||
        object->getControllingPlayer() != localPlayer())
        return nullptr;
    return object;
}

bool canAttackMove(Object* object)
{
    return object->isSelectable() && !object->isDisabled() &&
        !object->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED) &&
        !object->isKindOf(KINDOF_IMMOBILE) && !object->isKindOf(KINDOF_STRUCTURE) &&
        !object->getContainedBy() && !object->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
        object->getAIUpdateInterface() &&
        object->getAIUpdateInterface()->getLocomotorSet().getValidSurfaces() != NO_SURFACES &&
        object->isAbleToAttack();
}

const char* makeReason(CanMakeType result)
{
    switch (result)
    {
    case CANMAKE_OK: return "available";
    case CANMAKE_NO_PREREQ: return "missing_prerequisite";
    case CANMAKE_NO_MONEY: return "insufficient_money";
    case CANMAKE_FACTORY_IS_DISABLED: return "producer_disabled";
    case CANMAKE_QUEUE_FULL: return "queue_full";
    case CANMAKE_PARKING_PLACES_FULL: return "parking_full";
    case CANMAKE_MAXED_OUT_FOR_PLAYER: return "unit_limit";
    default: return "unavailable";
    }
}

const char* trainingRestriction(Object* object, const CommandButton* button)
{
    const ThingTemplate* type = button->getThingTemplate();
    if (!type || !object->getProductionUpdateInterface())
        return "not_a_producer";
    if (object->isDisabled() || object->hasSingleUseCommandBeenUsed() ||
        object->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
        object->getStatusBits().test(OBJECT_STATUS_SOLD))
        return "producer_disabled";
    if (type->getBuildable() == BSTATUS_NO || type->getBuildable() == BSTATUS_ONLY_BY_AI)
        return "not_buildable";
    if ((button->getOptions() & MUST_BE_STOPPED) && object->getAIUpdateInterface() &&
        object->getAIUpdateInterface()->isMoving())
        return "producer_must_stop";
    if ((button->getOptions() & NOT_QUEUEABLE) && object->getProductionUpdateInterface()->firstProduction())
        return "queue_must_be_empty";
    if ((button->getOptions() & NEED_UPGRADE) && button->getUpgradeTemplate())
    {
        const UpgradeTemplate* upgrade = button->getUpgradeTemplate();
        const bool completed = upgrade->getUpgradeType() == UPGRADE_TYPE_PLAYER ?
            localPlayer()->hasUpgradeComplete(upgrade) : object->hasUpgrade(upgrade);
        if (!completed)
            return "missing_upgrade";
    }
    const CanMakeType result = TheBuildAssistant->canMakeUnit(object, type);
    return result == CANMAKE_OK ? nullptr : makeReason(result);
}

std::string positionJson(const Coord3D& position)
{
    return "{\"x\":" + number(position.x) + ",\"y\":" + number(position.y) +
        ",\"z\":" + number(position.z) + "}";
}

std::string objectJson(Object* object)
{
    std::ostringstream out = jsonStream();
    BodyModuleInterface* body = object->getBodyModule();
    AIUpdateInterface* ai = object->getAIUpdateInterface();
    out << "{\"id\":" << static_cast<int>(object->getID())
        << ",\"template\":" << quote(object->getTemplate()->getName().str())
        << ",\"position\":" << positionJson(*object->getPosition())
        << ",\"health\":" << (body ? number(body->getHealth()) : "null")
        << ",\"max_health\":" << (body ? number(body->getMaxHealth()) : "null")
        << ",\"building\":" << static_cast<bool>(object->isKindOf(KINDOF_STRUCTURE))
        << ",\"disabled\":" << static_cast<bool>(object->isDisabled())
        << ",\"under_construction\":" << static_cast<bool>(object->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION))
        << ",\"can_attack_move\":" << canAttackMove(object)
        << ",\"moving\":" << (ai && ai->isMoving())
        << ",\"attacking\":" << (ai && ai->isAttacking())
        << ",\"production_queue\":[";
    ProductionUpdateInterface* production = object->getProductionUpdateInterface();
    size_t queueCount = 0;
    const ProductionEntry* entry = production ? production->firstProduction() : nullptr;
    for (; entry && queueCount < MAX_QUEUE_ENTRIES; entry = production->nextProduction(entry), ++queueCount)
    {
        if (queueCount)
            out << ',';
        const bool unit = entry->getProductionType() == PRODUCTION_UNIT;
        out << "{\"production_id\":" << static_cast<int>(entry->getProductionID())
            << ",\"kind\":" << quote(unit ? "unit" : "upgrade")
            << ",\"template\":";
        if (unit && entry->getProductionObject())
            out << quote(entry->getProductionObject()->getName().str());
        else if (!unit && entry->getProductionUpgrade())
            out << quote(entry->getProductionUpgrade()->getUpgradeName().str());
        else
            out << "null";
        out << ",\"progress\":" << number(entry->getPercentComplete()) << '}';
    }
    out << "],\"queue_truncated\":" << (entry != nullptr) << '}';
    return out.str();
}

std::string getState(const std::string& id)
{
    if (!matchReady())
        return success(id, "{\"status\":\"not_in_match\",\"commands_enabled\":false,\"objects\":[]}");

    Player* player = localPlayer();
    const char* restriction = writeRestriction();
    std::ostringstream out = jsonStream();
    out << "{\"status\":\"in_match\",\"frame\":" << TheGameLogic->getFrame()
        << ",\"mode\":" << quote(TheGameLogic->isInSkirmishGame() ? "skirmish" :
            TheGameLogic->isInReplayGame() ? "replay" :
            TheGameLogic->isInMultiplayerGame() ? "multiplayer" : "singleplayer")
        << ",\"paused\":" << static_cast<bool>(TheGameLogic->isGamePaused())
        << ",\"commands_enabled\":" << (restriction == nullptr)
        << ",\"command_restriction\":" << (restriction ? quote(restriction) : "null")
        << ",\"local_player_id\":" << static_cast<int>(player->getPlayerIndex())
        << ",\"money\":" << player->getMoney()->countMoney()
        << ",\"power\":{\"produced\":" << player->getEnergy()->getProduction()
        << ",\"consumed\":" << player->getEnergy()->getConsumption() << '}';
    if (TheTerrainLogic)
    {
        Region3D bounds;
        TheTerrainLogic->getExtent(&bounds);
        out << ",\"map_bounds\":{\"min\":" << positionJson(bounds.lo)
            << ",\"max\":" << positionJson(bounds.hi) << '}';
    }
    out << ",\"objects\":[";
    size_t total = 0;
    size_t emitted = 0;
    size_t bytes = 0;
    bool truncated = false;
    for (Object* object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
    {
        if (object->getControllingPlayer() != player || object->isDestroyed() ||
            object->isEffectivelyDead() ||
            (!object->isKindOf(KINDOF_SELECTABLE) && !object->isKindOf(KINDOF_STRUCTURE)))
            continue;
        ++total;
        if (truncated || emitted == MAX_OBSERVED_OBJECTS)
        {
            truncated = true;
            continue;
        }
        const std::string encoded = objectJson(object);
        if (bytes + encoded.size() > MAX_OBJECT_BYTES)
        {
            truncated = true;
            continue;
        }
        if (emitted++)
            out << ',';
        out << encoded;
        bytes += encoded.size();
    }
    out << "],\"object_count\":" << total << ",\"objects_returned\":" << emitted
        << ",\"objects_truncated\":" << truncated << '}';
    return success(id, out.str());
}

std::vector<ObjectID> selectedObjects()
{
    std::vector<ObjectID> result;
    const DrawableList* selected = TheInGameUI->getAllSelectedDrawables();
    if (selected)
    {
        for (DrawableListCIt it = selected->begin(); it != selected->end(); ++it)
        {
            Object* object = *it ? (*it)->getObject() : nullptr;
            if (object && ownedObject(object->getID()))
                result.push_back(object->getID());
        }
    }
    return result;
}

void selectObjects(const std::vector<ObjectID>& ids)
{
    // A message supports at most 255 arguments. Split large restored selections
    // into chunks while preserving their original ordering.
    TheMessageStream->appendMessage(GameMessage::MSG_DESTROY_SELECTED_GROUP);
    for (size_t start = 0; start < ids.size(); start += 128)
    {
        GameMessage* message = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
        message->appendBooleanArgument(start == 0);
        const size_t end = std::min(start + 128, ids.size());
        for (size_t i = start; i < end; ++i)
            message->appendObjectIDArgument(ids[i]);
    }
}

std::string getActions(const std::string& id, Object* object)
{
    std::ostringstream out = jsonStream();
    out << "{\"frame\":" << TheGameLogic->getFrame() << ",\"object_id\":"
        << static_cast<int>(object->getID()) << ",\"can_attack_move\":"
        << canAttackMove(object) << ",\"trainable_units\":[";
    const CommandSet* commands = TheControlBar->findCommandSet(object->getCommandSetString());
    bool first = true;
    if (commands)
    {
        for (int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
        {
            const CommandButton* button = commands->getCommandButton(i);
            if (!button || button->getCommandType() != GUI_COMMAND_UNIT_BUILD || !button->getThingTemplate())
                continue;
            const ThingTemplate* type = button->getThingTemplate();
            const char* reason = trainingRestriction(object, button);
            if (!first)
                out << ',';
            first = false;
            out << "{\"template\":" << quote(type->getName().str())
                << ",\"cost\":" << type->calcCostToBuild(localPlayer())
                << ",\"build_frames\":" << type->calcTimeToBuild(localPlayer())
                << ",\"available\":" << (reason == nullptr)
                << ",\"reason\":" << (reason ? quote(reason) : "null") << '}';
        }
    }
    out << "]}";
    return success(id, out.str());
}

std::string trainUnit(const std::string& id, Object* object, const std::string& name)
{
    const CommandSet* commands = TheControlBar->findCommandSet(object->getCommandSetString());
    const CommandButton* found = nullptr;
    if (commands)
    {
        for (int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
        {
            const CommandButton* button = commands->getCommandButton(i);
            if (button && button->getCommandType() == GUI_COMMAND_UNIT_BUILD && button->getThingTemplate() &&
                name == button->getThingTemplate()->getName().str())
            {
                found = button;
                break;
            }
        }
    }
    if (!found)
        return failure(id, "unsupported_unit", "The producer does not offer this template in its current command set.");
    const char* reason = trainingRestriction(object, found);
    if (reason)
        return failure(id, reason, "The unit cannot currently be produced; inspect get_actions for availability.");

    const std::vector<ObjectID> restore = selectedObjects();
    // This is the same client-side ID allocator used by normal unit-build UI.
    const ProductionID productionId = object->getProductionUpdateInterface()->requestUniqueUnitID();
    selectObjects(std::vector<ObjectID>(1, object->getID()));
    GameMessage* message = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UNIT_CREATE);
    message->appendIntegerArgument(found->getThingTemplate()->getTemplateID());
    message->appendIntegerArgument(productionId);
    selectObjects(restore);

    std::ostringstream out = jsonStream();
    out << "{\"status\":\"queued\",\"frame\":" << TheGameLogic->getFrame()
        << ",\"producer_id\":" << static_cast<int>(object->getID())
        << ",\"template\":" << quote(name) << ",\"production_id\":" << static_cast<int>(productionId) << '}';
    return success(id, out.str());
}

std::string attackMove(const std::string& id, const std::vector<std::string>& args)
{
    Coord3D destination;
    if (!parseCoordinate(args[1], destination.x) || !parseCoordinate(args[2], destination.y))
        return failure(id, "invalid_argument", "Coordinates must be finite decimal numbers.");
    Region3D bounds;
    TheTerrainLogic->getExtent(&bounds);
    if (destination.x < bounds.lo.x || destination.x > bounds.hi.x ||
        destination.y < bounds.lo.y || destination.y > bounds.hi.y)
        return failure(id, "outside_map", "The destination lies outside the playable map bounds.");
    destination.z = TheTerrainLogic->getGroundHeight(destination.x, destination.y);
    if (!std::isfinite(destination.z))
        return failure(id, "invalid_terrain", "The destination does not have a valid terrain height.");

    std::vector<ObjectID> ids;
    size_t start = 0;
    do
    {
        const size_t end = args[0].find(',', start);
        ObjectID objectId;
        if (!parseId(args[0].substr(start, end == std::string::npos ? end : end - start), objectId) ||
            ids.size() == MAX_ATTACK_UNITS || std::find(ids.begin(), ids.end(), objectId) != ids.end())
            return failure(id, "invalid_argument", "Supply 1 to 64 unique positive unit IDs separated by commas.");
        Object* object = ownedObject(objectId);
        if (!object)
            return failure(id, "invalid_unit", "A requested unit is missing, dead, or not controlled by the local player.");
        if (!canAttackMove(object))
            return failure(id, "unit_cannot_attack_move", "Every requested unit must be an available mobile combat unit.");
        ids.push_back(objectId);
        if (end == std::string::npos)
            break;
        start = end + 1;
    } while (true);

    const std::vector<ObjectID> restore = selectedObjects();
    selectObjects(ids);
    TheMessageStream->appendMessage(GameMessage::MSG_DO_ATTACKMOVETO)->appendLocationArgument(destination);
    selectObjects(restore);
    std::ostringstream out = jsonStream();
    out << "{\"status\":\"queued\",\"frame\":" << TheGameLogic->getFrame()
        << ",\"unit_ids\":[";
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (i)
            out << ',';
        out << static_cast<int>(ids[i]);
    }
    out << "],\"destination\":" << positionJson(destination) << '}';
    return success(id, out.str());
}
}

std::string Execute(const std::string& requestId, const std::string& method,
    const std::vector<std::string>& args)
{
    using namespace Detail;
    // GeneralsX @feature A realtime client cannot accidentally stop the clock.
    if (method == "set_control_mode")
    {
        if (args.size() != 1 || (args[0] != "realtime" && args[0] != "stepped"))
            return failure(requestId, "invalid_argument", "Expected realtime or stepped.");
        s_realtimeControl = args[0] == "realtime";
        return success(requestId, "{\"control_mode\":" + quote(args[0]) + "}");
    }
    // Bounded gameplay batches are dispatched at one simulation frame. Results
    // acknowledge submission, not atomic execution; each entry retains its error.
    if (method == "batch_orders")
    {
        if (args.size() < 3) return failure(requestId, "invalid_argument", "Missing batch header.");
        ObjectID count, age, expectedID = INVALID_ID;
        const bool checkFreshness = args[0] != "-1";
        if ((checkFreshness && args[0] != "0" && !parseId(args[0], expectedID)) ||
            !parseId(args[1], age) || age > 1800 || !parseId(args[2], count) || count > 32)
            return failure(requestId, "invalid_argument", "Invalid expected frame, age (1..1800), or count (1..32).");
        if (!matchReady()) return failure(requestId, "not_in_match", "An active match is required.");
        const UnsignedInt frame = TheGameLogic->getFrame();
        const UnsignedInt expected = static_cast<UnsignedInt>(expectedID);
        if (checkFreshness && (expected > frame || frame - expected > static_cast<UnsignedInt>(age)))
            return failure(requestId, "stale_snapshot", "Observe a fresh snapshot before submitting this batch.");
        struct Entry { std::string method; std::vector<std::string> args; };
        std::vector<Entry> entries;
        size_t cursor = 3;
        for (int i = 0; i < static_cast<int>(count); ++i)
        {
            if (cursor + 2 > args.size()) return failure(requestId, "invalid_argument", "Truncated batch.");
            const std::string name = args[cursor++];
            const int required = name == "unit_order" || name == "use_command" ? 6 :
                name == "train_unit" || name == "cancel_production" ? 2 :
                name == "build" ? 5 : name == "build_line" ? 7 :
                name == "purchase_science" ? 1 : name == "set_rally_point" || name == "attack_move" ? 3 : -1;
            ObjectID size;
            if (required < 0 || !parseId(args[cursor++], size) || size != required || cursor + size > args.size())
                return failure(requestId, "invalid_argument", "Invalid or non-gameplay batch entry.");
            entries.push_back({name, std::vector<std::string>(args.begin() + cursor, args.begin() + cursor + size)});
            cursor += size;
        }
        if (cursor != args.size()) return failure(requestId, "invalid_argument", "Unexpected batch fields.");
        std::ostringstream out = jsonStream();
        out << "{\"frame\":" << frame << ",\"atomic_execution\":false,\"results\":[";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (i) out << ',';
            out << Execute(requestId + "_" + std::to_string(i), entries[i].method, entries[i].args);
        }
        out << "]}";
        return success(requestId, out.str());
    }
    std::string extended = ExecuteSession(requestId, method, args);
    if (!extended.empty())
        return extended;
    extended = ExecuteObservation(requestId, method, args);
    if (!extended.empty())
        return extended;
    extended = ExecuteCommands(requestId, method, args);
    if (!extended.empty())
        return extended;
    extended = ExecuteLobby(requestId, method, args);
    if (!extended.empty())
        return extended;
    if (method == "get_state")
    {
        if (!args.empty())
            return failure(requestId, "invalid_argument", "get_state accepts no arguments.");
        return getState(requestId);
    }
    const bool actions = method == "get_actions";
    const bool train = method == "train_unit";
    const bool attack = method == "attack_move";
    if (!actions && !train && !attack)
        return failure(requestId, "unknown_method", "Unknown method; use get_capabilities to discover supported controls.");
    if (args.size() != (actions ? 1 : train ? 2 : 3))
        return failure(requestId, "invalid_argument", "Incorrect number of method arguments.");
    if (!matchReady())
        return failure(requestId, "not_in_match", "An initialized match with a local player is required.");
    if (!TheControlBar || !TheBuildAssistant)
        return failure(requestId, "control_unavailable", "Game command data is not initialized.");
    if (!actions)
    {
        const char* restriction = writeRestriction();
        if (restriction)
            return failure(requestId, restriction, "Orders require an active, unpaused match controlled by a human local player.");
    }
    if (attack)
        return attackMove(requestId, args);
    ObjectID objectId;
    if (!parseId(args[0], objectId))
        return failure(requestId, "invalid_argument", "Object ID must be a positive 32-bit integer.");
    Object* object = ownedObject(objectId);
    if (!object)
        return failure(requestId, "invalid_object", "The object is missing, dead, or not controlled by the local player.");
    if (actions)
        return getActions(requestId, object);
    if (args[1].empty() || args[1].size() > 256)
        return failure(requestId, "invalid_argument", "Template name must contain 1 to 256 characters.");
    return trainUnit(requestId, object, args[1]);
}
}
