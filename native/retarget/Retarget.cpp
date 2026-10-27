// M0b retarget spike. See Retarget.h and DESIGN.md (main) section 8.6.
//
// ENGINE-MEMORY WRITES in this file (and nowhere else in the plugin):
//   Apply()   : tracks[i] = our wrapper's BSIMusicTrack subobject   (replace)
//   Restore() : tracks[i] = the original pointer saved by Apply()   (kill switch only)
// Both are plain pointer stores into the type's EXISTING element storage. Nothing here
// resizes/pushes/erases/reserves, touches size/capacity, currentTrackIndex,
// trackHistory, any wrapper form or any inner track. Both run only from Tick(), i.e.
// inside the 250 ms SKSE task on the main thread, and only after IsSafe() returned
// true IN THE SAME TASK with no engine call between the check and the stores.
//
// Layout evidence (1.6.1170, M0a field log of 2026-10-27): BSIMusicType::tracks at +0x30
// was read for all 215 types (sizes and elements matched the identity map, 0 cast
// mismatches), and the MUST wrapper's BSIMusicTrack subobject is at +0x20 ("iface
// offset 32" logged for every entry). Element storage = BSTArray data()[i] (fork
// include/RE/B/BSTArray.h operator[]). The write side is header + read evidence only.
#include "PCH.h"
#include "retarget/Retarget.h"
#include "probe/Probe.h"
#include "slots/SlotWriter.h"

#include <map>

namespace MJ::Retarget {

    namespace {

        constexpr RE::FormID       kTargetId   = 0x000B9DCB;  // MUSExploreTundra, Skyrim.esm
        constexpr std::string_view kTargetEdid = "MUSExploreTundra"sv;
        constexpr std::string_view kPluginName = "MarthJukebox.esp"sv;
        constexpr RE::FormID       kLocalId[2] = { 0x800, 0x801 };  // A, B (tools/make_esp.py)
        constexpr const char*      kSlotName[2] = { "A", "B" };

        // M0c: true = every replaceable entry points to wrapper A only (B unused) and the
        // slot test is the play-event experiment on A. false = the M0b two-slot layout
        // (alternating A,B) with the status-edge/settle refresh of the other slot.
        constexpr bool kSingleWrapper = true;

        constexpr std::uint32_t kInactive  = 0;  // BSIMusicType/BSIMusicTrack MUSIC_STATUS
        constexpr std::uint32_t kPlaying   = 1;
        constexpr std::uint32_t kFinished  = 4;

        // Everything below is touched on the main thread only (kDataLoaded, SKSE
        // messages and the watcher task all run there), so no locking.
        RE::BGSMusicType*             g_targetForm{ nullptr };
        RE::BSIMusicType*             g_target{ nullptr };
        RE::BGSMusicTrackFormWrapper* g_wrapper[2]{ nullptr, nullptr };
        RE::BSIMusicTrack*            g_iface[2]{ nullptr, nullptr };
        bool                          g_ready{ false };   // target + both wrappers resolved and sane
        bool                          g_killed{ false };  // kill switch seen; latched for the session

        // index -> the entry that was there before we replaced it (for Restore()).
        std::map<std::uint32_t, RE::BSIMusicTrack*> g_originals;

        std::uint32_t g_lastSize{ 0 };
        bool          g_firstTick{ true };
        bool          g_loggedEmpty{ false };
        bool          g_deferLogged{ false };

        std::uint32_t g_prevSlotStatus[2]{ kInactive, kInactive };
        bool          g_wantRefresh[2]{ false, false };
        // Settle rule (review F2): Finished does not mean the file is closed (the audio still
        // fades out for fadeTime). A slot is refreshed only after its inner track has been
        // continuously Inactive/Finished for the type's fadeTime + 1 s. g_idleSince is the
        // steady-clock ms (Slots::SteadyMs) when that idle run started, -1 while busy.
        long long g_idleSince[2]{ -1, -1 };
        bool      g_settleLogged[2]{ false, false };

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

