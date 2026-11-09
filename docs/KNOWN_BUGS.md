# Known bugs

Open issues as of v0.59.0 (2026-10-23), after the item, NPC-movement and combat
batches. "Pre-existing" means the same failure
reproduces on a build without the change under test. Scenario names refer to
`scripts/scenarios.psd1`; run one with `kc_tc\run_locked.py --scenario <name>`.

## Player-reported

- **Backpacks and items vanish from non-host inventories** (Workshop comment:
  "Backpacks and Items will disappear from inventories, from people besides the
  host"). Fixed after 0.58.1, in two parts. First, the host never created a
  backpack a join character acquired, so the host's save lost the bag and its
  contents (pitfalls §27, `inv_join_bag`). Second, a snapshot past 64 entries
  (bag contents included) was cut off, so its tail never reached the host's
  copy. The cap is now 255 entries (protocol 66). `inv_bigstack` gives a join
  character 100 entries and the host's copy matches (run 20261023_161843_s2). A
  64-cap build stops the host at 64 (run 20261023_170252_s2). A character past
  255 entries is still cut off.

## Pre-existing failures

- **Join main-thread freeze in the Holy Nation town.** The main log stops right
  after a 3rd first-chance `InvalidParametersException@Ogre`, while the net
  thread keeps logging CLOCKSYNC and reconnects. Seen in about 2 of 25 chase runs
  earlier, and in 0 of 30 later. Cause unknown. The main-thread watchdog
  (`core/HangWatch.h`, on by default; added after 0.59.0, not released yet)
  now logs the main thread's stack as
  `[hang]   #NN module+0xRVA` lines after 20 s without a gameplay tick, again
  at 60 s, and `[hang] main thread ticking again` if it recovers. The next
  freeze will name where it is stuck.
- **`world_parity`** (probe): 4 of 6 runs pass after the furniture fixes below
  (2 of 6 before; runs 20261024_012704 to _013306_s3). Still open:
  - A chained, KO'd join PC sits a constant 7-11 u from its owner on the host
    while both sides chain it to the same pole (`pc-bad: Flashbox med=7.3`, run
    20261024_012709_s3; 7.9 and 11.2 u before the fixes). The down path's
    teleport (`applyRaw` past 2 u) does not hold a chained body, and the host
    copy's chain owner changed to another guard (1,40039064 -> 1,900146496)
    within 250 ms of the enter while the join's stayed. Needs RE on how the
    engine anchors a chained body.
  - Near position 0.798 against 0.8 (run 20261024_013306_s3): Holy Sentinels
    40-375 u off with no combat in the judged window, the class seen on
    baseline (0.791, run 20261023_192313_s2).
  - The host's Fuu is census-parked on the join 500-1100 u off, in the two slot-1
    runs only. The furniture events match on both sides in all six runs, so the
    "Fuu stays in furniture" report did not recur.
- **Test saves drift.** Every run's session start writes the world back into
  the save it loaded (the host saves for the save transfer), and nothing
  restores the fixtures. `bedcage1` no longer has a bed or cage within 60 u of
  the host's leader: `bed_put`, `cage_put` and `cage_peer_sync` fail with
  `author put never landed` on 0.59.0's code too
  (runs 20261024_012208 and _012210_s2). `pole_put` fails the same way on
  `pole1`. Regenerating them (`KENSHICOOP_SETUP=bedcage` with
  `KENSHICOOP_BAKESAVE`) overwrites those saves. Not done yet.
- **`combat_battle`**: `combat_snap_rate` fails 2 of 4 runs on 0.59.0's drive
  code (maxPersist 7 and 7, limit 6; runs
  20261024_003619 to _003624_s3), so the earlier "flaky" entry is a steady
  rate. The combat_order fix below measures the same (1 of 3, mean maxPersist
  5.3 against 5.2).
- **`assault_mint` parity** (advisory; the gate, `proxy_drift`, passes): the
  host streams a combat target for a body its own `readCombat` reports at
  peace. Run 20261023_194659_s3: 23 of 56 pairs fight on the join only.
- **`combat_crowd`: the host's crowd sometimes never queues** (2 of 12 runs;
  20261023_230005, before this session's drive changes, and
  20261024_070235_s3). The host logged 1 waiting-stance sample, against 17 in a
  passing run, so the gate fails with "host streamed only 1 WAITING sample".
  This is on the host side, upstream of any join replication. Not investigated.
- **Other oracles may miss re-keyed bodies.** `combat_crowd`, the fight
  disagreement measure (`Combat.ps1`, `Measure-FightDisagreement`) and the
  `Npc.ps1` pose/task gates pair host and join series by full hand, container
  included. A body the fight re-keys on the join drops out of them, as it did
  from `combat_order`. Not audited.
