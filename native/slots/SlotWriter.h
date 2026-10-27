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

    // steady_clock in ms (shared by the main thread and the worker for the settle rule).
    long long SteadyMs();

    // Main thread, every tick: the slot's INNER track status (BSIMusicTrack::MUSIC_STATUS),
    // the SteadyMs() at which its current Inactive/Finished run began (-1 while busy), and
    // the settle time required before the slot file may be replaced (fadeTime + 1 s).
    void PublishStatus(int a_slot, std::uint32_t a_status, long long a_idleSinceMs, long long a_settleMs);

    // Main thread. Queue a refresh of a slot (sine WAV -> .tmp -> verify -> rename).
    // The caller only requests a settled slot; the worker re-checks (from the published
    // atomics) before it writes and before the rename.
    void RequestRefresh(int a_slot);

    // Main thread. Queue a write-open probe of the slot file (no write, no truncate).
    void RequestLockProbe(int a_slot, const char* a_why);

    // M0c, main thread, every tick: whether wrapper A is MUSExploreTundra's current track,
    // and the handle/index of A's LAST play event (kept while A is not current, so only a
    // new play, not an interruption, makes a pending experiment drop).
    void PublishPlay(bool a_aCurrent, std::uint32_t a_handle, std::uint32_t a_idx);

    // M0c, main thread, on a play event of A. The worker waits until the event is 1500 ms
    // old, drops it if the published handle/index changed meanwhile, then runs (e) the
    // write-open probe and (f) the rename-over of the PLAYING explore_a.wav (odd n: a tone,
    // even n: the original from explore_a.wav.orig).
    void RequestPlayExperiment(std::uint32_t a_n, std::uint32_t a_handle, std::uint32_t a_idx);

}
