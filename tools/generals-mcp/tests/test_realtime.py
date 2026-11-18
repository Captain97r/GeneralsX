import asyncio
import copy
import threading
import time
import unittest

from generals_mcp.bridge import BridgeError, GameBridge
from generals_mcp.realtime import ReactionRule, RealtimeController
from generals_mcp.server import BatchCommand, encode_batch


class FakeBridge:
    def __init__(self):
        self.calls = []
        self.snapshot = {"frame": 100, "match_id": 1, "status": "in_match", "paused": False,
                         "match_result": "in_progress", "event_cursor": 1,
                         "players": [{"id": 2, "relation": "own"}],
                         "own": [{"id": 7, "health": 20, "max_health": 100,
                                  "current_order": {"state": "idle"}}]}
        self.events = []
        self.fail_write = False

    def call(self, method, *args):
        self.calls.append((method, args))
        if method == "get_snapshot":
            result = copy.deepcopy(self.snapshot)
        elif method == "get_events":
            result = {"events": copy.deepcopy(self.events), "next_since": 2, "match_result": "in_progress", "more": False}
        elif method == "unit_order" and self.fail_write:
            raise BridgeError("timeout", "unknown delivery", delivery_unknown=True)
        else:
            result = {"status": "queued", "frame": 100}
        return {"protocol": 1, "request_id": "test", "ok": True, "result": result}


class RealtimeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.bridge = FakeBridge()
        self.runtime = RealtimeController(self.bridge)

    def rule(self, **updates):
        return ReactionRule(**dict({"rule_id": "retreat", "trigger": "health_below", "unit_ids": [7],
                                    "order": "move", "x": 10., "y": 20.}, **updates))

    def writes(self):
        return [args for method, args in self.bridge.calls if method == "unit_order"]

    async def test_realtime_forbids_clock_controls_before_delivery(self):
        for method, args in (("set_paused", (True,)), ("advance_game", (30,))):
            with self.assertRaises(BridgeError):
                await self.runtime.call(method, *args)
        self.assertEqual(self.bridge.calls, [])
        await self.runtime.call("set_paused", False)
        self.assertEqual(self.bridge.calls[0], ("set_control_mode", ("realtime",)))

    async def test_health_reaction_once_without_pausing(self):
        await self.runtime.configure([self.rule()])
        await self.runtime.tick()
        await self.runtime.tick()
        self.assertEqual(len(self.writes()), 1)
        self.assertEqual(self.writes()[0][1], "move")
        self.assertFalse(any(name in {"advance_game", "set_paused"} for name, _ in self.bridge.calls))

    async def test_unknown_delivery_consumed_not_retried(self):
        await self.runtime.configure([self.rule()])
        self.bridge.fail_write = True
        await self.runtime.tick()
        await self.runtime.tick()
        self.assertEqual(len(self.writes()), 1)
        self.assertTrue(self.runtime.status()["recent_activity"][-1]["error"]["delivery_unknown"])

    async def test_manual_repair_and_manual_grace_take_priority(self):
        await self.runtime.configure([self.rule()])
        await self.runtime.call("unit_order", [7], "get_repaired", 8, None, None, False)
        await self.runtime.tick()
        self.assertEqual(len(self.writes()), 1)
        self.runtime.manual_until.clear()
        self.bridge.snapshot["own"][0]["current_order"]["state"] = "get_repaired"
        await self.runtime.tick()
        self.assertEqual(len(self.writes()), 1)

    async def test_automatic_reactions_leave_builders_with_pending_tasks_alone(self):
        await self.runtime.configure([self.rule(), self.rule(rule_id="idle", trigger="idle")])
        unit = self.bridge.snapshot["own"][0]
        unit["current_order"] = {"state": "idle", "idle": False, "has_builder_task": True}
        await self.runtime.tick()
        self.assertEqual(self.writes(), [])
        unit["health"] = 100
        unit["current_order"] = {"state": "idle", "idle": True, "has_builder_task": False}
        await self.runtime.tick()
        self.assertEqual(len(self.writes()), 1)

    async def test_cancel_generation_prevents_inflight_reaction(self):
        await self.runtime.configure([self.rule()])
        generation = self.runtime.generation
        self.runtime.cancel([])
        with self.assertRaises(BridgeError):
            await self.runtime.call("unit_order", [7], "move", 0, 10., 20., False, automatic=True, generation=generation)
        self.assertEqual(self.writes(), [])

    async def test_rule_maximum_and_expiry(self):
        self.bridge.snapshot["own"].append(dict(self.bridge.snapshot["own"][0], id=8))
        await self.runtime.configure([self.rule(unit_ids=[7, 8], max_fires=1)])
        await self.runtime.tick()
        self.assertEqual(len(self.writes()), 1)
        await self.runtime.configure([self.rule()])
        self.runtime.rules["retreat"]["expires"] = 0
        await self.runtime.tick()
        self.assertFalse(self.runtime.rules)
        self.assertEqual(len(self.writes()), 1)

    async def test_new_match_and_human_pause_do_not_issue_orders(self):
        await self.runtime.configure([self.rule()])
        self.bridge.snapshot["paused"] = True
        await self.runtime.tick()
        self.assertEqual(self.writes(), [])
        self.bridge.snapshot["match_id"] = 2
        await self.runtime.tick()
        self.assertFalse(self.runtime.rules)

    async def test_production_followup_uses_correlated_id_and_delay(self):
        await self.runtime.configure([self.rule(trigger="unit_produced", unit_ids=[], producer_id=9, production_id=21, delay_frames=90)])
        self.bridge.events = [{"sequence": 2, "match": 1, "type": "unit_produced", "object_id": 7,
                               "producer_id": 9, "production_id": 21, "frame": 100}]
        await self.runtime.tick()
        self.assertFalse(self.writes())
        self.bridge.events = []
        self.bridge.snapshot["frame"] = 190
        await self.runtime.tick()
        self.assertEqual(self.writes()[0][0], [7])

    async def test_owner_loss_is_not_acquisition(self):
        await self.runtime.configure([self.rule(trigger="ownership_acquired")])
        self.bridge.events = [{"match": 1, "type": "ownership_changed", "object_id": 7, "owner_id": 0, "frame": 100}]
        await self.runtime.tick()
        self.assertFalse(self.writes())
        self.bridge.events[0]["owner_id"] = 2
        await self.runtime.tick()
        self.assertEqual(len(self.writes()), 1)

    async def test_watch_returns_result_without_clock_change(self):
        self.bridge.events = [{"match": 1, "type": "match_result", "result": "defeat"}]
        result = await self.runtime.watch(1, .01, ["match_result"])
        self.assertEqual(result["events"][0]["result"], "defeat")
        self.assertFalse(any(method in {"set_paused", "advance_game"} for method, _ in self.bridge.calls))

    async def test_replacement_during_event_read_does_not_consume_new_rule(self):
        class GatedRuntime(RealtimeController):
            async def call(inner, method, *args, **kwargs):
                if method == "get_events":
                    entered.set()
                    await release.wait()
                return await super().call(method, *args, **kwargs)

        entered, release = asyncio.Event(), asyncio.Event()
        runtime = GatedRuntime(self.bridge)
        await runtime.configure([self.rule(rule_id="old")])
        tick = asyncio.create_task(runtime.tick())
        try:
            await asyncio.wait_for(entered.wait(), 1)
            await runtime.configure([self.rule(rule_id="new")])
        finally:
            release.set()
            await tick
        self.assertEqual(runtime.rules["new"]["fired"], set())
        self.assertEqual(self.writes(), [])
        await runtime.tick()
        self.assertEqual(len(self.writes()), 1)

    async def test_cancel_during_configuration_does_not_rearm_rules(self):
        class GatedRuntime(RealtimeController):
            async def call(inner, method, *args, **kwargs):
                if method == "get_snapshot":
                    entered.set()
                    await release.wait()
                return await super().call(method, *args, **kwargs)

        entered, release = asyncio.Event(), asyncio.Event()
        runtime = GatedRuntime(self.bridge)
        configuring = asyncio.create_task(runtime.configure([self.rule()]))
        try:
            await asyncio.wait_for(entered.wait(), 1)
            runtime.cancel([])
        finally:
            release.set()
        with self.assertRaises(BridgeError) as raised:
            await configuring
        self.assertEqual(raised.exception.code, "reaction_superseded")
        self.assertFalse(runtime.rules)

    async def test_uncertain_manual_order_keeps_priority_and_reconfigures_after_error(self):
        await self.runtime.configure([self.rule()])
        self.bridge.fail_write = True
        with self.assertRaises(BridgeError):
            await self.runtime.call("unit_order", [7], "get_repaired", 8, None, None, False)
        self.assertGreater(self.runtime.manual_until[7], time.monotonic())
        self.assertFalse(self.runtime.configured)
        self.bridge.fail_write = False
        await self.runtime.tick()
        self.assertEqual(len(self.writes()), 1)
        self.assertEqual(sum(name == "set_control_mode" for name, _ in self.bridge.calls), 2)

    async def test_generic_commands_and_builder_orders_reserve_manual_priority(self):
        for method, arguments in (
            ("use_command", (7, "Command_Repair", 8, None, None, 0)),
            ("build", (7, "AmericaPowerPlant", 10., 20., 0)),
            ("build_line", (7, "AmericaWall", 10., 20., 30., 40., 0)),
        ):
            with self.subTest(method=method):
                await self.runtime.configure([self.rule()])
                await self.runtime.call(method, *arguments)
                await self.runtime.tick()
                self.assertEqual(self.writes(), [])
                self.assertGreater(self.runtime.manual_until[7], time.monotonic())

    async def test_cancellation_holds_lock_until_worker_finishes(self):
        entered, release, finished = threading.Event(), threading.Event(), threading.Event()
        class BlockingBridge(FakeBridge):
            def call(inner, method, *args):
                if method == "get_snapshot":
                    entered.set()
                    release.wait(2)
                    finished.set()
                return super().call(method, *args)

        bridge = BlockingBridge()
        runtime = RealtimeController(bridge)
        runtime.configured = True
        first = asyncio.create_task(runtime.call("get_snapshot", 0, 1))
        second = None
        try:
            self.assertTrue(await asyncio.to_thread(entered.wait, 1))
            first.cancel()
            second = asyncio.create_task(runtime.call("get_events", 0, 1))
            await asyncio.sleep(.02)
            self.assertTrue(runtime.lock.locked())
            self.assertFalse(first.done())
            self.assertFalse(second.done())
            self.assertFalse(finished.is_set())
            self.assertFalse(bridge.calls)
        finally:
            release.set()
            results = await asyncio.gather(first, *([second] if second else []), return_exceptions=True)
        self.assertIsInstance(results[0], asyncio.CancelledError)
        self.assertTrue(finished.is_set())
        self.assertEqual([name for name, _ in bridge.calls], ["get_snapshot", "get_events"])

    async def test_background_schema_error_stops_rules_but_keeps_task_alive(self):
        await self.runtime.configure([self.rule()])
        self.bridge.snapshot["own"] = None
        self.runtime.poll_seconds = .001
        await self.runtime.start()
        try:
            async def wait_for_failure():
                while not self.runtime.activity:
                    await asyncio.sleep(.001)
            await asyncio.wait_for(wait_for_failure(), 1)
            self.assertFalse(self.runtime.rules)
            self.assertFalse(self.runtime.task.done())
            self.assertIn("TypeError", self.runtime.activity[-1]["reason"])
            self.assertEqual(self.writes(), [])
        finally:
            await self.runtime.close()

    async def test_expiry_during_slow_delivery_prevents_remaining_orders(self):
        entered, release = threading.Event(), threading.Event()
        class BlockingBridge(FakeBridge):
            def call(inner, method, *args):
                if method == "unit_order":
                    entered.set()
                    release.wait(2)
                return super().call(method, *args)

        bridge = BlockingBridge()
        bridge.snapshot["own"].append(dict(bridge.snapshot["own"][0], id=8))
        runtime = RealtimeController(bridge)
        await runtime.configure([self.rule(unit_ids=[7, 8])])
        tick = asyncio.create_task(runtime.tick())
        try:
            self.assertTrue(await asyncio.to_thread(entered.wait, 1))
            runtime.rules["retreat"]["expires"] = 0
        finally:
            release.set()
            await tick
        self.assertEqual(sum(name == "unit_order" for name, _ in bridge.calls), 1)

    def test_guard_reactions_allow_object_or_current_position(self):
        for order in ("guard", "guard_no_pursuit", "guard_air"):
            self.rule(order=order, target_id=8, x=None, y=None)
            self.rule(order=order, x=None, y=None)


class BatchTests(unittest.TestCase):
    def test_named_batch_arguments_are_ordered_and_validated(self):
        commands = [BatchCommand(tool="unit_order", arguments={"unit_ids": [7], "order": "guard_no_pursuit"})]
        encoded = encode_batch(commands)
        self.assertEqual(encoded[0]["arguments"], [[7], "guard_no_pursuit", 0, None, None, False])
        with self.assertRaises(ValueError):
            encode_batch([BatchCommand(tool="unit_order", arguments={"unit_ids": [7], "typo": "move"})])

    def test_invalid_late_entry_sends_nothing(self):
        class Transport:
            def exchange(self, *args):
                raise AssertionError("Invalid batch reached transport")
        bridge = GameBridge(transport=Transport())
        with self.assertRaises(ValueError):
            bridge.call("batch_orders", [{"method": "unit_order", "arguments": [[7], "stop", 0, None, None, False]},
                                         {"method": "end_match", "arguments": []}], 100, 90)
