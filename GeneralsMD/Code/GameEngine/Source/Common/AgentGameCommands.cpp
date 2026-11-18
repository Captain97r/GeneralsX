#include "PreRTS.h" // This must go first in EVERY cpp file in the GameEngine.

#include "Common/AgentGameInternal.h"
#include "Common/BuildAssistant.h"
#include "Common/ActionManager.h"
#include "Common/MessageStream.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerTemplate.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameClient/ControlBar.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/WeaponSet.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/DockUpdate.h"
#include "GameLogic/Module/HackInternetAIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/SpawnBehavior.h"

#include <algorithm>
#include <cmath>
#include <limits>

// GeneralsX @feature Expose loaded commands and normal player orders to
// the local agent. Validate the whole request before emitting synchronized messages.
namespace GeneralsAgent
{
namespace
{
using namespace Detail;

const char* makeReason(CanMakeType value)
{
    switch (value)
    {
    case CANMAKE_OK: return nullptr;
    case CANMAKE_NO_PREREQ: return "missing_prerequisite";
    case CANMAKE_NO_MONEY: return "insufficient_money";
    case CANMAKE_FACTORY_IS_DISABLED: return "producer_disabled";
    case CANMAKE_QUEUE_FULL: return "queue_full";
    case CANMAKE_PARKING_PLACES_FULL: return "parking_full";
    case CANMAKE_MAXED_OUT_FOR_PLAYER: return "unit_limit";
    default: return "unavailable";
    }
}

const char* locationReason(LegalBuildCode value)
{
    switch (value)
    {
    case LBC_OK: return nullptr;
    case LBC_RESTRICTED_TERRAIN: return "restricted_terrain";
    case LBC_NOT_FLAT_ENOUGH: return "not_flat_enough";
    case LBC_OBJECTS_IN_THE_WAY: return "objects_in_the_way";
    case LBC_NO_CLEAR_PATH: return "no_clear_path";
    case LBC_SHROUD: return "unrevealed_location";
    case LBC_TOO_CLOSE_TO_SUPPLIES: return "too_close_to_supplies";
    default: return "invalid_location";
    }
}

bool worldPosition(const std::string& x, const std::string& y, Coord3D& p)
{
    if (!parseCoordinate(x, p.x) || !parseCoordinate(y, p.y))
        return false;
    Region3D bounds;
    TheTerrainLogic->getExtent(&bounds);
    if (p.x < bounds.lo.x || p.x > bounds.hi.x || p.y < bounds.lo.y || p.y > bounds.hi.y)
        return false;
    p.z = TheTerrainLogic->getGroundHeight(p.x, p.y);
    return std::isfinite(p.z);
}

bool parseTarget(const std::string& value, Object*& target)
{
    target = nullptr;
    if (value == "0")
        return true;
    ObjectID objectId;
    if (!parseId(value, objectId))
        return false;
    target = observableObject(objectId);
    return target != nullptr;
}

Object* parseOwned(const std::string& value)
{
    ObjectID objectId;
    return parseId(value, objectId) ? ownedObject(objectId) : nullptr;
}

bool mobile(Object* object)
{
    return object->isSelectable() && !object->isDisabled() && !object->getContainedBy() &&
        !object->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED) &&
        !object->isKindOf(KINDOF_IMMOBILE) && !object->isKindOf(KINDOF_STRUCTURE) &&
        !object->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
        object->getAIUpdateInterface() &&
        object->getAIUpdateInterface()->getLocomotorSet().getValidSurfaces() != NO_SURFACES;
}

const CommandButton* findInSet(const CommandSet* set, const std::string& name)
{
    if (set)
        for (int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
        {
            const CommandButton* button = set->getCommandButton(i);
            if (button && name == button->getName().str())
                return button;
        }
    return nullptr;
}

struct ShortcutEntry
{
    const CommandButton* button;
    int slot;
    AsciiString commandSet;
    AsciiString selector;
};

struct SelectorMenus
{
    const ThingTemplate* thing;
    std::vector<AsciiString> sets;
};

void collectSelectorMenus(Object* object, void* data)
{
    SelectorMenus& menus = *static_cast<SelectorMenus*>(data);
    if (object->isEffectivelyDead() || object->testStatus(OBJECT_STATUS_SOLD) ||
        !menus.thing->isEquivalentTo(object->getTemplate())) return;
    const AsciiString name = object->getCommandSetString();
    if (std::find(menus.sets.begin(), menus.sets.end(), name) == menus.sets.end())
        menus.sets.push_back(name);
}

bool shortcutPower(const CommandButton* button)
{
    return button && button->getSpecialPowerTemplate() &&
        (button->getCommandType() == GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT ||
         button->getCommandType() == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT);
}

void collectShortcutSet(const AsciiString& name, const AsciiString& selector,
    std::vector<ShortcutEntry>& entries, std::vector<AsciiString>& visited, unsigned depth)
{
    if (name.isEmpty() || depth > 8 || visited.size() >= 64 ||
        std::find(visited.begin(), visited.end(), name) != visited.end()) return;
    visited.push_back(name);
    const CommandSet* set = TheControlBar->findCommandSet(name);
    if (!set) return;
    for (int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
    {
        const CommandButton* button = set->getCommandButton(i);
        if (!button || (button->getOptions() & SCRIPT_ONLY)) continue;
        if (shortcutPower(button))
        {
            bool duplicate = false;
            for (const ShortcutEntry& entry : entries)
                if (entry.button == button) { duplicate = true; break; }
            if (!duplicate) entries.push_back({button, i + 1, name, selector});
        }
        else if (button->getCommandType() == GUI_COMMAND_SELECT_ALL_UNITS_OF_TYPE && button->getThingTemplate())
        {
            // GeneralsX @feature Follow the same owned selection menus as the UI,
            // including invisible Contra menu holders, and retain dynamic commandsets.
            SelectorMenus menus = {button->getThingTemplate(), {}};
            localPlayer()->iterateObjects(collectSelectorMenus, &menus);
            for (const AsciiString& child : menus.sets)
                collectShortcutSet(child, button->getName(), entries, visited, depth + 1);
        }
    }
}

std::vector<ShortcutEntry> shortcutEntries()
{
    std::vector<ShortcutEntry> entries;
    std::vector<AsciiString> visited;
    const PlayerTemplate* playerTemplate = localPlayer()->getPlayerTemplate();
    if (playerTemplate)
        collectShortcutSet(playerTemplate->getSpecialPowerShortcutCommandSet(), AsciiString(), entries, visited, 0);
    return entries;
}

struct PowerSourceSearch
{
    const SpecialPowerTemplate* power;
    Object* source;
    UnsignedInt readyFrame;
};

bool readPowerCooldown(Object* object, SpecialPowerModuleInterface* power, UnsignedInt& frame)
{
    if (!object || !power || !power->getSpecialPowerTemplate())
        return false;
    // GeneralsX @feature Match stock effective readiness, including an
    // absent shared timer being ready now, without inserting a simulation timer.
    frame = power->getReadyFrameReadOnly();
    return true;
}

void considerPowerSource(Object* object, void* data)
{
    PowerSourceSearch& search = *static_cast<PowerSourceSearch*>(data);
    if (object->isEffectivelyDead() || object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ||
        object->testStatus(OBJECT_STATUS_SOLD)) return;
    SpecialPowerModuleInterface* module = object->getSpecialPowerModule(search.power);
    if (!module || module->isScriptOnly()) return;
    UnsignedInt ready = UINT_MAX;
    readPowerCooldown(object, module, ready);
    if (object->isDisabled()) ready = UINT_MAX - 1;
    if (!search.source || ready < search.readyFrame)
    {
        search.source = object;
        search.readyFrame = ready;
    }
}

Object* shortcutSource(const CommandButton* button)
{
    // Resolve the exact power template: mods can reuse a special-power type for
    // unrelated abilities. Never invoke a holder or an unrelated module by type.
    PowerSourceSearch search = {button->getSpecialPowerTemplate(), nullptr, UINT_MAX};
    localPlayer()->iterateObjects(considerPowerSource, &search);
    return search.source;
}

ScienceVec factionSciences()
{
    ScienceVec sciences;
    const PlayerTemplate* playerTemplate = localPlayer()->getPlayerTemplate();
    if (!playerTemplate) return sciences;
    const AsciiString names[] = {playerTemplate->getPurchaseScienceCommandSetRank1(),
        playerTemplate->getPurchaseScienceCommandSetRank3(), playerTemplate->getPurchaseScienceCommandSetRank8()};
    const int slots[] = {MAX_PURCHASE_SCIENCE_RANK_1, MAX_PURCHASE_SCIENCE_RANK_3, MAX_PURCHASE_SCIENCE_RANK_8};
    for (int rankMenu = 0; rankMenu < 3; ++rankMenu)
    {
        const CommandSet* set = TheControlBar->findCommandSet(names[rankMenu]);
        if (!set) continue;
        for (int i = 0; i < slots[rankMenu]; ++i)
        {
            const CommandButton* button = set->getCommandButton(i);
            if (!button || button->getCommandType() != GUI_COMMAND_PURCHASE_SCIENCE ||
                (button->getOptions() & SCRIPT_ONLY)) continue;
            for (ScienceType science : button->getScienceVec())
                if (science != SCIENCE_INVALID && std::find(sciences.begin(), sciences.end(), science) == sciences.end())
                    sciences.push_back(science);
        }
    }
    return sciences;
}

const CommandButton* findButton(Object* object, const std::string& name)
{
    const CommandButton* button = findInSet(TheControlBar->findCommandSet(object->getCommandSetString()), name);
    if (button)
        return button;
    if (object->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION) && name == "Command_CancelConstruction")
    {
        button = TheControlBar->findCommandButton("Command_CancelConstruction");
        if (button && button->getCommandType() == GUI_COMMAND_DOZER_CONSTRUCT_CANCEL) return button;
    }
    for (const ShortcutEntry& entry : shortcutEntries())
        if (name == entry.button->getName().str())
        {
            SpecialPowerModuleInterface* module = object->getSpecialPowerModule(entry.button->getSpecialPowerTemplate());
            if (module && !module->isScriptOnly()) return entry.button;
        }
    return nullptr;
}

const CommandButton* findBuildButton(Object* object, const std::string& name)
{
    const CommandSet* set = TheControlBar->findCommandSet(object->getCommandSetString());
    if (set)
        for (int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
        {
            const CommandButton* button = set->getCommandButton(i);
            if (button && button->getCommandType() == GUI_COMMAND_DOZER_CONSTRUCT &&
                button->getThingTemplate() && name == button->getThingTemplate()->getName().str())
                return button;
        }
    return nullptr;
}

const char* supportedReason(const CommandButton* button)
{
    if (button->getOptions() & SCRIPT_ONLY)
        return "script_only";
    switch (button->getCommandType())
    {
    case GUI_COMMAND_DOZER_CONSTRUCT: case GUI_COMMAND_DOZER_CONSTRUCT_CANCEL:
    case GUI_COMMAND_UNIT_BUILD: case GUI_COMMAND_PLAYER_UPGRADE: case GUI_COMMAND_OBJECT_UPGRADE:
    case GUI_COMMAND_STOP: case GUI_COMMAND_ATTACK_MOVE: case GUI_COMMAND_GUARD:
    case GUI_COMMAND_GUARD_WITHOUT_PURSUIT: case GUI_COMMAND_GUARD_FLYING_UNITS_ONLY:
    case GUI_COMMAND_EVACUATE: case GUI_COMMAND_EXIT_CONTAINER: case GUI_COMMAND_SET_RALLY_POINT:
    case GUI_COMMAND_SELL: case GUI_COMMAND_FIRE_WEAPON: case GUI_COMMAND_SPECIAL_POWER:
    case GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT: case GUI_COMMAND_SPECIAL_POWER_CONSTRUCT:
    case GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT: case GUI_COMMAND_HACK_INTERNET:
    case GUI_COMMAND_TOGGLE_OVERCHARGE: case GUI_COMMAND_SWITCH_WEAPON:
    case GUI_COMMAND_COMBATDROP: case GUI_COMMAND_EXECUTE_RAILED_TRANSPORT:
    case GUICOMMANDMODE_HIJACK_VEHICLE: case GUICOMMANDMODE_CONVERT_TO_CARBOMB:
    case GUICOMMANDMODE_SABOTAGE_BUILDING:
        return nullptr;
    case GUI_COMMAND_CANCEL_UNIT_BUILD: case GUI_COMMAND_CANCEL_UPGRADE:
        return "use_cancel_production";
    case GUI_COMMAND_PURCHASE_SCIENCE:
        return "use_purchase_science";
    default: return "unsupported_command_type";
    }
}

const char* commandReason(Object* object, const CommandButton* button, bool readOnly = false)
{
    if (const char* reason = supportedReason(button))
        return reason;
    GUICommandType type = button->getCommandType();
    if (object->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED) ||
        object->testScriptStatusBit(OBJECT_STATUS_SCRIPT_UNPOWERED) || object->getStatusBits().test(OBJECT_STATUS_SOLD))
        return "object_disabled";
    if (object->hasSingleUseCommandBeenUsed())
        return "single_use_already_used";
    if (object->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION) &&
        type != GUI_COMMAND_DOZER_CONSTRUCT_CANCEL && type != GUI_COMMAND_SELL)
        return "under_construction";
    bool disabled = object->isDisabled();
    if ((button->getOptions() & IGNORES_UNDERPOWERED) &&
        object->getDisabledFlags().test(DISABLED_UNDERPOWERED) && object->getDisabledFlags().count() == 1)
        disabled = false;
    if (disabled && type != GUI_COMMAND_SELL && type != GUI_COMMAND_EVACUATE &&
        type != GUI_COMMAND_EXIT_CONTAINER && type != GUI_COMMAND_SET_RALLY_POINT &&
        type != GUI_COMMAND_STOP && type != GUI_COMMAND_SWITCH_WEAPON)
        return "object_disabled";
    if ((button->getOptions() & MUST_BE_STOPPED) && object->getAIUpdateInterface() && object->getAIUpdateInterface()->isMoving())
        return "must_stop";
    if (button->getOptions() & NEED_UPGRADE)
    {
        const UpgradeTemplate* upgrade = button->getUpgradeTemplate();
        if (upgrade && !(upgrade->getUpgradeType() == UPGRADE_TYPE_PLAYER ?
            localPlayer()->hasUpgradeComplete(upgrade) : object->hasUpgrade(upgrade)))
            return "missing_upgrade";
    }
    ProductionUpdateInterface* production = object->getProductionUpdateInterface();
    if ((button->getOptions() & NOT_QUEUEABLE) && production && production->firstProduction())
        return "queue_must_be_empty";
    // Shortcut science vectors describe upgrade tiers, not cumulative prerequisites.
    if (type == GUI_COMMAND_PLAYER_UPGRADE || type == GUI_COMMAND_OBJECT_UPGRADE)
        for (ScienceType science : button->getScienceVec())
            if (!localPlayer()->hasScience(science))
                return "missing_science";

    if (type == GUI_COMMAND_UNIT_BUILD)
        return trainingRestriction(object, button);
    if (type == GUI_COMMAND_DOZER_CONSTRUCT)
    {
        const ThingTemplate* thing = button->getThingTemplate();
        if (!thing || !object->getAIUpdateInterface() || !object->getAIUpdateInterface()->getDozerAIInterface())
            return "not_a_builder";
        if (thing->getBuildable() == BSTATUS_NO || thing->getBuildable() == BSTATUS_ONLY_BY_AI)
            return "not_buildable";
        if (object->getAIUpdateInterface()->getDozerAIInterface()->isTaskPending(DOZER_TASK_BUILD))
            return "builder_busy";
        return makeReason(TheBuildAssistant->canMakeUnit(object, thing));
    }
    if (type == GUI_COMMAND_PLAYER_UPGRADE || type == GUI_COMMAND_OBJECT_UPGRADE)
    {
        const UpgradeTemplate* upgrade = button->getUpgradeTemplate();
        if (!upgrade || !production || !TheUpgradeCenter)
            return "not_a_producer";
        if (type == GUI_COMMAND_PLAYER_UPGRADE &&
            (localPlayer()->hasUpgradeComplete(upgrade) || localPlayer()->hasUpgradeInProduction(upgrade)))
            return "upgrade_owned_or_queued";
        if (type == GUI_COMMAND_OBJECT_UPGRADE &&
            (object->hasUpgrade(upgrade) || production->isUpgradeInQueue(upgrade) || !object->affectedByUpgrade(upgrade)))
            return "upgrade_owned_queued_or_inapplicable";
        if (!TheUpgradeCenter->canAffordUpgrade(localPlayer(), upgrade, false))
            return "insufficient_money";
        return makeReason(production->canQueueUpgrade(upgrade));
    }
    if (button->getSpecialPowerTemplate())
    {
        SpecialPowerModuleInterface* power = object->getSpecialPowerModule(button->getSpecialPowerTemplate());
        if (!power)
            return "missing_special_power";
        if (power->isScriptOnly())
            return "script_only";
        ScienceType science = button->getSpecialPowerTemplate()->getRequiredScience();
        if (science != SCIENCE_INVALID && !localPlayer()->hasScience(science))
            return "missing_science";
        if (!(readOnly ? power->isReadyReadOnly() : power->isReady()))
            return "special_power_not_ready";
    }
    if (type == GUI_COMMAND_SELL && (object->testScriptStatusBit(OBJECT_STATUS_SCRIPT_UNSELLABLE) ||
        object->isDisabledByType(DISABLED_SUBDUED)))
        return "not_sellable";
    if (type == GUI_COMMAND_EVACUATE && (!object->getContain() || object->getContain()->getContainCount() == 0 ||
        object->isDisabledByType(DISABLED_SUBDUED)))
        return "cannot_evacuate";
    if (type == GUI_COMMAND_SWITCH_WEAPON && !object->getWeaponInWeaponSlot(button->getWeaponSlot()))
        return "missing_weapon";
    if (type == GUI_COMMAND_HACK_INTERNET && (!object->getAIUpdateInterface() ||
        !object->getAIUpdateInterface()->getHackInternetAIInterface()))
        return "cannot_hack_internet";
    if (type == GUI_COMMAND_HACK_INTERNET && object->getAIUpdateInterface()->getHackInternetAIInterface()->isHackingPackingOrUnpacking())
        return "already_hacking";
    if (type == GUI_COMMAND_DOZER_CONSTRUCT_CANCEL && !object->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION))
        return "not_under_construction";
    if (type == GUI_COMMAND_EXECUTE_RAILED_TRANSPORT && (!object->getDockUpdateInterface() || !object->getDockUpdateInterface()->isDockOpen()))
        return "dock_unavailable";
    if (type == GUI_COMMAND_COMBATDROP)
    {
        const ContainedItemsList* contents = object->getContain() ? object->getContain()->getContainedItemsList() : nullptr;
        bool rappeller = false;
        if (contents)
            for (Object* item : *contents)
                if (item->isKindOf(KINDOF_CAN_RAPPEL)) { rappeller = true; break; }
        if (!rappeller) return "no_rappelling_passengers";
    }
    return nullptr;
}

