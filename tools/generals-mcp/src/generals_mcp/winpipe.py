"""Bounded, overlapped Windows named-pipe I/O; no third-party Windows dependency."""

from __future__ import annotations

import os
import time

from .bridge import BridgeError, MAX_RESPONSE_BYTES


class WindowsPipe:
    def exchange(self, path: str, request: bytes, timeout: float) -> bytes:
        if os.name != "nt":
            raise BridgeError("unsupported_platform", "The game bridge currently requires Windows.")
        # Import Win32 types only here so schema/protocol tests can run on other systems.
        import ctypes as c
        from ctypes import wintypes as w

        class OVERLAPPED(c.Structure):
            _fields_ = [("Internal", c.c_size_t), ("InternalHigh", c.c_size_t),
                        ("Offset", w.DWORD), ("OffsetHigh", w.DWORD), ("hEvent", w.HANDLE)]

        api = c.WinDLL("kernel32", use_last_error=True)
        api.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
        api.CreateFileW.restype = w.HANDLE
        api.WaitNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD]
        api.WaitNamedPipeW.restype = w.BOOL
        api.CreateEventW.argtypes = [w.LPVOID, w.BOOL, w.BOOL, w.LPCWSTR]
        api.CreateEventW.restype = w.HANDLE
        api.CloseHandle.argtypes = [w.HANDLE]
        api.CloseHandle.restype = w.BOOL
        api.ResetEvent.argtypes = [w.HANDLE]
        api.ResetEvent.restype = w.BOOL
        for name in ("ReadFile", "WriteFile"):
            function = getattr(api, name)
            function.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, c.POINTER(w.DWORD), c.POINTER(OVERLAPPED)]
            function.restype = w.BOOL
        api.GetOverlappedResult.argtypes = [w.HANDLE, c.POINTER(OVERLAPPED), c.POINTER(w.DWORD), w.BOOL]
        api.GetOverlappedResult.restype = w.BOOL
        api.CancelIoEx.argtypes = [w.HANDLE, c.POINTER(OVERLAPPED)]
        api.CancelIoEx.restype = w.BOOL
        api.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
        api.WaitForSingleObject.restype = w.DWORD

        deadline = time.monotonic() + timeout
        sent = False
        invalid_handle = c.c_void_p(-1).value

        def remaining_ms() -> int:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BridgeError("timeout", "The game bridge did not reply before the timeout.", delivery_unknown=sent)
            return max(1, int(remaining * 1000))

        def io_error(error: int) -> BridgeError:
            return BridgeError("pipe_error", f"Windows pipe error {error}: {c.FormatError(error).strip()}", delivery_unknown=sent)

        while True:
            remaining_ms()
            # Identification-level impersonation prevents the server from acting as this client.
            handle = api.CreateFileW(path, 0xC0000000, 0, None, 3, 0x40000000 | 0x00110000, None)
            if handle != invalid_handle:
                break
            error = c.get_last_error()
            if error == 2:
                raise BridgeError("bridge_unavailable", "No game bridge is listening. Launch the MCP-enabled build with the matching GENERALS_MCP_PIPE value.")
            if error != 231:  # ERROR_PIPE_BUSY: only connection attempts are repeated, never orders.
                raise io_error(error)
            if not api.WaitNamedPipeW(path, remaining_ms()):
                error = c.get_last_error()
                if error == 121:
                    raise BridgeError("timeout", "The game bridge remained busy before request delivery.")
                raise io_error(error)

        event = None
        try:
            event = api.CreateEventW(None, True, False, None)
            if not event:
                raise io_error(c.get_last_error())

            def transfer(write: bool, buffer: c.Array, size: int) -> int:
                nonlocal sent
                remaining_ms()
                api.ResetEvent(event)
                overlapped = OVERLAPPED()
                overlapped.hEvent = event
                completed = w.DWORD()
                if write:
                    sent = True  # A write failure after submission has ambiguous delivery.
                operation = api.WriteFile if write else api.ReadFile
                success = operation(handle, buffer, size, c.byref(completed), c.byref(overlapped))
                if success:
                    return completed.value
                error = c.get_last_error()
                if error != 997:  # ERROR_IO_PENDING
                    raise io_error(error)
                try:
                    wait_result = api.WaitForSingleObject(event, remaining_ms())
                    if wait_result == 258:
                        raise BridgeError("timeout", "The game bridge did not reply before the timeout.", delivery_unknown=sent)
                    if wait_result != 0:
                        raise io_error(c.get_last_error())
                    if not api.GetOverlappedResult(handle, c.byref(overlapped), c.byref(completed), False):
                        raise io_error(c.get_last_error())
                    return completed.value
                finally:
                    # Complete cancelled I/O before stack buffers and OVERLAPPED storage disappear.
                    api.CancelIoEx(handle, c.byref(overlapped))
                    api.GetOverlappedResult(handle, c.byref(overlapped), c.byref(completed), True)

            offset = 0
            while offset < len(request):
                buffer = c.create_string_buffer(request[offset:])
                written = transfer(True, buffer, len(request) - offset)
                if not written:
                    raise BridgeError("pipe_error", "The pipe accepted zero request bytes.", delivery_unknown=True)
                offset += written
            response = bytearray()
            while True:
                buffer = c.create_string_buffer(65536)
                size = transfer(False, buffer, len(buffer))
                if not size:
                    raise BridgeError("pipe_closed", "The game closed the pipe without a complete reply.", delivery_unknown=True)
                response.extend(buffer.raw[:size])
                if len(response) > MAX_RESPONSE_BYTES:
                    raise BridgeError("response_too_large", "The bridge response exceeds 1 MiB.", delivery_unknown=True)
                if b"\n" in response:
                    return bytes(response)
        finally:
            if event:
                api.CloseHandle(event)
            api.CloseHandle(handle)
