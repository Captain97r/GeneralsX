"""Exercise the real C++ pipe transport with a stub adapter, never a running game.

Build Tests/AgentBridge into build-agent-pipe-tests first, then run this file with
the sidecar's Python environment. GENERALS_AGENT_HARNESS can override the binary.
"""

from contextlib import contextmanager
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import unittest
import uuid

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tools" / "generals-mcp" / "src"))

from generals_mcp.bridge import BridgeError, GameBridge, pipe_path
from generals_mcp.winpipe import WindowsPipe

HARNESS = Path(os.environ.get("GENERALS_AGENT_HARNESS", str(REPOSITORY / "build-agent-pipe-tests" / "Release" / "agent_pipe_harness.exe")))


def request(method, *args, request_id=None):
    return ("\t".join(["GZH1", request_id or uuid.uuid4().hex, method, *args]) + "\n").encode()


class RawConnection:
    """Small synchronous writer for fragmented/idle clients; bounded reads use PeekNamedPipe."""
    def __init__(self, path):
        self.api = c.WinDLL("kernel32", use_last_error=True)
        self.api.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
        self.api.CreateFileW.restype = w.HANDLE
        self.api.WaitNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD]
        self.api.WaitNamedPipeW.restype = w.BOOL
        self.api.CloseHandle.argtypes = [w.HANDLE]
        for name in ("ReadFile", "WriteFile"):
            function = getattr(self.api, name)
            function.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, c.POINTER(w.DWORD), w.LPVOID]
            function.restype = w.BOOL
        self.api.PeekNamedPipe.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, w.LPVOID, c.POINTER(w.DWORD), w.LPVOID]
        self.api.PeekNamedPipe.restype = w.BOOL
        deadline = time.monotonic() + 3
        while True:
            self.handle = self.api.CreateFileW(path, 0xC0000000, 0, None, 3, 0x00110000, None)
            if self.handle != c.c_void_p(-1).value:
                break
            error = c.get_last_error()
            if error != 231 or time.monotonic() >= deadline:
                raise c.WinError(error)
            self.api.WaitNamedPipeW(path, 100)

    def write(self, data):
        transferred = w.DWORD()
        if not self.api.WriteFile(self.handle, c.create_string_buffer(data), len(data), c.byref(transferred), None):
            raise c.WinError(c.get_last_error())
        if transferred.value != len(data):
            raise AssertionError("Fixture fragment was only partially written")

    def read_line(self, timeout=3):
        deadline = time.monotonic() + timeout
        response = bytearray()
        while b"\n" not in response:
            available = w.DWORD()
            if not self.api.PeekNamedPipe(self.handle, None, 0, None, c.byref(available), None):
                raise c.WinError(c.get_last_error())
            if available.value:
                buffer = c.create_string_buffer(min(available.value, 65536))
                transferred = w.DWORD()
                if not self.api.ReadFile(self.handle, buffer, len(buffer), c.byref(transferred), None):
                    raise c.WinError(c.get_last_error())
                response.extend(buffer.raw[:transferred.value])
            elif time.monotonic() >= deadline:
                raise TimeoutError("Native harness did not return a complete response")
            else:
                time.sleep(0.005)
        return bytes(response)

    def close(self):
        self.api.CloseHandle(self.handle)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


