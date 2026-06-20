#include "Globals.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <vector>

int selectedTrack = 0;
std::atomic<bool> g_useExternalMidiClock(false);
std::atomic<int> g_externalMidiTicksQueued(0);
std::atomic<bool> g_externalMidiStartTriggered(false);
std::atomic<bool> g_externalMidiStopTriggered(false);

Screen currentScreen = SCREEN_SEQ_1_4;
int cursorStep = 0;
int activePage = 0;

int masterLength = 32;     // Default to 32 steps
int stepUtilFocus = 0;     // Default focus on Microtiming

float g_audioCpuLoad = 0.0f;
bool showDiagnostics = false;
// Global State Definitions
Track tracks[8];
int synthMode = 0;

// Instantiate global sequencer clocks and master effects states
GlobalFX globalFX;
int playhead = 0;
double tempo = 120.0;
bool isPlaying = false;
bool liveKeyboardActive = true; // Defaults to true on startup
// Instantiate pattern slots & queued pattern flags
Pattern patterns[8];
int activePattern = 0;
int queuedPattern = -1;

// Instantiate File Browser & Typing State variables
bool systemMenuOpen = false;
int systemMenuCursor = 0;
int systemMenuState = 0;

// Instantiate Modulation Matrix Popup variables
bool lfoPopupOpen = false;
int lfoPopupLfoIdx = 0;
int lfoPopupSlot = 0;
int lfoPopupField = 0;
int fileBrowserCursor = 0;
int saveSlotCursor = 0;
std::string g_typingBuffer = "";
int g_typingCursor = 0;
std::vector<std::string> g_fileList;

// Instantiate the global sample pool
SampleAsset g_samplePool[16];
const char* menuFeedback = "";

extern const NoteBinding keyboardPiano[] = {
    { KEY_A, "C"  }, { KEY_W, "C#" }, { KEY_S, "D"  }, { KEY_E, "D#" },
    { KEY_D, "E"  }, { KEY_F, "F"  }, { KEY_T, "F#" }, { KEY_G, "G"  },
    { KEY_Y, "G#" }, { KEY_H, "A"  }, { KEY_U, "A#" }, { KEY_J, "B"  },
    { KEY_K, "C+" }
};
extern const int NUM_NOTES = sizeof(keyboardPiano) / sizeof(NoteBinding);

extern const std::vector<std::string> scaleNotes = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

const char* trackNames1_4[4] = { "TRK1", "TRK2", "TRK3", "TRK4" };
const char* trackNames5_8[4] = { "TRK5", "TRK6", "TRK7", "TRK8" };
const char* trackNamesTrigs[4] = { "VEL1", "VEL2", "VEL3", "VEL4" };

extern const float sineTable[32] = {
    0.0000f, 0.2013f, 0.3944f, 0.5712f, 0.7248f, 0.8491f, 0.9385f, 0.9898f,
    0.9997f, 0.9679f, 0.8952f, 0.7845f, 0.6406f, 0.4699f, 0.2802f, 0.0801f,
    -0.1205f, -0.3182f, -0.5035f, -0.6680f, -0.8044f, -0.9063f, -0.9694f, -0.9914f,
    -0.9712f, -0.9103f, -0.8115f, -0.6781f, -0.5155f, -0.3315f, -0.1345f, 0.0658f
};

extern const PixelOffset retrigPattern[16] = {
    {1, 10}, {2, 11}, {2, 9}, {3, 10}, {4, 11}, {4, 9}, {7, 10}, {6, 11},
    {6, 9}, {7, 10}, {8, 11}, {8, 9}, {13, 10}, {10, 11}, {10, 9}, {11, 10}
};

extern const std::vector<std::string> triggerOptions = {
    "", "AFT",
    "1:2", "2:2",
    "1:3", "2:3", "3:3",
    "1:4", "2:4", "3:4", "4:4",
    "1:5", "2:5", "3:5", "4:5", "5:5",
    "1:6", "2:6", "3:6", "4:6", "5:6", "6:6",
    "1:7", "2:7", "3:7", "4:7", "5:7", "6:7", "7:7",
    "1:8", "2:8", "3:8", "4:8", "5:8", "6:8", "7:8", "8:8"
};

extern const int defaultPitchSweepDepth[8] = { 20, 15, 0, 40, 10, 5, 0, 30 };
extern const int defaultPitchSweepTime[8]  = { 10, 25, 0, 15, 5,  8, 0, 20 };
extern const int defaultReverbSend[8]      = { 15, 30, 45, 0, 10, 20, 5, 12 };
extern const int defaultDelaySend[8]       = { 30, 20, 10, 5, 15, 25, 0, 8 };
extern const int defaultSampleRateRed[8]   = { 0,  0,  0,  0, 12, 24, 0, 8 };
extern const int defaultBitRed[8]          = { 0,  0,  0,  0, 4,  8,  0, 2 };

// --- Raw sequencer setup tables used exclusively during initialization ---
std::string rawNotes1_4[4][16] = {
    {"C4",  "",   "",   "",   "G4",  "",   "",   "",   "F4",  "",   "",   "",   "G6",  "",   "",   ""},
    {"",    "",   "",   "",   "C#4", "",   "",   "",   "",    "",   "",   "",   "F4",  "",   "",   ""},
    {"G4",  "",   "G4", "",   "G4",  "",   "G4", "",   "G4",  "",   "G4", "",   "G4",  "",   "G4", ""},
    {"",    "",   "D#4", "",  "",    "",   "",   "F4",  "",    "G4", "",   "",   "",    "",   "B4", "C5"}
};

std::string rawNotes5_8[4][16] = {
    {"C2",  "",   "C2", "",   "G2",  "",   "G2", "",   "F2",  "",   "F2", "",   "A2",  "",   "B2", ""},
    {"",    "",   "E4", "",   "",    "",   "G4", "",   "",    "",   "B4", "",   "",    "",   "C5", ""},
    {"C3",  "",   "",   "",   "C3",  "",   "",   "",   "C3",  "",   "",   "",   "C3",  "",   "",   ""},
    {"",    "",   "G5", "G5", "",    "",   "A5", "A5", "",    "",   "B5", "B5", "",    "",   "C6", "C6"}
};

std::string rawTrigs1_4[4][16] = {
    {"1:2", "",   "",   "",   "1:2",  "",   "",   "",   "1:2", "",   "",   "",   "3:4", "",   "",   ""},
    {"",    "",   "",   "",   "AFT", "",   "",   "",   "",    "",   "",   "",   "AFT", "",   "",   ""},
    {"1:2",  "",   "1:2", "",   "1:2",  "",   "1:2", "",   "1:2",  "",   "1:2", "",   "1:2",  "",   "1:2", ""},
    {"",    "",   "AFT", "",  "",    "",   "",   "3:4", "",   "2:2", "",   "",   "",    "",   "1:2", "1:2"}
};

std::string rawTrigs5_8[4][16] = {
    {"1:2",  "",   "1:2", "",   "1:2",  "",   "AFT", "",   "1:2",  "",   "1:2", "",   "1:2",  "",   "AFT", ""},
    {"",    "",   "",    "",   "1:2",  "",   "",    "",   "",    "",   "",    "",   "1:2",  "",   "",    ""},
    {"AFT", "",   "1:2",  "",   "",    "",   "1:2",  "",   "AFT", "",   "1:2",  "",   "",    "",   "1:2",  ""},
    {"",    "",   "3:4", "3:4", "",    "",   "2:2", "2:2", "",    "",   "1:2", "1:2", "",    "",   "1:2", "1:2"}
};

int rawRetrigs1_4[4][16] = {
    {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0},
    {0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 8,  0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0},
    {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0, 12, 0}
};

int rawRetrigs5_8[4][16] = {
    {0, 0, 4, 0, 0, 0, 8, 0, 0, 0, 4, 0, 0, 0, 8, 0},
    {0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 4, 4, 0, 0, 8, 8, 0, 0, 12, 12, 0, 0, 16, 16}
};

