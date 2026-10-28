// Generalised retarget (docs/INTEGRATION-1.0.md A.1/A.3). See Retarget.h.
//
// ENGINE-MEMORY WRITES in this file (and nowhere else in the plugin):
//   Apply()   : tracks[i] = slot wrapper's BSIMusicTrack subobject   (Single/Palette entries; or one of our
//               wrappers moved to another slot)
//   Restore() : tracks[i] = the original pointer saved by Apply()   (only where the entry is still ours)
// Plain pointer stores into the type's EXISTING element storage: nothing resizes/pushes/erases/reserves, touches
// size/capacity, currentTrackIndex, trackHistory, any wrapper form or any inner track. Both run only from Tick()
// (the 250 ms SKSE task, game thread) right after IsSafe() returned true for that type in the same task.
//
// Layout evidence (1.6.1170, M0a/M0b/M0c field logs): BSIMusicType::tracks at +0x30 read for all 215 types,
// the MUST wrapper's BSIMusicTrack subobject at +0x20; the M0b/M0c spike wrote exactly these stores for
// MUSExploreTundra (125 entries) with no error over two sessions. The engine re-opens the slot file on every
// play (new sound handle per pick) and resumes the same handle after an interruption (M0b (c)).
#include "PCH.h"
#include "retarget/Retarget.h"

#include "pool/Pool.h"
#include "probe/Probe.h"

#include <map>

namespace MJ::Retarget {

    namespace {

        constexpr std::string_view kPluginName = "MarthJukebox.esp"sv;
        constexpr int              kSlots      = mj::retarget::kSlotCount;
        constexpr std::uint32_t    kInactive   = 0;  // MUSIC_STATUS
        constexpr std::uint32_t    kFinished   = 4;
        constexpr long long        kServedGraceMs = 10'000;  // slot may stay bound this long after its track was played

        RE::BGSMusicTrackFormWrapper* g_wrapper[kSlots]{};
        RE::BSIMusicTrack*            g_iface[kSlots]{};
        bool                          g_ready{ false };
        bool                          g_catalogued{ false };

        struct TypeState {
            RE::BGSMusicType*                            form{ nullptr };
            RE::BSIMusicType*                            type{ nullptr };
            std::string                                  name;
            int                                          bound{ -1 };
            std::string                                  boundEnv;
            std::map<std::uint32_t, RE::BSIMusicTrack*>  originals;
            std::uint32_t                                appliedSize{ 0 };
            std::uint32_t                                ourCount{ 0 };   // entries pointing at the slot after Apply()
            bool                                         deferLogged{ false };
        };
        std::vector<TypeState>                                 g_types;  // catalogue order (== Plan::typeSlot index)
        std::unordered_map<const RE::BSIMusicType*, std::size_t> g_typeIndex;

        struct PlayState {
            bool                    have{ false };
            const RE::BSIMusicType* type{ nullptr };
            std::uint32_t           handle{ 0 }, idx{ 0 };
            std::uint32_t           n{ 0 };
            // Diagnosis (Deck field log): 500 ms after a play event, log what the engine is doing with it (first few per
            // slot per session): still current + same handle + Playing = it really plays; anything else = a failed start.
            long long               checkAtMs{ -1 };
            int                     checksLogged{ 0 };
            long long               noHandleSinceMs{ -1 };  // our entry is current but has no sound handle
            bool                    noHandleLogged{ false };
        };
        PlayState                   g_play[kSlots];
        std::string                 g_published[kSlots];
        std::uint64_t               g_loggedPlan{ 0 };


        const char* StatusName(std::uint32_t a_v) {
            switch (a_v) {
            case 0: return "Inactive";
            case 1: return "Playing";
            case 2: return "Paused";
            case 3: return "Finishing";
            case 4: return "Finished";
            default: return "?";
            }
        }

        std::string EdidOf(const RE::TESForm* a_f) {
            const char* e = a_f ? a_f->GetFormEditorID() : nullptr;
            return e ? std::string(e) : std::string();
        }

        std::string PluginOf(const RE::TESForm* a_f) {
            const auto* f = a_f ? a_f->GetFile(0) : nullptr;
            return f ? std::string(f->GetFilename()) : std::string();
        }

