// TheSuperHackers @feature Local, opt-in MCP bridge. No game objects are accessed by IO threads.
// This transport has no engine/PCH dependency so it can be tested independently.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include <deque>
#include <string>
#include <vector>

#include "Common/AgentBridge.h"
#include "Common/AgentGameAdapter.h"

namespace GeneralsAgent
{
namespace
{
const size_t MAX_REQUEST_BYTES = 16384;
const size_t MAX_RESPONSE_BYTES = 1024 * 1024;
const ULONGLONG CLIENT_TIMEOUT_MS = 5000;

bool IsIdentifier(const std::string& value, size_t limit)
{
	if (value.empty() || value.size() > limit)
		return false;
	for (char c : value)
	{
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
			return false;
	}
	return true;
}

std::string Error(const std::string& id, const char* code, const char* message)
{
	// Arguments are fixed strings or validated identifiers, never arbitrary request text.
	return "{\"protocol\":1,\"request_id\":\"" + id +
		"\",\"ok\":false,\"error\":{\"code\":\"" + code +
		"\",\"message\":\"" + message + "\"}}";
}

struct CachedCommand
{
	std::string id;
	std::string request;
	std::string response;
};

class Bridge
{
public:
	~Bridge() { Shutdown(); }
	bool IsIdle() const { return m_stage == CONNECTING; }
	bool IsEnabled() const { return m_pipe != INVALID_HANDLE_VALUE; }

	void Update()
	{
		if (!m_initialized)
			Initialize();
		if (m_pipe == INVALID_HANDLE_VALUE)
			return;
		if (!m_pending)
		{
			Reconnect();
			return;
		}

		// Poll only: a disconnected or slow MCP client must never stall the simulation.
		DWORD transferred = 0;
		if (!GetOverlappedResult(m_pipe, &m_overlapped, &transferred, FALSE))
		{
			const DWORD error = GetLastError();
			if (error == ERROR_IO_INCOMPLETE)
			{
				if (m_stage != CONNECTING && m_stage != CANCELLING &&
					GetTickCount64() >= m_deadline)
				{
					CancelIoEx(m_pipe, &m_overlapped);
					m_stage = CANCELLING;
				}
				return;
			}
			m_pending = false;
			Reconnect();
			return;
		}
		m_pending = false;

		switch (m_stage)
		{
			case CONNECTING:
				m_deadline = GetTickCount64() + CLIENT_TIMEOUT_MS;
				Read();
				break;
			case READING:
				if (transferred == 0)
				{
					Reconnect();
					break;
				}
				m_request.append(m_buffer, transferred);
				if (m_request.size() > MAX_REQUEST_BYTES)
					Reply(Error("", "request_too_large", "Request exceeds 16384 bytes."));
				else if (m_request.find('\n') != std::string::npos)
					Reply(Dispatch());
				else
					Read();
				break;
			case WRITING:
				if (transferred == 0)
				{
					Reconnect();
					break;
				}
				m_written += transferred;
				if (m_written == m_response.size())
					WaitForClientClose();
				else
					Write();
				break;
			case DRAINING:
				Reconnect();
				break;
			case CANCELLING:
				Reconnect();
				break;
		}
	}

	void Shutdown()
	{
		if (m_pipe != INVALID_HANDLE_VALUE)
		{
			if (m_pending)
			{
				// Shutdown only: drain cancellation before freeing OVERLAPPED storage.
				CancelIoEx(m_pipe, &m_overlapped);
				DWORD ignored = 0;
				GetOverlappedResult(m_pipe, &m_overlapped, &ignored, TRUE);
			}
			DisconnectNamedPipe(m_pipe);
			CloseHandle(m_pipe);
			m_pipe = INVALID_HANDLE_VALUE;
		}
		if (m_overlapped.hEvent)
		{
			CloseHandle(m_overlapped.hEvent);
			m_overlapped.hEvent = nullptr;
		}
		m_pending = false;
		m_initialized = true;
		m_cache.clear();
	}

private:
	enum Stage { CONNECTING, READING, WRITING, DRAINING, CANCELLING };
	bool m_initialized = false;
	bool m_pending = false;
	HANDLE m_pipe = INVALID_HANDLE_VALUE;
	OVERLAPPED m_overlapped = {};
	Stage m_stage = CONNECTING;
	ULONGLONG m_deadline = 0;
	char m_buffer[4096] = {};
	std::string m_request;
	std::string m_response;
	size_t m_written = 0;
	std::deque<CachedCommand> m_cache;

	void Initialize()
	{
		m_initialized = true;
		char suffix[65] = {};
		const DWORD count = GetEnvironmentVariableA("GENERALS_MCP_PIPE", suffix, sizeof(suffix));
		if (count == 0)
			return; // No environment variable: no pipe and no changes to normal gameplay.
		if (count >= sizeof(suffix) || !IsIdentifier(suffix, 64))
		{
			OutputDebugStringA("Generals MCP: invalid GENERALS_MCP_PIPE suffix; bridge disabled.\n");
			return;
		}

		// Only this process's Windows user can connect; remote clients are rejected.
		HANDLE token = nullptr;
		DWORD size = 0;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
			return;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<char> tokenData(size);
		if (!size || !GetTokenInformation(token, TokenUser, tokenData.data(), size, &size))
		{
			CloseHandle(token);
			return;
		}
		CloseHandle(token);
		EXPLICIT_ACCESSA access = {};
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = NO_INHERITANCE;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = reinterpret_cast<LPSTR>(
			reinterpret_cast<TOKEN_USER*>(tokenData.data())->User.Sid);
		PACL acl = nullptr;
		if (SetEntriesInAclA(1, &access, nullptr, &acl) != ERROR_SUCCESS)
			return;
		SECURITY_DESCRIPTOR descriptor = {};
		InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
		SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE);
		SECURITY_ATTRIBUTES security = {sizeof(security), &descriptor, FALSE};
		const std::string name = std::string("\\\\.\\pipe\\GeneralsZH.MCP.") + suffix;
		m_pipe = CreateNamedPipeA(name.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, 65536, 16384, 0, &security);
		LocalFree(acl);
		if (m_pipe == INVALID_HANDLE_VALUE)
		{
			OutputDebugStringA("Generals MCP: cannot create private pipe (possibly already in use).\n");
			return;
		}
		m_overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
		if (!m_overlapped.hEvent)
		{
			CloseHandle(m_pipe);
			m_pipe = INVALID_HANDLE_VALUE;
			return;
		}
		Connect();
	}

