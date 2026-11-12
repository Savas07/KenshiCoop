# KenshiCoop on macOS with CrossOver

Build `KenshiCoop.dll` on a Mac, with no Windows, no VM and no Visual Studio.
Then run it in Kenshi under CrossOver.

Tested on: Apple Silicon (M2 Pro), CrossOver 25.0.1, Kenshi 1.0.68 (Steam),
RE_Kenshi 0.3.5. The mod loads and reaches the main menu.

## Build

You need CrossOver and `git-lfs` (`brew install git-lfs`). About 1 GB of
downloads.

```bash
tools/crossover/setup-toolchain.sh   # once: new bottle + compiler
tools/crossover/fetch-deps.sh        # once: dependencies
tools/crossover/build.sh             # every build
```

The DLL ends up in `build/crossover/Release/KenshiCoop.dll`.

- `build.sh Harness` builds the test version.
- The bottle is called `KenshiCoop`. Use `KC_BOTTLE=<name>` for another name.
- The setup only works on a new bottle. It will not touch an existing one, such
  as your usual Steam bottle.
- On the Mac, the bottle's `C:` is
  `~/Library/Application Support/CrossOver/Bottles/<bottle>/drive_c`.

## Play

`wine` below means
`/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine`.

1. **Install Steam** in the bottle with Valve's `SteamSetup.exe` and sign in.
   To skip the 12 GB download, copy your Kenshi folder into
   `drive_c/Program Files (x86)/Steam/steamapps/common/Kenshi`, and copy
   `appmanifest_233860.acf` into `drive_c/Program Files (x86)/Steam/steamapps/`.

2. **Kenshi closes after 2 seconds?** The bottle is missing the Visual C++ 2010
   runtime. Install Microsoft's
   [vcredist_x64.exe](https://download.microsoft.com/download/1/6/5/165255E7-1014-4D0A-B094-B6A430A6BFFC/vcredist_x64.exe)
   (SHA-256 `f3b7a76d84d23f91957aa18456a14b4e90609e4ce8194c5653384ed38dada6f3`):
   `wine --bottle <bottle> vcredist_x64.exe /q /norestart`

3. **Install RE_Kenshi 0.3.5** from its GitHub release. Check the SHA-256
   shown there, run the installer in the bottle and choose the Kenshi folder.

4. **Stuck on the Wwise/Havok/PhysX screen?** Wine's shader compiler crashes
   when RE_Kenshi compiles shaders. Use the Microsoft one that comes with Kenshi:

   ```bash
   wine --bottle <bottle> reg add 'HKCU\Software\Wine\AppDefaults\kenshi_x64.exe\DllOverrides' \
        /v d3dcompiler_43 /t REG_SZ /d native,builtin /f
   ```

   Then copy `<Kenshi>\D3DCompiler_43.dll` into `<Kenshi>\RE_Kenshi\`.

5. **Add the mod.** Create `<Kenshi>\mods\KenshiCoop\` with your
   `KenshiCoop.dll`, plus `RE_Kenshi.json`, `KenshiCoop.mod` and
   `coop_config.json` from `dist/mod-kit/KenshiCoop/`. Enable it in the Mods
   menu.

## Details

**What the setup does.**
- Downloads two Microsoft files, both checked by SHA-256: the Windows SDK 7.1
  ISO and the VC++ 2010 SP1 compiler update (KB2519277).
- Unpacks them with `msiexec /a`. That only extracts files; no installer runs.
- Puts the compiler in `C:\vc10`.

**Bottle links to your Mac.** CrossOver links a new bottle to your home folder
and your whole disk. The setup removes those links (every drive except `C:`,
and the Documents/Downloads/... links), so the build only uses the files the
scripts copy in.

**Dependencies.** All are pinned to a commit or a SHA-256:
- KenshiLib 0.5.0;
- KenshiLib_Examples_deps `e75769b`, for the Ogre/MyGUI libs and Boost;
- ENet 1.3.18 with the repo's patches.

**Why KenshiLib 0.5.0.** The KenshiLib in KenshiLib_Examples_deps does not
compile the current source.
- 0.3.0 defines `BuildingDesignation` twice and lacks `CraftingItem`. KenshiLib
  fixed both on 2026-07-25, in commits `0e0ded7` and `6f9168d`.
- 0.4.0 moved `CombatClass.h` into `kenshi/combat/`.

0.5.0 is also the version RE_Kenshi 0.3.5 puts in the game. A one-line shim,
`third_party/kenshilib_compat/kenshi/CombatClass.h`, covers the moved header.
The 0.5.0 headers can differ from the ones official builds use. If something
works differently from an official build, check this first.

**How the build differs from MSBuild.**
- `cl` and `link` are called directly, with the project's Release settings and
  the source list read from the `.vcxproj`.
- It uses `/Z7` instead of `/Zi` (same code) and does not embed a manifest.
- The script stops `mspdbsrv.exe` at the end. Without that, Wine waits forever.

**Check.** For upstream v0.58.1, this DLL imports the same 474 symbols as the
official one. The only extra is one MyGUI constructor that the official build
inlined. `dumpbin /imports` comes with the toolchain.

**Not tested.** Intel Macs, other CrossOver versions, running the Harness build
in game, and `src/prototest`/`src/nettest`.
