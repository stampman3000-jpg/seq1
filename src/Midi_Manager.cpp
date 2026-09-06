#include "Midi_Manager.hpp"
#include "Globals.hpp"
#include "Audio_Engine.hpp"
#include <iostream>
#include <chrono>
#include <cmath>
#include <atomic>
#include <algorithm>

MidiManager* g_midiManager = nullptr;

namespace {
constexpr int kMaxMidiTicksQueued = 96;

uint64_t SteadyNowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Bounded increment: drop F8s once the queue is already full.
void EnqueueExternalMidiTick() {
    int prev = g_externalMidiTicksQueued.load(std::memory_order_relaxed);
    while (prev < kMaxMidiTicksQueued) {
        if (g_externalMidiTicksQueued.compare_exchange_weak(
                prev, prev + 1,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return;
        }
    }
    g_midiTickDrops.fetch_add(1u, std::memory_order_relaxed);
}
} // namespace

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
    (void)timeStamp;
    (void)userData;
    unsigned int nBytes = message->size();
    if (nBytes == 0) return;

    unsigned char status = message->at(0);

    // --- 1. HANDLE REAL-TIME SYSTEM COMMON SYNC MESSAGES & BPM CALCULATION ---
    if (status == 0xF8) {
        if (g_useExternalMidiClock.load(std::memory_order_relaxed)) {
            g_lastExternalClockMs.store(SteadyNowMs(), std::memory_order_relaxed);
            EnqueueExternalMidiTick();

            static int tickCounter = 0;
            static auto lastBeatTime = std::chrono::high_resolution_clock::now();
            static bool firstBeat = true;
            static float smoothedBpm = 120.0f;
            static int displayedBpm = 120;

            tickCounter++;
            // Calculate BPM once every 24 ticks (exactly 1 full quarter-note beat)
            if (tickCounter >= 24) {
                tickCounter = 0;
                auto now = std::chrono::high_resolution_clock::now();
                double beatDuration = std::chrono::duration<double>(now - lastBeatTime).count();
                lastBeatTime = now;

                float calculatedBpm = (float)tempo;

                if (firstBeat) {
                    // Seed display from current project tempo; skip interval math
                    // until we have two beat edges.
                    firstBeat = false;
                    calculatedBpm = (float)tempo;
                    if (calculatedBpm < 20.0f || calculatedBpm > 500.0f) {
                        calculatedBpm = 120.0f;
                    }
                    smoothedBpm = calculatedBpm;
                    displayedBpm = (int)std::round(smoothedBpm);
                } else if (beatDuration > 0.12 && beatDuration < 3.0) { // Safe limits (20 to 500 BPM)
                    calculatedBpm = 60.0f / (float)beatDuration;

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

                    // --- INTEGRAL HYSTERESIS FILTER ---
                    // Only snap the displayed integer once it drifts past the deadband threshold
                    float displayDiff = smoothedBpm - (float)displayedBpm;
                    if (displayDiff > 0.65f) {
                        displayedBpm = (int)std::round(smoothedBpm);
                    } else if (displayDiff < -0.65f) {
                        displayedBpm = (int)std::round(smoothedBpm);
                    }
                }

                tempo = (double)displayedBpm;
            }
        }
        return;
    }
    if (status == 0xFA || status == 0xFB) {
        if (g_useExternalMidiClock.load(std::memory_order_relaxed)) {
            g_lastExternalClockMs.store(SteadyNowMs(), std::memory_order_relaxed);
            g_externalMidiStartTriggered.store(true, std::memory_order_release);
        }
        return;
    }
    if (status == 0xFC) {
        if (g_useExternalMidiClock.load(std::memory_order_relaxed)) {
            g_externalMidiStopTriggered.store(true, std::memory_order_release);
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
        int snappedNote = MaybeSnapMidi((int)note, targetTrack);

        if (channel == 15 && targetTrack == selectedTrack) {
            bool isSeqPageOpen = (currentScreen == SCREEN_SEQ_1_4 || currentScreen == SCREEN_SEQ_5_8);
            if (isSeqPageOpen) {
                // Capture cursorStep now; audio applies the write (no tracks[] from MIDI thread).
                const int stepIdx = cursorStep;
                EnqueueMidiLiveFromRt(
                    ML_SEQ_STEP,
                    (uint8_t)targetTrack,
                    (uint8_t)snappedNote,
                    (uint8_t)mappedVelocity,
                    (uint8_t)std::clamp(stepIdx, 0, 255));
            }
        }

        EnqueueMidiLiveFromRt(
            ML_NOTE_ON,
            (uint8_t)targetTrack,
            (uint8_t)snappedNote,
            (uint8_t)mappedVelocity,
            0);
    } else if (command == 0x80 || (command == 0x90 && velocity == 0)) {
        int snappedNote = MaybeSnapMidi((int)note, targetTrack);
        EnqueueMidiLiveFromRt(
            ML_NOTE_OFF,
            (uint8_t)targetTrack,
            (uint8_t)snappedNote,
            0,
            0);
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