int rawVels1_4[4][16] = {
    {3, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0},
    {0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0},
    {2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0},
    {0, 0, 3, 0, 0, 0, 0, 2, 0, 2, 0, 0, 0, 0, 3, 3}
};

int rawVels5_8[4][16] = {
    {3, 0, 3, 0, 2, 0, 2, 0, 3, 0, 3, 0, 2, 0, 2, 0},
    {0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 2, 0},
    {3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0},
    {0, 0, 3, 3, 0, 0, 3, 3, 0, 0, 3, 3, 0, 0, 3, 3}
};

const char* GetTrackName(int trackIndex) {
    if (trackIndex < 4) {
        return trackNames1_4[trackIndex];
    } else {
        return trackNames5_8[trackIndex - 4];
    }
}

void InitializeTracks() {
    // Automatically create directories on startup if they do not exist
    try {
        std::filesystem::create_directories("projects");
        std::filesystem::create_directories("patterns");
        std::filesystem::create_directories("presets");
        std::filesystem::create_directories("samples");
    } catch (...) {
        // Fail silently if directory creation fails (e.g., read-only sandbox)
    }

    for (int t = 0; t < 8; ++t) {
            tracks[t].name = GetTrackName(t);
            
            // Loop up to 32 steps instead of 16
            for (int s = 0; s < 32; ++s) {
                if (s < 16) {
                    // Page 1 (Steps 1-16) receives your startup demo data
                    if (t < 4) {
                        tracks[t].steps[s].note = rawNotes1_4[t][s];
                        tracks[t].steps[s].velocity = rawVels1_4[t][s];
                        tracks[t].steps[s].condition = rawTrigs1_4[t][s];
                        tracks[t].steps[s].retrigger = rawRetrigs1_4[t][s];
                    } else {
                        tracks[t].steps[s].note = rawNotes5_8[t - 4][s];
                        tracks[t].steps[s].velocity = rawVels5_8[t - 4][s];
                        tracks[t].steps[s].condition = rawTrigs5_8[t - 4][s];
                        tracks[t].steps[s].retrigger = rawRetrigs5_8[t - 4][s];
                    }
                } else {
                    // Page 2 (Steps 17-32) is initialized as completely empty
                    tracks[t].steps[s].note = "";
                    tracks[t].steps[s].velocity = 0;
                    tracks[t].steps[s].condition = "";
                    tracks[t].steps[s].retrigger = 0;
                }
                // Reset all parameter locks and microtiming across all 32 steps
                tracks[t].steps[s].params.reset();
                tracks[t].steps[s].microtiming = 0;
            }
        tracks[t].localTick = -1;
        // Initialize default core engine routing (Track 1 & Tracks 5-8: Synths, Tracks 2-4: Samplers)
        if (t == 0 || t >= 4) {
            tracks[t].engineType = ENGINE_SYNTH;
            tracks[t].algorithm = ALGO_PARALLEL;
        } else {
            tracks[t].engineType = ENGINE_SAMPLER;
            tracks[t].algorithm = ALGO_SAMPLE;
        }

        tracks[t].sampleSlot = 0;
                tracks[t].morph = 0; tracks[t].coarse = 0; tracks[t].fine = 0; tracks[t].volume = 99;
                tracks[t].attack = 10; tracks[t].decay = 30; tracks[t].sustain = 75; tracks[t].release = 20;
                tracks[t].morph2 = 50; tracks[t].coarse2 = 12; tracks[t].fine2 = 15; tracks[t].volume2 = 70;
                tracks[t].attack2 = 20; tracks[t].decay2 = 45; tracks[t].sustain2 = 50; tracks[t].release2 = 30;

                tracks[t].pitchSweepDepth = 0; // Pitch sweep depth always down (0) on startup
                tracks[t].pitchSweepTime = defaultPitchSweepTime[t];
                tracks[t].reverbSend = defaultReverbSend[t];
                tracks[t].delaySend = defaultDelaySend[t];
                tracks[t].sampleRateRed = defaultSampleRateRed[t];
                tracks[t].bitRed = defaultBitRed[t];
                
                tracks[t].fmFeedback = 20;
                tracks[t].noiseVolume = 40;
                tracks[t].noiseAttack = 10;
                tracks[t].noiseHold = 30;
                tracks[t].noiseDecay = 40;

                // Populate sampler defaults
                tracks[t].sampleStart = 0;   // Start always at the beginning
                tracks[t].sampleLength = 99;  // End always at the end
                tracks[t].sampleLoop = 0;    // One-Shot by default
                tracks[t].sampleTune = 0;
                tracks[t].loopStart = 0;     // Loop start always at the beginning
                tracks[t].loopEnd = 99;      // Loop end always at the end
                tracks[t].sliceDivisions = 8;
        tracks[t].tapeMemory = 50;
                tracks[t].tapeHeads = 1;
                tracks[t].tapeSpread = 0;
                tracks[t].tapeSpeed = 74;
                tracks[t].tapeTether = 99;
                tracks[t].tapeDrift = 10;
                tracks[t].tapeDriftRate = 20;
                tracks[t].tapeFeedback = 30;
                tracks[t].tapeFbSpread = 10;
                tracks[t].tapeFbSource = 0;
                tracks[t].tapeFreeze = 0;
                tracks[t].tapeSmearRate = 0;
                tracks[t].tapeSmearSize = 40;
                tracks[t].tapeMix = 0;
                tracks[t].tapeFX.reset();
        tracks[t].polyMode = 1;

        // Populate granular defaults
        tracks[t].grainSize = 15;
        tracks[t].grainDensity = 50;
        tracks[t].grainPosition = 12;
        tracks[t].grainScatter = 10;

        tracks[t].filterCutoff = 80;
                tracks[t].filterResonance = 20;
                tracks[t].filterType = 0;
                tracks[t].filterEnvDepth = 30;
                tracks[t].filterAttack = 15;
                tracks[t].filterDecay = 35;
                tracks[t].filterSustain = 60;
                tracks[t].filterRelease = 25;
                tracks[t].lfo1Wave = 0;
                tracks[t].lfo1Speed = 25;
                tracks[t].lfo1Depth = 40;
                tracks[t].lfo1Trigger = 0;
                tracks[t].lfo1Sync = 0;
                tracks[t].lfo1Dest = 0;
                tracks[t].lfo2Wave = 1;
                tracks[t].lfo2Speed = 30;
                tracks[t].lfo2Depth = 20;
                tracks[t].lfo2Trigger = 0;
                tracks[t].lfo2Sync = 1;
                tracks[t].lfo2Dest = 3;

                // Initialize LFO Phase variables
                tracks[t].lfo1Phase = 0.0f;
                tracks[t].lfo2Phase = 0.0f;
                tracks[t].lfo1LastVal = 0.0f;
                tracks[t].lfo2LastVal = 0.0f;

                // Initialize 3 routing slots per LFO to clean "None" states
                for (int i = 0; i < 3; ++i) {
                    tracks[t].lfo1Slots[i] = ModSlot{0, 0, 0, 0};
                    tracks[t].lfo2Slots[i] = ModSlot{0, 0, 0, 0};
                }

                // Set Slot 1 of LFO 1 to target local Cutoff with +40 depth by default
                tracks[t].lfo1Slots[0] = ModSlot{1, t, DEST_CUTOFF, 40};

                // Set Slot 1 of LFO 2 to target local Morph 2 with +20 depth by default
                tracks[t].lfo2Slots[0] = ModSlot{1, t, DEST_MORPH2, 20};

                tracks[t].saturationSend = 0;
        tracks[t].masterVolume = 99;
        tracks[t].autoPanSend = 0;
        tracks[t].muted = false;
    }

    // Pre-populate all 8 pattern slots in RAM on startup with clean defaults
    for (int p = 0; p < 8; ++p) {
        for (int t = 0; t < 8; ++t) {
            patterns[p].tracks[t] = tracks[t];
        }
    }

    // --- GENERATE SYNTHETIC DEFAULT PLUCK INTO POOL SLOT 1 ---
        // Allows testing Slicing, Loops, and Granular cloud algorithms instantly on startup!
        g_samplePool[0].name = "DFL_PLK";
        g_samplePool[0].pcmData.resize(32000); // 1 second of audio at 32000Hz
        for (int i = 0; i < 32000; ++i) {
            float t = (float)i / 32000.0f;
            
            // Generate a 440Hz sine wave
            float sample = sinf(2.0f * 3.14159265f * 440.0f * t);
            
            // Apply a smooth exponential pluck decay envelope
            float envelope = expf(-6.0f * t);
            sample *= envelope;

            g_samplePool[0].pcmData[i] = (int16_t)(sample * 32767.0f);
        }

        // Pre-calculate visual peaks for the default pluck
        for (int i = 0; i < 97; ++i) {
            size_t startFrame = (i * 32000) / 97;
            size_t endFrame = ((i + 1) * 32000) / 97;
            int16_t peak = 0;
            for (size_t f = startFrame; f < endFrame; ++f) {
                int16_t absVal = std::abs(g_samplePool[0].pcmData[f]);
                if (absVal > peak) peak = absVal;
            }
            g_samplePool[0].visualPeaks[i] = (uint8_t)((peak / 32768.0f) * 11.0f);
        }
    }

