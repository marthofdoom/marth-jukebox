// Slot thread: keeps every planned slot file holding its environment's next (unplayed) pool track, and turns
// play events into "served" (docs/INTEGRATION-1.0.md A.3, docs/M0C-RESULTS.md). File IO only; no engine memory.
//
// Per slot s planned for environment E:
//   - prefill / re-target: when the file does not hold an unplayed track of E's pool, write E's pool head;
//   - play event (game thread): the track heard is the one placed before; it is marked served (removed from
//     the pool, added to the recent list, reported to Harmony), and once the play has been stable for 1.5 s the
//     NEXT pool head is renamed over the (playing) file;
//   - pool dry while the type is playing (A-F2): the environment's recently played tracks that are still in the
//     cache are rotated through the slot (least recently played first), so the last song does not loop; when the
//     type is not playing, the slot counts as not ready and the game thread restores the vanilla tracks;
//   - a failed rename keeps the prepared .tmp and retries only the rename, with a growing back-off and only while
//     the slot is not the one playing (A-F1);
//   - the file is never written for E while another environment's types still point at it.
// It also polls the kill-switch file every 2 s (A-F3: independent of network work on the pool thread).
#include "PCH.h"
#include "pool/Pool_internal.h"

#include "slots/SlotWriter.h"

namespace MJ::Pool::detail {

    namespace {

        namespace fs = std::filesystem;

        constexpr long long kStableMs        = 1500;     // M0c: let the engine open the file first
        constexpr long long kFailRetryMs     = 30'000;   // refused / failed preparation
        constexpr long long kGiveUpRetryMs   = 120'000;  // after giving up on a rename
        constexpr long long kSourceRejectMs  = 6 * 3600'000ll;
        constexpr int       kMaxDenied       = 3;
        constexpr long long kKillPollMs      = 2000;

        long long RenameBackoff(int a_attempts) {
            static constexpr long long kSteps[] = { 2'000, 5'000, 15'000, 60'000 };
            return kSteps[std::clamp(a_attempts - 1, 0, 3)];
        }

        struct Job {
            enum class Kind { kNone, kPrepare, kCommit, kDiscard } kind = Kind::kNone;
            int         slot = -1;
            std::string env, key, file, display;
            bool        replay = false;
            // kPrepare from the pool: one path. Replay: candidates (key, cache file names), least recently played first.
            std::vector<std::pair<std::string, std::vector<std::string>>> candidates;
        };

        std::pair<std::string, std::string> SplitKey(const std::string& a_key) {
            const auto c = a_key.find(':');
            if (c == std::string::npos) return { {}, {} };
            return { a_key.substr(0, c), a_key.substr(c + 1) };
        }

        // Caller holds mx. Handles one play event.
        void OnEventLocked(Shared& s, const PlayEvent& e, std::vector<std::function<void()>>& after) {
            auto& sl = s.slots[e.slot];
            std::string service, id, display = sl.contentKey;
            if (sl.owner == e.env && sl.fresh) {
                const auto key = sl.contentKey;
                if (const auto p = s.index.pools.find(e.env); p != s.index.pools.end()) {
                    for (const auto& en : p->second.entries) {
                        if (en.Key() == key) {
                            service = en.service;
                            id      = en.id;
                            display = en.Display();
                        }
                    }
                }
                mj::pool::RemoveEntry(s.index, e.env, key);
                mj::pool::AddRecent(s.index, e.env, key);
                sl.fresh      = false;
                sl.servedAtMs = e.ms;
                s.envs[e.env].nowPlaying = display;
                MarkDirtyLocked();  // also wakes the pool thread: refill the played track
                spdlog::info("[slots] slot {:02} ({}) PLAY handle=0x{:08X} idx={}: now playing {}", e.slot, e.env, e.handle, e.idx, display);
            } else if (sl.owner == e.env && sl.replay) {
                std::tie(service, id) = SplitKey(sl.contentKey);
                mj::pool::AddRecent(s.index, e.env, sl.contentKey);  // rotates it to the newest end
                sl.servedAtMs = e.ms;
                s.envs[e.env].nowPlaying = "(replay while Harmony refills) " + sl.contentKey;
                MarkDirtyLocked();
                spdlog::warn("[slots] slot {:02} ({}) PLAY handle=0x{:08X} idx={}: pool was dry, replaying recently played {}", e.slot,
                             e.env, e.handle, e.idx, sl.contentKey);
            } else {
                spdlog::warn("[slots] slot {:02} ({}) PLAY handle=0x{:08X} idx={}: the file held no fresh track of this environment "
                             "(owner {}, fresh {}); the previous track repeats",
                             e.slot, e.env, e.handle, e.idx, sl.owner.empty() ? "none" : sl.owner, sl.fresh);
            }
            if (s.played && !service.empty() && !id.empty()) {
                auto fn = s.played;
                after.push_back([fn, env = e.env, service, id] { fn(env, service, id); });
            }
            sl.refreshPending = true;
            sl.refreshAtMs    = e.ms + kStableMs;
            sl.eventHandle    = e.handle;
            sl.eventIdx       = e.idx;
        }

