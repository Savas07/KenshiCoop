# Known bugs

Open issues as of v0.58.1 (2026-10-23), updated after the item batch (local
work after 0.58.1, not released yet). "Pre-existing" means the same failure
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

## Regressions to decide on

- **`split_interest` tracks slightly worse on 0.58.1.** It fails on both
  versions, but the mean tracked ratio over 5 runs each is 0.52 (0.58.1) vs 0.72
  (0.58.0). The NPC that keeps drifting is `1,3688842752,1,17,308837888`, at a
  median of 9-17 u against a 6 u limit. Suspect: `reconcileProxy` now skips
  bodies in `drivenChars_` (§23), which leaves the peer's bar NPCs uncorrected.

## Pre-existing failures

- **Join main-thread freeze in the Holy Nation town.** The main log stops right
  after a 3rd first-chance `InvalidParametersException@Ogre`, while the net
  thread keeps logging CLOCKSYNC and reconnects. Seen in about 2 of 25 chase runs
  earlier, and in 0 of 30 later. Cause unknown. Next step: log a stack for that
  throw, or add a no-main-tick watchdog that samples the main thread.
- **`world_parity`**: Holy Sentinels within 200 u of the PCs are missing on the
  join (near existence 0.44 and 0.70 against 0.9). Fails the same way on a build
  without the item batch (run 20261023_170255_s3).
- **`npc_sync` rest-flap**: 560-1100 walk->rest flips/min on the join (limit
  60). Position tracking itself passes.
- **`assault_mint` parity**: the host streams a combat target for a body its own
  `readCombat` reports at peace.
- **`save_probe`** with `KENSHICOOP_SAVE_SYNC=0`: the join never finishes
  loading. Not investigated; probably the run setup rather than the mod.
- **`xfer_block`** (diagnostic, tier none): the cross-owner trade veto lets the
  drag through (`blockOk=0 give=1`), with or without the backpack fix. No player
  impact: the veto is opt-in (`KENSHICOOP_BLOCK_XFER=1`) and off by default.

## Fixed after 0.58.1 (not released)

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
