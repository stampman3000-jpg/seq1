# Soundboy

Soundboy is my working, one-off Raspberry Pi 4 groovebox and embedded-audio
platform. I built it as a personal instrument for exploring sequencing,
synthesis, sampling, DSP and purpose-built hardware rather than as a commercial
product.

[![Watch the demo](https://img.youtube.com/vi/UfSV9runVow/maxresdefault.jpg)](https://www.youtube.com/watch?v=UfSV9runVow)

## At a glance

- **Eight tracks:** each can use the FM synth or sample engine; the sampler also
  supports granular playback.
- **Sequencing:** parameter locks, independent track lengths/polymeter, retrigs
  and trigger conditions.
- **Performance variation:** the Chaos layer changes playback non-destructively,
  leaving the underlying pattern intact.
- **Effects:** tape-style processing per track, then master reverb, delay,
  compression/saturation and chorus/auto-pan.
- **Modulation:** two LFOs per track with routable voice, tape-effect and master
  effect destinations.
- **Storage:** project, pattern and sound-preset saving, plus a 16-slot sample
  pool.
- **Input:** the built-in playing surface and transport controls arrive through
  an RP2040 USB HID keyboard; a Pi GPIO rotary encoder handles focused editing.
  External MIDI note input is also supported through RtMidi.
- **Experimental:** Track 8 can become a stereo USB audio input. Device support
  and stable duplex operation remain hardware-dependent.

External MIDI clock handling exists inside the code, but is not currently
exposed or enabled as a user feature. It should not yet be treated as working
sync support.

## Architecture

Soundboy is a C++17 application built with CMake, raylib, miniaudio and RtMidi.
The audio callback owns sequencing and voice rendering; work is divided into
64-frame DSP chunks so modulation, mixing and effects remain independent of the
audio device's callback size.

```text
Sequencer / live keys / external MIDI notes
                    |
           per-track voice engine
              /             \
         FM synth       sample/granular
              \             /
            per-track tape FX
                    |
                track sum
                    |
 reverb + delay + compression/saturation + chorus/auto-pan
                    |
          miniaudio stereo output
```

Normal playback first requests **44.1 kHz with a 512-frame period**, with a
48 kHz fallback if required by the device. Experimental Track 8 USB duplex mode
first requests **48 kHz with a 256-frame period**, then tries progressively more
compatible fallbacks. The diagnostics view exposes audio CPU load, recent peak
load, callback deadline misses, output level, limiter activity and active
voices.

Sequencer timing and DSP run independently of the 60 fps display loop. The
intent is an instrument that can be iterated visually on macOS, then run
against the Pi's real audio and control hardware without maintaining a separate
UI implementation.

## Hardware and display

The current instrument combines:

- Raspberry Pi 4
- SSD1322 256×64 grayscale OLED
- RP2040-based MX switch keyboard/control surface
- Pi-connected rotary encoder

The UI draws to a CPU-side 256×64 framebuffer and scales the same image for a
raylib desktop preview. On Linux, two 4-bit pixels are packed per byte and sent
to the SSD1322 over SPI mode 0 at **8 MHz**. The interface deliberately uses
five grayscale levels—**0, 3, 7, 11 and 15**—for background, structure, labels,
values and active focus.

The RP2040 enumerates as an ordinary USB keyboard, so this application needs no
custom driver or serial protocol. Its firmware is a separate hardware project
and is not included in this repository.

## Build and project status

macOS is useful for building, UI work and audio development. Raspberry Pi/Linux
adds the physical OLED, GPIO encoder and ALSA paths. The repository currently
assumes it is launched from its own root because projects, patterns, presets and
samples use working-directory-relative paths.

See **[docs/BUILD.md](docs/BUILD.md)** for dependencies, build commands,
hardware/runtime assumptions, verification and current limitations.

Soundboy remains an evolving personal instrument. The core sequencer, sound
engines, effects, display path, hardware controls and save/load workflow are in
use; USB input, deployment reproducibility and external sync still need further
hardening.

## A note on process

I developed the implementation with substantial LLM assistance, especially for
writing and revising C++ and DSP plumbing. I created the instrument concept and
architecture, designed and built the physical hardware, brought up and
debugged the electronics, tested the system by playing it, and made the final
technical and musical decisions. I kept generated work only after hands-on
inspection, measurement and iteration on the real instrument.