        bool InPool(const Shared& s, const std::string& a_env, const std::string& a_key) {
            const auto pit = s.index.pools.find(a_env);
            if (pit == s.index.pools.end()) return false;
            for (const auto& en : pit->second.entries)
                if (en.Key() == a_key) return true;
            return false;
        }

        // Caller holds mx. Picks at most one slot job to do now; also maintains `failing`.
        Job PickJobLocked(Shared& s, const Plan& plan, long long now) {
            Job job;
            for (int i = 0; i < kSlots; ++i) {
                auto&              sl  = s.slots[i];
                const std::string& env = plan.slotEnv[i];
                const bool         writable = !env.empty() && plan.engineWrites && !s.killFile.load() &&
                                      (sl.bound.empty() || sl.bound == env);  // never under another environment's types

                // 1. A prepared .tmp waiting for its rename.
                if (sl.prepared) {
                    const bool valid = writable && sl.prepEnv == env && (sl.prepReplay || InPool(s, env, sl.prepKey));
                    if (!valid) {
                        sl.prepared = false;
                        job.kind    = Job::Kind::kDiscard;
                        job.slot    = i;
                        return job;
                    }
                    // Retries only while this slot is not the one playing (the engine holds the file then).
                    if (now < sl.retryAtMs || (sl.renameAttempts > 0 && s.playCurrent[i].load())) continue;
                    job.kind    = Job::Kind::kCommit;
                    job.slot    = i;
                    job.env     = sl.prepEnv;
                    job.key     = sl.prepKey;
                    job.file    = sl.prepFile;
                    job.display = sl.prepDisplay;
                    job.replay  = sl.prepReplay;
                    return job;
                }
                if (!writable) continue;

                const auto  pit  = s.index.pools.find(env);
                const auto* head = (pit == s.index.pools.end() || pit->second.entries.empty()) ? nullptr : &pit->second.entries.front();
                const bool  contentInPool = InPool(s, env, sl.contentKey);
                bool        need = sl.owner != env || !sl.fresh || !contentInPool;
                if (sl.refreshPending) {
                    if (now < sl.refreshAtMs) continue;
                    if (s.playHandle[i].load() != sl.eventHandle || s.playIdx[i].load() != sl.eventIdx) continue;  // newer event re-arms
                    need = true;
                } else if (sl.replay && sl.owner == env && !head) {
                    need = false;  // a replay is in place and still nothing new: leave it until the next play
                }
                if (!need) {
                    sl.failing = false;
                    continue;
                }
                if (now < sl.retryAtMs) continue;
                if (head) {
                    job.kind    = Job::Kind::kPrepare;
                    job.slot    = i;
                    job.env     = env;
                    job.key     = head->Key();
                    job.file    = head->file;
                    job.display = head->Display();
                    s.inUse.insert(job.file);
                    return job;
                }
                // Pool dry. While this slot is the one playing, rotate recently played tracks (A-F2); otherwise the
                // slot is not ready and the game thread falls back to the vanilla tracks.
                if (sl.refreshPending && s.playCurrent[i].load()) {
                    if (const auto r = s.index.recent.find(env); r != s.index.recent.end()) {
                        for (const auto& key : r->second) {  // oldest first
                            if (key == sl.contentKey) continue;
                            const auto [svc, id] = SplitKey(key);
                            if (svc.empty() || id.empty()) continue;
                            job.candidates.push_back(
                                { key, { mj::pool::CacheFileName(svc, id, "flac"), mj::pool::CacheFileName(svc, id, "wav") } });
                        }
                    }
                    if (!job.candidates.empty()) {
                        job.kind   = Job::Kind::kPrepare;
                        job.slot   = i;
                        job.env    = env;
                        job.replay = true;
                        return job;
                    }
                }
                if (!sl.failing) spdlog::warn("[slots] slot {:02} ({}): pool empty, no track to place (environment degrades to Vanilla)", i, env);
                sl.failing        = true;
                sl.fresh          = sl.owner == env && sl.fresh && contentInPool;
                sl.refreshPending = false;  // nothing to place; a new track arrives through the normal "need" path
            }
            return job;
        }