        const char* MessageName(std::uint32_t a_type) {
            switch (a_type) {
            case SKSE::MessagingInterface::kInputLoaded:  return "kInputLoaded";
            case SKSE::MessagingInterface::kNewGame:      return "kNewGame";
            case SKSE::MessagingInterface::kPostLoadGame: return "kPostLoadGame";
            case SKSE::MessagingInterface::kDataLoaded:   return "kDataLoaded";
            default:                                      return "message";
            }
        }

        enum class Kind { kOurs, kReplaceable, kSilent, kOther };

        // Resolves an entry through the M0a identity map (no RTTI, no virtual call on the
        // wrapper itself); the inner track's GetType() is what M0a used for all 1776 slots.
        Kind Classify(const RE::BSIMusicTrack* a_tr, const char** a_innerName = nullptr) {
            if (a_innerName) *a_innerName = "?";
            if (!a_tr) return Kind::kOther;
            if (a_tr == g_iface[0] || a_tr == g_iface[1]) return Kind::kOurs;
            const auto* w = Probe::WrapperOf(a_tr);
            if (!w || !w->track) return Kind::kOther;
            switch (w->track->GetType()) {
            case RE::BSIMusicTrack::TrackType::kSingleTrack:
                if (a_innerName) *a_innerName = "Single";
                return Kind::kReplaceable;
            case RE::BSIMusicTrack::TrackType::kPalette:
                if (a_innerName) *a_innerName = "Palette";
                return Kind::kReplaceable;
            case RE::BSIMusicTrack::TrackType::kSilentTrack:
                return Kind::kSilent;
            default:
                return Kind::kOther;
            }
        }

        // The write precondition (DESIGN constraint 2, relaxed per review F1): the type is
        // not the current type, not anywhere in the music queue, and its own status is
        // Inactive or Finished. In the 1.6.1170 binary a type that stops being current
        // keeps typeStatus Finishing/Finished until DoPrepare (when it becomes current
        // again) resets it to Inactive, and nothing reads a non-queued type's tracks, so
        // Finished + not current + not queued is idle. Finishing is still refused.
        bool IsSafe(std::string& a_why) {
            const auto* mgr = RE::BSMusicManager::GetSingleton();
            if (!mgr) {
                a_why = "BSMusicManager is null";
                return false;
            }
            if (mgr->current == g_target) {
                a_why = "it is BSMusicManager::current";
                return false;
            }
            for (std::uint32_t i = 0; i < mgr->musicQueue.size(); ++i) {
                if (mgr->musicQueue[i] == g_target) {
                    a_why = std::format("it is in musicQueue at {}", i);
                    return false;
                }
            }
            if (const auto st = g_target->typeStatus.underlying(); st != kInactive && st != kFinished) {
                a_why = std::format("typeStatus={}", StatusName(st));
                return false;
            }
            return true;
        }

        // ENGINE WRITE #2. Caller: Tick(), after IsSafe() in the same task.
        void Restore() {
            auto&               arr = g_target->tracks;
            const std::uint32_t n   = arr.size();
            std::uint32_t       restored = 0, skipped = 0;
            for (const auto& [i, orig] : g_originals) {
                // Only put back an entry that is still ours; anything else was changed by
                // someone after us and is left alone.
                if (i < n && (arr[i] == g_iface[0] || arr[i] == g_iface[1])) {
                    arr[i] = orig;
                    ++restored;
                } else {
                    ++skipped;
                    spdlog::warn("[retarget] restore: entry [{}] is no longer ours (size {}); left as is", i, n);
                }
            }
            g_originals.clear();
            spdlog::warn("[retarget] RESTORED {}: {} entries put back, {} skipped (tracks={})", Probe::TypeName(g_target), restored,
                         skipped, n);
        }

