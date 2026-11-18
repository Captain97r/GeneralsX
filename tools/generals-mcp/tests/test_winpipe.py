import os
from pathlib import Path
import subprocess
import sys
import time
import unittest
import uuid

from generals_mcp.bridge import BridgeError, GameBridge, pipe_path


@unittest.skipUnless(os.name == "nt", "Windows named-pipe integration")
class WindowsPipeTests(unittest.TestCase):
    def run_fixture(self, mode, callback):
        suffix = "test_" + uuid.uuid4().hex
        process = subprocess.Popen([sys.executable, str(Path(__file__).with_name("fake_pipe.py")), pipe_path(suffix), mode],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), "ready")
            callback(suffix)
        finally:
            if process.poll() is None:
                process.terminate()
            process.communicate(timeout=5)

    def test_fragmented_large_response(self):
        def check(suffix):
            reply = GameBridge(suffix, 3).call("get_state")
            self.assertEqual(reply["result"]["method"], "get_state")
            self.assertEqual(len(reply["result"]["padding"]), 80000)
        self.run_fixture("reply", check)

    def test_timeout_after_delivery(self):
        def check(suffix):
            start = time.monotonic()
            with self.assertRaises(BridgeError) as caught:
                GameBridge(suffix, 0.2).call("train_unit", 3, "Cyborg")
            self.assertEqual(caught.exception.code, "timeout")
            self.assertTrue(caught.exception.delivery_unknown)
            self.assertLess(time.monotonic() - start, 2)
        self.run_fixture("stall", check)

    def test_closed_connection_reports_unknown_delivery(self):
        def check(suffix):
            with self.assertRaises(BridgeError) as caught:
                GameBridge(suffix, 1).call("train_unit", 3, "Cyborg")
            self.assertTrue(caught.exception.delivery_unknown)
        self.run_fixture("close", check)

    def test_missing_bridge(self):
        with self.assertRaises(BridgeError) as caught:
            GameBridge("test_" + uuid.uuid4().hex, 0.2).call("get_state")
        self.assertEqual(caught.exception.code, "bridge_unavailable")
        self.assertFalse(caught.exception.delivery_unknown)


if __name__ == "__main__":
    unittest.main()
