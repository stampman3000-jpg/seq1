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
    inline float readInterpolated(float pos, float len);
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

        // High-pass filter history for feedback warmth
        float hpState;
    };
