#pragma once
#include <string>
#include <vector>
#include <array>
#include <cmath>        // <--- ADD THIS (Fixes powf and tanf)
#include <cstdint>      // <--- ADD THIS (Fixes uint8_t, uint32_t, int16_t)
#include <algorithm>    // <--- ADD THIS (Fixes std::clamp, std::min, std::max)
#include "raylib.h"
#include "Tape_Buffer_FX.hpp"

// Engine sample rate. Requested as 48000 so a USB class device (Digitone etc)
// can pass into the callback without a 48k→44.1k convert sitting in the
// duplex ring buffer — that convert is what reads as stutter on the Pi.
extern double g_sampleRate;

// --- GLOBAL RESOLUTION & SCALING CONFIGURATION ---
constexpr int OLED_WIDTH = 256;
constexpr int OLED_HEIGHT = 64;
constexpr int SCALE_FACTOR = 2;
constexpr int WINDOW_WIDTH = OLED_WIDTH * SCALE_FACTOR;
constexpr int WINDOW_HEIGHT = OLED_HEIGHT * SCALE_FACTOR;

// --- SEMANTIC OLED PALETTE ---
// The SSD1322 renders 16 grey levels. Each channel value below is chosen so
// that (value >> 4) lands exactly on the intended level, matching the shift
// the display driver uses when packing the frame. Use these rather than raw
// raylib colours so the desktop preview and the panel stay in agreement.
constexpr Color UI_BG     = {   0,   0,   0, 255 }; // level 0  - background
constexpr Color UI_CHROME = {  51,  51,  51, 255 }; // level 3  - dividers, frames, disabled
constexpr Color UI_LABEL  = { 119, 119, 119, 255 }; // level 7  - static parameter labels
constexpr Color UI_VALUE  = { 187, 187, 187, 255 }; // level 11 - live values, curves, waveforms
constexpr Color UI_ACTIVE = { 255, 255, 255, 255 }; // level 15 - selection, playhead, focus

// --- MOTION TIMING (seconds) ---
constexpr float UI_BLINK_PERIOD = 0.5f;  // step cursor / edit caret
constexpr float UI_BREATHE_PERIOD = 3.4f; // slow idle motion on pictograms

// Pictogram animation rates, gathered here so the whole screen can be calmed
// or livened from one place. Each pair is a base rate plus the amount added at
// a full-scale parameter value, in cycles or pixels per second.
constexpr float UI_WAVE_SCROLL_BASE  = 1.0f;  // helix scroll, pixels/sec
constexpr float UI_WAVE_SCROLL_SPAN  = 4.5f;
constexpr float UI_WAVE_SPRAY_RATE   = 2.0f;  // droplet reseeds per second
constexpr float UI_RAIN_FALL_BASE    = 2.0f;  // drop descent, pixels/sec
constexpr float UI_RAIN_FALL_SPAN    = 5.0f;
constexpr float UI_TORNADO_SPIN_BASE = 0.4f;  // funnel rotation, radians/sec
constexpr float UI_TORNADO_SPIN_SPAN = 1.6f;

// --- LAYOUT GRID ---
// Screens are built from a fixed header strip plus a body divided into rows.
// Anything positioned by these tokens rather than a literal will stay aligned
// with the rest of the UI when a row height or padding is retuned.
constexpr int UI_HEADER_H   = 7;  // header text band; the rule sits on this row
constexpr int UI_BODY_TOP   = UI_HEADER_H + 2;
constexpr int UI_PAD        = 2;  // inset from a panel edge to its content
constexpr int UI_GUTTER     = 4;  // horizontal space between sibling cells
constexpr int UI_ROW_H      = 9;  // pitch of a body row (7px highlight + 2px gap)
constexpr int UI_HILITE_H   = 7;  // height of a selection highlight
constexpr int UI_TEXT_INSET = 1;  // text offset inside a highlight

// Resolve a zero-based body row to its text baseline / highlight top.
constexpr int UiRowTextY(int row)   { return UI_BODY_TOP + row * UI_ROW_H + UI_TEXT_INSET; }
constexpr int UiRowHiliteY(int row) { return UI_BODY_TOP + row * UI_ROW_H; }