        enum class Kind { kOurs, kReplaceable, kSilent, kOther };

        Kind Classify(const RE::BSIMusicTrack* a_tr) {
            if (!a_tr) return Kind::kOther;
            if (SlotOf(a_tr) >= 0) return Kind::kOurs;
            const auto* w = Probe::WrapperOf(a_tr);
            if (!w || !w->track) return Kind::kOther;
            switch (w->track->GetType()) {
            case RE::BSIMusicTrack::TrackType::kSingleTrack:
            case RE::BSIMusicTrack::TrackType::kPalette:     return Kind::kReplaceable;
            case RE::BSIMusicTrack::TrackType::kSilentTrack: return Kind::kSilent;
            default:                                         return Kind::kOther;
            }
        }

        // M0b write precondition: not current, not queued, typeStatus Inactive or Finished.
        bool IsSafe(const RE::BSMusicManager* a_mgr, const RE::BSIMusicType* a_t, std::string& a_why) {
            if (!a_mgr) {
                a_why = "BSMusicManager is null";
                return false;
            }
            if (a_mgr->current == a_t) {
                a_why = "it is the current music type";
                return false;
            }
            for (std::uint32_t i = 0; i < a_mgr->musicQueue.size(); ++i) {
                if (a_mgr->musicQueue[i] == a_t) {
                    a_why = std::format("it is in the music queue at {}", i);
                    return false;
                }
            }
            if (const auto st = a_t->typeStatus.underlying(); st != kInactive && st != kFinished) {
                a_why = std::format("typeStatus={}", StatusName(st));
                return false;
            }
            return true;
        }

        // ENGINE WRITE #1. Caller: Tick(), right after IsSafe() for this type.
        void Apply(TypeState& a_t, int a_slot, const std::string& a_env) {
            auto&               arr = a_t.type->tracks;
            const std::uint32_t n   = arr.size();
            std::uint32_t       replaced = 0, moved = 0, already = 0, silent = 0, other = 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                switch (Classify(arr[i])) {
                case Kind::kOurs:
                    if (arr[i] != g_iface[a_slot]) {
                        arr[i] = g_iface[a_slot];  // ours, other slot: originals already recorded
                        ++moved;
                    } else {
                        ++already;
                    }
                    break;
                case Kind::kReplaceable:
                    a_t.originals.insert_or_assign(i, arr[i]);
                    arr[i] = g_iface[a_slot];  // plain pointer store into existing storage
                    ++replaced;
                    break;
                case Kind::kSilent: ++silent; break;
                case Kind::kOther:  ++other; break;
                }
            }
            a_t.bound       = a_slot;
            a_t.boundEnv    = a_env;
            a_t.appliedSize = n;
            a_t.ourCount    = replaced + moved + already;
            spdlog::info("[retarget] {} -> slot {:02} ({}): tracks={} replaced={} moved={} already={} kept-silent={} kept-other={} originals={}",
                         a_t.name, a_slot, a_env, n, replaced, moved, already, silent, other, a_t.originals.size());
        }

        // ENGINE WRITE #2. Caller: Tick(), right after IsSafe() for this type.
        void Restore(TypeState& a_t, const char* a_why) {
            auto&               arr = a_t.type->tracks;
            const std::uint32_t n   = arr.size();
            std::uint32_t       restored = 0, skipped = 0;
            for (const auto& [i, orig] : a_t.originals) {
                if (i < n && SlotOf(arr[i]) >= 0) {
                    arr[i] = orig;
                    ++restored;
                } else {
                    ++skipped;  // somebody else changed the entry after us: left alone
                }
            }
            a_t.originals.clear();
            spdlog::warn("[retarget] {} RESTORED to vanilla ({}): {} entries put back, {} skipped (tracks={}, was slot {:02} {})", a_t.name,
                         a_why, restored, skipped, n, a_t.bound, a_t.boundEnv);
            a_t.bound = -1;
            a_t.boundEnv.clear();
        }