void markSingleUse(Object* object, const CommandButton* button)
{
    // Match stock ControlBar UI bookkeeping after validation, before sending the order.
    if (button && (button->getOptions() & SINGLE_USE_COMMAND)) object->markSingleUseCommandUsed();
}

std::string buttonJson(Object* object, const CommandButton* button, int slot)
{
    const char* reason = object ? commandReason(object, button, true) : "missing_source";
    std::ostringstream out = jsonStream();
    out << "{\"slot\":" << slot << ",\"name\":" << quote(button->getName().str())
        << ",\"command_type\":" << static_cast<int>(button->getCommandType())
        << ",\"label\":" << quote(button->getTextLabel().str())
        << ",\"description_label\":" << quote(button->getDescriptionLabel().str())
        << ",\"options\":" << button->getOptions()
        << ",\"supported\":" << (supportedReason(button) == nullptr)
        << ",\"available\":" << (reason == nullptr)
        << ",\"reason\":" << (reason ? quote(reason) : "null")
        << ",\"requires_object_target\":" << ((button->getOptions() & COMMAND_OPTION_NEED_OBJECT_TARGET) != 0)
        << ",\"requires_position\":" << ((button->getOptions() & NEED_TARGET_POS) != 0)
        << ",\"requires_stopped\":" << ((button->getOptions() & MUST_BE_STOPPED) != 0)
        << ",\"requires_upgrade\":" << ((button->getOptions() & NEED_UPGRADE) != 0)
        << ",\"single_use\":" << ((button->getOptions() & SINGLE_USE_COMMAND) != 0)
        << ",\"target_validation\":\"checked_when_issued\"";
    if (const ThingTemplate* thing = button->getThingTemplate())
        out << ",\"template\":" << quote(thing->getName().str()) << ",\"cost\":" << thing->calcCostToBuild(localPlayer())
            << ",\"build_frames\":" << thing->calcTimeToBuild(localPlayer())
            << ",\"line_build\":" << static_cast<bool>(TheBuildAssistant->isLineBuildTemplate(thing));
    if (const UpgradeTemplate* upgrade = button->getUpgradeTemplate())
        out << ",\"upgrade\":" << quote(upgrade->getUpgradeName().str()) << ",\"upgrade_cost\":" << upgrade->calcCostToBuild(localPlayer())
            << ",\"upgrade_build_frames\":" << upgrade->calcTimeToBuild(localPlayer());
    out << ",\"sciences\":[";
    bool firstScience = true;
    for (ScienceType science : button->getScienceVec())
    {
        if (!TheScienceStore) break;
        if (!firstScience) out << ',';
        firstScience = false;
        out << "{\"name\":" << quote(TheScienceStore->getInternalNameForScience(science).str())
            << ",\"owned\":" << static_cast<bool>(localPlayer()->hasScience(science)) << '}';
    }
    out << ']';
    if (const SpecialPowerTemplate* power = button->getSpecialPowerTemplate())
    {
        SpecialPowerModuleInterface* module = object ? object->getSpecialPowerModule(power) : nullptr;
        UnsignedInt readyFrame = 0;
        const bool known = readPowerCooldown(object, module, readyFrame);
        out << ",\"special_power\":" << quote(power->getName().str())
            << ",\"ready_fraction\":" << (known ? number(module->getPercentReadyReadOnly()) : "null")
            << ",\"ready_frame\":" << (known ? std::to_string(readyFrame) : "null")
            << ",\"reload_frames\":" << power->getReloadTime()
            << ",\"required_science\":" << (TheScienceStore && power->getRequiredScience() != SCIENCE_INVALID ?
                quote(TheScienceStore->getInternalNameForScience(power->getRequiredScience()).str()) : "null");
    }
    if (button->getCommandType() == GUI_COMMAND_FIRE_WEAPON || button->getCommandType() == GUI_COMMAND_SWITCH_WEAPON)
        out << ",\"weapon_slot\":" << static_cast<int>(button->getWeaponSlot());
    out << '}';
    return out.str();
}

