#include "Master_FX.hpp"
#include <cmath>
#include <algorithm>

StereoDelay g_masterDelay;
GlueCompressor g_masterCompressor;
StereoReverb g_masterReverb;
TornadoEffect g_masterTornado;

// ==========================================
// 1. STEREO PING-PONG DELAY
// ==========================================
void StereoDelay::init(float sampleRate) {
    maxDelaySamples = (uint32_t)sampleRate; // 1.0 second
    bufferL.assign(maxDelaySamples, 0.0f);
    bufferR.assign(maxDelaySamples, 0.0f);
    writePtr = 0;
}

void StereoDelay::process(float inL, float inR, float& outL, float& outR, float timeNorm, float feedbackNorm, int pingPong, float mixNorm, float sampleRate) {
    // Buffers are owned by EnsureFxRate()/init() — never allocate here.
    if (maxDelaySamples == 0 || bufferL.empty() || bufferR.empty()) {
        outL = inL; outR = inR; return;
    }

    if (mixNorm <= 0.0f) {
        outL = inL; outR = inR; return;
    }

    float delayTimeSec = 0.010f + timeNorm * 0.990f;
    float delaySamplesFloat = delayTimeSec * sampleRate;
    if (delaySamplesFloat > maxDelaySamples - 2) {
        delaySamplesFloat = (float)(maxDelaySamples - 2);
    }

    float readPtrFloat = (float)writePtr - delaySamplesFloat;
    if (readPtrFloat < 0.0f) {
        readPtrFloat += maxDelaySamples;
    }

    uint32_t readPtr0 = (uint32_t)readPtrFloat % maxDelaySamples;
    uint32_t readPtr1 = (readPtr0 + 1) % maxDelaySamples;
    float frac = readPtrFloat - (float)((uint32_t)readPtrFloat);

    float delayOutL = bufferL[readPtr0] * (1.0f - frac) + bufferL[readPtr1] * frac;
    float delayOutR = bufferR[readPtr0] * (1.0f - frac) + bufferR[readPtr1] * frac;

    float feedbackGain = feedbackNorm * 0.92f;

    if (pingPong == 1) {
        bufferL[writePtr] = inL + delayOutR * feedbackGain;
        bufferR[writePtr] = inR + delayOutL * feedbackGain;
    } else {
        bufferL[writePtr] = inL + delayOutL * feedbackGain;
        bufferR[writePtr] = inR + delayOutR * feedbackGain;
    }

    writePtr = (writePtr + 1) % maxDelaySamples;

    outL = inL * (1.0f - mixNorm) + delayOutL * mixNorm;
    outR = inR * (1.0f - mixNorm) + delayOutR * mixNorm;
}

// ==========================================
// 2. STUDIO BUS GLUE COMPRESSOR
// ==========================================
void GlueCompressor::process(float inL, float inR, float& outL, float& outR, float driveNorm, float symNorm, float tightNorm, float mixNorm, float sampleRate) {
    float driveGain = 1.0f + driveNorm * 3.0f; // Up to +12dB boost
    float drivenL = inL * driveGain;
    float drivenR = inR * driveGain;

    float inputRMS = sqrtf((drivenL * drivenL + drivenR * drivenR) * 0.5f);
    
    static float lastSampleRate = 0.0f;
        static float attackCoef = 0.9977f;
        static float releaseCoef = 0.9998f;

        if (sampleRate != lastSampleRate) {
            lastSampleRate = sampleRate;
            attackCoef = expf(-1.0f / (0.010f * sampleRate));
            releaseCoef = expf(-1.0f / (0.150f * sampleRate));
        }
    
    if (inputRMS > envelope) {
        envelope = envelope * attackCoef + inputRMS * (1.0f - attackCoef);
    } else {
        envelope = envelope * releaseCoef + inputRMS * (1.0f - releaseCoef);
    }

    float gainReduction = 1.0f;
    if (tightNorm > 0.0f) {
        float threshold = 1.0f - tightNorm * 0.99f;
        float ratio = 1.0f + tightNorm * 11.0f;

        if (envelope > threshold) {
            // Linear-domain ratio: out = thresh + (in - thresh) / ratio.
            // Same control feel as the old log/exp path without a transcendental
            // per sample, which is the cost that showed up with SAT mix up.
            float compressed = threshold + (envelope - threshold) / ratio;
            gainReduction = compressed / envelope;
        }
    }

    float compressedL = drivenL * gainReduction;
    float compressedR = drivenR * gainReduction;

    float dcOffset = symNorm * 0.35f;
    
    auto SoftClip = [](float sample, float offset) {
        float x = sample + offset;
        if (x > 1.0f)  x = 1.0f;
        if (x < -1.0f) x = -1.0f;
        return (x - (x * x * x) / 3.0f) - offset;
    };

    float saturatedL = SoftClip(compressedL, dcOffset);
    float saturatedR = SoftClip(compressedR, dcOffset);

    outL = inL * (1.0f - mixNorm) + saturatedL * mixNorm;
    outR = inR * (1.0f - mixNorm) + saturatedR * mixNorm;
}

