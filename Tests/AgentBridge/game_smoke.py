"""Exercise the full MCP tool chain against an isolated headless Contra skirmish.

This starts only the supplied test executable, keeps its own userdata and logs,
and never attaches to or controls an existing game process.
"""
from __future__ import annotations

import argparse
import asyncio
from datetime import timedelta
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "generals-mcp" / "src"))
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from generals_mcp.bridge import BridgeError


class MCPGame:
    """Every observation and order goes through the official MCP client/server."""

    def __init__(self, session: ClientSession):
        self.session = session

    async def call(self, method: str, *values) -> dict:
        if method == "get_state":
            tool, arguments = "get_game_state", {}
        elif method == "get_actions":
            tool, arguments = "get_available_actions", {"object_id": values[0]}
        elif method == "train_unit":
            tool, arguments = "train_unit", {"producer_id": values[0], "template_name": values[1]}
        elif method == "attack_move":
            tool, arguments = "attack_move", {"unit_ids": values[0], "x": values[1], "y": values[2]}
        else:
            raise ValueError("Unknown smoke-test method: " + method)
        writing = method in ("train_unit", "attack_move")
        try:
            reply = await self.session.call_tool(tool, arguments)
        except Exception as exc:
            raise BridgeError("mcp_transport_error", str(exc), delivery_unknown=writing) from exc
        text = "\n".join(item.text for item in reply.content if item.type == "text")
        if reply.isError:
            # FastMCP prefixes tool errors; extract the bridge's JSON error object.
            details = {}
            try:
                details, _ = json.JSONDecoder().raw_decode(text[text.index("{"):])
            except (ValueError, TypeError):
                pass
            raise BridgeError(details.get("code", "mcp_tool_error"), details.get("message", text),
                              delivery_unknown=details.get("delivery_unknown", writing))
        envelope = reply.structuredContent
        if envelope is None:
            try:
                envelope = json.loads(text)
            except ValueError as exc:
                raise BridgeError("invalid_mcp_response", "MCP tool did not return a JSON envelope.", delivery_unknown=writing) from exc
        if (not isinstance(envelope, dict) or envelope.get("protocol") != 1
                or envelope.get("ok") is not True or not isinstance(envelope.get("result"), dict)):
            raise BridgeError("invalid_mcp_response", "MCP tool returned an invalid bridge envelope.", delivery_unknown=writing)
        return envelope


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--observe-only", action="store_true")
    parser.add_argument("--clean-exit", action="store_true", help="Wait for the isolated game's 30-second clean shutdown.")
    return asyncio.run(run(parser.parse_args()))


