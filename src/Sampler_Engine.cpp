#include "Sampler_Engine.hpp"
#include "Globals.hpp"
#include <cmath>
#include <cstdlib>
#include "Audio_Engine.hpp"

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

static inline float MapVolumeToGain(float volVal) {
    if (volVal <= 0.01f) return 0.0f;
    float norm = volVal / 99.0f;
    return norm * norm * norm;
}

// Fallback helper: uses step overrides if locked (-1), otherwise defaults to track parameters
static int GetParam(int stepVal, int trackVal) {
    return (stepVal == -1) ? trackVal : stepVal;
}

void SamplerVoice::Trigger(const int16_t* buffer, uint32_t length, float pitchCoarse, float pitchFine, int depth, int time, int velocity, bool isSeq, int noteLength) {
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
    modSampStartOffset = 0.0f; // ADDED [1]
       modGranSizeOffset = 0.0f;  // ADDED [1]
       modGranDensOffset = 0.0f;  // ADDED [1]
       modGranScatOffset = 0.0f;  // ADDED [1]
    
    // Clear grain pools safely
        for (int i = 0; i < MAX_GRAINS; ++i) {
            if (grainPool[i].active) {
                grainPool[i].active = false;
                g_globalActiveGrains--;
            }
        }
        samplesSinceLastGrain = 99999;

    // Auto-Gate Timer Initialization
           if (isSeq) {
               double tickLengthSeconds = 2.5 / tempo;
               uint32_t samplesPerTick = (uint32_t)(tickLengthSeconds * g_sampleRate);
               uint32_t samplesPerStep = samplesPerTick * 6;
               
               float holdStepsCount = (noteLength == 0) ? 0.85f : ((float)noteLength - 0.15f);
               gateTimerSamples = (uint32_t)(samplesPerStep * holdStepsCount);
               useGateTimer = true;
           } else {
               useGateTimer = false;
               gateTimerSamples = 0;
           }
       }


    void SamplerVoice::Release() {
        if (stage != ENV_IDLE) stage = ENV_RELEASE;
        
        // Release Filter Envelope
        if (filterStage != FLT_IDLE) filterStage = FLT_RELEASE;
    }

