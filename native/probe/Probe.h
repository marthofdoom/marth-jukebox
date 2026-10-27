#pragma once

// M0a read-only music probe. Passive: reads engine state, never writes it, takes
// no engine lock, installs no hook.
namespace MJ::Probe {

    // Main thread, at kDataLoaded: enumerate every BGSMusicType and its tracks,
    // look up our two MUST records, then start the 250 ms watcher.
    void OnDataLoaded();

}