enum Screen {
    SCREEN_SEQ_1_4,     // 1a: Tracks 1-4 Notes (Page 1)
    SCREEN_SEQ_5_8,     // 1b: Tracks 5-8 Notes (Page 1)
    SCREEN_SYNTH,       // Synth / Sampler Engine (Page 2)
    SCREEN_TRACK_PARAMS,// Filter / LFO / FX Sends Page (Page 3)
    SCREEN_PLACEHOLDER, // Dynamic Per-Track Placeholder Page (Page 4)
    SCREEN_GLOBAL_FX    // Global Master Effects Page (Page 5)
};

// --- CORE ENGINE DEFINITIONS ---
enum EngineType {
    ENGINE_SYNTH,
    ENGINE_SAMPLER,
    ENGINE_USB
};

enum SynthAlgo {
    ALGO_PARALLEL,
    ALGO_CARRIER_MOD
};

enum SamplerAlgo {
    ALGO_SAMPLE,
    ALGO_GRANULAR
};
// --- STATE-VARIABLE FILTER (Andrew Simper / Cytomic SVF via Trapezoidal Integration) ---
struct SvfFilter {
    float g = 0.0f;   // Pre-warped cutoff
    float k = 0.0f;   // Damping (1/Q)
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f; // Denominator solvers
    float s1 = 0.0f, s2 = 0.0f;            // Integrator states
    float bassComp = 1.0f;                 // Clean linear resonance bass compensation gain

    void reset() {
        s1 = 0.0f;
        s2 = 0.0f;
    }

    // Process a single sample and return the chosen filter type (LPF/HPF/BPF) output
        inline float process(float x, int type, float analog = 0.0f) {
            // Apply clean, linear bass compensation to the input signal to prevent bass drop-off
            float input = x * bassComp;

            // Pristine, linear Trapezoidal integration loop solver
            float v3 = input - s2;
            float v1 = a1 * s1 + a2 * v3;
            float v2 = s2 + a2 * s1 + a3 * v3;

            if (analog > 0.0f) {
                        // Soft-clip state variables inside the loop to simulate transistor saturation
                        // Scaling makes the former 100% limit (1.0) achievable at ~75%
                        // At 100%, the limit drops to ~0.77 for much heavier warm saturation
                        float saturationLimit = 1.0f / (analog * 1.15f + 0.15f);
                
                auto saturateInside = [saturationLimit](float val) {
                    float absVal = std::abs(val);
                    if (absVal > saturationLimit) {
                        return (val > 0.0f) ? saturationLimit : -saturationLimit;
                    }
                    float norm = val / saturationLimit;
                    return val - (val * norm * norm) / 3.0f; // Cubic distortion curve
                };

                v1 = saturateInside(v1);
                v2 = saturateInside(v2);
            }

            // State update for next sample
            s1 = 2.0f * v1 - s1;
            s2 = 2.0f * v2 - s2;

            if (type == 0) {
                return v2; // Low-Pass
            } else if (type == 1) {
                return input - k * v1 - v2; // High-Pass
            } else {
                return v1; // Band-Pass
            }
        }

    // Recalculate coefficients once per 64-sample block
    void calculateCoefficients(float cutoffHz, float resonanceNorm, float sampleRate) {
        // Clamps to safe range
        if (cutoffHz < 15.0f) cutoffHz = 15.0f;
        if (cutoffHz > sampleRate * 0.49f) cutoffHz = sampleRate * 0.49f;

        // Q factor scaled from 0.5 to 15.0 (exponential curve for smooth resonance sweeps)
        float Q = 0.5f + (resonanceNorm * resonanceNorm) * 14.5f;
                k = 1.0f / Q;

        // Pre-warp cutoff using tanf
        g = tanf(3.14159265f * cutoffHz / sampleRate);
        
        // Denominator solvers
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;

        // Clean Bass Compensation: Gently scales gain up to +3.0dB as resonance increases
        // to maintain low-frequency fullness without any overdrive or saturation.
        bassComp = 1.0f + resonanceNorm * 0.41f;
    }
};
enum ModDestination {
    DEST_NONE,
    
    // Voice Target Parameters (Local or Cross-Track)
    DEST_CUTOFF,
    DEST_RESONANCE,
    DEST_VOLUME,
    DEST_MORPH1,
    DEST_MORPH2,
    DEST_PITCH,
    DEST_DECAY,
    DEST_VOLUME2,
    
