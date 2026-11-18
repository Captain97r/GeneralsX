"""Command-line MCP client for game tools; all calls use the official stdio protocol."""

from __future__ import annotations

import argparse
import asyncio
from datetime import timedelta
import json
import os
from pathlib import Path
import sys
from typing import Any

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def parse_json(text: str) -> Any:
    if len(text.encode("utf-8")) > 131072:
        raise ValueError("JSON input exceeds 128 KiB.")
    def reject_constant(value: str):
        raise ValueError("JSON must not contain non-finite values: " + value)
    return json.loads(text, parse_constant=reject_constant)


def arguments_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Call Generals tools through an official MCP stdio session.")
    parser.add_argument("--pipe", default=os.environ.get("GENERALS_MCP_PIPE", "local"))
    parser.add_argument("--timeout", type=float, default=5.0, help="Native pipe deadline, 0.1-30 seconds.")
    parser.add_argument("--control-mode", choices=("realtime", "stepped"), default="realtime")
    commands = parser.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list-tools", help="Print the server's tool names or full typed schemas.")
    listing.add_argument("--names-only", action="store_true")
    call = commands.add_parser("call", help="Call one named MCP tool once; no automatic retry.")
    call.add_argument("tool")
    source = call.add_mutually_exclusive_group()
    source.add_argument("--args", default=None, help="Tool arguments as a JSON object (default: {}).")
    source.add_argument("--args-file", type=Path, help="Read tool arguments from a UTF-8 JSON file.")
    batch = commands.add_parser("batch", help="Run up to 64 tool calls sequentially in one MCP session; stop on the first error.")
    batch.add_argument("--file", type=Path, required=True, help='JSON array of {"tool": name, "arguments": {...}} objects.')
    return parser


def read_json_file(path: Path) -> Any:
    with path.open("rb") as stream:
        data = stream.read(131073)
    if len(data) > 131072:
        raise ValueError("JSON input exceeds 128 KiB.")
    return parse_json(data.decode("utf-8"))


def requested_calls(args) -> list[dict[str, Any]]:
    if args.command == "list-tools":
        return []
    if args.command == "call":
        values = read_json_file(args.args_file) if args.args_file else parse_json(args.args or "{}")
        calls = [{"tool": args.tool, "arguments": values}]
    else:
        calls = read_json_file(args.file)
    if not isinstance(calls, list) or not 1 <= len(calls) <= 64:
        raise ValueError("Supply a JSON array containing 1-64 tool calls.")
    for item in calls:
        if (not isinstance(item, dict) or set(item) - {"tool", "arguments"}
                or not isinstance(item.get("tool"), str)
                or not isinstance(item.get("arguments", {}), dict)):
            raise ValueError("Each call must have a tool name and an arguments object.")
    return calls


def tool_output(name: str, reply) -> dict[str, Any]:
    output: dict[str, Any] = {"tool": name, "is_error": reply.isError}
    text = "\n".join(item.text for item in reply.content if item.type == "text")
    if reply.isError:
        output["error"] = {"message": text}
        try:
            error, _ = json.JSONDecoder().raw_decode(text[text.index("{"):])
            if isinstance(error, dict):
                output["error"] = error
        except (ValueError, TypeError):
            pass
    elif reply.structuredContent is not None:
        output["result"] = reply.structuredContent
    else:
        try:
            output["result"] = json.loads(text)
        except ValueError:
            output["result"] = text
    return output


async def execute(args, *, parameters: StdioServerParameters | None = None) -> tuple[dict[str, Any], int]:
    # Parse all calls before starting the server or submitting any order.
    calls = requested_calls(args)
    if parameters is None:
        from .bridge import pipe_path

        pipe_path(args.pipe)
        if not 0.1 <= args.timeout <= 30:
            raise ValueError("Timeout must be between 0.1 and 30 seconds.")
        env = dict(os.environ)
        env["PYTHONPATH"] = str(Path(__file__).resolve().parents[1]) + os.pathsep + env.get("PYTHONPATH", "")
        parameters = StdioServerParameters(
            command=sys.executable,
            args=["-m", "generals_mcp", "--pipe", args.pipe, "--timeout", str(args.timeout), "--control-mode", args.control_mode],
            env=env,
        )
    results = []
    async with stdio_client(parameters) as (read, write):
        async with ClientSession(read, write, read_timeout_seconds=timedelta(seconds=args.timeout + 10)) as session:
            initialized = await session.initialize()
            catalog = await session.list_tools()
            names = {tool.name for tool in catalog.tools}
            if args.command == "list-tools":
                return {"server": initialized.serverInfo.model_dump(), "tools": sorted(names) if args.names_only else
                        [tool.model_dump(mode="json", exclude_none=True) for tool in catalog.tools]}, 0
            unknown = [call["tool"] for call in calls if call["tool"] not in names]
            if unknown:
                raise ValueError("Unknown MCP tool(s): " + ", ".join(unknown))
            for item in calls:
                # A failed/ambiguous write is never repeated, and later batch calls are not submitted.
                try:
                    reply = await session.call_tool(item["tool"], item.get("arguments", {}))
                except Exception as exc:
                    tool = next(tool for tool in catalog.tools if tool.name == item["tool"])
                    read_only = tool.annotations is not None and tool.annotations.readOnlyHint is True
                    results.append({"tool": item["tool"], "is_error": True,
                                    "error": {"code": "mcp_transport_error", "message": str(exc), "delivery_unknown": not read_only}})
                    return {"calls": results, "stopped_on_error": True}, 1
                output = tool_output(item["tool"], reply)
                results.append(output)
                if reply.isError:
                    return {"calls": results, "stopped_on_error": True}, 1
    return {"calls": results, "stopped_on_error": False}, 0


def main() -> int:
    parser = arguments_parser()
    args = parser.parse_args()
    try:
        output, code = asyncio.run(execute(args))
    except Exception as exc:
        output, code = {"error": {"code": "client_error", "message": str(exc)}}, 1
    print(json.dumps(output, ensure_ascii=True, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
