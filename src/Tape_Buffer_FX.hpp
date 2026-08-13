#pragma once
#include <vector>

class TapeBufferFX {
public:
    TapeBufferFX();
    void reset();
    
    // Core DSP execution call. Takes parameters in clean 0..99 integer bounds
    float process(float input,
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
                  double sampleRate);

private:
    inline float readInterpolated(const float* buf, float pos, int len);
    inline float generateNoise();

    std::vector<float> delayBuf;
    unsigned int noiseSeed;

    // State histories
    float writePos;
    float masterPhase;
    float prevFeedback;
    float writeGain;
    float fbOffset;

    // Head Fade and Smear offsets
    int fadeCount[4];
    float smearOffset[4];

    // Wow & Flutter Drift states
        float driftState[4];
        float driftOffset[4];
        float driftInc[4];

        // High-pass filter history for feedback warmth
        float hpState;
    // New block-rate parameter caches
        float cachedMemSamples = 0.0f;
        int cachedMemLen = 0;
        float cachedHeadsParam = 0.0f;
        int cachedHeadsLimit = 1;
        float cachedSpreadParam = 0.0f;
        float cachedSpeedParam = 0.0f;
        float cachedTetherParam = 0.0f;
        float cachedDriftParam = 0.0f;
        float cachedDriftRateParam = 0.0f;
        float cachedFeedbackParam = 0.0f;
        float cachedFbSpreadParam = 0.0f;
        float cachedFbSourceParam = 0.0f;
        float cachedFreezeParam = 0.0f;
        float cachedSmearRateParam = 0.0f;
        float cachedSmearSizeParam = 0.0f;
        float cachedMixParam = 0.0f;
        float cachedCrossFeedbackParam = 0.0f;
        int blockCounter = 9999; // Initialize high to force setup on first sample
        float cachedMaxDriftSamples = 0.0f;
        float cachedDriftFilterCoeff = 0.0f;
        float cachedJumpRange = 0.0f;
    };
