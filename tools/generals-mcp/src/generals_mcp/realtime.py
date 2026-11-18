"""TheSuperHackers @feature Continuous, bounded reactions that never pause the simulation."""
from __future__ import annotations

import asyncio
from collections import Counter, deque
from contextlib import suppress
import time
from typing import Annotated, Any, Literal

import anyio
from pydantic import BaseModel, ConfigDict, Field, model_validator

from .bridge import BridgeError, GameBridge

Order = Literal["move", "attack", "attack_move", "guard", "guard_no_pursuit", "guard_air", "stop",
                "repair", "gather", "enter", "exit", "capture", "attack_ground", "force_attack",
                "force_move", "scatter", "get_repaired", "get_healed", "salvage"]
ID = Annotated[int, Field(strict=True, gt=0, le=2147483647)]
Coord = Annotated[float, Field(strict=True, allow_inf_nan=False)]


class ReactionRule(BaseModel):
    """One action per matching unit, bounded to this match and an expiry time."""
    model_config = ConfigDict(extra="forbid")
    rule_id: Annotated[str, Field(pattern=r"^[A-Za-z0-9_-]{1,64}$")]
    trigger: Literal["health_below", "unit_produced", "ownership_acquired", "idle"]
    unit_ids: Annotated[list[ID], Field(max_length=64)] = Field(default_factory=list)
    producer_id: ID | None = None
    production_id: ID | None = None
    health_fraction: Annotated[float, Field(strict=True, gt=0, le=1, allow_inf_nan=False)] = .3
    order: Order
    target_id: Annotated[int, Field(strict=True, ge=0, le=2147483647)] = 0
    x: Coord | None = None
    y: Coord | None = None
    delay_frames: Annotated[int, Field(strict=True, ge=0, le=1800)] = 0
    expires_seconds: Annotated[float, Field(strict=True, gt=0, le=3600, allow_inf_nan=False)] = 300.
    max_fires: Annotated[int, Field(strict=True, ge=1, le=128)] = 64

    @model_validator(mode="after")
    def validate_rule(self):
        if (self.x is None) != (self.y is None):
            raise ValueError("Supply both x and y.")
        if self.trigger == "unit_produced":
            if self.producer_id is None or self.unit_ids:
                raise ValueError("Production reactions require producer_id and no predetermined unit_ids.")
        elif not self.unit_ids or len(set(self.unit_ids)) != len(self.unit_ids):
            raise ValueError("This trigger requires distinct unit_ids.")
        if self.order in {"move", "force_move", "attack_move", "attack_ground"} and self.x is None:
            raise ValueError("This reaction order requires x and y.")
        if self.order in {"attack", "force_attack", "repair", "gather", "enter", "capture", "get_repaired", "get_healed", "salvage"} and not self.target_id:
            raise ValueError("This reaction order requires target_id.")
        return self


