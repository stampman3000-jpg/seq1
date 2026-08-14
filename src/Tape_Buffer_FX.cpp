#include "Tape_Buffer_FX.hpp"
#include <cmath>
#include <algorithm>

// Symmetrical cubic soft-clipper to emulate warm analog tape saturation
static inline float saturate(float x) {
    if (x > 1.2f)  return 0.8f;
    if (x < -1.2f) return -0.8f;
    return x - (x * x * x) * 0.25f;
}

TapeBufferFX::TapeBufferFX() {
    delayBuf.assign(96000, 0.0f); // 2 seconds max at 48kHz
    noiseSeed = 0x12345678u;
    reset();
}

void TapeBufferFX::reset() {
    std::fill(delayBuf.begin(), delayBuf.end(), 0.0f);
    writePos = 0.0f;
    masterPhase = 0.0f;
    prevFeedback = 0.0f;
    writeGain = 1.0f;
    fbOffset = 0.0f;
    hpState = 0.0f;
    blockCounter = 9999; // Force immediate recalculation on reset

    for (int i = 0; i < 4; ++i) {
        fadeCount[i] = 0;
        smearOffset[i] = 0.0f;
        driftState[i] = 0.0f;
        driftOffset[i] = 0.0f;
        driftInc[i] = 0.0f;
    }
}

inline float TapeBufferFX::generateNoise() {
    noiseSeed = noiseSeed * 1103515245u + 12345u;
    return ((float)(noiseSeed / 65536 % 32768) / 16384.0f) - 1.0f; // [-1.0, 1.0]
}

inline float TapeBufferFX::readInterpolated(const float* buf, float pos, int len) {
    int idx0 = (int)pos;
    float frac = pos - (float)idx0;
    int idx1 = idx0 + 1;

    if (idx0 < 0) idx0 += len;
    else if (idx0 >= len) idx0 -= len;

    if (idx1 < 0) idx1 += len;
    else if (idx1 >= len) idx1 -= len;

    return buf[idx0] + frac * (buf[idx1] - buf[idx0]);
}

