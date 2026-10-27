#pragma once

// M0a read-only music probe. Passive: reads engine state, never writes it, takes
// no engine lock, installs no hook. M0b extends its logging (now-playing label,
// 1 s position fields) and runs the retarget tick after its own (see Retarget.h).
namespace MJ::Probe {

    // Main thread, at kDataLoaded: enumerate every BGSMusicType and its tracks,
    // look up our two MUST records, then start the 250 ms watcher.
    void OnDataLoaded();

    // Main thread only, after OnDataLoaded. Identity lookup of a track interface
    // pointer (what BSIMusicType::tracks[] holds) to its MUST wrapper form;
    // nullptr if it is not a wrapper enumerated at kDataLoaded.
    RE::BGSMusicTrackFormWrapper* WrapperOf(const RE::BSIMusicTrack* a_track);

    // Main thread only. "EDID(FORMID)" for an enumerated music type, else a pointer string.
    std::string TypeName(const RE::BSIMusicType* a_type);

    // ms since the DLL was loaded (shared clock for all log lines).
    long long NowMs();

}
