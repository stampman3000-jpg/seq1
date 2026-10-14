# Soundboy

Soundboy is a one-off groovebox: an eight-track sequencer and sampler I use as a personal instrument. The program is C++. On the desk it runs on a Raspberry Pi 4, with a small grayscale screen and a keypad. The same program opens in a window on a Mac, which is where most of the day-to-day building happens.

[![Watch the demo](https://img.youtube.com/vi/UfSV9runVow/maxresdefault.jpg)](https://www.youtube.com/watch?v=UfSV9runVow)

## What it does

- **Eight tracks.** Each track is an FM synth or a sampler. The sampler can play a sample straight, as slices, or as grains.
- **A step sequencer.** Patterns are 32 steps, shown as two pages of 16. Tracks can have their own lengths, so they can drift against each other. Steps can lock parameters, retrigger, and follow trigger conditions.
- **Chaos.** A separate layer can lift notes, fill gaps, skip hits, add retrigs, and nudge timing. Your original pattern stays underneath. Turn the chaos knob back to zero and the pattern you wrote is still there.
- **Tape, then the master.** Each track has a tape-style delay and texture effect. After the tracks mix, a master chain adds reverb, delay, compression with saturation, and a stereo chorus.
- **Modulation.** Two LFOs per track can move voice settings, tape settings, or master mixes.
- **Saving.** Whole projects, single patterns, and sound presets go to disk. There is a 16-slot sample pool. Drop WAV files in `samples/` and load them from the system menu. Imports are read as mono and kept to 16 seconds.
- **How you play it.** The Pi keypad is an RP2040 board that shows up as a normal USB keyboard, so the program has no custom driver. A rotary encoder on the Pi GPIO is the focused edit knob. A computer keyboard does the same jobs on a Mac. External MIDI notes arrive through RtMidi: channels 1–8 land on tracks 1–8, and channel 16 follows whichever track is selected.
- **Track 8 USB input.** Track 8 can switch to a stereo USB audio input. That path is experimental and depends on the interface and the driver.

External MIDI clock can be turned on from the diagnostics page. Treat it as experimental. If the clock goes quiet for more than half a second while the transport is running, playback falls back to the internal clock.

## A short tour of the keys

The RP2040 firmware lives in a different project. On a Mac, a normal keyboard is enough, and Command does the same job as Ctrl:

| Keys | What they do |
| --- | --- |
| Space or Enter | Start and stop, when a menu is closed |
| A S D F G H J K | Piano keys, C through the C above. Comma and period change octave |
| Shift + A S D F G H J K | Select tracks 1–8 |
| Ctrl + left / right | Move between the sequencer, the engine, filter and LFOs, tape, and the master effects |
| Ctrl + A S D F G H J K | Queue one of the eight patterns |
| M | Mute the selected track |
| Tab | A short press opens the system menu. On the Pi, holding Tab for three seconds shuts the machine down |
| Ctrl + Shift + 9 | Diagnostics: audio load, missed deadlines, level, limiter, voices. Up or C switches the clock |

## Build and run

You need a C++17 compiler and CMake 3.15 or newer. Raylib is used for the window. CMake uses a raylib already installed on the machine, and otherwise downloads raylib 5.0. The audio library (miniaudio) and MIDI library (RtMidi) are already in this repository.

Always start the program from the repository root. Projects, patterns, presets, and samples are saved in the folder you launched from. The first launch tries to create `projects/`, `patterns/`, `presets/`, and `samples/` there.

Hardware wiring, permissions, audio device fallbacks, and a longer troubleshooting list are in [docs/BUILD.md](docs/BUILD.md).

### Mac

```sh
xcode-select --install
brew install cmake raylib
cmake -S . -B build
cmake --build build --parallel
./build/seq1
```

The Mac build uses CoreAudio and CoreMIDI. The physical screen driver does nothing there. You see the same 256×64 picture, scaled up, in the window.

### Raspberry Pi and other Linux

On Debian or Raspberry Pi OS this set is a reasonable start. Package names can differ by release. If CMake asks for another X11 or OpenGL package, install that one and configure again.

```sh
sudo apt update
sudo apt install build-essential cmake git libasound2-dev \
  libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
  libgl1-mesa-dev libglu1-mesa-dev
cmake -S . -B build
cmake --build build --parallel
./build/seq1
```

Linux audio goes through ALSA. The Linux compile also turns on `-march=native`, so build on the Pi itself (or on a machine with the same kind of CPU). A binary built on a desktop PC will not be the right one to copy over.

On the Pi the program also talks to the SSD1322 screen over SPI and reads the encoder from GPIO. Those details, including the permissions the process needs, are in [docs/BUILD.md](docs/BUILD.md).

### Windows

Windows is not a working build of this instrument. The `.bat` files in the root still call `premake5`, and that tool is not part of this repository. The CMake setup that actually builds Soundboy has no Windows audio or MIDI backend. Use a Mac or the Pi.

## Where the project stands

The sequencer, both sound engines, the effects, the screen path, the hardware controls, and save/load are in regular use. USB audio input, a fully repeatable Pi install, and external clock sync still need more hardening. There is no frozen Pi operating-system image yet.

If you are changing the code, read [AGENTS.md](AGENTS.md) first. It maps the files, the build, and the parts that are easy to break.

## A note on process

I developed the implementation with substantial LLM assistance, especially for writing and revising C++ and DSP plumbing. I created the instrument concept and architecture, designed and built the physical hardware, brought up and debugged the electronics, tested the system by playing it, and made the final technical and musical decisions. I kept generated work only after hands-on inspection, measurement, and iteration on the real instrument.
