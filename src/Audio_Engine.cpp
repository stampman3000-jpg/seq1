#include "Audio_Engine.hpp"
#include "Sampler_Engine.hpp"
#include "Master_FX.hpp"
#include "Common.hpp"
#include "Globals.hpp"
#include "Chaos.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <cstdlib> // std::abs, rand
#include <vector>
#include <string>
#include <cstring>
#include <atomic>
#if defined(__SSE2__) || defined(_M_X64)
#include <pmmintrin.h>
#endif

// Denormals are the numbers that appear when a feedback tail decays past the
// smallest normal float. Nothing here ever fully reaches zero on its own, so
// the reverb, delay and tape buffers all settle into that range and stay
// there, and on some CPUs every operation on one costs orders of magnitude
// more than on a normal float. Flushing them to zero is inaudible.
// The mode is a per-thread register setting, so it has to be armed from inside
// the audio thread rather than at startup.
static inline void EnableFlushToZero() {
    static thread_local bool s_armed = false;
    if (s_armed) return;
    s_armed = true;
#if defined(__SSE2__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040u); // FTZ | DAZ
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ULL << 24))); // FZ
#elif defined(__arm__)
    uint32_t fpscr;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24))); // FZ
#endif
}

#define MINIAUDIO_IMPLEMENTATION
#include "../miniaudio.h"

// Real-time Audio Device state
static ma_device g_audioDevice;
static ma_context g_maContext;
static bool g_audioInitialized = false;
static bool g_maContextInit = false;

struct UsbCaptureDev {
    ma_device_id id;
    std::string name;
};
static std::vector<UsbCaptureDev> g_usbCaptures;
static int g_usbCaptureIndex = 0;       // last successfully opened capture
static int g_usbCaptureRequested = 0;   // UI-thread pending SRC
static bool g_usbDuplexActive = false;  // current device is duplex
static bool g_usbCaptureLive = false;   // duplex opened with a real capture device
static EngineType g_usbPreviousEngine = ENGINE_SYNTH;
static int g_usbPreviousAlgo = ALGO_PARALLEL;
static ma_uint32 g_audioPeriodFrames = 256;

double g_sampleRate = 48000.0;

// --- MASTER PERFORMANCE FX DSP STATES ---
#define STUTTER_BUF_SIZE 176400 // 4 seconds of stereo buffer at 44.1kHz
static float g_stutterBufferL[STUTTER_BUF_SIZE] = { 0.0f };
static float g_stutterBufferR[STUTTER_BUF_SIZE] = { 0.0f };
static uint32_t g_stutterWritePtr = 0;

static bool g_lastStutterActive = false;
static int g_lastStutterKey = -1;
static uint32_t g_stutterCapturePtr = 0;
static float g_stutterPlayhead = 0.0f;

static SvfFilter g_masterPerfFilterL;
static SvfFilter g_masterPerfFilterR;

// Stutter rhythmic subdivisions mapping to divFactors
static const float stutterDivFactors[8] = {
    1.0f,       // 1/1 beat
    0.5f,       // 1/2 beat
    0.25f,      // 1/4 beat
    0.125f,     // 1/8 beat
    0.166667f,  // 1/12 beat (eighth triplet)
    0.0625f,    // 1/16 beat
    0.083333f,  // 1/24 beat (sixteenth triplet)
    0.03125f    // 1/32 beat
};

// Global thread-safe modulated output buffer populated at block rate
float g_globalLFOValues[8][2] = { 0.0f };

// Fast, thread-safe, lock-free LCG pseudo-random number generator
static uint32_t FastRand(uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    return seed;
}

static float FastRandFloat(uint32_t& seed) {
    return (float)FastRand(seed) / 4294967295.0f;
}

// Fast, inline parameter-lock resolver
static inline int GetParam(int stepVal, int trackVal) {
    return (stepVal == -1) ? trackVal : stepVal;
}

static int SamplerSliceForNote(int trackIdx, int midiNote, const StepParams* sp) {
    const Track& trk = tracks[trackIdx];
    int algo = sp ? GetParam(sp->algorithm, trk.algorithm) : trk.algorithm;
    int sdiv = sp ? GetParam(sp->sliceDivisions, trk.sliceDivisions) : trk.sliceDivisions;
    if (trk.engineType == ENGINE_SAMPLER && algo == ALGO_SAMPLE && sdiv > 1)
        return SliceIndexFromMidi(midiNote, sdiv);
    return -1;
}

// Track-8 USB: always-on mixer channel. Envelope is stuck at 1; gated notes later.
static SvfFilter g_usbFilter;
static float g_usbSmoothCutoff = -1.0f;
static float g_usbSmoothVol = -1.0f;
static int g_usbFilterCounter = 0;
static float g_usbModCutoff = 0.0f;
static float g_usbModRes = 0.0f;
static float g_usbModVol = 0.0f;

static void ResetUsbFilter() {
    g_usbFilter.reset();
    g_usbSmoothCutoff = -1.0f;
    g_usbSmoothVol = -1.0f;
    g_usbFilterCounter = 0;
}

