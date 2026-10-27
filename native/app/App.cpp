// AppApi.h implementation (the seam to the menu) + the plugin-side host (AppHost.h).
// docs/INTEGRATION-1.0.md A.4/A.5. Every AppApi function is non-blocking: work runs on the pool/slot threads
// (pool/) or on this file's UI thread (connection test, search, peek, now-playing reports, INI saves), each
// with its own Harmony client. Nothing here touches engine memory.
#include "PCH.h"
#include "app/AppApi.h"
#include "app/AppHost.h"

#include "categories/Environments.h"
#include "config/ProfileBuilder.h"
#include "net/HarmonyClient.h"
#include "pool/Pool.h"
#include "probe/Probe.h"
#include "retarget/Retarget.h"
#include "slots/SlotWriter.h"

#include <cctype>
#include <deque>
#include <map>
#include <set>

namespace MJ::App {

    namespace {

        std::unique_ptr<mj::config::SettingsStore> g_store;

        // ---- UI thread state (guarded by g_mx) ----
        struct SearchReq {
            std::string kind, text;
            bool        streaming = false;
        };
        struct PlayedReq {
            std::string env, service, id;
        };

        std::mutex                  g_mx;
        std::condition_variable_any g_cv;
        bool                        g_testWanted{ false };
        bool                        g_testRunning{ false };
        bool                        g_haveTest{ false };   // a test result newer than the last settings change
        Connection                  g_test;
        std::optional<SearchReq>    g_searchWanted;
        std::stop_source            g_searchStop;
        SearchState                 g_search;
        std::set<std::string>       g_peekWanted;
        std::map<std::string, std::vector<std::string>> g_peek;
        std::deque<PlayedReq>       g_played;
        bool                        g_saveWanted{ false };

        std::shared_ptr<const mj::config::Settings> Snap() { return g_store->Snapshot(); }

        std::string Lower(std::string_view s) {
            std::string o(s);
            for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return o;
        }

        bool ConfigureClient(Net::HarmonyClient& a_cl, const mj::config::Settings& a_s, std::string& a_err) {
            Net::Config c;
            c.baseUrl     = a_s.global.serverUrl;
            c.personalKey = a_s.global.personalKey;
            const auto v  = SKSE::PluginDeclaration::GetSingleton()->GetVersion();
            c.userAgent   = std::format("MarthJukebox/{}.{}.{}", v.major(), v.minor(), v.patch());
            c.waitS       = 5;  // the UI thread never downloads audio
            const auto r  = a_cl.Configure(c);
            if (!r.Ok()) a_err = "The server address is not valid.";
            return r.Ok();
        }

        std::string PlainError(const Net::Result& r) {
            switch (r.error) {
            case Net::Error::kBadKey:      return "Harmony refused the personal key.";
            case Net::Error::kDisabled:    return "The game source is switched off on the Harmony server.";
            case Net::Error::kUnreachable: return "Harmony is not reachable. Is the server running and the address right?";
            case Net::Error::kTimeout:     return "Harmony did not answer in time.";
            case Net::Error::kRedirect:    return "The server address redirects elsewhere; enter the final address.";
            case Net::Error::kBadConfig:   return "The server address is not valid.";
            case Net::Error::kCancelled:   return "Cancelled.";
            case Net::Error::kNotFound:    return "This Harmony does not offer that list.";
            default:                       return std::string("Harmony answered with an error (") + Net::ErrorName(r.error) + ").";
            }
        }

        // Search hits the menu may store as an Artist/Playlist selector. Never Qobuz (review B3): Settings refuses
        // Service=qobuz, so Qobuz only ever enters through `sources` (AllowQobuz=1) for genre/query selectors.
        bool ServiceAllowed(const std::string& a_svc, bool a_streaming, bool) {
            if (a_svc == "local") return true;
            if (a_svc == "ytmusic") return a_streaming;
            return false;
        }

        // ---- UI thread jobs ----

