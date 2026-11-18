# GeneralsX Zero Hour MCP controller

This local server exposes structured observations and gameplay commands from the loaded Zero Hour/Contra engine. An MCP client can inspect the world, find legal building sites, construct buildings, run production, issue unit orders, use loaded abilities and upgrades, and manage a single-player session without screenshots, camera positioning, or keyboard/mouse automation. Gameplay orders enter the normal game command pipeline.

The server supplies controls and observations; the connected agent supplies the strategy. **Real-time control is the default:** the game continues running during observations, tool calls, and model reasoning. Pausing and frame stepping are rejected in this mode. Optional bounded reaction rules run locally between model responses. Observations respect the local player's fog of war, stealth, and disguises. Replay and observer gameplay is read-only. Normal player orders use the engine's existing message path in single-player or multiplayer; mixed-executable multiplayer synchronization remains unverified. Save/load and skirmish session management are single-player operations. Separate LAN tools expose hosting, joining, slot and map settings, readiness, and starting a multiplayer match.

A returned `queued` result means the order was submitted; query state again to check its effect. The game can reject a previously valid action when the situation changes before execution. An engine stall can still prevent replies.

## Build the game bridge

Use the repository's normal Windows build prerequisites and add `-DRTS_ENABLE_MCP_BRIDGE=ON` when configuring. This feature currently requires the modern Windows build of GeneralsX Zero Hour. The bridge is off by default and uses Windows named pipes.

Run these commands from an x86 Visual Studio developer prompt with `VCPKG_ROOT` set to your vcpkg installation.

```bat
cmake --preset win32-vcpkg -B build-mcp-vcpkg -DVCPKG_TARGET_TRIPLET=x86-windows -DRTS_ENABLE_MCP_BRIDGE=ON -DSAGE_USE_DX8=ON -DRTS_BUILD_GENERALS=OFF -DRTS_BUILD_CORE_TOOLS=OFF -DRTS_BUILD_ZEROHOUR_TOOLS=OFF -DRTS_BUILD_GENERALS_TOOLS=OFF
cmake --build build-mcp-vcpkg --config Release --target z_generals --parallel 8
```

Keep the resulting executable separate from your normal game installation. It needs the usual game assets and runtime libraries. When a prepared package exists at `build-mcp-realtime-package`, launch it from PowerShell with the correct game-data path:

```powershell
$env:GENERALS_MCP_PIPE = 'local'
& 'C:\path\to\GeneralsGameCode\build-mcp-realtime-package\GeneralsXZH-MCP.exe' `
    -setCwd 'C:\path\to\Contra009' -assetPath 'C:\path\to\Contra009' -win -quickstart
