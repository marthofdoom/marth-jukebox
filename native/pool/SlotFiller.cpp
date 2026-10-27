// Slot thread: keeps every planned slot file holding its environment's next (unplayed) pool track, and turns
// play events into "served" (docs/INTEGRATION-1.0.md A.3, docs/M0C-RESULTS.md). File IO only; no engine memory.
//
// Per slot s planned for environment E:
//   - prefill / re-target: when the file does not hold an unplayed track of E's pool, write E's pool head;
//   - play event (game thread): the track heard is the one placed before; it is marked served (removed from
//     the pool, added to the recent list, reported to Harmony), and once the play has been stable for 1.5 s the
//     NEXT pool head is renamed over the (playing) file;
//   - the file is never written for E while another environment's types still point at the slot.
#include "PCH.h"
#include "pool/Pool_internal.h"

#include "slots/SlotWriter.h"

namespace MJ::Pool::detail {

    namespace {

        namespace fs = std::filesystem;

        constexpr long long kStableMs    = 1500;   // M0c: let the engine open the file first
        constexpr long long kRetryMs     = 2000;   // retryable rename failure (sharing violation)
        constexpr long long kFailRetryMs = 30000;  // refused / failed write

        struct Job {
            int         slot = -1;
            std::string env;
            std::string key, file, display;
            fs::path    path;
        };

        // Caller holds mx. Handles one play event.
        void OnEventLocked(Shared& s, const PlayEvent& e, std::vector<std::function<void()>>& after) {
            auto& sl = s.slots[e.slot];
            if (sl.owner == e.env && sl.fresh) {
                const auto key = sl.contentKey;
                std::string service, id, display = key;
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
                if (s.played && !service.empty()) {
                    auto fn = s.played;
                    after.push_back([fn, env = e.env, service, id] { fn(env, service, id); });
                }
            } else {
                spdlog::warn("[slots] slot {:02} ({}) PLAY handle=0x{:08X} idx={}: the file held no unplayed track of this environment "
                             "(owner {}, fresh {}); the previous track repeats",
                             e.slot, e.env, e.handle, e.idx, sl.owner.empty() ? "none" : sl.owner, sl.fresh);
            }
            sl.refreshPending = true;
            sl.refreshAtMs    = e.ms + kStableMs;
            sl.eventHandle    = e.handle;
            sl.eventIdx       = e.idx;
        }

        // Caller holds mx. Picks at most one slot write to do now; also maintains `failing`.
        bool PickJobLocked(Shared& s, const Plan& plan, Job& job, long long now) {
            for (int i = 0; i < kSlots; ++i) {
                auto&             sl = s.slots[i];
                const std::string& env = plan.slotEnv[i];
                if (env.empty() || !plan.engineWrites) continue;
                if (!sl.bound.empty() && sl.bound != env) continue;  // the game thread has not restored the old owner yet
                const auto  pit  = s.index.pools.find(env);
                const auto* head = (pit == s.index.pools.end() || pit->second.entries.empty()) ? nullptr : &pit->second.entries.front();
                bool        contentInPool = false;
                if (pit != s.index.pools.end()) {
                    for (const auto& en : pit->second.entries) contentInPool |= en.Key() == sl.contentKey;
                }
                bool need = sl.owner != env || !sl.fresh || !contentInPool;
                if (sl.refreshPending) {
                    if (now < sl.refreshAtMs) continue;
                    if (s.playHandle[i].load() != sl.eventHandle || s.playIdx[i].load() != sl.eventIdx) continue;  // newer event re-arms
                    need = true;
                }
                if (!need) {
                    sl.failing = false;
                    continue;
                }
                if (!head) {
                    if (!sl.failing) spdlog::warn("[slots] slot {:02} ({}): pool empty, no track to place (environment degrades to Vanilla)", i, env);
                    sl.failing = true;
                    sl.fresh   = sl.owner == env && sl.fresh && contentInPool;
                    continue;
                }
                if (now < sl.retryAtMs) continue;
                job.slot    = i;
                job.env     = env;
                job.key     = head->Key();
                job.file    = head->file;
                job.display = head->Display();
                job.path    = s.cacheDir / head->file;
                s.inUse.insert(job.file);
                return true;
            }
            return false;
        }

    }  // namespace

    void SlotThread(std::stop_token a_st) {
        auto& s = S();
        spdlog::info("[slots] thread started; slot dir {}", Slots::Utf8(Slots::HarmonyDir()));
        Slots::LoadManifest();
        Slots::CleanTempFiles();
        while (!a_st.stop_requested()) {
            std::vector<std::function<void()>> after;
            Job                                job;
            bool                               haveJob = false;
            {
                std::unique_lock lk(s.mx);
                (void)s.slotCv.wait_for(lk, a_st, 250ms, [&] { return !s.events.empty(); });
                if (a_st.stop_requested()) break;
                while (!s.events.empty()) {
                    const PlayEvent e = std::move(s.events.front());
                    s.events.pop_front();
                    OnEventLocked(s, e, after);
                }
                if (s.plan && s.cacheReady) haveJob = PickJobLocked(s, *s.plan, job, SteadyMs());
            }
            for (auto& f : after) f();
            if (!haveJob) continue;

            const auto r = Slots::WriteSlot(job.slot, job.path, a_st);
            std::scoped_lock lk(s.mx);
            s.inUse.erase(job.file);
            auto& sl = s.slots[job.slot];
            if (r.ok) {
                sl.owner          = job.env;
                sl.contentKey     = job.key;
                sl.contentFile    = job.file;
                sl.fresh          = true;
                sl.failing        = false;
                sl.refreshPending = false;
                sl.retryAtMs      = 0;
                sl.lastError.clear();
                spdlog::info("[slots] slot {:02} ({}) <- {} ({:.1f} MB, {} ms)", job.slot, job.env, job.display, r.bytes / 1048576.0, r.ms);
            } else {
                // The file is unchanged. A retryable failure (a sharing violation while the engine holds the file on
                // Windows) is retried in 2 s; anything else in 30 s, and the slot counts as failing (Vanilla).
                sl.retryAtMs = SteadyMs() + (r.retryable ? kRetryMs : kFailRetryMs);
                if (!r.retryable) sl.failing = true;
                if (sl.lastError != r.error) spdlog::error("[slots] slot {:02} ({}): write FAILED: {}", job.slot, job.env, r.error);
                sl.lastError = r.error;
            }
        }
        spdlog::info("[slots] thread stopped");
    }

}  // namespace MJ::Pool::detail
