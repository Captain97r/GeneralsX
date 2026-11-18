#include "AgentFileReader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace GeneralsAgent
{
AgentFileReader::~AgentFileReader()
{
    if (m_file)
        CloseHandle(m_file);
}

bool AgentFileReader::open(const std::string& path, bool requireClosed, std::string& error)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ,
        requireClosed ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD code = GetLastError();
        error = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? "replay_not_found" :
            code == ERROR_SHARING_VIOLATION ? "replay_busy" : "replay_read_failed";
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    {
        CloseHandle(file);
        error = "invalid_replay";
        return false;
    }
    if (m_file)
        CloseHandle(m_file);
    m_file = file;
    m_size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return true;
}

bool AgentFileReader::readPrefix(void* buffer, std::size_t capacity, std::size_t& bytesRead)
{
    LARGE_INTEGER start{};
    DWORD read = 0;
    if (!m_file || capacity > MAXDWORD || !SetFilePointerEx(m_file, start, nullptr, FILE_BEGIN) ||
        !ReadFile(m_file, buffer, static_cast<DWORD>(capacity), &read, nullptr))
        return false;
    bytesRead = read;
    return true;
}

bool AgentFileReader::copyToNewFile(const std::string& path)
{
    LARGE_INTEGER start{};
    if (!m_file || !SetFilePointerEx(m_file, start, nullptr, FILE_BEGIN))
        return false;
    HANDLE destination = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (destination == INVALID_HANDLE_VALUE)
        return false;
    char buffer[65536];
    std::uint64_t total = 0;
    bool copied = true;
    while (total < m_size)
    {
        DWORD read = 0, written = 0;
        const DWORD remaining = static_cast<DWORD>(m_size - total < sizeof(buffer) ? m_size - total : sizeof(buffer));
        if (!ReadFile(m_file, buffer, remaining, &read, nullptr) || !read ||
            !WriteFile(destination, buffer, read, &written, nullptr) || written != read)
        {
            copied = false;
            break;
        }
        total += written;
    }
    copied = copied && FlushFileBuffers(destination);
    CloseHandle(destination);
    if (!copied)
        DeleteFileA(path.c_str()); // This call exclusively created the destination.
    return copied;
}
}
