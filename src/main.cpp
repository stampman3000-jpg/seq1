#include "raylib.h"
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>
#include <cmath>
#include "boot_animation.h"
#include "Common.hpp"
#include "Globals.hpp"
#include "UI_Draw.hpp"
#include "UI_Screens.hpp"
#include "Audio_Engine.hpp"
#include "Midi_Manager.hpp"
#include "OledDriver.hpp"
#include <unistd.h>


// Static safe character set for on-screen retro text scrolling
static const char kCharSet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
constexpr int kCharSetSize = sizeof(kCharSet) - 1; // 39 characters

// =========================================================================
// FILE-SCOPE STATIC GLOBAL VARIABLES
// =========================================================================
static int cursorTrack = 0;
static int synthGridRow = 1;
static int synthGridCol = 0;
static int trackParamsGridCol = 0;
static int currentOctave = 4;
static int activeScreenRow = 0;
static Step copiedStep;
static bool hasCopiedStep = false;

// Unified action key helper mapping Space and Enter/Return to a single key event
static inline bool IsActionKeyPressed() {
    return IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
}

#if defined(__linux__)
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdlib>

std::atomic<int> g_encoderTurnQueue(0);
std::atomic<bool> g_encoderButtonState(false);
std::atomic<bool> g_encoderThreadRunning(true);

void runEncoderThread() {
    std::system("sudo pinctrl set 17,22,27 ip pu");
    usleep(10000);

    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (fd < 0) {
        std::cerr << "[ENCODER] Failed to open /dev/gpiomem. Thread aborting." << std::endl;
        return;
    }

    volatile uint32_t* gpio = (volatile uint32_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (gpio == MAP_FAILED) {
        std::cerr << "[ENCODER] Memory mapping /dev/gpiomem failed." << std::endl;
        return;
    }

    const int GPLEV0 = 13;
    uint32_t levels = gpio[GPLEV0];
    bool lastCLK = (levels & (1 << 17)) != 0;
    
    auto lastTurnTime = std::chrono::steady_clock::now();

    while (g_encoderThreadRunning) {
        uint32_t levels = gpio[GPLEV0];
        bool clkVal = (levels & (1 << 17)) != 0;
        bool dtVal  = (levels & (1 << 27)) != 0;
        bool swVal  = (levels & (1 << 22)) == 0;

        g_encoderButtonState.store(swVal);

        if (lastCLK && !clkVal) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTurnTime).count();

            // 1. Lower the debounce limit to 6ms to capture rapid physical turns
            if (elapsed > 6) {
                
                // 2. Calculate dynamic velocity scaling (Acceleration)
                int multiplier = 1;
                if (elapsed < 14) {
                    multiplier = 5; // Fast spin -> jump by 5
                } else if (elapsed < 28) {
                    multiplier = 2; // Moderate spin -> jump by 2
                }

                lastTurnTime = now;
                if (dtVal) {
                    g_encoderTurnQueue.fetch_add(multiplier);
                } else {
                    g_encoderTurnQueue.fetch_sub(multiplier);
                }
            }
        }
        lastCLK = clkVal;
        usleep(1000);
    }

    munmap((void*)gpio, 4096);
}
#endif

// --- CUSTOM CHORD INPUT HELPER ---
static void ToggleStepChordNote(Step& step, int keyIdx) {
    int8_t noteVal = 60 + keyIdx; // Represent key indices as absolute MIDI values
    
    if (step.note == noteVal) {
        step.note = -1;
        step.velocity = 0;
        return;
    }
    for (int i = 0; i < 3; ++i) {
        if (step.chordNotes[i] == noteVal) {
            step.chordNotes[i] = -1;
            return;
        }
    }
    
    if (step.note == -1) {
        step.note = noteVal;
        step.velocity = 3;
        return;
    }
    for (int i = 0; i < 3; ++i) {
        if (step.chordNotes[i] == -1) {
            step.chordNotes[i] = noteVal;
            return;
        }
    }
}

// =========================================================================
// MODULAR INPUT & DSP EVENT HANDLERS
// =========================================================================
// Manage navigation and tactile parameter editing inside the Performance Popup
static void HandlePerformancePopupInputs(int encoderTurn, bool encoderButton, bool isShiftDown) {
    bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                      IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
                      
    // Exit popup using Escape or Enter/Return
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        perfPopupOpen = false;
        activeStutterKey = -1; // Clear stutter immediately on close
        menuFeedback = "PERFORMANCE FX CLOSED";
        return;
    }

    // --- I. DYNAMIC KEY REPEAT TIMERS ---
    
    // 1. Navigation Auto-Repeat Timer (Shift is NOT held)
    static float popupNavTimer = 0.0f;
    bool anyNavKeyHeld = IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT);
    bool triggerNav = false;

    if (anyNavKeyHeld && !isShiftDown) {
        if (popupNavTimer == 0.0f) {
            triggerNav = true;
            popupNavTimer += GetFrameTime();
        } else {
            popupNavTimer += GetFrameTime();
            const float INITIAL_DELAY = 0.25f;  // Standard Soundboy delay before scrolling starts
            const float REPEAT_INTERVAL = 0.06f; // Scroll speed
            if (popupNavTimer >= INITIAL_DELAY) {
                triggerNav = true;
                popupNavTimer -= REPEAT_INTERVAL;
            }
        }
    } else {
        popupNavTimer = 0.0f; // Reset timer when keys are released
    }

    // 2. Parameter Editing Auto-Repeat Timer (Shift IS held)
    static float popupEditTimer = 0.0f;
    bool anyEditKeyHeld = IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT);
    bool triggerEdit = false;

    if (anyEditKeyHeld && isShiftDown) {
        if (popupEditTimer == 0.0f) {
            triggerEdit = true;
            popupEditTimer += GetFrameTime();
        } else {
            popupEditTimer += GetFrameTime();
            const float INITIAL_DELAY = 0.30f;  // Standard edit start delay
            const float REPEAT_INTERVAL = 0.08f; // Parameter shift rate
            if (popupEditTimer >= INITIAL_DELAY) {
                triggerEdit = true;
                popupEditTimer -= REPEAT_INTERVAL;
            }
        }
    } else {
        popupEditTimer = 0.0f; // Reset timer when keys are released
    }

    // --- II. NAVIGATION PROCESSING ---
    if (triggerNav) {
        if (IsKeyDown(KEY_LEFT)) {
            synthGridCol--;
            if (synthGridCol < 0) synthGridCol = 2; // Wrap left to TYP
        }
        if (IsKeyDown(KEY_RIGHT)) {
            synthGridCol++;
            if (synthGridCol > 2) synthGridCol = 0; // Wrap right to FRQ
        }
    }

    // --- III. PARAMETER EDITING VALUE CHANGES ---
    int change = 0;
    
    if (encoderTurn != 0) {
        // Rotary encoder always edits value directly
        change = encoderTurn;
    }
    else if (triggerEdit) {
        // Shift + Keys matches your global fine/coarse editing format:
        if (IsKeyDown(KEY_UP))    change = 10;  // Fast Scroll (+10)
        if (IsKeyDown(KEY_DOWN))  change = -10; // Fast Scroll (-10)
        if (IsKeyDown(KEY_RIGHT)) change = 1;   // Precise Scroll (+1)
        if (IsKeyDown(KEY_LEFT))  change = -1;  // Precise Scroll (-1)
    }

    if (change != 0) {
        if (synthGridCol == 0) {
            perfFilterCutoff = std::clamp(perfFilterCutoff + change, 0, 99);
        } else if (synthGridCol == 1) {
            perfFilterResonance = std::clamp(perfFilterResonance + change, 0, 99);
        } else if (synthGridCol == 2) {
            // Cycle through 0 = LPF, 1 = HPF, 2 = BPF
            perfFilterType += (change > 0) ? 1 : -1;
            if (perfFilterType < 0) perfFilterType = 2;
            if (perfFilterType > 2) perfFilterType = 0;
        }
    }

    // --- IV. MOMENTARY STUTTER KEY PRESS SCANNING ---
    if (IsKeyDown(KEY_A))      activeStutterKey = 0;
    else if (IsKeyDown(KEY_S)) activeStutterKey = 1;
    else if (IsKeyDown(KEY_D)) activeStutterKey = 2;
    else if (IsKeyDown(KEY_F)) activeStutterKey = 3;
    else if (IsKeyDown(KEY_G)) activeStutterKey = 4;
    else if (IsKeyDown(KEY_H)) activeStutterKey = 5;
    else if (IsKeyDown(KEY_J)) activeStutterKey = 6;
    else if (IsKeyDown(KEY_K)) activeStutterKey = 7;
    else                       activeStutterKey = -1;
}

// Handles live piano input playability
static void HandlePianoKeysInput(int octaveValue) {
    if (!liveKeyboardActive) return;
    
    bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                      IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    bool isShiftDown = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    // Return early if control or shift are held down to prevent note overlaps on navigation
    if (isCtrlDown || isShiftDown) return;

    for (int i = 0; i < NUM_NOTES; ++i) {
        std::string noteName = keyboardPiano[i].noteName;
        int octaveToUse = octaveValue;
        if (noteName == "C+") {
            noteName = "C";
            octaveToUse = octaveValue + 1;
            if (octaveToUse > 8) octaveToUse = 8;
        }
        std::string noteStr = noteName + std::to_string(octaveToUse);
        int midiNote = NoteToMidi(noteStr);

        if (IsKeyPressed(keyboardPiano[i].key)) {
            TriggerVoiceLive(selectedTrack, midiNote, 3);
        }
        if (IsKeyReleased(keyboardPiano[i].key)) {
            ReleaseVoiceLive(selectedTrack, midiNote);
        }
    }
}

