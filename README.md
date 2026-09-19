# Neo Geo Pocket (RACE) — Retro-Go SD dynamic core

Standalone [RACE](https://github.com/jshsakura/RACE) port for
[Game & Watch Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd).

Produces `race.bin` → `/cores/race.bin`. ROMs under `/roms/ngp/`
(`.ngp`, `.ngc`, `.ngpc`). Cart ROM stays XIP in flash (up to 4 MiB).

## Memory layout

| Region | Use |
|--------|-----|
| **ITCM** | Hot `.text` only: `tlcs900h`, `graphics`, `cz80`, `race-memory` (~50 KiB) |
| **DTCM** | Framebuffer + colour LUT + DAC ring via `dtc_malloc` (~95 KiB) |
| **RAM_EMU** | Core image, `mainram` (224 KiB), `cpurom` (64 KiB), cold code/BSS |
| **AHB** | Avoided for audio (Blip path off by default) |

ITCM is **code only** — no heap data.

## Build

```bash
make                  # → race.bin
make host             # → race_host (SDL2)
make host HOST_SDL=3
make docker
```

```bash
./race_host /path/to/game.ngc
```

Controls: arrows = D-pad, `Z`/`X` = B/A, Enter/Shift/`A`/`S` = Option.
`F1`/`F2` = save/load state under `./host_saves/`.

Audio 44100 Hz mono, RGB565 160×152. Savestates work; cart-flash `.NGF`
persistence across boots is still stubbed (same as the firmware overlay).
