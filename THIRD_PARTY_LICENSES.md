# Third-Party Licenses

The project's original code and combined distribution are licensed under GPL-3.0
(see [LICENSE](LICENSE)). Inherited loader portions and third-party components keep
their original notices. The prior loader-level MIT notice is preserved at
`LICENSES/Loader-MIT.txt`; component texts also remain in their source trees and the
`LICENSES` directory.

## Bundled in this repo

| Component | Path | License | Copyright |
|---|---|---|---|
| FalsoJNI | `lib/falso_jni/` | MIT (+ Apache-2.0 parts) | V. Atamanenko; A. Nguyen; Rinnegatamante |
| so_util | `lib/so_util/so_util.*` | MIT | Andy Nguyen |
| elf.h | `lib/so_util/elf.h` | LGPL-2.1-or-later | Free Software Foundation |
| fios | `lib/fios/` | MIT | Andy Nguyen |
| sha1 | `lib/sha1/` | Public domain | Brad Conte |
| miniz | `third_party/miniz/` | MIT | Rich Geldreich; RAD Game Tools; Valve |
| kubridge (stub + header) | `lib/kubridge/` | none stated | bythos14; Andy Nguyen |
| libc_bridge | `lib/libc_bridge/` | MIT | Andy Nguyen |
| ttarchext | `data-builder/ThirdParty/ttarchext/` | GPL-2.0-or-later | Luigi Auriemma |

Notes:
- **FalsoJNI** is mostly MIT; two bundled bits are Apache-2.0 (Android/Dalvik JNI
  parts; `converter.c` by Jonathan Bennett). See `lib/falso_jni/LICENSE` + its README;
  full Apache-2.0 text is shipped at `LICENSES/Apache-2.0.txt`.
- **elf.h** is glibc's, LGPL-2.1-or-later. Header kept intact; full license text is
  shipped at `LICENSES/LGPL-2.1.txt`.
- **kubridge** has no upstream license file. Vendored as the build stub + header on
  the de-facto terms used across the Vita homebrew scene; contact the author for terms.
- **miniz** ships its own `LICENSE` under `third_party/miniz/`.
- **ttarchext** is embedded in the Windows data builder to extract the exact
  fingerprint-verified NCTT OBBs selected by the user. Its source headers retain the
  GPL-2.0-or-later notice and the complete corresponding source is shipped beside the
  builder source.

## Linked at build time (not in this repo)

These come from VitaSDK and are **not** redistributed here:

| Component | License | Source |
|---|---|---|
| vitaGL | LGPL-3.0 | <https://github.com/Rinnegatamante/vitaGL> |
| vitaShaRK | LGPL-3.0 | <https://github.com/Rinnegatamante/vitaShaRK> |
| mathneon | zlib/MIT | <https://github.com/Rinnegatamante/math-neon> |
| VitaSDK / vita-toolchain / stubs | MIT (and others) | <https://github.com/vitasdk> |

> If you ever attach a built `.vpk` to a release, it statically links vitaGL and
> vitaShaRK — then add an LGPL-3.0 notice + those source links to the release notes.
> A source-only release (this one) doesn't need that.

## Original game

*Minecraft: Story Mode* © Telltale Games. *Minecraft* is a trademark of Mojang /
Microsoft. No Android game executable, native library, OBB, episode, or gameplay
archive is included in this repository.