std::string getActions(const std::string& id, Object* object)
{
    std::ostringstream out = jsonStream();
    out << "{\"frame\":" << TheGameLogic->getFrame() << ",\"object_id\":" << static_cast<int>(object->getID())
        << ",\"can_attack_move\":" << canAttackMove(object)
        << ",\"can_set_rally_point\":" << static_cast<bool>(object->getObjectExitInterface()) << ",\"commands\":[";
    const CommandSet* set = TheControlBar->findCommandSet(object->getCommandSetString());
    bool first = true;
    if (set)
        for (int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
            if (const CommandButton* button = set->getCommandButton(i))
            {
                if (!first) out << ',';
                first = false;
                out << buttonJson(object, button, i + 1);
            }
    if (object->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION) && !findInSet(set, "Command_CancelConstruction"))
    {
        const CommandButton* cancel = findButton(object, "Command_CancelConstruction");
        if (cancel)
        {
            if (!first) out << ',';
            first = false;
            out << buttonJson(object, cancel, 0);
        }
    }
    for (const ShortcutEntry& entry : shortcutEntries())
        if (!findInSet(set, entry.button->getName().str()) &&
            object->getSpecialPowerModule(entry.button->getSpecialPowerTemplate()))
        {
            if (!first) out << ',';
            first = false;
            out << buttonJson(object, entry.button, 0);
        }
    out << "],\"trainable_units\":[";
    first = true;
    if (set)
        for (int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
        {
            const CommandButton* button = set->getCommandButton(i);
            if (!button || button->getCommandType() != GUI_COMMAND_UNIT_BUILD || !button->getThingTemplate()) continue;
            const ThingTemplate* thing = button->getThingTemplate();
            const char* reason = commandReason(object, button);
            if (!first) out << ',';
            first = false;
            out << "{\"template\":" << quote(thing->getName().str()) << ",\"cost\":" << thing->calcCostToBuild(localPlayer())
                << ",\"build_frames\":" << thing->calcTimeToBuild(localPlayer()) << ",\"available\":" << (reason == nullptr)
                << ",\"reason\":" << (reason ? quote(reason) : "null") << '}';
        }
    out << "]}";
    return success(id, out.str());
}

LegalBuildCode legalLocation(Object* builder, const CommandButton* button, const Coord3D& position, Real angle)
{
    return TheBuildAssistant->isLocationLegalToBuild(&position, button->getThingTemplate(), angle,
        BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::CLEAR_PATH |
        BuildAssistant::NO_OBJECT_OVERLAP | BuildAssistant::SHROUD_REVEALED | BuildAssistant::IGNORE_STEALTHED,
        builder, localPlayer());
}

std::string queued(const std::string& id, const std::string& extra = "")
{
    std::ostringstream out = jsonStream();
    out << "{\"status\":\"queued\",\"frame\":" << TheGameLogic->getFrame() << extra << '}';
    return success(id, out.str());
}

class SelectionScope
{
public:
    explicit SelectionScope(const std::vector<ObjectID>& ids) : m_previous(selectedObjects()) { selectObjects(ids); }
    ~SelectionScope() { selectObjects(m_previous); }
private:
    std::vector<ObjectID> m_previous;
};

std::string buildCommand(const std::string& id, const std::vector<std::string>& args, bool issue)
{
    Object* builder = parseOwned(args[0]);
    if (!builder) return failure(id, "invalid_builder", "Builder must be a live object controlled by the local player.");
    const CommandButton* button = findBuildButton(builder, args[1]);
    if (!button) return failure(id, "invalid_template", "Template is not in the builder's current construction commands.");
    if (issue && TheBuildAssistant->isLineBuildTemplate(button->getThingTemplate()))
        return failure(id, "use_build_line", "This template requires start and end positions through build_line.");
    Coord3D position;
    Real angle;
    if (!worldPosition(args[2], args[3], position) || !parseCoordinate(args[4], angle))
        return failure(id, "invalid_argument", "Supply an in-bounds position and finite angle in radians.");
    const char* reason = commandReason(builder, button);
    LegalBuildCode location = legalLocation(builder, button, position, angle);
    if (!issue)
    {
        std::ostringstream out = jsonStream();
        out << "{\"can_build\":" << (!reason && location == LBC_OK) << ",\"reason\":"
            << (reason ? quote(reason) : locationReason(location) ? quote(locationReason(location)) : "null")
            << ",\"placement_legal\":" << (location == LBC_OK) << ",\"position\":" << positionJson(position)
            << ",\"angle\":" << number(angle) << ",\"cost\":" << button->getThingTemplate()->calcCostToBuild(localPlayer()) << '}';
        return success(id, out.str());
    }
    if (reason) return failure(id, reason, "Construction is currently unavailable.");
    if (location != LBC_OK) return failure(id, locationReason(location), "The building cannot be placed there.");
    markSingleUse(builder, button);
    SelectionScope selection(std::vector<ObjectID>(1, builder->getID()));
    GameMessage* message = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT);
    message->appendIntegerArgument(button->getThingTemplate()->getTemplateID());
    message->appendLocationArgument(position);
    message->appendRealArgument(angle);
    return queued(id, ",\"template\":" + quote(args[1]) + ",\"position\":" + positionJson(position));
}

