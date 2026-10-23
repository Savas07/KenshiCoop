# Known bugs

Open issues as of v0.58.1 (2026-10-23), updated after the item, NPC-movement and
combat batches (local work after 0.58.1, not released yet). "Pre-existing" means the same failure
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
  earlier, and in 0 of 30 later. Cause unknown. Next step: log a stack for that
  throw, or add a no-main-tick watchdog that samples the main thread.
- **`world_parity`** (probe): fails 3 of 9 runs after the oracle fix below, on
  two real desyncs. (a) The Holy Nation chains or cages the join's PC Flashbox
  in every run (the host authors a `[furn] PEER-ENTER`, kind 2 or 3). In 2
  runs the host's copy then stood 26-38 u from the join's body for over a
  minute, until `[furn] SWEEP EXIT` (run 20261023_185725_s2). (b) Near position
  0.791 against 0.8 (run 20261023_192313_s2). Also seen: the join kept the
  prisoner Fuu in furniture while the host's Fuu stood 400-800 u away
  (run 20261023_185728_s3).
- **`npc_sync` rest-flap**: 75-700 walk->rest flips/min on the join (limit 60;
  3 runs, 20261023_193202 to _193206_s3). Position tracking passes. Cause,
  traced with `KENSHICOOP_DEBUG_REGIME=1`: a walker far from every host PC is
  simulated coarsely on the host. Its stream holds still ~0.5 s, then jumps
  15-20 u, which reads as 300-3300 u/s and then 0. The 300 ms walk hold ends
  inside every step, so each step parks the copy (a small teleport) and walks
  it again. That is 58-73% of all flips in the traced world_parity runs.
  Holding the walk verdict across steps was tried and reverted. It cut flips
  2-5x, but march failed 2 of 3 runs and snap_rate 1 of 3: walk-ordered animal
  copies stalled for 5 s, then snapped 200-400 u. The note above `midTier` in
  `ReplicatorDrive.cpp` has the numbers. The gate scores only time after the
  join's clock slew ends. In a run where the slew took 60 of 65 s, it skipped.
- **`combat_order`**: the join's copies fight less than the host's. With all 10
  duelists judged (oracle fix below), the post-order combat ratio was 0.505
  (host 0.673, PASS) and 0.305 (host 0.614, FAIL, limit 0.40). Runs
  20261023_194654 and _195152_s3. In the failing run the join re-ordered its
  copies up to 5 times each (`r=2`, `forced=1`), but `localFight` stayed 0
  while nine copies converged on one target. The host had 5 KOs in the same
  window.
- **`assault_mint` parity** (advisory; the gate, `proxy_drift`, passes): the
  host streams a combat target for a body its own `readCombat` reports at
  peace. Run 20261023_194659_s3: 23 of 56 pairs fight on the join only.
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

## Fixed after 0.58.1 (not released)

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

- `combat_order`: the join saw too little combat (0.295 < 0.4), then 0.417 on
  the retry.
- `combat_battle`: one hand snapped 8 times (limit 6). This happened in 0.58.1's
  regression run and again in a single run during the item batch.
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