// Handles inputs and directory scanning while inside the Modal System Menu
static void HandleSystemMenuInputs(int menuDir, int encoderTurn) {
    if (systemMenuState == 0) {
        // ==========================================
        // SUBMENU 0: MAIN SYSTEM DIRECTORY LIST
        // ==========================================
        if (menuDir == -1) {
            systemMenuCursor--;
            if (systemMenuCursor < 0) systemMenuCursor = 6;
            menuFeedback = "";
        }
        if (menuDir == 1) {
            systemMenuCursor++;
            if (systemMenuCursor > 6) systemMenuCursor = 0;
            menuFeedback = "";
        }
        if (IsActionKeyPressed()) {
            bool isSaveOption = (systemMenuCursor == 0 || systemMenuCursor == 2 || systemMenuCursor == 4);
            if (isSaveOption) {
                systemMenuState = 2;
                fileBrowserCursor = 0;
            } else {
                std::string targetDir = "projects";
                std::string targetExt = ".prj";

                if (systemMenuCursor == 1)      { targetDir = "projects"; targetExt = ".prj"; }
                else if (systemMenuCursor == 3) { targetDir = "patterns"; targetExt = ".pat"; }
                else if (systemMenuCursor == 5) { targetDir = "presets";  targetExt = ".snd"; }
                else if (systemMenuCursor == 6) { targetDir = "samples";  targetExt = ".wav"; }

                g_fileList = GetFileList(targetDir, targetExt);
                fileBrowserCursor = 0;
                systemMenuState = 1;
            }
        }
    }
    else if (systemMenuState == 1) {
        // ==========================================
        // SUBMENU 1: FILE BROWSER (Loading named files)
        // ==========================================
        if (!g_fileList.empty()) {
            if (menuDir == -1) {
                fileBrowserCursor--;
                if (fileBrowserCursor < 0) fileBrowserCursor = (int)g_fileList.size() - 1;
            }
            if (menuDir == 1) {
                fileBrowserCursor++;
                if (fileBrowserCursor >= (int)g_fileList.size()) fileBrowserCursor = 0;
            }
            if (IsActionKeyPressed()) {
                if (systemMenuCursor == 6) {
                    saveSlotCursor = fileBrowserCursor;
                    systemMenuState = 2;
                    fileBrowserCursor = 0;
                } else {
                    std::string selectedFile = g_fileList[fileBrowserCursor];
                    bool success = false;
                    
                    if (systemMenuCursor == 1)      success = LoadProject(selectedFile);
                    else if (systemMenuCursor == 3) success = LoadPattern(activePattern, selectedFile);
                    else if (systemMenuCursor == 5) success = LoadSoundPreset(selectedTrack, selectedFile);

                    if (success) menuFeedback = "LOADED SUCCESSFULLY!";
                    else         menuFeedback = "LOAD FAILED!";
                    
                    systemMenuState = 0;
                }
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE)) {
            systemMenuState = 0;
        }
    }
    else if (systemMenuState == 2) {
        // ==========================================
        // SUBMENU 2: SAVE/POOL SLOT SELECTOR
        // ==========================================
        int maxSlots = (systemMenuCursor == 0 || systemMenuCursor == 6) ? 16 : ((systemMenuCursor == 2) ? 128 : 1024);
        
        if (menuDir == -1) {
            fileBrowserCursor--;
            if (fileBrowserCursor < 0) fileBrowserCursor = maxSlots - 1;
        }
        if (menuDir == 1) {
            fileBrowserCursor++;
            if (fileBrowserCursor >= maxSlots) fileBrowserCursor = 0;
        }
        if (IsActionKeyPressed()) {
            if (systemMenuCursor == 6) {
                std::string selectedFile = "";
                if (saveSlotCursor >= 0 && saveSlotCursor < (int)g_fileList.size()) {
                    selectedFile = g_fileList[saveSlotCursor];
                    bool success = LoadSampleToPool(fileBrowserCursor, selectedFile);
                    if (success) menuFeedback = "SAMPLE CRUNCHED TO POOL!";
                    else         menuFeedback = "CONVERSION FAILED!";
                } else {
                    menuFeedback = "INVALID FILE SELECTION!";
                }
                systemMenuState = 0;
            } else {
                            systemMenuState = 3;
                            g_typingBuffer = std::string(15, ' '); // 15 completely blank spaces
                            g_typingCursor = 0;                   // Reset cursor to index 0
                        }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE)) {
            systemMenuState = 0;
        }
    }
    else if (systemMenuState == 3) {
        // ==========================================
        // SUBMENU 3: SCROLLING TEXT SELECTOR
        // ==========================================
        
        // 1. Left/Right Navigation moves the cursor slot (0 to 14)
        // Hold-and-repeat, mirroring the Up/Down character-scroll repeat already used on this screen.
        static float typingCursorRepeatTimer = 0.0f;
        bool typingCursorHeld = IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT);
        bool triggerCursorMove = false;

        if (typingCursorHeld) {
            if (typingCursorRepeatTimer == 0.0f) {
                triggerCursorMove = true;
                typingCursorRepeatTimer += GetFrameTime();
            } else {
                typingCursorRepeatTimer += GetFrameTime();
                const float INITIAL_DELAY = 0.250f;
                const float REPEAT_INTERVAL = 0.060f;
                if (typingCursorRepeatTimer >= INITIAL_DELAY) {
                    triggerCursorMove = true;
                    typingCursorRepeatTimer -= REPEAT_INTERVAL;
                }
            }
        } else {
            typingCursorRepeatTimer = 0.0f;
        }

        if (triggerCursorMove) {
            if (IsKeyDown(KEY_LEFT)) {
                g_typingCursor--;
                if (g_typingCursor < 0) g_typingCursor = (int)g_typingBuffer.length() - 1; // Wrap to end
            }
            if (IsKeyDown(KEY_RIGHT)) {
                g_typingCursor++;
                if (g_typingCursor >= (int)g_typingBuffer.length()) g_typingCursor = 0; // Wrap to start
            }
        }

        // 2. Up/Down or Encoder Turn scrolls characters at the active slot
        int charChange = 0;
        if (menuDir == -1)    charChange = -1; // Arrow UP scrolls forward
        if (menuDir == 1)     charChange = 1;  // Arrow DOWN scrolls backward
        if (encoderTurn != 0) charChange = (encoderTurn > 0) ? 1 : -1; // Encoder turn support

        if (charChange != 0 && g_typingCursor >= 0 && g_typingCursor < (int)g_typingBuffer.length()) {
            char curChar = g_typingBuffer[g_typingCursor];
            int charIdx = 0;
            // Find current character index in our safe charset
            for (int i = 0; i < kCharSetSize; ++i) {
                if (kCharSet[i] == curChar) {
                    charIdx = i;
                    break;
                }
            }
            // Increment/wrap character index
            charIdx += charChange;
            if (charIdx < 0) charIdx = kCharSetSize - 1;
            if (charIdx >= kCharSetSize) charIdx = 0;
            
            g_typingBuffer[g_typingCursor] = kCharSet[charIdx];
        }

        if (IsKeyPressed(KEY_ESCAPE)) {
            systemMenuState = 2; // Go back to slot selection
        }

        // 3. Confirm Selection on Action Key (Space or Enter)
        if (IsActionKeyPressed()) {
            if (!g_typingBuffer.empty()) {
                // Trim trailing spaces cleanly before writing file to disk
                std::string finalName = g_typingBuffer;
                size_t end = finalName.find_last_not_of(' ');
                finalName = (end == std::string::npos) ? "" : finalName.substr(0, end + 1);

                if (finalName.empty()) {
                    finalName = "UNTITLED"; // Fallback default
                }

                bool success = false;
                int activeSlotNum = fileBrowserCursor + 1;
                
                if (systemMenuCursor == 0)      success = SaveProject(activeSlotNum, finalName);
                else if (systemMenuCursor == 2) success = SavePattern(activePattern, activeSlotNum, finalName);
                else if (systemMenuCursor == 4) success = SaveSoundPreset(selectedTrack, activeSlotNum, finalName);

                if (success) menuFeedback = "SAVED SUCCESSFULLY!";
                else         menuFeedback = "SAVE FAILED!";
            }
            systemMenuState = 0; // Close system menu on completion
        }
    }
}

// Handles the LFO Modulation matrix popup grid
static void HandleLfoPopupInputs(int encoderTurn, bool encoderButton, bool isShiftDown) {
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        lfoPopupOpen = false;
        return;
    }

    if (!isShiftDown) {
        if (IsKeyPressed(KEY_UP)) {
            lfoPopupSlot--;
            if (lfoPopupSlot < 0) lfoPopupSlot = 2;
        }
        if (IsKeyPressed(KEY_DOWN)) {
            lfoPopupSlot++;
            if (lfoPopupSlot > 2) lfoPopupSlot = 0;
        }
        if (IsKeyPressed(KEY_LEFT)) {
            lfoPopupField--;
            if (lfoPopupField < 0) lfoPopupField = 3;
        }
        if (IsKeyPressed(KEY_RIGHT)) {
            lfoPopupField++;
            if (lfoPopupField > 3) lfoPopupField = 0;
        }
    }

    bool triggerIncBy10 = false;
    bool triggerDecBy10 = false;
    bool triggerIncBy1  = IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD);
    bool triggerDecBy1  = IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT);

    static float lfoEditRepeatTimer = 0.0f;
    bool anyLfoEditHeld = isShiftDown && (IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT));

    if (anyLfoEditHeld) {
        bool actionThisFrame = false;
        if (lfoEditRepeatTimer == 0.0f) {
            actionThisFrame = true;
            lfoEditRepeatTimer += GetFrameTime();
        } else {
            lfoEditRepeatTimer += GetFrameTime();
            const float INITIAL_DELAY = 0.250f;
            const float REPEAT_INTERVAL = 0.050f;
            if (lfoEditRepeatTimer >= INITIAL_DELAY) {
                actionThisFrame = true;
                lfoEditRepeatTimer -= REPEAT_INTERVAL;
            }
        }

        if (actionThisFrame) {
            if (IsKeyDown(KEY_UP))    triggerIncBy10 = true;
            if (IsKeyDown(KEY_DOWN))  triggerDecBy10 = true;
            if (IsKeyDown(KEY_RIGHT)) triggerIncBy1  = true;
            if (IsKeyDown(KEY_LEFT))  triggerDecBy1  = true;
        }
    } else {
        lfoEditRepeatTimer = 0.0f;
    }

    if (triggerIncBy10 || triggerDecBy10 || triggerIncBy1 || triggerDecBy1) {
        Track& trk = tracks[selectedTrack];
        ModSlot& m = (lfoPopupLfoIdx == 0) ? trk.lfo1Slots[lfoPopupSlot] : trk.lfo2Slots[lfoPopupSlot];

        int delta = 0;
        if (triggerIncBy10)      delta = 10;
        else if (triggerDecBy10) delta = -10;
        else if (triggerIncBy1)  delta = 1;
        else if (triggerDecBy1)  delta = -1;

        if (lfoPopupField == 0) {
            int step = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
            if (step != 0) {
                m.destType += step;
                if (m.destType < 0) m.destType = 2;
                if (m.destType > 2) m.destType = 0;
                
                if (m.destType == 1) {
                    m.destParam = DEST_CUTOFF;
                    if (m.depth == 0) m.depth = 40;
                }
                else if (m.destType == 2) {
                    m.destParam = DEST_REV_MIX;
                    if (m.depth == 0) m.depth = 40;
                }
                else {
                    m.destParam = DEST_NONE;
                }
            }
        }
        else if (lfoPopupField == 1 && m.destType == 1) {
            int step = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
            if (step != 0) {
                m.destTrack += step;
                if (m.destTrack < 0) m.destTrack = 7;
                if (m.destTrack > 7) m.destTrack = 0;
            }
        }
        else if (lfoPopupField == 2 && m.destType != 0) {
            int step = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
            if (step != 0) {
                if (m.destType == 1) {
                    m.destParam += step;
                    if (m.destParam < DEST_CUTOFF) m.destParam = DEST_TAPE_MIX;
                    if (m.destParam > DEST_TAPE_MIX)  m.destParam = DEST_CUTOFF;
                } else if (m.destType == 2) {
                    m.destParam += step;
                    if (m.destParam < DEST_REV_MIX) m.destParam = DEST_PAN_MIX;
                    if (m.destParam > DEST_PAN_MIX)  m.destParam = DEST_REV_MIX;
                }
            }
        }
        else if (lfoPopupField == 3 && m.destType != 0) {
            m.depth = std::clamp(m.depth + delta, -99, 99);
        }
    }
}

/// Handles input events inside the Step Utility Sub-Popup (Z Trigger)
static void HandleStepPopupInputs(int encoderTurn, bool encoderButton, bool isShiftDown) {
    // Detect if Control/Command is held down inside this scope
    bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                      IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    bool isCtrlXPressed = isCtrlDown && IsKeyPressed(KEY_X);

    // Close on Escape or the Ctrl+X toggle macro (unifying Enter/Return to act as Space instead)
    if (IsKeyPressed(KEY_ESCAPE) || isCtrlXPressed) {
        stepPopupOpen = false;
        menuFeedback = "POPUP CLOSED";
        return;
    }

    int activeTrackIdx = (currentScreen == SCREEN_SEQ_5_8) ? cursorTrack + 4 : cursorTrack;
    Step& step = tracks[activeTrackIdx].steps[cursorStep];

    if (!isShiftDown) {
        if (stepPopupFocusX == 0) {
            if (IsKeyPressed(KEY_UP)) {
                if (stepPopupFocusY == 1) {
                    if (stepPopupCondCol >= 8) {
                        stepPopupCondCol -= 8;
                    } else {
                        stepPopupFocusY = 0;
                    }
                } else if (stepPopupFocusY == 2) {
                    stepPopupFocusY = 1;
                    stepPopupCondCol = 8;
                } else {
                    stepPopupFocusY = 2;
                }
            }
            if (IsKeyPressed(KEY_DOWN)) {
                if (stepPopupFocusY == 0) {
                    stepPopupFocusY = 1;
                    stepPopupCondCol = 0;
                } else if (stepPopupFocusY == 1) {
                    stepPopupFocusY = 2;
                } else {
                    stepPopupFocusY = 0;
                }
            }
            if (stepPopupFocusY == 1) {
                if (IsKeyPressed(KEY_LEFT)) {
                    int curRow = stepPopupCondCol / 8;
                    int curCol = stepPopupCondCol % 8;
                    curCol--;
                    if (curCol < 0) curCol = 7;
                    stepPopupCondCol = curRow * 8 + curCol;
                }
                if (IsKeyPressed(KEY_RIGHT)) {
                    int curRow = stepPopupCondCol / 8;
                    int curCol = stepPopupCondCol % 8;
                    curCol++;
                    if (curCol > 7) {
                        stepPopupFocusX = 1;
                    } else {
                        stepPopupCondCol = curRow * 8 + curCol;
                    }
                }
            } else {
                if (IsKeyPressed(KEY_RIGHT)) {
                    stepPopupFocusX = 1;
                }
            }
        }
        else if (stepPopupFocusX == 1) {
            if (IsKeyPressed(KEY_LEFT)) {
                stepPopupChordKey--;
                if (stepPopupChordKey < 0) {
                    stepPopupFocusX = 0;
                    stepPopupChordKey = 0;
                }
            }
            if (IsKeyPressed(KEY_RIGHT)) {
                stepPopupChordKey++;
                if (stepPopupChordKey > 12) {
                    stepPopupChordKey = 12;
                }
            }
            if (IsKeyPressed(KEY_UP)) {
                step.chordType--;
                if (step.chordType < 0) step.chordType = 6;
            }
            if (IsKeyPressed(KEY_DOWN)) {
                step.chordType++;
                if (step.chordType > 6) step.chordType = 0;
            }
        }
    }

    int popupEditChange = 0;
    if (encoderTurn != 0) {
        popupEditChange = (encoderTurn > 0) ? 1 : -1;
    } else if (isShiftDown) {
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_UP))   popupEditChange = 1;
        if (IsKeyPressed(KEY_LEFT) || IsKeyDown(KEY_DOWN))    popupEditChange = -1;
    }

    if (stepPopupFocusX == 0) {
        if (stepPopupFocusY == 0) {
            if (popupEditChange != 0) {
                step.retrigger = std::clamp(step.retrigger + popupEditChange, 0, 16);
            }
        }
        else if (stepPopupFocusY == 1) {
            bool shiftArrowPressed = isShiftDown && (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT));
            // Unify SPACE and ENTER: we use IsActionKeyPressed() here instead of IsKeyPressed(KEY_SPACE)
            if (shiftArrowPressed || IsActionKeyPressed() || (encoderTurn != 0 && encoderButton)) {
                if (stepPopupCondCol < 8) {
                    step.condMask ^= (1 << stepPopupCondCol);
                } else {
                    step.condMask &= ~0xFF00;
                    step.condMask |= (1 << stepPopupCondCol);
                }
            }
        }
        else if (stepPopupFocusY == 2) {
            if (popupEditChange != 0) {
                step.noteLength = std::clamp(step.noteLength + popupEditChange, 0, 16);
            }
        }
    }
    else if (stepPopupFocusX == 1) {
        if (popupEditChange != 0) {
            step.chordType = std::clamp(step.chordType + popupEditChange, 0, 6);
        }
        // Unify SPACE and ENTER: we use IsActionKeyPressed() here instead of IsKeyPressed(KEY_SPACE)
        if (IsActionKeyPressed() || (encoderTurn != 0 && encoderButton)) {
            if (step.chordType == 0) {
                ToggleStepChordNote(step, stepPopupChordKey);
            }
        }
    }
}

