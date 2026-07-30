#include "Audio_Engine.hpp"
#include "Sampler_Engine.hpp"
#include "Master_FX.hpp"
#include "Common.hpp"
#include "Globals.hpp"
#include <cmath>
#include <filesystem>
#include <cstdlib> // std::abs, rand

#define MINIAUDIO_IMPLEMENTATION
#include "../miniaudio.h"

// Real-time Audio Device state
static ma_device g_audioDevice;
static bool g_audioInitialized = false;

static const double g_sampleRate = 44100.0;

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
            float phaseInc = (2.0f * 3.14159265f * lfoHz * 64.0f) / 44100.0f;

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
            float phaseInc = (2.0f * 3.14159265f * lfoHz * 64.0f) / 44100.0f;

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
            if (stage1 == ENV1_IDLE || !active) {
                currentFreq = targetFreq; // Start glide from target pitch if triggering from silence
            }
            baseFreq = targetFreq;
            active = true;
            stage1 = ENV1_ATTACK;
            stage2 = ENV2_ATTACK;
            lastModOutput = 0.0f; // Flush feedback buffer on note trigger
            envLevel1 = 0.0f;
            envLevel2 = 0.0f;
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
        noiseEnvLevel = 0.0f;
        noiseHoldCounter = 0;

        // Initialize Filter State and Envelope
        filterStage = FLT_ATTACK;
        filterEnvLevel = 0.0f;
        filterUpdateCounter = 9999; // Force instant coefficient calculation
        filter.reset();

        // Reset smooth state flags to trigger instant snapping on first process frame
        smoothCutoff = -1.0f;
        smoothMorph1 = -1.0f;
        smoothMorph2 = -1.0f;
        smoothVol1 = -1.0f;
        smoothVol1Raw = -1.0f;
        smoothVol2 = -1.0f;
        
        // Reset analog emulation filters and drifts on note trigger
        osc1Drift = 0.0f;
        osc2Drift = 0.0f;
        osc1LPState = 0.0f;
        osc2LPState = 0.0f;
        
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
        }    // Process a single wave slice dynamically (expects normalized phase in [0, 1))
    float ProcessWave(float normPhase, float dt, int morph) {
        // Wrap normalized phase to [0.0, 1.0)
        while (normPhase >= 1.0f) normPhase -= 1.0f;
        while (normPhase < 0.0f)  normPhase += 1.0f;

        // Scale by 2*PI only at the moment of sine calculation
        float sineSample = sinf(normPhase * 6.2831853f);
        
        float triSample = 0.0f;
        if (normPhase < 0.25f)      triSample = normPhase * 4.0f;
        else if (normPhase < 0.75f) triSample = 2.0f - (normPhase * 4.0f);
        else                        triSample = (normPhase * 4.0f) - 4.0f;

        // Band-limit the Saw wave (step of +2.0 at wrap point)
        float naiveSaw = 1.0f - (normPhase * 2.0f);
        float sawSample = naiveSaw + blep(normPhase, dt);

        // Band-limit the Square wave (step of +1.0 at 0.0, and -1.0 at 0.5)
        float naiveSqr = (normPhase < 0.5f) ? 0.5f : -0.5f;
        float phaseSquare2 = normPhase + 0.5f;
        if (phaseSquare2 >= 1.0f) phaseSquare2 -= 1.0f; // Symmetrical wrap
        
        float sqrSample = naiveSqr + 0.5f * blep(normPhase, dt) - 0.5f * blep(phaseSquare2, dt);

        if (morph < 33) {
            float t = morph / 33.0f;
            return (1.0f - t) * sineSample + t * triSample;
        } else if (morph < 66) {
            float t = (morph - 33) / 33.0f;
            return (1.0f - t) * triSample + t * sawSample;
        } else {
            float t = (morph - 66) / 33.0f;
            return (1.0f - t) * sawSample + t * sqrSample;
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

            // Reset mod offsets, then sum LFO targets before envelope coeffs
            // so DEST_DECAY can lengthen/shorten amp decay in this same block.
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

            for (int srcTrkIdx = 0; srcTrkIdx < 8; ++srcTrkIdx) {
                const Track& srcTrk = tracks[srcTrkIdx];

                for (int s = 0; s < 3; ++s) {
                    const ModSlot& m = srcTrk.lfo1Slots[s];
                    if (m.destType == 1 && m.destTrack == trackIdx) {
                        float modVal = g_globalLFOValues[srcTrkIdx][0] * (m.depth / 99.0f);
                        if (m.destParam == DEST_CUTOFF)          modCutoffOffset += modVal * 99.0f;
                        else if (m.destParam == DEST_RESONANCE)  modResOffset += modVal * 99.0f;
                        else if (m.destParam == DEST_VOLUME)     modVol1Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_MORPH1)     modMorph1Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_MORPH2)     modMorph2Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_PITCH)      modPitchOffset += modVal * 12.0f;
                        else if (m.destParam == DEST_DECAY)      modDecayOffset += modVal * 99.0f;
                        else if (m.destParam == DEST_VOLUME2)    modVol2Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_FINE1)      modFine1Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_FINE2)      modFine2Offset += modVal * 99.0f;
                    }
                }

                for (int s = 0; s < 3; ++s) {
                    const ModSlot& m = srcTrk.lfo2Slots[s];
                    if (m.destType == 1 && m.destTrack == trackIdx) {
                        float modVal = g_globalLFOValues[srcTrkIdx][1] * (m.depth / 99.0f);
                        if (m.destParam == DEST_CUTOFF)          modCutoffOffset += modVal * 99.0f;
                        else if (m.destParam == DEST_RESONANCE)  modResOffset += modVal * 99.0f;
                        else if (m.destParam == DEST_VOLUME)     modVol1Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_MORPH1)     modMorph1Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_MORPH2)     modMorph2Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_PITCH)      modPitchOffset += modVal * 12.0f;
                        else if (m.destParam == DEST_DECAY)      modDecayOffset += modVal * 99.0f;
                        else if (m.destParam == DEST_VOLUME2)    modVol2Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_FINE1)      modFine1Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_FINE2)      modFine2Offset += modVal * 99.0f;
                    }
                }
            }

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

                float fine1Mod = std::clamp(GetParam(sp.fine, trk.fine) + modFine1Offset, -99.0f, 99.0f);
                float semitoneOffset1 = GetParam(sp.coarse, trk.coarse) + (fine1Mod / 100.0f);
                float freq1 = currentFreq * pitchModFactor * powf(2.0f, (semitoneOffset1 + modPitchOffset) / 12.0f); // Modulated by pitch envelope

        float finalSample = 0.0f;

        if (algo == ALGO_PARALLEL) {
            // ==========================================
            // ALGORITHM A: DUAL-OSCILLATOR MIX (PARALLEL)
            // ==========================================
            float fine2Mod = std::clamp(GetParam(sp.fine2, trk.fine2) + modFine2Offset, -99.0f, 99.0f);
            float semitoneOffset2 = GetParam(sp.coarse2, trk.coarse2) + (fine2Mod / 100.0f);
            float freq2 = baseFreq * pitchModFactor * powf(2.0f, semitoneOffset2 / 12.0f);

            float freq1AnalogScale = 1.0f;
            float freq2AnalogScale = 1.0f;

            if (analogAmount > 0.0f) {
                // 1. Apply random pitch slop (drift)
                freq1AnalogScale += osc1Drift * analogAmount * 0.0022f;
                freq2AnalogScale += osc2Drift * analogAmount * 0.0022f;
                
                // 2. Apply static detune offset
                freq2AnalogScale += analogAmount * 0.00042f;

                // 3. Apply Differential Keyboard Tracking Error (Oscillator Divergence)
                float octavesFromCenter = log2f(baseFreq / 261.63f);
                float trackingDivergence = octavesFromCenter * analogAmount * 0.005f;
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

// Keep track of bar loops per track to evaluate trigger conditions (e.g., 1:2)
static int g_trackBarCount[8] = { 0 };

// Sample-accurate retrigger state per track
struct ActiveRetrig {
    int midiNote = -1;
    int remainingTriggers = 0;
    ma_uint32 sampleInterval = 0;
    ma_uint32 sampleCounter = 0;
    int velocity = 3; // Tracks velocity on ratchets
};
static ActiveRetrig g_activeRetrigs[8];

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
    auto startTime = std::chrono::high_resolution_clock::now(); // Record start time
    float* pOutputF = (float*)pOutput;
    ma_uint32 samplesProcessed = 0;

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

        UpdateGlobalLFOs();
        
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

            if (g_externalMidiStartTriggered) {
                g_externalMidiStartTriggered = false;
                g_currentTick = -1;
                isPlaying = true;
            }
            if (g_externalMidiStopTriggered) {
                g_externalMidiStopTriggered = false;
                isPlaying = false;
            }

            if (isPlaying) {
                bool tickTriggered = false;
                int masterTicksLimit = (masterLength == 0) ? 192 : (masterLength * 6);

                if (g_useExternalMidiClock) {
                    if (g_externalMidiTicksQueued > 0) {
                        g_externalMidiTicksQueued--;
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
                    for (int t = 0; t < 8; ++t) {
                        int nextTick = (tracks[t].localTick + 1) % (tracks[t].stepLength * 6);
                        if (nextTick == 0 && tracks[t].localTick >= 0) {
                            g_trackBarCount[t]++;
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
                            }
                        } else if (masterLength > 0) {
                            for (int t = 0; t < 8; ++t) {
                                tracks[t].localTick = -1;
                                g_trackBarCount[t] = 0;
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
                            int triggerTick = (s * 6 + step.microtiming);
                            int localLengthTicks = tracks[t].stepLength * 6;
                            
                            triggerTick = triggerTick % localLengthTicks;
                            if (triggerTick < 0) triggerTick += localLengthTicks;

                            if (triggerTick == tracks[t].localTick) {
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

                                            for (int n = 0; n < notesCount; ++n) {
                                                int midiNote = midiNotesToTrigger[n];
                                                float freq = 440.0f * powf(2.0f, (midiNote - 69.0f) / 12.0f);

                                                if (tracks[t].engineType == ENGINE_SYNTH) {
                                                    g_trackVoiceIndex[t] = g_trackVoiceIndex[t] % finalPolyMode;
                                                    int targetIdx = g_trackVoiceIndex[t];

                                                    if (g_trackVoices[t][targetIdx].active) {
                                                        g_trackVoices[t][targetIdx].Choke();
                                                    }
                                                    g_trackVoices[t][targetIdx].Trigger(freq, depth, time, step.velocity, true, step.noteLength, glide); // Pass Glide Time
                                                    g_trackVoiceIndex[t] = (g_trackVoiceIndex[t] + 1) % finalPolyMode;
                                                } else {
                                                    int slot = GetParam(step.params.sampleSlot, tracks[t].sampleSlot);
                                                    const int16_t* buffer = g_samplePool[slot].pcmData.data();
                                                    uint32_t length = (uint32_t)g_samplePool[slot].pcmData.size();
                                                    float noteOffset = (float)(midiNote - 60);

                                                    g_samplerVoiceIndex[t] = g_samplerVoiceIndex[t] % finalPolyMode;
                                                    int targetIdx = g_samplerVoiceIndex[t];

                                                    if (g_samplerVoices[t][targetIdx].active) {
                                                        g_samplerVoices[t][targetIdx].Choke();
                                                    }
                                                    g_samplerVoices[t][g_samplerVoiceIndex[t]].Trigger(buffer, length, noteOffset, (float)tracks[t].fine2, depth, time, step.velocity, true, step.noteLength);
                                                    g_samplerVoiceIndex[t] = (g_samplerVoiceIndex[t] + 1) % finalPolyMode;
                                                }
                                            }

                                            if (step.retrigger > 1) {
                                                ActiveRetrig& ar = g_activeRetrigs[t];
                                                ar.midiNote = midiNoteRoot;
                                                ar.remainingTriggers = step.retrigger - 1;
                                                ar.sampleInterval = samplesPerStep / step.retrigger;
                                                ar.sampleCounter = 0;
                                                ar.velocity = step.velocity;
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
                                    } else {
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
                        ar.sampleCounter++;
                        if (ar.sampleCounter >= ar.sampleInterval) {
                            ar.sampleCounter = 0;
                            ar.remainingTriggers--;

                            if (ar.midiNote >= 0) {
                                float freq = 440.0f * powf(2.0f, (ar.midiNote - 69.0f) / 12.0f);
                                int currentStepIdx = (tracks[t].localTick / 6) % tracks[t].stepLength;
                                const Step& step = tracks[t].steps[currentStepIdx];
                                int depth = (step.params.pitchSweepDepth == -1) ? tracks[t].pitchSweepDepth : step.params.pitchSweepDepth;
                                int time = (step.params.pitchSweepTime == -1) ? tracks[t].pitchSweepTime : step.params.pitchSweepTime;
                                int glide = (step.params.glideTime == -1) ? tracks[t].glideTime : step.params.glideTime; // Resolve Glide Time

                                if (tracks[t].engineType == ENGINE_SYNTH) {
                                    g_trackVoices[t][g_trackVoiceIndex[t]].Release();
                                    g_trackVoices[t][g_trackVoiceIndex[t]].Trigger(freq, depth, time, ar.velocity, true, 0, glide); // Pass Glide Time
                                    g_trackVoiceIndex[t] = (g_trackVoiceIndex[t] + 1) % 4;
                                } else {
                                    int slot = (step.params.sampleSlot == -1) ? tracks[t].sampleSlot : step.params.sampleSlot;
                                    const int16_t* buffer = g_samplePool[slot].pcmData.data();
                                    uint32_t length = (uint32_t)g_samplePool[slot].pcmData.size();
                                    float noteOffset = (float)(ar.midiNote - 60);

                                    g_samplerVoices[t][g_samplerVoiceIndex[t]].Release();
                                    g_samplerVoices[t][g_samplerVoiceIndex[t]].Trigger(buffer, length, noteOffset, (float)tracks[t].fine2, depth, time, ar.velocity, true);
                                    g_samplerVoiceIndex[t] = (g_samplerVoiceIndex[t] + 1) % 4;
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
                }
            }

            float masterDryMono = 0.0f;
            float delaySendBusMono = 0.0f;
            float satSendBusMono = 0.0f;
            float reverbSendBusMono = 0.0f;
            float autoPanSendBusMono = 0.0f;

            for (int t = 0; t < 8; ++t) {
                if (tracks[t].muted) {
                    for (int v = 0; v < 4; ++v) {
                        g_trackVoices[t][v].Release();
                        g_samplerVoices[t][v].Release();
                    }
                    continue;
                }

                float trackSampleSum = 0.0f;
                for (int v = 0; v < 4; ++v) {
                    if (tracks[t].engineType == ENGINE_SYNTH) {
                        trackSampleSum += g_trackVoices[t][v].Process(t);
                    } else {
                        trackSampleSum += g_samplerVoices[t][v].Process(t);
                    }
                }
                
                float normalTrackSum = trackSampleSum * 0.25f;
                int currentStepIdx = (tracks[t].localTick / 6) % tracks[t].stepLength;
                if (currentStepIdx < 0) currentStepIdx = 0;
                const Step& step = tracks[t].steps[currentStepIdx];

                float processedSum = tracks[t].tapeFX.process(
                                normalTrackSum,
                                s_finalMem[t], s_finalHds[t], s_finalSpr[t], s_finalSpd[t], s_finalTet[t],
                                s_finalDrf[t], s_finalDrt[t], s_finalFdb[t], s_finalFsp[t], s_finalFsc[t],
                                s_finalFrz[t], s_finalSmr[t], s_finalSms[t], s_finalMix[t],
                                g_sampleRate
                            );

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

            // Replaced the punishing /16.0f division (ghost track attenuation)
                        // with a standard master headroom scaling factor (0.35f ~ -9dB headroom)
                        const float kMasterScale = 0.35f;

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

            float revMixMod = 0.0f;
            float delMixMod = 0.0f;
            float satMixMod = 0.0f;
            float panMixMod = 0.0f;

            for (int t = 0; t < 8; ++t) {
                for (int s = 0; s < 3; ++s) {
                    if (tracks[t].lfo1Slots[s].destType == 2) {
                        float modVal = g_globalLFOValues[t][0] * (tracks[t].lfo1Slots[s].depth / 99.0f);
                        if (tracks[t].lfo1Slots[s].destParam == DEST_REV_MIX)       revMixMod += modVal;
                        else if (tracks[t].lfo1Slots[s].destParam == DEST_DEL_MIX)  delMixMod += modVal;
                        else if (tracks[t].lfo1Slots[s].destParam == DEST_SAT_MIX)  satMixMod += modVal;
                        else if (tracks[t].lfo1Slots[s].destParam == DEST_PAN_MIX)  panMixMod += modVal;
                    }
                    if (tracks[t].lfo2Slots[s].destType == 2) {
                        float modVal = g_globalLFOValues[t][1] * (tracks[t].lfo2Slots[s].depth / 99.0f);
                        if (tracks[t].lfo2Slots[s].destParam == DEST_REV_MIX)       revMixMod += modVal;
                        else if (tracks[t].lfo2Slots[s].destParam == DEST_DEL_MIX)  delMixMod += modVal;
                        else if (tracks[t].lfo2Slots[s].destParam == DEST_SAT_MIX)  satMixMod += modVal;
                        else if (tracks[t].lfo2Slots[s].destParam == DEST_PAN_MIX)  panMixMod += modVal;
                    }
                }
            }

            float reverbWetL = 0.0f;
            float reverbWetR = 0.0f;
            g_masterReverb.process(
                reverbSendL, reverbSendR, reverbWetL, reverbWetR,
                (float)globalFX.reverbDecay / 99.0f,
                (float)globalFX.reverbSize / 99.0f,
                (float)globalFX.reverbPredelay / 99.0f,
                1.0f,
                (float)g_sampleRate
            );
            float globalRevMix = std::clamp(((float)globalFX.reverbMix / 99.0f) + revMixMod, 0.0f, 1.0f);
            reverbWetL *= globalRevMix;
            reverbWetR *= globalRevMix;

            float delayWetL = 0.0f;
            float delayWetR = 0.0f;
            g_masterDelay.process(
                delaySendL, delaySendR, delayWetL, delayWetR,
                (float)globalFX.delayTime / 99.0f,
                (float)globalFX.delayFeedback / 99.0f,
                globalFX.delayPingPong,
                1.0f,
                (float)g_sampleRate
            );
            float globalDelayMixNorm = std::clamp(((float)globalFX.delayMix / 99.0f) + delMixMod, 0.0f, 1.0f);
            delayWetL *= globalDelayMixNorm;
            delayWetR *= globalDelayMixNorm;

            float satWetL = 0.0f;
            float satWetR = 0.0f;
            g_masterCompressor.process(
                satSendL, satSendR, satWetL, satWetR,
                (float)globalFX.satLevel / 99.0f,
                (float)globalFX.satSymmetry / 99.0f,
                (float)globalFX.satOverdrive / 99.0f,
                1.0f,
                (float)g_sampleRate
            );
            float globalSatMixNorm = std::clamp(((float)globalFX.satMix / 99.0f) + satMixMod, 0.0f, 1.0f);
            satWetL *= globalSatMixNorm;
            satWetR *= globalSatMixNorm;

            float panWetL = 0.0f;
            float panWetR = 0.0f;
            g_masterTornado.process(
                panSendL, panSendR, panWetL, panWetR,
                (float)globalFX.autoPanTime / 99.0f,
                (float)globalFX.autoPanFeedback / 99.0f,
                (float)globalFX.autoPanWidth / 99.0f,
                1.0f,
                (float)g_sampleRate
            );
            float globalPanMix = std::clamp(((float)globalFX.autoPanMix / 99.0f) + panMixMod, 0.0f, 1.0f);
            panWetL *= globalPanMix;
            panWetR *= globalPanMix;

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

            // Master Soft-Limiter: 100% transparent below 0.85f, soft-knee tanh compression up to 1.0f
                        auto ApplyMasterLimit = [](float sample) -> float {
                            float absSample = std::abs(sample);
                            if (absSample < 0.85f) {
                                return sample; // Fully linear & transparent below 0.85f
                            }
                            float excess = absSample - 0.85f;
                            // Asymptotically approaches 1.0f (0.85f + 0.15f * 1.0f)
                            float compressed = 0.85f + 0.15f * tanhf(excess / 0.15f);
                            return (sample > 0.0f) ? compressed : -compressed;
                        };

                        pOutputF[2 * outIdx]     = ApplyMasterLimit(outL);
                        pOutputF[2 * outIdx + 1] = ApplyMasterLimit(outR);

        } // End of i (chunkSize) loop

        samplesProcessed += chunkSize;
    } // End of while loop

    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();
    double expectedSec = (double)frameCount / g_sampleRate;
    float instantCpu = (float)((elapsedSec / expectedSec) * 100.0);
    if (instantCpu > 100.0f) instantCpu = 100.0f;

    static float s_smoothedCpu = 0.0f;
    s_smoothedCpu += 0.05f * (instantCpu - s_smoothedCpu);
    g_audioCpuLoad = s_smoothedCpu;
}
// ==========================================
// PUBLIC CONTROLLER INTERFACE
// ==========================================
void InitAudioEngine() {
    InitVolumeCurve();
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_f32;
    deviceConfig.playback.channels = 2; // Stereo
    deviceConfig.sampleRate        = (ma_uint32)g_sampleRate;
    deviceConfig.dataCallback      = ma_audio_callback;
    
    deviceConfig.periodSizeInFrames = 512;
        deviceConfig.periodSizeInMilliseconds = 0;

    // Initialize delay lines with device sample rate on launch [2]
    g_masterDelay.init((float)g_sampleRate);
    g_masterReverb.init((float)g_sampleRate);
    g_masterTornado.init((float)g_sampleRate);

    if (ma_device_init(NULL, &deviceConfig, &g_audioDevice) == MA_SUCCESS) {
        ma_device_start(&g_audioDevice);
        g_audioInitialized = true;
    }
}

void ShutdownAudioEngine() {
    if (g_audioInitialized) {
        ma_device_uninit(&g_audioDevice);
        g_audioInitialized = false;
    }
}

bool LoadSampleToPool(int slotIdx, const std::string& filename) {
    if (slotIdx < 0 || slotIdx >= 16) {
        menuFeedback = "DIAG: invalid slot index";
        return false;
    }
    if (filename.empty()) {
        menuFeedback = "DIAG: filename empty";
        return false;
    }

    // RAII guard automatically stops the audio device and restarts it when this function exits,
    // protecting against dangling references on any early error return paths.
    struct AudioStopGuard {
        bool stopped = false;
        AudioStopGuard() {
            if (g_audioInitialized) {
                ma_device_stop(&g_audioDevice);
                stopped = true;
            }
        }
        ~AudioStopGuard() {
            if (stopped && g_audioInitialized) {
                ma_device_start(&g_audioDevice);
            }
        }
    } guard;

    // 1. Resolve file path BEFORE initializing the decoder to prevent double-init crash

    // 1. Resolve file path BEFORE initializing the decoder to prevent double-init crash
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

    // 2. Initialize the config
    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 1, 32000);
    ma_decoder decoder;

    // 3. Call init_file EXACTLY once
    menuFeedback = "DIAG: calling init_file";
    ma_result result = ma_decoder_init_file(filepath.c_str(), &config, &decoder);
    if (result != MA_SUCCESS) {
        menuFeedback = "DIAG: init_file failed";
        return false;
    }

    // 4. Allocate temporary buffer on heap (16 seconds maximum)
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

    // 5. Read PCM frames safely
    menuFeedback = "DIAG: calling read_pcm_frames";
    ma_uint64 framesRead = 0;
    ma_result readResult = ma_decoder_read_pcm_frames(&decoder, tempBuffer.data(), maxFrames, &framesRead);
    
    // 6. Uninitialize immediately to release resources
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

    // 7. Clamp and shrink to fit loaded frames
    menuFeedback = "DIAG: resizing buffer";
    if (framesRead > maxFrames) {
        framesRead = maxFrames;
    }
    tempBuffer.resize(framesRead);

    // 8. Peak Amplitude Normalization (Normalizes file volume cleanly)
    menuFeedback = "DIAG: normalizing volume";
    int16_t peak = 0;
    for (size_t i = 0; i < tempBuffer.size(); ++i) {
        int16_t absVal = std::abs(tempBuffer[i]);
        if (absVal > peak) {
            peak = absVal;
        }
    }
    if (peak > 0) {
        float scale = 32767.0f; // Scale factor base
        scale /= peak;
        for (size_t i = 0; i < tempBuffer.size(); ++i) {
            float scaledVal = tempBuffer[i] * scale;
            if (scaledVal > 32767.0f)  scaledVal = 32767.0f;
            if (scaledVal < -32768.0f) scaledVal = -32768.0f;
            tempBuffer[i] = (int16_t)scaledVal;
        }
    }

        // 9b. Pre-calculate 97 peak values for the visual display cache
        std::array<uint8_t, 97> peaks{};
        if (!tempBuffer.empty()) {
            for (int i = 0; i < 97; ++i) {
                size_t startFrame = (i * tempBuffer.size()) / 97;
                size_t endFrame = ((i + 1) * tempBuffer.size()) / 97;
                if (endFrame > tempBuffer.size()) endFrame = tempBuffer.size();
                if (startFrame >= endFrame) startFrame = (endFrame > 0) ? (endFrame - 1) : 0;

                int16_t peak = 0;
                for (size_t f = startFrame; f < endFrame; ++f) {
                    int16_t absVal = std::abs(tempBuffer[f]);
                    if (absVal > peak) peak = absVal;
                }
                // Scale the absolute peak (0 to 32767) to a vertical drawing radius (0 to 11 pixels)
                peaks[i] = (uint8_t)((peak / 32768.0f) * 11.0f);
            }
        }

        // 10. Swap loaded sample into RAM pool safely
        g_samplePool[slotIdx].name = filename;
        g_samplePool[slotIdx].pcmData = std::move(tempBuffer);
        g_samplePool[slotIdx].visualPeaks = peaks;

        menuFeedback = "SAMPLE CRUNCHED TO POOL!";
        return true;
    }

void TriggerVoiceLive(int trackIdx, int midiNote, int velocity) {
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
        g_trackVoiceIndex[trackIdx] = g_trackVoiceIndex[trackIdx] % finalPolyMode;
        int targetIdx = g_trackVoiceIndex[trackIdx];

        if (g_trackVoices[trackIdx][targetIdx].active) {
            g_trackVoices[trackIdx][targetIdx].Choke();
        }
        g_trackVoices[trackIdx][targetIdx].Trigger(freq, depth, time, velocity, false, 0, glide); // Pass Glide Time
        g_trackVoiceIndex[trackIdx] = (g_trackVoiceIndex[trackIdx] + 1) % finalPolyMode;
    } else {
        int slot = trk.sampleSlot;
        const int16_t* buffer = g_samplePool[slot].pcmData.data();
        uint32_t length = (uint32_t)g_samplePool[slot].pcmData.size();
        float noteOffset = (float)(midiNote - 60);

        g_samplerVoiceIndex[trackIdx] = g_samplerVoiceIndex[trackIdx] % finalPolyMode;
        int targetIdx = g_samplerVoiceIndex[trackIdx];

        if (g_samplerVoices[trackIdx][targetIdx].active) {
            g_samplerVoices[trackIdx][targetIdx].Choke();
        }
        g_samplerVoices[trackIdx][g_samplerVoiceIndex[trackIdx]].Trigger(buffer, length, noteOffset, (float)trk.fine2, depth, time, velocity, false);
        g_samplerVoiceIndex[trackIdx] = (g_samplerVoiceIndex[trackIdx] + 1) % finalPolyMode;
    }
}

void ReleaseVoiceLive(int trackIdx, int midiNote) {
    if (trackIdx < 0 || trackIdx >= 8) return;
    
    float freq = 440.0f * powf(2.0f, (midiNote - 69.0f) / 12.0f);

    if (tracks[trackIdx].engineType == ENGINE_SYNTH) {
        for (int v = 0; v < 4; ++v) {
            if (g_trackVoices[trackIdx][v].active && std::abs(g_trackVoices[trackIdx][v].baseFreq - freq) < 0.01f) {
                g_trackVoices[trackIdx][v].Release();
            }
        }
    } else {
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

bool IsSynthVoiceActive(int trackIdx, int voiceIdx) {
    if (trackIdx < 0 || trackIdx >= 8 || voiceIdx < 0 || voiceIdx >= 4) return false;
    return g_trackVoices[trackIdx][voiceIdx].active;
}
