#pragma once
#include "Common.hpp"

// High-level controls to govern the real-time audio thread
void InitAudioEngine();
void ShutdownAudioEngine();
void PollUsbAudioDevice();          // UI thread: stop/reinit/start when SRC or USB engine changes
void ToggleTrack8Usb();             // Shift+T on track 8
void RefreshUsbCaptureDevices();
int GetUsbCaptureCount();
int GetUsbCaptureIndex();
void SetUsbCaptureIndex(int idx);
const char* GetUsbCaptureName(int idx);
int GetAudioSampleRate();
int GetAudioPeriodFrames();

// Public interface functions to trigger and release track voices in real-time
void TriggerVoiceLive(int trackIdx, int midiNote, int velocity);
void ReleaseVoiceLive(int trackIdx, int midiNote);
bool IsSynthVoiceActive(int trackIdx, int voiceIdx);
float VolumeCurve(float value0to99);

// LFO → voice offsets, filled once per 64-sample chunk. Voices copy these
// instead of walking 8×6 slots themselves.
struct TrackVoiceMod {
    float cutoff = 0.0f;
    float res = 0.0f;
    float vol = 0.0f;
    float morph1 = 0.0f;
    float morph2 = 0.0f;
    float pitch = 0.0f;
    float decay = 0.0f;
    float vol2 = 0.0f;
    float fine1 = 0.0f;
    float fine2 = 0.0f;
    float sampStart = 0.0f;
    float granSize = 0.0f;
    float granDens = 0.0f;
    float granScat = 0.0f;
};
extern TrackVoiceMod g_trackVoiceMod[8];