        // ENGINE WRITE #1. Caller: Tick(), after IsSafe() in the same task. Overwrites only
        // Single/Palette entries; with kSingleWrapper all of them get A, otherwise A,B
        // alternate by position among our entries.
        void Apply() {
            auto&               arr = g_target->tracks;
            const std::uint32_t n   = arr.size();
            std::uint32_t       k = 0, newNow = 0, silent = 0, other = 0, countAB[2]{ 0, 0 };
            for (std::uint32_t i = 0; i < n; ++i) {
                const char* innerName = "?";
                switch (Classify(arr[i], &innerName)) {
                case Kind::kOurs:
                    ++countAB[arr[i] == g_iface[0] ? 0 : 1];
                    ++k;
                    break;
                case Kind::kReplaceable:
                    {
                        RE::BSIMusicTrack* orig = arr[i];
                        const int          s    = kSingleWrapper ? 0 : static_cast<int>(k % 2);
                        const auto*        w    = Probe::WrapperOf(orig);
                        g_originals.insert_or_assign(i, orig);
                        arr[i] = g_iface[s];  // plain pointer store into existing storage
                        ++countAB[s];
                        ++k;
                        ++newNow;
                        spdlog::info("[retarget]   [{}] {:08X} ({}) {:p} -> {}", i, w ? w->GetFormID() : 0u, innerName,
                                     static_cast<const void*>(orig), kSlotName[s]);
                        break;
                    }
                case Kind::kSilent: ++silent; break;
                case Kind::kOther:  ++other; break;
                }
            }
            spdlog::info("[retarget] {} tracks={} replaced={} (A={} B={}) kept-silent={} kept-other={} (new this pass: {}) "
                         "originals saved={} t=+{}ms",
                         Probe::TypeName(g_target), n, k, countAB[0], countAB[1], silent, other, newNow, g_originals.size(),
                         Probe::NowMs());
        }

        bool Idle(std::uint32_t a_st) { return a_st == kInactive || a_st == kFinished; }

        // fadeTime (BSIMusicType +0x10, seconds) + 1 s, in ms; clamped to [1 s, 61 s].
        long long SettleMs() {
            float f = g_target->fadeTime;
            if (!(f >= 0.0f)) f = 0.0f;  // also catches NaN
            if (f > 60.0f) f = 60.0f;
            return static_cast<long long>(f * 1000.0f) + 1000;
        }

        // M0c play-event state (main thread). A PLAY EVENT is: MUSExploreTundra is
        // BSMusicManager::current, its tracks[currentTrackIndex] is wrapper A, and (A's inner
        // trackHandle.soundID differs from the last seen one OR currentTrackIndex differs from
        // the last seen one). M0b showed a new sound handle per pick while the status stays
        // Playing, so status edges miss re-plays. Last-seen values are kept while A is not
        // current, so a resume after combat (same handle, same index) is not an event.
        bool          g_haveLastPlay{ false };
        std::uint32_t g_lastHandle{ 0 };
        std::uint32_t g_lastIdx{ 0 };
        std::uint32_t g_playN{ 0 };

        void TickPlayEvents() {
            const auto*   mgr      = RE::BSMusicManager::GetSingleton();
            bool          aCurrent = false;
            std::uint32_t handle = 0, idx = 0;
            if (mgr && mgr->current == g_target) {
                idx = g_target->currentTrackIndex;
                if (idx < g_target->tracks.size() && g_target->tracks[idx] == g_iface[0]) {
                    aCurrent = true;
                    // Inner track checked Single at load: static_cast is a no-op (primary base).
                    const auto* st = static_cast<const RE::BGSMusicSingleTrack*>(g_wrapper[0]->track);
                    handle         = st->trackHandle.soundID;
                }
            }
            if (!aCurrent) {
                Slots::PublishPlay(false, g_lastHandle, g_lastIdx);
                return;
            }
            Slots::PublishPlay(true, handle, idx);
            if (g_haveLastPlay && handle == g_lastHandle && idx == g_lastIdx) return;
            g_haveLastPlay = true;
            g_lastHandle   = handle;
            g_lastIdx      = idx;
            ++g_playN;
            spdlog::info("[slots] play #{} handle=0x{:08X} idx={} trackStatus={} t=+{}ms", g_playN, handle, idx,
                         StatusName(g_wrapper[0]->track->trackStatus.underlying()), Probe::NowMs());
            Probe::LogTrackHistory(g_target, std::format("play #{}", g_playN).c_str());
            Slots::RequestPlayExperiment(g_playN, handle, idx);
        }

