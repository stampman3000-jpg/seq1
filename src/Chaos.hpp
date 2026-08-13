#pragma once
#include <atomic>

// =============================================================================
// GENERATIVE CHAOS LAYER
//
// The pattern you program lives in Track::source. Each bar the generator
// renders that source into Track::steps, so the sequencer grid visibly
// rewrites itself while the original loop stays intact behind it. A chaos
// macro of zero renders an exact copy, so your pattern is always one knob
// turn away.
//
// Every decision is a hash of (seed, bar, step, behaviour) rather than a
// running random number generator. Being stateless makes it safe to run on
// the audio thread, and because the bar number is an input we can fold it to
// make a variation repeat on a phrase length instead of drifting forever.
// =============================================================================

// Set by the UI whenever a chaos parameter changes, so a knob turn is heard
// on the next tick rather than at the next bar.
extern std::atomic<bool> g_chaosDirty;

// Salts keep the behaviours independent; two of them must never roll the same
// number for the same step or they would fire together every time.
enum ChaosSalt {
    CHAOS_SALT_SKIP = 1,
    CHAOS_SALT_FILL,
    CHAOS_SALT_LIFT,
    CHAOS_SALT_LIFT_PICK,
    CHAOS_SALT_RATCHET,
    CHAOS_SALT_RATCHET_PICK,
    CHAOS_SALT_TIME,
    CHAOS_SALT_TIME_PICK
};

// Inline so the UI thread can compute exactly what the audio thread will,
// which is what would let the grid preview a bar before it is heard.
static inline unsigned int ChaosHash(unsigned int seed, unsigned int bar,
                                     unsigned int step, unsigned int salt) {
    unsigned int h = seed * 0x9E3779B1u;
    h ^= (bar  + 0x165667B1u) * 0x85EBCA77u;
    h ^= (step + 0x27D4EB2Fu) * 0xC2B2AE3Du;
    h ^= (salt + 0x165667B1u) * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

// Folds the raw bar count onto the repeat length, so a variation can recur on
// a phrase boundary. A repeat of 0 never comes back around.
unsigned int ChaosFoldBar(unsigned int rawBar);

// Renders source[] into steps[] for one track. Audio-thread safe: a bounded
// loop of plain integer writes with no allocation.
void ChaosRenderTrack(int trackIdx, unsigned int rawBar);
void ChaosRenderAll(unsigned int rawBar);

// --- Source buffer lifecycle ---
struct Track;
void ChaosCaptureInto(Track& trk);               // steps[] -> source[] on any Track
void ChaosSyncSource(int trackIdx, int stepIdx); // mirror one hand edit
void ChaosCaptureTrack(int trackIdx);            // steps[] -> source[]
void ChaosCaptureAll();                          // promote the current mutation
void ChaosRevertAll();                           // source[] -> steps[]
void ChaosReseed();                              // roll a new variation

// Called after any chaos parameter is edited. Captures the peace pattern the
// first time the macro leaves zero, and restores it when it returns.
void ChaosNotifyParamChanged(int previousChaos);
