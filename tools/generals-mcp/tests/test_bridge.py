import json
import unittest

from generals_mcp.bridge import BridgeError, GameBridge, pipe_path, MUTATING_METHODS, READ_METHODS


class FakeExchange:
    def __init__(self, change=None, failure=None):
        self.requests = []
        self.change = change
        self.failure = failure

    def exchange(self, path, request, timeout):
        self.requests.append((path, request, timeout))
        if self.failure:
            raise self.failure
        fields = request.decode().rstrip("\n").split("\t")
        reply = {"protocol": 1, "request_id": fields[1], "ok": True,
                 "result": {"frame": 42, "method": fields[2], "args": fields[3:]}}
        if self.change:
            return self.change(reply)
        return (json.dumps(reply) + "\n").encode()


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.transport = FakeExchange()
        self.bridge = GameBridge("test", 1.0, self.transport)

    def test_correlated_request_and_args(self):
        result = self.bridge.call("attack_move", [12, 34], 100.25, 200)
        self.assertEqual(result["result"]["args"], ["12,34", "100.25", "200"])
        self.assertRegex(result["request_id"], r"^[0-9a-f]{32}$")
        self.assertEqual(self.transport.requests[0][0], r"\\.\pipe\GeneralsZH.MCP.test")
        self.assertTrue(self.transport.requests[0][1].startswith(b"GZH1\t"))

    def test_methods_and_unique_ids(self):
        first = self.bridge.call("get_state")
        second = self.bridge.call("get_actions", 3)
        third = self.bridge.call("train_unit", 3, "Cyborg_A")
        self.assertEqual(second["result"]["args"], ["3"])
        self.assertEqual(third["result"]["args"], ["3", "Cyborg_A"])
        self.assertNotEqual(first["request_id"], second["request_id"])

    def test_invalid_orders_never_reach_transport(self):
        cases = [("get_actions", True), ("get_actions", 0), ("get_actions", 2147483648),
                 ("train_unit", 3, "A\tget_state"), ("train_unit", 3, "A\n"),
                 ("attack_move", [], 1, 2), ("attack_move", [1] * 65, 1, 2),
                 ("attack_move", [1, 1], 1, 2), ("attack_move", [1], float("nan"), 2),
                 ("attack_move", [1], float("inf"), 2), ("attack_move", [1], True, 2),
                 ("unknown",)]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                self.bridge.call(*case)
        self.assertFalse(self.transport.requests)

    def test_pipe_suffix_validation(self):
        for suffix in ("", "x" * 65, "../other", "a\\b", "x\ny", "é"):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                pipe_path(suffix)

    def test_timeout_validation(self):
        for timeout in (0, 31, float("nan"), float("inf")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                GameBridge("test", timeout, self.transport)

    def test_bridge_error(self):
        def rejection(reply):
            reply.update(ok=False, error={"code": "not_skirmish", "message": "Only skirmish orders are supported."})
            return (json.dumps(reply) + "\n").encode()
        bridge = GameBridge(transport=FakeExchange(change=rejection))
        with self.assertRaises(BridgeError) as caught:
            bridge.call("train_unit", 3, "Cyborg")
        self.assertEqual(caught.exception.code, "not_skirmish")
        self.assertFalse(caught.exception.delivery_unknown)

    def test_invalid_responses(self):
        def changed(**values):
            return lambda reply: (json.dumps(dict(reply, **values)) + "\n").encode()
        changes = [lambda r: b"{}", lambda r: b"{}\n{}\n", lambda r: b"\xff\n",
                   lambda r: b"NaN\n", changed(request_id="other"), changed(protocol=True),
                   changed(protocol=2), changed(ok="yes"), changed(result=[]),
                   changed(ok=False, error={"message": "missing code"})]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(BridgeError) as caught:
                GameBridge(transport=FakeExchange(change=change)).call("train_unit", 3, "Cyborg")
            self.assertTrue(caught.exception.delivery_unknown)

    def test_native_adapter_exception_preserves_unknown_outcome(self):
        def exception(reply):
            reply.update(ok=False, error={"code": "internal_error", "message": "Adapter failed; outcome may be uncertain."})
            return (json.dumps(reply) + "\n").encode()
        with self.assertRaises(BridgeError) as caught:
            GameBridge(transport=FakeExchange(change=exception)).call("train_unit", 3, "Cyborg")
        self.assertTrue(caught.exception.delivery_unknown)

    def test_unknown_delivery_is_not_retried(self):
        transport = FakeExchange(failure=BridgeError("timeout", "Timed out", delivery_unknown=True))
        with self.assertRaises(BridgeError) as caught:
            GameBridge(transport=transport).call("train_unit", 3, "Cyborg")
        self.assertTrue(caught.exception.delivery_unknown)
        self.assertEqual(len(transport.requests), 1)

    def test_expanded_wire_formats(self):
        cases = {
            "set_control_mode": (("realtime",), ["realtime"]),
            "get_snapshot": ((0, 128), ["0", "128"]),
            "get_unit_profile": ((7,), ["7"]),
            "set_rally_point": ((7, 10, 20), ["7", "10", "20"]),
            "check_order": (([7], "guard_no_pursuit", 0, 10, 20, False), ["7", "guard_no_pursuit", "0", "10", "20", "0"]),
            "get_replay_status": ((), []), "list_replays": ((), []),
            "save_replay": (("match.rep",), ["match.rep"]), "play_replay": (("match.rep",), ["match.rep"]),
            "batch_orders": (([{"method": "unit_order", "arguments": [[7], "stop", 0, None, None, False]}], 42, 90),
                             ["42", "90", "1", "unit_order", "6", "7", "stop", "0", "", "", "0"]),
            "get_state": ((), []), "get_capabilities": ((), []), "get_players": ((), []),
            "get_player_actions": ((), []), "get_session": ((), []), "list_maps": ((), []),
            "list_saves": ((), []), "end_match": ((), []),
            "get_lobby": ((), []), "enter_lan": (("MCP-Test", "127.0.0.2"), ["MCP-Test", "127.0.0.2"]), "lan_host": ((), []),
            "lan_accept": ((), []), "lan_start": ((), []), "lan_leave": ((), []),
            "lan_join": (("192.168.1.23", False), ["192.168.1.23", "0"]),
            "lan_set_slot": ((1, "medium", "FactionGLA", -1, 0, 1), ["1", "medium", "FactionGLA", "-1", "0", "1"]),
            "lan_set_options": (("Maps/Alpine Assault/Alpine Assault.map", 10000, True),
                                ["Maps/Alpine Assault/Alpine Assault.map", "10000", "1"]),
            "get_actions": ((7,), ["7"]), "get_object": ((7,), ["7"]),
            "get_world": ((5, 16, "enemy"), ["5", "16", "enemy"]),
            "get_map": ((10, 20, 50, 80, 10), ["10", "20", "50", "80", "10"]),
            "get_events": ((51, 10), ["51", "10"]),
            "train_unit": ((7, "Cyborg"), ["7", "Cyborg"]),
            "attack_move": (([7, 8], 100, 200), ["7,8", "100", "200"]),
            "check_build": ((7, "Barracks", 100, 200, 0), ["7", "Barracks", "100", "200", "0"]),
            "build": ((7, "Barracks", 100, 200, 0), ["7", "Barracks", "100", "200", "0"]),
            "build_line": ((7, "Fence", 100, 200, 300, 200, 0), ["7", "Fence", "100", "200", "300", "200", "0"]),
            "find_build_sites": ((7, "Barracks", 100, 200, 100, 20, 4), ["7", "Barracks", "100", "200", "100", "20", "4"]),
            "unit_order": (([7, 8], "attack", 9, None, None, False), ["7,8", "attack", "9", "", "", "0"]),
            "use_command": ((7, "Command_Sell", 0, None, None, 0), ["7", "Command_Sell", "0", "", "", "0"]),
            "cancel_production": ((7, 0), ["7", "0"]),
            "purchase_science": (("SCIENCE_SpyDrone",), ["SCIENCE_SpyDrone"]),
            "set_paused": ((True,), ["1"]), "advance_game": ((30,), ["30"]),
            "save_game": (("test_save",), ["test_save"]), "load_game": (("test_save.sav",), ["test_save.sav"]),
            "start_skirmish": (("Maps/Alpine Assault/Alpine Assault.map", "FactionAmerica", "FactionChina", "hard", 10000),
                               ["Maps/Alpine Assault/Alpine Assault.map", "FactionAmerica", "FactionChina", "hard", "10000"]),
        }
        self.assertEqual(set(cases), MUTATING_METHODS | READ_METHODS)
        for method, (args, expected) in cases.items():
            with self.subTest(method=method):
                result = self.bridge.call(method, *args)
                self.assertEqual(result["result"]["args"], expected)
                failing = FakeExchange(failure=BridgeError("timeout", "Timed out", delivery_unknown=True))
                with self.assertRaises(BridgeError) as caught:
                    GameBridge(transport=failing).call(method, *args)
                self.assertEqual(caught.exception.delivery_unknown, method in MUTATING_METHODS)
                self.assertEqual(len(failing.requests), 1)

    def test_expanded_bounds_and_injection_rejections(self):
        cases = [
            ("get_world", -1, 20, "all"), ("get_world", 0, 257, "all"), ("get_world", 0, 1, "hidden"),
            ("get_map", 0, 0, 5000, 5000, 5), ("get_map", 0, 0, 0, 10, 5), ("get_map", 0, 0, 10, 10, 4),
            ("get_events", -1, 10), ("get_events", 9007199254740992, 10),
            ("find_build_sites", 7, "Barracks", 0, 0, 2001, 10, 1),
            ("find_build_sites", 7, "Barracks", 0, 0, 100, 9, 1),
            ("find_build_sites", 7, "Barracks", 0, 0, 100, 10, 33),
            ("check_build", 7, "Barracks\nInjected", 0, 0, 0),
            ("build", 7, "Barracks", 0, 0, float("inf")),
            ("build_line", 7, "Fence", 0, 0, 200, float("inf"), 0),
            ("unit_order", [7], "attack", 9, None, None, True),
            ("unit_order", [7], "move", 0, 2, None, False),
            ("unit_order", [7], "unknown", 0, None, None, False),
            ("use_command", 7, "Command_X", 0, None, 2, 0),
            ("cancel_production", 7, -1), ("cancel_production", 7, "upgrade:X\nInjected"),
            ("cancel_production", 7, "anything"), ("purchase_science", "SCIENCE_X\tInjected"),
            ("set_paused", 1), ("advance_game", 1801), ("advance_game", 0),
            ("save_game", "../save"), ("load_game", "C:\\save.sav"), ("save_game", "x" * 81),
            ("start_skirmish", "map\nInjected", "FactionAmerica", "FactionChina", "hard", 10000),
            ("start_skirmish", "map", "FactionAmerica", "FactionChina", "unknown", 10000),
            ("start_skirmish", "map", "FactionAmerica", "FactionChina", "hard", 1000001),
            ("lan_join", "192.168.1.256"), ("lan_join", "192.168.1.1\tInjected"),
            ("lan_join", "localhost"), ("lan_join", "1.2.3"), ("lan_join", 2130706433),
            ("lan_join", "127.0.0.2", 1),
            ("enter_lan", "Name\nInjected", "127.0.0.2"), ("enter_lan", "Name", "127.0.0.256"),
            ("enter_lan", "x" * 13, ""), ("enter_lan", "", 2130706433),
            ("enter_lan", "Name,", ""), ("enter_lan", " Name", ""), ("enter_lan", "Name ", ""),
            ("lan_set_slot", 8, "keep", "", -1, -1, -1),
            ("lan_set_slot", 1, "human", "", -1, -1, -1),
            ("lan_set_slot", 1, "medium", "Faction\nInjected", -1, -1, -1),
            ("lan_set_slot", 1, "keep", "", 256, -1, -1),
            ("lan_set_slot", 1, "keep", "", -1, 4, -1),
            ("lan_set_slot", 1, "keep", "", -1, -1, 8),
            ("lan_set_options", "Map\nInjected", 10000, True),
            ("lan_set_options", "Map", 2147483648, True),
            ("lan_set_options", "Map", 10000, 1),
        ]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                self.bridge.call(*case)
        self.assertFalse(self.transport.requests)

    def test_named_upgrade_cancellation(self):
        result = self.bridge.call("cancel_production", 7, "upgrade:Upgrade_Armor")
        self.assertEqual(result["result"]["args"], ["7", "upgrade:Upgrade_Armor"])

    def test_lan_preserves_empty_faction_and_loaded_choice_bounds(self):
        self.assertEqual(self.bridge.call("enter_lan", "", "")["result"]["args"], ["", ""])
        self.assertEqual(self.bridge.call("enter_lan")["result"]["args"], [])
        self.assertEqual(self.bridge.call("lan_join", "127.0.0.2", True)["result"]["args"], ["127.0.0.2", "1"])
        result = self.bridge.call("lan_set_slot", 7, "closed", "", 255, 3, 7)
        self.assertEqual(result["result"]["args"], ["7", "closed", "", "255", "3", "7"])
        result = self.bridge.call("lan_set_options", "Map", 2147483647, False)
        self.assertEqual(result["result"]["args"], ["Map", "2147483647", "0"])

    def test_extended_unit_orders(self):
        for order in ("attack_ground", "force_move"):
            result = self.bridge.call("unit_order", [1], order, 0, 10, 20, False)
            self.assertEqual(result["result"]["args"], ["1", order, "0", "10", "20", "0"])
        for order in ("force_attack", "get_repaired", "get_healed", "salvage", "scatter"):
            result = self.bridge.call("unit_order", [1], order, 0 if order == "scatter" else 2, None, None, False)
            self.assertEqual(result["result"]["args"], ["1", order, "0" if order == "scatter" else "2", "", "", "0"])
            with self.assertRaises(ValueError):
                self.bridge.call("unit_order", [1], order, 2, None, None, True)

    def test_native_signed_id_boundary(self):
        self.assertEqual(self.bridge.call("get_actions", 2147483647)["result"]["args"], ["2147483647"])
        self.assertEqual(self.bridge.call("unit_order", [1], "attack", 2147483647, None, None, False)
                         ["result"]["args"][2], "2147483647")
        self.assertEqual(self.bridge.call("cancel_production", 1, 2147483647)["result"]["args"], ["1", "2147483647"])
        for args in (("get_object", 2147483648),
                     ("unit_order", [1], "attack", 2147483648, None, None, False),
                     ("use_command", 1, "Command_Attack", 2147483648, None, None, 0),
                     ("cancel_production", 1, 2147483648)):
            with self.subTest(args=args), self.assertRaises(ValueError):
                self.bridge.call(*args)


if __name__ == "__main__":
    unittest.main()
