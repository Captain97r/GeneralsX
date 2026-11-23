# Collecting LAN stutter logs

On this diagnostic branch, start a LAN match normally on each computer. Both
hosting and joining start a log automatically, including in Release builds.
Use the same diagnostic revision, edition, map and game data across the players.

After a stutter, note the approximate in-game time and which players noticed
it. Finish or leave the match normally. Collect **one `.log` per player from
that match**, including players whose game felt smooth. Zip them together.
The filename contains the UTC start time, the shared game seed, the
local lobby slot and a suffix to keep consecutive matches separate.

Logs are in **`LANStutterLogs` inside the game's user-data directory**, alongside
its saves and replays. The game also prints the exact path to stderr on startup
of the match. The usual Zero Hour paths depend on the operating system.

- Windows: open `shell:Personal` from the Run dialog to reach the active
  Documents folder, then `Command and Conquer Generals Zero Hour Data\LANStutterLogs`.
  A different configured user-data folder or a redirected Documents folder is
  supported.
- Native Linux: `$XDG_DATA_HOME/GeneralsX/GeneralsZH/LANStutterLogs`, or
  `~/.local/share/GeneralsX/GeneralsZH/LANStutterLogs` with the default XDG paths.

The header identifies the build, edition, platform, map checksum, seed and
local player slot. Numbered slots identify the players and connections.

## Reading the same stall on different computers

Match the seed and map checksum, then compare simulation frames. Slots are
zero-based. In a slot mask, `0x1` means slot 0, `0x2` slot 1, and `0x4` slot 2.
Arrays use this same slot order. `t_us` is monotonic microseconds since this
machine opened its log. Use frames when the computers' wall clocks disagree.

| Event | Meaning |
| --- | --- |
| `summary` | Approximately one second of observations when the game thread runs. Includes counts, peak stage times, network target rate, run-ahead in frames, frame-arrival cushion, and longest interval between simulation grants. Loading can produce long frames before any network polls. |
| `slow_loop` | At least 50 ms between the end of the previous engine iteration and the end of this one. `gap_us` is the gap before this iteration. `frame_us` is the iteration itself. `draw_us`, `client_us` (excluding draw), `net_us`, `logic_us` and `pacer_us` are separate portions. Remaining time includes event handling and other engine work. |
| `wait_open`, `wait_end` | An outstanding lockstep wait has reached 50 ms, or has just resolved. `wait_frame` identifies the blocked simulation frame. `wait_info` identifies slots whose frame announcements were missing, `wait_commands` identifies slots whose announced commands were incomplete, and `wait_disconnect` records a hold by disconnect handling. A client that resumes before the next poll can produce just `wait_end`. |
| `grant_gap` | At least 50 ms between locally authorized simulation ticks. `grant_gap_us` is the interval at this frame. Compare it with the render timings, peer waits and negotiated rate. |
| `router_plan` | The packet router lists each slot's reported FPS and ACK latency, the slowest slot, the target rates and run-ahead it chose, and the frame when the change takes effect. |
| `end`, `closed` | The match ended and buffered rows were drained. `dropped_records` counts rows discarded if the disk writer fell behind. |

For example, a `wait_info=0x2` on the host and a long `gap_us` on slot 1 a few
frames earlier are consistent with slot 1 stopping its game loop. The host may
continue briefly on buffered commands. If slot 1 kept running, compare its
outbound counters with the recipients' missing frames. `frame_info_high` is
the highest execution frame observed per slot. `queued_retries` counts queued
datagrams containing a previously sent command that requires acknowledgment.
Late ACKs can produce retries even when the original datagram arrived.
`frame_ack_ms` is the game's rolling frame-ACK estimate, initially 200 ms.

The writer flushes new records in the background roughly every 250 ms and on
normal exit. Each summary counts all slow loops and grant gaps, with individual
rows for the first four of each per summary interval. Peaks for each stage can
come from different iterations. A hard process termination can lose the tail.
