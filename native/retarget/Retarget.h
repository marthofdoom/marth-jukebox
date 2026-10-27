#pragma once

#include <cstdint>
#include <vector>

#include "retarget/SlotAssign.h"

// The ONLY engine-memory writer of the plugin (docs/INTEGRATION-1.0.md A.1/A.3). Game thread only.
//
// Every music type the published Plan (pool/Pool.h) maps to slot s gets its Single/Palette entries pointed, in
// place, at wrapper MJ_SLOT_s (array size never changed, Silent and other entries kept), and back to the
// saved originals when the slot is not ready, the plan changes, or a kill switch trips. Writes happen only in
// Tick(), while the type is neither current nor queued and its status is Inactive/Finished (M0b rule).
// Play events (sound handle / index change of a slot wrapper while its type is current) are posted to the slot
// thread. Diegetic and unmapped types are never touched.
namespace MJ::Retarget {

    // kDataLoaded, after Probe::OnDataLoaded: resolves MarthJukebox.esp's 24 slot wrappers. No writes.
    void OnDataLoaded();

    // Watcher task (250 ms). The first call builds the music-type catalogue (the arrays are final by then, M0b)
    // and hands it to the pool thread.
    void Tick();

    // Game thread. Slot index 0..23 of one of our wrappers, else -1.
    int SlotOf(const RE::BSIMusicTrack* a_track);

    // Game thread: log the state of every retargeted type (kPostLoadGame etc.). No writes.
    void LogState(const char* a_why);

}