        void DoTest(Net::HarmonyClient& a_cl) {
            const auto snap = Snap();
            Connection c;
            std::string err;
            if (snap->global.serverUrl.empty()) {
                c.state   = ConnState::NotConfigured;
                c.message = "Enter your Harmony server address first.";
            } else if (!snap->global.features.network) {
                c.state   = ConnState::Failed;
                c.message = "Network is switched off in the settings ([Features] Network=0).";
            } else if (!ConfigureClient(a_cl, *snap, err)) {
                c.state   = ConnState::Failed;
                c.message = err;
            } else {
                const auto st = a_cl.Status();
                if (!st.Ok()) {
                    c.state   = ConnState::Failed;
                    c.message = PlainError(st);
                } else if (!st.enabled) {
                    c.state   = ConnState::Failed;
                    c.message = "Connected, but the game source is switched off on the Harmony server.";
                } else if (!st.ffmpeg) {
                    c.state   = ConnState::Failed;
                    c.message = "Connected, but Harmony has no ffmpeg, so it cannot prepare audio.";
                } else {
                    c.state         = ConnState::Ok;
                    c.supportsFlac  = std::find(st.containers.begin(), st.containers.end(), "flac") != st.containers.end();
                    c.supportsPools = st.servicesReported;
                    c.youtubeUsable = st.servicesReported && st.ytmusicUsable;
                    std::string srcs;
                    auto        add = [&](const char* n) { srcs += srcs.empty() ? n : std::string(", ") + n; };
                    if (st.libraryUsable) add("your library");
                    if (c.youtubeUsable) add("YouTube Music");
                    if (st.qobuzUsable && snap->global.allowQobuz) add("Qobuz");
                    if (!st.servicesReported)
                        c.message = snap->global.allowQobuz
                                        ? "Connected. This Harmony is older than 1.0.13: streaming environments use all its sources (Qobuz included, AllowQobuz=1)."
                                        : "Connected. This Harmony is older than 1.0.13: library only, update it for streaming.";
                    else if (srcs.empty())
                        c.message = "Connected, but no music source is usable on the server.";
                    else
                        c.message = "Connected. Sources ready: " + srcs + ".";
                }
            }
            spdlog::info("[app] connection test: {}", c.message);
            std::scoped_lock lk(g_mx);
            g_test        = c;
            g_haveTest    = true;
            g_testRunning = false;
        }

        void AddHits(std::vector<SearchHit>& a_out, const std::vector<Net::SearchItem>& a_items, const char* a_kind, bool a_streaming,
                     bool a_qobuz, const std::string& a_filter) {
            for (const auto& it : a_items) {
                if (a_out.size() >= 200) return;
                if (!ServiceAllowed(it.service, a_streaming, a_qobuz)) continue;
                if (!a_filter.empty() && Lower(it.title).find(a_filter) == std::string::npos) continue;
                SearchHit h;
                h.kind    = a_kind;
                h.service = it.service;
                h.id      = it.id;
                h.title   = it.title;
                if (!it.artist.empty()) h.subtitle = it.artist;
                else if (!it.owner.empty()) h.subtitle = it.owner;
                if (it.trackCount > 0)
                    h.subtitle += (h.subtitle.empty() ? "" : ", ") + std::to_string(it.trackCount) + " tracks";
                a_out.push_back(std::move(h));
            }
        }

        void DoSearch(Net::HarmonyClient& a_cl, const SearchReq& a_req, std::stop_token a_st) {
            const auto             snap = Snap();
            std::vector<SearchHit> hits;
            std::string            error, err;
            const bool             qobuz = snap->global.allowQobuz;
            const std::string      text  = a_req.text;
            if (!mj::config::NetworkActive(*snap)) {
                error = "Set up the Harmony server first.";
            } else if (!ConfigureClient(a_cl, *snap, err)) {
                error = err;
            } else if (a_req.kind == "genre") {
                // Genres always come from the LIVE library (never assumed names); empty text = the whole list.
                const auto r = a_cl.LibraryView("genres", 5000, a_st);
                if (!r.Ok()) error = PlainError(r);
                else if (!r.enabled) error = "This Harmony has no music library, so there are no genres to pick.";
                else AddHits(hits, r.items, "genre", false, false, Lower(text));
            } else if (a_req.kind == "artist") {
                if (text.empty()) {
                    const auto r = a_cl.LibraryView("artists", 2000, a_st);
                    if (!r.Ok()) error = PlainError(r);
                    else AddHits(hits, r.items, "artist", false, false, {});
                } else {
                    const auto r = a_cl.Search(text, "artists", 50, a_st);
                    if (!r.Ok()) error = PlainError(r);
                    else AddHits(hits, r.artists, "artist", a_req.streaming, qobuz, {});
                }
            } else if (a_req.kind == "playlist") {
                if (text.empty()) {
                    const auto r = a_cl.Playlists(a_st);
                    if (!r.Ok()) error = PlainError(r);
                    else AddHits(hits, r.items, "playlist", a_req.streaming, qobuz, {});
                } else {
                    const auto r = a_cl.Search(text, "playlists", 50, a_st);
                    if (!r.Ok()) error = PlainError(r);
                    else AddHits(hits, r.playlists, "playlist", a_req.streaming, qobuz, {});
                }
                if (error.empty() && hits.empty() && !a_req.streaming)
                    error = "Your library has no playlists; allow streaming to pick a YouTube Music playlist.";
            } else if (a_req.kind == "query") {
                if (!text.empty()) {
                    const auto r = a_cl.Search(text, "tracks", 50, a_st);
                    if (!r.Ok()) error = PlainError(r);
                    else AddHits(hits, r.tracks, "track", a_req.streaming, qobuz, {});
                }
            } else {
                error = "Unknown search kind.";
            }
            if (a_st.stop_requested()) return;  // superseded: the newer search publishes
            std::scoped_lock lk(g_mx);
            g_search.busy  = g_searchWanted.has_value();
            g_search.error = error;
            g_search.hits  = std::move(hits);
            ++g_search.seq;
        }

