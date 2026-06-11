#pragma once
#include "RtMidi.h"
#include <vector>
#include <string>

class MidiManager {
public:
    MidiManager();
    ~MidiManager();
    
    bool isConnected() const;
    std::string getPortName() const;
    
    // Safely closes current port and opens a new one dynamically
    void cyclePort(int direction);

private:
    static void midiCallback(double timeStamp, std::vector<unsigned char> *message, void *userData);
    
    RtMidiIn *midiIn;
    bool connected;
    std::string activePortName;
    unsigned int activePortIndex; // Tracks current opened port
};

extern MidiManager* g_midiManager;

void InitMidi();
void ShutdownMidi();
