#!/bin/bash
# Builds KenshiCoop.dll (x64, v100 toolset) inside a CrossOver bottle, without
# MSBuild or Visual Studio. Run tools/crossover/setup-toolchain.sh and
# tools/crossover/fetch-deps.sh first.
#
# Usage: tools/crossover/build.sh [Release|Harness]
#   KC_BOTTLE  bottle name (default: KenshiCoop)
# Output: build/crossover/<config>/KenshiCoop.{dll,pdb,map} + build.log
#
# The source list comes from src/plugin/KenshiCoop.vcxproj (honouring
# ExcludedFromBuild for the config). The cl/link options follow the project's
# Release/Harness settings under the v100 toolset; differences from an MSBuild
# build: /Z7 instead of /Zi (same code), and no embedded manifest.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CONFIG="${1:-Release}"
BOTTLE="${KC_BOTTLE:-KenshiCoop}"
[[ "$BOTTLE" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "invalid bottle name: $BOTTLE" >&2; exit 2; }
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin"
DRIVE_C="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c"
STAGE="$DRIVE_C/kc"                      # C:\kc inside the bottle
OUT="$REPO/build/crossover/$CONFIG"
KL5="$REPO/third_party/KenshiLib_0.5.0"
DEPS="$REPO/third_party/KenshiLib_deps"

case "$CONFIG" in
  Release) DEFS="/D NDEBUG /D KENSHICOOP_EXPORTS /D _WINDOWS /D _USRDLL /D WIN32_LEAN_AND_MEAN" ;;
  Harness) DEFS="/D NDEBUG /D KENSHICOOP_EXPORTS /D KENSHICOOP_HARNESS /D _WINDOWS /D _USRDLL /D WIN32_LEAN_AND_MEAN" ;;
  *) echo "unsupported config: $CONFIG (Release|Harness)" >&2; exit 2 ;;
esac

for p in "$DRIVE_C/vc10/vcvars64.bat" "$KL5/KenshiLib.lib" "$KL5/src/Include/kenshi" \
         "$DEPS/KenshiLib/Libraries/OgreMain_x64.lib" "$DEPS/KenshiLib/Libraries/MyGUIEngine_x64.lib" \
         "$DEPS/boost_1_60_0/.extracted" "$REPO/third_party/enet/enet/include/enet/enet.h"; do
  [ -e "$p" ] || { echo "missing: $p (run setup-toolchain.sh / fetch-deps.sh)" >&2; exit 1; }
done

# 1) Stage only what the build needs into the bottle (it has no drive mapped
#    to the Mac, so sources are copied in rather than read in place).
mkdir -p "$STAGE/kenshilib/lib" "$STAGE/third_party/KenshiLib_deps" "$STAGE/third_party/enet"
rsync -a --delete "$REPO/src/" "$STAGE/src/"
rsync -a --delete "$REPO/third_party/vc10_compat/" "$STAGE/third_party/vc10_compat/"
rsync -a --delete "$REPO/third_party/kenshilib_compat/" "$STAGE/kenshilib/compat/"
rsync -a --delete "$KL5/src/Include/" "$STAGE/kenshilib/Include/"
cp "$KL5/KenshiLib.lib" "$DEPS/KenshiLib/Libraries/OgreMain_x64.lib" \
   "$DEPS/KenshiLib/Libraries/MyGUIEngine_x64.lib" "$STAGE/kenshilib/lib/"
rsync -a --delete --exclude boost.zip --exclude '.tmp.*' "$DEPS/boost_1_60_0/" "$STAGE/third_party/KenshiLib_deps/boost_1_60_0/"
rsync -a --delete --exclude .git "$REPO/third_party/enet/enet/" "$STAGE/third_party/enet/enet/"

# 2) Sources for this config, from the .vcxproj. A file is skipped when it has
#    ExcludedFromBuild=true with no condition, or with a condition naming this
#    configuration ('<cfg>' or '<cfg>|x64').
SRCS=$(awk -v cfg="$CONFIG" '
  function flush() { if (pend != "" && !skip) print pend; pend = ""; skip = 0 }
  /<ClCompile Include="/ {
    flush()
    match($0, /Include="[^"]*"/); pend = substr($0, RSTART+9, RLENGTH-10)
    if ($0 ~ /\/>[[:space:]]*$/) flush()
    next
  }
  pend != "" && /<ExcludedFromBuild/ && />[[:space:]]*true[[:space:]]*</ {
    if ($0 !~ /Condition=/ || index($0, "==\x27" cfg "\x27") || index($0, "==\x27" cfg "|x64\x27")) skip = 1
    next
  }
  pend != "" && /<\/ClCompile>/ { flush() }
  END { flush() }