        void DoPeek(Net::HarmonyClient& a_cl, const std::string& a_env) {
            const auto               snap = Snap();
            std::vector<std::string> lines;
            std::string              err;
            const auto               plan = Pool::CurrentPlan();
            const Pool::EnvPlan*     ep   = nullptr;
            if (plan) {
                if (const auto it = plan->envs.find(a_env); it != plan->envs.end()) ep = &it->second;
            }
            if (!mj::config::NetworkActive(*snap)) {
                lines.push_back("(No preview: Harmony is not set up.)");
            } else if (!ep) {
                lines.push_back("(No preview: this environment is not active yet. Apply the settings first.)");
            } else if (!ConfigureClient(a_cl, *snap, err)) {
                lines.push_back("(No preview: " + err + ")");
            } else {
                const auto r = a_cl.Next(snap->global.harmonyProfileName, a_env, ep->sources, 5, true, "");
                if (r.error == Net::Error::kNoTracks) lines.push_back("(No matching tracks for this selection.)");
                else if (!r.Ok()) lines.push_back("(No preview: " + PlainError(r) + ")");
                for (const auto& t : r.tracks) lines.push_back(t.artist.empty() ? t.title : t.artist + " - " + t.title);
            }
            std::scoped_lock lk(g_mx);
            g_peek[a_env] = std::move(lines);
        }

        void UiThread(std::stop_token a_st) {
            Net::HarmonyClient cl;
            spdlog::info("[app] UI worker started");
            while (!a_st.stop_requested()) {
                bool                     test = false, save = false;
                std::optional<SearchReq> search;
                std::stop_token          searchStop;
                std::string              peek;
                std::optional<PlayedReq> played;
                {
                    std::unique_lock lk(g_mx);
                    (void)g_cv.wait(lk, a_st, [] {
                        return g_testWanted || g_saveWanted || g_searchWanted || !g_peekWanted.empty() || !g_played.empty();
                    });
                    if (a_st.stop_requested()) break;
                    if (g_saveWanted) {
                        save         = true;
                        g_saveWanted = false;
                    } else if (g_testWanted) {
                        test         = true;
                        g_testWanted = false;
                    } else if (g_searchWanted) {
                        search = std::move(g_searchWanted);
                        g_searchWanted.reset();
                        searchStop = g_searchStop.get_token();
                    } else if (!g_played.empty()) {
                        played = std::move(g_played.front());
                        g_played.pop_front();
                    } else if (!g_peekWanted.empty()) {
                        peek = *g_peekWanted.begin();
                        g_peekWanted.erase(g_peekWanted.begin());
                    }
                }
                if (save) {
                    std::string err;
                    if (!g_store->Save(&err)) spdlog::error("[app] saving the settings failed: {}", err);
                } else if (test) {
                    DoTest(cl);
                } else if (search) {
                    DoSearch(cl, *search, searchStop);
                } else if (played) {
                    const auto snap = Snap();
                    std::string err;
                    if (mj::config::NetworkActive(*snap) && ConfigureClient(cl, *snap, err)) {
                        const auto r = cl.ReportPlaying(snap->global.harmonyProfileName, played->env, played->service, played->id);
                        if (!r.Ok()) spdlog::debug("[app] now-playing report failed (best effort): {}", Net::ErrorName(r.error));
                    }
                } else if (!peek.empty()) {
                    DoPeek(cl, peek);
                }
            }
            spdlog::info("[app] UI worker stopped");
        }