- **`save_probe`** with `KENSHICOOP_SAVE_SYNC=0`: the join never finishes
  loading. Not investigated; probably the run setup rather than the mod.
- **`xfer_block`** (diagnostic, tier none): the cross-owner trade veto lets the
  drag through (`blockOk=0 give=1`), with or without the backpack fix. No player
  impact: the veto is opt-in (`KENSHICOOP_BLOCK_XFER=1`) and off by default.
- **`load_sync`**: the host's probe building is never placed (`[build]
  probe-place no template`), so the gate has no building to carry across the
  load and fails with `hostSites=0 joinSites=0`. Same failure with and without
  the hang watchdog (runs 20261023_212540 and 20261023_211821). The coordinated
  load itself completes on both clients; the join's world swap took 9.5 s.

## Fixed after 0.59.0 (not released)

- **Security: a connected peer could read files outside the save folder**
  (private report, Oct 2026; affects every release up to 0.59.0). The host
  took save names from join packets (`SAVE_REQ`, `LOAD_REQ`, `LOAD_NACK`) and
  the join from `LOAD_GO` without validation, so `..\..\Documents` made the host
  stream that folder to the join, or create a folder and write the roster file
  there. A host could also probe the join's folders through the `LOAD_GO`
  fingerprint. Fixed: every network save name passes `savexfer::saveNameSafe`,
  `beginSend` and `folderFingerprint` refuse an unsafe name themselves, and a
  `LOAD_NACK` now streams the save the host's `LOAD_GO` announced, never the
  name in the NACK. The same report's smaller items are also fixed:
  - The host drops a join packet whose `ownerId` is not the id it assigned that
    connection, so a join cannot act as the host or as another join.
  - Received fixed-size strings are NUL-terminated before use.
  - Floats that reach the engine must be finite. `lerpAngle` wraps in constant
    time; an infinite heading used to hang the main thread.
  - Door, deed, machine, build, container and furniture hands must be BUILDING
    hands before a `Building` cast, and trader hands must resolve as characters.
  - A peer's character-creation look applies only to another player's squad
    member, and needs a race.
  - Save/load handshakes are accepted only in their own direction (the other
    side never drained those queues).
  - Trade tables are capped (4096 expanded entries, 1024 lines).
  - The money pool sums in 64 bits.

  Tested by replaying hostile join packets (`kcreplay`) into a lone host. On
  0.59.0's code the host streamed a file from outside the save folder and wrote
  a save there. On the fixed build both were refused, and so were a traversal
  `LOAD_REQ`, an `ownerId` posing as the host, a NaN speed and a wrong-direction
  `SAVE_FILE`. In 40 regression runs no honest packet was dropped. The failures
  in that set (`inv_bidir`, `world_item_sync`, `build_sync`, `bdoor_sync`,
  `bed_put`, `pole_put`, `latejoin_sync`, `load_sync`) fail the same way on the
  same tree without the fix: a host-side placement or drop never lands.
  `speed_sync` passed 3 of 4 runs; its failure was the combat-window timing gate
  on both sides. No wire change.
  Still open: direct-UDP hosting has no password (anyone who can reach the
  port can join), and a joined player is trusted with game state (items,
  damage, recruits, deeds), by design.
- **`combat_order`: the join's NPC copies fought less than the host's** (mod
  bug). The combat drive's soft-band walk used `engine::walkTo`, which also
  issues the player move order. For a world NPC that becomes its current task
  (`MOVE_ON_FREE_WILL`) and replaces the attack it was arming, and the copy
  stood idle once the walk ended. In runs 20261023_224018, _224020_s2 and
  _224023_s3, every `MOVE_ON_FREE_WILL` sample of a combat copy came within 2 s
  of a drive walk (208/208, 242/242, 74/74). World-NPC copies are now steered by
  the movement controller alone (`engine::steerTo`) inside the 20 u churn
  ceiling. Past it they still get the move order: steering alone loses to a
  fighting copy's footwork, and a steer-only build snapped 2-3x more in
  `combat_battle`. Squad copies are unchanged. Result: 7 of 8 judged
  `combat_order` runs pass, the join fighting at 0.75-1.49x the host's ratio
  (one single-duelist run at 0.58x), against 4 of 7 at 0.64-0.73x before.
  `combat_crowd` passes 2 of 2 and `combat_battle` matches the old rate.
  `mint_aggro` passes 6 of 7 and `assault_mint` 3 of 3 (0.59.0's code: 3 of 3
  each). The `mint_aggro` failure was one `proxy_drift` sample at 135 u (run
  20261024_014044_s3). With the rest-flap fix below added, an interleaved
  `combat_order` A/B (9 runs each, 20261024_083822 to _091705_s3; 2 of 9 per
  arm unjudged because the host barely fought) passed 7 of 7 judged runs
  (median 1.12x the host) against this fix alone at 5 of 7 (0.91x).
  `mint_aggro` passed 7 of 9 on that build, both failures again a single
  `proxy_drift` sample (218 u and 176 u).
- **`combat_order` judged the wrong window** (test bugs): the host's capture
  ran to about T+34 s while the join's ended near T+16 s. The pre-order bar
  brawl was counted as order-driven combat (host pre-order ratio 0.39-0.54 in 3
  of 9 runs). The join was held to a fixed 0.40 even when the host itself barely
  fought (0.01-0.25). The gate now judges both sides over the join's window
  from T+5 s, drops pre-order brawlers, SKIPs when the host's own ratio is
  under 0.25, and requires the join to reach 0.75x the host.
  `Combat.Fixture.ps1` covers the brawler, weak-host and half-rate cases.
- **A guard caging a KO'd, chained join PC looped every second** (mod bug). The
  owner streams chained-not-caged, so the host broke the cage as an "unvouched"
  seat on every tick, and the guard carried the body back, each cycle a pickup
  and drop on the owner's own body. Runs 20261024_010654_s2 and _010657_s3 had
  51 and 32 `CAGE-BREAK` lines and failed `pc-bad` at 11.2 and 7.9 u. When the
  owner's body is down, the host now authors the cage as a protocol-36
  PEER-ENTER and holds it; the owner applies it to its own body. A conscious
  owner keeps the break. In 6 `world_parity` runs: 0 `CAGE-BREAK` lines; the
  guard caged the PC in 4, the join applied every one, and both sides sat at
  0.0 u.
- **A refused third-party placement held the host's copy until the owner
  left** (mod bug). The host holds a PEER placement while the body is down, and
  "down" included the copy's own state, which the hold never updates. If the
  owner's body was up, it skipped the enter (`SKIP (down=0 ...)`) and the host
  kept its copy caged until `[furn] SWEEP EXIT` on disconnect. The host now
  stops holding after 10 s (`FURN_PEER_REFUSE_MS`) of an owner stream that is up
  and out of furniture, and logs `[furn] PEER-ENTER refused`. Not seen in any
  run since. The new `cage_peer_refuse` scenario can't run until `bedcage1` is
  regenerated (see "Test saves drift"), so `CagePeerRefuse.Fixture.ps1` checks
  its gate on synthetic logs.
- **`npc_sync` rest-flap: far NPC copies on the join flipped walk/rest 70-830
  times a minute** (mod bug, plus a `march` oracle fix). The host simulates a
  walker far from every PC in steps: its stream holds still a median 0.36-0.40 s,
  then jumps 6-20 u, while the owner keeps streaming `cMoving=1` at walk speed.
  The 300 ms walk hold ended inside each still phase, so every step parked the
  copy and walked it again. The drive now follows the owner's own walking state.
  While the owner streams moving at 2 u/s or more (`NPC_STEP_MIN_SPEED`) and its
  last step is at most 1.5 s old (`NPC_STEP_HOLD_MS`), the copy keeps walking,
  led along that step's direction at the owner's speed. Past 80 u behind
  (`NPC_STEP_MAX_GAP`), the rest branch parks it instead. Holding the verdict
  without that pacing was the earlier, reverted attempt: copies stalled, then
  snapped 200-400 u. NPC walk lead is also capped at 2x the owner's speed
  (`NPC_LEAD_VEL_CAP`), so a 15 u step no longer reads as a sprint.
  The oracle fix: the join's engine also moves far copies in jumps (`lstep`
  traces: walk-ordered copies stand still a median 16-17 frames, then jump
  6-12 u). `march` used to count those still frames as marching in place. It
  now skips frames where the owner streams such a step and logs the count
  (`SCENARIO MARCHSTEP skipped=`).
  `npc_sync`, 6 runs per arm: before, 70-829 flips/min and `rest_flap` failed 6
  of 6. After, 30-49 flips/min and every gate passed 6 of 6 (runs
  20261024_055743 to _060205_s3). Snaps of every kind: 168 against 176 without
  the step logic; snaps over 200 u: 16 against 24. On the final build, every
  scenario gated on `march` or `rest_flap` (`npc_sync`, `leader_move`,
  `coop_presence`, `death_order`, `down_order`) passed 15 of 15 runs
  (20261024_064048 to _071331_s2).
  A narrower bridge, limited to sources whose last move was a jump of more than
  2x the owner's speed, was tried and dropped: it did not change `combat_order`
  and cost `npc_sync` a `snap_rate` failure (runs 20261024_075546 to
  _082953_s3).
- **The game crashed while closing** (RE_Kenshi bug, now guarded by the mod).
  From a player's host log (0.59.0, 2026-10-23), at the title with a friend in
  the lobby: `[crash] VEH ... RE_Kenshi.dll+0x381e9`, READ of 0x30. RE_Kenshi
  keeps a focus listener on the game window. When the window goes inactive, the
  listener reads `ou->player->camera` without a null check. The log's stack is
  the render window being destroyed: `~D3D11RenderWindowHwnd` -> `destroy` ->
  `DestroyWindow` -> Ogre `_WndProc` -> RE_Kenshi. The world was already freed
  (`player` null). Windows sends a focused window `WM_ACTIVATE(WA_INACTIVE)`
  from inside `DestroyWindow`, so the listener read the null player. Only a
  game focused at exit hits this, which is why test runs never did: they never
  hold focus and end with `taskkill`.
  At startup the mod now finds the listener's vtable through RE_Kenshi's RTTI
  and skips the handler when there is no player (`[rek-focus]` log lines,
  `game/ReKenshiFocus.cpp`). With focus and a player, the handler runs
  unchanged.
  `KENSHICOOP_TEST_FOCUS_NOPLAYER=1` (harness build) recreates the state at the
  title. Without the guard it faults at `+0x381e9` with the same top of stack;
  with the guard it logs the skip and keeps running.
  The player reported several crashes, but the log is rewritten at each launch,
  so it only held this last one.

## Fixed in 0.59.0

- **`combat_battle`** (test bug): `combat_snap_rate` took persistence from the
  snap line's `n=`, which counts from the copy's first drive. The host places
  runtime-spawned battlers in the first seconds, so a copy arrived with n=3-4
  from the settle window the other buckets skip. Two runs failed at 7 and 8
  snaps against a limit of 6, with 5 and 4 of them in the window (runs
  20261023_195148 and _195149_s2). Persistence now counts in-window snap lines
  per hand. That also stops `n=` restarting when a body is re-acquired.
  `Combat.Fixture.ps1` checks that 7 in-window snaps still fail.
- **`combat_order` judged 2 of 10 duelists** (test bug): the fight re-keys most
  of the join's duelists into fresh local platoons, and their RECV lines then
  carry a hand and container the host never uses. The oracle now pairs by wire
  `index,serial` through `Get-RekeyWireOf`. `Combat.Fixture.ps1` checks that a
  re-keyed copy that never fights still fails.
- **`split_interest`** (test bug): the oracle compared the join and host at the
  same instant. A patron walking 18-25 u/s on the host's path, 0.25-0.5 s
  behind (the interp render delay), read as 9-13 u off against a 6 u limit. It
  now measures the distance to the host's path within the interp delay window
  (`Get-WindowPathDistance`). `SplitInterest.Fixture.ps1` checks that a frozen
  or unstreamed copy still fails. Runs 20261023_182820, _182822_s2 and
  _182826_s3 pass (worst median 0.5-0.9 u). `reconcileProxy`, the earlier
  suspect, never touches these native NPCs.
- **`world_parity` near existence** (test bug): when a fight splits a platoon,
  the join's engine re-keys the body (`[rekey] wire=W local=L`) and dumps it
  under the local hand. The host never re-keys, so the oracle read it as a
  missing Sentinel (9 of the 10 missing ones in run 20261023_183922_s2). The
  oracle now maps re-keyed hands back to the wire hand (`Get-RekeyWireOf`).
  Near existence is 0.98-1.0 in 9 runs.
- **`limb_loss`** (mod bug): a severed limb never reached the other client
  (pitfalls §28). Passes since run 20261023_155852_s2.
- **`world_item_join`** (test bug): the join dropped an item from the host's
  character. The join does not own that character, so the drop correctly never
  streamed. It now drops from its own character (run 20261023_161245_s3).
- **`store_sync`** (test bug): the probe's chest add tried items the chest
  refuses. It now tries building materials first.
- **`trade_lock`** (test bug): when two traders stood about equally near the
  counter (35.5 u vs 36.3 u), the two clients picked different ones and never
  contended for the lock. Near-ties now go to the lowest hand on both clients.
- **`vendor_ui`**: operator-driven; neither game exits on its own. It is now
  tier `none`.

## Flaky (failed once, passed on retry)

- `weapon_loot`: the join did not exit cleanly once. It passed in both later
  runs.

## Unverified or untested

- The carry HANDOVER path (§24) never ran in any test.
- The dialog auto-pause fix (§26) is verified on the host, but has not been
  reproduced on the join.
- The smoothness/march advisories fail; they were already worse on baseline.
- The far-NPC census (1 Hz) was left untouched on purpose.
- `createItemAndAdd` (`EngineInventory.cpp`) leaks the item it created when
  `tryAddItem` refuses it. No gate measures this.
