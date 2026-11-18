// TheSuperHackers @feature Optional local MCP transport, serviced by the game thread.
#pragma once

namespace GeneralsAgent
{
void UpdateBridge();
void ShutdownBridge();
bool IsBridgeIdle();
bool IsBridgeEnabled();
}
