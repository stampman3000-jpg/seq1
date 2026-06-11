#include "Sampler_Engine.hpp"
#include "Globals.hpp"
#include <cmath>
#include <cstdlib>

// Import the global modulated LFO matrix populated by the audio callback
extern float g_globalLFOValues[8][2];

// Track active grains globally to protect real-time performance
int g_globalActiveGrains = 0;

// Fast, lock-free LCG pseudo-random number generator
static uint32_t FastRand(uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    return seed;
}

static float FastRandFloat(uint32_t& seed) {
    return (float)FastRand(seed) / 4294967295.0f;
}

// Instantiate Global Sampler Voices with 4-voice polyphony
SamplerVoice g_samplerVoices[8][4];
int g_samplerVoiceIndex[8] = { 0 };

static double g_sampleRate = 44100.0;

// Helper: Converts envelope parameters to time durations in seconds
static float GetEnvTime(int val) {
    float norm = val / 99.0f;
    return 0.0010f * powf(15000.0f, norm * norm * norm);
}

// Fallback helper: uses step overrides if locked (-1), otherwise defaults to track parameters
static int GetParam(int stepVal, int trackVal) {
    return (stepVal == -1) ? trackVal : stepVal;
}

void SamplerVoice::Trigger(const int16_t* buffer, uint32_t length, float pitchCoarse, float pitchFine, int depth, int time, int velocity, bool isSeq) {
    sampleBuffer = buffer;
    sampleLengthSamples = length;
    active = (buffer != nullptr && length > 0);
    choking = false;      // Reset choke flags so sample plays cleanly
    chokeVolume = 1.0f;
    
    // Store keyboard note pitch tracking offset (relative to C4)
    notePitchOffset = pitchCoarse;

    // Store if triggered by sequencer or live
    triggeredBySequencer = isSeq;
    
    playheadPosition = 0.0f;
    envLevel = 0.0f;
    stage = ENV_ATTACK;

    // Seed local PRNG deterministically based on trigger properties
    randomSeed = 0x12345678u + (uint32_t)(pitchCoarse * 100.0f) + length;

    // Map velocity steps (1..3) to linear scaling (0.33 to 1.0)
    velocityScale = (velocity == 1) ? 0.33f : ((velocity == 2) ? 0.66f : 1.0f);
    envLevel = 0.0f;
    stage = ENV_ATTACK;

    // Initialize Exponential Pitch Sweep (pd and st)
    if (depth > 0) {
        float semitones = (depth / 99.0f) * 48.0f; // Clamps to up to +4 octaves
        pitchModFactor = powf(2.0f, semitones / 12.0f);
        
        // Sweep decay duration (from 1ms up to 1.5 seconds)
        float durationSec = 0.001f + (time / 99.0f) * 1.5f;
        pitchDecayRate = expf(-6.9078f / (g_sampleRate * durationSec));
    } else {
        pitchModFactor = 1.0f;
        pitchDecayRate = 1.0f;
    }

    // Initialize Filter State and Envelope
    filterStage = FLT_ATTACK;
    filterEnvLevel = 0.0f;
    filterUpdateCounter = 9999; // Force instant coefficient calculation
    filter.reset();

    // Reset smooth state flags to trigger instant snapping on first process frame
    smoothCutoff = -1.0f;
    smoothVol = -1.0f;

    // Reset mod offsets
    modCutoffOffset = 0.0f;
    modResOffset = 0.0f;
    modVolOffset = 0.0f;
    modPitchOffset = 0.0f;
    modMorphOffset = 0.0f;

    // Clear grain pools safely, maintaining exact synchronization on the global budget counter
    for (int i = 0; i < MAX_GRAINS; ++i) {
        if (grainPool[i].active) {
            grainPool[i].active = false;
            g_globalActiveGrains--;
        }
    }
    samplesSinceLastGrain = 99999; // Force instant grain spawn on first sample
}

void SamplerVoice::Release() {
    if (stage != ENV_IDLE) stage = ENV_RELEASE;
    
    // Release Filter Envelope
    if (filterStage != FLT_IDLE) filterStage = FLT_RELEASE;
}

