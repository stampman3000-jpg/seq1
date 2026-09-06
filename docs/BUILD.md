# Building and running Soundboy

This guide describes the build and hardware assumptions visible in the current
repository. It has **not** been verified from a clean Raspberry Pi OS image, and
there is not yet a frozen OS image or complete package manifest. Treat it as a
reproducible build guide in progress.

## Dependency baseline

- C++17 compiler
- CMake 3.15 or newer
- raylib: CMake uses an installed package if found; otherwise it fetches raylib
  5.0 from GitHub
- miniaudio v0.11.25: vendored as `miniaudio.h` at the repository root
- RtMidi 6.0.0: vendored as `src/RtMidi.cpp` and `src/RtMidi.h`

The raylib fallback needs network access during the first CMake configure.
Because CMake checks for a system raylib first, an already installed version is
not forced to 5.0.

## Clone

```sh
git clone https://github.com/stampman3000-jpg/seq1.git
cd seq1
```

## macOS

Install Apple's command-line tools if they are not already present:

```sh
xcode-select --install
```

With Homebrew, the simplest dependency setup is:

```sh
brew install cmake raylib
```

If raylib is omitted, CMake can fetch and build raylib 5.0 instead. Configure
and build from the repository root:

```sh
cmake -S . -B build
cmake --build build --parallel
```

Run from the repository root:

```sh
./build/seq1
```

macOS uses CoreAudio and CoreMIDI. The physical OLED driver is a no-op there,
while the raylib window provides the development preview.

## Raspberry Pi / Linux

The Pi build uses ALSA and raylib's Linux desktop dependencies. On Debian or
Raspberry Pi OS, a likely starting set is:

```sh
sudo apt update
sudo apt install build-essential cmake git libasound2-dev \
  libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
  libgl1-mesa-dev libglu1-mesa-dev
```

Distribution package names and the exact raylib/GLFW dependency set may vary by
OS release. Add the corresponding development packages if CMake reports a
missing X11, OpenGL or window-system library.

Configure and build:

```sh
cmake -S . -B build
cmake --build build --parallel
```

Run from the repository root:

```sh
./build/seq1
```

### Pi applications menu / clean boot launch

Direct `./build/seq1` needs a working `DISPLAY` and must be started from the
repo root. Menu entries and some SSH sessions often miss one or both.

Use the wrapper instead:

```sh
chmod +x ~/seq1/scripts/run-seq1.sh
~/seq1/scripts/run-seq1.sh
```

Install a desktop menu entry (paths assume user `soundboy1` and clone at
`~/seq1` — edit `scripts/soundboy.desktop` if yours differ):

```sh
mkdir -p ~/.local/share/applications
cp ~/seq1/scripts/soundboy.desktop ~/.local/share/applications/
update-desktop-database ~/.local/share/applications 2>/dev/null || true
```

Then launch **Soundboy** from the applications menu. The wrapper sets
`DISPLAY=:0` when unset and `cd`s to the repo root before exec.

If `InitWindow` still fails, the app now exits with a log message instead of
segfaulting.

The Linux build applies `-march=native`, so build on the Pi—or on a machine
targeting the same CPU—rather than copying an arbitrary Linux binary.

## Working directory and data

The application currently uses paths relative to its working directory. Run it
from the repository root so it reads and writes the intended folders. On first
run it attempts to create:

```text
seq1/
├── projects/   # complete projects (.prj)
├── patterns/   # individual patterns (.pat)
├── presets/    # saved sounds (.snd)
└── samples/    # imported/source WAV files
```

Directory creation failures are currently swallowed by the application, so
check ownership and write permissions if saving or sample browsing does not
work.

## Audio modes

Normal playback opens a stereo float output device. It first requests:

- 44,100 Hz
- 512-frame period
- three periods

If the device rejects 44.1 kHz, the code falls back to 48 kHz. Internally, the
callback processes audio in chunks of up to 64 frames for voice mixing,
modulation and effects.

Track 8 has an experimental USB input engine. Enabling it reopens miniaudio in
duplex mode and first requests:

- 48,000 Hz
- 256-frame period
- four periods

Linux first tries ALSA compatibility combinations for mmap and automatic
resampling, then can fall back to 44.1 kHz. If duplex opening fails, the
application attempts to remain running in playback-only mode. USB capture is
device- and driver-dependent; do not assume every class-compliant interface
will negotiate cleanly.

