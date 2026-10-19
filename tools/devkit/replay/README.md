# kcrecdump / kcreplay — packet recordings

The plugin's packet recorder (`src/plugin/devtools/PacketRecorder.cpp`) writes every
ENet packet a session sends or receives to a `.kcrec` file. These two tools read
those files:

- **kcrecdump**: prints a recording as JSON lines, so you can grep, diff or plot it.
- **kcreplay**: plays one side of a recording into a live peer. You can reproduce a
  sync bug without the second machine, the second player or Kenshi on the other end.

## Build

```
tools\devkit\replay\build.cmd
```

The script uses the plugin's VC++ 2010 (v100) x64 toolchain. If the toolchain isn't
installed in the default location, set `VS10` and `SDK` first:

```
set VS10=C:\Users\savas\Documents\kc_tc\vs10
set SDK=C:\Users\savas\Documents\kc_tc\sdk71
tools\devkit\replay\build.cmd
```

The output is `tools\devkit\replay\bin\kcrecdump.exe` and `kcreplay.exe`. The `bin\`
directory is gitignored. Both tools include `src/netproto/Wire.h` and decode packets
with the `PROTOCOL_VERSION` they were built against. Rebuild them after a protocol bump.

## Recording

There are two ways to start a recording:

- **Armed.** Turn recording on in the devtools config; see `docs/DEVTOOLS.md`. Every
  net session (host listening, or join connecting) then writes
  `<dir>\YYYYMMDD_HHMMSS_<host|join>.kcrec`. The file closes when the net thread exits.
- **On demand.** Send `record start` or `record stop` to the instance's devtools
  command port. The file being written appears as `"rec"` in the global telemetry
  line.

When nothing is recording, the cost is one interlocked read per packet. While
recording, packets go through a 64 KB buffer that is flushed at most once per second.

The `t` field is milliseconds since the file was opened. What `peer` means depends
on the side:

| side | `in` records | `out` records |
|------|--------------|---------------|
| host | the sender's player id. `0` means the join hasn't been welcomed yet, which is how its HELLO arrives. | the target id. `255` means a send to every join, including star relays. A relay appears as an `in` record from N followed by an `out 255` record with the same bytes. |
| join | always `0` (the host) | always `0` |

The file format is documented in `src/plugin/devtools/PacketRecorder.h`. The reader
is `kcrec.h`.

## kcrecdump

```
kcrecdump <file.kcrec> [--peer N] [--dir in|out] [--types 4,5,29-36]
```

- **Line 1** is the file header: protocol, role, start time (UTC), plugin version,
  note, record count, duration and a truncated flag.
- **Every following line** is one record:

  ```
  {"t":564,"dir":"in","peer":1,"ch":0,"rel":1,"type":5,"name":"EVENT","len":54,
   "event":1,"ev":"KNOCKOUT","owner":1,"eventId":5,"subject":"1,3332275456","actor":"7,99","arg":12.500}
  ```

- **A per-type in/out count** goes to stderr.

Packets are decoded as follows:

- **ENTITY_BATCH:** `owner`, `sendMs`, `epoch`, `count`, plus `entities[]`. Each
  entity has `hand` (`"index,serial"`, the same format as the log lines), `x/y/z`,
  `heading`, `bs` (bodyState), `down`, `task`, `rawTask`, `cSpeed`, `cMotion`,
  `cMoving`, `subj` and `htype`/`hcont`.
- **EVENT:** `event`/`ev`, `owner`, `eventId`, `subject`, `actor` and `arg`.
- **Other decoded types:** HELLO (version, identity), WELCOME (playerId), TIME_PING,
  TIME_PONG, PEER_STATE, ROSTER and CAM_HINT.
- **Everything else:** `owner` only.

Filters combine with AND. Examples:

```
rem one body's track as received by the host
kcrecdump rec.kcrec --dir in --types 4 | findstr "1,3332275456"
rem every reliable event in either direction
kcrecdump rec.kcrec --types 5
```

## kcreplay

```
kcreplay <file> --as join [--host 127.0.0.1] [--port 27800] [--peer N]
                [--speed 1.0] [--skip-types 29-36] [--linger 2] [--wait 10]
kcreplay <file> --as host [--port 27800] [--peer N] [--speed 1.0]
                [--skip-types 29-36] [--linger 0] [--wait 120]
```

### `--as join` (stands in for a join, connects to a real or replayed host)

1. Connects and sends the recorded HELLO of peer N. If the recording has no HELLO, it
   sends a fresh `HelloPacket` with `PROTOCOL_VERSION`.
2. Waits for WELCOME.
3. Sends what that join sent, with the recorded relative timing, channel and
   reliability:
   - **From a host-side recording:** every `in` record from peer N after N's first
     WELCOME. N defaults to the first welcomed join.
   - **From a join-side recording:** its `out` records after its WELCOME.
4. Drains and counts the host's traffic. Stays `--linger` seconds after the stream
   ends (default 2), then disconnects.

### `--as host` (stands in for the host, a real or replayed join connects to it)

1. Listens on `--port`.
2. Answers HELLO with WELCOME. The playerId comes from the recording's first WELCOME;
   if there is none, it is 1.
3. Answers TIME_PING with TIME_PONG, using the local time-of-day clock as NetLink does.
4. Sends what the host sent:
   - **From a join-side recording:** its `in` records after the WELCOME.
   - **From a host-side recording:** `out` records to peer N or 255.
5. Exits when the join disconnects. Pass `--linger S` to cap the time spent after the
   stream ends.

### Options and progress

| option | meaning |
|--------|---------|
| `--speed F` | scales the recorded timing. `2` is twice as fast; `0` means no pacing. |
| `--skip-types L` | drops packet types, e.g. `29-36` for the save/load transfer. |

Progress (`sent x/y`, `received n`) prints every 5 s. The final lines show the
replay's timing accuracy (`lateness avg/max`) and the received packets by type.

HELLO and WELCOME are never re-sent from the stream, because the handshake is live.
`--as host` also drops recorded TIME_PONGs.

### Example: reproduce a join-side desync on one machine

```
rem host recording from the bad session; replay join 1's stream into a fresh host
kcreplay 20261019_153927_host.kcrec --as join --host 127.0.0.1 --port 27800 --peer 1
```

### Example: offline loopback, no Kenshi

```
kcreplay some_join.kcrec --as host --port 27911
kcreplay some_host.kcrec --as join --host 127.0.0.1 --port 27911
```

## Limits

- **Payloads are replayed verbatim.** `ownerId`, hands and epochs are not rewritten.
  If the live host assigns a different id than the recorded one, kcreplay warns, and
  the batches still claim the recorded owner.
- **HELLO carries the recorded identity.** The recorded HELLO includes the recorded
  identity token, so the live host reserves that player id for it.
- **Protocol versions must match.** A recording from another protocol version is
  rejected at HELLO by a live peer. The tools warn when the versions differ.
- **The replay is one-sided.** It does not react to what the other side sends, for
  example SPAWN_REQ or save/load requests.
- **`--as host` serves one join.** Later HELLOs are counted but not welcomed.
