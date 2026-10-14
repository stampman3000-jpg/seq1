# AGENTS.md

Notes for a coding agent working in this repository. Soundboy is a personal instrument. The owner builds hardware first and checks changes by playing. Keep edits small, and leave the playing feel, the save files, and the audio callback alone unless the task is specifically about them.

This is a public repository.

## Repository rules

- Commit as `Stampman3000 <stampman3000@gmail.com>`.
- Leave history as it is. Do not force-push, rebase commits that are already on the remote, or amend a commit that has been pushed.
- Do not commit build output, compiled binaries, or `.DS_Store`. That includes `build/`, `bin/`, `obj/`, object files, executables, libraries, WASM, and editor junk. `.gitignore` already names `build/`, `bin/`, `obj/`, `.vscode/`, and `.DS_Store`. A file that is already tracked stays tracked; the ignore file does not remove it. `src/.DS_Store` is one of those already-tracked files. Leave it unless the owner asks for a cleanup.
- Do not add a home address, a personal email address, or private files (keys, photos, notes, machine-local paths that identify a home). Third-party copyright lines that already ship inside vendored libraries stay as they are.
- Do not add `.cursor` rules or skills into this repository.

## What the program is

One C++17 executable, `seq1`. It is an eight-track groovebox: sequencer, FM synth, sampler and granular player, per-track tape effect, master effects, and a 256×64 user interface. On the Raspberry Pi that interface is also pushed to an SSD1322 OLED. On a Mac the OLED driver is a no-op and the same picture is shown in a raylib window at 2× scale.