int NoteToMidi(const std::string& noteStr) {
    if (noteStr.empty()) return -1;
    std::string name = "";
    name += noteStr[0];
    size_t i = 1;
    if (i < noteStr.length() && (noteStr[i] == '#' || noteStr[i] == 'b' || noteStr[i] == '-')) {
        name += noteStr[i];
        i++;
    }
    int octave = 4;
    if (i < noteStr.length()) octave = noteStr[i] - '0';
    auto it = std::find(scaleNotes.begin(), scaleNotes.end(), name);
    int semitone = 0;
    if (it != scaleNotes.end()) semitone = std::distance(scaleNotes.begin(), it);
    return (octave + 1) * 12 + semitone;
}

std::string MidiToNote(int midi) {
    if (midi < 0 || midi > 127) return "";
    int octave = (midi / 12) - 1;
    int semitone = midi % 12;
    return scaleNotes[semitone] + std::to_string(octave);
}

std::string TransposeNote(const std::string& noteStr, int semitones) {
    if (noteStr.empty()) return "C4";
    int midi = NoteToMidi(noteStr);
    midi += semitones;
    if (midi < 12)  midi = 12;
    if (midi > 108) midi = 108;
    return MidiToNote(midi);
}

// Reset a single track's parameters back to startup defaults
void ResetTrackToDefault(int t) {
    if (t == 0 || t >= 4) {
        tracks[t].engineType = ENGINE_SYNTH;
        tracks[t].algorithm = ALGO_PARALLEL;
    } else {
        tracks[t].engineType = ENGINE_SAMPLER;
        tracks[t].algorithm = ALGO_SAMPLE;
    }

    // Reset step parameters lock overrides
    for (int s = 0; s < 16; ++s) {
        tracks[t].steps[s].params.reset();
        tracks[t].steps[s].microtiming = 0;
    }

    tracks[t].sampleSlot = 0;
    tracks[t].morph = 0;
    tracks[t].coarse = 0;
    tracks[t].fine = 0;
    tracks[t].volume = 99;
    tracks[t].attack = 10;
    tracks[t].decay = 30;
    tracks[t].sustain = 75;
    tracks[t].release = 20;

    tracks[t].morph2 = 50;
    tracks[t].coarse2 = 12;
    tracks[t].fine2 = 15;
    tracks[t].volume2 = 70;
    tracks[t].attack2 = 20;
    tracks[t].decay2 = 45;
    tracks[t].sustain2 = 50;
    tracks[t].release2 = 30;

    tracks[t].fmFeedback = 20;
        tracks[t].noiseVolume = 40;
        tracks[t].noiseAttack = 10;
        tracks[t].noiseHold = 30;
        tracks[t].noiseDecay = 40;

        tracks[t].pitchSweepDepth = 0; // Pitch sweep depth always down (0) on reset
        tracks[t].pitchSweepTime = defaultPitchSweepTime[t];
        tracks[t].reverbSend = defaultReverbSend[t];
        tracks[t].delaySend = defaultDelaySend[t];
        tracks[t].sampleRateRed = defaultSampleRateRed[t];
        tracks[t].bitRed = defaultBitRed[t];

        tracks[t].sampleStart = 0;   // Start always at the beginning
        tracks[t].sampleLength = 99;  // End always at the end
        tracks[t].sampleLoop = 0;
        tracks[t].sampleTune = 0;
        tracks[t].loopStart = 0;     // Loop start always at the beginning
        tracks[t].loopEnd = 99;      // Loop end always at the end
        tracks[t].sliceDivisions = 8;
    tracks[t].tapeMemory = 50;
        tracks[t].tapeHeads = 1;
        tracks[t].tapeSpread = 0;
        tracks[t].tapeSpeed = 74;
        tracks[t].tapeTether = 99;
        tracks[t].tapeDrift = 10;
        tracks[t].tapeDriftRate = 20;
        tracks[t].tapeFeedback = 30;
        tracks[t].tapeFbSpread = 10;
        tracks[t].tapeFbSource = 0;
        tracks[t].tapeFreeze = 0;
        tracks[t].tapeSmearRate = 0;
        tracks[t].tapeSmearSize = 40;
        tracks[t].tapeMix = 0;
        tracks[t].tapeFX.reset();
    tracks[t].polyMode = 1;
    tracks[t].grainSize = 15;
    tracks[t].grainDensity = 50;
    tracks[t].grainPosition = 12;
    tracks[t].grainScatter = 10;

    tracks[t].filterCutoff = 80;
    tracks[t].filterResonance = 20;
    tracks[t].filterType = 0;
    tracks[t].filterEnvDepth = 30;
    tracks[t].filterAttack = 15;
    tracks[t].filterDecay = 35;
    tracks[t].filterSustain = 60;
    tracks[t].filterRelease = 25;
    tracks[t].lfo1Wave = 0;
        tracks[t].lfo1Speed = 25;
        tracks[t].lfo1Depth = 40;
        tracks[t].lfo1Trigger = 0;
        tracks[t].lfo1Sync = 0;
        tracks[t].lfo1Dest = 0;
        tracks[t].lfo2Wave = 1;
        tracks[t].lfo2Speed = 30;
        tracks[t].lfo2Depth = 20;
        tracks[t].lfo2Trigger = 0;
        tracks[t].lfo2Sync = 1;
        tracks[t].lfo2Dest = 3;

        // Reset LFO Phase variables
        tracks[t].lfo1Phase = 0.0f;
        tracks[t].lfo2Phase = 0.0f;
        tracks[t].lfo1LastVal = 0.0f;
        tracks[t].lfo2LastVal = 0.0f;

        // Reset slots to safe "None" states
        for (int i = 0; i < 3; ++i) {
            tracks[t].lfo1Slots[i] = ModSlot{0, 0, 0, 0};
            tracks[t].lfo2Slots[i] = ModSlot{0, 0, 0, 0};
        }
        tracks[t].lfo1Slots[0] = ModSlot{1, t, DEST_CUTOFF, 40};
        tracks[t].lfo2Slots[0] = ModSlot{1, t, DEST_MORPH2, 20};

        tracks[t].saturationSend = 0;
    tracks[t].autoPanSend = 0;
    tracks[t].masterVolume = 99;
    tracks[t].muted = false;
}

