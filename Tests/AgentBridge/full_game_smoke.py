"""Prove screen-free gameplay through official MCP tools in an isolated game.

No input automation, screenshots, injected buildings, or direct pipe calls.
Only the child process created here is stopped during cleanup.
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

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[2]


async def run(args) -> int:
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    suffix = "full_" + uuid.uuid4().hex[:12]
    env = os.environ.copy()
    env.update(GENERALS_MCP_PIPE=suffix, GENERALS_MCP_TEST_MAP="",
               GENERALS_MCP_TEST_SESSION="1", GENERALS_MCP_TEST_USER_DATA=str(output),
               GENERALS_MCP_TEST_TIMEOUT_SECONDS="600",
               PYTHONPATH=str(ROOT / "tools" / "generals-mcp" / "src"))
    evidence = {"exe": str(args.exe.resolve()), "pipe": suffix,
                "transport": "official MCP client -> stdio MCP server -> engine bridge",
                "screenshots": 0, "mouse_or_keyboard_inputs": 0, "injected_fixtures": 0,
                "checks": [], "tools_used": [], "passed": False}
    with (output / "stdout.log").open("w") as stdout, (output / "stderr.log").open("w") as stderr, \
            (output / "mcp-stderr.log").open("w") as mcp_log:
        process = subprocess.Popen([str(args.exe.resolve()), "-headless", "-quickstart", "-noaudio",
                                    "-setCwd", str(args.assets.resolve()), "-assetPath", str(args.assets.resolve())], cwd=args.exe.resolve().parent,
                                   env=env, stdout=stdout, stderr=stderr,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        evidence["pid"] = process.pid
        print(f"Isolated full-control test PID {process.pid}", flush=True)
        try:
            params = StdioServerParameters(command=sys.executable,
                    args=["-m", "generals_mcp", "--pipe", suffix, "--timeout", "8", "--control-mode", "stepped"], env=env)
            async with stdio_client(params, errlog=mcp_log) as (read, write):
                async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=12)) as client:
                    await client.initialize()
                    catalog = await client.list_tools()
                    evidence["mcp_tools"] = [tool.name for tool in catalog.tools]

                    async def call(name, /, **arguments):
                        if process.poll() is not None:
                            raise RuntimeError(f"Game exited: {process.returncode}")
                        if name not in evidence["tools_used"]:
                            evidence["tools_used"].append(name)
                        reply = await client.call_tool(name, arguments)
                        text = "\n".join(item.text for item in reply.content if item.type == "text")
                        if reply.isError:
                            raise RuntimeError(f"{name}: {text}")
                        envelope = reply.structuredContent or json.loads(text)
                        if not envelope.get("ok"):
                            raise RuntimeError(f"{name}: {envelope}")
                        return envelope["result"]

                    async def until(read_value, predicate, seconds=45):
                        deadline = time.monotonic() + seconds
                        last = None
                        while time.monotonic() < deadline:
                            try:
                                last = await read_value()
                                if predicate(last): return last
                            except RuntimeError as error:
                                last = str(error)
                                if process.poll() is not None: raise
                            await asyncio.sleep(0.15)
                        raise RuntimeError(f"Wait expired; last result: {last}")

                    async def session_done():
                        value = await until(lambda: call("get_session"),
                                            lambda s: s["status"] in ("completed", "failed"))
                        if value["status"] != "completed": raise RuntimeError(str(value))
                        return value

                    async def state(): return await call("get_game_state")

                    async def advance(frames):
                        result = await call("advance_game", frames=frames)
                        target = result["advance_until_frame"]
                        value = await until(state, lambda s: s.get("paused") and s.get("frame", 0) >= target,
                                            seconds=max(25, frames / 30 + 15))
                        if value["frame"] != target:
                            raise RuntimeError(f"Frame step overshot: {target} -> {value['frame']}")
                        return value

                    async def reject(name, expected=None, /, **arguments):
                        reply = await client.call_tool(name, arguments)
                        text = "\n".join(item.text for item in reply.content if item.type == "text")
                        if not reply.isError or (expected and expected not in text):
                            raise RuntimeError(f"Expected {expected or 'rejection'} for {name}: {text}")

                    def checked(description):
                        evidence["checks"].append(description)
                        print("PASS " + description, flush=True)
                        (output / "result.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")

                    capabilities = await until(lambda: call("get_capabilities"), lambda value: value.get("version"))
                    evidence["capabilities"] = capabilities
                    maps = await call("list_maps")
                    faction = next(f["name"] for f in maps["factions"] if f["starting_building"] == "Infa_ChinaCommandCenter")
                    opponent = next(f["name"] for f in maps["factions"] if f["name"] == "FactionGLA")
                    map_name = next(m["name"] for m in maps["maps"] if "alpine assault" in m["name"].lower())
                    await call("start_skirmish", map_name=map_name, faction=faction,
                               opponent_faction=opponent, difficulty="easy", money=50000)
                    await session_done()
                    initial = await state()
                    evidence["initial_state"] = initial
                    if not initial.get("paused") or initial.get("mode") != "skirmish":
                        raise RuntimeError(f"Expected paused skirmish: {initial}")
                    if len(initial["objects"]) != 2:
                        raise RuntimeError("Expected only the normal starting command center and dozer.")
                    checked("MCP starts a paused Contra skirmish with only normal starting objects")
                    await reject("start_skirmish", "match_active", map_name=map_name, faction=faction,
                                 opponent_faction=opponent, difficulty="easy", money=50000)

                    cc = next(o for o in initial["objects"] if o["template"] == "Infa_ChinaCommandCenter")
                    builder = next(o for o in initial["objects"] if o["template"] == "Infa_ChinaVehicleDozer")
                    center = cc["position"]
                    world = await call("get_world", offset=0, limit=256, relation="all")
                    own = await call("get_object", object_id=builder["id"])
                    players = await call("get_players")
                    terrain = await call("get_map", x=max(0., center["x"]-200), y=max(0., center["y"]-200),
                                         width=400., height=400., step=40.)
                    evidence["observation"] = {"world": world, "builder": own, "players": players, "map": terrain}
                    checked("MCP observes units, resources, players and sampled terrain without images")
                    await reject("get_object", object_id=2147483000)

                    async def construct(template, frames, interrupt=False):
                        actions = await call("get_available_actions", object_id=builder["id"])
                        action = next(c for c in actions["commands"] if c.get("template") == template)
                        if not action["available"]: raise RuntimeError(str(action))
                        overlap = await call("check_build", builder_id=builder["id"], template_name=template,
                                             x=center["x"], y=center["y"])
                        if overlap["can_build"]: raise RuntimeError("Overlapping command center accepted.")
                        locations = await call("find_build_sites", builder_id=builder["id"], template_name=template,
                                               x=center["x"], y=center["y"], radius=400., spacing=40., limit=8)
                        if not locations["sites"]: raise RuntimeError(str(locations))
                        position = locations["sites"][0]["position"]
                        await call("set_paused", paused=False)
                        await call("build", builder_id=builder["id"], template_name=template,
                                   x=position["x"], y=position["y"], angle=0.)
                        if interrupt:
                            await advance(150)
                            snapshot = await call("get_snapshot")
                            scaffold = next(o for o in snapshot["own"] if o["template"] == template)
                            worker = next(o for o in snapshot["own"] if o["id"] == builder["id"])
                            assert scaffold["under_construction"] and scaffold["builder_id"] == builder["id"]
                            assert worker["current_order"]["has_builder_task"] and not worker["current_order"]["idle"]
                            await call("set_paused", paused=False)
                            await call("unit_order", unit_ids=[builder["id"]], order="stop")
                            await advance(30)
                            stalled = (await call("get_object", object_id=scaffold["id"]))["object"]
                            assert stalled["under_construction"] and stalled["builder_id"] is None
                            await call("set_paused", paused=False)
                            await call("unit_order", unit_ids=[builder["id"]], order="repair", target_id=scaffold["id"])
                        current = await advance(frames)
                        built = next((o for o in current["objects"] if o["template"] == template), None)
                        for attempt in range(3):
                            if built and not built["under_construction"]:
                                break
                            current = await advance(600)
                            built = next((o for o in current["objects"] if o["template"] == template), None)
                        if not built or built["under_construction"]:
                            raise RuntimeError(f"Construction failed to complete: {built}")
                        if interrupt:
                            assert built["id"] == scaffold["id"]
                            checked("Pending construction protects the builder from idle classification; stopped construction resumes on the same scaffold")
                        checked(f"MCP finds a legal site and constructs {template} through normal orders")
                        return built

                    await construct("Infa_ChinaPowerPlant", 900, interrupt=True)
                    barracks = await construct("Infa_ChinaBarracks", 900)
                    powered = await state()
                    if powered["power"]["produced"] <= 0 or powered["money"] >= initial["money"]:
                        raise RuntimeError("Construction did not consume money and produce power.")
                    checked("Construction changes actual player money and power")

                    await call("set_paused", paused=False)
                    queued = await call("train_unit", producer_id=barracks["id"], template_name="Infa_ChinaInfantryMiniGunner")
                    trained = await advance(180)
                    soldier = next(o for o in trained["objects"] if o["template"] == "Infa_ChinaInfantryMiniGunner")
                    checked("MCP trains infantry in the newly constructed barracks")
                    start_position = soldier["position"]
                    await call("set_paused", paused=False)
                    await call("unit_order", unit_ids=[soldier["id"]], order="move",
                               x=start_position["x"] + 60., y=start_position["y"])
                    moved = await advance(150)
                    moved_soldier = next(o for o in moved["objects"] if o["id"] == soldier["id"])
                    if abs(moved_soldier["position"]["x"] - start_position["x"]) < 5:
                        raise RuntimeError("Movement order had no observable effect.")
                    await call("set_paused", paused=False)
                    await call("unit_order", unit_ids=[soldier["id"]], order="stop")
                    await call("attack_move", unit_ids=[soldier["id"]], x=start_position["x"], y=start_position["y"])
                    await advance(150)
                    checked("MCP moves, stops and attack-moves an owned unit; exact frame advance pauses automatically")

                    await call("set_paused", paused=False)
                    pending = await call("train_unit", producer_id=barracks["id"], template_name="Infa_ChinaInfantryMiniGunner")
                    await call("cancel_production", producer_id=barracks["id"], production_id=pending["production_id"])
                    cancelled = await advance(10)
                    if len([o for o in cancelled["objects"] if o["template"] == "Infa_ChinaInfantryMiniGunner"]) != 1:
                        raise RuntimeError("Cancelled infantry still appeared.")
                    checked("MCP cancels production before the queued unit is created")
                    promotions = await call("get_player_actions")
                    evidence["player_actions"] = promotions
                    science = next((s for s in promotions["sciences"] if s["available"] and not s["owned"]
                                    and "RedGuardTraining" in s["name"]), None)
                    if not science:
                        science = next((s for s in promotions["sciences"] if s["available"] and not s["owned"]), None)
                    if not science: raise RuntimeError("No initial purchasable science discovered.")
                    await call("set_paused", paused=False)
                    await call("purchase_science", science_name=science["name"])
                    await advance(5)
                    if (await call("get_player_actions"))["science_points"] != promotions["science_points"] - science["cost"]:
                        raise RuntimeError("Science purchase did not consume the expected points.")
                    checked("MCP discovers and purchases a general science")

                    barracks_actions = await call("get_available_actions", object_id=barracks["id"])
                    upgrade = next(c for c in barracks_actions["commands"] if c.get("upgrade") and c["available"])
                    await call("set_paused", paused=False)
                    await call("use_command", object_id=barracks["id"], command_button=upgrade["name"])
                    await advance(5)
                    await call("set_paused", paused=False)
                    await call("cancel_production", producer_id=barracks["id"], production_id="upgrade:" + upgrade["upgrade"])
                    await advance(5)
                    await call("set_paused", paused=False)
                    await call("use_command", object_id=barracks["id"], command_button=upgrade["name"])
                    await advance(1500)
                    researched = await call("get_players")
                    local = next(p for p in researched["players"] if p["id"] == researched["local_player_id"])
                    upgraded_object = (await call("get_object", object_id=barracks["id"]))["object"]
                    if upgrade["upgrade"] not in local["upgrades"] + upgraded_object["upgrades"]:
                        raise RuntimeError("Research did not complete through the loaded command button.")
                    checked("MCP researches and cancels upgrades through loaded command definitions")
                    evidence["events"] = await call("get_events", since=0, limit=256)
                    checked("MCP reports general actions and observed game events")

                    save_name = "mcp_full_" + uuid.uuid4().hex[:10]
                    saved_state = await state()
                    await call("save_game", name=save_name)
                    await session_done()
                    saves = await call("list_saves")
                    if not any(s["name"] == save_name + ".sav" for s in saves["saves"]):
                        raise RuntimeError("Saved file missing from MCP save listing.")
                    await reject("save_game", "save_exists", name=save_name)
                    step = await call("advance_game", frames=30)
                    await reject("save_game", "game_advancing", name="must_not_cancel_frame_advance")
                    await until(state, lambda s: s.get("paused") and s.get("frame", 0) >= step["advance_until_frame"])
                    await call("load_game", name=save_name)
                    await session_done()
                    loaded = await state()
                    if not loaded["paused"] or loaded["money"] != saved_state["money"]:
                        raise RuntimeError(f"Loaded state differs: {loaded}")
                    after_load_events = await call("get_events", since=0, limit=256)
                    starts = [e for e in after_load_events["events"] if e["type"] == "match_started"]
                    if len(starts) < 2:
                        raise RuntimeError("Loading a save did not start a new observation epoch.")
                    checked("MCP saves, lists, prevents overwrite, and reloads the match paused")
                    await call("end_match")
                    await session_done()
                    if (await state())["status"] != "not_in_match":
                        raise RuntimeError("Match did not end.")
                    checked("MCP ends the isolated match without desktop interaction")
                    evidence["passed"] = True
        except BaseException as error:
            evidence["error"] = repr(error)
            print("FAIL", repr(error), flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            evidence["game_exit_code"] = process.returncode
            evidence["cleanup"] = "stopped only this test's isolated child process"
            (output / "result.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    raise SystemExit(asyncio.run(run(parser.parse_args())))