static float UsbProcess(int trackIdx, float inL, float inR) {
    Track& trk = tracks[trackIdx];
    int currentStepIdx = (trk.localTick / 6) % trk.stepLength;
    if (currentStepIdx < 0) currentStepIdx = 0;
    const StepParams& sp = trk.steps[currentStepIdx].params;

    g_usbFilterCounter++;
    if (g_usbFilterCounter >= 64) {
        g_usbFilterCounter = 0;
        g_usbModCutoff = 0.0f;
        g_usbModRes = 0.0f;
        g_usbModVol = 0.0f;
        for (int srcTrkIdx = 0; srcTrkIdx < 8; ++srcTrkIdx) {
            const Track& srcTrk = tracks[srcTrkIdx];
            for (int s = 0; s < 3; ++s) {
                const ModSlot& m1 = srcTrk.lfo1Slots[s];
                if (m1.destType == 1 && m1.destTrack == trackIdx) {
                    float modVal = g_globalLFOValues[srcTrkIdx][0] * (m1.depth / 99.0f);
                    if (m1.destParam == DEST_CUTOFF)         g_usbModCutoff += modVal * 99.0f;
                    else if (m1.destParam == DEST_RESONANCE) g_usbModRes += modVal * 99.0f;
                    else if (m1.destParam == DEST_VOLUME)    g_usbModVol += modVal * 99.0f;
                }
                const ModSlot& m2 = srcTrk.lfo2Slots[s];
                if (m2.destType == 1 && m2.destTrack == trackIdx) {
                    float modVal = g_globalLFOValues[srcTrkIdx][1] * (m2.depth / 99.0f);
                    if (m2.destParam == DEST_CUTOFF)         g_usbModCutoff += modVal * 99.0f;
                    else if (m2.destParam == DEST_RESONANCE) g_usbModRes += modVal * 99.0f;
                    else if (m2.destParam == DEST_VOLUME)    g_usbModVol += modVal * 99.0f;
                }
            }
        }

        float targetCutoff = std::clamp(GetParam(sp.filterCutoff, trk.filterCutoff) + g_usbModCutoff, 0.0f, 99.0f);
        if (g_usbSmoothCutoff < 0.0f) g_usbSmoothCutoff = targetCutoff;
        else g_usbSmoothCutoff += (targetCutoff - g_usbSmoothCutoff) * 0.25f; // block-rate slew

        float normCut = std::clamp(g_usbSmoothCutoff / 99.0f, 0.0f, 1.0f);
        float finalCutoffHz = 15.0f + (normCut * normCut * normCut * normCut) * 15985.0f;
        float resNorm = std::clamp(GetParam(sp.filterResonance, trk.filterResonance) + g_usbModRes, 0.0f, 99.0f) / 99.0f;
        g_usbFilter.calculateCoefficients(finalCutoffHz, resNorm, (float)g_sampleRate);
    }

    float mono = 0.5f * (inL + inR);
    int fType = GetParam(sp.filterType, trk.filterType);
    float filtered = g_usbFilter.process(mono, fType);

    float rawVol = std::clamp(GetParam(sp.masterVolume, trk.masterVolume) + g_usbModVol, 0.0f, 99.0f);
    float targetVol = VolumeCurve(rawVol);
    if (g_usbSmoothVol < 0.0f) g_usbSmoothVol = targetVol;
    else g_usbSmoothVol += (targetVol - g_usbSmoothVol) * 0.005f;

    return filtered * g_usbSmoothVol;
}
// Block-rate Global LFO phase generator engine
static void UpdateGlobalLFOs() {
    static uint32_t lfoRandSeed = 0x87654321u;
    static int lastStepIdx[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

    for (int t = 0; t < 8; ++t) {
        Track& trk = tracks[t];
        if (trk.muted) {
            g_globalLFOValues[t][0] = 0.0f;
            g_globalLFOValues[t][1] = 0.0f;
            continue;
        }

        // Resolve LFO locks from this track's local playing step (not selected-track playhead)
        int stepIdx = (trk.localTick / 6) % trk.stepLength;
        if (stepIdx < 0) stepIdx = 0;
        const StepParams& sp = trk.steps[stepIdx].params;

        int lfo1Speed = GetParam(sp.lfo1Speed, trk.lfo1Speed);
        int lfo1Wave  = GetParam(sp.lfo1Wave,  trk.lfo1Wave);
        int lfo1Depth = GetParam(sp.lfo1Depth, trk.lfo1Depth);
        int lfo1Trig  = GetParam(sp.lfo1Trigger, trk.lfo1Trigger);
        // int lfo1Sync = GetParam(sp.lfo1Sync, trk.lfo1Sync); // reserved: no DSP yet (track-level also unused)

        int lfo2Speed = GetParam(sp.lfo2Speed, trk.lfo2Speed);
        int lfo2Wave  = GetParam(sp.lfo2Wave,  trk.lfo2Wave);
        int lfo2Depth = GetParam(sp.lfo2Depth, trk.lfo2Depth);
        int lfo2Trig  = GetParam(sp.lfo2Trigger, trk.lfo2Trigger);

        // TRG: re-arm phase when the track advances to a new step
        if (stepIdx != lastStepIdx[t]) {
            if (lfo1Trig) trk.lfo1Phase = 0.0f;
            if (lfo2Trig) trk.lfo2Phase = 0.0f;
            lastStepIdx[t] = stepIdx;
        }

        // --- 1. EVALUATE LFO 1 ---
        {
            float normSpeed = lfo1Speed / 99.0f;
            float lfoHz = 0.05f * powf(400.0f, normSpeed); // Exponential mapping: 0.05Hz to 20Hz
            float phaseInc = (2.0f * 3.14159265f * lfoHz * 64.0f) / (float)g_sampleRate;

            trk.lfo1Phase += phaseInc;
            bool wrapped = false;
            if (trk.lfo1Phase >= 2.0f * 3.14159265f) {
                trk.lfo1Phase -= 2.0f * 3.14159265f;
                wrapped = true;
            }

            float val = 0.0f;
            if (lfo1Wave == 0) { // Sine
                val = sinf(trk.lfo1Phase);
            } else if (lfo1Wave == 1) { // Triangle
                float norm = trk.lfo1Phase / (2.0f * 3.14159265f);
                val = (norm < 0.25f) ? (norm * 4.0f) : ((norm < 0.75f) ? (2.0f - (norm * 4.0f)) : ((norm * 4.0f) - 4.0f));
            } else if (lfo1Wave == 2) { // Saw
                float norm = trk.lfo1Phase / (2.0f * 3.14159265f);
                val = 1.0f - (norm * 2.0f);
            } else if (lfo1Wave == 3) { // Square
                float norm = trk.lfo1Phase / (2.0f * 3.14159265f);
                val = (norm < 0.5f) ? 1.0f : -1.0f;
            } else if (lfo1Wave == 4) { // Sample & Hold
                                            if (wrapped || trk.lfo1LastVal == 0.0f) {
                                                trk.lfo1LastVal = FastRandFloat(lfoRandSeed) * 2.0f - 1.0f;
                                            }
                                            val = trk.lfo1LastVal;
            } else { // SEQ: Track Note Pitch Sequencer Follower [1]
                const Step& step = trk.steps[stepIdx];
                
                if (step.note >= 0) { // Read the integer sentinel directly (-1 means empty)
                    int midiVal = step.note; // Read direct integer (No NoteToMidi lookup!)
                    // Scale pitch symmetrically around C4 (MIDI 60) over a 2-octave range
                    val = (float)(midiVal - 60) / 24.0f;
                    val = std::clamp(val, -1.0f, 1.0f);
                } else {
                    val = 0.0f;
                }
            }
            // Scale and output value relative to depth
            g_globalLFOValues[t][0] = val * (lfo1Depth / 99.0f);
        }

        // --- 2. EVALUATE LFO 2 ---
        {
            float normSpeed = lfo2Speed / 99.0f;
            float lfoHz = 0.05f * powf(400.0f, normSpeed); // Exponential mapping: 0.05Hz to 20Hz
            float phaseInc = (2.0f * 3.14159265f * lfoHz * 64.0f) / (float)g_sampleRate;

            trk.lfo2Phase += phaseInc;
            bool wrapped = false;
            if (trk.lfo2Phase >= 2.0f * 3.14159265f) {
                trk.lfo2Phase -= 2.0f * 3.14159265f;
                wrapped = true;
            }

            float val = 0.0f;
            if (lfo2Wave == 0) { // Sine
                val = sinf(trk.lfo2Phase);
            } else if (lfo2Wave == 1) { // Triangle
                float norm = trk.lfo2Phase / (2.0f * 3.14159265f);
                val = (norm < 0.25f) ? (norm * 4.0f) : ((norm < 0.75f) ? (2.0f - (norm * 4.0f)) : ((norm * 4.0f) - 4.0f));
            } else if (lfo2Wave == 2) { // Saw
                float norm = trk.lfo2Phase / (2.0f * 3.14159265f);
                val = 1.0f - (norm * 2.0f);
            } else if (lfo2Wave == 3) { // Square
                float norm = trk.lfo2Phase / (2.0f * 3.14159265f);
                val = (norm < 0.5f) ? 1.0f : -1.0f;
            } else if (lfo2Wave == 4) { // Sample & Hold
                                            if (wrapped || trk.lfo2LastVal == 0.0f) {
                                                trk.lfo2LastVal = FastRandFloat(lfoRandSeed) * 2.0f - 1.0f;
                                            }
                                            val = trk.lfo2LastVal;
            } else { // SEQ: Track Note Pitch Sequencer Follower [1]
                const Step& step = trk.steps[stepIdx];
                
                if (step.note >= 0) { // Read the integer sentinel directly (-1 means empty)
                    int midiVal = step.note; // Read direct integer (No NoteToMidi lookup!)
                    val = (float)(midiVal - 60) / 24.0f;
                    val = std::clamp(val, -1.0f, 1.0f);
                } else {
                    val = 0.0f;
                }
            }
            // Scale and output value relative to depth
            g_globalLFOValues[t][1] = val * (lfo2Depth / 99.0f);
        }
    }
}
// Maps linear 0..99 volume parameters to a cubic gain curve
static inline float MapVolumeToGain(float volVal) {
    if (volVal <= 0.01f) return 0.0f;
    float norm = volVal / 99.0f;
    return norm * norm * norm; // Cubic curve mimics decibel projection
}

// Converts a 0..99 value to an exponential time duration in seconds.
// 1ms..15s, exponent 1.5: snappy 0-40 for perc, usable mid, pads from ~80 up
static float GetEnvTime(float val) {
    float norm = std::clamp(val, 0.0f, 99.0f) / 99.0f;
    return 0.0010f * powf(15000.0f, powf(norm, 1.5f));
}

// Cubic soft-clipper shaper to create warm, sweltering saturated waveforms
static float ApplySaturation(float input, float drive) {
    float driven = input * drive;
    if (driven > 1.0f)  driven = 1.0f;
    if (driven < -1.0f) driven = -1.0f;
    return driven - (driven * driven * driven) / 3.0f;
}
// --- PERCEPTUAL VOLUME LOOKUP TABLE (LUT) ---
constexpr int kVolCurveSize = 128;
float g_volCurveLUT[kVolCurveSize]; // Shared global array

// Populate the exponential dB fader curve once at startup
constexpr int kSineLutSize = 2048;
static float g_sineLUT[kSineLutSize + 1];

static void InitSineLut() {
    for (int i = 0; i <= kSineLutSize; ++i)
        g_sineLUT[i] = sinf((float)i * 6.2831853f / (float)kSineLutSize);
}

static inline float FastSine(float normPhase) {
    float x = normPhase * (float)kSineLutSize;
    int i = (int)x;
    float frac = x - (float)i;
    return g_sineLUT[i] + frac * (g_sineLUT[i + 1] - g_sineLUT[i]);
}

TrackVoiceMod g_trackVoiceMod[8];

static void AccumulateTrackVoiceMods() {
    for (int t = 0; t < 8; ++t)
        g_trackVoiceMod[t] = TrackVoiceMod{};

    for (int src = 0; src < 8; ++src) {
        const Track& srcTrk = tracks[src];
        for (int s = 0; s < 3; ++s) {
            const ModSlot* slots[2] = { &srcTrk.lfo1Slots[s], &srcTrk.lfo2Slots[s] };
            const float lfoVal[2] = { g_globalLFOValues[src][0], g_globalLFOValues[src][1] };
            for (int L = 0; L < 2; ++L) {
                const ModSlot& m = *slots[L];
                if (m.destType != 1) continue;
                if (m.destTrack < 0 || m.destTrack > 7) continue;
                TrackVoiceMod& d = g_trackVoiceMod[m.destTrack];
                float modVal = lfoVal[L] * (m.depth / 99.0f);
                float scaled99 = modVal * 99.0f;
                switch (m.destParam) {
                    case DEST_CUTOFF:     d.cutoff += scaled99; break;
                    case DEST_RESONANCE:  d.res += scaled99; break;
                    case DEST_VOLUME:     d.vol += scaled99; break;
                    case DEST_MORPH1:     d.morph1 += scaled99; break;
                    case DEST_MORPH2:     d.morph2 += scaled99; break;
                    case DEST_PITCH:      d.pitch += modVal * 12.0f; break;
                    case DEST_DECAY:      d.decay += scaled99; break;
                    case DEST_VOLUME2:    d.vol2 += scaled99; break;
                    case DEST_FINE1:      d.fine1 += scaled99; break;
                    case DEST_FINE2:      d.fine2 += scaled99; break;
                    case DEST_SAMP_POS:   d.morph1 += scaled99; break;
                    case DEST_SAMP_START: d.sampStart += scaled99; break;
                    case DEST_GRAN_SIZE:  d.granSize += scaled99; break;
                    case DEST_GRAN_DENS:  d.granDens += scaled99; break;
                    case DEST_GRAN_SCAT:  d.granScat += scaled99; break;
                    default: break;
                }
            }
        }
    }
}

void InitVolumeCurve() {
    constexpr float kMinDb = -40.0f; // Lower to -50.0f or -60.0f for more attenuation
    for (int i = 0; i < kVolCurveSize; ++i) {
        float norm = (float)i / (kVolCurveSize - 1);
        float db = kMinDb * (1.0f - norm);
        g_volCurveLUT[i] = powf(10.0f, db / 20.0f);
    }
}

// Cheap, sample-rate safe lookup with linear interpolation
float VolumeCurve(float value0to99) {
    if (value0to99 <= 0.0f) return 0.0f; // Hard zero at fader 0 prevents bleed
    
    float norm = std::clamp(value0to99, 0.0f, 99.0f) / 99.0f;
    float pos = norm * (kVolCurveSize - 1);
    int idx = (int)pos;
    float frac = pos - (float)idx;
    int idxNext = std::min(idx + 1, kVolCurveSize - 1);
    
    return g_volCurveLUT[idx] + frac * (g_volCurveLUT[idxNext] - g_volCurveLUT[idx]);
}
struct SynthVoice {
    bool choking = false;
    float chokeVolume = 1.0f;
    
    float phase1 = 0.0f;
    float phase2 = 0.0f;
    
    float baseFreq = 0.0f;
    float currentFreq = 0.0f;
    bool active = false;

    // Smooth parameter states to eliminate transition clicks
    float smoothCutoff = -1.0f;
    float smoothMorph1 = -1.0f;
    float smoothMorph2 = -1.0f;
    float smoothVol1 = -1.0f;
    float smoothVol1Raw = -1.0f; // Track-normalized 0..1 for drive coupling
    float smoothVol2 = -1.0f;

    // Virtual Auto-Gate States
    uint32_t gateTimerSamples = 0;
    bool useGateTimer = false;
    
    // Voice-local thread-safe random seed state
    uint32_t randomSeed = 0x12345678u;
    // Warm Analog Emulation States
    float osc1Drift = 0.0f;
    float osc2Drift = 0.0f;
    float osc1LPState = 0.0f;
    float osc2LPState = 0.0f;

    // Running modulation offsets evaluated at block-rate
    float modCutoffOffset = 0.0f;
    float modResOffset = 0.0f;
    float modVol1Offset = 0.0f;
    float modVol2Offset = 0.0f;
    float modMorph1Offset = 0.0f;
    float modMorph2Offset = 0.0f;
    float modPitchOffset = 0.0f;
    float modDecayOffset = 0.0f;
    float modFine1Offset = 0.0f;
    float modFine2Offset = 0.0f;

    // Pitch Sweep Modulation States
    float pitchModFactor = 1.0f;
    float pitchDecayRate = 1.0f;

    // Velocity Gain Scaling State
    float velocityScale = 1.0f;

    // Noise Generator AHD Envelope States
    float noiseEnvLevel = 0.0f;
    enum NoiseEnvStage { NOISE_IDLE, NOISE_ATTACK, NOISE_HOLD, NOISE_DECAY } noiseStage = NOISE_IDLE;
    uint32_t noiseHoldCounter = 0;

    // --- Filter & Filter Envelope States ---
    SvfFilter filter;
    float filterEnvLevel = 0.0f;
    enum FilterEnvStage { FLT_IDLE, FLT_ATTACK, FLT_DECAY, FLT_SUSTAIN, FLT_RELEASE } filterStage = FLT_IDLE;
    uint32_t filterUpdateCounter = 0;

    // Coarse/fine interval ratios, refreshed with the rest of the block-rate
    // parameters so the per-sample path is free of powf.
    float pitchRatio1 = 1.0f;
    float pitchRatio2 = 1.0f;
    // Keyboard tracking error for analog drift; baseFreq only changes on
    // trigger, so this is exact at block rate and free per sample.
    float trackingOctaves = 0.0f;

    // Voice Origin Tracker (Isolates live play from sequencer gate choking)
    bool triggeredBySequencer = false;

    // Feedback buffer for Operator 2 (Modulator self-feedback)
    float lastModOutput = 0.0f;
    
    // Envelope 1 (Carrier / Osc 1)
    float envLevel1 = 0.0f;
    enum EnvStage1 { ENV1_IDLE, ENV1_ATTACK, ENV1_DECAY, ENV1_SUSTAIN, ENV1_RELEASE } stage1 = ENV1_IDLE;

    // Envelope 2 (Modulator / Osc 2)
    float envLevel2 = 0.0f;
    enum EnvStage2 { ENV2_IDLE, ENV2_ATTACK, ENV2_DECAY, ENV2_SUSTAIN, ENV2_RELEASE } stage2 = ENV2_IDLE;
    
    // Cached envelope increment rates and levels calculated at block rate
    float envAtkRate1 = 0.0f;
    float envDecCoeff1 = 1.0f;
    float envRelCoeff1 = 1.0f;
    float envSusLevel1 = 0.0f;

    float envAtkRate2 = 0.0f;
    float envDecCoeff2 = 1.0f;
    float envRelCoeff2 = 1.0f;
    float envSusLevel2 = 0.0f;

    float noiseAtkRate = 0.0f;
    float noiseDecRate = 0.0f;
    uint32_t noiseHoldSamples = 0;

    float filterAtkRate = 0.0f;
    float filterDecCoeff = 1.0f;
    float filterRelCoeff = 1.0f;
    float filterSusLevel = 0.0f;
    
    void Choke() {
        choking = true;
        chokeVolume = 1.0f;
    }
    
    void Trigger(float targetFreq, int depth, int time, int velocity, bool isSeq = false, int noteLength = 0, int glideTimeVal = 0) {
            // --- FIXED ACID LEGATO GLIDE CHECK ---
            // A note is only legato if the voice is active AND currently making sound (not idle/silent)
            bool isLegato = active && (stage1 != ENV1_IDLE) && (glideTimeVal > 0);

            if (isLegato) {
                baseFreq = targetFreq; // Set the new pitch destination
                choking = false;      // Cancel any voice choking
                chokeVolume = 1.0f;   // Reset choke gain to full
                
                // Re-trigger velocity scaling and auto-gate timers for the new note
                velocityScale = (velocity == 1) ? 0.33f : ((velocity == 2) ? 0.66f : 1.0f);

                if (isSeq) {
                    double tickLengthSeconds = 2.5 / tempo;
                    uint32_t samplesPerTick = (uint32_t)(tickLengthSeconds * g_sampleRate);
                    uint32_t samplesPerStep = samplesPerTick * 6;
                    
                    float holdStepsCount = (noteLength == 0) ? 0.85f : ((float)noteLength - 0.15f);
                    gateTimerSamples = (uint32_t)(samplesPerStep * holdStepsCount);
                    useGateTimer = true;
                }
                return; // Exit early to prevent envelope re-triggering!
            }

            // --- NORMAL DETACHED TRIGGER ---
            // Retriggering a voice that is still sounding must not slam its
            // state to zero. Every zeroed variable below is a step change in
            // the output, and ratchets and voice stealing hit this path
            // constantly, so the clicks pile up into a continuous crunch.
            // Re-enter attack from wherever the envelope already is instead.
            bool wasSounding = active && (stage1 != ENV1_IDLE);

            if (stage1 == ENV1_IDLE || !active) {
                currentFreq = targetFreq; // Start glide from target pitch if triggering from silence
            }
            baseFreq = targetFreq;
            active = true;
            stage1 = ENV1_ATTACK;
            stage2 = ENV2_ATTACK;
            if (!wasSounding) {
                lastModOutput = 0.0f; // Flush feedback buffer on note trigger
                envLevel1 = 0.0f;
                envLevel2 = 0.0f;
            }
            choking = false;     // Reset choke flags so note plays cleanly
            chokeVolume = 1.0f;

        // Map velocity steps (1..3) to linear scaling (0.33 to 1.0)
        velocityScale = (velocity == 1) ? 0.33f : ((velocity == 2) ? 0.66f : 1.0f);

        // Store voice trigger origin
        triggeredBySequencer = isSeq;

        // Initialize Exponential Pitch Sweep (pd and st)
        if (depth > 0) {
            float semitones = (depth / 99.0f) * 48.0f; // Clamps to up to +4 octaves
            pitchModFactor = powf(2.0f, semitones / 12.0f);
            
            // Sweep decay duration (from 1ms up to 1.5 seconds)
            float durationSec = 0.001f + (time / 99.0f) * 1.5f;
            pitchDecayRate = expf(-6.9078f / (g_sampleRate * durationSec)); // Standard 60dB decay coefficient
        } else {
            pitchModFactor = 1.0f;
            pitchDecayRate = 1.0f;
        }

        // Initialize Noise Generator AHD Envelope
        noiseStage = NOISE_ATTACK;
        noiseHoldCounter = 0;
        if (!wasSounding) noiseEnvLevel = 0.0f;

        // Initialize Filter State and Envelope
        filterStage = FLT_ATTACK;
        filterEnvLevel = 0.0f;
        filterUpdateCounter = 9999; // Force instant coefficient calculation
        // s1 and s2 hold the filter's stored energy. Clearing them on a voice
        // that is still ringing is the loudest click of the lot, especially
        // with resonance up.
        if (!wasSounding) filter.reset();

        // Reset smooth state flags to trigger instant snapping on first process
        // frame. A sounding voice already has valid smoothed values, so let it
        // glide to the new ones rather than jumping.
        if (!wasSounding) {
            smoothCutoff = -1.0f;
            smoothMorph1 = -1.0f;
            smoothMorph2 = -1.0f;
            smoothVol1 = -1.0f;
            smoothVol1Raw = -1.0f;
            smoothVol2 = -1.0f;
        }

        // Reset analog emulation filters and drifts on note trigger
        osc1Drift = 0.0f;
        osc2Drift = 0.0f;
        if (!wasSounding) {
            osc1LPState = 0.0f;
            osc2LPState = 0.0f;
        }
        
        // Seed voice-local random generator uniquely based on trigger properties
        randomSeed = 0x12345678u + (uint32_t)(targetFreq * 100.0f);

        // Reset mod offsets
        modCutoffOffset = 0.0f;
        modResOffset = 0.0f;
        modVol1Offset = 0.0f;
        modVol2Offset = 0.0f;
        modMorph1Offset = 0.0f;
        modMorph2Offset = 0.0f;
        modPitchOffset = 0.0f;
        modDecayOffset = 0.0f;
        modFine1Offset = 0.0f;
        modFine2Offset = 0.0f;

        // Auto-Gate Timer Initialization
        if (isSeq) {
            double tickLengthSeconds = 2.5 / tempo;
            uint32_t samplesPerTick = (uint32_t)(tickLengthSeconds * g_sampleRate);
            uint32_t samplesPerStep = samplesPerTick * 6;
            
            // Hold for noteLength steps (subtracting 15% step decay interval for release spacing)
            float holdStepsCount = (noteLength == 0) ? 0.85f : ((float)noteLength - 0.15f);
            gateTimerSamples = (uint32_t)(samplesPerStep * holdStepsCount);
            useGateTimer = true;
        } else {
            useGateTimer = false;
            gateTimerSamples = 0;
        }
    }

    void Release() {
        if (stage1 != ENV1_IDLE) stage1 = ENV1_RELEASE;
        if (stage2 != ENV2_IDLE) stage2 = ENV2_RELEASE;
        
        // Quiet noise immediately on release
        if (noiseStage != NOISE_IDLE) noiseStage = NOISE_DECAY;

        // Release Filter Envelope
        if (filterStage != FLT_IDLE) filterStage = FLT_RELEASE;
    }
    // Polynomial band-limited step correction helper
        static inline float blep(float t, float dt) {
            if (t < dt) {
                float x = t / dt - 1.0f;
                return -x * x;
            } else if (t > 1.0f - dt) {
                float x = (t - 1.0f) / dt + 1.0f;
                return x * x;
            }
            return 0.0f;
        }    static inline float WaveTri(float normPhase) {
        if (normPhase < 0.25f)      return normPhase * 4.0f;
        else if (normPhase < 0.75f) return 2.0f - (normPhase * 4.0f);
        else                        return (normPhase * 4.0f) - 4.0f;
    }

    // Band-limit the Saw wave (step of +2.0 at wrap point)
    static inline float WaveSaw(float normPhase, float dt) {
        return (1.0f - (normPhase * 2.0f)) + blep(normPhase, dt);
    }

    // Band-limit the Square wave (step of +1.0 at 0.0, and -1.0 at 0.5)
    static inline float WaveSqr(float normPhase, float dt) {
        float naiveSqr = (normPhase < 0.5f) ? 0.5f : -0.5f;
        float phaseSquare2 = normPhase + 0.5f;
        if (phaseSquare2 >= 1.0f) phaseSquare2 -= 1.0f; // Symmetrical wrap
        return naiveSqr + 0.5f * blep(normPhase, dt) - 0.5f * blep(phaseSquare2, dt);
    }

    // Process a single wave slice dynamically (expects normalized phase in [0, 1))
    // Only the two waveforms either side of the morph position are evaluated.
    // This runs twice per voice per sample, so computing all four and discarding
    // half of them was the largest avoidable cost in the engine: a sinf and up to
    // four blep calls thrown away on every single sample.
    float ProcessWave(float normPhase, float dt, int morph) {
        // Wrap normalized phase to [0.0, 1.0)
        while (normPhase >= 1.0f) normPhase -= 1.0f;
        while (normPhase < 0.0f)  normPhase += 1.0f;

        if (morph < 33) {
            // Scale by 2*PI only at the moment of sine calculation
            float t = morph / 33.0f;
            float sineSample = FastSine(normPhase);
            if (t <= 0.0f) return sineSample;
            return (1.0f - t) * sineSample + t * WaveTri(normPhase);
        } else if (morph < 66) {
            float t = (morph - 33) / 33.0f;
            float triSample = WaveTri(normPhase);
            if (t <= 0.0f) return triSample;
            return (1.0f - t) * triSample + t * WaveSaw(normPhase, dt);
        } else {
            float t = (morph - 66) / 33.0f;
            float sawSample = WaveSaw(normPhase, dt);
            if (t <= 0.0f) return sawSample;
            return (1.0f - t) * sawSample + t * WaveSqr(normPhase, dt);
        }
    }

    float Process(int trackIdx) {
        if (stage1 == ENV1_IDLE && stage2 == ENV2_IDLE && noiseStage == NOISE_IDLE && filterStage == FLT_IDLE) return 0.0f;

        // 1. Process the Auto-Gate Timer (Tells the ADSR to release)
        if (useGateTimer) {
            if (gateTimerSamples > 0) {
                gateTimerSamples--;
                if (gateTimerSamples == 0) {
                    Release();
                    useGateTimer = false;
                }
            }
        }

        // 2. Fast crossfade choke ramp (Prevents clicks on sudden overlaps)
        if (choking) {
            chokeVolume -= 1.0f / 256.0f;
            if (chokeVolume <= 0.0f) {
                chokeVolume = 0.0f;
                active = false;
                choking = false;
                stage1 = ENV1_IDLE;
                stage2 = ENV2_IDLE;
                noiseStage = NOISE_IDLE;
                filterStage = FLT_IDLE;
                return 0.0f;
            }
        }

        const Track& trk = tracks[trackIdx];
        // Per-track playhead — not the selected-track global (polymeter-safe)
        int stepIdx = (trk.localTick / 6) % trk.stepLength;
        if (stepIdx < 0) stepIdx = 0;
        const StepParams& sp = trk.steps[stepIdx].params;

        // Reusable fallback helper: if a step parameter is unlocked (-1), use the global track default
        auto GetParam = [](int stepVal, int trackVal) {
            return (stepVal == -1) ? trackVal : stepVal;
        };

        int algo = GetParam(sp.algorithm, trk.algorithm);

        // Resolve active Analog value contextually (Reuses fmFeedback parameter in Parallel mode)
        int analogVal = GetParam(sp.fmFeedback, trk.fmFeedback);
        float analogAmount = (algo == ALGO_PARALLEL) ? (analogVal / 99.0f) : 0.0f;
        
        // --- 0. PARAMETER SMOOTHING / GLIDE CALCULATIONS (Per-Sample) ---
        // Combine base parameters with LFO modulation offsets (clamped to safe ranges)
        float targetMorph1 = std::clamp(GetParam(sp.morph, trk.morph) + modMorph1Offset, 0.0f, 99.0f);
        float targetMorph2 = std::clamp(GetParam(sp.morph2, trk.morph2) + modMorph2Offset, 0.0f, 99.0f);
        
        // Resolve Oscillator 1 targets
        float targetVol1Val = std::clamp(GetParam(sp.volume, trk.volume) + modVol1Offset, 0.0f, 99.0f);
        float targetVol1Gain = VolumeCurve(targetVol1Val); // Perceptual volume LUT lookup
        float targetVol1Raw  = targetVol1Val / 99.0f;       // Raw normalized 0..1 for drive coupling

        // Resolve Oscillator 2 targets contextually
        float targetVol2Val = std::clamp(GetParam(sp.volume2, trk.volume2) + modVol2Offset, 0.0f, 99.0f);
        float targetVol2Gain = (algo == ALGO_CARRIER_MOD)
            ? (targetVol2Val / 99.0f)                      // Raw 0..1 in FM mode to protect sweet spot
            : VolumeCurve(targetVol2Val);                  // Perceptual volume LUT in Parallel mode

        if (smoothMorph1 < 0.0f) {
            smoothMorph1 = targetMorph1;
            smoothMorph2 = targetMorph2;
            smoothVol1 = targetVol1Gain;
            smoothVol1Raw = targetVol1Raw;
            smoothVol2 = targetVol2Gain;
        } else {
            smoothMorph1 += (targetMorph1 - smoothMorph1) * 0.005f;
            smoothMorph2 += (targetMorph2 - smoothMorph2) * 0.005f;
            smoothVol1 += (targetVol1Gain - smoothVol1) * 0.005f;
            smoothVol1Raw += (targetVol1Raw - smoothVol1Raw) * 0.005f;
            smoothVol2 += (targetVol2Gain - smoothVol2) * 0.005f;
        }

        // Slew cutoff parameter per-sample to eliminate block-rate zipper noise and step-lock clicks
        float targetCutoff = std::clamp(GetParam(sp.filterCutoff, trk.filterCutoff) + modCutoffOffset, 0.0f, 99.0f);
        if (smoothCutoff < 0.0f) {
            smoothCutoff = targetCutoff;
        } else {
            smoothCutoff += (targetCutoff - smoothCutoff) * 0.004f; // Smooth 5-10ms slew
        }
        
        // --- 1. PROCESS ENVELOPE 1 (Carrier) ---
        switch (stage1) {
            case ENV1_ATTACK:
                envLevel1 += envAtkRate1;
                if (envLevel1 >= 1.0f) { envLevel1 = 1.0f; stage1 = ENV1_DECAY; }
                break;
            case ENV1_DECAY:
                envLevel1 = envSusLevel1 + (envLevel1 - envSusLevel1) * envDecCoeff1;
                if (envLevel1 - envSusLevel1 <= 0.0001f) { envLevel1 = envSusLevel1; stage1 = ENV1_SUSTAIN; }
                break;
            case ENV1_SUSTAIN:
                envLevel1 = envSusLevel1;
                break;
            case ENV1_RELEASE:
                envLevel1 *= envRelCoeff1;
                if (envLevel1 <= 0.0001f) { envLevel1 = 0.0f; stage1 = ENV1_IDLE; }
                break;
            default: break;
        }

        // --- 2. PROCESS ENVELOPE 2 (Modulator) ---
        switch (stage2) {
            case ENV2_ATTACK:
                envLevel2 += envAtkRate2;
                if (envLevel2 >= 1.0f) { envLevel2 = 1.0f; stage2 = ENV2_DECAY; }
                break;
            case ENV2_DECAY:
                envLevel2 = envSusLevel2 + (envLevel2 - envSusLevel2) * envDecCoeff2;
                if (envLevel2 - envSusLevel2 <= 0.0001f) { envLevel2 = envSusLevel2; stage2 = ENV2_SUSTAIN; }
                break;
            case ENV2_SUSTAIN:
                envLevel2 = envSusLevel2;
                break;
            case ENV2_RELEASE:
                envLevel2 *= envRelCoeff2;
                if (envLevel2 <= 0.0001f) { envLevel2 = 0.0f; stage2 = ENV2_IDLE; }
                break;
            default: break;
        }

        // --- 3. PROCESS NOISE GENERATOR AHD ENVELOPE ---
        switch (noiseStage) {
            case NOISE_ATTACK:
                noiseEnvLevel += noiseAtkRate;
                if (noiseEnvLevel >= 1.0f) {
                    noiseEnvLevel = 1.0f;
                    noiseStage = NOISE_HOLD;
                    noiseHoldCounter = 0;
                }
                break;
            case NOISE_HOLD:
                noiseHoldCounter++;
                if (noiseHoldCounter >= noiseHoldSamples) {
                    noiseStage = NOISE_DECAY;
                }
                break;
            case NOISE_DECAY:
                noiseEnvLevel -= noiseDecRate;
                if (noiseEnvLevel <= 0.0f) {
                    noiseEnvLevel = 0.0f;
                    noiseStage = NOISE_IDLE;
                }
                break;
            default:
                break;
        }

        // --- 4. PROCESS FILTER ENVELOPE (ADSR) ---
        switch (filterStage) {
            case FLT_ATTACK:
                filterEnvLevel += filterAtkRate;
                if (filterEnvLevel >= 1.0f) { filterEnvLevel = 1.0f; filterStage = FLT_DECAY; }
                break;
            case FLT_DECAY:
                filterEnvLevel = filterSusLevel + (filterEnvLevel - filterSusLevel) * filterDecCoeff;
                if (filterEnvLevel - filterSusLevel <= 0.0001f) { filterEnvLevel = filterSusLevel; filterStage = FLT_SUSTAIN; }
                break;
            case FLT_SUSTAIN:
                filterEnvLevel = filterSusLevel;
                break;
            case FLT_RELEASE:
                filterEnvLevel *= filterRelCoeff;
                if (filterEnvLevel <= 0.0001f) { filterEnvLevel = 0.0f; filterStage = FLT_IDLE; }
                break;
            default: break;
        }

        // Below ~-60 dB the oscillators are inaudible; stop running them.
        constexpr float kSilentAmp = 0.001f;
        if (stage1 != ENV1_ATTACK && envLevel1 <= kSilentAmp
            && stage2 != ENV2_ATTACK && envLevel2 <= kSilentAmp
            && (noiseStage == NOISE_IDLE || noiseEnvLevel <= kSilentAmp)
            && !choking) {
            envLevel1 = 0.0f;
            envLevel2 = 0.0f;
            noiseEnvLevel = 0.0f;
            filterEnvLevel = 0.0f;
            stage1 = ENV1_IDLE;
            stage2 = ENV2_IDLE;
            noiseStage = NOISE_IDLE;
            filterStage = FLT_IDLE;
            active = false;
            return 0.0f;
        }

        // --- 5. PROCESS REAL-TIME PITCH DECAY SWEEP ---
        if (pitchModFactor > 1.0f) {
            pitchModFactor = 1.0f + (pitchModFactor - 1.0f) * pitchDecayRate;
        } else {
            pitchModFactor = 1.0f;
        }

        // --- 6. BLOCK-RATE SVF COEFFICIENTS UPDATE (Every 64 Samples) ---
        filterUpdateCounter++;
        if (filterUpdateCounter >= 64) {
            filterUpdateCounter = 0;
            // Update slow-moving pitch drift (slop)
            if (analogAmount > 0.0f) {
                float rawNoise1 = FastRandFloat(randomSeed) * 2.0f - 1.0f;
                float rawNoise2 = FastRandFloat(randomSeed) * 2.0f - 1.0f;
                osc1Drift = osc1Drift * 0.92f + rawNoise1 * 0.08f;
                osc2Drift = osc2Drift * 0.92f + rawNoise2 * 0.08f;
            } else {
                osc1Drift = 0.0f;
                osc2Drift = 0.0f;
            }
            // Recalculate envelope parameters at block rate instead of per sample
            float invSampleRate = 1.0f / (float)g_sampleRate;

            // LFO offsets are track-rate (filled once per chunk).
            const TrackVoiceMod& tm = g_trackVoiceMod[trackIdx];
            modCutoffOffset = tm.cutoff;
            modResOffset = tm.res;
            modVol1Offset = tm.vol;
            modVol2Offset = tm.vol2;
            modMorph1Offset = tm.morph1;
            modMorph2Offset = tm.morph2;
            modPitchOffset = tm.pitch;
            modDecayOffset = tm.decay;
            modFine1Offset = tm.fine1;
            modFine2Offset = tm.fine2;

            envAtkRate1 = invSampleRate / GetEnvTime((float)GetParam(sp.attack, trk.attack));
            float decTime1 = GetEnvTime((float)GetParam(sp.decay, trk.decay) + modDecayOffset);
            envDecCoeff1 = expf(-6.9078f / ((float)g_sampleRate * decTime1));
            float relTime1 = GetEnvTime((float)GetParam(sp.release, trk.release));
            envRelCoeff1 = expf(-6.9078f / ((float)g_sampleRate * relTime1));
            envSusLevel1 = GetParam(sp.sustain, trk.sustain) / 99.0f;

            envAtkRate2 = invSampleRate / GetEnvTime((float)GetParam(sp.attack2, trk.attack2));
            float decTime2 = GetEnvTime((float)GetParam(sp.decay2, trk.decay2));
            envDecCoeff2 = expf(-6.9078f / ((float)g_sampleRate * decTime2));
            float relTime2 = GetEnvTime((float)GetParam(sp.release2, trk.release2));
            envRelCoeff2 = expf(-6.9078f / ((float)g_sampleRate * relTime2));
            envSusLevel2 = GetParam(sp.sustain2, trk.sustain2) / 99.0f;

            noiseAtkRate = invSampleRate / GetEnvTime((float)GetParam(sp.noiseAttack, trk.noiseAttack));
            noiseDecRate = invSampleRate / GetEnvTime((float)GetParam(sp.noiseDecay, trk.noiseDecay));
            noiseHoldSamples = (uint32_t)(GetEnvTime((float)GetParam(sp.noiseHold, trk.noiseHold)) * g_sampleRate);

            filterAtkRate = invSampleRate / GetEnvTime((float)GetParam(sp.filterAttack, trk.filterAttack));
            float fDecTime = GetEnvTime((float)GetParam(sp.filterDecay, trk.filterDecay));
            filterDecCoeff = expf(-6.9078f / ((float)g_sampleRate * fDecTime));
            float fRelTime = GetEnvTime((float)GetParam(sp.filterRelease, trk.filterRelease));
            filterRelCoeff = expf(-6.9078f / ((float)g_sampleRate * fRelTime));
            filterSusLevel = GetParam(sp.filterSustain, trk.filterSustain) / 99.0f;

            // Modulate the normalized control position (0.0 to 1.0) logarithmically before mapping to Hz
            float normCut = smoothCutoff / 99.0f;
            int dptVal = GetParam(sp.filterEnvDepth, trk.filterEnvDepth);
            float envMod = filterEnvLevel * (dptVal / 99.0f);

            // Blend base cutoff and envelope modulation in the normalized control domain
            float finalNormCut = std::clamp(normCut + envMod, 0.0f, 1.0f);

            // Quartic mapping of modulated cutoff (15Hz to 16000Hz)
            float finalCutoffHz = 15.0f + (finalNormCut * finalNormCut * finalNormCut * finalNormCut) * 15985.0f;
            float resNorm = std::clamp(GetParam(sp.filterResonance, trk.filterResonance) + modResOffset, 0.0f, 99.0f) / 99.0f;

            // Recalculate SVF coefficients
            filter.calculateCoefficients(finalCutoffHz, resNorm, (float)g_sampleRate);

            // --- BLOCK-RATE PITCH RATIOS ---
            // These are pure interval ratios: coarse, fine and the pitch mod
            // offset are all resolved in this same block, so nothing about them
            // changes between samples. Running powf per sample per oscillator
            // was buying no extra accuracy, only cost.
            float bFine1 = std::clamp(GetParam(sp.fine, trk.fine) + modFine1Offset, -99.0f, 99.0f);
            float bSemi1 = GetParam(sp.coarse, trk.coarse) + (bFine1 / 100.0f);
            pitchRatio1 = powf(2.0f, (bSemi1 + modPitchOffset) / 12.0f);

            float bFine2 = std::clamp(GetParam(sp.fine2, trk.fine2) + modFine2Offset, -99.0f, 99.0f);
            float bSemi2 = GetParam(sp.coarse2, trk.coarse2) + (bFine2 / 100.0f);
            pitchRatio2 = powf(2.0f, bSemi2 / 12.0f);

            trackingOctaves = log2f(std::max(baseFreq, 1.0f) / 261.63f);
        }

        // --- 7. FREQUENCY CALCULATIONS & PORTAMENTO ---
                int glideVal = GetParam(sp.glideTime, trk.glideTime);
                if (glideVal > 0) {
                    // Cubic scaling maps 1..99 control to a highly musical 1ms to 2.5s curve
                    float normGlide = glideVal / 99.0f;
                    float glideTimeSec = 0.001f + (normGlide * normGlide * normGlide) * 2.5f;
                    float glideCoeff = 1.0f / (glideTimeSec * g_sampleRate);
                    
                    if (glideCoeff > 1.0f) glideCoeff = 1.0f;
                    
                    currentFreq += glideCoeff * (baseFreq - currentFreq);
                } else {
                    currentFreq = baseFreq; // Snap instantly if glide is off
                }

                // currentFreq glides and pitchModFactor decays per sample; the
                // interval ratio is block-rate and cached above.
                float freq1 = currentFreq * pitchModFactor * pitchRatio1;

        float finalSample = 0.0f;

        if (algo == ALGO_PARALLEL) {
            // ==========================================
            // ALGORITHM A: DUAL-OSCILLATOR MIX (PARALLEL)
            // ==========================================
            float freq2 = baseFreq * pitchModFactor * pitchRatio2;

            float freq1AnalogScale = 1.0f;
            float freq2AnalogScale = 1.0f;

            if (analogAmount > 0.0f) {
                // 1. Apply random pitch slop (drift)
                freq1AnalogScale += osc1Drift * analogAmount * 0.0022f;
                freq2AnalogScale += osc2Drift * analogAmount * 0.0022f;
                
                // 2. Apply static detune offset
                freq2AnalogScale += analogAmount * 0.00042f;

                // 3. Apply Differential Keyboard Tracking Error (Oscillator Divergence)
                float trackingDivergence = trackingOctaves * analogAmount * 0.005f;
                freq1AnalogScale += trackingDivergence;
                freq2AnalogScale -= trackingDivergence;
            }

            float finalFreq1 = freq1 * freq1AnalogScale;
            float finalFreq2 = freq2 * freq2AnalogScale;

            float dt1 = finalFreq1 / (float)g_sampleRate;
            float dt2 = finalFreq2 / (float)g_sampleRate;

            float rawOsc1 = ProcessWave(phase1, dt1, (int)smoothMorph1);
            float rawOsc2 = ProcessWave(phase2, dt2, (int)smoothMorph2);

            // Apply 1-pole low pass filter waveform softening
            if (analogAmount > 0.0f) {
                float lpCoeff = 1.0f - (analogAmount * 0.75f);
                osc1LPState += lpCoeff * (rawOsc1 - osc1LPState);
                osc2LPState += lpCoeff * (rawOsc2 - osc2LPState);
                rawOsc1 = osc1LPState;
                rawOsc2 = osc2LPState;
            }

            float drive = 1.0f + (smoothVol1Raw * 3.0f);
            float saturatedOsc1 = ApplySaturation(rawOsc1, drive);

            float osc1 = saturatedOsc1 * envLevel1 * smoothVol1;
            float osc2 = rawOsc2 * envLevel2 * smoothVol2;

            finalSample = (osc1 + osc2) * 0.5f;

            phase1 += finalFreq1 / (float)g_sampleRate;
            phase2 += finalFreq2 / (float)g_sampleRate;
        }
        else {
            // ==========================================
            // ALGORITHM B: 2-OP PHASE MODULATION FM (CARRIER / MODULATOR)
            // ==========================================
            float fine2Mod = std::clamp(GetParam(sp.fine2, trk.fine2) + modFine2Offset, -99.0f, 99.0f);
            float ratio = GetParam(sp.coarse2, trk.coarse2) + (fine2Mod / 100.0f);
            if (ratio < 0.05f) ratio = 0.05f;
            float freq2 = freq1 * ratio;

            float dt1 = freq1 / (float)g_sampleRate;
            float dt2 = freq2 / (float)g_sampleRate;

            float feedbackScale = (GetParam(sp.fmFeedback, trk.fmFeedback) / 99.0f) * 0.5f;
            float feedbackPhase = phase2 + lastModOutput * feedbackScale;

            float modDry = ProcessWave(feedbackPhase, dt2, (int)smoothMorph2) * envLevel2;
            lastModOutput = modDry;

            float index = smoothVol2 * smoothVol2 * smoothVol2 * 1.2732395f;

            float modulatedPhase1 = phase1 + modDry * index;
            float carrier = ProcessWave(modulatedPhase1, dt1, (int)smoothMorph1) * envLevel1 * smoothVol1;

            finalSample = carrier;

            phase1 += freq1 / (float)g_sampleRate;
            phase2 += freq2 / (float)g_sampleRate;
        }

        // Wrap normalized phases
        if (phase1 >= 1.0f) phase1 -= 1.0f;
        if (phase2 >= 1.0f) phase2 -= 1.0f;

        // --- 8. PROCESS WHITE NOISE TRANSIENT ---
        float rawNoise = FastRandFloat(randomSeed) * 2.0f - 1.0f;
        float scaledNoise = rawNoise * noiseEnvLevel * VolumeCurve(GetParam(sp.noiseVolume, trk.noiseVolume));

        // Mix noise with synthesized waves BEFORE filter stage
        float combinedSignal = finalSample + scaledNoise;
        int fType = GetParam(sp.filterType, trk.filterType);

        // --- 9. RUN THROUGH SILKY STATE-VARIABLE FILTER (SVF) ---
        float targetMasterVol = VolumeCurve(GetParam(sp.masterVolume, trk.masterVolume));
        return filter.process(combinedSignal, fType, analogAmount) * velocityScale * chokeVolume * targetMasterVol;
    }
};

// 8 Synth Tracks, each polyphonic by 4 voices
static SynthVoice g_trackVoices[8][4];
static int g_trackVoiceIndex[8] = { 0 };

static ma_uint32 g_tickSampleAccumulator = 0;
static int g_currentTick = -1; // -1 allows step 0 to trigger instantly on play

// Live MIDI note queues: producers (RtMidi / UI) never touch voices directly.
MidiLiveQueue g_midiLiveQueueFromRt;
MidiLiveQueue g_midiLiveQueueFromUi;

static bool PushMidiLive(MidiLiveQueue& q, const MidiLiveEvent& ev) {
    const uint32_t w = q.writeIdx.load(std::memory_order_relaxed);
    const uint32_t r = q.readIdx.load(std::memory_order_acquire);
    if ((w - r) >= (uint32_t)kMidiLiveQueueCap) {
        g_midiLiveDrops.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    q.slots[w % (uint32_t)kMidiLiveQueueCap] = ev;
    q.writeIdx.store(w + 1, std::memory_order_release);
    return true;
}

static void TriggerVoiceLiveImpl(int trackIdx, int midiNote, int velocity);
static void ReleaseVoiceLiveImpl(int trackIdx, int midiNote);
static void ApplyMidiLiveEvent(const MidiLiveEvent& ev);

static void DrainMidiLiveQueue(MidiLiveQueue& q) {
    uint32_t r = q.readIdx.load(std::memory_order_relaxed);
    const uint32_t w = q.writeIdx.load(std::memory_order_acquire);
    while (r != w) {
        ApplyMidiLiveEvent(q.slots[r % (uint32_t)kMidiLiveQueueCap]);
        ++r;
    }
    q.readIdx.store(r, std::memory_order_release);
}

static void DrainAllMidiLiveQueues() {
    DrainMidiLiveQueue(g_midiLiveQueueFromRt);
    DrainMidiLiveQueue(g_midiLiveQueueFromUi);
}

static uint64_t SteadyNowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Keep track of bar loops per track to evaluate trigger conditions (e.g., 1:2)
static int g_trackBarCount[8] = { 0 };

// Trig conditions want a bar number that restarts with the pattern loop, but
// the generator wants one that keeps climbing, otherwise chaos would replay the
// same handful of bars forever instead of evolving. Reset only on transport
// stop and pattern change, so a take is still reproducible from the top.
static unsigned int g_chaosBar[8] = { 0 };

// Sample-accurate retrigger state per track
struct ActiveRetrig {
    int midiNote = -1;
    int remainingTriggers = 0;
    ma_uint32 sampleInterval = 0;
    ma_uint32 sampleCounter = 0;
    int velocity = 3; // Tracks velocity on ratchets
    int voiceIndex = 0;   // sampler ratchets reuse this slot
    int polyMode = 4;     // synth ratchets stay inside poly
    ma_uint32 samplesLeftInStep = 0;
};
static ActiveRetrig g_activeRetrigs[8];

static int StealSynthVoice(int t, int polyMode) {
    int best = 0;
    float bestScore = 1e30f;
    for (int v = 0; v < polyMode; ++v) {
        const SynthVoice& vo = g_trackVoices[t][v];
        if (!vo.active || vo.stage1 == SynthVoice::ENV1_IDLE)
            return v;
        float score = vo.envLevel1;
        if (vo.stage1 == SynthVoice::ENV1_ATTACK)
            score += 2.0f;
        if (score < bestScore) {
            bestScore = score;
            best = v;
        }
    }
    return best;
}

static int StealSamplerVoice(int t, int polyMode) {
    int best = 0;
    float bestScore = 1e30f;
    for (int v = 0; v < polyMode; ++v) {
        const SamplerVoice& vo = g_samplerVoices[t][v];
        if (!vo.active || vo.stage == SamplerVoice::ENV_IDLE)
            return v;
        float score = vo.envLevel;
        if (vo.stage == SamplerVoice::ENV_ATTACK)
            score += 2.0f;
        if (score < bestScore) {
            bestScore = score;
            best = v;
        }
    }
    return best;
}

// Dynamic fraction parser: matches loop expressions of form "N:M" (e.g., "3:4", "7:8")
static bool EvaluateCondition(const std::string& cond, int trackIdx) {
    if (cond.empty() || cond == "-" || cond == "") return true;

    int bar = g_trackBarCount[trackIdx]; // 0-indexed count of bar loops

    if (cond == "AFT") {
        return bar > 0; // True on second loop and onward
    }

    // Dynamic extraction of "N:M" patterns (Numerator : Denominator)
    size_t colonPos = cond.find(':');
    if (colonPos != std::string::npos) {
        int n = std::atoi(cond.substr(0, colonPos).c_str());
        int m = std::atoi(cond.substr(colonPos + 1).c_str());
        if (m > 0) {
            // Numerator N is 1-indexed. Evaluate if the remainder matches
            return (bar % m) == (n - 1);
        }
    }

    return true;
}

// ==========================================================================
// REAL-TIME AUDIO DSP CALLBACK (24 PPQN Tick Clock with Synth & Sampler)
// ==========================================================================
void ma_audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    EnableFlushToZero();
    auto startTime = std::chrono::high_resolution_clock::now(); // Record start time
    float* pOutputF = (float*)pOutput;
    const float* pInputF = (const float*)pInput;
    ma_uint32 samplesProcessed = 0;

    // --- MASTER LIMITER STATE ---
    // Threshold sits where the old soft clipper did so the character is
    // unchanged, but the knee now drives a smoothed gain rather than the
    // sample values themselves.
    constexpr float kLimitThreshold = 0.85f;
    static float s_limiterGain = 1.0f;
    static double s_limiterCoefRate = 0.0;
    static float kLimitAttackStore = 0.0f;
    static float kLimitReleaseStore = 0.0f;
    if (s_limiterCoefRate != g_sampleRate) {
        s_limiterCoefRate = g_sampleRate;
        // One-pole coefficients for a 1 ms grab and an 80 ms recovery.
        kLimitAttackStore  = 1.0f - expf(-1.0f / (0.001f * (float)g_sampleRate));
        kLimitReleaseStore = 1.0f - expf(-1.0f / (0.080f * (float)g_sampleRate));
    }
    const float kLimitAttack  = kLimitAttackStore;
    const float kLimitRelease = kLimitReleaseStore;

    float blockMasterPeak = 0.0f;
    float blockMinGain = 1.0f;
    int blockActiveVoices = 0;
    double accVoiceSec = 0.0;
    double accTapeSec = 0.0;
    double accFxSec = 0.0;

    static int s_finalMem[8]  = {50};
    static int s_finalHds[8]  = {1};
    static int s_finalSpr[8]  = {0};
    static int s_finalSpd[8]  = {74};
    static int s_finalTet[8]  = {99};
    static int s_finalDrf[8]  = {10};
    static int s_finalDrt[8]  = {20};
    static int s_finalFdb[8]  = {30};
    static int s_finalFsp[8]  = {10};
    static int s_finalFsc[8]  = {0};
    static int s_finalFrz[8]  = {0};
    static int s_finalSmr[8]  = {0};
    static int s_finalSms[8]  = {40};
    static int s_finalMix[8]  = {0};

    double tickLengthSeconds = 2.5 / tempo;
    ma_uint32 samplesPerTick = (ma_uint32)(tickLengthSeconds * g_sampleRate);
    ma_uint32 samplesPerStep = samplesPerTick * 6;

    static bool lastPlayingState = false;
    if (isPlaying != lastPlayingState) {
        if (!isPlaying) {
            for (int t = 0; t < 8; ++t) {
                for (int v = 0; v < 4; ++v) {
                    g_trackVoices[t][v].Release();
                    g_samplerVoices[t][v].Release();
                }
                g_activeRetrigs[t].remainingTriggers = 0;
            }
        }
        lastPlayingState = isPlaying;
    }

    while (samplesProcessed < frameCount) {
        ma_uint32 chunkSize = (frameCount - samplesProcessed < 64) ? (frameCount - samplesProcessed) : 64;

        // Live MIDI (Rt + UI) must mutate voices only here, before LFO/DSP work.
        DrainAllMidiLiveQueues();

        // UI requested leave-external: flush ticks and re-arm internal accumulator.
        if (g_externalClockDisablePending.exchange(false, std::memory_order_acq_rel)) {
            g_useExternalMidiClock.store(false, std::memory_order_relaxed);
            g_externalMidiTicksQueued.store(0, std::memory_order_relaxed);
            g_tickSampleAccumulator = 0;
        }

        // Soft recovery: if the F8 queue ballooned, keep at most one beat ahead.
        if (g_useExternalMidiClock.load(std::memory_order_relaxed)) {
            int queued = g_externalMidiTicksQueued.load(std::memory_order_relaxed);
            if (queued > 24) {
                g_externalMidiTicksQueued.store(24, std::memory_order_relaxed);
                g_tickSampleAccumulator = 0;
            }
            // Silence fallback: no F8 for >500ms while playing → internal clock.
            if (isPlaying) {
                const uint64_t last = g_lastExternalClockMs.load(std::memory_order_relaxed);
                const uint64_t now = SteadyNowMs();
                if (last != 0 && now > last && (now - last) > 500ull) {
                    g_useExternalMidiClock.store(false, std::memory_order_relaxed);
                    g_externalMidiTicksQueued.store(0, std::memory_order_relaxed);
                    g_tickSampleAccumulator = 0;
                }
            }
        }

        UpdateGlobalLFOs();
        AccumulateTrackVoiceMods();

        // Master FX mix modulation reads only g_globalLFOValues, which updates
        // once per chunk. Computing it per sample was 48 Track-straddling
        // iterations for a value that cannot change inside the chunk.
        float chunkRevMixMod = 0.0f;
        float chunkDelMixMod = 0.0f;
        float chunkSatMixMod = 0.0f;
        float chunkPanMixMod = 0.0f;
        for (int t = 0; t < 8; ++t) {
            for (int s = 0; s < 3; ++s) {
                if (tracks[t].lfo1Slots[s].destType == 2) {
                    float modVal = g_globalLFOValues[t][0] * (tracks[t].lfo1Slots[s].depth / 99.0f);
                    if (tracks[t].lfo1Slots[s].destParam == DEST_REV_MIX)       chunkRevMixMod += modVal;
                    else if (tracks[t].lfo1Slots[s].destParam == DEST_DEL_MIX)  chunkDelMixMod += modVal;
                    else if (tracks[t].lfo1Slots[s].destParam == DEST_SAT_MIX)  chunkSatMixMod += modVal;
                    else if (tracks[t].lfo1Slots[s].destParam == DEST_PAN_MIX)  chunkPanMixMod += modVal;
                }
                if (tracks[t].lfo2Slots[s].destType == 2) {
                    float modVal = g_globalLFOValues[t][1] * (tracks[t].lfo2Slots[s].depth / 99.0f);
                    if (tracks[t].lfo2Slots[s].destParam == DEST_REV_MIX)       chunkRevMixMod += modVal;
                    else if (tracks[t].lfo2Slots[s].destParam == DEST_DEL_MIX)  chunkDelMixMod += modVal;
                    else if (tracks[t].lfo2Slots[s].destParam == DEST_SAT_MIX)  chunkSatMixMod += modVal;
                    else if (tracks[t].lfo2Slots[s].destParam == DEST_PAN_MIX)  chunkPanMixMod += modVal;
                }
            }
        }
        
        for (int t = 0; t < 8; ++t) {
            const Track& trk = tracks[t];
            int currentStepIdx = (trk.localTick / 6) % trk.stepLength;
            if (currentStepIdx < 0) currentStepIdx = 0;
            const Step& step = trk.steps[currentStepIdx];

            float modTapeMem = 0.0f;
            float modTapeHds = 0.0f;
            float modTapeSpr = 0.0f;
            float modTapeSpd = 0.0f;
            float modTapeTet = 0.0f;
            float modTapeDrf = 0.0f;
            float modTapeDrt = 0.0f;
            float modTapeFdb = 0.0f;
            float modTapeFsp = 0.0f;
            float modTapeFsc = 0.0f;
            float modTapeFrz = 0.0f;
            float modTapeSmr = 0.0f;
            float modTapeSms = 0.0f;
            float modTapeMix = 0.0f;

            for (int srcTrkIdx = 0; srcTrkIdx < 8; ++srcTrkIdx) {
                const Track& srcTrk = tracks[srcTrkIdx];
                for (int s = 0; s < 3; ++s) {
                    {
                        const ModSlot& m = srcTrk.lfo1Slots[s];
                        if (m.destType == 1 && m.destTrack == t) {
                            float modVal = g_globalLFOValues[srcTrkIdx][0] * (m.depth / 99.0f);
                            if (m.destParam == DEST_TAPE_MEM)        modTapeMem += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_HDS)   modTapeHds += modVal * 3.0f;
                            else if (m.destParam == DEST_TAPE_SPR)   modTapeSpr += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_SPD)   modTapeSpd += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_TET)   modTapeTet += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_DRF)   modTapeDrf += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_DRT)   modTapeDrt += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_FDB)   modTapeFdb += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_FSP)   modTapeFsp += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_FSC)   modTapeFsc += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_FRZ)   modTapeFrz += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_SMR)   modTapeSmr += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_SMS)   modTapeSms += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_MIX)   modTapeMix += modVal * 99.0f;
                        }
                    }
                    {
                        const ModSlot& m = srcTrk.lfo2Slots[s];
                        if (m.destType == 1 && m.destTrack == t) {
                            float modVal = g_globalLFOValues[srcTrkIdx][1] * (m.depth / 99.0f);
                            if (m.destParam == DEST_TAPE_MEM)        modTapeMem += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_HDS)   modTapeHds += modVal * 3.0f;
                            else if (m.destParam == DEST_TAPE_SPR)   modTapeSpr += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_SPD)   modTapeSpd += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_TET)   modTapeTet += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_DRF)   modTapeDrf += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_DRT)   modTapeDrt += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_FDB)   modTapeFdb += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_FSP)   modTapeFsp += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_FSC)   modTapeFsc += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_FRZ)   modTapeFrz += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_SMR)   modTapeSmr += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_SMS)   modTapeSms += modVal * 99.0f;
                            else if (m.destParam == DEST_TAPE_MIX)   modTapeMix += modVal * 99.0f;
                        }
                    }
                }
            }

            s_finalMem[t]  = std::clamp((int)(GetParam(step.params.tapeMemory,     tracks[t].tapeMemory)    + modTapeMem), 0, 99);
            s_finalHds[t]  = std::clamp((int)(GetParam(step.params.tapeHeads,      tracks[t].tapeHeads)     + modTapeHds), 1, 4);
            s_finalSpr[t]  = std::clamp((int)(GetParam(step.params.tapeSpread,     tracks[t].tapeSpread)    + modTapeSpr), 0, 99);
            s_finalSpd[t]  = std::clamp((int)(GetParam(step.params.tapeSpeed,      tracks[t].tapeSpeed)     + modTapeSpd), 0, 99);
            s_finalTet[t]  = std::clamp((int)(GetParam(step.params.tapeTether,     tracks[t].tapeTether)    + modTapeTet), 0, 99);
            s_finalDrf[t]  = std::clamp((int)(GetParam(step.params.tapeDrift,      tracks[t].tapeDrift)     + modTapeDrf), 0, 99);
            s_finalDrt[t]  = std::clamp((int)(GetParam(step.params.tapeDriftRate,  tracks[t].tapeDriftRate) + modTapeDrt), 0, 99);
            s_finalFdb[t]  = std::clamp((int)(GetParam(step.params.tapeFeedback,   tracks[t].tapeFeedback)  + modTapeFdb), 0, 99);
            s_finalFsp[t]  = std::clamp((int)(GetParam(step.params.tapeFbSpread,   tracks[t].tapeFbSpread)  + modTapeFsp), 0, 99);
            s_finalFsc[t]  = std::clamp((int)(GetParam(step.params.tapeFbSource,   tracks[t].tapeFbSource)  + modTapeFsc), 0, 99);
            s_finalFrz[t]  = std::clamp((int)(GetParam(step.params.tapeFreeze,     tracks[t].tapeFreeze)    + modTapeFrz), 0, 99);
            s_finalSmr[t]  = std::clamp((int)(GetParam(step.params.tapeSmearRate,  tracks[t].tapeSmearRate) + modTapeSmr), 0, 99);
            s_finalSms[t]  = std::clamp((int)(GetParam(step.params.tapeSmearSize,  tracks[t].tapeSmearSize) + modTapeSms), 0, 99);
            s_finalMix[t]  = std::clamp((int)(GetParam(step.params.tapeMix,        tracks[t].tapeMix)       + modTapeMix), 0, 99);
        }

        // --- CHUNK-RATE MASTER PERFORMANCE FILTER UPDATE ---
        static int lastPerfCutoff = -1;
        static int lastPerfRes = -1;
        static int lastPerfType = -1;

        if (perfFilterCutoff != lastPerfCutoff || perfFilterResonance != lastPerfRes || perfFilterType != lastPerfType) {
            lastPerfCutoff = perfFilterCutoff;
            lastPerfRes = perfFilterResonance;
            lastPerfType = perfFilterType;

            float normCut = perfFilterCutoff / 99.0f;
            float finalCutoffHz = 15.0f + (normCut * normCut * normCut * normCut) * 15985.0f;
            float resNorm = perfFilterResonance / 99.0f;

            g_masterPerfFilterL.calculateCoefficients(finalCutoffHz, resNorm, (float)g_sampleRate);
            g_masterPerfFilterR.calculateCoefficients(finalCutoffHz, resNorm, (float)g_sampleRate);
        }

        for (ma_uint32 i = 0; i < chunkSize; ++i) {
            ma_uint32 outIdx = samplesProcessed + i;

            if (g_externalMidiStartTriggered.exchange(false, std::memory_order_acq_rel)) {
                g_currentTick = -1;
                isPlaying = true;
            }
            if (g_externalMidiStopTriggered.exchange(false, std::memory_order_acq_rel)) {
                isPlaying = false;
            }

            if (isPlaying) {
                bool tickTriggered = false;
                int masterTicksLimit = (masterLength == 0) ? 192 : (masterLength * 6);

                if (g_useExternalMidiClock.load(std::memory_order_relaxed)) {
                    int queued = g_externalMidiTicksQueued.load(std::memory_order_relaxed);
                    if (queued > 0) {
                        g_externalMidiTicksQueued.fetch_sub(1, std::memory_order_relaxed);
                        if (g_currentTick == -1) {
                            g_currentTick = 0;
                            g_tickSampleAccumulator = 0;
                        } else {
                            g_currentTick = (g_currentTick + 1) % masterTicksLimit;
                        }
                        tickTriggered = true;
                    }
                } else {
                    if (g_currentTick == -1) {
                        g_currentTick = 0;
                        g_tickSampleAccumulator = 0;
                        tickTriggered = true;
                    } else {
                        g_tickSampleAccumulator++;
                        if (g_tickSampleAccumulator >= samplesPerTick) {
                            g_tickSampleAccumulator = 0;
                            g_currentTick = (g_currentTick + 1) % masterTicksLimit;
                            tickTriggered = true;
                        }
                    }
                }

                if (tickTriggered) {
                    // A knob turn should be audible now rather than at the next
                    // bar, so a dirty flag re-renders every track on this tick.
                    bool chaosDirty = g_chaosDirty.exchange(false);

                    for (int t = 0; t < 8; ++t) {
                        int nextTick = (tracks[t].localTick + 1) % (tracks[t].stepLength * 6);
                        if (nextTick == 0 && tracks[t].localTick >= 0) {
                            g_trackBarCount[t]++;
                            g_chaosBar[t]++;
                            // Rendering exactly on the wrap means no step is
                            // rewritten while the scan below is mid-bar on it.
                            ChaosRenderTrack(t, g_chaosBar[t]);
                        } else if (chaosDirty) {
                            ChaosRenderTrack(t, g_chaosBar[t]);
                        }
                        tracks[t].localTick = nextTick;
                    }

                    if (g_currentTick == 0) {
                        if (queuedPattern != -1) {
                            SwitchPattern(queuedPattern);
                            queuedPattern = -1;
                            for (int t = 0; t < 8; ++t) {
                                tracks[t].localTick = -1;
                                g_trackBarCount[t] = 0;
                                g_chaosBar[t] = 0; // a new pattern starts its own evolution
                            }
                        } else if (masterLength > 0) {
                            for (int t = 0; t < 8; ++t) {
                                tracks[t].localTick = -1;
                                g_trackBarCount[t] = 0;
                                // The master loop force-restarts every track, so
                                // it is a bar boundary too. Counting it here is
                                // what puts a fresh variation at the top of the
                                // pattern rather than only halfway through it.
                                g_chaosBar[t]++;
                                ChaosRenderTrack(t, g_chaosBar[t]);
                            }
                        }
                    }
                }

                playhead = (tracks[selectedTrack].localTick / 6) % tracks[selectedTrack].stepLength;

                if (tickTriggered) {
                    for (int t = 0; t < 8; ++t) {
                        if (tracks[t].muted) continue;

                        for (int s = 0; s < tracks[t].stepLength; ++s) {
                            const Step& step = tracks[t].steps[s];
                            int triggerTick = (s * 6 + step.microtiming + ((s % 2 == 1) ? (tracks[t].swing * 5) / 99 : 0));
                            int localLengthTicks = tracks[t].stepLength * 6;
                            
                            triggerTick = triggerTick % localLengthTicks;
                            if (triggerTick < 0) triggerTick += localLengthTicks;

                            if (triggerTick == tracks[t].localTick) {
                                g_activeRetrigs[t].remainingTriggers = 0;
                                if (step.velocity > 0 && step.note >= 0) { // Changed Note String check to quick Integer sentinel check
                                    int cycleLength = 1;
                                    for (int c = 0; c < 8; ++c) {
                                        if (step.condMask & (1 << (8 + c))) {
                                            cycleLength = c + 1;
                                            break;
                                        }
                                    }
                                    int currentCycleIdx = g_trackBarCount[t] % cycleLength;
                                    bool maskActive = (step.condMask & (1 << currentCycleIdx)) != 0;

                                    if (maskActive) {
                                        int midiNoteRoot = step.note; // Directly read the raw integer (NoteToMidi call removed!)
                                        // Tracks pinned to their own key ignore the
                                        // global transpose, so drums stay put while
                                        // the melodic parts move.
                                        if (midiNoteRoot >= 0 && tracks[t].keyScope == 0 && g_globalTranspose != 0) {
                                            midiNoteRoot = std::clamp(midiNoteRoot + g_globalTranspose, 0, 127);
                                        }
                                        if (midiNoteRoot >= 0) {
                                            // Fixed stack buffer: no heap in audio callback
                                            int midiNotesToTrigger[4];
                                            int noteCount = 0;
                                            midiNotesToTrigger[noteCount++] = midiNoteRoot;

                                            if (step.chordType > 0) {
                                                // Indexed by chordType 1..6: maj, min, sus4, dom7, maj7, min7
                                                static const int kChordOffsets[7][3] = {
                                                    {0, 0, 0},
                                                    {4, 7, 0},
                                                    {3, 7, 0},
                                                    {5, 7, 0},
                                                    {4, 7, 10},
                                                    {4, 7, 11},
                                                    {3, 7, 10}
                                                };
                                                static const int kChordOffsetCounts[7] = {0, 2, 2, 2, 3, 3, 3};
                                                int ct = step.chordType;
                                                if (ct >= 1 && ct <= 6) {
                                                    for (int i = 0; i < kChordOffsetCounts[ct] && noteCount < 4; ++i) {
                                                        midiNotesToTrigger[noteCount++] = midiNoteRoot + kChordOffsets[ct][i];
                                                    }
                                                }
                                            } else {
                                                for (int k = 0; k < 3 && noteCount < 4; ++k) {
                                                    if (step.chordNotes[k] >= 0) { // Read standard chord integers directly
                                                        midiNotesToTrigger[noteCount++] = step.chordNotes[k];
                                                    }
                                                }
                                            }
                                            
                                            int depth = (step.params.pitchSweepDepth == -1) ? tracks[t].pitchSweepDepth : step.params.pitchSweepDepth;
                                            int time = (step.params.pitchSweepTime == -1) ? tracks[t].pitchSweepTime : step.params.pitchSweepTime;
                                            int glide = (step.params.glideTime == -1) ? tracks[t].glideTime : step.params.glideTime; // Resolve Glide Time

                                            int finalPolyMode = GetParam(step.params.polyMode, tracks[t].polyMode);
                                            if (finalPolyMode < 1) finalPolyMode = 1;
                                            if (finalPolyMode > 4) finalPolyMode = 4;

                                            int notesCount = std::min(noteCount, finalPolyMode);
                                            int samplerRetrigVoice = 0;

                                            for (int n = 0; n < notesCount; ++n) {
                                                int midiNote = midiNotesToTrigger[n];
                                                float freq = 440.0f * powf(2.0f, (midiNote - 69.0f) / 12.0f);

                                                if (tracks[t].engineType == ENGINE_SYNTH) {
                                                    int targetIdx = StealSynthVoice(t, finalPolyMode);

                                                    if (g_trackVoices[t][targetIdx].active) {
                                                        g_trackVoices[t][targetIdx].Choke();
                                                    }
                                                    g_trackVoices[t][targetIdx].Trigger(freq, depth, time, step.velocity, true, step.noteLength, glide); // Pass Glide Time
                                                    g_trackVoiceIndex[t] = (targetIdx + 1) % finalPolyMode;
                                                } else if (tracks[t].engineType == ENGINE_SAMPLER) {
                                                    int slot = GetParam(step.params.sampleSlot, tracks[t].sampleSlot);
                                                    const int16_t* buffer = g_samplePool[slot].pcmData.data();
                                                    uint32_t length = (uint32_t)g_samplePool[slot].pcmData.size();
                                                    int sliceIdx = SamplerSliceForNote(t, midiNote, &step.params);
                                                    float noteOffset = (sliceIdx >= 0) ? 0.0f : (float)(midiNote - 60);

                                                    int targetIdx = StealSamplerVoice(t, finalPolyMode);

                                                    if (g_samplerVoices[t][targetIdx].active) {
                                                        g_samplerVoices[t][targetIdx].Choke();
                                                    }
                                                    g_samplerVoices[t][targetIdx].Trigger(buffer, length, noteOffset, (float)tracks[t].fine2, depth, time, step.velocity, true, step.noteLength, sliceIdx);
                                                    g_samplerVoiceIndex[t] = (targetIdx + 1) % finalPolyMode;
                                                    if (n == 0) samplerRetrigVoice = targetIdx;
                                                }
                                            }

                                            if (step.retrigger > 1) {
                                                ActiveRetrig& ar = g_activeRetrigs[t];
                                                ar.midiNote = midiNoteRoot;
                                                ar.remainingTriggers = step.retrigger - 1;
                                                ar.sampleInterval = samplesPerStep / step.retrigger;
                                                ar.sampleCounter = 0;
                                                ar.velocity = step.velocity;
                                                ar.voiceIndex = samplerRetrigVoice;
                                                ar.polyMode = finalPolyMode;
                                                ar.samplesLeftInStep = samplesPerStep;
                                            } else {
                                                g_activeRetrigs[t].remainingTriggers = 0;
                                            }
                                        }
                                    }
                                } else if (step.velocity == 0 && step.note >= 0) { // Changed String check to Integer sentinel check
                                    if (tracks[t].engineType == ENGINE_SYNTH) {
                                        for (int v = 0; v < 4; ++v) {
                                            if (g_trackVoices[t][v].triggeredBySequencer) {
                                                g_trackVoices[t][v].Release();
                                            }
                                        }
                                    } else if (tracks[t].engineType == ENGINE_SAMPLER) {
                                        for (int v = 0; v < 4; ++v) {
                                            if (g_samplerVoices[t][v].triggeredBySequencer) {
                                                g_samplerVoices[t][v].Release();
                                            }
                                        }
                                    }
                                    g_activeRetrigs[t].remainingTriggers = 0;
                                }
                            }
                        }
                    }
                }

                for (int t = 0; t < 8; ++t) {
                    ActiveRetrig& ar = g_activeRetrigs[t];
                    if (ar.remainingTriggers > 0 && !tracks[t].muted) {
                        if (ar.samplesLeftInStep > 0)
                            ar.samplesLeftInStep--;
                        else
                            ar.remainingTriggers = 0;

                        ar.sampleCounter++;
                        if (ar.remainingTriggers > 0 && ar.sampleCounter >= ar.sampleInterval) {
                            ar.sampleCounter = 0;
                            ar.remainingTriggers--;

                            if (ar.midiNote >= 0) {
                                float freq = 440.0f * powf(2.0f, (ar.midiNote - 69.0f) / 12.0f);
                                int currentStepIdx = (tracks[t].localTick / 6) % tracks[t].stepLength;
                                const Step& step = tracks[t].steps[currentStepIdx];
                                int depth = (step.params.pitchSweepDepth == -1) ? tracks[t].pitchSweepDepth : step.params.pitchSweepDepth;
                                int time = (step.params.pitchSweepTime == -1) ? tracks[t].pitchSweepTime : step.params.pitchSweepTime;
                                int glide = (step.params.glideTime == -1) ? tracks[t].glideTime : step.params.glideTime; // Resolve Glide Time

                                int retrigPoly = ar.polyMode;
                                if (retrigPoly < 1) retrigPoly = 1;
                                if (retrigPoly > 4) retrigPoly = 4;

                                if (tracks[t].engineType == ENGINE_SYNTH) {
                                    int targetIdx = StealSynthVoice(t, retrigPoly);
                                    g_trackVoices[t][targetIdx].Release();
                                    g_trackVoices[t][targetIdx].Trigger(freq, depth, time, ar.velocity, true, 0, glide);
                                    g_trackVoiceIndex[t] = (targetIdx + 1) % retrigPoly;
                                } else if (tracks[t].engineType == ENGINE_SAMPLER) {
                                    int slot = (step.params.sampleSlot == -1) ? tracks[t].sampleSlot : step.params.sampleSlot;
                                    const int16_t* buffer = g_samplePool[slot].pcmData.data();
                                    uint32_t length = (uint32_t)g_samplePool[slot].pcmData.size();
                                    int sliceIdx = SamplerSliceForNote(t, ar.midiNote, &step.params);
                                    float noteOffset = (sliceIdx >= 0) ? 0.0f : (float)(ar.midiNote - 60);

                                    int targetIdx = ar.voiceIndex;
                                    if (targetIdx < 0) targetIdx = 0;
                                    if (targetIdx > 3) targetIdx = 3;

                                    g_samplerVoices[t][targetIdx].Trigger(buffer, length, noteOffset, (float)tracks[t].fine2, depth, time, ar.velocity, true, 0, sliceIdx);
                                    g_samplerVoices[t][targetIdx].gateTimerSamples = ar.samplesLeftInStep;
                                    g_samplerVoices[t][targetIdx].useGateTimer = true;
                                }
                            }
                        }
                    }
                }
            } else {
                g_tickSampleAccumulator = 0;
                g_currentTick = -1;
                for (int t = 0; t < 8; ++t) {
                    tracks[t].localTick = -1;
                    g_trackBarCount[t] = 0;
                    g_chaosBar[t] = 0;
                }
            }

            float masterDryMono = 0.0f;
            float delaySendBusMono = 0.0f;
            float satSendBusMono = 0.0f;
            float reverbSendBusMono = 0.0f;
            float autoPanSendBusMono = 0.0f;

            int soundingTracks = 0;
            int activeVoices = 0;
            float trackDry[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

            auto tVoice0 = std::chrono::high_resolution_clock::now();
            for (int t = 0; t < 8; ++t) {
                if (tracks[t].muted) {
                    for (int v = 0; v < 4; ++v) {
                        g_trackVoices[t][v].Release();
                        g_samplerVoices[t][v].Release();
                    }
                    continue;
                }

                if (tracks[t].engineType == ENGINE_USB) {
                    float inL = 0.0f, inR = 0.0f;
                    if (pInputF && g_usbCaptureLive) {
                        inL = pInputF[2 * outIdx];
                        inR = pInputF[2 * outIdx + 1];
                    }
                    trackDry[t] = UsbProcess(t, inL, inR);
                    soundingTracks++;
                    continue;
                }

                float trackSampleSum = 0.0f;
                int trackVoicesSounding = 0;
                for (int v = 0; v < 4; ++v) {
                    if (tracks[t].engineType == ENGINE_SYNTH) {
                        SynthVoice& vo = g_trackVoices[t][v];
                        bool sounding = vo.choking
                            || vo.stage1 != SynthVoice::ENV1_IDLE
                            || vo.stage2 != SynthVoice::ENV2_IDLE
                            || vo.noiseStage != SynthVoice::NOISE_IDLE
                            || vo.filterStage != SynthVoice::FLT_IDLE;
                        if (!sounding) continue;
                        if (vo.active && vo.stage1 != SynthVoice::ENV1_IDLE) trackVoicesSounding++;
                        trackSampleSum += vo.Process(t);
                    } else if (tracks[t].engineType == ENGINE_SAMPLER) {
                        SamplerVoice& vo = g_samplerVoices[t][v];
                        if (!vo.active && !vo.choking) continue;
                        if (vo.stage == SamplerVoice::ENV_IDLE && !vo.choking) continue;
                        if (vo.active && vo.stage != SamplerVoice::ENV_IDLE) trackVoicesSounding++;
                        trackSampleSum += vo.Process(t);
                    }
                }
                activeVoices += trackVoicesSounding;
                if (trackVoicesSounding > 0) soundingTracks++;
                trackDry[t] = trackSampleSum * 0.25f;
            }
            auto tVoice1 = std::chrono::high_resolution_clock::now();
            accVoiceSec += std::chrono::duration<double>(tVoice1 - tVoice0).count();

            auto tTape0 = std::chrono::high_resolution_clock::now();
            for (int t = 0; t < 8; ++t) {
                if (tracks[t].muted) continue;

                int currentStepIdx = (tracks[t].localTick / 6) % tracks[t].stepLength;
                if (currentStepIdx < 0) currentStepIdx = 0;
                const Step& step = tracks[t].steps[currentStepIdx];

                float processedSum = trackDry[t];
                if (s_finalMix[t] > 0) {
                    processedSum = g_trackTapeFX[t].process(
                                trackDry[t],
                                s_finalMem[t], s_finalHds[t], s_finalSpr[t], s_finalSpd[t], s_finalTet[t],
                                s_finalDrf[t], s_finalDrt[t], s_finalFdb[t], s_finalFsp[t], s_finalFsc[t],
                                s_finalFrz[t], s_finalSmr[t], s_finalSms[t], s_finalMix[t],
                                g_sampleRate
                            );
                }

                masterDryMono += processedSum;

                int delSendVal = GetParam(step.params.delaySend, tracks[t].delaySend);
                float delSendNorm = (float)delSendVal / 99.0f;
                delaySendBusMono += processedSum * delSendNorm;

                int satSendVal = GetParam(step.params.saturationSend, tracks[t].saturationSend);
                float satSendNorm = (float)satSendVal / 99.0f;
                satSendBusMono += processedSum * satSendNorm;

                int revSendVal = GetParam(step.params.reverbSend, tracks[t].reverbSend);
                float revSendNorm = (float)revSendVal / 99.0f;
                reverbSendBusMono += processedSum * revSendNorm;

                int panSendVal = GetParam(step.params.autoPanSend, tracks[t].autoPanSend);
                float panSendNorm = (float)panSendVal / 99.0f;
                autoPanSendBusMono += processedSum * panSendNorm;
            }
            auto tTape1 = std::chrono::high_resolution_clock::now();
            accTapeSec += std::chrono::duration<double>(tTape1 - tTape0).count();

            // --- MASTER HEADROOM ---
                        // A fixed 0.35 was fine for one track and hopeless for eight:
                        // the sum grew with track count until the limiter was pinned
                        // permanently, which is exactly the state chaos puts you in.
                        // Scaling by the square root of the number of tracks actually
                        // sounding keeps the perceived level roughly constant as
                        // density rises, and leaves a solo track exactly where it was.
                        constexpr float kMasterScaleBase = 0.35f;
                        static float s_masterScale = kMasterScaleBase;
                        float targetMasterScale = kMasterScaleBase / sqrtf((float)std::max(1, soundingTracks));
                        // Roughly 100 ms, slow enough that the level change is not
                        // heard as pumping when a part drops in or out.
                        s_masterScale += 0.00023f * (targetMasterScale - s_masterScale);
                        const float kMasterScale = s_masterScale;

                        float masterL = masterDryMono * kMasterScale;
                        float masterR = masterDryMono * kMasterScale;

                        float delaySendL = delaySendBusMono * kMasterScale;
                        float delaySendR = delaySendBusMono * kMasterScale;

                        float satSendL = satSendBusMono * kMasterScale;
                        float satSendR = satSendBusMono * kMasterScale;

                        float reverbSendL = reverbSendBusMono * kMasterScale;
                        float reverbSendR = reverbSendBusMono * kMasterScale;

                        float panSendL = autoPanSendBusMono * kMasterScale;
                        float panSendR = autoPanSendBusMono * kMasterScale;

            auto tFx0 = std::chrono::high_resolution_clock::now();
            // These four run as sends: mixNorm is deliberately 1.0 so process()
            // returns pure wet, which is then scaled by the global mix below.
            // That means their internal "mix <= 0" early-outs can never fire, so
            // all four used to run full DSP even with every mix at zero. The
            // decision has to be made here instead.
            float globalRevMix = std::clamp(((float)globalFX.reverbMix / 99.0f) + chunkRevMixMod, 0.0f, 1.0f);
            float reverbWetL = 0.0f;
            float reverbWetR = 0.0f;
            if (globalRevMix > 0.0f) {
                g_masterReverb.process(
                    reverbSendL, reverbSendR, reverbWetL, reverbWetR,
                    (float)globalFX.reverbDecay / 99.0f,
                    (float)globalFX.reverbSize / 99.0f,
                    (float)globalFX.reverbPredelay / 99.0f,
                    1.0f,
                    (float)g_sampleRate
                );
                reverbWetL *= globalRevMix;
                reverbWetR *= globalRevMix;
            }

            float globalDelayMixNorm = std::clamp(((float)globalFX.delayMix / 99.0f) + chunkDelMixMod, 0.0f, 1.0f);
            float delayWetL = 0.0f;
            float delayWetR = 0.0f;
            if (globalDelayMixNorm > 0.0f) {
                g_masterDelay.process(
                    delaySendL, delaySendR, delayWetL, delayWetR,
                    (float)globalFX.delayTime / 99.0f,
                    (float)globalFX.delayFeedback / 99.0f,
                    globalFX.delayPingPong,
                    1.0f,
                    (float)g_sampleRate
                );
                delayWetL *= globalDelayMixNorm;
                delayWetR *= globalDelayMixNorm;
            }

            float globalSatMixNorm = std::clamp(((float)globalFX.satMix / 99.0f) + chunkSatMixMod, 0.0f, 1.0f);
            float satWetL = 0.0f;
            float satWetR = 0.0f;
            if (globalSatMixNorm > 0.0f) {
                g_masterCompressor.process(
                    satSendL, satSendR, satWetL, satWetR,
                    (float)globalFX.satLevel / 99.0f,
                    (float)globalFX.satSymmetry / 99.0f,
                    (float)globalFX.satOverdrive / 99.0f,
                    1.0f,
                    (float)g_sampleRate
                );
                satWetL *= globalSatMixNorm;
                satWetR *= globalSatMixNorm;
            }

            float globalPanMix = std::clamp(((float)globalFX.autoPanMix / 99.0f) + chunkPanMixMod, 0.0f, 1.0f);
            float panWetL = 0.0f;
            float panWetR = 0.0f;
            if (globalPanMix > 0.0f) {
                g_masterTornado.process(
                    panSendL, panSendR, panWetL, panWetR,
                    (float)globalFX.autoPanTime / 99.0f,
                    (float)globalFX.autoPanFeedback / 99.0f,
                    (float)globalFX.autoPanWidth / 99.0f,
                    1.0f,
                    (float)g_sampleRate
                );
                panWetL *= globalPanMix;
                panWetR *= globalPanMix;
            }
            auto tFx1 = std::chrono::high_resolution_clock::now();
            accFxSec += std::chrono::duration<double>(tFx1 - tFx0).count();

            // --- MASTER MIX SUM ---
            float finalL = masterL + reverbWetL + delayWetL + satWetL + panWetL;
            float finalR = masterR + reverbWetR + delayWetR + satWetR + panWetR;

            float outL = finalL;
            float outR = finalR;

            // --- MASTER BEAT REPEAT (MOMENTARY LOOPING) ---
            if (activeStutterKey >= 0 && activeStutterKey <= 7) {
                double beatLenSec = 60.0 / tempo;
                uint32_t samplesPerBeat = (uint32_t)(beatLenSec * g_sampleRate);
                uint32_t loopLen = (uint32_t)(stutterDivFactors[activeStutterKey] * samplesPerBeat);
                
                if (loopLen < 32) loopLen = 32;
                if (loopLen >= STUTTER_BUF_SIZE) loopLen = STUTTER_BUF_SIZE - 1;

                if (!g_lastStutterActive || activeStutterKey != g_lastStutterKey) {
                    if (!g_lastStutterActive) {
                        g_stutterCapturePtr = (g_stutterWritePtr >= loopLen) ?
                                              (g_stutterWritePtr - loopLen) :
                                              (g_stutterWritePtr + STUTTER_BUF_SIZE - loopLen);
                    }
                    g_stutterPlayhead = 0.0f;
                    g_lastStutterActive = true;
                    g_lastStutterKey = activeStutterKey;
                }

                uint32_t readIdx = (g_stutterCapturePtr + (uint32_t)g_stutterPlayhead) % STUTTER_BUF_SIZE;
                outL = g_stutterBufferL[readIdx];
                outR = g_stutterBufferR[readIdx];

                g_stutterPlayhead += 1.0f;
                if (g_stutterPlayhead >= (float)loopLen) {
                    g_stutterPlayhead = 0.0f;
                }
            } else {
                g_stutterBufferL[g_stutterWritePtr] = finalL;
                g_stutterBufferR[g_stutterWritePtr] = finalR;
                g_stutterWritePtr = (g_stutterWritePtr + 1) % STUTTER_BUF_SIZE;
                
                g_lastStutterActive = false;
                g_lastStutterKey = -1;
            }

            // --- MASTER PERFORMANCE FILTER ---
            if (perfFilterCutoff < 99 || perfFilterType != 0) {
                outL = g_masterPerfFilterL.process(outL, perfFilterType);
                outR = g_masterPerfFilterR.process(outR, perfFilterType);
            }

            // --- MASTER LIMITER ---
            // The old version applied the soft knee to each sample directly. A
            // memoryless waveshaper is a distortion unit: the curve reshapes the
            // waveform itself and generates harmonics that move with the signal,
            // which is the grind heard as soon as the mix crosses the threshold.
            // Here the same knee only decides how much gain to ask for; that gain
            // is then smoothed with a fast attack and slow release and applied to
            // both channels, so loud passages are turned down instead of reshaped.
            float peak = std::max(std::abs(outL), std::abs(outR));
            if (peak > blockMasterPeak) blockMasterPeak = peak;
            if (activeVoices > blockActiveVoices) blockActiveVoices = activeVoices;

            float targetGain = 1.0f;
            if (peak > kLimitThreshold) {
                float excess = peak - kLimitThreshold;
                float knee = kLimitThreshold + (1.0f - kLimitThreshold) * tanhf(excess / (1.0f - kLimitThreshold));
                targetGain = knee / peak;
            }

            // Clamp down immediately, recover gently, so the gain change itself
            // stays well below audio rate and does not become modulation.
            float coef = (targetGain < s_limiterGain) ? kLimitAttack : kLimitRelease;
            s_limiterGain += coef * (targetGain - s_limiterGain);

            outL *= s_limiterGain;
            outR *= s_limiterGain;

            if (s_limiterGain < blockMinGain) blockMinGain = s_limiterGain;

            // Backstop for transients faster than the attack time.
            pOutputF[2 * outIdx]     = std::clamp(outL, -1.0f, 1.0f);
            pOutputF[2 * outIdx + 1] = std::clamp(outR, -1.0f, 1.0f);

        } // End of i (chunkSize) loop

        samplesProcessed += chunkSize;
    } // End of while loop

    // --- LEVEL METERING ---
    // Peak falls back slowly so a transient stays readable on a 30 Hz display.
    if (blockMasterPeak > g_masterPeak) g_masterPeak = blockMasterPeak;
    else g_masterPeak *= 0.92f;

    float blockReduction = 1.0f - blockMinGain;
    if (blockReduction > g_limiterReduction) g_limiterReduction = blockReduction;
    else g_limiterReduction *= 0.92f;

    g_activeVoiceCount = blockActiveVoices;

    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();
    double expectedSec = (double)frameCount / g_sampleRate;
    float instantCpu = (float)((elapsedSec / expectedSec) * 100.0);

    // No clamp. The old meter capped at 100 and then smoothed over roughly
    // twenty callbacks, so an overrun could never reach the display, which is
    // why a broken-sounding mix still read as comfortable.
    static float s_smoothedCpu = 0.0f;
    s_smoothedCpu += 0.05f * (instantCpu - s_smoothedCpu);
    g_audioCpuLoad = s_smoothedCpu;

    static float s_smoothVoices = 0.0f;
    static float s_smoothTape = 0.0f;
    static float s_smoothFx = 0.0f;
    float invBudget = (expectedSec > 0.0) ? (float)(100.0 / expectedSec) : 0.0f;
    s_smoothVoices += 0.05f * ((float)accVoiceSec * invBudget - s_smoothVoices);
    s_smoothTape   += 0.05f * ((float)accTapeSec  * invBudget - s_smoothTape);
    s_smoothFx     += 0.05f * ((float)accFxSec    * invBudget - s_smoothFx);
    g_audioCpuVoices = s_smoothVoices;
    g_audioCpuTape   = s_smoothTape;
    g_audioCpuFx     = s_smoothFx;

    if (instantCpu >= 100.0f) g_audioDeadlineMisses++;

    // Peak-hold, decayed once a second so it tracks the recent worst case.
    static double s_peakWindowSec = 0.0;
    static float s_peakAccum = 0.0f;
    if (instantCpu > s_peakAccum) s_peakAccum = instantCpu;
    s_peakWindowSec += expectedSec;
    if (s_peakWindowSec >= 1.0) {
        g_audioCpuPeak = s_peakAccum;
        s_peakAccum = 0.0f;
        s_peakWindowSec = 0.0;
    } else if (s_peakAccum > g_audioCpuPeak) {
        g_audioCpuPeak = s_peakAccum;
    }
}
// ==========================================
// PUBLIC CONTROLLER INTERFACE
// ==========================================
static void RefreshUsbCaptureDevicesInternal() {
    g_usbCaptures.clear();
    if (!g_maContextInit) return;

    ma_device_info* pPlayback = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* pCapture = nullptr;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&g_maContext, &pPlayback, &playbackCount, &pCapture, &captureCount) != MA_SUCCESS)
        return;

    g_usbCaptures.reserve(captureCount);
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        UsbCaptureDev d;
        d.id = pCapture[i].id;
        d.name = pCapture[i].name;
        if (d.name.empty()) d.name = "CAPTURE " + std::to_string((int)i);
        g_usbCaptures.push_back(d);
    }
    if (g_usbCaptureRequested < 0) g_usbCaptureRequested = 0;
    if (!g_usbCaptures.empty() && g_usbCaptureRequested >= (int)g_usbCaptures.size())
        g_usbCaptureRequested = (int)g_usbCaptures.size() - 1;
}

