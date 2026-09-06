#pragma once
#include "Common.hpp"
#include <atomic>
#include <cstdint>

// Fixed-size SPSC live MIDI queues: RtMidi / UI enqueue; audio callback drains.
enum MidiLiveEventType : uint8_t { ML_NOTE_ON = 1, ML_NOTE_OFF = 2, ML_SEQ_STEP = 3 };
struct MidiLiveEvent {
    uint8_t type;
    uint8_t track;
    uint8_t note;
    uint8_t velocity;
    uint8_t stepIndex; // captured at enqueue for ML_SEQ_STEP
};
constexpr int kMidiLiveQueueCap = 64;
struct MidiLiveQueue {
    MidiLiveEvent slots[kMidiLiveQueueCap];
    std::atomic<uint32_t> writeIdx{0};
    std::atomic<uint32_t> readIdx{0};
};

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

// Nested-safe pause around sample/project loads. First Pause stops the device;
// matching Resume restarts it. PollUsbAudioDevice skips while paused.
void PauseSeqAudio();
void ResumeSeqAudio();
int GetSeqAudioPauseDepth();

// Decode a WAV into out without touching g_samplePool or pausing audio.
// On failure leaves out empty (name "Empty") and returns false.
bool LoadSampleIntoAsset(SampleAsset& out, const std::string& filename);

// UI thread: enqueue live note on/off (audio drains and mutates voices).
void TriggerVoiceLive(int trackIdx, int midiNote, int velocity);
void ReleaseVoiceLive(int trackIdx, int midiNote);
// RtMidi thread: enqueue note on/off or seq-step write.
void EnqueueMidiLiveFromRt(uint8_t type, uint8_t track, uint8_t note, uint8_t velocity, uint8_t stepIndex = 0);
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