float SamplerVoice::Process(int trackIdx) {
    if (stage == ENV_IDLE || !active) return 0.0f;

    // Fast crossfade choke ramp
    if (choking) {
        chokeVolume -= 1.0f / 256.0f;
        if (chokeVolume <= 0.0f) {
            chokeVolume = 0.0f;
            active = false;
            choking = false;
            stage = ENV_IDLE;
            for (int i = 0; i < MAX_GRAINS; ++i) {
                if (grainPool[i].active) {
                    grainPool[i].active = false;
                    g_globalActiveGrains--;
                }
            }
            return 0.0f;
        }
    }

    const Track& trk = tracks[trackIdx];
    const StepParams& sp = trk.steps[playhead].params;

    // --- 0. PARAMETER SMOOTHING / GLIDE CALCULATIONS (Per-Sample) ---
    float targetVol = std::clamp(GetParam(sp.volume, trk.volume) + modVolOffset, 0.0f, 99.0f);
    if (smoothVol < 0.0f) {
        smoothVol = targetVol;
    } else {
        smoothVol += (targetVol - smoothVol) * 0.005f;
    }

    // Process Pitch Decay Sweep
    if (pitchModFactor > 1.0f) {
        pitchModFactor = 1.0f + (pitchModFactor - 1.0f) * pitchDecayRate;
    } else {
        pitchModFactor = 1.0f;
    }

    // 1. Process Volume Envelope
    float atk = 1.0f / (g_sampleRate * GetEnvTime(GetParam(sp.attack, trk.attack)));
    float dec = 1.0f / (g_sampleRate * GetEnvTime(GetParam(sp.decay, trk.decay)));
    float rel = 1.0f / (g_sampleRate * GetEnvTime(GetParam(sp.release, trk.release)));
    float sus = GetParam(sp.sustain, trk.sustain) / 99.0f;

    switch (stage) {
        case ENV_ATTACK:  envLevel += atk; if (envLevel >= 1.0f) { envLevel = 1.0f; stage = ENV_DECAY; } break;
        case ENV_DECAY:   envLevel -= dec; if (envLevel <= sus) { envLevel = sus; stage = ENV_SUSTAIN; } break;
        case ENV_SUSTAIN: envLevel = sus; break;
        case ENV_RELEASE:
            envLevel -= rel;
            if (envLevel <= 0.0f) {
                envLevel = 0.0f;
                stage = ENV_IDLE;
                active = false;
                // Flush remaining active grains of this voice to prevent global counter drift
                for (int i = 0; i < MAX_GRAINS; ++i) {
                    if (grainPool[i].active) {
                        grainPool[i].active = false;
                        g_globalActiveGrains--;
                    }
                }
            }
            break;
        default: break;
    }

    // 2. Process Filter Envelope (ADSR)
    float fAtk = 1.0f / (g_sampleRate * GetEnvTime(GetParam(sp.filterAttack, trk.filterAttack)));
    float fDec = 1.0f / (g_sampleRate * GetEnvTime(GetParam(sp.filterDecay, trk.filterDecay)));
    float fRel = 1.0f / (g_sampleRate * GetEnvTime(GetParam(sp.filterRelease, trk.filterRelease)));
    float fSus = GetParam(sp.filterSustain, trk.filterSustain) / 99.0f;

    switch (filterStage) {
        case FLT_ATTACK:  filterEnvLevel += fAtk; if (filterEnvLevel >= 1.0f) { filterEnvLevel = 1.0f; filterStage = FLT_DECAY; } break;
        case FLT_DECAY:   filterEnvLevel -= fDec; if (filterEnvLevel <= fSus) { filterEnvLevel = fSus; filterStage = FLT_SUSTAIN; } break;
        case FLT_SUSTAIN: filterEnvLevel = fSus; break;
        case FLT_RELEASE: filterEnvLevel -= fRel; if (filterEnvLevel <= 0.0f) { filterEnvLevel = 0.0f; filterStage = FLT_IDLE; } break;
        default: break;
    }

    // 3. Block-Rate SVF Coefficients Update (Every 64 Samples)
    filterUpdateCounter++;
    if (filterUpdateCounter >= 64) {
        filterUpdateCounter = 0;

        // Reset mod offsets
        modCutoffOffset = 0.0f;
        modResOffset = 0.0f;
        modVolOffset = 0.0f;
        modPitchOffset = 0.0f;
        modMorphOffset = 0.0f;

        // Pull and sum modulation offsets from all 16 global LFO outputs targeting this sampler voice
        for (int srcTrkIdx = 0; srcTrkIdx < 8; ++srcTrkIdx) {
            const Track& srcTrk = tracks[srcTrkIdx];

            // Check LFO 1 Slots
            for (int s = 0; s < 3; ++s) {
                const ModSlot& m = srcTrk.lfo1Slots[s];
                if (m.destType == 1 && m.destTrack == trackIdx) {
                    float modVal = g_globalLFOValues[srcTrkIdx][0] * (m.depth / 99.0f);
                    if (m.destParam == DEST_CUTOFF)          modCutoffOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_RESONANCE)  modResOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_VOLUME)     modVolOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_PITCH)      modPitchOffset += modVal * 12.0f; // Scale to semitones
                    else if (m.destParam == DEST_MORPH1)     modMorphOffset += modVal * 99.0f; // Modulates position
                }
            }

            // Check LFO 2 Slots
            for (int s = 0; s < 3; ++s) {
                const ModSlot& m = srcTrk.lfo2Slots[s];
                if (m.destType == 1 && m.destTrack == trackIdx) {
                    float modVal = g_globalLFOValues[srcTrkIdx][1] * (m.depth / 99.0f);
                    if (m.destParam == DEST_CUTOFF)          modCutoffOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_RESONANCE)  modResOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_VOLUME)     modVolOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_PITCH)      modPitchOffset += modVal * 12.0f;
                    else if (m.destParam == DEST_MORPH1)     modMorphOffset += modVal * 99.0f;
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

        // Recalculate coefficients
        filter.calculateCoefficients(finalCutoffHz, resNorm, (float)g_sampleRate);
    }

    // 4. Select Processing Pipeline Contextually
    float sampleOut = 0.0f;
    if (trk.algorithm == ALGO_SAMPLE) {
        sampleOut = ProcessStandard(trk, sp);
    } else {
        sampleOut = ProcessGranular(trk, sp);
    }

    // 5. Pump the sampler volume to 4.5x to match the synths (utilizing smoothed volume)
    float boosted = sampleOut * envLevel * (smoothVol / 99.0f) * 4.5f;

    // 6. Warm cubic soft-clipper protects against digital clipping
    if (boosted > 1.0f)  boosted = 1.0f;
    if (boosted < -1.0f) boosted = -1.0f;
    float saturated = boosted - (boosted * boosted * boosted) / 3.0f;

    int fType = GetParam(sp.filterType, trk.filterType);

    // --- 7. Run through State-Variable Filter (SVF) Stage & Scale by Note Velocity ---
        return filter.process(saturated, fType) * velocityScale * chokeVolume;
}

