// Read-only music probe. See Probe.h. Nothing here writes engine memory, takes an engine lock, or installs a
// hook. The enumeration and the music-state lines are at debug level ("[probe]"); the totals are at info.
// The watcher task also runs Retarget::Tick (the only engine writer, in its own file) after the probe's
// read-only Tick. The stall detector logs main-thread gaps > 400 ms at debug (the game's ~5 min autosaves,
// M0c) and gaps > 5 s at warn.
#include "PCH.h"
#include "probe/Probe.h"
#include "retarget/Retarget.h"
#include "app/AppHost.h"
#include "pool/Pool.h"

namespace MJ::Probe {

    namespace {


        // ms since the DLL was loaded (static init runs at LoadLibrary). NowMs() is public (Probe.h).
        const auto g_t0 = std::chrono::steady_clock::now();

        // Identity maps built once at kDataLoaded on the main thread and read only on the
        // main thread afterwards (Tick runs as an SKSE task), so no locking is needed.
        // The key is the address of the BSIMusicType / BSIMusicTrack SUBOBJECT, i.e. what
        // BSMusicManager::current and BSIMusicType::tracks[] actually hold; the compiler
        // computes it with static_cast, so no RTTI is involved in resolving a pointer.
        std::unordered_map<const RE::BSIMusicType*, RE::BGSMusicType*>             g_typeByIface;
        std::unordered_map<const RE::BSIMusicTrack*, RE::BGSMusicTrackFormWrapper*> g_trackByIface;

        const char* TrackTypeName(std::uint32_t a_v) {
            switch (static_cast<RE::BSIMusicTrack::TrackType>(a_v)) {
            case RE::BSIMusicTrack::TrackType::kSilentTrack: return "Silent";
            case RE::BSIMusicTrack::TrackType::kSingleTrack: return "Single";
            case RE::BSIMusicTrack::TrackType::kPalette:     return "Palette";
            default:                                         return "UNKNOWN";
            }
        }

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

        std::string MstFlags(std::uint32_t a_f) {
            static constexpr std::pair<std::uint32_t, const char*> names[] = {
                { 1u << 0, "PlaysOnce" },      { 1u << 1, "AbruptTransition" }, { 1u << 2, "CycleTracks" },
                { 1u << 3, "UseTrackOrder" },  { 1u << 4, "RemovalQueued" },    { 1u << 5, "PlaysOver" },
                { 1u << 6, "DoesntQueue" },
            };
            std::string s;
            for (const auto& [bit, n] : names) {
                if (a_f & bit) {
                    if (!s.empty()) s += '|';
                    s += n;
                }
            }
            return s.empty() ? "None" : s;
        }

        std::string ExtStr(const char (&a_ext)[4]) {
            std::string s;
            for (char c : a_ext) s += (c >= 0x20 && c < 0x7F) ? c : '.';
            return s;
        }

        std::string IdStr(const RE::BSResource::ID& a_id) {
            std::uint32_t extRaw = 0;
            std::memcpy(&extRaw, a_id.ext, 4);
            return std::format("file=0x{:08X} ext=0x{:08X}(\"{}\") dir=0x{:08X}", a_id.file, extRaw, ExtStr(a_id.ext), a_id.dir);
        }

        std::string PluginOf(const RE::TESForm* a_form) {
            if (!a_form) return "?";
            const auto* f = a_form->GetFile(0);
            return f ? std::string(f->GetFilename()) : std::string("?");
        }

        const char* EdidOf(const RE::TESForm* a_form) {
            if (!a_form) return "";
            const char* e = a_form->GetFormEditorID();
            return e ? e : "";
        }

        // Logs the single-track payload of an inner BGSMusicTrack. Returns true if it was a
        // BGSMusicSingleTrack. Safe cast: GetType() is the class-identity CRC, and
        // BGSMusicSingleTrack derives from BGSMusicTrack as its PRIMARY base (single
        // inheritance chain BSIMusicTrack -> BGSMusicTrack -> BGSMusicSingleTrack), so the
        // static_cast is a no-op pointer adjustment.
        void LogInnerTrack(const char* a_indent, RE::BGSMusicTrack* a_inner, bool& a_isSingle) {
            a_isSingle = false;
            if (!a_inner) {
                spdlog::debug("[probe] {}inner track = null", a_indent);
                return;
            }
            const auto t = static_cast<std::uint32_t>(a_inner->GetType());
            spdlog::debug("[probe] {}inner track {:p} type=0x{:08X}({}) status={}", a_indent,
                         static_cast<const void*>(a_inner), t, TrackTypeName(t), StatusName(a_inner->trackStatus.underlying()));
            if (a_inner->GetType() == RE::BSIMusicTrack::TrackType::kSingleTrack) {
                a_isSingle    = true;
                const auto* s = static_cast<RE::BGSMusicSingleTrack*>(a_inner);
                spdlog::debug("[probe] {}  trackID  {}", a_indent, IdStr(s->trackID));
                spdlog::debug("[probe] {}  finaleID {}", a_indent, IdStr(s->finaleID));
                spdlog::debug("[probe] {}  cuePoints={} loopData={}", a_indent, s->cuePoints.size(),
                             s->loopData ? std::format("present(begin={} end={} count={})", s->loopData->loopBegin,
                                                       s->loopData->loopEnd, s->loopData->loopCount)
                                         : std::string("none"));
            }
        }

