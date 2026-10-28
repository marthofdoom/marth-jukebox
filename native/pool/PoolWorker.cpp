// Pool thread + the Pool.h entry points. See Pool.h / Pool_internal.h. No engine memory is touched here:
// everything the game thread needs is published as an immutable Plan or a small copied snapshot.
#include "PCH.h"
#include "pool/Pool_internal.h"

#include "config/ProfileBuilder.h"
#include "net/HarmonyClient.h"
#include "pool/Audio.h"
#include "pool/ToastPolicy.h"
#include "slots/SlotWriter.h"

#include <fstream>
#include <iterator>
#include <sstream>

namespace MJ::Pool {

    namespace fs = std::filesystem;
    using namespace detail;

    namespace detail {
        Shared& S() {
            static Shared s;
            return s;
        }
        void MarkDirtyLocked() {
            S().indexDirty = true;
            S().poolWake   = true;
            S().poolCv.notify_all();
        }
    }

    namespace {

        constexpr long long kBackoffMinMs = 2'000;
        constexpr long long kBackoffMaxMs = 300'000;
        constexpr long long kRejectShortMs = 10 * 60'000ll;    // transient download failure: skip the pick a while
        constexpr long long kRejectLongMs  = 6 * 3600'000ll;   // the file itself is bad (preview, undecodable)

        void QueueToast(const std::optional<std::string>& a_t) {
            if (!a_t) return;
            auto&            s = S();
            std::scoped_lock lk(s.mx);
            if (s.toasts.size() < 4) s.toasts.push_back(*a_t);
            spdlog::info("[toast] {}", *a_t);
        }

        std::string ToUtf8(const fs::path& p) { return Slots::Utf8(p); }

        // ---------------------------------------------------------------- index persistence (pool thread)

        void LoadIndex() {
            auto&                    s    = S();
            const fs::path           path = s.cacheDir / mj::pool::kIndexFileName;
            mj::pool::PoolIndex      idx;
            std::vector<std::string> diag;
            std::error_code          ec;
            if (fs::exists(path, ec)) {
                std::ifstream     f(path, std::ios::binary);
                const std::string text{ std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
                (void)mj::pool::ParseIndex(text, idx, diag);
            } else {
                spdlog::info("[pool] no pool index yet in {}", ToUtf8(s.cacheDir));
            }
            const auto dropped = mj::pool::Revalidate(idx, s.cacheDir, diag);
            for (const auto& d : diag) spdlog::warn("[pool] {}", d);
            std::size_t entries = 0;
            for (const auto& [env, p] : idx.pools) entries += p.entries.size();
            spdlog::info("[pool] index loaded: {} pool(s), {} validated track(s), {} dropped at revalidation", idx.pools.size(), entries,
                         dropped);
            std::scoped_lock lk(s.mx);
            s.index      = std::move(idx);
            s.indexDirty = dropped > 0;
            s.cacheReady = true;
        }

        void SaveIndexIfDirty() {
            auto&       s = S();
            std::string text;
            fs::path    dir;
            {
                std::scoped_lock lk(s.mx);
                if (!s.indexDirty || !s.cacheReady) return;
                s.indexDirty = false;
                text         = mj::pool::SerializeIndex(s.index);
                dir          = s.cacheDir;
            }
            const fs::path dst = dir / mj::pool::kIndexFileName;
            fs::path       tmp = dst;
            tmp += ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f.write(text.data(), static_cast<std::streamsize>(text.size()));
                f.flush();
                if (!f) {
                    spdlog::error("[pool] writing {} failed; the index will be retried", ToUtf8(tmp));
                    std::scoped_lock lk(s.mx);
                    s.indexDirty = true;
                    return;
                }
            }
            std::error_code ec;
            fs::rename(tmp, dst, ec);  // MSVC: MoveFileExW(REPLACE_EXISTING)
            if (ec) {
                spdlog::error("[pool] replacing {} failed: {}", ToUtf8(dst), ec.message());
                std::scoped_lock lk(s.mx);
                s.indexDirty = true;
            }
        }

        // ---------------------------------------------------------------- LRU cache cap (pool thread)

        std::set<std::string> KeepSetLocked() {
            auto& s    = S();
            auto  keep = mj::pool::PooledFiles(s.index);
            keep.insert(s.inUse.begin(), s.inUse.end());
            for (const auto& sl : s.slots) {
                if (!sl.contentFile.empty()) keep.insert(sl.contentFile);
            }
            return keep;
        }

        // Leftovers of a crash: "<16 hex>.(wav|flac).part" (an interrupted download) and index.json.tmp. Only those
        // exact shapes are touched.
        void CleanOrphans() {
            auto&           s = S();
            std::error_code ec;
            std::size_t     n = 0;
            for (fs::directory_iterator it(s.cacheDir, ec), end; !ec && it != end; it.increment(ec)) {
                const auto name = it->path().filename().string();
                const bool part = name.ends_with(".part") && mj::pool::IsCacheFileName(name.substr(0, name.size() - 5));
                if (part || name == std::string(mj::pool::kIndexFileName) + ".tmp") {
                    std::error_code e2;
                    if (fs::remove(it->path(), e2)) ++n;
                }
            }
            if (n) spdlog::info("[cache] removed {} leftover partial file(s)", n);
        }

        // App::CacheBytes() = our cache files (pooled, slotted and LRU) + the 24 slot files under Data\Music\Harmony,
        // i.e. everything this mod keeps on disk for music. Re-measured after every refill step, eviction and Clear
        // cache, and every few seconds (slot writes) (Deck review: the number was only recomputed in EnforceCap).
        void MeasureDiskNow() {
            auto&                              s = S();
            std::vector<fs::path>              slots;
            for (int i = 0; i < kSlots; ++i) slots.push_back(Slots::HarmonyDir() / mj::retarget::SlotFileName(i));
            const auto d = mj::pool::MeasureDisk(s.cacheDir, slots);
            s.cacheBytes = d.cacheBytes + d.extraBytes;
        }

