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
    // 1. Programmatically force physical pull-ups on BCM 17, 22, and 27 on startup
    std::system("sudo pinctrl set 17,22,27 ip pu");
    usleep(10000);

    // 2. Open gpiomem device
    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (fd < 0) {
        std::cerr << "[ENCODER] Failed to open /dev/gpiomem. Thread aborting." << std::endl;
        return;
    }

    // Map 4KB of physical GPIO registers directly into our process RAM
    volatile uint32_t* gpio = (volatile uint32_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (gpio == MAP_FAILED) {
        std::cerr << "[ENCODER] Memory mapping /dev/gpiomem failed." << std::endl;
        return;
    }

    // GPLEV0 is the register index holding the level of pins 0 to 31
    const int GPLEV0 = 13;

    // Read the initial atomic states of all pins at once
    uint32_t levels = gpio[GPLEV0];
    bool lastCLK = (levels & (1 << 17)) != 0; // Read BCM 17
    
    auto lastTurnTime = std::chrono::steady_clock::now();

    while (g_encoderThreadRunning) {
        // Read the exact microsecond level of all 32 GPIO pins in a single CPU cycle
        uint32_t levels = gpio[GPLEV0];

        bool clkVal = (levels & (1 << 17)) != 0; // BCM 17 (CLK / Pin 11)
        bool dtVal  = (levels & (1 << 27)) != 0; // BCM 27 (DT / Pin 13)
        bool swVal  = (levels & (1 << 22)) == 0; // BCM 22 (SW / Pin 15), LOW (0) means pressed

        g_encoderButtonState.store(swVal);

        // Falling-edge detection on CLK (Transitions from High to Low)
        if (lastCLK && !clkVal) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTurnTime).count();

            if (elapsed > 20) { // 20ms software debounce lockout
                lastTurnTime = now;
                
                // If DT is High when CLK falls: Clockwise. Otherwise Counter-Clockwise
                if (dtVal) {
                    g_encoderTurnQueue.fetch_add(1);  // Clockwise (positive increment)
                } else {
                    g_encoderTurnQueue.fetch_sub(1);  // Counter-Clockwise (negative decrement)
                }
            }
        }
        lastCLK = clkVal;

        // Poll at 1000Hz (1ms) with practically 0% CPU overhead
        usleep(1000);
    }

    // Clean up memory mapping on exit
    munmap((void*)gpio, 4096);
}
#endif

int main() {
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "8-Track Sequencer - Premium UI");
    SetTargetFPS(60);

    RenderTexture2D oledScreen = LoadRenderTexture(OLED_WIDTH, OLED_HEIGHT);
    SetTextureFilter(oledScreen.texture, TEXTURE_FILTER_POINT);

    InitializeTracks();
    InitAudioEngine();
    InitMidi(); // Spin up the RtMidi background ports
    InitOled();
    
#if defined(__linux__)
    std::thread encoderThread(runEncoderThread);
