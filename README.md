# Minecraft: Story Mode — PS Vita (unofficial loader)

Runs the Android build of *Minecraft: Story Mode* on the PS Vita. The package contains the Vita loader, LiveArea presentation, and trophy resources — **no APK, OBBs, episodes, or Android game binaries**. Bring your own legally-owned copy. Built on [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate), rendered with [vitaGL](https://github.com/Rinnegatamante/vitaGL).

> **Public testing release.** Expect rough edges and keep backups of your saves.

## Download

The [latest GitHub release](../../releases/latest) contains both files needed for setup:

- **`MCSM-1.10.vpk`** — complete loader with LiveArea artwork and trophy support; install with VitaShell.
- **`MCSM-Vita-Data-Builder-v1.6.exe`** — creates the required `ux0:data/mcsm` folder from your legally owned Android files.

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

- Homebrew-enabled Vita / PS TV (HENkaku ensō, 3.60 / 3.65) with **VitaShell**.
- `kubridge.skprx` and `fd_fix.skprx` in `ur0:tai/` (both listed under `*KERNEL` in `config.txt`), and `libshacccg.suprx` in `ur0:data/`. `fd_fix.skprx` raises the open-file limit the loader needs to stream the game archives — without it, archive reads thrash on `EMFILE`. (Don't use it alongside the rePatch plugin.)
- **NoTrpDrm** installed and enabled. The bundled homebrew trophy set cannot register without it.
- Your own legally-owned game: the `com.telltalegames.minecraft100` APK plus its matching main and patch OBBs. The supported set is **PowerVR v1.37** (`40137`).

## Easy data setup

The Windows [MCSM Vita Data Builder](data-builder/README.md) turns user-owned Android files into the exact ready-to-copy Vita folder. It does not download or contain game data.

1. Open `MCSM-Vita-Data-Builder.exe`.
2. Press **Scan folder** and choose the folder containing your Android files. The builder finds the exact PowerVR v1.37 APK, both matching base OBBs, and recognizable Episodes 2–8 automatically. You can still browse for each file manually.
3. Keep **Balanced — recommended**, choose another preset, press **View profiles** for a quick comparison, or press **Make custom** for an Easy/Advanced profile editor.
4. Optionally add Episode 2–8 folders, chapter OBBs, ZIPs, or full nested chapter bundles.
5. Press **Build Data Folder**, then copy the resulting `mcsm` folder to `ux0:data/` with VitaShell.

The final required path is `ux0:data/mcsm/assets`. The compact builder keeps version, renderer, OBB, bundled-fix, profile, and readiness information visible without exposing unnecessary setup controls. It reads `AndroidManifest.xml`, verifies the exact known PowerVR fingerprint, and accepts only the supported v1.37 APK (`versionCode 40137`); renaming an older or Mali/Adreno APK does not bypass the check. Both fingerprint-matched PowerVR OBBs are required. Extra chapter `.ttarch2` and descriptor `.lua` files are detected, validated, and put in the correct shared assets folder.

To build the self-contained Windows EXE from source:

```powershell
.\data-builder\build.ps1
```

The app also creates `settings/graphics.txt` and `settings/game.txt`. **Balanced** is the recommended starting profile. The profile guide shows every preset's resolution, FPS cap, reported PowerVR GPU, detail, distance, and main tradeoff. The built-in Custom Profile maker exposes six simple Easy choices or exact Advanced resolution, FPS, PowerVR GPU name, effects, detail, distance, clock, filtering, and compatibility fixes. The generated text remains editable afterward. Advanced mode includes a 60 FPS cap, but that is a maximum target rather than a guaranteed lock—crowded scenes with many characters can fall to around 20 FPS.

The **Fix & mods** panel can install a supplied controller-button asset fix and merge extra folders/ZIPs into the generated data directory. Locally distributed builds can also include the supported offline `choice.prop` dataset required by the crowd-choice statistics screen. General mod installation is experimental and has not been tested on Vita; add-ons are applied last, while the canonical OBBs and native runtime libraries remain protected.

## Known limitations

- Crowded scenes can become CPU-heavy and fall to around 20 FPS.
- The first boot can remain black for roughly 30 seconds while caches are created.
- Save-slot titles cannot currently be renamed.
- Experimental data add-ons/mods have not been validated on Vita hardware.
- If a previous trophy test build left an `MCSM00001` registration and Vita reports an NP preparation/data error, remove the old set completely with Trophy Manager before reinstalling. Deleting only its folders can leave a stale database row.

## Building

The Vita loader requires the **softfp** VitaSDK with vitaGL, vitaShaRK, mathneon, OpenSLES, and kubridge. The build script rejects a hard-float SDK instead of producing an ABI-unsafe package.

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

## Acknowledgments

Built with AI assistance from **Codex 5.5**, **Claude 4.8**, and **DeepSeek 4 Pro**.

## License

Copyright © 2026 LeZergan.

This project is licensed under the [GNU General Public License v3.0](LICENSE).
Inherited and third-party components retain their original notices; see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) and the `LICENSES` directory.