        // Main thread. Our two inner tracks' status (+ idle-since, settle time) -> worker
        // atomics; refresh triggers. With kSingleWrapper only the publishing runs and the
        // play-event path replaces the status-edge trigger.
        void TickSlots() {
            const long long now    = Slots::SteadyMs();
            const long long settle = SettleMs();
            std::uint32_t   st[2]{};
            for (int s = 0; s < 2; ++s) {
                st[s] = g_wrapper[s]->track->trackStatus.underlying();
                if (!Idle(st[s])) {
                    g_idleSince[s]    = -1;
                    g_settleLogged[s] = false;
                } else if (g_idleSince[s] < 0) {
                    g_idleSince[s] = now;  // first tick of this idle run
                }
                Slots::PublishStatus(s, st[s], g_idleSince[s], settle);
            }
            for (int s = 0; s < 2; ++s) {
                if (st[s] == g_prevSlotStatus[s]) continue;
                spdlog::info("[slots] t=+{}ms slot {} inner status {} -> {}", Probe::NowMs(), kSlotName[s],
                             StatusName(g_prevSlotStatus[s]), StatusName(st[s]));
                if (!kSingleWrapper && st[s] == kPlaying) {
                    if (s == 0) Slots::RequestLockProbe(0, "A is Playing");
                    g_wantRefresh[1 - s] = true;
                    spdlog::info("[slots] {} started playing: refresh of slot {} wanted (posted once it is idle)", kSlotName[s],
                                 kSlotName[1 - s]);
                }
                g_prevSlotStatus[s] = st[s];
            }
            if constexpr (kSingleWrapper) {
                TickPlayEvents();
                return;
            }
            for (int s = 0; s < 2; ++s) {
                if (!g_wantRefresh[s] || g_idleSince[s] < 0) continue;
                const long long idleFor = now - g_idleSince[s];
                if (idleFor < settle) {
                    if (!g_settleLogged[s]) {
                        spdlog::info("[slots] slot {} settling ({} s left; idle {} ms of {} ms = fadeTime + 1 s)", kSlotName[s],
                                     (settle - idleFor + 999) / 1000, idleFor, settle);
                        g_settleLogged[s] = true;
                    }
                    continue;
                }
                g_wantRefresh[s]  = false;
                g_settleLogged[s] = false;
                Slots::RequestRefresh(s);
            }
        }

    }  // namespace

    int SlotOf(const RE::BSIMusicTrack* a_track) {
        if (!a_track) return -1;
        if (a_track == g_iface[0]) return 0;
        if (a_track == g_iface[1]) return 1;
        return -1;
    }

    const RE::BSIMusicType* Target() { return g_target; }

    void OnDataLoaded() {
        spdlog::info("[retarget] ===== M0b retarget spike: {} -> {} 0x{:03X}/0x{:03X} =====", kTargetEdid, kPluginName, kLocalId[0],
                     kLocalId[1]);

        g_targetForm = RE::TESForm::LookupByID<RE::BGSMusicType>(kTargetId);
        if (!g_targetForm) {
            spdlog::error("[retarget] DISABLED: music type {:08X} ({}) not found", kTargetId, kTargetEdid);
            return;
        }
        const char* edid = g_targetForm->GetFormEditorID();
        if (!edid || kTargetEdid != edid) {
            spdlog::error("[retarget] DISABLED: {:08X} has editor id \"{}\", expected \"{}\"", kTargetId, edid ? edid : "", kTargetEdid);
            g_targetForm = nullptr;
            return;
        }
        g_target = static_cast<RE::BSIMusicType*>(g_targetForm);
        g_lastSize = g_target->tracks.size();
        spdlog::info("[retarget] kDataLoaded: {} tracks={} typeStatus={} (BSIMusicType* {:p})", kTargetEdid, g_lastSize,
                     StatusName(g_target->typeStatus.underlying()), static_cast<const void*>(g_target));

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !dh->LookupModByName(kPluginName)) {
            spdlog::error("[retarget] DISABLED: {} is not in the load order", kPluginName);
            return;
        }
        for (int s = 0; s < 2; ++s) {
            auto* w = dh->LookupForm<RE::BGSMusicTrackFormWrapper>(kLocalId[s], kPluginName);
            if (!w || !w->track || w->track->GetType() != RE::BSIMusicTrack::TrackType::kSingleTrack) {
                spdlog::error("[retarget] DISABLED: wrapper {} (local 0x{:03X}) {}", kSlotName[s], kLocalId[s],
                              !w ? "not found" : (!w->track ? "has no inner track" : "inner track is not Single"));
                return;
            }
            g_wrapper[s] = w;
            g_iface[s]   = static_cast<RE::BSIMusicTrack*>(w);
            spdlog::info("[retarget] slot {}: wrapper {:08X} {:p} iface {:p} (offset {}) inner {:p}", kSlotName[s], w->GetFormID(),
                         static_cast<const void*>(w), static_cast<const void*>(g_iface[s]),
                         reinterpret_cast<const char*>(g_iface[s]) - reinterpret_cast<const char*>(w),
                         static_cast<const void*>(w->track));
        }