```

To reproduce the local package after building, run `scripts\build\windows\package-mcp.ps1 -GameDirectory '<your game directory>' -PackageDirectory build-mcp-realtime-package` from the repository root. It creates `GeneralsXZH-MCP.exe`, launchers, a checksum manifest, and an example `codex-mcp.toml`; it does not edit the installed game or your Codex configuration. Close other ordinary Generals instances before launching the windowed build. Restart with the new executable to use new native features; reconnecting MCP cannot update an already running executable.

The installed Miles audio library crashed during startup in the automated test environment. `launch-contra-noaudio.cmd` uses the supported `-noaudio` flag to bypass audio initialization. The normal `launch-contra.cmd` keeps audio enabled. The live automated checks below use the no-audio path; normal audio startup has not been validated.

If the Windows Direct3D 8 driver cannot initialize a render device, pass `-GraphicsRuntimeDirectory '<path to the extracted DXVK x32 folder>'` to the packaging script. It copies the supplied Direct3D 8 and 9 DLLs next to the MCP executable. The two-peer LAN smoke used the x86 runtime from [DXVK v2.7.1](https://github.com/doitsujin/dxvk/releases/tag/v2.7.1).

`launch-contra-headless.cmd` runs the same game without a rendering window or audio. Use MCP `list_maps` and `start_skirmish` to begin playing. This supports skirmishes entirely through MCP. LAN lobby controls require the normal engine, such as `launch-contra-noaudio.cmd`, because the engine omits its lobby callbacks in headless mode. The window can render without the agent taking or interpreting any screenshots.

The environment variable enables the bridge for that game process. Without it, the bridge remains disabled. Use the same suffix for the MCP server; valid suffixes contain 1-64 ASCII letters, digits, underscores, or hyphens. The example opens `\\.\pipe\GeneralsZH.MCP.local`. Use different suffixes for different game processes.

## Install and run the MCP server

Python 3.11 or later is required. This package uses the official MCP Python SDK 1.x and has been tested with 1.28.1. Install into a local environment:

```powershell
Set-Location 'C:\path\to\GeneralsGameCode\tools\generals-mcp'
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e .
.\.venv\Scripts\python.exe -m generals_mcp --pipe local --timeout 5 --control-mode realtime
```

The last command starts a **stdio** MCP server and waits for protocol messages; it is not an interactive console. MCP clients normally launch this command themselves. Logging uses stderr, keeping stdout reserved for MCP. The server opens no HTTP port and does not launch, stop, or focus the game.

For Codex, add an entry like this to the relevant `config.toml`, adjusting the path if necessary:

```toml
[mcp_servers.generals_zh]
command = 'C:\path\to\GeneralsGameCode\tools\generals-mcp\.venv\Scripts\python.exe'
args = ["-m", "generals_mcp", "--pipe", "local", "--timeout", "5", "--control-mode", "realtime"]
tool_timeout_sec = 30
```

The `command`/`args` configuration follows the [official Codex MCP documentation](https://developers.openai.com/codex/mcp). This repository does not change your client configuration automatically. Run the game and sidecar under the same Windows account so they can access the local pipe.

## Tools

| Area | Tools |
|---|---|
| Discovery and state | `get_capabilities`, `get_game_state`, `get_available_actions`, `get_player_actions`, `get_unit_profile` |
| World and events | `get_snapshot`, `get_world`, `get_object`, `get_map`, `get_players`, `get_events`, `watch_game` |
| Construction | `check_build`, `find_build_sites`, `build`, `build_line` |
| Production and combat | `train_unit`, `cancel_production`, `unit_order`, `attack_move`, `use_command`, `purchase_science`, `set_rally_point`, `check_order` |
| Continuous control | `batch_orders`, `configure_reactions`, `cancel_reactions`, `get_realtime_status` |
| Single-player sessions | `list_maps`, `start_skirmish`, `set_paused`, `advance_game`, `list_saves`, `save_game`, `load_game`, `end_match`, `get_session` |
| Replays | `get_replay_status`, `list_replays`, `save_replay`, `play_replay` |
| LAN setup | `get_lobby`, `enter_lan`, `lan_host`, `lan_join`, `lan_set_slot`, `lan_set_options`, `lan_accept`, `lan_start`, `lan_leave` |

These 50 tools expose the loaded engine's supported operations. `unit_order` handles movement, attack, ground attack, forced attack/movement, guard, guard without pursuit, air guard, scatter, stop, repair/resume construction, gathering, capture, salvaging, getting healed/repaired, entering, and leaving containers. Queued movement adds waypoints. `use_command` accepts exact button names discovered from the loaded mod: upgrades, special powers, weapon abilities, selling, evacuation, hacking, overcharge, weapon switching, hijacking, sabotage, combat drops, and supported rail controls. Nested Contra powers are discovered through their owned menu holders, with the actual source object ID. `get_player_actions` separates the complete owned science inventory from faction-specific purchasable choices. `set_rally_point` works directly on producers even when no rally button is present. Unsupported or UI-only buttons are explicitly marked; target validation runs when issuing the action. Line construction uses `build_line` with two endpoints.

`get_snapshot` returns own objects, a visible-world page, player state, match result, and an event cursor from one simulation frame. Check `own_truncated`, `tracking_truncated`, and `world.next_offset`; pages from later calls can describe later frames. `get_world` includes supply sources, salvage crates, capturable structures, detected mines, visible projectiles, and scout drones. Invisible mines and other hidden enemies remain hidden. `get_object` adds detailed owned-unit queues, ammunition, cargo, upgrades, current orders, and rally points. `get_unit_profile` exposes combat rules, deployment, detection, healing, and ability diagnostics; enemy runtime state remains private. `check_order` gives per-unit eligibility and engine attack checks including range, pitch, and terrain line of sight. Eligibility is not a guarantee that a weapon will fire before circumstances change.

`get_map` returns a bounded terrain grid; each compact sample follows the returned `sample_fields` array, and unexplored terrain remains unknown. `get_events` uses a sequence cursor with bounded retention and distinguishes losing sight of an enemy from witnessing its destruction. New events cover ownership, construction, upgrades, ability and order state transitions. `unit_produced` correlates the exact producer ID and production queue ID with the new object ID. An order-state transition is an observation, not proof of successful completion of an arbitrary command. Projectiles are omitted from event history so transient effects do not crowd out important events. `get_players` reports the match result when the engine's victory conditions determine it.

Use current object IDs and exact template names returned by the game. Coordinates are map coordinates, not screen pixels. The bridge validates ownership, match mode, and order prerequisites again when the request reaches the game thread. Queries do not expose hidden enemies.

Every successful tool result contains `protocol`, `request_id`, `ok`, and a `result` object. Bridge errors are returned as MCP tool errors. Transport errors also include `delivery_unknown`: if true, an order may already have reached the game. Observe state before deciding whether to submit another order. The client never automatically retries an order.

The native bridge remembers the last 128 write request IDs for the current process lifetime. Repeating an identical cached request returns its original result; reusing its ID with different arguments is rejected. This is bounded duplicate protection, not persistence across restarts.

The timeout is an end-to-end deadline including connection, request write, and response read. Pending Windows I/O is cancelled and completed before handles and buffers are released. MCP cancellation waits for this bounded cleanup.

## Play continuously without a screen

1. Keep one MCP client/server connection open for the match. Discover maps/factions with `list_maps`, then call `start_skirmish`. Poll `get_session` until it completes; real-time matches start running.
2. Read `get_snapshot`, terrain where needed, and loaded actions for builders/producers. Cache stable template rules; refresh resources, targets, cooldowns, and availability when deciding.
3. Use `find_build_sites`, `check_build`, and `check_order` when needed. Submit independent gameplay commands with `batch_orders`, supplying the snapshot's `frame` and an acceptable `max_age_frames`. Stale batches are rejected before any entry is submitted. Entries otherwise succeed or fail individually; this is not an atomic transaction, and the whole batch must not be retried after partial success.
4. Set bounded `configure_reactions` rules for urgent known contingencies. Call `watch_game` with the last `next_since` cursor to wait for changes while the game and local reactions continue. On `history_lost`, refresh the snapshot. The LLM host must continue this decision loop until the match ends.
5. After victory/defeat, call `end_match` and poll `get_session`. The recorder is finalized before resetting the match. Inspect `get_replay_status`, call `save_replay` with a new name, and poll `get_session` for the completed copy. `list_replays` and `play_replay` support inspection and read-only playback from the idle shell.

Example `batch_orders` arguments (replace IDs and coordinates with current observations):

```json
{
  "expected_frame": 1200,
  "max_age_frames": 90,
  "commands": [
    {"tool": "set_rally_point", "arguments": {"object_id": 686, "x": 1300, "y": 700}},
    {"tool": "unit_order", "arguments": {"unit_ids": [770], "order": "guard_no_pursuit"}}
  ]
}
```

Example `configure_reactions` arguments:

```json
{
  "rules": [
    {"rule_id": "retreat_tanks", "trigger": "health_below", "unit_ids": [770, 771],
     "health_fraction": 0.3, "order": "move", "x": 1300, "y": 700, "expires_seconds": 120},
    {"rule_id": "new_dozer", "trigger": "unit_produced", "producer_id": 686,
     "production_id": 21, "delay_frames": 90, "order": "move", "x": 1100, "y": 650,
     "max_fires": 1, "expires_seconds": 90}
  ]
}
```

Rules support health thresholds, newly produced units, ownership acquisition, and idle units. They poll every 250 ms **plus bridge processing time**, fire at most once per matched unit, and are bounded by expiry and fire count. They are replaced by `configure_reactions`, cancellable, and scoped to the current match and server process. Manual orders get a five-second priority interval; active repair/healing and protected busy states suppress automatic orders. Rejected or uncertain writes are consumed, never retried automatically. Match changes, completion, or a bridge failure stop the rules. They respect a human pause; they never pause or resume the simulation themselves. Keep a single controlling server per game pipe.

For reproducible testing only, launch the server with `--control-mode stepped`. This explicitly enables the old start-paused and `advance_game(1..1800)` behavior. Real-time mode rejects those clock controls in both the MCP server and native bridge. An already paused match can be resumed explicitly with `set_paused(false)`.

When assigning workers, check `current_order.has_builder_task` as well as `idle`. A worker can have a pending construction, repair, or fortification task in a separate engine state machine. Automatic reaction rules leave these workers alone. For an owned unfinished structure, `builder_id` identifies the assigned worker, or is null if the structure has no worker with a matching pending build task. If `construction_percent` stops advancing and the builder is absent, choose an available worker, validate `check_order(order="repair", target_id=...)`, then issue the same `unit_order` to resume the existing scaffold. Observe progress before sending the worker back to gather.

## LLM latency and unattended play

Disabling reasoning alone does not make an RTS controller responsive enough. One decision still includes snapshot transfer, model and network latency, tool dispatch, and the engine's command delay. `get_realtime_status.bridge_latency_ms` measures only native bridge calls; it is not model latency or an end-to-end reaction guarantee. Measure both decision latency and stale-batch rejection rate under actual gameplay load.

Use a fast model at low or no reasoning for routine decisions where it remains accurate, and allow more deliberation for strategic decisions. Available reasoning settings depend on the model. Keep tool outputs and model responses concise, use one snapshot instead of many sequential observations, cache stable data, and batch independent orders. These choices follow the official [latency guidance](https://developers.openai.com/api/docs/guides/latency-optimization) and [reasoning guidance](https://developers.openai.com/api/docs/guides/reasoning). The local reaction controller covers configured contingencies while a model call is pending; it is not a complete tactical AI and cannot guarantee a sub-second response during an engine stall.

The LLM host must maintain the MCP connection, schedule successive decisions or event waits, enforce a match/runtime budget, and reconnect by observing fresh state rather than replaying old orders. Attaching an MCP server alone does not implement that loop. This package remains model/provider independent and does not make paid model API calls. Benchmark the chosen model on full unpaused matches before judging playing strength.

The command-line client is useful when an MCP host cannot dynamically attach a server. It uses the official MCP client over stdio for every tool call, including discovery; it does not bypass MCP or use desktop input:

```powershell
.venv\Scripts\python.exe -m generals_mcp.cli --pipe local list-tools --names-only
.venv\Scripts\python.exe -m generals_mcp.cli --pipe local call get_game_state
.venv\Scripts\python.exe -m generals_mcp.cli --pipe local call build --args-file build-order.json
.venv\Scripts\python.exe -m generals_mcp.cli --pipe local batch --file orders.json
```

The CLI batch file contains `[{"tool":"tool_name","arguments":{...}}, ...]`. This convenience command makes sequential MCP calls and stops at the first error; use the `batch_orders` tool for a native batch at one frame. CLI invocations start a temporary server, so their reaction rules disappear when that invocation exits. Use a persistent MCP host for continuous control. Coordinates and rotation angles are world coordinates and radians.

## LAN lobby compatibility

From the idle main menu, call `enter_lan` and poll `get_lobby` until its transition finishes. Optional `player_name` and `local_ip` arguments apply to this LAN session without changing saved preferences. Use an assigned local interface address, such as the VPN address, when needed.

To host, call `lan_host`, then inspect `current_game` for your slot and current options. Use `lan_set_options` with a loaded map and a value from `starting_cash_choices`; use `lan_set_slot` to configure your own slot or AI opponents. Other human players control their own slots. To join, use a discovered host IP from `get_lobby.games`, or explicitly request `lan_join(host_ip, direct=true)` for the stock Direct Connect path. After configuring your slot, call `lan_accept`. The host calls `lan_start` once everyone is ready. Read observations to confirm the match has begun. `lan_leave` backs out of game setup to discovery, then from discovery to the main menu; it does not exit an active multiplayer match.

These tools invoke the normal lobby lifecycle and network requests. Successful submission is asynchronous: always inspect the returned lobby state before issuing the next dependent action. Campaign menus, audio/video preferences, and other cosmetic UI settings are outside this gameplay interface.

The lobby tools use the GeneralsX LAN implementation and its existing directed packet routing. GeneralsOnline is outside the MCP lobby interface.

Use matching game data and maps on every peer. Mixed executables can advance differently even when the lobby allows a join. A two-peer loopback test does not establish a remote VPN connection or full mixed-build match compatibility.

## Verification

Run automated tests from this directory:

```powershell
$env:PYTHONPATH = 'src'
python -m unittest discover -s tests -v
```

Tests cover protocol validation, typed tool schemas, an official MCP client/server stdio exchange, and Windows pipe transfer/disconnect/timeout behavior. Their game responses are fixtures; they do not constitute live gameplay validation and do not send orders to a running game.

The real-time integration test starts a separate headless Contra process and uses one persistent official MCP session:

```powershell
tools\generals-mcp\.venv\Scripts\python.exe Tests\AgentBridge\realtime_game_smoke.py --exe build-mcp-realtime-package\GeneralsXZH-MCP.exe --assets 'C:\path\to\Contra009' --output build-mcp-tests\realtime-control-new
```

Use a new output directory each run. It checks a continuously advancing clock, rejected pause/step requests, mixed-unit order diagnostics, same-frame batches and stale-plan rejection, rally points, faction sciences and nested powers, exact production correlation and automatic follow-up movement, then replay finalization/save/list/play/stop. Its `result.json` includes game-frame progression, MCP call latency, native bridge latency, checks, and `passed`; `calls.jsonl` records the evidence. No model API is called, so these timings do not measure an LLM's decision latency. The test uses private userdata and only stops its own child process.


The separate native integration suite exercises the real C++ pipe transport with a stub game adapter. From the repository root, after building `Tests/AgentBridge` into `build-agent-pipe-tests`, run:

```powershell
tools\generals-mcp\.venv\Scripts\python.exe Tests\AgentBridge\test_native_pipe.py
```

It checks fragmented and malformed requests, command deduplication, large responses, idle clients, slow readers, and nonblocking game-thread updates. Every test uses a uniquely named hidden harness process; it does not connect to the game.

The original gameplay smoke test checks the four original tools against a separate headless Contra skirmish:

```powershell
tools\generals-mcp\.venv\Scripts\python.exe Tests\AgentBridge\game_smoke.py --exe build-mcp-realtime-package\GeneralsXZH-MCP.exe --assets 'C:\path\to\Contra009' --output build-mcp-tests\verification --clean-exit
```

It uses a unique pipe and private userdata directory, starts a fixed skirmish, and supplies a reactor and barracks as test fixtures. It verifies the original tool names, reads live state, trains `Cybr_AmericaInfantryRanger` (Cyborg), observes the newly created unit, attack-moves it, and verifies its position changes. Invalid object IDs, unsupported production, and out-of-map orders must be rejected. `--clean-exit` also checks normal shutdown after 30 seconds.

The expanded integration test starts at the menu, creates its skirmish through MCP, constructs buildings through normal builder orders, inspects terrain and resources, trains and moves infantry, cancels production, purchases a science, researches and cancels upgrades, saves/loads, and ends the match:

```powershell
tools\generals-mcp\.venv\Scripts\python.exe Tests\AgentBridge\full_game_smoke.py --exe build-mcp-realtime-package\GeneralsXZH-MCP.exe --assets 'C:\path\to\Contra009' --output build-mcp-tests\full-control-verification
```

Every observation and command in this test goes through an official MCP client and the stdio server. It uses **zero screenshots, zero mouse/keyboard inputs, and zero injected gameplay fixtures**. Its private userdata and separate process are guarded by explicit test environment variables; normal launchers clear them. Test cleanup stops only that child process. Evidence is written incrementally to `result.json`; only `passed: true` indicates a completed run. These tests do not establish every mod-specific ability, audio startup, or mixed-build multiplayer synchronization.

The LAN test starts isolated windowed peers with separate userdata, pipes, and loopback addresses. With `--two-peers`, it exercises hosting, joining, slot/options changes, Accept, and starting a real multiplayer match through MCP. All observations and commands use MCP; no images or desktop input are used:

```powershell
tools\generals-mcp\.venv\Scripts\python.exe Tests\AgentBridge\lan_game_smoke.py --exe build-mcp-realtime-package\GeneralsXZH-MCP.exe --assets 'C:\path\to\Contra009' --output build-mcp-tests\lan-control-verification --two-peers --windowed --check-leave
```

`--check-leave` additionally verifies that leaving and rehosting preserves the explicitly selected interface and name. The two-peer test also moves an owned dozer through the normal multiplayer command path and verifies that both distinct players continue advancing for more than 120 frames afterward. Without `--two-peers`, it checks hosting against an AI opponent. This test does not use the original GeneralsX executable or validate a remote VPN connection. Its `result.json` records completed checks, and failures include a traceback in `failure.log`. Windowed tests need access to the Windows graphics session; a restricted execution token can cause Direct3D initialization to fail before the bridge starts.

## Local bridge protocol

Each connection carries one printable-ASCII request line and one UTF-8 JSON response line:

```text
GZH1<TAB>request_id<TAB>method[<TAB>argument...]<LF>
```

`get_capabilities` lists native method names. Typed MCP tools validate and serialize positional wire fields; optional targets use ID `0` and absent coordinates use empty fields. Request IDs are generated as UUID hex strings. Requests are limited to 16 KiB and replies, including the newline, to 1 MiB. Embedded tabs/newlines and invalid numeric values are rejected before transmission. Every mutating method has bounded request-ID deduplication. The C++ bridge invokes the adapter on the game thread; pipe I/O does not run gameplay code on an MCP worker thread.
