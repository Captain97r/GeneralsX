"""Validate and correlate the small, versioned game bridge protocol."""

from __future__ import annotations

import json
import ipaddress
import math
import re
import uuid
from typing import Any, Protocol

MAX_REQUEST_BYTES = 16_384
MAX_RESPONSE_BYTES = 1_048_576
PIPE_PREFIX = "\\\\.\\pipe\\GeneralsZH.MCP."
MUTATING_METHODS = frozenset({
    "train_unit", "attack_move", "build", "build_line", "unit_order", "use_command", "cancel_production",
    "purchase_science", "set_paused", "advance_game", "save_game", "load_game", "start_skirmish", "end_match",
    "enter_lan", "lan_host", "lan_join", "lan_accept", "lan_start", "lan_leave", "lan_set_slot", "lan_set_options",
    "set_rally_point", "batch_orders", "set_control_mode", "save_replay", "play_replay",
})
READ_METHODS = frozenset({
    "get_state", "get_actions", "get_capabilities", "get_world", "get_object", "get_map",
    "get_events", "get_players", "check_build", "find_build_sites", "get_player_actions",
    "list_maps", "list_saves", "get_session", "get_lobby",
    "get_snapshot", "get_unit_profile", "check_order", "get_replay_status", "list_replays",
})


class BridgeError(Exception):
    def __init__(self, code: str, message: str, *, delivery_unknown: bool = False):
        super().__init__(message)
        self.code = code
        self.delivery_unknown = delivery_unknown

    def as_dict(self) -> dict[str, Any]:
        return {"code": self.code, "message": str(self), "delivery_unknown": self.delivery_unknown}


class Exchange(Protocol):
    def exchange(self, path: str, request: bytes, timeout: float) -> bytes: ...


def pipe_path(suffix: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", suffix):
        raise ValueError("Pipe suffix must contain 1-64 ASCII letters, digits, underscores or hyphens.")
    return PIPE_PREFIX + suffix


def object_id(value: Any) -> int:
    if type(value) is not int or not 0 < value <= 2147483647:
        raise ValueError("Object IDs must be integers between 1 and 2147483647.")
    return value


def integer(value: Any, minimum: int, maximum: int) -> str:
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"Expected an integer between {minimum} and {maximum}.")
    return str(value)


def number(value: Any, *, minimum: float | None = None, maximum: float | None = None, positive: bool = False) -> str:
    try:
        valid = type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        valid = False
    if not valid or (minimum is not None and value < minimum) or (maximum is not None and value > maximum) or (positive and value <= 0):
        raise ValueError("Expected a finite numeric value within the documented bounds.")
    return format(value, ".17g")


def identifier(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_]{1,128}", value):
        raise ValueError("Use an exact ASCII template, command, science, or faction identifier returned by the game.")
    return value


def text_argument(value: Any, maximum: int = 512) -> str:
    if (not isinstance(value, str) or not 1 <= len(value) <= maximum
            or any(ord(char) < 32 or ord(char) > 126 for char in value)):
        raise ValueError(f"Expected 1-{maximum} printable ASCII characters without tabs or newlines.")
    return value


def boolean(value: Any) -> str:
    if type(value) is not bool:
        raise ValueError("Expected a boolean.")
    return "1" if value else "0"


def unit_ids(values: Any) -> str:
    if not isinstance(values, (list, tuple)) or not 1 <= len(values) <= 64:
        raise ValueError("Supply between 1 and 64 unit IDs.")
    ids = [object_id(unit) for unit in values]
    if len(set(ids)) != len(ids):
        raise ValueError("Unit IDs must be unique.")
    return ",".join(map(str, ids))


def target_location(x: Any, y: Any) -> list[str]:
    if (x is None) != (y is None):
        raise ValueError("Supply both x and y, or neither.")
    return ["", ""] if x is None else [number(x), number(y)]


