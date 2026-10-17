# KenshiCoop

Setup + Demo: [https://www.youtube.com/watch?v=OqwVRRZEYGM](https://www.youtube.com/watch?v=OqwVRRZEYGM)

Experimental **co-op multiplayer for [Kenshi](https://lofigames.com/)**, built as an
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) /
[KenshiLib](https://github.com/BFrizzleFoShizzle/KenshiLib) plugin.

One player hosts their world; a friend connects (LAN, direct UDP, or Steam P2P)
and plays their own squad inside it. The plugin replicates squads, NPCs, combat,
inventory and equipment, direct trades between the players' squads, items
dropped on the ground (both directions), base building and container contents,
one shared money pool, game speed, and more - and saves are coordinated: any save
either player makes becomes one shared save, streamed to both machines
automatically.

> **Status: work in progress.** This is a hobby project under active
> development. Expect rough edges, desyncs, and crashes. Sessions support up
> to 8 players (host + 7); 2 players is the most-tested setup.

## How it works

- `KenshiCoop.dll` is loaded into the game by RE_Kenshi. It hooks the engine via
  KenshiLib and drives all game mutation on the main thread.
- Networking is [ENet](https://github.com/lsalzman/enet) over UDP, with an
  optional Steam P2P tunnel (no port forwarding needed).
- The host is authoritative for the world; each client is authoritative for its
  own squad. See `docs/API_REFERENCE.md` for the full engine-control surface and
  wire protocol.

```
src/plugin/       The KenshiCoop plugin (net, sync/replication, engine facade, scenarios)
src/netproto/     Shared wire-protocol headers (plain C++03, compiled by everything)
src/nettest/      Standalone ENet console app (transport de-risking)
src/netsim/       Protocol simulator
src/prototest/    Wire-protocol unit tests
src/tunneltest/   Steam-tunnel socket-hook tests
scripts/          Build, deploy, session, and automated-test tooling (PowerShell)
docs/             Build guide, engine/API reference, replication pitfalls
third_party/      ENet patches, VC10 compat shim (deps are fetched, not committed)
```

## Try it (play with a friend)

Up to 8 players, one machine each. You set up the session **inside the game** with an
in-game panel (press **F2**): the host clicks **Host co-op** and then **Invite**
next to a friend's name, and the friend accepts the Steam invite (or clicks
**Join** next to the host's name). Nobody types or swaps a Steam ID, edits a
config file or runs a launcher script. (A tiny `coop_config.json` is only
needed for LAN / direct-UDP games.)

### Before you start (both players)

1. **Kenshi 1.0.65 (Steam)**.
2. **[RE_Kenshi 0.3.1+](https://www.nexusmods.com/kenshi/mods/847)** installed
   (free Nexus mod - it loads the co-op plugin into the game).
3. **Steam running and online** on both machines. That's the whole network
   setup: the connection is Steam P2P, so there's no port forwarding, no
   router configuration, and no IP addresses. (A direct-UDP mode is also
   available for LAN / port-forwarded games.)

### 1. Install the mod

Grab `KenshiCoop-kit.zip` from the
[latest release](https://github.com/nhoral/KenshiCoop/releases/latest) and
unzip it anywhere (both players). You do not need to clone this repository -
but if you did, the same kit is in [dist/mod-kit](dist/mod-kit).

The zip contains a single **`KenshiCoop`** folder. Copy that folder into your
Kenshi `mods` directory so you end up with
`<Kenshi>\mods\KenshiCoop\KenshiCoop.dll` (default Steam path:
`C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\`). Then launch
Kenshi and enable **KenshiCoop** in the Mods menu.

### 2. Connect in-game (press F2)

The Co-op panel works at the **main menu** (before you load a game) as well as
in-game, so the joining player doesn't need to load anything first.

1. **Host:** load the save you want to play, or start a new game - pick
   **Multiplayer (Wanderer xN)** matching your group size (x2 to x8, one squad
   per player) from the start list for a ready-made co-op start (see
   below). Press **F2** and click **Host co-op**. Your online Steam friends
   appear in the panel (friends already in Kenshi first); click **Invite** next
   to each friend's name to send them a Steam invite. The list stays open while
   you play, so you can invite more friends later.
2. **Join:** any of these works, straight from the **main menu** with no save
   needed:
   - accept the Steam invite notification, or
   - press **F2** - friends who are hosting appear under *Join a friend*; click
     **Join** next to the host's name, or
   - right-click the host in your Steam friends list and choose **Join Game**.

   If Kenshi isn't running when you accept the invite, Steam starts it and
   you join automatically once the main menu is up. The host streams its world
   to you on connect and you load right into it. (If you already have an
   identical copy of the host's save on disk, it's used as-is instead of
   transferring.)
3. The status line in the panel shows live state, and a status banner in the
   **top-left corner** shows it too - at the main menu as well as in-game, so a
   joining player can watch the transfer before the world loads. Click
   **Leave co-op session** (or **Stop hosting** before anyone joined) to leave.

The panel only lists your **online** Steam friends, six per page; **More
friends** pages through the rest. If the host runs a different KenshiCoop
version, their entry reads *different mod version* and joining is refused -
both players need the same release.

**Advanced (manual Steam ID / LAN):** click **Advanced** in the panel for the
original controls - Role / Transport / Connection toggles plus **Copy my Steam
ID** / **Paste friend's Steam ID** (a per-session clipboard swap; nothing is
written to disk). Use it when Steam invites aren't available, or for
LAN / direct-UDP: open `<Kenshi>\mods\KenshiCoop\coop_config.json`, set
`"transport": "udp"`, and put the host's address in `"ip"` / `"port"`. Then set
**Transport: UDP** and toggle **Connection** to **ONLINE**. The `ip`/`port` are
re-read whenever you go ONLINE, so no restart is needed after an edit.

### Good to know

- **You each control your own squad.** One squad tab per player, in join
  order: the host runs squad 1, the first friend to join squad 2, the next
  squad 3, and so on (a reconnecting friend gets the same squad back). Other
  players' squads are visible and synced on your screen, but answer only to
  them. Your save needs at least as many squad tabs as players - use a
  Wanderer x8 start, or split units into extra squad tabs in-game.
- **More than two players.** The host relays every player's state to the
  others, so the host's upload carries everyone - the host should have the
  best connection. A friend joining mid-game loads into the host's world; the
  players already in the game keep playing.
- **Co-op starts included.** The KenshiCoop mod ships four game starts (New
  Game -> pick one from the list), all the vanilla Wanderer start with the
  wanderers already split into separate squads, so the host gets squad 1 and the
  joining player squad 2 with no manual tab-splitting:
  - **"Multiplayer (Wanderer x2)"** - the plain version, vanilla in every other
    way. Authored by [zeroit789](https://github.com/zeroit789).
  - **"Multiplayer+ (Wanderer x2)"** - the same start with **500,000 cats** and both
    characters at **50 in every stat**, for skipping the early grind. Kenshi has
    a single player wallet and co-op shares it, so the 500,000 is the pair's
    combined purse, not 500,000 each. The stats are a floor applied when the
    world loads, so training past 50 sticks normally.
  - **"Multiplayer (Wanderer x8)"** / **"Multiplayer+ (Wanderer x8)"** - the
    same two starts with **eight** wanderers, one per squad: the host gets squad 1
    and joining players take squads 2-8 in join order. Fewer than eight players is
    fine - unclaimed squads simply stay with the host. The "+" version gives every
    character the same 50-in-every-stat floor and the shared 500,000 cats.
- **The joining player doesn't need the host's save.** The host picks the save
  (or starts a new game); when the join connects from the menu, the host's world
  is streamed over automatically. Already having an identical copy on disk just
  skips the transfer.
- **Saving just works.** Any save either player makes during a session becomes
  one shared save on both machines, streamed to the other side automatically.
  To resume next time, the host loads that save and goes online, and the join
  can reconnect straight from the main menu again.

### If something goes wrong

- **"The co-op plugin has not started"** - RE_Kenshi didn't load it. Check
  `<Kenshi>\RE_Kenshi_log.txt` for `KenshiCoop`; reinstalling
  [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) usually fixes it.
- **No connection (Steam)** - both Steams must be online (not offline mode).
  A friend only shows up under *Join a friend* while they are hosting (clicked
  **Host co-op**) and Steam reports them in Kenshi; the list refreshes every few
  seconds while the panel is open. With the Advanced manual flow, each side must
  have **Pasted** the *other* player's ID. Look for `[invite]` lines and
  `[steam] session ... active=1` in `<Kenshi>\KenshiCoop_*.log`.
- **"protocol mismatch" in the log** - one of you has an older build; both
  players should re-install from the same release.

The kit's `README.txt` has the full setup + troubleshooting list.

## Building

The plugin must be compiled with the **Visual C++ 2010 (v100) x64 toolset** (a
KenshiLib requirement). Full toolchain setup, gotchas, and install steps are in
[docs/BUILD_SETUP.md](docs/BUILD_SETUP.md). Short version, once prerequisites
are in place:

```bash
cmd //c scripts/build_plugin.cmd
```

Dependencies are fetched, not committed:

- KenshiLib + precompiled libs: clone
  [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps)
  into `third_party/KenshiLib_deps/`
- ENet: clone [lsalzman/enet](https://github.com/lsalzman/enet) into
  `third_party/enet/enet/` and apply the patches in `third_party/enet/patches/`
  (see `third_party/enet/README.md`)

## Development and testing

`scripts/` contains an automated two-client test harness: `dev_cycle.ps1`
rebuilds, deploys to two local installs, launches host + join, runs a named
scenario, and produces a numeric PASS/FAIL verdict from the two logs.
`regress.ps1` runs the scenario regression suite. See
[docs/BUILD_SETUP.md](docs/BUILD_SETUP.md) Parts D-E for details.

## Credits

- [BFrizzleFoShizzle](https://github.com/BFrizzleFoShizzle) - RE_Kenshi and
  KenshiLib, which make plugins like this possible
- [lsalzman/enet](https://github.com/lsalzman/enet) - UDP networking library
- [zeroit789](https://github.com/zeroit789) - the "Multiplayer (Wanderer x2)"
  co-op game start ([#15](https://github.com/nhoral/KenshiCoop/pull/15))
- Lo-Fi Games - Kenshi

## License

[AGPL-3.0](LICENSE). KenshiLib and RE_Kenshi are GPLv3; this plugin links
KenshiLib under GPLv3 section 13 (GPL/AGPL combination). Not affiliated with
Lo-Fi Games. Non-commercial fan project.
