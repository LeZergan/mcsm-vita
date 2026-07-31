# `ux0:data/mcsm/` — data folder and config reference

No game data ships in this repo — you supply it from your own legally-owned copy.

## Layout

```
ux0:data/mcsm/
  assets/            the game's .ttarch2 archives + _resdesc_*.lua descriptors
  Net/               upstream resource descriptors
  Temp/              SAVES live here, plus choice.prop (see below)
  User/              engine <User> location (the port redirects saves out of it)
  settings/          all tunables below go here
  loader.log         written every launch — TRUNCATED ON EACH BOOT, see note
```

**`Temp/` holds your saves.** `saveslotN.bundle`, `_saveslotN_*.bundle`,
`_saveslotN_id.estore` and its `.epage` files. Installing a new VPK never touches
`ux0:data/mcsm/`, so saves survive reinstalls; deleting this folder loses them.

**`choice.prop`** is the pre-baked crowd-choice data behind the end-of-episode
"% of players chose" screen. The loader mirrors it between the data root and `Temp/`
at boot and logs what it found (`CHOICEDATA:`), so if that screen is blank the log
says whether the file was there.

## Where settings go

Every tunable is looked up with `mcsm_open_setting()`, which tries
`ux0:data/mcsm/settings/<name>` first and falls back to `ux0:data/mcsm/<name>`.
**Prefer `settings/`** — the root also holds game data, so it gets crowded.

Two kinds of file:

- **`graphics.txt` / `game.txt`** — `key = value`, many settings per file. The shipped
  copies document themselves; read those rather than duplicating them here.
- **Everything below** — a bare file whose *existence* is the switch, or whose first
  line is a single number. Contents are ignored unless a value is listed.

## Switches (create the file to activate)

| file | effect |
|---|---|
| `no_core3.txt` | Stop using the 4th CPU core. Only matters with the capUnlocker plugin; without it the 4-core mask is refused anyway and this changes nothing. |
| `no_present_lock.txt` | Drop the present-side frame pacing and let vsync alone clock the display. The sim pacer still runs. Try it if pacing feels worse than plain vsync. |
| `no_gxm_tune.txt` | Restore vitaGL's stock GXM ring sizes instead of the enlarged VDM/vertex/fragment rings. Use if heavy scenes misbehave. |
| `no_render_hooks.txt` | Disable the far-clip and brush-detail hooks, which also stops `draw_distance` and `detail` in graphics.txt from having any effect. |
| `mipmaps.txt` | Build mip chains for power-of-two RGBA textures. Off by default. |
| `nearest_filter.txt` | Force NEAREST filtering on compressed/world textures. Fixes the white seams between blocks, at the cost of sharper aliasing. |
| `fbfetch_zero.txt` | Make the framebuffer-fetch stub return `vec4(0.0)` instead of `vec4(1.0)`. Try it if additively-blended surfaces render pure white. |
| `keep_resident.txt` | Keep Jesse's character sets loaded to remove the ~4–5 s swap freeze. **☠ This forces MALE Jesse regardless of your choice** — which is exactly why it is opt-in. |
| `anim_nonskel.txt` | Contents `0` excludes non-skeleton chores from the animation walk. Default follows the profile's `skinning` setting. |
| `dump_shaders.txt` | Write cooked shader source to `diag/shaders/` for inspection. Diagnostic only. |

## Values (first line is a number)

| file | value | meaning |
|---|---|---|
| `audio_gain.txt` | 50–200 | Master volume percent, default 125. The Vita's hardware volume is already 0 dB, so this is the loader's only gain control. |
| `audio_rate.txt` | e.g. `24000` | Force the FMOD output sample rate. Leave absent unless audio misbehaves. |
| `vram_reserve.txt` | MB | User RAM held back from vitaGL's texture pool, default 48. Lower = bigger texture pool but less headroom for the engine's own allocations. |
| `mipmap_min.txt` | 1–4096 | Smallest texture edge that gets a mip chain (needs `mipmaps.txt`). |
| `downsample_min.txt` | e.g. `1024` | Smallest texture that gets halved on upload. `4096` effectively disables it. |

## Trophies

| file | meaning |
|---|---|
| `trophy_commid.txt` | NP communication id, e.g. `MCSM00002`, **without** the `_00` suffix. Overrides the built-in default. It must agree with the folder the pack is installed into (`ux0:app/<TITLEID>/sce_sys/trophy/<id>_00/TROPHY.TRP`) *and* with the `<npcommid>` inside the pack — all three, or there are simply no trophies. |
| `trophies.txt` | `<achievement name> = <trophy id>`, one per line. Only needed if the automatic id derivation is ever wrong; unmapped names are logged with a ready-to-paste line. |

Trophies also require the **NoTrpDrm** plugin. Without it the trophy context fails to
open, which is logged and otherwise harmless — the game runs normally.

## ☠ `loader.log` is truncated on every launch

Only the most recent run survives. If something goes wrong, **copy the log off the
device before relaunching**, or the evidence is gone. Its first line is the build
stamp (`BUILD=...`); if that line is missing, an older eboot is running.