    DEST_SAMP_START, // ADDED: Sample Start target [1]
       DEST_GRAN_SIZE,  // ADDED: Grain Size target [1]
       DEST_GRAN_DENS,  // ADDED: Grain Density target [1]
       DEST_GRAN_SCAT,  // ADDED: Grain Scatter target [1]
    // Inserted after GRAN_SCAT (before TAPE): shifts saved destParam ints for TAPE_*/FX routes by +3.
    DEST_FINE1,     // Synth osc1 fine (±99¢)
    DEST_FINE2,     // Synth osc2 fine2 + sampler fine2 (±99¢)
    DEST_SAMP_POS,  // Sampler/grain POS (alias of DEST_MORPH1 on sampler)

    // Tape Buffer Parameters (Page 4 targets)
    DEST_TAPE_MEM,
    DEST_TAPE_HDS,
    DEST_TAPE_SPR,
    DEST_TAPE_SPD,
    DEST_TAPE_TET,
    DEST_TAPE_DRF,
    DEST_TAPE_DRT,
    DEST_TAPE_FDB,
    DEST_TAPE_FSP,
    DEST_TAPE_FSC,
    DEST_TAPE_FRZ,
    DEST_TAPE_SMR,
    DEST_TAPE_SMS,
    DEST_TAPE_MIX,

    // Master FX Send/Mix Targets (Global)
    DEST_REV_MIX,
    DEST_DEL_MIX,
    DEST_SAT_MIX,
    DEST_PAN_MIX
};

struct ModSlot {
    int destType = 0;   // 0 = None, 1 = Voice, 2 = Global FX
    int destTrack = 0;  // Target Track Index (0 to 7) - only used if destType == 1
    int destParam = 0;  // ModDestination Enum Index
    int depth = 0;      // -99 to +99 (Bipolar modulation depth)
};
struct NoteBinding {
    KeyboardKey key;
    const char* noteName;
};

struct PixelOffset {
    int dx;
    int dy;
};

struct StepParams {
    // Only parameters that can be 'locked' per step are defined here.
    // Initialize to a sentinel value (-1) to indicate "no lock" / "use track default".
    int polyMode = -1; // -1 = no lock, 0 = Mono, 1 = Poly-4
    // Synth Parameters (Page 3 Source - Wave 1)
    int morph = -1;
    int coarse = -1;
    int fine = -1;
    int volume = -1;
    int attack = -1;
    int decay = -1;
    int sustain = -1;
    int release = -1;

    // Synth Parameters (Page 3 Source - Wave 2 / Modulator)
    int morph2 = -1;
    int coarse2 = -1;
    int fine2 = -1;
    int volume2 = -1;
    int attack2 = -1;
    int decay2 = -1;
    int sustain2 = -1;
    int release2 = -1;

    // Noise Generator & FM Feedback Parameters (Page 3 Right Panel)
    int fmFeedback = -1;
    int noiseVolume = -1;
    int noiseAttack = -1;
    int noiseHold = -1;
    int noiseDecay = -1;
    
    // Page 3 Legacy parameters that can be step-locked
    int pitchSweepDepth = -1;
    int pitchSweepTime = -1;
    int bitRed = -1;

    // Sampler Parameters (Page 3)
    int sampleSlot = -1;
    int sampleStart = -1;
    int sampleLength = -1;
    int sampleLoop = -1;
    int sampleTune = -1;
    int loopStart = -1;
    int loopEnd = -1;
    int sliceDivisions = -1;
    int grainSize = -1;
    int grainDensity = -1;
    int grainPosition = -1;
    int grainScatter = -1;
    int algorithm = -1; // Synth Parallel/FM or Sampler Sample/Granular (not filterType)

    // Filter & LFO Parameters (Page 4)
    int filterCutoff = -1;
    int filterResonance = -1;
    int filterType = -1; // 0=LPF, 1=HPF, 2=BPF
    int filterEnvDepth = -1;
    int filterAttack = -1;
    int filterDecay = -1;
    int filterSustain = -1;
    int filterRelease = -1;
    
    // LFO 1 State & Parameters (routing is track-level ModSlots — not per-step)
    int lfo1Wave = -1;
    int lfo1Speed = -1;
    int lfo1Depth = -1;
    int lfo1Trigger = -1;
    int lfo1Sync = -1;

    // LFO 2 State & Parameters
    int lfo2Wave = -1;
    int lfo2Speed = -1;
    int lfo2Depth = -1;
    int lfo2Trigger = -1;
    int lfo2Sync = -1;

    int reverbSend = -1;
    int delaySend = -1;
    int saturationSend = -1;
    int autoPanSend = -1;
    int masterVolume = -1;
    int glideTime = -1;
    
