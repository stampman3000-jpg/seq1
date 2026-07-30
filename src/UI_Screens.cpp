#include "UI_Screens.hpp"
#include "UI_Draw.hpp"
#include "Globals.hpp"
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdio> // Required for snprintf
#include <cmath>
#include "Audio_Engine.hpp"
#include "Sampler_Engine.hpp"
#include "Midi_Manager.hpp"

// Helper function to build the active/queued pattern indicator string dynamically
static std::string GetPatternHeaderString() {
    std::string patStr = "P" + std::to_string(activePattern + 1);
    if (queuedPattern != -1) {
        patStr += "->P" + std::to_string(queuedPattern + 1); // e.g. "P1->P2"
    }
    return patStr;
}

static void DrawMicrotimingPopup(const UIState& state) {
    // Centered window, slightly scaled up
    int mx = 38, my = 4, mw = 180, mh = 56;

    // Draw high-contrast black backplane and borders
    DrawRectangle(mx, my, mw, mh, BLACK);
    DrawPixelRectLines(mx, my, mw, mh, WHITE);

    // Title Header
    Draw5x5String("STEP UTILITIES", mx + 46, my + 4, WHITE);
    DrawPixelLine(mx + 4, my + 11, mx + mw - 5, my + 11, WHITE);

    // --- ROW 1: MICROTIMING ---
    bool row1Selected = (stepUtilFocus == 0);
    int row1Y = my + 15;
    
    char stepLabel[32];
    snprintf(stepLabel, sizeof(stepLabel), "TRK %d S%d MT", state.selectedTrack + 1, state.cursorStep + 1);
    
    if (row1Selected) {
        DrawRectangle(mx + 6, row1Y - 1, 52, 7, WHITE);
        Draw3x5String(stepLabel, mx + 8, row1Y, BLACK);
    } else {
        Draw3x5String(stepLabel, mx + 8, row1Y, WHITE);
    }

    // Numerical value read-out
    int mt = tracks[state.selectedTrack].steps[state.cursorStep].microtiming;
    char valBuf[16];
    snprintf(valBuf, sizeof(valBuf), "%s%d TKS", (mt > 0 ? "+" : ""), mt);
    Draw3x5String(valBuf, mx + 138, row1Y, row1Selected ? WHITE : GRAY);

    // Slider track line
    int lineY = row1Y + 11;
    DrawPixelLine(mx + 42, lineY, mx + 138, lineY, row1Selected ? WHITE : GRAY);
    DrawPixelLine(mx + 90, lineY - 2, mx + 90, lineY + 2, row1Selected ? WHITE : GRAY); // Center tick
    
    int cursorX = mx + 90 + mt * 8;
    DrawRectangle(cursorX - 2, lineY - 2, 5, 5, row1Selected ? WHITE : GRAY);


    // --- ROW 2: TRACK LENGTH ---
    bool row2Selected = (stepUtilFocus == 1);
    int row2Y = my + 34;

    if (row2Selected) {
        DrawRectangle(mx + 6, row2Y - 1, 52, 7, WHITE);
        Draw3x5String("TRACK LEN", mx + 8, row2Y, BLACK);
    } else {
        Draw3x5String("TRACK LEN", mx + 8, row2Y, WHITE);
    }

    char lenBuf[16];
    snprintf(lenBuf, sizeof(lenBuf), "[ %2d STEPS ]", tracks[state.selectedTrack].stepLength);
    Draw3x5String(lenBuf, mx + 96, row2Y, row2Selected ? WHITE : GRAY);


    // --- ROW 3: GLOBAL LENGTH ---
    bool row3Selected = (stepUtilFocus == 2);
    int row3Y = my + 44;

    if (row3Selected) {
        DrawRectangle(mx + 6, row3Y - 1, 52, 7, WHITE);
        Draw3x5String("GLOBAL LEN", mx + 8, row3Y, BLACK);
    } else {
        Draw3x5String("GLOBAL LEN", mx + 8, row3Y, WHITE);
    }

    char globBuf[16];
    if (masterLength == 0) {
        snprintf(globBuf, sizeof(globBuf), "[   INF   ]");
    } else {
        snprintf(globBuf, sizeof(globBuf), "[ %2d STEPS ]", masterLength);
    }
    Draw3x5String(globBuf, mx + 96, row3Y, row3Selected ? WHITE : GRAY);
}

