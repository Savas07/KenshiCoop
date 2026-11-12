#!/bin/bash
# Fetches the plugin's build dependencies at pinned versions (macOS / any POSIX).
#
#   third_party/KenshiLib_deps   KenshiLib_Examples_deps @ e75769b (Ogre/MyGUI
#                                import libs + Boost 1.60 headers), via Git LFS
#   third_party/KenshiLib_0.5.0  KenshiLib 0.5.0 headers (tag v0.5.0) + KenshiLib.lib
#                                (release zip). 0.5.0 is the KenshiLib that
#                                RE_Kenshi 0.3.5 installs, so the plugin links
#                                against exactly what the game loads.
#   third_party/enet/enet        ENet v1.3.18 + the two patches in third_party/enet/patches
#
# Usage: tools/crossover/fetch-deps.sh      Requires: git, git-lfs, curl, unzip, shasum.
# Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")/../.."
TP=third_party

command -v git-lfs >/dev/null || { echo "git-lfs is required (brew install git-lfs)" >&2; exit 1; }

checkout() { # url dir commit
  [ -e "$2/.git" ] || GIT_LFS_SKIP_SMUDGE=1 git clone -q "$1" "$2"
  git -C "$2" fetch -q origin
  git -C "$2" checkout -q "$3"
  [ "$(git -C "$2" rev-parse HEAD)" = "$3" ] || { echo "unexpected commit in $2" >&2; exit 1; }
}
safe_unzip_list() { # zip -> fails if any entry is absolute or contains ".."
  local list; list="$(unzip -Z1 "$1")"
  if grep -qE '^/|(^|/)\.\.(/|$)' <<<"$list"; then
    echo "unsafe path in $1" >&2; exit 1
  fi
}

# KenshiLib_Examples_deps: Ogre/MyGUI .lib files and boost.zip are LFS objects;
# git-lfs verifies each one against its SHA-256 pointer.
checkout https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps.git \
         $TP/KenshiLib_deps e75769b60dc7e7f0161580b1e86a01912f6404dd
git -C $TP/KenshiLib_deps lfs install --local >/dev/null
git -C $TP/KenshiLib_deps lfs pull
git -C $TP/KenshiLib_deps lfs fsck >/dev/null
B=$TP/KenshiLib_deps/boost_1_60_0
if [ ! -f "$B/.extracted" ]; then
  safe_unzip_list "$B/boost.zip"
  T="$(mktemp -d "$B/.tmp.XXXXXX")"
  unzip -q "$B/boost.zip" -d "$T"
  rm -rf "$B/boost"; mv "$T/boost" "$B/boost"; rm -rf "$T"
  touch "$B/.extracted"
fi

# KenshiLib 0.5.0: headers from the tag, import library from the release.
K=$TP/KenshiLib_0.5.0
checkout https://github.com/BFrizzleFoShizzle/KenshiLib.git $K/src 11cd65d557dea76c6540239acf467e0e06e09cf7
if [ ! -f $K/KenshiLib.lib ]; then
  Z="$(mktemp -d)"; trap 'rm -rf "$Z"' EXIT
  curl -fsSL --proto '=https' --proto-redir '=https' -o "$Z/kl.zip" \
    https://github.com/BFrizzleFoShizzle/KenshiLib/releases/download/v0.5.0/KenshiLib_v0.5.0.zip
  if ! echo "ad4534e92a3a244cf25d724d6a695d42ef8770e4b7a1f91ed53affcac171d0c4  $Z/kl.zip" | shasum -a 256 -c - >/dev/null 2>&1; then
    echo "SHA-256 mismatch for KenshiLib_v0.5.0.zip" >&2; exit 1
  fi
  unzip -q "$Z/kl.zip" KenshiLib.lib -d "$Z"
  mv "$Z/KenshiLib.lib" "$K/KenshiLib.lib"
fi

# ENet 1.3.18 + KenshiCoop's patches (C89 loops for the v100 C compiler; socket hooks).
checkout https://github.com/lsalzman/enet.git $TP/enet/enet 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a
for p in 0001-enet-c89-for-loops.patch 0002-enet-socket-hooks.patch; do
  if git -C $TP/enet/enet apply -p4 --reverse --check "../patches/$p" 2>/dev/null; then
    continue                                    # already applied
  fi
  git -C $TP/enet/enet apply -p4 "../patches/$p"
done
echo "Dependencies ready."