class RealtimeController:
    def __init__(self, bridge: GameBridge, mode: str = "realtime", poll_seconds: float = .25):
        self.bridge, self.mode, self.poll_seconds = bridge, mode, poll_seconds
        self.lock = asyncio.Lock()
        self.configured = False
        self.rules: dict[str, dict] = {}
        self.match_id: int | None = None
        self.cursor = 0
        self.manual_until: dict[int, float] = {}
        self.latencies: deque[float] = deque(maxlen=512)
        self.counts: Counter[str] = Counter()
        self.activity: deque[dict] = deque(maxlen=128)
        self.task: asyncio.Task | None = None
        self.generation = 0

    async def start(self):
        self.task = asyncio.create_task(self.run())

    async def close(self):
        if self.task:
            self.task.cancel()
            with suppress(asyncio.CancelledError):
                await self.task
        self.rules.clear()

    async def exchange(self, method: str, *args: Any) -> dict:
        worker = asyncio.create_task(anyio.to_thread.run_sync(self.bridge.call, method, *args))
        try:
            return await asyncio.shield(worker)
        except asyncio.CancelledError:
            # Raw asyncio cancellation also reaches this path (including close()).
            # Keep the caller's lock until bounded pipe I/O and Win32 cleanup finish.
            with anyio.CancelScope(shield=True):
                while not worker.done():
                    try:
                        await asyncio.shield(worker)
                    except asyncio.CancelledError:
                        continue
                    except Exception:
                        break
                with suppress(BaseException):
                    worker.result()
            raise

    async def call(self, method: str, *args: Any, automatic: bool = False, generation: int | None = None,
                   expires: float | None = None) -> dict:
        if self.mode == "realtime" and (method == "advance_game" or (method == "set_paused" and args[0])):
            raise BridgeError("realtime_mode", "Pausing and frame stepping are disabled. Use --control-mode stepped only for tests.")
        async with self.lock:
            if not self.configured:
                try:
                    await self.exchange("set_control_mode", self.mode)
                except BridgeError as error:
                    if error.code != "unknown_method":
                        raise
                self.configured = True
            if automatic and method == "unit_order":
                if generation != self.generation or any(time.monotonic() < self.manual_until.get(unit, 0) for unit in args[0]):
                    raise BridgeError("reaction_superseded", "A manual order or rule change superseded this reaction.")
                if expires is not None and time.monotonic() >= expires:
                    raise BridgeError("reaction_expired", "The reaction expired before order delivery.")
            reserved_units: set[int] = set()
            if not automatic:
                # Reserve priority before delivery: a lost reply can still follow a
                # successfully executed manual order. Do not override that intention.
                now = time.monotonic()
                self.manual_until = {unit: until for unit, until in self.manual_until.items() if until > now}
                entries = args[0] if method == "batch_orders" else [{"method": method, "arguments": args}]
                for entry in entries:
                    if entry["method"] in {"unit_order", "attack_move"}:
                        units = entry["arguments"][0]
                    elif entry["method"] in {"use_command", "build", "build_line"}:
                        units = [entry["arguments"][0]]
                    else:
                        units = []
                    if isinstance(units, (list, tuple)):
                        for unit_id in units:
                            if type(unit_id) is int and 0 < unit_id <= 2147483647:
                                reserved_units.add(unit_id)
                                self.manual_until[unit_id] = now + 5
                if method in {"end_match", "start_skirmish", "load_game", "play_replay"}:
                    self.rules.clear()
                    self.manual_until.clear()
                    self.generation += 1
            started = time.monotonic()
            try:
                result = await self.exchange(method, *args)
            except BridgeError:
                self.configured = False
                raise
            finally:
                self.latencies.append((time.monotonic() - started) * 1000)
                self.counts[method] += 1
                for unit_id in reserved_units:
                    self.manual_until[unit_id] = time.monotonic() + 5
            return result

    async def configure(self, rules: list[ReactionRule]) -> dict:
        if len({r.rule_id for r in rules}) != len(rules):
            raise ValueError("Reaction rule IDs must be unique.")
        self.generation += 1
        generation = self.generation
        self.rules.clear()
        snapshot = (await self.call("get_snapshot", 0, 1))["result"]
        if generation != self.generation:
            raise BridgeError("reaction_superseded", "This configuration was cancelled or replaced while reading the match.")
        if snapshot.get("status") != "in_match" or snapshot.get("match_result") != "in_progress":
            raise BridgeError("not_in_match", "Reactions require an active playable match.")
        now = time.monotonic()
        self.match_id, self.cursor = snapshot["match_id"], snapshot["event_cursor"]
        self.rules = {r.rule_id: {"rule": r, "expires": now + r.expires_seconds, "fired": set(), "pending": {}} for r in rules}
        return self.status()

    def cancel(self, rule_ids: list[str]) -> dict:
        self.generation += 1
        if not rule_ids:
            self.rules.clear()
        for name in rule_ids:
            self.rules.pop(name, None)
        return self.status()

    def status(self) -> dict:
        samples = sorted(self.latencies)
        percentile = lambda fraction: round(samples[min(len(samples)-1, int((len(samples)-1)*fraction))], 2) if samples else None
        return {"control_mode": self.mode, "poll_interval_seconds": self.poll_seconds, "match_id": self.match_id,
                "rules": [{"rule_id": name, "trigger": state["rule"].trigger, "fired_unit_ids": sorted(state["fired"]),
                           "expires_in_seconds": max(0, round(state["expires"]-time.monotonic(), 1))} for name,state in self.rules.items()],
                "bridge_latency_ms": {"samples": len(samples), "p50": percentile(.5), "p95": percentile(.95), "max": max(samples) if samples else None},
                "calls": dict(self.counts), "recent_activity": list(self.activity)}

    async def tick(self):
        generation = self.generation
        snapshot = (await self.call("get_snapshot", 0, 1, automatic=True))["result"]
        if generation != self.generation:
            return
        if (snapshot.get("status") != "in_match" or snapshot.get("match_result") != "in_progress" or
                snapshot.get("match_id") != self.match_id):
            self.activity.append({"event": "reactions_stopped", "reason": "match_changed_or_finished"})
            self.rules.clear()
            self.manual_until.clear()
            self.generation += 1
            return
        if snapshot.get("paused"):
            return  # Respect a human pause; this controller never changes the clock.
        events = (await self.call("get_events", self.cursor, 256, automatic=True))["result"]
        if generation != self.generation:
            return
        self.cursor = events["next_since"]
        if events.get("history_lost"):
            self.activity.append({"event": "history_lost", "cursor": self.cursor})
        own = {o["id"]: o for o in snapshot["own"]}
        local_id = next((p["id"] for p in snapshot.get("players", []) if p.get("relation") == "own"), None)
        now, frame = time.monotonic(), snapshot["frame"]
        for name, state in list(self.rules.items()):
            rule: ReactionRule = state["rule"]
            if state["expires"] <= now or len(state["fired"]) >= rule.max_fires:
                self.rules.pop(name, None)
                continue
            for event in events["events"]:
                if event.get("match") != self.match_id:
                    continue
                produced = (rule.trigger == "unit_produced" and event["type"] == "unit_produced" and
                            event.get("producer_id") == rule.producer_id and
                            (rule.production_id is None or event.get("production_id") == rule.production_id))
                acquired = (rule.trigger == "ownership_acquired" and event["type"] == "ownership_changed" and
                            event.get("object_id") in rule.unit_ids and event.get("owner_id") == local_id)
                if produced or acquired:
                    state["pending"][event["object_id"]] = event["frame"] + rule.delay_frames
            candidates = rule.unit_ids if rule.trigger in {"health_below", "idle"} else list(state["pending"])
            for unit_id in candidates:
                if len(state["fired"]) >= rule.max_fires or time.monotonic() >= state["expires"]:
                    break
                unit = own.get(unit_id)
                if (not unit or unit_id in state["fired"] or now < self.manual_until.get(unit_id, 0) or
                        unit.get("disabled") or unit.get("contained_by") or unit.get("under_construction")):
                    continue
                current = unit.get("current_order") or {}
                if current.get("has_builder_task") or current.get("state") in {
                    "get_repaired", "get_healed", "repair", "enter", "exit", "busy"
                }:
                    continue
                if rule.trigger == "health_below" and unit["health"] > unit["max_health"] * rule.health_fraction:
                    continue
                if rule.trigger == "idle" and (
                    not current.get("idle", current.get("state") == "idle") or
                    unit.get("moving") or unit.get("attacking")
                ):
                    continue
                if unit_id in state["pending"] and frame < state["pending"][unit_id]:
                    continue
                # Consume before I/O: uncertain delivery and errors never cause automatic retries.
                state["fired"].add(unit_id)
                try:
                    result = await self.call("unit_order", [unit_id], rule.order, rule.target_id, rule.x, rule.y, False,
                                             automatic=True, generation=generation, expires=state["expires"])
                    self.activity.append({"rule_id": name, "unit_id": unit_id, "frame": frame, "result": result["result"]})
                except BridgeError as error:
                    self.activity.append({"rule_id": name, "unit_id": unit_id, "frame": frame, "error": error.as_dict()})
                if generation != self.generation:
                    return

    async def run(self):
        while True:
            await asyncio.sleep(self.poll_seconds)
            if not self.rules:
                continue
            generation = self.generation
            try:
                await self.tick()
            except Exception as error:
                self.activity.append({"event": "reactions_stopped", "reason": f"{type(error).__name__}: {error}"})
                if generation == self.generation:
                    self.rules.clear()  # A disconnect/schema mismatch cannot replay stale intentions later.
                    self.generation += 1
                    self.configured = False

    async def watch(self, since: int, timeout_seconds: float, event_types: list[str]) -> dict:
        deadline = time.monotonic() + timeout_seconds
        while True:
            result = (await self.call("get_events", since, 256))["result"]
            since = result["next_since"]
            selected = [event for event in result["events"] if not event_types or event["type"] in event_types]
            if selected or result.get("history_lost") or result.get("match_result") in {"victory", "defeat", "ended"} or time.monotonic() >= deadline:
                return dict(result, events=selected, timed_out=not selected and time.monotonic() >= deadline)
            await asyncio.sleep(0 if result.get("more") else min(self.poll_seconds, max(0, deadline-time.monotonic())))