        struct UiWorker {
            std::jthread th;
            ~UiWorker() {
                th.request_stop();
                if (th.joinable()) th.join();
            }
        } g_ui;

        void Wake() { g_cv.notify_all(); }

        std::string Label(const std::string& a_id) {
            std::string g, sub;
            if (!mj::categories::SplitEnvironmentId(a_id, g, sub)) return a_id;
            const auto* gi = mj::categories::FindGroup(g);
            if (!gi) return a_id;
            std::string l(gi->displayName);
            if (!sub.empty()) {
                std::string sname = sub;
                for (const auto& si : gi->subs) {
                    if (si.id == sub) sname = std::string(si.displayName);
                }
                l += " (" + sname + ")";
            }
            return l;
        }

    }  // namespace

    // ================================================================ host (AppHost.h)

    void Init() {
        if (g_store) return;
        g_store = std::make_unique<mj::config::SettingsStore>(Slots::PluginDir() / L"MarthJukebox.ini",
                                                              Slots::PluginDir() / L"MarthJukebox" / L"cache");
    }

    mj::config::SettingsStore* Store() { return g_store.get(); }

    void OnDataLoaded() {
        Init();
        Probe::OnDataLoaded();     // identity maps + watcher (the watcher runs Retarget::Tick)
        Retarget::OnDataLoaded();  // resolves the 24 slot wrappers; no writes
        Pool::SetPlayedCallback([](const std::string& a_env, const std::string& a_svc, const std::string& a_id) {
            {
                std::scoped_lock lk(g_mx);
                if (g_played.size() < 16) g_played.push_back({ a_env, a_svc, a_id });
            }
            Wake();
        });
        Pool::Start(g_store.get());
        if (!g_ui.th.joinable()) g_ui.th = std::jthread(UiThread);
    }

    void OnGameLoaded(const char* a_why) { Retarget::LogState(a_why); }