When Track 8 is in USB mode, its engine page displays the negotiated sample rate
and period below the capture-device name. The system menu's **MIDI /
DIAGNOSTICS** page shows audio CPU load, recent peak load (`PK`), callback
deadline misses (`XR`), output level, limiter activity and active voice counts.
For a basic validation, play a busy pattern for several minutes and confirm
that audio is clean, peak CPU stays below 100%, and `XR` does not continue to
increase.

## Pi hardware and permissions

The Linux path currently assumes the hardware layout used by the existing
instrument:

- SSD1322 OLED, 256×64, on `/dev/spidev0.0`
- SPI mode 0, 8 bits per word, 8 MHz
- OLED DC uses code pin `512 + 25`
- OLED RESET uses code pin `512 + 24`
- rotary encoder BCM GPIO: CLK 17, switch 22, DT 27
- encoder register access through `/dev/gpiomem`

The `512 + GPIO` OLED values reflect the current sysfs GPIO numbering
assumption. They are code-level identifiers, not a claim about physical header
pin numbers. This repository does not contain enough wiring information to
document OLED power, ground, MOSI or SCLK header pins safely.

SPI must be enabled and the runtime user must have appropriate access to
`/dev/spidev0.0`, `/dev/gpiomem` and the sysfs GPIO controls used for OLED DC
and RESET. Prefer deliberate group, udev or service configuration appropriate
to the installed OS rather than running the whole application as root.

The code also invokes:

```text
sudo pinctrl set 17,22,27 ip pu
sudo shutdown -h now
```

The first configures encoder pull-ups; the second is used by the instrument's
shutdown gestures. A headless installation therefore needs a suitable
`pinctrl` command and narrowly scoped sudo policy, or these operations will
prompt/fail. Review any sudoers change carefully.

### RP2040 control surface

The separate RP2040 controller is expected to enumerate as a standard USB HID
keyboard. Soundboy consumes ordinary key events, so no custom Pi driver is
required. RP2040 firmware and its flashing instructions are outside this
repository.

### OLED UDP mirror

On Linux, the OLED driver also sends each packed frame over non-blocking UDP to
`192.168.0.94:5006`. This is an optional, development-specific screen mirror
with a currently hardcoded destination. The receiver is not required for the
physical OLED or audio engine, and the address will not suit most networks.

## Verification checklist

1. CMake configures without missing compiler, ALSA, X11 or OpenGL dependencies.
2. `cmake --build build --parallel` produces `build/seq1`.
3. Launching from the repo root opens the 256×64 UI preview.
4. `projects/`, `patterns/`, `presets/` and `samples/` exist and are writable.
5. A pattern plays through the expected output without clicks or dropouts.
6. Diagnostics stays below 100% peak CPU and `XR` remains stable.
7. On Pi, the encoder turns and presses reliably and the OLED updates.
8. Save and reload a disposable test pattern/project.
9. If testing Track 8 USB, confirm the intended capture device plus the shown
   sample rate/period before judging stability.

## Focused troubleshooting

- **CMake cannot find raylib dependencies:** install the missing X11/OpenGL
  development package named in the error, remove only the incomplete `build/`
  directory if necessary, then configure again.
- **No audio:** confirm ALSA can see the desired playback device and that no
  other process has opened it exclusively. Track 8 USB may fall back to
  playback-only if capture initialization fails.
- **Audio crackles:** open Diagnostics. Rising `XR` or `PK` near/above 100%
  points to missed callback deadlines; a pinned output meter with limiter
  activity points instead to excessive gain.
- **OLED is blank:** confirm SPI is enabled, `/dev/spidev0.0` exists, runtime
  permissions are correct, and the code's sysfs GPIO numbering matches the
  installed kernel.
- **Encoder is inactive:** check `/dev/gpiomem` access, `pinctrl` availability
  and the BCM 17/22/27 assumptions.
- **Files appear missing:** verify the process was launched from the repository
  root. The browser does not currently resolve data relative to the executable.

## Current limitations

- External MIDI note input works, but external MIDI clock is not exposed or
  enabled as a user feature.
- Projects, patterns, presets and samples use working-directory-relative paths.
- SPI, sysfs GPIO, `/dev/gpiomem` and `pinctrl` paths are Pi/Linux-specific.
- Track 8 USB duplex input is experimental.
- There is no frozen Raspberry Pi OS image or verified package manifest yet.
- This setup has not been certified from a clean Pi image.
