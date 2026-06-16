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
    delayBuf.assign(88200, 0.0f); // 2 seconds max at 44.1kHz
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
    }
}

inline float TapeBufferFX::generateNoise() {
    noiseSeed = noiseSeed * 1103515245u + 12345u;
    return ((float)(noiseSeed / 65536 % 32768) / 16384.0f) - 1.0f; // [-1.0, 1.0]
}

inline float TapeBufferFX::readInterpolated(float pos, float len) {
    // Fast integer cast instead of std::floor
    int idx0 = (int)pos;
    float frac = pos - (float)idx0;

    int idx1 = idx0 + 1;

    // Fast conditional wrapping instead of fmod/modulo
    if (idx0 < 0) idx0 += (int)len;
    else if (idx0 >= (int)len) idx0 -= (int)len;

    if (idx1 < 0) idx1 += (int)len;
    else if (idx1 >= (int)len) idx1 -= (int)len;

    float v0 = delayBuf[idx0];
    float v1 = delayBuf[idx1];

    // Linear blend
    return v0 + frac * (v1 - v0);
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
        if (cachedMemSamples > 88200.0f) cachedMemSamples = 88200.0f;
        if (cachedMemSamples < 2205.0f)  cachedMemSamples = 2205.0f;

        cachedHeadsParam = 1.0f + (headsVal / 99.0f) * 3.0f;
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
    }

    if (cachedMixParam <= 0.0f) {
        delayBuf[(int)writePos] = saturate(input);
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

    // --- WOW & FLUTTER DRIFT ---
    float d[4] = {0.0f};
    if (cachedDriftParam > 0.0f) {
        float driftFilterCoeff = 0.00005f + cachedDriftRateParam * 0.00295f;
        float maxDriftSamples = cachedDriftParam * sampleRate * 0.010f;

        for (int i = 0; i < 4; ++i) {
            driftState[i] += driftFilterCoeff * (generateNoise() - driftState[i]);
            d[i] = driftState[i] * maxDriftSamples * 8.0f;
        }
    }

    // --- GRANULAR JUMPS ---
    int numHeadsLimit = std::clamp((int)cachedHeadsParam, 1, 4);
    float gain[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    if (cachedSmearRateParam > 0.0f) {
        float smearProb = cachedSmearRateParam * 0.0005f;
        int fadeLen = 800;
        int fadeHalf = fadeLen / 2;
        float jumpRange = (cachedSmearSizeParam * cachedSmearSizeParam * cachedSmearSizeParam) * cachedMemSamples;

        for (int i = 0; i < numHeadsLimit; ++i) {
            if (fadeCount[i] == 0 && (std::abs(generateNoise()) < smearProb)) {
                fadeCount[i] = fadeLen;
            }
            if (fadeCount[i] > 0) {
                fadeCount[i]--;
                if (fadeCount[i] == fadeHalf) {
                    smearOffset[i] = generateNoise() * jumpRange;
                }
                gain[i] = (float)std::abs(fadeCount[i] - fadeHalf) / fadeHalf;
            } else {
                gain[i] = 1.0f;
            }
        }
    }

    // --- PLAYHEADS READ ---
    float sig[4] = {0.0f};
    float baseReadPos[4];

    baseReadPos[0] = activePhase;
    baseReadPos[1] = activePhase - spacing;
    baseReadPos[2] = activePhase - spacing * 2.0f;
    baseReadPos[3] = activePhase - spacing * 3.0f;

    for (int i = 0; i < 4; ++i) {
        if (baseReadPos[i] < 0.0f) baseReadPos[i] += cachedMemSamples;
        else if (baseReadPos[i] >= cachedMemSamples) baseReadPos[i] -= cachedMemSamples;
    }

    for (int i = 0; i < numHeadsLimit; ++i) {
        float finalReadPos = baseReadPos[i] + d[i] + smearOffset[i];
        if (finalReadPos < 0.0f) finalReadPos += cachedMemSamples;
        else if (finalReadPos >= cachedMemSamples) finalReadPos -= cachedMemSamples;

        sig[i] = readInterpolated(finalReadPos, cachedMemSamples) * gain[i];
    }

    // --- HEAD CROSS-FEEDBACK MATRIX ---
    float fb[4] = {0.0f};
    fb[0] = sig[0] + cachedCrossFeedbackParam * sig[3];
    fb[1] = sig[1] + cachedCrossFeedbackParam * sig[0];
    fb[2] = sig[2] + cachedCrossFeedbackParam * sig[1];
    fb[3] = sig[3] + cachedCrossFeedbackParam * sig[2];

    float h1 = std::clamp(cachedHeadsParam, 0.0f, 1.0f);
    float h2 = std::clamp(cachedHeadsParam - 1.0f, 0.0f, 1.0f);
    float h3 = std::clamp(cachedHeadsParam - 2.0f, 0.0f, 1.0f);
    float h4 = std::clamp(cachedHeadsParam - 3.0f, 0.0f, 1.0f);

    static const float invSqrtTable[5] = { 0.0f, 1.0f, 0.70710678f, 0.57735027f, 0.5f };
    float totalReadSignal = (fb[0] * h1 + fb[1] * h2 + fb[2] * h3 + fb[3] * h4) * invSqrtTable[numHeadsLimit];

    // --- FEEDBACK ROUTING ---
    fbOffset += 0.0005f * (cachedFbSpreadParam * cachedMemSamples * (generateNoise() * 0.5f + 0.5f) - fbOffset);
    float fbReadPos = activePhase - fbOffset;
    if (fbReadPos < 0.0f) fbReadPos += cachedMemSamples;
    else if (fbReadPos >= cachedMemSamples) fbReadPos -= cachedMemSamples;
    
    float diffusedFbSignal = readInterpolated(fbReadPos, cachedMemSamples);

    float selectedFeedback = (1.0f - cachedFbSourceParam) * totalReadSignal + cachedFbSourceParam * diffusedFbSignal;

    float feedbackFilter = prevFeedback + 0.2f * (selectedFeedback - prevFeedback);
    prevFeedback = feedbackFilter;

    hpState += 0.08f * (feedbackFilter - hpState);
    float hpFiltered = feedbackFilter - hpState;

    float fbSignal = hpFiltered * cachedFeedbackParam;

    // --- WRITE BLOCK ---
    float currentVal = delayBuf[(int)writePos];
    float writeSignal = (1.0f - writeGain) * currentVal + writeGain * saturate(input + fbSignal);
    delayBuf[(int)writePos] = writeSignal;

    writePos += 1.0f;
    if (writePos >= cachedMemSamples) {
        writePos = 0.0f;
    }

    return (1.0f - cachedMixParam) * input + cachedMixParam * (totalReadSignal * 1.40f);
}
