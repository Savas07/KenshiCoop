#!/bin/bash
# Sets up the Visual C++ 2010 SP1 x64 (v100) toolchain inside a NEW CrossOver
# bottle on macOS, without running any Microsoft installer.
#
# What it does:
#   1. Downloads the official Microsoft packages (HTTPS, pinned SHA-256).
#   2. Creates a dedicated bottle and removes its links to your Mac
#      (every drive except C:, and the Documents/Downloads/... symlinks).
#   3. Extracts the compiler + Windows SDK 7.1 headers/libs with
#      "msiexec /a" (an administrative image: files only, no setup code).
#   4. Lays them out as C:\vc10 and writes C:\vc10\vcvars64.bat.
#
# Usage: tools/crossover/setup-toolchain.sh
#   KC_BOTTLE   bottle name            (default: KenshiCoop)
#   KC_CACHE    download cache folder  (default: ~/Library/Caches/kenshicoop-toolchain)
# Re-running it on a bottle it created is safe (it redoes steps 3-4).
# Requires: CrossOver (tested with 25.0.1), curl, shasum, hdiutil (all on macOS).
set -euo pipefail

BOTTLE="${KC_BOTTLE:-KenshiCoop}"
CACHE="${KC_CACHE:-$HOME/Library/Caches/kenshicoop-toolchain}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CXBIN="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin"
B="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
MARKER="$B/.kenshicoop-toolchain"

[[ "$BOTTLE" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "invalid bottle name: $BOTTLE" >&2; exit 2; }
[ -x "$CXBIN/wine" ] || { echo "CrossOver not found in /Applications" >&2; exit 1; }
# Never rewire a bottle this script did not create (e.g. your normal Steam bottle).
if [ -d "$B" ] && [ ! -f "$MARKER" ]; then
  echo "Bottle '$BOTTLE' already exists and was not created by this script." >&2
  echo "Pick another name with KC_BOTTLE=<name>." >&2
  echo "If an earlier run of this script failed, delete that bottle in CrossOver and retry." >&2
  exit 1
fi

# --- 1. Downloads (official Microsoft URLs, pinned hashes) ------------------
mkdir -p "$CACHE"
fetch() { # url sha256
  local f; f="$CACHE/$(basename "$1")"
  if [ ! -f "$f" ]; then
    echo "Downloading $(basename "$1")..."
    curl -fL --proto '=https' --proto-redir '=https' --retry 3 -o "$f.part" "$1"
    mv "$f.part" "$f"
  fi
  if ! echo "$2  $f" | shasum -a 256 -c - >/dev/null 2>&1; then
    echo "SHA-256 mismatch for $f (delete it and retry)" >&2; exit 1
  fi
}
# Windows SDK 7.1 x64 ISO (headers/libs) and the VC++ 2010 SP1 compiler update
# (KB2519277), which ships complete vc_stdx86/vc_stdamd64 MSIs.
fetch https://download.microsoft.com/download/F/1/0/F10113F5-B750-4969-A255-274341AC6BCE/GRMSDKX_EN_DVD.iso \
      9f495e52f33d68125277cbca3565ce57904af7d506758a14691fa6e2479ad65c
fetch https://download.microsoft.com/download/7/5/0/75040801-126C-4591-BCE4-4CD1FD1499AA/VC-Compiler-KB2519277.exe \
      46497a249dac03f8a6d6b4f4a737546246752ca751ae16c34127a1d784232645

# --- 2. Bottle, cut off from the Mac ------------------------------------------
if [ ! -d "$B" ]; then
  echo "Creating bottle '$BOTTLE'..."
  "$CXBIN/cxbottle" --bottle "$BOTTLE" --create --template win10_64 \
      --description "KenshiCoop build bottle" >/dev/null 2>&1
  touch "$MARKER"
fi
# CrossOver maps Y: to your home and Z: to / (plus any mounted volume), and
# links the user folders to the real Mac ones. Keep only C: and plain folders,
# so the build only uses the files the scripts copy in.
for d in "$B/dosdevices/"*; do
  case "$(basename "$d")" in c:) ;; *) rm -f "$d" ;; esac
