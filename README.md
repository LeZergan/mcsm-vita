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

No *Minecraft: Story Mode* code or assets are included or linked — supply your own `.apk` + `.obb`. Unofficial fan project, not affiliated with Mojang, Microsoft, Telltale/LCG, Skybound, or Netflix. Trademarks belong to their owners.

## Requirements

- Homebrew-enabled Vita / PS TV (HENkaku ensō, 3.60 / 3.65) with **VitaShell**.
- `kubridge.skprx` and `fd_fix.skprx` in `ur0:tai/` (both listed under `*KERNEL` in `config.txt`), and `libshacccg.suprx` in `ur0:data/`. `fd_fix.skprx` raises the open-file limit the loader needs to stream the game archives — without it, archive reads thrash on `EMFILE`. (Don't use it alongside the rePatch plugin.)
- Your own legally-owned game: the `com.telltalegames.minecraft100` APK + its OBB. Target **v1.37** (`40137`).

## Data folder

> ⚠️ **Setup guide coming soon.** You supply your own game data (APK + OBB); none ships in this repo.

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
