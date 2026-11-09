<#
.SYNOPSIS
  Zero-game fixture for two combat oracles.
  combat_order: a duelist the join re-keys into a fresh platoon when the fight
  starts is still judged, so a copy that fights under its new local hand PASSES
  and one that does not FAILS. A host NPC already brawling before the order is
  not judged, a host fight too weak to compare SKIPS, and a copy fighting at half
  its host's rate FAILS even above the old absolute 0.40.
  combat_snap_rate: a battler the host places during the settle window PASSES;
  a copy that keeps snapping inside the window FAILS, also when its snap counter
  restarted mid-window.

.DESCRIPTION
  combat_order paired host MEMBER and join RECV series by the full hand, which
  includes the squad container. The fight re-keys most of the join's duelists
  ([rekey] wire=W local=L localc=C), so they logged under a hand and container
  the host never uses and the gate judged 2 of 10 duelists.

  combat_snap_rate's persistence read the snap line's n=, which counts from when
  the copy was first driven: a runtime-spawned battler arrived with n=3-4 from
  its placement in the first seconds, the settle window every other bucket
  skips. n also restarts when a body is re-acquired, which hid snaps.

  Exit code = number of failed assertions (0 = PASS), matching prototest and
  Contract.Tests so it can gate a commit.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot "scripts\CoopOracles.psm1") -Force

$script:failed = 0
function Check {
    param([string]$Name, [bool]$Ok)
    if ($Ok) { Write-Host "  ok   $Name" }
    else { Write-Host "  FAIL $Name"; $script:failed++ }
}

function Stamp { param([int]$Ms)
    return "10:{0:00}:{1:00}.{2:000}" -f [int][Math]::Floor($Ms / 60000), ([int][Math]::Floor($Ms / 1000) % 60), ($Ms % 1000)
}

function Get-Gate { param([string]$Name)
    return (Get-GateResults | Where-Object { $_.gate -eq $Name } | Select-Object -First 1)
}

# ---- combat_order logs ----------------------------------------------------------
# The host orders the duel at 20 s and its duelist 3,33 (container 10,100) fights
# from then on. The join re-keys it at the order to local 1,77 in container 50,500
# and logs it there until 36 s. $HostPost / $JoinPost: how many of every 8
# post-order samples are melee on each side. -Brawler adds host NPC 4,44, which
# fights from the start and whose join copy mirrors that before and after.
$WIRE = "3,33,1,10,100"; $LOCAL = "1,77,1,50,500"; $BRAWL = "4,44,1,10,100"
function New-OrderPair {
    param([string]$HostPath, [string]$JoinPath, [int]$HostPost = 8, [int]$JoinPost = 8,
          [switch]$Brawler)
    $h = New-Object System.Collections.Generic.List[string]
    $j = New-Object System.Collections.Generic.List[string]
    for ($ms = 0; $ms -le 40000; $ms += 500) {
        $i = [int]($ms / 500)
        if ($ms -eq 20000) { $h.Add(("[{0}] [HOST] INFO: SCENARIO COMBAT issued orders=2" -f (Stamp $ms))) }
        $task = if ($ms -ge 20000 -and ($i % 8) -lt $HostPost) { 65024 } else { 65535 }
        $h.Add(("[{0}] [HOST] INFO: SCENARIO MEMBER hand={1} pos=10.00,0.00,10.00 task={2}" -f (Stamp $ms), $WIRE, $task))
        if ($Brawler) { $h.Add(("[{0}] [HOST] INFO: SCENARIO MEMBER hand={1} pos=12.00,0.00,10.00 task=65024" -f (Stamp $ms), $BRAWL)) }
        if ($ms -gt 36000) { continue }
        if ($ms -eq 20000) { $j.Add(("[{0}] [JOIN] INFO: [rekey] wire=3,33 local=1,77 localc=50,500" -f (Stamp $ms))) }
        if ($ms -lt 20000) {
            $j.Add(("[{0}] [JOIN] INFO: SCENARIO RECV hand={1} pos=10.00,0.00,10.00 task=65535" -f (Stamp $ms), $WIRE))
        } else {
            $jt = if (($i % 8) -lt $JoinPost) { 65024 } else { 65535 }
            $j.Add(("[{0}] [JOIN] INFO: SCENARIO RECV hand={1} pos=10.00,0.00,10.00 task={2}" -f (Stamp $ms), $LOCAL, $jt))
        }
        if ($Brawler) { $j.Add(("[{0}] [JOIN] INFO: SCENARIO RECV hand={1} pos=12.00,0.00,10.00 task=65024" -f (Stamp $ms), $BRAWL)) }
    }
    $h | Set-Content -Path $HostPath -Encoding UTF8
    $j | Set-Content -Path $JoinPath -Encoding UTF8
}

