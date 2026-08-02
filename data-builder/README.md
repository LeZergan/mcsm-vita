# MCSM Vita Data Builder

A simple Windows app that creates the exact `ux0:data/mcsm` folder required by the loader. It contains no game files: every APK, OBB, library, and episode archive comes from files the user selects locally.

## Using the app

1. Choose the 32-bit PowerVR `com.telltalegames.minecraft100` `.apk` file.
2. Choose its main `.obb` and patch `.obb`. Selecting the main OBB automatically fills one matching `patch.*.obb` beside it, or you can choose the patch manually.
3. Optionally add Episode 2–8 sources. The app accepts either:
   - the episode folder (including the original `com.telltalegames.minecraft100/files/Net` layout), or
   - a ZIP containing that episode's `.ttarch2` and descriptor `.lua` files.
4. Choose the output location and press **Build Data Folder**.
5. Copy the resulting `mcsm` folder to `ux0:data/` with VitaShell.

The app detects episode numbers from their real archive names, flattens episode files into `mcsm/assets`, creates the runtime folders, writes easy-to-edit graphics/game settings, and validates the result before replacing an existing output. Existing output is moved to a timestamped backup instead of being deleted.

### Controller button fix

A distributed EXE can have a user-supplied controller button-fix pack built directly into it. When present, the app copies every included `.d3dtx` and `.d3dmesh` into `mcsm/assets` automatically. Open **Fix & mods** to supply a different complete fix folder/ZIP or to see whether the current EXE contains one.

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

The generated local `LocalAssets/button-fix.zip` is ignored by Git. The source repository never contains or publishes those game assets.

The self-contained executable is written to `dist/MCSM-Vita-Data-Builder.exe`. The `dist`, `bin`, and `obj` folders are intentionally ignored and should be distributed as release artifacts rather than committed to source control.

## Supported inputs

- APK: one `.apk` ARMv7 build containing all five required native libraries.
- Base data: one main `.obb` plus one separate patch `.obb`; both are required.
- Extra episodes: folders or ZIPs containing `.ttarch2` and `.lua` files for Episodes 2–8.
- Controller fix: a folder/ZIP containing `.d3dtx` files and all four required face-button `.d3dmesh` files.
- Experimental mods: data folders or ZIPs to merge into `mcsm`.

The builder does not download, bundle, or link to copyrighted game data.

The base-game pickers expose only `.apk` and `.obb` formats. The Build button stays locked until the APK, main OBB, and patch OBB have each passed validation.