    // Tape Buffer Parameters (Page 4)
    int tapeMemory = -1;
    int tapeHeads = -1;
    int tapeSpread = -1;
    int tapeSpeed = -1;
    int tapeTether = -1;
    int tapeDrift = -1;
    int tapeDriftRate = -1;
    int tapeFeedback = -1;
    int tapeFbSpread = -1;
    int tapeFbSource = -1;
    int tapeFreeze = -1;
    int tapeSmearRate = -1;
    int tapeSmearSize = -1;
    int tapeMix = -1;

    void reset() {
        polyMode = -1;
        morph = -1; coarse = -1; fine = -1; volume = -1; attack = -1; decay = -1; sustain = -1; release = -1;
        morph2 = -1; coarse2 = -1; fine2 = -1; volume2 = -1; attack2 = -1; decay2 = -1; sustain2 = -1; release2 = -1;
        fmFeedback = -1; noiseVolume = -1; noiseAttack = -1; noiseHold = -1; noiseDecay = -1;
        pitchSweepDepth = -1; pitchSweepTime = -1; bitRed = -1;
        sampleSlot = -1; sampleStart = -1; sampleLength = -1; sampleLoop = -1; sampleTune = -1; loopStart = -1; loopEnd = -1; sliceDivisions = -1; grainSize = -1; grainDensity = -1; grainPosition = -1; grainScatter = -1;
        algorithm = -1;
        filterCutoff = -1; filterResonance = -1; filterType = -1; filterEnvDepth = -1;
        filterAttack = -1; filterDecay = -1; filterSustain = -1; filterRelease = -1;
        lfo1Wave = -1; lfo1Speed = -1; lfo1Depth = -1; lfo1Trigger = -1; lfo1Sync = -1;
        lfo2Wave = -1; lfo2Speed = -1; lfo2Depth = -1; lfo2Trigger = -1; lfo2Sync = -1;
        reverbSend = -1; delaySend = -1; saturationSend = -1; autoPanSend = -1; masterVolume = -1;  glideTime = -1;
        tapeMemory = -1; tapeHeads = -1; tapeSpread = -1; tapeSpeed = -1; tapeTether = -1;
        tapeDrift = -1; tapeDriftRate = -1; tapeFeedback = -1; tapeFbSpread = -1; tapeFbSource = -1;
        tapeFreeze = -1; tapeSmearRate = -1; tapeSmearSize = -1; tapeMix = -1;
    }
};

struct Step {
    int8_t note = -1;           // MIDI note number (0 to 127). -1 represents empty/inactive step.
    int velocity = 0;           // 0 to 3
    int retrigger = 0;          // retrigger count (0 to 16)
    int microtiming = 0;        // Microtiming offset (-6 to +6 ticks)
    StepParams params;          // Per-step parameter locks

    // --- Step Properties Popup fields ---
    uint16_t condMask = 0xFFFF; // Custom loop bars condition bitmask (plays on all loops by default)
    int noteLength = 0;         // Gate hold duration (0 = AUTO, 1 to 16 steps)
    int chordType = 0;          // Chord formula (0 = NONE, 1 = MAJOR, etc.)
    int8_t chordNotes[3] = {-1, -1, -1}; // Recorded custom chord notes (MIDI numbers, -1 for empty)
};

// The pattern as programmed, before the generative layer rewrites it. Only the
// four fields the generator touches are mirrored, so the whole bank costs 1 KB.
struct StepSource {
    int8_t note = -1;
    int8_t velocity = 0;
    int8_t retrigger = 0;
    int8_t microtiming = 0;
};

// --- SEQUENCER TRACK STRUCT ---
struct Track {
    std::string name;
    Step steps[32];
    StepSource source[32]; // the "peace" pattern; steps[] is the rendered output

    int stepLength = 16; // <--- ADD THIS LINE (Polymeter active step limit)
       int localTick = -1;   // <--- ADD THIS LINE (Track-local clock accumulator)
    