// Fetch all files inside a directory and its subfolders ending with a specific extension [2]
std::vector<std::string> GetFileList(const std::string& directory, const std::string& extension) {
    std::vector<std::string> files;
    if (!std::filesystem::exists(directory)) return files;

    // Use recursive_directory_iterator to scan subfolders automatically
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            // Obtain path relative to the base directory (e.g. "drums/kick01")
            std::filesystem::path relPath = std::filesystem::relative(entry.path(), directory);
            files.push_back(relPath.replace_extension("").string());
        }
    }
    
    // Sort the list alphabetically and return it
    std::sort(files.begin(), files.end());
    return files;
}
// Search a directory for a specific slot-formatted file [2]
std::string GetSlotName(const std::string& directory, int slotNum, const std::string& extension) {
    if (!std::filesystem::exists(directory)) return "Empty";
    
    std::string prefix = "slot_" + std::to_string(slotNum) + "_";
    
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            std::string name = entry.path().stem().string();
            if (name.rfind(prefix, 0) == 0) {
                return name.substr(prefix.length()); // Extract custom string name [2]
            }
        }
    }
    return "Empty";
}

// Scan directory and delete any file starting with our target slot prefix [2]
void ClearSlotFile(const std::string& directory, int slotNum, const std::string& extension) {
    if (!std::filesystem::exists(directory)) return;
    
    std::string prefix = "slot_" + std::to_string(slotNum) + "_";
    
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            std::string name = entry.path().stem().string();
            if (name.rfind(prefix, 0) == 0) {
                std::filesystem::remove(entry.path()); // Delete old slot file [2]
            }
        }
    }
}

bool SaveSoundPreset(int trackIdx, int slot, const std::string& filename) {
    ClearSlotFile("presets", slot, ".snd");

    std::string path = "presets/slot_" + std::to_string(slot) + "_" + filename + ".snd";
    std::ofstream file(path);
    if (!file.is_open()) return false;

    const Track& trk = tracks[trackIdx];
    file << (int)trk.engineType << "\n" << trk.algorithm << "\n";
    file << trk.morph << "\n" << trk.coarse << "\n" << trk.fine << "\n" << trk.volume << "\n";
    file << trk.attack << "\n" << trk.decay << "\n" << trk.sustain << "\n" << trk.release << "\n";
    file << trk.morph2 << "\n" << trk.coarse2 << "\n" << trk.fine2 << "\n" << trk.volume2 << "\n";
    file << trk.attack2 << "\n" << trk.decay2 << "\n" << trk.sustain2 << "\n" << trk.release2 << "\n";
    file << trk.fmFeedback << "\n" << trk.noiseVolume << "\n";
    file << trk.noiseAttack << "\n" << trk.noiseHold << "\n" << trk.noiseDecay << "\n";
    file << trk.filterCutoff << "\n" << trk.filterResonance << "\n" << trk.filterType << "\n" << trk.filterEnvDepth << "\n";
    file << trk.filterAttack << "\n" << trk.filterDecay << "\n" << trk.filterSustain << "\n" << trk.filterRelease << "\n";
    file << trk.lfo1Wave << "\n" << trk.lfo1Speed << "\n" << trk.lfo1Depth << "\n" << trk.lfo1Trigger << "\n" << trk.lfo1Sync << "\n" << trk.lfo1Dest << "\n";
    for (int i = 0; i < 3; ++i) {
        file << trk.lfo1Slots[i].destType << "\n"
             << trk.lfo1Slots[i].destTrack << "\n"
             << trk.lfo1Slots[i].destParam << "\n"
             << trk.lfo1Slots[i].depth << "\n";
    }
    file << trk.lfo2Wave << "\n" << trk.lfo2Speed << "\n" << trk.lfo2Depth << "\n" << trk.lfo2Trigger << "\n" << trk.lfo2Sync << "\n" << trk.lfo2Dest << "\n";
    for (int i = 0; i < 3; ++i) {
        file << trk.lfo2Slots[i].destType << "\n"
             << trk.lfo2Slots[i].destTrack << "\n"
             << trk.lfo2Slots[i].destParam << "\n"
             << trk.lfo2Slots[i].depth << "\n";
    }
    file << trk.reverbSend << "\n" << trk.delaySend << "\n" << trk.saturationSend << "\n" << trk.autoPanSend << "\n";
    file << trk.sampleStart << "\n" << trk.sampleLength << "\n" << trk.sampleLoop << "\n" << trk.sampleTune << "\n";
    file << trk.loopStart << "\n" << trk.loopEnd << "\n" << trk.sliceDivisions << "\n";
    file << trk.grainSize << "\n" << trk.grainDensity << "\n" << trk.grainPosition << "\n" << trk.grainScatter << "\n";
    file << trk.masterVolume << "\n";

    // Save Tape Buffer parameters
    file << trk.tapeMemory << "\n" << trk.tapeHeads << "\n" << trk.tapeSpread << "\n" << trk.tapeSpeed << "\n"
         << trk.tapeTether << "\n" << trk.tapeDrift << "\n" << trk.tapeDriftRate << "\n" << trk.tapeFeedback << "\n"
         << trk.tapeFbSpread << "\n" << trk.tapeFbSource << "\n" << trk.tapeFreeze << "\n" << trk.tapeSmearRate << "\n"
         << trk.tapeSmearSize << "\n" << trk.tapeMix << "\n";

    // Save Poly/Mono Voice Mode
    file << trk.polyMode << "\n";

    // Save referenced Sample Slot and Name for automatic recall
    file << trk.sampleSlot << "\n";
    file << (g_samplePool[trk.sampleSlot].name.empty() ? "Empty" : g_samplePool[trk.sampleSlot].name) << "\n";

    return true;
}

bool LoadSoundPreset(int trackIdx, const std::string& filename) {
    std::string path = "presets/" + filename + ".snd";
    std::ifstream file(path);
    if (!file.is_open()) return false;

    Track& trk = tracks[trackIdx];
    int engineTypeVal = 0;
    file >> engineTypeVal; trk.engineType = (EngineType)engineTypeVal;
    file >> trk.algorithm;
    file >> trk.morph >> trk.coarse >> trk.fine >> trk.volume;
    file >> trk.attack >> trk.decay >> trk.sustain >> trk.release;
    file >> trk.morph2 >> trk.coarse2 >> trk.fine2 >> trk.volume2;
    file >> trk.attack2 >> trk.decay2 >> trk.sustain2 >> trk.release2;
    file >> trk.fmFeedback >> trk.noiseVolume;
    file >> trk.noiseAttack >> trk.noiseHold >> trk.noiseDecay;
    file >> trk.filterCutoff >> trk.filterResonance >> trk.filterType >> trk.filterEnvDepth;
    file >> trk.filterAttack >> trk.filterDecay >> trk.filterSustain >> trk.filterRelease;
    file >> trk.lfo1Wave >> trk.lfo1Speed >> trk.lfo1Depth >> trk.lfo1Trigger >> trk.lfo1Sync >> trk.lfo1Dest;
    for (int i = 0; i < 3; ++i) {
        file >> trk.lfo1Slots[i].destType
             >> trk.lfo1Slots[i].destTrack
             >> trk.lfo1Slots[i].destParam
             >> trk.lfo1Slots[i].depth;
    }
    file >> trk.lfo2Wave >> trk.lfo2Speed >> trk.lfo2Depth >> trk.lfo2Trigger >> trk.lfo2Sync >> trk.lfo2Dest;
    for (int i = 0; i < 3; ++i) {
        file >> trk.lfo2Slots[i].destType
             >> trk.lfo2Slots[i].destTrack
             >> trk.lfo2Slots[i].destParam
             >> trk.lfo2Slots[i].depth;
    }
    file >> trk.reverbSend >> trk.delaySend >> trk.saturationSend >> trk.autoPanSend;
    file >> trk.sampleStart >> trk.sampleLength >> trk.sampleLoop >> trk.sampleTune;
    file >> trk.loopStart >> trk.loopEnd >> trk.sliceDivisions;
    file >> trk.grainSize >> trk.grainDensity >> trk.grainPosition >> trk.grainScatter;
    if (!(file >> trk.masterVolume)) trk.masterVolume = 99; // <--- ADD THIS LINE (Safe fallback)
    // Load Tape Buffer parameters
    file >> trk.tapeMemory >> trk.tapeHeads >> trk.tapeSpread >> trk.tapeSpeed
         >> trk.tapeTether >> trk.tapeDrift >> trk.tapeDriftRate >> trk.tapeFeedback
         >> trk.tapeFbSpread >> trk.tapeFbSource >> trk.tapeFreeze >> trk.tapeSmearRate
         >> trk.tapeSmearSize >> trk.tapeMix;

    // Load Poly/Mono Voice Mode
    file >> trk.polyMode;

    // Load referenced Sample Slot and Name
    file >> trk.sampleSlot;
    std::string sampleName;
    file >> sampleName;

    if (trk.engineType == ENGINE_SAMPLER && sampleName != "Empty" && !sampleName.empty()) {
        if (g_samplePool[trk.sampleSlot].name != sampleName) {
            LoadSampleToPool(trk.sampleSlot, sampleName);
        }
    }

    return true;
}

