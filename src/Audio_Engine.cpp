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

    for (int t = 0; t < 8; ++t) {
        Track& trk = tracks[t];
        if (trk.muted) {
            g_globalLFOValues[t][0] = 0.0f;
            g_globalLFOValues[t][1] = 0.0f;
            continue;
        }

        // --- 1. EVALUATE LFO 1 ---
        {
            float normSpeed = trk.lfo1Speed / 99.0f;
            float lfoHz = 0.05f * powf(400.0f, normSpeed); // Exponential mapping: 0.05Hz to 20Hz
            float phaseInc = (2.0f * 3.14159265f * lfoHz * 64.0f) / 44100.0f;

            trk.lfo1Phase += phaseInc;
            bool wrapped = false;
            if (trk.lfo1Phase >= 2.0f * 3.14159265f) {
                trk.lfo1Phase -= 2.0f * 3.14159265f;
                wrapped = true;
            }

            float val = 0.0f;
            if (trk.lfo1Wave == 0) { // Sine
                val = sinf(trk.lfo1Phase);
            } else if (trk.lfo1Wave == 1) { // Triangle
                float norm = trk.lfo1Phase / (2.0f * 3.14159265f);
                val = (norm < 0.25f) ? (norm * 4.0f) : ((norm < 0.75f) ? (2.0f - (norm * 4.0f)) : ((norm * 4.0f) - 4.0f));
            } else if (trk.lfo1Wave == 2) { // Saw
                float norm = trk.lfo1Phase / (2.0f * 3.14159265f);
                val = 1.0f - (norm * 2.0f);
            } else if (trk.lfo1Wave == 3) { // Square
                float norm = trk.lfo1Phase / (2.0f * 3.14159265f);
                val = (norm < 0.5f) ? 1.0f : -1.0f;
            } else { // Sample & Hold
                                if (wrapped || trk.lfo1LastVal == 0.0f) {
                                    trk.lfo1LastVal = FastRandFloat(lfoRandSeed) * 2.0f - 1.0f;
                                }
                                val = trk.lfo1LastVal;
                            }
            // Scale and output value relative to depth
            g_globalLFOValues[t][0] = val * (trk.lfo1Depth / 99.0f);
        }

        // --- 2. EVALUATE LFO 2 ---
        {
            float normSpeed = trk.lfo2Speed / 99.0f;
            float lfoHz = 0.05f * powf(400.0f, normSpeed); // Exponential mapping: 0.05Hz to 20Hz
            float phaseInc = (2.0f * 3.14159265f * lfoHz * 64.0f) / 44100.0f;

            trk.lfo2Phase += phaseInc;
            bool wrapped = false;
            if (trk.lfo2Phase >= 2.0f * 3.14159265f) {
                trk.lfo2Phase -= 2.0f * 3.14159265f;
                wrapped = true;
            }

            float val = 0.0f;
            if (trk.lfo2Wave == 0) { // Sine
                val = sinf(trk.lfo2Phase);
            } else if (trk.lfo2Wave == 1) { // Triangle
                float norm = trk.lfo2Phase / (2.0f * 3.14159265f);
                val = (norm < 0.25f) ? (norm * 4.0f) : ((norm < 0.75f) ? (2.0f - (norm * 4.0f)) : ((norm * 4.0f) - 4.0f));
            } else if (trk.lfo2Wave == 2) { // Saw
                float norm = trk.lfo2Phase / (2.0f * 3.14159265f);
                val = 1.0f - (norm * 2.0f);
            } else if (trk.lfo2Wave == 3) { // Square
                float norm = trk.lfo2Phase / (2.0f * 3.14159265f);
                val = (norm < 0.5f) ? 1.0f : -1.0f;
            } else { // Sample & Hold
                                if (wrapped || trk.lfo2LastVal == 0.0f) {
                                    trk.lfo2LastVal = FastRandFloat(lfoRandSeed) * 2.0f - 1.0f;
                                }
                                val = trk.lfo2LastVal;
                            }
            // Scale and output value relative to depth
            g_globalLFOValues[t][1] = val * (trk.lfo2Depth / 99.0f);
        }
    }
}

// Converts a 0..99 value to an exponential time duration in seconds.
static float GetEnvTime(int val) {
    float norm = val / 99.0f;
    return 0.0010f * powf(15000.0f, norm * norm * norm);
}