async def run(args) -> int:
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    suffix = "smoke_" + uuid.uuid4().hex[:12]
    env = os.environ.copy()
    env.update(
        GENERALS_MCP_PIPE=suffix,
        GENERALS_MCP_TEST_MAP=r"Maps\Alpine Assault\Alpine Assault.map",
        GENERALS_MCP_TEST_USER_DATA=str(output),
        PYTHONPATH=str(REPO / "tools" / "generals-mcp" / "src"),
    )
    evidence: dict = {"pipe": suffix, "exe": str(args.exe.resolve()),
                      "transport": "official MCP stdio client -> MCP sidecar -> native pipe -> game adapter",
                      "checks": []}
    if args.clean_exit:
        env["GENERALS_MCP_TEST_TIMEOUT_SECONDS"] = "30"
    with (output / "stdout.log").open("w") as stdout, (output / "stderr.log").open("w") as stderr, \
            (output / "mcp-stderr.log").open("w") as mcp_stderr:
        process = subprocess.Popen(
            [str(args.exe.resolve()), "-headless", "-quickstart", "-noaudio", "-setCwd", str(args.assets.resolve()), "-assetPath", str(args.assets.resolve())],
            cwd=args.exe.resolve().parent, env=env, stdout=stdout, stderr=stderr,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        print(f"Started isolated test PID {process.pid}", flush=True)

        def alive() -> None:
            if process.poll() is not None:
                raise RuntimeError(f"Test game exited: {process.returncode} (0x{process.returncode & 0xffffffff:08x})")

        async def observe() -> dict:
            alive()
            return (await bridge.call("get_state"))["result"]

        async def wait_for(predicate, seconds: float = 35) -> dict:
            deadline = time.monotonic() + seconds
            last: object = None
            while time.monotonic() < deadline:
                alive()
                try:
                    last = await observe()
                    if predicate(last):
                        return last
                except BridgeError as exc:
                    last = exc.as_dict()
                await asyncio.sleep(0.2)
            raise RuntimeError(f"Observation deadline exceeded; last state: {last}")

        async def must_reject(expected: str, method: str, *values) -> None:
            try:
                result = await bridge.call(method, *values)
            except BridgeError as exc:
                if exc.code != expected or exc.delivery_unknown:
                    raise RuntimeError(f"Expected certain {expected} rejection; got {exc.as_dict()}") from exc
                evidence.setdefault("rejected_orders", []).append({"method": method, "arguments": values, **exc.as_dict()})
                return
            raise RuntimeError(f"Invalid request unexpectedly succeeded: {method} {values}; {result}")

        async def exercise() -> None:
            state = await wait_for(lambda s: s.get("commands_enabled") and any(
                "Barracks" in obj["template"] and not obj["under_construction"]
                for obj in s.get("objects", [])
            ))
            evidence["initial_state"] = state
            evidence["checks"].append("live skirmish state with enabled commands")
            if not args.observe_only:
                candidates = []
                evidence["available_actions"] = []
                for obj in state["objects"]:
                    if obj["building"] and not obj["under_construction"]:
                        actions = (await bridge.call("get_actions", obj["id"]))["result"]
                        evidence["available_actions"].append(actions)
                        for unit in actions.get("trainable_units", []):
                            if unit.get("available"):
                                candidates.append((obj, unit))
                # The test fixture supplies a barracks; train its basic infantry.
                choice = next((
                    (p, u) for p, u in candidates
                    if "Barracks" in p["template"]
                    and ("Ranger" in u["template"] or "Cyborg" in u["template"])
                ), None)
                if choice is None:
                    raise RuntimeError(f"No available basic infantry in fixture production: {evidence['available_actions']}")
                producer, unit = choice
                existing = {obj["id"] for obj in state["objects"]}
                invalid_id = 2147483646
                if invalid_id in existing:
                    raise RuntimeError("Reserved invalid test ID unexpectedly exists")
                await must_reject("invalid_object", "get_actions", invalid_id)
                await must_reject("invalid_object", "train_unit", invalid_id, unit["template"])
                await must_reject("unsupported_unit", "train_unit", producer["id"], "MCP_Nonexistent_Unit")
                evidence["checks"].append("invalid object IDs and unsupported training template rejected through MCP")
                queued = await bridge.call("train_unit", producer["id"], unit["template"])
                evidence["training_acknowledgement"] = queued
                state = await wait_for(lambda s: any(
                    obj["id"] not in existing and obj["template"] == unit["template"]
                    for obj in s.get("objects", [])
                ))
                trained = next(obj for obj in state["objects"]
                               if obj["id"] not in existing and obj["template"] == unit["template"])
                evidence["trained_unit"] = trained
                evidence["checks"].append("normal production command creates the requested unit")
                if not trained["can_attack_move"]:
                    raise RuntimeError("Trained test unit cannot attack-move")
                bounds = state["map_bounds"]
                start = trained["position"]
                x = min(bounds["max"]["x"] - 100, max(bounds["min"]["x"] + 100, start["x"] + 180))
                y = min(bounds["max"]["y"] - 100, max(bounds["min"]["y"] + 100, start["y"] + 100))
                await must_reject("invalid_unit", "attack_move", [invalid_id], x, y)
                await must_reject("outside_map", "attack_move", [trained["id"]], bounds["max"]["x"] + 10000, y)
                evidence["checks"].append("invalid combat unit and out-of-map destination rejected through MCP")
                evidence["attack_move_acknowledgement"] = await bridge.call("attack_move", [trained["id"]], x, y)
                state = await wait_for(lambda s: any(
                    obj["id"] == trained["id"] and (
                        (obj["position"]["x"] - start["x"]) ** 2
                        + (obj["position"]["y"] - start["y"]) ** 2 > 400
                    ) for obj in s.get("objects", [])
                ), seconds=20)
                evidence["moved_unit"] = next(obj for obj in state["objects"] if obj["id"] == trained["id"])
                evidence["checks"].append("attack-move command changes the trained unit position")

        try:
            parameters = StdioServerParameters(
                command=sys.executable,
                args=["-m", "generals_mcp", "--pipe", suffix, "--timeout", "1", "--control-mode", "stepped"],
                env=env, cwd=REPO,
            )
            async with stdio_client(parameters, errlog=mcp_stderr) as (read, write):
                async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=10)) as session:
                    initialized = await session.initialize()
                    evidence["mcp_server"] = initialized.serverInfo.model_dump()
                    evidence["mcp_protocol_version"] = initialized.protocolVersion
                    tools = await session.list_tools()
                    evidence["mcp_tools"] = [tool.name for tool in tools.tools]
                    expected_tools = {"get_game_state", "get_available_actions", "train_unit", "attack_move"}
                    if not expected_tools.issubset(set(evidence["mcp_tools"])):
                        raise RuntimeError(f"Unexpected MCP tool catalog: {evidence['mcp_tools']}")
                    bridge = MCPGame(session)
                    evidence["checks"].append("official MCP initialization and original tool compatibility")
                    await exercise()
            if args.clean_exit:
                exit_code = await asyncio.to_thread(process.wait, timeout=40)
                evidence["game_exit_code"] = exit_code
                if exit_code != 0:
                    raise RuntimeError(f"Game shutdown failed: {exit_code} (0x{exit_code & 0xffffffff:08x})")
                evidence["checks"].append("isolated game exits cleanly after test deadline")
            evidence["passed"] = True
            print(json.dumps({"passed": True, "checks": evidence["checks"]}), flush=True)
            return 0
        except Exception as exc:
            evidence["passed"] = False
            evidence["error"] = str(exc)
            print(str(exc), file=sys.stderr, flush=True)
            return 1
        finally:
            (output / "result.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
            if process.poll() is None:
                # This is the child test process created above, never the user's game.
                process.terminate()
            process.wait(timeout=10)
            print(f"Test process finished; logs: {output}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