// ==========================================
// 3. STEREO SCHROEDER REVERB
// ==========================================
void StereoReverb::init(float sampleRate) {
    uint32_t preDelaySize = (uint32_t)(0.200f * sampleRate); // 200ms pre-delay limit
    preDelayBufferL.assign(preDelaySize, 0.0f);
    preDelayBufferR.assign(preDelaySize, 0.0f);
    preDelayWritePtr = 0;

    // Initialize comb buffers with standard prime spacings scaled to room size
    for (int i = 0; i < 4; ++i) {
        float sizeScale = 1.0f + (float)i * 0.15f;
        uint32_t lLen = (uint32_t)(combLengths[i] * sizeScale);
        combBufferL[i].assign(lLen, 0.0f);
        combBufferR[i].assign(lLen + 23, 0.0f); // Prime offset to widen stereo image [2]
        combWritePtr[i] = 0;
        combWritePtrR[i] = 0;
    }

    // Initialize All-Pass diffusion buffers
    for (int i = 0; i < 2; ++i) {
        allPassBufferL[i].assign(allPassLengths[i], 0.0f);
        allPassBufferR[i].assign(allPassLengths[i] + 13, 0.0f); // Prime offset [2]
        allPassWritePtr[i] = 0;
        allPassWritePtrR[i] = 0;
    }
}

void StereoReverb::process(float inL, float inR, float& outL, float& outR, float decayNorm, float sizeNorm, float predelayNorm, float mixNorm, float sampleRate) {
    // Buffers are owned by EnsureFxRate()/init() — never allocate here.
    if (preDelayBufferL.empty()) {
        outL = inL; outR = inR; return;
    }

    if (mixNorm <= 0.0f) {
        outL = inL; outR = inR; return;
    }

    // 1. Process Pre-delay Line (up to 200ms)
    uint32_t preDelaySize = (uint32_t)preDelayBufferL.size();
    float preDelaySec = predelayNorm * 0.200f;
    uint32_t preDelaySamples = (uint32_t)(preDelaySec * sampleRate);
    if (preDelaySamples >= preDelaySize) preDelaySamples = preDelaySize - 1;

    preDelayBufferL[preDelayWritePtr] = inL;
    preDelayBufferR[preDelayWritePtr] = inR;

    int readPtr = (int)preDelayWritePtr - (int)preDelaySamples;
    if (readPtr < 0) readPtr += preDelaySize;

    float wetInL = preDelayBufferL[readPtr];
    float wetInR = preDelayBufferR[readPtr];

    preDelayWritePtr = (preDelayWritePtr + 1) % preDelaySize;

    // 2. Process 4 Parallel Comb Filters
    float combSumL = 0.0f;
    float combSumR = 0.0f;

    // Base feedback decay gain (RT60 decay time up to 5 seconds)
    float fbGain = 0.5f + decayNorm * 0.45f;

    for (int i = 0; i < 4; ++i) {
            uint32_t lenL = (uint32_t)combBufferL[i].size();
            uint32_t lenR = (uint32_t)combBufferR[i].size();

            float outCombL = combBufferL[i][combWritePtr[i]];
            float outCombR = combBufferR[i][combWritePtrR[i]]; // Use right pointer

            float combInputL = wetInL + outCombL * fbGain;
            float combInputR = wetInR + outCombR * fbGain;

            combBufferL[i][combWritePtr[i]] = combInputL;
            combBufferR[i][combWritePtrR[i]] = combInputR; // Use right pointer

            combSumL += outCombL;
            combSumR += outCombR;

            // Wrap using branch-predicted increments instead of slow modulo
            combWritePtr[i]++;
            if (combWritePtr[i] >= lenL) combWritePtr[i] = 0;

            combWritePtrR[i]++;
            if (combWritePtrR[i] >= lenR) combWritePtrR[i] = 0;
        }

    // Scale down parallel comb sum
    float diffuseInL = combSumL * 0.25f;
    float diffuseInR = combSumR * 0.25f;

    
    // 3. Diffuse reflections using 2 Cascaded All-Pass filters
        for (int i = 0; i < 2; ++i) {
            uint32_t lenL = (uint32_t)allPassBufferL[i].size();
            uint32_t lenR = (uint32_t)allPassBufferR[i].size();

            float apOutL = allPassBufferL[i][allPassWritePtr[i]];
            float apOutR = allPassBufferR[i][allPassWritePtrR[i]]; // Use right pointer

            // Standard All-pass equations (0.5 feedback gain)
            float apInputL = diffuseInL + apOutL * 0.5f;
            float apInputR = diffuseInR + apOutR * 0.5f;

            allPassBufferL[i][allPassWritePtr[i]] = apInputL;
            allPassBufferR[i][allPassWritePtrR[i]] = apInputR; // Use right pointer

            diffuseInL = -0.5f * apInputL + apOutL;
            diffuseInR = -0.5f * apInputR + apOutR;

            // Wrap using branch-predicted increments instead of slow modulo
            allPassWritePtr[i]++;
            if (allPassWritePtr[i] >= lenL) allPassWritePtr[i] = 0;

            allPassWritePtrR[i]++;
            if (allPassWritePtrR[i] >= lenR) allPassWritePtrR[i] = 0;
        }

    // Parallel Wet/Dry Reverb Mix
    outL = inL * (1.0f - mixNorm) + diffuseInL * mixNorm;
    outR = inR * (1.0f - mixNorm) + diffuseInR * mixNorm;
}