        // Deletes unprotected cache files, least recently used first, down to `a_cap` (0 = all of them).
        void EnforceCap(std::uint64_t a_cap, const char* a_why) {
            auto&                            s = S();
            std::vector<mj::pool::CacheFile> files;
            std::uint64_t                    total = 0;
            std::error_code                  ec;
            for (fs::directory_iterator it(s.cacheDir, ec), end; !ec && it != end; it.increment(ec)) {
                const auto name = it->path().filename().string();
                if (!mj::pool::IsCacheFileName(name) || !it->is_regular_file(ec)) continue;
                const auto sz = it->file_size(ec);
                const auto t  = it->last_write_time(ec);
                files.push_back({ name, sz, static_cast<std::int64_t>(t.time_since_epoch().count()) });
                total += sz;
            }
            std::vector<std::string> victims;
            {
                std::scoped_lock lk(s.mx);
                victims = mj::pool::PlanEviction(files, KeepSetLocked(), a_cap);
            }
            std::uint64_t freed = 0;
            for (const auto& v : victims) {
                const fs::path p = s.cacheDir / v;
                std::error_code e2;
                const auto      sz = fs::file_size(p, e2);
                // Re-check under the lock just before the delete: a file may have entered a pool meanwhile.
                {
                    std::scoped_lock lk(s.mx);
                    if (KeepSetLocked().contains(v)) continue;
                }
                if (fs::remove(p, e2)) {
                    freed += e2 ? 0 : sz;
                } else if (e2) {
                    spdlog::warn("[cache] could not delete {}: {}", v, e2.message());
                }
            }
            if (!victims.empty())
                spdlog::info("[cache] {}: deleted {} file(s), {:.1f} MB freed", a_why, victims.size(), freed / 1048576.0);
            (void)total;
            MeasureDiskNow();
        }

        // ---------------------------------------------------------------- pool thread state

        struct NetState {
            std::uint64_t               seenRev = ~0ull;
            bool                        settingsLoaded = false;
            std::shared_ptr<const mj::config::Settings> snap;
            mj::config::ProfileBuild    build;
            Net::HarmonyClient          client;
            std::string                 cfgUrl, cfgKey;
            bool                        clientOk = false;
            bool                        connected = false;      // last Status() succeeded and the server is enabled
            bool                        statusKnown = false;    // at least one Status() answered this session
            Net::StatusResult           status;
            bool                        profileOk = false;      // PUT accepted for the current settings
            bool                        profileRejected = false;
            bool                        needProfile = true;
            long long                   nextConnectMs = 0;
            long long                   connBackoffMs = kBackoffMinMs;
            bool                        authStop = false;       // 401/403: no network until the settings change
            std::map<std::string, int>  prevSlots;
            std::uint64_t               planGen = 0;
            mj::pool::ToastPolicy       toast;
            std::set<std::string>       loggedProblems;  // profile problems already logged (logged again only after they cleared)
        };

        void ToastNetFailure(NetState& ns) { QueueToast(ns.toast.OnNetworkFailure(SteadyMs())); }
        void ToastSuccess(NetState& ns) { QueueToast(ns.toast.OnSuccess(SteadyMs())); }

        void ApplyLogLevel(const mj::config::Settings& a_s) {
            if (auto log = spdlog::default_logger()) {
                log->set_level(mj::config::ToSpdlog(a_s.global.logLevel));
            }
        }

        void SetConn(bool a_configured, bool a_ok, std::string a_msg, const NetState* a_ns = nullptr) {
            auto&            s = S();
            std::scoped_lock lk(s.mx);
            s.conn.configured = a_configured;
            s.conn.ok         = a_ok;
            s.conn.tested     = true;
            s.conn.message    = std::move(a_msg);
            if (a_ns && a_ns->statusKnown) {
                const auto& st         = a_ns->status;
                s.conn.flac            = std::find(st.containers.begin(), st.containers.end(), "flac") != st.containers.end();
                s.conn.servicesReported = st.servicesReported;
                s.conn.ytmusic         = st.ytmusicUsable;
                s.conn.qobuz           = st.qobuzUsable;
            }
        }

        // Rebuilds and publishes the Plan from the current snapshot + catalogue + connection facts.
        void Replan(NetState& ns) {
            auto& s = S();
            std::vector<mj::retarget::TypeFacts> cat;
            {
                std::scoped_lock lk(s.mx);
                if (!s.haveCatalogue) return;
                cat = s.catalogue;
            }
            const auto& snap   = *ns.snap;
            auto        plan   = std::make_shared<Plan>();
            plan->gen          = ++ns.planGen;
            const bool netOn   = mj::config::NetworkActive(snap);
            const bool kill    = s.killFile.load();
            plan->engineWrites = netOn && snap.global.features.retarget && !kill && !ns.profileRejected;
            if (!snap.global.features.retarget) plan->why = "Retarget switched off in [Features]";
            else if (kill) plan->why = "kill switch file MarthJukebox.noretarget present";
            else if (!netOn) plan->why = snap.global.serverUrl.empty() ? "no Harmony server set" : "Network switched off in [Features]";
            else if (ns.profileRejected) plan->why = "Harmony rejected the profile";

            plan->typeSlot.assign(cat.size(), -1);
            std::map<std::string, std::vector<std::size_t>> byEnv;
            if (netOn && !ns.profileRejected) {
                for (std::size_t i = 0; i < cat.size(); ++i) {
                    const auto t = mj::retarget::EnvironmentForType(cat[i], snap, ns.build);
                    if (!t.envId.empty()) byEnv[t.envId].push_back(i);
                }
            }
            std::vector<std::string> envIds;
            for (const auto& [id, v] : byEnv) envIds.push_back(id);
            const auto slots = mj::retarget::AssignSlots(envIds, ns.prevSlots);
            ns.prevSlots     = slots.slotOf;
            const bool list  = ns.statusKnown && ns.status.servicesReported;
            for (const auto& [id, idxs] : byEnv) {
                const auto* e = snap.Find(id);
                if (!e) continue;  // cannot happen: EnvironmentForType only returns configured ids
                EnvPlan ep;
                ep.id                  = id;
                const auto it          = slots.slotOf.find(id);
                ep.slot                = it == slots.slotOf.end() ? -1 : it->second;
                ep.poolSize            = mj::pool::EffectivePoolSize(id, e->poolSize);  // combat/boss keep >= 5 (field #4)
                const auto src         = mj::config::ChooseSources(snap, *e, list);
                ep.sources             = src.sources;
                ep.streamingDropped    = src.streamingDropped && ns.statusKnown;
                ep.fingerprint         = mj::pool::Fingerprint(id, *e, ep.sources);
                if (ep.slot >= 0) {
                    plan->slotEnv[ep.slot] = id;
                    for (const auto i : idxs) plan->typeSlot[i] = ep.slot;
                }
                plan->envs.emplace(id, std::move(ep));
            }
            for (const auto& u : slots.unassigned)
                spdlog::warn("[plan] environment {} is active but all {} slots are taken: it stays Vanilla", u, kSlots);

            std::vector<std::string> dropped;
            {
                std::scoped_lock lk(s.mx);
                // Pools are matched against the new fingerprints only once the server's capabilities are known
                // (they decide `sources`), and only when the network is on (off = keep everything for later).
                if (ns.statusKnown && netOn && s.cacheReady) {
                    std::map<std::string, std::string> wanted;
                    for (const auto& [id, ep] : plan->envs) wanted[id] = ep.fingerprint;
                    dropped = mj::pool::DropChangedPools(s.index, wanted);
                    if (!dropped.empty()) MarkDirtyLocked();
                }
                for (auto& [id, ep] : plan->envs) {
                    // Kept until the cause changes (the next Replan recomputes it), never cleared by a refill.
                    s.envs[id].streamingNotice = ep.streamingDropped
                                                     ? "Streaming needs Harmony 1.0.13+ (or AllowQobuz=1 in the INI); playing from your library only"
                                                     : "";
                }
                for (auto& [id, es] : s.envs)
                    if (!plan->envs.contains(id)) es.nowPlaying.clear();  // not playing from Harmony any more
                s.plan = plan;
            }
            for (const auto& d : dropped) spdlog::info("[pool] pool {} dropped (selector, sources or mode changed, or now Vanilla)", d);
            std::size_t typesMapped = 0;
            for (const auto v : plan->typeSlot) typesMapped += v >= 0;
            spdlog::info("[plan] #{}: engine writes {}{}; {} environment(s), {} music type(s) mapped to slots", plan->gen,
                         plan->engineWrites ? "on" : "off", plan->why.empty() ? "" : " (" + plan->why + ")", plan->envs.size(), typesMapped);
            for (const auto& [id, ep] : plan->envs)
                spdlog::info("[plan]   {} -> slot {} pool {} sources={}", id, ep.slot, ep.poolSize, ep.sources);
            bool active = false;
            for (const auto& [id, ep] : plan->envs) active |= plan->engineWrites && ep.slot >= 0;
            ns.toast.SetActive(active);
            s.slotCv.notify_all();
        }

