"""Subprocess fixture: real MCP protocol backed by a fake game, never the live bridge."""
import json

from generals_mcp.bridge import GameBridge
from generals_mcp.server import create_server


class FakeGame:
    def exchange(self, path, request, timeout):
        fields = request.decode().rstrip("\n").split("\t")
        reply = {"protocol": 1, "request_id": fields[1], "ok": True,
                 "result": {"frame": 42, "method": fields[2], "args": fields[3:]}}
        if fields[2] == "train_unit":
            reply.update(ok=False, error={"code": "not_skirmish", "message": "Fake game rejects production."})
        return (json.dumps(reply) + "\n").encode()


create_server(GameBridge("test", transport=FakeGame()), control_mode="stepped").run(transport="stdio")