        if (Slots::KillSwitchPresentNow()) {
            g_killed = true;
            spdlog::warn("[retarget] KILL SWITCH present at load: no retarget this session");
        } else {
            spdlog::info("[retarget] kill switch absent at load");
        }
        g_ready = true;
        Slots::Start();
        spdlog::info("[retarget] ready; writes happen only in the watcher task while the type is not current, not queued, "
                     "and Inactive/Finished");
    }

    void OnMessage(std::uint32_t a_type) {
        if (!g_target) {
            spdlog::info("[retarget] {}: {} not resolved (yet)", MessageName(a_type), kTargetEdid);
            return;
        }
        spdlog::info("[retarget] {}: {} tracks={} typeStatus={} replaced-originals={} t=+{}ms", MessageName(a_type), kTargetEdid,
                     g_target->tracks.size(), StatusName(g_target->typeStatus.underlying()), g_originals.size(), Probe::NowMs());
    }

    void Tick() {
        if (!g_ready) return;

        const std::uint32_t n = g_target->tracks.size();
        if (g_firstTick) {
            g_firstTick = false;
            spdlog::info("[retarget] first watcher tick: {} tracks={} t=+{}ms", kTargetEdid, n, Probe::NowMs());
        } else if (n != g_lastSize) {
            spdlog::info("[retarget] {} tracks.size changed {} -> {} t=+{}ms", kTargetEdid, g_lastSize, n, Probe::NowMs());
        }
        g_lastSize = n;

        TickSlots();

        if (n == 0) {
            if (!g_loggedEmpty) spdlog::error("[retarget] {} has NO tracks: nothing to retarget (documented degrade)", kTargetEdid);
            g_loggedEmpty = true;
            return;
        }

        // Kill switch seen by the worker's poll after load: latch, then restore once safe.
        if (!g_killed && Slots::KillSwitchSeen()) {
            g_killed = true;
            spdlog::warn("[retarget] KILL SWITCH appeared: retarget stopped; {} replaced entries will be restored when safe",
                         g_originals.size());
        }

        std::string why;
        if (g_killed) {
            if (g_originals.empty()) return;
            if (!IsSafe(why)) return;
            Restore();
            return;
        }

        std::uint32_t needed = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (Classify(g_target->tracks[i]) == Kind::kReplaceable) ++needed;
        }
        if (needed == 0) return;

        if (!IsSafe(why)) {
            if (!g_deferLogged) spdlog::info("[retarget] {} entries to replace, deferred: {} t=+{}ms", needed, why, Probe::NowMs());
            g_deferLogged = true;
            return;
        }
        g_deferLogged = false;

        // Immediately before the stores, in the same task: kill switch, then the safe
        // condition again (nothing above could have changed it, but it is the contract).
        if (Slots::KillSwitchSeen()) return;  // latched and restored on the next tick
        if (!IsSafe(why)) return;
        Apply();
    }

}  // namespace MJ::Retarget