	void ResetOperation()
	{
		const HANDLE event = m_overlapped.hEvent;
		m_overlapped = {};
		m_overlapped.hEvent = event;
		ResetEvent(event);
	}

	void Connect()
	{
		ResetOperation();
		m_stage = CONNECTING;
		const BOOL connected = ConnectNamedPipe(m_pipe, &m_overlapped);
		const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
		if (connected || error == ERROR_IO_PENDING)
			m_pending = true;
		else if (error == ERROR_PIPE_CONNECTED)
		{
			m_deadline = GetTickCount64() + CLIENT_TIMEOUT_MS;
			Read();
		}
		else if (error == ERROR_NO_DATA)
			m_pending = false; // Client disconnected before Connect; retry next frame.
		else
			Shutdown(); // Do not spin or recreate a failed listener every game frame.
	}

	void Reconnect()
	{
		DisconnectNamedPipe(m_pipe);
		m_request.clear();
		m_response.clear();
		m_written = 0;
		Connect();
	}

	void Read()
	{
		ResetOperation();
		m_stage = READING;
		DWORD ignored = 0;
		const BOOL success = ReadFile(m_pipe, m_buffer, sizeof(m_buffer), &ignored, &m_overlapped);
		if (success || GetLastError() == ERROR_IO_PENDING)
			m_pending = true;
		else
			Reconnect();
	}

	void Reply(std::string response)
	{
		if (response.size() + 1 > MAX_RESPONSE_BYTES)
			response = Error("", "response_too_large", "Game observation exceeds the response limit.");
		m_response = response + "\n";
		m_written = 0;
		Write();
	}

	void Write()
	{
		ResetOperation();
		m_stage = WRITING;
		DWORD ignored = 0;
		const BOOL success = WriteFile(m_pipe, m_response.data() + m_written,
			static_cast<DWORD>(m_response.size() - m_written), &ignored, &m_overlapped);
		if (success || GetLastError() == ERROR_IO_PENDING)
			m_pending = true;
		else
			Reconnect();
	}

	void WaitForClientClose()
	{
		// DisconnectNamedPipe discards unread buffered output. Let the client read the
		// complete response and close first, without blocking on FlushFileBuffers.
		ResetOperation();
		m_stage = DRAINING;
		DWORD ignored = 0;
		const BOOL success = ReadFile(m_pipe, m_buffer, 1, &ignored, &m_overlapped);
		if (success || GetLastError() == ERROR_IO_PENDING)
			m_pending = true;
		else
			Reconnect();
	}

	std::string Dispatch()
	{
		if (m_request.back() != '\n' || m_request.find('\n') != m_request.size() - 1)
			return Error("", "invalid_request", "Send exactly one newline-terminated request per connection.");
		std::string line = m_request.substr(0, m_request.size() - 1);
		for (unsigned char c : line)
		{
			if (c != '\t' && (c < 32 || c > 126))
				return Error("", "invalid_request", "Request fields must contain printable ASCII.");
		}
		std::vector<std::string> fields;
		size_t begin = 0;
		for (;;)
		{
			const size_t end = line.find('\t', begin);
			fields.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
			if (end == std::string::npos)
				break;
			begin = end + 1;
		}
		if (fields.size() < 3 || fields[0] != "GZH1" ||
			!IsIdentifier(fields[1], 64) || !IsIdentifier(fields[2], 64))
			return Error("", "invalid_request", "Expected GZH1, request identifier, method, and arguments.");
		const std::string& id = fields[1];
		for (const CachedCommand& cached : m_cache)
		{
			if (cached.id == id)
				return cached.request == line ? cached.response :
					Error(id, "request_id_conflict", "Request identifier was already used for another command.");
		}
		const std::vector<std::string> args(fields.begin() + 3, fields.end());
		std::string response;
		try
		{
			response = Execute(id, fields[2], args);
		}
		catch (...)
		{
			response = Error(id, "internal_error", "Game adapter failed; command outcome may be uncertain.");
		}
		// Every mutating method gets the same bounded duplicate-delivery protection.
		const std::string& method = fields[2];
		const bool readOnly = method.compare(0, 4, "get_") == 0 ||
			method.compare(0, 5, "list_") == 0 || method == "check_build" || method == "find_build_sites";
		if (!readOnly)
		{
			m_cache.push_back({id, line, response});
			if (m_cache.size() > 128)
				m_cache.pop_front();
		}
		return response;
	}
};

// The engine's allocator is initialized after CRT static construction and is
// shut down before CRT destruction. Keep all owning STL storage inside that lifetime.
Bridge* s_bridge = nullptr;
}

void UpdateBridge()
{
	if (!s_bridge)
		s_bridge = new Bridge;
	s_bridge->Update();
}
void ShutdownBridge()
{
	delete s_bridge;
	s_bridge = nullptr;
}
bool IsBridgeIdle() { return s_bridge && s_bridge->IsEnabled() && s_bridge->IsIdle(); }
bool IsBridgeEnabled() { return s_bridge && s_bridge->IsEnabled(); }
}