bool SavePattern(int patternIdx, int slot, const std::string& filename) {
    ClearSlotFile("patterns", slot, ".pat");

    std::string path = "patterns/slot_" + std::to_string(slot) + "_" + filename + ".pat";
    std::ofstream file(path);
    if (!file.is_open()) return false;

    // Temporarily dump global live tracks to RAM slot before writing
    for (int t = 0; t < 8; ++t) {
        patterns[patternIdx].tracks[t] = tracks[t];
    }

    const Pattern& pat = patterns[patternIdx];
    for (int t = 0; t < 8; ++t) {
        const Track& trk = pat.tracks[t];
        file << (int)trk.engineType << "\n" << trk.algorithm << "\n";
        file << trk.morph << "\n" << trk.coarse << "\n" << trk.fine << "\n" << trk.volume << "\n";
        file << trk.attack << "\n" << trk.decay << "\n" << trk.sustain << "\n" << trk.release << "\n";
        file << trk.morph2 << "\n" << trk.coarse2 << "\n" << trk.fine2 << "\n" << trk.volume2 << "\n";
        file << trk.attack2 << "\n" << trk.decay2 << "\n" << trk.sustain2 << "\n" << trk.release2 << "\n";
        file << trk.fmFeedback << "\n" << trk.noiseVolume << "\n";
        file << trk.noiseAttack << "\n" << trk.noiseHold << "\n" << trk.noiseDecay << "\n";
        file << trk.filterCutoff << "\n" << trk.filterResonance << "\n" << trk.filterType << "\n" << trk.filterEnvDepth << "\n";
        file << trk.filterAttack << "\n" << trk.filterDecay << "\n" << trk.filterSustain << "\n" << trk.filterRelease << "\n";
        file << trk.lfo1Wave << "\n" << trk.lfo1Speed << "\n" << trk.lfo1Depth << "\n" << trk.lfo1Trigger << "\n" << trk.lfo1Sync << "\n" << trk.lfo1Dest << "\n";
        for (int i = 0; i < 3; ++i) {
            file << trk.lfo1Slots[i].destType << "\n"
                 << trk.lfo1Slots[i].destTrack << "\n"
                 << trk.lfo1Slots[i].destParam << "\n"
                 << trk.lfo1Slots[i].depth << "\n";
        }
        file << trk.lfo2Wave << "\n" << trk.lfo2Speed << "\n" << trk.lfo2Depth << "\n" << trk.lfo2Trigger << "\n" << trk.lfo2Sync << "\n" << trk.lfo2Dest << "\n";
        for (int i = 0; i < 3; ++i) {
            file << trk.lfo2Slots[i].destType << "\n"
                 << trk.lfo2Slots[i].destTrack << "\n"
                 << trk.lfo2Slots[i].destParam << "\n"
                 << trk.lfo2Slots[i].depth << "\n";
        }
        file << trk.reverbSend << "\n" << trk.delaySend << "\n" << trk.saturationSend << "\n" << trk.autoPanSend << "\n";
        file << trk.sampleStart << "\n" << trk.sampleLength << "\n" << trk.sampleLoop << "\n" << trk.sampleTune << "\n";
        file << trk.loopStart << "\n" << trk.loopEnd << "\n" << trk.sliceDivisions << "\n";
        file << trk.grainSize << "\n" << trk.grainDensity << "\n" << trk.grainPosition << "\n" << trk.grainScatter << "\n";
        file << trk.masterVolume << "\n";

        // Save Tape Buffer track defaults
        file << trk.tapeMemory << "\n" << trk.tapeHeads << "\n" << trk.tapeSpread << "\n" << trk.tapeSpeed << "\n"
             << trk.tapeTether << "\n" << trk.tapeDrift << "\n" << trk.tapeDriftRate << "\n" << trk.tapeFeedback << "\n"
             << trk.tapeFbSpread << "\n" << trk.tapeFbSource << "\n" << trk.tapeFreeze << "\n" << trk.tapeSmearRate << "\n"
             << trk.tapeSmearSize << "\n" << trk.tapeMix << "\n";

        // Save Poly/Mono Voice Mode
        file << trk.polyMode << "\n";

        // Save 16 steps
        for (int s = 0; s < 32; ++s) {
            const Step& step = trk.steps[s];
            file << (step.note.empty() ? "-" : step.note) << "\n";
            file << step.velocity << "\n";
            file << (step.condition.empty() ? "-" : step.condition) << "\n";
            file << step.retrigger << "\n";
            file << step.microtiming << "\n"; // Save microtiming

            // Step overrides
            const StepParams& sp = step.params;
            file << sp.morph << "\n" << sp.coarse << "\n" << sp.fine << "\n" << sp.volume << "\n";
            file << sp.attack << "\n" << sp.decay << "\n" << sp.sustain << "\n" << sp.release << "\n";
            file << sp.morph2 << "\n" << sp.coarse2 << "\n" << sp.fine2 << "\n" << sp.volume2 << "\n";
            file << sp.attack2 << "\n" << sp.decay2 << "\n" << sp.sustain2 << "\n" << sp.release2 << "\n";
            file << sp.fmFeedback << "\n" << sp.noiseVolume << "\n" << sp.noiseAttack << "\n" << sp.noiseHold << "\n" << sp.noiseDecay << "\n";
            file << sp.filterCutoff << "\n" << sp.filterResonance << "\n" << sp.filterType << "\n" << sp.filterEnvDepth << "\n";
            file << sp.filterAttack << "\n" << sp.filterDecay << "\n" << sp.filterSustain << "\n" << sp.filterRelease << "\n";
            file << sp.lfo1Wave << "\n" << sp.lfo1Speed << "\n" << sp.lfo1Depth << "\n" << sp.lfo1Trigger << "\n" << sp.lfo1Sync << "\n" << sp.lfo1Dest << "\n";
            file << sp.lfo2Wave << "\n" << sp.lfo2Speed << "\n" << sp.lfo2Depth << "\n" << sp.lfo2Trigger << "\n" << sp.lfo2Sync << "\n" << sp.lfo2Dest << "\n";
            file << sp.reverbSend << "\n" << sp.delaySend << "\n" << sp.saturationSend << "\n" << sp.autoPanSend << "\n";
            file << sp.sampleSlot << "\n"; // Save step-locked sample slot [2]
            file << sp.masterVolume << "\n";
            
            // Save step-locked Tape Buffer parameters
            file << sp.tapeMemory << "\n" << sp.tapeHeads << "\n" << sp.tapeSpread << "\n" << sp.tapeSpeed << "\n"
                 << sp.tapeTether << "\n" << sp.tapeDrift << "\n" << sp.tapeDriftRate << "\n" << sp.tapeFeedback << "\n"
                 << sp.tapeFbSpread << "\n" << sp.tapeFbSource << "\n" << sp.tapeFreeze << "\n" << sp.tapeSmearRate << "\n"
                 << sp.tapeSmearSize << "\n" << sp.tapeMix << "\n";

            // Save step-locked Poly/Mono Voice Mode
            file << sp.polyMode << "\n";
        }

        // Save track-default Sample Slot and Name
        file << trk.sampleSlot << "\n";
        file << (g_samplePool[trk.sampleSlot].name.empty() ? "Empty" : g_samplePool[trk.sampleSlot].name) << "\n";
    }
    return true;
}

