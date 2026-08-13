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
extern int stepUtilFocus;  // TRK tab: 0 = Track Len, 1 = Global Len, 2 = Swing, 3 = Key
extern int globalKeyRoot;  // 0=C .. 11=B
extern int globalKeyLock;  // 0=CHR, 1=MAJ

// --- GENERATIVE CHAOS (global; see Chaos.hpp) ---
extern int g_chaos;              // 0..99 macro; scales every weight below
extern int g_chaosLift;          // 0..99 weight: +7 / +12 / +24 voicing lifts
extern int g_chaosFill;          // 0..99 weight: notes added on empty steps
extern int g_chaosSkip;          // 0..99 weight: existing notes dropped
extern int g_chaosRatchet;       // 0..99 weight: random retrigs
extern int g_chaosTime;          // 0..99 weight: microtiming / velocity jitter
extern int g_chaosRepeat;        // 0..8 bars before the dice repeat; 0 = never repeats
extern unsigned int g_chaosSeed; // re-rolled to get a different variation
extern int g_globalTranspose;    // semitones; applies to keyScope == GLOBAL tracks
extern bool settingsHubOpen;
extern int settingsHubKind;   // 0 = STEP/TRK, 1 = LIVE/ALGO
extern int settingsHubTab;    // 0 or 1 within the current kind
extern int settingsHubSeqTab; // remembered STEP/TRK tab
extern int settingsHubFocus;  // 0 = tab bar, 1 = tab body
extern int liveFxFocusCol;    // 0 = FRQ, 1 = RES, 2 = TYP
// The ALGO tab is a grid: one macro row over two rows of five aligned columns.
extern int algoRow;           // 0 = CHAOS macro, 1 = weights, 2 = utility strip
extern int algoCol;           // 0..ALGO_COLS-1, kept while moving through row 0
constexpr int ALGO_ROWS = 3;
constexpr int ALGO_COLS = 5;
extern int stepPopupFocusX;   // 0 = Left column, 1 = Right column (chord / microtiming)
extern int stepPopupFocusY;   // Left: 0 Retrig, 1 Condit, 2 Length. Right: 0 Chord, 1 Microtiming
extern int stepPopupCondCol;  // 0..15 focused condition bit
extern int stepPopupChordKey; // 0..12 hovered piano key index
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
// Per-track tape delay lines, kept alongside the tracks rather than inside them
// so pattern data stays copyable and small.
extern TapeBufferFX g_trackTapeFX[8];
extern int synthMode;
extern const char* menuFeedback;
extern SampleAsset g_samplePool[16];
bool LoadSampleToPool(int slotIdx, const std::string& filename);
extern bool g_hardwareEncoderClicked;
extern float g_audioCpuLoad;
// Audio-thread health, written by the callback and read by the diagnostics screen.
extern float g_audioCpuPeak;            // peak-hold over the last second, uncapped
extern unsigned int g_audioDeadlineMisses; // callbacks that took longer than their buffer
extern float g_masterPeak;              // pre-limiter peak, so clipping is visible
extern float g_limiterReduction;        // 0 = open, 1 = fully clamped
extern int g_activeVoiceCount;          // voices sounding, the number chaos moves
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

// Global Performance FX (LIVE tab) variables
extern int perfFilterCutoff;
extern int perfFilterResonance;
extern int perfFilterType;   // 0 = LPF, 1 = HPF, 2 = BPF
extern int activeStutterKey; // -1 if inactive, 0..7 if a stutter pad is pressed

// Functions
const char* GetTrackName(int trackIndex);
void InitializeTracks();
void ResetTrackToDefault(int t);

int NoteToMidi(const std::string& noteStr);
std::string MidiToNote(int midi);
std::string TransposeNote(const std::string& noteStr, int semitones);

int ResolveTrackKeyRoot(int trackIdx);
int ResolveTrackKeyLock(int trackIdx);
int SnapMidiToScale(int midi, int root, int lock);
int StepMidiInScale(int midi, int root, int dir, int lock);
int KeyRootMidiAtOctave(int root, int octave);
void SnapTrackSequenceToKey(int trackIdx);

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
