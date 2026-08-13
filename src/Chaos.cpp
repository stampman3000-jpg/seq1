#include "Chaos.hpp"
#include "Globals.hpp"
#include <algorithm>

std::atomic<bool> g_chaosDirty(false);

unsigned int ChaosFoldBar(unsigned int rawBar) {
    if (g_chaosRepeat <= 0) return rawBar;
    return rawBar % (unsigned int)g_chaosRepeat;
}

// A weight only reaches its full strength when the macro is fully open, so the
// macro alone decides how far the pattern travels and the weights only decide
// which behaviours get there first.
static inline int ChaosWeight(int weight) {
    return (g_chaos * weight) / 99;
}

void ChaosRenderTrack(int trackIdx, unsigned int rawBar) {
    if (trackIdx < 0 || trackIdx >= 8) return;
    if (g_chaos <= 0) return;

    Track& trk = tracks[trackIdx];
    const unsigned int bar = ChaosFoldBar(rawBar);
    const int len = std::clamp(trk.stepLength, 1, 32);

    const int effSkip    = ChaosWeight(g_chaosSkip);
    const int effFill    = ChaosWeight(g_chaosFill);
    const int effLift    = ChaosWeight(g_chaosLift);
    const int effRatchet = ChaosWeight(g_chaosRatchet);
    const int effTime    = ChaosWeight(g_chaosTime);

    const int keyRoot = ResolveTrackKeyRoot(trackIdx);
    const int keyLock = ResolveTrackKeyLock(trackIdx);
    const unsigned int seed = g_chaosSeed + (unsigned int)trackIdx * 0x9E3779B1u;

    for (int s = 0; s < 32; ++s) {
        const StepSource& src = trk.source[s];
        int note   = src.note;
        int vel    = src.velocity;
        int retrig = src.retrigger;
        int micro  = src.microtiming;

        // Steps past the polymeter limit are never scanned by the engine, so
        // leave them showing the source rather than inventing content there.
        if (s < len) {
            auto roll = [&](unsigned int salt) {
                return (int)(ChaosHash(seed, bar, (unsigned int)s, salt) % 100u);
            };
            bool active = (note >= 0 && vel > 0);

            // Dropping notes comes first so the macro thins the pattern as
            // well as thickening it; without this it only ever gets denser.
            if (active && roll(CHAOS_SALT_SKIP) < effSkip) {
                note = -1;
                vel = 0;
                retrig = 0;
                micro = 0;
                active = false;
            }

            // An empty step borrows the note from whichever neighbour has one.
            if (!active && note < 0 && roll(CHAOS_SALT_FILL) < effFill) {
                const StepSource& prev = trk.source[(s + len - 1) % len];
                const StepSource& next = trk.source[(s + 1) % len];
                const StepSource* donor = nullptr;
                if (prev.note >= 0 && prev.velocity > 0)      donor = &prev;
                else if (next.note >= 0 && next.velocity > 0) donor = &next;

                if (donor) {
                    note = donor->note;
                    vel = 2;
                    retrig = 0;
                    micro = 0;
                    active = true;
                }
            }

            // The voicing ladder: fifth, octave, twelfth, double octave. Each
            // one is a pure interval measured from the original note, so a lift
            // is always a different voice of the note you wrote rather than a
            // stack of fifths climbing out of the key.
            if (active && roll(CHAOS_SALT_LIFT) < effLift) {
                static const int kLiftLadder[4] = { 7, 12, 19, 24 };
                unsigned int pick = ChaosHash(seed, bar, (unsigned int)s, CHAOS_SALT_LIFT_PICK) % 100u;
                int rung = (pick < 40u) ? 0 : (pick < 70u) ? 1 : (pick < 90u) ? 2 : 3;
                int lifted = note + kLiftLadder[rung];

                // The one interval that can still misfire is a fifth off the
                // seventh degree, which lands a tritone above the root, so the
                // scale snap catches it when a key is set.
                lifted = SnapMidiToScale(lifted, keyRoot, keyLock);
                if (lifted <= 127) note = lifted;
            }

            if (active && roll(CHAOS_SALT_RATCHET) < effRatchet) {
                retrig = 2 + (int)(ChaosHash(seed, bar, (unsigned int)s, CHAOS_SALT_RATCHET_PICK) % 3u);
            }

            if (active && roll(CHAOS_SALT_TIME) < effTime) {
                unsigned int p = ChaosHash(seed, bar, (unsigned int)s, CHAOS_SALT_TIME_PICK);
                micro = (int)(p % 7u) - 3;
                vel += ((p >> 8) & 1u) ? 1 : -1;
            }
        }

        Step& dst = trk.steps[s];
        dst.note        = (int8_t)std::clamp(note, -1, 127);
        dst.velocity    = std::clamp(vel, 0, 3);
        dst.retrigger   = std::clamp(retrig, 0, 16);
        dst.microtiming = std::clamp(micro, -6, 6);
    }
}

void ChaosRenderAll(unsigned int rawBar) {
    if (g_chaos <= 0) return;
    for (int t = 0; t < 8; ++t) ChaosRenderTrack(t, rawBar);
}

void ChaosSyncSource(int trackIdx, int stepIdx) {
    if (trackIdx < 0 || trackIdx >= 8) return;
    if (stepIdx < 0 || stepIdx >= 32) return;
    const Step& st = tracks[trackIdx].steps[stepIdx];
    StepSource& sr = tracks[trackIdx].source[stepIdx];
    sr.note        = st.note;
    sr.velocity    = (int8_t)st.velocity;
    sr.retrigger   = (int8_t)st.retrigger;
    sr.microtiming = (int8_t)st.microtiming;
}

void ChaosCaptureInto(Track& trk) {
    for (int s = 0; s < 32; ++s) {
        const Step& st = trk.steps[s];
        StepSource& sr = trk.source[s];
        sr.note        = st.note;
        sr.velocity    = (int8_t)st.velocity;
        sr.retrigger   = (int8_t)st.retrigger;
        sr.microtiming = (int8_t)st.microtiming;
    }
}

void ChaosCaptureTrack(int trackIdx) {
    if (trackIdx < 0 || trackIdx >= 8) return;
    ChaosCaptureInto(tracks[trackIdx]);
}

void ChaosCaptureAll() {
    for (int t = 0; t < 8; ++t) ChaosCaptureTrack(t);
}

void ChaosRevertAll() {
    for (int t = 0; t < 8; ++t) {
        Track& trk = tracks[t];
        for (int s = 0; s < 32; ++s) {
            const StepSource& sr = trk.source[s];
            Step& st = trk.steps[s];
            st.note        = sr.note;
            st.velocity    = sr.velocity;
            st.retrigger   = sr.retrigger;
            st.microtiming = sr.microtiming;
        }
    }
}

void ChaosReseed() {
    g_chaosSeed = g_chaosSeed * 1664525u + 1013904223u;
    g_chaosDirty.store(true);
}

void ChaosNotifyParamChanged(int previousChaos) {
    // Snapshot the pattern the moment the macro first opens, and hand it back
    // untouched when it closes again.
    if (previousChaos <= 0 && g_chaos > 0) ChaosCaptureAll();
    else if (previousChaos > 0 && g_chaos <= 0) ChaosRevertAll();
    g_chaosDirty.store(true);
}
