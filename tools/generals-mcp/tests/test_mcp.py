import asyncio
import importlib.util
import os
from pathlib import Path
import sys
import unittest

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from generals_mcp.bridge import BridgeError, MUTATING_METHODS, READ_METHODS


class MCPTests(unittest.TestCase):
    def test_real_stdio_protocol(self):
        asyncio.run(self.exercise_protocol())

    async def exercise_protocol(self):
        base = Path(__file__).resolve().parents[1]
        env = dict(os.environ, PYTHONPATH=str(base / "src"))
        process = StdioServerParameters(command=sys.executable, args=[str(base / "tests" / "fake_mcp.py")], env=env)
        async with stdio_client(process) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                tools = {tool.name: tool for tool in (await session.list_tools()).tools}
                expected = (MUTATING_METHODS | READ_METHODS) - {"get_state", "get_actions", "set_control_mode"} | {
                    "get_game_state", "get_available_actions", "watch_game", "configure_reactions", "cancel_reactions", "get_realtime_status"}
                self.assertEqual(set(tools), expected)
                self.assertTrue(tools["get_game_state"].annotations.readOnlyHint)
                self.assertFalse(tools["train_unit"].annotations.idempotentHint)
                self.assertEqual(tools["attack_move"].inputSchema["properties"]["unit_ids"]["maxItems"], 64)
                self.assertEqual(tools["get_available_actions"].inputSchema["properties"]["object_id"]["maximum"], 2147483647)
                self.assertEqual(tools["unit_order"].inputSchema["properties"]["target_id"]["maximum"], 2147483647)
                self.assertTrue({"attack_ground", "force_attack", "force_move", "scatter", "get_repaired", "get_healed", "salvage"}.issubset(
                    tools["unit_order"].inputSchema["properties"]["order"]["enum"]))
                for method in MUTATING_METHODS - {"set_control_mode"}:
                    self.assertFalse(tools[method].annotations.readOnlyHint)
                    self.assertFalse(tools[method].annotations.idempotentHint)
                self.assertEqual(tools["get_world"].inputSchema["properties"]["limit"]["maximum"], 256)
                self.assertEqual(tools["advance_game"].inputSchema["properties"]["frames"]["maximum"], 1800)
                self.assertEqual(tools["find_build_sites"].inputSchema["properties"]["limit"]["maximum"], 32)
                self.assertTrue(tools["get_lobby"].annotations.readOnlyHint)
                self.assertEqual(tools["lan_set_slot"].inputSchema["properties"]["slot"]["maximum"], 7)
                self.assertEqual(tools["lan_set_slot"].inputSchema["properties"]["color"]["maximum"], 255)
                self.assertEqual(tools["lan_set_options"].inputSchema["properties"]["starting_cash"]["maximum"], 2147483647)
                lobby = await session.call_tool("get_lobby", {})
                self.assertEqual(lobby.structuredContent["result"]["args"], [])
                entered = await session.call_tool("enter_lan", {})
                self.assertEqual(entered.structuredContent["result"]["args"], ["", ""])
                join = await session.call_tool("lan_join", {"host_ip": "192.168.1.23"})
                self.assertEqual(join.structuredContent["result"]["args"], ["192.168.1.23", "0"])
                direct = await session.call_tool("lan_join", {"host_ip": "127.0.0.2", "direct": True})
                self.assertEqual(direct.structuredContent["result"]["args"], ["127.0.0.2", "1"])
                slot = await session.call_tool("lan_set_slot", {"slot": 1, "state": "closed", "faction": "", "color": -1, "team": -1, "start_position": -1})
                self.assertEqual(slot.structuredContent["result"]["args"], ["1", "closed", "", "-1", "-1", "-1"])
                options = await session.call_tool("lan_set_options", {"map_name": "Maps/Alpine Assault/Alpine Assault.map", "starting_cash": 10000, "limit_superweapons": True})
                self.assertEqual(options.structuredContent["result"]["args"], ["Maps/Alpine Assault/Alpine Assault.map", "10000", "1"])
                defaults = await session.call_tool("get_world", {})
                self.assertEqual(defaults.structuredContent["result"]["args"], ["0", "128", "all"])
                command = await session.call_tool("use_command", {"object_id": 7, "command_button": "Command_Sell"})
                self.assertEqual(command.structuredContent["result"]["args"], ["7", "Command_Sell", "0", "", "", "0"])
                move_order = await session.call_tool("unit_order", {"unit_ids": [7], "order": "move", "x": 100, "y": 200, "queued": True})
                self.assertEqual(move_order.structuredContent["result"]["args"], ["7", "move", "0", "100", "200", "1"])
                cancelled = await session.call_tool("cancel_production", {"producer_id": 7, "production_id": "upgrade:Upgrade_Armor"})
                self.assertEqual(cancelled.structuredContent["result"]["args"], ["7", "upgrade:Upgrade_Armor"])
                line = await session.call_tool("build_line", {"builder_id": 7, "template_name": "Fence", "x1": 10, "y1": 20, "x2": 30, "y2": 20})
                self.assertEqual(line.structuredContent["result"]["args"], ["7", "Fence", "10", "20", "30", "20", "0"])
                boundary = await session.call_tool("get_available_actions", {"object_id": 2147483647})
                self.assertFalse(boundary.isError)
                state = await session.call_tool("get_game_state")
                self.assertFalse(state.isError)
                self.assertEqual(state.structuredContent["result"]["frame"], 42)
                smoke_path = base.parents[1] / "Tests" / "AgentBridge" / "game_smoke.py"
                smoke_spec = importlib.util.spec_from_file_location("game_smoke_for_test", smoke_path)
                smoke = importlib.util.module_from_spec(smoke_spec)
                smoke_spec.loader.exec_module(smoke)
                smoke_client = smoke.MCPGame(session)
                self.assertEqual((await smoke_client.call("get_state"))["result"]["frame"], 42)
                with self.assertRaises(BridgeError) as smoke_error:
                    await smoke_client.call("train_unit", 3, "Cyborg")
                self.assertEqual(smoke_error.exception.code, "not_skirmish")
                self.assertFalse(smoke_error.exception.delivery_unknown)
                move = await session.call_tool("attack_move", {"unit_ids": [1, 2], "x": 12.5, "y": 20})
                self.assertFalse(move.isError)
                self.assertEqual(move.structuredContent["result"]["args"], ["1,2", "12.5", "20"])
                failed = await session.call_tool("train_unit", {"producer_id": 3, "template_name": "Cyborg"})
                self.assertTrue(failed.isError)
                self.assertIn("not_skirmish", failed.content[0].text)
                for args in ({"unit_ids": [], "x": 1, "y": 2}, {"unit_ids": [True], "x": 1, "y": 2},
                             {"unit_ids": [1], "x": True, "y": 2}, {"unit_ids": [1, 1], "x": 1, "y": 2}):
                    invalid = await session.call_tool("attack_move", args)
                    self.assertTrue(invalid.isError)
                for tool, arguments in (
                    ("set_paused", {"paused": 1}),
                    ("get_available_actions", {"object_id": 2147483648}),
                    ("unit_order", {"unit_ids": [1], "order": "attack", "target_id": 2147483648}),
                    ("get_events", {"since": True}),
                    ("load_game", {"name": "../save"}),
                    ("advance_game", {"frames": 1801}),
                    ("find_build_sites", {"builder_id": 7, "template_name": "Barracks", "x": 0, "y": 0, "radius": 2001}),
                    ("lan_join", {"host_ip": "192.168.1.256"}),
                    ("lan_join", {"host_ip": "localhost"}),
                    ("lan_set_slot", {"slot": 1, "state": "human", "faction": "", "color": -1, "team": -1, "start_position": -1}),
                    ("lan_set_options", {"map_name": "Map", "starting_cash": 10000, "limit_superweapons": 1}),
                ):
                    with self.subTest(tool=tool):
                        self.assertTrue((await session.call_tool(tool, arguments)).isError)


if __name__ == "__main__":
    unittest.main()
