#pragma once
#include "Common.hpp"

// High-level controls to govern the real-time audio thread
void InitAudioEngine();
void ShutdownAudioEngine();

// Public interface functions to trigger and release track voices in real-time
void TriggerVoiceLive(int trackIdx, int midiNote, int velocity);
void ReleaseVoiceLive(int trackIdx, int midiNote);
bool IsSynthVoiceActive(int trackIdx, int voiceIdx);
