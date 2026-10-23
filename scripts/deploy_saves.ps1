<#
.SYNOPSIS
  Restore pristine fixture save(s) from the repo (fixtures\saves\<name>) into the
  live Kenshi save folder (%LOCALAPPDATA%\kenshi\save\<name>) before a test run.

.DESCRIPTION
  Both the host and Kenshi-Join installs load saves from the SAME per-user folder.
  During a co-op session the host connect-push bakes the live world over the loaded
  save and the join stream-commits into it, so a validation fixture drifts across
  runs. This script mirrors the repo's pristine copy over the AppData copy so every
  run starts from a known-good state; the game only ever mutates the AppData copy,
  which this discards on the next restore.

  Uses robocopy /MIR, so the destination becomes an EXACT copy of the repo fixture
  (stale drift files are removed). Debug saves that have no repo fixture are skipped.

.EXAMPLE
  # Restore one save (what run_test.ps1 calls automatically):
  powershell -ExecutionPolicy Bypass -File scripts\deploy_saves.ps1 -Save sync

.EXAMPLE
  # Restore every tracked fixture (e.g. once before a full regression):
  powershell -ExecutionPolicy Bypass -File scripts\deploy_saves.ps1 -All
#>
[CmdletBinding()]
param(
    # Single fixture name to restore. Ignored when -All is given.
    [string]$Save = "",
    # Restore every fixture present under fixtures\saves.
    [switch]$All,
    # Restore even if a Kenshi process is running (unsafe: may fight a mid-write).
    [switch]$Force,
    # Override the live save root (defaults to the per-user AppData folder).
    [string]$SaveRoot = (Join-Path $env:LOCALAPPDATA "kenshi\save"),
    # Live folder name for the single -Save fixture (default: the fixture name).
    # Parallel test pairs each load a private copy (e.g. sync -> sync_s2): the
    # game takes saves from the one per-user folder whatever its install dir.
    [string]$As = "",
    # ';'-separated folders whose Kenshi processes the running-game refusal
    # ignores: other test pairs' private installs, which only ever load their
    # own suffixed copies and so cannot be writing the folder restored here.
    [string]$IgnoreProcessesUnder = ""
)

$ErrorActionPreference = "Stop"
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot    = Split-Path -Parent $scriptDir
$fixturesDir = Join-Path $repoRoot "fixtures\saves"

if (-not $All -and $Save -eq "") {
    throw "Specify -Save <name> or -All."
}
if ($All -and $As -ne "") {
    throw "-As renames one fixture; it cannot be combined with -All."
}
if (-not (Test-Path $fixturesDir)) {
    throw "Fixture store not found: $fixturesDir"
}

# Refuse to clobber a save the game might be mid-writing.
if (-not $Force) {
    $ignore = @($IgnoreProcessesUnder -split ';' | Where-Object { $_.Trim() -ne "" } |
                ForEach-Object { $_.Trim().TrimEnd('\') + '\' })
    $live = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue |
              Where-Object {
                  $p = $_.Path
                  -not ($p -and @($ignore | Where-Object { $p.StartsWith($_, [System.StringComparison]::OrdinalIgnoreCase) }).Count -gt 0)
              })
    if ($live.Count -gt 0) {
        throw "Kenshi is running ($($live.Count) process(es)); refusing to restore saves. Close it or pass -Force."
    }
}

New-Item -ItemType Directory -Force -Path $SaveRoot | Out-Null

# Resolve the target list.
if ($All) {
    $targets = Get-ChildItem -Path $fixturesDir -Directory -ErrorAction SilentlyContinue | ForEach-Object Name
    if (-not $targets -or $targets.Count -eq 0) { Write-Host "No fixtures under $fixturesDir; nothing to restore."; exit 0 }
} else {
    $targets = @($Save)
}

$restored = 0
$skipped  = 0
foreach ($name in $targets) {
    $src = Join-Path $fixturesDir $name
    if (-not (Test-Path (Join-Path $src "quick.save"))) {
        # No repo fixture (e.g. a debug save): leave whatever is in AppData as-is.
        Write-Host "  skip '$name' (no repo fixture)"
        $skipped++
        continue
    }
    $liveName = if ($As -ne "") { $As } else { $name }
    $dst = Join-Path $SaveRoot $liveName
    & robocopy $src $dst /MIR /R:1 /W:1 /NFL /NDL /NP /NJH /NJS | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -ge 8) { throw "robocopy failed restoring '$name' (rc=$rc): $src -> $dst" }
    Write-Host "  restored '$name' -> $dst"
    $restored++
}

Write-Host "Saves restored: $restored (skipped $skipped)."
exit 0
