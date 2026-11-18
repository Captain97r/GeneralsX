// Exercises the real nonblocking native transport without starting a game.
#include <windows.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include "Common/AgentBridge.h"
#include "Common/AgentGameAdapter.h"

namespace GeneralsAgent
{
static unsigned executions = 0;

std::string Execute(const std::string& id, const std::string& method,
	const std::vector<std::string>& args)
{
	++executions;
	std::string result = "{\"executions\":" + std::to_string(executions) + "}";
	if (method == "large")
		result = "{\"payload\":\"" + std::string(200000, 'x') + "\"}";
	return "{\"protocol\":1,\"request_id\":\"" + id + "\",\"ok\":true,\"result\":" + result + "}";
}
}

int main(int argc, char** argv)
{
	const ULONGLONG deadline = GetTickCount64() + (argc > 1 ? std::atoi(argv[1]) : 30) * 1000ULL;
	ULONGLONG maxTick = 0;
	while (GetTickCount64() < deadline)
	{
		const ULONGLONG start = GetTickCount64();
		GeneralsAgent::UpdateBridge();
		const ULONGLONG elapsed = GetTickCount64() - start;
		if (elapsed > maxTick)
			maxTick = elapsed;
		Sleep(1);
	}
	GeneralsAgent::ShutdownBridge();
	std::cout << "max_tick_ms=" << maxTick << "\n";
	return 0;
}