bool LoadPattern(int patternIdx, const std::string& filename) {
    std::string path = "patterns/" + filename + ".pat";
    std::ifstream file(path);
    if (!file.is_open()) return false;

    Pattern& pat = patterns[patternIdx];
    for (int t = 0; t < 8; ++t) {
        Track& trk = pat.tracks[t];
        int engineVal = 0;
        file >> engineVal; trk.engineType = (EngineType)engineVal;
        file >> trk.algorithm;
        file >> trk.morph >> trk.coarse >> trk.fine >> trk.volume;
        file >> trk.attack >> trk.decay >> trk.sustain >> trk.release;
        file >> trk.morph2 >> trk.coarse2 >> trk.fine2 >> trk.volume2;
        file >> trk.attack2 >> trk.decay2 >> trk.sustain2 >> trk.release2;
        file >> trk.fmFeedback >> trk.noiseVolume;
        file >> trk.noiseAttack >> trk.noiseHold >> trk.noiseDecay;
        file >> trk.filterCutoff >> trk.filterResonance >> trk.filterType >> trk.filterEnvDepth;
        file >> trk.filterAttack >> trk.filterDecay >> trk.filterSustain >> trk.filterRelease;
        file >> trk.lfo1Wave >> trk.lfo1Speed >> trk.lfo1Depth >> trk.lfo1Trigger >> trk.lfo1Sync >> trk.lfo1Dest;
        for (int i = 0; i < 3; ++i) {
            file >> trk.lfo1Slots[i].destType
                 >> trk.lfo1Slots[i].destTrack
                 >> trk.lfo1Slots[i].destParam
                 >> trk.lfo1Slots[i].depth;
        }
        file >> trk.lfo2Wave >> trk.lfo2Speed >> trk.lfo2Depth >> trk.lfo2Trigger >> trk.lfo2Sync >> trk.lfo2Dest;
        for (int i = 0; i < 3; ++i) {
            file >> trk.lfo2Slots[i].destType
                 >> trk.lfo2Slots[i].destTrack
                 >> trk.lfo2Slots[i].destParam
                 >> trk.lfo2Slots[i].depth;
        }
        file >> trk.reverbSend >> trk.delaySend >> trk.saturationSend >> trk.autoPanSend;
        file >> trk.sampleStart >> trk.sampleLength >> trk.sampleLoop >> trk.sampleTune;
        file >> trk.loopStart >> trk.loopEnd >> trk.sliceDivisions;
        file >> trk.grainSize >> trk.grainDensity >> trk.grainPosition >> trk.grainScatter;
        if (!(file >> trk.masterVolume)) trk.masterVolume = 99;
        
        // Load Tape Buffer track defaults
        file >> trk.tapeMemory >> trk.tapeHeads >> trk.tapeSpread >> trk.tapeSpeed
             >> trk.tapeTether >> trk.tapeDrift >> trk.tapeDriftRate >> trk.tapeFeedback
             >> trk.tapeFbSpread >> trk.tapeFbSource >> trk.tapeFreeze >> trk.tapeSmearRate
             >> trk.tapeSmearSize >> trk.tapeMix;

        // Load Poly/Mono Voice Mode
        file >> trk.polyMode;

        for (int s = 0; s < 32; ++s) {
            Step& step = trk.steps[s];
            file >> step.note; if (step.note == "-") step.note = "";
            file >> step.velocity;
            file >> step.condition; if (step.condition == "-") step.condition = "";
            file >> step.retrigger;
            file >> step.microtiming; // Load microtiming

            StepParams& sp = step.params;
            file >> sp.morph >> sp.coarse >> sp.fine >> sp.volume;
            file >> sp.attack >> sp.decay >> sp.sustain >> sp.release;
            file >> sp.morph2 >> sp.coarse2 >> sp.fine2 >> sp.volume2;
            file >> sp.attack2 >> sp.decay2 >> sp.sustain2 >> sp.release2;
            file >> sp.fmFeedback >> sp.noiseVolume >> sp.noiseAttack >> sp.noiseHold >> sp.noiseDecay;
            file >> sp.filterCutoff >> sp.filterResonance >> sp.filterType >> sp.filterEnvDepth;
            file >> sp.filterAttack >> sp.filterDecay >> sp.filterSustain >> sp.filterRelease;
            file >> sp.lfo1Wave >> sp.lfo1Speed >> sp.lfo1Depth >> sp.lfo1Trigger >> sp.lfo1Sync >> sp.lfo1Dest;
            file >> sp.lfo2Wave >> sp.lfo2Speed >> sp.lfo2Depth >> sp.lfo2Trigger >> sp.lfo2Sync >> sp.lfo2Dest;
            file >> sp.reverbSend >> sp.delaySend >> sp.saturationSend >> sp.autoPanSend;
            file >> sp.sampleSlot; // Load step-locked sample slot [2]
            if (!(file >> sp.masterVolume)) sp.masterVolume = -1;
            
            // Load step-locked Tape Buffer parameters
            file >> sp.tapeMemory >> sp.tapeHeads >> sp.tapeSpread >> sp.tapeSpeed
                 >> sp.tapeTether >> sp.tapeDrift >> sp.tapeDriftRate >> sp.tapeFeedback
                 >> sp.tapeFbSpread >> sp.tapeFbSource >> sp.tapeFreeze >> sp.tapeSmearRate
                 >> sp.tapeSmearSize >> sp.tapeMix;

            // Load step-locked Poly/Mono Voice Mode
            file >> sp.polyMode;
        }

        // Load track-default Sample Slot and Name
        file >> trk.sampleSlot;
        std::string sampleName;
        file >> sampleName;

        if (trk.engineType == ENGINE_SAMPLER && sampleName != "Empty" && !sampleName.empty()) {
            if (g_samplePool[trk.sampleSlot].name != sampleName) {
                LoadSampleToPool(trk.sampleSlot, sampleName);
            }
        }

        // Sanitize missing/legacy variables
        if (trk.sliceDivisions < 1) trk.sliceDivisions = 8;
        if (trk.sampleLength < 1)   trk.sampleLength = 99;
        if (trk.loopEnd < 1)        trk.loopEnd = 99;
    }

    // Force flush RAM tracks back to screen if we are loading into the currently active slot
    if (patternIdx == activePattern) {
        for (int t = 0; t < 8; ++t) {
            tracks[t] = pat.tracks[t];
        }
    }

    return true;
}