# ---- combat_snap_rate logs ------------------------------------------------------
# The first order is at 0 s, so the oracle settles until 8 s and scores up to the
# last [combat] line at 40 s (a 32 s window). $Snaps = list of @(ms, n), each a
# chase-bucket snap (drift 60 u).
function New-SnapLog {
    param([string]$Path, [string]$Hand, [object[]]$Snaps)
    $l = New-Object System.Collections.Generic.List[string]
    $l.Add(("[{0}] [JOIN] INFO: [combat] order hand={1} tgt=9,9" -f (Stamp 0), $Hand))
    foreach ($s in $Snaps) {
        $l.Add(("[{0}] [JOIN] INFO: [combat] snap hand={1} drift=60.0 srcVel=30.0 localFight=0 wrongTgt=0 arming=0 wait=0 seg=50 n={2}" -f (Stamp $s[0]), $Hand, $s[1]))
    }
    $l.Add(("[{0}] [JOIN] INFO: [combat] stats snap=0 slide=0" -f (Stamp 40000)))
    $l | Set-Content -Path $Path -Encoding UTF8
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_combat_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    # ---- 1. combat_order: re-keyed duelist fights under its local hand. ----
    Write-Host "== combat_order: duelist re-keyed at the order, copy fights (expect PASS) =="
    $h1 = Join-Path $tmp "host_fight.log"; $j1 = Join-Path $tmp "join_fight.log"
    New-OrderPair -HostPath $h1 -JoinPath $j1
    Reset-GateResults
    $null = Test-CombatOrder -HostFile $h1 -JoinFile $j1
    $g1 = Get-Gate 'combat_order'
    Check "re-keyed fighting copy PASSes" ($null -ne $g1 -and $g1.status -eq 'PASS')
    Check "post-order samples come from the local hand" ($null -ne $g1 -and $g1.metrics.postRatio -eq 1)

    # ---- 2. combat_order: re-keyed duelist stays at peace. ----
    Write-Host "== combat_order: duelist re-keyed at the order, copy never fights (expect FAIL) =="
    $h2 = Join-Path $tmp "host_peace.log"; $j2 = Join-Path $tmp "join_peace.log"
    New-OrderPair -HostPath $h2 -JoinPath $j2 -JoinPost 0
    Reset-GateResults
    $null = Test-CombatOrder -HostFile $h2 -JoinFile $j2
    $g2 = Get-Gate 'combat_order'
    Check "re-keyed peaceful copy FAILs" ($null -ne $g2 -and $g2.status -eq 'FAIL')
    Check "failure is the post-order ratio" ($null -ne $g2 -and $g2.metrics.postRatio -eq 0)

    # ---- 3. combat_order: the bar's brawl was already running at the order. ----
    Write-Host "== combat_order: a host NPC fights before the order too (expect PASS, brawler not judged) =="
    $h6 = Join-Path $tmp "host_brawl.log"; $j6 = Join-Path $tmp "join_brawl.log"
    New-OrderPair -HostPath $h6 -JoinPath $j6 -Brawler
    Reset-GateResults
    $null = Test-CombatOrder -HostFile $h6 -JoinFile $j6
    $g6 = Get-Gate 'combat_order'
    Check "mirrored pre-order brawl PASSes" ($null -ne $g6 -and $g6.status -eq 'PASS')
    Check "the brawler is left out" ($null -ne $g6 -and $g6.metrics.brawlers -eq 1 -and $g6.metrics.duelists -eq 1)

    # ---- 4. combat_order: the host's own fight is too weak to compare. ----
    Write-Host "== combat_order: host melee in 1 of 8 samples (expect SKIP) =="
    $h7 = Join-Path $tmp "host_weak.log"; $j7 = Join-Path $tmp "join_weak.log"
    New-OrderPair -HostPath $h7 -JoinPath $j7 -HostPost 1 -JoinPost 0
    Reset-GateResults
    $null = Test-CombatOrder -HostFile $h7 -JoinFile $j7
    $g7 = Get-Gate 'combat_order'
    Check "weak host fight SKIPs" ($null -ne $g7 -and $g7.status -eq 'SKIP')

    # ---- 5. combat_order: the copy fights half as much as its host. ----
    Write-Host "== combat_order: host melee 8 of 8, join 4 of 8 (expect FAIL) =="
    $h8 = Join-Path $tmp "host_half.log"; $j8 = Join-Path $tmp "join_half.log"
    New-OrderPair -HostPath $h8 -JoinPath $j8 -JoinPost 4
    Reset-GateResults
    $null = Test-CombatOrder -HostFile $h8 -JoinFile $j8
    $g8 = Get-Gate 'combat_order'
    Check "half the host's fight FAILs" ($null -ne $g8 -and $g8.status -eq 'FAIL')
    Check "join ratio is above the old absolute floor" ($null -ne $g8 -and $g8.metrics.postRatio -ge 0.40)

    # ---- 6. combat_snap_rate: four placement snaps before settle, three in the window. ----
    Write-Host "== combat_snap_rate: spawn placement snaps n=1-4, then 3 in-window (expect PASS) =="
    $p3 = Join-Path $tmp "spawn.log"
    New-SnapLog -Path $p3 -Hand "5,1919" -Snaps @(
        @(1000, 1), @(1100, 2), @(1400, 3), @(1900, 4),
        @(12000, 5), @(24000, 6), @(30000, 7))
    Reset-GateResults
    $st3 = Test-CombatSnapRate -JoinFile $p3
    Check "placement snaps do not count" ($st3 -eq 'PASS')
    $g3 = Get-Gate 'combat_snap_rate'
    Check "persistence is the 3 in-window snaps" ($null -ne $g3 -and $g3.metrics.maxPersistPerHand -eq 3)

    # ---- 7. combat_snap_rate: the copy keeps snapping inside the window. ----
    Write-Host "== combat_snap_rate: 7 in-window snaps on one hand (expect FAIL) =="
    $p4 = Join-Path $tmp "persist.log"
    New-SnapLog -Path $p4 -Hand "3,3090" -Snaps @(
        @(10000, 1), @(14000, 2), @(18000, 3), @(22000, 4), @(26000, 5), @(30000, 6), @(34000, 7))
    Reset-GateResults
    $st4 = Test-CombatSnapRate -JoinFile $p4
    Check "persistent divergence FAILs" ($st4 -eq 'FAIL')
    $g4 = Get-Gate 'combat_snap_rate'
    Check "failure names the 7 snaps" ($null -ne $g4 -and $g4.detail -match 'snapped 7x')

    # ---- 8. combat_snap_rate: same 7 snaps, the copy re-acquired after the fourth. ----
    Write-Host "== combat_snap_rate: 7 in-window snaps, counter restarted mid-window (expect FAIL) =="
    $p5 = Join-Path $tmp "reacquired.log"
    New-SnapLog -Path $p5 -Hand "1,2590" -Snaps @(
        @(10000, 1), @(14000, 2), @(18000, 3), @(22000, 4), @(26000, 1), @(30000, 2), @(34000, 3))
    Reset-GateResults
    $st5 = Test-CombatSnapRate -JoinFile $p5
    Check "re-acquired copy still FAILs" ($st5 -eq 'FAIL')
    $g5 = Get-Gate 'combat_snap_rate'
    Check "all 7 snaps counted" ($null -ne $g5 -and $g5.metrics.maxPersistPerHand -eq 7)
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failed -eq 0) { Write-Host "combat fixture: all checks passed - PASS" }
else { Write-Host "combat fixture: $script:failed check(s) FAILED" }
exit $script:failed
