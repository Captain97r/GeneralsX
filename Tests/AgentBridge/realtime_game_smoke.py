"""Verify real-time gameplay and replay tools through a persistent official MCP client.

Uses the isolated-process/session pattern from full_game_smoke.py. No screenshots,
desktop input, fixtures, normal userdata, or existing game processes are accessed.
"""
from __future__ import annotations

import argparse
import asyncio
from datetime import timedelta
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from full_game_smoke import ROOT


async def run(args) -> int:
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = output / "bin"
    userdata = output / "userdata"
    binary.mkdir()
    userdata.mkdir()
    executable = binary / "generalszh-realtime-test.exe"
    shutil.copy2(args.exe.resolve(), executable)
    for dependency in args.exe.resolve().parent.glob("*.dll"):
        if dependency.name.lower() not in {"binkw32.dll", "mss32.dll"}:
            shutil.copy2(dependency, binary / dependency.name)
    for name in ("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll"):
        source = args.exe.resolve().parent / name
        if not source.exists():
            source = args.assets.resolve() / name
        if source.exists():
            shutil.copy2(source, binary / name)
    suffix = "realtime_" + uuid.uuid4().hex[:12]
    env = os.environ.copy()
    env.update(GENERALS_MCP_PIPE=suffix, GENERALS_MCP_TEST_MAP="",
               GENERALS_MCP_TEST_SESSION="1", GENERALS_MCP_TEST_USER_DATA=str(userdata),
               GENERALS_MCP_TEST_TIMEOUT_SECONDS="240",
               PYTHONPATH=str(ROOT / "tools" / "generals-mcp" / "src"))
    evidence = {"source_exe": str(args.exe.resolve()), "exe": str(executable),
                "userdata": str(userdata), "pipe": suffix,
                "transport": "persistent official MCP stdio client -> sidecar -> native bridge",
                "screenshots": 0, "desktop_inputs": 0, "fixtures": 0,
                "checks": [], "passed": False}
    latencies = []

    def save():
        (output / "result.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")

    def checked(message):
        evidence["checks"].append(message)
        save()
        print("PASS " + message, flush=True)

    with (output / "stdout.log").open("w") as stdout, (output / "stderr.log").open("w") as stderr, \
            (output / "mcp-stderr.log").open("w") as mcp_log, (output / "calls.jsonl").open("w") as calls:
        child = subprocess.Popen([str(executable), "-headless", "-quickstart", "-noaudio",
                                  "-setCwd", str(args.assets.resolve()), "-assetPath", str(args.assets.resolve())], cwd=binary, env=env,
                                 stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
        evidence["pid"] = child.pid
        save()
        print(f"Isolated real-time test PID {child.pid}; evidence {output}", flush=True)
        try:
            params = StdioServerParameters(command=sys.executable,
                args=["-m", "generals_mcp", "--pipe", suffix, "--timeout", "8", "--control-mode", "realtime"], env=env)
            async with stdio_client(params, errlog=mcp_log) as (read, write):
                async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=15)) as client:
                    await client.initialize()
                    evidence["mcp_tools"] = [tool.name for tool in (await client.list_tools()).tools]

                    async def call(tool, /, expected_error=None, **arguments):
                        if child.poll() is not None:
                            raise RuntimeError(f"Isolated game exited: {child.returncode}")
                        started = time.perf_counter()
                        reply = await client.call_tool(tool, arguments)
                        elapsed = (time.perf_counter() - started) * 1000
                        text = "\n".join(c.text for c in reply.content if c.type == "text")
                        envelope = reply.structuredContent
                        if envelope is None and not reply.isError:
                            envelope = json.loads(text)
                        calls.write(json.dumps({"tool": tool, "arguments": arguments, "wall_ms": elapsed,
                                                "is_error": bool(reply.isError), "response": envelope or text}) + "\n")
                        calls.flush()
                        latencies.append({"tool": tool, "ms": elapsed})
                        if expected_error:
                            if not reply.isError or expected_error not in text:
                                raise RuntimeError(f"Expected {expected_error} from {tool}: {text}")
                            return text
                        if reply.isError or not envelope or not envelope.get("ok"):
                            raise RuntimeError(f"{tool}: {envelope or text}")
                        return envelope["result"]

                    async def until(reader, predicate, timeout=30, tolerate=False):
                        deadline = time.monotonic() + timeout
                        last = None
                        while time.monotonic() < deadline:
                            try:
                                last = await reader()
                                if predicate(last):
                                    return last
                            except RuntimeError as error:
                                if not tolerate or child.poll() is not None:
                                    raise
                                last = str(error)
                            await asyncio.sleep(.15)
                        raise RuntimeError(f"Timed out: {last}")

                    async def session_done():
                        value = await until(lambda: call("get_session"), lambda s: s["status"] in ("completed", "failed"))
                        if value["status"] != "completed":
                            raise RuntimeError(f"Session operation failed: {value}")
                        return value

                    async def snapshot():
                        value = await call("get_snapshot", limit=128)
                        if value.get("status") == "in_match" and value.get("paused"):
                            raise RuntimeError("Real-time test observed an unexpected pause")
                        return value

                    evidence["capabilities"] = await until(lambda: call("get_capabilities"), lambda s: s.get("version"), tolerate=True)
                    assert evidence["capabilities"]["engine"] == "GeneralsX"
                    maps = await call("list_maps")
                    faction = next(f["name"] for f in maps["factions"] if f["starting_building"] == "Lazr_AmericaCommandCenter")
                    opponent = next(f["name"] for f in maps["factions"] if f["name"] == "FactionGLA")
                    map_name = next(m["name"] for m in maps["maps"] if "alpine assault" in m["name"].lower())
                    await call("start_skirmish", map_name=map_name, faction=faction,
                               opponent_faction=opponent, difficulty="easy", money=10000)
                    await session_done()
                    initial = await snapshot()
                    evidence["initial_snapshot"] = initial
                    assert initial["status"] == "in_match" and not initial["paused"]
                    cc = next(o for o in initial["own"] if o["template"] == "Lazr_AmericaCommandCenter")
                    dozer = next(o for o in initial["own"] if o["template"] == "Lazr_AmericaVehicleDozer")
                    pos = dozer["position"]
                    checked("Laser skirmish starts unpaused through MCP with private userdata")

                    frame_start, wall_start = initial["frame"], time.perf_counter()
                    watch = await call("watch_game", since=initial["event_cursor"], timeout_seconds=2., event_types=["match_ended"])
                    later = await snapshot()
                    wall_seconds = time.perf_counter() - wall_start
                    frames = later["frame"] - frame_start
                    assert frames >= 20, (frames, wall_seconds)
                    evidence["realtime_clock"] = {"frames": frames, "wall_seconds": wall_seconds,
                                                   "frames_per_wall_second": frames / wall_seconds, "watch": watch}
                    await call("set_paused", paused=True, expected_error="realtime_mode")
                    await call("advance_game", frames=1, expected_error="realtime_mode")
                    checked("watch_game advances real frames; real-time controller rejects pause and stepping")

                    before = (await call("get_object", object_id=dozer["id"]))["object"]
                    eligibility = await call("check_order", unit_ids=[dozer["id"], cc["id"]], order="move",
                                             x=float(pos["x"] + 35), y=float(pos["y"]))
                    assert [u["eligible"] for u in eligibility["units"]] == [True, False]
                    after = (await call("get_object", object_id=dozer["id"]))["object"]
                    assert before.get("current_order") == after.get("current_order")
                    assert math.dist([before["position"][k] for k in ("x", "y")],
                                     [after["position"][k] for k in ("x", "y")]) < .1
                    profile = await call("get_unit_profile", object_id=dozer["id"])
                    assert "Lazr_AmericaVehicleDozer" in json.dumps(profile)
                    attack_check = await call("check_order", unit_ids=[dozer["id"]],
                                              order="force_attack", target_id=cc["id"])
                    assert "engine_result" in attack_check["units"][0]["attack"]
                    evidence["check_order"] = eligibility
                    evidence["attack_check"] = attack_check
                    evidence["unit_profile"] = profile
                    checked("Mixed check_order reports unit-specific eligibility without moving; own profile resolves")

                    # Spawn orientation varies. Continue away from the command
                    # center through the starting dozer's already traversable cell.
                    dx, dy = pos["x"] - cc["position"]["x"], pos["y"] - cc["position"]["y"]
                    distance = math.hypot(dx, dy)
                    assert distance > 1
                    dx, dy = dx / distance, dy / distance
                    rally = {"x": float(pos["x"] + dx * 30), "y": float(pos["y"] + dy * 30)}
                    fresh = await snapshot()
                    batch = await call("batch_orders", commands=[
                        {"tool": "unit_order", "arguments": {"unit_ids": [dozer["id"]], "order": "guard_no_pursuit"}},
                        {"tool": "set_rally_point", "arguments": {"object_id": cc["id"], **rally}}],
                        expected_frame=fresh["frame"], max_age_frames=90)
                    assert len(batch["results"]) == 2 and all(r["ok"] for r in batch["results"]), batch
                    assert len({r["result"]["frame"] for r in batch["results"]}) == 1
                    rally_object = await until(lambda: call("get_object", object_id=cc["id"]),
                        lambda s: s["object"].get("rally_point") and abs(s["object"]["rally_point"]["x"] - rally["x"]) < .1)
                    await call("batch_orders", commands=[{"tool": "unit_order", "arguments": {
                        "unit_ids": [dozer["id"]], "order": "stop"}}], expected_frame=1, max_age_frames=1,
                        expected_error="stale_snapshot")
                    evidence["batch"] = batch
                    evidence["rally"] = rally_object
                    checked("Native batch dispatch shares one frame; rally applies; stale snapshot batch is rejected")

                    actions = await call("get_player_actions")
                    science = next(s for s in actions["sciences"] if s["name"] == "SCIENCE_SpyDrone")
                    assert science["available"] and not science["owned"]
                    assert not any(s["name"] == "SCIENCE_CenturionTank" for s in actions["sciences"])
                    await call("purchase_science", science_name="SCIENCE_CenturionTank", expected_error="science_not_in_faction_menu")
                    await call("purchase_science", science_name=science["name"])
                    promoted = await until(lambda: call("get_player_actions"),
                                           lambda p: science["name"] in p["owned_sciences"])
                    spy = next(s for s in promoted["shortcut_commands"] if s["command"]["name"] == "Command_SpyDroneFromShortcut")
                    assert spy["source_id"] == cc["id"] and spy["selector"] == "Lazr_SelectSpecialPowersFromShortcut", spy
                    assert promoted["science_points"] == actions["science_points"] - science["cost"]
                    evidence["player_actions"] = promoted
                    checked("Faction science rejects foreign entry; purchase appears in full owned inventory and nested power menu")
                    assert spy["command"]["available"], spy
                    await call("use_command", object_id=spy["source_id"], command_button=spy["command"]["name"],
                               x=float(pos["x"] + 100), y=float(pos["y"] + 100))
                    used = await until(lambda: call("get_player_actions"), lambda p: any(
                        s["command"]["name"] == spy["command"]["name"] and not s["command"]["available"]
                        for s in p["shortcut_commands"]))
                    evidence["power_after_use"] = used
                    checked("Discovered nested SpyDrone power executes from the command center and starts cooldown")

                    cc_actions = await call("get_available_actions", object_id=cc["id"])
                    unit = next(t for t in cc_actions["trainable_units"] if t["template"] == "Lazr_AmericaVehicleDozer")
                    assert unit["available"]
                    queued = await call("train_unit", producer_id=cc["id"], template_name=unit["template"])
                    production_id = queued["production_id"]
                    target = {"x": float(pos["x"] + dx * 150), "y": float(pos["y"] + dy * 150)}
                    await call("configure_reactions", rules=[{"rule_id": "new_dozer_move", "trigger": "unit_produced",
                        "producer_id": cc["id"], "production_id": production_id, "order": "move", **target,
                        "expires_seconds": 80., "max_fires": 1}])
                    cursor = promoted.get("event_cursor", 0)
                    produced = None
                    deadline = time.monotonic() + 70
                    while time.monotonic() < deadline and produced is None:
                        events = await call("watch_game", since=cursor, timeout_seconds=3., event_types=["unit_produced"])
                        cursor = events["next_since"]
                        produced = next((e for e in events["events"] if e.get("producer_id") == cc["id"] and
                                         e.get("production_id") == production_id), None)
                    assert produced is not None, "No exact-ID produced event"
                    assert produced["object_id"] != dozer["id"]
                    new_id = produced["object_id"]
                    reaction = await until(lambda: call("get_realtime_status"), lambda s: any(
                        a.get("rule_id") == "new_dozer_move" and a.get("unit_id") == new_id and "result" in a
                        for a in s["recent_activity"]), timeout=10)
                    moved = await until(lambda: call("get_object", object_id=new_id), lambda s:
                        math.dist([s["object"]["position"][k] for k in ("x", "y")], [target["x"], target["y"]]) < 20, timeout=25)
                    await call("cancel_reactions")
                    final_snapshot = await snapshot()
                    evidence["production_reaction"] = {"queued": queued, "event": produced, "status": reaction, "moved": moved}
                    evidence["final_snapshot"] = final_snapshot
                    checked("Exact producer/production ID event identifies new dozer; bounded reaction moves that unit in real time")

                    await call("end_match")
                    await session_done()
                    replay = await until(lambda: call("get_replay_status"), lambda r: r["can_save"])
                    assert replay["last_replay"]["finalized"] and replay["last_replay"]["frame_count"] >= final_snapshot["frame"]
                    name = "realtime_smoke_" + uuid.uuid4().hex[:10]
                    await call("save_replay", name=name)
                    saved = await session_done()
                    listed = await call("list_replays")
                    recorded = next(r for r in listed["replays"] if r["name"] == name + ".rep")
                    assert recorded["finalized"] and recorded["frame_count"] == replay["last_replay"]["frame_count"]
                    await call("save_replay", name=name, expected_error="replay_exists")
                    await call("play_replay", name=name)
                    await session_done()
                    playback = await call("get_replay_status")
                    assert playback["playing"] and playback["playback_frame_count"] == recorded["frame_count"]
                    replay_start = await snapshot()
                    await asyncio.sleep(.6)
                    replay_later = await snapshot()
                    assert replay_later["frame"] > replay_start["frame"] and not replay_later["paused"]
                    await call("unit_order", unit_ids=[dozer["id"]], order="stop", expected_error="replay_read_only")
                    await call("end_match")
                    await session_done()
                    stopped = await call("get_replay_status")
                    assert not stopped["playing"]
                    evidence["replay"] = {"finalized": replay, "saved_session": saved, "listed": listed,
                                          "playback": playback, "stopped": stopped}
                    evidence["realtime_status"] = await call("get_realtime_status")
                    checked("End finalizes replay; save/list/play preserve frame count; playback advances read-only and ends cleanly")
                    evidence["passed"] = True
        except BaseException as error:
            evidence["error"] = repr(error)
            print("FAIL " + repr(error)[:1800], flush=True)
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
            evidence["exit_code"] = child.returncode
            evidence["cleanup"] = "stopped only the child process created by this test"
            measured = sorted(v["ms"] for v in latencies if v["tool"] != "watch_game")
            evidence["mcp_latency_ms"] = {"samples": len(measured), "p50": statistics.median(measured) if measured else None,
                "p95": measured[int((len(measured) - 1) * .95)] if measured else None, "max": max(measured) if measured else None}
            save()
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    raise SystemExit(asyncio.run(run(parser.parse_args())))
