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
    float mixParam = mixVal / 99.0f;

    // --- BLOCK-RATE SETUP (Inexpensive) ---
    float memorySec = 0.05f + (memoryVal / 99.0f) * 1.95f;
    float memSamples = std::floor(memorySec * sampleRate);
    if (memSamples > 88200.0f) memSamples = 88200.0f;
    if (memSamples < 2205.0f)  memSamples = 2205.0f;

    // Quick background bypass recording if mix is zero
    if (mixParam <= 0.0f) {
        delayBuf[(int)writePos] = saturate(input);
        writePos = std::fmod(writePos + 1.0f, memSamples);
        return input;
    }

    float headsParam = 1.0f + (headsVal / 99.0f) * 3.0f;
    float spreadParam = spreadVal / 99.0f;
    float speedParam = -2.0f + (speedVal / 99.0f) * 4.0f;
    float tetherParam = tetherVal / 99.0f;
    float driftParam = driftVal / 99.0f;
    float driftRateParam = driftRateVal / 99.0f;
    float feedbackParam = (feedbackVal / 99.0f) * 1.15f;
    float fbSpreadParam = fbSpreadVal / 99.0f;
    float fbSourceParam = fbSourceVal / 99.0f;
    float freezeParam = freezeVal / 99.0f;
    float smearRateParam = smearRateVal / 99.0f;
    float smearSizeParam = smearSizeVal / 99.0f;

    // Map cross-feedback to automatically scale with feedback
    float crossFeedbackParam = feedbackParam * 0.35f;

    // Slew write gain
    float targetGain = 1.0f - freezeParam;
    writeGain += 0.001f * (targetGain - writeGain);

    // --- ACCUMULATE AND WRAP PHASE (Fast conditional branch) ---
    masterPhase += speedParam;
    if (masterPhase >= memSamples) masterPhase -= memSamples;
    else if (masterPhase < 0.0f) masterPhase += memSamples;

    float baseDelaySamples = memSamples * 0.95f;
    float tetheredPhase = writePos - baseDelaySamples;
    if (tetheredPhase < 0.0f) tetheredPhase += memSamples;

    // Morph free tape loop (0) to delay line (1)
    float activePhase = (1.0f - tetherParam) * masterPhase + tetherParam * tetheredPhase;
    if (activePhase >= memSamples) activePhase -= memSamples;
    else if (activePhase < 0.0f) activePhase += memSamples;

    float spacing = spreadParam * (memSamples / 4.0f);

    // --- WOW & FLUTTER DRIFT ---
    float d[4] = {0.0f};
    if (driftParam > 0.0f) {
        float driftFilterCoeff = 0.00005f + driftRateParam * 0.00295f;
        float maxDriftSamples = driftParam * sampleRate * 0.010f;

        for (int i = 0; i < 4; ++i) {
            driftState[i] += driftFilterCoeff * (generateNoise() - driftState[i]);
            d[i] = driftState[i] * maxDriftSamples * 8.0f; // Smoother scaling factor
        }
    }

    // --- GRANULAR JUMPS ---
    int numHeadsLimit = std::clamp((int)headsParam, 1, 4);
    float gain[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    if (smearRateParam > 0.0f) {
        float smearProb = smearRateParam * 0.0005f;
        int fadeLen = 800;
        int fadeHalf = fadeLen / 2;
        float jumpRange = (smearSizeParam * smearSizeParam * smearSizeParam) * memSamples;

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

    // --- PLAYHEADS READ (Conditional wrapping) ---
    float sig[4] = {0.0f};
    float baseReadPos[4];

    baseReadPos[0] = activePhase;
    baseReadPos[1] = activePhase - spacing;
    baseReadPos[2] = activePhase - spacing * 2.0f;
    baseReadPos[3] = activePhase - spacing * 3.0f;

    for (int i = 0; i < 4; ++i) {
        if (baseReadPos[i] < 0.0f) baseReadPos[i] += memSamples;
        else if (baseReadPos[i] >= memSamples) baseReadPos[i] -= memSamples;
    }

    for (int i = 0; i < numHeadsLimit; ++i) {
        float finalReadPos = baseReadPos[i] + d[i] + smearOffset[i];
        if (finalReadPos < 0.0f) finalReadPos += memSamples;
        else if (finalReadPos >= memSamples) finalReadPos -= memSamples;

        sig[i] = readInterpolated(finalReadPos, memSamples) * gain[i];
    }

    // --- HEAD CROSS-FEEDBACK MATRIX ---
    float fb[4] = {0.0f};
    fb[0] = sig[0] + crossFeedbackParam * sig[3];
    fb[1] = sig[1] + crossFeedbackParam * sig[0];
    fb[2] = sig[2] + crossFeedbackParam * sig[1];
    fb[3] = sig[3] + crossFeedbackParam * sig[2];

    // Equal-Power Head Mixing
    float h1 = std::clamp(headsParam, 0.0f, 1.0f);
    float h2 = std::clamp(headsParam - 1.0f, 0.0f, 1.0f);
    float h3 = std::clamp(headsParam - 2.0f, 0.0f, 1.0f);
    float h4 = std::clamp(headsParam - 3.0f, 0.0f, 1.0f);

    // PI-OPTIMIZED: Pre-calculated Reciprocal Square Root Table
    static const float invSqrtTable[5] = { 0.0f, 1.0f, 0.70710678f, 0.57735027f, 0.5f };
    float totalReadSignal = (fb[0] * h1 + fb[1] * h2 + fb[2] * h3 + fb[3] * h4) * invSqrtTable[numHeadsLimit];

    // --- FEEDBACK ROUTING ---
    fbOffset += 0.0005f * (fbSpreadParam * memSamples * (generateNoise() * 0.5f + 0.5f) - fbOffset);
    float fbReadPos = activePhase - fbOffset;
    if (fbReadPos < 0.0f) fbReadPos += memSamples;
    else if (fbReadPos >= memSamples) fbReadPos -= memSamples;
    
    float diffusedFbSignal = readInterpolated(fbReadPos, memSamples);

    float selectedFeedback = (1.0f - fbSourceParam) * totalReadSignal + fbSourceParam * diffusedFbSignal;

    // Filter feedback path (Low-pass)
    float feedbackFilter = prevFeedback + 0.2f * (selectedFeedback - prevFeedback);
    prevFeedback = feedbackFilter;

    // Filter feedback path (High-pass)
    hpState += 0.08f * (feedbackFilter - hpState);
    float hpFiltered = feedbackFilter - hpState;

    // Warm cubic saturation (Discontinuity removed)
    float fbSignal = hpFiltered * feedbackParam;

    // --- WRITE BLOCK ---
    float currentVal = delayBuf[(int)writePos];
    float writeSignal = (1.0f - writeGain) * currentVal + writeGain * saturate(input + fbSignal);
    delayBuf[(int)writePos] = writeSignal;

    // Increment write head
    writePos = std::fmod(writePos + 1.0f, memSamples);

    // Dry/Wet Mix
    return (1.0f - mixParam) * input + mixParam * (totalReadSignal * 1.40f);
   }