' "$REPO/src/plugin/KenshiCoop.vcxproj")
NSRC=$(printf '%s\n' "$SRCS" | grep -c . || true)
[ "$NSRC" -gt 0 ] || { echo "no sources found in KenshiCoop.vcxproj" >&2; exit 1; }
echo "Sources ($CONFIG): $NSRC"
printf '%s\n' "$SRCS" | sed 's/$/\r/' > "$STAGE/srcs_$CONFIG.rsp"

# 3) Build script for cmd.exe. Include order follows the project: KenshiLib,
#    Ogre, Boost and ENet first, then the compiler and SDK headers.
OBJ='C:\kc\obj\'"$CONFIG"
{
  echo '@echo off'
  echo 'call C:\vc10\vcvars64.bat'
  echo 'set "KL=C:\kc\kenshilib"'
  echo 'set "INCLUDE=%KL%\compat;%KL%\Include;%KL%\Include\ogre;C:\kc\third_party\KenshiLib_deps\boost_1_60_0;C:\kc\third_party\enet\enet\include;%INCLUDE%;C:\kc\third_party\vc10_compat"'
  echo 'set "LIB=%KL%\lib;%LIB%"'
  echo "if exist $OBJ rmdir /s /q $OBJ"
  echo "mkdir $OBJ"
  echo 'cd /d C:\kc\src\plugin'
  # /Z7 keeps debug info in the .obj files: /Zi would need mspdbsrv for every compile.
  echo "cl /c /nologo /W3 /O2 /Oi /GL /Gy /MD /EHsc /GS /fp:precise /Zc:wchar_t /Zc:forScope /Gd /Z7 $DEFS /D UNICODE /D _UNICODE /D _WINDLL /Fo$OBJ\\ @C:\\kc\\srcs_$CONFIG.rsp"
  echo 'if errorlevel 1 goto fail'
  echo "link /nologo /DLL /OUT:$OBJ\\KenshiCoop.dll /DEBUG /PDB:$OBJ\\KenshiCoop.pdb /MAP:$OBJ\\KenshiCoop.map /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG /DYNAMICBASE /NXCOMPAT /MACHINE:X64 /MANIFEST:NO /INCREMENTAL:NO $OBJ\\*.obj kenshilib.lib OgreMain_x64.lib MyGUIEngine_x64.lib ws2_32.lib winmm.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib"
  echo 'if errorlevel 1 goto fail'
  echo 'echo BUILD_OK'
  echo 'goto end'
  echo ':fail'
  echo 'echo BUILD_FAILED'
  echo ':end'
  # link.exe leaves mspdbsrv.exe running, and Wine would wait for it forever.
  echo 'mspdbsrv.exe -stop >nul 2>&1'
} | sed 's/$/\r/' > "$STAGE/build_$CONFIG.bat"

# 4) Build.
LOG="$STAGE/build_$CONFIG.log"
"$CX/wine" --bottle "$BOTTLE" cmd /c "C:\\kc\\build_$CONFIG.bat" > "$LOG" 2>&1 || true
tr -d '\r' < "$LOG" | grep -E ' error |fatal error|LNK[0-9]|BUILD_' | tail -40 || true

# 5) Copy the results back to the Mac.
grep -q BUILD_OK "$LOG" || { echo "Build failed; full log: $LOG" >&2; exit 1; }
mkdir -p "$OUT"
cp "$STAGE/obj/$CONFIG/KenshiCoop.dll" "$STAGE/obj/$CONFIG/KenshiCoop.pdb" "$STAGE/obj/$CONFIG/KenshiCoop.map" "$OUT/"
cp "$LOG" "$OUT/build.log"
echo "OK -> $OUT"
shasum -a 256 "$OUT/KenshiCoop.dll"