        struct Totals {
            std::uint32_t types{ 0 }, tracks{ 0 }, wrappers{ 0 }, nonWrapper{ 0 };
            std::uint32_t single{ 0 }, palette{ 0 }, silent{ 0 }, other{ 0 }, nullInner{ 0 };
            std::uint32_t castMismatch{ 0 };
        };

        void EnumerateTypes(RE::TESDataHandler* a_dh, Totals& a_t) {
            auto& types = a_dh->GetFormArray<RE::BGSMusicType>();
            for (std::uint32_t ti = 0; ti < types.size(); ++ti) {
                RE::BGSMusicType* mt = types[ti];
                if (!mt) continue;
                ++a_t.types;
                // static_cast to the secondary base: the compiler adds the +0x20 offset.
                RE::BSIMusicType* iface = static_cast<RE::BSIMusicType*>(mt);
                g_typeByIface[iface]    = mt;

                spdlog::debug("[probe] MUSC {:08X} [{}] edid=\"{}\" flags=0x{:X}({}) priority={} fadeTime={:.3f} ducksOtherMusicBy={} "
                             "currentTrackIndex={} typeStatus={} tracks={} (BSIMusicType* {:p})",
                             mt->GetFormID(), PluginOf(mt), EdidOf(mt), iface->flags.underlying(), MstFlags(iface->flags.underlying()),
                             iface->priority, iface->fadeTime, iface->ducksOtherMusicBy, iface->currentTrackIndex,
                             StatusName(iface->typeStatus.underlying()), iface->tracks.size(), static_cast<const void*>(iface));

                for (std::uint32_t i = 0; i < iface->tracks.size(); ++i) {
                    RE::BSIMusicTrack* tr = iface->tracks[i];
                    ++a_t.tracks;
                    if (!tr) {
                        spdlog::debug("[probe]   track[{}] = null", i);
                        continue;
                    }
                    // BSIMusicTrack* -> wrapper. Resolved by identity (primary), cross-checked
                    // with skyrim_cast (RTDynamicCast) so a bad cast would show up as a mismatch.
                    // The wrapper's own BSIMusicTrack vfuncs forward through w->track with no null
                    // check, and the engine never updates the wrapper's own trackStatus, so
                    // nothing virtual is called on `tr`; type and status are read from w->track.
                    RE::BGSMusicTrackFormWrapper* w = nullptr;
                    if (const auto it = g_trackByIface.find(tr); it != g_trackByIface.end()) w = it->second;
                    auto* viaCast = skyrim_cast<RE::BGSMusicTrackFormWrapper*>(tr);
                    if (viaCast != w) {
                        ++a_t.castMismatch;
                        spdlog::warn("[probe]   track[{}] CAST MISMATCH identity={:p} skyrim_cast={:p}", i,
                                     static_cast<const void*>(w), static_cast<const void*>(viaCast));
                    }
                    if (!w) {
                        ++a_t.nonWrapper;
                        spdlog::debug("[probe]   track[{}] {:p} is NOT a known MUST wrapper", i, static_cast<const void*>(tr));
                        continue;
                    }
                    ++a_t.wrappers;
                    spdlog::debug("[probe]   track[{}] wrapper {:08X} [{}] edid=\"{}\" (wrapper {:p}, iface offset {}) wrapperStatus={}", i,
                                 w->GetFormID(), PluginOf(w), EdidOf(w), static_cast<const void*>(w),
                                 reinterpret_cast<const char*>(tr) - reinterpret_cast<const char*>(w),
                                 StatusName(tr->trackStatus.underlying()));
                    if (!w->track) {
                        ++a_t.nullInner;
                        spdlog::debug("[probe]   track[{}] wrapper has NO inner track; skipped", i);
                        continue;
                    }
                    bool single = false;
                    LogInnerTrack("    ", w->track, single);
                    switch (w->track->GetType()) {
                    case RE::BSIMusicTrack::TrackType::kSingleTrack: ++a_t.single; break;
                    case RE::BSIMusicTrack::TrackType::kPalette:     ++a_t.palette; break;
                    case RE::BSIMusicTrack::TrackType::kSilentTrack: ++a_t.silent; break;
                    default:                                         ++a_t.other; break;
                    }
                }
            }
        }


