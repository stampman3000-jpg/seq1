#pragma once
#include "Common.hpp"
#include <vector>

// High-quality Stereo Delay with feedback and cross-channel Ping-Pong routing
struct StereoDelay {
    std::vector<float> bufferL;
    std::vector<float> bufferR;
    uint32_t writePtr = 0;
    uint32_t maxDelaySamples = 44100; // 1.0 second maximum delay

    void init(float sampleRate);
    void process(float inL, float inR, float& outL, float& outR, float timeNorm, float feedbackNorm, int pingPong, float mixNorm, float sampleRate);
};

// Studio-grade Master Bus Glue Compressor with dynamic envelope tracking and parallel saturation
struct GlueCompressor {
    float envelope = 0.0f;

    void process(float inL, float inR, float& outL, float& outR, float driveNorm, float symNorm, float tightNorm, float mixNorm, float sampleRate);
};

// Lightweight, lush Stereo Schroeder Reverb with prime-number comb diffusion
struct StereoReverb {
    // Pre-delay buffer
    std::vector<float> preDelayBufferL;
    std::vector<float> preDelayBufferR;
    uint32_t preDelayWritePtr = 0;

    // 4 Parallel Comb Filters per channel
    std::vector<float> combBufferL[4];
    std::vector<float> combBufferR[4];
    uint32_t combWritePtr[4] = {0};
    uint32_t combLengths[4] = {1117, 1373, 1409, 1601}; // Prime sample spacing

    // 2 Cascaded All-Pass Filters per channel to diffuse reflections
    std::vector<float> allPassBufferL[2];
    std::vector<float> allPassBufferR[2];
    uint32_t allPassWritePtr[2] = {0};
    uint32_t allPassLengths[2] = {223, 443};

    void init(float sampleRate);
    void process(float inL, float inR, float& outL, float& outR, float decayNorm, float sizeNorm, float predelayNorm, float mixNorm, float sampleRate);
};

// Swirling Quad-LFO Stereo Chorus and Auto-Pan (Tornado)
struct TornadoEffect {
    float lfoPhase = 0.0f;
    std::vector<float> delayBufferL;
    std::vector<float> delayBufferR;
    uint32_t writePtr = 0;
    uint32_t maxDelaySamples = 2048;

    void init(float sampleRate);
    void process(float inL, float inR, float& outL, float& outR, float rateNorm, float feedbackNorm, float widthNorm, float mixNorm, float sampleRate);
};

extern StereoDelay g_masterDelay;
extern GlueCompressor g_masterCompressor;
extern StereoReverb g_masterReverb;
extern TornadoEffect g_masterTornado;
