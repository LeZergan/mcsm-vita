# MCSM Vita Data Builder

A simple Windows app that creates the exact `ux0:data/mcsm` folder required by the loader. It contains no game files: every APK, OBB, library, and episode archive comes from files the user selects locally.

## Using the app

1. Choose the 32-bit PowerVR `com.telltalegames.minecraft100` APK.
2. Choose its main `.obb` file. A single matching `patch.*.obb` beside the main OBB is included automatically.
3. Optionally add Episode 2–8 sources. The app accepts either:
   - the episode folder (including the original `com.telltalegames.minecraft100/files/Net` layout), or
   - a ZIP containing that episode's `.ttarch2` and descriptor `.lua` files.
4. Choose the output location and press **Build Data Folder**.
5. Copy the resulting `mcsm` folder to `ux0:data/` with VitaShell.

The app detects episode numbers from their real archive names, flattens episode files into `mcsm/assets`, creates the runtime folders, writes easy-to-edit graphics/game settings, and validates the result before replacing an existing output. Existing output is moved to a timestamped backup instead of being deleted.

## Build the EXE

Windows with the .NET 8 SDK:

```powershell
.\build.ps1
```

The self-contained executable is written to `dist/MCSM-Vita-Data-Builder.exe`. The `dist`, `bin`, and `obj` folders are intentionally ignored and should be distributed as release artifacts rather than committed to source control.

## Supported inputs

- APK: ARMv7 build containing all five required native libraries.
- Base data: one main OBB. A neighboring patch OBB is optional.
- Extra episodes: folders or ZIPs containing `.ttarch2` and `.lua` files for Episodes 2–8.

The builder does not download, bundle, or link to copyrighted game data.