The real build is CMake. See [Build and test](#build-and-test).

## Map of the files

```text
CMakeLists.txt              the build that works
miniaudio.h                 vendored audio backend (large; do not reformat)
docs/BUILD.md               dependencies, Pi hardware, troubleshooting
src/main.cpp                window loop, keys, navigation, menus
src/Common.hpp              Track, Step, Pattern, screens, colours, filter
src/Globals.cpp / .hpp      shared state, startup, save and load
src/Audio_Engine.cpp / .hpp device open, callback, clock, synth voices
src/Sampler_Engine.*        sample, slice, and grain voices
src/Chaos.*                 non-destructive pattern variation
src/Tape_Buffer_FX.*        per-track tape delay
src/Master_FX.*             reverb, delay, glue compressor, chorus
src/Midi_Manager.*          RtMidi input callback
src/RtMidi.cpp / .h         vendored MIDI library
src/OledDriver.*            SSD1322 SPI plus an optional UDP mirror
src/UI_Draw.* UI_Screens.*  drawing
src/UI_Font.cpp             bitmap font
src/boot_animation.h        startup frames (large generated table)
```

These files are leftovers from a raylib starter and are not part of the CMake target. Leave them unless a task is about removing the starter:

- `build-MinGW-W64.bat`, `build-VisualStudio2022.bat`, `build-VisualStudio2026.bat` call `premake5`, which is not in the repo
- `.vscode/tasks.json` and `.vscode/launch.json` also call premake and expect `bin/Debug`
- `include/resource_dir.h`
- `resources/wabbit_alpha.png`
- `src/application.rc`, `src/icon.ico`

## How sound gets out

```text
UI keys / encoder / RtMidi notes
        |
        v
 sequencer clock in the audio callback
        |
        v
 per-track voice (FM synth, sampler/grains, or track-8 USB input)
        |
        v
 per-track tape  (g_trackTapeFX[8], not inside Track)
        |
        v
 track sum
        |
        v
 reverb + delay + compression/saturation + chorus
        |
        v
 limiter, then miniaudio stereo float output
```

Normal playback asks for 44.1 kHz and a 512-frame period, with a 48 kHz fallback. Track 8 USB duplex asks for 48 kHz and a 256-frame period first. Inside the callback, work is done in chunks of at most 64 frames so modulation and effects do not depend on the device period.

There is no separate UI thread and audio thread design beyond that: raylib's main loop draws at 60 fps, and miniaudio calls `ma_audio_callback` on the audio device thread. On Linux two more threads exist: one polls the encoder through `/dev/gpiomem`, and one pushes OLED frames over SPI.

## Threads and who may touch what

| Thread | May do | Must not do |
| --- | --- | --- |
| Main / UI (`main.cpp`) | Read keys, edit `tracks[]` pattern data, draw, call `PauseSeqAudio` around loads | Render voices, take locks the callback needs, do DSP sample by sample |
| Audio callback (`Audio_Engine.cpp`) | Sequence, render voices, run tape and master FX, drain the live-MIDI queues | Allocate, lock, read files, call into raylib, log on the hot path |
| RtMidi callback (`Midi_Manager.cpp`) | Push note and clock events into the fixed queues | Write `tracks[]` or voice state |
| Encoder thread (Linux, `main.cpp`) | Count detents into an atomic | Touch audio or the UI |
| OLED worker (Linux, `OledDriver.cpp`) | Write the packed frame to SPI and UDP | Touch pattern data |

Live notes from the UI go through `TriggerVoiceLive`, `ReleaseVoiceLive`, and `AuditionVoiceLive`. Live notes from RtMidi go through `EnqueueMidiLiveFromRt`. The callback is the only place that mutates voices.

Chaos keeps two copies of each step. `Track::source[]` is the pattern the player wrote. `Track::steps[]` is what the sequencer plays, and chaos may rewrite it. A hand edit has to call `ChaosSyncSource` (or the capture helpers) or the next regeneration will throw the edit away. A chaos macro of zero copies source onto steps exactly.

The tape delay lines live in `g_trackTapeFX[8]`, beside the tracks. They are live audio memory, not pattern data. Do not move them back inside `Track`. A pattern switch used to copy those buffers on the audio thread.

## Save files

Writers in `src/Globals.cpp` emit text files:

| Kind | Folder | Name | Header |
| --- | --- | --- | --- |
| Project | `projects/` | `slot_N_Name.prj` | `SOUNDBOY_PRJ 5` |
| Pattern | `patterns/` | `slot_N_Name.pat` | `SOUNDBOY_PAT 5` |
| Preset | `presets/` | `slot_N_Name.snd` | no magic header; the file just has to be non-empty |
| Sample | `samples/` | `*.wav` | decoded at load, not rewritten |

Loaders still accept pattern and project versions 2 through 5. Saves write a `.tmp` file, check it, then rename it over the slot. Field order in those writers is the on-disk format. Adding a field means a new version and a loader path that still opens the owner's existing songs. `GlobalFX` still uses the names `autoPanTime`, `autoPanFeedback`, and `autoPanWidth` because that is the save order; on screen they are chorus rate, stereo spread, and depth.

Samples are loaded by `LoadSampleIntoAsset` as mono 16-bit at 32 kHz, capped at 16 seconds, then peak-normalized. Paths are relative to the process working directory. Launch from the repo root. `SetupDirectories` swallows directory-creation errors.

USB input is legal only on track 8 (`SanitizeEngineType`).

## Screens and input

`Screen` in `Common.hpp` is the page order. `SCREEN_PLACEHOLDER` is the tape page; the enum name is a leftover.

Drawing goes through a CPU framebuffer (`g_oledCPUPixels`) at 256×64, then a point-filtered raylib texture scaled to 512×128. Use the `UI_BG`, `UI_CHROME`, `UI_LABEL`, `UI_VALUE`, and `UI_ACTIVE` colours. Those five values land on OLED grey levels 0, 3, 7, 11, and 15. Raw raylib colours will not match the panel.

Key repeat timings, encoder acceleration (6 ms debounce, faster spins step by more than one), and the HID keyboard mapping are the feel of the instrument. Change them only when the task is about that feel.

On Linux, Ctrl plus the encoder-alt key plus grave, or holding Tab for three seconds, runs `sudo shutdown -h now`. The encoder thread also runs `sudo pinctrl set 17,22,27 ip pu` at startup.

The OLED driver sends each packed frame by UDP to a hardcoded address and port 5006. That mirror is optional and specific to one development machine. The panel and the audio path do not need it. Do not replace that address with a new personal address unless the owner asks.

## Build and test

There is no unit-test target and no test suite. A successful configure and compile is the check an agent can run. Hearing the instrument, and trying the Pi screen and encoder, is the owner's check. Do not claim the instrument was play-tested from a cloud or desktop-only session.

Mac:

```sh
cmake -S . -B build
cmake --build build --parallel
./build/seq1
```

Raspberry Pi / Linux uses the same commands. The Linux link needs `libasound` (`asound` and `pthread`). Full package lists and the hardware checklist are in [docs/BUILD.md](docs/BUILD.md).

CMake defaults a fresh build directory to `RelWithDebInfo`, and also always passes `-O3 -fno-math-errno -g` on the `seq1` target so an old Debug cache still optimizes the DSP. `-ffast-math` is left off on purpose: it reassociates feedback filters. Linux adds `-march=native`.

Raylib is found on the system first (Homebrew on Apple Silicon is added to the prefix path). If it is missing, CMake fetches raylib 5.0, which needs network access. An already-installed raylib is not forced up to 5.0.

Do not build by running the `.bat` files or the premake tasks in `.vscode/`. They are not wired to `CMakeLists.txt`.

`.clangd` expects a compilation database at `build/`.

## Conventions

- Match the style of the file you are in. `main.cpp` and `Globals.cpp` are long and hold a lot of process-lifetime state. Do not reorganize them as a side effect.
- Parameters that the player edits are mostly integers, often 0..99. DSP converts them at block rate.
- Filter coefficients and LFO phase advance update once per 64-sample chunk.
- `EnableFlushToZero()` on the audio thread is the denormal handling. Do not "fix" that by turning `-ffast-math` on.
- Pattern data must stay copyable and small. Runtime audio state (delay lines, voice envelopes, LFO phase) stays outside the saved structs where it already does.
- Comments that describe a real constraint (save order, why tape is outside `Track`, why the CPU meter is uncapped) are part of the design. Keep them accurate if you change the code next to them.

## Easy things to break

- Editing `ma_audio_callback`, the tick math (`2.5 / tempo` seconds per tick, 6 ticks per step), or the external-clock queue without a musical reason. External clock is toggled from the diagnostics page and drops back to the internal clock after 500 ms of silence.
- Changing `SOUNDBOY_PAT` / `SOUNDBOY_PRJ` layout, or the integer values of `ModDestination`. A comment in `Common.hpp` notes that inserting a destination shifts saved routing integers.
- Retuning key-repeat timers, encoder multipliers, or page-navigation keys while doing something else.
- Allocating or locking inside the audio callback.
- Opening the audio device before master-FX init. `EnsureFxRate` runs after the device reports its sample rate and before `ma_device_start`.
- Assuming the process can see `projects/` when it was started from another directory.
- Copying a Linux binary off the machine that built it (`-march=native`).
- Treating `docs/BUILD.md` as a certified clean-image guide. It describes the current instrument and says it has not been verified from a fresh Pi OS install.