static bool OpenAudioDeviceOnce(bool duplex, int captureIdx, ma_uint32 rate, ma_uint32 period,
                                bool alsaNoMMap, bool alsaNoAutoResample) {
    ma_device_config deviceConfig = ma_device_config_init(duplex ? ma_device_type_duplex : ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate        = rate;
    deviceConfig.dataCallback      = ma_audio_callback;
    deviceConfig.periodSizeInFrames = period;
    deviceConfig.periodSizeInMilliseconds = 0;
    deviceConfig.periods = duplex ? 4 : 3;
    deviceConfig.resampling.linear.lpfOrder = 8;
#if defined(__APPLE__)
    deviceConfig.coreaudio.allowNominalSampleRateChange = MA_TRUE;
#endif
#if defined(__linux__)
    deviceConfig.alsa.noMMap = alsaNoMMap ? MA_TRUE : MA_FALSE;
    deviceConfig.alsa.noAutoResample = alsaNoAutoResample ? MA_TRUE : MA_FALSE;
#endif

    if (duplex) {
        deviceConfig.capture.format   = ma_format_f32;
        deviceConfig.capture.channels = 2;
        if (captureIdx >= 0 && captureIdx < (int)g_usbCaptures.size()) {
            deviceConfig.capture.pDeviceID = &g_usbCaptures[captureIdx].id;
        }
    }

    ma_context* ctx = g_maContextInit ? &g_maContext : nullptr;
    if (ma_device_init(ctx, &deviceConfig, &g_audioDevice) != MA_SUCCESS)
        return false;
    if (ma_device_start(&g_audioDevice) != MA_SUCCESS) {
        ma_device_uninit(&g_audioDevice);
        return false;
    }
    g_audioInitialized = true;
    g_usbDuplexActive = duplex;
    g_usbCaptureLive = duplex;
    if (duplex) g_usbCaptureIndex = captureIdx;
    if (g_audioDevice.sampleRate != 0)
        g_sampleRate = (double)g_audioDevice.sampleRate;
    g_audioPeriodFrames = g_audioDevice.playback.internalPeriodSizeInFrames;
    if (g_audioPeriodFrames == 0)
        g_audioPeriodFrames = period;
    return true;
}

static bool OpenAudioDevice(bool duplex, int captureIdx) {
    if (duplex) {
        // Kept for Digitone USB revisit (Shift+T). Default playback does not
        // take this path.
        const ma_uint32 usbPeriod = 256;
#if defined(__linux__)
        if (OpenAudioDeviceOnce(true, captureIdx, 48000, usbPeriod, true, true))   return true;
        if (OpenAudioDeviceOnce(true, captureIdx, 48000, usbPeriod, true, false))  return true;
        if (OpenAudioDeviceOnce(true, captureIdx, 44100, usbPeriod, true, false))  return true;
#else
        if (OpenAudioDeviceOnce(true, captureIdx, 48000, usbPeriod, false, false)) return true;
        if (OpenAudioDeviceOnce(true, captureIdx, 44100, usbPeriod, false, false)) return true;
#endif
        return false;
    }

    // Playback: 44.1 kHz / 512 frames (~9% fewer samples than 48k).
    const ma_uint32 period = 512;
#if defined(__linux__)
    if (OpenAudioDeviceOnce(false, captureIdx, 44100, period, false, true))  return true;
    if (OpenAudioDeviceOnce(false, captureIdx, 44100, period, false, false)) return true;
    if (OpenAudioDeviceOnce(false, captureIdx, 48000, period, false, false)) return true;
#else
    if (OpenAudioDeviceOnce(false, captureIdx, 44100, period, false, false)) return true;
    if (OpenAudioDeviceOnce(false, captureIdx, 48000, period, false, false)) return true;
#endif
    return false;
}

static int g_audioPauseDepth = 0;

void PauseSeqAudio() {
    if (g_audioInitialized && g_audioPauseDepth++ == 0) {
        ma_device_stop(&g_audioDevice);
    }
}

void ResumeSeqAudio() {
    if (g_audioPauseDepth > 0 && --g_audioPauseDepth == 0 && g_audioInitialized) {
        ma_device_start(&g_audioDevice);
    }
}

int GetSeqAudioPauseDepth() {
    return g_audioPauseDepth;
}

static void CloseSeqAudioDevice() {
    if (g_audioInitialized) {
        ma_device_uninit(&g_audioDevice);
        g_audioInitialized = false;
    }
    g_usbDuplexActive = false;
    g_usbCaptureLive = false;
    // Device is gone — drop any outstanding pause depth so Resume cannot
    // start a destroyed handle after a USB reinit nested under a load.
    g_audioPauseDepth = 0;
}

static double s_fxInitedRate = 0.0;
static void EnsureFxRate() {
    if (s_fxInitedRate == g_sampleRate && s_fxInitedRate > 0.0) return;
    s_fxInitedRate = g_sampleRate;
    g_masterDelay.init((float)g_sampleRate);
    g_masterReverb.init((float)g_sampleRate);
    g_masterTornado.init((float)g_sampleRate);
}

static void ReinitAudioDevice(bool duplex, int captureIdx) {
    CloseSeqAudioDevice();
    if (OpenAudioDevice(duplex, captureIdx)) {
        EnsureFxRate();
        return;
    }
    // Duplex failed (missing capture, busy gadget): stay alive as playback-only.
    if (duplex && OpenAudioDevice(false, captureIdx))
        EnsureFxRate();
}

void InitAudioEngine() {
    InitVolumeCurve();
    InitSineLut();
    if (ma_context_init(nullptr, 0, nullptr, &g_maContext) == MA_SUCCESS)
        g_maContextInit = true;
    RefreshUsbCaptureDevicesInternal();

    OpenAudioDevice(false, g_usbCaptureRequested);
    EnsureFxRate();
}

void ShutdownAudioEngine() {
    CloseSeqAudioDevice();
    if (g_maContextInit) {
        ma_context_uninit(&g_maContext);
        g_maContextInit = false;
    }
    g_usbCaptures.clear();
}

void RefreshUsbCaptureDevices() {
    RefreshUsbCaptureDevicesInternal();
}

int GetUsbCaptureCount() {
    return (int)g_usbCaptures.size();
}

int GetUsbCaptureIndex() {
    return g_usbCaptureRequested;
}

void SetUsbCaptureIndex(int idx) {
    int count = (int)g_usbCaptures.size();
    if (count <= 0) {
        g_usbCaptureRequested = 0;
        return;
    }
    if (idx < 0) idx = count - 1;
    if (idx >= count) idx = 0;
    g_usbCaptureRequested = idx;
}

const char* GetUsbCaptureName(int idx) {
    if (idx < 0 || idx >= (int)g_usbCaptures.size())
        return "NO INPUT";
    return g_usbCaptures[idx].name.c_str();
}

int GetAudioSampleRate() {
    return (int)g_sampleRate;
}

int GetAudioPeriodFrames() {
    return (int)g_audioPeriodFrames;
}

void ToggleTrack8Usb() {
    Track& trk = tracks[7];
    if (trk.engineType == ENGINE_USB) {
        trk.engineType = (g_usbPreviousEngine == ENGINE_SAMPLER) ? ENGINE_SAMPLER : ENGINE_SYNTH;
        trk.algorithm = g_usbPreviousAlgo;
        if (trk.engineType == ENGINE_SAMPLER && trk.algorithm != ALGO_GRANULAR)
            trk.algorithm = ALGO_SAMPLE;
        if (trk.engineType == ENGINE_SYNTH && trk.algorithm != ALGO_CARRIER_MOD)
            trk.algorithm = ALGO_PARALLEL;
        menuFeedback = (trk.engineType == ENGINE_SAMPLER) ? "T8 SAMPLER" : "T8 SYNTH";
    } else {
        g_usbPreviousEngine = trk.engineType;
        g_usbPreviousAlgo = trk.algorithm;
        for (int v = 0; v < 4; ++v) {
            g_trackVoices[7][v].Release();
            g_samplerVoices[7][v].Release();
        }
        ResetUsbFilter();
        RefreshUsbCaptureDevicesInternal();
        trk.engineType = ENGINE_USB;
        menuFeedback = "T8 USB IN";
    }
}

void PollUsbAudioDevice() {
    // Never reinit while a load owns a PauseSeqAudio window.
    if (g_audioPauseDepth > 0) return;

    bool wantDuplex = (tracks[7].engineType == ENGINE_USB);
    int wantCap = g_usbCaptureRequested;

    if (wantDuplex && g_usbCaptures.empty()) {
        static auto s_lastEmptyRefresh = std::chrono::steady_clock::now() - std::chrono::seconds(3);
        auto now = std::chrono::steady_clock::now();
        if (now - s_lastEmptyRefresh > std::chrono::seconds(2)) {
            s_lastEmptyRefresh = now;
            RefreshUsbCaptureDevicesInternal();
        }
    }

    // Reinit only when the request changes. A failed duplex must not retry
    // every frame (that would stop/start the DAC in a loop).
    static bool s_appliedDuplex = false;
    static int s_appliedCap = -1;
    static int s_appliedListCount = -1;
    int listCount = (int)g_usbCaptures.size();
    bool listGrewWhileSilent = wantDuplex && !g_usbCaptureLive && listCount != s_appliedListCount;

    if (g_audioInitialized && wantDuplex == s_appliedDuplex
        && (!wantDuplex || wantCap == s_appliedCap)
        && !listGrewWhileSilent) {
        return;
    }

    ReinitAudioDevice(wantDuplex, wantCap);
    s_appliedDuplex = wantDuplex;
    s_appliedCap = wantCap;
    s_appliedListCount = listCount;
}

static void ClearSampleAsset(SampleAsset& out) {
    out.name = "Empty";
    out.pcmData.clear();
    out.visualPeaks = {};
}

bool LoadSampleIntoAsset(SampleAsset& out, const std::string& filename) {
    ClearSampleAsset(out);

    if (filename.empty()) {
        menuFeedback = "DIAG: filename empty";
        return false;
    }

    // Resolve file path BEFORE initializing the decoder to prevent double-init crash
    std::string filepath = "samples/" + filename + ".wav";
    menuFeedback = "DIAG: checking .wav file path";
    if (!std::filesystem::exists(filepath)) {
        filepath = "samples/" + filename;
        menuFeedback = "DIAG: checking raw file path";
        if (!std::filesystem::exists(filepath)) {
            menuFeedback = "DIAG: file not found on disk";
            return false;
        }
    }

    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 1, 32000);
    ma_decoder decoder;

    menuFeedback = "DIAG: calling init_file";
    ma_result result = ma_decoder_init_file(filepath.c_str(), &config, &decoder);
    if (result != MA_SUCCESS) {
        menuFeedback = "DIAG: init_file failed";
        return false;
    }

    menuFeedback = "DIAG: allocating tempBuffer";
    ma_uint64 maxFrames = 16 * 32000;
    std::vector<int16_t> tempBuffer;

    try {
        tempBuffer.resize(maxFrames, 0);
    } catch (...) {
        menuFeedback = "DIAG: bad_alloc";
        ma_decoder_uninit(&decoder);
        return false;
    }

    menuFeedback = "DIAG: calling read_pcm_frames";
    ma_uint64 framesRead = 0;
    ma_result readResult = ma_decoder_read_pcm_frames(&decoder, tempBuffer.data(), maxFrames, &framesRead);

    menuFeedback = "DIAG: calling uninit";
    ma_decoder_uninit(&decoder);

    if (readResult != MA_SUCCESS) {
        menuFeedback = "DIAG: read_pcm_frames failed";
        return false;
    }
    if (framesRead == 0) {
        menuFeedback = "DIAG: 0 frames read";
        return false;
    }

    menuFeedback = "DIAG: resizing buffer";
    if (framesRead > maxFrames) {
        framesRead = maxFrames;
    }
    tempBuffer.resize(framesRead);

    menuFeedback = "DIAG: normalizing volume";
    int16_t peak = 0;
    for (size_t i = 0; i < tempBuffer.size(); ++i) {
        int16_t absVal = std::abs(tempBuffer[i]);
        if (absVal > peak) {
            peak = absVal;
        }
    }
    if (peak > 0) {
        float scale = 32767.0f / peak;
        for (size_t i = 0; i < tempBuffer.size(); ++i) {
            float scaledVal = tempBuffer[i] * scale;
            if (scaledVal > 32767.0f)  scaledVal = 32767.0f;
            if (scaledVal < -32768.0f) scaledVal = -32768.0f;
            tempBuffer[i] = (int16_t)scaledVal;
        }
    }

    std::array<uint8_t, 97> peaks{};
    if (!tempBuffer.empty()) {
        for (int i = 0; i < 97; ++i) {
            size_t startFrame = (i * tempBuffer.size()) / 97;
            size_t endFrame = ((i + 1) * tempBuffer.size()) / 97;
            if (endFrame > tempBuffer.size()) endFrame = tempBuffer.size();
            if (startFrame >= endFrame) startFrame = (endFrame > 0) ? (endFrame - 1) : 0;

            int16_t binPeak = 0;
            for (size_t f = startFrame; f < endFrame; ++f) {
                int16_t absVal = std::abs(tempBuffer[f]);
                if (absVal > binPeak) binPeak = absVal;
            }
            peaks[i] = (uint8_t)((binPeak / 32768.0f) * 11.0f);
        }
    }

    out.name = filename;
    out.pcmData = std::move(tempBuffer);
    out.visualPeaks = peaks;
    return true;
}