#endif
    
    // --- STARTUP BOOT ANIMATION STAGE ---
        bool playBootAnimation = true;
        int bootFrame = 0;

        while (playBootAnimation && !WindowShouldClose()) {
            // Force a stable, locked 12 FPS time-step by sleeping (83ms per frame)
            usleep(83000);
            
            bootFrame++;
            if (bootFrame >= BOOT_FRAME_COUNT) {
                playBootAnimation = false; // Transition cleanly to sequencer
            }

            // Draw current animation frame to the CPU framebuffer
                        CpuClearBackground(BLACK);
                        
            // Render the active frame
                                    if (bootFrame < BOOT_FRAME_COUNT) {
                                        for (int r = 0; r < BOOT_ROWS; ++r) {
                                            for (int c = 0; c < BOOT_COLS; ++c) {
                                                if (bootAnimationData[bootFrame][r][c] != 0) {
                                                    CpuDrawPixel(c, r, WHITE); // Standard, no subtraction!
                                                }
                                            }
                                        }
                                    }

                        // Sync the CPU buffer to Raylib's GPU texture for simulated window
                        UpdateTexture(oledScreen.texture, g_oledCPUPixels);
                        UpdateOled(oledScreen);

            // Render scaled up virtual texture to the physical window
                            BeginDrawing();
                                ClearBackground(DARKGRAY);
                                
                                // FIXED: Using positive height is correct for standard uploaded textures
                                Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
                                Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                                Vector2 origin = { 0.0f, 0.0f };
                                DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
                            EndDrawing();
        }
    
    int uiFrameCounter = 0;
    
    // Active Parameter Grid selectors
    int synthGridRow = 1;
    int synthGridCol = 0;
    int trackParamsGridCol = 0;

    int cursorTrack = 0;

    int currentOctave = 4;
    
    // Time-based key repeat timers
    float keyRepeatTimer = 0.0f;
    float navRepeatTimer = 0.0f;
    float tempoRepeatTimer = 0.0f;
    float menuRepeatTimer = 0.0f;
    
    int activeScreenRow = 0;

    // Struct Step clipboard
    Step copiedStep;
    bool hasCopiedStep = false;

    // Sample-accurate clock states
    double sampleRate = 44100.0;
    double simulatedSampleAccumulator = 0.0;

    while (!WindowShouldClose()) {
        uiFrameCounter++;
        bool blinkOn = (uiFrameCounter % 30 < 15);

        // Detect Modifier Keys
                bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                                  IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);

        // Pull hardware rotary encoder events
                int encoderTurn = 0;
                bool encoderButton = false;
                #if defined(__linux__)
                encoderTurn = g_encoderTurnQueue.exchange(0);
                encoderButton = g_encoderButtonState.load();
                g_hardwareEncoderClicked = encoderButton; // Update the global drawing flag
                #else
                g_hardwareEncoderClicked = false;
                #endif

                // Shift is simulated on any encoder turn so parameter values can change
                bool isShiftDown = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) || (encoderTurn != 0);

                // RESTORED: Keep Alt/X strictly mapped to keyboard 'X' key to prevent popups on Page 1 clicks
                bool isAltDown = IsKeyDown(KEY_X);
                bool isAltHeld = IsKeyDown(KEY_X);
        
        // --- SAFE SYSTEM SHUTDOWN HOTKEY (Ctrl + Alt + §) ---
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

        // --- DIAGNOSTICS KEYBOARD INTERCEPT OVERLAY ---
        if (showDiagnostics) {
            // Close the page on standard exit keys
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                showDiagnostics = false;
                menuFeedback = "DIAGNOSTICS CLOSED";
            }

            // Toggle logic shortcut is preserved
            if (isCtrlDown && isShiftDown && IsKeyPressed(KEY_NINE)) {
                showDiagnostics = false;
                menuFeedback = "DIAGNOSTICS CLOSED";
            }

            // Left/Right Arrow keys cycle the active MIDI hardware input port
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

            // Render and draw only the diagnostics frame, completely bypassing all other input checks
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

            // Render scaled up virtual texture to the physical window
                            BeginDrawing();
                                ClearBackground(DARKGRAY);
                                // Fixed: Height is now positive to match the main loop
                                Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
                                Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                                Vector2 origin = { 0.0f, 0.0f };
                                DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
                            EndDrawing();

            continue; // Force jump directly to next sample/draw frame
        }

        // --- MODAL LFO MATRIX INTERCEPT (Bypasses rest of main controls while active) ---
        if (lfoPopupOpen) {
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                lfoPopupOpen = false;
            }

            // Navigation is active ONLY when Shift is not held (avoids colliding with value edits)
            if (!isShiftDown) {
                // Up/Down navigates rows (slots 1 to 3)
                if (IsKeyPressed(KEY_UP)) {
                    lfoPopupSlot--;
                    if (lfoPopupSlot < 0) lfoPopupSlot = 2;
                }
                if (IsKeyPressed(KEY_DOWN)) {
                    lfoPopupSlot++;
                    if (lfoPopupSlot > 2) lfoPopupSlot = 0;
                }

                // Left/Right navigates columns (fields 0 to 3)
                if (IsKeyPressed(KEY_LEFT)) {
                    lfoPopupField--;
                    if (lfoPopupField < 0) lfoPopupField = 3;
                }
                if (IsKeyPressed(KEY_RIGHT)) {
                    lfoPopupField++;
                    if (lfoPopupField > 3) lfoPopupField = 0;
                }
            }

            // Base trigger states for non-shift keys (single press)
            bool triggerIncBy10 = false;
            bool triggerDecBy10 = false;
            bool triggerIncBy1  = IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD);
            bool triggerDecBy1  = IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT);

            // Dedicated LFO Popup continuous key-repeat timer
            static float lfoEditRepeatTimer = 0.0f;
            bool anyLfoEditHeld = isShiftDown && (IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_RIGHT));

            if (anyLfoEditHeld) {
                bool actionThisFrame = false;
                if (lfoEditRepeatTimer == 0.0f) {
                    actionThisFrame = true; // First frame fires instantly
                    lfoEditRepeatTimer += GetFrameTime();
                } else {
                    lfoEditRepeatTimer += GetFrameTime();
                    const float INITIAL_DELAY = 0.250f;   // 250ms hold delay before continuous repeat kicks in
                    const float REPEAT_INTERVAL = 0.050f; // 50ms repeat rate (20 edits per second)
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
                lfoEditRepeatTimer = 0.0f; // Reset timer on release
            }

            bool changeValue = triggerIncBy10 || triggerDecBy10 || triggerIncBy1 || triggerDecBy1;

            if (changeValue) {
                Track& trk = tracks[selectedTrack];
                ModSlot& m = (lfoPopupLfoIdx == 0) ? trk.lfo1Slots[lfoPopupSlot] : trk.lfo2Slots[lfoPopupSlot];

                // Resolve edit delta based on key combinations
                int delta = 0;
                if (triggerIncBy10)      delta = 10;
                else if (triggerDecBy10) delta = -10;
                else if (triggerIncBy1)  delta = 1;
                else if (triggerDecBy1)  delta = -1;

                if (lfoPopupField == 0) { // Toggle Destination Type: Off, Voice, Global FX
                    // For discrete lists, we extract the edit direction (+1 or -1)
                    int step = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
                    if (step != 0) {
                        m.destType += step;
                        if (m.destType < 0) m.destType = 2;
                        if (m.destType > 2) m.destType = 0;
                        
                        // Sanitize parameter default selection when target type changes
                        if (m.destType == 1) {
                            m.destParam = DEST_CUTOFF;
                            if (m.depth == 0) m.depth = 40; // Default initialization to prevent silent routing
                        }
                        else if (m.destType == 2) {
                            m.destParam = DEST_REV_MIX;
                            if (m.depth == 0) m.depth = 40; // Default initialization to prevent silent routing
                        }
                        else {
                            m.destParam = DEST_NONE;
                        }
                    }
                }
                else if (lfoPopupField == 1 && m.destType == 1) { // Select Target Track Index (0 to 7)
                    int step = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
                    if (step != 0) {
                        m.destTrack += step;
                        if (m.destTrack < 0) m.destTrack = 7;
                        if (m.destTrack > 7) m.destTrack = 0;
                    }
                }
                else if (lfoPopupField == 2 && m.destType != 0) { // Select Parameter Destination
                    int step = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
                    if (step != 0) {
                        if (m.destType == 1) { // Track Target
                            m.destParam += step;
                            if (m.destParam < DEST_CUTOFF) m.destParam = DEST_TAPE_MIX;
                            if (m.destParam > DEST_TAPE_MIX)  m.destParam = DEST_CUTOFF;
                        } else if (m.destType == 2) { // Global FX
                            m.destParam += step;
                            if (m.destParam < DEST_REV_MIX) m.destParam = DEST_PAN_MIX;
                            if (m.destParam > DEST_PAN_MIX)  m.destParam = DEST_REV_MIX;
                        }
                    }
                }
                else if (lfoPopupField == 3 && m.destType != 0) { // Adjust Depth
                    m.depth = std::clamp(m.depth + delta, -99, 99);
                }
            }

            // Bypass all standard update processing and jump directly to draw cycle
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
            UpdateOled(oledScreen);
            
            BeginDrawing();
                ClearBackground(DARKGRAY);
                Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, -(float)oledScreen.texture.height };
                Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                Vector2 origin = { 0.0f, 0.0f };
                DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
            EndDrawing();

            continue;
        }

        // --- GLOBAL LIVE PLAYABLE KEYS CONTROLLER ---
        if (liveKeyboardActive && !isCtrlDown) {
            for (int i = 0; i < NUM_NOTES; ++i) {
                std::string noteName = keyboardPiano[i].noteName;
                int octaveToUse = currentOctave;
                if (noteName == "C+") {
                    noteName = "C";
                    octaveToUse = currentOctave + 1;
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

        // --- GLOBAL MENU TOGGLE INTERCEPT ---
        if (IsKeyPressed(KEY_TAB)) {
            systemMenuOpen = !systemMenuOpen;
            systemMenuState = 0;
        }

        // --- LIVE KEYBOARD TOGGLE INTERCEPT (Cmd/Ctrl + 0 Key) ---
                if (isCtrlDown && IsKeyPressed(KEY_ZERO)) {
                    liveKeyboardActive = !liveKeyboardActive;
                    menuFeedback = liveKeyboardActive ? "LIVE KEYBOARD ENABLED" : "LIVE KEYBOARD DISABLED";
                }

                // --- 9 & 0 KEYS: SEQUENCER PAGE TOGGLING ---
                if (!isShiftDown && !isCtrlDown) {
                    if (IsKeyPressed(KEY_NINE)) {
                        activePage = 0;
                        if (cursorStep >= 16) cursorStep -= 16;
                        menuFeedback = "VIEWING: PAGE 1 (STEPS 1-16)";
                    }
                    if (IsKeyPressed(KEY_ZERO)) {
                        activePage = 1;
                        if (cursorStep < 16) cursorStep += 16;
                        menuFeedback = "VIEWING: PAGE 2 (STEPS 17-32)";
                    }
                }
        // --- ADD THIS: TOGGLE SEQUENCER / TRIGGERS PAGE (Z Key) ---
        if (IsKeyPressed(KEY_Z)) {
            if (currentScreen == SCREEN_SEQ_1_4) {
                currentScreen = SCREEN_TRIG_1_4;
                menuFeedback = "TRIGGERS VIEW (1-4)";
            } else if (currentScreen == SCREEN_TRIG_1_4) {
                currentScreen = SCREEN_SEQ_1_4;
                menuFeedback = "NOTES VIEW (1-4)";
            } else if (currentScreen == SCREEN_SEQ_5_8) {
                currentScreen = SCREEN_TRIG_5_8;
                menuFeedback = "TRIGGERS VIEW (5-8)";
            } else if (currentScreen == SCREEN_TRIG_5_8) {
                currentScreen = SCREEN_SEQ_5_8;
                menuFeedback = "NOTES VIEW (5-8)";
            }
        }

        // --- TOGGLE DIAGNOSTICS DASHBOARD (Cmd + Shift + 9) ---
        if (isCtrlDown && isShiftDown && IsKeyPressed(KEY_NINE)) {
            showDiagnostics = !showDiagnostics;
            menuFeedback = showDiagnostics ? "DIAGNOSTICS OPEN" : "DIAGNOSTICS CLOSED";
        }

        // --- TOGGLE CLOCK SOURCE (Cmd + Shift + C) ---
        if (isCtrlDown && isShiftDown && IsKeyPressed(KEY_C)) {
            g_useExternalMidiClock = !g_useExternalMidiClock;
            menuFeedback = g_useExternalMidiClock ? "SYNC: EXTERNAL (MIDI)" : "SYNC: INTERNAL (AUTO)";
        }

        if (systemMenuOpen) {
                    // Calculate rapid key-repeat scrolling direction
                    int menuDir = 0; // -1 for UP, 1 for DOWN, 0 for idle
                    bool triggerMenuNav = false;

                    bool isUpDownHeld = IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN);
                    if (isUpDownHeld) {
                        if (menuRepeatTimer == 0.0f) {
                            triggerMenuNav = true;
                            menuRepeatTimer += GetFrameTime();
                        } else {
                            menuRepeatTimer += GetFrameTime();
                            const float INITIAL_DELAY = 0.250f;   // 250ms hold delay before continuous repeat
                            const float REPEAT_INTERVAL = 0.060f; // 60ms repeat speed (16 files per second)
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
                        menuRepeatTimer = 0.0f; // Reset the timer when keys are released
                    }

            if (systemMenuState == 0) {
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
                if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
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
                            if (!g_fileList.empty()) {
                                if (menuDir == -1) {
                                    fileBrowserCursor--;
                                    if (fileBrowserCursor < 0) fileBrowserCursor = (int)g_fileList.size() - 1;
                                }
                                if (menuDir == 1) {
                                    fileBrowserCursor++;
                                    if (fileBrowserCursor >= (int)g_fileList.size()) fileBrowserCursor = 0;
                                }
                    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
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
                            int maxSlots = (systemMenuCursor == 0 || systemMenuCursor == 6) ? 16 : ((systemMenuCursor == 2) ? 128 : 1024);
                            
                            if (menuDir == -1) {
                                fileBrowserCursor--;
                                if (fileBrowserCursor < 0) fileBrowserCursor = maxSlots - 1;
                            }
                            if (menuDir == 1) {
                                fileBrowserCursor++;
                                if (fileBrowserCursor >= maxSlots) fileBrowserCursor = 0;
                            }
                if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
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
                        g_typingBuffer = "";
                        g_typingCursor = 0;
                    }
                }
                if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE)) {
                    systemMenuState = 0;
                }
            }
            else if (systemMenuState == 3) {
                int key = GetCharPressed();
                while (key > 0) {
                    key = toupper(key);
                    bool isSafeChar = (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
                                      (key == '_') || (key == '-');
                    if (isSafeChar && g_typingBuffer.length() < 15) {
                        g_typingBuffer.insert(g_typingCursor, 1, (char)key);
                        g_typingCursor++;
                    }
                    key = GetCharPressed();
                }

                if (IsKeyPressed(KEY_BACKSPACE)) {
                    if (g_typingCursor > 0 && !g_typingBuffer.empty()) {
                        g_typingBuffer.erase(g_typingCursor - 1, 1);
                        g_typingCursor--;
                    }
                }

                if (IsKeyPressed(KEY_LEFT)) {
                    g_typingCursor--;
                    if (g_typingCursor < 0) g_typingCursor = 0;
                }
                if (IsKeyPressed(KEY_RIGHT)) {
                    g_typingCursor++;
                    if (g_typingCursor > (int)g_typingBuffer.length()) g_typingCursor = (int)g_typingBuffer.length();
                }

                if (IsKeyPressed(KEY_ESCAPE)) {
                    systemMenuState = 2;
                }

                if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                    if (!g_typingBuffer.empty()) {
                        bool success = false;
                        int activeSlotNum = fileBrowserCursor + 1;
                        
                        if (systemMenuCursor == 0)      success = SaveProject(activeSlotNum, g_typingBuffer);
                        else if (systemMenuCursor == 2) success = SavePattern(activePattern, activeSlotNum, g_typingBuffer);
                        else if (systemMenuCursor == 4) success = SaveSoundPreset(selectedTrack, activeSlotNum, g_typingBuffer);

                        if (success) menuFeedback = "SAVED SUCCESSFULLY!";
                        else         menuFeedback = "SAVE FAILED!";
                    }
                    systemMenuState = 0;
                }
            }
            
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

                        // --- RENDER CURRENT SCREEN ---
                        if (showDiagnostics) {
                            DrawDiagnosticsScreen(state);
                        } else {
                            if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) {
                                DrawSequencerScreen(state);
                            }
                            else if (currentScreen == SCREEN_TRIG_1_4 || currentScreen == SCREEN_TRIG_5_8) {
                                DrawTriggersScreen(state);
                            }
                            else if (currentScreen == SCREEN_SYNTH) {
                                DrawSynthScreen(state);
                            }
                            else if (currentScreen == SCREEN_TRACK_PARAMS) {
                                DrawFilterLfoPage(state);
                            }
                            else if (currentScreen == SCREEN_PLACEHOLDER) {
                                DrawPlaceholderPage(state);
                            }
                            else if (currentScreen == SCREEN_GLOBAL_FX) {
                                DrawGlobalFXPage(state);
                            }
                        }

                        // Draw modal LFO popup centered on top
                        if (lfoPopupOpen) {
                            DrawModulationPopup(state);
                        }
            // --- DRAW SYSTEM MENU OVERLAY (Add this back!) ---
                        DrawSystemMenu(state);
            
                        // Sync CPU framebuffer to Raylib GPU texture for simulated window
                        UpdateTexture(oledScreen.texture, g_oledCPUPixels);
                        UpdateOled(oledScreen);
            
            BeginDrawing();
                ClearBackground(DARKGRAY);
                Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, -(float)oledScreen.texture.height };
                Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                Vector2 origin = { 0.0f, 0.0f };
                DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
            EndDrawing();

            continue;
        }

        if (IsKeyPressed(KEY_SPACE)) {
            isPlaying = !isPlaying;
        }
        // --- SINGLE STEP DELETE (BACKSPACE BY ITSELF) ---
                if (IsKeyPressed(KEY_BACKSPACE) && !isShiftDown && !isCtrlDown) {
                    bool isSequencerPage = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8 ||
                                            currentScreen == SCREEN_TRIG_1_4 || currentScreen == SCREEN_TRIG_5_8);
                    if (isSequencerPage) {
                        Step& step = tracks[selectedTrack].steps[cursorStep];
                        step.note = "";
                        step.velocity = 0;
                        step.condition = "";
                        step.retrigger = 0;
                        step.microtiming = 0;
                        step.params.reset();
                        menuFeedback = "STEP CLEARED";
                    }
                }
        // --- GLOBAL KEY COMMAND PROCESSING ---
        if (isShiftDown && !isCtrlDown) {
            if (IsKeyPressed(KEY_E)) {
                if (tracks[selectedTrack].engineType == ENGINE_SYNTH) {
                    tracks[selectedTrack].engineType = ENGINE_SAMPLER;
                    tracks[selectedTrack].algorithm = ALGO_SAMPLE;
                } else {
                    tracks[selectedTrack].engineType = ENGINE_SYNTH;
                    tracks[selectedTrack].algorithm = ALGO_PARALLEL;
                }
            }

            if (IsKeyPressed(KEY_R)) {
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

            if (IsKeyPressed(KEY_M)) {
                tracks[selectedTrack].muted = !tracks[selectedTrack].muted;
            }

            if (IsKeyPressed(KEY_BACKSPACE)) {
                bool isAltHeld = IsKeyDown(KEY_X);
                if (isAltHeld) {
                    tracks[selectedTrack].steps[cursorStep].params.reset();
                    menuFeedback = "STEP LOCKS CLEARED";
                } else {
                    bool isSequencerPage = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8 ||
                                            currentScreen == SCREEN_TRIG_1_4 || currentScreen == SCREEN_TRIG_5_8);
                    
                    if (isSequencerPage) {
                        for (int s = 0; s < 16; ++s) {
                            Step& step = tracks[selectedTrack].steps[s];
                            step.note = "";
                            step.velocity = 0;
                            step.condition = "";
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

            int targetPattern = -1;
            if (IsKeyPressed(KEY_ONE))   targetPattern = 0;
            if (IsKeyPressed(KEY_TWO))   targetPattern = 1;
            if (IsKeyPressed(KEY_THREE)) targetPattern = 2;
            if (IsKeyPressed(KEY_FOUR))  targetPattern = 3;
            if (IsKeyPressed(KEY_FIVE))  targetPattern = 4;
            if (IsKeyPressed(KEY_SIX))   targetPattern = 5;
            if (IsKeyPressed(KEY_SEVEN)) targetPattern = 6;
            if (IsKeyPressed(KEY_EIGHT)) targetPattern = 7;

            if (targetPattern >= 0) {
                queuedPattern = targetPattern;
            }
        }

        // --- 1-8 KEYS: TRACK SELECT, MUTE, AND SOLO ---
        int numKeyTriggered = -1;
        if (IsKeyPressed(KEY_ONE))        numKeyTriggered = 0;
        else if (IsKeyPressed(KEY_TWO))   numKeyTriggered = 1;
        else if (IsKeyPressed(KEY_THREE)) numKeyTriggered = 2;
        else if (IsKeyPressed(KEY_FOUR))  numKeyTriggered = 3;
        else if (IsKeyPressed(KEY_FIVE))  numKeyTriggered = 4;
        else if (IsKeyPressed(KEY_SIX))   numKeyTriggered = 5;
        else if (IsKeyPressed(KEY_SEVEN)) numKeyTriggered = 6;
        else if (IsKeyPressed(KEY_EIGHT)) numKeyTriggered = 7;

        if (numKeyTriggered >= 0) {
            if (isShiftDown && isCtrlDown) {
                // --- SOLO TOGGLE (Cmd/Ctrl + Shift + [1-8]) ---
                bool isAnyOtherUnmuted = false;
                for (int t = 0; t < 8; ++t) {
                    if (t != numKeyTriggered && !tracks[t].muted) {
                        isAnyOtherUnmuted = true;
                        break;
                    }
                }
                if (isAnyOtherUnmuted) {
                    for (int t = 0; t < 8; ++t) {
                        tracks[t].muted = (t != numKeyTriggered);
                    }
                    menuFeedback = "TRACK SOLO ACTIVE";
                } else {
                    for (int t = 0; t < 8; ++t) {
                        tracks[t].muted = false;
                    }
                    menuFeedback = "ALL TRACKS UNMUTED";
                }
            }
            else if (isCtrlDown) {
                // --- MUTE TOGGLE (Cmd/Ctrl + [1-8]) ---
                // Changed from Shift to Ctrl to prevent the pattern-switch clash!
                tracks[numKeyTriggered].muted = !tracks[numKeyTriggered].muted;
                menuFeedback = tracks[numKeyTriggered].muted ? "TRACK MUTED" : "TRACK UNMUTED";
            }
            else if (isShiftDown) {
                // Left empty here on purpose!
                // This allows the Pattern Switch block (Shift + [1-8]) to handle it
                // without accidentally muting the track.
            }
            else {
                // --- SELECT ACTIVE TRACK ([1-8] Keys) ---
                selectedTrack = numKeyTriggered;
                cursorTrack = numKeyTriggered % 4;

                        if (numKeyTriggered < 4) {
                            if (currentScreen == SCREEN_SEQ_5_8) {
                                currentScreen = SCREEN_SEQ_1_4;
                                activeScreenRow = 0;
                            } else if (currentScreen == SCREEN_TRIG_5_8) {
                                currentScreen = SCREEN_TRIG_1_4;
                                activeScreenRow = 0;
                            }
                        } else {
                            if (currentScreen == SCREEN_SEQ_1_4) {
                                currentScreen = SCREEN_SEQ_5_8;
                                activeScreenRow = 1;
                            } else if (currentScreen == SCREEN_TRIG_1_4) {
                                currentScreen = SCREEN_TRIG_5_8;
                                activeScreenRow = 1;
                            }
                        }
                    }
                }

        if (IsKeyPressed(KEY_BACKSPACE) && isShiftDown && !isCtrlDown) {
                    bool isSequencerPage = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8 ||
                                            currentScreen == SCREEN_TRIG_1_4 || currentScreen == SCREEN_TRIG_5_8);
                    
                    int startStep = activePage * 16;

                    if (isSequencerPage) {
                        // Clear trigs on active page
                        for (int s = startStep; s < startStep + 16; ++s) {
                            Step& step = tracks[selectedTrack].steps[s];
                            step.note = "";
                            step.velocity = 0;
                            step.condition = "";
                            step.retrigger = 0;
                            step.microtiming = 0;
                            step.params.reset();
                        }
                        menuFeedback = (activePage == 0) ? "PAGE 1 TRIGS RESET" : "PAGE 2 TRIGS RESET";
                    } else {
                        // Clear step-lock parameter overrides on active page (Preserves sound presets!)
                        for (int s = startStep; s < startStep + 16; ++s) {
                            tracks[selectedTrack].steps[s].params.reset();
                        }
                        menuFeedback = (activePage == 0) ? "PAGE 1 PARAM LOCKS RESET" : "PAGE 2 PARAM LOCKS RESET";
                    }
                }

        if (!isCtrlDown) {
            if (IsKeyPressed(KEY_COMMA)) {
                currentOctave--;
                if (currentOctave < 0) currentOctave = 0;
            }
            if (IsKeyPressed(KEY_PERIOD)) {
                currentOctave++;
                if (currentOctave > 8) currentOctave = 8;
            }
        }

        if (isCtrlDown) {
            if (IsKeyPressed(KEY_C)) {
                int activeTrack = (currentScreen == SCREEN_SEQ_5_8 || currentScreen == SCREEN_TRIG_5_8) ? cursorTrack + 4 : cursorTrack;
                copiedStep = tracks[activeTrack].steps[cursorStep];
                hasCopiedStep = true;
            }
            if (IsKeyPressed(KEY_V)) {
                if (hasCopiedStep) {
                    int activeTrack = (currentScreen == SCREEN_SEQ_5_8 || currentScreen == SCREEN_TRIG_5_8) ? cursorTrack + 4 : cursorTrack;
                    tracks[activeTrack].steps[cursorStep] = copiedStep;
                }
            }
        }

        if (!isCtrlDown) {
            double deltaTempo = 0.0;
            bool triggerTempo = false;

            bool isTempoKeyDown = IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD) ||
                                  IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT);

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
                if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD)) deltaTempo = 1.0;
                if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)) deltaTempo = -1.0;

                if (isShiftDown) {
                    deltaTempo *= 10.0;
                }

                tempo += deltaTempo;
                if (tempo < 20.0)   tempo = 20.0;
                if (tempo > 300.0)  tempo = 300.0;
            }
        }

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
                    
                    tracks[targetTrack].steps[cursorStep].note = noteStr;
                    tracks[targetTrack].steps[cursorStep].velocity = 3;
                }
            }
        }

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

            int activeTrack = (currentScreen == SCREEN_SEQ_5_8 || currentScreen == SCREEN_TRIG_5_8) ? cursorTrack + 4 : cursorTrack;
                        Step& step = tracks[activeTrack].steps[cursorStep];

                        bool isAltHeld = IsKeyDown(KEY_X);

            // --- STEP & TIMING UTILITIES MENU EDITING ---
                        bool isSequencerPage = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8 ||
                                                currentScreen == SCREEN_TRIG_1_4 || currentScreen == SCREEN_TRIG_5_8);

                        if (isShiftDown && isAltHeld && isSequencerPage) { // CHANGED: Added screen guard
                                                    bool editStepUtil = false;
                                        int editDirection = 0;
                                        // EDIT: Allow encoder to edit values directly
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

            if ((triggerAction || (encoderTurn != 0)) && !isAltHeld && (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8)) {
                            // 1. If unclicked turn: Transpose Note up/down
                            if (encoderTurn != 0 && !encoderButton) {
                                std::string curNote = step.note;
                                if (curNote.empty()) {
                                    step.note = "C" + std::to_string(currentOctave);
                                    step.velocity = 3;
                                } else {
                                    // Transposes cleanly by the number of clicks turned
                                    step.note = TransposeNote(curNote, encoderTurn);
                                }
                            } else {
                                if (IsKeyDown(KEY_UP)) {
                                    std::string curNote = step.note;
                                    if (curNote.empty()) {
                                        step.note = "C" + std::to_string(currentOctave);
                                        step.velocity = 3;
                                    } else {
                                        step.note = TransposeNote(curNote, 1);
                                    }
                                }
                                if (IsKeyDown(KEY_DOWN)) {
                                    std::string curNote = step.note;
                                    if (curNote.empty()) {
                                        step.note = "C" + std::to_string(currentOctave);
                                        step.velocity = 3;
                                    } else {
                                        step.note = TransposeNote(curNote, -1);
                                    }
                                }
                            }
                            
                            // 2. If clicked turn or Keyboard Left/Right: Change Step Velocity
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
                                if (v > 0 && step.note.empty()) {
                                    step.note = "C" + std::to_string(currentOctave);
                                }
                            }
                        }
            if (currentScreen == SCREEN_TRIG_1_4 || currentScreen == SCREEN_TRIG_5_8) {
                            int changeCondition = 0;
                            // If unclicked turn: Change Step Condition
                            if (encoderTurn != 0 && !encoderButton) {
                                changeCondition = (encoderTurn > 0) ? 1 : -1;
                            } else if (triggerAction) {
                                if (IsKeyDown(KEY_UP))   changeCondition = 1;
                                if (IsKeyDown(KEY_DOWN)) changeCondition = -1;
                            }

                            if (changeCondition != 0) {
                                std::string curTrig = step.condition;
                                    
                                auto it = std::find(triggerOptions.begin(), triggerOptions.end(), curTrig);
                                int index = 0;
                                if (it != triggerOptions.end()) {
                                    index = std::distance(triggerOptions.begin(), it);
                                }
                                
                                index += changeCondition;
                                if (index < 0) index = triggerOptions.size() - 1;
                                if (index >= (int)triggerOptions.size()) index = 0;

                                step.condition = triggerOptions[index];
                            }

                            int changeRetrig = 0;
                            // If clicked turn: Change Step Retrigger/Ratchets (0 to 16)
                            if (encoderTurn != 0 && encoderButton) {
                                changeRetrig = (encoderTurn > 0) ? 1 : -1;
                            } else if (triggerAction) {
                                if (IsKeyDown(KEY_RIGHT)) changeRetrig = 1;
                                if (IsKeyDown(KEY_LEFT))  changeRetrig = -1;
                            }

                            if (changeRetrig != 0) {
                                int r = step.retrigger + changeRetrig;
                                if (r < 0) r = 0;
                                if (r > 16) r = 16;
                                step.retrigger = r;
                            }
                        }
            else if (currentScreen == SCREEN_SYNTH) {
                            int change = 0;
                            if (triggerAction || (encoderTurn != 0)) {
                                bool isVolumeCol = (tracks[selectedTrack].engineType == ENGINE_SYNTH && synthGridCol == 3) ||
                                                   (synthGridCol == 11) ||
                                                   (tracks[selectedTrack].engineType == ENGINE_SAMPLER && synthGridRow == 1 && synthGridCol == 12);
                                bool isFeedbackCol = (synthGridRow == 1 && synthGridCol == 10);
                                bool isSamplerToggleCol = (tracks[selectedTrack].engineType == ENGINE_SAMPLER) && (synthGridCol == 0);

                                if (encoderTurn != 0) {
                                    // Fine single-step edits for encoders on standard columns, slightly wider steps for volume
                                    if (isVolumeCol || isFeedbackCol) {
                                        change = encoderTurn * 5;
                                    } else {
                                        change = encoderTurn;
                                    }
                                } else if (isVolumeCol || isFeedbackCol) {
                                    if (IsKeyDown(KEY_RIGHT)) change = 5;
                                    if (IsKeyDown(KEY_LEFT))  change = -5;
                                } else if (isSamplerToggleCol) {
                                    if (IsKeyDown(KEY_RIGHT)) change = 1;
                                    if (IsKeyDown(KEY_LEFT))  change = -1;
                                } else {
                                    if (IsKeyDown(KEY_RIGHT)) change = 1;
                                    if (IsKeyDown(KEY_LEFT))  change = -1;
                                    if (IsKeyDown(KEY_UP))    change = 10;
                                    if (IsKeyDown(KEY_DOWN))  change = -10;
                                }
                            }

                            if (change != 0) {
                                Track& trk = tracks[selectedTrack];
                                StepParams& sp = trk.steps[cursorStep].params;
                                // EDIT: Allows both the keyboard 'X' key and the encoder click to act as a step-lock!
                                bool isStepLock = IsKeyDown(KEY_X) || encoderButton;

                    auto EditParam = [&](int& stepVal, int trackVal, int changeAmt, int minV, int maxV) {
                        int base = (stepVal == -1) ? trackVal : stepVal;
                        stepVal = std::clamp(base + changeAmt, minV, maxV);
                    };

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
                                        if (isStepLock) sp.filterType = (sp.filterType == -1) ? ((trk.filterType == ALGO_SAMPLE) ? ALGO_GRANULAR : ALGO_SAMPLE) : ((sp.filterType == ALGO_SAMPLE) ? ALGO_GRANULAR : ALGO_SAMPLE);
                                        else            trk.algorithm = (trk.algorithm == ALGO_SAMPLE) ? ALGO_GRANULAR : ALGO_SAMPLE;
                                    }
                                    else if (synthGridCol == 1) {
                                        if (isStepLock) EditParam(sp.sampleSlot, trk.sampleSlot, change, 0, 15);
                                        else            trk.sampleSlot = std::clamp(trk.sampleSlot + change, 0, 15);
                                    }
                                    else if (synthGridCol == 2) {
                                        if (trk.algorithm == ALGO_SAMPLE) {
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
                                    else if (synthGridCol == 3) { if (isStepLock) EditParam(sp.volume2, trk.volume2, change, 0, 99); else trk.volume2 = std::clamp(tracks[selectedTrack].volume2 + change, 0, 99); }
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
                    }
            else if (currentScreen == SCREEN_TRACK_PARAMS) {
                            int change = 0;
                            if (triggerAction || (encoderTurn != 0)) {
                                bool isToggleCol = (synthGridCol == 2 || (synthGridRow == 3 && synthGridCol == 4) || (synthGridRow == 1 && synthGridCol == 4) || synthGridCol == 5 || synthGridCol == 8 || synthGridCol == 9);

                                if (encoderTurn != 0) {
                                    change = encoderTurn;
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

                            if (change != 0) {
                                Track& trk = tracks[selectedTrack];
                                StepParams& sp = trk.steps[cursorStep].params;
                                bool isStepLock = IsKeyDown(KEY_X) || encoderButton;

                            auto EditParam = [&](int& stepVal, int trackVal, int changeAmt, int minV, int maxV) {
                                int base = (stepVal == -1) ? trackVal : stepVal;
                                stepVal = std::clamp(base + changeAmt, minV, maxV);
                            };

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
                                                                    if (newW < 0) newW = 5; if (newW > 5) newW = 0; // CHANGED: Allowed 5 (SEQ) [1]
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
                                else if (synthGridCol == 4) { // VOY (Voice limit)
                                    if (isStepLock) {
                                        int base = (sp.polyMode == -1) ? trk.polyMode : sp.polyMode;
                                        sp.polyMode = std::clamp(base + change, 1, 4);
                                    } else {
                                        trk.polyMode = std::clamp(trk.polyMode + change, 1, 4);
                                    }
                                }
                                else if (synthGridCol == 5) { // Shipped LFO 2 WAV
                                                                    int baseW2 = (isStepLock && sp.lfo2Wave != -1) ? sp.lfo2Wave : trk.lfo2Wave;
                                                                    int newW2 = baseW2 + change;
                                                                    if (newW2 < 0) newW2 = 5; if (newW2 > 5) newW2 = 0; // CHANGED: Allowed 5 (SEQ) [1]
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
                    }
            else if (currentScreen == SCREEN_PLACEHOLDER) {
                            int change = 0;
                            if (triggerAction || (encoderTurn != 0)) {
                                bool isToggleCol = (synthGridCol == 1 && synthGridRow == 1);
                                
                                if (encoderTurn != 0) {
                                    change = encoderTurn;
                                } else if (isToggleCol) {
                                    if (IsKeyDown(KEY_RIGHT)) change = 1;
                                    if (IsKeyDown(KEY_LEFT))  change = -1;
                                } else {
                                    if (IsKeyDown(KEY_RIGHT)) change = 1;
                                    if (IsKeyDown(KEY_LEFT))  change = -1;
                                    if (IsKeyDown(KEY_UP))    change = 10;
                                    if (IsKeyDown(KEY_DOWN))  change = -10;
                                }
                            }

                            if (change != 0) {
                                Track& trk = tracks[selectedTrack];
                                StepParams& sp = trk.steps[cursorStep].params;
                                bool isStepLock = IsKeyDown(KEY_X) || encoderButton;

                            auto EditParam = [&](int& stepVal, int trackVal, int changeAmt, int minV, int maxV) {
                                int base = (stepVal == -1) ? trackVal : stepVal;
                                stepVal = std::clamp(base + changeAmt, minV, maxV);
                            };

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
                    }
            else if (currentScreen == SCREEN_GLOBAL_FX) {
                                    int change = 0;
                                    if (triggerAction || (encoderTurn != 0)) {
                                        bool isToggleCol = (synthGridCol == 4 && synthGridRow == 2);
                                        
                                        if (encoderTurn != 0) {
                                            change = encoderTurn;
                                        } else if (isToggleCol) {
                                            if (IsKeyDown(KEY_RIGHT)) change = 1;
                                            if (IsKeyDown(KEY_LEFT))  change = -1;
                                        } else {
                                            if (IsKeyDown(KEY_RIGHT)) change = 1;
                                            if (IsKeyDown(KEY_LEFT))  change = -1;
                                            if (IsKeyDown(KEY_UP))    change = 10;
                                            if (IsKeyDown(KEY_DOWN))  change = -10;
                                        }
                                    }
                        if (change != 0) {
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
                }
                if (isCtrlDown) {
                    if (IsKeyPressed(KEY_RIGHT)) {
                        // Skips trigger pages; routes straight to Synth (Page 2) from Notes
                        if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_TRIG_1_4) {
                            currentScreen = SCREEN_SYNTH;
                        } else if (currentScreen == SCREEN_SEQ_5_8 || currentScreen == SCREEN_TRIG_5_8) {
                            currentScreen = SCREEN_SYNTH;
                        } else if (currentScreen == SCREEN_SYNTH) {
                            currentScreen = SCREEN_TRACK_PARAMS;
                        } else if (currentScreen == SCREEN_TRACK_PARAMS) {
                            currentScreen = SCREEN_PLACEHOLDER;
                        } else if (currentScreen == SCREEN_PLACEHOLDER) {
                            currentScreen = SCREEN_GLOBAL_FX;
                        }
                    }
                    if (IsKeyPressed(KEY_LEFT)) {
                        if (currentScreen == SCREEN_GLOBAL_FX) {
                            currentScreen = SCREEN_PLACEHOLDER;
                        } else if (currentScreen == SCREEN_PLACEHOLDER) {
                            currentScreen = SCREEN_TRACK_PARAMS;
                        } else if (currentScreen == SCREEN_TRACK_PARAMS) {
                            currentScreen = SCREEN_SYNTH;
                        } else if (currentScreen == SCREEN_SYNTH) {
                            // Skips trigger pages; routes straight back to active Sequencer bank (Page 1)
                            if (selectedTrack >= 4) {
                                currentScreen = SCREEN_SEQ_5_8;
                            } else {
                                currentScreen = SCREEN_SEQ_1_4;
                            }
                        } else if (currentScreen == SCREEN_TRIG_1_4) {
                            currentScreen = SCREEN_SEQ_1_4;
                        } else if (currentScreen == SCREEN_TRIG_5_8) {
                            currentScreen = SCREEN_SEQ_5_8;
                        }
                    }
                    if (IsKeyPressed(KEY_DOWN)) {
                        if (currentScreen == SCREEN_SEQ_1_4) {
                            currentScreen = SCREEN_SEQ_5_8;
                            activeScreenRow = 1;
                        } else if (currentScreen == SCREEN_TRIG_1_4) {
                            currentScreen = SCREEN_TRIG_5_8;
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
                        } else if (currentScreen == SCREEN_TRIG_5_8) {
                            currentScreen = SCREEN_TRIG_1_4;
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

                    if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8 || currentScreen == SCREEN_TRIG_1_4 || currentScreen == SCREEN_TRIG_5_8) {
                                            if (triggerNav) {
                                                if (IsKeyDown(KEY_UP))    cursorTrack = (cursorTrack - 1 + 4) % 4;
                                                if (IsKeyDown(KEY_DOWN))  cursorTrack = (cursorTrack + 1) % 4;
                                                
                                                if (IsKeyDown(KEY_LEFT)) {
                                                    cursorStep--;
                                                    if (cursorStep < 0) cursorStep = 31;
                                                    activePage = cursorStep / 16; // Automatically align visual page
                                                }
                                                if (IsKeyDown(KEY_RIGHT)) {
                                                    cursorStep++;
                                                    if (cursorStep > 31) cursorStep = 0;
                                                    activePage = cursorStep / 16; // Automatically align visual page
                                                }
                                            }

                        if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_TRIG_1_4) {
                            selectedTrack = cursorTrack;
                        } else if (currentScreen == SCREEN_SEQ_5_8 || currentScreen == SCREEN_TRIG_5_8) {
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
                        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
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
                            bool isStepLock = IsKeyDown(KEY_X) || encoderButton;
                            
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

        // --- DRAW TO VIRTUAL OLED ---
                        // We clear and draw to the CPU framebuffer directly to minimize Pi CPU overhead
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

                        // --- RENDER CURRENT SCREEN ---
                        if (showDiagnostics) {
                            DrawDiagnosticsScreen(state);
                        } else {
                            // FIXED: Corrected the typo where SCREEN_SEQ_1_4 was checked twice instead of checking SCREEN_SEQ_5_8
                            if (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8) {
                                DrawSequencerScreen(state);
                            }
                            else if (currentScreen == SCREEN_TRIG_1_4 || currentScreen == SCREEN_TRIG_5_8) {
                                DrawTriggersScreen(state);
                            }
                            else if (currentScreen == SCREEN_SYNTH) {
                                DrawSynthScreen(state);
                            }
                            else if (currentScreen == SCREEN_TRACK_PARAMS) {
                                DrawFilterLfoPage(state);
                            }
                            else if (currentScreen == SCREEN_PLACEHOLDER) {
                                DrawPlaceholderPage(state);
                            }
                            else if (currentScreen == SCREEN_GLOBAL_FX) {
                                DrawGlobalFXPage(state);
                            }
                        }

                        // Draw modal LFO popup centered on top
                        if (lfoPopupOpen) {
                            DrawModulationPopup(state);
                        }

                        // Sync CPU framebuffer to Raylib's GPU texture for simulated window
                        UpdateTexture(oledScreen.texture, g_oledCPUPixels);
                        UpdateOled(oledScreen);

        // --- RENDER SCALED-UP CANVAS TO WINDOW ---
                        BeginDrawing();
                            ClearBackground(DARKGRAY);

                            // FIXED: Using positive height is correct for standard uploaded textures
                            Rectangle sourceRec = { 0.0f, 0.0f, (float)oledScreen.texture.width, (float)oledScreen.texture.height };
                            Rectangle destRec = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT };
                            Vector2 origin = { 0.0f, 0.0f };

                            DrawTexturePro(oledScreen.texture, sourceRec, destRec, origin, 0.0f, WHITE);
                            
                        EndDrawing();
                    }

                    ShutdownAudioEngine();
                    ShutdownMidi(); // Safely unbind and delete RtMidi ports
                    ShutdownOled();
                    UnloadRenderTexture(oledScreen);
                    CloseWindow();
#if defined(__linux__)
    g_encoderThreadRunning = false;
    if (encoderThread.joinable()) {
        encoderThread.join();
    }
#endif

    UnloadRenderTexture(oledScreen);
    CloseWindow();
    return 0;
                }
