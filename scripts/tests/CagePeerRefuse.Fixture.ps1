<#
.SYNOPSIS
  Zero-game fixture for the cage_peer_refuse oracle: proves Test-CagePeerRefuse
  PASSES when the host releases a placement its owner refused and brings the copy
  back onto the owner, and FAILS on the pre-fix shape where the host holds the
  copy caged for the rest of the session.

.DESCRIPTION
  The live scenario needs the baked cage in save 'bedcage1'; while that fixture
  is unusable this is the only proof the gate can tell the two shapes apart.

  Exit code = number of failed assertions (0 = PASS).
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
    $m = [int][Math]::Floor($Ms / 60000); $s = [int][Math]::Floor(($Ms % 60000) / 1000)
    return ("[10:{0:00}:{1:00}.{2:000}]" -f $m, $s, ($Ms % 1000))
}

$PutMs = 14000

# One FURN series line for subject 1,500 at x (z fixed).
function Furn { param([int]$Ms, [int]$In, [double]$X)
    return ((Stamp $Ms) + (" SCENARIO FURN hand=1,500 t=$Ms in=$In furn=9,9 pos={0:F2},5.00,50.00 bs=0" -f $X))
}

# Host log. $EjectMs = when the host's copy leaves the cage (0 = never);
# $EndX = where it sits afterwards. $Released = log the refusal + HEAL EXIT.
function New-HostLog {
    param([string]$Path, [bool]$Released, [int]$EjectMs, [double]$EndX, [bool]$Authored = $true)
    $lines = @("[10:00:00.000] KenshiCoop: gameplay started", "[10:00:00.100] CLOCKSYNC offset=0 rtt=10")
    $lines += (Stamp $PutMs) + " SCENARIO FURNACT host put hand=1,500 kind=2 ok=1"
    if ($Authored) { $lines += (Stamp ($PutMs + 50)) + " [furn] SEND PEER-ENTER id=7 occ=1,500 furn=9,9 kind=2" }
    if ($Released) {
        $lines += (Stamp ($EjectMs - 3100)) + " [furn] PEER-ENTER refused occ=1,500 kind=2 up=10000ms (owner not down) - releasing"
        $lines += (Stamp $EjectMs) + " [furn] HEAL EXIT occ=1,500 kind=2 ok=1"
    }
    for ($ms = 500; $ms -le 62000; $ms += 500) {
        $caged = $ms -ge $PutMs -and ($EjectMs -eq 0 -or $ms -lt $EjectMs)
        $x = if ($caged) { 130.0 } elseif ($ms -ge $PutMs) { $EndX } else { 100.0 }
        $lines += (Furn -Ms $ms -In ($(if ($caged) { 2 } else { 0 })) -X $x)
    }
    $lines | Set-Content -Path $Path -Encoding UTF8
}

# Join log: the owner's own L1 stands at x=100 the whole run. $Skipped = it
# refused the enter (its body was up); $JoinIn = what its own body reads.
function New-JoinLog {
    param([string]$Path, [bool]$Skipped = $true, [int]$JoinIn = 0)
    $lines = @("[10:00:00.000] KenshiCoop: gameplay started", "[10:00:00.100] CLOCKSYNC offset=0 rtt=10",
               "[10:00:00.200] SCENARIO FURN L1 hand=1,500 kind=2")
    if ($Skipped) {
        $lines += (Stamp ($PutMs + 100)) + " [furn] RECV PEER-ENTER own occ=1,500 SKIP (down=0 already=0 exited=0)"
    } else {
        $lines += (Stamp ($PutMs + 100)) + " [furn] RECV ENTER id=7 occ=1,500 furn=9,9 kind=2 ok=1"
    }
    for ($ms = 500; $ms -le 56000; $ms += 500) {
        $in = if ($ms -ge $PutMs) { $JoinIn } else { 0 }
        $lines += (Furn -Ms $ms -In $in -X 100.0)
    }
    $lines | Set-Content -Path $Path -Encoding UTF8
}

function Run-Case { param([string]$H, [string]$J)
    Reset-GateResults
    $st = Test-CagePeerRefuse -HostFile $H -JoinFile $J
    $g = Get-GateResults | Where-Object { $_.gate -eq 'cage_peer_refuse' } | Select-Object -First 1
    return @($st, $g)
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_cpr_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    $j = Join-Path $tmp "join.log"; New-JoinLog -Path $j

    Write-Host "== cage_peer_refuse: released and back on the owner (expect PASS) =="
    $h1 = Join-Path $tmp "host_ok.log"
    New-HostLog -Path $h1 -Released $true -EjectMs 27500 -EndX 100.5
    $st, $g = Run-Case $h1 $j
    Check "fixed shape PASSes" ($st -eq 'PASS')
    Check "records the eject latency" ($null -ne $g -and $g.metrics.releaseMs -eq 13500)

    Write-Host "== cage_peer_refuse: held caged until the end (expect FAIL) =="
    $h2 = Join-Path $tmp "host_held.log"
    New-HostLog -Path $h2 -Released $false -EjectMs 0 -EndX 130.0
    $st, $g = Run-Case $h2 $j
    Check "pre-fix shape FAILs" ($st -eq 'FAIL')
    Check "failure names the hold" ($null -ne $g -and $g.detail -match 'never released')
    Check "failure names the occupancy" ($null -ne $g -and $g.detail -match 'still occupied')

    Write-Host "== cage_peer_refuse: ejected but left 20 u off (expect FAIL) =="
    $h3 = Join-Path $tmp "host_off.log"
    New-HostLog -Path $h3 -Released $true -EjectMs 27500 -EndX 120.0
    $st, $g = Run-Case $h3 $j
    Check "stranded copy FAILs" ($st -eq 'FAIL')
    Check "failure names the gap" ($null -ne $g -and $g.detail -match 'from its owner')

    Write-Host "== cage_peer_refuse: owner accepted instead (expect FAIL premise) =="
    $j4 = Join-Path $tmp "join_acc.log"; New-JoinLog -Path $j4 -Skipped $false -JoinIn 2
    $st, $g = Run-Case $h1 $j4
    Check "accepted enter FAILs" ($st -eq 'FAIL')
    Check "failure names the premise" ($null -ne $g -and $g.detail -match 'never skipped')
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failed -eq 0) { Write-Host "cage_peer_refuse fixture: all checks passed - PASS" }
else { Write-Host "cage_peer_refuse fixture: $script:failed check(s) FAILED" }
exit $script:failed