bool SaveProject(int slot, const std::string& filename) {
    ClearSlotFile("projects", slot, ".prj");

    std::string path = "projects/slot_" + std::to_string(slot) + "_" + filename + ".prj";
    std::ofstream file(path);
    if (!file.is_open()) return false;

    // Sync live edits to current pattern slot
    for (int t = 0; t < 8; ++t) {
        patterns[activePattern].tracks[t] = tracks[t];
    }

    // Write the filenames of all 16 Sample Pool slots at the top of the project
    for (int i = 0; i < 16; ++i) {
        file << (g_samplePool[i].name.empty() ? "Empty" : g_samplePool[i].name) << "\n";
    }

    file << tempo << "\n";
    file << activePattern << "\n";

    // Save global master bus FX
    file << globalFX.reverbDecay << "\n" << globalFX.reverbSize << "\n" << globalFX.reverbPredelay << "\n" << globalFX.reverbMix << "\n";
    file << globalFX.satLevel << "\n" << globalFX.satSymmetry << "\n" << globalFX.satOverdrive << "\n" << globalFX.satMix << "\n";
    file << globalFX.delayTime << "\n" << globalFX.delayFeedback << "\n" << globalFX.delayPingPong << "\n" << globalFX.delayMix << "\n";
    file << globalFX.autoPanTime << "\n" << globalFX.autoPanFeedback << "\n" << globalFX.autoPanWidth << "\n" << globalFX.autoPanMix << "\n";

    // Save 8 patterns
    for (int p = 0; p < 8; ++p) {
        const Pattern& pat = patterns[p];
        for (int t = 0; t < 8; ++t) {
            const Track& trk = pat.tracks[t];
            file << (int)trk.engineType << "\n" << trk.algorithm << "\n";
            file << trk.morph << "\n" << trk.coarse << "\n" << trk.fine << "\n" << trk.volume << "\n";
            file << trk.attack << "\n" << trk.decay << "\n" << trk.sustain << "\n" << trk.release << "\n";
            file << trk.morph2 << "\n" << trk.coarse2 << "\n" << trk.fine2 << "\n" << trk.volume2 << "\n";
            file << trk.attack2 << "\n" << trk.decay2 << "\n" << trk.sustain2 << "\n" << trk.release2 << "\n";
            file << trk.fmFeedback << "\n" << trk.noiseVolume << "\n";
            file << trk.noiseAttack << "\n" << trk.noiseHold << "\n" << trk.noiseDecay << "\n";
            file << trk.filterCutoff << "\n" << trk.filterResonance << "\n" << trk.filterType << "\n" << trk.filterEnvDepth << "\n";
            file << trk.filterAttack << "\n" << trk.filterDecay << "\n" << trk.filterSustain << "\n" << trk.filterRelease << "\n";
            file << trk.lfo1Wave << "\n" << trk.lfo1Speed << "\n" << trk.lfo1Depth << "\n" << trk.lfo1Trigger << "\n" << trk.lfo1Sync << "\n" << trk.lfo1Dest << "\n";
            for (int i = 0; i < 3; ++i) {
                file << trk.lfo1Slots[i].destType << "\n"
                     << trk.lfo1Slots[i].destTrack << "\n"
                     << trk.lfo1Slots[i].destParam << "\n"
                     << trk.lfo1Slots[i].depth << "\n";
            }
            file << trk.lfo2Wave << "\n" << trk.lfo2Speed << "\n" << trk.lfo2Depth << "\n" << trk.lfo2Trigger << "\n" << trk.lfo2Sync << "\n" << trk.lfo2Dest << "\n";
            for (int i = 0; i < 3; ++i) {
                file << trk.lfo2Slots[i].destType << "\n"
                     << trk.lfo2Slots[i].destTrack << "\n"
                     << trk.lfo2Slots[i].destParam << "\n"
                     << trk.lfo2Slots[i].depth << "\n";
            }
            file << trk.reverbSend << "\n" << trk.delaySend << "\n" << trk.saturationSend << "\n" << trk.autoPanSend << "\n";
            file << trk.sampleStart << "\n" << trk.sampleLength << "\n" << trk.sampleLoop << "\n" << trk.sampleTune << "\n";
            file << trk.loopStart << "\n" << trk.loopEnd << "\n" << trk.sliceDivisions << "\n";
            file << trk.grainSize << "\n" << trk.grainDensity << "\n" << trk.grainPosition << "\n" << trk.grainScatter << "\n";
            file << trk.masterVolume << "\n";
            
            // Save Tape Buffer track defaults
            file << trk.tapeMemory << "\n" << trk.tapeHeads << "\n" << trk.tapeSpread << "\n" << trk.tapeSpeed << "\n"
                 << trk.tapeTether << "\n" << trk.tapeDrift << "\n" << trk.tapeDriftRate << "\n" << trk.tapeFeedback << "\n"
                 << trk.tapeFbSpread << "\n" << trk.tapeFbSource << "\n" << trk.tapeFreeze << "\n" << trk.tapeSmearRate << "\n"
                 << trk.tapeSmearSize << "\n" << trk.tapeMix << "\n";

            // Save Poly/Mono Voice Mode
            file << trk.polyMode << "\n";

            for (int s = 0; s < 32; ++s) {
                const Step& step = trk.steps[s];
                file << (step.note.empty() ? "-" : step.note) << "\n";
                file << step.velocity << "\n";
                file << (step.condition.empty() ? "-" : step.condition) << "\n";
                file << step.retrigger << "\n";
                file << step.microtiming << "\n"; // Save microtiming

                const StepParams& sp = step.params;
                file << sp.morph << "\n" << sp.coarse << "\n" << sp.fine << "\n" << sp.volume << "\n";
                file << sp.attack << "\n" << sp.decay << "\n" << sp.sustain << "\n" << sp.release << "\n";
                file << sp.morph2 << "\n" << sp.coarse2 << "\n" << sp.fine2 << "\n" << sp.volume2 << "\n";
                file << sp.attack2 << "\n" << sp.decay2 << "\n" << sp.sustain2 << "\n" << sp.release2 << "\n";
                file << sp.fmFeedback << "\n" << sp.noiseVolume << "\n" << sp.noiseAttack << "\n" << sp.noiseHold << "\n" << sp.noiseDecay << "\n";
                file << sp.filterCutoff << "\n" << sp.filterResonance << "\n" << sp.filterType << "\n" << sp.filterEnvDepth << "\n";
                file << sp.filterAttack << "\n" << sp.filterDecay << "\n" << sp.filterSustain << "\n" << sp.filterRelease << "\n";
                file << sp.lfo1Wave << "\n" << sp.lfo1Speed << "\n" << sp.lfo1Depth << "\n" << sp.lfo1Trigger << "\n" << sp.lfo1Sync << "\n" << sp.lfo1Dest << "\n";
                file << sp.lfo2Wave << "\n" << sp.lfo2Speed << "\n" << sp.lfo2Depth << "\n" << sp.lfo2Trigger << "\n" << sp.lfo2Sync << "\n" << sp.lfo2Dest << "\n";
                file << sp.reverbSend << "\n" << sp.delaySend << "\n" << sp.saturationSend << "\n" << sp.autoPanSend << "\n";
                file << sp.sampleSlot << "\n"; // Save step-locked sample slot [2]

                // Save step-locked Tape Buffer parameters
                file << sp.tapeMemory << "\n" << sp.tapeHeads << "\n" << sp.tapeSpread << "\n" << sp.tapeSpeed << "\n"
                     << sp.tapeTether << "\n" << sp.tapeDrift << "\n" << sp.tapeDriftRate << "\n" << sp.tapeFeedback << "\n"
                     << sp.tapeFbSpread << "\n" << sp.tapeFbSource << "\n" << sp.tapeFreeze << "\n" << sp.tapeSmearRate << "\n"
                     << sp.tapeSmearSize << "\n" << sp.tapeMix << "\n";

                // Save step-locked Poly/Mono Voice Mode
                file << sp.polyMode << "\n";
            }

            // Save track-default Sample Slot and Name
            file << trk.sampleSlot << "\n";
            file << (g_samplePool[trk.sampleSlot].name.empty() ? "Empty" : g_samplePool[trk.sampleSlot].name) << "\n";
        }
    }
    return true;
}