// Manages real-time value increments and bounds editing for active synth and master FX screens
static void HandleParameterEditingInput(int encoderTurn, bool encoderButton, bool isShiftDown, float frameTime) {
    static float keyRepeatTimer = 0.0f;
    bool triggerAction = false;
    
    // Require either Left or Right Shift to be held for arrow-key parameter editing
    bool keyboardModifierHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool anyEditKeyDown = keyboardModifierHeld &&
                          (IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT));

    if (anyEditKeyDown) {
        if (keyRepeatTimer == 0.0f) {
            triggerAction = true;
            keyRepeatTimer += frameTime;
        } else {
            keyRepeatTimer += frameTime;
            const float INITIAL_DELAY = 0.30f;
            const float REPEAT_INTERVAL = 0.08f;
            if (keyRepeatTimer >= INITIAL_DELAY) {
                triggerAction = true;
                keyRepeatTimer -= REPEAT_INTERVAL;
            }
        }
    } else {
        keyRepeatTimer = 0.0f;
    }

    int change = 0;
        if (triggerAction || (encoderTurn != 0)) {
            bool isVolumeCol = false;
            bool isFeedbackCol = false;
            bool isToggleCol = false;

            if (currentScreen == SCREEN_SYNTH) {
                isVolumeCol = (tracks[selectedTrack].engineType == ENGINE_SYNTH && synthGridCol == 3) ||
                              (synthGridCol == 11) ||
                              (tracks[selectedTrack].engineType == ENGINE_SAMPLER && synthGridRow == 1 && synthGridCol == 12);
            }
            else if (currentScreen == SCREEN_TRACK_PARAMS) {
                // VOY is a toggle (1..4), but PRT (0..99) is a standard continuous range
                isToggleCol = (synthGridCol == 2 || (synthGridRow == 1 && synthGridCol == 4) || synthGridCol == 5 || synthGridCol == 8 || synthGridCol == 9);
                if (synthGridRow == 3 && synthGridCol == 4 && !isShiftDown) {
                    isToggleCol = true;
                }
            }
            else if (currentScreen == SCREEN_PLACEHOLDER) {
                isToggleCol = (synthGridCol == 1 && synthGridRow == 1);
            }
            else if (currentScreen == SCREEN_GLOBAL_FX) {
                isToggleCol = (synthGridCol == 4 && synthGridRow == 2);
            }

            if (encoderTurn != 0) {
                if (currentScreen == SCREEN_SYNTH && (isVolumeCol || isFeedbackCol)) {
                    change = encoderTurn * 5;
                } else {
                    change = encoderTurn;
                }
            } else if (currentScreen == SCREEN_SYNTH && (isVolumeCol || isFeedbackCol)) {
                if (IsKeyDown(KEY_RIGHT)) change = 5;
                if (IsKeyDown(KEY_LEFT))  change = -5;
            } else if (isToggleCol) {
                if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_UP)) change = 1;
                if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_DOWN))  change = -1;
            } else {
                if (IsKeyDown(KEY_RIGHT)) change = 1;
                if (IsKeyDown(KEY_LEFT))  change = -1;
                if (IsKeyDown(KEY_UP))    change = 10;
                if (IsKeyDown(KEY_DOWN))  change = -10;
            }
        }

        if (change == 0) return;

    Track& trk = tracks[selectedTrack];
    StepParams& sp = trk.steps[cursorStep].params;
    bool isStepLock = IsKeyDown(KEY_X) || encoderButton;

    auto EditParam = [&](int& stepVal, int trackVal, int changeAmt, int minV, int maxV) {
        int base = (stepVal == -1) ? trackVal : stepVal;
        stepVal = std::clamp(base + changeAmt, minV, maxV);
    };

    if (currentScreen == SCREEN_SYNTH) {
        if (trk.engineType == ENGINE_SAMPLER) {
            if (synthGridRow == 1) {
                if (synthGridCol == 0) {
                    if (isStepLock) sp.sampleLoop = (sp.sampleLoop == -1) ? ((trk.sampleLoop == 0) ? 1 : 0) : ((sp.sampleLoop == 0) ? 1 : 0);
                    else            trk.sampleLoop = (trk.sampleLoop == 0) ? 1 : 0;
                }
                else if (synthGridCol == 1) { if (isStepLock) EditParam(sp.sampleStart, trk.sampleStart, change, 0, 99); else trk.sampleStart = std::clamp(trk.sampleStart + change, 0, 99); }
                else if (synthGridCol == 2) { if (isStepLock) EditParam(sp.sampleLength, trk.sampleLength, change, 0, 99); else trk.sampleLength = std::clamp(trk.sampleLength + change, 0, 99); }
                else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.loopStart, trk.loopStart, change, 0, 99); else trk.loopStart = std::clamp(trk.loopStart + change, 0, 99); }
                else if (synthGridCol == 4) { if (isStepLock) EditParam(sp.loopEnd, trk.loopEnd, change, 0, 99); else trk.loopEnd = std::clamp(trk.loopEnd + change, 0, 99); }
                else if (synthGridCol == 5) { if (isStepLock) EditParam(sp.attack, trk.attack, change, 0, 99); else trk.attack = std::clamp(trk.attack + change, 0, 99); }
                else if (synthGridCol == 6) { if (isStepLock) EditParam(sp.decay, trk.decay, change, 0, 99); else trk.decay = std::clamp(trk.decay + change, 0, 99); }
                else if (synthGridCol == 7) { if (isStepLock) EditParam(sp.sustain, trk.sustain, change, 0, 99); else trk.sustain = std::clamp(trk.sustain + change, 0, 99); }
                else if (synthGridCol == 8) { if (isStepLock) EditParam(sp.release, trk.release, change, 0, 99); else trk.release = std::clamp(trk.release + change, 0, 99); }
                else if (synthGridCol == 9) { if (isStepLock) EditParam(sp.pitchSweepDepth, trk.pitchSweepDepth, change, 0, 99); else trk.pitchSweepDepth = std::clamp(trk.pitchSweepDepth + change, 0, 99); }
                else if (synthGridCol == 10){ if (isStepLock) EditParam(sp.pitchSweepTime, trk.pitchSweepTime, change, 0, 99); else trk.pitchSweepTime = std::clamp(trk.pitchSweepTime + change, 0, 99); }
                else if (synthGridCol == 11){ if (isStepLock) EditParam(sp.bitRed, trk.bitRed, change, 0, 99); else trk.bitRed = std::clamp(trk.bitRed + change, 0, 99); }
                else if (synthGridCol == 12){ if (isStepLock) EditParam(sp.volume, trk.volume, change, 0, 99); else trk.volume = std::clamp(trk.volume + change, 0, 99); }
            }
            else if (synthGridRow == 2) {
                if (synthGridCol == 0) {
                    // Lock real algorithm — never abuse filterType for sample↔granular
                    if (isStepLock) {
                        int base = (sp.algorithm == -1) ? trk.algorithm : sp.algorithm;
                        sp.algorithm = (base == ALGO_SAMPLE) ? ALGO_GRANULAR : ALGO_SAMPLE;
                    } else {
                        trk.algorithm = (trk.algorithm == ALGO_SAMPLE) ? ALGO_GRANULAR : ALGO_SAMPLE;
                    }
                }
                else if (synthGridCol == 1) {
                    if (isStepLock) EditParam(sp.sampleSlot, trk.sampleSlot, change, 0, 15);
                    else            trk.sampleSlot = std::clamp(trk.sampleSlot + change, 0, 15);
                }
                else if (synthGridCol == 2) {
                    int effAlgo = (isStepLock && sp.algorithm != -1) ? sp.algorithm : trk.algorithm;
                    if (effAlgo == ALGO_SAMPLE) {
                        int baseSdiv = (isStepLock && sp.sliceDivisions != -1) ? sp.sliceDivisions : trk.sliceDivisions;
                        int newSdiv = baseSdiv;
                        if (change > 0)      newSdiv = std::clamp(baseSdiv * 2, 1, 64);
                        else if (change < 0) newSdiv = std::clamp(baseSdiv / 2, 1, 64);
                        if (isStepLock) sp.sliceDivisions = newSdiv;
                        else            trk.sliceDivisions = newSdiv;
                    }
                }
                else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.sampleTune, trk.sampleTune, change, -24, 24); else trk.sampleTune = std::clamp(trk.sampleTune + change, -24, 24); }
                else if (synthGridCol == 4) { if (isStepLock) EditParam(sp.fine2, trk.fine2, change, -99, 99); else trk.fine2 = std::clamp(trk.fine2 + change, -99, 99); }
                else if (synthGridCol == 5) { if (isStepLock) EditParam(sp.grainPosition, trk.grainPosition, change, 0, 99); else trk.grainPosition = std::clamp(trk.grainPosition + change, 0, 99); }
                else if (synthGridCol == 6) { if (isStepLock) EditParam(sp.grainSize, trk.grainSize, change, 0, 99); else trk.grainSize = std::clamp(trk.grainSize + change, 0, 99); }
                else if (synthGridCol == 7) { if (isStepLock) EditParam(sp.grainDensity, trk.grainDensity, change, 0, 99); else trk.grainDensity = std::clamp(trk.grainDensity + change, 0, 99); }
                else if (synthGridCol == 8) { if (isStepLock) EditParam(sp.grainScatter, trk.grainScatter, change, 0, 99); else trk.grainScatter = std::clamp(trk.grainScatter + change, 0, 99); }
            }
        } else {
            if (synthGridRow == 1) {
                if (synthGridCol == 0)      { if (isStepLock) EditParam(sp.morph, trk.morph, change, 0, 99); else trk.morph = std::clamp(trk.morph + change, 0, 99); }
                else if (synthGridCol == 1) { if (isStepLock) EditParam(sp.coarse, trk.coarse, change, -24, 24); else trk.coarse = std::clamp(trk.coarse + change, -24, 24); }
                else if (synthGridCol == 2) { if (isStepLock) EditParam(sp.fine, trk.fine, change, -99, 99); else trk.fine = std::clamp(trk.fine + change, -99, 99); }
                else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.volume, trk.volume, change, 0, 99); else trk.volume = std::clamp(trk.volume + change, 0, 99); }
                else if (synthGridCol == 4) { if (isStepLock) EditParam(sp.attack, trk.attack, change, 0, 99); else trk.attack = std::clamp(trk.attack + change, 0, 99); }
                else if (synthGridCol == 5) { if (isStepLock) EditParam(sp.decay, trk.decay, change, 0, 99); else trk.decay = std::clamp(trk.decay + change, 0, 99); }
                else if (synthGridCol == 6) { if (isStepLock) EditParam(sp.sustain, trk.sustain, change, 0, 99); else trk.sustain = std::clamp(trk.sustain + change, 0, 99); }
                else if (synthGridCol == 7) { if (isStepLock) EditParam(sp.release, trk.release, change, 0, 99); else trk.release = std::clamp(trk.release + change, 0, 99); }
                else if (synthGridCol == 8) { if (isStepLock) EditParam(sp.pitchSweepDepth, trk.pitchSweepDepth, change, 0, 99); else trk.pitchSweepDepth = std::clamp(trk.pitchSweepDepth + change, 0, 99); }
                else if (synthGridCol == 9) { if (isStepLock) EditParam(sp.pitchSweepTime, trk.pitchSweepTime, change, 0, 99); else trk.pitchSweepTime = std::clamp(trk.pitchSweepTime + change, 0, 99); }
                else if (synthGridCol == 10){ if (isStepLock) EditParam(sp.fmFeedback, trk.fmFeedback, change, 0, 99); else trk.fmFeedback = std::clamp(trk.fmFeedback + change, 0, 99); }
                else if (synthGridCol == 11){ if (isStepLock) EditParam(sp.masterVolume, trk.masterVolume, change, 0, 99); else trk.masterVolume = std::clamp(trk.masterVolume + change, 0, 99); }
            }
            else if (synthGridRow == 2) {
                if (synthGridCol == 0)      { if (isStepLock) EditParam(sp.morph2, trk.morph2, change, 0, 99); else trk.morph2 = std::clamp(trk.morph2 + change, 0, 99); }
                else if (synthGridCol == 1) {
                    if (trk.algorithm == ALGO_CARRIER_MOD) {
                        int baseC2 = (isStepLock && sp.coarse2 != -1) ? sp.coarse2 : trk.coarse2;
                        int newC2 = std::clamp(baseC2 + ((change > 0) ? 1 : ((change < 0) ? -1 : 0)), 1, 16);
                        if (isStepLock) sp.coarse2 = newC2;
                        else            trk.coarse2 = newC2;
                    } else {
                        if (isStepLock) EditParam(sp.coarse2, trk.coarse2, change, -24, 24);
                        else            trk.coarse2 = std::clamp(trk.coarse2 + change, -24, 24);
                    }
                }
                else if (synthGridCol == 2) { if (isStepLock) EditParam(sp.fine2, trk.fine2, change, -99, 99); else trk.fine2 = std::clamp(trk.fine2 + change, -99, 99); }
                else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.volume2, trk.volume2, change, 0, 99); else trk.volume2 = std::clamp(trk.volume2 + change, 0, 99); }
                else if (synthGridCol == 4) { if (isStepLock) EditParam(sp.attack2, trk.attack2, change, 0, 99); else trk.attack2 = std::clamp(trk.attack2 + change, 0, 99); }
                else if (synthGridCol == 5) { if (isStepLock) EditParam(sp.decay2, trk.decay2, change, 0, 99); else trk.decay2 = std::clamp(trk.decay2 + change, 0, 99); }
                else if (synthGridCol == 6) { if (isStepLock) EditParam(sp.sustain2, trk.sustain2, change, 0, 99); else trk.sustain2 = std::clamp(trk.sustain2 + change, 0, 99); }
                else if (synthGridCol == 7) { if (isStepLock) EditParam(sp.release2, trk.release2, change, 0, 99); else trk.release2 = std::clamp(trk.release2 + change, 0, 99); }
                else if (synthGridCol == 8) { if (isStepLock) EditParam(sp.noiseAttack, trk.noiseAttack, change, 0, 99); else trk.noiseAttack = std::clamp(trk.noiseAttack + change, 0, 99); }
                else if (synthGridCol == 9) { if (isStepLock) EditParam(sp.noiseHold, trk.noiseHold, change, 0, 99); else trk.noiseHold = std::clamp(trk.noiseHold + change, 0, 99); }
                else if (synthGridCol == 10){ if (isStepLock) EditParam(sp.noiseDecay, trk.noiseDecay, change, 0, 99); else trk.noiseDecay = std::clamp(trk.noiseDecay + change, 0, 99); }
                else if (synthGridCol == 11){ if (isStepLock) EditParam(sp.noiseVolume, trk.noiseVolume, change, 0, 99); else trk.noiseVolume = std::clamp(trk.noiseVolume + change, 0, 99); }
            }
        }
    }
    else if (currentScreen == SCREEN_TRACK_PARAMS) {
        if (synthGridRow == 1) {
            if (synthGridCol == 0)      { if (isStepLock) EditParam(sp.filterCutoff, trk.filterCutoff, change, 0, 99); else trk.filterCutoff = std::clamp(trk.filterCutoff + change, 0, 99); }
            else if (synthGridCol == 1) { if (isStepLock) EditParam(sp.filterResonance, trk.filterResonance, change, 0, 99); else trk.filterResonance = std::clamp(trk.filterResonance + change, 0, 99); }
            else if (synthGridCol == 2) {
                int baseT = (isStepLock && sp.filterType != -1) ? sp.filterType : trk.filterType;
                int newT = baseT + change;
                if (newT < 0) newT = 2; if (newT > 2) newT = 0;
                if (isStepLock) sp.filterType = newT;
                else            trk.filterType = newT;
            }
            else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.filterEnvDepth, trk.filterEnvDepth, change, 0, 99); else trk.filterEnvDepth = std::clamp(trk.filterEnvDepth + change, 0, 99); }
            else if (synthGridCol == 4) {
                int baseW = (isStepLock && sp.lfo1Wave != -1) ? sp.lfo1Wave : trk.lfo1Wave;
                int newW = baseW + change;
                if (newW < 0) newW = 5; if (newW > 5) newW = 0;
                if (isStepLock) sp.lfo1Wave = newW;
                else            trk.lfo1Wave = newW;
            }
            else if (synthGridCol == 5) { if (isStepLock) EditParam(sp.lfo1Speed, trk.lfo1Speed, change, 0, 99); else trk.lfo1Speed = std::clamp(trk.lfo1Speed + change, 0, 99); }
            else if (synthGridCol == 6) { if (isStepLock) EditParam(sp.lfo1Depth, trk.lfo1Depth, change, 0, 99); else trk.lfo1Depth = std::clamp(trk.lfo1Depth + change, 0, 99); }
            else if (synthGridCol == 7) {
                int baseTrg = (isStepLock && sp.lfo1Trigger != -1) ? sp.lfo1Trigger : trk.lfo1Trigger;
                int newTrg = (baseTrg == 0) ? 1 : 0;
                if (isStepLock) sp.lfo1Trigger = newTrg;
                else            trk.lfo1Trigger = newTrg;
            }
            else if (synthGridCol == 8) {
                int baseSync = (isStepLock && sp.lfo1Sync != -1) ? sp.lfo1Sync : trk.lfo1Sync;
                int newSync = (baseSync == 0) ? 1 : 0;
                if (isStepLock) sp.lfo1Sync = newSync;
                else            trk.lfo1Sync = newSync;
            }
        }
        else if (synthGridRow == 3) {
            if (synthGridCol == 0)      { if (isStepLock) EditParam(sp.filterAttack, trk.filterAttack, change, 0, 99); else trk.filterAttack = std::clamp(trk.filterAttack + change, 0, 99); }
            else if (synthGridCol == 1) { if (isStepLock) EditParam(sp.filterDecay, trk.filterDecay, change, 0, 99); else trk.filterDecay = std::clamp(trk.filterDecay + change, 0, 99); }
            else if (synthGridCol == 2) { if (isStepLock) EditParam(sp.filterSustain, trk.filterSustain, change, 0, 99); else trk.filterSustain = std::clamp(trk.filterSustain + change, 0, 99); }
            else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.filterRelease, trk.filterRelease, change, 0, 99); else trk.filterRelease = std::clamp(trk.filterRelease + change, 0, 99); }
            else if (synthGridCol == 4) { // VOY & PRT Slot
                                // Both parameters require Shift combinations because Shift-less arrows navigate.
                                if (encoderTurn != 0) {
                                    // 1. ROTARY ENCODER: Turn normally to edit VOY, turn with Shift to edit PRT
                                    if (isShiftDown) {
                                        if (isStepLock) {
                                            int base = (sp.glideTime == -1) ? trk.glideTime : sp.glideTime;
                                            sp.glideTime = std::clamp(base + encoderTurn, 0, 99);
                                        } else {
                                            trk.glideTime = std::clamp(trk.glideTime + encoderTurn, 0, 99);
                                        }
                                    } else {
                                        if (isStepLock) {
                                            int base = (sp.polyMode == -1) ? trk.polyMode : sp.polyMode;
                                            sp.polyMode = std::clamp(base + encoderTurn, 1, 4);
                                        } else {
                                            trk.polyMode = std::clamp(trk.polyMode + encoderTurn, 1, 4);
                                        }
                                    }
                                }
                                else if (isShiftDown) {
                                    // 2. ARROW KEYS: Only process edits when Shift is held down
                                    bool pressUp = IsKeyDown(KEY_UP);
                                    bool pressDown = IsKeyDown(KEY_DOWN);
                                    bool pressRight = IsKeyDown(KEY_RIGHT);
                                    bool pressLeft = IsKeyDown(KEY_LEFT);

                                    if (pressUp || pressDown) {
                                        // Shift + Up/Down edits Voice limit (VOY: 1..4)
                                        int voyDir = pressUp ? 1 : -1;
                                        if (isStepLock) {
                                            int base = (sp.polyMode == -1) ? trk.polyMode : sp.polyMode;
                                            sp.polyMode = std::clamp(base + voyDir, 1, 4);
                                        } else {
                                            trk.polyMode = std::clamp(trk.polyMode + voyDir, 1, 4);
                                        }
                                    }
                                    else if (pressRight || pressLeft) {
                                        // Shift + Left/Right edits Portamento Glide (PRT: 0..99)
                                        int prtDir = pressRight ? 1 : -1;
                                        if (isStepLock) {
                                            int base = (sp.glideTime == -1) ? trk.glideTime : sp.glideTime;
                                            sp.glideTime = std::clamp(base + prtDir, 0, 99);
                                        } else {
                                            trk.glideTime = std::clamp(trk.glideTime + prtDir, 0, 99);
                                        }
                                    }
                                }
                            }
            else if (synthGridCol == 5) {
                int baseW2 = (isStepLock && sp.lfo2Wave != -1) ? sp.lfo2Wave : trk.lfo2Wave;
                int newW2 = baseW2 + change;
                if (newW2 < 0) newW2 = 5; if (newW2 > 5) newW2 = 0;
                if (isStepLock) sp.lfo2Wave = newW2;
                else            trk.lfo2Wave = newW2;
            }
            else if (synthGridCol == 6) { if (isStepLock) EditParam(sp.lfo2Speed, trk.lfo2Speed, change, 0, 99); else trk.lfo2Speed = std::clamp(trk.lfo2Speed + change, 0, 99); }
            else if (synthGridCol == 7) { if (isStepLock) EditParam(sp.lfo2Depth, trk.lfo2Depth, change, 0, 99); else trk.lfo2Depth = std::clamp(trk.lfo2Depth + change, 0, 99); }
            else if (synthGridCol == 8) {
                int baseTrg2 = (isStepLock && sp.lfo2Trigger != -1) ? sp.lfo2Trigger : trk.lfo2Trigger;
                int newTrg2 = (baseTrg2 == 0) ? 1 : 0;
                if (isStepLock) sp.lfo2Trigger = newTrg2;
                else            trk.lfo2Trigger = newTrg2;
            }
            else if (synthGridCol == 9) {
                int baseSync2 = (isStepLock && sp.lfo2Sync != -1) ? sp.lfo2Sync : trk.lfo2Sync;
                int newSync2 = (baseSync2 == 0) ? 1 : 0;
                if (isStepLock) sp.lfo2Sync = newSync2;
                else            trk.lfo2Sync = newSync2;
            }
        }
    }
    else if (currentScreen == SCREEN_PLACEHOLDER) {
        if (synthGridRow == 1) {
            if (synthGridCol == 0)      { if (isStepLock) EditParam(sp.tapeMemory, trk.tapeMemory, change, 0, 99); else trk.tapeMemory = std::clamp(trk.tapeMemory + change, 0, 99); }
            else if (synthGridCol == 1) { if (isStepLock) EditParam(sp.tapeHeads, trk.tapeHeads, change, 1, 4); else trk.tapeHeads = std::clamp(trk.tapeHeads + change, 1, 4); }
            else if (synthGridCol == 2) { if (isStepLock) EditParam(sp.tapeSpread, trk.tapeSpread, change, 0, 99); else trk.tapeSpread = std::clamp(trk.tapeSpread + change, 0, 99); }
            else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.tapeSpeed, trk.tapeSpeed, change, 0, 99); else trk.tapeSpeed = std::clamp(trk.tapeSpeed + change, 0, 99); }
            else if (synthGridCol == 4) { if (isStepLock) EditParam(sp.tapeTether, trk.tapeTether, change, 0, 99); else trk.tapeTether = std::clamp(trk.tapeTether + change, 0, 99); }
            else if (synthGridCol == 5) { if (isStepLock) EditParam(sp.tapeDrift, trk.tapeDrift, change, 0, 99); else trk.tapeDrift = std::clamp(trk.tapeDrift + change, 0, 99); }
            else if (synthGridCol == 6) { if (isStepLock) EditParam(sp.tapeDriftRate, trk.tapeDriftRate, change, 0, 99); else trk.tapeDriftRate = std::clamp(trk.tapeDriftRate + change, 0, 99); }
            else if (synthGridCol == 7) { if (isStepLock) EditParam(sp.reverbSend, trk.reverbSend, change, 0, 99); else trk.reverbSend = std::clamp(trk.reverbSend + change, 0, 99); }
            else if (synthGridCol == 8) { if (isStepLock) EditParam(sp.delaySend, trk.delaySend, change, 0, 99); else trk.delaySend = std::clamp(trk.delaySend + change, 0, 99); }
        }
        else if (synthGridRow == 3) {
            if (synthGridCol == 0)      { if (isStepLock) EditParam(sp.tapeFeedback, trk.tapeFeedback, change, 0, 99); else trk.tapeFeedback = std::clamp(trk.tapeFeedback + change, 0, 99); }
            else if (synthGridCol == 1) { if (isStepLock) EditParam(sp.tapeFbSpread, trk.tapeFbSpread, change, 0, 99); else trk.tapeFbSpread = std::clamp(trk.tapeFbSpread + change, 0, 99); }
            else if (synthGridCol == 2) { if (isStepLock) EditParam(sp.tapeFbSource, trk.tapeFbSource, change, 0, 99); else trk.tapeFbSource = std::clamp(trk.tapeFbSource + change, 0, 99); }
            else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.tapeFreeze, trk.tapeFreeze, change, 0, 99); else trk.tapeFreeze = std::clamp(trk.tapeFreeze + change, 0, 99); }
            else if (synthGridCol == 4) { if (isStepLock) EditParam(sp.tapeSmearRate, trk.tapeSmearRate, change, 0, 99); else trk.tapeSmearRate = std::clamp(trk.tapeSmearRate + change, 0, 99); }
            else if (synthGridCol == 5) { if (isStepLock) EditParam(sp.tapeSmearSize, trk.tapeSmearSize, change, 0, 99); else trk.tapeSmearSize = std::clamp(trk.tapeSmearSize + change, 0, 99); }
            else if (synthGridCol == 6) { if (isStepLock) EditParam(sp.tapeMix, trk.tapeMix, change, 0, 99); else trk.tapeMix = std::clamp(trk.tapeMix + change, 0, 99); }
            else if (synthGridCol == 7) { if (isStepLock) EditParam(sp.saturationSend, trk.saturationSend, change, 0, 99); else trk.saturationSend = std::clamp(trk.saturationSend + change, 0, 99); }
            else if (synthGridCol == 8) { if (isStepLock) EditParam(sp.autoPanSend, trk.autoPanSend, change, 0, 99); else trk.autoPanSend = std::clamp(trk.autoPanSend + change, 0, 99); }
        }
    }
    else if (currentScreen == SCREEN_GLOBAL_FX) {
        if (synthGridRow == 1) {
            if (synthGridCol == 0)      { globalFX.reverbDecay = std::clamp(globalFX.reverbDecay + change, 0, 99); }
            else if (synthGridCol == 1) { globalFX.reverbSize = std::clamp(globalFX.reverbSize + change, 0, 99); }
            else if (synthGridCol == 2) { globalFX.satLevel = std::clamp(globalFX.satLevel + change, 0, 99); }
            else if (synthGridCol == 3) { globalFX.satSymmetry = std::clamp(globalFX.satSymmetry + change, 0, 99); }
            else if (synthGridCol == 4) { globalFX.delayTime = std::clamp(globalFX.delayTime + change, 0, 99); }
            else if (synthGridCol == 5) { globalFX.delayFeedback = std::clamp(globalFX.delayFeedback + change, 0, 99); }
            else if (synthGridCol == 6) { globalFX.autoPanTime = std::clamp(globalFX.autoPanTime + change, 0, 99); }
            else if (synthGridCol == 7) { globalFX.autoPanFeedback = std::clamp(globalFX.autoPanFeedback + change, 0, 99); }
        }
        else if (synthGridRow == 2) {
            if (synthGridCol == 0)      { globalFX.reverbPredelay = std::clamp(globalFX.reverbPredelay + change, 0, 99); }
            else if (synthGridCol == 1) { globalFX.reverbMix = std::clamp(globalFX.reverbMix + change, 0, 99); }
            else if (synthGridCol == 2) { globalFX.satOverdrive = std::clamp(globalFX.satOverdrive + change, 0, 99); }
            else if (synthGridCol == 3) { globalFX.satMix = std::clamp(globalFX.satMix + change, 0, 99); }
            else if (synthGridCol == 4) { globalFX.delayPingPong = (globalFX.delayPingPong == 0) ? 1 : 0; }
            else if (synthGridCol == 5) { globalFX.delayMix = std::clamp(globalFX.delayMix + change, 0, 99); }
            else if (synthGridCol == 6) { globalFX.autoPanWidth = std::clamp(globalFX.autoPanWidth + change, 0, 99); }
            else if (synthGridCol == 7) { globalFX.autoPanMix = std::clamp(globalFX.autoPanMix + change, 0, 99); }
        }
    }
}