float SamplerVoice::ProcessStandard(const Track& trk, const StepParams& sp) {
    if (sampleBuffer == nullptr || sampleLengthSamples == 0) return 0.0f;

    // Linear mapping from 0..99 parameters to samples
    uint32_t startIdx = (uint32_t)((GetParam(sp.sampleStart, trk.sampleStart) / 99.0f) * sampleLengthSamples);
    uint32_t lengthVal = (uint32_t)((GetParam(sp.sampleLength, trk.sampleLength) / 99.0f) * sampleLengthSamples);
    uint32_t endIdx = startIdx + lengthVal;
    if (endIdx > sampleLengthSamples) endIdx = sampleLengthSamples;

    // Pitch speed factor modulated dynamically by envelope, keyboard tracking, and LFO modulation
    float baseSpeed = 32000.0f / 44100.0f;
    float semitoneOffset = notePitchOffset + GetParam(sp.sampleTune, trk.sampleTune) + (GetParam(sp.fine2, trk.fine2) / 100.0f) + modPitchOffset;
    float playbackSpeed = baseSpeed * pitchModFactor * powf(2.0f, semitoneOffset / 12.0f);

    // Apply Slice Divisions if enabled
    int sdiv = trk.sliceDivisions;
    if (sdiv < 1) sdiv = 1;
    uint32_t totalRange = endIdx - startIdx;
    uint32_t sliceWidth = totalRange / sdiv;
    
    // Choose active slice based on modulated position/start offset
    int activeSlice = std::clamp((int)(GetParam(sp.grainPosition, trk.grainPosition) + modMorphOffset), 0, 99) % sdiv;
    uint32_t sliceStart = startIdx + activeSlice * sliceWidth;
    uint32_t sliceEnd = sliceStart + sliceWidth;

    // --- LOOP END FUNCTIONALITY (LE) ---
    // Read Loop Start & Loop End parameters (0..99) scaled to the active slice width
    uint32_t loopStartOffset = (uint32_t)((GetParam(sp.loopStart, trk.loopStart) / 99.0f) * sliceWidth);
    uint32_t loopEndOffset = (uint32_t)((GetParam(sp.loopEnd, trk.loopEnd) / 99.0f) * sliceWidth);
    
    if (loopEndOffset <= loopStartOffset) loopEndOffset = sliceWidth;

    uint32_t boundaryStart = sliceStart;
    uint32_t boundaryEnd = sliceStart + loopEndOffset;
    if (boundaryEnd > sliceEnd) boundaryEnd = sliceEnd;

    // Interpolate playhead index
    uint32_t currentFrame = boundaryStart + (uint32_t)playheadPosition;
    float outSample = 0.0f;

    if (currentFrame < boundaryEnd) {
        outSample = sampleBuffer[currentFrame] / 32768.0f;
    }

    playheadPosition += playbackSpeed;

    // Wrap loop boundary if LP mode is active
    if (boundaryStart + playheadPosition >= boundaryEnd) {
        if (GetParam(sp.sampleLoop, trk.sampleLoop) == 1) {
            playheadPosition = (float)loopStartOffset; // Loop exactly back to LS
        } else {
            active = false;
            stage = ENV_IDLE;
        }
    }

    return outSample;
}