std::string findBuildSites(const std::string& id, const std::vector<std::string>& args)
{
    Object* builder = parseOwned(args[0]);
    if (!builder) return failure(id, "invalid_builder", "Builder must be controlled by the local player.");
    const CommandButton* button = findBuildButton(builder, args[1]);
    if (!button) return failure(id, "invalid_template", "Template is not a current construction command.");
    Coord3D center;
    Real radius, spacing;
    ObjectID limitValue;
    if (!worldPosition(args[2], args[3], center) || !parseCoordinate(args[4], radius) || radius < 0 || radius > 2000 ||
        !parseCoordinate(args[5], spacing) || spacing < 10 || spacing > 2000 || !parseId(args[6], limitValue) || static_cast<int>(limitValue) > 32)
        return failure(id, "invalid_argument", "Use radius 0..2000, spacing 10..2000, limit 1..32 and an in-bounds center.");
    const char* restriction = commandReason(builder, button);
    Region3D bounds;
    TheTerrainLogic->getExtent(&bounds);
    std::vector<Coord3D> candidates;
    const int rings = static_cast<int>(std::ceil(radius / spacing));
    for (int ring = 0; ring <= rings && candidates.size() < 1024; ++ring)
        for (int dy = -ring; dy <= ring && candidates.size() < 1024; ++dy)
            for (int dx = -ring; dx <= ring && candidates.size() < 1024; ++dx)
            {
                if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
                Coord3D p = center;
                p.x += dx * spacing; p.y += dy * spacing;
                if ((p.x-center.x)*(p.x-center.x)+(p.y-center.y)*(p.y-center.y) > radius*radius ||
                    p.x < bounds.lo.x || p.x > bounds.hi.x || p.y < bounds.lo.y || p.y > bounds.hi.y) continue;
                p.z = TheTerrainLogic->getGroundHeight(p.x, p.y);
                if (std::isfinite(p.z)) candidates.push_back(p);
            }
    std::stable_sort(candidates.begin(), candidates.end(), [&center](const Coord3D& a, const Coord3D& b) {
        return (a.x-center.x)*(a.x-center.x)+(a.y-center.y)*(a.y-center.y) <
            (b.x-center.x)*(b.x-center.x)+(b.y-center.y)*(b.y-center.y);
    });
    std::ostringstream out = jsonStream();
    out << "{\"builder_available\":" << (restriction == nullptr) << ",\"reason\":" << (restriction ? quote(restriction) : "null") << ",\"sites\":[";
    int found = 0, checked = 0;
    for (const Coord3D& p : candidates)
    {
        ++checked;
        if (legalLocation(builder, button, p, 0) != LBC_OK) continue;
        if (found++) out << ',';
        out << "{\"position\":" << positionJson(p) << ",\"angle\":0}";
        if (found == static_cast<int>(limitValue)) break;
    }
    out << "],\"checked\":" << checked << ",\"search_limited\":" << (candidates.size() == 1024 || static_cast<size_t>(checked) < candidates.size()) << '}';
    return success(id, out.str());
}

std::string buildLine(const std::string& id, const std::vector<std::string>& args)
{
    Object* builder = parseOwned(args[0]);
    if (!builder) return failure(id, "invalid_builder", "Builder must be a live owned object.");
    const CommandButton* button = findBuildButton(builder, args[1]);
    if (!button || !TheBuildAssistant->isLineBuildTemplate(button->getThingTemplate()))
        return failure(id, "invalid_template", "Template must be a line-building command in the builder's command set.");
    if (const char* reason = commandReason(builder, button)) return failure(id, reason, "Construction is currently unavailable.");
    Coord3D start, end;
    Real angle;
    if (!worldPosition(args[2], args[3], start) || !worldPosition(args[4], args[5], end) || !parseCoordinate(args[6], angle))
        return failure(id, "invalid_argument", "Supply two in-bounds positions and a finite angle in radians.");
    const ThingTemplate* thing = button->getThingTemplate();
    const Real tileSize = thing->getTemplateGeometryInfo().getMajorRadius() * 2.0f;
    const Real dx = end.x-start.x, dy = end.y-start.y;
    const Real length = std::sqrt(dx*dx + dy*dy);
    if (!std::isfinite(tileSize) || tileSize <= 0 || !std::isfinite(length) || !TheGlobalData ||
        TheGlobalData->m_maxLineBuildObjects < 1 || TheGlobalData->m_maxLineBuildObjects > 128)
        return failure(id, "invalid_line_geometry", "Line geometry or configured line limit is unsupported.");
    const int maximum = TheGlobalData->m_maxLineBuildObjects;
    const int tiles = std::min(maximum, REAL_TO_INT(std::min(length / tileSize, static_cast<Real>(maximum))) + 1);
    for (int i = 0; i < tiles; ++i)
    {
        Coord3D p = start;
        if (length > 0) { p.x += dx/length*(tileSize*i); p.y += dy/length*(tileSize*i); }
        p.z = TheTerrainLogic->getGroundHeight(p.x, p.y);
        LegalBuildCode legal = legalLocation(builder, button, p, angle);
        if (legal != LBC_OK) return failure(id, locationReason(legal), "A segment of the requested line cannot be built.");
    }
    const unsigned long long totalCost = static_cast<unsigned long long>(std::max(0, thing->calcCostToBuild(localPlayer()))) * tiles;
    if (totalCost > localPlayer()->getMoney()->countMoney()) return failure(id, "insufficient_money", "Not enough money for all segments.");
    markSingleUse(builder, button);
    SelectionScope selection(std::vector<ObjectID>(1, builder->getID()));
    GameMessage* message = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT_LINE);
    message->appendIntegerArgument(thing->getTemplateID());
    message->appendLocationArgument(start);
    message->appendRealArgument(angle);
    message->appendLocationArgument(end);
    return queued(id, ",\"segments\":" + std::to_string(tiles));
}

void emitSpecial(Object* object, const CommandButton* button, Object* target, const Coord3D* position, Real angle)
{
    GameMessage* message;
    const bool constructed = button->getCommandType() == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT ||
        button->getCommandType() == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT;
    if (!constructed && (button->getOptions() & COMMAND_OPTION_NEED_OBJECT_TARGET))
    {
        message = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT);
        message->appendIntegerArgument(button->getSpecialPowerTemplate()->getID());
        message->appendObjectIDArgument(target->getID());
    }
    else if (constructed || (button->getOptions() & NEED_TARGET_POS))
    {
        message = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
        message->appendIntegerArgument(button->getSpecialPowerTemplate()->getID());
        message->appendLocationArgument(*position);
        message->appendRealArgument(constructed ? angle : INVALID_ANGLE);
        message->appendObjectIDArgument(target ? target->getID() : INVALID_ID);
    }
    else
    {
        message = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER);
        message->appendIntegerArgument(button->getSpecialPowerTemplate()->getID());
    }
    message->appendIntegerArgument(button->getOptions());
    message->appendObjectIDArgument(object->getID());
}

