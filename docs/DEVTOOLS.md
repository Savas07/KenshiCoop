# Developer tools

Tools for seeing, measuring and reproducing co-op sync bugs (frozen models,
nameplates wandering off without their model, sliding without animation,
teleports). They are compiled into every DLL and do **nothing** until switched on:
no sockets, no per-tick engine probes, no files, no labels.

| Tool | Where | What it answers |
|---|---|---|
| In-world overlay | plugin (`src/plugin/devtools/DevOverlay.cpp`) | Is the MODEL where the nameplate is? Does the body have a physics character? Which drive regime is it in? |
| Telemetry + dashboard | plugin (`DevTools.cpp`) + `tools/devkit/dashboard/` | Live per-character state from both machines side by side, 30 s charts |
| Command channel | plugin (`DevTools.cpp`) + dashboard buttons | Try an engine call (restore physics, end get-up, halt...) on one character, without rebuilding |
| Desync watchdog | plugin (`DevTools.cpp`) | Catches the first frame a rule breaks and dumps the last 6 s of that character |
| Packet recorder | plugin (`PacketRecorder.cpp`, NetLink hooks) | Every packet of a session, in and out, to a `.kcrec` file |
| Recording dump / replay | `tools/devkit/replay/` (`kcrecdump`, `kcreplay`) | Read a recording; replay a friend's side of a session into ONE local game |

## Switching them on

`mods\KenshiCoop\coop_config.json` (or the environment variable in brackets):

```json
{
  "devtools": 1,
  "devOverlay": 1,
  "devRecord": 1,
  "devWatchdog": 1,
  "devPort": 27850
}
```

- `devtools` (`KENSHICOOP_DEVTOOLS`): master switch, default 0. The other flags
  do nothing without it.
- `devOverlay` (`KENSHICOOP_DEV_OVERLAY`): overlay on at start. **Ctrl+F9**
  toggles it in game.
- `devRecord` (`KENSHICOOP_DEV_RECORD`): record every network session.
- `devWatchdog` (`KENSHICOOP_DEV_WATCHDOG`): desync rules, default 1 when
  `devtools` is on.
- `devPort` (`KENSHICOOP_DEV_PORT`): telemetry target `127.0.0.1:<port>`.

Values `1`/`true`/`on` enable a flag. The log confirms with
`[dev] devtools ON overlay=.. record=.. watchdog=.. port=.. dir='..'`.

Files go next to the log (`KenshiCoop_host.log`, normally the Kenshi folder):
`kc_records\` and `kc_dumps\`.

**For a friend's bug report:** they set `"devtools": 1, "devRecord": 1`, play
until it happens, and send `KenshiCoop_host.log`, the newest `kc_records\*.kcrec`
and `kc_dumps\*.json`.

## Overlay

Per player-squad character, at its feet:
`<name> <regime> hk=<0|1> t=<task> gap=<u>`, plus `DETACHED <u>` when the model
is more than 15 u (horizontal) away from the nameplate.

- Green: normal. Yellow: glide, snap, or walking with gap > 30. Red: detached, or
  upright and free with no physics character (`hk=0`) - the frozen-model bug.
- `[M]` (green) marks the model (pelvis bone), `[H]` (red) the Havok physics body.
  Both appear only when more than 2 u away from the nameplate.

Kenshi draws the nameplate and our numbers from `Character::getPosition()`; the
model is drawn from the Havok character. When that is missing, moving the
position moves only the nameplate. That is why the overlay reads the bones.

## Dashboard

```
python tools\devkit\dashboard\server.py [--log session.jsonl]
```

Open <http://127.0.0.1:8765/>. Both Kenshi windows (host and join on one PC, or
just yours in a real session) appear as instances. The hands table shows each
instance's view of every character side by side; a row turns red when the model
is detached or the physics character is missing. Click a row for 30 s charts
(gap, |model - nameplate|, foot swing, speed) with both machines overlaid, and
per-character buttons. `fake_telemetry.py` drives the page without the game.
Details: `tools/devkit/dashboard/README.md`.

Regimes (`reg`): `own` (we stream it), `walk` (move orders), `glide` (placement
+ gait mirror), `snap` (hard teleport this tick), `rest`, `down`, `carried`,
`carrier`, `furn`, `crawl`, `combat`, `paused`, `refused` (cell authority), `none`.

## Commands

UDP text to the instance's `cmdPort` (shown in the dashboard and the log), or the
dashboard buttons. `<hand>` = `index,serial` as in log lines (`hand=1,3332275456`).

| Command | Effect |
|---|---|
| `restore <hand>` | `CharMovement::restore` (rebuild a missing physics character) |
| `endaction <hand>` | end the current task (e.g. a stuck get-up) |
| `knockdown <hand> <0\|1>` | knock out / stand up |
| `halt <hand>` | stop movement |
| `move <hand> <dx> <dz>` | player move order to (current x+dx, z+dz): scripted walks for repros |
| `glide <on\|off>` | squad glide regime on/off |
| `overlay <on\|off>`, `watchdog <on\|off>` | toggles |
| `dump <hand>` | write that character's last 6 s to `kc_dumps\` now |
| `mark <text>` | `[dev] MARK <text>` in the log (bookmark a moment) |
| `record <start\|stop>` | start/stop a packet recording now |
| `ping`, `help` | |

## Watchdog rules

Each trip: one log line `[watchdog] TRIP rule=..`, a dump file
`kc_dumps\<time>_<role>_<rule>_<index>_<serial>.json` (6 s of samples + global
state) and a dashboard event. At most once per 10 s per character and rule.

| Rule | Fires when |
|---|---|
| `model_detached` | model > 15 u from nameplate for 500 ms (not carried) |
| `no_physics` | driven copy upright and free with no physics character for 1 s |
| `frozen_copy` | owner moving (> 30 u/s) for 700 ms, our copy moved < 1 u |
| `big_gap` | copy > 100 u from the newest stream sample |
| `stale_getup` | copy stuck in a get-up task (28/65/66) for 1 s while the owner walks |
| `slide_no_anim` | copy moving > 20 u/s for 1 s with no foot swing (not down/carried) |

## Recording and replay

`devRecord` writes `kc_records\YYYYMMDD_HHMMSS_<host|join>.kcrec` per session
(format in `src/plugin/devtools/PacketRecorder.h`). Build the tools once:

```
set VS10=C:\Users\savas\Documents\kc_tc\vs10
set SDK=C:\Users\savas\Documents\kc_tc\sdk71
tools\devkit\replay\build.cmd
```

- `kcrecdump <file> [--peer N] [--types 4,5]`: JSON lines (entity batches with
  hands, positions, body state, task, motion; events; handshake).
- `kcreplay <host-side.kcrec> --as join --host 127.0.0.1 --port <port> --peer 1`:
  acts as the recorded join. Start your game as host on the same save first; the
  copy of the friend's character then does exactly what it did in the session.
- `kcreplay <join-side.kcrec> --as host --port <port>`: the other direction.

Measured (2026-10-19): a 95 s host-side recording of a carry/KO session replayed
into a lone host; its copy of the join's character stayed within median 0.3 u
(p95 3.4 u) of the recorded path, through the KO, carry and walk phases.

Details: `tools/devkit/replay/README.md`.

## Limits

- Overlay labels exist for at most 24 characters.
- A recording of a game that is killed (not quit) can miss its last second.
- Replay reproduces what the OTHER side sent; the local world is whatever your
  save holds, so start from the save the session used.
