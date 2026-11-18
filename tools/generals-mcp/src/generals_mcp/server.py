"""TheSuperHackers @feature Expose the local skirmish bridge as typed MCP tools."""

from __future__ import annotations

import argparse
import json
import logging
import os
import uuid
from contextlib import asynccontextmanager
from typing import Annotated, Any, Literal

from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError
from mcp.types import ToolAnnotations
from pydantic import BaseModel, ConfigDict, Field

from .bridge import BridgeError, GameBridge
from .realtime import Order, ReactionRule, RealtimeController

ObjectID = Annotated[int, Field(strict=True, gt=0, le=2147483647, description="Object ID from the current game state.")]
Coordinate = Annotated[float, Field(strict=True, allow_inf_nan=False, description="World-space map coordinate, not screen pixels.")]
TemplateName = Annotated[str, Field(pattern=r"^[A-Za-z0-9_]{1,128}$", description="Exact template_name from get_available_actions.")]
Identifier = Annotated[str, Field(pattern=r"^[A-Za-z0-9_]{1,128}$", description="Exact identifier returned by the game.")]
TargetID = Annotated[int, Field(strict=True, ge=0, le=2147483647, description="Target object ID, or 0 when there is no object target.")]
ProductionID = (Annotated[int, Field(strict=True, ge=0, le=2147483647)] |
                Annotated[str, Field(pattern=r"^upgrade:[A-Za-z0-9_]{1,128}$")])
Flag = Annotated[bool, Field(strict=True)]
UnitIDs = Annotated[list[ObjectID], Field(min_length=1, max_length=64, description="Unique IDs of owned units.")]
Limit = Annotated[int, Field(strict=True, ge=1, le=256)]
FileName = Annotated[str, Field(pattern=r"^[A-Za-z0-9_-]{1,80}(?:\.sav)?$", description="Simple save name, optionally followed by .sav; no directories.")]
MapName = Annotated[str, Field(min_length=1, max_length=240, pattern=r"^[\x20-\x7e]+$", description="Exact map identifier returned by list_maps.")]
HostIP = Annotated[str, Field(min_length=7, max_length=15, pattern=r"^[0-9]{1,3}(?:\.[0-9]{1,3}){3}$", description="Dotted IPv4 host address; normally use get_lobby discovery.")]
LobbyFaction = Annotated[str, Field(pattern=r"^(?:[A-Za-z0-9_]{1,128})?$", description="Loaded playable faction identifier, random, observer, or empty to preserve the current faction.")]


class BatchCommand(BaseModel):
    model_config = ConfigDict(extra="forbid")
    tool: Literal["unit_order", "use_command", "train_unit", "cancel_production", "build", "build_line", "purchase_science", "set_rally_point", "attack_move"]
    arguments: dict[str, Any]


COMMAND_FIELDS = {
    "unit_order": (("unit_ids", "order"), {"target_id": 0, "x": None, "y": None, "queued": False}),
    "use_command": (("object_id", "command_button"), {"target_id": 0, "x": None, "y": None, "angle": 0}),
    "train_unit": (("producer_id", "template_name"), {}),
    "cancel_production": (("producer_id", "production_id"), {}),
    "build": (("builder_id", "template_name", "x", "y"), {"angle": 0}),
    "build_line": (("builder_id", "template_name", "x1", "y1", "x2", "y2"), {"angle": 0}),
    "purchase_science": (("science_name",), {}),
    "set_rally_point": (("object_id", "x", "y"), {}),
    "attack_move": (("unit_ids", "x", "y"), {}),
}


def encode_batch(commands: list[BatchCommand]) -> list[dict]:
    entries = []
    for command in commands:
        required, defaults = COMMAND_FIELDS[command.tool]
        keys = (*required, *defaults)
        if set(command.arguments) - set(keys) or set(required) - set(command.arguments):
            raise ValueError(f"Invalid arguments for {command.tool}; requires {required}, optional {tuple(defaults)}.")
        values = dict(defaults, **command.arguments)
        entries.append({"method": command.tool, "arguments": [values[key] for key in keys]})
    return entries


