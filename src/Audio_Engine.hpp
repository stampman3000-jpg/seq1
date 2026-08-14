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

// Public interface functions to trigger and release track voices in real-time
void TriggerVoiceLive(int trackIdx, int midiNote, int velocity);
void ReleaseVoiceLive(int trackIdx, int midiNote);
bool IsSynthVoiceActive(int trackIdx, int voiceIdx);
float VolumeCurve(float value0to99);