bool LoadSampleToPool(int slotIdx, const std::string& filename, bool pauseAudio) {
    if (slotIdx < 0 || slotIdx >= 16) {
        menuFeedback = "DIAG: invalid slot index";
        return false;
    }

    // Nested-safe pause: menu sample loads pass pauseAudio=true; project/
    // pattern loads that already called PauseSeqAudio pass false.
    struct AudioPauseGuard {
        bool owns = false;
        explicit AudioPauseGuard(bool shouldPause) {
            if (shouldPause) {
                PauseSeqAudio();
                owns = true;
            }
        }
        ~AudioPauseGuard() {
            if (owns) ResumeSeqAudio();
        }
    } guard(pauseAudio);

    SampleAsset loaded;
    if (!LoadSampleIntoAsset(loaded, filename)) {
        return false;
    }

    g_samplePool[slotIdx] = std::move(loaded);
    menuFeedback = "SAMPLE CRUNCHED TO POOL!";
    return true;
}

static void TriggerVoiceLiveImpl(int trackIdx, int midiNote, int velocity) {
    if (trackIdx < 0 || trackIdx >= 8) return;
    if (midiNote < 0 || midiNote > 127) return;

    float freq = 440.0f * powf(2.0f, (midiNote - 69.0f) / 12.0f);

    const Track& trk = tracks[trackIdx];
    int depth = trk.pitchSweepDepth;
    int time = trk.pitchSweepTime;
    int glide = trk.glideTime; // Resolve Glide Time

    int finalPolyMode = trk.polyMode;
    if (finalPolyMode < 1) finalPolyMode = 1;
    if (finalPolyMode > 4) finalPolyMode = 4;

    if (trk.engineType == ENGINE_SYNTH) {
        int targetIdx = StealSynthVoice(trackIdx, finalPolyMode);

        if (g_trackVoices[trackIdx][targetIdx].active) {
            g_trackVoices[trackIdx][targetIdx].Choke();
        }
        g_trackVoices[trackIdx][targetIdx].Trigger(freq, depth, time, velocity, false, 0, glide); // Pass Glide Time
        g_trackVoiceIndex[trackIdx] = (targetIdx + 1) % finalPolyMode;
    } else if (trk.engineType == ENGINE_SAMPLER) {
        int slot = trk.sampleSlot;
        const int16_t* buffer = g_samplePool[slot].pcmData.data();
        uint32_t length = (uint32_t)g_samplePool[slot].pcmData.size();
        int sliceIdx = SamplerSliceForNote(trackIdx, midiNote, nullptr);
        float noteOffset = (sliceIdx >= 0) ? 0.0f : (float)(midiNote - 60);

        int targetIdx = StealSamplerVoice(trackIdx, finalPolyMode);

        if (g_samplerVoices[trackIdx][targetIdx].active) {
            g_samplerVoices[trackIdx][targetIdx].Choke();
        }
        g_samplerVoices[trackIdx][targetIdx].Trigger(buffer, length, noteOffset, (float)trk.fine2, depth, time, velocity, false, 0, sliceIdx);
        g_samplerVoiceIndex[trackIdx] = (targetIdx + 1) % finalPolyMode;
    }
}

