#pragma once

#include <filesystem>

// M0b slot protocol test. ONE worker thread does all file IO (WAV generation, .tmp
// write, verify, rename, lock probe, kill-switch poll). It never touches engine
// memory: the main thread publishes each slot's inner-track status through atomics.
namespace MJ::Slots {

    // "<directory of the game exe>\Data". Absolute, so it does not depend on the CWD;
    // under MO2 the USVFS hooks map it, so the engine and this plugin see the same tree.
    std::filesystem::path DataDir();

    // Synchronous stat of Data/SKSE/Plugins/MarthJukebox.noretarget. Load time only.
    bool KillSwitchPresentNow();

    // Latest result of the worker's kill-switch poll (every 2 s). Any thread.
    bool KillSwitchSeen();

    // Main thread, once (at kDataLoaded). Starts the worker; its first job is the
    // start-up write-lock probe of slot A.
    void Start();

    // Main thread, every tick: the slot's INNER track status (BSIMusicTrack::MUSIC_STATUS).
    void PublishStatus(int a_slot, std::uint32_t a_status);

    // Main thread. Queue a refresh of a slot (sine WAV -> .tmp -> verify -> rename).
    // The caller only requests an idle slot; the worker re-checks before it writes.
    void RequestRefresh(int a_slot);

    // Main thread. Queue a write-open probe of the slot file (no write, no truncate).
    void RequestLockProbe(int a_slot, const char* a_why);

}