// =========================================================================
// MAIN PROGRAM LOOP
// =========================================================================

int main() {
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "8-Track Sequencer - Premium UI");
    SetTargetFPS(60);

    RenderTexture2D oledScreen = LoadRenderTexture(OLED_WIDTH, OLED_HEIGHT);
    SetTextureFilter(oledScreen.texture, TEXTURE_FILTER_POINT);

    InitializeTracks();
    InitAudioEngine();
    InitMidi();
    InitOled();
    
#if defined(__linux__)
    std::thread encoderThread(runEncoderThread);
#endif
    
    // --- STARTUP BOOT ANIMATION STAGE ---
    bool playBootAnimation = true;
    int bootFrame = 0;

    while (playBootAnimation && !WindowShouldClose()) {
        usleep(83000); // stable ~12 FPS
        
        bootFrame++;
        if (bootFrame >= BOOT_FRAME_COUNT) {
            playBootAnimation = false;
        }

        CpuClearBackground(BLACK);
        if (bootFrame < BOOT_FRAME_COUNT) {
            for (int r = 0; r < BOOT_ROWS; ++r) {
                for (int c = 0; c < BOOT_COLS; ++c) {
                    if (bootAnimationData[bootFrame][r][c] != 0) {
                        CpuDrawPixel(c, r, WHITE);
                    }
                }
            }
        }

        UpdateTexture(oledScreen.texture, g_oledCPUPixels);
        UpdateOled(oledScreen);

        BeginDrawing();
            ClearBackground(DARKGRAY);
            Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
            Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
        EndDrawing();
    }
    
    int uiFrameCounter = 0;
    
    // Time-based key repeat timers
    float keyRepeatTimer = 0.0f;
    float navRepeatTimer = 0.0f;
    float tempoRepeatTimer = 0.0f;
    float menuRepeatTimer = 0.0f;
    
    copiedStep = Step();
    hasCopiedStep = false;

    while (!WindowShouldClose()) {
        uiFrameCounter++;
        bool blinkOn = (uiFrameCounter % 30 < 15);

        // Detect Modifier Keys
        bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                          IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);

        int encoderTurn = 0;
        bool encoderButton = false;
        #if defined(__linux__)
        encoderTurn = g_encoderTurnQueue.exchange(0);
        encoderButton = g_encoderButtonState.load();
        g_hardwareEncoderClicked = encoderButton;
        #else
        g_hardwareEncoderClicked = false;
        #endif

        bool isShiftDown = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                // Exclude Control modifier to prevent Ctrl+X step-popup triggers from writing parameter locks
                bool isAltDown = IsKeyDown(KEY_X) && !isCtrlDown;
                bool isAltHeld = IsKeyDown(KEY_X) && !isCtrlDown;
        
        #if defined(__linux__)
        if (isCtrlDown && isAltDown && IsKeyPressed(KEY_GRAVE)) {
            ShutdownOled();
            ShutdownAudioEngine();
            ShutdownMidi();
            std::cout << "[SYSTEM] Safe system shutdown initiated by keyboard..." << std::endl;
            system("sudo shutdown -h now");
        }
        #endif
        
        // --- GLOBAL ACTIVE PIANO KEYS STRING DETERMINATION ---
        std::string activeNotesString = "";
        int keysPressedCount = 0;
        if (liveKeyboardActive) {
            for (int i = 0; i < NUM_NOTES; ++i) {
                if (IsKeyDown(keyboardPiano[i].key)) {
                    if (keysPressedCount < 3) {
                        if (!activeNotesString.empty()) activeNotesString += " ";
                        activeNotesString += keyboardPiano[i].noteName;
                        keysPressedCount++;
                    } else if (keysPressedCount == 3) {
                        activeNotesString += "+";
                        keysPressedCount++;
                    }
                }
            }
        }

        // --- SMART TAB BUTTON MANAGER (MENU / SAFE SHUTDOWN) ---
                static float tabHoldTimer = 0.0f;
                static bool tabWasDown = false;

                if (IsKeyDown(KEY_TAB)) {
                    tabHoldTimer += GetFrameTime();
                    tabWasDown = true;

                    // Start showing visual countdown feedback after 1 second of holding
                    if (tabHoldTimer >= 1.0f) {
                        int remaining = 3 - (int)tabHoldTimer;
                        if (remaining < 1) remaining = 1;
                        
                        char shutBuf[64];
                        snprintf(shutBuf, sizeof(shutBuf), "SHUTDOWN IN %d...", remaining);
                        menuFeedback = shutBuf;
                    }
                    
                    // Trigger safe shutdown once held for a full 3 seconds
                    if (tabHoldTimer >= 3.0f) {
                        #if defined(__linux__)
                        // Unload all active hardware drivers cleanly to prevent card corruption
                        ShutdownOled();
                        ShutdownAudioEngine();
                        ShutdownMidi();
                        std::cout << "[SYSTEM] Safe system shutdown initiated by TAB hold..." << std::endl;
                        
                        // Instruct Linux to safely power down the Raspberry Pi
                        std::system("sudo shutdown -h now");
                        #else
                        menuFeedback = "MAC SHUTDOWN SIMULATED";
                        tabHoldTimer = 0.0f;
                        tabWasDown = false;
                        #endif
                    }
                } else {
                    if (tabWasDown) {
                        // Key was just released!
                        if (tabHoldTimer < 0.5f) {
                            // Short Press: Toggle the standard System Menu
                            systemMenuOpen = !systemMenuOpen;
                            systemMenuState = 0;
                            menuFeedback = systemMenuOpen ? "SYSTEM MENU OPEN" : "";
                        } else {
                            // Long Press aborted: Clear countdown and return to normal
                            menuFeedback = "";
                        }
                        tabHoldTimer = 0.0f;
                        tabWasDown = false;
                    }
                }
      
        // Ctrl + . (Ctrl + Full Stop) to toggle the Performance Popup
                if (isCtrlDown && IsKeyPressed(KEY_PERIOD)) {
                    perfPopupOpen = !perfPopupOpen;
                    if (perfPopupOpen) {
                        // Initialize default parameter column positions on open
                        synthGridRow = 0;
                        synthGridCol = 0;
                        activeStutterKey = -1; // Reset stutter state
                        menuFeedback = "PERFORMANCE FX OPEN";
                    } else {
                        menuFeedback = "PERFORMANCE FX CLOSED";
                    }
                }
        
        
        // --- DIAGNOSTICS KEYBOARD INTERCEPT ---
        if (showDiagnostics) {
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                showDiagnostics = false;
                menuFeedback = "DIAGNOSTICS CLOSED";
            }
            if (isCtrlDown && isShiftDown && IsKeyPressed(KEY_NINE)) {
                showDiagnostics = false;
                menuFeedback = "DIAGNOSTICS CLOSED";
            }
            if (IsKeyPressed(KEY_LEFT)) {
                if (g_midiManager && g_midiManager->isConnected()) {
                    g_midiManager->cyclePort(-1);
                    menuFeedback = "MIDI PORT PREVIOUS";
                }
            }
            if (IsKeyPressed(KEY_RIGHT)) {
                if (g_midiManager && g_midiManager->isConnected()) {
                    g_midiManager->cyclePort(1);
                    menuFeedback = "MIDI PORT NEXT";
                }
            }

            BeginTextureMode(oledScreen);
                ClearBackground(BLACK);
                UIState state = {
                    currentScreen, selectedTrack, cursorTrack, cursorStep,
                    currentOctave, tempo, isPlaying, playhead,
                    synthGridRow, synthGridCol, trackParamsGridCol,
                    blinkOn, activeNotesString,
                    systemMenuOpen, systemMenuCursor,
                    menuFeedback,
                    systemMenuState, fileBrowserCursor, g_typingBuffer.c_str(), g_typingCursor
                };
                DrawDiagnosticsScreen(state);
            EndTextureMode();

            BeginDrawing();
                ClearBackground(DARKGRAY);
                Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
                Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                Vector2 origin = { 0.0f, 0.0f };
                DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
            EndDrawing();
            continue;
        }
        if (perfPopupOpen) {
                    HandlePerformancePopupInputs(encoderTurn, encoderButton, isShiftDown);
                    BeginTextureMode(oledScreen);
                        ClearBackground(BLACK);
                        UIState state = {
                            currentScreen, selectedTrack, cursorTrack, cursorStep,
                            currentOctave, tempo, isPlaying, playhead,
                            synthGridRow, synthGridCol, trackParamsGridCol,
                            blinkOn, activeNotesString,
                            systemMenuOpen, systemMenuCursor,
                            menuFeedback,
                            systemMenuState, fileBrowserCursor, g_typingBuffer.c_str(), g_typingCursor
                        };
                        
                        // Draw whatever active page is running in the background behind our overlay
                        if (showDiagnostics) {
                            DrawDiagnosticsScreen(state);
                        } else {
                            if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) DrawSequencerScreen(state);
                            else if (currentScreen == SCREEN_SYNTH) DrawSynthScreen(state);
                            else if (currentScreen == SCREEN_TRACK_PARAMS) DrawFilterLfoPage(state);
                            else if (currentScreen == SCREEN_PLACEHOLDER) DrawPlaceholderPage(state);
                            else if (currentScreen == SCREEN_GLOBAL_FX) DrawGlobalFXPage(state);
                        }
                        
                        // Draw the performance popup on top of the layout
                        DrawPerformancePopup(state);
                    EndTextureMode();

                    UpdateTexture(oledScreen.texture, g_oledCPUPixels);
                    UpdateOled(oledScreen);

                    BeginDrawing();
                        ClearBackground(DARKGRAY);
                        Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
                        Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                        Vector2 origin = { 0.0f, 0.0f };
                        DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
                    EndDrawing();
                    continue; // Intercept inputs and skip regular main loop parsing
                }
        
        
        // --- MODAL POPUPS DISPATCH ---
                if (lfoPopupOpen) {
                    HandleLfoPopupInputs(encoderTurn, encoderButton, isShiftDown);
                    BeginTextureMode(oledScreen);
                        ClearBackground(BLACK);
                        UIState state = {
                            currentScreen, selectedTrack, cursorTrack, cursorStep,
                            currentOctave, tempo, isPlaying, playhead,
                            synthGridRow, synthGridCol, trackParamsGridCol,
                            blinkOn, activeNotesString,
                            systemMenuOpen, systemMenuCursor,
                            menuFeedback,
                            systemMenuState, fileBrowserCursor, g_typingBuffer.c_str(), g_typingCursor
                        };
                        DrawFilterLfoPage(state);
                        DrawModulationPopup(state);
                    EndTextureMode();

                    // UPLOAD THE NEW CPU PIXELS TO THE GPU TEXTURE FOR MAC RENDER
                    UpdateTexture(oledScreen.texture, g_oledCPUPixels);

                    UpdateOled(oledScreen);
                    
                    BeginDrawing();
                        ClearBackground(DARKGRAY);
                        Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
                        Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                        Vector2 origin = { 0.0f, 0.0f };
                        DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
                    EndDrawing();
                    continue;
                }

        if (stepPopupOpen) {
            HandleStepPopupInputs(encoderTurn, encoderButton, isShiftDown);
            BeginTextureMode(oledScreen);
                ClearBackground(BLACK);
                UIState state = {
                    currentScreen, selectedTrack, cursorTrack, cursorStep,
                    currentOctave, tempo, isPlaying, playhead,
                    synthGridRow, synthGridCol, trackParamsGridCol,
                    blinkOn, activeNotesString,
                    systemMenuOpen, systemMenuCursor,
                    menuFeedback,
                    systemMenuState, fileBrowserCursor, g_typingBuffer.c_str(), g_typingCursor
                };
                if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) {
                    DrawSequencerScreen(state);
                }
                DrawStepPopup(state);
            EndTextureMode();

            UpdateTexture(oledScreen.texture, g_oledCPUPixels);
            UpdateOled(oledScreen);

            BeginDrawing();
                ClearBackground(DARKGRAY);
                Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
                Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                Vector2 origin = { 0.0f, 0.0f };
                DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
            EndDrawing();
            continue;
        }

        if (systemMenuOpen) {
            int menuDir = 0;
            bool triggerMenuNav = false;
            bool isUpDownHeld = IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN);
            if (isUpDownHeld) {
                if (menuRepeatTimer == 0.0f) {
                    triggerMenuNav = true;
                    menuRepeatTimer += GetFrameTime();
                } else {
                    menuRepeatTimer += GetFrameTime();
                    const float INITIAL_DELAY = 0.250f;
                    const float REPEAT_INTERVAL = 0.060f;
                    if (menuRepeatTimer >= INITIAL_DELAY) {
                        triggerMenuNav = true;
                        menuRepeatTimer -= REPEAT_INTERVAL;
                    }
                }

                if (triggerMenuNav) {
                    if (IsKeyDown(KEY_UP))    menuDir = -1;
                    if (IsKeyDown(KEY_DOWN))  menuDir = 1;
                }
            } else {
                menuRepeatTimer = 0.0f;
            }

            HandleSystemMenuInputs(menuDir, encoderTurn);
            // --- DRAW TO CPU FRAMEBUFFER ---
            CpuClearBackground(BLACK);
            CpuDrawLine(0, 7, OLED_WIDTH, 7, WHITE);

            UIState state = {
                currentScreen, selectedTrack, cursorTrack, cursorStep,
                currentOctave, tempo, isPlaying, playhead,
                synthGridRow, synthGridCol, trackParamsGridCol,
                blinkOn, activeNotesString,
                systemMenuOpen, systemMenuCursor,
                menuFeedback,
                systemMenuState, fileBrowserCursor, g_typingBuffer.c_str(), g_typingCursor
            };

            if (showDiagnostics) {
                DrawDiagnosticsScreen(state);
            } else {
                if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) DrawSequencerScreen(state);
                else if (currentScreen == SCREEN_SYNTH) DrawSynthScreen(state);
                else if (currentScreen == SCREEN_TRACK_PARAMS) DrawFilterLfoPage(state);
                else if (currentScreen == SCREEN_PLACEHOLDER) DrawPlaceholderPage(state);
                else if (currentScreen == SCREEN_GLOBAL_FX) DrawGlobalFXPage(state);
            }

            if (lfoPopupOpen) DrawModulationPopup(state);
            DrawSystemMenu(state);

            UpdateTexture(oledScreen.texture, g_oledCPUPixels);
            UpdateOled(oledScreen);
            
            BeginDrawing();
                ClearBackground(DARKGRAY);
                Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
                Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                Vector2 origin = { 0.0f, 0.0f };
                DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
            EndDrawing();
            continue;
        }

        // --- GLOBAL LIVE KEYBOARD DISPATCH ---
        HandlePianoKeysInput(currentOctave);

        // --- GLOBAL KEY COMMAND PROCESSING ---
                if (IsActionKeyPressed()) {
                    // Check if we are selecting LFO rows on the parameters page to prevent play/stop triggers
                    bool isLfoRowSelected = (currentScreen == SCREEN_TRACK_PARAMS && (synthGridRow == 2 || synthGridRow == 4));

                    // Only toggle transport if we aren't in menus, popups, or selecting modal pages
                    if (!systemMenuOpen && !lfoPopupOpen && !stepPopupOpen && !isLfoRowSelected) {
                        isPlaying = !isPlaying;
                    }
                }

        // Delete Step (Backspace)
        if (IsKeyPressed(KEY_BACKSPACE) && !isShiftDown && !isCtrlDown) {
            bool isSequencerPage = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8);
            if (isSequencerPage) {
                Step& step = tracks[selectedTrack].steps[cursorStep];
                step.note = -1;                  // Change empty string to -1 sentinel
                step.velocity = 0;
                // Removed step.condition = "";  // Deleted dead weight field
                step.retrigger = 0;
                step.microtiming = 0;
                step.params.reset();
                menuFeedback = "STEP CLEARED";
            }
        }

        if (isShiftDown && !isCtrlDown) {
            // Shift + E: Swap Engine
            if (IsKeyPressed(KEY_E)) {
                if (tracks[selectedTrack].engineType == ENGINE_SYNTH) {
                    tracks[selectedTrack].engineType = ENGINE_SAMPLER;
                    tracks[selectedTrack].algorithm = ALGO_SAMPLE;
                } else {
                    tracks[selectedTrack].engineType = ENGINE_SYNTH;
                    tracks[selectedTrack].algorithm = ALGO_PARALLEL;
                }
            }
            // Shift + R: Switch Engine Algorithm Mode
            if (IsKeyPressed(KEY_W)) {
                if (tracks[selectedTrack].engineType == ENGINE_SAMPLER) {
                    tracks[selectedTrack].algorithm = (tracks[selectedTrack].algorithm == ALGO_SAMPLE) ? ALGO_GRANULAR : ALGO_SAMPLE;
                } else {
                    if (tracks[selectedTrack].algorithm == ALGO_PARALLEL) {
                        tracks[selectedTrack].algorithm = ALGO_CARRIER_MOD;
                        tracks[selectedTrack].coarse2 = std::clamp(tracks[selectedTrack].coarse2, 1, 16);
                    } else {
                        tracks[selectedTrack].algorithm = ALGO_PARALLEL;
                    }
                }
            }
            
            // Shift + Backspace: Clear Tracks or Step Lock resets
                        if (IsKeyPressed(KEY_BACKSPACE)) {
                            bool isAltHeld = IsKeyDown(KEY_X);
                            if (isAltHeld) {
                                tracks[selectedTrack].steps[cursorStep].params.reset();
                                menuFeedback = "STEP LOCKS CLEARED";
                            } else {
                                bool isSequencerPage = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8);
                                if (isSequencerPage) {
                                    for (int s = 0; s < 16; ++s) {
                                        Step& step = tracks[selectedTrack].steps[s];
                                        step.note = -1;  // Replaced empty string with our -1 sentinel
                                        step.velocity = 0;
                                        // Removed step.condition = ""; // Deleted legacy string
                                        step.retrigger = 0;
                                        step.microtiming = 0;
                                        step.params.reset();
                                    }
                                    menuFeedback = "TRACK TRIGS CLEARED";
                                } else {
                                    ResetTrackToDefault(selectedTrack);
                                    menuFeedback = "TRACK SOUND DEFAULTED";
                                }
                            }
                        }
        }

        // --- TRACK MUTE / SOLO CONTROL ENGINE ---
        // 1. Unshifted M: Toggles mute on the currently selected active track
        if (IsKeyPressed(KEY_M) && !isShiftDown && !isCtrlDown) {
            tracks[selectedTrack].muted = !tracks[selectedTrack].muted;
            menuFeedback = tracks[selectedTrack].muted ? "TRACK MUTED" : "TRACK UNMUTED";
        }

        // 2. M + Modifiers + Keybed: Targeted Mute or Solo on any track
        if (IsKeyDown(KEY_M)) {
            int targetTrk = -1;
            if (IsKeyPressed(KEY_A))      targetTrk = 0;
            else if (IsKeyPressed(KEY_S)) targetTrk = 1;
            else if (IsKeyPressed(KEY_D)) targetTrk = 2;
            else if (IsKeyPressed(KEY_F)) targetTrk = 3;
            else if (IsKeyPressed(KEY_G)) targetTrk = 4;
            else if (IsKeyPressed(KEY_H)) targetTrk = 5;
            else if (IsKeyPressed(KEY_J)) targetTrk = 6;
            else if (IsKeyPressed(KEY_K)) targetTrk = 7;

            if (targetTrk >= 0) {
                if (isShiftDown) {
                    // M + Shift + Track Key: Mute/Unmute target
                    tracks[targetTrk].muted = !tracks[targetTrk].muted;
                    menuFeedback = tracks[targetTrk].muted ? "TRACK MUTED" : "TRACK UNMUTED";
                }
                else if (isCtrlDown) {
                    // M + Ctrl + Track Key: Solo target track
                    bool isAnyOtherUnmuted = false;
                    for (int t = 0; t < 8; ++t) {
                        if (t != targetTrk && !tracks[t].muted) {
                            isAnyOtherUnmuted = true;
                            break;
                        }
                    }
                    if (isAnyOtherUnmuted) {
                        for (int t = 0; t < 8; ++t) {
                            tracks[t].muted = (t != targetTrk);
                        }
                        menuFeedback = "TRACK SOLO ACTIVE";
                    } else {
                        for (int t = 0; t < 8; ++t) {
                            tracks[t].muted = false;
                        }
                        menuFeedback = "ALL TRACKS UNMUTED";
                    }
                }
            }
        }

        // --- PATTERN SWAPPING (Ctrl + ASDFGHJK) ---
        int targetPattern = -1;
        if (isCtrlDown && !IsKeyDown(KEY_M)) {
            if (IsKeyPressed(KEY_A))      targetPattern = 0;
            else if (IsKeyPressed(KEY_S)) targetPattern = 1;
            else if (IsKeyPressed(KEY_D)) targetPattern = 2;
            else if (IsKeyPressed(KEY_F)) targetPattern = 3;
            else if (IsKeyPressed(KEY_G)) targetPattern = 4;
            else if (IsKeyPressed(KEY_H)) targetPattern = 5;
            else if (IsKeyPressed(KEY_J)) targetPattern = 6;
            else if (IsKeyPressed(KEY_K)) targetPattern = 7;
        }

        if (targetPattern >= 0) {
            queuedPattern = targetPattern;
            menuFeedback = "PATTERN QUEUED";
        }

        // --- TRACK NAVIGATION (Shift + ASDFGHJK) ---
        int trackKeyTriggered = -1;
        if (isShiftDown && !IsKeyDown(KEY_M) && !isCtrlDown) {
            if (IsKeyPressed(KEY_A))      trackKeyTriggered = 0;
            else if (IsKeyPressed(KEY_S)) trackKeyTriggered = 1;
            else if (IsKeyPressed(KEY_D)) trackKeyTriggered = 2;
            else if (IsKeyPressed(KEY_F)) trackKeyTriggered = 3;
            else if (IsKeyPressed(KEY_G)) trackKeyTriggered = 4;
            else if (IsKeyPressed(KEY_H)) trackKeyTriggered = 5;
            else if (IsKeyPressed(KEY_J)) trackKeyTriggered = 6;
            else if (IsKeyPressed(KEY_K)) trackKeyTriggered = 7;
        }

        if (trackKeyTriggered >= 0) {
            selectedTrack = trackKeyTriggered;
            cursorTrack = trackKeyTriggered % 4;

            if (trackKeyTriggered < 4) {
                if (currentScreen == SCREEN_SEQ_5_8) {
                    currentScreen = SCREEN_SEQ_1_4;
                    activeScreenRow = 0;
                }
            } else {
                if (currentScreen == SCREEN_SEQ_1_4) {
                    currentScreen = SCREEN_SEQ_5_8;
                    activeScreenRow = 1;
                }
            }
        }

        // Shift + Backspace: Clear active page trigs/parameter locks
        // Locate Clear Page (Shift + Backspace) inside main's loop:
        // Shift + Backspace: Clear active page trigs/parameter locks
        if (IsKeyPressed(KEY_BACKSPACE) && isShiftDown && !isCtrlDown) {
            bool isSequencerPage = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8);
            int startStep = activePage * 16;

            if (isSequencerPage) {
                for (int s = startStep; s < startStep + 16; ++s) {
                    Step& step = tracks[selectedTrack].steps[s];
                    step.note = -1;                  // Change empty string to -1 sentinel
                    step.velocity = 0;
                    // Removed step.condition = "";  // Deleted dead weight field
                    step.retrigger = 0;
                    step.microtiming = 0;
                    step.params.reset();
                }
                menuFeedback = (activePage == 0) ? "PAGE 1 TRIGS RESET" : "PAGE 2 TRIGS RESET";
                    } else {
                        for (int s = startStep; s < startStep + 16; ++s) {
                            tracks[selectedTrack].steps[s].params.reset();
                        }
                        menuFeedback = (activePage == 0) ? "PAGE 1 PARAM LOCKS RESET" : "PAGE 2 PARAM LOCKS RESET";
                    }
                }

        // Octave modification via Comma/Period (Only un-shifted)
        if (!isCtrlDown && !isShiftDown) {
            if (IsKeyPressed(KEY_COMMA)) {
                currentOctave--;
                if (currentOctave < 0) currentOctave = 0;
            }
            if (IsKeyPressed(KEY_PERIOD)) {
                currentOctave++;
                if (currentOctave > 8) currentOctave = 8;
            }
        }

        // --- CLIPBOARD COMMANDS (Ctrl+C to Copy, Shift+C to Paste) ---
        if (isCtrlDown) {
            if (IsKeyPressed(KEY_C)) {
                int activeTrack = (currentScreen == SCREEN_SEQ_5_8) ? cursorTrack + 4 : cursorTrack;
                copiedStep = tracks[activeTrack].steps[cursorStep];
                hasCopiedStep = true;
                menuFeedback = "STEP COPIED";
            }
        }
        
        if (isShiftDown && IsKeyPressed(KEY_C)) {
            if (hasCopiedStep) {
                int activeTrack = (currentScreen == SCREEN_SEQ_5_8) ? cursorTrack + 4 : cursorTrack;
                tracks[activeTrack].steps[cursorStep] = copiedStep;
                menuFeedback = "STEP PASTED";
            }
        }

        // BPM Tempo increments (Shift + Comma/Period)
        if (!isCtrlDown && isShiftDown) {
            double deltaTempo = 0.0;
            bool triggerTempo = false;
            bool isTempoKeyDown = IsKeyDown(KEY_COMMA) || IsKeyDown(KEY_PERIOD);

            if (isTempoKeyDown) {
                if (tempoRepeatTimer == 0.0f) {
                    triggerTempo = true;
                    tempoRepeatTimer += GetFrameTime();
                } else {
                    tempoRepeatTimer += GetFrameTime();
                    const float INITIAL_DELAY = 0.30f;
                    const float REPEAT_INTERVAL = 0.08f;
                    if (tempoRepeatTimer >= INITIAL_DELAY) {
                        triggerTempo = true;
                        tempoRepeatTimer -= REPEAT_INTERVAL;
                    }
                }
            } else {
                tempoRepeatTimer = 0.0f;
            }

            if (triggerTempo) {
                if (IsKeyDown(KEY_PERIOD)) deltaTempo = 1.0;
                if (IsKeyDown(KEY_COMMA))  deltaTempo = -1.0;

                // Hold Alt (X) alongside Shift to jump by 10 BPM
                if (isAltHeld) {
                    deltaTempo *= 10.0;
                }
                tempo += deltaTempo;
                if (tempo < 20.0)   tempo = 20.0;
                if (tempo > 300.0)  tempo = 300.0;
            }
        }

        // Polyphonic Live step disperser
        if (liveKeyboardActive && !isCtrlDown && (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8)) {
                   bool anyKeyPressed = false;
                   for (int i = 0; i < NUM_NOTES; ++i) {
                       if (IsKeyPressed(keyboardPiano[i].key)) {
                           anyKeyPressed = true;
                           break;
                       }
                   }

                   if (anyKeyPressed) {
                       std::vector<NoteBinding> heldNotes;
                       for (int i = 0; i < NUM_NOTES; ++i) {
                           if (IsKeyDown(keyboardPiano[i].key)) {
                               heldNotes.push_back(keyboardPiano[i]);
                           }
                       }

                       int notesToDisperse = std::min((int)heldNotes.size(), 4);
                       for (int n = 0; n < notesToDisperse; ++n) {
                           std::string noteName = heldNotes[n].noteName;
                           int octaveToUse = currentOctave;
                           
                           if (noteName == "C+") {
                               noteName = "C";
                               octaveToUse = currentOctave + 1;
                               if (octaveToUse > 8) octaveToUse = 8;
                           }
                           
                           std::string noteStr = noteName + std::to_string(octaveToUse);
                           int trackOffset = (currentScreen == SCREEN_SEQ_5_8) ? 4 : 0;
                           int targetTrack = trackOffset + ((cursorTrack + n) % 4);
                           
                           // Translate string notation to MIDI numbers on keypress
                           tracks[targetTrack].steps[cursorStep].note = NoteToMidi(noteStr);
                           tracks[targetTrack].steps[cursorStep].velocity = 3;
                       }
                   }
               }

        // Toggle Step Popup Open (Ctrl/Cmd + X Hotkey)
                if (isCtrlDown && IsKeyPressed(KEY_X)) {
                    if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) {
                        stepPopupOpen = !stepPopupOpen;
                        menuFeedback = stepPopupOpen ? "STEP POPUP OPEN" : "STEP POPUP CLOSED";
                        if (stepPopupOpen) {
                            stepPopupFocusX = 0;
                            stepPopupFocusY = 0;
                            stepPopupCondCol = 0;
                            stepPopupChordKey = 0;
                        }
                    }
                }
        
        // Ctrl + Shift + 9 (or Cmd + Shift + 9) to toggle the Diagnostics screen
        if (isCtrlDown && isShiftDown && IsKeyPressed(KEY_NINE)) {
            showDiagnostics = !showDiagnostics;
            menuFeedback = showDiagnostics ? "DIAGNOSTICS OPEN" : "DIAGNOSTICS CLOSED";
        }
        
        // Shift + Navigations
        if (isShiftDown && !isCtrlDown) {
            bool triggerAction = false;
            bool anyEditKeyDown = IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) ||
                                  IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT);

            if (anyEditKeyDown) {
                if (keyRepeatTimer == 0.0f) {
                    triggerAction = true;
                    keyRepeatTimer += GetFrameTime();
                } else {
                    keyRepeatTimer += GetFrameTime();
                    const float INITIAL_DELAY = 0.30f;
                    const float REPEAT_INTERVAL = 0.08f;
                    if (keyRepeatTimer >= INITIAL_DELAY) {
                        triggerAction = true;
                        keyRepeatTimer -= REPEAT_INTERVAL;
                    }
                }
            } else {
                keyRepeatTimer = 0.0f;
            }

            int activeTrack = (currentScreen == SCREEN_SEQ_5_8) ? cursorTrack + 4 : cursorTrack;
            Step& step = tracks[activeTrack].steps[cursorStep];
            bool isAltHeld = IsKeyDown(KEY_X);

            // Sequencer Microtiming Utilities
            bool isSequencerPage = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8);
            if (isShiftDown && isAltHeld && isSequencerPage) {
                bool editStepUtil = false;
                int editDirection = 0;
                if (encoderTurn != 0) {
                    editStepUtil = true;
                    editDirection = (encoderTurn > 0) ? 1 : -1;
                } else {
                    if (IsKeyPressed(KEY_LEFT))  { editStepUtil = true; editDirection = -1; }
                    if (IsKeyPressed(KEY_RIGHT)) { editStepUtil = true; editDirection = 1; }
                }

                if (IsKeyPressed(KEY_UP)) {
                    stepUtilFocus--;
                    if (stepUtilFocus < 0) stepUtilFocus = 2;
                }
                if (IsKeyPressed(KEY_DOWN)) {
                    stepUtilFocus++;
                    if (stepUtilFocus > 2) stepUtilFocus = 0;
                }

                if (editStepUtil) {
                    if (stepUtilFocus == 0) {
                        step.microtiming = std::clamp(step.microtiming + editDirection, -6, 6);
                    }
                    else if (stepUtilFocus == 1) {
                        tracks[selectedTrack].stepLength = std::clamp(tracks[selectedTrack].stepLength + editDirection, 1, 32);
                    }
                    else if (stepUtilFocus == 2) {
                        if (editDirection > 0) {
                            if (masterLength == 0)       masterLength = 16;
                            else if (masterLength == 16) masterLength = 32;
                            else if (masterLength == 32) masterLength = 64;
                            else if (masterLength == 64) masterLength = 0;
                        } else {
                            if (masterLength == 0)       masterLength = 64;
                            else if (masterLength == 64) masterLength = 32;
                            else if (masterLength == 32) masterLength = 16;
                            else if (masterLength == 16) masterLength = 0;
                        }
                    }
                }
            }

            // Locate transposition triggers (Shift + Up/Down or Encoder Turn) inside main's loop:
                        if ((triggerAction || (encoderTurn != 0)) && !isAltHeld && (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8)) {
                            if (encoderTurn != 0 && !encoderButton) {
                                int8_t curNote = step.note;
                                if (curNote == -1) {
                                    step.note = NoteToMidi("C" + std::to_string(currentOctave));
                                    step.velocity = 3;
                                } else {
                                    // Transpose directly using standard integers!
                                    int transposed = curNote + encoderTurn;
                                    step.note = (int8_t)std::clamp(transposed, 0, 127);
                                }
                            } else {
                                if (IsKeyDown(KEY_UP)) {
                                    int8_t curNote = step.note;
                                    if (curNote == -1) {
                                        step.note = NoteToMidi("C" + std::to_string(currentOctave));
                                        step.velocity = 3;
                                    } else {
                                        step.note = (int8_t)std::clamp(curNote + 1, 0, 127);
                                    }
                                }
                                if (IsKeyDown(KEY_DOWN)) {
                                    int8_t curNote = step.note;
                                    if (curNote == -1) {
                                        step.note = NoteToMidi("C" + std::to_string(currentOctave));
                                        step.velocity = 3;
                                    } else {
                                        step.note = (int8_t)std::clamp(curNote - 1, 0, 127);
                                    }
                                }
                            }
                            
                            int velocityChange = 0;
                            if (encoderTurn != 0 && encoderButton) {
                                velocityChange = (encoderTurn > 0) ? 1 : -1;
                            } else if (IsKeyDown(KEY_RIGHT)) {
                                velocityChange = 1;
                            } else if (IsKeyDown(KEY_LEFT)) {
                                velocityChange = -1;
                            }

                            if (velocityChange != 0) {
                                int v = step.velocity + velocityChange;
                                if (v > 3) v = 3;
                                if (v < 0) v = 0;
                                step.velocity = v;
                                if (v > 0 && step.note == -1) {
                                    step.note = NoteToMidi("C" + std::to_string(currentOctave));
                                }
                            }
                        }
        }

        // --- MANAGE GRID PARAMETER CONTROLS ---
        HandleParameterEditingInput(encoderTurn, encoderButton, isShiftDown, GetFrameTime());

        // --- NAVIGATION INTERCEPT FOR SCREENS (Ctrl + Keys) ---
        if (isCtrlDown) {
            if (IsKeyPressed(KEY_RIGHT)) {
                if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) currentScreen = SCREEN_SYNTH;
                else if (currentScreen == SCREEN_SYNTH) currentScreen = SCREEN_TRACK_PARAMS;
                else if (currentScreen == SCREEN_TRACK_PARAMS) currentScreen = SCREEN_PLACEHOLDER;
                else if (currentScreen == SCREEN_PLACEHOLDER) currentScreen = SCREEN_GLOBAL_FX;
            }
            if (IsKeyPressed(KEY_LEFT)) {
                if (currentScreen == SCREEN_GLOBAL_FX) currentScreen = SCREEN_PLACEHOLDER;
                else if (currentScreen == SCREEN_PLACEHOLDER) currentScreen = SCREEN_TRACK_PARAMS;
                else if (currentScreen == SCREEN_TRACK_PARAMS) currentScreen = SCREEN_SYNTH;
                else if (currentScreen == SCREEN_SYNTH) {
                    if (selectedTrack >= 4) currentScreen = SCREEN_SEQ_5_8;
                    else currentScreen = SCREEN_SEQ_1_4;
                }
            }
            if (IsKeyPressed(KEY_DOWN)) {
                if (currentScreen == SCREEN_SEQ_1_4) {
                    currentScreen = SCREEN_SEQ_5_8;
                    activeScreenRow = 1;
                } else if (currentScreen == SCREEN_SYNTH || currentScreen == SCREEN_TRACK_PARAMS || currentScreen == SCREEN_PLACEHOLDER) {
                    selectedTrack = (selectedTrack + 1) % 8;
                    cursorTrack = selectedTrack % 4;
                }
            }
            if (IsKeyPressed(KEY_UP)) {
                if (currentScreen == SCREEN_SEQ_5_8) {
                    currentScreen = SCREEN_SEQ_1_4;
                    activeScreenRow = 0;
                } else if (currentScreen == SCREEN_SYNTH || currentScreen == SCREEN_TRACK_PARAMS || currentScreen == SCREEN_PLACEHOLDER) {
                    selectedTrack = (selectedTrack - 1 + 8) % 8;
                    cursorTrack = selectedTrack % 4;
                }
            }
        }
        else if (!isShiftDown) {
            bool anyNavKeyDown = IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) ||
                                 IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT);
            bool triggerNav = false;

            if (anyNavKeyDown) {
                if (navRepeatTimer == 0.0f) {
                    triggerNav = true;
                    navRepeatTimer += GetFrameTime();
                } else {
                    navRepeatTimer += GetFrameTime();
                    const float INITIAL_DELAY = 0.25f;
                    const float REPEAT_INTERVAL = 0.06f;
                    if (navRepeatTimer >= INITIAL_DELAY) {
                        triggerNav = true;
                        navRepeatTimer -= REPEAT_INTERVAL;
                    }
                }
            } else {
                navRepeatTimer = 0.0f;
            }

            if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) {
                if (triggerNav) {
                    if (IsKeyDown(KEY_UP))    cursorTrack = (cursorTrack - 1 + 4) % 4;
                    if (IsKeyDown(KEY_DOWN))  cursorTrack = (cursorTrack + 1) % 4;
                    
                    if (IsKeyDown(KEY_LEFT)) {
                        cursorStep--;
                        if (cursorStep < 0) cursorStep = 31;
                        activePage = cursorStep / 16;
                    }
                    if (IsKeyDown(KEY_RIGHT)) {
                        cursorStep++;
                        if (cursorStep > 31) cursorStep = 0;
                        activePage = cursorStep / 16;
                    }
                }

                if (currentScreen == SCREEN_SEQ_1_4) {
                    selectedTrack = cursorTrack;
                } else if (currentScreen == SCREEN_SEQ_5_8) {
                    selectedTrack = cursorTrack + 4;
                }
            }
            else if (currentScreen == SCREEN_SYNTH) {
                if (triggerNav) {
                    bool isAltHeld = IsKeyDown(KEY_X);
                    if (isAltHeld) {
                        if (IsKeyDown(KEY_LEFT))  cursorStep = (cursorStep - 1 + 16) % 16;
                        if (IsKeyDown(KEY_RIGHT)) cursorStep = (cursorStep + 1) % 16;
                    } else {
                        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN)) {
                            synthGridRow = (synthGridRow == 1) ? 2 : 1;
                            int maxCol = 11;
                            if (tracks[selectedTrack].engineType == ENGINE_SAMPLER) {
                                if (synthGridRow == 1) maxCol = 12;
                                else if (synthGridRow == 2) maxCol = 8;
                            }
                            if (synthGridCol > maxCol) {
                                synthGridCol = maxCol;
                            }
                        }
                        
                        int maxCol = 11;
                        if (tracks[selectedTrack].engineType == ENGINE_SAMPLER) {
                            if (synthGridRow == 1)      maxCol = 12;
                            else if (synthGridRow == 2) maxCol = 8;
                        }

                        if (synthGridRow > 0) {
                            if (IsKeyDown(KEY_LEFT)) {
                                synthGridCol--;
                                if (synthGridCol < 0) synthGridCol = maxCol;
                            }
                            if (IsKeyDown(KEY_RIGHT)) {
                                synthGridCol++;
                                if (synthGridCol > maxCol) synthGridCol = 0;
                            }
                        }
                    }
                }
            }
            else if (currentScreen == SCREEN_TRACK_PARAMS) {
                            if (IsActionKeyPressed()) {
                                if (synthGridRow == 2) {
                                    lfoPopupOpen = true;
                                    lfoPopupLfoIdx = 0;
                                    lfoPopupSlot = 0;
                                    lfoPopupField = 0;
                                } else if (synthGridRow == 4) {
                                    lfoPopupOpen = true;
                                    lfoPopupLfoIdx = 1;
                                    lfoPopupSlot = 0;
                                    lfoPopupField = 0;
                                }
                            }

                if (triggerNav) {
                    bool isAltHeld = IsKeyDown(KEY_X);
                    if (isAltHeld) {
                        if (IsKeyDown(KEY_LEFT))  cursorStep = (cursorStep - 1 + 16) % 16;
                        if (IsKeyDown(KEY_RIGHT)) cursorStep = (cursorStep + 1) % 16;
                    } else {
                        if (IsKeyDown(KEY_UP)) {
                            synthGridRow--;
                            if (synthGridRow < 1) synthGridRow = 4;
                        }
                        if (IsKeyDown(KEY_DOWN)) {
                            synthGridRow++;
                            if (synthGridRow > 4) synthGridRow = 1;
                        }
                        if (IsKeyDown(KEY_LEFT)) {
                            synthGridCol--;
                            int maxCol = (synthGridRow == 3) ? 9 : 8;
                            if (synthGridCol < 0) synthGridCol = maxCol;
                        }
                        if (IsKeyDown(KEY_RIGHT)) {
                            synthGridCol++;
                            int maxCol = (synthGridRow == 3) ? 9 : 8;
                            if (synthGridCol > maxCol) synthGridCol = 0;
                        }
                    }
                }
            }
            else if (currentScreen == SCREEN_PLACEHOLDER) {
                if (triggerNav) {
                    bool isAltHeld = IsKeyDown(KEY_X);
                    if (isAltHeld) {
                        if (IsKeyDown(KEY_LEFT))  cursorStep = (cursorStep - 1 + 16) % 16;
                        if (IsKeyDown(KEY_RIGHT)) cursorStep = (cursorStep + 1) % 16;
                    } else {
                        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN)) {
                            synthGridRow = (synthGridRow == 1) ? 3 : 1;
                        }
                        if (IsKeyDown(KEY_LEFT)) {
                            synthGridCol--;
                            if (synthGridCol < 0) synthGridCol = 8;
                        }
                        if (IsKeyDown(KEY_RIGHT)) {
                            synthGridCol++;
                            if (synthGridCol > 8) synthGridCol = 0;
                        }
                    }
                }
            }
            else if (currentScreen == SCREEN_GLOBAL_FX) {
                if (triggerNav) {
                    bool isAltHeld = IsKeyDown(KEY_X);
                    if (isAltHeld) {
                        if (IsKeyDown(KEY_LEFT))  cursorStep = (cursorStep - 1 + 16) % 16;
                        if (IsKeyDown(KEY_RIGHT)) cursorStep = (cursorStep + 1) % 16;
                    } else {
                        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN)) {
                            synthGridRow = (synthGridRow == 1) ? 2 : 1;
                        }
                        if (IsKeyDown(KEY_LEFT)) {
                            synthGridCol--;
                            if (synthGridCol < 0) synthGridCol = 7;
                        }
                        if (IsKeyDown(KEY_RIGHT)) {
                            synthGridCol++;
                            if (synthGridCol > 7) synthGridCol = 0;
                        }
                    }
                }
            }
        }

        // --- DRAW VIRTUAL OLED FRAMEBUFFER ---
        CpuClearBackground(BLACK);
        CpuDrawLine(0, 7, OLED_WIDTH, 7, WHITE);

        UIState state = {
            currentScreen, selectedTrack, cursorTrack, cursorStep,
            currentOctave, tempo, isPlaying, playhead,
            synthGridRow, synthGridCol, trackParamsGridCol,
            blinkOn, activeNotesString,
            systemMenuOpen, systemMenuCursor,
            menuFeedback,
            systemMenuState, fileBrowserCursor, g_typingBuffer.c_str(), g_typingCursor
        };

        if (showDiagnostics) {
            DrawDiagnosticsScreen(state);
        } else {
            if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) DrawSequencerScreen(state);
            else if (currentScreen == SCREEN_SYNTH) DrawSynthScreen(state);
            else if (currentScreen == SCREEN_TRACK_PARAMS) DrawFilterLfoPage(state);
            else if (currentScreen == SCREEN_PLACEHOLDER) DrawPlaceholderPage(state);
            else if (currentScreen == SCREEN_GLOBAL_FX) DrawGlobalFXPage(state);
        }

        if (lfoPopupOpen) DrawModulationPopup(state);
        if (stepPopupOpen) DrawStepPopup(state);

        UpdateTexture(oledScreen.texture, g_oledCPUPixels);
        UpdateOled(oledScreen);

        // --- DESKTOP RAYLIB RENDERING STAGE ---
        BeginDrawing();
            ClearBackground(DARKGRAY);
            
            Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
            Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
            Vector2 origin = { 0.0f, 0.0f };
            DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
            
            if (lfoPopupOpen) DrawModulationPopup(state);
            if (stepPopupOpen) DrawStepPopup(state);
        EndDrawing();
    }

    ShutdownAudioEngine();
    ShutdownMidi();
    ShutdownOled();
    UnloadRenderTexture(oledScreen);
    CloseWindow();

#if defined(__linux__)
    g_encoderThreadRunning = false;
    if (encoderThread.joinable()) {
        encoderThread.join();
    }
#endif

    return 0;
}