bool LoadProject(const std::string& filename) {
    std::string path = "projects/" + filename + ".prj";
    std::ifstream file(path);
    if (!file.is_open()) return false;

    // Load and automatically crunch the 16 Sample Pool slots in the background [2]
    for (int i = 0; i < 16; ++i) {
        std::string sampleName;
        file >> sampleName;
        if (sampleName != "Empty" && !sampleName.empty()) {
            LoadSampleToPool(i, sampleName);
        } else {
            g_samplePool[i].name = "Empty";
            g_samplePool[i].pcmData.clear();
        }
    }

    file >> tempo;
    file >> activePattern;

    file >> globalFX.reverbDecay >> globalFX.reverbSize >> globalFX.reverbPredelay >> globalFX.reverbMix;
    file >> globalFX.satLevel >> globalFX.satSymmetry >> globalFX.satOverdrive >> globalFX.satMix;
    file >> globalFX.delayTime >> globalFX.delayFeedback >> globalFX.delayPingPong >> globalFX.delayMix;
    file >> globalFX.autoPanTime >> globalFX.autoPanFeedback >> globalFX.autoPanWidth >> globalFX.autoPanMix;

    for (int p = 0; p < 8; ++p) {
        Pattern& pat = patterns[p];
        for (int t = 0; t < 8; ++t) {
            Track& trk = pat.tracks[t];
            int engineVal = 0;
            file >> engineVal; trk.engineType = (EngineType)engineVal;
            file >> trk.algorithm;
            file >> trk.morph >> trk.coarse >> trk.fine >> trk.volume;
            file >> trk.attack >> trk.decay >> trk.sustain >> trk.release;
            file >> trk.morph2 >> trk.coarse2 >> trk.fine2 >> trk.volume2;
            file >> trk.attack2 >> trk.decay2 >> trk.sustain2 >> trk.release2;
            file >> trk.fmFeedback >> trk.noiseVolume;
            file >> trk.noiseAttack >> trk.noiseHold >> trk.noiseDecay;
            file >> trk.filterCutoff >> trk.filterResonance >> trk.filterType >> trk.filterEnvDepth;
            file >> trk.filterAttack >> trk.filterDecay >> trk.filterSustain >> trk.filterRelease;
            file >> trk.lfo1Wave >> trk.lfo1Speed >> trk.lfo1Depth >> trk.lfo1Trigger >> trk.lfo1Sync >> trk.lfo1Dest;
            for (int i = 0; i < 3; ++i) {
                file >> trk.lfo1Slots[i].destType
                     >> trk.lfo1Slots[i].destTrack
                     >> trk.lfo1Slots[i].destParam
                     >> trk.lfo1Slots[i].depth;
            }
            file >> trk.lfo2Wave >> trk.lfo2Speed >> trk.lfo2Depth >> trk.lfo2Trigger >> trk.lfo2Sync >> trk.lfo2Dest;
            for (int i = 0; i < 3; ++i) {
                file >> trk.lfo2Slots[i].destType
                     >> trk.lfo2Slots[i].destTrack
                     >> trk.lfo2Slots[i].destParam
                     >> trk.lfo2Slots[i].depth;
            }
            file >> trk.reverbSend >> trk.delaySend >> trk.saturationSend >> trk.autoPanSend;
            file >> trk.sampleStart >> trk.sampleLength >> trk.sampleLoop >> trk.sampleTune;
            file >> trk.loopStart >> trk.loopEnd >> trk.sliceDivisions;
            file >> trk.grainSize >> trk.grainDensity >> trk.grainPosition >> trk.grainScatter;
            if (!(file >> trk.masterVolume)) trk.masterVolume = 99;
            
            // Load Tape Buffer track defaults
            file >> trk.tapeMemory >> trk.tapeHeads >> trk.tapeSpread >> trk.tapeSpeed
                 >> trk.tapeTether >> trk.tapeDrift >> trk.tapeDriftRate >> trk.tapeFeedback
                 >> trk.tapeFbSpread >> trk.tapeFbSource >> trk.tapeFreeze >> trk.tapeSmearRate
                 >> trk.tapeSmearSize >> trk.tapeMix;

            // Load Poly/Mono Voice Mode
            file >> trk.polyMode;

            for (int s = 0; s < 32; ++s) {
                Step& step = trk.steps[s];
                file >> step.note; if (step.note == "-") step.note = "";
                file >> step.velocity;
                file >> step.condition; if (step.condition == "-") step.condition = "";
                file >> step.retrigger;
                file >> step.microtiming; // Load microtiming

                StepParams& sp = step.params;
                file >> sp.morph >> sp.coarse >> sp.fine >> sp.volume;
                file >> sp.attack >> sp.decay >> sp.sustain >> sp.release;
                file >> sp.morph2 >> sp.coarse2 >> sp.fine2 >> sp.volume2;
                file >> sp.attack2 >> sp.decay2 >> sp.sustain2 >> sp.release2;
                file >> sp.fmFeedback >> sp.noiseVolume >> sp.noiseAttack >> sp.noiseHold >> sp.noiseDecay;
                file >> sp.filterCutoff >> sp.filterResonance >> sp.filterType >> sp.filterEnvDepth;
                file >> sp.filterAttack >> sp.filterDecay >> sp.filterSustain >> sp.filterRelease;
                file >> sp.lfo1Wave >> sp.lfo1Speed >> sp.lfo1Depth >> sp.lfo1Trigger >> sp.lfo1Sync >> sp.lfo1Dest;
                file >> sp.lfo2Wave >> sp.lfo2Speed >> sp.lfo2Depth >> sp.lfo2Trigger >> sp.lfo2Sync >> sp.lfo2Dest;
                file >> sp.reverbSend >> sp.delaySend >> sp.saturationSend >> sp.autoPanSend;
                file >> sp.sampleSlot; // Load step-locked sample slot [2]
                if (!(file >> sp.masterVolume)) sp.masterVolume = -1;
                
                // Load step-locked Tape Buffer parameters
                file >> sp.tapeMemory >> sp.tapeHeads >> sp.tapeSpread >> sp.tapeSpeed
                     >> sp.tapeTether >> sp.tapeDrift >> sp.tapeDriftRate >> sp.tapeFeedback
                     >> sp.tapeFbSpread >> sp.tapeFbSource >> sp.tapeFreeze >> sp.tapeSmearRate
                     >> sp.tapeSmearSize >> sp.tapeMix;

                // Load step-locked Poly/Mono Voice Mode
                file >> sp.polyMode;
            }

            // Load track-default Sample Slot and Name
            file >> trk.sampleSlot;
            std::string sampleName;
            file >> sampleName;

            if (trk.engineType == ENGINE_SAMPLER && sampleName != "Empty" && !sampleName.empty()) {
                if (g_samplePool[trk.sampleSlot].name != sampleName) {
                    LoadSampleToPool(trk.sampleSlot, sampleName);
                }
            }

            // Sanitize missing/legacy variables
            if (trk.sliceDivisions < 1) trk.sliceDivisions = 8;
            if (trk.sampleLength < 1)   trk.sampleLength = 99;
            if (trk.loopEnd < 1)        trk.loopEnd = 99;
        }
    }

    // Force load the active pattern data into global rendering array
    for (int t = 0; t < 8; ++t) {
        tracks[t] = patterns[activePattern].tracks[t];
    }

    return true;
}

// Thread-safe memory swapper performs a pattern swap in RAM
void SwitchPattern(int newPatternIndex) {
    if (newPatternIndex < 0 || newPatternIndex >= 8) return;

    // 1. Save all active user tweaks from the global tracks[8] array into current pattern slot
    for (int t = 0; t < 8; ++t) {
        patterns[activePattern].tracks[t] = tracks[t];
    }

    // 2. Update pattern index
    activePattern = newPatternIndex;

    // 3. Copy the target pattern slot's tracks back into global tracks[8]
    for (int t = 0; t < 8; ++t) {
        tracks[t] = patterns[activePattern].tracks[t];
    }
}