// Complete main rendering engine for Page 1: SEQ
void DrawSequencerScreen(const UIState& state) {
    std::string patStr = GetPatternHeaderString();
    
    // Append page indicator (P1 or P2) cleanly into the header title
    std::string pageIndicator = (activePage == 0) ? " P1" : " P2";
    std::string headerTitle = (state.currentScreen == SCREEN_SEQ_1_4) ? "TRK 1-4 " + patStr + pageIndicator : "TRK 5-8 " + patStr + pageIndicator;

    Draw5x5String(headerTitle.c_str(), 2, 1, WHITE);
    Draw5x5String(state.isPlaying ? "[PLAY]" : "[STOP]", 84, 1, WHITE);

    std::string octStr = "OCT:" + std::to_string(state.currentOctave);
    Draw5x5String(octStr.c_str(), 124, 1, WHITE);

    if (!state.activeNotesString.empty()) {
        std::string displayStr = "KEYS:[" + state.activeNotesString + "]";
        Draw5x5String(displayStr.c_str(), 162, 1, WHITE);
    } else {
        // Displays active state on the Note page when no keys are pressed
        Draw5x5String(liveKeyboardActive ? "KB: ON" : "KB: OFF", 162, 1, WHITE);
    }

    std::string bpmStr = std::to_string((int)state.tempo) + "BPM";
    Draw5x5String(bpmStr.c_str(), 218, 1, WHITE);

    DrawLine(29, 7, 29, OLED_HEIGHT, WHITE);

    int trackOffset = (state.currentScreen == SCREEN_SEQ_5_8) ? 4 : 0;

    // Loop 1: Draw Track Names & Mute Strikethrough with Active Highlights
    for (int track = 0; track < 4; ++track) {
        int y = 9 + (track * 13) + 4;
        int absTrack = trackOffset + track;
        
        bool isSelected = (absTrack == state.selectedTrack);
        if (isSelected) {
            DrawRectangle(1, y - 1, 26, 7, WHITE);
            Draw5x5String(tracks[absTrack].name.c_str(), 2, y, BLACK);
        } else {
            Draw5x5String(tracks[absTrack].name.c_str(), 2, y, WHITE);
        }

        if (tracks[absTrack].muted) {
            Color strikeColor = isSelected ? BLACK : WHITE;
            DrawPixelLine(2, y + 2, 24, y + 2, strikeColor);
        }
    }

    // Resolve current visual page offset (0 or 16)
    int stepOffset = activePage * 16;

    // Loop 2: Draw Steps Grid
    for (int track = 0; track < 4; ++track) {
        int absTrack = trackOffset + track;
        
        // Calculate playhead locally for this track (rests on step 1 when stopped)
        int localPlayhead = (tracks[absTrack].localTick < 0) ? 0 : (tracks[absTrack].localTick / 6) % tracks[absTrack].stepLength;

        for (int step = 0; step < 16; ++step) {
            int absStep = stepOffset + step;
            int x = 31 + (step * 14);
            int y = 9 + (track * 13);

            // Locate Sequencer cell draw inside DrawSequencerScreen:
                        bool stepOutOfBounds = (absStep >= tracks[absTrack].stepLength);
                        bool isCurrentPlayhead = (absStep == localPlayhead);
                        Step& s = tracks[absTrack].steps[absStep];

                        bool drawActive = (s.velocity > 0 && s.note >= 0); // Sentinel check
                        bool isStepCursor = (absStep == state.cursorStep && track == state.cursorTrack);
                        if (isStepCursor && state.blinkOn) {
                            drawActive = !drawActive;
                        }

                        if (stepOutOfBounds) {
                            DrawRectangleLines(x, y, 13, 13, GRAY);
                            DrawPixel(x + 6, y + 6, GRAY);
                        } else {
                            if (drawActive) {
                                DrawRectangle(x, y, 13, 13, WHITE);
                                
                                // Convert integer to string dynamically for drawing
                                std::string noteName = (s.note >= 0) ? MidiToNote(s.note) : "";

                                if (isCurrentPlayhead && state.isPlaying) {
                                    DrawRectangle(x + 1, y + 1, 11, 11, BLACK);
                                    ParseAndDrawNote(noteName.c_str(), s.velocity, false, x, y, WHITE);
                                } else {
                                    ParseAndDrawNote(noteName.c_str(), s.velocity, false, x, y, BLACK);
                                }
                            } else {
                                // ... rest of inactive step drawing stays identical ...
                    DrawRectangle(x, y, 13, 13, WHITE);
                    DrawRectangle(x + 1, y + 1, 11, 11, BLACK);
                    
                    if (isCurrentPlayhead && state.isPlaying) {
                        DrawRectangle(x + 4, y + 4, 5, 5, WHITE);
                    }
                }

                // Draw Microtiming Early/Late indicators inside Step frames
                if (s.microtiming != 0) {
                    Color mtColor = drawActive ? BLACK : WHITE;
                    if (isCurrentPlayhead && state.isPlaying && drawActive) mtColor = WHITE;

                    if (s.microtiming < 0) {
                        DrawPixel(x + 2, y + 11, mtColor);
                        DrawPixel(x + 3, y + 11, mtColor);
                    } else {
                        DrawPixel(x + 9, y + 11, mtColor);
                        DrawPixel(x + 10, y + 11, mtColor);
                    }
                }
            }
        }
    }

    // Draw Microtiming Popup overlay when editing on this page
    bool isShiftHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool isAltHeld = IsKeyDown(KEY_X);
    if (isShiftHeld && isAltHeld) {
        DrawMicrotimingPopup(state);
    }
}
// Complete main rendering engine for Page 2: TRIGS
/*void DrawTriggersScreen(const UIState& state) {
    std::string patStr = GetPatternHeaderString();
    
    // Append page indicator (P1 or P2) cleanly into the header title
    std::string pageIndicator = (activePage == 0) ? " P1" : " P2";
    std::string headerTitle = (state.currentScreen == SCREEN_TRIG_1_4) ? "TRIG 1-4 " + patStr + pageIndicator : "TRIG 5-8 " + patStr + pageIndicator;

    Draw5x5String(headerTitle.c_str(), 2, 1, WHITE);
    Draw5x5String(state.isPlaying ? "[PLAY]" : "[STOP]", 84, 1, WHITE);

    std::string octStr = "OCT:" + std::to_string(state.currentOctave);
    Draw5x5String(octStr.c_str(), 124, 1, WHITE);

    if (!state.activeNotesString.empty()) {
        std::string displayStr = "KEYS:[" + state.activeNotesString + "]";
        Draw5x5String(displayStr.c_str(), 162, 1, WHITE);
    } else {
        // Displays active state on the Trigger page when no keys are pressed
        Draw5x5String(liveKeyboardActive ? "KB: ON" : "KB: OFF", 162, 1, WHITE);
    }

    std::string bpmStr = std::to_string((int)state.tempo) + "BPM";
    Draw5x5String(bpmStr.c_str(), 218, 1, WHITE);

    DrawLine(29, 7, 29, OLED_HEIGHT, WHITE);

    int trackOffset = (state.currentScreen == SCREEN_TRIG_5_8) ? 4 : 0;

    // Loop 1: Draw Track Names & Mute Strikethrough with Active Highlights
    for (int track = 0; track < 4; ++track) {
        int y = 9 + (track * 13) + 4;
        int absTrack = trackOffset + track;
        
        bool isSelected = (absTrack == state.selectedTrack);
        if (isSelected) {
            DrawRectangle(1, y - 1, 26, 7, WHITE);
            Draw5x5String(tracks[absTrack].name.c_str(), 2, y, BLACK);
        } else {
            Draw5x5String(tracks[absTrack].name.c_str(), 2, y, WHITE);
        }

        if (tracks[absTrack].muted) {
            Color strikeColor = isSelected ? BLACK : WHITE;
            DrawPixelLine(2, y + 2, 24, y + 2, strikeColor);
        }
    }

    // Resolve current visual page offset (0 or 16)
    int stepOffset = activePage * 16;

    // Loop 2: Draw Triggers Grid
    for (int track = 0; track < 4; ++track) {
        int absTrack = trackOffset + track;
        
        // Calculate playhead locally for this track (rests on step 1 when stopped)
        int localPlayhead = (tracks[absTrack].localTick < 0) ? 0 : (tracks[absTrack].localTick / 6) % tracks[absTrack].stepLength;

        for (int step = 0; step < 16; ++step) {
            // Map the screen column directly to our absolute step index (0..31)
            int absStep = stepOffset + step;
            int x = 31 + (step * 14);
            int y = 9 + (track * 13);

            bool stepOutOfBounds = (absStep >= tracks[absTrack].stepLength);
            bool isCurrentPlayhead = (absStep == localPlayhead);
            Step& s = tracks[absTrack].steps[absStep];

            // Sync triggers display directly with active notes
            bool drawActive = (s.velocity > 0 && !s.note.empty());
            bool isStepCursor = (absStep == state.cursorStep && track == state.cursorTrack);
            if (isStepCursor && state.blinkOn) {
                drawActive = !drawActive;
            }

            if (stepOutOfBounds) {
                // Draw a beautiful, dimmed hollow box with center dot for inactive polymetric steps
                DrawRectangleLines(x, y, 13, 13, GRAY);
                DrawPixel(x + 6, y + 6, GRAY);
            } else {
                if (drawActive) {
                    DrawRectangle(x, y, 13, 13, WHITE);
                    
                    if (isCurrentPlayhead && state.isPlaying) {
                        DrawRectangle(x + 1, y + 1, 11, 11, BLACK);
                        DrawTrigInBox(s.condition.c_str(), s.retrigger, x, y, WHITE);
                    } else {
                        DrawTrigInBox(s.condition.c_str(), s.retrigger, x, y, BLACK);
                    }
                } else {
                    DrawRectangle(x, y, 13, 13, WHITE);
                    DrawRectangle(x + 1, y + 1, 11, 11, BLACK);
                    
                    if (isCurrentPlayhead && state.isPlaying) {
                        DrawRectangle(x + 4, y + 4, 5, 5, WHITE);
                    }
                }
            }
        }
    }

    // Draw Microtiming Popup overlay when editing on this page
    bool isShiftHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool isAltHeld = IsKeyDown(KEY_X);
    if (isShiftHeld && isAltHeld) {
        DrawMicrotimingPopup(state);
    }
}*/
// Complete main rendering engine for Page 3: SYNTH
void DrawSynthScreen(const UIState& state) {
    std::string headerTrack = "TRACK " + std::to_string(state.selectedTrack + 1) + " " + GetPatternHeaderString();
    Draw5x5String(headerTrack.c_str(), 2, 1, WHITE);
    
    std::string bpmStr = std::to_string((int)state.tempo) + "BPM";
    Draw5x5String(bpmStr.c_str(), 218, 1, WHITE);

    int row1Y = 8;
    int row2Y = 36;

    const Track& trk = tracks[state.selectedTrack];
    const StepParams& sp = trk.steps[state.cursorStep].params; // Read overrides from active step

    extern bool g_hardwareEncoderClicked;
        bool isAltHeld = IsKeyDown(KEY_X) || g_hardwareEncoderClicked;    auto GetEffectiveVal = [&](int stepVal, int trackVal, int drawX, int drawY, bool& isLocked) {
        if (!isAltHeld) {
            isLocked = false;
            return trackVal;
        }
        if (stepVal == -1) {
            isLocked = false;
            return trackVal;
        }
        isLocked = true;
        DrawRectangle(drawX, drawY, 2, 2, WHITE); // 2x2 dot
        return stepVal;
    };

    if (trk.engineType == ENGINE_SAMPLER) {
        // --- SAMPLER ENGINE VIEWPORT ---
        bool lockedAlgo = false;
        int effAlgo = GetEffectiveVal(sp.algorithm, trk.algorithm, 15, 53, lockedAlgo);

        if (effAlgo == ALGO_GRANULAR) {
            Draw5x5String("ENG: GRANULAR", 76, 1, WHITE);
        } else {
            Draw5x5String("ENG: SAMPLE", 76, 1, WHITE);
        }

        int displayStep = isAltHeld ? (state.cursorStep + 1) : (state.playhead + 1);
        std::string stepStr = "STEP: " + std::to_string(displayStep);
        Draw5x5String(stepStr.c_str(), 168, 1, WHITE);

        DrawPixelLine(179, 7, 179, OLED_HEIGHT, WHITE);

        DrawPixelRectLines(3, 11, 101, 25, WHITE);
        
        bool lockedDivs = false;
        int divs = GetEffectiveVal(sp.sliceDivisions, trk.sliceDivisions, 86, 53, lockedDivs);
        if (divs < 1) divs = 1;

        // Render pre-calculated waveform representation of loaded sample from visual cache
                bool lockedSamp = false;
                int samp = GetEffectiveVal(sp.sampleSlot, trk.sampleSlot, 38, 53, lockedSamp);
                const auto& pcm = g_samplePool[samp].pcmData;
                const auto& peaks = g_samplePool[samp].visualPeaks;

                if (!pcm.empty()) {
                    for (int i = 0; i < 97; ++i) {
                        int peakHeight = peaks[i];
                        int px = 5 + i;
                        if (peakHeight > 0) {
                            DrawLine(px, 23 - peakHeight, px, 23 + peakHeight, WHITE);
                        } else {
                            DrawPixel(px, 23, WHITE); // Baseline dot
                        }
                    }
                } else {
                    // Empty slot baseline representation
                    DrawPixelLine(5, 23, 101, 23, WHITE);
                    Draw3x5String("EMPTY SLOT", 32, 21, WHITE);
                }

        if (effAlgo == ALGO_SAMPLE) {
            bool lockedPos = false;
            int activeSlice = GetEffectiveVal(sp.grainPosition, trk.grainPosition, 160, 46, lockedPos) % divs;
            float sliceWidth = 97.0f / divs;
            
            for (int i = 1; i < divs; ++i) {
                int tickX = 5 + (int)(i * sliceWidth);
                DrawLine(tickX, 11, tickX, 13, WHITE);
                DrawLine(tickX, 33, tickX, 35, WHITE);
            }

            int startX = 5 + (int)(activeSlice * sliceWidth);
            int endX = 5 + (int)((activeSlice + 1) * sliceWidth);

            // Top-Left [
            DrawPixel(startX, 12, WHITE); DrawPixel(startX, 13, WHITE); DrawPixel(startX, 14, WHITE);
            DrawPixel(startX + 1, 12, WHITE); DrawPixel(startX + 2, 12, WHITE);

            // Top-Right ]
            DrawPixel(endX - 1, 12, WHITE); DrawPixel(endX - 1, 13, WHITE); DrawPixel(endX - 1, 14, WHITE);
            DrawPixel(endX - 2, 12, WHITE); DrawPixel(endX - 3, 12, WHITE);

            // Bottom-Left [
            DrawPixel(startX, 34, WHITE); DrawPixel(startX, 33, WHITE); DrawPixel(startX, 32, WHITE);
            DrawPixel(startX + 1, 34, WHITE); DrawPixel(startX + 2, 34, WHITE);

            // Bottom-Right ]
            DrawPixel(endX - 1, 34, WHITE); DrawPixel(endX - 1, 33, WHITE); DrawPixel(endX - 1, 32, WHITE);
            DrawPixel(endX - 2, 34, WHITE); DrawPixel(endX - 3, 34, WHITE);
        }

        bool lockedSS = false;
        int effSS = GetEffectiveVal(sp.sampleStart, trk.sampleStart, 110, 53, lockedSS);
        int ss_x = 5 + (effSS * 97) / 99;

        bool lockedSL = false;
        int effSL = GetEffectiveVal(sp.sampleLength, trk.sampleLength, 133, 53, lockedSL);
        int se_val = effSS + effSL;
        if (se_val > 99) se_val = 99;
        int se_x = 5 + (se_val * 97) / 99;

        for (int dy = 12; dy <= 34; dy += 2) {
            DrawPixel(ss_x, dy, WHITE);
            DrawPixel(se_x, dy, WHITE);
        }

        // Determine selector states for Row 1
        bool ssSelected = (state.synthGridRow == 1 && state.synthGridCol == 1);
        bool seSelected = (state.synthGridRow == 1 && state.synthGridCol == 2);
        bool lsSelected = (state.synthGridRow == 1 && state.synthGridCol == 3);
        bool leSelected = (state.synthGridRow == 1 && state.synthGridCol == 4);

        int ss_text_x = ss_x - 3;
        int se_text_x = se_x - 3;
        if (std::abs(ss_text_x - se_text_x) < 10) {
            se_text_x = ss_text_x + 10; // Shift SE right if too close to SS
        }

        // Highlight SS on selection
        if (ssSelected) {
            DrawRectangle(ss_text_x - 1, 37, 9, 7, WHITE);
            Draw3x5String("SS", ss_text_x, 38, BLACK);
        } else {
            Draw3x5String("SS", ss_text_x, 38, WHITE);
        }

        // Highlight SE on selection
        if (seSelected) {
            DrawRectangle(se_text_x - 1, 37, 9, 7, WHITE);
            Draw3x5String("SE", se_text_x, 38, BLACK);
        } else {
            Draw3x5String("SE", se_text_x, 38, WHITE);
        }

        bool lockedLoop = false;
        int effLoop = GetEffectiveVal(sp.sampleLoop, trk.sampleLoop, 15, 44, lockedLoop);
        if (effLoop == 1) {
            bool lockedLS = false;
            int effLS = GetEffectiveVal(sp.loopStart, trk.loopStart, 3, 38, lockedLS);
            int ls_x = 5 + (effLS * 97) / 99;

            bool lockedLE = false;
            int effLE = GetEffectiveVal(sp.loopEnd, trk.loopEnd, 2, 53, lockedLE);
            int le_x = 5 + (effLE * 97) / 99;

            for (int dy = 12; dy <= 34; dy += 4) {
                DrawPixel(ls_x, dy, WHITE);
                DrawPixel(le_x, dy, WHITE);
            }

            int ls_text_x = ls_x - 3;
            int le_text_x = le_x - 3;
            if (std::abs(ls_text_x - le_text_x) < 10) {
                le_text_x = ls_text_x + 10; // Shift LE right if too close to LS
            }

            // Highlight LS on selection
            if (lsSelected) {
                DrawRectangle(ls_text_x - 1, 37, 9, 7, WHITE);
                Draw3x5String("LS", ls_text_x, 38, BLACK);
            } else {
                Draw3x5String("LS", ls_text_x, 38, WHITE);
            }

            // Highlight LE on selection
            if (leSelected) {
                DrawRectangle(le_text_x - 1, 37, 9, 7, WHITE);
                Draw3x5String("LE", le_text_x, 38, BLACK);
            } else {
                Draw3x5String("LE", le_text_x, 38, WHITE);
            }
        }

        bool col0Row1Selected = (state.synthGridRow == 1 && state.synthGridCol == 0);
        bool col0Row2Selected = (state.synthGridRow == 2 && state.synthGridCol == 0);

        if (effLoop == 0) {
            if (col0Row1Selected) {
                DrawRectangle(2, 44, 11, 7, WHITE);
                Draw3x5String("1S", 4, 45, BLACK);
                Draw3x5String("LP", 18, 45, WHITE);
            } else {
                DrawPixelRectLines(2, 44, 11, 7, WHITE);
                Draw3x5String("1S", 4, 45, WHITE);
                Draw3x5String("LP", 18, 45, WHITE);
            }
        } else {
            if (col0Row1Selected) {
                DrawRectangle(16, 44, 11, 7, WHITE);
                Draw3x5String("1S", 4, 45, WHITE);
                Draw3x5String("LP", 18, 45, BLACK);
            } else {
                Draw3x5String("1S", 4, 45, WHITE);
                Draw3x5String("LP", 18, 45, WHITE);
                DrawPixelRectLines(16, 44, 11, 7, WHITE);
            }
        }

        // --- COLUMN 0 ROW 2: Slicing vs Granular Algorithm ---
        if (effAlgo == ALGO_SAMPLE) {
            if (col0Row2Selected) {
                DrawRectangle(2, 53, 11, 7, WHITE);
                Draw3x5String("SL", 4, 54, BLACK);
                Draw3x5String("GR", 18, 54, WHITE);
            } else {
                DrawPixelRectLines(2, 53, 11, 7, WHITE);
                Draw3x5String("SL", 4, 54, WHITE);
                Draw3x5String("GR", 18, 54, WHITE);
            }
        } else {
            if (col0Row2Selected) {
                DrawRectangle(16, 53, 11, 7, WHITE);
                Draw3x5String("SL", 4, 54, WHITE);
                Draw3x5String("GR", 18, 54, BLACK);
            } else {
                Draw3x5String("SL", 4, 54, WHITE);
                Draw3x5String("GR", 18, 54, WHITE);
                DrawPixelRectLines(16, 53, 11, 7, WHITE);
            }
        }

        // --- COLUMN 1 ROW 2: SAMP (Active Pool Wave Selector) ---
        bool sampSelected = (state.synthGridRow == 2 && state.synthGridCol == 1);
        std::string sampName = g_samplePool[samp].name;
        if (sampName.length() > 4) sampName = sampName.substr(0, 4); // Truncate to 4 characters
        
        char sampBuf[32];
        snprintf(sampBuf, sizeof(sampBuf), "%02d:%s", samp + 1, sampName.c_str());

        if (sampSelected) {
            DrawRectangle(38, 53, 44, 7, WHITE);
            Draw3x5String("SMP", 40, 54, BLACK);
            Draw3x5String(sampBuf, 56, 54, BLACK);
        } else {
            Draw3x5String("SMP", 40, 54, WHITE);
            Draw3x5String(sampBuf, 56, 54, WHITE);
        }

        // --- COLUMN 2 ROW 2: SL.DIV (DV) ---
        // Exclusively handles Slice divisions now that LE has moved to Row 1
        bool col2Selected = (state.synthGridRow == 2 && state.synthGridCol == 2);
        if (effAlgo == ALGO_SAMPLE) {
            std::string sdivStr = std::to_string(divs);
            Draw3x5String(sdivStr.c_str(), 88, 46, WHITE); // Centered above box
            if (col2Selected) {
                DrawRectangle(84, 53, 21, 7, WHITE);
                Draw3x5String("DV", 86, 54, BLACK);
            } else {
                DrawPixelRectLines(84, 53, 21, 7, WHITE);
                Draw3x5String("DV", 86, 54, WHITE);
            }
        }

        // Envelope 1 parameters (Shifted rightward to Columns 5, 6, 7, and 8)
        bool lAtk = false, lDec = false, lSus = false, lRel = false;
        int effAtk = GetEffectiveVal(sp.attack, trk.attack, 111, 27, lAtk);
        int effDec = GetEffectiveVal(sp.decay, trk.decay, 128, 27, lDec);
        int effSus = GetEffectiveVal(sp.sustain, trk.sustain, 145, 27, lSus);
        int effRel = GetEffectiveVal(sp.release, trk.release, 162, 27, lRel);

        DrawAdsrEnvelope(effAtk, effDec, effSus, effRel, 107, 11, 67, 14, WHITE);
        
        if (state.synthGridRow == 1 && state.synthGridCol == 5) {
            DrawRectangle(110, 26, 7, 7, WHITE);
            Draw5x5Char('A', 111, 27, BLACK);
        } else {
            Draw5x5Char('A', 111, 27, WHITE);
        }
        
        if (state.synthGridRow == 1 && state.synthGridCol == 6) {
            DrawRectangle(127, 26, 7, 7, WHITE);
            Draw5x5Char('D', 128, 27, BLACK);
        } else {
            Draw5x5Char('D', 128, 27, WHITE);
        }
        
        if (state.synthGridRow == 1 && state.synthGridCol == 7) {
            DrawRectangle(144, 26, 7, 7, WHITE);
            Draw5x5Char('S', 145, 27, BLACK);
        } else {
            Draw5x5Char('S', 145, 27, WHITE);
        }
        
        if (state.synthGridRow == 1 && state.synthGridCol == 8) {
            DrawRectangle(161, 26, 7, 7, WHITE);
            Draw5x5Char('R', 162, 27, BLACK);
        } else {
            Draw5x5Char('R', 162, 27, WHITE);
        }

        // Tunings & Slide Positions (Columns 3, 4, 5)
        bool lCrs = false, lFine = false, lPos = false;
        int crsVal = GetEffectiveVal(sp.sampleTune, trk.sampleTune, 111, 54, lCrs);
        int fineVal = GetEffectiveVal(sp.fine2, trk.fine2, 134, 54, lFine);
        int posVal = GetEffectiveVal(sp.grainPosition, trk.grainPosition, 161, 54, lPos);

        DrawSlider(crsVal, -24, 24, 107, 46, 21, (state.synthGridRow == 2 && state.synthGridCol == 3), true, WHITE);
        if (state.synthGridRow == 2 && state.synthGridCol == 3) {
            DrawRectangle(110, 53, 19, 7, WHITE);
            Draw5x5String("CRS", 111, 54, BLACK);
        } else {
            Draw5x5String("CRS", 111, 54, WHITE);
        }

        DrawSlider(fineVal, -99, 99, 132, 46, 21, (state.synthGridRow == 2 && state.synthGridCol == 4), true, WHITE);
        if (state.synthGridRow == 2 && state.synthGridCol == 4) {
            DrawRectangle(133, 53, 23, 7, WHITE);
            Draw5x5String("FINE", 134, 54, BLACK);
        } else {
            Draw5x5String("FINE", 134, 54, WHITE);
        }

        // Only restrict POS range to slice indexes if slicing is enabled and slice count is greater than 1
        int maxPosVal = (effAlgo == ALGO_SAMPLE && divs > 1) ? (divs - 1) : 99;
        posVal = std::clamp(posVal, 0, maxPosVal);

        DrawSlider(posVal, 0, maxPosVal, 157, 46, 21, (state.synthGridRow == 2 && state.synthGridCol == 5), true, WHITE);
        if (state.synthGridRow == 2 && state.synthGridCol == 5) {
            DrawRectangle(160, 53, 19, 7, WHITE);
            Draw5x5String("POS", 161, 54, BLACK);
        } else {
            Draw5x5String("POS", 161, 54, WHITE);
        }

        // Columns 9, 10, 11
        bool pdSelected   = (state.synthGridRow == 1 && state.synthGridCol == 9);
        bool ptSelected   = (state.synthGridRow == 1 && state.synthGridCol == 10);
        bool bcSelected   = (state.synthGridRow == 1 && state.synthGridCol == 11);
        bool mvolSelected = (state.synthGridRow == 1 && state.synthGridCol == 12);

        bool lPd = false, lPt = false, lBc = false, lMvol = false;
        int effPd = GetEffectiveVal(sp.pitchSweepDepth, trk.pitchSweepDepth, 184, 27, lPd);
        int effPt = GetEffectiveVal(sp.pitchSweepTime, trk.pitchSweepTime, 197, 27, lPt);
        int effBc = GetEffectiveVal(sp.bitRed, trk.bitRed, 220, 27, lBc);
        int effMvol = GetEffectiveVal(sp.volume, trk.volume, 239, 27, lMvol);
        
        DrawPitchCurve(effPd, effPt, 184, 11, 23, 14, (pdSelected || ptSelected), WHITE);
        DrawConcentricSquares(effBc, 212, 11, 23, 14, bcSelected, WHITE);
        DrawLevelBars(effMvol, 240, 11, 13, 14, mvolSelected, WHITE);

        if (pdSelected) {
            DrawRectangle(183, 26, 9, 7, WHITE);
            Draw3x5String("PD", 184, 27, BLACK);
        } else {
            Draw3x5String("PD", 184, 27, WHITE);
        }
        Draw3x5String("/", 193, 27, WHITE);
        if (ptSelected) {
            DrawRectangle(196, 26, 9, 7, WHITE);
            Draw3x5String("PT", 197, 27, BLACK);
        } else {
            Draw3x5String("PT", 197, 27, WHITE);
        }

        if (bcSelected) {
            DrawRectangle(219, 26, 9, 7, WHITE);
            Draw3x5String("BC", 220, 27, BLACK);
        } else {
            Draw3x5String("BC", 220, 27, WHITE);
        }

        if (mvolSelected) {
            DrawRectangle(238, 26, 17, 7, WHITE);
            Draw3x5String("M.VOL", 239, 27, BLACK);
        } else {
            Draw3x5String("M.VOL", 239, 27, WHITE);
        }
       
        // Granular Rings / Spacing parameter panel (Columns 6, 7, 8)
        bool lSz = false, lDens = false, lScat = false;
        int effSz = GetEffectiveVal(sp.grainSize, trk.grainSize, 184, 55, lSz);
        int effDens = GetEffectiveVal(sp.grainDensity, trk.grainDensity, 210, 55, lDens);
        int effScat = GetEffectiveVal(sp.grainScatter, trk.grainScatter, 234, 55, lScat);

        DrawGranularRings(effSz, effDens, effScat, 218, 44, WHITE);
        
        bool sizeSelected = (state.synthGridRow == 2 && state.synthGridCol == 6);
        bool densSelected = (state.synthGridRow == 2 && state.synthGridCol == 7);
        bool scatSelected = (state.synthGridRow == 2 && state.synthGridCol == 8);

        if (sizeSelected) {
            DrawRectangle(183, 54, 17, 7, WHITE);
            Draw3x5String("SIZE", 184, 55, BLACK);
        } else {
            Draw3x5String("SIZE", 184, 55, WHITE);
        }

        if (densSelected) {
            DrawRectangle(209, 54, 17, 7, WHITE);
            Draw3x5String("DENS", 210, 55, BLACK);
        } else {
            Draw3x5String("DENS", 210, 55, WHITE);
        }

        if (scatSelected) {
            DrawRectangle(233, 54, 17, 7, WHITE);
            Draw3x5String("SCAT", 234, 55, BLACK);
        } else {
            Draw3x5String("SCAT", 234, 55, WHITE);
        }
    } else { // --- SYNTH TRACK VIEWPORT ---
        if (tracks[state.selectedTrack].algorithm == ALGO_CARRIER_MOD) {
            Draw5x5String("ENGINE: FM (C/M)", 54, 1, WHITE);
        } else {
            Draw5x5String("ENGINE: DUAL OSC", 54, 1, WHITE);
        }

        int displayStep = isAltHeld ? (state.cursorStep + 1) : (state.playhead + 1);
        std::string stepStr = "STEP: " + std::to_string(displayStep) + "  " + GetPatternHeaderString();
        Draw5x5String(stepStr.c_str(), 148, 1, WHITE);

        DrawPixelLine(179, 7, 179, OLED_HEIGHT, WHITE);

        bool lMorph1 = false, lCrs1 = false, lFine1 = false, lVol1 = false;
        int effMorph1 = GetEffectiveVal(sp.morph, trk.morph, 2, row1Y + 19, lMorph1);
        int effCrs1 = GetEffectiveVal(sp.coarse, trk.coarse, 31, row1Y + 19, lCrs1);
        int effFine1 = GetEffectiveVal(sp.fine, trk.fine, 56, row1Y + 19, lFine1);
        int effVol1 = GetEffectiveVal(sp.volume, trk.volume, 82, row1Y + 19, lVol1);
     
        DrawSourceWaveform(effMorph1, 2, row1Y + 3, 23, 14, (state.synthGridRow == 1 && state.synthGridCol == 0), WHITE);
        DrawSlider(effCrs1, -24, 24, 31, row1Y + 9, 21, (state.synthGridRow == 1 && state.synthGridCol == 1), true, WHITE);
        DrawSlider(effFine1, -99, 99, 58, row1Y + 9, 21, (state.synthGridRow == 1 && state.synthGridCol == 2), true, WHITE);
        DrawLevelBars(effVol1, 84, row1Y + 3, 13, 14, (state.synthGridRow == 1 && state.synthGridCol == 3), WHITE);

        bool lAtk1 = false, lDec1 = false, lSus1 = false, lRel1 = false;
        int effAtk1 = GetEffectiveVal(sp.attack, trk.attack, 111, row1Y + 19, lAtk1);
        int effDec1 = GetEffectiveVal(sp.decay, trk.decay, 128, row1Y + 19, lDec1);
        int effSus1 = GetEffectiveVal(sp.sustain, trk.sustain, 145, row1Y + 19, lSus1);
        int effRel1 = GetEffectiveVal(sp.release, trk.release, 162, row1Y + 19, lRel1);

        DrawAdsrEnvelope(effAtk1, effDec1, effSus1, effRel1, 107, row1Y + 3, 67, 14, WHITE);
        
        Draw5x5String("WAVE", 2, row1Y + 19, WHITE);
        Draw5x5String("CRS", 31, row1Y + 19, WHITE);
        Draw5x5String("FINE", 56, row1Y + 19, WHITE);
        Draw5x5String("VOL", 82, row1Y + 19, WHITE);
        
        if (state.synthGridRow == 1 && state.synthGridCol == 4) {
            DrawRectangle(110, row1Y + 18, 7, 7, WHITE);
            Draw5x5Char('A', 111, row1Y + 19, BLACK);
        } else {
            Draw5x5Char('A', 111, row1Y + 19, WHITE);
        }
        
        if (state.synthGridRow == 1 && state.synthGridCol == 5) {
            DrawRectangle(127, row1Y + 18, 7, 7, WHITE);
            Draw5x5Char('D', 128, row1Y + 19, BLACK);
        } else {
            Draw5x5Char('D', 128, row1Y + 19, WHITE);
        }
        
        if (state.synthGridRow == 1 && state.synthGridCol == 6) {
            DrawRectangle(144, row1Y + 18, 7, 7, WHITE);
            Draw5x5Char('S', 145, row1Y + 19, BLACK);
        } else {
            Draw5x5Char('S', 145, row1Y + 19, WHITE);
        }
        
        if (state.synthGridRow == 1 && state.synthGridCol == 7) {
            DrawRectangle(161, row1Y + 18, 7, 7, WHITE);
            Draw5x5Char('R', 162, row1Y + 19, BLACK);
        } else {
            Draw5x5Char('R', 162, row1Y + 19, WHITE);
        }

        bool lMorph2 = false, lCrs2 = false, lFine2 = false, lVol2 = false;
        int effMorph2 = GetEffectiveVal(sp.morph2, trk.morph2, 2, row2Y + 19, lMorph2);
        int effCrs2 = GetEffectiveVal(sp.coarse2, trk.coarse2, 31, row2Y + 19, lCrs2);
        int effFine2 = GetEffectiveVal(sp.fine2, trk.fine2, 56, row2Y + 19, lFine2);
        int effVol2 = GetEffectiveVal(sp.volume2, trk.volume2, 82, row2Y + 19, lVol2);

        DrawSourceWaveform(effMorph2, 2, row2Y + 3, 23, 14, (state.synthGridRow == 2 && state.synthGridCol == 0), WHITE);
        
        if (tracks[state.selectedTrack].algorithm == ALGO_CARRIER_MOD) {
            DrawSlider(effCrs2, 1, 16, 31, row2Y + 9, 21, (state.synthGridRow == 2 && state.synthGridCol == 1), true, WHITE);
        } else {
            DrawSlider(effCrs2, -24, 24, 31, row2Y + 9, 21, (state.synthGridRow == 2 && state.synthGridCol == 1), true, WHITE);
        }
        
        DrawSlider(effFine2, -99, 99, 58, row2Y + 9, 21, (state.synthGridRow == 2 && state.synthGridCol == 2), true, WHITE);
        DrawLevelBars(effVol2, 84, row2Y + 3, 13, 14, (state.synthGridRow == 2 && state.synthGridCol == 3), WHITE);

        bool lAtk2 = false, lDec2 = false, lSus2 = false, lRel2 = false;
        int effAtk2 = GetEffectiveVal(sp.attack2, trk.attack2, 111, row2Y + 19, lAtk2);
        int effDec2 = GetEffectiveVal(sp.decay2, trk.decay2, 128, row2Y + 19, lDec2);
        int effSus2 = GetEffectiveVal(sp.sustain2, trk.sustain2, 145, row2Y + 19, lSus2);
        int effRel2 = GetEffectiveVal(sp.release2, trk.release2, 162, row2Y + 19, lRel2);

        DrawAdsrEnvelope(effAtk2, effDec2, effSus2, effRel2, 107, row2Y + 3, 67, 14, WHITE);
        
        Draw5x5String("WAVE", 2, row2Y + 19, WHITE);
        if (tracks[state.selectedTrack].algorithm == ALGO_CARRIER_MOD) {
            Draw5x5String("RAT", 31, row2Y + 19, WHITE);
            Draw5x5String("FINE", 56, row2Y + 19, WHITE);
            Draw5x5String("IDX", 82, row2Y + 19, WHITE);
        } else {
            Draw5x5String("CRS", 31, row2Y + 19, WHITE);
            Draw5x5String("FINE", 56, row2Y + 19, WHITE);
            Draw5x5String("VOL", 82, row2Y + 19, WHITE);
        }

        if (state.synthGridRow == 2 && state.synthGridCol == 4) {
            DrawRectangle(110, row2Y + 18, 7, 7, WHITE);
            Draw5x5Char('A', 111, row2Y + 19, BLACK);
        } else {
            Draw5x5Char('A', 111, row2Y + 19, WHITE);
        }
        
        if (state.synthGridRow == 2 && state.synthGridCol == 5) {
            DrawRectangle(127, row2Y + 18, 7, 7, WHITE);
            Draw5x5Char('D', 128, row2Y + 19, BLACK);
        } else {
            Draw5x5Char('D', 128, row2Y + 19, WHITE);
        }
        
        if (state.synthGridRow == 2 && state.synthGridCol == 6) {
            DrawRectangle(144, row2Y + 18, 7, 7, WHITE);
            Draw5x5Char('S', 145, row2Y + 19, BLACK);
        } else {
            Draw5x5Char('S', 145, row2Y + 19, WHITE);
        }
        
        if (state.synthGridRow == 2 && state.synthGridCol == 7) {
            DrawRectangle(161, row2Y + 18, 7, 7, WHITE);
            Draw5x5Char('R', 162, row2Y + 19, BLACK);
        } else {
            Draw5x5Char('R', 162, row2Y + 19, WHITE);
        }

        bool pdSelected   = (state.synthGridRow == 1 && state.synthGridCol == 8);
        bool ptSelected   = (state.synthGridRow == 1 && state.synthGridCol == 9);
        bool fbSelected   = (state.synthGridRow == 1 && state.synthGridCol == 10);
        bool mvolSelected = (state.synthGridRow == 1 && state.synthGridCol == 11);

        bool lPd = false, lPt = false, lFb = false, lMvol = false;
                int effPd = GetEffectiveVal(sp.pitchSweepDepth, trk.pitchSweepDepth, 184, 27, lPd);
                int effPt = GetEffectiveVal(sp.pitchSweepTime, trk.pitchSweepTime, 197, 27, lPt);
                int effFb = GetEffectiveVal(sp.fmFeedback, trk.fmFeedback, 220, 27, lFb);
                int effMvol = GetEffectiveVal(sp.masterVolume, trk.masterVolume, 239, 27, lMvol); // <--- Swapped to masterVolume

        DrawPitchCurve(effPd, effPt, 184, 11, 23, 14, (pdSelected || ptSelected), WHITE);
        DrawConcentricSquares(effFb, 212, 11, 23, 14, fbSelected, WHITE);
        DrawLevelBars(effMvol, 240, 11, 13, 14, mvolSelected, WHITE);

        if (pdSelected) {
            DrawRectangle(183, 26, 9, 7, WHITE);
            Draw3x5String("PD", 184, 27, BLACK);
        } else {
            Draw3x5String("PD", 184, 27, WHITE);
        }
        Draw3x5String("/", 193, 27, WHITE);
        if (ptSelected) {
            DrawRectangle(196, 26, 9, 7, WHITE);
            Draw3x5String("PT", 197, 27, BLACK);
        } else {
            Draw3x5String("PT", 197, 27, WHITE);
        }

        const char* fbLabel = (trk.algorithm == ALGO_PARALLEL) ? "AN" : "FB";
                if (fbSelected) {
                    DrawRectangle(219, 26, 9, 7, WHITE);
                    Draw3x5String(fbLabel, 220, 27, BLACK);
                } else {
                    Draw3x5String(fbLabel, 220, 27, WHITE);
                }

        if (mvolSelected) {
            DrawRectangle(238, 26, 17, 7, WHITE);
            Draw3x5String("M.VOL", 239, 27, BLACK);
        } else {
            Draw3x5String("M.VOL", 239, 27, WHITE);
        }
        // Draw PLY parameter under Sampler volume
                bool plySelected = (state.synthGridRow == 2 && state.synthGridCol == 11);
                bool lPly = false;
                int effPly = GetEffectiveVal(sp.polyMode, trk.polyMode, 239, 55, lPly);

                if (plySelected) {
                    DrawRectangle(238, 54, 17, 7, WHITE);
                    Draw3x5String((effPly == 0 ? "MN" : "PL"), 239, 55, BLACK);
                } else {
                    Draw3x5String((effPly == 0 ? "MN" : "PL"), 239, 55, WHITE);
                }
        bool naSelected   = (state.synthGridRow == 2 && state.synthGridCol == 8);
        bool nhSelected   = (state.synthGridRow == 2 && state.synthGridCol == 9);
        bool ndSelected   = (state.synthGridRow == 2 && state.synthGridCol == 10);
        bool nvolSelected = (state.synthGridRow == 2 && state.synthGridCol == 11);

        bool lNa = false, lNh = false, lNd = false, lNvol = false;
        int effNa = GetEffectiveVal(sp.noiseAttack, trk.noiseAttack, 184, 55, lNa);
        int effNh = GetEffectiveVal(sp.noiseHold, trk.noiseHold, 200, 55, lNh);
        int effNd = GetEffectiveVal(sp.noiseDecay, trk.noiseDecay, 216, 55, lNd);
        int effNvol = GetEffectiveVal(sp.noiseVolume, trk.noiseVolume, 239, 55, lNvol);

        DrawAhdEnvelope(effNa, effNh, effNd, 184, 39, 51, 14, WHITE);
        DrawLevelBars(effNvol, 240, 39, 13, 14, nvolSelected, WHITE);

        if (naSelected) {
            DrawRectangle(183, 54, 11, 7, WHITE);
            Draw3x5String("N.A", 184, 55, BLACK);
        } else {
            Draw3x5String("N.A", 184, 55, WHITE);
        }
        Draw3x5String("/", 195, 55, WHITE);
        if (nhSelected) {
            DrawRectangle(199, 54, 11, 7, WHITE);
            Draw3x5String("N.H", 200, 55, BLACK);
        } else {
            Draw3x5String("N.H", 200, 55, WHITE);
        }
        Draw3x5String("/", 211, 55, WHITE);
        if (ndSelected) {
            DrawRectangle(215, 54, 11, 7, WHITE);
            Draw3x5String("N.D", 216, 55, BLACK);
        } else {
            Draw3x5String("N.D", 216, 55, WHITE);
        }

        if (nvolSelected) {
            DrawRectangle(238, 54, 17, 7, WHITE);
            Draw3x5String("N.VOL", 239, 55, BLACK);
        } else {
            Draw3x5String("N.VOL", 239, 55, WHITE);
        }
    }
}