static void ReleaseVoiceLiveImpl(int trackIdx, int midiNote) {
    if (trackIdx < 0 || trackIdx >= 8) return;
    
    float freq = 440.0f * powf(2.0f, (midiNote - 69.0f) / 12.0f);

    if (tracks[trackIdx].engineType == ENGINE_SYNTH) {
        for (int v = 0; v < 4; ++v) {
            if (g_trackVoices[trackIdx][v].active && std::abs(g_trackVoices[trackIdx][v].baseFreq - freq) < 0.01f) {
                g_trackVoices[trackIdx][v].Release();
            }
        }
    } else if (tracks[trackIdx].engineType == ENGINE_SAMPLER) {
        // Releases sampler voices on key release
        for (int v = 0; v < 4; ++v) {
            if (g_samplerVoices[trackIdx][v].active) {
                // SOLUTION B: Only trigger release if looping or granular
                if (tracks[trackIdx].sampleLoop == 1 || tracks[trackIdx].algorithm == ALGO_GRANULAR) {
                    g_samplerVoices[trackIdx][v].Release();
                }
            }
        }
    }
}

static void ApplyMidiLiveEvent(const MidiLiveEvent& ev) {
    switch (ev.type) {
        case ML_NOTE_ON:
            TriggerVoiceLiveImpl((int)ev.track, (int)ev.note, (int)ev.velocity);
            break;
        case ML_NOTE_OFF:
            ReleaseVoiceLiveImpl((int)ev.track, (int)ev.note);
            break;
        case ML_SEQ_STEP: {
            const int t = (int)ev.track;
            const int step = (int)ev.stepIndex;
            if (t < 0 || t >= 8) break;
            if (step < 0 || step >= tracks[t].stepLength) break;
            tracks[t].steps[step].note = (int8_t)ev.note;
            tracks[t].steps[step].velocity = (int)ev.velocity;
            break;
        }
        default:
            break;
    }
}