float TapeBufferFX::process(float input,
                            int memoryVal,
                            int headsVal,
                            int spreadVal,
                            int speedVal,
                            int tetherVal,
                            int driftVal,
                            int driftRateVal,
                            int feedbackVal,
                            int fbSpreadVal,
                            int fbSourceVal,
                            int freezeVal,
                            int smearRateVal,
                            int smearSizeVal,
                            int mixVal,
                            double sampleRate)
{
    // Update conversions only once every 64 samples
    blockCounter++;
    if (blockCounter >= 64) {
        blockCounter = 0;

        cachedMixParam = mixVal / 99.0f;

        float memorySec = 0.05f + (memoryVal / 99.0f) * 1.95f;
        cachedMemSamples = std::floor(memorySec * sampleRate);
        if (cachedMemSamples > 96000.0f) cachedMemSamples = 96000.0f;
        if (cachedMemSamples < 2205.0f)  cachedMemSamples = 2205.0f;
        cachedMemLen = (int)cachedMemSamples;

        // headsVal is already 1..4 (the control range and the LFO clamp).
        // The old 0..99 rescale collapsed every setting onto a single head.
        cachedHeadsParam = (float)headsVal;
        cachedHeadsLimit = std::clamp(headsVal, 1, 4);
        cachedSpreadParam = spreadVal / 99.0f;
        cachedSpeedParam = -2.0f + (speedVal / 99.0f) * 4.0f;
        cachedTetherParam = tetherVal / 99.0f;
        cachedDriftParam = driftVal / 99.0f;
        cachedDriftRateParam = driftRateVal / 99.0f;
        cachedFeedbackParam = (feedbackVal / 99.0f) * 1.15f;
        cachedFbSpreadParam = fbSpreadVal / 99.0f;
        cachedFbSourceParam = fbSourceVal / 99.0f;
        cachedFreezeParam = freezeVal / 99.0f;
        cachedSmearRateParam = smearRateVal / 99.0f;
        cachedSmearSizeParam = smearSizeVal / 99.0f;

        cachedCrossFeedbackParam = cachedFeedbackParam * 0.35f;

        cachedDriftFilterCoeff = 0.00005f + cachedDriftRateParam * 0.00295f;
        cachedMaxDriftSamples = cachedDriftParam * (float)sampleRate * 0.010f;
        cachedJumpRange = (cachedSmearSizeParam * cachedSmearSizeParam * cachedSmearSizeParam) * cachedMemSamples;

        // Drift's one-pole is a 300-to-20000 sample time constant, so the
        // noise and the filter itself belong at block rate. Per-sample we
        // just lerp the read offset so it does not step every 64 samples.
        if (cachedDriftParam > 0.0f) {
            for (int i = 0; i < cachedHeadsLimit; ++i) {
                driftState[i] += cachedDriftFilterCoeff * (generateNoise() - driftState[i]);
                float target = driftState[i] * cachedMaxDriftSamples * 8.0f;
                driftInc[i] = (target - driftOffset[i]) * (1.0f / 64.0f);
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                driftState[i] = 0.0f;
                driftInc[i] = -driftOffset[i] * (1.0f / 64.0f);
            }
        }

        // Smear used to roll a probability per head per sample. Scale by 64
        // so the expected jump rate is unchanged, then start the fade here.
        if (cachedSmearRateParam > 0.0f) {
            float smearProb = cachedSmearRateParam * 0.0005f * 64.0f;
            for (int i = 0; i < cachedHeadsLimit; ++i) {
                if (fadeCount[i] == 0 && (std::abs(generateNoise()) < smearProb)) {
                    fadeCount[i] = 800;
                }
            }
        }
    }

    float* buf = delayBuf.data();
    const int memLen = cachedMemLen > 0 ? cachedMemLen : (int)cachedMemSamples;

    if (cachedMixParam <= 0.0f) {
        int w = (int)writePos;
        if (w < 0) w = 0;
        else if (w >= memLen && memLen > 0) w = 0;
        buf[w] = saturate(input);
        writePos += 1.0f;
        if (writePos >= cachedMemSamples) {
            writePos = 0.0f;
        }
        return input;
    }

    // --- SLEW WRITE GAIN ---
    float targetGain = 1.0f - cachedFreezeParam;
    writeGain += 0.001f * (targetGain - writeGain);

    // --- ACCUMULATE AND WRAP PHASE ---
    masterPhase += cachedSpeedParam;
    if (masterPhase >= cachedMemSamples) masterPhase -= cachedMemSamples;
    else if (masterPhase < 0.0f) masterPhase += cachedMemSamples;

    float baseDelaySamples = cachedMemSamples * 0.95f;
    float tetheredPhase = writePos - baseDelaySamples;
    if (tetheredPhase < 0.0f) tetheredPhase += cachedMemSamples;

    float activePhase = (1.0f - cachedTetherParam) * masterPhase + cachedTetherParam * tetheredPhase;
    if (activePhase >= cachedMemSamples) activePhase -= cachedMemSamples;
    else if (activePhase < 0.0f) activePhase += cachedMemSamples;

    float spacing = cachedSpreadParam * (cachedMemSamples / 4.0f);
    const int numHeadsLimit = cachedHeadsLimit;

    // --- PLAYHEADS READ ---
    float sig[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float gain[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    constexpr int fadeLen = 800;
    constexpr int fadeHalf = fadeLen / 2;

    for (int i = 0; i < numHeadsLimit; ++i) {
        driftOffset[i] += driftInc[i];

        if (fadeCount[i] > 0) {
            fadeCount[i]--;
            if (fadeCount[i] == fadeHalf) {
                smearOffset[i] = generateNoise() * cachedJumpRange;
            }
            gain[i] = (float)std::abs(fadeCount[i] - fadeHalf) / (float)fadeHalf;
        }

        float readPos = activePhase - spacing * (float)i + driftOffset[i] + smearOffset[i];
        if (readPos < 0.0f) readPos += cachedMemSamples;
        else if (readPos >= cachedMemSamples) readPos -= cachedMemSamples;

        sig[i] = readInterpolated(buf, readPos, memLen) * gain[i];
    }

    // --- HEAD CROSS-FEEDBACK MATRIX (active heads only) ---
    float fb[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (numHeadsLimit >= 1) fb[0] = sig[0] + ((numHeadsLimit >= 4) ? cachedCrossFeedbackParam * sig[3] : 0.0f);
    if (numHeadsLimit >= 2) fb[1] = sig[1] + cachedCrossFeedbackParam * sig[0];
    if (numHeadsLimit >= 3) fb[2] = sig[2] + cachedCrossFeedbackParam * sig[1];
    if (numHeadsLimit >= 4) fb[3] = sig[3] + cachedCrossFeedbackParam * sig[2];

    float h1 = std::clamp(cachedHeadsParam, 0.0f, 1.0f);
    float h2 = std::clamp(cachedHeadsParam - 1.0f, 0.0f, 1.0f);
    float h3 = std::clamp(cachedHeadsParam - 2.0f, 0.0f, 1.0f);
    float h4 = std::clamp(cachedHeadsParam - 3.0f, 0.0f, 1.0f);

    static const float invSqrtTable[5] = { 0.0f, 1.0f, 0.70710678f, 0.57735027f, 0.5f };
    float totalReadSignal = (fb[0] * h1 + fb[1] * h2 + fb[2] * h3 + fb[3] * h4) * invSqrtTable[numHeadsLimit];

    // --- FEEDBACK ROUTING ---
    // Default fbSource is 0, so the extra delay-line read was previously
    // half of all tape reads and then multiplied by zero. Skip it, and skip
    // the whole chain when feedback itself is off.
    float fbSignal = 0.0f;
    if (cachedFeedbackParam > 0.0f) {
        float selectedFeedback = totalReadSignal;
        if (cachedFbSourceParam > 0.0f) {
            fbOffset += 0.0005f * (cachedFbSpreadParam * cachedMemSamples * (generateNoise() * 0.5f + 0.5f) - fbOffset);
            float fbReadPos = activePhase - fbOffset;
            if (fbReadPos < 0.0f) fbReadPos += cachedMemSamples;
            else if (fbReadPos >= cachedMemSamples) fbReadPos -= cachedMemSamples;

            float diffusedFbSignal = readInterpolated(buf, fbReadPos, memLen);
            selectedFeedback = (1.0f - cachedFbSourceParam) * totalReadSignal + cachedFbSourceParam * diffusedFbSignal;
        }

        float feedbackFilter = prevFeedback + 0.2f * (selectedFeedback - prevFeedback);
        prevFeedback = feedbackFilter;

        hpState += 0.08f * (feedbackFilter - hpState);
        float hpFiltered = feedbackFilter - hpState;

        fbSignal = hpFiltered * cachedFeedbackParam;
    }

    // --- WRITE BLOCK ---
    int w = (int)writePos;
    if (w < 0) w = 0;
    else if (w >= memLen && memLen > 0) w = 0;
    float currentVal = buf[w];
    float writeSignal = (1.0f - writeGain) * currentVal + writeGain * saturate(input + fbSignal);
    buf[w] = writeSignal;

    writePos += 1.0f;
    if (writePos >= cachedMemSamples) {
        writePos = 0.0f;
    }

    return (1.0f - cachedMixParam) * input + cachedMixParam * (totalReadSignal * 1.40f);
}