const char* specialReason(Object* object, const CommandButton* button, Object* target, const Coord3D* position, Real angle)
{
    if (!TheActionManager || !button->getSpecialPowerTemplate()) return "missing_special_power";
    const bool constructed = button->getCommandType() == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT ||
        button->getCommandType() == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT;
    if (constructed)
    {
        if (!position || !button->getThingTemplate()) return "position_required";
        LegalBuildCode legal = legalLocation(object, button, *position, angle);
        if (legal != LBC_OK) return locationReason(legal);
    }
    if (!constructed && (button->getOptions() & COMMAND_OPTION_NEED_OBJECT_TARGET))
    {
        if (!target || !button->isValidObjectTarget(object, target)) return "invalid_target";
        return TheActionManager->canDoSpecialPowerAtObject(object, target, CMD_FROM_PLAYER,
            button->getSpecialPowerTemplate(), button->getOptions(), true) ? nullptr : "special_power_unavailable";
    }
    if (constructed || (button->getOptions() & NEED_TARGET_POS))
        return position && TheActionManager->canDoSpecialPowerAtLocation(object, position, CMD_FROM_PLAYER,
            button->getSpecialPowerTemplate(), target, button->getOptions(), true) ? nullptr : "special_power_unavailable";
    return TheActionManager->canDoSpecialPower(object, button->getSpecialPowerTemplate(), CMD_FROM_PLAYER,
        button->getOptions(), true) ? nullptr : "special_power_unavailable";
}

const CommandButton* captureButton(Object* object)
{
    const CommandSet* set = TheControlBar->findCommandSet(object->getCommandSetString());
    if (set)
        for (int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
        {
            const CommandButton* button = set->getCommandButton(i);
            if (button && button->getCommandType() == GUI_COMMAND_SPECIAL_POWER && button->getSpecialPowerTemplate())
            {
                SpecialPowerType type = button->getSpecialPowerTemplate()->getSpecialPowerType();
                if (type == SPECIAL_INFANTRY_CAPTURE_BUILDING || type == SPECIAL_BLACKLOTUS_CAPTURE_BUILDING)
                    return button;
            }
        }
    return nullptr;
}

bool canAttackGround(Object* object, const Coord3D& position)
{
    if (!object->isSelectable() || !object->isAbleToAttack() || object->getContainedBy() ||
        object->isDisabled() || object->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED) ||
        object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
        return false;
    // Match CommandTranslator::canObjectForceAttack, including stationary
    // defenses whose weapons belong to spawned soldiers or garrison occupants.
    Object* weaponSource = object;
    if (object->isKindOf(KINDOF_IMMOBILE) || object->isKindOf(KINDOF_SPAWNS_ARE_THE_WEAPONS))
    {
        SpawnBehaviorInterface* spawn = object->getSpawnBehaviorInterface();
        if (spawn)
        {
            Object* slave = spawn->getClosestSlave(&position);
            if (slave) weaponSource = slave;
        }
        else if (object->getAbleToUseWeaponAgainstTarget(ATTACK_NEW_TARGET, nullptr,
            &position, CMD_FROM_PLAYER) != ATTACKRESULT_POSSIBLE)
        {
            ContainModuleInterface* contain = object->getContain();
            Object* rider = contain ? contain->getClosestRider(&position) : nullptr;
            if (rider) weaponSource = rider;
        }
    }
    const CanAttackResult result = weaponSource->getAbleToUseWeaponAgainstTarget(
        ATTACK_NEW_TARGET, nullptr, &position, CMD_FROM_PLAYER);
    return result == ATTACKRESULT_POSSIBLE || result == ATTACKRESULT_POSSIBLE_AFTER_MOVING;
}

const char* attackResultName(CanAttackResult result)
{
    switch (result)
    {
    case ATTACKRESULT_POSSIBLE: return "possible";
    case ATTACKRESULT_POSSIBLE_AFTER_MOVING: return "possible_after_moving";
    case ATTACKRESULT_INVALID_SHOT: return "invalid_shot";
    default: return "not_possible";
    }
}

std::string attackDiagnosis(Object* object, Object* target, const Coord3D* position, AbleToAttackType attackType)
{
    const CanAttackResult result = target ?
        TheActionManager->getCanAttackObject(object, target, CMD_FROM_PLAYER, attackType) :
        object->getAbleToUseWeaponAgainstTarget(attackType, nullptr, position, CMD_FROM_PLAYER);
    WeaponSlotType currentSlot = PRIMARY_WEAPON;
    const Weapon* current = object->getCurrentWeapon(&currentSlot);
    std::ostringstream out = jsonStream();
    out << "{\"engine_result\":" << quote(attackResultName(result))
        << ",\"current_weapon_slot\":" << (current ? std::to_string(static_cast<int>(currentSlot)) : "null")
        << ",\"weapons\":[";
    bool first = true;
    for (int i = 0; i < WEAPONSLOT_COUNT; ++i)
    {
        const WeaponSlotType slot = static_cast<WeaponSlotType>(i);
        const Weapon* weapon = object->getWeaponInWeaponSlot(slot);
        if (!weapon) continue;
        if (!first) out << ',';
        first = false;
        const CanAttackResult slotResult = target ?
            object->getAbleToAttackSpecificObject(attackType, target, CMD_FROM_PLAYER, slot) :
            object->getAbleToUseWeaponAgainstTarget(attackType, nullptr, position, CMD_FROM_PLAYER, slot);
        out << "{\"slot\":" << i << ",\"name\":" << quote(weapon->getName().str())
            << ",\"engine_result\":" << quote(attackResultName(slotResult))
            << ",\"range\":" << number(weapon->getAttackRange(object))
            << ",\"minimum_range\":" << number(weapon->getTemplate()->getMinimumAttackRange())
            << ",\"in_range\":" << static_cast<bool>(target ? weapon->isWithinAttackRange(object, target) : weapon->isWithinAttackRange(object, position))
            << ",\"within_target_pitch\":" << (target ? (weapon->isWithinTargetPitch(object, target) ? "true" : "false") : "null")
            << ",\"clear_terrain_line_of_sight\":" << static_cast<bool>(target ?
                weapon->isClearFiringLineOfSightTerrain(object, target) : weapon->isClearFiringLineOfSightTerrain(object, *position))
            << ",\"damage_weapon\":" << static_cast<bool>(weapon->isDamageWeapon())
            << ",\"ammo\":" << weapon->getRemainingAmmo()
            << ",\"status\":" << static_cast<int>(weapon->getStatus())
            << ",\"command_source_mask\":" << object->getWeaponInWeaponSlotCommandSourceMask(slot) << '}';
    }
    out << "],\"firing_guaranteed\":false}";
    return out.str();
}

const char* orderReason(Object* object, const std::string& order, Object* target, const Coord3D& position, bool readOnly)
{
    if (!object) return "unit_not_owned_or_missing";
    bool valid = false;
    if (order == "move" || order == "force_move" || order == "scatter") valid = mobile(object);
    else if (order == "attack_move") valid = canAttackMove(object);
    else if (order == "guard" || order == "guard_no_pursuit" || order == "guard_air")
        valid = object->getAIUpdateInterface() && !object->isDisabled() && !object->getContainedBy();
    else if (order == "stop") valid = object->isSelectable() && object->getAIUpdateInterface();
    else if (order == "attack" || order == "force_attack")
    {
        if (!object->isAbleToAttack()) return "cannot_attack";
        const CanAttackResult result = TheActionManager->getCanAttackObject(object, target, CMD_FROM_PLAYER,
            order == "force_attack" ? ATTACK_NEW_TARGET_FORCED : ATTACK_NEW_TARGET);
        return result >= ATTACKRESULT_POSSIBLE_AFTER_MOVING ? nullptr : attackResultName(result);
    }
    else if (order == "attack_ground") valid = canAttackGround(object, position);
    else if (order == "repair") valid = target->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ?
        TheActionManager->canResumeConstructionOf(object, target, CMD_FROM_PLAYER) : TheActionManager->canRepairObject(object, target, CMD_FROM_PLAYER);
    else if (order == "gather") valid = object->isKindOf(KINDOF_HARVESTER) && TheActionManager->canDockAt(object, target, CMD_FROM_PLAYER);
    else if (order == "get_repaired") valid = mobile(object) && TheActionManager->canGetRepairedAt(object, target, CMD_FROM_PLAYER);
    else if (order == "get_healed") valid = mobile(object) && TheActionManager->canGetHealedAt(object, target, CMD_FROM_PLAYER);
    else if (order == "salvage") valid = mobile(object) && object->isKindOf(KINDOF_SALVAGER) && target->isSalvageCrate();
    else if (order == "enter") valid = TheActionManager->canEnterObject(object, target, CMD_FROM_PLAYER, CHECK_CAPACITY);
    else if (order == "exit") valid = object->getContainedBy() && object->getContainedBy()->getControllingPlayer() == localPlayer() &&
        !object->getContainedBy()->isDisabledByType(DISABLED_SUBDUED);
    else if (order == "capture")
    {
        const CommandButton* button = captureButton(object);
        if (!button) return "missing_capture_command";
        if (const char* reason = commandReason(object, button, readOnly)) return reason;
        if (const char* reason = specialReason(object, button, target, nullptr, 0)) return reason;
        valid = TheActionManager->canCaptureBuilding(object, target, CMD_FROM_PLAYER);
    }
    if (valid) return nullptr;
    if (object->isDisabled() || object->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED)) return "object_disabled";
    if (object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) return "under_construction";
    if (object->getContainedBy()) return "unit_contained";
    if (order == "move" || order == "force_move" || order == "scatter") return "not_mobile";
    if (order == "attack_move") return "cannot_attack_move";
    if (order == "repair") return "cannot_repair_target";
    if (order == "gather") return "cannot_gather_target";
    if (order == "get_repaired") return "cannot_get_repaired_at_target";
    if (order == "get_healed") return "cannot_get_healed_at_target";
    if (order == "salvage") return "cannot_salvage_target";
    if (order == "enter") return "cannot_enter_target_or_capacity_full";
    if (order == "exit") return "not_in_owned_container";
    if (order == "capture") return "cannot_capture_target";
    if (order == "attack_ground") return "cannot_attack_ground";
    return "order_unavailable";
}

