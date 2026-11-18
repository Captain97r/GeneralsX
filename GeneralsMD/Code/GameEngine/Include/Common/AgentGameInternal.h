#pragma once

// Internal, main-thread-only support shared by the agent adapter modules.
// Include PreRTS.h before this header in engine translation units.
#include "Common/GameType.h"
#include <sstream>
#include <string>
#include <vector>

class Object;
class Player;
class CommandButton;
class UnicodeString;

namespace GeneralsAgent
{
namespace Detail
{
std::string quote(const std::string& value);
std::ostringstream jsonStream();
std::string number(Real value);
std::string success(const std::string& id, const std::string& result);
std::string failure(const std::string& id, const char* code, const std::string& message);
bool parseId(const std::string& text, ObjectID& id);
bool parseCoordinate(const std::string& text, Real& value);
Player* localPlayer();
bool matchReady();
const char* writeRestriction();
Object* ownedObject(ObjectID id);
Object* observableObject(ObjectID id);
bool canAttackMove(Object* object);
const char* trainingRestriction(Object* object, const CommandButton* button);
std::string positionJson(const Coord3D& position);
std::string objectJson(Object* object);
std::vector<ObjectID> selectedObjects();
void selectObjects(const std::vector<ObjectID>& ids);
}

std::string ExecuteCommands(const std::string& id, const std::string& method,
    const std::vector<std::string>& args);
std::string ExecuteObservation(const std::string& id, const std::string& method,
    const std::vector<std::string>& args);
std::string ExecuteSession(const std::string& id, const std::string& method,
    const std::vector<std::string>& args);
std::string ExecuteLobby(const std::string& id, const std::string& method,
    const std::vector<std::string>& args);
bool ApplyLobbyOverrides(UnsignedInt& localIP, UnicodeString& playerName);
bool HasLobbyNameOverride();
void ClearLobbyOverrides();
void UpdateObservation();
void ResetObservationMatch();
void ShutdownObservation();
void UpdateSession();
void ShutdownSession();
bool IsRealtimeControl();
void RecordUnitProduced(ObjectID producerID, int productionID, ObjectID objectID);
}