def _arguments(method: str, values: tuple[Any, ...]) -> list[str]:
    if method == "set_control_mode" and len(values) == 1 and values[0] in {"realtime", "stepped"}:
        return [values[0]]
    if method in {"get_replay_status", "list_replays"} and not values:
        return []
    if method == "get_snapshot" and len(values) == 2:
        return [integer(values[0], 0, 100000), integer(values[1], 1, 256)]
    if method == "get_unit_profile" and len(values) == 1:
        return [str(object_id(values[0]))]
    if method == "set_rally_point" and len(values) == 3:
        return [str(object_id(values[0])), number(values[1]), number(values[2])]
    if method in {"save_replay", "play_replay"} and len(values) == 1:
        if not isinstance(values[0], str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}(?:\.rep)?", values[0]):
            raise ValueError("Replay names must be simple ASCII names, optionally ending in .rep.")
        return [values[0]]
    if method == "batch_orders" and len(values) == 3:
        entries, frame, age = values
        if not isinstance(entries, (list, tuple)) or not 1 <= len(entries) <= 32:
            raise ValueError("A batch contains 1..32 gameplay commands.")
        fields = [integer(frame, -1, 2147483647), integer(age, 1, 1800), str(len(entries))]
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != {"method", "arguments"}:
                raise ValueError("Each batch entry requires method and positional arguments.")
            name, arguments = entry["method"], entry["arguments"]
            if name not in {"unit_order", "use_command", "train_unit", "cancel_production", "build", "build_line",
                            "purchase_science", "set_rally_point", "attack_move"} or not isinstance(arguments, (list, tuple)):
                raise ValueError("Only normal gameplay commands are allowed inside a batch.")
            encoded = _arguments(name, tuple(arguments))
            fields.extend([name, str(len(encoded)), *encoded])
        return fields
    if method in {"get_state", "get_capabilities", "get_players", "get_player_actions", "list_maps", "list_saves", "get_session", "end_match",
                  "get_lobby", "enter_lan", "lan_host", "lan_accept", "lan_start", "lan_leave"} and not values:
        return []
    if method == "lan_join" and len(values) in (1, 2):
        host_ip = values[0]
        if not isinstance(host_ip, str):
            raise ValueError("Use the dotted IPv4 host address returned by get_lobby.")
        return [str(ipaddress.IPv4Address(host_ip)), *([boolean(values[1])] if len(values) == 2 else [])]
    if method == "enter_lan" and len(values) == 2:
        player_name, local_ip = values
        if player_name != "":
            player_name = text_argument(player_name, 12)
            if player_name != player_name.strip() or any(char in player_name for char in ",:;"):
                raise ValueError("LAN names cannot contain commas, colons, semicolons, or leading/trailing spaces.")
        if local_ip != "":
            if not isinstance(local_ip, str):
                raise ValueError("Local IP must be an assigned interface or loopback IPv4 address.")
            local_ip = str(ipaddress.IPv4Address(local_ip))
        return [player_name, local_ip]
    if method == "lan_set_slot" and len(values) == 6:
        slot, state, faction, color, team, start_position = values
        if state not in {"keep", "open", "closed", "easy", "medium", "brutal"}:
            raise ValueError("Slot state must be keep, open, closed, easy, medium, or brutal.")
        if faction != "":
            faction = identifier(faction)
        return [integer(slot, 0, 7), state, faction, integer(color, -1, 255), integer(team, -1, 3),
                integer(start_position, -1, 7)]
    if method == "lan_set_options" and len(values) == 3:
        return [text_argument(values[0], 240), integer(values[1], 0, 2147483647), boolean(values[2])]
    if method in {"get_actions", "get_object"} and len(values) == 1:
        return [str(object_id(values[0]))]
    if method == "get_world" and len(values) == 3:
        offset, limit, relation = values
        if relation not in {"all", "own", "ally", "enemy", "neutral"}:
            raise ValueError("Relation must be all, own, ally, enemy, or neutral.")
        return [integer(offset, 0, 100000), integer(limit, 1, 256), relation]
    if method == "get_map" and len(values) == 5:
        x, y, width, height, step = values
        result = [number(x), number(y), number(width, positive=True), number(height, positive=True), number(step, minimum=5)]
        if (math.floor(width / step) + 1) * (math.floor(height / step) + 1) > 4096:
            raise ValueError("Map request exceeds the 4096-sample limit; increase step or reduce the area.")
        return result
    if method == "get_events" and len(values) == 2:
        return [integer(values[0], 0, 9007199254740991), integer(values[1], 1, 256)]
    if method == "train_unit" and len(values) == 2:
        return [str(object_id(values[0])), identifier(values[1])]
    if method == "attack_move" and len(values) == 3:
        return [unit_ids(values[0]), number(values[1]), number(values[2])]
    if method in {"check_build", "build"} and len(values) == 5:
        builder, template, x, y, angle = values
        return [str(object_id(builder)), identifier(template), number(x), number(y), number(angle)]
    if method == "build_line" and len(values) == 7:
        builder, template, x1, y1, x2, y2, angle = values
        return [str(object_id(builder)), identifier(template), *map(number, (x1, y1, x2, y2, angle))]
    if method == "find_build_sites" and len(values) == 7:
        builder, template, x, y, radius, spacing, limit = values
        return [str(object_id(builder)), identifier(template), number(x), number(y), number(radius, minimum=0, maximum=2000),
                number(spacing, minimum=10), integer(limit, 1, 32)]
    if method in {"unit_order", "check_order"} and len(values) == 6:
        units, order, target_id, x, y, queued = values
        if order not in {"move", "attack", "attack_move", "guard", "stop", "repair", "gather", "enter", "exit", "capture",
                         "attack_ground", "force_attack", "force_move", "scatter", "get_repaired", "get_healed", "salvage",
                         "guard_no_pursuit", "guard_air"}:
            raise ValueError("Unknown unit order.")
        if queued is True and order != "move":
            raise ValueError("Only move supports queued waypoints.")
        return [unit_ids(units), identifier(order), integer(target_id, 0, 2147483647),
                *target_location(x, y), boolean(queued)]
    if method == "use_command" and len(values) == 6:
        source, command, target_id, x, y, angle = values
        return [str(object_id(source)), identifier(command), integer(target_id, 0, 2147483647),
                *target_location(x, y), number(angle)]
    if method == "cancel_production" and len(values) == 2:
        token = values[1]
        if isinstance(token, str):
            if not re.fullmatch(r"upgrade:[A-Za-z0-9_]{1,128}", token):
                raise ValueError("Named production cancellation must use upgrade:<UpgradeName> from the queue.")
        else:
            token = integer(token, 0, 2147483647)
        return [str(object_id(values[0])), token]
    if method == "purchase_science" and len(values) == 1:
        return [identifier(values[0])]
    if method == "set_paused" and len(values) == 1:
        return [boolean(values[0])]
    if method == "advance_game" and len(values) == 1:
        return [integer(values[0], 1, 1800)]
    if method in {"save_game", "load_game"} and len(values) == 1:
        if not isinstance(values[0], str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}(?:\.sav)?", values[0]):
            raise ValueError("Save names must contain 1-80 ASCII letters, digits, underscores or hyphens, optionally followed by .sav.")
        return [values[0]]
    if method == "start_skirmish" and len(values) == 5:
        map_name, faction, opponent_faction, difficulty, money = values
        if difficulty not in {"easy", "medium", "hard"}:
            raise ValueError("Difficulty must be easy, medium, or hard.")
        return [text_argument(map_name, 240), identifier(faction), identifier(opponent_faction), difficulty,
                integer(money, 0, 1000000)]
    raise ValueError("Unknown bridge method or wrong argument count.")