        // Settings revision changed (or first run): reload derived state, reconfigure the client.
        void OnSettings(NetState& ns) {
            auto& s  = S();
            ns.snap  = s.store->Snapshot();
            ns.seenRev = s.store->Revision();
            ApplyLogLevel(*ns.snap);
            ns.build = mj::config::BuildProfile(*ns.snap, false);
            // Each distinct problem is logged once, not on every settings change (Deck field log).
            std::set<std::string> now;
            for (const auto& p : ns.build.problems) {
                const auto line = std::format("{}{}: {}", p.environment.empty() ? "(profile)" : p.environment,
                                              p.severity == mj::config::ProblemSeverity::Error ? " (left Vanilla)" : "", p.message);
                now.insert(line);
                if (!ns.loggedProblems.contains(line)) spdlog::warn("[profile] {}", line);
            }
            ns.loggedProblems = std::move(now);
            ns.needProfile     = true;
            ns.profileOk       = false;
            ns.profileRejected = false;
            ns.authStop        = false;
            ns.nextConnectMs   = 0;
            ns.connBackoffMs   = kBackoffMinMs;
            ns.toast.Configure({ ns.snap->global.toastEnabled, mj::config::NetworkActive(*ns.snap),
                                 static_cast<long long>(ns.snap->global.toastIntervalMinutes) * 60'000ll });
            {
                std::scoped_lock lk(s.mx);
                s.capBytes = static_cast<std::uint64_t>(ns.snap->global.cacheCapMB) * 1048576ull;
                for (auto& [id, es] : s.envs) {
                    es.problem.clear();
                    es.smallSelection = false;
                    es.waitForPlay    = false;
                    es.backoffUntilMs = 0;
                    es.failures       = 0;
                }
            }
            if (!mj::config::NetworkActive(*ns.snap)) {
                ns.connected = false;
                SetConn(false, false, ns.snap->global.serverUrl.empty() ? "Not set up yet: enter your Harmony server address and key."
                                                                        : "Network is switched off ([Features] Network=0).");
                spdlog::info("[net] network inactive ({}): everything stays Vanilla",
                             ns.snap->global.serverUrl.empty() ? "no server URL" : "Network=0");
            } else if (ns.snap->global.serverUrl != ns.cfgUrl || ns.snap->global.personalKey != ns.cfgKey || !ns.clientOk) {
                Net::Config c;
                c.baseUrl     = ns.snap->global.serverUrl;
                c.personalKey = ns.snap->global.personalKey;
                c.userAgent   = s.userAgent;
                const auto r  = ns.client.Configure(c);
                ns.clientOk   = r.Ok();
                ns.cfgUrl     = c.baseUrl;
                ns.cfgKey     = c.personalKey;
                {
                    // ONLY these two assignments under the lock: SetConn below takes s.mx itself (review R3-1: holding
                    // it here deadlocked the pool thread on any address Configure rejects, freezing the game thread).
                    std::scoped_lock lk(s.mx);
                    s.netUrl = c.baseUrl;
                    s.netKey = c.personalKey;
                }
                ns.connected  = false;
                if (!r.Ok()) {
                    SetConn(true, false, "The server address is not valid: " + r.message);
                    spdlog::error("[net] client configuration rejected: {}", r.message);
                }
            }
            Replan(ns);
        }

        std::string ConnMessage(const Net::StatusResult& st, bool a_allowQobuz) {
            std::string srcs;
            auto        add = [&](const char* n) { srcs += srcs.empty() ? n : std::string(", ") + n; };
            if (st.servicesReported) {
                if (st.libraryUsable) add("Library");
                if (st.ytmusicUsable) add("YouTube Music");
                if (st.qobuzUsable && a_allowQobuz) add("Qobuz");
            }
            const bool flac = std::find(st.containers.begin(), st.containers.end(), "flac") != st.containers.end();
            std::string m   = "Connected.";
            if (st.servicesReported) m += srcs.empty() ? " No music source is usable on the server." : " Sources: " + srcs + ".";
            else if (a_allowQobuz) m += " This Harmony is older than 1.0.13: streaming environments use all its sources (Qobuz included, AllowQobuz=1).";
            else m += " This Harmony is older than 1.0.13: library only (streaming needs 1.0.13+, or AllowQobuz=1).";
            m += flac ? " FLAC transfer available." : " WAV transfer.";
            return m;
        }

        void NetFailure(NetState& ns, const Net::Result& r, const char* a_what) {
            if (Net::ClassifyFailure(r.error) == Net::FailureClass::kCancelled) return;  // our own cancel: not an outage
            ns.connected    = false;
            const long long now = SteadyMs();
            ns.nextConnectMs = now + ns.connBackoffMs;
            spdlog::warn("[net] {} failed: {} ({}); next attempt in {} s", a_what, Net::ErrorName(r.error), r.message, ns.connBackoffMs / 1000);
            ns.connBackoffMs = std::min(ns.connBackoffMs * 2, kBackoffMaxMs);
            ToastNetFailure(ns);
        }

        std::string PlainNetError(const Net::Result& r) {
            switch (r.error) {
            case Net::Error::kBadKey:      return "Harmony refused the personal key. Check the key in the settings.";
            case Net::Error::kDisabled:    return "The game source is switched off on the Harmony server.";
            case Net::Error::kUnreachable: return "Harmony is not reachable. Is the server running and the address right?";
            case Net::Error::kTimeout:     return "Harmony did not answer in time.";
            case Net::Error::kRedirect:    return "The server address redirects elsewhere; enter the final address.";
            case Net::Error::kBadConfig:   return "The server address is not valid.";
            default:                       return std::string("Harmony answered with an error (") + Net::ErrorName(r.error) + ").";
            }
        }

        // Status + profile PUT when needed. Returns true when the pool may use the network now.
        bool EnsureConnected(NetState& ns, const std::stop_token& a_st) {
            if (!ns.snap || !mj::config::NetworkActive(*ns.snap) || !ns.clientOk || ns.authStop) return false;
            const long long now = SteadyMs();
            if (!ns.connected) {
                if (now < ns.nextConnectMs) return false;
                const auto st = ns.client.Status(a_st);
                if (st.error == Net::Error::kCancelled) return false;
                if (!st.Ok() || !st.enabled || !st.ffmpeg) {
                    std::string msg = st.Ok() ? (!st.enabled ? "The game source is switched off on the Harmony server."
                                                             : "Harmony has no ffmpeg, so it cannot prepare audio.")
                                              : PlainNetError(st);
                    SetConn(true, false, msg);
                    if (st.error == Net::Error::kBadKey || st.error == Net::Error::kDisabled) {
                        ns.authStop = true;
                        spdlog::error("[net] {}: no further requests until the settings change", msg);
                        if (st.error == Net::Error::kBadKey) QueueToast(ns.toast.OnAuthFailure(SteadyMs()));
                        else ToastNetFailure(ns);  // the game source is off on the server: "not reachable" for us
                    } else {
                        NetFailure(ns, st, "status");  // includes a bad status (disabled / no ffmpeg)
                    }
                    return false;
                }
                const bool firstCaps = !ns.statusKnown || ns.status.servicesReported != st.servicesReported;
                ns.status        = st;
                ns.statusKnown   = true;
                ns.connected     = true;
                ns.connBackoffMs = kBackoffMinMs;
                SetConn(true, true, ConnMessage(st, ns.snap->global.allowQobuz), &ns);
                spdlog::info("[net] {}", ConnMessage(st, ns.snap->global.allowQobuz));
                ToastSuccess(ns);
                if (firstCaps) Replan(ns);  // sources (and so fingerprints) depend on the capabilities
            }
            if (ns.needProfile) {
                if (ns.build.json.empty()) {
                    ns.profileOk   = true;  // nothing to publish (every environment Vanilla)
                    ns.needProfile = false;
                    return true;
                }
                // `trim_silence` (cuts leading/trailing silence, so a track starts at once) only on a server new enough to
                // know the field: an older Harmony rejects unknown profile keys.
                const auto json = ns.statusKnown && ns.status.servicesReported ? mj::config::BuildProfile(*ns.snap, false, true).json
                                                                               : ns.build.json;
                const auto r = ns.client.PutProfile(ns.snap->global.harmonyProfileName, json, a_st);
                if (r.Ok()) {
                    ns.profileOk   = true;
                    ns.needProfile = false;
                    spdlog::info("[profile] published {} environment type(s) to Harmony profile {}", ns.build.publishedTypes.size(),
                                 ns.snap->global.harmonyProfileName);
                    Replan(ns);
                } else if (r.error == Net::Error::kBadRequest || r.error == Net::Error::kBadKey || r.error == Net::Error::kDisabled) {
                    ns.profileRejected = true;
                    ns.needProfile     = false;
                    spdlog::error("[profile] Harmony rejected the profile: {} ({}); every environment stays Vanilla", Net::ErrorName(r.error),
                                  r.message);
                    SetConn(true, false, "Harmony rejected the music settings: " + r.message, &ns);
                    Replan(ns);
                    return false;
                } else if (r.error == Net::Error::kCancelled) {
                    return false;  // settings changed / kill switch / shutdown: retried by the next loop, no back-off (R2-2)
                } else {
                    NetFailure(ns, r, "profile PUT");
                    return false;
                }
            }
            return ns.profileOk;
        }

        std::string ContainerOf(const std::string& a_url) {
            const auto q    = a_url.find('?');
            const auto path = a_url.substr(0, q);
            return path.ends_with(".flac") ? "flac" : "wav";
        }

        // Full decode check + the preview guard (DESIGN 8.8). True = acceptable.
        bool ValidateFile(const fs::path& a_p, double a_expectedS, std::string& a_why, const std::stop_token& a_st) {
            const auto r = mj::pool::ConvertToCanonicalWav(a_p, {}, a_st);
            if (!r.ok) {
                a_why = r.error;
                return false;
            }
            if (a_expectedS > 60.0 && r.durationS < 0.9 * a_expectedS) {
                a_why = std::format("preview or truncated stream ({:.1f} s of {:.0f} s)", r.durationS, a_expectedS);
                return false;
            }
            if (a_expectedS > 0.0 && r.durationS > 1.1 * a_expectedS + 30.0) {
                a_why = std::format("far longer than announced ({:.0f} s of {:.0f} s)", r.durationS, a_expectedS);
                return false;
            }
            return true;
        }

        void EnvBackoff(const std::string& a_env, long long a_ms, std::string a_problem) {
            auto&            s = S();
            std::scoped_lock lk(s.mx);
            auto&            es = s.envs[a_env];
            es.failures++;
            es.backoffUntilMs = SteadyMs() + a_ms;
            if (!a_problem.empty()) es.problem = std::move(a_problem);
        }

        // Settings changed, kill switch, clear-cache, stop: the current refill must yield (A-F3).
        // Between two picks of one refill: stop only when this environment's picks are no longer wanted (its selector,
        // sources or streaming consent changed, the server/key changed, the network went off), or for the kill switch,
        // Clear cache or shutdown. Edits of other environments, pool size, toasts... let the refill go on (marth: "only
        // dump downloads if the changed setting invalidates them").
        bool AbortWanted(NetState& ns, const EnvPlan& a_ep, const std::stop_token& a_st) {
            auto& s = S();
            if (a_st.stop_requested() || s.killFile.load()) return true;
            const auto snap  = s.store->Snapshot();
            bool       valid = true;
            {
                std::scoped_lock lk(s.mx);
                if (s.clearCacheRequested) return true;
                if (snap->revision == ns.snap->revision) return false;
                valid = mj::pool::DownloadStillValid(*snap, s.netUrl, s.netKey, a_ep.id, a_ep.fingerprint, s.conn.servicesReported);
                if (valid) s.settingsDirty = false;  // handled right here (below), not again at the top of the loop
            }
            if (!valid) return true;
            // Review R3-2: another environment changed (e.g. combat -> Vanilla, a new selector): apply it now (re-plan,
            // profile PUT queued) instead of after every pick of this refill; this refill and its download go on.
            spdlog::info("[pool] settings changed during the refill of {}; applying them now, the refill continues", a_ep.id);
            OnSettings(ns);
            return false;
        }

        void Reject(const std::string& a_env, const std::string& a_key, long long a_ms) {
            auto&            s = S();
            std::scoped_lock lk(s.mx);
            s.envs[a_env].rejected[a_key] = SteadyMs() + a_ms;
        }

        // One refill step for the neediest environment. Returns true when it did network work.
        bool RefillOnce(NetState& ns, const std::stop_token& a_st) {
            auto&       s = S();
            std::shared_ptr<const Plan> plan;
            const EnvPlan* pick = nullptr;
            int         have = 0;
            std::uint64_t pooledBytes = 0;
            {
                std::scoped_lock lk(s.mx);
                plan = s.plan;
                if (!plan || !plan->engineWrites || !s.cacheReady) return false;
                for (const auto& [id, p] : s.index.pools)
                    for (const auto& e : p.entries) pooledBytes += e.bytes;
                const long long now  = SteadyMs();
                int             best = INT32_MIN;
                for (const auto& [id, ep] : plan->envs) {
                    if (ep.slot < 0) continue;
                    if (const auto e = s.envs.find(id); e != s.envs.end() && e->second.waitForPlay) continue;  // small selection
                    const auto pit = s.index.pools.find(id);
                    const int  n   = pit == s.index.pools.end() ? 0 : static_cast<int>(pit->second.entries.size());
                    if (n >= ep.poolSize) continue;
                    if (const auto e = s.envs.find(id); e != s.envs.end() && now < e->second.backoffUntilMs) continue;
                    // Urgent pools first (empty, or combat/boss below 2), then combat/boss, then the largest gap.
                    const int score = mj::pool::RefillScore(id, n, ep.poolSize);
                    if (score > best) {
                        best = score;
                        pick = &ep;
                        have = n;
                    }
                }
                if (pick && pooledBytes >= s.capBytes) {
                    s.envs[pick->id].problem = "Cache size cap reached: raise CacheCapMB or use smaller pools";
                    s.envs[pick->id].backoffUntilMs = SteadyMs() + 60'000;
                    return false;
                }
            }
            if (!pick) return false;
            const EnvPlan ep     = *pick;  // copy: the plan pointer stays alive anyway
            const bool    urgent = mj::pool::UrgentRefill(ep.id, have);
            {
                std::scoped_lock lk(s.mx);  // what SettingsChanged() and the slot thread check before cancelling this job
                s.jobEnv    = ep.id;
                s.jobFp     = ep.fingerprint;
                s.jobUrgent = urgent;
            }
            struct JobClear {
                ~JobClear() {
                    std::scoped_lock lk(S().mx);
                    S().jobEnv.clear();
                    S().jobFp.clear();
                    S().jobUrgent = false;
                }
            } jobClear;
            const int     need = ep.poolSize - have;
            const bool    flac = ns.snap->global.features.flac && ns.statusKnown &&
                              std::find(ns.status.containers.begin(), ns.status.containers.end(), "flac") != ns.status.containers.end();
            const int count = std::clamp(std::max(3, need), 1, 10);
            // The container is sent ONLY for FLAC: a Harmony before 1.0.13 rejects unknown keys in `next` (review B2), and
            // WAV is its default anyway.
            const auto nr   = ns.client.Next(ns.snap->global.harmonyProfileName, ep.id, ep.sources, count, false, flac ? "flac" : "", a_st);
            if (nr.Ok()) ToastSuccess(ns);
            if (!nr.Ok()) {
                switch (nr.error) {
                case Net::Error::kCancelled: return false;
                case Net::Error::kTimeout:
                case Net::Error::kUnreachable: NetFailure(ns, nr, "next"); return false;
                case Net::Error::kBadKey:
                case Net::Error::kDisabled:
                    ns.authStop = true;
                    SetConn(true, false, PlainNetError(nr), &ns);
                    if (nr.error == Net::Error::kBadKey) QueueToast(ns.toast.OnAuthFailure(SteadyMs()));
                    return false;
                case Net::Error::kNoTracks:
                    EnvBackoff(ep.id, 600'000, ep.sources == "library" ? "No matching tracks in your library (try another genre or allow streaming)"
                                                                      : "No matching tracks (try another selection)");
                    spdlog::warn("[pool] {}: Harmony has no tracks for this selection (sources={}); retry in 10 min", ep.id, ep.sources);
                    return true;
                case Net::Error::kUnknownType:
                case Net::Error::kUnknownProfile:
                    ns.needProfile = true;  // the server lost or never had our profile: PUT it again
                    EnvBackoff(ep.id, 30'000, "");
                    spdlog::warn("[pool] {}: {} -> the profile is PUT again", ep.id, Net::ErrorName(nr.error));
                    return true;
                case Net::Error::kBusy:
                    EnvBackoff(ep.id, std::max(nr.retryAfterS, 5) * 1000ll, "");
                    return true;
                default:
                    {
                        long long ms = 15'000;
                        {
                            std::scoped_lock lk(s.mx);
                            ms = std::min<long long>(15'000ll << std::min(s.envs[ep.id].failures, 6), 600'000);
                        }
                        EnvBackoff(ep.id, ms, "Harmony could not pick a track right now");
                        spdlog::warn("[pool] {}: next failed: {} ({}); retry in {} s", ep.id, Net::ErrorName(nr.error), nr.message, ms / 1000);
                        return true;
                    }
                }
            }
            std::vector<std::pair<std::string, std::string>> pairs;
            for (const auto& t : nr.tracks) pairs.emplace_back(t.service, t.id);
            mj::pool::PickChoice choice;
            {
                std::scoped_lock      lk(s.mx);
                auto&                 es  = s.envs[ep.id];
                const long long       now = SteadyMs();
                std::set<std::string> exclude;
                for (auto it = es.rejected.begin(); it != es.rejected.end();) {
                    if (it->second <= now) {
                        it = es.rejected.erase(it);
                    } else {
                        exclude.insert(it->first);
                        ++it;
                    }
                }
                choice            = mj::pool::AcceptablePicks(s.index, ep.id, pairs, static_cast<std::size_t>(need), exclude);
                es.smallSelection = choice.repeats;
                if (choice.repeats)
                    spdlog::info("[pool] {}: only recently played tracks came back (small selection, server pool {}): taking the least "
                                 "recently played", ep.id, nr.poolSize);
            }
            const auto& ok = choice.picks;
            if (ok.empty()) {
                // Everything the server offered is already pooled (or just failed): a selection no bigger than the pool.
                // Do not poll `next` every 20 s: wait for the next play event (or a settings change) (review B r2).
                std::scoped_lock lk(s.mx);
                std::set<std::string> pooled;
                if (const auto pit = s.index.pools.find(ep.id); pit != s.index.pools.end())
                    for (const auto& e : pit->second.entries) pooled.insert(e.Key());
                bool allPooled = !pairs.empty();
                for (const auto& [svc, id] : pairs) allPooled &= pooled.contains(mj::pool::TrackKey(svc, id));
                if (allPooled || (nr.poolSize > 0 && nr.poolSize <= static_cast<std::int64_t>(pooled.size()))) {
                    auto& es          = s.envs[ep.id];
                    es.smallSelection = true;
                    es.waitForPlay    = true;
                    spdlog::info("[pool] {}: the selection offers no track beyond the {} pooled (server pool {}); waiting for the next "
                                 "play before asking again", ep.id, pooled.size(), nr.poolSize);
                    return true;
                }
            }
            if (ok.size() < pairs.size())
                spdlog::info("[pool] {}: {} of {} pick(s) skipped (recently played, already pooled, just failed or duplicate)", ep.id,
                             pairs.size() - ok.size(), pairs.size());
            int added = 0;
            for (const auto i : ok) {
                if (AbortWanted(ns, ep, a_st)) return true;
                const auto&       t    = nr.tracks[i];
                const std::string cont = ContainerOf(t.audioUrl);
                const std::string file = mj::pool::CacheFileName(t.service, t.id, cont);
                const fs::path    path = s.cacheDir / file;
                const auto        t0   = std::chrono::steady_clock::now();
                std::string       why;
                std::error_code   ec;
                bool              good = false;
                long long         rejectMs = kRejectShortMs;
                bool              kept = false;  // pooled elsewhere / in a slot / being read: already validated, never replaced
                {
                    std::scoped_lock lk(s.mx);
                    kept = KeepSetLocked().contains(file);
                }
                if (fs::exists(path, ec) && (kept || ValidateFile(path, static_cast<double>(t.durationS), why, a_st))) {
                    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // LRU touch
                    good = true;
                    spdlog::info("[pool] {}: cache hit {} for a pick", ep.id, file);
                } else if (!kept) {
                    // A pool that is not urgent gives a cold transcode 60 s, not 180 s: the server keeps transcoding,
                    // so the pick comes back fast after a short wait, and an empty pool is never stuck behind it
                    // (field #4: a dungeon pick held the only pool thread for 180 s while combat was dry).
                    {
                        std::scoped_lock lk(s.mx);
                        s.netWhat = std::format("{} download ({}{})", ep.id, urgent ? "urgent" : "not urgent", ", pool " +
                                                std::to_string(have) + "/" + std::to_string(ep.poolSize));
                    }
                    s.netSince    = SteadyMs();
                    const auto dr = ns.client.DownloadAudio(t.audioUrl, path, static_cast<double>(t.durationS), a_st, urgent ? 0 : 60);
                    s.netSince    = 0;
                    if (dr.error == Net::Error::kCancelled) {
                        if (!a_st.stop_requested() || urgent) return true;
                        spdlog::info("[pool] {}: download cancelled (settings, kill switch, or an empty pool needs the thread)", ep.id);
                        return true;
                    }
                    if (dr.error == Net::Error::kTimeout || dr.error == Net::Error::kUnreachable) {
                        NetFailure(ns, dr, "download");
                        return true;
                    }
                    if (!dr.Ok()) {
                        why = std::format("{} ({})", Net::ErrorName(dr.error), dr.message);
                        if (dr.error == Net::Error::kPreviewOrTruncated || dr.error == Net::Error::kBadAudio) rejectMs = kRejectLongMs;
                        if (dr.error == Net::Error::kDeadline && !urgent) rejectMs = 3 * 60'000;  // still transcoding: back soon
                    } else if (ValidateFile(path, static_cast<double>(t.durationS), why, a_st)) {
                        good = true;
                        ToastSuccess(ns);
                    } else {
                        rejectMs = kRejectLongMs;
                        bool held = false;
                        {
                            std::scoped_lock lk(s.mx);
                            held = KeepSetLocked().contains(file);
                        }
                        // Never delete a file a pool, a slot or the slot thread holds (review B SEV-4). Only this thread
                        // adds pool entries, so the check cannot go stale before the remove.
                        if (!held) fs::remove(path, ec);
                    }
                }
                if (!good) {
                    // Skipped for a while, but NOT counted as played (review B1).
                    spdlog::warn("[pool] {}: pick rejected for {} min: {}; file {}", ep.id, rejectMs / 60'000, why, file);
                    Reject(ep.id, mj::pool::TrackKey(t.service, t.id), rejectMs);
                    continue;
                }
                mj::pool::PoolEntry e;
                e.service   = t.service;
                e.id        = t.id;
                e.title     = t.title;
                e.artist    = t.artist;
                e.durationS = static_cast<double>(t.durationS);
                e.bytes     = fs::file_size(path, ec);
                e.file      = file;
                e.sources   = ep.sources;
                if (ec || e.bytes == 0) continue;
                {
                    std::scoped_lock lk(s.mx);
                    // The plan may have changed during the download: only add while the fingerprint still matches.
                    const EnvPlan* now = nullptr;
                    if (s.plan) {
                        if (const auto it = s.plan->envs.find(ep.id); it != s.plan->envs.end()) now = &it->second;
                    }
                    if (!now || now->fingerprint != ep.fingerprint) {
                        spdlog::info("[pool] {}: settings changed during the download; {} kept only as cache", ep.id, file);
                        continue;
                    }
                    auto& pool       = s.index.pools[ep.id];
                    pool.fingerprint = ep.fingerprint;
                    if (static_cast<int>(pool.entries.size()) >= now->poolSize) continue;
                    pool.entries.push_back(e);
                    auto& es    = s.envs[ep.id];
                    es.failures = 0;
                    es.problem.clear();
                    MarkDirtyLocked();
                    s.slotCv.notify_all();
                }
                ++added;
                spdlog::info("[pool] {}: + {} ({} s, {:.1f} MB {}, {} ms) pool {}/{}", ep.id, e.Display(), t.durationS, e.bytes / 1048576.0,
                             cont, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(),
                             have + added, ep.poolSize);
            }
            if (added == 0 && !ok.empty()) {
                bool repeated = false;
                {
                    std::scoped_lock lk(s.mx);
                    repeated = s.envs[ep.id].failures >= 2;  // this makes 3 rounds in a row
                }
                // Never a silent "Filling pool": say why after a few rounds of rejected picks.
                EnvBackoff(ep.id, 15'000, repeated ? "Harmony's tracks for this selection keep failing to download or check (see "
                                                     "MarthJukebox.log); retrying" : "");
            }
            if (ok.empty()) EnvBackoff(ep.id, 20'000, "");  // nothing usable came back: let the server's bag move on
            return true;  // the pool thread's loop enforces the cap and re-measures the disk after every step
        }

    }  // namespace

    namespace detail {

        void PoolThread(std::stop_token a_st) {
            auto&    s = S();
            NetState ns;
            Slots::LowerCurrentThreadPriority("pool");
            spdlog::info("[pool] thread started");
            // 1. Settings (file IO kept off the game thread).
            const bool okLoad = s.store->Load();
            spdlog::info("[settings] {} loaded{}", Slots::Utf8(s.store->IniPath()), okLoad ? "" : " (defaults: file missing or unusable)");
            {
                const auto snap = s.store->Snapshot();
                ApplyLogLevel(*snap);
                std::scoped_lock lk(s.mx);
                s.cacheDir = s.store->EffectiveCacheDir();
                s.capBytes = static_cast<std::uint64_t>(snap->global.cacheCapMB) * 1048576ull;
            }
            // Remember-place OFF has no safe mechanism yet (docs/INTEGRATION-1.0.md A.3): the setting is kept and
            // persisted, and the engine's own resume (M0b) is what plays in 1.0.
            spdlog::info("[pool] remember track place: always on in 1.0 (engine default); the per-environment setting is stored only");
            std::error_code ec;
            fs::create_directories(s.cacheDir, ec);
            if (ec) spdlog::error("[pool] cannot create the cache dir {}: {}", Slots::Utf8(s.cacheDir), ec.message());
            spdlog::info("[pool] cache dir {}", Slots::Utf8(s.cacheDir));
            CleanOrphans();
            LoadIndex();
            EnforceCap(s.capBytes, "startup cap check");
            // The slot thread polls the kill-switch file every 2 s; this first check covers the time before that.
            if (Slots::KillSwitchFilePresent() && !s.killFile.exchange(true))
                spdlog::warn("[retarget] KILL SWITCH file present: no retarget this session (until it is removed)");

            long long lastMeasure = SteadyMs();
            while (!a_st.stop_requested()) {
                bool dirty = false, clear = false, cat = false, replan = false;
                // A fresh per-job stop source (NET-CLIENT.md): SettingsChanged() / the kill switch cancel only this job.
                std::stop_source job;
                {
                    std::scoped_lock lk(s.mx);
                    dirty                 = s.settingsDirty || s.store->Revision() != ns.seenRev;
                    s.settingsDirty       = false;
                    clear                 = s.clearCacheRequested;
                    s.clearCacheRequested = false;
                    cat                   = s.haveCatalogue && ns.planGen == 0;
                    replan                = s.replanWanted;
                    s.replanWanted        = false;
                    s.poolWake            = false;
                    s.jobStop             = job;
                }
                std::stop_callback onStop(a_st, [job]() mutable { job.request_stop(); });
                if (dirty) OnSettings(ns);
                else if (cat || (replan && ns.snap)) Replan(ns);
                if (clear) {
                    EnforceCap(0, "Clear cache");
                }
                QueueToast(ns.toast.Tick(SteadyMs()));
                bool worked = false;
                if (EnsureConnected(ns, job.get_token())) worked = RefillOnce(ns, job.get_token());
                SaveIndexIfDirty();
                if (worked || SteadyMs() - lastMeasure >= 5000) {
                    lastMeasure = SteadyMs();
                    EnforceCap(s.capBytes, "cache cap");  // also re-measures CacheBytes()
                }
                if (worked) continue;
                std::unique_lock lk(s.mx);
                (void)s.poolCv.wait_for(lk, a_st, 1000ms, [&] {
                    return s.settingsDirty || s.clearCacheRequested || s.poolWake || s.replanWanted || s.store->Revision() != ns.seenRev ||
                           (s.haveCatalogue && ns.planGen == 0);
                });
            }
            SaveIndexIfDirty();
            spdlog::info("[pool] thread stopped");
        }

    }  // namespace detail

    // ---------------------------------------------------------------- entry points

    namespace {
        struct Threads {
            std::jthread pool, slot;
            ~Threads() {
                // DLL_PROCESS_DETACH: request stop and join (the threads only wait on stop-aware waits).
                pool.request_stop();
                slot.request_stop();
                if (pool.joinable()) pool.join();
                if (slot.joinable()) slot.join();
            }
        } g_threads;
    }

    long long SteadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void Start(mj::config::SettingsStore* a_store, std::string a_userAgent) {
        if (g_threads.pool.joinable()) return;
        S().store      = a_store;
        S().userAgent  = std::move(a_userAgent);
        g_threads.pool = std::jthread(PoolThread);
        g_threads.slot = std::jthread(SlotThread);
    }

    void SetCatalogue(std::vector<mj::retarget::TypeFacts> a_types) {
        auto&            s = S();
        std::scoped_lock lk(s.mx);
        s.catalogue     = std::move(a_types);
        s.haveCatalogue = true;
        s.poolCv.notify_all();
    }

    void SettingsChanged() {
        auto&      s    = S();
        const auto snap = s.store ? s.store->Snapshot() : nullptr;
        std::scoped_lock lk(s.mx);
        s.settingsDirty = true;
        if (!snap) {  // before Start(): nothing runs yet (review R3-3)
            s.poolCv.notify_all();
            return;
        }
        // Cancel the download in flight ONLY when it cannot be used any more: the server address or key changed, or
        // the network was switched off. Any other edit (a selector, a checkbox, typing in the menu) lets it finish; a
        // track whose selection changed meanwhile simply stays in the cache (Deck field log: 4 of 32 downloads were
        // cancelled by unrelated menu edits). The refill re-checks the settings between picks (AbortWanted).
        const bool serverSame = snap && mj::config::NetworkActive(*snap) && snap->global.serverUrl == s.netUrl &&
                                snap->global.personalKey == s.netKey;
        const bool jobValid   = s.jobEnv.empty() ? serverSame
                                                 : mj::pool::DownloadStillValid(*snap, s.netUrl, s.netKey, s.jobEnv, s.jobFp,
                                                                                s.conn.servicesReported);
        if (!jobValid) s.jobStop.request_stop();  // signal only: a download in flight returns kCancelled at once
        s.poolCv.notify_all();
    }

    std::string PerfNow() {
        auto&           s   = S();
        const long long now = SteadyMs();
        std::string     out;
        if (const long long t = s.slotJobSince.load(); t > 0)
            out = std::format("slot thread writing slot {:02} for {} ms", s.slotJobSlot.load(), now - t);
        else if (const long long e = s.lastSlotJobEnd.load(); e > 0)
            out = std::format("slot thread idle, last slot write took {} ms and ended {} ms ago", s.lastSlotJobMs.load(), now - e);
        else
            out = "slot thread idle";
        std::string what;
        {
            std::scoped_lock lk(s.mx);
            what = s.netWhat;
        }
        if (const long long n = s.netSince.load(); n > 0) out += std::format("; pool thread in a {} for {} ms (network-paced writes)", what, now - n);
        else out += "; pool thread not downloading";
        return out;
    }

    std::optional<std::string> PopToast() {
        auto&            s = S();
        std::scoped_lock lk(s.mx);
        if (s.toasts.empty()) return std::nullopt;
        auto t = std::move(s.toasts.front());
        s.toasts.pop_front();
        return t;
    }

    void ClearCache() {
        auto&            s = S();
        std::scoped_lock lk(s.mx);
        s.clearCacheRequested = true;
        s.poolCv.notify_all();
    }

    std::uint64_t CacheBytes() { return S().cacheBytes.load(); }

    std::shared_ptr<const Plan> CurrentPlan() {
        auto&            s = S();
        std::scoped_lock lk(s.mx);
        return s.plan;
    }

    std::array<SlotView, kSlots> SlotViews() {
        auto&                        s = S();
        std::array<SlotView, kSlots> out;
        std::scoped_lock             lk(s.mx);
        for (int i = 0; i < kSlots; ++i) out[i] = s.slots[i];
        return out;
    }

    void PublishBound(int a_slot, const std::string& a_env) {
        if (a_slot < 0 || a_slot >= kSlots) return;
        auto&            s = S();
        std::scoped_lock lk(s.mx);
        if (s.slots[a_slot].bound != a_env) {
            s.slots[a_slot].bound = a_env;
            s.slotCv.notify_all();
        }
    }

    void PublishPlay(int a_slot, bool a_current, std::uint32_t a_handle, std::uint32_t a_idx) {
        if (a_slot < 0 || a_slot >= kSlots) return;
        auto& s = S();
        s.playHandle[a_slot].store(a_handle);
        s.playIdx[a_slot].store(a_idx);
        s.playCurrent[a_slot].store(a_current);
    }

    void PostPlayEvent(int a_slot, const std::string& a_env, std::uint32_t a_handle, std::uint32_t a_idx) {
        if (a_slot < 0 || a_slot >= kSlots) return;
        auto&            s = S();
        std::scoped_lock lk(s.mx);
        s.events.push_back({ a_slot, a_env, a_handle, a_idx, SteadyMs() });
        s.slotCv.notify_all();
    }

    bool KillSwitchSeen() { return S().killFile.load(); }

    std::vector<EnvReport> Report() {
        auto&                  s = S();
        std::vector<EnvReport> out;
        std::scoped_lock       lk(s.mx);
        std::set<std::string>  ids;
        if (s.plan)
            for (const auto& [id, ep] : s.plan->envs) ids.insert(id);
        for (const auto& [id, es] : s.envs) ids.insert(id);
        for (const auto& id : ids) {
            EnvReport r;
            r.id = id;
            if (s.plan) {
                if (const auto it = s.plan->envs.find(id); it != s.plan->envs.end()) {
                    r.slot    = it->second.slot;
                    r.planned = it->second.slot >= 0;
                    r.target  = it->second.poolSize;
                }
            }
            if (const auto p = s.index.pools.find(id); p != s.index.pools.end()) {
                r.ready = static_cast<int>(p->second.entries.size());
                for (const auto& e : p->second.entries) r.titles.push_back(e.Display());
            }
            if (const auto e = s.envs.find(id); e != s.envs.end()) {
                std::string p = e->second.problem;
                auto        add = [&p](const std::string& a_t) { p += (p.empty() || a_t.empty() ? "" : "; ") + a_t; };
                add(e->second.streamingNotice);
                if (e->second.smallSelection) add("Small selection: tracks will repeat (pick a broader genre or allow streaming)");
                r.problem    = p;
                r.nowPlaying = e->second.nowPlaying;
            }
            out.push_back(std::move(r));
        }
        return out;
    }

    ConnReport Connection() {
        auto&            s = S();
        std::scoped_lock lk(s.mx);
        return s.conn;
    }

    void SetPlayedCallback(PlayedFn a_fn) {
        auto&            s = S();
        std::scoped_lock lk(s.mx);
        s.played = std::move(a_fn);
    }

}  // namespace MJ::Pool
