#pragma once

// State shared by PoolWorker.cpp (pool thread), SlotFiller.cpp (slot thread) and the Pool.h entry points.
// LOCK RULE: `mx` guards everything below that is not atomic. Nobody does file or network IO, or calls into
// the engine, while holding it; the game thread takes it only to copy small snapshots.

#include "pool/Pool.h"
#include "pool/PoolModel.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <stop_token>

namespace MJ::Pool::detail {

    struct EnvState {
        std::string problem;            // plain language for the menu ("" = none); cleared when a track arrives
        std::string streamingNotice;    // set by every Replan from the plan (streaming not possible on this server)
        bool        smallSelection = false;  // the last refill could only offer recently played tracks
        bool        waitForPlay = false;     // nothing new to ask for until the next play event / settings change
        long long   backoffUntilMs = 0; // SteadyMs(): no `next` for this environment before then
        int         failures = 0;
        std::string nowPlaying;
        // Picks whose download or validation just failed (busy, deadline, truncated, preview, bad audio...): skipped
        // until the time given, and NEVER counted as played (review B1).
        std::map<std::string, long long> rejected;
    };

    struct SlotState : SlotView {
        std::string contentKey;     // "service:id" of the track in the file
        std::string contentFile;    // cache file it came from
        std::string bound;          // PublishBound (game thread)
        // Post-play refresh: write the next track once the play (handle/idx) has been stable since this time.
        bool          refreshPending = false;
        long long     refreshAtMs = 0;
        std::uint32_t eventHandle = 0, eventIdx = 0;
        long long     retryAtMs = 0;  // after a failed write
        std::string   lastError;
        bool          replay = false; // the file holds an already played track (pool dry while the type plays, A-F2)
        // A prepared slot_NN.wav.tmp whose rename failed (A-F1): only the rename is retried.
        bool          prepared = false;
        std::string   prepEnv, prepKey, prepFile, prepDisplay;
        bool          prepReplay = false;
        std::uint64_t prepBytes = 0;
        int           renameAttempts = 0;
        int           deniedAttempts = 0;
    };

    struct PlayEvent {
        int           slot;
        std::string   env;
        std::uint32_t handle, idx;
        long long     ms;
    };

    struct Shared {
        std::mutex                  mx;
        std::condition_variable_any poolCv;   // wakes the pool thread
        std::condition_variable_any slotCv;   // wakes the slot thread

        mj::config::SettingsStore*  store = nullptr;
        std::filesystem::path       cacheDir;           // set by the pool thread once settings are loaded
        std::uint64_t               capBytes = 0;
        bool                        cacheReady = false; // index loaded + revalidated

        mj::pool::PoolIndex         index;
        bool                        indexDirty = false;
        std::set<std::string>       inUse;              // cache files the slot thread is reading right now

        std::vector<mj::retarget::TypeFacts> catalogue;
        bool                        haveCatalogue = false;
        std::shared_ptr<const Plan> plan;
        std::map<std::string, EnvState> envs;
        std::array<SlotState, kSlots> slots;
        std::deque<PlayEvent>       events;

        ConnReport                  conn;
        bool                        settingsDirty = true;
        bool                        poolWake = false;   // MarkDirtyLocked / new pool work: wake the pool thread once
        bool                        replanWanted = false;  // the kill-switch file changed (slot thread poll)
        std::stop_source            jobStop;            // the pool thread's current network job (A-F3)
        std::deque<std::string>     toasts;             // in-game notifications for the game thread
        std::map<std::string, std::string> playedTitles; // "service:id" -> "Artist - Title" of served tracks (replay lines)
        bool                        clearCacheRequested = false;
        PlayedFn                    played;

        std::atomic<std::uint64_t>  cacheBytes{ 0 };
        std::atomic<bool>           killFile{ false };
        // Game-thread play view per slot (lock-free; read by the slot thread for the stability rule).
        std::array<std::atomic<std::uint32_t>, kSlots> playHandle{};
        std::array<std::atomic<std::uint32_t>, kSlots> playIdx{};
        std::array<std::atomic<bool>, kSlots>          playCurrent{};
    };

    Shared& S();

    void PoolThread(std::stop_token a_st);
    void SlotThread(std::stop_token a_st);

    // Pool thread helpers used by the slot thread (call with `mx` held): mark the index for saving and wake
    // the pool thread (refill after a played track).
    void MarkDirtyLocked();

}