        void BuildCatalogue() {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            std::vector<mj::retarget::TypeFacts> facts;
            auto&                                arr = dh->GetFormArray<RE::BGSMusicType>();
            for (std::uint32_t ti = 0; ti < arr.size(); ++ti) {
                RE::BGSMusicType* mt = arr[ti];
                if (!mt) continue;
                TypeState t;
                t.form = mt;
                t.type = static_cast<RE::BSIMusicType*>(mt);
                t.name = Probe::TypeName(t.type);
                mj::retarget::TypeFacts f;
                f.editorId   = EdidOf(mt);
                f.plugin     = PluginOf(mt);
                f.trackCount = t.type->tracks.size();
                bool allSilent = f.trackCount > 0;
                for (std::uint32_t i = 0; i < f.trackCount; ++i) {
                    const auto* w = Probe::WrapperOf(t.type->tracks[i]);
                    if (!w || !w->track || w->track->GetType() != RE::BSIMusicTrack::TrackType::kSilentTrack) {
                        allSilent = false;
                        break;
                    }
                }
                f.allSilent = allSilent;
                g_typeIndex[t.type] = g_types.size();
                g_types.push_back(std::move(t));
                facts.push_back(std::move(f));
            }
            spdlog::info("[retarget] catalogue: {} music types handed to the pool thread (t=+{}ms)", facts.size(), Probe::NowMs());
            Pool::SetCatalogue(std::move(facts));
        }

        // ---- Field diagnosis #3 (Deck: combat bound and ready, yet no combat play event and no music heard) ----
        // At info level: every change of BSMusicManager::current (rate-limited), and one line per retargeted type that
        // becomes current without producing a play event within 2 s. Read-only.
        const RE::BSIMusicType* g_lastCurrent{ nullptr };
        bool                    g_haveLastCurrent{ false };
        int                     g_currentLines{ 0 };            // lines logged this session
        constexpr int           kCurrentLinesFull = 200;        // then every 20th change
        long long               g_curSinceMs{ -1 };             // when a bound type became current
        std::size_t             g_curType{ ~std::size_t{ 0 } }; // its catalogue index
        bool                    g_curPlayed{ false };           // a play event of its slot since then
        std::vector<bool>       g_noPlayLogged;                 // per catalogue index, once per session

        std::string QueueNames(const RE::BSMusicManager* a_mgr) {
            std::string q;
            for (std::uint32_t i = 0; a_mgr && i < a_mgr->musicQueue.size() && i < 4; ++i)
                q += (q.empty() ? "" : ",") + Probe::TypeName(a_mgr->musicQueue[i]);
            return q;
        }

        std::string EntryInfo(const RE::BSIMusicType* a_t, int a_slot) {
            const std::uint32_t idx = a_t->currentTrackIndex;
            const std::uint32_t n   = a_t->tracks.size();
            if (idx >= n) return std::format("idx={}/{} (out of range)", idx, n);
            const RE::BSIMusicTrack* e = a_t->tracks[idx];
            const int                s = SlotOf(e);
            if (s < 0) {
                const auto* w = Probe::WrapperOf(e);
                return std::format("idx={}/{} entry=vanilla({:08X}){}", idx, n, w ? w->GetFormID() : 0u,
                                   a_slot >= 0 ? "  <- NOT our slot although the type is retargeted" : "");
            }
            const auto* st = static_cast<const RE::BGSMusicSingleTrack*>(g_wrapper[s]->track);
            return std::format("idx={}/{} entry=slot {:02} handle=0x{:08X} trackStatus={}", idx, n, s, st->trackHandle.soundID,
                               StatusName(st->trackStatus.underlying()));
        }