done
U="$B/drive_c/users/crossover"
for f in "$U"/*; do
  if [ -L "$f" ]; then rm "$f"; mkdir "$f"; fi
done
for f in Documents Downloads Music Pictures Videos; do
  mkdir -p "$U/$f"
  # A non-empty folder keeps Wine from turning it back into a symlink.
  [ -f "$U/$f/README-bottle.txt" ] || echo "Plain bottle folder (not linked to the Mac)." > "$U/$f/README-bottle.txt"
done

# --- 3. Extract the MSIs (administrative image, no setup code runs) ----------
SRC="$B/drive_c/_toolsrc"; X="$B/drive_c/_vc10x"; LOG="$B/drive_c/vc10-setup.log"
rm -rf "$SRC" "$X"
mkdir -p "$SRC/kb" "$SRC/sdkbuild" "$SRC/sdk"
"$CXBIN/cabextract" -q -d "$SRC/kb" -F 'vc_stdx86*' "$CACHE/VC-Compiler-KB2519277.exe"
"$CXBIN/cabextract" -q -d "$SRC/kb" -F 'vc_stdamd64*' "$CACHE/VC-Compiler-KB2519277.exe"
MNT="$(mktemp -d "${TMPDIR:-/tmp}/sdk71.XXXXXX")"
trap 'hdiutil detach "$MNT" >/dev/null 2>&1 || true; rmdir "$MNT" 2>/dev/null || true' EXIT
hdiutil attach -readonly -nobrowse -mountpoint "$MNT" "$CACHE/GRMSDKX_EN_DVD.iso" >/dev/null
cp "$MNT/Setup/WinSDKBuild_amd64/"* "$SRC/sdkbuild/"
cp "$MNT/Setup/WinSDK_amd64/"* "$SRC/sdk/"
chmod -R u+w "$SRC"
: > "$LOG"
for m in 'kb\vc_stdx86.msi' 'kb\vc_stdamd64.msi' 'sdkbuild\WinSDKBuild_amd64.msi' 'sdk\WinSDK_amd64.msi'; do
  echo "Extracting $m..."
  "$CXBIN/wine" --bottle "$BOTTLE" msiexec /a "C:\\_toolsrc\\$m" /qn 'TARGETDIR=C:\_vc10x' >>"$LOG" 2>&1 \
    || { echo "msiexec failed for $m, see $LOG" >&2; exit 1; }
done

# --- 4. Lay out C:\vc10 -------------------------------------------------------
V="$B/drive_c/vc10"
VCX86="$X/Program Files/Microsoft Visual Studio 10.0/VC"
VC64="$X/Program Files(64)/Microsoft Visual Studio 10.0/VC"
SDK="$X/Program Files/Microsoft SDKs/Windows/v7.1"
for p in "$VCX86/include" "$VC64/bin/amd64/cl.exe" "$SDK/Include/Windows.h" "$SDK/Lib/x64"; do
  [ -e "$p" ] || { echo "extraction incomplete, missing: $p (see $LOG)" >&2; exit 1; }
done
rm -rf "$V"
mkdir -p "$V/VC/bin" "$V/VC/lib" "$V/SDK/Lib" "$V/compat"
ditto "$VCX86/include"        "$V/VC/include"
ditto "$VCX86/lib/amd64"      "$V/VC/lib/amd64"
ditto "$VC64/lib/amd64"       "$V/VC/lib/amd64"
ditto "$VC64/bin/amd64"       "$V/VC/bin/amd64"
ditto "$SDK/Include"          "$V/SDK/Include"
ditto "$SDK/Lib/x64"          "$V/SDK/Lib/x64"
# The 2010 x64 runtime next to cl.exe, so nothing has to be installed to build.
cp "$X/Win/System64/msvcr100.dll" "$X/Win/System64/msvcp100.dll" "$V/VC/bin/amd64/"
# KB2519277's intrin.h includes ammintrin.h, which only the full VS2010 SP1 ships.
cp "$REPO/third_party/vc10_compat/ammintrin.h" "$V/compat/"
printf '%s\r\n' '@echo off' \
  'rem VC++ 2010 SP1 x64 + Windows SDK 7.1 (extracted, no installer).' \
  'set "VC10=C:\vc10"' \
  'set "PATH=%VC10%\VC\bin\amd64;%PATH%"' \
  'set "INCLUDE=%VC10%\VC\include;%VC10%\SDK\Include;%VC10%\compat"' \
  'set "LIB=%VC10%\VC\lib\amd64;%VC10%\SDK\Lib\x64"' > "$V/vcvars64.bat"
rm -rf "$SRC" "$X"

# --- Smoke test ----------------------------------------------------------------
BANNER="$("$CXBIN/wine" --bottle "$BOTTLE" 'C:\vc10\VC\bin\amd64\cl.exe' 2>&1 || true)"
case "$BANNER" in
  *"Compiler Version 16.00."*) echo "$BANNER" | grep "Compiler Version" ;;
  *) echo "cl.exe did not start; output:" >&2; echo "$BANNER" | tail -5 >&2; exit 1 ;;
esac
echo "Toolchain ready in bottle '$BOTTLE' (C:\\vc10)."
