#include "PreRTS.h"

#include "Common/AgentGameInternal.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameClient/Drawable.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/Armor.h"
#include "GameLogic/ArmorSet.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/AutoHealBehavior.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/DeployStyleAIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/SpecialPowerUpdateModule.h"
#include "GameLogic/Module/StealthDetectorUpdate.h"
#include "GameLogic/Module/StealthUpdate.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"
#include "GameLogic/Module/UpdateModule.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Pathfinder/PathfindCell.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/Weapon.h"
#include "GameNetwork/GameInfo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

namespace GeneralsAgent
{
namespace
{
using namespace Detail;
const size_t MAX_PAGE = 256;
const size_t MAX_RESULT_BYTES = 480 * 1024;
const size_t MAX_EVENTS = 512;
const size_t MAX_TRACKED_OBJECTS = 4096;
const size_t MAX_MAP_SAMPLES = 4096;

bool parseUnsigned(const std::string& text, unsigned long long maximum, unsigned long long& value)
{
    if (text.empty() || text.size() > 16)
        return false;
    value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9' || value > (maximum - static_cast<unsigned>(c - '0')) / 10)
            return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value <= maximum;
}

std::string unicodeJson(const UnicodeString& value)
{
    static const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    const WideChar* text = value.str();
    for (size_t i = 0; text && text[i]; ++i)
    {
        unsigned c = static_cast<unsigned short>(text[i]);
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
            result += static_cast<char>(c);
        else
        {
            result += "\\u";
            for (int shift = 12; shift >= 0; shift -= 4)
                result += hex[(c >> shift) & 15];
        }
    }
    return result + '"';
}

const char* relationshipTo(Player* player)
{
    if (player == localPlayer())
        return "own";
    if (!player || player == ThePlayerList->getNeutralPlayer())
        return "neutral";
    switch (localPlayer()->getRelationship(player->getDefaultTeam()))
    {
    case ALLIES: return "ally";
    case ENEMIES: return "enemy";
    default: return "neutral";
    }
}

// Use the local player's actual partition visibility, including any map reveal
// the engine grants after defeat, rather than an independent omniscient view.
bool visibleToLocal(Object* object)
{
    if (!object || !localPlayer() || !object->areModulesReady())
        return false;
    if (object->getControllingPlayer() == localPlayer())
        return true;
    if (object->testStatus(OBJECT_STATUS_MASKED) || (object->getDrawable() &&
        object->getDrawable()->isDrawableEffectivelyHidden()))
        return false;
    // Contained objects can lack partition data, which the engine reports as
    // CLEAR. They must not reveal a transport's or garrison's hidden contents.
    if (object->getContainedBy() || object->getIsUndetectedDefector() || !object->friend_getPartitionData())
        return false;
    const ObjectShroudStatus shroud = object->friend_getPartitionData()->getShroudedStatus(localPlayer()->getPlayerIndex());
    if (shroud != OBJECTSHROUD_CLEAR && shroud != OBJECTSHROUD_PARTIAL_CLEAR)
        return false;
    if (object->testStatus(OBJECT_STATUS_STEALTHED) && !object->testStatus(OBJECT_STATUS_DETECTED) &&
        localPlayer()->getRelationship(object->getTeam()) != ALLIES)
    {
        StealthUpdate* stealth = object->getStealth();
        if (!stealth || !stealth->isDisguised())
            return false;
    }
    return true;
}

StealthUpdate* disguiseForLocal(Object* object)
{
    StealthUpdate* stealth = object->getStealth();
    if (object->getControllingPlayer() != localPlayer() &&
        localPlayer()->getRelationship(object->getTeam()) != ALLIES &&
        !object->testStatus(OBJECT_STATUS_DETECTED) && stealth && stealth->isDisguised())
        return stealth;
    return nullptr;
}

const ThingTemplate* apparentTemplate(Object* object)
{
    StealthUpdate* disguise = disguiseForLocal(object);
    return disguise ? disguise->getDisguisedTemplate() : object->getTemplate();
}

const ModuleData* templateModule(const ThingTemplate* type, const char* name)
{
    if (!type)
        return nullptr;
    const ModuleInfo& modules = type->getBehaviorModuleInfo();
    for (Int i = 0; i < modules.getCount(); ++i)
        if (!std::strcmp(modules.getNthName(i).str(), name))
            return modules.getNthData(i);
    return nullptr;
}

bool usefulObject(Object* object)
{
    if (!object || !object->areModulesReady())
        return false;
    const ThingTemplate* type = apparentTemplate(object);
    // GeneralsX @feature Report observable tactical hazards and scouts,
    // including nonselectable ones, through the same fog/stealth filter as units.
    return type && (type->isKindOf(KINDOF_SELECTABLE) || type->isKindOf(KINDOF_STRUCTURE) ||
        type->isKindOf(KINDOF_SUPPLY_SOURCE) || type->isKindOf(KINDOF_SUPPLY_SOURCE_ON_PREVIEW) ||
        type->isKindOf(KINDOF_TECH_BUILDING) || type->isKindOf(KINDOF_MINE) ||
        type->isKindOf(KINDOF_BOOBY_TRAP) || type->isKindOf(KINDOF_PROJECTILE) ||
        type->isKindOf(KINDOF_DRONE) || templateModule(type, "StealthDetectorUpdate") ||
        (!disguiseForLocal(object) && object->isSalvageCrate()));
}

Player* apparentPlayer(Object* object)
{
    if (object->getControllingPlayer() == localPlayer())
        return localPlayer();
    StealthUpdate* disguise = disguiseForLocal(object);
    if (disguise)
    {
        const int index = disguise->getDisguisedPlayerIndex();
        if (index >= 0 && index < ThePlayerList->getPlayerCount())
            return ThePlayerList->getNthPlayer(index);
        return nullptr;
    }
    ContainModuleInterface* contain = object->getContain();
    const Player* player = contain ? contain->getApparentControllingPlayer(localPlayer()) : nullptr;
    return player ? const_cast<Player*>(player) : object->getControllingPlayer();
}

std::string upgradeList(Object* object, Player* player)
{
    std::ostringstream out = jsonStream();
    out << '[';
    size_t count = 0;
    for (const UpgradeTemplate* upgrade = TheUpgradeCenter ? TheUpgradeCenter->firstUpgradeTemplate() : nullptr;
         upgrade && count < 256; upgrade = upgrade->friend_getNext())
    {
        const bool complete = object ? object->hasUpgrade(upgrade) : player->hasUpgradeComplete(upgrade);
        if (!complete)
            continue;
        if (count++)
            out << ',';
        out << quote(upgrade->getUpgradeName().str());
    }
    out << ']';
    return out.str();
}

const char* aiStateName(AIStateType state)
{
    switch (state)
    {
    case AI_IDLE: return "idle";
    case AI_MOVE_TO: return "move";
    case AI_ATTACK_MOVE_TO: return "attack_move";
    case AI_ATTACK_POSITION: return "attack_position";
    case AI_ATTACK_OBJECT: return "attack_object";
    case AI_FORCE_ATTACK_OBJECT: return "force_attack_object";
    case AI_ATTACK_AND_FOLLOW_OBJECT: return "attack_and_follow";
    case AI_GUARD: return "guard";
    case AI_GUARD_RETALIATE: return "guard_retaliate";
    case AI_GUARD_TUNNEL_NETWORK: return "guard_tunnel";
    case AI_HUNT: return "hunt";
    case AI_DOCK: return "dock";
    case AI_ENTER: return "enter";
    case AI_EXIT: return "exit";
    case AI_EXIT_INSTANTLY: return "exit";
    case AI_GET_REPAIRED: return "get_repaired";
    case AI_HACK_INTERNET: return "hack_internet";
    case AI_BUSY: return "busy";
    case AI_WAIT: return "wait";
    case AI_DEAD: return "dead";
    default: return "other";
    }
}

const char* currentMatchResult()
{
    Player* local = localPlayer();
    if (!matchReady() || !local)
        return "unknown";
    if (TheVictoryConditions && TheVictoryConditions->hasAchievedVictory(local))
        return "victory";
    if (local->isPlayerDead() || (TheVictoryConditions &&
        (TheVictoryConditions->hasBeenDefeated(local) || TheVictoryConditions->isLocalDefeat())))
        return "defeat";
    return local->isPlayerObserver() ? "observing" : "in_progress";
}

std::string kindMaskJson(const KindOfMaskType& mask)
{
    std::ostringstream out = jsonStream();
    out << '[';
    bool first = true;
    for (Int i = 0; i < KINDOF_COUNT; ++i)
        if (mask.test(i))
        {
            if (!first) out << ',';
            first = false;
            out << quote(KindOfMaskType::getBitNames()[i]);
        }
    out << ']';
    return out.str();
}

std::string weaponRulesJson(const WeaponTemplate* weapon)
{
    const WeaponBonus baseBonus;
    std::ostringstream out = jsonStream();
    const char* damageName = DamageTypeFlags::getBitNames()[weapon->getDamageType()];
    out << "{\"template\":" << quote(weapon->getName().str())
        << ",\"damage_type\":" << quote(damageName)
        << ",\"primary_damage\":" << number(weapon->getPrimaryDamage(baseBonus))
        << ",\"primary_radius\":" << number(weapon->getPrimaryDamageRadius(baseBonus))
        << ",\"secondary_damage\":" << number(weapon->getSecondaryDamage(baseBonus))
        << ",\"secondary_radius\":" << number(weapon->getSecondaryDamageRadius(baseBonus))
        << ",\"range\":" << number(weapon->getUnmodifiedAttackRange())
        << ",\"minimum_range\":" << number(weapon->getMinimumAttackRange())
        << ",\"minimum_target_pitch\":" << number(weapon->getMinTargetPitch())
        << ",\"maximum_target_pitch\":" << number(weapon->getMaxTargetPitch())
        << ",\"pitch_units\":\"radians\",\"clip_size\":" << weapon->getClipSize()
        << ",\"clip_reload_frames\":" << weapon->getClipReloadTime(baseBonus)
        << ",\"reloads_automatically\":" << static_cast<bool>(weapon->getAutoReloadsClip())
        << ",\"affects_mask\":" << weapon->getAffectsMask()
        << ",\"target_classes\":[";
    static const char* names[] = {"airborne_vehicle", "ground", "projectile", "small_missile",
        "mine", "airborne_infantry", "ballistic_missile", "parachute"};
    bool first = true;
    for (unsigned i = 0; i < 8; ++i)
        if (weapon->getAntiMask() & (1u << i))
        {
            if (!first) out << ',';
            first = false;
            out << quote(names[i]);
        }
    out << "]}";
    return out.str();
}

std::string armorJson(const ThingTemplate* type, Object* owned)
{
    ArmorSetFlags flags;
    if (owned)
        for (Int i = 0; i < ARMORSET_COUNT; ++i)
            if (owned->testArmorSetFlag(static_cast<ArmorSetType>(i)))
                flags.set(i);
    const ArmorTemplateSet* set = type->findArmorTemplateSet(flags);
    const ArmorTemplate* armor = set ? set->getArmorTemplate() : nullptr;
    if (!armor)
        return "null";
    std::ostringstream out = jsonStream();
    out << "{\"conditions\":[";
    bool first = true;
    for (Int i = 0; i < ARMORSET_COUNT; ++i)
        if (flags.test(i))
        {
            if (!first) out << ',';
            first = false;
            out << quote(ArmorSetFlags::getBitNames()[i]);
        }
    out << "],\"damage_multipliers\":{";
    for (Int i = 0; i < DAMAGE_NUM_TYPES; ++i)
    {
        if (i) out << ',';
        out << quote(DamageTypeFlags::getBitNames()[i]) << ':'
            << number(armor->adjustDamage(static_cast<DamageType>(i), 1.0f));
    }
    out << "}}";
    return out.str();
}

std::string orderJson(Object* object)
{
    AIUpdateInterface* ai = object->getAIUpdateInterface();
    if (!ai)
        return "null";
    Object* target = ai->getGoalObject();
    const bool targetVisible = !target || Detail::observableObject(target->getID());
    const Coord3D* goal = ai->getGoalPosition();
    const AIStateType state = ai->getAIStateType();
    // GeneralsX @bugfix A worker's construction state machine can be active
    // even when its general AI reports idle. Keep those workers out of idle rules.
    DozerAIInterface* builder = ai->getDozerAIInterface();
    const bool hasBuilderTask = builder &&
        (builder->isTaskPending(DOZER_TASK_BUILD) || builder->isTaskPending(DOZER_TASK_REPAIR) ||
         builder->isTaskPending(DOZER_TASK_FORTIFY));
    std::ostringstream out = jsonStream();
    out << "{\"state\":" << quote(aiStateName(state)) << ",\"state_id\":" << static_cast<int>(state)
        << ",\"target_id\":" << (target && targetVisible ? std::to_string(target->getID()) : "null")
        << ",\"goal_position\":" << (targetVisible && goal && state != AI_IDLE ? positionJson(*goal) : "null")
        << ",\"idle\":" << (ai->isIdle() && !hasBuilderTask)
        << ",\"has_builder_task\":" << hasBuilderTask
        << ",\"waiting_for_path\":" << static_cast<bool>(ai->isWaitingForPath())
        << ",\"has_path\":" << (ai->getPath() != nullptr) << '}';
    return out.str();
}

std::string abilitiesJson(Object* object)
{
    std::ostringstream out = jsonStream();
    out << '[';
    bool first = true;
    BehaviorModule** modules = object->getBehaviorModules();
    for (size_t i = 0; modules && modules[i] && i < 128; ++i)
    {
        SpecialPowerModuleInterface* power = modules[i]->getSpecialPower();
        SpecialPowerUpdateInterface* update = modules[i]->getSpecialPowerUpdateInterface();
        if (!power && !update)
            continue;
        if (!first) out << ',';
        first = false;
        out << "{\"module_tag\":" << static_cast<unsigned>(modules[i]->getModuleTagNameKey());
        if (power)
        {
            const bool shared = power->getSpecialPowerTemplate() && power->getSpecialPowerTemplate()->isSharedNSync();
            UnsignedInt existingFrame = 0;
            const bool initialized = !shared || object->getControllingPlayer()->tryGetSpecialPowerReadyFrame(
                power->getSpecialPowerTemplate(), existingFrame);
            out << ",\"power\":" << quote(power->getPowerName().str())
                << ",\"ready\":" << static_cast<bool>(power->isReadyReadOnly())
                << ",\"ready_percent\":" << number(power->getPercentReadyReadOnly())
                << ",\"ready_frame\":" << power->getReadyFrameReadOnly()
                << ",\"shared_timer_initialized\":" << (shared ? (initialized ? "true" : "false") : "null");
        }
        if (update)
            out << ",\"active\":" << static_cast<bool>(update->isActive())
                << ",\"in_use\":" << static_cast<bool>(update->isPowerCurrentlyInUse())
                << ",\"special_ability\":" << static_cast<bool>(update->isSpecialAbility());
        out << '}';
    }
    out << ']';
    return out.str();
}

std::string deploymentJson(Object* object)
{
    DeployStyleAIUpdate* deploy = static_cast<DeployStyleAIUpdate*>(
        object->findUpdateModule(NAMEKEY("DeployStyleAIUpdate")));
    if (!deploy)
        return "null";
    const DeployStateTypes phase = deploy->getDeployState();
    const char* phaseName = phase == READY_TO_MOVE ? "ready_to_move" : phase == DEPLOY ? "deploying" :
        phase == READY_TO_ATTACK ? "ready_to_attack" : phase == UNDEPLOY ? "packing" : "aligning_turrets";
    const bool timed = phase == DEPLOY || phase == UNDEPLOY;
    const UnsignedInt now = TheGameLogic->getFrame();
    const UnsignedInt end = deploy->getDeployCompletionFrame();
    const UnsignedInt remaining = timed && end > now ? end - now : 0;
    std::ostringstream out = jsonStream();
    out << "{\"deployed\":" << static_cast<bool>(object->testStatus(OBJECT_STATUS_DEPLOYED))
        << ",\"phase\":" << quote(phaseName) << ",\"phase_id\":" << static_cast<int>(phase)
        << ",\"packing\":" << (phase == UNDEPLOY) << ",\"unpacking\":" << (phase == DEPLOY)
        << ",\"completion_frame\":" << (timed ? std::to_string(end) : "null")
        << ",\"remaining_frames\":" << (timed ? std::to_string(remaining) : "null")
        << ",\"pack_frames\":" << deploy->getPackTime() << ",\"unpack_frames\":" << deploy->getUnpackTime()
        << ",\"turrets_require_deployment\":" << static_cast<bool>(deploy->doTurretsFunctionOnlyWhenDeployed())
        << ",\"turrets_require_centering\":" << static_cast<bool>(deploy->doTurretsHaveToCenterBeforePacking())
        << ",\"progress\":null}";
    return out.str();
}

std::string observedObjectJson(Object* object, bool detailed)
{
    const bool owned = object->getControllingPlayer() == localPlayer();
    const ThingTemplate* type = apparentTemplate(object);
    if (!type)
        type = object->getTemplate();
    Player* owner = apparentPlayer(object);
    const bool disguised = disguiseForLocal(object) != nullptr;
    BodyModuleInterface* body = object->getBodyModule();
    ContainModuleInterface* contain = disguised ? nullptr : object->getContain();
    std::ostringstream out = jsonStream();
    if (owned)
    {
        std::string base = objectJson(object);
        base.pop_back();
        out << base;
    }
    else
    {
        out << "{\"id\":" << static_cast<unsigned>(object->getID())
            << ",\"template\":" << quote(type->getName().str())
            << ",\"position\":" << positionJson(*object->getPosition())
            << ",\"health\":" << (body && !disguised ? number(body->getHealth()) : "null")
            << ",\"max_health\":" << (body && !disguised ? number(body->getMaxHealth()) : "null")
            << ",\"building\":" << static_cast<bool>(type->isKindOf(KINDOF_STRUCTURE));
    }
    out << ",\"owner_id\":" << (owner ? std::to_string(owner->getPlayerIndex()) : "null")
        << ",\"relation\":" << quote(relationshipTo(owner))
        << ",\"display_name\":" << unicodeJson(type->getDisplayName())
        << ",\"infantry\":" << static_cast<bool>(type->isKindOf(KINDOF_INFANTRY))
        << ",\"vehicle\":" << static_cast<bool>(type->isKindOf(KINDOF_VEHICLE))
        << ",\"aircraft\":" << static_cast<bool>(type->isKindOf(KINDOF_AIRCRAFT))
        << ",\"builder\":" << static_cast<bool>(type->isKindOf(KINDOF_DOZER))
        << ",\"projectile\":" << static_cast<bool>(type->isKindOf(KINDOF_PROJECTILE))
        << ",\"hazard\":" << static_cast<bool>(type->isKindOf(KINDOF_MINE) || type->isKindOf(KINDOF_BOOBY_TRAP))
        << ",\"drone\":" << static_cast<bool>(type->isKindOf(KINDOF_DRONE))
        << ",\"detector\":" << (templateModule(type, "StealthDetectorUpdate") != nullptr)
        << ",\"supply_source\":" << static_cast<bool>(type->isKindOf(KINDOF_SUPPLY_SOURCE) ||
            type->isKindOf(KINDOF_SUPPLY_SOURCE_ON_PREVIEW))
        << ",\"tech_building\":" << static_cast<bool>(type->isKindOf(KINDOF_TECH_BUILDING))
        << ",\"salvage_crate\":" << (!disguised && object->isSalvageCrate())
        << ",\"capturable\":" << static_cast<bool>(type->isKindOf(KINDOF_CAPTURABLE))
        << ",\"garrisonable\":" << (contain && contain->isGarrisonable());
    if (!disguised)
    {
        const GeometryInfo& geometry = object->getGeometryInfo();
        out << ",\"orientation\":" << number(object->getOrientation())
            << ",\"geometry\":{\"major_radius\":" << number(geometry.getMajorRadius())
            << ",\"minor_radius\":" << number(geometry.getMinorRadius())
            << ",\"height\":" << number(geometry.getMaxHeightAbovePosition()) << '}';
        if (type->isKindOf(KINDOF_SUPPLY_SOURCE) || type->isKindOf(KINDOF_SUPPLY_SOURCE_ON_PREVIEW))
        {
            SupplyWarehouseDockUpdate* supply = static_cast<SupplyWarehouseDockUpdate*>(
                object->findUpdateModule(NAMEKEY("SupplyWarehouseDockUpdate")));
            if (supply)
                out << ",\"supply_boxes\":" << supply->getBoxesStored()
                    << ",\"supply_cash_value\":" <<
                    static_cast<long long>(supply->getBoxesStored()) * TheGlobalData->m_baseValuePerSupplyBox;
        }
        if (contain)
            out << ",\"capacity\":" << contain->getContainMax();
    }
    if (owned)
    {
        Real progress = object->getConstructionPercent();
        out << ",\"construction_percent\":" << number(progress < 0 ? 100 : progress)
            << ",\"contained_by\":" << (object->getContainedBy() ?
                std::to_string(object->getContainedBy()->getID()) : "null")
            << ",\"producer_id\":" << static_cast<unsigned>(object->getProducerID())
            << ",\"current_order\":" << orderJson(object)
            << ",\"deployment\":" << deploymentJson(object);
        if (object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
        {
            Object* assigned = ownedObject(object->getBuilderID());
            AIUpdateInterface* ai = assigned ? assigned->getAIUpdateInterface() : nullptr;
            DozerAIInterface* builder = ai ? ai->getDozerAIInterface() : nullptr;
            const bool hasBuilder = builder && builder->isTaskPending(DOZER_TASK_BUILD) &&
                builder->getTaskTarget(DOZER_TASK_BUILD) == object->getID();
            out << ",\"builder_id\":" << (hasBuilder ? std::to_string(assigned->getID()) : "null");
        }
        if (detailed)
        {
            const ExitInterface* exit = object->getObjectExitInterface();
            const Coord3D* rally = exit ? exit->getRallyPoint() : nullptr;
            out << ",\"rally_point\":" << (rally ? positionJson(*rally) : "null");
            out << ",\"upgrades\":" << upgradeList(object, nullptr)
                << ",\"abilities\":" << abilitiesJson(object)
                << ",\"vision_range\":" << number(object->getVisionRange())
                << ",\"armor\":" << armorJson(type, object);
            WeaponSlotType currentSlot = PRIMARY_WEAPON;
            const Weapon* currentWeapon = object->getCurrentWeapon(&currentSlot);
            out << ",\"selected_weapon_slot\":" << (currentWeapon ? std::to_string(currentSlot) : "null")
                << ",\"last_damage_frame\":" << (body && body->getLastDamageTimestamp() ?
                    std::to_string(*body->getLastDamageTimestamp()) : "null")
                << ",\"subdual_damage\":" << (body ? number(body->getCurrentSubdualDamageAmount()) : "null")
                << ",\"weapons\":[";
            bool first = true;
            for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot)
            {
                const Weapon* weapon = object->getWeaponInWeaponSlot(static_cast<WeaponSlotType>(slot));
                if (!weapon)
                    continue;
                if (!first)
                    out << ',';
                first = false;
                out << "{\"slot\":" << slot << ",\"template\":" << quote(weapon->getName().str())
                    << ",\"range\":" << number(weapon->getAttackRange(object))
                    << ",\"minimum_range\":" << number(weapon->getTemplate()->getMinimumAttackRange())
                    << ",\"ammo\":" << weapon->getRemainingAmmo()
                    << ",\"clip_size\":" << weapon->getClipSize()
                    << ",\"ready_percent\":" << number(weapon->getPercentReadyToFire())
                    << ",\"status\":" << static_cast<int>(weapon->getStatus())
                    << ",\"base_rules\":" << weaponRulesJson(weapon->getTemplate()) << '}';
            }
            out << "],\"cargo\":[";
            size_t count = 0;
            const ContainedItemsList* cargo = contain ? contain->getContainedItemsList() : nullptr;
            if (cargo)
                for (ContainedItemsList::const_iterator it = cargo->begin(); it != cargo->end() && count < 64; ++it)
                {
                    Object* passenger = *it;
                    if (!passenger || passenger->getControllingPlayer() != localPlayer())
                        continue;
                    if (count++)
                        out << ',';
                    out << static_cast<unsigned>(passenger->getID());
                }
            out << ']';
            if (contain)
                out << ",\"cargo_count\":" << contain->getContainCount()
                    << ",\"passengers_fire\":" << static_cast<bool>(contain->isPassengerAllowedToFire());
        }
    }
    out << '}';
    return out.str();
}

struct AbilityRecord
{
    std::string power;
    UnsignedInt readyFrame = 0;
    bool ready = false;
    bool active = false;
};

struct ObservedRecord
{
    std::string name;
    Coord3D position;
    Real health;
    bool own;
    int owner = -1;
    bool underConstruction = false;
    bool deployed = false;
    int deploymentPhase = -1;
    int order = -1;
    ObjectID target = INVALID_ID;
    ObjectID containedBy = INVALID_ID;
    UpgradeMaskType upgrades;
    std::map<NameKeyType, AbilityRecord> abilities;
};

struct ProductionRecord
{
    ObjectID producer;
    int production;
    ObjectID object;
    UnsignedInt frame;
};

struct ObservedEvent
{
    unsigned long long sequence;
    std::string json;
};

struct ObservationState
{
    bool active = false;
    bool truncated = false;
    UnsignedInt frame = 0;
    unsigned match = 0;
    unsigned long long sequence = 0;
    std::string result = "unknown";
    std::map<ObjectID, ObservedRecord> objects;
    std::deque<ObservedEvent> events;
    std::vector<ProductionRecord> productions;
    UpgradeMaskType playerUpgrades;
};

// Construct and destroy explicitly while the engine allocator exists.
ObservationState* s_observation = nullptr;

void recordEvent(const char* type, ObjectID objectID = INVALID_ID,
    const ObservedRecord* object = nullptr, Real amount = 0, const std::string& fields = std::string())
{
    ObservationState& state = *s_observation;
    std::ostringstream out = jsonStream();
    out << "{\"sequence\":" << ++state.sequence << ",\"match\":" << state.match << ",\"match_id\":" << state.match
        << ",\"frame\":" << state.frame << ",\"type\":" << quote(type);
    if (!std::strncmp(type, "match_", 6))
        out << ",\"result\":" << quote(state.result);
    if (objectID != INVALID_ID)
        out << ",\"object_id\":" << static_cast<unsigned>(objectID);
    if (object)
        out << ",\"template\":" << quote(object->name) << ",\"last_observed_position\":"
            << positionJson(object->position) << ",\"owner_id\":" << object->owner;
    if (amount > 0)
        out << ",\"amount\":" << number(amount);
    out << fields << '}';
    state.events.push_back({state.sequence, out.str()});
    if (state.events.size() > MAX_EVENTS)
        state.events.pop_front();
}

void recordUpgrades(const UpgradeMaskType& before, const UpgradeMaskType& after,
    ObjectID objectID = INVALID_ID, const ObservedRecord* object = nullptr)
{
    if (before == after)
        return;
    for (const UpgradeTemplate* upgrade = TheUpgradeCenter ? TheUpgradeCenter->firstUpgradeTemplate() : nullptr;
         upgrade; upgrade = upgrade->friend_getNext())
        if (!before.testForAll(upgrade->getUpgradeMask()) && after.testForAll(upgrade->getUpgradeMask()))
            recordEvent("upgrade_completed", objectID, object, 0,
                ",\"scope\":" + quote(object ? "object" : "player") +
                ",\"upgrade\":" + quote(upgrade->getUpgradeName().str()));
}

CellShroudStatus cellShroud(const Coord3D& position)
{
    return ThePartitionManager ? ThePartitionManager->getShroudStatusForPlayer(
        localPlayer()->getPlayerIndex(), &position) : CELLSHROUD_SHROUDED;
}

const char* shroudName(CellShroudStatus status)
{
    switch (status)
    {
    case CELLSHROUD_CLEAR: return "clear";
    case CELLSHROUD_FOGGED: return "fogged";
    default: return "unexplored";
    }
}

std::string getWorld(const std::string& id, const std::vector<std::string>& args)
{
    unsigned long long offset, limit;
    if (args.size() != 3 || !parseUnsigned(args[0], 100000, offset) ||
        !parseUnsigned(args[1], MAX_PAGE, limit) || limit == 0 ||
        (args[2] != "all" && args[2] != "own" && args[2] != "ally" &&
         args[2] != "enemy" && args[2] != "neutral"))
        return failure(id, "invalid_arguments", "Expected offset 0..100000, limit 1..256, and relation all/own/ally/enemy/neutral.");
    if (!matchReady())
        return failure(id, "not_in_match", "World observations require a loaded match.");
    std::vector<Object*> objects;
    for (Object* object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
        if (Detail::observableObject(object->getID()) && usefulObject(object) &&
            (args[2] == "all" || args[2] == relationshipTo(apparentPlayer(object))))
            objects.push_back(object);
    std::sort(objects.begin(), objects.end(), [](const Object* a, const Object* b) { return a->getID() < b->getID(); });
    std::ostringstream out = jsonStream();
    out << "{\"frame\":" << TheGameLogic->getFrame() << ",\"total\":" << objects.size()
        << ",\"offset\":" << offset << ",\"objects\":[";
    size_t emitted = 0;
    size_t bytes = 0;
    for (size_t i = static_cast<size_t>(offset); i < objects.size() && emitted < limit; ++i)
    {
        const std::string object = observedObjectJson(objects[i], false);
        if (bytes + object.size() > MAX_RESULT_BYTES)
            break;
        if (emitted++)
            out << ',';
        out << object;
        bytes += object.size();
    }
    const bool more = offset + emitted < objects.size();
    out << "],\"returned\":" << emitted << ",\"next_offset\":" <<
        (more ? std::to_string(offset + emitted) : "null")
        << ",\"visibility\":\"local_player_current\"}";
    return success(id, out.str());
}

std::string getObject(const std::string& id, const std::vector<std::string>& args)
{
    ObjectID objectID;
    if (args.size() != 1 || !parseId(args[0], objectID))
        return failure(id, "invalid_arguments", "Expected one positive object ID.");
    if (!matchReady())
        return failure(id, "not_in_match", "Object observations require a loaded match.");
    Object* object = Detail::observableObject(objectID);
    if (!object)
        return failure(id, "object_not_observable", "The object is absent or not currently observable by the local player.");
    return success(id, "{\"frame\":" + std::to_string(TheGameLogic->getFrame()) +
        ",\"object\":" + observedObjectJson(object, true) + "}");
}

std::string getUnitProfile(const std::string& id, const std::vector<std::string>& args)
{
    ObjectID objectID;
    if (args.size() != 1 || !parseId(args[0], objectID))
        return failure(id, "invalid_arguments", "Expected an observable object ID.");
    Object* object = Detail::observableObject(objectID);
    if (!object)
        return failure(id, "not_observable", "The object is not currently observable.");
    const ThingTemplate* type = apparentTemplate(object);
    if (!type)
        return failure(id, "unavailable", "The apparent object template is unavailable.");
    const bool owned = object->getControllingPlayer() == localPlayer();
    // GeneralsX @feature Enemy profiles use only the apparent base template.
    // Runtime modules, upgrades, ammo and weapon/armor conditions remain own-only.
    std::ostringstream out = jsonStream();
    out << "{\"frame\":" << TheGameLogic->getFrame() << ",\"object\":" << observedObjectJson(object, owned)
        << ",\"rules_scope\":\"apparent_base_template\",\"base_rules\":{\"vision_range\":"
        << number(type->friend_calcVisionRange()) << ",\"armor\":" << armorJson(type, nullptr)
        << ",\"weapons\":[";
    const WeaponTemplateSet* weapons = type->findWeaponTemplateSet(WeaponSetFlags());
    bool first = true;
    if (weapons)
        for (Int slot = 0; slot < WEAPONSLOT_COUNT; ++slot)
        {
            const WeaponTemplate* weapon = weapons->getNth(static_cast<WeaponSlotType>(slot));
            if (!weapon) continue;
            if (!first) out << ',';
            first = false;
            out << "{\"slot\":" << slot << ",\"command_source_mask\":"
                << weapons->getNthCommandSourceMask(static_cast<WeaponSlotType>(slot))
                << ",\"rules\":" << weaponRulesJson(weapon) << '}';
        }
    out << "],\"modules\":[";
    first = true;
    const ModuleInfo& modules = type->getBehaviorModuleInfo();
    for (Int i = 0; i < modules.getCount() && i < 128; ++i)
    {
        const ModuleData* data = modules.getNthData(i);
        const AsciiString name = modules.getNthName(i);
        const bool detector = name == "StealthDetectorUpdate";
        const bool heal = name == "AutoHealBehavior";
        const bool deploy = name == "DeployStyleAIUpdate";
        if (!data || (!detector && !heal && !data->isAiModuleData()))
            continue;
        if (!first) out << ',';
        first = false;
        out << "{\"module\":" << quote(name.str()) << ",\"tag\":" << quote(modules.getNthTag(i).str());
        BehaviorModule* runtime = nullptr;
        if (owned)
            for (BehaviorModule** b = object->getBehaviorModules(); b && *b; ++b)
                if ((*b)->getModuleTagNameKey() == data->getModuleTagNameKey())
                {
                    runtime = *b;
                    break;
                }
        if (detector)
        {
            const StealthDetectorUpdateModuleData* d = static_cast<const StealthDetectorUpdateModuleData*>(data);
            out << ",\"range\":" << number(d->m_detectionRange)
                << ",\"update_frames\":" << d->m_updateRate
                << ",\"initially_disabled\":" << static_cast<bool>(d->m_initiallyDisabled)
                << ",\"while_garrisoned\":" << static_cast<bool>(d->m_canDetectWhileGarrisoned)
                << ",\"while_transported\":" << static_cast<bool>(d->m_canDetectWhileTransported)
                << ",\"extra_detect_kindof\":" << kindMaskJson(d->m_extraDetectKindof)
                << ",\"extra_detect_not_kindof\":" << kindMaskJson(d->m_extraDetectKindofNot);
            if (runtime)
                out << ",\"currently_enabled\":" << static_cast<bool>(static_cast<StealthDetectorUpdate*>(runtime)->isSDEnabled());
        }
        if (heal)
        {
            const AutoHealBehaviorModuleData* d = static_cast<const AutoHealBehaviorModuleData*>(data);
            out << ",\"radius\":" << number(d->m_radius) << ",\"healing_amount\":" << d->m_healingAmount
                << ",\"healing_delay_frames\":" << d->m_healingDelay
                << ",\"start_delay_after_damage_frames\":" << d->m_startHealingDelay
                << ",\"whole_player\":" << static_cast<bool>(d->m_affectsWholePlayer)
                << ",\"skip_self\":" << static_cast<bool>(d->m_skipSelfForHealing)
                << ",\"single_burst\":" << static_cast<bool>(d->m_singleBurst)
                << ",\"allowed_kindof\":" << kindMaskJson(d->m_kindOf)
                << ",\"forbidden_kindof\":" << kindMaskJson(d->m_forbiddenKindOf);
            if (runtime && runtime->getUpgrade())
                out << ",\"upgrade_active\":" << static_cast<bool>(runtime->getUpgrade()->isAlreadyUpgraded());
        }
        if (data->isAiModuleData())
        {
            const AIUpdateModuleData* d = static_cast<const AIUpdateModuleData*>(data);
            out << ",\"auto_acquire_mask\":" << d->m_autoAcquireEnemiesWhenIdle
                << ",\"auto_acquire_idle\":" << static_cast<bool>(d->m_autoAcquireEnemiesWhenIdle & AAS_Idle)
                << ",\"auto_acquire_not_while_attacking\":" << static_cast<bool>(d->m_autoAcquireEnemiesWhenIdle & AAS_Idle_Not_While_Attacking);
        }
        if (deploy)
        {
            const DeployStyleAIUpdateModuleData* d = static_cast<const DeployStyleAIUpdateModuleData*>(data);
            out << ",\"pack_frames\":" << d->m_packTime << ",\"unpack_frames\":" << d->m_unpackTime
                << ",\"turrets_require_deployment\":" << static_cast<bool>(d->m_turretsFunctionOnlyWhenDeployed);
        }
        out << '}';
    }
    out << "]}}";
    return success(id, out.str());
}

std::string getMap(const std::string& id, const std::vector<std::string>& args)
{
    Real x, y, width, height, step;
    if (args.size() != 5 || !parseCoordinate(args[0], x) || !parseCoordinate(args[1], y) ||
        !parseCoordinate(args[2], width) || !parseCoordinate(args[3], height) ||
        !parseCoordinate(args[4], step) || width <= 0 || height <= 0 || step < 5)
        return failure(id, "invalid_arguments", "Expected finite x,y,width,height,step; positive dimensions and step >= 5.");
    if (!matchReady() || !TheTerrainLogic)
        return failure(id, "not_in_match", "Map observations require a loaded match.");
    Region3D bounds;
    TheTerrainLogic->getExtent(&bounds);
    if (x < bounds.lo.x || y < bounds.lo.y || static_cast<double>(x) + width > bounds.hi.x ||
        static_cast<double>(y) + height > bounds.hi.y)
        return failure(id, "outside_map", "The requested rectangle must be inside the playable map bounds.");
    const double columnsValue = std::floor(static_cast<double>(width) / step) + 1;
    const double rowsValue = std::floor(static_cast<double>(height) / step) + 1;
    if (columnsValue * rowsValue > MAX_MAP_SAMPLES)
        return failure(id, "too_many_samples", "Use a larger step: at most 4096 terrain samples are allowed.");
    const unsigned columns = static_cast<unsigned>(columnsValue);
    const unsigned rows = static_cast<unsigned>(rowsValue);
    std::ostringstream out = jsonStream();
    out << "{\"frame\":" << TheGameLogic->getFrame() << ",\"bounds\":{\"min\":"
        << positionJson(bounds.lo) << ",\"max\":" << positionJson(bounds.hi)
        << "},\"columns\":" << columns << ",\"rows\":" << rows << ",\"step\":" << number(step)
        << ",\"passability_note\":\"Ground-layer samples; not a unit-specific route or building-placement guarantee. Hidden obstacles are omitted.\""
        << ",\"sample_fields\":[\"x\",\"y\",\"shroud\",\"height\",\"water\",\"cliff\",\"passable_ground\",\"obstacle_id\"],\"samples\":[";
    for (unsigned row = 0; row < rows; ++row)
        for (unsigned column = 0; column < columns; ++column)
        {
            if (row || column)
                out << ',';
            Coord3D position = {x + column * step, y + row * step, 0};
            const CellShroudStatus shroud = cellShroud(position);
            out << '[' << number(position.x) << ',' << number(position.y)
                << ',' << quote(shroudName(shroud));
            if (shroud != CELLSHROUD_SHROUDED)
            {
                position.z = TheTerrainLogic->getGroundHeight(position.x, position.y);
                const bool water = TheTerrainLogic->isUnderwater(position.x, position.y);
                const bool cliff = TheTerrainLogic->isCliffCell(position.x, position.y);
                bool passable = !water && !cliff;
                Object* obstacle = nullptr;
                if (shroud == CELLSHROUD_CLEAR && TheAI && TheAI->pathfinder())
                {
                    PathfindCell* cell = TheAI->pathfinder()->getCell(LAYER_GROUND, &position);
                    if (cell)
                    {
                        obstacle = Detail::observableObject(cell->getObstacleID());
                        if (obstacle || cell->getObstacleID() == INVALID_ID)
                            passable = cell->getType() == PathfindCell::CELL_CLEAR || cell->getType() == PathfindCell::CELL_RUBBLE;
                    }
                }
                out << ',' << number(position.z) << ',' << water
                    << ',' << cliff << ',' << passable
                    << ',' << (obstacle ? std::to_string(obstacle->getID()) : "null");
            }
            else
                out << ",null,null,null,null,null";
            out << ']';
        }
    out << "],\"waypoints\":[";
    size_t waypoints = 0;
    bool waypointsTruncated = false;
    for (Waypoint* waypoint = TheTerrainLogic->getFirstWaypoint(); waypoint; waypoint = waypoint->getNext())
    {
        const Coord3D& position = *waypoint->getLocation();
        if (position.x < x || position.y < y || position.x > x + width || position.y > y + height ||
            cellShroud(position) == CELLSHROUD_SHROUDED)
            continue;
        if (waypoints == 128)
        {
            waypointsTruncated = true;
            break;
        }
        if (waypoints++)
            out << ',';
        out << "{\"name\":" << quote(waypoint->getName().str()) << ",\"position\":" << positionJson(position) << '}';
    }
    out << "],\"waypoints_truncated\":" << waypointsTruncated << '}';
    return success(id, out.str());
}

std::string playersJson()
{
    Player* local = localPlayer();
    std::ostringstream out = jsonStream();
    out << '[';
    bool first = true;
    for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
    {
        Player* player = ThePlayerList->getNthPlayer(i);
        if (!player)
            continue;
        const Int slotIndex = ThePlayerList->getSlotIndex(player->getPlayerIndex());
        if (slotIndex < 0 && player != local && player != ThePlayerList->getNeutralPlayer())
            continue;
        if (!first)
            out << ',';
        first = false;
        const GameSlot* slot = TheGameInfo && slotIndex >= 0 && slotIndex < MAX_SLOTS ?
            TheGameInfo->getConstSlot(slotIndex) : nullptr;
        out << "{\"id\":" << player->getPlayerIndex() << ",\"name\":" << unicodeJson(player->getPlayerDisplayName())
            << ",\"relation\":" << quote(relationshipTo(player))
            << ",\"team\":" << (slot ? std::to_string(slot->getTeamNumber()) : "null")
            << ",\"color\":" << static_cast<unsigned>(player->getPlayerColor())
            << ",\"active\":" << static_cast<bool>(player->isPlayerActive());
        if (player == local)
        {
            out << ",\"side\":" << quote(player->getSide().str())
                << ",\"money\":" << player->getMoney()->countMoney()
                << ",\"power\":{\"produced\":" << player->getEnergy()->getProduction()
                << ",\"consumed\":" << player->getEnergy()->getConsumption() << '}'
                << ",\"rank\":" << player->getRankLevel()
                << ",\"skill_points\":" << player->getSkillPoints()
                << ",\"science_points\":" << player->getSciencePurchasePoints()
                << ",\"upgrades\":" << upgradeList(nullptr, player)
                << ",\"available_sciences\":[";
            ScienceVec available, potential;
            if (TheScienceStore)
                TheScienceStore->getPurchasableSciences(player, available, potential);
            for (size_t j = 0; j < available.size() && j < 128; ++j)
            {
                if (j)
                    out << ',';
                out << "{\"id\":" << static_cast<int>(available[j])
                    << ",\"name\":" << quote(TheScienceStore->getInternalNameForScience(available[j]).str())
                    << ",\"cost\":" << TheScienceStore->getSciencePurchaseCost(available[j]) << '}';
            }
            out << ']';
        }
        out << '}';
    }
    out << ']';
    return out.str();
}

std::string getPlayers(const std::string& id, const std::vector<std::string>& args)
{
    if (!args.empty())
        return failure(id, "invalid_arguments", "get_players takes no arguments.");
    if (!matchReady())
        return failure(id, "not_in_match", "Player observations require a loaded match.");
    std::ostringstream out = jsonStream();
    out << "{\"frame\":" << TheGameLogic->getFrame() << ",\"local_player_id\":" << localPlayer()->getPlayerIndex()
        << ",\"match_result\":" << quote(currentMatchResult()) << ",\"players\":" << playersJson() << '}';
    return success(id, out.str());
}

std::string getSnapshot(const std::string& id, const std::vector<std::string>& args)
{
    unsigned long long offset, limit;
    if (args.size() != 2 || !parseUnsigned(args[0], 100000, offset) ||
        !parseUnsigned(args[1], MAX_PAGE, limit) || limit == 0)
        return failure(id, "invalid_arguments", "Expected world offset 0..100000 and limit 1..256.");
    // This handler runs on the game thread: every field and the event cursor
    // describe this single simulation frame, including while the game is paused.
    UpdateObservation();
    const bool ready = matchReady();
    std::ostringstream out = jsonStream();
    out << "{\"frame\":" << (ready ? TheGameLogic->getFrame() : (s_observation ? s_observation->frame : 0))
        << ",\"match_id\":" << (s_observation ? s_observation->match : 0)
        << ",\"status\":" << quote(ready ? "in_match" : "not_in_match")
        << ",\"paused\":" << static_cast<bool>(TheGameLogic && TheGameLogic->isGamePaused())
        << ",\"match_result\":" << quote(s_observation ? s_observation->result : "unknown")
        << ",\"event_cursor\":" << (s_observation ? s_observation->sequence : 0)
        << ",\"tracking_truncated\":" << (s_observation && s_observation->truncated)
        << ",\"local_player_id\":" << (ready ? std::to_string(localPlayer()->getPlayerIndex()) : "null")
        << ",\"own\":[";
    std::vector<Object*> own, world;
    if (ready)
        for (Object* object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
        {
            if (!visibleToLocal(object) || !usefulObject(object) || object->isDestroyed() || object->isEffectivelyDead())
                continue;
            world.push_back(object);
            if (object->getControllingPlayer() == localPlayer())
                own.push_back(object);
        }
    const auto byId = [](Object* a, Object* b) { return a->getID() < b->getID(); };
    std::sort(own.begin(), own.end(), byId);
    std::sort(world.begin(), world.end(), byId);
    size_t ownCount = 0;
    for (Object* object : own)
    {
        const std::string value = observedObjectJson(object, false);
        if (ownCount == 512 || static_cast<size_t>(out.tellp()) + value.size() > 380 * 1024)
            break;
        if (ownCount++) out << ',';
        out << value;
    }
    out << "],\"own_total\":" << own.size() << ",\"own_truncated\":" << (ownCount < own.size())
        << ",\"world\":{\"total\":" << world.size() << ",\"offset\":" << offset << ",\"objects\":[";
    size_t returned = 0;
    for (size_t i = static_cast<size_t>(offset); i < world.size() && returned < limit; ++i)
    {
        const std::string value = observedObjectJson(world[i], false);
        if (static_cast<size_t>(out.tellp()) + value.size() > 850 * 1024)
            break;
        if (returned++) out << ',';
        out << value;
    }
    out << "],\"returned\":" << returned << ",\"next_offset\":"
        << (offset + returned < world.size() ? std::to_string(offset + returned) : "null")
        << ",\"visibility\":\"local_player_current\"},\"players\":" << (ready ? playersJson() : "[]") << '}';
    if (static_cast<size_t>(out.tellp()) > 960 * 1024)
        return failure(id, "result_too_large", "The snapshot exceeded its bounded response budget.");
    return success(id, out.str());
}

std::string getEvents(const std::string& id, const std::vector<std::string>& args)
{
    unsigned long long since, limit;
    if (args.size() != 2 || !parseUnsigned(args[0], 9007199254740991ULL, since) ||
        !parseUnsigned(args[1], MAX_PAGE, limit) || limit == 0)
        return failure(id, "invalid_arguments", "Expected since 0..9007199254740991 and limit 1..256.");
    UpdateObservation();
    std::ostringstream out = jsonStream();
    unsigned long long next = since;
    const unsigned long long newest = s_observation ? s_observation->sequence : 0;
    const unsigned long long oldest = s_observation && !s_observation->events.empty() ?
        s_observation->events.front().sequence : 0;
    out << "{\"oldest_sequence\":" << oldest << ",\"newest_sequence\":" << newest
        << ",\"frame\":" << (s_observation ? s_observation->frame : 0)
        << ",\"match_id\":" << (s_observation ? s_observation->match : 0)
        << ",\"match_result\":" << quote(s_observation ? s_observation->result : "unknown")
        << ",\"history_lost\":" << (oldest && since + 1 < oldest)
        << ",\"tracking_truncated\":" << (s_observation && s_observation->truncated)
        << ",\"events\":[";
    size_t count = 0;
    if (s_observation)
        for (const ObservedEvent& event : s_observation->events)
        {
            if (event.sequence <= since)
                continue;
            if (count == limit)
                break;
            if (count++)
                out << ',';
            out << event.json;
            next = event.sequence;
        }
    out << "],\"next_since\":" << next << ",\"more\":" << (next < newest) << '}';
    return success(id, out.str());
}
}

namespace Detail
{
Object* observableObject(ObjectID id)
{
    if (!matchReady())
        return nullptr;
    Object* object = TheGameLogic->findObjectByID(id);
    return object && !object->isDestroyed() && !object->isEffectivelyDead() && visibleToLocal(object) ? object : nullptr;
}
}

void ResetObservationMatch()
{
    if (s_observation && s_observation->active)
    {
        if (s_observation->result == "in_progress" || s_observation->result == "observing")
            s_observation->result = "ended";
        recordEvent("match_ended");
        s_observation->objects.clear();
        s_observation->productions.clear();
        s_observation->active = false;
    }
}

void UpdateObservation()
{
    if (!matchReady())
    {
        ResetObservationMatch();
        return;
    }
    if (!s_observation)
        s_observation = new ObservationState;
    ObservationState& state = *s_observation;
    const UnsignedInt frame = TheGameLogic->getFrame();
    if (!state.active || frame < state.frame)
    {
        if (state.active)
            ResetObservationMatch();
        state.objects.clear();
        state.active = true;
        ++state.match;
        state.frame = frame;
        state.result = currentMatchResult();
        state.playerUpgrades = localPlayer()->getCompletedUpgradeMask();
        recordEvent("match_started");
    }
    const std::string result = currentMatchResult();
    if (result != state.result)
    {
        state.result = result;
        state.frame = frame;
        recordEvent("match_result");
    }
    else if (state.active && frame == state.frame && !state.objects.empty() && state.productions.empty())
        return;
    state.frame = frame;
    state.truncated = false;
    recordUpgrades(state.playerUpgrades, localPlayer()->getCompletedUpgradeMask());
    state.playerUpgrades = localPlayer()->getCompletedUpgradeMask();
    std::map<ObjectID, ObservedRecord> current;
    for (Object* object = TheGameLogic->getFirstObject(); object; object = object->getNextObject())
    {
        if (!usefulObject(object) || !visibleToLocal(object))
            continue;
        // Projectiles remain in snapshots, but their routine creation/removal
        // would evict production and ownership events from the bounded history.
        const ThingTemplate* apparent = apparentTemplate(object);
        if (apparent && apparent->isKindOf(KINDOF_PROJECTILE) && !apparent->isKindOf(KINDOF_MINE))
            continue;
        const ObjectID objectID = object->getID();
        std::map<ObjectID, ObservedRecord>::iterator old = state.objects.find(objectID);
        if (object->isDestroyed() || object->isEffectivelyDead())
        {
            if (old != state.objects.end())
            {
                recordEvent("destroyed", objectID, &old->second);
                state.objects.erase(old);
            }
            continue;
        }
        if (current.size() == MAX_TRACKED_OBJECTS)
        {
            state.truncated = true;
            break;
        }
        const ThingTemplate* type = apparentTemplate(object);
        BodyModuleInterface* body = object->getBodyModule();
        ObservedRecord observed;
        observed.name = type ? type->getName().str() : "";
        observed.position = *object->getPosition();
        observed.health = body && !disguiseForLocal(object) ? body->getHealth() : -1;
        observed.own = object->getControllingPlayer() == localPlayer();
        Player* owner = apparentPlayer(object);
        observed.owner = owner ? owner->getPlayerIndex() : -1;
        if (observed.own)
        {
            observed.underConstruction = object->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION);
            observed.deployed = object->testStatus(OBJECT_STATUS_DEPLOYED);
            DeployStyleAIUpdate* deploy = static_cast<DeployStyleAIUpdate*>(
                object->findUpdateModule(NAMEKEY("DeployStyleAIUpdate")));
            if (deploy)
                observed.deploymentPhase = static_cast<int>(deploy->getDeployState());
            observed.containedBy = object->getContainedBy() ? object->getContainedBy()->getID() : INVALID_ID;
            observed.upgrades = object->getObjectCompletedUpgradeMask();
            AIUpdateInterface* ai = object->getAIUpdateInterface();
            if (ai)
            {
                observed.order = static_cast<int>(ai->getAIStateType());
                Object* target = ai->getGoalObject();
                if (target && Detail::observableObject(target->getID()))
                    observed.target = target->getID();
            }
            BehaviorModule** modules = object->getBehaviorModules();
            for (size_t i = 0; modules && modules[i] && i < 128; ++i)
            {
                SpecialPowerModuleInterface* power = modules[i]->getSpecialPower();
                SpecialPowerUpdateInterface* update = modules[i]->getSpecialPowerUpdateInterface();
                if (!power && !update) continue;
                AbilityRecord ability;
                if (power)
                {
                    ability.power = power->getPowerName().str();
                    ability.ready = power->isReadyReadOnly();
                    ability.readyFrame = power->getReadyFrameReadOnly();
                }
                ability.active = update && update->isActive();
                observed.abilities.emplace(modules[i]->getModuleTagNameKey(), ability);
            }
        }
        if (old == state.objects.end())
            recordEvent(observed.own ? "created_or_acquired" : "created_or_revealed", objectID, &observed);
        else
        {
            const ObservedRecord& previous = old->second;
            if (observed.health >= 0 && previous.health > observed.health)
                recordEvent("damaged", objectID, &observed, previous.health - observed.health);
            if (observed.owner != previous.owner)
                recordEvent("ownership_changed", objectID, &observed, 0,
                    ",\"previous_owner_id\":" + std::to_string(previous.owner));
            if (observed.own && previous.own)
            {
                if (previous.underConstruction && !observed.underConstruction)
                    recordEvent("construction_completed", objectID, &observed);
                if (previous.deployed != observed.deployed)
                    recordEvent(observed.deployed ? "deployed" : "undeployed", objectID, &observed);
                if (previous.deploymentPhase != observed.deploymentPhase)
                    recordEvent("deployment_phase_changed", objectID, &observed, 0,
                        ",\"deployment\":" + deploymentJson(object));
                if (previous.containedBy != observed.containedBy)
                    recordEvent(observed.containedBy == INVALID_ID ? "exited_container" : "entered_container",
                        objectID, &observed, 0, ",\"contained_by\":" +
                        (observed.containedBy == INVALID_ID ? std::string("null") : std::to_string(observed.containedBy)));
                if (previous.order != observed.order || previous.target != observed.target)
                    recordEvent("order_state_changed", objectID, &observed, 0,
                        ",\"observation_only\":true,\"current_order\":" + orderJson(object));
                recordUpgrades(previous.upgrades, observed.upgrades, objectID, &observed);
                for (const auto& ability : observed.abilities)
                {
                    const auto before = previous.abilities.find(ability.first);
                    if (before == previous.abilities.end()) continue;
                    const std::string fields = ",\"module_tag\":" + std::to_string(ability.first) +
                        ",\"power\":" + quote(ability.second.power) + ",\"ready_frame\":" +
                        std::to_string(ability.second.readyFrame) + ",\"observation_only\":true";
                    if (ability.second.readyFrame > before->second.readyFrame && before->second.ready && !ability.second.ready)
                        recordEvent("ability_cooldown_started", objectID, &observed, 0, fields);
                    if (ability.second.ready && !before->second.ready)
                        recordEvent("ability_ready", objectID, &observed, 0, fields);
                    if (ability.second.active != before->second.active)
                        recordEvent(ability.second.active ? "ability_started" : "ability_ended",
                            objectID, &observed, 0, fields);
                }
            }
        }
        current.emplace(objectID, observed);
    }
    if (!state.truncated)
        for (const auto& old : state.objects)
            if (current.find(old.first) == current.end())
                recordEvent(old.second.own ? "removed" : "lost_sight", old.first, &old.second);
    state.objects.swap(current);
    for (const ProductionRecord& production : state.productions)
    {
        const auto object = state.objects.find(production.object);
        recordEvent("unit_produced", production.object, object == state.objects.end() ? nullptr : &object->second,
            0, ",\"producer_id\":" + std::to_string(production.producer) +
            ",\"production_id\":" + std::to_string(production.production) +
            ",\"creation_frame\":" + std::to_string(production.frame));
    }
    state.productions.clear();
}

void RecordUnitProduced(ObjectID producerID, int productionID, ObjectID objectID)
{
    if (!Detail::matchReady())
        return;
    Object* object = TheGameLogic->findObjectByID(objectID);
    if (!object || object->getControllingPlayer() != Detail::localPlayer())
        return;
    if (!s_observation)
        s_observation = new ObservationState;
    // The normal production callback supplies the exact queue ID. Do not infer
    // completion from a disappearing queue entry (which may be cancellation).
    if (s_observation->productions.size() < MAX_EVENTS)
        s_observation->productions.push_back({producerID, productionID, objectID, TheGameLogic->getFrame()});
}

void ShutdownObservation()
{
    delete s_observation;
    s_observation = nullptr;
}

std::string ExecuteObservation(const std::string& id, const std::string& method,
    const std::vector<std::string>& args)
{
    if (method == "get_world") return getWorld(id, args);
    if (method == "get_object") return getObject(id, args);
    if (method == "get_map") return getMap(id, args);
    if (method == "get_players") return getPlayers(id, args);
    if (method == "get_events") return getEvents(id, args);
    if (method == "get_snapshot") return getSnapshot(id, args);
    if (method == "get_unit_profile") return getUnitProfile(id, args);
    return std::string();
}
}