        void TickCurrentDiagnosis(const RE::BSMusicManager* a_mgr) {
            const RE::BSIMusicType* cur = a_mgr ? a_mgr->current : nullptr;
            const long long         now = Pool::SteadyMs();
            if (!g_haveLastCurrent || cur != g_lastCurrent) {
                g_haveLastCurrent = true;
                g_lastCurrent     = cur;
                std::size_t idx   = ~std::size_t{ 0 };
                if (cur) {
                    if (const auto it = g_typeIndex.find(cur); it != g_typeIndex.end()) idx = it->second;
                }
                const bool  known = idx != ~std::size_t{ 0 };
                const int   slot  = known ? g_types[idx].bound : -1;
                g_curType         = slot >= 0 ? idx : ~std::size_t{ 0 };
                g_curSinceMs      = slot >= 0 ? now : -1;
                g_curPlayed       = false;
                ++g_currentLines;
                if (g_currentLines <= kCurrentLinesFull || g_currentLines % 20 == 0) {
                    if (!cur) {
                        spdlog::info("[music] current: none  queue=[{}]", QueueNames(a_mgr));
                    } else {
                        spdlog::info("[music] current: {} {} typeStatus={} {}  queue=[{}]{}", Probe::TypeName(cur),
                                     !known ? "(not a catalogued music type: never retargeted)"
                                            : (slot >= 0 ? std::format("-> slot {:02} ({})", slot, g_types[idx].boundEnv) : std::string("(vanilla)")),
                                     StatusName(cur->typeStatus.underlying()), EntryInfo(cur, slot), QueueNames(a_mgr),
                                     g_currentLines == kCurrentLinesFull ? "  (further changes: every 20th)" : "");
                    }
                }
            }
            // A retargeted type is current but its slot produced no play event within 2 s: say what the engine shows.
            if (g_curSinceMs >= 0 && !g_curPlayed && now - g_curSinceMs >= 2000 && g_curType < g_types.size()) {
                if (g_noPlayLogged.size() < g_types.size()) g_noPlayLogged.resize(g_types.size(), false);
                if (!g_noPlayLogged[g_curType]) {
                    g_noPlayLogged[g_curType] = true;
                    const auto& t             = g_types[g_curType];
                    spdlog::warn("[music] {} is current (-> slot {:02} {}) but its slot gave no play event for 2 s: typeStatus={} {} "
                                 "(logged once per type per session)",
                                 t.name, t.bound, t.boundEnv, StatusName(t.type->typeStatus.underlying()), EntryInfo(t.type, t.bound));
                }
                g_curSinceMs = -1;
            }
        }