float SamplerVoice::Process(int trackIdx) {
    if (stage == ENV_IDLE || !active) return 0.0f;

    const Track& trk = tracks[trackIdx];
    const StepParams& sp = trk.steps[playhead].params;
    int loopMode = (sp.sampleLoop == -1) ? trk.sampleLoop : sp.sampleLoop;

    // 1. Process the Auto-Gate Timer
    if (useGateTimer) {
        if (gateTimerSamples > 0) {
            gateTimerSamples--;
            if (gateTimerSamples == 0) {
                // Only trigger the release stage if looping or granular
                if (loopMode == 1 || trk.algorithm == ALGO_GRANULAR) {
                    Release();
                }
                useGateTimer = false;
            }
        }
    }

    // 2. Fast crossfade choke ramp
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

    // --- 0. PARAMETER SMOOTHING / GLIDE CALCULATIONS (Per-Sample) ---
     float rawTargetVol = std::clamp(GetParam(sp.volume, trk.volume) + modVolOffset, 0.0f, 99.0f);
     float targetVolGain = VolumeCurve(rawTargetVol); // Mapped using our global LUT
     if (smoothVol < 0.0f) {
         smoothVol = targetVolGain;
     } else {
         smoothVol += (targetVolGain - smoothVol) * 0.005f;
     }
    
    // Slew cutoff parameter per-sample to eliminate block-rate zipper noise and step-lock clicks
    float targetCutoff = std::clamp(GetParam(sp.filterCutoff, trk.filterCutoff) + modCutoffOffset, 0.0f, 99.0f);
    if (smoothCutoff < 0.0f) {
        smoothCutoff = targetCutoff;
    } else {
        smoothCutoff += (targetCutoff - smoothCutoff) * 0.004f; // Smooth 5-10ms slew
    }

    // Process Pitch Decay Sweep
    if (pitchModFactor > 1.0f) {
        pitchModFactor = 1.0f + (pitchModFactor - 1.0f) * pitchDecayRate;
    } else {
        pitchModFactor = 1.0f;
    }

    // 1. Process Volume Envelope
     float effectiveSustain = (loopMode == 1 || trk.algorithm == ALGO_GRANULAR) ? envSusLevel : 0.0f;

     switch (stage) {
         case ENV_ATTACK:
             envLevel += envAtkRate;
             if (envLevel >= 1.0f) {
                 envLevel = 1.0f;
                 stage = ENV_DECAY;
             }
             break;
             
         case ENV_DECAY:
             envLevel = effectiveSustain + (envLevel - effectiveSustain) * envDecCoeff;
             if (envLevel - effectiveSustain <= 0.0001f) {
                 envLevel = effectiveSustain;
                 if (effectiveSustain == 0.0f) {
                     stage = ENV_IDLE;
                     active = false;
                 } else {
                     stage = ENV_SUSTAIN;
                 }
             }
             break;
             
         case ENV_SUSTAIN:
             envLevel = envSusLevel;
             break;
             
         case ENV_RELEASE:
             envLevel *= envRelCoeff;
             if (envLevel <= 0.0001f) {
                 envLevel = 0.0f;
                 stage = ENV_IDLE;
                 active = false;
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

    // 3. Block-Rate SVF Coefficients Update (Every 64 Samples)
    filterUpdateCounter++;
    if (filterUpdateCounter >= 64) {
        filterUpdateCounter = 0;
        
        // Recalculate granular spawn interval once per block (Modulated by LFO)
        float rawDensity = GetParam(sp.grainDensity, trk.grainDensity) + modGranDensOffset;
        float clampedDensity = std::clamp(rawDensity, 0.0f, 99.0f);
        float normDensity = clampedDensity / 99.0f;
        float spawnIntervalSec = 0.0015f + (1.0f - normDensity) * 0.1485f;
        cachedSpawnIntervalSamples = (uint32_t)(spawnIntervalSec * g_sampleRate);

        // Recalculate envelope parameters at block rate instead of per sample
        float invSampleRate = 1.0f / (float)g_sampleRate;

        envAtkRate = invSampleRate / GetEnvTime(GetParam(sp.attack, trk.attack));
        float decTime = GetEnvTime(GetParam(sp.decay, trk.decay));
        envDecCoeff = expf(-6.9078f / ((float)g_sampleRate * decTime));
        float relTime = GetEnvTime(GetParam(sp.release, trk.release));
        envRelCoeff = expf(-6.9078f / ((float)g_sampleRate * relTime));
        envSusLevel = GetParam(sp.sustain, trk.sustain) / 99.0f;

        filterAtkRate = invSampleRate / GetEnvTime(GetParam(sp.filterAttack, trk.filterAttack));
        float fDecTime = GetEnvTime(GetParam(sp.filterDecay, trk.filterDecay));
        filterDecCoeff = expf(-6.9078f / ((float)g_sampleRate * fDecTime));
        float fRelTime = GetEnvTime(GetParam(sp.filterRelease, trk.filterRelease));
        filterRelCoeff = expf(-6.9078f / ((float)g_sampleRate * fRelTime));
        filterSusLevel = GetParam(sp.filterSustain, trk.filterSustain) / 99.0f;

        // Reset mod offsets
        modCutoffOffset = 0.0f;
        modResOffset = 0.0f;
        modVolOffset = 0.0f;
        modPitchOffset = 0.0f;
        modMorphOffset = 0.0f;
        modSampStartOffset = 0.0f;
        modGranSizeOffset = 0.0f;
        modGranDensOffset = 0.0f;
        modGranScatOffset = 0.0f;

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
                    else if (m.destParam == DEST_PITCH)      modPitchOffset += modVal * 12.0f;
                    else if (m.destParam == DEST_MORPH1)     modMorphOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_SAMP_START) modSampStartOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_GRAN_SIZE)  modGranSizeOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_GRAN_DENS)  modGranDensOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_GRAN_SCAT)  modGranScatOffset += modVal * 99.0f;
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
                    else if (m.destParam == DEST_SAMP_START) modSampStartOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_GRAN_SIZE)  modGranSizeOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_GRAN_DENS)  modGranDensOffset += modVal * 99.0f;
                    else if (m.destParam == DEST_GRAN_SCAT)  modGranScatOffset += modVal * 99.0f;
                }
            }
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
     float boosted = sampleOut * envLevel * smoothVol * 4.5f;
    
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

    // 1. Calculate startIdx using SS
    float rawStartVal = GetParam(sp.sampleStart, trk.sampleStart) + modSampStartOffset;
    float clampedStartVal = std::clamp(rawStartVal, 0.0f, 99.0f);
    uint32_t startIdx = (uint32_t)((clampedStartVal / 99.0f) * sampleLengthSamples);

    // 2. Calculate endIdx using Length (SE)
    uint32_t lengthVal = (uint32_t)((GetParam(sp.sampleLength, trk.sampleLength) / 99.0f) * sampleLengthSamples);
    uint32_t endIdx = startIdx + lengthVal;
    if (endIdx > sampleLengthSamples) endIdx = sampleLengthSamples;

    // 3. Define the active range window
    uint32_t totalRange = endIdx - startIdx;

    // Pitch speed factor calculations...
    float baseSpeed = 32000.0f / 44100.0f;
    float semitoneOffset = notePitchOffset + GetParam(sp.sampleTune, trk.sampleTune) + (GetParam(sp.fine2, trk.fine2) / 100.0f) + modPitchOffset;
    float playbackSpeed = baseSpeed * pitchModFactor * powf(2.0f, semitoneOffset / 12.0f);

    int sdiv = trk.sliceDivisions;
    if (sdiv < 1) sdiv = 1;

    uint32_t sliceStart = startIdx;
    uint32_t activeRangeWidth = totalRange;

    // --- GRACEFUL SEPARATION OF SINGLE-SAMPLE vs SLICED SAMPLES ---
    if (sdiv == 1) {
        // Continuous Playback: POS behaves as a smooth playhead scrub/offset *inside* your SS and SE bounds!
        float normPos = std::clamp((float)(GetParam(sp.grainPosition, trk.grainPosition) + modMorphOffset), 0.0f, 99.0f) / 99.0f;
        uint32_t posOffset = (uint32_t)(normPos * totalRange);
        
        sliceStart = startIdx + posOffset;
        activeRangeWidth = totalRange - posOffset; // Dynamic remaining space for bounds check
    }
    else {
        // Slice Mode (sdiv > 1): Exactly matches your original, loved slicing math!
        uint32_t sliceWidth = totalRange / sdiv;
        int activeSlice = std::clamp((int)(GetParam(sp.grainPosition, trk.grainPosition) + modMorphOffset), 0, 99) % sdiv;
        
        sliceStart = startIdx + activeSlice * sliceWidth;
        activeRangeWidth = sliceWidth;
    }

    // --- LOOP END FUNCTIONALITY (LE) ---
    // Loops are now cleanly scaled relative to either the active slice width or the remaining single-sample width
    uint32_t loopStartOffset = (uint32_t)((GetParam(sp.loopStart, trk.loopStart) / 99.0f) * activeRangeWidth);
    uint32_t loopEndOffset = (uint32_t)((GetParam(sp.loopEnd, trk.loopEnd) / 99.0f) * activeRangeWidth);
    
    if (loopEndOffset <= loopStartOffset) loopEndOffset = activeRangeWidth;

    uint32_t boundaryStart = sliceStart;
    uint32_t boundaryEnd = sliceStart + loopEndOffset;
    
    // Safety guard boundary clamps
    uint32_t absoluteSliceEnd = sliceStart + activeRangeWidth;
    if (boundaryEnd > absoluteSliceEnd) boundaryEnd = absoluteSliceEnd;

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
            playheadPosition = (float)loopStartOffset;
        } else {
            active = false;
            stage = ENV_IDLE;
        }
    }

    return outSample;
}