class GameBridge:
    def __init__(self, suffix: str = "local", timeout: float = 5.0, transport: Exchange | None = None):
        self.path = pipe_path(suffix)
        if not math.isfinite(timeout) or not 0.1 <= timeout <= 30:
            raise ValueError("Timeout must be between 0.1 and 30 seconds.")
        self.timeout = timeout
        if transport is None:
            from .winpipe import WindowsPipe

            transport = WindowsPipe()
        self.transport = transport

    def call(self, method: str, *values: Any) -> dict[str, Any]:
        arguments = _arguments(method, values)
        request_id = uuid.uuid4().hex
        request = ("\t".join(["GZH1", request_id, method, *arguments]) + "\n").encode("utf-8")
        if len(request) > MAX_REQUEST_BYTES:
            raise ValueError("Bridge request exceeds the size limit.")
        writing = method in MUTATING_METHODS
        try:
            response = self.transport.exchange(self.path, request, self.timeout)
        except BridgeError as exc:
            exc.delivery_unknown = writing and exc.delivery_unknown
            raise
        # A malformed/lost reply can follow a successfully submitted order. Never retry it automatically.
        if len(response) > MAX_RESPONSE_BYTES or not response.endswith(b"\n") or b"\n" in response[:-1]:
            raise BridgeError("invalid_response", "The bridge returned invalid response framing.", delivery_unknown=writing)
        try:
            envelope = json.loads(response.decode("utf-8"), parse_constant=lambda value: _invalid_constant(value))
        except (UnicodeError, ValueError) as exc:
            raise BridgeError("invalid_response", "The bridge returned invalid JSON.", delivery_unknown=writing) from exc
        if (not isinstance(envelope, dict) or type(envelope.get("protocol")) is not int
                or envelope["protocol"] != 1 or envelope.get("request_id") != request_id
                or type(envelope.get("ok")) is not bool):
            raise BridgeError("invalid_response", "Bridge response version or request ID does not match.", delivery_unknown=writing)
        if envelope["ok"]:
            if not isinstance(envelope.get("result"), dict):
                raise BridgeError("invalid_response", "Bridge result must be an object.", delivery_unknown=writing)
        else:
            error = envelope.get("error")
            if (not isinstance(error, dict) or not isinstance(error.get("code"), str)
                    or not isinstance(error.get("message"), str)):
                raise BridgeError("invalid_response", "Malformed bridge error.", delivery_unknown=writing)
            raise BridgeError(error["code"], error["message"],
                              delivery_unknown=writing and (error.get("delivery_unknown") is True or error["code"] == "internal_error"))
        return envelope


def _invalid_constant(value: str) -> None:
    raise ValueError("Non-finite JSON constant: " + value)
