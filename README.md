# Soundboy

Soundboy is a standalone Raspberry Pi groovebox designed as both a musical instrument and an experimental embedded audio platform.

Built around a Raspberry Pi 4 with an RP2040 keyboard controller, SSD1322 OLED display and custom UI, it combines sequencing, synthesis, sampling and real-time DSP into dedicated hardware.

[![Watch the demo](https://img.youtube.com/vi/UfSV9runVow/maxresdefault.jpg)](https://www.youtube.com/watch?v=UfSV9runVow)

## Features

- 8-track sequencer
- FM synthesis engine
- Sample playback engine
- Modulation matrix
- Performance effects
- OLED interface
- External MIDI clock support
- Custom hardware controls
- Standalone launcher for multiple embedded applications

## Hardware

- Raspberry Pi 4
- RP2040 keyboard controller
- MX Cherry switches
- SSD1322 OLED display
- Rotary encoder

## Software

- C++
- Raylib
- miniaudio
- RtMidi
- CMake

## Audio engine

Audio runs through miniaudio on a stereo, 32-bit float device configured with a
fixed `periodSizeInFrames = 512` (period size in milliseconds is deliberately
left disabled so the period is exact regardless of platform). At 512 frames
that's roughly 11.6 ms of hardware latency, which already feels tight enough to
play live on the Pi 4; internally, the callback breaks that period down into
64-frame chunks for block-rate DSP work (voice mixing, modulation, FX).
512 was chosen as a safe first target rather than a proven floor — 256 or 128
frames are likely possible and are on the list to try once the rest of the
engine is stable enough to make underrun debugging straightforward. The DSP
itself was largely prototyped visually (scope/plot first, code second) before
any of it was implemented in the callback.

## Signal flow

Each of the 8 tracks can source audio from either the FM synth engine or the
sampler, gets its own per-track tape-style effect, and is then summed into a
shared master bus:

```
Sequencer / live MIDI input
            │
     per-track voice trigger
            │
      ┌─────┴─────┐
      │           │
  FM Synth   Sampler_Engine
      │           │
      └─────┬─────┘
            │
   Tape_Buffer_FX (per track: freeze / smear / mix)
            │
      track sum → final mix
            │
   Master_FX bus (reverb, delay, saturation, auto-pan)
            │
  miniaudio device callback → audio out
```

Every track also carries two LFOs whose slots can be routed to destinations
across the engine — including the master bus mix amounts (reverb, delay,
saturation, auto-pan). Those modulation values are summed into the target
parameter for that audio block before the FX are applied, so a track's LFOs
can, for example, sweep the whole mix's delay send rather than just their own
sound. This is the modulation matrix mentioned in Features: a small, shared
routing layer sitting alongside the signal path above rather than a separate
per-effect system.

## Custom keyboard controller (RP2040)

The playing surface (MX Cherry switches, transport and mode buttons) is scanned
entirely by a separate RP2040, which enumerates over USB as a standard USB
keyboard/HID device. From the Pi's point of view there is no custom driver or
serial protocol to maintain — it just sees keypresses. This keeps key scanning
and debounce jitter-free and off the Pi's Linux scheduler entirely, and it
means the electrically messy part of the design (a large switch matrix) stays
on a small, swappable board instead of eating into the Pi's own GPIO.

## Raylib-to-OLED display pipeline

The UI is drawn once into a CPU-side 256×64 framebuffer using Raylib-style drawing
calls redirected to pixel primitives. That same framebuffer then serves two purposes:

1. It is uploaded to a Raylib `RenderTexture2D` and nearest-neighbour scaled for
   the large desktop preview during development.
2. On Raspberry Pi, it is vertically flipped and converted for the SSD1322:
   each source pixel is hard-thresholded to black or full 4-bit white (`0` or
   `15`), two 4-bit pixels are packed into each byte, and the resulting 8,192-byte
   frame is pushed over `/dev/spidev0.0` at 2 MHz.

So the development workflow is “draw at the real OLED resolution, preview large,
ship tiny”: the desktop window is a scaled view of the exact pixel buffer sent to
the OLED. There is no grayscale dithering in the current path; the SSD1322 is
configured for 4-bit pixels, but the UI currently uses only the two endpoint
levels. On Linux the packed frame is also optionally sent via UDP for a Mac
screen mirror; on macOS the physical OLED driver is a no-op. Like the DSP, the
UI layout and readability were worked out visually on the desktop preview
first, then carried over to the real panel and re-tuned by eye against the
hardware (contrast, spacing, flicker) once it was running on actual glass.

## Goals

Soundboy is my platform for exploring new workflows, DSP techniques and embedded instrument design rather than an attempt to emulate existing grooveboxes.

## A note on process

Most of the implementation code in this project — the C++, the DSP plumbing,
the display packing logic — was written with heavy LLM assistance. I don't
want to hide that. But "LLM-assisted" doesn't mean "hands-off": every piece
here went through rounds of hand debugging, hardware bring-up, and refinement
that no model did for me — soldering and wiring the switch matrix and
controller boards, designing and iterating the physical enclosure in CAD,
reading scope traces and SPI captures to chase down real hardware bugs,
deciding what the architecture and signal flow should actually be, and picking
through generated code line by line to work out which approach was worth
keeping. The AI was a very capable pair of hands for typing code; the design
decisions, the debugging, and the physical build are mine.
