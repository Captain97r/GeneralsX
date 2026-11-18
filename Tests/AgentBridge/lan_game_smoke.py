"""Exercise LAN lobby controls through official MCP in isolated game processes.

Default: one host with an AI opponent. --two-peers additionally tests Direct
Connect, join acknowledgement, acceptance and start with a second local game.
Use --windowed for the game's real LAN menu callbacks. Startup does not request
activation; no screenshots, keyboard or mouse input are used. All endpoints bind
loopback. No existing game is attached or stopped.
"""
from __future__ import annotations

import argparse
import asyncio
from contextlib import AsyncExitStack
from datetime import timedelta
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "generals-mcp" / "src"))
from generals_mcp.bridge import BridgeError, MUTATING_METHODS
from generals_mcp.cli import tool_output

LAN_TOOLS = {"get_lobby", "enter_lan", "lan_host", "lan_join", "lan_accept", "lan_start",
             "lan_leave", "lan_set_slot", "lan_set_options"}


def process_window_text(pid: int) -> list[dict]:
    """Read error-dialog text only from the owned fixture; no input or images."""
    import ctypes
    from ctypes import wintypes
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.EnumChildWindows.argtypes = [wintypes.HWND, callback_type, wintypes.LPARAM]
    result = []

    def record(window):
        text = ctypes.create_unicode_buffer(4096)
        kind = ctypes.create_unicode_buffer(256)
        user.GetWindowTextW(window, text, len(text))
        user.GetClassNameW(window, kind, len(kind))
        if text.value:
            result.append({"class": kind.value, "text": text.value})

    @callback_type
    def child(window, _):
        record(window)
        return True

    @callback_type
    def parent(window, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid:
            record(window)
            user.EnumChildWindows(window, child, 0)
        return True

    user.EnumWindows(parent, 0)
    return result


async def run(args) -> int:
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = {"exe": str(args.exe.resolve()), "two_peers": args.two_peers,
                "windowed": args.windowed, "screenshots": 0, "mouse_or_keyboard_inputs": 0,
                "transport": "official MCP client -> stdio MCP server -> native engine bridge",
                "endpoints": [], "checks": [], "passed": False}
    processes: list[subprocess.Popen] = []

    def checked(description):
        evidence["checks"].append(description)
        print("PASS " + description, flush=True)
        (output / "result.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")

    async def until(reader, predicate, seconds=45):
        deadline = time.monotonic() + seconds
        last = None
        while time.monotonic() < deadline:
            try:
                last = await reader()
                if predicate(last):
                    return last
            except BridgeError as error:
                last = error.as_dict()
            await asyncio.sleep(0.15)
        raise RuntimeError(f"Read polling expired; last result: {last}")

    try:
        async with AsyncExitStack() as stack:
            async def start_peer(label, ip):
                directory = output / label
                directory.mkdir(parents=True, exist_ok=True)
                suffix = "lan_" + uuid.uuid4().hex[:12]
                env = dict(os.environ, GENERALS_MCP_PIPE=suffix, GENERALS_MCP_TEST_MAP="",
                           GENERALS_MCP_TEST_SESSION="1", GENERALS_MCP_TEST_USER_DATA=str(directory),
                           GENERALS_MCP_TEST_TIMEOUT_SECONDS="240",
                           PYTHONPATH=str(ROOT / "tools" / "generals-mcp" / "src"))
                env["GENERALS_MCP_TEST_WINDOWED"] = "1" if args.windowed else "0"
                stdout = stack.enter_context((directory / "stdout.log").open("w"))
                stderr = stack.enter_context((directory / "stderr.log").open("w"))
                mcp_log = stack.enter_context((directory / "mcp-stderr.log").open("w"))
                startupinfo = None
                if args.windowed:
                    startupinfo = subprocess.STARTUPINFO()
                    startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                    startupinfo.wShowWindow = 7 if args.minimized else 4  # SW_SHOWMINNOACTIVE / SW_SHOWNOACTIVATE.
                display_args = ["-win"] if args.windowed else ["-headless"]
                process = subprocess.Popen(
                    [str(args.exe.resolve()), *display_args, "-quickstart", "-noaudio",
                     "-setCwd", str(args.assets.resolve()), "-assetPath", str(args.assets.resolve())], cwd=args.exe.resolve().parent,
                    env=env, stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW,
                    startupinfo=startupinfo)
                processes.append(process)
                evidence["endpoints"].append({"label": label, "pid": process.pid, "pipe": suffix, "ip": ip})
                print(f"Isolated LAN {label} PID {process.pid}", flush=True)
                params = StdioServerParameters(command=sys.executable,
                    args=["-m", "generals_mcp", "--pipe", suffix, "--timeout", "8"], env=env)
                read, write = await stack.enter_async_context(stdio_client(params, errlog=mcp_log))
                client = await stack.enter_async_context(ClientSession(read, write, read_timeout_seconds=timedelta(seconds=12)))
                await client.initialize()
                catalog = {tool.name for tool in (await client.list_tools()).tools}
                if not LAN_TOOLS.issubset(catalog):
                    raise RuntimeError(f"Missing LAN tools: {LAN_TOOLS - catalog}")

                async def call(name, **arguments):
                    if process.poll() is not None:
                        raise RuntimeError(f"Isolated {label} game exited: {process.returncode}")
                    try:
                        reply = await client.call_tool(name, arguments)
                    except Exception as error:
                        raise BridgeError("mcp_transport_error", str(error), delivery_unknown=name in MUTATING_METHODS) from error
                    parsed = tool_output(name, reply)
                    if parsed["is_error"]:
                        error = parsed["error"]
                        evidence.setdefault("failed_calls", []).append({"peer": label, "tool": name, "arguments": arguments, "error": error})
                        if name != "get_lobby":
                            try:
                                diagnostic = await client.call_tool("get_lobby", {})
                                evidence[label + "_lobby_after_error"] = tool_output("get_lobby", diagnostic)
                            except Exception as diagnostic_error:
                                evidence[label + "_diagnostic_error"] = str(diagnostic_error)
                        raise BridgeError(error.get("code", "mcp_tool_error"), error.get("message", str(error)),
                                          delivery_unknown=error.get("delivery_unknown", name in MUTATING_METHODS))
                    envelope = parsed.get("result")
                    if (not isinstance(envelope, dict) or envelope.get("protocol") != 1
                            or envelope.get("ok") is not True or not isinstance(envelope.get("result"), dict)):
                        raise BridgeError("invalid_mcp_response", str(envelope), delivery_unknown=name in MUTATING_METHODS)
                    return envelope["result"]

                await until(lambda: call("get_capabilities"), lambda value: value.get("version"))
                before_enter = await until(lambda: call("get_lobby"), lambda value: value.get("screen") == "Menus/MainMenu.wnd" and not value.get("transitioning"))
                evidence[label + "_before_enter"] = before_enter
                await call("enter_lan", player_name="MCP_" + label, local_ip=ip)
                lobby = await until(lambda: call("get_lobby"), lambda value: value.get("available") and not value.get("transitioning") and not value.get("pending") and value.get("screen") == "Menus/LanLobbyMenu.wnd")
                if lobby.get("local_ip") != ip:
                    raise RuntimeError(f"LAN fixture did not bind requested loopback address: {lobby}")
                evidence[label + "_initial_lobby"] = lobby
                return call

            host = await start_peer("host", "127.0.0.2")
            checked("LAN menu entered on an isolated loopback endpoint through MCP")
            await host("lan_host")
            def hosted(value):
                return value.get("is_host") and value.get("current_game") and not value.get("transitioning") and not value.get("pending") and value.get("screen") == "Menus/LanGameOptionsMenu.wnd"
            lobby = await until(lambda: host("get_lobby"), hosted)
            checked("LAN hosting creates the local player's game through normal lobby controls")
            if args.check_leave:
                # Optional regression check: keep an explicit local endpoint across submenus.
                await host("lan_leave")
                left = await until(lambda: host("get_lobby"), lambda value: value.get("current_game") is None and not value.get("transitioning") and not value.get("pending") and value.get("screen") == "Menus/LanLobbyMenu.wnd")
                await asyncio.sleep(0.5)
                left = await host("get_lobby")
                if left["local_ip"] != "127.0.0.2" or left["local_name"] != "MCP_host":
                    raise RuntimeError(f"Leaving the LAN game changed the explicit test binding or name: {left}")
                evidence["host_after_leave"] = left
                await until(lambda: host("get_lobby"), lambda value: not value.get("transitioning") and not value.get("pending"))
                await host("lan_host")
                lobby = await until(lambda: host("get_lobby"), hosted)
                checked("LAN leave/rehost preserves the explicitly selected local endpoint and name")

            maps = await host("list_maps")
            map_name = next(item["name"] for item in maps["maps"] if "alpine assault" in item["name"].lower())
            faction = next(item["name"] for item in maps["factions"] if item["starting_building"] == "Infa_ChinaCommandCenter")
            opponent = next(item["name"] for item in maps["factions"] if item["name"] == "FactionGLA")
            cash_choices = lobby["starting_cash_choices"]
            money = 10000 if 10000 in cash_choices else cash_choices[0]
            await host("lan_set_options", map_name=map_name, starting_cash=money, limit_superweapons=True)
            local_slot = lobby["current_game"]["local_slot"]
            await host("lan_set_slot", slot=local_slot, state="keep", faction=faction, color=0, team=-1, start_position=0)

            if args.two_peers:
                guest = await start_peer("guest", "127.0.0.3")
                await guest("lan_join", host_ip="127.0.0.2", direct=True)
                guest_lobby = await until(lambda: guest("get_lobby"), lambda value: value.get("current_game") and value["current_game"]["local_slot"] >= 0)
                remote_slot = guest_lobby["current_game"]["local_slot"]
                await until(lambda: host("get_lobby"), lambda value: any(slot.get("ip") == "127.0.0.3" and slot["state"] == "human" for slot in value["current_game"]["slots"]))
                checked("Second loopback peer joins through Direct Connect and receives a game slot")
                await until(lambda: guest("get_lobby"), lambda value: not value.get("pending") and not value.get("transitioning") and value.get("screen") == "Menus/LanGameOptionsMenu.wnd")
                await guest("lan_set_slot", slot=remote_slot, state="keep", faction=opponent, color=1, team=-1, start_position=1)
                await until(lambda: guest("get_lobby"), lambda value: not value.get("pending") and any(slot["slot"] == remote_slot and slot["faction"] == opponent and slot["color"] == 1 and slot["start_position"] == 1 for slot in value["current_game"]["slots"]))
                await guest("lan_accept")
                await until(lambda: host("get_lobby"), lambda value: any(slot["slot"] == remote_slot and slot["accepted"] and slot["has_map"] for slot in value["current_game"]["slots"]))
                checked("Client slot settings and acceptance reach the host through native LAN messages")
            else:
                remote_slot = next(slot["slot"] for slot in lobby["current_game"]["slots"] if slot["slot"] != local_slot)
                await host("lan_set_slot", slot=remote_slot, state="medium", faction=opponent, color=1, team=-1, start_position=1)

            lobby = await host("get_lobby")
            game = lobby["current_game"]
            if game["map"].replace("/", "\\").lower() != map_name.replace("/", "\\").lower():
                raise RuntimeError(f"LAN map change not observed: {game}")
            if game["starting_cash"] != money or game["limit_superweapons"] is not True:
                raise RuntimeError(f"LAN options not observed: {game}")
            host_slot = next(slot for slot in game["slots"] if slot["slot"] == local_slot)
            enemy_slot = next(slot for slot in game["slots"] if slot["slot"] == remote_slot)
            if host_slot["faction"] != faction or enemy_slot["faction"] != opponent:
                raise RuntimeError(f"LAN faction settings not observed: {game}")
            if not args.two_peers and enemy_slot["state"] != "medium":
                raise RuntimeError(f"AI slot state not observed: {enemy_slot}")
            evidence["configured_lobby"] = lobby
            checked("LAN map, starting money, superweapon limit and player faction settings are observable")
            await host("lan_start")
            state = await until(lambda: host("get_game_state"), lambda value: value.get("mode") == "multiplayer" and value.get("objects"), seconds=90)
            evidence["host_match"] = state
            dozer = next(obj for obj in state["objects"] if "dozer" in obj["template"].lower())
            position = dozer["position"]
            bounds = state["map_bounds"]
            x = max(bounds["min"]["x"] + 5, position["x"] - 200)
            y = max(bounds["min"]["y"] + 5, position["y"] - 200)
            terrain = await host("get_map", x=x, y=y,
                                 width=min(400., bounds["max"]["x"] - x - 5),
                                 height=min(400., bounds["max"]["y"] - y - 5), step=50.)
            samples = [dict(zip(terrain["sample_fields"], values)) for values in terrain["samples"]]
            targets = [sample for sample in samples if sample["passable_ground"] is True
                       and sample["obstacle_id"] in (0, None)
                       and 150 ** 2 <= (sample["x"] - position["x"]) ** 2 + (sample["y"] - position["y"]) ** 2 <= 250 ** 2]
            if not targets:
                raise RuntimeError("No observed passable nearby dozer destination in the selected map bounds.")
            target = min(targets, key=lambda sample: (sample["x"] - position["x"]) ** 2 + (sample["y"] - position["y"]) ** 2)
            evidence["move_order"] = {"unit_id": dozer["id"], "before": position, "terrain_target": target}
            evidence["move_order"]["acknowledgement"] = await host("unit_order", unit_ids=[dozer["id"]], order="move", x=target["x"], y=target["y"])
            moved = await until(lambda: host("get_game_state"), lambda value: any(
                obj["id"] == dozer["id"] and (obj["position"]["x"] - target["x"]) ** 2
                + (obj["position"]["y"] - target["y"]) ** 2 < 40 ** 2 for obj in value.get("objects", [])))
            evidence["move_order"]["after"] = next(obj["position"] for obj in moved["objects"] if obj["id"] == dozer["id"])
            evidence["move_order"]["observed_frame"] = moved["frame"]
            checked("MCP gameplay move order drives the host dozer to an observed passable destination in the LAN match")
            later = await until(lambda: host("get_game_state"), lambda value: value.get("frame", 0) > moved["frame"] + 120)
            evidence["host_later_frame"] = later["frame"]
            if args.two_peers:
                guest_state = await until(lambda: guest("get_game_state"), lambda value: value.get("mode") == "multiplayer" and value.get("objects"), seconds=45)
                evidence["guest_match"] = guest_state
                if state["local_player_id"] == guest_state["local_player_id"]:
                    raise RuntimeError("Both LAN peers report the same local player ID.")
                guest_later = await until(lambda: guest("get_game_state"), lambda value: value.get("frame", 0) > guest_state["frame"] + 120)
                evidence["guest_later_frame"] = guest_later["frame"]
                checked("Distinct native LAN players each advance more than 120 simulation frames after the gameplay order")
            else:
                checked("Host starts a LAN match against its configured AI and advances more than 120 frames")
            evidence["passed"] = True
            return 0
    except Exception as error:
        cause = error
        while isinstance(cause, BaseExceptionGroup) and len(cause.exceptions) == 1:
            cause = cause.exceptions[0]
        evidence["error"] = cause.as_dict() if isinstance(cause, BridgeError) else str(cause)
        evidence["traceback"] = "".join(traceback.format_exception(error))
        for process in processes:
            if process.poll() is None:
                try:
                    evidence.setdefault("window_diagnostics", []).append({"pid": process.pid, "windows": process_window_text(process.pid)})
                except Exception as diagnostic_error:
                    evidence.setdefault("window_diagnostic_errors", []).append(str(diagnostic_error))
        (output / "failure.log").write_text(evidence["traceback"], encoding="utf-8")
        print(json.dumps(evidence["error"]), file=sys.stderr, flush=True)
        print(evidence["traceback"], file=sys.stderr, flush=True)
        return 1
    finally:
        cleanup_errors = []
        for process in processes:
            try:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
                evidence.setdefault("cleanup", []).append({"pid": process.pid, "exit_code": process.returncode})
            except Exception as cleanup_error:
                cleanup_errors.append({"pid": process.pid, "error": str(cleanup_error)})
        if cleanup_errors:
            evidence["cleanup_errors"] = cleanup_errors
            evidence["passed"] = False
        (output / "result.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
        print(f"{'LAN fixture cleanup incomplete' if cleanup_errors else 'Owned LAN fixtures stopped'}; evidence: {output}", flush=True)
        if cleanup_errors:
            return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--two-peers", action="store_true")
    parser.add_argument("--windowed", action="store_true", help="Use the real game menus (required for LAN callbacks) without requesting activation.")
    parser.add_argument("--minimized", action="store_true", help="Request minimized windowed startup (some renderers cannot initialize minimized).")
    parser.add_argument("--check-leave", action="store_true", help="Also verify temporary interface binding survives leaving and hosting again.")
    return asyncio.run(run(parser.parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
