# MCSM Vita Data Builder

A simple Windows app that creates the exact `ux0:data/mcsm` folder required by the loader. It contains no game files: every APK, OBB, library, and episode archive comes from files the user selects locally.

## Using the app

1. Press **Scan folder** and choose the folder containing your game files. The app searches subfolders for the supported PowerVR APK, both base OBBs, Episode 2–8 folders, chapter OBBs, ZIPs, and full nested chapter bundles.
2. Review the compact verification labels. Missing files can still be selected individually with **Browse**. The APK manifest and exact supported PowerVR fingerprint are checked; Mali, Adreno, altered, and older APKs are rejected. Both OBBs must match too.
3. Keep **Balanced — recommended**, choose another preset, or press **Make custom**. **View profiles** shows the useful differences at a glance; the custom maker offers an Easy mode with six clear choices and an Advanced mode with every supported exact value.
4. Optionally add Episode 2–8 sources. The app accepts either:
   - the episode folder (including the original `com.telltalegames.minecraft100/files/Net` layout), or
   - a chapter `.obb`, ZIP, or large full bundle containing one or more nested chapter packages.

The chapter importer copies generic and PowerVR/SGX assets, ignores Mali/Adreno variants in mixed bundles, and rejects packages with no PowerVR evidence.
5. Choose the output location and press **Build Data Folder**.
6. Copy the resulting `mcsm` folder to `ux0:data/` with VitaShell.

The app copies both OBBs and extracts their NCTT contents into `mcsm/assets`, then adds the APK assets and optional episodes. A correct Episode 1 base contains 144 active OBB archives/descriptors (about 817 MB) before APK assets or controller fixes are added. The build is rejected instead of finalized if that boot-critical set is incomplete. It also detects episode numbers from their real archive names, creates the runtime folders, writes easy-to-edit graphics/game settings, and moves an existing output to a timestamped backup instead of deleting it.

The main screen intentionally stays compact: exact version/renderer results, both OBB states, automatic support packs, episodes/mods, profile details, and final readiness remain visible without separate wizard pages.

### Custom profile maker

The output screen includes **Make custom** beside the profile selector. Saving the dialog automatically selects Custom and writes the chosen Easy or Advanced values into `settings/graphics.txt`. Easy mode covers picture, motion, PowerVR GPU identity, effects, world detail, and CPU power. Advanced mode includes the 60 FPS cap, exact resolution, detail, draw distance, clock, VSync, upscale filter, outlines, shadows, thin-seam filtering, and the white glass/light fix. These remain normal text settings that users can change later.

### Controller button fix

A distributed EXE can have a user-supplied controller button-fix pack built directly into it. When present, the app copies every included `.d3dtx` and `.d3dmesh` into `mcsm/assets` automatically. Open **Fix & mods** to supply a different complete fix folder/ZIP or to see whether the current EXE contains one.

### Offline choice statistics

A local EXE can also embed the supported `choice.prop` crowd-choice dataset. The builder installs it at the data root and under `Temp`, matching the loader's offline statistics path. CPU/memory stubs, preference seeds, diagnostics, and shader-cache directories are not bundled because the loader creates or maintains them at runtime.

### Data add-ons / mods

Open **Fix & mods** to add folders or ZIPs that should be merged into the finished `mcsm` directory. A selected folder named `mcsm`, a folder containing `mcsm`, and ZIP paths containing `mcsm/` are recognized automatically.

> **Experimental and untested on Vita.** Data add-ons are applied last and may replace assets or settings. The builder path-checks archive entries and blocks replacement of the canonical OBBs and native runtime libraries, but it cannot prove that a mod is compatible with this port.

## Build the EXE

Windows with the .NET 8 SDK:

```powershell
.\build.ps1
```

To embed your own local controller button-fix assets:

```powershell
.\build.ps1 -ButtonFixPath "C:\path\to\button fix"
```

To include both local support packs:

```powershell
.\build.ps1 `
  -ButtonFixPath "C:\path\to\button fix" `
  -ChoiceDataPath "C:\path\to\choice.prop"
```

The generated local `LocalAssets/button-fix.zip` is ignored by Git. The source repository never contains or publishes those game assets.

The self-contained executable is written to `dist/MCSM-Vita-Data-Builder.exe`. The `dist`, `bin`, and `obj` folders are intentionally ignored and should be distributed as release artifacts rather than committed to source control.

## Supported inputs

- APK: the exact supported PowerVR v1.37 (`versionCode 40137`) build of `com.telltalegames.minecraft100`, with all five required ARMv7 libraries.
- Base data: the exact supported PowerVR main `.obb` plus patch `.obb`; both are required and fingerprint-checked.
- Folder scan: recursively discovers the verified base set and recognizable Episode 2–8 folders, chapter OBBs, ZIPs, and nested full bundles; it never accepts a candidate that fails the normal checks.
- Graphics: Balanced (recommended), Performance, Quality, Battery, or a builder-created Easy/Advanced Custom profile.
- Extra episodes: folders, ZIPs, or ZIP-compatible chapter OBBs containing PowerVR `.ttarch2` and `.lua` files for Episodes 2–8. ZIPs and OBBs may be nested up to four levels deep.
- Controller fix: a folder/ZIP containing `.d3dtx` files and all four required face-button `.d3dmesh` files.
- Offline statistics: the supported non-empty `choice.prop` dataset when built into the local EXE.
- Experimental mods: data folders or ZIPs to merge into `mcsm`.

The builder does not download or bundle game data. Its GPL-compatible embedded `ttarchext` helper extracts only the exact user-selected, fingerprint-verified MCSM OBBs; source and attribution are under `ThirdParty/ttarchext`.

The base-game pickers expose only `.apk` and `.obb` formats. All hashes are calculated locally, and the Build button stays locked until the PowerVR APK, main OBB, and patch OBB have each passed validation.
