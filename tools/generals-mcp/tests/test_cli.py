import asyncio
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

from mcp import StdioServerParameters

from generals_mcp.cli import arguments_parser, execute, parse_json, requested_calls


class CLITests(unittest.TestCase):
    def parameters(self):
        base = Path(__file__).resolve().parents[1]
        return StdioServerParameters(command=sys.executable, args=[str(base / "tests" / "fake_mcp.py")],
                                     env=dict(os.environ, PYTHONPATH=str(base / "src")))

    def test_lists_tools_through_official_mcp(self):
        args = arguments_parser().parse_args(["list-tools", "--names-only"])
        result, code = asyncio.run(execute(args, parameters=self.parameters()))
        self.assertEqual(code, 0)
        self.assertIn("get_capabilities", result["tools"])
        self.assertIn("build", result["tools"])
        self.assertIn("get_game_state", result["tools"])

    def test_calls_tool_through_official_mcp(self):
        args = arguments_parser().parse_args(["call", "get_world", "--args", '{"relation":"enemy"}'])
        result, code = asyncio.run(execute(args, parameters=self.parameters()))
        self.assertEqual(code, 0)
        self.assertEqual(result["calls"][0]["result"]["result"]["args"], ["0", "128", "enemy"])

    def test_batch_stops_after_bridge_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "batch.json"
            path.write_text(json.dumps([
                {"tool": "get_game_state"},
                {"tool": "train_unit", "arguments": {"producer_id": 3, "template_name": "Cyborg"}},
                {"tool": "get_players"},
            ]))
            args = arguments_parser().parse_args(["batch", "--file", str(path)])
            result, code = asyncio.run(execute(args, parameters=self.parameters()))
        self.assertEqual(code, 1)
        self.assertTrue(result["stopped_on_error"])
        self.assertEqual(len(result["calls"]), 2)
        self.assertEqual(result["calls"][1]["error"]["code"], "not_skirmish")
        self.assertFalse(result["calls"][1]["error"]["delivery_unknown"])

    def test_bad_cli_arguments_are_rejected_before_requests(self):
        for text in ('[]', '{"x": NaN}', '"string"'):
            args = arguments_parser().parse_args(["call", "build", "--args", text])
            with self.subTest(text=text), self.assertRaises(ValueError):
                requested_calls(args)
        with self.assertRaises(ValueError):
            parse_json('"' + 'x' * 131072 + '"')


if __name__ == "__main__":
    unittest.main()