    void TickGameThread() {
        // Toasts (pool/ToastPolicy.h) are shown here only: this runs inside the watcher's SKSE task on the game thread.
        // Not over the main menu or a loading screen: the message waits in the queue until they close.
        auto* ui = RE::UI::GetSingleton();
        if (!ui || ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;
        if (const auto t = Pool::PopToast()) RE::DebugNotification(t->c_str());
    }

    // ================================================================ AppApi.h

    void TestConnectionAsync() {
        {
            std::scoped_lock lk(g_mx);
            g_testWanted  = true;
            g_testRunning = true;
        }
        Wake();
    }

    Connection GetConnection() {
        const auto snap = Snap();
        {
            std::scoped_lock lk(g_mx);
            if (g_testRunning) {
                Connection c = g_test;
                c.state      = ConnState::Testing;
                c.message    = "Testing the connection...";
                return c;
            }
            if (g_haveTest) return g_test;
        }
        Connection c;
        const auto p = Pool::Connection();
        if (snap->global.serverUrl.empty()) {
            c.state   = ConnState::NotConfigured;
            c.message = "Not set up yet: enter your Harmony server address and key.";
            return c;
        }
        c.state         = !p.tested ? ConnState::Testing : (p.ok ? ConnState::Ok : ConnState::Failed);
        c.message       = p.tested ? p.message : "Connecting...";
        c.supportsFlac  = p.flac;
        c.supportsPools = p.servicesReported;
        c.youtubeUsable = p.servicesReported && p.ytmusic;
        return c;
    }

    void SearchAsync(const std::string& a_kind, const std::string& a_text, bool a_includeStreaming) {
        {
            std::scoped_lock lk(g_mx);
            g_searchStop.request_stop();  // cancel the one in flight (signal only)
            g_searchStop   = std::stop_source{};
            g_searchWanted = SearchReq{ a_kind, a_text.substr(0, 200), a_includeStreaming };
            g_search.busy  = true;
            ++g_search.seq;
        }
        Wake();
    }

    SearchState GetSearch() {
        std::scoped_lock lk(g_mx);
        return g_search;
    }

    std::vector<EnvStatus> GetEnvStatus() {
        const auto snap    = Snap();
        const auto reports = Pool::Report();
        const auto plan    = Pool::CurrentPlan();
        const auto views   = Pool::SlotViews();
        const auto conn    = Pool::Connection();
        const auto build   = mj::config::BuildProfile(*snap);
        std::map<std::string, Pool::EnvReport> byId;
        for (const auto& r : reports) byId[r.id] = r;
        std::set<std::string> ids;
        for (const auto& [id, e] : snap->environments) ids.insert(id);
        for (const auto& r : reports) ids.insert(r.id);

        std::vector<EnvStatus> out;
        for (const auto& id : ids) {
            EnvStatus st;
            st.id    = id;
            st.label = Label(id);
            const auto* e = snap->Find(id);
            st.target     = e ? e->poolSize : 3;
            const auto rit = byId.find(id);
            const Pool::EnvReport* r = rit == byId.end() ? nullptr : &rit->second;
            if (r) {
                st.ready      = r->ready;
                st.nowPlaying = r->nowPlaying;
                st.poolTitles = r->titles;
            }
            const bool vanilla = !e || e->mode == mj::config::Mode::Vanilla;
            st.active          = !vanilla && build.publishedTypes.contains(id) && r && r->planned;
            std::string problem;
            for (const auto& p : build.problems) {
                if (p.environment == id && p.severity == mj::config::ProblemSeverity::Error) problem = p.message;
            }
            if (vanilla) {
                st.state = "Vanilla";
            } else if (!problem.empty()) {
                st.state = "Vanilla (" + problem + ")";
            } else if (!mj::config::NetworkActive(*snap)) {
                st.state = snap->global.serverUrl.empty() ? "Vanilla (Harmony not set up)" : "Vanilla (network switched off)";
            } else if (plan && !plan->engineWrites) {
                st.state = "Vanilla (" + plan->why + ")";
            } else if (!r || r->slot < 0) {
                st.state = plan && plan->envs.contains(id) ? "Vanilla (all 24 slots are in use)" : "Vanilla (no game music uses this environment)";
            } else {
                const auto& v       = views[r->slot];
                const bool  playing = v.owner == id && v.fresh;
                if (playing) {
                    st.state = r->ready < st.target ? std::format("Playing from Harmony (refilling, {} of {} ready)", r->ready, st.target)
                                                    : "Playing from Harmony";
                } else if (!r->problem.empty()) {
                    st.state = "Vanilla (" + r->problem + ")";
                } else if (conn.tested && !conn.ok) {
                    st.state = "Vanilla (Harmony unreachable)";
                } else {
                    st.state = std::format("Filling pool ({} of {})", r->ready, st.target);
                }
                if (playing && !r->problem.empty()) st.state += " - " + r->problem;
            }
            out.push_back(std::move(st));
        }
        return out;
    }

    void ApplySettingsChanged() {
        {
            std::scoped_lock lk(g_mx);
            g_haveTest = false;  // the next GetConnection reflects the new settings
            g_peek.clear();
        }
        Pool::SettingsChanged();
    }

    void ResetEnvironmentToVanilla(const std::string& a_envId) {
        g_store->Update([&](mj::config::Settings& s) {
            if (auto it = s.environments.find(a_envId); it != s.environments.end()) it->second.mode = mj::config::Mode::Vanilla;
        });
        {
            std::scoped_lock lk(g_mx);
            g_saveWanted = true;
        }
        Wake();
        ApplySettingsChanged();
    }

    void ClearCacheAsync() { Pool::ClearCache(); }

    std::uint64_t CacheBytes() { return Pool::CacheBytes(); }

    void PeekAsync(const std::string& a_envId) {
        {
            std::scoped_lock lk(g_mx);
            g_peekWanted.insert(a_envId);
            g_peek[a_envId] = { "(Loading...)" };
        }
        Wake();
    }

    std::vector<std::string> GetPeek(const std::string& a_envId) {
        std::scoped_lock lk(g_mx);
        const auto       it = g_peek.find(a_envId);
        return it == g_peek.end() ? std::vector<std::string>{} : it->second;
    }

}  // namespace MJ::App
