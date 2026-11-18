#pragma once

class AsciiString;

namespace rts
{
// Test-only startup, enabled by GENERALS_MCP_TEST_MAP together with an explicit
// pipe name and an existing private user-data directory.
bool agentSkirmishTestUserData(AsciiString& directory);

// Called on the game thread before message propagation. Starts one fresh
// skirmish and requests normal process shutdown after the test deadline.
void updateAgentSkirmishTest();
}