float SamplerVoice::ProcessGranular(const Track& trk, const StepParams& sp) {
    if (sampleBuffer == nullptr || sampleLengthSamples == 0) return 0.0f;

    // 1. Process continuous grain-spawning rate
    samplesSinceLastGrain++;
    
    // Scale density (0..99) to a highly dense interval (from 1.5ms up to 150ms)
    int density = GetParam(sp.grainDensity, trk.grainDensity);
    float normDensity = density / 99.0f;
    float spawnIntervalSec = 0.0015f + (1.0f - normDensity) * 0.1485f;
    uint32_t spawnIntervalSamples = (uint32_t)(spawnIntervalSec * g_sampleRate);

    if (samplesSinceLastGrain >= spawnIntervalSamples) {
        samplesSinceLastGrain = 0;
        SpawnGrain(trk, sp);
    }

    // 2. Process and sum all active grains
    float sumGrains = 0.0f;
    int activeGrainsCount = 0;

    for (int i = 0; i < MAX_GRAINS; ++i) {
        Grain& g = grainPool[i];
        if (g.active) {
            // Read forward or backward safely depending on active playhead direction
            int32_t currentIdx = (g.playbackSpeed < 0.0f) ?
                ((int32_t)g.startSample - (int32_t)g.currentOffset) :
                ((int32_t)g.startSample + (int32_t)g.currentOffset);

            // Bounds protect inside the sample
            if (currentIdx < 0) currentIdx = 0;
            if (currentIdx >= (int32_t)sampleLengthSamples) currentIdx = sampleLengthSamples - 1;

            float sample = sampleBuffer[currentIdx] / 32768.0f;

            // Scale by cosine window to prevent digital clicks
            float progress = (float)g.currentOffset / g.durationSamples;
            float window = sinf(progress * 3.14159265f); // Smooth raise and fade

            sumGrains += sample * window;

            g.currentOffset += (uint32_t)std::abs(g.playbackSpeed);
            if (g.currentOffset >= g.durationSamples) {
                g.active = false; // Grain finished
                g_globalActiveGrains--; // Decrement global count
            }
            activeGrainsCount++;
        }
    }

    // Normalize overlapping grains using Root-Mean-Square scaling to maintain stable volume
    return activeGrainsCount > 0 ? sumGrains / sqrtf(activeGrainsCount) : 0.0f;
}