// ==========================================
// 4. CHORUS & QUAD-LFO AUTO-PAN (TORNADO)
// ==========================================
void TornadoEffect::init(float sampleRate) {
    maxDelaySamples = 2048;
    delayBufferL.assign(maxDelaySamples, 0.0f);
    delayBufferR.assign(maxDelaySamples, 0.0f);
    writePtr = 0;
    lfoPhase = 0.0f;
}

void TornadoEffect::process(float inL, float inR, float& outL, float& outR, float rateNorm, float feedbackNorm, float widthNorm, float mixNorm, float sampleRate) {
    // Buffers are owned by EnsureFxRate()/init() — never allocate here.
    if (delayBufferL.empty() || maxDelaySamples == 0) {
        outL = inL; outR = inR; return;
    }

    if (mixNorm <= 0.0f) {
        outL = inL; outR = inR; return;
    }

    // 1. Process Stereo LFO (Quadrature Phase shift: Left is Sine, Right is Cosine!)
    float lfoHz = 0.1f + rateNorm * 11.9f; // LFO speed from 0.1Hz to 12Hz
    lfoPhase += (2.0f * 3.14159265f * lfoHz) / sampleRate;
    if (lfoPhase >= 2.0f * 3.14159265f) {
        lfoPhase -= 2.0f * 3.14159265f;
    }

    float lfoL = sinf(lfoPhase);
    float lfoR = cosf(lfoPhase); // 90-degree phase shift for maximum stereo wideness [2]

    // 2. Modulate Delay Chorus Lines (5ms to 25ms delay sweeps)
    float baseDelaySec = 0.015f;
    float modDepthSec = widthNorm * 0.010f; // Modulate depth based on WIDTH

    float delaySecL = baseDelaySec + lfoL * modDepthSec;
    float delaySecR = baseDelaySec + lfoR * modDepthSec;

    float delaySamplesL = delaySecL * sampleRate;
    float delaySamplesR = delaySecR * sampleRate;

    // Fractional delay reads
    float readPtrL = (float)writePtr - delaySamplesL;
    if (readPtrL < 0.0f) readPtrL += maxDelaySamples;
    uint32_t rL0 = (uint32_t)readPtrL % maxDelaySamples;
    uint32_t rL1 = (rL0 + 1) % maxDelaySamples;
    float fracL = readPtrL - (float)((uint32_t)readPtrL);
    float chorusL = delayBufferL[rL0] * (1.0f - fracL) + delayBufferL[rL1] * fracL;

    float readPtrR = (float)writePtr - delaySamplesR;
    if (readPtrR < 0.0f) readPtrR += maxDelaySamples;
    uint32_t rR0 = (uint32_t)readPtrR % maxDelaySamples;
    uint32_t rR1 = (rR0 + 1) % maxDelaySamples;
    float fracR = readPtrR - (float)((uint32_t)readPtrR);
    float chorusR = delayBufferR[rR0] * (1.0f - fracR) + delayBufferR[rR1] * fracR;

    // Write delay buffers with modulated feedback (feedbackNorm)
    float fbGain = feedbackNorm * 0.85f;
    delayBufferL[writePtr] = inL + chorusL * fbGain;
    delayBufferR[writePtr] = inR + chorusR * fbGain;

    writePtr = (writePtr + 1) % maxDelaySamples;

    // 3. Process Quadrature Auto-Pan Stage
    // Modulation sweeps left and right channels in opposition
    float panL = 1.0f - widthNorm * (0.5f + lfoL * 0.5f);
    float panR = 1.0f - widthNorm * (0.5f + lfoR * 0.5f);

    float wetL = chorusL * panL;
    float wetR = chorusR * panR;

    // Parallel Wet/Dry Mix
    outL = inL * (1.0f - mixNorm) + wetL * mixNorm;
    outR = inR * (1.0f - mixNorm) + wetR * mixNorm;
}