std::string unitOrder(const std::string& id, const std::vector<std::string>& args,
    const CommandButton* sourceButton = nullptr, bool checkOnly = false)
{
    const std::string& order = args[1];
    const bool guard = order == "guard" || order == "guard_no_pursuit" || order == "guard_air";
    const GuardMode guardMode = order == "guard_no_pursuit" ? GUARDMODE_GUARD_WITHOUT_PURSUIT :
        order == "guard_air" ? GUARDMODE_GUARD_FLYING_UNITS_ONLY : GUARDMODE_NORMAL;
    if (order != "move" && order != "attack" && order != "attack_move" && order != "guard" && order != "stop" &&
        order != "repair" && order != "gather" && order != "enter" && order != "exit" && order != "capture" &&
        order != "attack_ground" && order != "force_attack" && order != "force_move" && order != "scatter" &&
        order != "get_repaired" && order != "get_healed" && order != "salvage" && !guard)
        return failure(id, "unsupported_order", "Unknown unit order.");
    if (args[5] != "0" && args[5] != "1") return failure(id, "invalid_argument", "queued must be 0 or 1.");
    if (args[5] == "1" && order != "move") return failure(id, "unsupported_queue", "Only move orders support appending waypoints.");
    Object* target;
    if (!parseTarget(args[2], target)) return failure(id, "invalid_target", "Target must currently be visible or owned.");
    Coord3D position = {0, 0, 0};
    bool hasPosition = !args[3].empty() || !args[4].empty();
    if (hasPosition && !worldPosition(args[3], args[4], position)) return failure(id, "invalid_position", "Position must be finite and within the map.");
    if ((order == "move" || order == "attack_move" || order == "force_move" || order == "attack_ground") && !hasPosition)
        return failure(id, "position_required", "This order needs x and y.");
    if ((order == "attack" || order == "force_attack" || order == "repair" || order == "gather" ||
        order == "enter" || order == "capture" || order == "get_repaired" || order == "get_healed" ||
        order == "salvage") && !target)
        return failure(id, "target_required", "This order needs an observed target object.");
    if (!TheActionManager) return failure(id, "control_unavailable", "Action validation is unavailable.");
    std::vector<ObjectID> ids;
    std::vector<Object*> objects;
    std::ostringstream checks = jsonStream();
    bool allEligible = true;
    size_t start = 0;
    do
    {
        size_t end = args[0].find(',', start);
        ObjectID objectId;
        if (!parseId(args[0].substr(start, end == std::string::npos ? end : end-start), objectId) ||
            ids.size() >= 64 || std::find(ids.begin(), ids.end(), objectId) != ids.end())
            return failure(id, "invalid_argument", "Supply 1..64 distinct positive unit IDs.");
        Object* object = ownedObject(objectId);
        if (!object && !checkOnly) return failure(id, "invalid_unit", "Every unit must be alive and owned.");
        const char* reason = orderReason(object, order, target, position, checkOnly);
        if (reason) allEligible = false;
        if (reason && !checkOnly) return failure(id, "order_unavailable", "Unit " +
            std::to_string(static_cast<int>(objectId)) + ": " + reason + ". Use check_order for details.");
        if (checkOnly)
        {
            if (!ids.empty()) checks << ',';
            checks << "{\"object_id\":" << static_cast<int>(objectId) << ",\"eligible\":" << (reason == nullptr)
                << ",\"reason\":" << (reason ? quote(reason) : "null");
            if (object && (order == "attack" || order == "force_attack" || order == "attack_ground"))
                checks << ",\"attack\":" << attackDiagnosis(object, order == "attack_ground" ? nullptr : target,
                    &position, order == "force_attack" ? ATTACK_NEW_TARGET_FORCED : ATTACK_NEW_TARGET);
            checks << '}';
        }
        ids.push_back(objectId); objects.push_back(object);
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    if (checkOnly)
    {
        std::ostringstream out = jsonStream();
        out << "{\"frame\":" << TheGameLogic->getFrame() << ",\"order\":" << quote(order)
            << ",\"target_id\":" << (target ? std::to_string(static_cast<int>(target->getID())) : "null")
            << ",\"all_eligible\":" << allEligible << ",\"units\":[" << checks.str() << "]}";
        return success(id, out.str());
    }
    if (sourceButton) markSingleUse(objects.front(), sourceButton);
    SelectionScope selection(ids);
    if (order == "capture")
        for (Object* object : objects) emitSpecial(object, captureButton(object), target, nullptr, 0);
    else if (order == "exit")
        for (Object* object : objects)
        {
            // MSG_EXIT uses the selected container and an explicit passenger ID.
            selectObjects(std::vector<ObjectID>(1, object->getContainedBy()->getID()));
            TheMessageStream->appendMessage(GameMessage::MSG_EXIT)->appendObjectIDArgument(object->getID());
        }
    else if (guard && !target && !hasPosition)
        for (Object* object : objects)
        {
            selectObjects(std::vector<ObjectID>(1, object->getID()));
            GameMessage* message = TheMessageStream->appendMessage(GameMessage::MSG_DO_GUARD_POSITION);
            message->appendLocationArgument(*object->getPosition());
            message->appendIntegerArgument(guardMode);
        }
    else
    {
        GameMessage::Type type = GameMessage::MSG_DO_STOP;
        if (order == "move") type = args[5] == "1" ? GameMessage::MSG_ADD_WAYPOINT : GameMessage::MSG_DO_MOVETO;
        else if (order == "force_move") type = GameMessage::MSG_DO_FORCEMOVETO;
        else if (order == "scatter") type = GameMessage::MSG_DO_SCATTER;
        else if (order == "attack_move") type = GameMessage::MSG_DO_ATTACKMOVETO;
        else if (order == "attack") type = GameMessage::MSG_DO_ATTACK_OBJECT;
        else if (order == "force_attack") type = GameMessage::MSG_DO_FORCE_ATTACK_OBJECT;
        else if (order == "attack_ground") type = GameMessage::MSG_DO_FORCE_ATTACK_GROUND;
        else if (order == "repair") type = target->getStatusBits().test(OBJECT_STATUS_UNDER_CONSTRUCTION) ?
            GameMessage::MSG_RESUME_CONSTRUCTION : GameMessage::MSG_DO_REPAIR;
        else if (order == "gather") type = GameMessage::MSG_DOCK;
        else if (order == "get_repaired") type = GameMessage::MSG_GET_REPAIRED;
        else if (order == "get_healed") type = GameMessage::MSG_GET_HEALED;
        else if (order == "salvage") type = GameMessage::MSG_DO_SALVAGE;
        else if (order == "enter") type = GameMessage::MSG_ENTER;
        else if (guard) type = target ? GameMessage::MSG_DO_GUARD_OBJECT : GameMessage::MSG_DO_GUARD_POSITION;
        GameMessage* message = TheMessageStream->appendMessage(type);
        if (order == "enter") message->appendObjectIDArgument(INVALID_ID);
        if (order == "attack" || order == "force_attack" || order == "repair" || order == "gather" ||
            order == "get_repaired" || order == "get_healed" || order == "enter" || (guard && target))
            message->appendObjectIDArgument(target->getID());
        else if (order == "salvage") message->appendLocationArgument(*target->getPosition());
        else if (order == "move" || order == "force_move" || order == "attack_move" || order == "attack_ground" || guard)
            message->appendLocationArgument(position);
        if (guard) message->appendIntegerArgument(guardMode);
    }
    return queued(id, ",\"order\":" + quote(order));
}

std::string useCommand(const std::string& id, const std::vector<std::string>& args)
{
    Object* object = parseOwned(args[0]);
    if (!object) return failure(id, "invalid_object", "Source must be a live owned object.");
    const CommandButton* button = findButton(object, args[1]);
    if (!button) return failure(id, "invalid_command", "Command is not in this object's current command set or player shortcuts.");
    if (const char* reason = commandReason(object, button)) return failure(id, reason, "The command is unavailable.");
    Object* target;
    if (!parseTarget(args[2], target)) return failure(id, "invalid_target", "Target must currently be visible or owned.");
    Coord3D position;
    bool hasPosition = !args[3].empty() || !args[4].empty();
    Real angle;
    if (!parseCoordinate(args[5], angle) || (hasPosition && !worldPosition(args[3], args[4], position)))
        return failure(id, "invalid_argument", "Provide finite coordinates and angle in radians.");
    GUICommandType type = button->getCommandType();
    if (type == GUI_COMMAND_DOZER_CONSTRUCT)
        return buildCommand(id, {args[0], button->getThingTemplate()->getName().str(), args[3], args[4], args[5]}, true);
    if (type == GUI_COMMAND_SPECIAL_POWER || type == GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT ||
        type == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT || type == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT)
    {
        if (const char* reason = specialReason(object, button, target, hasPosition ? &position : nullptr, angle))
            return failure(id, reason, "Special power cannot be used on this target or location.");
        markSingleUse(object, button);
        SelectionScope selection(std::vector<ObjectID>(1, object->getID()));
        emitSpecial(object, button, target, hasPosition ? &position : nullptr, angle);
        return queued(id, ",\"command\":" + quote(args[1]));
    }
    if (type == GUI_COMMAND_ATTACK_MOVE || type == GUI_COMMAND_STOP)
        return unitOrder(id, {args[0], type == GUI_COMMAND_STOP ? "stop" : "attack_move", args[2], args[3], args[4], "0"}, button);
    if (type == GUI_COMMAND_GUARD || type == GUI_COMMAND_GUARD_WITHOUT_PURSUIT || type == GUI_COMMAND_GUARD_FLYING_UNITS_ONLY)
    {
        if (!object->getAIUpdateInterface()) return failure(id, "order_unavailable", "Guard requires an AI movement interface.");
        if (target && !button->isValidObjectTarget(object, target)) return failure(id, "invalid_target", "Invalid guard target.");
        if (!hasPosition && !target) position = *object->getPosition();
    }
    if (type == GUI_COMMAND_SET_RALLY_POINT && !hasPosition)
        return failure(id, "position_required", "Rally point needs x and y.");
    if (type == GUI_COMMAND_FIRE_WEAPON)
    {
        bool valid = false;
        if (button->getOptions() & COMMAND_OPTION_NEED_OBJECT_TARGET)
        {
            if (target && (button->getOptions() & ATTACK_OBJECTS_POSITION))
            {
                position = *target->getPosition();
                hasPosition = true;
                valid = button->isValidObjectTarget(object, target) &&
                    TheActionManager->canFireWeaponAtLocation(object, &position, CMD_FROM_PLAYER, button->getWeaponSlot(), target);
            }
            else valid = target && button->isValidObjectTarget(object, target) && TheActionManager->canFireWeaponAtObject(object, target, CMD_FROM_PLAYER, button->getWeaponSlot());
        }
        else if (button->getOptions() & NEED_TARGET_POS)
            valid = hasPosition && TheActionManager->canFireWeaponAtLocation(object, &position, CMD_FROM_PLAYER, button->getWeaponSlot(), target);
        else valid = TheActionManager->canFireWeapon(object, button->getWeaponSlot(), CMD_FROM_PLAYER);
        if (!valid) return failure(id, "weapon_unavailable", "Weapon cannot fire at this target.");
    }
    if (type == GUICOMMANDMODE_HIJACK_VEHICLE || type == GUICOMMANDMODE_CONVERT_TO_CARBOMB || type == GUICOMMANDMODE_SABOTAGE_BUILDING)
    {
        bool valid = target && (type == GUICOMMANDMODE_HIJACK_VEHICLE ? TheActionManager->canHijackVehicle(object, target, CMD_FROM_PLAYER) :
            type == GUICOMMANDMODE_CONVERT_TO_CARBOMB ? TheActionManager->canConvertObjectToCarBomb(object, target, CMD_FROM_PLAYER) :
            TheActionManager->canSabotageBuilding(object, target, CMD_FROM_PLAYER));
        if (!valid) return failure(id, "invalid_target", "The context command cannot use that target.");
    }
    if (type == GUI_COMMAND_EXIT_CONTAINER && (!target || target->getControllingPlayer() != localPlayer() || target->getContainedBy() != object))
        return failure(id, "invalid_target", "Exit target must be an owned passenger in the source container.");
    if (type == GUI_COMMAND_COMBATDROP)
    {
        if (target)
        {
            if (!button->isValidObjectTarget(object, target) || !TheActionManager->canEnterObject(object, target, CMD_FROM_PLAYER, COMBATDROP_INTO))
                return failure(id, "invalid_target", "The transport cannot combat-drop into this target.");
        }
        else if (!hasPosition) return failure(id, "position_required", "Combat drop requires a target or location.");
    }
    markSingleUse(object, button);
    SelectionScope selection(std::vector<ObjectID>(1, object->getID()));
    GameMessage* message = nullptr;
    switch (type)
    {
    case GUI_COMMAND_UNIT_BUILD:
    {
        ProductionID productionId = object->getProductionUpdateInterface()->requestUniqueUnitID();
        message = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UNIT_CREATE);
        message->appendIntegerArgument(button->getThingTemplate()->getTemplateID());
        message->appendIntegerArgument(productionId);
        return queued(id, ",\"production_id\":" + std::to_string(static_cast<int>(productionId)));
    }
    case GUI_COMMAND_PLAYER_UPGRADE: case GUI_COMMAND_OBJECT_UPGRADE:
        message = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UPGRADE);
        message->appendObjectIDArgument(object->getID());
        message->appendIntegerArgument(button->getUpgradeTemplate()->getUpgradeNameKey());
        break;
    case GUI_COMMAND_DOZER_CONSTRUCT_CANCEL: TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CANCEL_CONSTRUCT); break;
    case GUI_COMMAND_EVACUATE: TheMessageStream->appendMessage(GameMessage::MSG_EVACUATE); break;
    case GUI_COMMAND_EXECUTE_RAILED_TRANSPORT: TheMessageStream->appendMessage(GameMessage::MSG_EXECUTE_RAILED_TRANSPORT); break;
    case GUI_COMMAND_COMBATDROP:
        message = TheMessageStream->appendMessage(target ? GameMessage::MSG_COMBATDROP_AT_OBJECT : GameMessage::MSG_COMBATDROP_AT_LOCATION);
        if (target) message->appendObjectIDArgument(target->getID()); else message->appendLocationArgument(position);
        break;
    case GUI_COMMAND_EXIT_CONTAINER: TheMessageStream->appendMessage(GameMessage::MSG_EXIT)->appendObjectIDArgument(target->getID()); break;
    case GUI_COMMAND_SELL: TheMessageStream->appendMessage(GameMessage::MSG_SELL); break;
    case GUI_COMMAND_HACK_INTERNET: TheMessageStream->appendMessage(GameMessage::MSG_INTERNET_HACK); break;
    case GUI_COMMAND_TOGGLE_OVERCHARGE: TheMessageStream->appendMessage(GameMessage::MSG_TOGGLE_OVERCHARGE); break;
    case GUI_COMMAND_SWITCH_WEAPON: TheMessageStream->appendMessage(GameMessage::MSG_SWITCH_WEAPONS)->appendIntegerArgument(button->getWeaponSlot()); break;
    case GUI_COMMAND_SET_RALLY_POINT:
        message = TheMessageStream->appendMessage(GameMessage::MSG_SET_RALLY_POINT);
        message->appendObjectIDArgument(object->getID()); message->appendLocationArgument(position); break;
    case GUI_COMMAND_GUARD: case GUI_COMMAND_GUARD_WITHOUT_PURSUIT: case GUI_COMMAND_GUARD_FLYING_UNITS_ONLY:
        message = TheMessageStream->appendMessage(target ? GameMessage::MSG_DO_GUARD_OBJECT : GameMessage::MSG_DO_GUARD_POSITION);
        if (target) message->appendObjectIDArgument(target->getID()); else message->appendLocationArgument(position);
        message->appendIntegerArgument(type == GUI_COMMAND_GUARD_WITHOUT_PURSUIT ? GUARDMODE_GUARD_WITHOUT_PURSUIT :
            type == GUI_COMMAND_GUARD_FLYING_UNITS_ONLY ? GUARDMODE_GUARD_FLYING_UNITS_ONLY : GUARDMODE_NORMAL);
        break;
    case GUICOMMANDMODE_HIJACK_VEHICLE: case GUICOMMANDMODE_CONVERT_TO_CARBOMB: case GUICOMMANDMODE_SABOTAGE_BUILDING:
        message = TheMessageStream->appendMessage(GameMessage::MSG_ENTER);
        message->appendObjectIDArgument(INVALID_ID); message->appendObjectIDArgument(target->getID()); break;
    case GUI_COMMAND_FIRE_WEAPON:
        if ((button->getOptions() & COMMAND_OPTION_NEED_OBJECT_TARGET) && !(button->getOptions() & ATTACK_OBJECTS_POSITION))
        {
            message = TheMessageStream->appendMessage(GameMessage::MSG_DO_WEAPON_AT_OBJECT);
            message->appendIntegerArgument(button->getWeaponSlot()); message->appendObjectIDArgument(target->getID());
            message->appendIntegerArgument(button->getMaxShotsToFire());
        }
        else if (button->getOptions() & (NEED_TARGET_POS | ATTACK_OBJECTS_POSITION))
        {
            message = TheMessageStream->appendMessage(GameMessage::MSG_DO_WEAPON_AT_LOCATION);
            message->appendIntegerArgument(button->getWeaponSlot()); message->appendLocationArgument(position);
            message->appendIntegerArgument(button->getMaxShotsToFire()); message->appendObjectIDArgument(target ? target->getID() : INVALID_ID);
        }
        else
        {
            message = TheMessageStream->appendMessage(GameMessage::MSG_DO_WEAPON);
            message->appendIntegerArgument(button->getWeaponSlot()); message->appendIntegerArgument(button->getMaxShotsToFire());
        }
        break;
    default: return failure(id, "unsupported_command_type", "This button has no command adapter.");
    }
    return queued(id, ",\"command\":" + quote(args[1]));
}

