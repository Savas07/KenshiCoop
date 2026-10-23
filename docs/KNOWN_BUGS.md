# Known bugs

Open issues as of v0.58.1 (2026-10-23). "Pre-existing" means the same failure
reproduces on a v0.58.0 build of the last commit, so 0.58.1 did not cause it.
Scenario names refer to `scripts/scenarios.psd1`; run one with
`kc_tc\run_locked.py --scenario <name>`.

## Player-reported

- **Backpacks and items vanish from non-host inventories** (Workshop comment:
  "Backpacks and Items will disappear from inventories, from people besides the
  host"). Fixed after 0.58.1: the host never created a backpack a join character
  acquired, so the host's save lost it and its contents (pitfalls §27,
  `inv_join_bag`). Not yet covered: a snapshot past `INV_ITEMS_MAX` (64 entries
  incl. bag contents) is truncated, so the tail never reaches the host's copy
  either.

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
- **`vendor_ui`**: neither game self-exits (the harness reports "no clean exit");
  the last lines are normal VUIWINS samples.
- **`world_item_join`**: the host never shows the join's dropped item
  (`observerSpawned=False joinCulled=True`).
- **`store_sync`**: the host's own chest add fails
  (`SCENARIO CONTWRITE ... got=0 ok=0`), so nothing crosses to the join.
- **`limb_loss`**: no severed limb appears on either side. This may be the
  scenario failing to sever rather than a sync bug.
- **`npc_sync` rest-flap**: 560-1100 walk->rest flips/min on the join (limit
  60). Position tracking itself passes.
- **`assault_mint` parity**: the host streams a combat target for a body its own
  `readCombat` reports at peace.
- **`xfer_block`** (diagnostic, tier none): the cross-owner trade veto lets the
  drag through (`blockOk=0 give=1`), with or without the backpack fix.

## Flaky (failed once, passed on retry)

- `combat_order` (join saw no combat), `combat_battle` (one hand snapped 8x),
  `weapon_loot` (join did not exit cleanly).

## Unverified or untested

- The carry HANDOVER path (§24) never ran in any test.
- The dialog auto-pause fix (§26) is verified on the host, but has not been
  reproduced on the join.
- The smoothness/march advisories fail; they were already worse on baseline.
- The far-NPC census (1 Hz) was left untouched on purpose.