        // Play events: a slot wrapper is the current track of its (current) type and its sound handle or the index
        // changed (M0b: re-plays keep status Playing but get a new handle). Last-seen values survive while the slot
        // is not current, so a resume after combat (same handle, same index) is not an event.
        void TickPlay(const RE::BSMusicManager* a_mgr) {
            bool          current[kSlots]{};
            std::uint32_t liveHandle[kSlots]{}, liveIdx[kSlots]{};
            if (a_mgr && a_mgr->current) {
                const auto it = g_typeIndex.find(a_mgr->current);
                if (it != g_typeIndex.end()) {
                    const auto&         t   = g_types[it->second];
                    const std::uint32_t idx = t.type->currentTrackIndex;
                    if (t.bound >= 0 && idx < t.type->tracks.size() && t.type->tracks[idx] == g_iface[t.bound]) {
                        const int s = t.bound;
                        // Inner track checked Single at load: static_cast is a no-op (primary base).
                        const auto* st = static_cast<const RE::BGSMusicSingleTrack*>(g_wrapper[s]->track);
                        const auto  h  = st->trackHandle.soundID;
                        current[s]     = true;
                        liveHandle[s]  = h;
                        liveIdx[s]     = idx;
                        auto& p        = g_play[s];
                        if (h != RE::BSSoundHandle::kInvalidID && h != 0 &&
                            (!p.have || h != p.handle || idx != p.idx || t.type != p.type)) {
                            p.have   = true;
                            p.type   = t.type;
                            p.handle = h;
                            p.idx    = idx;
                            ++p.n;
                            spdlog::debug("[retarget] slot {:02} play #{} in {} handle=0x{:08X} idx={} t=+{}ms", s, p.n, t.name, h, idx,
                                          Probe::NowMs());
                            if (spdlog::should_log(spdlog::level::debug)) Probe::LogTrackHistory(t.type, "play event");
                            Pool::PostPlayEvent(s, t.boundEnv, h, idx);
                            if (p.checksLogged < 5) p.checkAtMs = Pool::SteadyMs() + 500;
                            if (it->second == g_curType) g_curPlayed = true;
                        }
                    }
                }
            }
            const long long now = Pool::SteadyMs();
            for (int s = 0; s < kSlots; ++s) {
                auto& p = g_play[s];
                if (p.checkAtMs >= 0 && now >= p.checkAtMs) {
                    p.checkAtMs = -1;
                    ++p.checksLogged;
                    const auto* st = static_cast<const RE::BGSMusicSingleTrack*>(g_wrapper[s]->track);
                    spdlog::info("[retarget] slot {:02} play check +500 ms: slot still current={} handle now=0x{:08X} (event 0x{:08X}) "
                                 "inner trackStatus={}{}",
                                 s, current[s], st->trackHandle.soundID, p.handle, StatusName(st->trackStatus.underlying()),
                                 current[s] && st->trackHandle.soundID == p.handle ? "" : "  <- NOT playing: the start failed or it was skipped");
                }
                // Our entry is the current track but the engine never built a sound handle for it.
                const bool noHandle = current[s] && (liveHandle[s] == RE::BSSoundHandle::kInvalidID || liveHandle[s] == 0);
                if (!noHandle) {
                    p.noHandleSinceMs = -1;
                } else if (p.noHandleSinceMs < 0) {
                    p.noHandleSinceMs = now;
                } else if (!p.noHandleLogged && now - p.noHandleSinceMs >= 1000) {
                    p.noHandleLogged = true;
                    spdlog::warn("[retarget] slot {:02} is the current entry (idx={}) but has had no sound handle for 1 s: the game did "
                                 "not open {} (logged once per session)",
                                 s, liveIdx[s], mj::retarget::SlotFileName(s));
                }
                if (current[s]) Pool::PublishPlay(s, true, liveHandle[s], liveIdx[s]);
                else Pool::PublishPlay(s, false, p.handle, p.idx);
            }
        }

    }  // namespace

    int SlotOf(const RE::BSIMusicTrack* a_track) {
        if (!a_track) return -1;
        for (int s = 0; s < kSlots; ++s) {
            if (g_iface[s] == a_track) return s;
        }
        return -1;
    }

    void OnDataLoaded() {
        Pool::MarkGameThread();  // PerfProbe: the game thread's waits for the pool lock are measured from now on
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !dh->LookupModByName(kPluginName)) {
            spdlog::error("[retarget] DISABLED: {} is not in the load order (everything stays Vanilla)", kPluginName);
            return;
        }
        for (int s = 0; s < kSlots; ++s) {
            const RE::FormID local = mj::retarget::kFirstLocalId + s;
            auto*            w     = dh->LookupForm<RE::BGSMusicTrackFormWrapper>(local, kPluginName);
            if (!w || !w->track || w->track->GetType() != RE::BSIMusicTrack::TrackType::kSingleTrack) {
                spdlog::error("[retarget] DISABLED: slot wrapper {} (local 0x{:03X}) {}; install the {}-slot {} of this release",
                              mj::retarget::SlotEditorId(s), local,
                              !w ? "not found" : (!w->track ? "has no inner track" : "inner track is not Single"), kSlots, kPluginName);
                return;
            }
            g_wrapper[s] = w;
            g_iface[s]   = static_cast<RE::BSIMusicTrack*>(w);
        }
        spdlog::info("[retarget] {} slot wrappers resolved ({:08X} .. {:08X}); writes only in the watcher task while a type is not "
                     "current, not queued and Inactive/Finished",
                     kSlots, g_wrapper[0]->GetFormID(), g_wrapper[kSlots - 1]->GetFormID());
        g_ready = true;
    }

    void LogState(const char* a_why) {
        std::size_t bound = 0;
        for (auto& t : g_types) {
            if (t.bound < 0) continue;
            ++bound;
            // A-F10 (read-only): after a load the bound types must still hold exactly the entries we placed. A mismatch
            // (something rebuilt or edited the array) is logged loudly and re-asserted on the next safe tick.
            std::uint32_t ours = 0;
            for (std::uint32_t i = 0; i < t.type->tracks.size(); ++i) ours += t.type->tracks[i] == g_iface[t.bound];
            if (ours != t.ourCount || t.type->tracks.size() != t.appliedSize) {
                spdlog::error("[retarget] {}: {} has {} entries on slot {:02} (expected {}), tracks={} (was {}); re-asserting when safe",
                              a_why, t.name, ours, t.bound, t.ourCount, t.type->tracks.size(), t.appliedSize);
                t.appliedSize = 0;  // forces Apply() on the next tick where the type is safe
            }
            spdlog::info("[retarget] {}: {} -> slot {:02} ({}) tracks={} originals={} typeStatus={}", a_why, t.name, t.bound, t.boundEnv,
                         t.type->tracks.size(), t.originals.size(), StatusName(t.type->typeStatus.underlying()));
        }
        spdlog::info("[retarget] {}: {} music type(s) retargeted, t=+{}ms", a_why, bound, Probe::NowMs());
    }

    void Tick() {
        if (!g_ready) return;
        if (!g_catalogued) {
            g_catalogued = true;
            BuildCatalogue();
        }
        const auto  plan   = Pool::CurrentPlan();
        const auto  views  = Pool::SlotViews();
        const bool  killed = Pool::KillSwitchSeen();
        const auto  now    = Pool::SteadyMs();
        const auto* mgr    = RE::BSMusicManager::GetSingleton();

        // Diagnosis first: it resets "played" on a type change, and a play event in the same tick must count (field #4:
        // the "no play event for 2 s" warning fired although the PLAY line was logged in that very tick).
        TickCurrentDiagnosis(mgr);
        TickPlay(mgr);

        if (plan && plan->gen != g_loggedPlan) {
            g_loggedPlan = plan->gen;
            for (auto& t : g_types) t.deferLogged = false;
        }

        for (std::size_t i = 0; i < g_types.size(); ++i) {
            auto&       t       = g_types[i];
            int         desired = -1;
            std::string env;
            const char* why     = "plan";
            if (killed) {
                why = "kill switch";
            } else if (!plan || !plan->engineWrites) {
                why = "retarget off";
            } else if (i < plan->typeSlot.size() && plan->typeSlot[i] >= 0) {
                const int   s = plan->typeSlot[i];
                const auto& v = views[s];
                env           = plan->slotEnv[s];
                const bool ready = mj::retarget::SlotReady(v.owner, env, v.fresh, v.replay, v.failing, v.servedAtMs, now, kServedGraceMs);
                if (ready) desired = s;
                else why = "slot not ready (pool empty or file not written)";
            } else {
                why = "environment not active";
            }
            const bool reassert = t.bound >= 0 && desired == t.bound && t.type->tracks.size() != t.appliedSize;
            if (desired == t.bound && t.boundEnv == env && !reassert) {
                t.deferLogged = false;
                continue;
            }
            if (desired < 0 && t.bound < 0) continue;
            std::string unsafe;
            if (!IsSafe(mgr, t.type, unsafe)) {
                if (!t.deferLogged)
                    spdlog::info("[retarget] {}: change to {} deferred: {}", t.name, desired < 0 ? std::string("vanilla") : std::format("slot {:02}", desired),
                                 unsafe);
                t.deferLogged = true;
                continue;
            }
            t.deferLogged = false;
            if (t.bound >= 0 && (desired != t.bound || t.boundEnv != env)) Restore(t, why);
            if (desired >= 0) Apply(t, desired, env);
        }

        // Which environment's types point at each slot ("" none, "*" several): the slot thread only writes a
        // slot for its planned environment when no other environment still uses it.
        std::string bound[kSlots];
        for (const auto& t : g_types) {
            if (t.bound < 0) continue;
            auto& b = bound[t.bound];
            b       = b.empty() || b == t.boundEnv ? t.boundEnv : std::string("*");
        }
        for (int s = 0; s < kSlots; ++s) {
            if (bound[s] != g_published[s]) {
                g_published[s] = bound[s];
                Pool::PublishBound(s, bound[s]);
            }
        }
    }

}  // namespace MJ::Retarget