        // ---------------------------------------------------------------- watcher

        struct State {
            bool                       haveMgr{ false };
            const RE::BSIMusicType*    current{ nullptr };
            std::uint32_t              typeStatus{ 0 };
            std::uint32_t              index{ 0 };
            std::uint32_t              ntracks{ 0 };
            const RE::BSIMusicTrack*   track{ nullptr };
            std::uint32_t              trackStatus{ 0 };    // INNER track's status (wrapper's own is never updated)
            std::uint32_t              wrapperStatus{ 0 };  // wrapper's own field, logged to prove that claim
            std::vector<const RE::BSIMusicType*> queue;
            bool operator==(const State&) const = default;
        };

        State g_prev;
        bool  g_havePrev{ false };

        // "slot NN" for our slot wrappers, "vanilla(FORMID)" for any other MUST wrapper.
        std::string WrapperLabel(const RE::BSIMusicTrack* a_t) {
            if (!a_t) return "none";
            if (const int s = Retarget::SlotOf(a_t); s >= 0) return std::format("slot {:02}", s);
            if (const auto* w = WrapperOf(a_t)) return std::format("vanilla({:08X})", w->GetFormID());
            return std::format("<unknown {:p}>", static_cast<const void*>(a_t));
        }

        // On every change of (current type, trackIndex, track pointer): which wrapper plays.
        const RE::BSIMusicType*  g_npType{ nullptr };
        std::uint32_t            g_npIndex{ 0xFFFFFFFF };
        const RE::BSIMusicTrack* g_npTrack{ nullptr };

        void LogNowPlaying(const State& a_s) {
            if (a_s.current == g_npType && a_s.index == g_npIndex && a_s.track == g_npTrack) return;
            g_npType  = a_s.current;
            g_npIndex = a_s.index;
            g_npTrack = a_s.track;
            if (!a_s.track) return;
            std::string inner = "?";
            if (const auto* w = WrapperOf(a_s.track); w && w->track)
                inner = TrackTypeName(static_cast<std::uint32_t>(w->track->GetType()));
            spdlog::debug("[probe] now-playing t=+{}ms type={} trackIndex={}/{} wrapper={} inner={} typeStatus={}", NowMs(),
                         TypeName(a_s.current), a_s.index, a_s.ntracks, WrapperLabel(a_s.track), inner,
                         StatusName(a_s.typeStatus));
        }

        // Field #5 PerfProbe: the game thread's time inside OUR watcher task, reported once a minute at info, with the
        // game thread's waits for the pool lock (Pool::TakeLockWaitStats). If these stay small while [stall] lines show
        // 20 s gaps, the gaps are not ours.
        long long g_perfTicks{ 0 }, g_perfSumUs{ 0 }, g_perfMaxUs{ 0 }, g_perfSinceMs{ -1 };

        void PerfAccount(long long a_us) {
            ++g_perfTicks;
            g_perfSumUs += a_us;
            g_perfMaxUs = (std::max)(g_perfMaxUs, a_us);
            const long long now = NowMs();
            if (g_perfSinceMs < 0) g_perfSinceMs = now;
            if (now - g_perfSinceMs < 60'000) return;
            const auto w = MJ::Pool::TakeLockWaitStats();
            spdlog::info("[perf] game thread in our watcher task over {} s: {} ticks, avg {} us, max {} us; waits > 20 ms for the pool "
                         "lock: {}{}",
                         (now - g_perfSinceMs) / 1000, g_perfTicks, g_perfTicks ? g_perfSumUs / g_perfTicks : 0, g_perfMaxUs, w.count,
                         w.count ? std::format(" (longest {} ms, held by {})", w.maxUs / 1000, w.holder) : std::string());
            g_perfTicks = g_perfSumUs = g_perfMaxUs = 0;
            g_perfSinceMs = now;
        }

        bool LoadingOpen() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        }
        bool g_loadingLastTick{ false };

        // M0c stall detector: NowMs() of the previous watcher task (-1 before the first).
        long long g_prevTickMs{ -1 };