void SamplerVoice::SpawnGrain(const Track& trk, const StepParams& sp) {
    // If we have hit our global active budget limit, reject the spawn to protect the DSP loop
    if (g_globalActiveGrains >= MAX_GLOBAL_GRAINS) {
        return;
    }

    // Look for an inactive slot in our pre-allocated pool
    for (int i = 0; i < MAX_GRAINS; ++i) {
        if (!grainPool[i].active) {
            Grain& g = grainPool[i];
            
            // 1. Linear base playhead position modulated by LFO offset
            float normPos = std::clamp(GetParam(sp.grainPosition, trk.grainPosition) + modMorphOffset, 0.0f, 99.0f) / 99.0f;
            uint32_t baseStart = (uint32_t)(normPos * sampleLengthSamples);

            // 2. Playhead Position Scatter (Up to 100% of the entire file length)
            int scatter = GetParam(sp.grainScatter, trk.grainScatter);
            if (scatter > 0) {
                int maxScatterSamples = (int)((scatter / 99.0f) * sampleLengthSamples);
                // Compute random offset using lock-free PRNG
                int range = maxScatterSamples * 2;
                int randomOffset = (range > 0) ? (int)(FastRand(randomSeed) % range) - maxScatterSamples : 0;
                baseStart = (uint32_t)std::clamp((int)baseStart + randomOffset, 0, (int)sampleLengthSamples - 1);
            }

            // 3. Duration (Size) of grain (Expanded from 2ms up to 1000ms)
            float normSize = GetParam(sp.grainSize, trk.grainSize) / 99.0f;
            float durationSec = 0.002f + normSize * 0.998f;
            g.durationSamples = (uint32_t)(durationSec * g_sampleRate);

            // Speed matching pitch CRS / FINE modulated by envelope, keyboard tracking, and LFO modulation
            float baseSpeed = 32000.0f / 44100.0f;
            float semitoneOffset = notePitchOffset + GetParam(sp.sampleTune, trk.sampleTune) + (GetParam(sp.fine2, trk.fine2) / 100.0f) + modPitchOffset;
            float speedFactor = baseSpeed * pitchModFactor * powf(2.0f, semitoneOffset / 12.0f);
            
            // Apply randomized micro-pitch detuning (drift) based on Scatter
            if (scatter > 0) {
                float maxPitchScatter = (scatter / 99.0f) * 1.5f; // Up to +/- 1.5 semitones of detuning
                float pitchOffset = (FastRandFloat(randomSeed) * (maxPitchScatter * 2.0f)) - maxPitchScatter;
                speedFactor *= powf(2.0f, pitchOffset / 12.0f);
            }

            // --- HARMONIC SHIMMER OCTAVE SPRAY ---
            // As Scatter increases, trigger grains on harmonically shifted perfect intervals!
            uint32_t pitchRoll = FastRand(randomSeed) % 100;
            if (scatter > 75) {
                if (pitchRoll < 20)      speedFactor *= 2.0f;    // +1 Octave
                else if (pitchRoll < 35) speedFactor *= 0.5f;    // -1 Octave
                else if (pitchRoll < 50) speedFactor *= 1.5f;    // Perfect 5th
                else if (pitchRoll < 60) speedFactor *= 1.333f;  // Perfect 4th
            } else if (scatter > 50) {
                if (pitchRoll < 25)      speedFactor *= 2.0f;    // +1 Octave
                else if (pitchRoll < 40) speedFactor *= 0.5f;    // -1 Octave
            } else if (scatter > 25) {
                if (pitchRoll < 20)      speedFactor *= 2.0f;    // +1 Octave
            }

            // --- GHOSTLY REVERSED GRAIN SWEEPS ---
            // If grain size is large, we have a randomized chance (up to 40% at max size) of reading backward
            bool reverse = false;
            if (normSize > 0.40f) {
                uint32_t revRoll = FastRand(randomSeed) % 100;
                float revProbability = (normSize - 0.40f) * (40.0f / 0.60f); // Scales up to 40%
                if (revRoll < (uint32_t)revProbability) {
                    reverse = true;
                }
            }

            g.playbackSpeed = reverse ? -speedFactor : speedFactor;
            g.startSample = baseStart;
            g.currentOffset = 0;
            g.active = true;
            g_globalActiveGrains++; // Increment global budget
            break;
        }
    }
}
bool IsSamplerVoiceActive(int trackIdx, int voiceIdx) {
    if (trackIdx < 0 || trackIdx >= 8 || voiceIdx < 0 || voiceIdx >= 4) return false;
    return g_samplerVoices[trackIdx][voiceIdx].active;
}
void SamplerVoice::Choke() {
    choking = true;
    chokeVolume = 1.0f;
}