@unittest.skipUnless(os.name == "nt", "Native pipe harness requires Windows")
class NativePipeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HARNESS.is_file():
            raise RuntimeError(f"Build the native transport harness first: {HARNESS}")

    @contextmanager
    def harness(self, lifetime=15):
        suffix = "native_test_" + uuid.uuid4().hex
        env = dict(os.environ, GENERALS_MCP_PIPE=suffix)
        process = subprocess.Popen([str(HARNESS), str(lifetime)], env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
        bridge = GameBridge(suffix, 3)
        try:
            deadline = time.monotonic() + 3
            while True:
                if process.poll() is not None:
                    stdout, stderr = process.communicate()
                    self.fail(f"Harness exited before listening: {process.returncode}; {stdout}; {stderr}")
                try:
                    bridge.call("get_state")
                    break
                except BridgeError as exc:
                    if exc.code != "bridge_unavailable" or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.01)
            yield suffix, bridge, process
        finally:
            if process.poll() is None:
                process.terminate()
            process.communicate(timeout=5)

    def exchange(self, suffix, wire):
        return json.loads(WindowsPipe().exchange(pipe_path(suffix), wire, 3))

    def assert_responsive(self, bridge, process):
        self.assertIsNone(process.poll(), "Malformed or idle client killed the native harness")
        self.assertTrue(bridge.call("get_state")["ok"])

    def test_game_bridge_calls_native_transport(self):
        with self.harness() as (suffix, bridge, process):
            state = bridge.call("get_state")
            actions = bridge.call("get_actions", 17)
            trained = bridge.call("train_unit", 17, "Cyborg")
            moved = bridge.call("attack_move", [21, 22], 200, 300)
            self.assertEqual(actions["result"]["executions"], state["result"]["executions"] + 1)
            self.assertEqual(moved["result"]["executions"], trained["result"]["executions"] + 1)
            self.assert_responsive(bridge, process)

    def test_order_replay_and_conflict(self):
        with self.harness() as (suffix, bridge, process):
            for method, arguments in (("train_unit", ("17", "Cyborg")), ("attack_move", ("21,22", "200", "300"))):
                with self.subTest(method=method):
                    identifier = uuid.uuid4().hex
                    wire = request(method, *arguments, request_id=identifier)
                    first = self.exchange(suffix, wire)
                    repeated = self.exchange(suffix, wire)
                    self.assertEqual(repeated, first, "Same command ID must replay the cached response without executing again")
                    conflict = self.exchange(suffix, request("get_state", request_id=identifier))
                    self.assertFalse(conflict["ok"])
                    self.assertEqual(conflict["error"]["code"], "request_id_conflict")
                    after = bridge.call("get_state")
                    self.assertEqual(after["result"]["executions"], first["result"]["executions"] + 1)
            self.assert_responsive(bridge, process)

    def test_fragmented_request(self):
        with self.harness() as (suffix, bridge, process):
            wire = request("get_state")
            with RawConnection(pipe_path(suffix)) as connection:
                for fragment in (wire[:4], wire[4:10], wire[10:-1], wire[-1:]):
                    connection.write(fragment)
                    time.sleep(0.01)
                reply = json.loads(connection.read_line())
            self.assertTrue(reply["ok"])
            self.assertEqual(reply["request_id"], wire.decode().split("\t")[1])
            self.assert_responsive(bridge, process)

    def test_malformed_and_oversize_requests_recover(self):
        with self.harness() as (suffix, bridge, process):
            cases = [(b"wrong\n", "invalid_request"),
                     (request("get_state") + b"extra\n", "invalid_request"),
                     (b'GZH1\tbad"id\tget_state\n', "invalid_request"),
                     (b"GZH1\tid\tget_state\t\x00\n", "invalid_request"),
                     (b"GZH1\tid\tget_state\t\xff\n", "invalid_request"),
                     (b"GZH1\tid\tget_state\t" + b"x" * 17000 + b"\n", "request_too_large")]
            for wire, code in cases:
                with self.subTest(code=code, size=len(wire)):
                    reply = self.exchange(suffix, wire)
                    self.assertFalse(reply["ok"])
                    self.assertEqual(reply["error"]["code"], code)
                    self.assert_responsive(bridge, process)

    def test_large_response_is_not_discarded(self):
        with self.harness() as (suffix, bridge, process):
            reply = self.exchange(suffix, request("large"))
            self.assertEqual(reply["result"]["payload"], "x" * 200000)
            self.assert_responsive(bridge, process)

    def test_idle_client_times_out_and_update_stays_nonblocking(self):
        # Let the harness exit normally to inspect its maximum UpdateBridge call duration.
        with self.harness(lifetime=7) as (suffix, bridge, process):
            with RawConnection(pipe_path(suffix)):
                start = time.monotonic()
                recovered = GameBridge(suffix, 6.5).call("get_state")
                duration = time.monotonic() - start
                self.assertTrue(recovered["ok"])
                self.assertGreater(duration, 4.5, "Idle connection was not held for the documented 5-second deadline")
                self.assertLess(duration, 6.5)
            self.assert_responsive(bridge, process)
            stdout, stderr = process.communicate(timeout=3)
            self.assertEqual(process.returncode, 0, stderr)
            match = re.search(r"max_tick_ms=(\d+)", stdout)
            self.assertIsNotNone(match, stdout)
            self.assertLess(int(match.group(1)), 1000, "Slow client blocked the game update loop")

    def test_slow_reader_is_disconnected(self):
        with self.harness() as (suffix, bridge, process):
            with RawConnection(pipe_path(suffix)) as connection:
                connection.write(request("large"))
                # Reading no bytes forces a pending response larger than the pipe output buffer.
                recovered = GameBridge(suffix, 6.5).call("get_state")
                self.assertTrue(recovered["ok"])
            self.assert_responsive(bridge, process)


if __name__ == "__main__":
    unittest.main(verbosity=2)