std::string cancelProduction(const std::string& id, const std::vector<std::string>& args)
{
    Object* object = parseOwned(args[0]);
    ObjectID productionId = INVALID_ID;
    const bool namedUpgrade = args[1].compare(0, 8, "upgrade:") == 0 && args[1].size() > 8;
    if (!object || !object->getProductionUpdateInterface() || (!namedUpgrade && args[1] != "0" && !parseId(args[1], productionId)))
        return failure(id, "invalid_argument", "Supply an owned producer and production ID or upgrade:UpgradeName.");
    ProductionUpdateInterface* production = object->getProductionUpdateInterface();
    const ProductionEntry* found = nullptr;
    for (const ProductionEntry* entry = production->firstProduction(); entry; entry = production->nextProduction(entry))
    {
        bool matches = namedUpgrade ? entry->getProductionType() == PRODUCTION_UPGRADE && entry->getProductionUpgrade() &&
            args[1].substr(8) == entry->getProductionUpgrade()->getUpgradeName().str() :
            static_cast<int>(entry->getProductionID()) == static_cast<int>(productionId);
        if (matches)
        {
            if (found) return failure(id, "ambiguous_production", "Multiple upgrades use ID 0; specify upgrade:UpgradeName.");
            found = entry;
        }
    }
    if (!found) return failure(id, "production_not_found", "The production ID is not in this producer's queue.");
    if (found->getProductionType() != PRODUCTION_UNIT &&
        (found->getProductionType() != PRODUCTION_UPGRADE || !found->getProductionUpgrade()))
        return failure(id, "unsupported_production", "Production type cannot be cancelled.");
    SelectionScope selection(std::vector<ObjectID>(1, object->getID()));
    if (found->getProductionType() == PRODUCTION_UNIT)
        TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UNIT_CREATE)->appendIntegerArgument(static_cast<int>(productionId));
    else if (found->getProductionType() == PRODUCTION_UPGRADE && found->getProductionUpgrade())
        TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UPGRADE)->appendIntegerArgument(found->getProductionUpgrade()->getUpgradeNameKey());
    return queued(id);
}

