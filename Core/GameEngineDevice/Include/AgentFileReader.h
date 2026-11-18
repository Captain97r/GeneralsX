#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace GeneralsAgent
{
// GeneralsX @feature Open a regular replay file with bounded reads and an
// exclusive destination for copies. Platform handles stay in the device layer.
class AgentFileReader
{
public:
    AgentFileReader() = default;
    ~AgentFileReader();
    AgentFileReader(const AgentFileReader&) = delete;
    AgentFileReader& operator=(const AgentFileReader&) = delete;

    bool open(const std::string& path, bool requireClosed, std::string& error);
    bool readPrefix(void* buffer, std::size_t capacity, std::size_t& bytesRead);
    bool copyToNewFile(const std::string& path);
    std::uint64_t size() const { return m_size; }

private:
    void* m_file = nullptr;
    std::uint64_t m_size = 0;
};
}
