# Minecraft: Story Mode — PS Vita

Unofficial PS Vita loader for the Android PowerVR build of *Minecraft: Story Mode*.

> [!WARNING]
> **MCSM 1.10 is a public prerelease for testing.** Bugs, freezes, and performance problems are still possible. The release does not yet include a complete progcache, so new scenes will stutter while shaders compile. Replaying cached scenes should be smoother.

[Download prerelease](../../releases/tag/v1.10) · [Report a game problem](../../issues/new?template=bug-report.yml) · [Join Discord](https://discord.gg/EYYTxeXCq)

| Component | Current version |
| --- | --- |
| Vita loader | MCSM 1.10 prerelease |
| Windows data builder | v1.9 |
| Supported Android set | PowerVR v1.37 (`40137`) |
| Final data path | `ux0:data/mcsm` |

## Download

The [MCSM 1.10 prerelease](../../releases/tag/v1.10) contains:

- **`MCSM-1.10.vpk`** — the Vita loader, LiveArea artwork, and trophy resources.
- **`MCSM-Vita-Data-Builder-v1.9.exe`** — prepares the Vita data folder from files you legally own.

Android game files are not included. You must supply your own supported APK, main OBB, patch OBB, and optional episode files.

## Quick setup

### 1. Prepare the Vita

Install or enable:

- [VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases)
- [kubridge](https://github.com/TheOfficialFloW/kubridge/releases/tag/v0.1) under `*KERNEL`
- [FdFix](https://github.com/TheOfficialFloW/FdFix/releases/tag/v1.0) under `*KERNEL` — do not use it alongside rePatch
- `libshacccg.suprx` in `ur0:data/` — [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D/releases) can install it

[NoTrpDrm](https://github.com/Rinnegatamante/NoTrpDrm/releases/tag/v.1.1) is **not required to play**. It is required only if you want trophy support.

### 2. Install the loader

Install `MCSM-1.10.vpk` with VitaShell.

### 3. Build the data folder

1. Open `MCSM-Vita-Data-Builder-v1.9.exe` on Windows.
2. Leave the mode on **Full setup**.
3. Press **Scan folder** and select the folder containing your Android files.
4. Confirm that the APK, main OBB, and patch OBB are marked as verified PowerVR v1.37 files.
5. Leave **Balanced — recommended** selected unless you want another profile.
6. Add any owned Episodes 2–8 now, or add them later.
7. Press **Build Data Folder**.
8. Copy the finished `mcsm` folder into `ux0:data/` on the Vita.

The final layout must contain `ux0:data/mcsm/assets` and `ux0:data/mcsm/settings`.

### Add episodes later

You do not need to rebuild the base data:

1. Select **Add episodes only** in the builder.
2. Add chapter folders, PowerVR chapter OBBs, ZIPs, or full nested bundles.
3. Press **Build Chapter Pack**.
4. Copy the small resulting `mcsm` folder into `ux0:data/` and choose Merge/Replace in VitaShell.

The chapter pack contains only selected chapter assets and copy instructions. It does not contain the APK libraries, rebuilt base data, settings, saves, or preferences.

## Help complete the progcache

The prerelease builds shader programs while you play. A new program can cause a visible stutter the first time it appears. We need community caches from all eight episodes so a later release can start with broader coverage.

To contribute:

1. Play an episode through as much content as possible.
2. Close the game.
3. Copy the complete `ux0:data/mcsm_progcache` folder to your PC.
4. ZIP that folder.
5. Upload it to the matching episode issue and mention your graphics profile.

| Episode | Progcache upload issue |
| --- | --- |
| 1 — The Order of the Stone | [Upload for Episode 1](../../issues/3) |
| 2 — Assembly Required | [Upload for Episode 2](../../issues/4) |
| 3 — The Last Place You Look | [Upload for Episode 3](../../issues/5) |
| 4 — A Block and a Hard Place | [Upload for Episode 4](../../issues/6) |
| 5 — Order Up! | [Upload for Episode 5](../../issues/7) |
| 6 — A Portal to Mystery | [Upload for Episode 6](../../issues/8) |
| 7 — Access Denied | [Upload for Episode 7](../../issues/9) |
| 8 — A Journey's End? | [Upload for Episode 8](../../issues/10) |

Upload **only** the zipped `mcsm_progcache` folder. Do not include `ux0:data/mcsm`, assets, saves, APKs, OBBs, logs, or other game data. You can also share the ZIP in the [MCSM Vita Discord](https://discord.gg/EYYTxeXCq).

## Report problems

Please report any issue you have with the game—not only crashes. This includes freezes, dead stops, input problems, missing choices, saves not loading, settings not saving, language problems, audio delay, broken graphics, or severe performance drops.

Use the [game bug report form](../../issues/new?template=bug-report.yml) or post in the [Discord](https://discord.gg/EYYTxeXCq). Include:

- episode and exact scene/checkpoint
- what happened and what you expected
- whether it happens every time
- graphics profile and resolution
- installed plugins, especially CapUnlocker or overclock tools
- a screenshot or short video when useful

Never upload proprietary game assets, APKs, OBBs, or saves containing private information.

## Builder and game features

- Exact APK version and PowerVR renderer verification; Mali, Adreno, altered, and older builds are rejected.
- Main and patch OBBs are PC-side extraction inputs only. OBB clones are removed from the finished Vita folder.
- Balanced, Performance, Quality, Battery, and editable Easy/Advanced Custom graphics profiles.
- Advanced uncapped, 60, 30, 20, and 15 FPS options.
- English, French, German, Spanish, Portuguese, Russian, and Chinese text selection. Voices remain English.
- Persistent font-size/preferences synchronization.
- Vita on-screen keyboard support for save-slot title renaming.
- Personal choices and offline crowd-choice percentages.
- Physical L/R input is delivered as both L1/L2 and R1/R2 without duplicate presses.
- Built-in controller prompts and optional experimental data add-ons/mods.

More builder details are in [data-builder/README.md](data-builder/README.md).

## Optional performance plugins

[CapUnlocker v1.4](https://github.com/GrapheneCt/CapUnlocker/releases/tag/v1.4) exposes the reserved fourth core. The loader still runs without it and falls back to the normal three user cores.

Use at most one overclock menu:

- [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus/releases/tag/v1.4)
- [PSV-VSH-Menu](https://github.com/joel16/PSV-VSH-Menu/releases/tag/3.40)
- [LOLIcon](https://github.com/dots-tb/LOLIcon/releases/tag/1.0.1)

## Trophy support and repair

Trophies require [NoTrpDrm](https://github.com/Rinnegatamante/NoTrpDrm/releases/tag/v.1.1) under `*KERNEL`. The game itself does not.

If an older test build left a broken trophy registration:

1. Install [Trophy Manager v1.03](https://github.com/ONElua/TrophyManager/releases/tag/1.03).
2. Disable Wi-Fi and delete only the Minecraft: Story Mode trophy set through Trophy Manager.
3. Open the normal Vita Trophies app once, then reboot.
4. Reinstall the latest VPK with NoTrpDrm enabled.

Do not manually delete random files from `ur0:user/.../trophy`; that can leave the trophy database inconsistent.

## Known prerelease limitations

- New scenes can stutter while missing progcache entries compile. This is expected in the current prerelease.
- Crowded scenes may still fall to around 20 FPS even when the target is 30 or 60 FPS.
- First boot can remain black for roughly 30 seconds while caches are created. An indefinite black screen is not normal—please report it.
- Experimental mods/data add-ons are not yet validated on Vita hardware.
- All features still need broader real-hardware coverage across Episodes 1–8.

## Videos

1. [First build — menus](https://youtu.be/q_X8j8XZ-NU)
2. [Gameplay](https://youtu.be/TzjYkUHpF6k)
3. [Full trial chapter — fixed animations + audio](https://youtu.be/BltUYWmdRq8)
4. [Gameplay & chapter presentation](https://youtu.be/QsmPfikGh0A)
5. [Texture fixes](https://youtu.be/BrGuSMM_j5U)
6. [Stutter fix & optimisation — shader caching](https://youtu.be/IK8h5NXLh2k)
7. [Performance improvements & fixes](https://youtu.be/4aSJUfWHK3w)

## Build from source

The loader requires the **softfp** VitaSDK with vitaGL, vitaShaRK, mathneon, and kubridge. The build rejects a hard-float SDK.

```powershell
$env:VITASDK = "C:\path\to\vitasdk-softfp"
.\build_vpk.ps1
```

Build the Windows data preparer with:

```powershell
.\data-builder\build.ps1
```

The loader output is `build_local/MCSM-1.10.vpk`. VPKs, extracted game data, and proprietary Android files are intentionally excluded from source control.

## Legal

No Android executable, native library, OBB, episode, or gameplay archive is included or linked. This is an unofficial fan project and is not affiliated with Mojang, Microsoft, Telltale/LCG, Skybound, or Netflix. Trademarks belong to their owners.

## Credits

- **Andy "TheFloW" Nguyen** — `.so` loader, so_util, fios, libc_bridge, kubridge.
- **Rinnegatamante** — vitaGL, vitaShaRK, mathneon.
- **Volodymyr Atamanenko** — soloader-boilerplate, FalsoJNI.
- **bythos14** — kubridge fork. **Brad Conte** — SHA-1. miniz authors. VitaSDK team.
- **Telltale / Mojang / Microsoft** — game and IP; not affiliated.

Full attributions: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

This whole thing is vibecoded slop made with **Claude 5** and **ChatGPT 5.6 sol**.

## License

Copyright © 2026 LeZergan.

Licensed under the [GNU General Public License v3.0](LICENSE). Inherited and third-party components retain their own notices; see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) and `LICENSES/`.
