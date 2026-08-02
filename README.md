# Minecraft: Story Mode — PS Vita (unofficial loader)

Runs the Android build of *Minecraft: Story Mode* on the PS Vita. The package contains the Vita loader, LiveArea presentation, and trophy resources — **no APK, OBBs, episodes, or Android game binaries**. Bring your own legally-owned copy. Built on [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate), rendered with [vitaGL](https://github.com/Rinnegatamante/vitaGL).

> **Public testing release.** Expect rough edges and keep backups of your saves.

## Download

The [latest GitHub release](../../releases/latest) contains both files needed for setup:

- **`MCSM-1.10.vpk`** — complete loader with LiveArea artwork and trophy support; install with VitaShell.
- **`MCSM-Vita-Data-Builder-v1.8.exe`** — creates the required `ux0:data/mcsm` folder from your legally owned Android files.

The release does not contain the Android game APK, OBBs, episodes, native libraries, or gameplay archives.

## Videos

Development progress, oldest to newest:

1. [First build — menus](https://youtu.be/q_X8j8XZ-NU)
2. [Gameplay](https://youtu.be/TzjYkUHpF6k)
3. [Full trial chapter — fixed animations + audio](https://youtu.be/BltUYWmdRq8)
4. [Gameplay & chapter presentation](https://youtu.be/QsmPfikGh0A)
5. [Texture fixes](https://youtu.be/BrGuSMM_j5U)
6. [Stutter fix & optimisation — shader caching](https://youtu.be/IK8h5NXLh2k)

## Legal

No Android game executable, native library, OBB, episode, or gameplay archive is included or linked — supply your own `.apk`, main `.obb`, and patch `.obb`. The VPK contains only the loader-side Vita presentation and trophy resources used by this homebrew build. Unofficial fan project, not affiliated with Mojang, Microsoft, Telltale/LCG, Skybound, or Netflix. Trademarks belong to their owners.

## Requirements

- Homebrew-enabled Vita / PS TV (HENkaku ensō, 3.60 / 3.65) with [VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases).
- [kubridge](https://github.com/TheOfficialFloW/kubridge/releases/tag/v0.1) and [FdFix](https://github.com/TheOfficialFloW/FdFix/releases/tag/v1.0) in `ur0:tai/`, both listed under `*KERNEL`. FdFix raises the open-file limit needed by the game archives. Do not use FdFix alongside rePatch.
- `libshacccg.suprx` in `ur0:data/`; [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D/releases) is the one-click installer.
- **NoTrpDrm is optional for the game and required only for trophies.** The game launches and plays without it. Install [NoTrpDrm](https://github.com/Rinnegatamante/NoTrpDrm/releases/tag/v.1.1) under `*KERNEL` only if you want the bundled trophy set to register and unlock.
- **CapUnlocker is optional.** [CapUnlocker v1.4](https://github.com/GrapheneCt/CapUnlocker/releases/tag/v1.4) exposes the reserved fourth core. Without it, the loader detects the rejection and safely uses the three normal user cores; no manual compatibility file is required.
- Your own legally-owned game: the `com.telltalegames.minecraft100` APK plus its matching main and patch OBBs. The supported set is **PowerVR v1.37** (`40137`).

### Optional performance plugins

Use one overclock menu, not several at the same time. Good options are [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus/releases/tag/v1.4), [PSV-VSH-Menu](https://github.com/joel16/PSV-VSH-Menu/releases/tag/3.40), or [LOLIcon](https://github.com/dots-tb/LOLIcon/releases/tag/1.0.1). CapUnlocker is separate from the clock menu and can be used with one of these. The loader still boots without CapUnlocker and requests only clocks allowed by the current system/plugin setup.

### Trophy setup and repair

MCSM 1.10 packages the trophy set under `MCSM00001_00`, creates the matching runtime context, and performs first-run registration without blocking the game forever. NoTrpDrm is required for trophy support, but it is not required to launch or play the game.

If trophies work normally, do nothing. If an older test VPK left a broken registration and Vita reports an NP preparation/corruption error:

1. Install [Trophy Manager v1.03](https://github.com/ONElua/TrophyManager/releases/tag/1.03).
2. Disable Wi-Fi, open Trophy Manager, select the Minecraft: Story Mode set, and delete that set.
3. Open the Vita's normal Trophies application once so its database refreshes, then reboot.
4. Reinstall the latest MCSM VPK, confirm NoTrpDrm is enabled, and launch MCSM again.

Do not manually delete random `ur0:user/.../trophy` files: that can leave the database row behind while removing only part of the registered set.

## Easy data setup

The Windows [MCSM Vita Data Builder](data-builder/README.md) turns user-owned Android files into the exact ready-to-copy Vita folder. It does not download or contain game data.

1. Open `MCSM-Vita-Data-Builder.exe`.
2. Press **Scan folder** and choose the folder containing your Android files. The builder finds the exact PowerVR v1.37 APK, both matching base OBBs, and recognizable Episodes 2–8 automatically. You can still browse for each file manually. Version 1.8 uses both OBBs only as PC-side extraction inputs, keeps their boot-critical NCTT contents in `mcsm/assets`, and removes the temporary OBB copies before finalizing the Vita folder.
3. Keep **Balanced — recommended**, choose another preset, press **View profiles** for a quick comparison, or press **Make custom** for an Easy/Advanced profile editor.
4. Optionally add Episode 2–8 folders, chapter OBBs, ZIPs, or full nested chapter bundles.
5. Press **Build Data Folder**, then copy the resulting `mcsm` folder to `ux0:data/` with VitaShell.

The final required path is `ux0:data/mcsm/assets`. The compact builder keeps version, renderer, input, bundled-fix, profile, and readiness information visible without exposing unnecessary setup controls. It reads `AndroidManifest.xml`, verifies the exact known PowerVR fingerprint, and accepts only the supported v1.37 APK (`versionCode 40137`); renaming an older or Mali/Adreno APK does not bypass the check. Both fingerprint-matched PowerVR OBBs are required only to produce the extracted base assets on the PC; no `.obb` is copied to the finished Vita folder. Extra chapter `.ttarch2` and descriptor `.lua` files are detected, validated, and put in the correct shared assets folder.

To build the self-contained Windows EXE from source:

```powershell
.\data-builder\build.ps1
```

The app also creates `settings/graphics.txt` and `settings/game.txt`. **Balanced** is the recommended starting profile. The profile guide shows every preset's resolution, FPS cap, reported PowerVR GPU, detail, distance, and main tradeoff. The built-in Custom Profile maker exposes six simple Easy choices or exact Advanced resolution, uncapped/60/30/20/15 FPS, PowerVR GPU name, effects, animation rate, detail, distance, clock, filtering, and compatibility fixes. The generated text remains editable afterward. A demanding frame can still miss its target, but the presenter no longer converts brief misses into a persistent 20 FPS lock. Graphics have one authority: `ux0:data/mcsm/settings/graphics.txt`.

The **Fix & mods** panel can install a supplied controller-button asset fix and merge extra folders/ZIPs into the generated data directory. Locally distributed builds can also include the supported offline `choice.prop` dataset required by the crowd-choice statistics screen. General mod installation is experimental and has not been tested on Vita; add-ons are applied last, while the canonical OBBs and native runtime libraries remain protected.

## Language, saves, and choices

- The builder's language selector writes `settings/game.txt`. The loader forces both the initial system-language query and every later engine language-set call, so an old saved English preference cannot override Russian or another selected language. Voices remain English.
- Personal story decisions and the “seen choice screen” state are stored with the local save bundles under `ux0:data/mcsm/Temp`. The `<User>`/bare-name redirects cover create, load, save, property, bundle, event-log, and both one-slash/two-slash choice paths.
- Crowd percentages are offline data, not a network login. The builder installs the supported `choice.prop` at both required locations and verifies its SHA-256. The loader will no longer create an empty fake dataset: if valid crowd data is missing, statistics remain unavailable instead of opening a blank results screen.
- Preferences such as font size are synchronized between the engine's native root resource and Lua's `<Temp>` resource after every `SavePrefs`, so both paths reload the same settings.

Back up the entire `ux0:data/mcsm/Temp` directory before replacing data or testing save changes.

## Known limitations

- Crowded scenes can still produce individual ~20 FPS frame intervals when CPU-heavy, but the configured 30/60 FPS target is retried immediately instead of being auto-downgraded.
- The first boot can remain black for roughly 30 seconds while caches are created.
- Save-slot titles cannot currently be renamed.
- Experimental data add-ons/mods have not been validated on Vita hardware.
- Choice persistence, crowd results, language switching, and the newest timing/stall fixes need broader real-hardware coverage across all eight episodes.

## Building

The Vita loader requires the **softfp** VitaSDK with vitaGL, vitaShaRK, mathneon, and kubridge. The build script rejects a hard-float SDK instead of producing an ABI-unsafe package. OpenSL ES is not required; audio uses the loader's native FMOD-to-`sceAudioOut` output path.

```powershell
$env:VITASDK = "C:\path\to\vitasdk-softfp"
.\build_vpk.ps1
```

The production package is written to `build_local/MCSM-1.10.vpk` with telemetry disabled by default. A source build includes trophies when `extras/trophy/TROPHY.TRP` is present and cleanly compiles trophy initialization out when it is absent. Build the Windows data preparer with `data-builder/build.ps1`.

VPKs and extracted game data are intentionally excluded from this source repository.

## Credits

- **Andy "TheFloW" Nguyen** — `.so` loader, so_util, fios, libc_bridge, kubridge.
- **Rinnegatamante** — vitaGL, vitaShaRK, mathneon.
- **Volodymyr Atamanenko** — soloader-boilerplate, FalsoJNI.
- **bythos14** — kubridge fork. **Brad Conte** — SHA-1. miniz authors. VitaSDK team.
- **Telltale / Mojang / Microsoft** — game and IP (not affiliated).

Full attributions: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Development note

This whole thing is vibecoded slop made with **Claude 5** and **ChatGPT 5.6 sol**.

## License

Copyright © 2026 LeZergan.

This project is licensed under the [GNU General Public License v3.0](LICENSE).
Inherited and third-party components retain their original notices; see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) and the `LICENSES` directory.
