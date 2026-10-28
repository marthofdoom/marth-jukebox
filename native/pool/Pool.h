#pragma once

// Pool workers (docs/INTEGRATION-1.0.md A.2/A.3). Two threads, no engine memory:
//  - the POOL thread (PoolWorker.cpp): loads the settings, talks to Harmony (status, profile PUT, `next`,
//    downloads), keeps the pool index + recent lists + the LRU cache cap, computes the Plan;
//  - the SLOT thread (SlotFiller.cpp): writes slot files from the pools, handles play events.
// The game thread (retarget/) only reads the published Plan / SlotView snapshots and posts play events.

#include "config/Settings.h"
#include "retarget/SlotAssign.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MJ::Pool {

    constexpr int kSlots = mj::retarget::kSlotCount;

    struct EnvPlan {
        std::string id;             // Settings environment id == Harmony type name
        int         slot = -1;      // -1: more active environments than slots (stays vanilla)
        int         poolSize = 3;
        std::string sources;        // `sources` for `next` (config::ChooseSources)
        std::string fingerprint;    // mj::pool::Fingerprint
        bool        streamingDropped = false;
    };

    // Immutable once published.
    struct Plan {
        std::uint64_t                  gen = 0;
        bool                           engineWrites = false;  // Retarget feature on, no kill-switch file, network active
        std::string                    why;                   // plain language when engineWrites is false
        std::map<std::string, EnvPlan> envs;                  // every environment that a catalogue type maps to
        std::vector<int>               typeSlot;              // catalogue index -> slot, -1 = stays vanilla
        std::array<std::string, kSlots> slotEnv;              // slot -> environment ("" = unused)
    };

    // What the slot thread says about each slot file.
    struct SlotView {
        std::string owner;             // environment whose track the file holds ("" = none / unknown)
        bool        fresh = false;     // holds an unplayed pool track
        bool        failing = false;   // cannot be (re)filled right now (pool empty, write refused)
        bool        replay = false;    // holds an earlier track of `owner` placed while its pool was dry (still playable)
        long long   servedAtMs = -1;   // SteadyMs() of the play event that consumed the content
    };

    // ---- lifecycle (called by app/App.cpp) ----
    // Starts both threads. The pool thread loads the store first (file IO stays off the game thread).
    // a_userAgent: "MarthJukebox/<version>" (the caller knows the plugin version; the pool code stays engine-free).
    void Start(mj::config::SettingsStore* a_store, std::string a_userAgent);
    // The music-type catalogue (game thread, at the first watcher tick): facts only, index = catalogue index.
    void SetCatalogue(std::vector<mj::retarget::TypeFacts> a_types);
    // Re-read the settings snapshot, re-PUT the profile, re-plan (any thread; never blocks).
    void SettingsChanged();
    void ClearCache();
    std::uint64_t CacheBytes();

    // ---- game thread ----
    std::shared_ptr<const Plan> CurrentPlan();          // may be null before the first plan
    std::array<SlotView, kSlots> SlotViews();               // copy under a short lock (no IO is ever done under it)
    // The environment whose types currently point at the slot ("" none; "*" more than one). The slot thread never
    // writes a file for environment E while another environment's types still point at it.
    void PublishBound(int a_slot, const std::string& a_env);
    // Main-thread view of the slot's current play (for the 1.5 s stability rule).
    void PublishPlay(int a_slot, bool a_current, std::uint32_t a_handle, std::uint32_t a_idx);
    void PostPlayEvent(int a_slot, const std::string& a_env, std::uint32_t a_handle, std::uint32_t a_idx);
    bool KillSwitchSeen();   // the slot thread's 2 s poll of the kill-switch file
    // Next in-game notification text queued by the pool thread (pool/ToastPolicy.h), if any. Game thread shows it.
    std::optional<std::string> PopToast();
    // What the workers are doing right now (slot write, download) and the last slot job's timing: appended to the
    // stall detector's warning so a log separates a game stall (autosave, loading) from our IO (field #4).
    std::string PerfNow();
    long long SteadyMs();

    // ---- status for the menu (any thread) ----
    struct EnvReport {
        std::string id;
        bool        planned = false;   // a catalogue type maps to it and it has a slot
        int         slot = -1;
        int         ready = 0;
        int         target = 0;
        std::string problem;           // plain language, "" = none
        std::string nowPlaying;
        std::vector<std::string> titles;
    };
    std::vector<EnvReport> Report();
    // Plain-language connection result of the pool thread's own status check (used when no test is running).
    struct ConnReport {
        bool        configured = false;
        bool        ok = false;
        bool        tested = false;
        std::string message;
        bool        flac = false, servicesReported = false, ytmusic = false, qobuz = false;
    };
    ConnReport Connection();

    // Called by the slot thread when a track starts being heard (App reports it to Harmony on its UI thread).
    using PlayedFn = std::function<void(const std::string& a_env, const std::string& a_service, const std::string& a_id)>;
    void SetPlayedCallback(PlayedFn a_fn);

}
