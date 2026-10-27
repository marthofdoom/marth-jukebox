#pragma once

// M0b retarget spike (hardcoded): MUSExploreTundra's Single/Palette track entries are
// replaced IN PLACE by our two slot wrappers (MarthJukebox.esp 0x800 = A, 0x801 = B).
// Silent entries, other entries and the array size are never touched. Writes happen
// only in Tick(), on the main thread, while the type is inactive and not queued.
// Kill switch: Data/SKSE/Plugins/MarthJukebox.noretarget. See DESIGN.md 8.6.
namespace MJ::Retarget {

    // Main thread, at kDataLoaded, AFTER Probe::OnDataLoaded (needs its identity map).
    // Resolves the target type and our wrappers, logs, starts the slot worker. No writes.
    void OnDataLoaded();

    // Main thread, from the SKSE message listener: logs the target's tracks.size()
    // for kInputLoaded / kNewGame / kPostLoadGame. No writes.
    void OnMessage(std::uint32_t a_type);

    // Main thread, from the 250 ms watcher task. The ONLY place engine memory is written.
    void Tick();

    // Main thread only. 0 = slot A wrapper, 1 = slot B wrapper, -1 = anything else.
    int SlotOf(const RE::BSIMusicTrack* a_track);

    // Main thread only. MUSExploreTundra's BSIMusicType subobject, or nullptr if unresolved.
    const RE::BSIMusicType* Target();

}
