#pragma once
#include "Common.hpp"
#include <vector>
#include <string>
#include <atomic>

extern std::atomic<bool> g_useExternalMidiClock;
extern std::atomic<int> g_externalMidiTicksQueued;
extern std::atomic<bool> g_externalMidiStartTriggered;
extern std::atomic<bool> g_externalMidiStopTriggered;
extern int selectedTrack;
extern Screen currentScreen;
extern int cursorStep;
extern int activePage;
extern int masterLength;   // Pattern loop limit: 16, 32, 64, or 0 (INF)
extern int stepUtilFocus;  // 0 = Microtiming, 1 = Track Length, 2 = Global Lengt

extern float g_audioCpuLoad;
extern bool showDiagnostics;
// Global Arrays and Tables
extern const NoteBinding keyboardPiano[];
extern const int NUM_NOTES;
extern const std::vector<std::string> scaleNotes;
extern const char* trackNames1_4[4];
extern const char* trackNames5_8[4];
extern const char* trackNamesTrigs[4];
extern const float sineTable[32];
extern const PixelOffset retrigPattern[16];
extern const std::vector<std::string> triggerOptions;
extern bool liveKeyboardActive;
// Global State Variables
extern Track tracks[8];
extern int synthMode;
extern const char* menuFeedback;
extern SampleAsset g_samplePool[16];
bool LoadSampleToPool(int slotIdx, const std::string& filename);
extern bool g_hardwareEncoderClicked;
extern float g_audioCpuLoad;
extern bool showDiagnostics;


// Track Initialization Defaults
extern const int defaultPitchSweepDepth[8];
extern const int defaultPitchSweepTime[8];
extern const int defaultReverbSend[8];
extern const int defaultDelaySend[8];
extern const int defaultSampleRateRed[8];
extern const int defaultBitRed[8];

// Decoupled global sequencer clocks, pattern slots, and master effects
extern GlobalFX globalFX;
extern int playhead;
extern double tempo;
extern bool isPlaying;

extern Pattern patterns[8];
extern int activePattern;
extern int queuedPattern;

// System Menu & File Browser state variables
extern bool systemMenuOpen;
extern int systemMenuCursor;
extern int systemMenuState;       // 0=Main, 1=Browser (Load), 2=Slot Select (Save), 3=Typing (Save)

// Global Modulation Matrix popup state variables
extern bool lfoPopupOpen;
extern int lfoPopupLfoIdx;        // 0 = LFO 1, 1 = LFO 2
extern int lfoPopupSlot;          // Active focused slot (0 to 2)
extern int lfoPopupField;         // Active focused field (0 = Type, 1 = Track, 2 = Param, 3 = Depth)
extern int fileBrowserCursor;     // Index of selected file in load browser
extern int saveSlotCursor;        // Index of selected slot in save selector (0..N-1)
extern std::string g_typingBuffer;
extern int g_typingCursor;
extern std::vector<std::string> g_fileList;

// Functions
const char* GetTrackName(int trackIndex);
void InitializeTracks();
void ResetTrackToDefault(int t);

int NoteToMidi(const std::string& noteStr);
std::string MidiToNote(int midi);
std::string TransposeNote(const std::string& noteStr, int semitones);

// Directory & Disk Serialization Functions
void SetupDirectories();
std::vector<std::string> GetFileList(const std::string& directory, const std::string& extension);
std::string GetSlotName(const std::string& directory, int slotNum, const std::string& extension);
void ClearSlotFile(const std::string& directory, int slotNum, const std::string& extension);

bool SaveSoundPreset(int trackIdx, int slot, const std::string& filename);
bool LoadSoundPreset(int trackIdx, const std::string& filename);

bool SavePattern(int patternIdx, int slot, const std::string& filename);
bool LoadPattern(int patternIdx, const std::string& filename);

bool SaveProject(int slot, const std::string& filename);
bool LoadProject(const std::string& filename);


void SwitchPattern(int newPatternIndex);