        void DoCommit(Shared& s, const Job& j) {
            const auto r = [&] {
                std::uint64_t bytes = 0;
                bool          first = false;
                {
                    std::scoped_lock lk(s.mx);
                    bytes = s.slots[j.slot].prepBytes;
                    first = s.slots[j.slot].renameAttempts == 0;
                }
                return Slots::CommitSlot(j.slot, bytes, first, j.env, j.key, j.file);
            }();
            bool discard = false;
            {
            std::scoped_lock lk(s.mx);
            auto&            sl = s.slots[j.slot];
            ++sl.renameAttempts;
            if (r.ok) {
                sl.prepared       = false;
                sl.owner          = j.env;
                sl.contentKey     = j.key;
                sl.contentFile    = j.file;
                sl.fresh          = !j.replay;
                sl.replay         = j.replay;
                sl.failing        = false;
                sl.refreshPending = false;
                sl.retryAtMs      = 0;
                sl.renameAttempts = 0;
                sl.deniedAttempts = 0;
                sl.lastError.clear();
                spdlog::info("[slots] slot {:02} ({}) <- {}{} ({:.1f} MB)", j.slot, j.env, j.replay ? "REPLAY " : "", j.display,
                             sl.prepBytes / 1048576.0);
                return;
            }
            if (r.accessDenied) ++sl.deniedAttempts;
            if (!r.retryable || sl.deniedAttempts >= kMaxDenied) {
                // Give up loudly: drop the .tmp, the slot counts as failing (Vanilla when idle), try again much later.
                sl.prepared       = false;
                sl.failing        = true;
                sl.renameAttempts = 0;
                sl.deniedAttempts = 0;
                sl.retryAtMs      = SteadyMs() + kGiveUpRetryMs;
                spdlog::error("[slots] slot {:02} ({}): giving up on this write: {}", j.slot, j.env, r.error);
                sl.lastError = r.error;
                discard      = true;
            } else {
            const long long wait = RenameBackoff(sl.renameAttempts);
            sl.retryAtMs         = SteadyMs() + wait;
            if (sl.lastError != r.error || sl.renameAttempts <= 2)
                spdlog::warn("[slots] slot {:02} ({}): {}; the prepared file is kept, rename retried in {} s when the slot is not playing",
                             j.slot, j.env, r.error, wait / 1000);
            sl.lastError = r.error;
            }
            }
            if (discard) Slots::DiscardTmp(j.slot);  // outside the lock
        }