def create_server(bridge: GameBridge, control_mode: str = "realtime") -> FastMCP:
    runtime = RealtimeController(bridge, control_mode)

    @asynccontextmanager
    async def lifespan(server):
        await runtime.start()
        try:
            yield {"runtime": runtime}
        finally:
            await runtime.close()

    server = FastMCP(
        "Generals Zero Hour",
        instructions=("Controls an opt-in local Zero Hour build through structured game data; no screenshots are required. "
                      "Query get_capabilities, state, and available actions first. Normal gameplay orders use the engine's "
                      "command pipeline; pause, stepping, loading, and starting games have single-player restrictions. "
                      "Headless mode supports skirmishes; LAN menu tools require the normal/windowed engine. "
                      "Poll get_lobby to observe asynchronous LAN transitions before issuing the next lobby action. "
                      "Ownership, visibility, and action prerequisites are checked by the engine. "
                      "An accepted order is queued, not proof it completed. Refresh state/events to observe its effects. "
                      "If delivery_unknown is true, observe state before considering another order; do not automatically repeat it. "
                      "Default realtime mode forbids pauses and frame stepping. Use get_snapshot, batch_orders, watch_game, "
                      "and bounded configure_reactions for continuous play. Manual orders take priority over reactions. "
                      "Keep one persistent MCP connection: reaction rules live only for that server lifetime."),
        log_level="WARNING",
        lifespan=lifespan,
    )

    async def invoke(method: str, *args: Any) -> dict[str, Any]:
        try:
            # Keep the stdio event loop responsive. Cancellation waits for bounded I/O cleanup.
            return await runtime.call(method, *args)
        except BridgeError as exc:
            raise ToolError(json.dumps(exc.as_dict(), ensure_ascii=True)) from exc
        except ValueError as exc:
            raise ToolError(str(exc)) from exc

    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, idempotentHint=True, openWorldHint=False)
    write = ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False)

    def local_result(value: dict) -> dict:
        return {"protocol": 1, "request_id": uuid.uuid4().hex, "ok": True, "result": value}

    @server.tool(annotations=read)
    async def get_snapshot(offset: Annotated[int, Field(strict=True, ge=0, le=100000)] = 0, limit: Limit = 128) -> dict[str, Any]:
        """Read own objects, visible world page, players, result and event cursor from one simulation frame. Check truncation and pagination."""
        return await invoke("get_snapshot", offset, limit)

    @server.tool(annotations=read)
    async def get_unit_profile(object_id: ObjectID) -> dict[str, Any]:
        """Inspect combat rules and action diagnostics for an observable object; enemy private state stays hidden."""
        return await invoke("get_unit_profile", object_id)

    @server.tool(annotations=read)
    async def check_order(unit_ids: UnitIDs, order: Order, target_id: TargetID = 0, x: Coordinate | None = None, y: Coordinate | None = None, queued: Flag = False) -> dict[str, Any]:
        """Validate a proposed order without executing it; returns per-unit eligibility and attack diagnostics at the observed frame."""
        return await invoke("check_order", unit_ids, order, target_id, x, y, queued)

    @server.tool(annotations=write)
    async def set_rally_point(object_id: ObjectID, x: Coordinate, y: Coordinate) -> dict[str, Any]:
        """Set the normal rally point of an owned producer, even if its menu has no rally button."""
        return await invoke("set_rally_point", object_id, x, y)

    @server.tool(annotations=write)
    async def batch_orders(commands: Annotated[list[BatchCommand], Field(min_length=1, max_length=32)], expected_frame: Annotated[int, Field(strict=True, ge=-1, le=2147483647)] = -1, max_age_frames: Annotated[int, Field(strict=True, ge=1, le=1800)] = 90) -> dict[str, Any]:
        """Submit bounded gameplay orders together at one frame. Reject stale plans using expected_frame; -1 disables freshness check. Each entry reports queued/rejected independently; execution is not atomic. Never replay the whole batch after partial success."""
        try:
            return await invoke("batch_orders", encode_batch(commands), expected_frame, max_age_frames)
        except ValueError as error:
            raise ToolError(str(error)) from error

    @server.tool(annotations=read)
    async def watch_game(since: Annotated[int, Field(strict=True, ge=0, le=9007199254740991)] = 0, timeout_seconds: Annotated[float, Field(strict=True, ge=0, le=10, allow_inf_nan=False)] = 5., event_types: Annotated[list[str], Field(max_length=32)] = []) -> dict[str, Any]:
        """Wait for observed events or a result without pausing or blocking other MCP calls. Continue with returned next_since; history_lost requires a fresh snapshot."""
        try:
            return local_result(await runtime.watch(since, timeout_seconds, event_types))
        except BridgeError as error:
            raise ToolError(json.dumps(error.as_dict())) from error

    @server.tool(annotations=write)
    async def configure_reactions(rules: Annotated[list[ReactionRule], Field(max_length=32)]) -> dict[str, Any]:
        """Replace bounded local reaction rules for this match. Each rule fires once per matched unit, never retries a rejected/uncertain order, expires, and respects manual repair/retreat orders. Requires a persistent MCP connection. Use cancel_reactions to stop."""
        try:
            return local_result(await runtime.configure(rules))
        except (BridgeError, ValueError) as error:
            raise ToolError(json.dumps(error.as_dict()) if isinstance(error, BridgeError) else str(error)) from error

    @server.tool(annotations=write)
    async def cancel_reactions(rule_ids: Annotated[list[str], Field(max_length=32)] = []) -> dict[str, Any]:
        """Cancel named local reaction rules; an empty list cancels all rules without issuing game orders."""
        return local_result(runtime.cancel(rule_ids))

    @server.tool(annotations=read)
    async def get_realtime_status() -> dict[str, Any]:
        """Read controller rules, recent automatic order results and measured bridge latency; model/network latency is not included."""
        return local_result(runtime.status())

    @server.tool(annotations=read)
    async def get_replay_status() -> dict[str, Any]:
        """Inspect recording/finalization status and the retained last match result before saving a replay."""
        return await invoke("get_replay_status")

    @server.tool(annotations=read)
    async def list_replays() -> dict[str, Any]:
        """List recordings in the game's replay directory with their header metadata."""
        return await invoke("list_replays")

    @server.tool(annotations=write)
    async def save_replay(name: Annotated[str, Field(pattern=r"^[A-Za-z0-9_-]{1,80}(?:\.rep)?$")]) -> dict[str, Any]:
        """Save the finalized last replay under a new name, never overwriting. Poll get_session for completion."""
        return await invoke("save_replay", name)

    @server.tool(annotations=write)
    async def play_replay(name: Annotated[str, Field(pattern=r"^[A-Za-z0-9_-]{1,80}(?:\.rep)?$")]) -> dict[str, Any]:
        """Start a listed replay from the idle shell. Playback gameplay is read-only; poll get_session for completion."""
        return await invoke("play_replay", name)

    @server.tool(annotations=read)
    async def get_capabilities() -> dict[str, Any]:
        """Read implemented operations, current match restrictions, and runtime support before planning game actions."""
        return await invoke("get_capabilities")

    @server.tool(annotations=read)
    async def get_game_state() -> dict[str, Any]:
        """Read the current match frame, local player resources, and owned units/buildings. Does not expose hidden enemies."""
        return await invoke("get_state")

    @server.tool(annotations=read)
    async def get_available_actions(object_id: ObjectID) -> dict[str, Any]:
        """Inspect an owned object's loaded commands, production/build options, abilities, and current eligibility. Use exact returned identifiers."""
        return await invoke("get_actions", object_id)

    @server.tool(annotations=read)
    async def get_world(
        offset: Annotated[int, Field(strict=True, ge=0, le=100000)] = 0,
        limit: Limit = 128,
        relation: Literal["all", "own", "ally", "enemy", "neutral"] = "all",
    ) -> dict[str, Any]:
        """Page through observable world objects, filtered by relation to the local player. Hidden enemy objects are not exposed."""
        return await invoke("get_world", offset, limit, relation)

    @server.tool(annotations=read)
    async def get_object(object_id: ObjectID) -> dict[str, Any]:
        """Inspect one observable object. Owned objects can include production, cargo, ammunition and upgrades; opponents' private state is omitted."""
        return await invoke("get_object", object_id)

    @server.tool(annotations=read)
    async def get_map(
        x: Coordinate, y: Coordinate,
        width: Annotated[float, Field(strict=True, gt=0, allow_inf_nan=False)],
        height: Annotated[float, Field(strict=True, gt=0, allow_inf_nan=False)],
        step: Annotated[float, Field(strict=True, ge=5, allow_inf_nan=False)] = 50,
    ) -> dict[str, Any]:
        """Sample structured terrain in a world-space rectangle starting at x,y. The rectangle must fit map bounds; at most 4096 grid samples."""
        return await invoke("get_map", x, y, width, height, step)

    @server.tool(annotations=read)
    async def get_events(
        since: Annotated[int, Field(strict=True, ge=0, le=9007199254740991)] = 0,
        limit: Limit = 128,
    ) -> dict[str, Any]:
        """Read recent observation events after a cursor. History is bounded; losing visibility is reported separately from confirmed destruction."""
        return await invoke("get_events", since, limit)

    @server.tool(annotations=read)
    async def get_players() -> dict[str, Any]:
        """Read player identities, factions and relationships available to the local player."""
        return await invoke("get_players")

    @server.tool(annotations=write)
    async def train_unit(producer_id: ObjectID, template_name: TemplateName) -> dict[str, Any]:
        """Queue one unit at an owned producer. Costs game money. Discover valid templates first; check state afterward."""
        return await invoke("train_unit", producer_id, template_name)

    @server.tool(annotations=write)
    async def attack_move(
        unit_ids: UnitIDs,
        x: Coordinate,
        y: Coordinate,
    ) -> dict[str, Any]:
        """Order 1-64 owned combat units to attack-move to a world position. This can put units into combat."""
        return await invoke("attack_move", unit_ids, x, y)

    @server.tool(annotations=read)
    async def check_build(builder_id: ObjectID, template_name: TemplateName, x: Coordinate, y: Coordinate, angle: Coordinate = 0) -> dict[str, Any]:
        """Check whether the owned builder can place a structure at world x,y with rotation angle in radians; does not place it."""
        return await invoke("check_build", builder_id, template_name, x, y, angle)

    @server.tool(annotations=read)
    async def find_build_sites(
        builder_id: ObjectID, template_name: TemplateName, x: Coordinate, y: Coordinate,
        radius: Annotated[float, Field(strict=True, ge=0, le=2000, allow_inf_nan=False)] = 300,
        spacing: Annotated[float, Field(strict=True, ge=10, allow_inf_nan=False)] = 40,
        limit: Annotated[int, Field(strict=True, ge=1, le=32)] = 8,
    ) -> dict[str, Any]:
        """Search bounded placement candidates around x,y for an owned builder. Returned sites use zero rotation; does not construct anything."""
        return await invoke("find_build_sites", builder_id, template_name, x, y, radius, spacing, limit)

    @server.tool(annotations=write)
    async def build(builder_id: ObjectID, template_name: TemplateName, x: Coordinate, y: Coordinate, angle: Coordinate = 0) -> dict[str, Any]:
        """Order an owned builder to construct a valid structure at world x,y; angle is in radians. Check availability and placement first."""
        return await invoke("build", builder_id, template_name, x, y, angle)

    @server.tool(annotations=write)
    async def build_line(
        builder_id: ObjectID, template_name: TemplateName,
        x1: Coordinate, y1: Coordinate, x2: Coordinate, y2: Coordinate, angle: Coordinate = 0,
    ) -> dict[str, Any]:
        """Build a line between world positions for a template advertising line_build. Angle is radians; validation is bounded to 128 tiles and the engine may shorten the line if conditions change."""
        return await invoke("build_line", builder_id, template_name, x1, y1, x2, y2, angle)

    @server.tool(annotations=write)
    async def unit_order(
        unit_ids: UnitIDs,
        order: Order,
        target_id: TargetID = 0, x: Coordinate | None = None, y: Coordinate | None = None, queued: Flag = False,
    ) -> dict[str, Any]:
        """Issue an engine order to owned units. Move/force_move/attack_move/attack_ground require x,y; force_attack/get_repaired/get_healed/salvage require an observable target; scatter needs neither. Only move supports queued waypoints."""
        return await invoke("unit_order", unit_ids, order, target_id, x, y, queued)

    @server.tool(annotations=write)
    async def use_command(
        object_id: ObjectID, command_button: Identifier,
        target_id: TargetID = 0, x: Coordinate | None = None, y: Coordinate | None = None, angle: Coordinate = 0,
    ) -> dict[str, Any]:
        """Execute a supported loaded command button (ability, upgrade, toggle or sell). Discover its identifier and target requirements first; angle is radians."""
        return await invoke("use_command", object_id, command_button, target_id, x, y, angle)

    @server.tool(annotations=write)
    async def cancel_production(producer_id: ObjectID, production_id: ProductionID) -> dict[str, Any]:
        """Cancel a queued unit by positive production ID, or a queued upgrade by 'upgrade:<UpgradeName>'. Zero is allowed only for one unambiguous queued upgrade."""
        return await invoke("cancel_production", producer_id, production_id)

    @server.tool(annotations=read)
    async def get_player_actions() -> dict[str, Any]:
        """Inspect nested player powers with actual source IDs, faction science choices, and the complete owned science inventory."""
        return await invoke("get_player_actions")

    @server.tool(annotations=write)
    async def purchase_science(science_name: Identifier) -> dict[str, Any]:
        """Purchase a currently available general science by exact identifier; consumes promotion points."""
        return await invoke("purchase_science", science_name)

    @server.tool(annotations=write)
    async def set_paused(paused: Flag) -> dict[str, Any]:
        """Resume a single-player match with false. Pausing requires explicit stepped control mode; realtime mode rejects true."""
        return await invoke("set_paused", paused)

    @server.tool(annotations=write)
    async def advance_game(frames: Annotated[int, Field(strict=True, ge=1, le=1800)]) -> dict[str, Any]:
        """Debug/test tool requiring stepped mode: advance a single-player simulation by bounded frames, then pause. Realtime mode rejects this."""
        return await invoke("advance_game", frames)

    @server.tool(annotations=read)
    async def list_maps() -> dict[str, Any]:
        """List game-discovered maps and loaded faction identifiers for starting a skirmish."""
        return await invoke("list_maps")

    @server.tool(annotations=read)
    async def list_saves() -> dict[str, Any]:
        """List available saved games in the game's own save location."""
        return await invoke("list_saves")

    @server.tool(annotations=write)
    async def save_game(name: FileName) -> dict[str, Any]:
        """Save a supported single-player match under a new name; existing saves are not overwritten. Poll get_session for deferred completion."""
        return await invoke("save_game", name)

    @server.tool(annotations=write)
    async def load_game(name: FileName) -> dict[str, Any]:
        """Load a listed single-player save, replacing the current match. Realtime mode continues running; stepped mode pauses. Poll get_session."""
        return await invoke("load_game", name)

    @server.tool(annotations=write)
    async def start_skirmish(
        map_name: MapName, faction: Identifier, opponent_faction: Identifier,
        difficulty: Literal["easy", "medium", "hard"] = "medium",
        money: Annotated[int, Field(strict=True, ge=0, le=1000000)] = 10000,
    ) -> dict[str, Any]:
        """Start a single-player skirmish with listed map/factions; realtime mode starts running. End an existing match first; poll get_session."""
        return await invoke("start_skirmish", map_name, faction, opponent_faction, difficulty, money)

    @server.tool(annotations=read)
    async def get_session() -> dict[str, Any]:
        """Read status, request ID, and result for deferred match, save/load, and replay operations."""
        return await invoke("get_session")

    @server.tool(annotations=write)
    async def end_match() -> dict[str, Any]:
        """Finalize the replay and end a single-player match, or stop replay playback, returning to the shell. Save game first if progress is needed."""
        return await invoke("end_match")

    @server.tool(annotations=read)
    async def get_lobby() -> dict[str, Any]:
        """Read LAN discovery, current lobby, player slots, acceptance, maps, colors and starting cash choices. Does not join or change a lobby."""
        return await invoke("get_lobby")

    @server.tool(annotations=write)
    async def enter_lan(
        player_name: Annotated[str, Field(max_length=12, pattern=r"^[\x20-\x2b\x2d-\x39\x3c-\x7e]*$", description="Temporary ASCII LAN name without comma/colon/semicolon or leading/trailing spaces; empty uses the existing name.")] = "",
        local_ip: Annotated[str, Field(max_length=15, pattern=r"^(?:[0-9]{1,3}(?:\.[0-9]{1,3}){3})?$", description="Assigned local interface or loopback IPv4 address; empty uses the normal interface.")] = "",
    ) -> dict[str, Any]:
        """Enter LAN and begin normal discovery with optional temporary name/interface overrides. Requires idle main menu and real game menus; headless dummy menus are unsupported. Observe get_lobby for the transition. Does not change saved preferences."""
        return await invoke("enter_lan", player_name, local_ip)

    @server.tool(annotations=write)
    async def lan_host() -> dict[str, Any]:
        """Create a LAN game from the LAN menu using the normal hosting flow. Other LAN players may discover the game."""
        return await invoke("lan_host")

    @server.tool(annotations=write)
    async def lan_join(host_ip: HostIP, direct: Flag = False) -> dict[str, Any]:
        """Request entry to a discovered LAN host. Set direct=true only to explicitly use the game's standard Direct Connect flow for the given IPv4 address. Observe get_lobby for acceptance."""
        return await invoke("lan_join", host_ip, direct)

    @server.tool(annotations=write)
    async def lan_accept() -> dict[str, Any]:
        """Mark the local LAN player's settings accepted using the normal lobby controls. Requires membership in a LAN game."""
        return await invoke("lan_accept")

    @server.tool(annotations=write)
    async def lan_start() -> dict[str, Any]:
        """Start the hosted LAN match when normal map, player and acceptance requirements are met. This advances all participating players."""
        return await invoke("lan_start")

    @server.tool(annotations=write)
    async def lan_leave() -> dict[str, Any]:
        """Leave the current LAN lobby through normal game controls. Hosts close their hosted lobby; this does not end an active match."""
        return await invoke("lan_leave")

    @server.tool(annotations=write)
    async def lan_set_slot(
        slot: Annotated[int, Field(strict=True, ge=0, le=7)],
        state: Literal["keep", "open", "closed", "easy", "medium", "brutal"],
        faction: LobbyFaction,
        color: Annotated[int, Field(strict=True, ge=-1, le=255, description="Loaded color index below get_lobby.color_count, or -1 for random.")],
        team: Annotated[int, Field(strict=True, ge=-1, le=3)],
        start_position: Annotated[int, Field(strict=True, ge=-1, le=7)],
    ) -> dict[str, Any]:
        """Configure an allowed LAN slot. Use keep for a human slot and empty faction to preserve it. Hosts can edit their own/AI slots; remote human slots are protected. Team/start -1 means unassigned/random."""
        return await invoke("lan_set_slot", slot, state, faction, color, team, start_position)

    @server.tool(annotations=write)
    async def lan_set_options(
        map_name: MapName,
        starting_cash: Annotated[int, Field(strict=True, ge=0, le=2147483647, description="Exact loaded starting_cash_choices value from get_lobby.")],
        limit_superweapons: Flag,
    ) -> dict[str, Any]:
        """Set the hosted LAN map, a loaded starting cash choice, and the superweapon limit through normal lobby updates. Only the host can change options."""
        return await invoke("lan_set_options", map_name, starting_cash, limit_superweapons)

    return server


def main() -> None:
    parser = argparse.ArgumentParser(description="Local Generals Zero Hour MCP server (stdio).")
    parser.add_argument("--pipe", default=os.environ.get("GENERALS_MCP_PIPE", "local"), help="Pipe suffix matching the game's GENERALS_MCP_PIPE (default: local).")
    parser.add_argument("--timeout", type=float, default=5.0, help="End-to-end game request timeout, 0.1-30 seconds (default: 5).")
    parser.add_argument("--control-mode", choices=("realtime", "stepped"), default="realtime", help="Realtime forbids pausing; stepped is for tests and debugging only.")
    args = parser.parse_args()
    try:
        bridge = GameBridge(args.pipe, args.timeout)
    except ValueError as exc:
        parser.error(str(exc))
    logging.basicConfig(level=logging.WARNING)
    create_server(bridge, args.control_mode).run(transport="stdio")