        // Runs on the game main thread (SKSE task). Read-only.
        void Tick() {
            const long long tickMs = NowMs();  // the stall detector's only per-tick cost (+ one subtraction below)
            State s;
            if (auto* mgr = RE::BSMusicManager::GetSingleton()) {
                s.haveMgr = true;
                s.current = mgr->current;
                for (std::uint32_t i = 0; i < mgr->musicQueue.size() && i < 8; ++i) s.queue.push_back(mgr->musicQueue[i]);
                // Read through `current` only if it is a music type we enumerated.
                if (s.current && g_typeByIface.contains(s.current)) {
                    s.typeStatus = s.current->typeStatus.underlying();
                    s.index      = s.current->currentTrackIndex;
                    s.ntracks    = s.current->tracks.size();
                    if (s.index < s.ntracks) {
                        s.track = s.current->tracks[s.index];
                        if (s.track) {
                            if (const auto it = g_trackByIface.find(s.track); it != g_trackByIface.end()) {
                                s.wrapperStatus = s.track->trackStatus.underlying();
                                if (it->second->track) s.trackStatus = it->second->track->trackStatus.underlying();
                            }
                        }
                    }
                }
            }
            // Gap between two consecutive watcher tasks (nominally 250 ms). Logged only when
            // > 400 ms: the main thread did not run SKSE tasks for that long.
            if (g_prevTickMs >= 0 && tickMs - g_prevTickMs > 400) {
                const bool big     = tickMs - g_prevTickMs > 5000;
                const bool loading = LoadingOpen();
                spdlog::log(big ? spdlog::level::warn : spdlog::level::debug,
                            "[stall] main-thread tick gap {} ms (previous tick at +{}ms, this at +{}ms) current={} idx={} status={}{}{}{}",
                            tickMs - g_prevTickMs, g_prevTickMs, tickMs, TypeName(s.current), s.index, StatusName(s.typeStatus),
                            big ? std::format(" | loading screen before/now: {}/{}", g_loadingLastTick ? "yes" : "no", loading ? "yes" : "no")
                                : std::string(),
                            big ? " | ours: " : "", big ? MJ::Pool::PerfNow() : std::string());
            }
            g_prevTickMs      = tickMs;
            g_loadingLastTick = LoadingOpen();
            LogNowPlaying(s);
            if (g_havePrev && s == g_prev) return;
            g_prev     = s;
            g_havePrev = true;

            std::string q;
            for (const auto* t : s.queue) q += (q.empty() ? "" : ",") + TypeName(t);
            std::string wrapper = "none";
            if (s.track) {
                if (const auto it = g_trackByIface.find(s.track); it != g_trackByIface.end())
                    wrapper = std::format("{:08X}", it->second->GetFormID());
                else
                    wrapper = std::format("<unknown {:p}>", static_cast<const void*>(s.track));
            }
            spdlog::debug("[probe] t=+{}ms music: mgr={} current={} typeStatus={} trackIndex={}/{} trackStatus={} wrapperStatus={} wrapper={} queue=[{}]",
                         NowMs(), s.haveMgr ? "yes" : "NULL", TypeName(s.current), StatusName(s.typeStatus), s.index, s.ntracks,
                         StatusName(s.trackStatus), StatusName(s.wrapperStatus), wrapper, q);
        }

        // One non-detached jthread, owned by a static guard whose destructor requests stop
        // and joins. jthread's stop_token wakes the condition_variable_any wait at once, so
        // the join is immediate. Caveat: static destructors run at DLL_PROCESS_DETACH; if
        // the process is killed (TerminateProcess) they never run, which is harmless for a
        // read-only thread. On ExitProcess the OS has already terminated the thread by then,
        // so join returns at once. Tasks already queued check g_alive before touching state.
        std::atomic<bool> g_alive{ false };
        std::atomic<bool> g_taskPending{ false };

        struct Watcher {
            std::jthread th;
            ~Watcher() {
                g_alive = false;
                th.request_stop();
                if (th.joinable()) th.join();
            }
        } g_watcher;

        void WatchLoop(std::stop_token a_st) {
            std::mutex                 mx;
            std::condition_variable_any cv;
            std::unique_lock           lk(mx);
            while (!a_st.stop_requested()) {
                // Returns true early only if stop was requested (predicate is the stop state).
                if (cv.wait_for(lk, a_st, 250ms, [&] { return a_st.stop_requested(); })) break;
                // Coalesce: never queue a second task while one is still waiting for the main
                // thread (loading screens stall it for seconds).
                if (g_taskPending.exchange(true)) continue;
                if (const auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([] {
                        if (g_alive) {
                            const auto t0 = std::chrono::steady_clock::now();
                            Tick();             // read-only probe
                            Retarget::Tick();   // the only engine-memory writer (guarded, see Retarget.cpp)
                            App::TickGameThread();  // queued "Harmony not reachable" notifications
                            PerfAccount(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
                        }
                        g_taskPending = false;
                    });
                } else {
                    g_taskPending = false;
                }
            }
        }

    }  // namespace