float SamplerVoice::ProcessGranular(const Track& trk, const StepParams& sp) {
    if (sampleBuffer == nullptr || sampleLengthSamples == 0) return 0.0f;

    // 1. Process continuous grain-spawning rate using pre-cached interval
    samplesSinceLastGrain++;
    if (samplesSinceLastGrain >= cachedSpawnIntervalSamples) {
        samplesSinceLastGrain = 0;
        SpawnGrain(trk, sp);
    }

    // 2. Process and sum all active grains
    float sumGrains = 0.0f;
    int activeGrainsCount = 0;

    for (int i = 0; i < MAX_GRAINS; ++i) {
        Grain& g = grainPool[i];
        if (g.active) {
            // Calculate the exact floating-point read position
            float readPos = (g.playbackSpeed < 0.0f) ?
                ((float)g.startSample - g.currentOffset) :
                ((float)g.startSample + g.currentOffset);

            // Safe boundary clamping: we clamp up to (length - 1.0001f)
            // to make sure our index+1 offset remains within legal memory limits.
            float maxLimit = (float)sampleLengthSamples - 1.0001f;
            if (maxLimit < 0.0f) maxLimit = 0.0f;
            if (readPos < 0.0f) readPos = 0.0f;
            if (readPos > maxLimit) readPos = maxLimit;

            // Determine floor index and fractional remainder
            int32_t idx0 = (int32_t)readPos;
            int32_t idx1 = idx0 + 1;
            float frac = readPos - (float)idx0;

            // Defensive safety check
            if (idx1 >= (int32_t)sampleLengthSamples) {
                idx1 = sampleLengthSamples - 1;
            }

            // Read the two closest samples
            float sample0 = sampleBuffer[idx0] / 32768.0f;
            float sample1 = sampleBuffer[idx1] / 32768.0f;

            // Perform a smooth linear interpolation between the two points
            float sample = sample0 + frac * (sample1 - sample0);

            // Scale by parabolic window (highly efficient approximation of sine)
            float progress = g.currentOffset / (float)g.durationSamples;
            float window = 4.0f * progress * (1.0f - progress);

            sumGrains += sample * window;

            // CHANGED: Increment offset using float addition (no longer truncating to uint32_t)
            g.currentOffset += std::abs(g.playbackSpeed);
            
            if (g.currentOffset >= (float)g.durationSamples) {
                g.active = false; // Grain finished
                g_globalActiveGrains--; // Decrement global count
            }
            activeGrainsCount++;
        }
    }

    // Normalize overlapping grains using Root-Mean-Square scaling to maintain stable volume
    return activeGrainsCount > 0 ? sumGrains / sqrtf((float)activeGrainsCount) : 0.0f;
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

            // 2. Playhead Position Scatter (Modulated by LFO)
                        float rawScatter = GetParam(sp.grainScatter, trk.grainScatter) + modGranScatOffset;
                        int scatter = std::clamp((int)rawScatter, 0, 99);
            if (scatter > 0) {
                int maxScatterSamples = (int)((scatter / 99.0f) * sampleLengthSamples);
                // Compute random offset using lock-free PRNG
                int range = maxScatterSamples * 2;
                int randomOffset = (range > 0) ? (int)(FastRand(randomSeed) % range) - maxScatterSamples : 0;
                baseStart = (uint32_t)std::clamp((int)baseStart + randomOffset, 0, (int)sampleLengthSamples - 1);
            }

            // 3. Duration (Size) of grain (Modulated by LFO)
                        float rawSizeVal = GetParam(sp.grainSize, trk.grainSize) + modGranSizeOffset;
                        float normSize = std::clamp(rawSizeVal, 0.0f, 99.0f) / 99.0f;
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
            g.currentOffset = 0.0f;
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
