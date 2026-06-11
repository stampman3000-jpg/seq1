#pragma once
#include "Common.hpp"
#include <vector>

// Global budget limits to keep CPU load safe on Raspberry Pi
extern int g_globalActiveGrains;
constexpr int MAX_GLOBAL_GRAINS = 128;

// Representing a single active playing grain inside the granular engine
struct Grain {
    uint32_t startSample = 0;
    uint32_t currentOffset = 0;
    uint32_t durationSamples = 0;
    float playbackSpeed = 1.0f;
    bool active = false;
};

// Unified Voice designed for standard sample playback, slicing, and granular synthesis
struct SamplerVoice {
    // Pointer to the shared crunched sample buffer in memory
    const int16_t* sampleBuffer = nullptr;
    uint32_t sampleLengthSamples = 0;

    // Standard / Slice Playback state
    float playheadPosition = 0.0f;
    bool active = false;

    // Smooth parameter states to eliminate transition clicks
    float smoothCutoff = -1.0f;
    float smoothVol = -1.0f;

    // Running modulation offsets evaluated at block-rate
    float modCutoffOffset = 0.0f;
    float modResOffset = 0.0f;
    float modVolOffset = 0.0f;
    float modPitchOffset = 0.0f;
    float modMorphOffset = 0.0f; // Maps to grain position / slice selection offset
    
    // Voice-local thread-safe random seed state
    uint32_t randomSeed = 0x12345678;
    
    // Envelope 1 (Volume Envelope matching Page 3)
    float envLevel = 0.0f;
    enum EnvStage { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } stage = ENV_IDLE;

    // Pitch Sweep Modulation States
    float pitchModFactor = 1.0f;
    float pitchDecayRate = 1.0f;

    // Standard Keyboard Pitch Tracking State
    float notePitchOffset = 0.0f;

    // Velocity Gain Scaling State
    float velocityScale = 1.0f;

    // Filter & Filter Envelope States
    SvfFilter filter;
    float filterEnvLevel = 0.0f;
    enum FilterEnvStage { FLT_IDLE, FLT_ATTACK, FLT_DECAY, FLT_SUSTAIN, FLT_RELEASE } filterStage = FLT_IDLE;
    uint32_t filterUpdateCounter = 0;

    // Voice Origin Tracker (Isolates live play from sequencer gate choking)
    bool triggeredBySequencer = false;

    // Choke state variables
    bool choking = false;
    float chokeVolume = 1.0f;

    // Granular Engine State
    static constexpr int MAX_GRAINS = 16; // Pre-allocated pool to prevent real-time heap allocation
    Grain grainPool[MAX_GRAINS];
    uint32_t samplesSinceLastGrain = 0;

    // Interface Functions
    void Trigger(const int16_t* buffer, uint32_t length, float pitchCoarse, float pitchFine, int depth, int time, int velocity, bool isSeq = false);
    void Release();
    void Choke(); // Click-free fadeout Choke
    float Process(int trackIdx);

private:
    float ProcessStandard(const Track& trk, const StepParams& sp);
    float ProcessGranular(const Track& trk, const StepParams& sp);
    void SpawnGrain(const Track& trk, const StepParams& sp);
};

// Global polyphonic array of Sampler voices expanded to 4-voice polyphony
extern SamplerVoice g_samplerVoices[8][4];
extern int g_samplerVoiceIndex[8];

// Public look-up helper for the diagnostics page
bool IsSamplerVoiceActive(int trackIdx, int voiceIdx);
