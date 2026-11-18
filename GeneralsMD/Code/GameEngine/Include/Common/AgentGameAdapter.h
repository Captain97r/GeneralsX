#pragma once

#include <string>
#include <vector>

// TheSuperHackers @feature Expose a small, main-thread-only skirmish control API.
namespace GeneralsAgent
{

// Called before MessageStream::propagateMessages(). Arguments are UTF-8 wire
// fields; game objects must never be accessed by the transport's I/O machinery.
std::string Execute(const std::string& requestId, const std::string& method,
    const std::vector<std::string>& args);

}