void DrawFilterLfoPage(const UIState& state) {
    const Track& trk = tracks[state.selectedTrack];
    const StepParams& sp = trk.steps[state.cursorStep].params; // Overrides on active editing step

    // Local helper: gets effective value & renders dot next to label if locked
    bool isAltHeld = IsKeyDown(KEY_X);
    auto GetEffectiveVal = [&](int stepVal, int trackVal, int drawX, int drawY, bool& isLocked) {
        if (!isAltHeld) {
            isLocked = false;
            return trackVal;
        }
        if (stepVal == -1) {
            isLocked = false;
            return trackVal;
        }
        isLocked = true;
        DrawRectangle(drawX, drawY, 2, 2, WHITE); // 2x2 dot
        return stepVal;
    };

    bool lType = false;
    int effType = GetEffectiveVal(sp.filterType, trk.filterType, 60, 27, lType);

    std::string typeStr = (effType == 0) ? "LPF" : ((effType == 1) ? "HPF" : "BPF");

    // Track & Pattern Header (x=2)
    std::string headerTrack = "TRACK " + std::to_string(state.selectedTrack + 1) + " " + GetPatternHeaderString();
    Draw5x5String(headerTrack.c_str(), 2, 1, WHITE);

    // Active Filter Type (x=76)
    std::string filterTypeStr = "TYPE: " + typeStr;
    Draw5x5String(filterTypeStr.c_str(), 76, 1, WHITE);

    // Dynamic Step Counter (x=168)
    int displayStep = isAltHeld ? (state.cursorStep + 1) : (state.playhead + 1);
    std::string stepStr = "STEP: " + std::to_string(displayStep);
    Draw5x5String(stepStr.c_str(), 168, 1, WHITE);

    // BPM Header (x=218)
    std::string bpmStr = std::to_string((int)state.tempo) + "BPM";
    Draw5x5String(bpmStr.c_str(), 218, 1, WHITE);

    // Symmetrical single dividing line down the exact middle of the screen
    DrawPixelLine(128, 7, 128, OLED_HEIGHT, WHITE);

    bool lCut = false, lRes = false;
    int effCut = GetEffectiveVal(sp.filterCutoff, trk.filterCutoff, 4, 27, lCut);
    int effRes = GetEffectiveVal(sp.filterResonance, trk.filterResonance, 32, 27, lRes);

    // Spacious Filter Curve (w = 80)
    DrawFilterCurve(effCut, effRes, effType, 4, 11, 80, 14, WHITE);

    bool lFAtk = false, lFDec = false, lFSus = false, lFRel = false;
    int effFAtk = GetEffectiveVal(sp.filterAttack, trk.filterAttack, 12, 55, lFAtk);
    int effFDec = GetEffectiveVal(sp.filterDecay, trk.filterDecay, 36, 55, lFDec);
    int effFSus = GetEffectiveVal(sp.filterSustain, trk.filterSustain, 60, 55, lFSus);
    int effFRel = GetEffectiveVal(sp.filterRelease, trk.filterRelease, 84, 55, lFRel);

    // Squashed envelope height to 11 pixels, shifted down to y=42, and narrowed to w=84 to clear Column 3
    DrawAdsrEnvelope(effFAtk, effFDec, effFSus, effFRel, 4, 42, 84, 11, WHITE);

    auto DrawLeftLabel = [&](const char* lbl, int lx, int ly, bool selected, int boxW) {
        if (selected) {
            DrawRectangle(lx - 1, ly - 1, boxW, 7, WHITE);
            Draw3x5String(lbl, lx, ly, BLACK);
        } else {
            Draw3x5String(lbl, lx, ly, WHITE);
        }
    };

    DrawLeftLabel("CUT", 4, 26, (state.synthGridRow == 1 && state.synthGridCol == 0), 19);
    DrawLeftLabel("RES", 32, 26, (state.synthGridRow == 1 && state.synthGridCol == 1), 19);
    DrawLeftLabel("TYP", 60, 26, (state.synthGridRow == 1 && state.synthGridCol == 2), 19);
    DrawLeftLabel("DPT", 94, 26, (state.synthGridRow == 1 && state.synthGridCol == 3), 19);

    bool lFDpt = false;
    int effFDpt = GetEffectiveVal(sp.filterEnvDepth, trk.filterEnvDepth, 94, 26, lFDpt);
    DrawDepthWedgeGraphic(effFDpt, 97, 11, 24, 14, (state.synthGridRow == 1 && state.synthGridCol == 3), WHITE);

    // A, D, S, R are squeezed leftward to clear Column 3
    DrawLeftLabel("A", 12, 56, (state.synthGridRow == 3 && state.synthGridCol == 0), 7);
    DrawLeftLabel("D", 36, 56, (state.synthGridRow == 3 && state.synthGridCol == 1), 7);
    DrawLeftLabel("S", 60, 56, (state.synthGridRow == 3 && state.synthGridCol == 2), 7);
    DrawLeftLabel("R", 84, 56, (state.synthGridRow == 3 && state.synthGridCol == 3), 7);

    // Draw VOY (Voice Count) and PRT (Portamento Glide) Stacked block (Row 3, Col 4)
        bool voyPrtSelected = (state.synthGridRow == 3 && state.synthGridCol == 4);
        bool isShiftHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

        // Resolve Voice effective value
        bool lVoy = false;
        int effVoy = GetEffectiveVal(sp.polyMode, trk.polyMode, 114, 39, lVoy);
        if (effVoy < 1) effVoy = 1;
        if (effVoy > 4) effVoy = 4;

        // Resolve Glide effective value
        bool lPrt = false;
        int effPrt = GetEffectiveVal(sp.glideTime, trk.glideTime, 114, 49, lPrt);

        // Draw clean bounding box outline around the entire block when focused
        if (voyPrtSelected) {
            DrawPixelRectLines(92, 36, 33, 21, WHITE);
        }

    // 1. Draw Voice Limit (VOY)
        Draw3x5String("VOY:", 94, 39, WHITE);
        // VOY is highlighted whenever focused (with or without Shift)
        if (voyPrtSelected) {
            DrawRectangle(113, 38, 11, 7, WHITE);
            Draw3x5Char('0' + effVoy, 114, 39, BLACK);
        } else {
            Draw3x5Char('0' + effVoy, 114, 39, WHITE);
        }

        // 2. Draw Portamento Glide (PRT)
        Draw3x5String("PRT:", 94, 49, WHITE);
        // PRT is highlighted ONLY when focused AND Shift is held
        if (effPrt == 0) {
            if (voyPrtSelected && isShiftHeld) {
                DrawRectangle(113, 48, 11, 7, WHITE);
                Draw3x5String("--", 114, 49, BLACK);
            } else {
                Draw3x5String("--", 114, 49, WHITE);
            }
        } else {
            char prtBuf[8];
            snprintf(prtBuf, sizeof(prtBuf), "%02d", effPrt);
            if (voyPrtSelected && isShiftHeld) {
                DrawRectangle(113, 48, 11, 7, WHITE);
                Draw3x5String(prtBuf, 114, 49, BLACK);
            } else {
                Draw3x5String(prtBuf, 114, 49, WHITE);
            }
        }

    // Spacious LFO Layout on the Right Half of the screen (x = 128 to 255)
    auto DrawLfoRow = [&](int rowNum, int yPos, int lfoWave, int lfoSpeed, int lfoDepth, int lfoTrig, int lfoSync, const char* modDesc) {
        // Offset visual column checks by 1 if processing LFO 2 (Row 3)
        int colOffset = (rowNum == 3) ? 1 : 0;

        bool wSelected = (state.synthGridRow == rowNum && state.synthGridCol == 4 + colOffset);
        bool sSelected = (state.synthGridRow == rowNum && state.synthGridCol == 5 + colOffset);
        bool dSelected = (state.synthGridRow == rowNum && state.synthGridCol == 6 + colOffset);
        bool tSelected = (state.synthGridRow == rowNum && state.synthGridCol == 7 + colOffset);
        bool sySelected = (state.synthGridRow == rowNum && state.synthGridCol == 8 + colOffset);

        // --- FETCH EFFECTIVE VALUE LOCKS ---
        bool lLWave = false, lLSpeed = false, lLDepth = false, lLTrig = false, lLSync = false;
        int effLWave = (rowNum == 1) ? GetEffectiveVal(sp.lfo1Wave, lfoWave, 134, yPos + 10, lLWave) : GetEffectiveVal(sp.lfo2Wave, lfoWave, 134, yPos + 10, lLWave);
        int effLSpeed = (rowNum == 1) ? GetEffectiveVal(sp.lfo1Speed, lfoSpeed, 159, yPos + 10, lLSpeed) : GetEffectiveVal(sp.lfo2Speed, lfoSpeed, 159, yPos + 10, lLSpeed);
        int effLDepth = (rowNum == 1) ? GetEffectiveVal(sp.lfo1Depth, lfoDepth, 185, yPos + 10, lLDepth) : GetEffectiveVal(sp.lfo2Depth, lfoDepth, 185, yPos + 10, lLDepth);
        int effLTrig = (rowNum == 1) ? GetEffectiveVal(sp.lfo1Trigger, lfoTrig, 211, yPos + 10, lLTrig) : GetEffectiveVal(sp.lfo2Trigger, lfoTrig, 211, yPos + 10, lLTrig);
        int effLSync = (rowNum == 1) ? GetEffectiveVal(sp.lfo1Sync, lfoSync, 229, yPos + 10, lLSync) : GetEffectiveVal(sp.lfo2Sync, lfoSync, 229, yPos + 10, lLSync);

        if (wSelected) DrawRectangle(134, yPos, 16, 8, WHITE);
        if (effLWave == 0)      Draw3x5String("SIN", 135, yPos + 1, wSelected ? BLACK : WHITE);
                else if (effLWave == 1) Draw3x5String("TRI", 135, yPos + 1, wSelected ? BLACK : WHITE);
                else if (effLWave == 2) Draw3x5String("SAW", 135, yPos + 1, wSelected ? BLACK : WHITE);
                else if (effLWave == 3) Draw3x5String("SQR", 135, yPos + 1, wSelected ? BLACK : WHITE);
                else if (effLWave == 4) Draw3x5String("S&H", 135, yPos + 1, wSelected ? BLACK : WHITE);
                else                    Draw3x5String("SEQ", 135, yPos + 1, wSelected ? BLACK : WHITE); // ADDED: "SEQ" label [1]

        DrawOctagonKnob(168, yPos + 4, effLSpeed / 99.0f, sSelected, WHITE);
        
        DrawDepthWedgeGraphic(effLDepth, 186, yPos, 16, 8, dSelected, WHITE);

        DrawPixelRectLines(213, yPos, 10, 8, WHITE);
        if (effLTrig) DrawRectangle(215, yPos + 2, 6, 4, WHITE);

        DrawPixelRectLines(231, yPos, 10, 8, WHITE);
        if (effLSync) DrawRectangle(233, yPos + 2, 6, 4, WHITE);

        auto DrawMidLabel = [&](const char* lbl, int lx, int ly, bool selected, int boxW) {
            if (selected) {
                DrawRectangle(lx - 1, ly - 1, boxW, 7, WHITE);
                Draw3x5String(lbl, lx, ly, BLACK);
            } else {
                Draw3x5String(lbl, lx, ly, WHITE);
            }
        };

        DrawMidLabel("WAV", 134, yPos + 10, wSelected, 18);
        DrawMidLabel("SPD", 159, yPos + 10, sSelected, 18);
        DrawMidLabel("DPT", 185, yPos + 10, dSelected, 18);
        DrawMidLabel("TRG", 211, yPos + 10, tSelected, 18);
        DrawMidLabel("SYN", 229, yPos + 10, sySelected, 18);

        bool routingSelected = false;
        if (rowNum == 1 && state.synthGridRow == 2) routingSelected = true;
        if (rowNum == 3 && state.synthGridRow == 4) routingSelected = true;

        if (routingSelected) {
            DrawRectangle(132, yPos + 16, 121, 7, WHITE);
            Draw3x5String(modDesc, 134, yPos + 17, BLACK);
        } else {
            Draw3x5String(modDesc, 134, yPos + 17, WHITE);
        }
    }; // Semicolon explicitly closes DrawLfoRow

    std::string mod1Text = "LFO1MOD:TR " + std::to_string(state.selectedTrack + 1) + " C T";
    std::string mod2Text = "LFO2MOD:TR " + std::to_string(state.selectedTrack + 1) + " M2";

    DrawLfoRow(1, 10, trk.lfo1Wave, trk.lfo1Speed, trk.lfo1Depth, trk.lfo1Trigger, trk.lfo1Sync, mod1Text.c_str());
    DrawLfoRow(3, 37, trk.lfo2Wave, trk.lfo2Speed, trk.lfo2Depth, trk.lfo2Trigger, trk.lfo2Sync, mod2Text.c_str());
}// Complete main rendering engine for Page 5: GLOBAL MASTER EFFECTS
void DrawGlobalFXPage(const UIState& state) {
    std::string patStr = GetPatternHeaderString();
    Draw5x5String(("GLOBAL MASTER FX " + patStr).c_str(), 2, 1, WHITE); // Draws clean header

    // Standardized Step Indicator
    bool isAltHeld = IsKeyDown(KEY_X);
    int displayStep = isAltHeld ? (state.cursorStep + 1) : (state.playhead + 1);
    std::string stepStr = "STEP: " + std::to_string(displayStep);
    Draw5x5String(stepStr.c_str(), 168, 1, WHITE);

    std::string bpmStr = std::to_string((int)state.tempo) + "BPM";
    Draw5x5String(bpmStr.c_str(), 218, 1, WHITE);

    // Symmetrical vertical columns dividers
    DrawPixelLine(64, 7, 64, OLED_HEIGHT, WHITE);
    DrawPixelLine(128, 7, 128, OLED_HEIGHT, WHITE);
    DrawPixelLine(192, 7, 192, OLED_HEIGHT, WHITE);

    // Loop over the 4 master blocks (Width of each column = 64 pixels)
    for (int col = 0; col < 4; ++col) {
        int baseX = col * 64;
        
        // Pass max value (99) to force static fully drawn peak icons!
        if (col == 0)      DrawWavesIcon(baseX + 22, 11, 99, WHITE);
        else if (col == 1) DrawSunIcon(baseX + 22, 11, 99, WHITE);
        else if (col == 2) DrawRainIcon(baseX + 22, 11, 99, WHITE);
        else               DrawTornadoIcon(baseX + 22, 11, 99, WHITE);

        // Helper lambda to procedurally render sliders & selection labels
        auto DrawFXControl = [&](const char* label, int val, int minVal, int maxVal, int gridX, int gridY, int xOffset) {
            bool selected = (state.synthGridRow == gridY && state.synthGridCol == gridX);
            int sx = baseX + xOffset;
            
            // Spaced out vertical coordinates completely eliminates text overlapping:
            // Row 1 Slider at y=28, Label at y=35
            // Row 2 Slider at y=44, Label at y=51
            int sy = (gridY == 1) ? 28 : 44;
            
            // Draw compact slider with showValue = false to remove numerical overlays
            DrawSlider(val, minVal, maxVal, sx, sy, 21, selected, false, WHITE);
            
            // Render selection label
            int lx = sx + 10 - (int)(strlen(label) * 2);
            if (selected) {
                DrawRectangle(lx - 1, sy + 6, (int)(strlen(label) * 4) + 1, 7, WHITE);
                Draw3x5String(label, lx, sy + 7, BLACK);
            } else {
                Draw3x5String(label, lx, sy + 7, WHITE);
            }
        };

        // Render parameters contextually per column
        if (col == 0) {
            DrawFXControl("DEC", globalFX.reverbDecay, 0, 99, 0, 1, 6);
            DrawFXControl("SIZ", globalFX.reverbSize, 0, 99, 1, 1, 36);
            DrawFXControl("PRE", globalFX.reverbPredelay, 0, 99, 0, 2, 6);
            DrawFXControl("MIX", globalFX.reverbMix, 0, 99, 1, 2, 36);
        } else if (col == 1) {
            DrawFXControl("SAT", globalFX.satLevel, 0, 99, 2, 1, 6);
            DrawFXControl("SYM", globalFX.satSymmetry, 0, 99, 3, 1, 36);
            DrawFXControl("TGT",  globalFX.satOverdrive, 0, 99, 2, 2, 6);
            DrawFXControl("MIX", globalFX.satMix, 0, 99, 3, 2, 36);
        } else if (col == 2) {
            DrawFXControl("TIM", globalFX.delayTime, 0, 99, 4, 1, 6);
            DrawFXControl("FB",  globalFX.delayFeedback, 0, 99, 5, 1, 36);
            DrawFXControl("PNG", globalFX.delayPingPong, 0, 1, 4, 2, 6);
            DrawFXControl("MIX", globalFX.delayMix, 0, 99, 5, 2, 36);
        } else {
                    DrawFXControl("TIM", globalFX.autoPanTime, 0, 99, 6, 1, 6);
                    DrawFXControl("FB",  globalFX.autoPanFeedback, 0, 99, 7, 1, 36);
                    DrawFXControl("WDT", globalFX.autoPanWidth, 0, 99, 6, 2, 6);
                    DrawFXControl("MIX", globalFX.autoPanMix, 0, 99, 7, 2, 36);
                }
            }
        }
