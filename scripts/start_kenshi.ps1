# start_kenshi.ps1 - launch a Kenshi instance and get it past the launcher.
#
# Kenshi's startup shows a Win32/MFC graphics launcher (Video settings / Mods /
# Changelog, with an OK button) and blocks there until OK is clicked. RE_Kenshi
# and our plugin only load AFTER that click. The root kenshi_x64.exe is just a
# loader that relaunches the real game as a separate process named "Kenshi_x64",
# so we also have to discover that real process (a different PID).
#
# This script:
#   1. snapshots existing Kenshi_x64 PIDs,
#   2. launches the loader (default: the Steam install's kenshi_x64.exe),
#   3. waits for a NEW Kenshi_x64 window that has an "OK" button,
#   4. clicks OK via BM_CLICK (cross-process; no screen coordinates needed),
#   5. returns the real game's process Id (written to stdout as "GAMEPID=<id>").
#
# Env vars (KENSHICOOP_*) must be set by the caller before invoking; they are
# inherited by the launched process and propagate to the relaunched game.

param(
    [string]$ExePath = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi\kenshi_x64.exe",
    [string]$WorkDir = "",
    [int]$TimeoutSec = 90,
    # Automated runs on a machine someone is using: start the game so that it never
    # takes the foreground. The launcher dialog is the game's first window, and
    # Windows shows a process's first window in the show state the CreateProcess
    # caller asks for - but the root exe drops that when it relaunches the real game
    # (`./RE_Kenshi/kenshi_x64.exe --norestart`), so with RE_Kenshi present this
    # starts that command itself, minimized without activation. The plugin's
    # KENSHICOOP_NO_FOCUS guard covers the game window after that.
    [switch]$NoActivate
)

$ErrorActionPreference = "Stop"
if (-not $WorkDir) { $WorkDir = Split-Path $ExePath }

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class KenLauncher {
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  public const int SW_HIDE = 0;
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  public const uint BM_CLICK = 0x00F5;
  static IntPtr found;
  public static IntPtr FindButton(IntPtr top, string text) {
    found = IntPtr.Zero;
    EnumChildWindows(top, (h,p) => {
      var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() == "Button") {
        var t = new StringBuilder(64); GetWindowText(h, t, 64);
        if (t.ToString() == text) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static bool ClickButton(IntPtr top, string text) {
    IntPtr b = FindButton(top, text);
    if (b == IntPtr.Zero) return false;
    SendMessage(b, BM_CLICK, IntPtr.Zero, IntPtr.Zero);
    return true;
  }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct STARTUPINFO {
    public int cb; public string lpReserved, lpDesktop, lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2;
    public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern bool CreateProcess(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                   IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  // Start app (environment inherited) with its first window minimized and NOT activated.
  public static int StartNoActivate(string app, string cmdLine, string cwd) {
    STARTUPINFO si = new STARTUPINFO();
    si.cb = Marshal.SizeOf(si);
    si.dwFlags = 1;      // STARTF_USESHOWWINDOW
    si.wShowWindow = 7;  // SW_SHOWMINNOACTIVE
    PROCESS_INFORMATION pi;
    if (!CreateProcess(app, new StringBuilder(cmdLine), IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi))
      return -Marshal.GetLastWin32Error();
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }
}
"@

$before = @{}
foreach ($p in (Get-Process -Name "Kenshi_x64" -ErrorAction SilentlyContinue)) { $before[$p.Id] = $true }

$realExe = Join-Path $WorkDir "RE_Kenshi\kenshi_x64.exe"
if ($NoActivate -and (Test-Path $realExe)) {
    # The exact command the root exe runs, from the same working directory.
    Write-Host ("Launching (no activation): " + $realExe)
    $rc = [KenLauncher]::StartNoActivate($realExe, "./RE_Kenshi/kenshi_x64.exe --norestart", $WorkDir)
    if ($rc -le 0) { Write-Error "CreateProcess failed for $realExe (error $(-$rc))"; exit 4 }
} else {
    Write-Host ("Launching: " + $ExePath)
    Start-Process -FilePath $ExePath -WorkingDirectory $WorkDir | Out-Null
}

# Find a NEW Kenshi_x64 window that exposes the launcher's OK button, then click it.
# Only a process running from OUR WorkDir qualifies: with several test pairs launching
# at once, "new since the snapshot" also matches another pair's launcher, and taking it
# would hand that pair's game to this caller (wrong PID watched, killed and captured).
$ownRoot = (Resolve-Path $WorkDir).Path.TrimEnd('\') + '\'
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$game = $null
while ((Get-Date) -lt $deadline) {
    # Short poll: the launcher dialog is on screen (and may hold the foreground)
    # until it is found and hidden below.
    Start-Sleep -Milliseconds 100
    $cands = Get-Process -Name "Kenshi_x64" -ErrorAction SilentlyContinue |
             Where-Object { -not $before.ContainsKey($_.Id) -and $_.MainWindowHandle -ne 0 -and
                            $_.Path -and $_.Path.StartsWith($ownRoot, [System.StringComparison]::OrdinalIgnoreCase) }
    foreach ($c in $cands) {
        if ([KenLauncher]::FindButton($c.MainWindowHandle, "OK") -ne [IntPtr]::Zero) { $game = $c; break }
    }
    if ($game) { break }
}
if (-not $game) { Write-Error "Timed out waiting for the Kenshi launcher (OK button)."; exit 2 }

Write-Host ("Launcher up: PID=" + $game.Id + " '" + $game.MainWindowTitle + "' - clicking OK")
# Hide the dialog first: the click is a message, so it needs no visible window, and the
# person using this machine never sees the launcher come up over their own window.
[void][KenLauncher]::ShowWindow($game.MainWindowHandle, [KenLauncher]::SW_HIDE)
if (-not [KenLauncher]::ClickButton($game.MainWindowHandle, "OK")) {
    Write-Error "Found launcher window but failed to click OK."; exit 3
}

# Emit on the output stream (not Write-Host) so callers can capture it.
Write-Output ("GAMEPID=" + $game.Id)
exit 0