// Cubic soft-clipper shaper to create warm, sweltering saturated waveforms
static float ApplySaturation(float input, float drive) {
    float driven = input * drive;
    if (driven > 1.0f)  driven = 1.0f;
    if (driven < -1.0f) driven = -1.0f;
    return driven - (driven * driven * driven) / 3.0f;
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
        float smoothVol2 = -1.0f;

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
        float envDecRate1 = 0.0f;
        float envRelRate1 = 0.0f;
        float envSusLevel1 = 0.0f;

        float envAtkRate2 = 0.0f;
        float envDecRate2 = 0.0f;
        float envRelRate2 = 0.0f;
        float envSusLevel2 = 0.0f;

        float noiseAtkRate = 0.0f;
        float noiseDecRate = 0.0f;
        uint32_t noiseHoldSamples = 0;

        float filterAtkRate = 0.0f;
        float filterDecRate = 0.0f;
        float filterRelRate = 0.0f;
        float filterSusLevel = 0.0f;
    
    void Choke() {
            choking = true;
            chokeVolume = 1.0f;
        }
    
    void Trigger(float targetFreq, int depth, int time, int velocity, bool isSeq = false) {
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
    }

    void Release() {
        if (stage1 != ENV1_IDLE) stage1 = ENV1_RELEASE;
        if (stage2 != ENV2_IDLE) stage2 = ENV2_RELEASE;
        
        // Quiet noise immediately on release
        if (noiseStage != NOISE_IDLE) noiseStage = NOISE_DECAY;

        // Release Filter Envelope
        if (filterStage != FLT_IDLE) filterStage = FLT_RELEASE;
    }

    // Process a single wave slice dynamically (expects normalized phase in [0, 1))
        float ProcessWave(float normPhase, int morph) {
            // Wrap normalized phase to [0.0, 1.0)
            while (normPhase >= 1.0f) normPhase -= 1.0f;
            while (normPhase < 0.0f)  normPhase += 1.0f;

            // Scale by 2*PI only at the moment of sine calculation
            float sineSample = sinf(normPhase * 6.2831853f);
            
            float triSample = 0.0f;
            if (normPhase < 0.25f)      triSample = normPhase * 4.0f;
            else if (normPhase < 0.75f) triSample = 2.0f - (normPhase * 4.0f);
            else                        triSample = (normPhase * 4.0f) - 4.0f;

            float sawSample = 1.0f - (normPhase * 2.0f);
            float sqrSample = (normPhase < 0.5f) ? 0.5f : -0.5f;

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
    // Dynamic processing: receives track index to query appropriate variables
    float Process(int trackIdx) {
            if (stage1 == ENV1_IDLE && stage2 == ENV2_IDLE && noiseStage == NOISE_IDLE && filterStage == FLT_IDLE) return 0.0f;

            // Fast crossfade choke ramp (256 samples is approx 5.8ms at 44.1kHz)
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
        const StepParams& sp = trk.steps[playhead].params; // Parameter overrides on the active step

        // Reusable fallback helper: if a step parameter is unlocked (-1), use the global track default
        auto GetParam = [](int stepVal, int trackVal) {
            return (stepVal == -1) ? trackVal : stepVal;
        };

        // Resolve active Analog value contextually (Reuses fmFeedback parameter in Parallel mode)
                int analogVal = GetParam(sp.fmFeedback, trk.fmFeedback);
                float analogAmount = (trk.algorithm == ALGO_PARALLEL) ? (analogVal / 99.0f) : 0.0f;
        
        // --- 0. PARAMETER SMOOTHING / GLIDE CALCULATIONS (Per-Sample) ---
        // Combine base parameters with LFO modulation offsets (clamped to safe ranges)
        float targetMorph1 = std::clamp(GetParam(sp.morph, trk.morph) + modMorph1Offset, 0.0f, 99.0f);
        float targetMorph2 = std::clamp(GetParam(sp.morph2, trk.morph2) + modMorph2Offset, 0.0f, 99.0f);
        float targetVol1 = std::clamp(GetParam(sp.volume, trk.volume) + modVol1Offset, 0.0f, 99.0f);
        float targetVol2 = std::clamp(GetParam(sp.volume2, trk.volume2) + modVol2Offset, 0.0f, 99.0f);

        if (smoothMorph1 < 0.0f) {
            smoothMorph1 = targetMorph1;
            smoothMorph2 = targetMorph2;
            smoothVol1 = targetVol1;
            smoothVol2 = targetVol2;
        } else {
            smoothMorph1 += (targetMorph1 - smoothMorph1) * 0.005f;
            smoothMorph2 += (targetMorph2 - smoothMorph2) * 0.005f;
            smoothVol1 += (targetVol1 - smoothVol1) * 0.005f;
            smoothVol2 += (targetVol2 - smoothVol2) * 0.005f;
        }

        // --- 1. PROCESS ENVELOPE 1 (Carrier) ---
                switch (stage1) {
                    case ENV1_ATTACK:  envLevel1 += envAtkRate1; if (envLevel1 >= 1.0f) { envLevel1 = 1.0f; stage1 = ENV1_DECAY; } break;
                    case ENV1_DECAY:   envLevel1 -= envDecRate1; if (envLevel1 <= envSusLevel1) { envLevel1 = envSusLevel1; stage1 = ENV1_SUSTAIN; } break;
                    case ENV1_SUSTAIN: envLevel1 = envSusLevel1; break;
                    case ENV1_RELEASE: envLevel1 -= envRelRate1; if (envLevel1 <= 0.0f) { envLevel1 = 0.0f; stage1 = ENV1_IDLE; } break;
                    default: break;
                }
        // --- 2. PROCESS ENVELOPE 2 (Modulator) ---
                switch (stage2) {
                    case ENV2_ATTACK:  envLevel2 += envAtkRate2; if (envLevel2 >= 1.0f) { envLevel2 = 1.0f; stage2 = ENV2_DECAY; } break;
                    case ENV2_DECAY:   envLevel2 -= envDecRate2; if (envLevel2 <= envSusLevel2) { envLevel2 = envSusLevel2; stage2 = ENV2_SUSTAIN; } break;
                    case ENV2_SUSTAIN: envLevel2 = envSusLevel2; break;
                    case ENV2_RELEASE: envLevel2 -= envRelRate2; if (envLevel2 <= 0.0f) { envLevel2 = 0.0f; stage2 = ENV2_IDLE; } break;
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
                    case FLT_ATTACK:  filterEnvLevel += filterAtkRate; if (filterEnvLevel >= 1.0f) { filterEnvLevel = 1.0f; filterStage = FLT_DECAY; } break;
                    case FLT_DECAY:   filterEnvLevel -= filterDecRate; if (filterEnvLevel <= filterSusLevel) { filterEnvLevel = filterSusLevel; filterStage = FLT_SUSTAIN; } break;
                    case FLT_SUSTAIN: filterEnvLevel = filterSusLevel; break;
                    case FLT_RELEASE: filterEnvLevel -= filterRelRate; if (filterEnvLevel <= 0.0f) { filterEnvLevel = 0.0f; filterStage = FLT_IDLE; } break;
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

                    envAtkRate1 = invSampleRate / GetEnvTime(GetParam(sp.attack, trk.attack));
                    envDecRate1 = invSampleRate / GetEnvTime(GetParam(sp.decay, trk.decay));
                    envRelRate1 = invSampleRate / GetEnvTime(GetParam(sp.release, trk.release));
                    envSusLevel1 = GetParam(sp.sustain, trk.sustain) / 99.0f;

                    envAtkRate2 = invSampleRate / GetEnvTime(GetParam(sp.attack2, trk.attack2));
                    envDecRate2 = invSampleRate / GetEnvTime(GetParam(sp.decay2, trk.decay2));
                    envRelRate2 = invSampleRate / GetEnvTime(GetParam(sp.release2, trk.release2));
                    envSusLevel2 = GetParam(sp.sustain2, trk.sustain2) / 99.0f;

                    noiseAtkRate = invSampleRate / GetEnvTime(GetParam(sp.noiseAttack, trk.noiseAttack));
                    noiseDecRate = invSampleRate / GetEnvTime(GetParam(sp.noiseDecay, trk.noiseDecay));
                    noiseHoldSamples = (uint32_t)(GetEnvTime(GetParam(sp.noiseHold, trk.noiseHold)) * g_sampleRate);

                    filterAtkRate = invSampleRate / GetEnvTime(GetParam(sp.filterAttack, trk.filterAttack));
                    filterDecRate = invSampleRate / GetEnvTime(GetParam(sp.filterDecay, trk.filterDecay));
                    filterRelRate = invSampleRate / GetEnvTime(GetParam(sp.filterRelease, trk.filterRelease));
                    filterSusLevel = GetParam(sp.filterSustain, trk.filterSustain) / 99.0f;
            // Reset mod offsets
            modCutoffOffset = 0.0f;
            modResOffset = 0.0f;
            modVol1Offset = 0.0f;
            modVol2Offset = 0.0f;
            modMorph1Offset = 0.0f;
            modMorph2Offset = 0.0f;
            modPitchOffset = 0.0f;

            // Pull and sum modulation offsets from all 16 system LFO outputs targeting this voice
            for (int srcTrkIdx = 0; srcTrkIdx < 8; ++srcTrkIdx) {
                const Track& srcTrk = tracks[srcTrkIdx];

                // Check LFO 1 Slots
                for (int s = 0; s < 3; ++s) {
                    const ModSlot& m = srcTrk.lfo1Slots[s];
                    if (m.destType == 1 && m.destTrack == trackIdx) {
                        float modVal = g_globalLFOValues[srcTrkIdx][0] * (m.depth / 99.0f);
                        if (m.destParam == DEST_CUTOFF)          modCutoffOffset += modVal * 99.0f;
                        else if (m.destParam == DEST_RESONANCE)  modResOffset += modVal * 99.0f;
                        else if (m.destParam == DEST_VOLUME)     modVol1Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_MORPH1)     modMorph1Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_MORPH2)     modMorph2Offset += modVal * 99.0f;
                        else if (m.destParam == DEST_PITCH)      modPitchOffset += modVal * 12.0f; // Scale to pitch semitones (max +/-1 octave)
                    }
                }

                // Check LFO 2 Slots
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
                    }
                }
            }

            float targetCutoff = std::clamp(GetParam(sp.filterCutoff, trk.filterCutoff) + modCutoffOffset, 0.0f, 99.0f);
            if (smoothCutoff < 0.0f) {
                smoothCutoff = targetCutoff;
            } else {
                smoothCutoff += (targetCutoff - smoothCutoff) * 0.1f;
            }

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

        // --- 7. FREQUENCY CALCULATIONS ---
        float semitoneOffset1 = GetParam(sp.coarse, trk.coarse) + (GetParam(sp.fine, trk.fine) / 100.0f);
        float freq1 = baseFreq * pitchModFactor * powf(2.0f, (semitoneOffset1 + modPitchOffset) / 12.0f); // Modulated by pitch envelope

        float finalSample = 0.0f;

        if (trk.algorithm == ALGO_PARALLEL) {
                            // ==========================================
                            // ALGORITHM A: DUAL-OSCILLATOR MIX (PARALLEL)
                            // ==========================================
                            float semitoneOffset2 = GetParam(sp.coarse2, trk.coarse2) + (GetParam(sp.fine2, trk.fine2) / 100.0f);
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
                                    // Calculate how many octaves the note is away from center C4 (261.63 Hz)
                                    float octavesFromCenter = log2f(baseFreq / 261.63f);

                                    // As you play further from C4, Osc 1 drifts slightly sharp, and Osc 2 flat.
                                    // At 100% Analog, this creates up to ~0.08% divergence per octave (~1.5 cents).
                                    float trackingDivergence = octavesFromCenter * analogAmount * 0.0008f;
                                    freq1AnalogScale += trackingDivergence;
                                    freq2AnalogScale -= trackingDivergence;
                                }

                                float finalFreq1 = freq1 * freq1AnalogScale;
                                float finalFreq2 = freq2 * freq2AnalogScale;

                                float rawOsc1 = ProcessWave(phase1, (int)smoothMorph1);
                                float rawOsc2 = ProcessWave(phase2, (int)smoothMorph2);

                                // Apply 1-pole low pass filter waveform softening (old 100% softening occurs at ~75%)
                                if (analogAmount > 0.0f) {
                                    float lpCoeff = 1.0f - (analogAmount * 0.75f); // Transitions down to 0.25 (darker, warmer)
                                    osc1LPState += lpCoeff * (rawOsc1 - osc1LPState);
                                    osc2LPState += lpCoeff * (rawOsc2 - osc2LPState);
                                    rawOsc1 = osc1LPState;
                                    rawOsc2 = osc2LPState;
                                }

                            float drive = 1.0f + (smoothVol1 / 33.0f);
                            float saturatedOsc1 = ApplySaturation(rawOsc1, drive);

                            float osc1 = saturatedOsc1 * envLevel1 * (smoothVol1 / 99.0f);
                            float osc2 = rawOsc2 * envLevel2 * (smoothVol2 / 99.0f);

                            finalSample = (osc1 + osc2) * 0.5f;

                            // Normalized phase increments: no multiplication by 2*PI needed!
                            phase1 += finalFreq1 / (float)g_sampleRate;
                            phase2 += finalFreq2 / (float)g_sampleRate;
                        }
        else {
                    // ==========================================
                    // ALGORITHM B: 2-OP PHASE MODULATION FM (CARRIER / MODULATOR)
                    // ==========================================
                    float ratio = GetParam(sp.coarse2, trk.coarse2) + (GetParam(sp.fine2, trk.fine2) / 100.0f);
                    if (ratio < 0.05f) ratio = 0.05f;
                    float freq2 = freq1 * ratio;

                    // Modulator self-feedback: scaled by 1/(2*PI) so feedback scale simplifies to exactly 0.5!
                    float feedbackScale = (GetParam(sp.fmFeedback, trk.fmFeedback) / 99.0f) * 0.5f;
                    float feedbackPhase = phase2 + lastModOutput * feedbackScale;

                    // Compute Modulator dry shape
                    float modDry = ProcessWave(feedbackPhase, (int)smoothMorph2) * envLevel2;
                    lastModOutput = modDry;

                    // Quadratic Index Scaling: folded 1/(2*PI) division directly into the index multiplier!
                    // 8.0f / (2 * PI) = 1.2732395f
                    float normIdx = smoothVol2 / 99.0f;
                    float index = normIdx * normIdx * 1.2732395f;

                    // Modulate Carrier phase
                    float modulatedPhase1 = phase1 + modDry * index;
                    float carrier = ProcessWave(modulatedPhase1, (int)smoothMorph1) * envLevel1 * (smoothVol1 / 99.0f);

                    finalSample = carrier;

                    // Normalized phase increments
                    phase1 += freq1 / (float)g_sampleRate;
                    phase2 += freq2 / (float)g_sampleRate;
                }

        // Wrap normalized phases
                if (phase1 >= 1.0f) phase1 -= 1.0f;
                if (phase2 >= 1.0f) phase2 -= 1.0f;

        // --- 8. PROCESS WHITE NOISE TRANSIENT ---
        float rawNoise = FastRandFloat(randomSeed) * 2.0f - 1.0f;
        float scaledNoise = rawNoise * noiseEnvLevel * (GetParam(sp.noiseVolume, trk.noiseVolume) / 99.0f);

        // Mix noise with synthesized waves BEFORE filter stage
        float combinedSignal = finalSample + scaledNoise;
        int fType = GetParam(sp.filterType, trk.filterType);

        // --- 9. RUN THROUGH SILKY STATE-VARIABLE FILTER (SVF) ---
                float targetMasterVol = GetParam(sp.masterVolume, trk.masterVolume) / 99.0f; // <--- Add this line
                       return filter.process(combinedSignal, fType, analogAmount) * velocityScale * chokeVolume * targetMasterVol; // <--- Multiply here
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

    // --- ADD THESE BLOCK-RATE PARAMETER ARRAYS ---
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

    // 1 Tick = 60.0 / (tempo * 24.0) = 2.5 / tempo seconds
    double tickLengthSeconds = 2.5 / tempo;
    ma_uint32 samplesPerTick = (ma_uint32)(tickLengthSeconds * g_sampleRate);
    ma_uint32 samplesPerStep = samplesPerTick * 6;

    // Track active sequencer state transition to prevent live playing choke on STOP
    static bool lastPlayingState = false;
    if (isPlaying != lastPlayingState) {
        if (!isPlaying) {
            // Just stopped: cleanly release all voices exactly once
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

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        float mixedSample = 0.0f;

        // Block-rate generator interval trigger (every 64 samples)
                static uint32_t lfoBlockCounter = 9999; // Force update on first sample
                lfoBlockCounter++;
                if (lfoBlockCounter >= 64) {
                    lfoBlockCounter = 0;
                    UpdateGlobalLFOs();

                    // Calculate tape parameters once per 64-sample block for all 8 tracks
                    for (int t = 0; t < 8; ++t) {
                        const Track& trk = tracks[t];
                        
                        // Helper lambda to safely obtain active step params
                        int currentStepIdx = (trk.localTick / 6) % trk.stepLength;
                        if (currentStepIdx < 0) currentStepIdx = 0; // Safety guard
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
                                // LFO 1 Slot Taps
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
                                // LFO 2 Slot Taps
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
                }

        // Check for external MIDI Start/Stop triggers inside the sample block
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

            // Resolve active master length ticks (0 represents INF, fallback to 192 ticks)
            int masterTicksLimit = (masterLength == 0) ? 192 : (masterLength * 6);

            if (g_useExternalMidiClock) {
                // Consume asynchronous external MIDI clock tick (24 PPQN)
                if (g_externalMidiTicksQueued > 0) {
                    g_externalMidiTicksQueued--; // Consume one queued tick
                    
                    if (g_currentTick == -1) {
                        g_currentTick = 0;
                        g_tickSampleAccumulator = 0;
                    } else {
                        g_currentTick = (g_currentTick + 1) % masterTicksLimit;
                    }
                    tickTriggered = true;
                }
            } else {
                // Run off internal sample accumulator
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

            // Advance track local clocks if a master clock tick occurred
            if (tickTriggered) {
                for (int t = 0; t < 8; ++t) {
                    int nextTick = (tracks[t].localTick + 1) % (tracks[t].stepLength * 6);
                    
                    // If the track clock completes a full loop, increment its bar counter
                    if (nextTick == 0 && tracks[t].localTick >= 0) {
                        g_trackBarCount[t]++;
                    }
                    
                    tracks[t].localTick = nextTick;
                }

                // Handle bar increments, pattern swaps, and master loop alignments
                if (g_currentTick == 0) {
                    for (int t = 0; t < 8; ++t) {
                        g_trackBarCount[t]++;
                    }

                    // Quantized downbeat pattern swapper
                    if (queuedPattern != -1) {
                        SwitchPattern(queuedPattern);
                        queuedPattern = -1; 
                        
                        // Flush all track clocks and bar counts on swap
                        for (int t = 0; t < 8; ++t) {
                            tracks[t].localTick = -1;
                            g_trackBarCount[t] = 0; 
                        }
                    } else if (masterLength > 0) {
                        // Scenario A: Aligned Polymeter (Pattern Reset)
                        // Reset track clocks and bar counts back on master downbeat
                        for (int t = 0; t < 8; ++t) {
                            tracks[t].localTick = -1;
                            g_trackBarCount[t] = 0; 
                        }
                    }
                }
            }

            // Keep the global visual playhead updated (0 to 31) representing the active track's view
            playhead = (tracks[selectedTrack].localTick / 6) % tracks[selectedTrack].stepLength;

            // 1. Check for active note triggers at the beginning of a tick
            if (tickTriggered) {
                for (int t = 0; t < 8; ++t) {
                    if (tracks[t].muted) continue;

                    // Scan steps specifically up to this track's active step length limit
                    for (int s = 0; s < tracks[t].stepLength; ++s) {
                        const Step& step = tracks[t].steps[s];

                        int triggerTick = (s * 6 + step.microtiming);
                        int localLengthTicks = tracks[t].stepLength * 6;
                        
                        // Wrap microtiming relative to this track's local loop length
                        triggerTick = triggerTick % localLengthTicks;
                        if (triggerTick < 0) triggerTick += localLengthTicks;

                        // Trigger notes relative to track-local clock
                        if (triggerTick == tracks[t].localTick) {
                            if (step.velocity > 0 && !step.note.empty()) {
                                if (EvaluateCondition(step.condition, t)) {
                                    int midiNote = NoteToMidi(step.note);
                                    if (midiNote >= 0) {
                                        float freq = 440.0f * powf(2.0f, (midiNote - 69.0f) / 12.0f);
                                        
                                        // Resolve Pitch Sweep settings (supporting Step-locks)
                                        int depth = (step.params.pitchSweepDepth == -1) ? tracks[t].pitchSweepDepth : step.params.pitchSweepDepth;
                                        int time = (step.params.pitchSweepTime == -1) ? tracks[t].pitchSweepTime : step.params.pitchSweepTime;

                                        int finalPolyMode = GetParam(step.params.polyMode, tracks[t].polyMode);
                                        if (finalPolyMode < 1) finalPolyMode = 1;
                                        if (finalPolyMode > 4) finalPolyMode = 4;

                                        if (tracks[t].engineType == ENGINE_SYNTH) {
                                            g_trackVoiceIndex[t] = g_trackVoiceIndex[t] % finalPolyMode;
                                            int targetIdx = g_trackVoiceIndex[t];

                                            if (g_trackVoices[t][targetIdx].active) {
                                                g_trackVoices[t][targetIdx].Choke(); // Smoothly crossfade/choke oldest voice
                                            }
                                            g_trackVoices[t][targetIdx].Trigger(freq, depth, time, step.velocity, true);
                                            g_trackVoiceIndex[t] = (g_trackVoiceIndex[t] + 1) % finalPolyMode;
                                        } else {
                                            int slot = GetParam(step.params.sampleSlot, tracks[t].sampleSlot);
                                            const int16_t* buffer = g_samplePool[slot].pcmData.data();
                                            uint32_t length = (uint32_t)g_samplePool[slot].pcmData.size();
                                            float noteOffset = (float)(midiNote - 60);

                                            g_samplerVoiceIndex[t] = g_samplerVoiceIndex[t] % finalPolyMode;
                                            int targetIdx = g_samplerVoiceIndex[t];

                                            if (g_samplerVoices[t][targetIdx].active) {
                                                g_samplerVoices[t][targetIdx].Choke(); // Smoothly crossfade/choke oldest voice
                                            }
                                            g_samplerVoices[t][g_samplerVoiceIndex[t]].Trigger(buffer, length, noteOffset, (float)tracks[t].fine2, depth, time, step.velocity, true);
                                            g_samplerVoiceIndex[t] = (g_samplerVoiceIndex[t] + 1) % finalPolyMode;
                                        }

                                        // Set up ratchet queue
                                        if (step.retrigger > 1) {
                                            ActiveRetrig& ar = g_activeRetrigs[t];
                                            ar.midiNote = midiNote;
                                            ar.remainingTriggers = step.retrigger - 1;
                                            ar.sampleInterval = samplesPerStep / step.retrigger;
                                            ar.sampleCounter = 0;
                                            ar.velocity = step.velocity; // Store velocity for ratchets
                                        } else {
                                            g_activeRetrigs[t].remainingTriggers = 0; // Clear queue
                                        }
                                    }
                                }
                            } else if (step.velocity == 0 && !step.note.empty()) {
                                // Explicit Gate Off / Release (ONLY releases if user placed an active gate-off step)
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

            // 2. Update and execute active sample-accurate retriggers
            for (int t = 0; t < 8; ++t) {
                ActiveRetrig& ar = g_activeRetrigs[t];
                if (ar.remainingTriggers > 0 && !tracks[t].muted) {
                    ar.sampleCounter++;
                    if (ar.sampleCounter >= ar.sampleInterval) {
                        ar.sampleCounter = 0;
                        ar.remainingTriggers--;

                        if (ar.midiNote >= 0) {
                            float freq = 440.0f * powf(2.0f, (ar.midiNote - 69.0f) / 12.0f);
                            
                            // Retrieve active step context to resolve step-locked pitch sweeps on ratchets
                            int currentStepIdx = (tracks[t].localTick / 6) % tracks[t].stepLength;
                            const Step& step = tracks[t].steps[currentStepIdx];
                            int depth = (step.params.pitchSweepDepth == -1) ? tracks[t].pitchSweepDepth : step.params.pitchSweepDepth;
                            int time = (step.params.pitchSweepTime == -1) ? tracks[t].pitchSweepTime : step.params.pitchSweepTime;

                            if (tracks[t].engineType == ENGINE_SYNTH) {
                                g_trackVoices[t][g_trackVoiceIndex[t]].Release();
                                g_trackVoices[t][g_trackVoiceIndex[t]].Trigger(freq, depth, time, ar.velocity, true); // Set isSeq = true
                                g_trackVoiceIndex[t] = (g_trackVoiceIndex[t] + 1) % 4; // Clamped to 4
                            } else {
                                int slot = (step.params.sampleSlot == -1) ? tracks[t].sampleSlot : step.params.sampleSlot;
                                const int16_t* buffer = g_samplePool[slot].pcmData.data();
                                uint32_t length = (uint32_t)g_samplePool[slot].pcmData.size();

                                // Calculate keyboard pitch tracking offset relative to root note C4 (MIDI 60)
                                float noteOffset = (float)(ar.midiNote - 60);

                                g_samplerVoices[t][g_samplerVoiceIndex[t]].Release();
                                g_samplerVoices[t][g_samplerVoiceIndex[t]].Trigger(buffer, length, noteOffset, (float)tracks[t].fine2, depth, time, ar.velocity, true); // Set isSeq = true
                                g_samplerVoiceIndex[t] = (g_samplerVoiceIndex[t] + 1) % 4; // Clamped to 4
                            }
                        }
                    }
                }
            }

        } else {
            // Sequencer is stopped: reset counters, but do NOT release voices (keeps live play active) [2]
            g_tickSampleAccumulator = 0;
            g_currentTick = -1;
            for (int t = 0; t < 8; ++t) {
                tracks[t].localTick = -1;
                g_trackBarCount[t] = 0; // Keep bar counts reset on stop
            }
        }

        // --- PRO-GRADE SEND-BUS ROUTING PIPELINE ---
        float masterDryMono = 0.0f;
        float delaySendBusMono = 0.0f;
        float satSendBusMono = 0.0f;
        float reverbSendBusMono = 0.0f;
        float autoPanSendBusMono = 0.0f;

        // Render and sum active outputs dynamically across all 4 polyphonic voices
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
            
            // Normalize polyphony sum to prevent digital clipping
            float normalTrackSum = trackSampleSum * 0.25f;

            // Retrieve active step overrides
            int currentStepIdx = (tracks[t].localTick / 6) % tracks[t].stepLength;
            if (currentStepIdx < 0) currentStepIdx = 0; // Safety guard
            const Step& step = tracks[t].steps[currentStepIdx];

            

            float processedSum = tracks[t].tapeFX.process(
                            normalTrackSum,
                            s_finalMem[t], s_finalHds[t], s_finalSpr[t], s_finalSpd[t], s_finalTet[t],
                            s_finalDrf[t], s_finalDrt[t], s_finalFdb[t], s_finalFsp[t], s_finalFsc[t],
                            s_finalFrz[t], s_finalSmr[t], s_finalSms[t], s_finalMix[t],
                            g_sampleRate
                        );

            // 1. Accumulate Dry Master Bus (Using the modulated processed output)
            masterDryMono += processedSum;

            // 2. Tap and Accumulate Delay Send Bus
            int delSendVal = GetParam(step.params.delaySend, tracks[t].delaySend);
            float delSendNorm = (float)delSendVal / 99.0f;
            delaySendBusMono += processedSum * delSendNorm;

            // 3. Tap and Accumulate Saturation Send Bus
            int satSendVal = GetParam(step.params.saturationSend, tracks[t].saturationSend);
            float satSendNorm = (float)satSendVal / 99.0f;
            satSendBusMono += processedSum * satSendNorm;

            // 4. Tap and Accumulate Reverb Send Bus
            int revSendVal = GetParam(step.params.reverbSend, tracks[t].reverbSend);
            float revSendNorm = (float)revSendVal / 99.0f;
            reverbSendBusMono += processedSum * revSendNorm;

            // 5. Tap and Accumulate Auto-Pan/Tornado Send Bus
            int panSendVal = GetParam(step.params.autoPanSend, tracks[t].autoPanSend);
            float panSendNorm = (float)panSendVal / 99.0f;
            autoPanSendBusMono += processedSum * panSendNorm;
        }

        // --- STEREO SPLITTING & MASTER SUMMING STAGE ---
        float masterL = (masterDryMono / 8.0f) * 0.5f;
        float masterR = (masterDryMono / 8.0f) * 0.5f;

        float delaySendL = (delaySendBusMono / 8.0f) * 0.5f;
        float delaySendR = (delaySendBusMono / 8.0f) * 0.5f;

        float satSendL = (satSendBusMono / 8.0f) * 0.5f;
        float satSendR = (satSendBusMono / 8.0f) * 0.5f;

        float reverbSendL = (reverbSendBusMono / 8.0f) * 0.5f;
        float reverbSendR = (reverbSendBusMono / 8.0f) * 0.5f;

        float panSendL = (autoPanSendBusMono / 8.0f) * 0.5f;
        float panSendR = (autoPanSendBusMono / 8.0f) * 0.5f;

        // --- PROCESS STEREO MASTER FX GROUP (SENDS Matrix) ---

        // --- EVALUATE GLOBAL LFO MODULATIONS ON MASTER FX SENDS ---
        float revMixMod = 0.0f;
        float delMixMod = 0.0f;
        float satMixMod = 0.0f;
        float panMixMod = 0.0f;

        for (int t = 0; t < 8; ++t) {
            for (int s = 0; s < 3; ++s) {
                // LFO 1
                if (tracks[t].lfo1Slots[s].destType == 2) {
                    float modVal = g_globalLFOValues[t][0] * (tracks[t].lfo1Slots[s].depth / 99.0f);
                    if (tracks[t].lfo1Slots[s].destParam == DEST_REV_MIX)       revMixMod += modVal;
                    else if (tracks[t].lfo1Slots[s].destParam == DEST_DEL_MIX)  delMixMod += modVal;
                    else if (tracks[t].lfo1Slots[s].destParam == DEST_SAT_MIX)  satMixMod += modVal;
                    else if (tracks[t].lfo1Slots[s].destParam == DEST_PAN_MIX)  panMixMod += modVal;
                }
                // LFO 2
                if (tracks[t].lfo2Slots[s].destType == 2) {
                    float modVal = g_globalLFOValues[t][1] * (tracks[t].lfo2Slots[s].depth / 99.0f);
                    if (tracks[t].lfo2Slots[s].destParam == DEST_REV_MIX)       revMixMod += modVal;
                    else if (tracks[t].lfo2Slots[s].destParam == DEST_DEL_MIX)  delMixMod += modVal;
                    else if (tracks[t].lfo2Slots[s].destParam == DEST_SAT_MIX)  satMixMod += modVal;
                    else if (tracks[t].lfo2Slots[s].destParam == DEST_PAN_MIX)  panMixMod += modVal;
                }
            }
        }

        // 1. Process Reverb Stage (Waves)
        float reverbWetL = 0.0f;
        float reverbWetR = 0.0f;
        g_masterReverb.process(
            reverbSendL, reverbSendR, reverbWetL, reverbWetR,
            (float)globalFX.reverbDecay / 99.0f,
            (float)globalFX.reverbSize / 99.0f,
            (float)globalFX.reverbPredelay / 99.0f,
            1.0f, // 100% wet send
            (float)g_sampleRate
        );
        float globalRevMix = std::clamp(((float)globalFX.reverbMix / 99.0f) + revMixMod, 0.0f, 1.0f);
        reverbWetL *= globalRevMix;
        reverbWetR *= globalRevMix;

        // 2. Process Stereo Delay Stage (Rain with optional Ping-Pong)
        float delayWetL = 0.0f;
        float delayWetR = 0.0f;
        g_masterDelay.process(
            delaySendL, delaySendR, delayWetL, delayWetR,
            (float)globalFX.delayTime / 99.0f,
            (float)globalFX.delayFeedback / 99.0f,
            globalFX.delayPingPong,
            1.0f, // 100% wet send
            (float)g_sampleRate
        );
        float globalDelayMixNorm = std::clamp(((float)globalFX.delayMix / 99.0f) + delMixMod, 0.0f, 1.0f);
        delayWetL *= globalDelayMixNorm;
        delayWetR *= globalDelayMixNorm;

        // 3. Process Saturated Bus Glue Compressor Stage (Sun)
        float satWetL = 0.0f;
        float satWetR = 0.0f;
        g_masterCompressor.process(
            satSendL, satSendR, satWetL, satWetR,
            (float)globalFX.satLevel / 99.0f,
            (float)globalFX.satSymmetry / 99.0f,
            (float)globalFX.satOverdrive / 99.0f, // TGT
            1.0f, // 100% wet send
            (float)g_sampleRate
        );
        float globalSatMixNorm = std::clamp(((float)globalFX.satMix / 99.0f) + satMixMod, 0.0f, 1.0f);
        satWetL *= globalSatMixNorm;
        satWetR *= globalSatMixNorm;

        // 4. Process Swirling Tornado Chorus & Auto-Pan Stage (Tornado)
        float panWetL = 0.0f;
        float panWetR = 0.0f;
        g_masterTornado.process(
            panSendL, panSendR, panWetL, panWetR,
            (float)globalFX.autoPanTime / 99.0f,
            (float)globalFX.autoPanFeedback / 99.0f,
            (float)globalFX.autoPanWidth / 99.0f,
            1.0f, // 100% wet send
            (float)g_sampleRate
        );
        float globalPanMix = std::clamp(((float)globalFX.autoPanMix / 99.0f) + panMixMod, 0.0f, 1.0f);
        panWetL *= globalPanMix;
        panWetR *= globalPanMix;

        // 5. Interleave Left and Right channels into the Stereo Hardware Output Buffer
        pOutputF[2 * i]     = masterL + reverbWetL + delayWetL + satWetL + panWetL; // Left
        pOutputF[2 * i + 1] = masterR + reverbWetR + delayWetR + satWetR + panWetR; // Right
    }

    // Calculate real-time CPU thread load
    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();
    double expectedSec = (double)frameCount / g_sampleRate;
    float instantCpu = (float)((elapsedSec / expectedSec) * 100.0);
    if (instantCpu > 100.0f) instantCpu = 100.0f; // Limit clamp to 100%

    static float s_smoothedCpu = 0.0f;
    s_smoothedCpu += 0.05f * (instantCpu - s_smoothedCpu); // Leaky integrator smoothing
    g_audioCpuLoad = s_smoothedCpu;
}
// ==========================================
// PUBLIC CONTROLLER INTERFACE
// ==========================================
void InitAudioEngine() {
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_f32;
    deviceConfig.playback.channels = 2; // Stereo
    deviceConfig.sampleRate        = (ma_uint32)g_sampleRate;
    deviceConfig.dataCallback      = ma_audio_callback;
    
    deviceConfig.periodSizeInMilliseconds = 50; // <--- CHANGED to 50ms for stable Pi playback

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

    // 9. Apply 14-bit resolution reduction crunch
        menuFeedback = "DIAG: crunching bits";
        for (size_t i = 0; i < tempBuffer.size(); ++i) {
            tempBuffer[i] = (tempBuffer[i] >> 2) << 2;
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

    int finalPolyMode = trk.polyMode;
    if (finalPolyMode < 1) finalPolyMode = 1;
    if (finalPolyMode > 4) finalPolyMode = 4;

    if (trk.engineType == ENGINE_SYNTH) {
        g_trackVoiceIndex[trackIdx] = g_trackVoiceIndex[trackIdx] % finalPolyMode;
        int targetIdx = g_trackVoiceIndex[trackIdx];

        if (g_trackVoices[trackIdx][targetIdx].active) {
            g_trackVoices[trackIdx][targetIdx].Choke();
        }
        g_trackVoices[trackIdx][targetIdx].Trigger(freq, depth, time, velocity, false);
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
    // Releases any active voices playing this specific MIDI note on the track
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
                    g_samplerVoices[trackIdx][v].Release();
                }
            }
        }
    }
bool IsSynthVoiceActive(int trackIdx, int voiceIdx) {
    if (trackIdx < 0 || trackIdx >= 8 || voiceIdx < 0 || voiceIdx >= 4) return false;
    return g_trackVoices[trackIdx][voiceIdx].active;
}