void TriggerVoiceLive(int trackIdx, int midiNote, int velocity) {
    if (trackIdx < 0 || trackIdx >= 8) return;
    if (midiNote < 0 || midiNote > 127) return;
    MidiLiveEvent ev{};
    ev.type = ML_NOTE_ON;
    ev.track = (uint8_t)trackIdx;
    ev.note = (uint8_t)midiNote;
    ev.velocity = (uint8_t)std::clamp(velocity, 0, 255);
    ev.stepIndex = 0;
    PushMidiLive(g_midiLiveQueueFromUi, ev);
}

void ReleaseVoiceLive(int trackIdx, int midiNote) {
    if (trackIdx < 0 || trackIdx >= 8) return;
    if (midiNote < 0 || midiNote > 127) return;
    MidiLiveEvent ev{};
    ev.type = ML_NOTE_OFF;
    ev.track = (uint8_t)trackIdx;
    ev.note = (uint8_t)midiNote;
    ev.velocity = 0;
    ev.stepIndex = 0;
    PushMidiLive(g_midiLiveQueueFromUi, ev);
}

void EnqueueMidiLiveFromRt(uint8_t type, uint8_t track, uint8_t note, uint8_t velocity, uint8_t stepIndex) {
    MidiLiveEvent ev{};
    ev.type = type;
    ev.track = track;
    ev.note = note;
    ev.velocity = velocity;
    ev.stepIndex = stepIndex;
    PushMidiLive(g_midiLiveQueueFromRt, ev);
}

bool IsSynthVoiceActive(int trackIdx, int voiceIdx) {
    if (trackIdx < 0 || trackIdx >= 8 || voiceIdx < 0 || voiceIdx >= 4) return false;
    return g_trackVoices[trackIdx][voiceIdx].active;
}
