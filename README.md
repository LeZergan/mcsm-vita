# Minecraft: Story Mode — PS Vita (unofficial loader)

Runs the Android build of *Minecraft: Story Mode* on the PS Vita. Loader code only — **no game code or assets**; bring your own legally-owned copy. Built on [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate), rendered with [vitaGL](https://github.com/Rinnegatamante/vitaGL).

> **WIP.**

## Videos

Development progress, oldest to newest:

1. [First build — menus](https://youtu.be/q_X8j8XZ-NU)
2. [Gameplay](https://youtu.be/TzjYkUHpF6k)
3. [Full trial chapter — fixed animations + audio](https://youtu.be/BltUYWmdRq8)
4. [Gameplay & chapter presentation](https://youtu.be/QsmPfikGh0A)
5. [Texture fixes](https://youtu.be/BrGuSMM_j5U)
6. [Stutter fix & optimisation — shader caching](https://youtu.be/IK8h5NXLh2k)

## Legal

No *Minecraft: Story Mode* code or assets are included or linked — supply your own `.apk`, main `.obb`, and patch `.obb`. Unofficial fan project, not affiliated with Mojang, Microsoft, Telltale/LCG, Skybound, or Netflix. Trademarks belong to their owners.

## Requirements

- Homebrew-enabled Vita / PS TV (HENkaku ensō, 3.60 / 3.65) with **VitaShell**.
- `kubridge.skprx` and `fd_fix.skprx` in `ur0:tai/` (both listed under `*KERNEL` in `config.txt`), and `libshacccg.suprx` in `ur0:data/`. `fd_fix.skprx` raises the open-file limit the loader needs to stream the game archives — without it, archive reads thrash on `EMFILE`. (Don't use it alongside the rePatch plugin.)
- Your own legally-owned game: the `com.telltalegames.minecraft100` APK + its OBB. Target **v1.37** (`40137`).

## Easy data setup

The Windows [MCSM Vita Data Builder](data-builder/README.md) turns user-owned Android files into the exact ready-to-copy Vita folder. It does not download or contain game data.

1. Open `MCSM-Vita-Data-Builder.exe`.
2. Press **Scan folder** and choose the folder containing your Android files. The builder finds the exact PowerVR v1.37 APK, both matching OBBs, and recognizable Episodes 2–8 automatically. You can still browse for each file manually.
3. Keep **Default — recommended**, choose another preset, or press **Make custom** for an Easy/Advanced profile editor.
4. Optionally add Episode 2–8 folders or chapter ZIPs.
5. Press **Build Data Folder**, then copy the resulting `mcsm` folder to `ux0:data/` with VitaShell.

The final required path is `ux0:data/mcsm/assets`. The compact builder keeps version, renderer, OBB, bundled-fix, profile, and readiness information visible without exposing unnecessary setup controls. It reads `AndroidManifest.xml`, verifies the exact known PowerVR fingerprint, and accepts only the supported v1.37 APK (`versionCode 40137`); renaming an older or Mali/Adreno APK does not bypass the check. Both fingerprint-matched PowerVR OBBs are required. Extra chapter `.ttarch2` and descriptor `.lua` files are detected, validated, and put in the correct shared assets folder.

To build the self-contained Windows EXE from source:

```powershell
.\data-builder\build.ps1
```

The app also creates `settings/graphics.txt` and `settings/game.txt`. **Default** is the recommended starting profile. The built-in Custom Profile maker exposes six simple Easy choices or exact Advanced resolution, FPS, PowerVR GPU name, effects, detail, distance, clock, filtering, and compatibility fixes. The generated text remains editable afterward. Advanced mode includes a 60 FPS cap, but that is a maximum target rather than a guaranteed lock—crowded scenes with many characters can fall to around 20 FPS.

The **Fix & mods** panel can install a supplied controller-button asset fix and merge extra folders/ZIPs into the generated data directory. Locally distributed builds can also include the supported offline `choice.prop` dataset required by the crowd-choice statistics screen. General mod installation is experimental and has not been tested on Vita; add-ons are applied last, while the canonical OBBs and native runtime libraries remain protected.

## Known issues

- Stutters and fluctuating frame rate.
- Your choices don't display in the menu.
- Can't change the save file title.

## Building

softfp VitaSDK with vitaGL, vitaShaRK, mathneon, OpenSLES, kubridge.

```bash
export VITASDK=/path/to/vitasdk-softfp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

VPK lands in `build/`. Windows: `build_vpk.ps1`.

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

MIT (see [LICENSE](LICENSE)). Links vitaGL/vitaShaRK (LGPL-3.0), not redistributed here.