void DrawPlaceholderPage(const UIState& state) {
    // 1. Header
    std::string headerTrack = "TRACK " + std::to_string(state.selectedTrack + 1) + " " + GetPatternHeaderString();
    Draw5x5String(headerTrack.c_str(), 2, 1, WHITE);

    // Standardized Step Indicator (Declared once at the top)
    bool isAltHeld = IsKeyDown(KEY_X);
    int displayStep = isAltHeld ? (state.cursorStep + 1) : (state.playhead + 1);
    std::string stepStr = "STEP: " + std::to_string(displayStep);
    Draw5x5String(stepStr.c_str(), 168, 1, WHITE);

    std::string bpmStr = std::to_string((int)state.tempo) + "BPM";
    Draw5x5String(bpmStr.c_str(), 218, 1, WHITE);

    // 2. Draw Left-Hand Side Tape Buffer Panel (x = 0 to 173)
    DrawPixelRectLines(3, 11, 169, 48, WHITE);

    const Track& trk = tracks[state.selectedTrack];
    const StepParams& sp = trk.steps[state.cursorStep].params;

    auto GetEffectiveVal = [&](int stepVal, int trackVal, int drawX, int drawY, bool& isLocked) {
        if (!isAltHeld) {
            isLocked = false;
            return trackVal;
        }
        if (stepVal == -1) {
            isLocked = false;
            return trackVal;
        }
        isLocked = true;
        DrawRectangle(drawX, drawY, 2, 2, WHITE); // 2x2 dot
        return stepVal;
    };

    auto DrawEncoder = [&](const char* label, int val, int minVal, int maxVal, int cx, int cy, int gridCol, int gridRow, bool locked) {
        bool selected = (state.synthGridRow == gridRow && state.synthGridCol == gridCol);
        
        // Draw the octagonal knob representing encoder state
        float norm = (float)(val - minVal) / (maxVal - minVal);
        DrawOctagonKnob(cx, cy, norm, selected, WHITE);

        // Draw selection label box and 3x5 text
        int lx = cx - 5;
        int ly = (gridRow == 1) ? 26 : 53;
        
        if (selected) {
            DrawRectangle(lx - 1, ly - 1, 13, 7, WHITE);
            Draw3x5String(label, lx, ly, BLACK);
        } else {
            Draw3x5String(label, lx, ly, WHITE);
        }
    };

    // Row 1 Encoders (TAPE FX Page 4, Columns 0 to 6)
    bool lMem = false, lHds = false, lSpr = false, lSpd = false, lTet = false, lDrf = false, lDrt = false;
    int effMem  = GetEffectiveVal(sp.tapeMemory, trk.tapeMemory, 14, 18, lMem);
    int effHds  = GetEffectiveVal(sp.tapeHeads, trk.tapeHeads, 38, 18, lHds);
    int effSpr  = GetEffectiveVal(sp.tapeSpread, trk.tapeSpread, 62, 18, lSpr);
    int effSpd  = GetEffectiveVal(sp.tapeSpeed, trk.tapeSpeed, 86, 18, lSpd);
    int effTet  = GetEffectiveVal(sp.tapeTether, trk.tapeTether, 110, 18, lTet);
    int effDrf  = GetEffectiveVal(sp.tapeDrift, trk.tapeDrift, 134, 18, lDrf);
    int effDrt  = GetEffectiveVal(sp.tapeDriftRate, trk.tapeDriftRate, 158, 18, lDrt);

    DrawEncoder("MEM", effMem, 0, 99, 14, 18, 0, 1, lMem);
    DrawEncoder("HDS", effHds, 1, 4, 38, 18, 1, 1, lHds);
    DrawEncoder("SPR", effSpr, 0, 99, 62, 18, 2, 1, lSpr);
    DrawEncoder("SPD", effSpd, 0, 99, 86, 18, 3, 1, lSpd);
    DrawEncoder("TET", effTet, 0, 99, 110, 18, 4, 1, lTet);
    DrawEncoder("DRF", effDrf, 0, 99, 134, 18, 5, 1, lDrf);
    DrawEncoder("DRT", effDrt, 0, 99, 158, 18, 6, 1, lDrt);

    // Row 2 (Indexed Row 3) Encoders (Columns 0 to 6)
    bool lFb = false, lFsp = false, lFsc = false, lFrz = false, lSmr = false, lSms = false, lMix = false;
    int effFb   = GetEffectiveVal(sp.tapeFeedback, trk.tapeFeedback, 14, 45, lFb);
    int effFsp  = GetEffectiveVal(sp.tapeFbSpread, trk.tapeFbSpread, 38, 45, lFsp);
    int effFsc  = GetEffectiveVal(sp.tapeFbSource, trk.tapeFbSource, 62, 45, lFsc);
    int effFrz  = GetEffectiveVal(sp.tapeFreeze, trk.tapeFreeze, 86, 45, lFrz);
    int effSmr  = GetEffectiveVal(sp.tapeSmearRate, trk.tapeSmearRate, 110, 45, lSmr);
    int effSms  = GetEffectiveVal(sp.tapeSmearSize, trk.tapeSmearSize, 134, 45, lSms);
    int effMix  = GetEffectiveVal(sp.tapeMix, trk.tapeMix, 158, 45, lMix);

    DrawEncoder("FDB", effFb, 0, 99, 14, 45, 0, 3, lFb);
    DrawEncoder("FSP", effFsp, 0, 99, 38, 45, 1, 3, lFsp);
    DrawEncoder("FSC", effFsc, 0, 99, 62, 45, 2, 3, lFsc);
    DrawEncoder("FRZ", effFrz, 0, 99, 86, 45, 3, 3, lFrz);
    DrawEncoder("SMR", effSmr, 0, 99, 110, 45, 4, 3, lSmr);
    DrawEncoder("SMS", effSms, 0, 99, 134, 45, 5, 3, lSms);
    DrawEncoder("MIX", effMix, 0, 99, 158, 45, 6, 3, lMix);

    // 3. Draw Right-Hand Side FX Sends Panel (x = 174 to 255)
    DrawPixelLine(174, 7, 174, OLED_HEIGHT, WHITE);

    // Columns are mapped to 7 (Left FX column) and 8 (Right FX column) on Page 4
    bool revSelected = (state.synthGridRow == 1 && state.synthGridCol == 7);
    bool delSelected = (state.synthGridRow == 1 && state.synthGridCol == 8);
    bool satSelected = (state.synthGridRow == 3 && state.synthGridCol == 7);
    bool panSelected = (state.synthGridRow == 3 && state.synthGridCol == 8);

    bool lRev = false, lDel = false, lSat = false, lPan = false;
    int effRev = GetEffectiveVal(sp.reverbSend, trk.reverbSend, 176, 10, lRev);
    int effDel = GetEffectiveVal(sp.delaySend, trk.delaySend, 216, 10, lDel);
    int effSat = GetEffectiveVal(sp.saturationSend, trk.saturationSend, 176, 33, lSat);
    int effPan = GetEffectiveVal(sp.autoPanSend, trk.autoPanSend, 216, 33, lPan);

    // Render static and animated structures
    if (revSelected) DrawPixelRectLines(176, 10, 38, 17, WHITE);
    DrawWavesIcon(176 + 9, 10 + 3, effRev, WHITE);

    if (delSelected) DrawPixelRectLines(216, 10, 38, 17, WHITE);
    DrawRainIcon(216 + 9, 10 + 3, effDel, WHITE);

    if (satSelected) DrawPixelRectLines(176, 33, 38, 17, WHITE);
    DrawSunIcon(176 + 9, 33 + 3, effSat, WHITE);

    if (panSelected) DrawPixelRectLines(216, 33, 38, 17, WHITE);
    DrawTornadoIcon(216 + 9, 33 + 3, effPan, WHITE);

    Draw5x5String("FX SENDS", 191, 55, WHITE);
}
        // Modal 4-state System Menu (0=Main, 1=Browser, 2=Save Slot Select, 3=Typing)
        void DrawSystemMenu(const UIState& state) {
            int mx = 48, my = 8, mw = 160, mh = 48;

            // Draw high-contrast black backplane
            DrawRectangle(mx, my, mw, mh, BLACK);
            DrawPixelRectLines(mx, my, mw, mh, WHITE);

            if (state.systemMenuState == 0) {
                // ==========================================
                // SUBMENU 0: MAIN SYSTEM DIRECTORY LIST
                // ==========================================
                Draw5x5String("SYSTEM MENU", mx + 48, my + 4, WHITE);
                DrawPixelLine(mx + 4, my + 11, mx + mw - 5, my + 11, WHITE);

                const char* menuItems[] = {
                    "SAVE PROJECT",
                    "LOAD PROJECT",
                    "SAVE ACTIVE PATTERN",
                    "LOAD ACTIVE PATTERN",
                    "SAVE ACTIVE SOUND",
                    "LOAD ACTIVE SOUND",
                    "IMPORT SAMPLE TO POOL"
                };

                int startIdx = 0;
                if (state.systemMenuCursor >= 3) {
                    startIdx = state.systemMenuCursor - 2;
                }
                if (startIdx > 4) startIdx = 4; // Clamped to 4 to allow 7th item to display

                for (int i = 0; i < 3; ++i) {
                    int itemIdx = startIdx + i;
                    int yPos = my + 15 + i * 10;
                    
                    bool selected = (state.systemMenuCursor == itemIdx);
                    if (selected) {
                        DrawRectangle(mx + 4, yPos - 1, mw - 8, 8, WHITE);
                        Draw3x5String("> ", mx + 8, yPos, BLACK);
                        Draw3x5String(menuItems[itemIdx], mx + 16, yPos, BLACK);
                    } else {
                        Draw3x5String(menuItems[itemIdx], mx + 16, yPos, WHITE);
                    }
                }
            }
            else if (state.systemMenuState == 1) {
                // ==========================================
                // SUBMENU 1: FILE BROWSER (Loading named files)
                // ==========================================
                std::string dirTitle = "SELECT FILE";
                if (state.systemMenuCursor == 1)      dirTitle = "LOAD PROJECT";
                else if (state.systemMenuCursor == 3) dirTitle = "LOAD PATTERN";
                else if (state.systemMenuCursor == 5) dirTitle = "LOAD PRESET";
                else if (state.systemMenuCursor == 6) dirTitle = "IMPORT SAMPLE";

                Draw5x5String(dirTitle.c_str(), mx + 10, my + 4, WHITE);
                DrawPixelLine(mx + 4, my + 11, mx + mw - 5, my + 11, WHITE);

                // Scan directories cleanly mapped to your menu items
                std::string targetDir = "projects";
                std::string targetExt = ".prj";

                if (state.systemMenuCursor == 1)      { targetDir = "projects"; targetExt = ".prj"; }
                else if (state.systemMenuCursor == 3) { targetDir = "patterns"; targetExt = ".pat"; }
                else if (state.systemMenuCursor == 5) { targetDir = "presets";  targetExt = ".snd"; }
                else if (state.systemMenuCursor == 6) { targetDir = "samples";  targetExt = ".wav"; }

                std::vector<std::string> fileList = GetFileList(targetDir, targetExt);

                if (fileList.empty()) {
                    Draw3x5String("NO FILES FOUND", mx + 38, my + 22, WHITE);
                } else {
                    int startIdx = 0;
                    if (state.fileBrowserCursor >= 3) {
                        startIdx = state.fileBrowserCursor - 2;
                    }
                    int maxStart = (int)fileList.size() - 3;
                    if (maxStart < 0) maxStart = 0;
                    if (startIdx > maxStart) startIdx = maxStart;

                    int drawCount = std::min((int)fileList.size(), 3);
                    for (int i = 0; i < drawCount; ++i) {
                        int itemIdx = startIdx + i;
                        int yPos = my + 15 + i * 10;
                        
                        bool selected = (state.fileBrowserCursor == itemIdx);
                        if (selected) {
                            DrawRectangle(mx + 4, yPos - 1, mw - 8, 8, WHITE);
                            std::string rawName = fileList[itemIdx];
                            std::string cleanName = rawName;
                            if (rawName.rfind("slot_", 0) == 0) {
                                size_t firstUnderscore = rawName.find('_');
                                size_t secondUnderscore = rawName.find('_', firstUnderscore + 1);
                                if (secondUnderscore != std::string::npos) {
                                    std::string slotNumStr = rawName.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1);
                                    int slotNum = std::stoi(slotNumStr);
                                    std::string customName = rawName.substr(secondUnderscore + 1);
                                    
                                    char slotBuf[32];
                                    snprintf(slotBuf, sizeof(slotBuf), "%02d SLOT (%s)", slotNum, customName.c_str());
                                    cleanName = slotBuf;
                                    // Render Visual Scrollbar on Right Margin
                                                        if (fileList.size() > 3) {
                                                            int sbX = mx + mw - 7;
                                                            int sbY = my + 14;
                                                            int sbH = 26; // Height of scrollbar track area

                                                            // Draw background track
                                                            DrawPixelLine(sbX, sbY, sbX, sbY + sbH, GRAY);

                                                            // Calculate indicator handle height and position dynamically
                                                            float percentWindow = 3.0f / (float)fileList.size();
                                                            int handleH = (int)(sbH * percentWindow);
                                                            if (handleH < 4) handleH = 4; // Minimum handle size

                                                            float percentScroll = (float)state.fileBrowserCursor / (float)(fileList.size() - 1);
                                                            int handleY = sbY + (int)(percentScroll * (sbH - handleH));

                                                            // Draw sliding handle block
                                                            DrawRectangle(sbX - 1, handleY, 3, handleH, WHITE);
                                                        }
                                }
                            }

                            Draw3x5String("> ", mx + 8, yPos, BLACK);
                            Draw3x5String(cleanName.c_str(), mx + 16, yPos, BLACK);
                        } else {
                            std::string rawName = fileList[itemIdx];
                            std::string cleanName = rawName;
                            if (rawName.rfind("slot_", 0) == 0) {
                                size_t firstUnderscore = rawName.find('_');
                                size_t secondUnderscore = rawName.find('_', firstUnderscore + 1);
                                if (secondUnderscore != std::string::npos) {
                                    std::string slotNumStr = rawName.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1);
                                    int slotNum = std::stoi(slotNumStr);
                                    std::string customName = rawName.substr(secondUnderscore + 1);
                                    
                                    char slotBuf[32];
                                    snprintf(slotBuf, sizeof(slotBuf), "%02d SLOT (%s)", slotNum, customName.c_str());
                                    cleanName = slotBuf;
                                }
                            }
                            Draw3x5String(cleanName.c_str(), mx + 16, yPos, WHITE);
                        }
                    }
                }
            }
            else if (state.systemMenuState == 2) {
                // ==========================================
                // SUBMENU 2: SAVE/POOL SLOT SELECTOR
                // ==========================================
                std::string dirTitle = "SELECT SLOT";
                if (state.systemMenuCursor == 0)      dirTitle = "SAVE PROJECT";
                else if (state.systemMenuCursor == 2) dirTitle = "SAVE PATTERN";
                else if (state.systemMenuCursor == 4) dirTitle = "SAVE PRESET";
                else if (state.systemMenuCursor == 6) dirTitle = "SELECT POOL SLOT";

                Draw5x5String(dirTitle.c_str(), mx + 10, my + 4, WHITE);
                DrawPixelLine(mx + 4, my + 11, mx + mw - 5, my + 11, WHITE);

                // Fetch slot limit parameters based on context
                int maxSlots = (state.systemMenuCursor == 0) ? 16 : ((state.systemMenuCursor == 2) ? 128 : 1024);
                if (state.systemMenuCursor == 6) maxSlots = 16; // 16 sample pool slots

                std::string targetDir = (state.systemMenuCursor == 0) ? "projects" : ((state.systemMenuCursor == 2) ? "patterns" : "presets");
                std::string targetExt = (state.systemMenuCursor == 0) ? ".prj" : ((state.systemMenuCursor == 2) ? ".pat" : ".snd");

                int startIdx = 0;
                if (state.fileBrowserCursor >= 3) {
                    startIdx = state.fileBrowserCursor - 2;
                }
                int maxStart = maxSlots - 3;
                if (startIdx > maxStart) startIdx = maxStart;

                for (int i = 0; i < 3; ++i) {
                    int slotIdx = startIdx + i;
                    int yPos = my + 15 + i * 10;
                    
                    bool selected = (state.fileBrowserCursor == slotIdx);
                    
                    char slotBuf[64];
                    if (state.systemMenuCursor == 6) {
                        // Display our live Sample Pool slot name in RAM [2]
                        snprintf(slotBuf, sizeof(slotBuf), "%02d POOL (%s)", slotIdx + 1, g_samplePool[slotIdx].name.c_str());
                    } else {
                        std::string customName = GetSlotName(targetDir, slotIdx + 1, targetExt);
                        snprintf(slotBuf, sizeof(slotBuf), "%02d SLOT (%s)", slotIdx + 1, customName.c_str());
                    }

                    if (selected) {
                        DrawRectangle(mx + 4, yPos - 1, mw - 8, 8, WHITE);
                        Draw3x5String("> ", mx + 8, yPos, BLACK);
                        Draw3x5String(slotBuf, mx + 16, yPos, BLACK);
                    } else {
                        Draw3x5String(slotBuf, mx + 16, yPos, WHITE);
                    }
                }
            }
            else if (state.systemMenuState == 3) {
                // ==========================================
                // SUBMENU 3: TEXT TYPING (Entering File Name)
                // ==========================================
                std::string dirTitle = "ENTER NAME";
                if (state.systemMenuCursor == 0)      dirTitle = "SAVE PROJECT";
                else if (state.systemMenuCursor == 2) dirTitle = "SAVE PATTERN";
                else if (state.systemMenuCursor == 4) dirTitle = "SAVE PRESET";

                Draw5x5String(dirTitle.c_str(), mx + 10, my + 4, WHITE);
                DrawPixelLine(mx + 4, my + 11, mx + mw - 5, my + 11, WHITE);

                char slotLabel[64];
                snprintf(slotLabel, sizeof(slotLabel), "SLOT %02d NAME:", state.fileBrowserCursor + 1);
                Draw3x5String(slotLabel, mx + 10, my + 18, WHITE);

                // Render input text field box
                int fieldX = mx + 10, fieldY = my + 26, fieldW = mw - 20, fieldH = 10;
                DrawPixelRectLines(fieldX, fieldY, fieldW, fieldH, WHITE);
                
                // Draw typed string (Uppercase alphabet is fully supported in 3x5 font now)
                                Draw3x5String(state.typingBuffer, fieldX + 4, fieldY + 3, WHITE);

                                // Draw high-visibility underline cursor under the active editing character slot
                                if (state.blinkOn) {
                                    int cursorOffset = fieldX + 4 + (state.typingCursor * 4);
                                    // Draw a 3-pixel wide line directly under the active slot inside the field box
                                    DrawPixelLine(cursorOffset, fieldY + 8, cursorOffset + 2, fieldY + 8, WHITE);
                                }
            }

            // Render interactive bottom toast confirmation message
            if (state.menuFeedback != nullptr && strlen(state.menuFeedback) > 0) {
                int textWidth = (int)(strlen(state.menuFeedback) * 4) - 1;
                DrawRectangle(mx + 4, my + mh - 9, mw - 8, 8, BLACK); // Masking band
                Draw3x5String(state.menuFeedback, mx + (mw / 2) - (textWidth / 2), my + mh - 8, WHITE);
            }
        }
