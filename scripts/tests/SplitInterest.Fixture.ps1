<#
.SYNOPSIS
  Zero-game fixture for the split_interest oracle: a bar NPC copy that walks the
  host's path a render delay late PASSES; a copy that stays put while the host's
  NPC walks, and a bar that stops streaming after the split, FAIL.

.DESCRIPTION
  The oracle compares each join sample with the host's PATH over a lag window
  instead of the host's position at the same instant (equal-time pairing charged
  a walking patron's speed x render delay as drift). The FAIL shapes prove the
  window did not open a hole: a frozen copy and an unstreamed bar are still
  caught.

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

# The bar anchor is the origin; the host's tab leaves at 8 s, so the oracle
# judges from 14 s (6 s grace) on. Two patrons sit; the third stands until 14 s,
# then walks +x at 25 u/s and leaves the 150 u anchor radius at 20 s - the
# shape of the runs that failed: every judged sample of the walker is a walk.
$SEAT1 = "1,11,1,8,3"; $SEAT2 = "2,22,1,8,3"; $WALK = "3,33,1,8,3"
function Get-HostPos {
    param([string]$Hand, [double]$Ms)
    switch ($Hand) {
        $SEAT1 { return @(10.0, 0.0, 10.0) }
        $SEAT2 { return @(-20.0, 0.0, 5.0) }
        default {
            $x = [Math]::Min(300.0, [Math]::Max(0.0, ($Ms - 14000.0) * 0.025))
            return @($x, 0.0, 0.0)
        }
    }
}

function Format-Line { param([int]$Ms, [string]$Kind, [string]$Hand, $P)
    return ("[{0}] SCENARIO {1} hand={2} pos={3:F2},{4:F2},{5:F2} task=65535" -f (Stamp $Ms), $Kind, $Hand, $P[0], $P[1], $P[2])
}

# $Walker: lag   = the copy walks the host's path 450 ms late (render delay)
#          frozen = the copy never leaves where the host's NPC started
# $StopAtMs: the join receives no bar NPC after this time (0 = never stops).
function New-SplitPair {
    param([string]$HostPath, [string]$JoinPath, [string]$Walker, [int]$StopAtMs = 0)
    $h = @("[10:00:00.000] KenshiCoop: gameplay started",
           "[10:00:00.500] SCENARIO SPLIT bar=0.00,0.00,0.00 have=1",
           "[10:00:08.000] SCENARIO SPLIT moved=2 to=260.00,0.00,0.00")
    $j = @("[10:00:00.000] KenshiCoop: gameplay started",
           "[10:00:00.100] CLOCKSYNC offset=0 rtt=10")
    for ($ms = 1000; $ms -le 40000; $ms += 500) {
        foreach ($hand in @($SEAT1, $SEAT2, $WALK)) {
            $h += (Format-Line -Ms $ms -Kind "MEMBER" -Hand $hand -P (Get-HostPos -Hand $hand -Ms $ms))
        }
        # The join samples on its own timer, 250 ms off the host's.
        $jm = $ms + 250
        if ($StopAtMs -gt 0 -and $jm -gt $StopAtMs) { continue }
        foreach ($hand in @($SEAT1, $SEAT2, $WALK)) {
            $p = Get-HostPos -Hand $hand -Ms $jm
            if ($hand -eq $WALK) {
                if ($Walker -eq 'frozen') { $p = Get-HostPos -Hand $hand -Ms 0 }
                else { $p = Get-HostPos -Hand $hand -Ms ($jm - 450) }
            }
            $j += (Format-Line -Ms $jm -Kind "RECV" -Hand $hand -P $p)
        }
    }
    $h | Set-Content -Path $HostPath -Encoding UTF8
    $j | Set-Content -Path $JoinPath -Encoding UTF8
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_split_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    # ---- 1. The walker trails the host by a render delay: tracking. ----
    Write-Host "== split_interest: walking patron, copy 450 ms behind on the same path (expect PASS) =="
    $hLag = Join-Path $tmp "host_lag.log"; $jLag = Join-Path $tmp "join_lag.log"
    New-SplitPair -HostPath $hLag -JoinPath $jLag -Walker 'lag'
    Reset-GateResults
    $st1 = Test-SplitInterest -HostFile $hLag -JoinFile $jLag
    Check "lagging walker PASSes" ($st1 -eq 'PASS')
    $g1 = Get-GateResults | Where-Object { $_.gate -eq 'split_interest' } | Select-Object -First 1
    Check "all three patrons judged" ($null -ne $g1 -and $g1.metrics.judged -eq 3)

    # ---- 2. The copy stays where the host's NPC started walking from. ----
    Write-Host "== split_interest: host's patron walks, copy frozen (expect FAIL) =="
    $hFrz = Join-Path $tmp "host_frz.log"; $jFrz = Join-Path $tmp "join_frz.log"
    New-SplitPair -HostPath $hFrz -JoinPath $jFrz -Walker 'frozen'
    Reset-GateResults
    $st2 = Test-SplitInterest -HostFile $hFrz -JoinFile $jFrz
    Check "frozen copy FAILs" ($st2 -eq 'FAIL')
    $g2 = Get-GateResults | Where-Object { $_.gate -eq 'split_interest' } | Select-Object -First 1
    Check "only the walker is a mismatch" ($null -ne $g2 -and $g2.metrics.judged -eq 3 -and $g2.metrics.tracked -eq 2)

    # ---- 3. The bar stops streaming once the host's tab leaves. ----
    Write-Host "== split_interest: bar not streamed after the split (expect FAIL) =="
    $hGone = Join-Path $tmp "host_gone.log"; $jGone = Join-Path $tmp "join_gone.log"
    New-SplitPair -HostPath $hGone -JoinPath $jGone -Walker 'lag' -StopAtMs 9000
    Reset-GateResults
    $st3 = Test-SplitInterest -HostFile $hGone -JoinFile $jGone
    Check "unstreamed bar FAILs" ($st3 -eq 'FAIL')
    $g3 = Get-GateResults | Where-Object { $_.gate -eq 'split_interest' } | Select-Object -First 1
    Check "failure names the missing stream" ($null -ne $g3 -and $g3.detail -match 'not streamed')
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

Write-Host ""
if ($script:failed -eq 0) { Write-Host "split_interest fixture: all checks passed - PASS" }
else { Write-Host "split_interest fixture: $script:failed check(s) FAILED" }
exit $script:failed
