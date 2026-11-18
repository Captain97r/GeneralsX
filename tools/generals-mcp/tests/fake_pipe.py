"""Isolated Windows named-pipe fixture used only with random test pipe names."""
import ctypes as c
from ctypes import wintypes as w
import json
import sys
import time

api = c.WinDLL("kernel32", use_last_error=True)
api.CreateNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.LPVOID]
api.CreateNamedPipeW.restype = w.HANDLE
api.ConnectNamedPipe.argtypes = [w.HANDLE, w.LPVOID]
api.ConnectNamedPipe.restype = w.BOOL
api.ReadFile.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, c.POINTER(w.DWORD), w.LPVOID]
api.ReadFile.restype = w.BOOL
api.WriteFile.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, c.POINTER(w.DWORD), w.LPVOID]
api.WriteFile.restype = w.BOOL
api.FlushFileBuffers.argtypes = [w.HANDLE]
api.DisconnectNamedPipe.argtypes = [w.HANDLE]
api.CloseHandle.argtypes = [w.HANDLE]
name, mode = sys.argv[1:]
handle = api.CreateNamedPipeW(name, 3 | 0x00080000, 8, 1, 65536, 65536, 0, None)
if handle == c.c_void_p(-1).value:
    raise c.WinError(c.get_last_error())
try:
    print("ready", flush=True)
    if not api.ConnectNamedPipe(handle, None) and c.get_last_error() != 535:
        raise c.WinError(c.get_last_error())
    buffer = c.create_string_buffer(16384)
    count = w.DWORD()
    if not api.ReadFile(handle, buffer, len(buffer), c.byref(count), None):
        raise c.WinError(c.get_last_error())
    fields = buffer.raw[:count.value].decode().rstrip("\n").split("\t")
    if mode == "stall":
        time.sleep(60)
    elif mode == "close":
        pass
    else:
        response = (json.dumps({"protocol": 1, "request_id": fields[1], "ok": True,
                               "result": {"method": fields[2], "padding": "x" * 80000}}) + "\n").encode()
        for start in range(0, len(response), 7919):
            fragment = response[start:start + 7919]
            if not api.WriteFile(handle, c.create_string_buffer(fragment), len(fragment), c.byref(count), None):
                raise c.WinError(c.get_last_error())
        api.FlushFileBuffers(handle)
finally:
    api.DisconnectNamedPipe(handle)
    api.CloseHandle(handle)