    long long NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_t0).count();
    }

    std::string TypeName(const RE::BSIMusicType* a_t) {
        if (!a_t) return "null";
        if (const auto it = g_typeByIface.find(a_t); it != g_typeByIface.end())
            return std::format("{}({:08X})", EdidOf(it->second), it->second->GetFormID());
        return std::format("<unknown BSIMusicType {:p}>", static_cast<const void*>(a_t));
    }

    void LogTrackHistory(const RE::BSIMusicType* a_type, const char* a_tag) {
        if (!a_type || !g_typeByIface.contains(a_type)) {
            spdlog::debug("[probe] trackHistory ({}): type {} not enumerated; not read", a_tag, TypeName(a_type));
            return;
        }
        const auto&         hist   = a_type->trackHistory;
        const auto&         tracks = a_type->tracks;
        const std::uint32_t hn     = hist.size();
        std::string         vals;
        for (std::uint32_t i = 0; i < hn && i < 3; ++i) vals += std::format("{}{}", vals.empty() ? "" : ",", hist[i]);
        std::string first = "n/a";
        if (hn > 0) {
            const std::uint32_t h0 = hist[0];
            if (h0 < tracks.size()) {
                const RE::BSIMusicTrack* t = tracks[h0];
                if (!t) {
                    first = std::format("tracks[{}]=null", h0);
                } else if (const auto* w = WrapperOf(t)) {
                    std::string inner = "?";
                    if (w->track) inner = TrackTypeName(static_cast<std::uint32_t>(w->track->GetType()));
                    const int slot = Retarget::SlotOf(t);
                    first = std::format("tracks[{}]={} ({:08X}, inner {})", h0, slot >= 0 ? std::format("slot {:02}", slot) : std::string("vanilla"),
                                        w->GetFormID(), inner);
                } else {
                    first = std::format("tracks[{}]=<unknown {:p}>", h0, static_cast<const void*>(t));
                }
            } else {
                first = std::format("trackHistory[0]={} OUT OF RANGE (tracks={})", h0, tracks.size());
            }
        }
        spdlog::debug("[probe] trackHistory ({}): type={} currentTrackIndex={} tracks={} history size={} first=[{}] head: {}", a_tag,
                     TypeName(a_type), a_type->currentTrackIndex, tracks.size(), hn, vals, first);
    }

    RE::BGSMusicTrackFormWrapper* WrapperOf(const RE::BSIMusicTrack* a_track) {
        if (!a_track) return nullptr;
        const auto it = g_trackByIface.find(a_track);
        return it != g_trackByIface.end() ? it->second : nullptr;
    }

    void OnDataLoaded() {
        spdlog::info("[probe] ===== music probe start: game {} =====", REL::Module::get().version().string());

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("[probe] TESDataHandler singleton is null; enumeration skipped");
        } else {
            // Wrapper identity map first (EnumerateTypes resolves tracks through it).
            auto& musts = dh->GetFormArray<RE::BGSMusicTrackFormWrapper>();
            for (std::uint32_t i = 0; i < musts.size(); ++i) {
                if (auto* w = musts[i]) g_trackByIface[static_cast<RE::BSIMusicTrack*>(w)] = w;
            }
            spdlog::info("[probe] MUST (BGSMusicTrackFormWrapper) forms in TESDataHandler: {}", musts.size());
            spdlog::info("[probe] MUSC (BGSMusicType) forms in TESDataHandler: {}", dh->GetFormArray<RE::BGSMusicType>().size());

            Totals t;
            EnumerateTypes(dh, t);
            spdlog::info("[probe] TOTALS: types={} trackSlots={} wrappers={} nonWrapper={} inner[single={} palette={} silent={} other={} null={}] "
                         "castMismatch={}",
                         t.types, t.tracks, t.wrappers, t.nonWrapper, t.single, t.palette, t.silent, t.other, t.nullInner, t.castMismatch);

        }

        if (!g_watcher.th.joinable()) {
            g_alive         = true;
            g_watcher.th    = std::jthread(WatchLoop);
            spdlog::info("[probe] watcher started (250 ms; probe read-only; runs the retarget tick after it)");
        }
        spdlog::info("[probe] ===== music probe enumeration done =====");
    }

}  // namespace MJ::Probe
