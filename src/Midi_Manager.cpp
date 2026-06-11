#include "Midi_Manager.hpp"
#include "Globals.hpp"
#include "Audio_Engine.hpp"
#include <iostream>
#include <chrono>

MidiManager* g_midiManager = nullptr;

MidiManager::MidiManager() : midiIn(nullptr), connected(false), activePortName("Offline"), activePortIndex(0) {
    try {
        midiIn = new RtMidiIn();
    } catch (RtMidiError &error) {
        std::cerr << "MIDI initialization failed: " << error.getMessage() << "\n";
        return;
    }

    unsigned int nPorts = midiIn->getPortCount();
    if (nPorts == 0) {
        std::cout << "No MIDI input ports found.\n";
        return;
    }

    unsigned int portToOpen = 0;
    for (unsigned int i = 0; i < nPorts; ++i) {
        std::string name = midiIn->getPortName(i);
        if (name.find("IAC") == std::string::npos && name.find("Virtual") == std::string::npos) {
            portToOpen = i;
            break;
        }
    }

    midiIn->openPort(portToOpen);
    midiIn->setCallback(&MidiManager::midiCallback, this);
    midiIn->ignoreTypes(false, false, false);
    
    connected = true;
    activePortIndex = portToOpen;
    activePortName = midiIn->getPortName(portToOpen);
    
    if (activePortName.length() > 14) {
        activePortName = activePortName.substr(0, 11) + "...";
    }
    std::cout << "Successfully connected to: " << activePortName << "\n";
}

MidiManager::~MidiManager() {
    if (midiIn) {
        midiIn->cancelCallback();
        delete midiIn;
    }
}

bool MidiManager::isConnected() const {
    return connected;
}

std::string MidiManager::getPortName() const {
    return activePortName;
}

void MidiManager::cyclePort(int direction) {
    if (!midiIn || !connected) return;

    unsigned int nPorts = midiIn->getPortCount();
    if (nPorts <= 1) return; // No other ports to cycle to

    // Calculate next port index with wrapping
    int targetPort = (int)activePortIndex + direction;
    if (targetPort < 0) targetPort = (int)nPorts - 1;
    if (targetPort >= (int)nPorts) targetPort = 0;

    // Safely swap the port stream
    midiIn->closePort();
    midiIn->cancelCallback();
    
    midiIn->openPort(targetPort);
    midiIn->setCallback(&MidiManager::midiCallback, this);

    activePortIndex = targetPort;
    activePortName = midiIn->getPortName(targetPort);
    
    if (activePortName.length() > 14) {
        activePortName = activePortName.substr(0, 11) + "...";
    }
    std::cout << "Swapped MIDI input to: " << activePortName << "\n";
}

void MidiManager::midiCallback(double timeStamp, std::vector<unsigned char> *message, void *userData) {
    unsigned int nBytes = message->size();
    if (nBytes == 0) return;

    unsigned char status = message->at(0);

    // --- 1. HANDLE REAL-TIME SYSTEM COMMON SYNC MESSAGES & BPM CALCULATION ---
        if (status == 0xF8) {
            if (g_useExternalMidiClock) {
                g_externalMidiTicksQueued++;

                static int tickCounter = 0;
                static auto lastBeatTime = std::chrono::high_resolution_clock::now();

                tickCounter++;
                // Calculate BPM once every 24 ticks (exactly 1 full quarter-note beat)
                if (tickCounter >= 24) {
                    tickCounter = 0;
                    auto now = std::chrono::high_resolution_clock::now();
                    double beatDuration = std::chrono::duration<double>(now - lastBeatTime).count();
                    lastBeatTime = now;

                    float calculatedBpm = (float)tempo;
                    static bool firstBeat = true;

                    if (firstBeat) {
                        firstBeat = false;
                        calculatedBpm = 120.0f; // Seed with a standard default on startup
                    } else {
                        if (beatDuration > 0.12 && beatDuration < 3.0) { // Safe limits (20 to 500 BPM)
                            calculatedBpm = 60.0f / (float)beatDuration;
                        }
                    }

                    static float smoothedBpm = 120.0f;
                                    static int displayedBpm = 120; // Tracks the currently displayed integer

                                    // Adaptive smoothing: calculate absolute difference in BPM
                                    float diff = std::abs(calculatedBpm - smoothedBpm);
                                    float coeff = 0.15f; // Default heavy smoothing for steady state

                                    if (diff > 5.0f) {
                                        coeff = 1.0f; // Instant snap for deliberate, large tempo changes
                                    } else if (diff > 2.0f) {
                                        // Smoothly ramp responsiveness between 2.0 and 5.0 BPM difference
                                        float t = (diff - 2.0f) / 3.0f; // Scale to 0.0 -- 1.0
                                        coeff = 0.15f + t * 0.85f;      // Ramps from 0.15 to 1.0
                                    }

                                    smoothedBpm += coeff * (calculatedBpm - smoothedBpm);

                                    // Initialize displayed integer to your project's active tempo on first beat
                                    if (firstBeat) {
                                        displayedBpm = (int)std::round(smoothedBpm);
                                    }

                                    // --- INTEGRAL HYSTERESIS FILTER ---
                                    // Only snap the displayed integer once it drifts past the deadband threshold
                                    float displayDiff = smoothedBpm - (float)displayedBpm;
                                    if (displayDiff > 0.65f) {
                                        displayedBpm = (int)std::round(smoothedBpm);
                                    } else if (displayDiff < -0.65f) {
                                        displayedBpm = (int)std::round(smoothedBpm);
                                    }

                                    tempo = (double)displayedBpm;
                }
            }
            return;
        }
    if (status == 0xFA || status == 0xFB) {
        if (g_useExternalMidiClock) {
            g_externalMidiStartTriggered = true;
        }
        return;
    }
    if (status == 0xFC) {
        if (g_useExternalMidiClock) {
            g_externalMidiStopTriggered = true;
        }
        return;
    }

    if (nBytes < 3) return;

    unsigned char command = status & 0xF0;
    unsigned char channel = status & 0x0F;
    unsigned char note = message->at(1);
    unsigned char velocity = message->at(2);

    int targetTrack = -1;
    if (channel >= 0 && channel <= 7) {
        targetTrack = channel;
    } else if (channel == 15) {
        targetTrack = selectedTrack;
    }

    if (targetTrack < 0 || targetTrack >= 8) return;

    if (command == 0x90 && velocity > 0) {
        int mappedVelocity = (velocity < 43) ? 1 : ((velocity < 86) ? 2 : 3);
        
        if (channel == 15 && targetTrack == selectedTrack) {
            bool isSeqPageOpen = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8);
            if (isSeqPageOpen) {
                Step& s = tracks[selectedTrack].steps[cursorStep];
                s.note = MidiToNote(note);
                s.velocity = mappedVelocity;
            }
        }

        TriggerVoiceLive(targetTrack, note, mappedVelocity);
    }
    else if (command == 0x80 || (command == 0x90 && velocity == 0)) {
        ReleaseVoiceLive(targetTrack, note);
    }
}

void InitMidi() {
    if (g_midiManager == nullptr) {
        g_midiManager = new MidiManager();
    }
}

void ShutdownMidi() {
    if (g_midiManager != nullptr) {
        delete g_midiManager;
        g_midiManager = nullptr;
    }
}