    // Core Engine Types & Algorithm Mappings
    EngineType engineType = ENGINE_SYNTH;
    int algorithm = 0;                    // Cast to SynthAlgo or SamplerAlgo depending on engineType
    int polyMode = 1; // Default to Polyphonic (1)
    int swing = 0;    // 0..99; delays odd steps by up to 5 ticks
    int keyRoot = 0;  // 0=C .. 11=B (major when keyLock == 1)
    int keyScope = 0; // 0=GLOBAL, 1=TRACK
    int keyLock = 0;  // 0=CHR (chromatic), 1=MAJ
    // Synth Parameters (Page 3 Source - Wave 1)
    int morph = 0;
    int coarse = 0;
    int fine = 0;
    int volume = 99;
    int attack = 10;
    int decay = 99;
    int sustain = 75;
    int release = 20;

    // Synth Parameters (Page 3 Source - Wave 2 / Modulator)
    int morph2 = 50;
    int coarse2 = 12; // Serves as RATIO (1..16) in FM algorithm
    int fine2 = 15;
    int volume2 = 70; // Serves as INDEX/MOD DEPTH in FM algorithm
    int attack2 = 20;
    int decay2 = 99;
    int sustain2 = 50;
    int release2 = 30;

    // Noise Generator & FM Feedback Parameters (Page 3 Right Panel)
    int fmFeedback = 20;
    int noiseVolume = 40;
    int noiseAttack = 10;
    int noiseHold = 30;
    int noiseDecay = 40;
    
    // Legacy Modulation/FX parameters (Needed for initialization and Page 3)
    int pitchSweepDepth = 0;
    int pitchSweepTime = 0;
    int reverbSend = 0;
    int delaySend = 0;
    int sampleRateRed = 0;
    int bitRed = 0;

    // --- SAMPLER ENGINE PARAMETERS ---
    int sampleSlot = 0;        // SL (0..15) -> Matches g_samplePool[sampleSlot]
    int sampleStart = 0;       // SS (0..99)
    int sampleLength = 99;     // SE (stored as length, drawn as Start + Length)
    int sampleLoop = 0;        // Loop Mode (0 = 1S, 1 = LP)
    int sampleTune = 0;        // Pitch Tune (CRS, -24..24)
    int loopStart = 20;        // LS (0..99)
    int loopEnd = 80;          // LE (0..99)
    int sliceDivisions = 8;    // SL.DIV (2, 4, 8, 16, 32, 64)

    // Granular parameters
    int grainSize = 20;        // SIZE (0..99)
    int grainDensity = 40;     // DENS (0..99)
    int grainPosition = 0;     // POS (0..99)
    int grainScatter = 10;     // SCAT (0..99)

    // --- NEW FILTER & FX SEND PARAMETERS (Page 4) ---
    int filterCutoff = 80;     // CUT (0..99)
    int filterResonance = 20;  // RES (0..99)
    int filterType = 0;         // TYPE (0 = LPF, 1 = HPF, 2 = BPF)
    int filterEnvDepth = 30;    // DPT (0..99)

    // Filter Envelope ADSR
    int filterAttack = 15;
    int filterDecay = 35;
    int filterSustain = 60;
    int filterRelease = 25;

    // LFO 1 State & Parameters
    int lfo1Wave = 0;           // WAV (0 = Sine, 1 = Tri, 2 = Saw, 3 = Sqr, 4 = S&H)
    int lfo1Speed = 25;         // SPD (0..99)
    int lfo1Depth = 40;         // DPT (0..99)
    int lfo1Trigger = 0;        // TRG (0 = Off, 1 = On)
    int lfo1Sync = 0;           // SYN (0 = Off, 1 = On)
    int lfo1Dest = 0;           // Kept temporarily for Step-Lock backwards compatibility
    ModSlot lfo1Slots[3];       // 3 independent output modulation routing slots

    // LFO 2 State & Parameters
    int lfo2Wave = 1;
    int lfo2Speed = 30;
    int lfo2Depth = 20;
    int lfo2Trigger = 0;
    int lfo2Sync = 1;
    int lfo2Dest = 3;           // Kept temporarily for Step-Lock backwards compatibility
    ModSlot lfo2Slots[3];       // 3 independent output modulation routing slots

    // Real-Time LFO Phase & State Tracking variables
    float lfo1Phase = 0.0f;
    float lfo2Phase = 0.0f;
    float lfo1LastVal = 0.0f;   // Used for Sample-and-Hold random steps
    float lfo2LastVal = 0.0f;   // Used for Sample-and-Hold random steps

    int saturationSend = 0;     // Saturation/Compression Send (Sun Icon)
    int autoPanSend = 0;        // Chorus/Auto-Pan Send (Tornado Icon)
    int masterVolume = 99;
    int glideTime = 0;
    
