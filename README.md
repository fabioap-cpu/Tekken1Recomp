# Tekken1Recomp

**Static recompilation of *Tekken 1* (USA) for the PlayStation 1.**

Built on [PSXRecomp](https://github.com/mstan/psxrecomp) — a MIPS R3000A → C → native x64 static recompilation framework. The game executable is recompiled to produce a single binary that runs without an emulator, delivering perfect native performance.

> ⚠️ **Engine Enhancement:** This repository features a custom-built CDDA engine patch explicitly engineered to solve the severe CD audio stuttering bugs that naturally occur when recompiling *Tekken 1*.

---

## Requirements

To run a release of Tekken1Recomp, you need your own legally obtained copy of:

- **Tekken 1 (USA)** — disc image (`.cue`/`.bin`)

No retail BIOS image, game disc image, or game assets are included in or distributed by this repository or its releases.

---

## Status

**Playable with Custom Enhancements.** What works today:

- ✅ **Boots and plays** — The game runs natively with full rendering, input, and memory-card saves.
- ✅ **CDDA Audio Bugfix** — Specifically patches the `0x03` (Play) command spam loop that otherwise causes infinite audio stuttering during transitions.
- ✅ **High Resolution** — Configured for HD scaling via `settings.toml`.
- ✅ **Supersampling & Filtering** — 2x Supersampling with nearest-neighbor filtering to preserve sharp pixel aesthetics.
- ⚠️ **Scope:** USA region only. Other regions are untested.

---

## Setup

### 1. Download a Release (recommended)

Grab the release archive from [Releases](https://github.com/fabioap-cpu/Tekken1Recomp/releases), extract it, and run the executable. 

1. **Provide the game disc** — You must provide your own legal copy of the game. Place your `.cue` and `.bin` files inside the extracted Release folder.
2. **Configure the path** — Open the `game.toml` file and set the `[disc]` path to your `.cue` file:
   ```toml
   [disc]
   path = "Tekken 1 (USA).cue"
   ```
3. **Launch** — Run `Tekken1Recomp.exe`.

### 2. The CDDA "Spam" Fix (Technical Details)

During the porting process, a critical issue was discovered where the game would infinitely "stutter" or repeat the first millisecond of music during pause menus or transitions. 

**The Cause:** The original Tekken 1 code constantly spams the `Play` command (`0x03`) to the CD-ROM drive while waiting in specific loops. Real hardware simply ignores the redundant command if the disc is already playing. However, the standard software recompiler obeys it blindly, constantly rewinding the audio track to the start position.

**The Fix:** We injected a protection layer directly into the engine's `cdrom.c` handler (`start_cdda_playback`). By validating if the engine is already playing the exact same track, we absorb the redundant `Play` commands and ensure the music flows without interruption:

```c
static int start_cdda_playback(int requested_track) {
    // [NOVA LIGA DE CÓDIGO INJETADA]
    // Proteção contra SPAM de comandos (ex: Tekken 1).
    // Se o CD já está a tocar a faixa sem redirecionamento explícito,
    // nós absorvemos o comando e protegemos a fluidez da música.
    if (cdda_playing && requested_track == 0 && iso_handle && iso_track_count(iso_handle) > 1) return 1;
    
    // ... continues original logic ...
}
```

### 3. True FPS Engine Reporting

We have also improved how the executable reports its performance metrics (`main.cpp`). Traditional emulators often display a static "60 FPS" based on the monitor's refresh rate (VBLANK), masking the game's actual internal framerate. Our engine explicitly tracks the true rendering rate (`s_game_flip_count`) and displays the real-time internal game FPS on the window title:

```c
            // [NOVA LIGA DE CÓDIGO INJETADA]
            // Leitura Real de FPS.
            // Em vez de apresentar sempre 60 FPS falsos (frequência do monitor/VBLANK),
            // o motor extrai a verdadeira taxa de renderização (game_fps) do próprio jogo.
            const double game_fps = (double)(s_game_flip_count - s_fps_last_flip) / seconds;
```

*You can view both full source modifications in `src/cdrom.c` and `src/main.cpp`.*

---

## Credits
* **Port / Configuration / Engine Fixes:** [fabioap-cpu](https://github.com/fabioap-cpu)
* **Base Recompiler Technology:** [psxrecomp](https://github.com/mstan/psxrecomp) by mstan
