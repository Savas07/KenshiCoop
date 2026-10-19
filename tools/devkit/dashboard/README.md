# KenshiCoop devtools dashboard

Live view of what every running KenshiCoop instance (host + join) thinks each
player-squad character is doing, side by side, with buttons that send dev
commands into the game. Python 3.12 standard library only; the page is a single
self-contained `index.html` (no CDN).

## Run

```
python tools\devkit\dashboard\server.py            # http://127.0.0.1:8765/
python tools\devkit\dashboard\server.py --udp 27850 --http 8765 --log session.jsonl
```

Enable devtools in the game (`"devtools": 1` in `coop_config.json`, or
`KENSHICOOP_DEVTOOLS=1`) on both instances; both send telemetry to
`127.0.0.1:27850` by default. Open http://127.0.0.1:8765/.

Without the game:

```
python tools\devkit\dashboard\fake_telemetry.py    # host + join, 2 hands each
```

The fake host's driven copy of "Bob" detaches its model every 20 s for 8 s
(hk lost, mesh frozen while the nameplate keeps moving, foot distance flat) so
the row goes red and the watchdog trips. `restore` on that hand re-attaches
it; `knockdown <hand> 1` sets BODY_DOWN; glide/overlay/watchdog/record flip the
instance flags. Options: `--udp`, `--detach-every`, `--detach-for`, `--duration`.

## server.py

| endpoint | what |
|---|---|
| UDP `127.0.0.1:--udp` | telemetry datagrams in (one JSON object each) |
| `GET /` | the page |
| `GET /events` | Server-Sent Events: every datagram (with `_rx` = receive wall-clock ms added), command replies, `{"k":"sent",...}` for each command sent, and an `{"k":"instances",...}` snapshot every second |
| `GET /instances` | the instances snapshot as JSON |
| `POST /cmd` | `{"instance":"<role>:<pid>","cmd":"<text>"}` -> UDP text to `127.0.0.1:<cmdPort>` of that instance |

Instances are keyed by `role:pid`; `cmdPort` comes from their `global`
datagram (commands to an instance that has not sent one yet return 404).
Commands go out from one dedicated UDP socket; instances reply to the source
address, so replies arrive there and are forwarded to the page. An instance
silent for more than 5 s is marked stale. `--log FILE.jsonl` appends every
datagram, reply and sent command, one JSON object per line.

## The page

- **Instances**: role, pid, version, peers, slew, speed, overlay / watchdog /
  recording state; buttons glide on/off, overlay on/off, watchdog on/off,
  record start/stop, ping, and mark with free text (Enter sends).
- **Hands**: one row per hand, one column group per instance (host view vs
  join view of the same character): own/author, regime, horizontal
  |mesh-np|, hk present, foot, gap, vlen, cspd, task/stask, bs (hover for the
  decoded flags). A row is red when, in any instance's view, |mesh-np| > 15 or
  hk is missing on a free (not carried / bed / cage / chained) upright
  (`!bodyIsDown`) body. Views older than 2 s are dimmed.
- **Selected hand** (click a row or a trip event): last 30 s of gap,
  |mesh-np|, foot and derived np speed, all instances overlaid; command
  buttons restore / endaction / knockdown 1 / knockdown 0 / halt / dump sent to
  the chosen instance (defaults to the instance driving that hand).
- **Events**: trips (with dump path), marks, command events, sent commands and
  replies, newest first.

Protocol (datagram fields, commands, regimes): `docs/DEVTOOLS.md`.