        void DoPrepare(Shared& s, Job j, const std::stop_token& a_st) {
            fs::path src;
            if (j.replay) {
                // First candidate whose cache file still exists (outside the lock).
                fs::path dir;
                {
                    std::scoped_lock lk(s.mx);
                    dir = s.cacheDir;
                }
                for (const auto& [key, files] : j.candidates) {
                    for (const auto& f : files) {
                        std::error_code ec;
                        if (fs::exists(dir / f, ec)) {
                            j.key     = key;
                            j.file    = f;
                            j.display = key;
                            src       = dir / f;
                            break;
                        }
                    }
                    if (!src.empty()) break;
                }
                std::scoped_lock lk(s.mx);
                auto&            sl = s.slots[j.slot];
                if (src.empty()) {
                    if (!sl.failing) spdlog::warn("[slots] slot {:02} ({}): pool empty and no recently played track is cached; the "
                                                  "previous track repeats", j.slot, j.env);
                    sl.failing        = true;
                    sl.refreshPending = false;
                    return;
                }
                s.inUse.insert(j.file);
            } else {
                std::scoped_lock lk(s.mx);
                src = s.cacheDir / j.file;
            }
            const auto r = Slots::PrepareSlot(j.slot, src, a_st);
            std::scoped_lock lk(s.mx);
            s.inUse.erase(j.file);
            auto& sl = s.slots[j.slot];
            if (r.ok) {
                sl.prepared       = true;
                sl.prepEnv        = j.env;
                sl.prepKey        = j.key;
                sl.prepFile       = j.file;
                sl.prepDisplay    = j.display;
                sl.prepReplay     = j.replay;
                sl.prepBytes      = r.bytes;
                sl.renameAttempts = 0;
                sl.deniedAttempts = 0;
                sl.retryAtMs      = 0;  // commit right away (the next loop iteration)
                return;
            }
            sl.retryAtMs = SteadyMs() + kFailRetryMs;
            if (r.sourceError && !j.replay) {
                // A-F4: the pooled file itself is bad: drop it from the pool (not "played"), so the next one is used.
                mj::pool::RemoveEntry(s.index, j.env, j.key);
                s.envs[j.env].rejected[j.key] = SteadyMs() + kSourceRejectMs;
                sl.retryAtMs                  = 0;
                MarkDirtyLocked();
                spdlog::error("[slots] slot {:02} ({}): pooled track {} is unusable and was dropped: {}", j.slot, j.env, j.display, r.error);
                return;
            }
            sl.failing = true;
            if (sl.lastError != r.error) spdlog::error("[slots] slot {:02} ({}): write FAILED: {}", j.slot, j.env, r.error);
            sl.lastError = r.error;
        }

    }  // namespace

    void SlotThread(std::stop_token a_st) {
        auto& s = S();
        spdlog::info("[slots] thread started; slot dir {}", Slots::Utf8(Slots::HarmonyDir()));
        const auto known = Slots::LoadManifest();
        Slots::CleanTempFiles();
        {
            std::scoped_lock lk(s.mx);
            for (const auto& [slot, rec] : known) {
                auto& sl       = s.slots[slot];
                sl.owner       = rec.env;
                sl.contentKey  = rec.key;
                sl.contentFile = rec.file;
                sl.fresh       = true;  // verified against the pool before use (contentInPool)
            }
        }
        long long lastKillPoll = 0;
        while (!a_st.stop_requested()) {
            // Kill-switch poll (A-F3): a stat every 2 s, never behind a download.
            if (SteadyMs() - lastKillPoll >= kKillPollMs) {
                lastKillPoll = SteadyMs();
                const bool k = Slots::KillSwitchFilePresent();
                if (k != s.killFile.load()) {
                    s.killFile = k;
                    spdlog::warn("[retarget] kill switch file {}", k ? "APPEARED: retarget stops, originals restored when safe" : "removed");
                    std::scoped_lock lk(s.mx);
                    s.replanWanted = true;
                    s.jobStop.request_stop();
                    s.poolCv.notify_all();
                }
            }
            std::vector<std::function<void()>> after;
            Job                                job;
            {
                std::unique_lock lk(s.mx);
                (void)s.slotCv.wait_for(lk, a_st, 250ms, [&] { return !s.events.empty(); });
                if (a_st.stop_requested()) break;
                while (!s.events.empty()) {
                    const PlayEvent e = std::move(s.events.front());
                    s.events.pop_front();
                    OnEventLocked(s, e, after);
                }
                if (s.plan && s.cacheReady) job = PickJobLocked(s, *s.plan, SteadyMs());
            }
            for (auto& f : after) f();
            switch (job.kind) {
            case Job::Kind::kPrepare: DoPrepare(s, std::move(job), a_st); break;
            case Job::Kind::kCommit:  DoCommit(s, job); break;
            case Job::Kind::kDiscard: Slots::DiscardTmp(job.slot); break;
            case Job::Kind::kNone:    break;
            }
        }
        spdlog::info("[slots] thread stopped");
    }

}  // namespace MJ::Pool::detail