std::string playerActions(const std::string& id)
{
    if (!TheScienceStore) return failure(id, "control_unavailable", "Science data is unavailable.");
    Player* player = localPlayer();
    const ScienceVec sciences = factionSciences();
    std::ostringstream out = jsonStream();
    out << "{\"rank\":" << player->getRankLevel() << ",\"science_points\":" << player->getSciencePurchasePoints() << ",\"sciences\":[";
    bool first = true;
    for (ScienceType science : sciences)
    {
        if (player->isScienceHidden(science)) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"name\":" << quote(TheScienceStore->getInternalNameForScience(science).str())
            << ",\"cost\":" << TheScienceStore->getSciencePurchaseCost(science)
            << ",\"available\":" << static_cast<bool>(player->isCapableOfPurchasingScience(science))
            << ",\"owned\":" << static_cast<bool>(player->hasScience(science))
            << ",\"reason\":" << (player->hasScience(science) ? quote("already_owned") :
                player->isScienceDisabled(science) ? quote("science_disabled") :
                !TheScienceStore->playerHasPrereqsForScience(player, science) ? quote("missing_prerequisite") :
                TheScienceStore->getSciencePurchaseCost(science) == 0 ? quote("not_purchasable") :
                TheScienceStore->getSciencePurchaseCost(science) > player->getSciencePurchasePoints() ? quote("insufficient_science_points") : "null") << '}';
    }
    out << "],\"owned_sciences\":[";
    first = true;
    for (ScienceType science : player->getOwnedSciences())
    {
        if (!first) out << ',';
        first = false;
        out << quote(TheScienceStore->getInternalNameForScience(science).str());
    }
    out << "],\"shortcut_commands\":[";
    first = true;
    for (const ShortcutEntry& entry : shortcutEntries())
    {
        Object* source = shortcutSource(entry.button);
        if (!first) out << ',';
        first = false;
        out << "{\"source_id\":" << (source ? std::to_string(static_cast<int>(source->getID())) : "null")
            << ",\"command_set\":" << quote(entry.commandSet.str())
            << ",\"selector\":" << (entry.selector.isEmpty() ? "null" : quote(entry.selector.str()))
            << ",\"command\":" << buttonJson(source, entry.button, entry.slot) << '}';
    }
    out << "]}";
    return success(id, out.str());
}

std::string setRallyPoint(const std::string& id, const std::vector<std::string>& args)
{
    Object* object = parseOwned(args[0]);
    if (!object) return failure(id, "invalid_object", "Rally source must be alive and owned.");
    if (!object->getObjectExitInterface() || object->testStatus(OBJECT_STATUS_SOLD) ||
        object->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED))
        return failure(id, "rally_unavailable", "Object has no usable production exit.");
    Coord3D position;
    if (!worldPosition(args[1], args[2], position))
        return failure(id, "invalid_position", "Position must be finite and within the map.");
    // Stock dispatch performs the normal path check before applying this point.
    GameMessage* message = TheMessageStream->appendMessage(GameMessage::MSG_SET_RALLY_POINT);
    message->appendObjectIDArgument(object->getID());
    message->appendLocationArgument(position);
    return queued(id, ",\"object_id\":" + std::to_string(static_cast<int>(object->getID())) +
        ",\"position\":" + positionJson(position));
}
}

std::string ExecuteCommands(const std::string& id, const std::string& method, const std::vector<std::string>& args)
{
    using namespace Detail;
    const bool read = method == "get_actions" || method == "get_player_actions" || method == "check_build" ||
        method == "find_build_sites" || method == "check_order";
    int count = -1;
    if (method == "get_actions" || method == "purchase_science") count = 1;
    else if (method == "get_player_actions") count = 0;
    else if (method == "check_build" || method == "build") count = 5;
    else if (method == "find_build_sites" || method == "build_line") count = 7;
    else if (method == "unit_order" || method == "check_order" || method == "use_command") count = 6;
    else if (method == "cancel_production") count = 2;
    else if (method == "set_rally_point") count = 3;
    if (count < 0) return std::string();
    if (args.size() != static_cast<size_t>(count)) return failure(id, "invalid_argument", "Incorrect argument count.");
    if (!matchReady()) return failure(id, "not_in_match", "An active match is required.");
    if (!TheControlBar || !TheBuildAssistant || !TheTerrainLogic || !TheActionManager)
        return failure(id, "control_unavailable", "Game command systems are unavailable.");
    if (!read)
        if (const char* restriction = writeRestriction()) return failure(id, restriction, "Player commands are unavailable in the current game state.");
    if (method == "get_actions")
    {
        Object* object = parseOwned(args[0]);
        return object ? getActions(id, object) : failure(id, "invalid_object", "Object must be alive and owned.");
    }
    if (method == "get_player_actions") return playerActions(id);
    if (method == "check_build" || method == "build") return buildCommand(id, args, method == "build");
    if (method == "find_build_sites") return findBuildSites(id, args);
    if (method == "build_line") return buildLine(id, args);
    if (method == "unit_order") return unitOrder(id, args);
    if (method == "check_order") return unitOrder(id, args, nullptr, true);
    if (method == "set_rally_point") return setRallyPoint(id, args);
    if (method == "use_command") return useCommand(id, args);
    if (method == "cancel_production") return cancelProduction(id, args);
    if (method == "purchase_science")
    {
        if (!TheScienceStore) return failure(id, "control_unavailable", "Science data is unavailable.");
        ScienceType science = TheScienceStore->getScienceFromInternalName(AsciiString(args[0].c_str()));
        const ScienceVec sciences = factionSciences();
        if (std::find(sciences.begin(), sciences.end(), science) == sciences.end())
            return failure(id, "science_not_in_faction_menu", "Science is not offered by this faction's purchase menus.");
        if (science == SCIENCE_INVALID || localPlayer()->isScienceHidden(science) || !localPlayer()->isCapableOfPurchasingScience(science))
            return failure(id, "science_unavailable", "Science is unavailable, already owned, or lacks points/prerequisites.");
        TheMessageStream->appendMessage(GameMessage::MSG_PURCHASE_SCIENCE)->appendIntegerArgument(science);
        return queued(id, ",\"science\":" + quote(args[0]));
    }
    return std::string();
}
}