// Modal Modulation Matrix Popup Menu Renderer (Fits comfortably within 256x64 canvas)
void DrawModulationPopup(const UIState& state) {
    int mx = 20, my = 5, mw = 216, mh = 54;

    // Draw high-contrast black backplane and white borders
    DrawRectangle(mx, my, mw, mh, BLACK);
    DrawPixelRectLines(mx, my, mw, mh, WHITE);

    // Title Header
    char titleBuf[64];
    snprintf(titleBuf, sizeof(titleBuf), "LFO %d MODULATION ROUTING", lfoPopupLfoIdx + 1);
    Draw5x5String(titleBuf, mx + 20, my + 4, WHITE);
    DrawPixelLine(mx + 4, my + 11, mx + mw - 5, my + 11, WHITE);

    const Track& trk = tracks[state.selectedTrack];

    for (int i = 0; i < 3; ++i) {
        // Read active slot routing properties
        const ModSlot& m = (lfoPopupLfoIdx == 0) ? trk.lfo1Slots[i] : trk.lfo2Slots[i];
        int yPos = my + 15 + i * 11;

        bool isRowFocused = (lfoPopupSlot == i);

        // Display Index Number
        char numBuf[16];
        snprintf(numBuf, sizeof(numBuf), "%d.", i + 1);
        Draw3x5String(numBuf, mx + 8, yPos, WHITE);

        // --- FIELD 0: TARGET DESTINATION TYPE ---
        std::string typeStr = "OFF  ";
        if (m.destType == 1)      typeStr = "VOICE";
        else if (m.destType == 2) typeStr = "G.FX ";

        bool f0Selected = (isRowFocused && lfoPopupField == 0);
        if (f0Selected) {
            DrawRectangle(mx + 20, yPos - 1, 23, 7, WHITE);
            Draw3x5String(typeStr.c_str(), mx + 21, yPos, BLACK);
        } else {
            Draw3x5String(typeStr.c_str(), mx + 21, yPos, WHITE);
        }

        // --- FIELD 1: TARGET TRACK INDEX ---
        std::string trkStr = " -   ";
        if (m.destType == 1) {
            trkStr = "TRK" + std::to_string(m.destTrack + 1);
        }

        bool f1Selected = (isRowFocused && lfoPopupField == 1);
        if (m.destType == 1) {
            if (f1Selected) {
                DrawRectangle(mx + 48, yPos - 1, 22, 7, WHITE);
                Draw3x5String(trkStr.c_str(), mx + 49, yPos, BLACK);
            } else {
                Draw3x5String(trkStr.c_str(), mx + 49, yPos, WHITE);
            }
        } else {
            Draw3x5String(" - ", mx + 49, yPos, DARKGRAY);
        }

        // --- FIELD 2: DESTINATION PARAMETER ---
        std::string paramStr = "NONE   ";
        if (m.destType == 1) {
            if (m.destParam == DEST_CUTOFF)         paramStr = "CUTOFF ";
            else if (m.destParam == DEST_RESONANCE) paramStr = "RES    ";
            else if (m.destParam == DEST_VOLUME)    paramStr = "VOLUME ";
            else if (m.destParam == DEST_MORPH1)    paramStr = "MORPH1 ";
            else if (m.destParam == DEST_MORPH2)    paramStr = "MORPH2 ";
            else if (m.destParam == DEST_PITCH)     paramStr = "PITCH  ";
            else if (m.destParam == DEST_DECAY)     paramStr = "DECAY  ";
                        else if (m.destParam == DEST_VOLUME2)   paramStr = "VOL2/FM";
                        else if (m.destParam == DEST_SAMP_START) paramStr = "SMP_STR"; // ADDED [1]
                        else if (m.destParam == DEST_GRAN_SIZE)  paramStr = "GR_SIZE"; // ADDED [1]
                        else if (m.destParam == DEST_GRAN_DENS)  paramStr = "GR_DENS"; // ADDED [1]
                        else if (m.destParam == DEST_GRAN_SCAT)  paramStr = "GR_SCAT"; // ADDED [1]
                        else if (m.destParam == DEST_FINE1)     paramStr = "FINE1  ";
                        else if (m.destParam == DEST_FINE2)     paramStr = "FINE2  ";
                        else if (m.destParam == DEST_SAMP_POS)  paramStr = "POS    ";
                        else if (m.destParam == DEST_TAPE_MEM)  paramStr = "T.MEM  ";
            else if (m.destParam == DEST_TAPE_HDS)  paramStr = "T.HDS  ";
            else if (m.destParam == DEST_TAPE_SPR)  paramStr = "T.SPR  ";
            else if (m.destParam == DEST_TAPE_SPD)  paramStr = "T.SPD  ";
            else if (m.destParam == DEST_TAPE_TET)  paramStr = "T.TET  ";
            else if (m.destParam == DEST_TAPE_DRF)  paramStr = "T.DRF  ";
            else if (m.destParam == DEST_TAPE_DRT)  paramStr = "T.DRT  ";
            else if (m.destParam == DEST_TAPE_FDB)  paramStr = "T.FDB  ";
            else if (m.destParam == DEST_TAPE_FSP)  paramStr = "T.FSP  ";
            else if (m.destParam == DEST_TAPE_FSC)  paramStr = "T.FSC  ";
            else if (m.destParam == DEST_TAPE_FRZ)  paramStr = "T.FRZ  ";
            else if (m.destParam == DEST_TAPE_SMR)  paramStr = "T.SMR  ";
            else if (m.destParam == DEST_TAPE_SMS)  paramStr = "T.SMS  ";
            else if (m.destParam == DEST_TAPE_MIX)  paramStr = "T.MIX  ";
        } else if (m.destType == 2) {
            if (m.destParam == DEST_REV_MIX)        paramStr = "REV.MIX";
            else if (m.destParam == DEST_DEL_MIX)   paramStr = "DEL.MIX";
            else if (m.destParam == DEST_SAT_MIX)   paramStr = "SAT.MIX";
            else if (m.destParam == DEST_PAN_MIX)   paramStr = "PAN.MIX";
        }

        bool f2Selected = (isRowFocused && lfoPopupField == 2);
        if (m.destType != 0) {
            if (f2Selected) {
                DrawRectangle(mx + 76, yPos - 1, 34, 7, WHITE);
                Draw3x5String(paramStr.c_str(), mx + 77, yPos, BLACK);
            } else {
                Draw3x5String(paramStr.c_str(), mx + 77, yPos, WHITE);
            }
        } else {
            Draw3x5String(" - ", mx + 77, yPos, DARKGRAY);
        }

        // --- FIELD 3: BIPOLAR MODULATION DEPTH ---
        char depthBuf[16];
        if (m.destType != 0) {
            snprintf(depthBuf, sizeof(depthBuf), "%s%d", (m.depth > 0 ? "+" : ""), m.depth);
        } else {
            snprintf(depthBuf, sizeof(depthBuf), " 0");
        }

        bool f3Selected = (isRowFocused && lfoPopupField == 3);
        if (m.destType != 0) {
            if (f3Selected) {
                DrawRectangle(mx + 116, yPos - 1, 18, 7, WHITE);
                Draw3x5String(depthBuf, mx + 117, yPos, BLACK);
            } else {
                Draw3x5String(depthBuf, mx + 117, yPos, WHITE);
            }
        } else {
            Draw3x5String(" - ", mx + 117, yPos, DARKGRAY);
        }
    }
}
void DrawDiagnosticsScreen(const UIState& state) {
    // Left column divider
    DrawPixelLine(82, 7, 82, OLED_HEIGHT, WHITE);
    // Right column divider
    DrawPixelLine(166, 7, 166, OLED_HEIGHT, WHITE);

    // --- COLUMN 1: SYSTEM HEALTH METRICS ---
    Draw5x5String("DIAGNOSTICS", 4, 1, WHITE);
    
    char cpuBuf[32];
    snprintf(cpuBuf, sizeof(cpuBuf), "CPU: %4.1f%%", g_audioCpuLoad);
    Draw3x5String(cpuBuf, 4, 12, WHITE);
    
    char fpsBuf[32];
    snprintf(fpsBuf, sizeof(fpsBuf), "FPS: %d", GetFPS());
    Draw3x5String(fpsBuf, 4, 21, WHITE);

    char grnBuf[32];
    snprintf(grnBuf, sizeof(grnBuf), "GRN: %d/128", g_globalActiveGrains);
    Draw3x5String(grnBuf, 4, 30, WHITE);

    char bpmBuf[32];
    snprintf(bpmBuf, sizeof(bpmBuf), "BPM: %3d", (int)state.tempo);
    Draw3x5String(bpmBuf, 4, 39, WHITE);

    char patBuf[32];
    snprintf(patBuf, sizeof(patBuf), "PAT: %d", activePattern + 1);
    Draw3x5String(patBuf, 4, 48, WHITE);

    // Connected MIDI device display
        char midiBuf[64];
        if (g_midiManager && g_midiManager->isConnected()) {
            // Formatted with scroll arrows showing Left/Right actions are active
            snprintf(midiBuf, sizeof(midiBuf), "MIDI:<%s>", g_midiManager->getPortName().c_str());
        } else {
            snprintf(midiBuf, sizeof(midiBuf), "MIDI: OFFLINE");
        }
        Draw3x5String(midiBuf, 4, 57, WHITE);
    // --- COLUMN 2: SYNTH VOICE GRID (8x4) ---
    Draw5x5String("SYN VOICES", 88, 1, WHITE);
    for (int t = 0; t < 8; ++t) {
        int y = 11 + t * 6;
        char trkLbl[8];
        snprintf(trkLbl, sizeof(trkLbl), "T%d", t + 1);
        Draw3x5String(trkLbl, 88, y + 1, WHITE);

        for (int v = 0; v < 4; ++v) {
            int x = 104 + v * 7;
            bool active = IsSynthVoiceActive(t, v);
            if (active) {
                DrawRectangle(x, y, 4, 5, WHITE); // Filled block
            } else {
                DrawRectangleLines(x, y, 4, 5, WHITE); // Hollow block
            }
        }
    }

    // --- COLUMN 3: SAMPLER VOICE GRID (8x4) ---
    Draw5x5String("SMP VOICES", 172, 1, WHITE);
    for (int t = 0; t < 8; ++t) {
        int y = 11 + t * 6;
        char trkLbl[8];
        snprintf(trkLbl, sizeof(trkLbl), "T%d", t + 1);
        Draw3x5String(trkLbl, 172, y + 1, WHITE);

        for (int v = 0; v < 4; ++v) {
            int x = 188 + v * 7;
            bool active = IsSamplerVoiceActive(t, v);
            if (active) {
                DrawRectangle(x, y, 4, 5, WHITE); // Filled block
            } else {
                DrawRectangleLines(x, y, 4, 5, WHITE); // Hollow block
            }
        }
    }
}
// --- STEP PROPERTIES EDITING POPUP ---
void DrawStepPopup(const UIState& state) {
    // Large, centered popup frame (180x56) matching your custom layout
    int mx = 38, my = 4, mw = 180, mh = 56;
    
    DrawRectangle(mx, my, mw, mh, BLACK);
    DrawPixelRectLines(mx, my, mw, mh, WHITE);

    // Fetch the active step context safely relative to track and cursor position
    int activeTrackIdx = (state.currentScreen == SCREEN_SEQ_5_8) ? state.cursorTrack + 4 : state.cursorTrack;
    const Step& step = tracks[activeTrackIdx].steps[state.cursorStep];

    // Left and Right column boundary starting points
    int leftX = mx + 4, leftY = my + 11;
    int rightX = mx + 94, rightY = my + 11;

    // Draw central vertical dividing line separating parameters from the chord piano
    DrawPixelLine(mx + 92, my + 11, mx + 92, my + mh - 5, WHITE);

    // =========================================================================
    // I. ROW 0: RETRIG (8x2 Grid)
    // =========================================================================
    bool retrigSelected = (stepPopupOpen && stepPopupFocusX == 0 && stepPopupFocusY == 0);
    if (retrigSelected) {
        DrawRectangle(leftX + 2, leftY + 2, 30, 7, WHITE);
        Draw3x5String("RETRIG", leftX + 3, leftY + 3, BLACK);
    } else {
        Draw3x5String("RETRIG", leftX + 3, leftY + 3, WHITE);
    }

    // Draw 8x2 grid representing 16 subdivisions
    int rx = leftX + 36;
    int ry0 = leftY + 1;
    for (int p = 0; p < 16; ++p) {
        int col = p % 8;
        int row = p / 8;
        DrawPixelRectLines(rx + col * 6, ry0 + row * 6, 4, 4, WHITE);
        if (p < step.retrigger) {
            DrawRectangle(rx + col * 6, ry0 + row * 6, 4, 4, WHITE);
        }
    }

    // =========================================================================
    // II. ROW 1: CONDIT (8x2 Grid)
    // =========================================================================
    bool conditSelected = (stepPopupOpen && stepPopupFocusX == 0 && stepPopupFocusY == 1);
    if (conditSelected) {
        DrawRectangle(leftX + 2, leftY + 15, 30, 7, WHITE);
        Draw3x5String("CONDIT", leftX + 3, leftY + 16, BLACK);
    } else {
        Draw3x5String("CONDIT", leftX + 3, leftY + 16, WHITE);
    }

    // Draw 8x2 grid representing 16 custom loop bars
        int cx = leftX + 36;
        int cy0 = leftY + 14;

        // 1. Determine the selected cycle length from bits 8..15 of condMask
        int cycleLength = 1;
        for (int c = 0; c < 8; ++c) {
            if (step.condMask & (1 << (8 + c))) {
                cycleLength = c + 1;
                break;
            }
        }

        for (int b = 0; b < 16; ++b) {
            int col = b % 8;
            int row = b / 8;
            
            bool isActive = false;
            if (row == 0) {
                // Top Row: Individual loop triggers
                isActive = (step.condMask & (1 << b)) != 0;
            } else {
                // Bottom Row: Fill from the left up to the active cycle length
                isActive = (col < cycleLength);
            }

            bool isCursorOnBit = (conditSelected && stepPopupCondCol == b);

            DrawPixelRectLines(cx + col * 6, cy0 + row * 6, 4, 4, WHITE);
            if (isActive) {
                DrawRectangle(cx + col * 6 + 1, cy0 + row * 6 + 1, 2, 2, WHITE);
            }
            
            // Draw visual cursor indicator on active selected block
            if (isCursorOnBit && state.blinkOn) {
                DrawPixelRectLines(cx + col * 6 - 1, cy0 + row * 6 - 1, 6, 6, WHITE);
            }
        }

    // =========================================================================
    // III. ROW 2: NT. LNGT (8x2 Grid)
    // =========================================================================
    bool lengthSelected = (stepPopupOpen && stepPopupFocusX == 0 && stepPopupFocusY == 2);
    if (lengthSelected) {
        DrawRectangle(leftX + 2, leftY + 28, 30, 7, WHITE);
        Draw3x5String("LENGTH", leftX + 3, leftY + 29, BLACK);
    } else {
        Draw3x5String("LENGTH", leftX + 3, leftY + 29, WHITE);
    }

    // Draw 8x2 grid representing 16 step length blocks
    int lx = leftX + 36;
    int ly0 = leftY + 27;
    if (step.noteLength == 0) {
        Draw3x5String("AUTO (0.85)", lx, ly0 + 4, WHITE);
    } else {
        for (int p = 0; p < 16; ++p) {
            int col = p % 8;
            int row = p / 8;
            DrawPixelRectLines(lx + col * 6, ly0 + row * 6, 4, 4, WHITE);
            if (p < step.noteLength) {
                DrawRectangle(lx + col * 6, ly0 + row * 6, 4, 4, WHITE);
            }
        }
    }

    // =========================================================================
    // IV. RIGHT COLUMN: CHORD PIANO KEYBOARD
    // =========================================================================
    bool chordSelected = (stepPopupOpen && stepPopupFocusX == 1);
    if (chordSelected) {
        DrawRectangle(rightX + 22, rightY + 3, 34, 7, WHITE);
        Draw3x5String("CHORD", rightX + 26, rightY + 4, BLACK);
    } else {
        Draw3x5String("CHORD", rightX + 26, rightY + 4, WHITE);
    }

    // Draw active chord formula name text
    const char* chordNames[] = { "NONE", "MAJOR", "MINOR", "SUS4", "DOM7", "MAJ7", "MIN7" };
    Draw3x5String(chordNames[step.chordType], rightX + 24, rightY + 12, GRAY);

    // Keyboard Base coordinates
    int kx = rightX + 16;
    int ky = rightY + 20;
    int kh = 14;

    // White keys list: C, D, E, F, G, A, B, C+ (drawn 6-pixels wide each)
    int whiteKeysX[] = { 0, 6, 12, 18, 24, 30, 36, 42 };
    for (int w = 0; w < 8; ++w) {
        DrawPixelRectLines(kx + whiteKeysX[w], ky, 6, kh, WHITE);
    }

    // Black keys list: C# (between C/D), D# (D/E), F# (F/G), G# (G/A), A# (A/B)
    int blackKeysX[] = { 4, 10, 22, 28, 34 };
    for (int b = 0; b < 5; ++b) {
        DrawRectangle(kx + blackKeysX[b], ky, 4, 9, BLACK);
        DrawPixelRectLines(kx + blackKeysX[b], ky, 4, 9, WHITE);
    }

    // Determine active notes belonging to either preset formulas OR custom recorded chord notes
    // Locate keyboard display inside DrawStepPopup:
        std::vector<int> activeSemitones;
        std::vector<std::string> allActiveNotes;
        
        if (step.note >= 0) { // Sentinel check
            allActiveNotes.push_back(MidiToNote(step.note)); // Dynamic string conversion
        }
        for (int k = 0; k < 3; ++k) {
            if (step.chordNotes[k] >= 0) { // Sentinel check
                allActiveNotes.push_back(MidiToNote(step.chordNotes[k])); // Dynamic string conversion
            }
        }

        if (step.chordType > 0) {
            std::vector<int> chordOffsets;
            if (step.chordType == 1)      chordOffsets = {0, 4, 7};
            else if (step.chordType == 2) chordOffsets = {0, 3, 7};
            else if (step.chordType == 3) chordOffsets = {0, 5, 7};
            else if (step.chordType == 4) chordOffsets = {0, 4, 7, 10};
            else if (step.chordType == 5) chordOffsets = {0, 4, 7, 11};
            else if (step.chordType == 6) chordOffsets = {0, 3, 7, 10};

            if (step.note >= 0) { // Sentinel check
                int rootMidi = step.note; // Read value directly (no lookup!)
                for (int offset : chordOffsets) {
                    int keyIdx = ((rootMidi + offset) % 12);
                    activeSemitones.push_back(keyIdx);
                }
            }
        } else {
            // Custom played chord notes (Map relative to C)
            for (const auto& nStr : allActiveNotes) {
                int midiVal = NoteToMidi(nStr);
                if (midiVal >= 0) {
                    activeSemitones.push_back(midiVal % 12);
                }
            }
        }

    // Draw solid indicator dots inside active chord keys
    for (int offset : activeSemitones) {
        int dotX = kx;
        int dotY = ky + kh - 4;
        bool isBlackKey = false;

        if (offset == 0)  dotX += 2; // C
        else if (offset == 1)  { dotX += 5; dotY = ky + 5; isBlackKey = true; } // C#
        else if (offset == 2)  dotX += 8; // D
        else if (offset == 3)  { dotX += 11; dotY = ky + 5; isBlackKey = true; } // D#
        else if (offset == 4)  dotX += 14; // E
        else if (offset == 5)  dotX += 20; // F
        else if (offset == 6)  { dotX += 23; dotY = ky + 5; isBlackKey = true; } // F#
        else if (offset == 7)  dotX += 26; // G
        else if (offset == 8)  { dotX += 29; dotY = ky + 5; isBlackKey = true; } // G#
        else if (offset == 9)  dotX += 32; // A
        else if (offset == 10) { dotX += 35; dotY = ky + 5; isBlackKey = true; } // A#
        else if (offset == 11) dotX += 38; // B

        DrawRectangle(dotX, dotY, 2, 2, isBlackKey ? BLACK : WHITE);
    }

    // Draw visual cursor arrow pointing to the currently hovered piano key
    if (chordSelected) {
        int arrowX = kx;
        int arrowY = ky + kh + 1;
        
        int k = stepPopupChordKey;
        if (k == 0)       arrowX += 2;  // C
        else if (k == 1)  arrowX += 5;  // C#
        else if (k == 2)  arrowX += 8;  // D
        else if (k == 3)  arrowX += 11; // D#
        else if (k == 4)  arrowX += 14; // E
        else if (k == 5)  arrowX += 20; // F
        else if (k == 6)  arrowX += 23; // F#
        else if (k == 7)  arrowX += 26; // G
        else if (k == 8)  arrowX += 29; // G#
        else if (k == 9)  arrowX += 32; // A
        else if (k == 10) arrowX += 35; // A#
        else if (k == 11) arrowX += 38; // B
        else if (k == 12) arrowX += 44; // C+

        Draw3x5Char('^', arrowX - 1, arrowY, WHITE);
    }
}
void DrawPerformancePopup(const UIState& state) {
    // Large, centered popup frame (180x56) matching your other popup overlays
    int mx = 38, my = 4, mw = 180, mh = 56;
    
    DrawRectangle(mx, my, mw, mh, BLACK);
    DrawPixelRectLines(mx, my, mw, mh, WHITE);

    // Title Header
    Draw5x5String("PERFORMANCE FX", mx + 46, my + 4, WHITE);
    DrawPixelLine(mx + 4, my + 11, mx + mw - 5, my + 11, WHITE);

    int leftX = mx + 4;
    int leftY = my + 14;

    // --- I. LEFT HALF: GLOBAL MASTER FILTER ---
    // Render the beautiful filter curve graph
    DrawFilterCurve(perfFilterCutoff, perfFilterResonance, perfFilterType, leftX, leftY, 78, 22, WHITE);

    // Render selector labels below the filter graph with highlights
    // (synthGridCol on Row 0 will determine which filter parameter you are tweaking)
    auto DrawFilterLabel = [&](const char* label, int lx, int ly, bool selected) {
        if (selected) {
            DrawRectangle(lx - 1, ly - 1, 15, 7, WHITE);
            Draw3x5String(label, lx, ly, BLACK);
        } else {
            Draw3x5String(label, lx, ly, WHITE);
        }
    };

    bool focusCut = (perfPopupOpen && state.synthGridRow == 0 && state.synthGridCol == 0);
    bool focusRes = (perfPopupOpen && state.synthGridRow == 0 && state.synthGridCol == 1);
    bool focusTyp = (perfPopupOpen && state.synthGridRow == 0 && state.synthGridCol == 2);

    DrawFilterLabel("FRQ", leftX + 4, leftY + 25, focusCut);
    DrawFilterLabel("RES", leftX + 28, leftY + 25, focusRes);
    DrawFilterLabel("TYP", leftX + 52, leftY + 25, focusTyp);

    // Draw central vertical dividing line separating the filter from the stutter grid
    DrawPixelLine(mx + 86, my + 11, mx + 86, my + mh - 5, WHITE);

    // --- II. RIGHT HALF: BEAT REPEAT / STUTTER ---
    int rightX = mx + 90;
    int rightY = my + 14;

    Draw3x5String("GLOBAL STUTTER", rightX + 6, rightY, WHITE);

    // Rhythmic loop subdivisions (using your newly fixed '/' character!)
    const char* stutterDivisions[] = { "1/1", "1/2", "1/4", "1/8", "1/12", "1/16", "1/24", "1/32" };

    // Render 8 boxes representing the 8 physical stutter pads
    int gridBaseX = rightX + 4;
    int gridBaseY = rightY + 9;

    for (int i = 0; i < 8; ++i) {
        int col = i % 4;
        int row = i / 4;
        int bx = gridBaseX + col * 20;
        int by = gridBaseY + row * 12;

        bool padPressed = (activeStutterKey == i);

        // Draw individual pad borders
        DrawPixelRectLines(bx, by, 18, 10, WHITE);

        if (padPressed) {
            // Fill pad fully in solid white when physically pressed
            DrawRectangle(bx + 1, by + 1, 16, 8, WHITE);
            Draw3x5String(stutterDivisions[i], bx + 3, by + 3, BLACK);
        } else {
            Draw3x5String(stutterDivisions[i], bx + 3, by + 3, WHITE);
        }
    }
}