    // Tape Buffer Parameter Defaults (Page 4)
        int tapeMemory = 50;       // ~1000ms delay length
        int tapeHeads = 1;        // 1 active playback head
        int tapeSpread = 0;       // Spacing zero
        int tapeSpeed = 74;       // ~1.0x playback speed (maps -2 to 2)
        int tapeTether = 99;      // Locked classic delay mode (1.0)
        int tapeDrift = 10;       // Low random drift depth
        int tapeDriftRate = 20;   // Slow wander speed
        int tapeFeedback = 30;    // 30% feedback gain
        int tapeFbSpread = 10;
        int tapeFbSource = 0;     // Self-reinforcing head mix
        int tapeFreeze = 0;       // Record open (no freeze)
        int tapeSmearRate = 0;    // Granular jumps off
        int tapeSmearSize = 40;
        int tapeMix = 0;          // Bypassed (mix at 0) on startup

        // The tape DSP state itself lives in g_trackTapeFX, not here. It is a
        // live delay line, not pattern data, and it carries a 96000-float
        // buffer: holding it inside Track meant every pattern slot stored its
        // own unused copy, and a pattern switch memcpy'd megabytes of audio
        // history on the audio thread.

    bool muted = false;         // Shift+M Track Mute State
};

// Sampler slice mode: DIV > 1 on the sample engine. Keys pick slices, not pitch.
inline bool TrackIsSliceMode(const Track& trk) {
    return trk.engineType == ENGINE_SAMPLER && trk.algorithm == ALGO_SAMPLE && trk.sliceDivisions > 1;
}

// USB is track 8 only. Old projects and presets cannot put it on 1–7.
inline EngineType SanitizeEngineType(int engineVal, int trackIdx) {
    if (engineVal == (int)ENGINE_USB)
        return (trackIdx == 7) ? ENGINE_USB : ENGINE_SYNTH;
    if (engineVal == (int)ENGINE_SAMPLER)
        return ENGINE_SAMPLER;
    return ENGINE_SYNTH;
}

// C4 (MIDI 60) is slice 0; chromatic notes wrap forever around the kit.
inline int SliceIndexFromMidi(int midiNote, int sdiv) {
    if (sdiv < 2) return 0;
    int n = midiNote - 60;
    int m = n % sdiv;
    if (m < 0) m += sdiv;
    return m;
}

// --- SEQUENCER PATTERN STRUCT ---
struct Pattern {
    Track tracks[8];
};

// --- GLOBAL MASTER BUS FX STATE ---
struct GlobalFX {
    int reverbDecay = 50;
    int reverbSize = 50;
    int reverbPredelay = 20;
    int reverbMix = 99; // Default mix to maximum

    int satLevel = 10;
    int satSymmetry = 20;
    int satOverdrive = 5;
    int satMix = 99; // Default mix to maximum

    int delayTime = 30;
    int delayFeedback = 45;
    int delayPingPong = 0; // Toggle (0 or 1)
    int delayMix = 99; // Default mix to maximum

    // Column 4: Chorus/Auto-Pan (Tornado)
    int autoPanTime = 25;
    int autoPanFeedback = 20;
    int autoPanWidth = 40;
    int autoPanMix = 99; // Default mix to maximum
};

struct SampleAsset {
    std::string name = "Empty";
    std::vector<int16_t> pcmData; // 32000Hz 14-bit
    std::array<uint8_t, 97> visualPeaks{}; // Cached peak heights (0 to 11 pixels)
};

// --- UNIFIED UI STATE (Used by high-level pages) ---
struct UIState {
    Screen currentScreen;
    int selectedTrack;
    int cursorTrack;
    int cursorStep;
    int currentOctave;
    double tempo;
    bool isPlaying;
    int playhead;
    int synthGridRow;
    int synthGridCol;
    int trackParamsGridCol;
    bool blinkOn;
    const std::string& activeNotesString;
    
    bool systemMenuOpen;
    int systemMenuCursor;
    const char* menuFeedback; // Toast feedback message (e.g. "PROJECT SAVED!")

    // --- RESTORED FILE SYSTEM & TYPING STATES ---
    int systemMenuState;       // 0=Main, 1=Browser (Load), 2=Typing (Save)
    int fileBrowserCursor;     // Index of selected file in directory
    const char* typingBuffer;  // Name of file currently being typed
    int typingCursor;          // Active cursor character index
};
