// mj_net_tests: console tests for MJ::Net::HarmonyClient against a scripted in-process HTTP/1.1
// server on 127.0.0.1 (Winsock, test-only). Never touches a real Harmony. Exit code = number of
// failed tests (0 = all passed), so the CI step fails the job on any failure.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "net/HarmonyClient.h"
#include "pool/Pool.h"

#include <future>

namespace MJ::Net {
    long LiveRequestContextsForTests();  // test hook in HarmonyClient.cpp (not in the public header)
}

using namespace MJ::Net;
namespace fs = std::filesystem;
using json   = nlohmann::json;

// ---------------------------------------------------------------------------------------------
// mini test harness
namespace {

    int         g_pass = 0, g_fail = 0;
    bool        g_curFailed = false;
    std::string g_curName;

    void Fail(int a_line, const std::string& a_expr, const std::string& a_detail = {}) {
        g_curFailed = true;
        std::printf("    FAIL line %d: %s %s\n", a_line, a_expr.c_str(), a_detail.c_str());
    }

#define CHECK(c) \
    do { if (!(c)) Fail(__LINE__, #c); } while (0)
#define CHECK_ERR(res, want) \
    do { \
        const auto& mj_r_ = (res); \
        if (mj_r_.error != (want)) Fail(__LINE__, "error == " #want, std::string("got ") + ErrorName(mj_r_.error) + " (" + mj_r_.message + ")"); \
    } while (0)

    std::ostringstream g_log;
    std::string        LogText() { return g_log.str(); }
    void               ClearLog() { g_log.str(std::string()); }

    fs::path g_root;

    fs::path TestDir() {
        fs::path d = g_root / g_curName;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    std::size_t CountFiles(const fs::path& a_dir) {
        std::size_t n = 0;
        std::error_code ec;
        for (auto it = fs::directory_iterator(a_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) ++n;
        return n;
    }

    // ---------------------------------------------------------------------------------------
    // fixtures
    void Put32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
    void Put16(std::string& s, std::uint16_t v) { for (int i = 0; i < 2; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }

    // Canonical PCM WAV of silence; a_list adds a 10-byte LIST chunk between fmt and data.
    std::string MakeWav(std::uint32_t a_rate, std::uint16_t a_ch, std::uint16_t a_bits, double a_seconds, bool a_list = false) {
        const std::uint64_t frames = static_cast<std::uint64_t>(a_rate * a_seconds);
        const std::uint32_t block  = a_ch * (a_bits / 8);
        const std::uint32_t data   = static_cast<std::uint32_t>(frames * block);
        std::string s = "RIFF";
        Put32(s, 4 + 24 + (a_list ? 18u : 0u) + 8 + data + (data & 1));
        s += "WAVEfmt ";
        Put32(s, 16);
        Put16(s, 1);
        Put16(s, a_ch);
        Put32(s, a_rate);
        Put32(s, a_rate * block);
        Put16(s, static_cast<std::uint16_t>(block));
        Put16(s, a_bits);
        if (a_list) {
            s += "LIST";
            Put32(s, 10);
            s += "INFOabcdef";
        }
        s += "data";
        Put32(s, data);
        s.append(data, '\0');
        if (data & 1) s.push_back('\0');
        return s;
    }

    // fLaC + STREAMINFO (+ junk standing in for frames); the client only reads the header.
    std::string MakeFlac(std::uint32_t a_rate, std::uint16_t a_ch, std::uint16_t a_bps, std::uint64_t a_total) {
        std::string s = "fLaC";
        s.push_back(static_cast<char>(0x80));  // last-metadata-block flag | type 0 (STREAMINFO)
        s.push_back(0);
        s.push_back(0);
        s.push_back(34);
        s.push_back(0x10); s.push_back(0x00);  // min block size 4096
        s.push_back(0x10); s.push_back(0x00);  // max block size 4096
        s.append(6, '\0');                     // min/max frame size unknown
        const std::uint64_t v = (static_cast<std::uint64_t>(a_rate) << 44) | (static_cast<std::uint64_t>(a_ch - 1) << 41) |
                                (static_cast<std::uint64_t>(a_bps - 1) << 36) | a_total;
        for (int i = 7; i >= 0; --i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        s.append(16, '\0');  // MD5
        s.append(2000, '\x5A');
        return s;
    }

    void WriteBytes(const fs::path& p, const std::string& d) {
        std::ofstream o(p, std::ios::binary);
        o.write(d.data(), static_cast<std::streamsize>(d.size()));
    }

    // ---------------------------------------------------------------------------------------
    // scripted HTTP/1.1 server ("Connection: close"). Default: one connection at a time. Threaded:
    // one thread per connection, so a stalled handler (or a connection the client abandoned after a
    // cancel) never blocks the next request.
    struct Req {
        std::string method, target, body;
        std::map<std::string, std::string> headers;  // lower-case names
    };

    struct Conn {
        SOCKET             s = INVALID_SOCKET;
        std::atomic<bool>* stop = nullptr;
        bool Send(const std::string& d) const {
            std::size_t off = 0;
            while (off < d.size()) {
                const int n = ::send(s, d.data() + off, static_cast<int>((std::min<std::size_t>)(d.size() - off, 1 << 20)), 0);
                if (n <= 0) return false;
                off += static_cast<std::size_t>(n);
            }
            return true;
        }
        // Sleeps up to a_ms; returns false early when the server is stopping.
        bool Wait(int a_ms) const {
            for (int left = a_ms; left > 0; left -= 20) {
                if (stop->load()) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return !stop->load();
        }
    };

    std::string Resp(int a_status, const char* a_reason, const std::string& a_body, const std::string& a_extra = {},
                     const char* a_ctype = "application/json") {
        std::string r = "HTTP/1.1 " + std::to_string(a_status) + " " + a_reason + "\r\nContent-Type: " + a_ctype +
                        "\r\nContent-Length: " + std::to_string(a_body.size()) + "\r\nConnection: close\r\n" + a_extra + "\r\n";
        return r + a_body;
    }
    std::string ErrResp(int a_status, const char* a_reason, const char* a_code, const char* a_msg, const std::string& a_extra = {}) {
        return Resp(a_status, a_reason, json{ { "error", a_msg }, { "code", a_code } }.dump(), a_extra);
    }
    std::string AudioHead(std::size_t a_len, const char* a_type = "audio/wav") {
        return "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(a_type) + "\r\nContent-Length: " + std::to_string(a_len) +
               "\r\nConnection: close\r\n\r\n";
    }

    class Server {
    public:
        using Handler = std::function<void(const Req&, Conn&, int)>;

        explicit Server(Handler a_h, bool a_threaded = false) : handler_(std::move(a_h)), threaded_(a_threaded) {
            ls_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{};
            a.sin_family      = AF_INET;
            a.sin_port        = 0;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(ls_, 16) != 0) {
                std::printf("    test server bind/listen failed (%d)\n", ::WSAGetLastError());
                std::exit(2);
            }
            int len = sizeof a;
            ::getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
            port_ = ntohs(a.sin_port);
            th_   = std::thread([this] { Loop(); });
        }
        ~Server() { Stop(); }
        void Stop() {
            if (stop_.exchange(true)) return;
            ::closesocket(ls_);
            if (th_.joinable()) th_.join();
            for (auto& w : workers_) {
                if (w.joinable()) w.join();
            }
        }
        std::uint16_t Port() const { return port_; }
        std::string   Url(const std::string& a_suffix = {}) const { return "http://127.0.0.1:" + std::to_string(port_) + a_suffix; }
        std::vector<Req> Seen() {
            std::lock_guard l(m_);
            return seen_;
        }
        void SetHandler(Handler h) {
            std::lock_guard l(m_);
            handler_ = std::move(h);
        }

    private:
        void Loop() {
            while (!stop_.load()) {
                SOCKET c = ::accept(ls_, nullptr, nullptr);
                if (c == INVALID_SOCKET) break;
                if (threaded_) workers_.emplace_back([this, c] { Serve(c); });
                else Serve(c);
            }
        }
        static bool ReadSome(SOCKET c, std::string& buf) {
            char tmp[4096];
            const int n = ::recv(c, tmp, sizeof tmp, 0);
            if (n <= 0) return false;
            buf.append(tmp, static_cast<std::size_t>(n));
            return true;
        }
        void Serve(SOCKET c) {
            DWORD to = 5000;
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
            std::string buf;
            std::size_t hend = std::string::npos;
            while ((hend = buf.find("\r\n\r\n")) == std::string::npos) {
                if (!ReadSome(c, buf)) {
                    ::closesocket(c);
                    return;
                }
            }
            Req r;
            std::istringstream hs(buf.substr(0, hend));
            std::string        line;
            std::getline(hs, line);
            {
                std::istringstream rl(line);
                std::string        ver;
                rl >> r.method >> r.target >> ver;
            }
            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string k = line.substr(0, colon);
                for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                std::string v = line.substr(colon + 1);
                while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                r.headers[k] = v;
            }
            std::size_t need = 0;
            if (const auto it = r.headers.find("content-length"); it != r.headers.end()) need = std::stoul(it->second);
            std::string body = buf.substr(hend + 4);
            while (body.size() < need) {
                if (!ReadSome(c, body)) break;
            }
            r.body = body.substr(0, need);
            int idx;
            Handler h;
            {
                std::lock_guard l(m_);
                idx = static_cast<int>(seen_.size());
                seen_.push_back(r);
                h = handler_;
            }
            Conn conn;
            conn.s    = c;
            conn.stop = &stop_;
            h(r, conn, idx);
            ::shutdown(c, SD_SEND);
            ::closesocket(c);
        }

        Handler           handler_;
        bool              threaded_ = false;
        std::vector<std::thread> workers_;  // threaded mode; touched only by the accept thread and Stop()
        SOCKET            ls_ = INVALID_SOCKET;
        std::uint16_t     port_ = 0;
        std::thread       th_;
        std::atomic<bool> stop_{ false };
        std::mutex        m_;
        std::vector<Req>  seen_;
    };

    Config MakeConfig(const std::string& a_url, const std::string& a_key = {}) {
        Config c;
        c.baseUrl          = a_url;
        c.personalKey      = a_key;
        c.connectTimeoutMs = 2000;
        c.sendTimeoutMs    = 5000;
        c.receiveTimeoutMs = 8000;
        return c;
    }

    void Configured(HarmonyClient& a_cl, const Config& a_cfg) {
        const Result r = a_cl.Configure(a_cfg);
        if (!r.Ok()) Fail(__LINE__, "Configure(" + a_cfg.baseUrl + ")", r.message);
    }

    // Busy-wait (Windows sleep_for has >= 1 ms granularity, far too coarse to hit a request start).
    void SpinUs(int a_us) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(a_us);
        while (std::chrono::steady_clock::now() < end) {}
    }

    long long MsSince(std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
    }

    const char* kAudioUrl = "/api/game/audio/local/87fbeaab3e0ef480.wav?rate=48000&bits=16&channels=2&normalize=1&trim=0";

    // ---------------------------------------------------------------------------------------
    // tests
    void T_Status() {
        Server s([](const Req&, Conn& c, int) {
            c.Send(Resp(200, "OK", R"({"enabled":true,"ffmpeg":true,"containers":["wav","flac"],
                "cache":{"bytes":1291990,"files":2,"in_progress_bytes":5,"cap":2147483648},
                "now_playing":null,"history":[],"profiles":["skyrim"],"future":{"x":1}})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const StatusResult r = cl.Status();
        CHECK_ERR(r, Error::kOk);
        CHECK(r.reachable && r.enabled && r.ffmpeg);
        CHECK(r.containers.size() == 2 && r.containers[0] == "wav" && r.containers[1] == "flac");
        CHECK(r.cache.bytes == 1291990 && r.cache.files == 2 && r.cache.inProgressBytes == 5 && r.cache.cap == 2147483648LL);
        const auto seen = s.Seen();
        CHECK(seen.size() == 1 && seen[0].method == "GET" && seen[0].target == "/api/game/status");
        CHECK(seen[0].headers.count("x-harmony-key") == 0);  // no key configured => no header
        CHECK(seen[0].headers.at("user-agent").find("MarthJukebox/") == 0);
    }

    void T_StatusNoContainers() {
        Server s([](const Req&, Conn& c, int) { c.Send(Resp(200, "OK", R"({"enabled":false,"ffmpeg":false})")); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const StatusResult r = cl.Status();
        CHECK_ERR(r, Error::kOk);
        CHECK(r.reachable && !r.enabled && !r.ffmpeg);
        CHECK(r.containers.size() == 1 && r.containers[0] == "wav");
        CHECK(!r.servicesReported && !r.ytmusicUsable);  // older server: no source-list support
    }

    // Harmony 1.0.13+: `services` + `excluded_services` (the capability check for `sources` lists).
    void T_StatusServices() {
        Server s([](const Req&, Conn& c, int) {
            c.Send(Resp(200, "OK", R"({"enabled":true,"ffmpeg":true,"containers":["wav","flac"],
              "services":{"library":true,"ytmusic":true,"qobuz":false},"excluded_services":["qobuz",3]})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const StatusResult r = cl.Status();
        CHECK_ERR(r, Error::kOk);
        CHECK(r.servicesReported && r.libraryUsable && r.ytmusicUsable && !r.qobuzUsable);
        CHECK(r.excludedServices.size() == 1 && r.excludedServices[0] == "qobuz");
        CHECK(r.containers.size() == 2);
    }

    // Library genres / artists and the playlists listing (menu browse without text entry).
    void T_Browse() {
        Server s([](const Req& q, Conn& c, int) {
            if (q.target.rfind("/api/library/view?view=genres", 0) == 0)
                c.Send(Resp(200, "OK", R"({"enabled":true,"view":"genres","genres":[{"name":"Folk","track_count":12},{"name":""},7],"total":2})"));
            else if (q.target.rfind("/api/library/view?view=artists", 0) == 0)
                c.Send(Resp(200, "OK", R"({"enabled":false,"view":"artists","artists":[],"total":0})"));
            else
                c.Send(Resp(200, "OK", R"({"playlists":[{"id":"PL1","title":"Epic","service":"ytmusic","track_count":40,"owner":"me"}]})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const BrowseResult g = cl.LibraryView("genres", 9000);
        CHECK_ERR(g, Error::kOk);
        CHECK(g.enabled && g.items.size() == 1 && g.items[0].id == "Folk" && g.items[0].title == "Folk" && g.items[0].service == "local" &&
              g.items[0].trackCount == 12);
        const BrowseResult a = cl.LibraryView("artists", 50);
        CHECK_ERR(a, Error::kOk);
        CHECK(!a.enabled && a.items.empty());
        const BrowseResult p = cl.Playlists();
        CHECK_ERR(p, Error::kOk);
        CHECK(p.items.size() == 1 && p.items[0].id == "PL1" && p.items[0].owner == "me" && p.items[0].trackCount == 40);
        CHECK_ERR(cl.LibraryView("songs", 5), Error::kBadRequest);
        const auto seen = s.Seen();
        CHECK(seen.size() == 3 && seen[0].target == "/api/library/view?view=genres&limit=5000" && seen[2].target == "/api/playlists");
    }

    // Review B2: Harmony 1.0.12 accepts only profile/type/sources/count/peek in `next` and answers 400 to anything
    // else. The plugin passes an EMPTY container for WAV, which must leave the key out of the body entirely.
    void T_NextOldServerRejectsUnknownKeys() {
        Server s([](const Req& q, Conn& c, int) {
            const json b = json::parse(q.body, nullptr, false);
            bool       ok = b.is_object();
            if (ok) {
                for (const auto& [k, v] : b.items()) {
                    if (k != "profile" && k != "type" && k != "sources" && k != "count" && k != "peek") ok = false;
                }
            }
            if (ok) c.Send(Resp(200, "OK", R"({"profile":"p","type":"explore","sources":"library","pool_size":3,"tracks":[]})"));
            else c.Send(Resp(400, "Bad Request", R"({"error":"unknown field","code":"bad_request"})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        CHECK_ERR(cl.Next("p", "explore", "library", 3, false, ""), Error::kOk);
        CHECK_ERR(cl.Next("p", "explore", "library", 3, false, "flac"), Error::kBadRequest);  // what an old server does
    }

    // Review R2-2: a cancel is never a network failure (no back-off, no toast count).
    void T_ClassifyFailure() {
        CHECK(ClassifyFailure(Error::kCancelled) == FailureClass::kCancelled);
        CHECK(ClassifyFailure(Error::kUnreachable) == FailureClass::kNetwork);
        CHECK(ClassifyFailure(Error::kTimeout) == FailureClass::kNetwork);
        CHECK(ClassifyFailure(Error::kDisabled) == FailureClass::kNetwork);
        CHECK(ClassifyFailure(Error::kBadKey) == FailureClass::kAuth);
        CHECK(ClassifyFailure(Error::kOk) == FailureClass::kNone);
        CHECK(ClassifyFailure(Error::kBadRequest) == FailureClass::kOther);
    }

    void T_Next() {
        Server s([](const Req&, Conn& c, int) {
            c.Send(Resp(200, "OK", R"({"profile":"skyrim","type":"default","requested_type":"explore","sources":"library",
              "peek":false,"pool_size":42,"newfield":[1,2],
              "tracks":[{"service":"local","id":"87fbeaab3e0ef480","title":"Slow Fog","artist":"Ambient Artist","album":"Drifts",
                "duration_s":247,"quality":{"codec":"FLAC","label":"FLAC 24-bit / 96 kHz Hi-Res"},
                "source":{"service":"local","label":"Library","instance":"x","own":true,"from_library":true},
                "audio_url":"/api/game/audio/local/87fbeaab3e0ef480.wav?rate=48000"},
               {"service":"local","title":"no id so skipped","audio_url":"/x.wav"},
               {"service":"ytmusic","id":"yt1","title":"T2","duration_s":61.7,"quality":null,"source":null,
                "audio_url":"/api/game/audio/ytmusic/yt1.wav"}]})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const NextResult r = cl.Next("skyrim", "explore", "library", 2, false, "wav");
        CHECK_ERR(r, Error::kOk);
        CHECK(r.poolSize == 42 && r.type == "default" && r.requestedType == "explore" && r.sources == "library");
        CHECK(r.tracks.size() == 2);
        if (r.tracks.size() == 2) {
            CHECK(r.tracks[0].id == "87fbeaab3e0ef480" && r.tracks[0].title == "Slow Fog" && r.tracks[0].durationS == 247);
            CHECK(r.tracks[0].qualityLabel.find("Hi-Res") != std::string::npos && r.tracks[0].sourceLabel == "Library" && r.tracks[0].fromLibrary);
            CHECK(r.tracks[1].id == "yt1" && r.tracks[1].durationS == 61 && r.tracks[1].qualityLabel.empty());
        }
        const auto seen = s.Seen();
        CHECK(seen.size() == 1 && seen[0].method == "POST" && seen[0].target == "/api/game/next");
        if (!seen.empty()) {
            const json b = json::parse(seen[0].body, nullptr, false);
            CHECK(b.is_object() && b.value("profile", "") == "skyrim" && b.value("type", "") == "explore");
            CHECK(b.value("sources", "") == "library" && b.value("count", 0) == 2 && b.value("container", "") == "wav");
            CHECK(!b.contains("peek"));
            CHECK(seen[0].headers.at("content-type").find("application/json") == 0);
        }
        const NextResult p = cl.Next("skyrim", "explore", "", 1, true, "");
        CHECK_ERR(p, Error::kOk);
        const json b2 = json::parse(s.Seen().back().body, nullptr, false);
        CHECK(b2.value("peek", false) && !b2.contains("sources") && !b2.contains("container"));
    }

    void T_NextNoTracks404() {
        Server s([](const Req&, Conn& c, int) { c.Send(ErrResp(404, "Not Found", "no_tracks", "nothing playable")); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const NextResult r = cl.Next("skyrim", "explore", "library", 1, false, "");
        CHECK_ERR(r, Error::kNoTracks);
        CHECK(r.httpStatus == 404 && r.code == "no_tracks" && r.message.find("nothing playable") != std::string::npos);
        CHECK(r.tracks.empty());
    }

    void T_ErrorMapping() {
        struct Row { int status; const char* reason; const char* code; const char* extra; Error want; };
        const Row rows[] = {
            { 401, "Unauthorized", "", "", Error::kBadKey },
            { 403, "Forbidden", "disabled", "", Error::kDisabled },
            { 404, "Not Found", "no_tracks", "", Error::kNoTracks },
            { 404, "Not Found", "unknown_type", "", Error::kUnknownType },
            { 404, "Not Found", "unknown_profile", "", Error::kUnknownProfile },
            { 404, "Not Found", "not_found", "", Error::kNotFound },
            { 400, "Bad Request", "bad_request", "", Error::kBadRequest },
            { 413, "Payload Too Large", "too_large", "", Error::kBadRequest },
            { 502, "Bad Gateway", "source_unavailable", "", Error::kSourceUnavailable },
            { 503, "Service Unavailable", "busy", "Retry-After: 3\r\n", Error::kBusy },
            { 503, "Service Unavailable", "cache_busy", "", Error::kBusy },
            { 503, "Service Unavailable", "cache_full", "", Error::kCacheFull },
            { 503, "Service Unavailable", "ffmpeg_missing", "", Error::kFfmpegMissing },
            { 504, "Gateway Timeout", "transcode_timeout", "", Error::kGatewayTimeout },
            { 500, "Internal Server Error", "", "", Error::kServerError },
        };
        std::atomic<int> sel{ 0 };
        Server s([&](const Req&, Conn& c, int) {
            const Row& r = rows[sel.load()];
            c.Send(r.code[0] ? ErrResp(r.status, r.reason, r.code, "boom", r.extra)
                             : Resp(r.status, r.reason, R"({"error":"personal key required"})", r.extra));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        for (int i = 0; i < static_cast<int>(sizeof rows / sizeof rows[0]); ++i) {
            sel = i;
            const StatusResult r = cl.Status();
            if (r.error != rows[i].want) {
                Fail(__LINE__, "row " + std::to_string(i) + " (HTTP " + std::to_string(rows[i].status) + " " + rows[i].code + ")",
                     std::string("got ") + ErrorName(r.error));
            }
            CHECK(r.httpStatus == rows[i].status && r.reachable);
            if (rows[i].code[0]) CHECK(r.code == rows[i].code);
        }
        sel = 9;  // 503 busy with Retry-After
        const StatusResult b = cl.Status();
        CHECK(b.error == Error::kBusy && b.retryAfterS == 3);
        // 200 with garbage => kBadResponse
        s.SetHandler([](const Req&, Conn& c, int) { c.Send(Resp(200, "OK", "<html>not json</html>")); });
        const StatusResult g = cl.Status();
        CHECK_ERR(g, Error::kBadResponse);
        CHECK(g.reachable);
    }

    void T_Unreachable() {
        std::uint16_t dead = 0;
        {
            SOCKET t = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{};
            a.sin_family      = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::bind(t, reinterpret_cast<sockaddr*>(&a), sizeof a);
            int len = sizeof a;
            ::getsockname(t, reinterpret_cast<sockaddr*>(&a), &len);
            dead = ntohs(a.sin_port);
            ::closesocket(t);
        }
        HarmonyClient cl;
        Configured(cl, MakeConfig("http://127.0.0.1:" + std::to_string(dead)));
        const StatusResult r = cl.Status();
        CHECK_ERR(r, Error::kUnreachable);
        CHECK(!r.reachable && r.httpStatus == 0);
    }

    void T_Timeout() {
        Server s([](const Req&, Conn& c, int) { c.Wait(4000); });  // accepts, never answers
        HarmonyClient cl;
        Config cfg = MakeConfig(s.Url());
        cfg.receiveTimeoutMs = 400;
        // Small connect/send too: under Wine the async send task itself waits for the reply and no
        // WinHTTP timeout fires, so the client's send-phase budget (2*500 + 500 + 400 + 500 ms) bounds it.
        cfg.connectTimeoutMs = 500;
        cfg.sendTimeoutMs    = 500;
        Configured(cl, cfg);
        const auto t0 = std::chrono::steady_clock::now();
        const StatusResult r = cl.Status();
        CHECK_ERR(r, Error::kTimeout);
        CHECK(MsSince(t0) < 3500);
        s.Stop();
    }

    void T_PutProfile() {
        Server s([](const Req&, Conn& c, int) { c.Send(Resp(200, "OK", R"({"name":"skyrim"})")); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const std::string body = R"({"types":{"explore":{"genre":"Ambient"}},"normalize":true})";
        const Result r = cl.PutProfile("skyrim", body);
        CHECK_ERR(r, Error::kOk);
        const auto seen = s.Seen();
        CHECK(seen.size() == 1 && seen[0].method == "PUT" && seen[0].target == "/api/game/profiles/skyrim" && seen[0].body == body);
        const Result bad = cl.PutProfile("bad/name", body);
        CHECK_ERR(bad, Error::kBadRequest);
        CHECK(s.Seen().size() == 1);  // rejected locally
        s.SetHandler([](const Req&, Conn& c, int) { c.Send(ErrResp(400, "Bad Request", "invalid_profile", "types: required")); });
        const Result inv = cl.PutProfile("skyrim", "{}");
        CHECK_ERR(inv, Error::kBadRequest);
        CHECK(inv.code == "invalid_profile");
    }

    void T_Search() {
        Server s([](const Req&, Conn& c, int) {
            c.Send(Resp(200, "OK", R"({"tracks":[{"service":"ytmusic","id":"abc","title":"T","artist":"A","album":"B","duration_s":120.5,"extra":1}],
              "artists":[{"service":"local","id":"7","name":"Artist One"}],
              "albums":[{"service":"qobuz","id":12345,"title":"Al","artist":{"name":"X"}}],
              "playlists":[{"service":"ytmusic","id":"PLx","title":"Mix","owner":"o","track_count":25}],"unknown":true})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const SearchResult r = cl.Search("dark ambient&x", "tracks,artists", 5);
        CHECK_ERR(r, Error::kOk);
        CHECK(r.tracks.size() == 1 && r.tracks[0].id == "abc" && r.tracks[0].durationS == 120 && r.tracks[0].album == "B");
        CHECK(r.artists.size() == 1 && r.artists[0].title == "Artist One" && r.artists[0].service == "local");
        CHECK(r.albums.size() == 1 && r.albums[0].id == "12345" && r.albums[0].artist == "X");
        CHECK(r.playlists.size() == 1 && r.playlists[0].id == "PLx" && r.playlists[0].owner == "o" && r.playlists[0].trackCount == 25);
        const auto seen = s.Seen();
        CHECK(seen.size() == 1 && seen[0].target == "/api/search?q=dark%20ambient%26x&kinds=tracks%2Cartists&limit=5");
    }

    // ---- downloads ----
    void T_DownloadWavOk() {
        const fs::path dir  = TestDir();
        const std::string wav = MakeWav(8000, 1, 16, 90.0);
        Server s([&](const Req&, Conn& c, int) { c.Send(AudioHead(wav.size()) + wav); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const fs::path dest = dir / "a.wav";
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dest, 90.0);
        CHECK_ERR(r, Error::kOk);
        CHECK(fs::exists(dest) && fs::file_size(dest) == wav.size());
        CHECK(!fs::exists(dir / "a.wav.part"));
        CHECK(r.audio.format == "wav" && r.audio.sampleRate == 8000 && r.audio.channels == 1 && r.audio.bitsPerSample == 16);
        CHECK(r.audio.durationS > 89.99 && r.audio.durationS < 90.01 && r.bytes == static_cast<std::int64_t>(wav.size()));
        const auto seen = s.Seen();
        CHECK(seen.size() == 1 && seen[0].target == std::string(kAudioUrl) + "&wait=30");
    }

    void T_DownloadReplacesAndPreserves() {
        const fs::path dir = TestDir();
        const std::string good = MakeWav(8000, 1, 16, 90.0);
        const std::string prev = MakeWav(8000, 1, 16, 30.0);
        std::atomic<bool> useGood{ false };
        Server s([&](const Req&, Conn& c, int) {
            const std::string& w = useGood ? good : prev;
            c.Send(AudioHead(w.size()) + w);
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const fs::path dest = dir / "a.wav";
        WriteBytes(dest, "OLD");
        const DownloadResult bad = cl.DownloadAudio(kAudioUrl, dest, 224.0);
        CHECK_ERR(bad, Error::kPreviewOrTruncated);
        CHECK(fs::file_size(dest) == 3);  // existing file untouched on failure
        useGood = true;
        const DownloadResult ok = cl.DownloadAudio(kAudioUrl, dest, 90.0);
        CHECK_ERR(ok, Error::kOk);
        CHECK(fs::file_size(dest) == good.size());  // replaced
        CHECK(CountFiles(dir) == 1);
    }

    void T_Download202Then200() {
        const fs::path dir = TestDir();
        const std::string wav = MakeWav(8000, 2, 16, 70.0);
        Server s([&](const Req&, Conn& c, int idx) {
            if (idx == 0) c.Send(Resp(202, "Accepted", R"({"status":"transcoding","retry_after_s":1})", "Retry-After: 1\r\n"));
            else c.Send(AudioHead(wav.size()) + wav);
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const auto t0 = std::chrono::steady_clock::now();
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dir / "b.wav", 70.0);
        CHECK_ERR(r, Error::kOk);
        CHECK(s.Seen().size() == 2);
        CHECK(MsSince(t0) >= 900);  // honoured Retry-After: 1
        CHECK(fs::exists(dir / "b.wav") && CountFiles(dir) == 1);
        CHECK(r.audio.channels == 2 && r.audio.durationS > 69.9 && r.audio.durationS < 70.1);
    }

    // Deck field log (core-fix-deck): a cold transcode makes the server HOLD the request for its whole `wait=`
    // before answering 202. Reproduces it with a server that sends nothing for `holdS` seconds per request (or until
    // its `wait=` value, whichever is shorter), then 202 + Retry-After: 1, until `readyAfterS` have passed since the
    // first request; then 200 + audio. Production timeouts (Config defaults) apart from waitS.
    void RunColdTranscode(int a_waitS, int a_holdS, int a_readyAfterS, const char* a_tag) {
        const fs::path    dir = TestDir();
        const std::string wav = MakeWav(8000, 2, 16, 70.0);
        const auto        t0  = std::chrono::steady_clock::now();
        Server s(
            [&](const Req& q, Conn& c, int) {
                int        wait = a_holdS;
                const auto w    = q.target.find("wait=");
                if (w != std::string::npos) wait = (std::min)(wait, std::atoi(q.target.c_str() + w + 5));
                const long long left = a_readyAfterS * 1000ll - MsSince(t0);
                if (left <= 0) {
                    c.Send(AudioHead(wav.size()) + wav);
                    return;
                }
                c.Wait(static_cast<int>((std::min)(left, wait * 1000ll)));
                if (a_readyAfterS * 1000ll - MsSince(t0) <= 0) c.Send(AudioHead(wav.size()) + wav);
                else c.Send(Resp(202, "Accepted", R"({"status":"transcoding","retry_after_s":1})", "Retry-After: 1\r\n"));
            },
            true);
        HarmonyClient cl;
        Config        cfg;  // production timeouts
        cfg.baseUrl = s.Url();
        cfg.waitS   = a_waitS;
        Configured(cl, cfg);
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dir / "cold.wav", 70.0);
        if (r.error != Error::kOk) Fail(__LINE__, a_tag, std::string(ErrorName(r.error)) + " " + r.message + " after " + std::to_string(MsSince(t0)) + " ms");
        CHECK(fs::exists(dir / "cold.wav"));
        std::printf("    %s: %zu request(s), %lld ms\n", a_tag, s.Seen().size(), MsSince(t0));
    }

    // The Deck case: one request held 25 s (> the 21 s seen in the field) under the PRODUCTION client settings.
    void T_DownloadColdTranscodeLongHold() { RunColdTranscode(Config{}.waitS, 25, 27, "hold 25 s, production waitS"); }
    // Scaled: the client must keep looping on 202 within its deadline (each hold = waitS = 1 s, ready after 4 s).
    void T_DownloadColdTranscodeLoops() { RunColdTranscode(1, 30, 4, "scaled 202 loop"); }

    // Review R3-1: the pool thread deadlocked (s.mx held across SetConn) on any server address the client rejects, and
    // the game thread then blocked forever in CurrentPlan()/SlotViews(). Drives the real pool workers through an invalid
    // address, a key-only change, an empty address and a closed port, and requires every game-thread call to return.
    void T_PoolSettingsNeverDeadlock() {
        const fs::path dir = TestDir();
        {
            std::ofstream ini(dir / "MJ.ini", std::ios::binary);
            ini << "[General]\nServerUrl=http://[::1]:8080\nPersonalKey=k\n[Env:explore]\nMode=Genre\nSelector=ambient\n";
        }
        // Leaked on purpose: the pool threads are process-lifetime statics and must never outlive the store.
        auto* store = new mj::config::SettingsStore(dir / "MJ.ini", dir / "cache");
        MJ::Pool::Start(store, "MarthJukebox/test");
        MJ::Pool::SetCatalogue({ { "MUSExploreTundra", "Skyrim.esm", false, 10 } });
        auto alive = [](const char* a_what) {
            auto f = std::async(std::launch::async, [] {
                for (int i = 0; i < 20; ++i) {
                    (void)MJ::Pool::CurrentPlan();
                    (void)MJ::Pool::SlotViews();
                    (void)MJ::Pool::Report();
                    (void)MJ::Pool::Connection();
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
            if (f.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
                Fail(__LINE__, a_what, "DEADLOCK: the game-thread calls did not return within 10 s");
                std::printf("mj_net_tests: aborting after a deadlock\n");
                std::fflush(stdout);
                std::quick_exit(3);
            }
        };
        auto waitFor = [](auto a_pred, int a_ms) {
            for (int t = 0; t < a_ms && !a_pred(); t += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return a_pred();
        };
        // 1. An address the client rejects (an IPv6 literal passes Settings but not HarmonyClient::Configure).
        CHECK(waitFor([] { return MJ::Pool::Connection().message.find("not valid") != std::string::npos; }, 8000));
        alive("invalid address");
        // 2. Key-only change.
        store->Update([](mj::config::Settings& a_s) { a_s.global.personalKey = "k2"; });
        MJ::Pool::SettingsChanged();
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        alive("key-only change");
        // 3. Empty address.
        store->Update([](mj::config::Settings& a_s) { a_s.global.serverUrl.clear(); });
        MJ::Pool::SettingsChanged();
        CHECK(waitFor([] { return MJ::Pool::Connection().message.find("Not set up") != std::string::npos; }, 8000));
        alive("empty address");
        // 4. A valid address with nothing listening (connect refused, back-off).
        store->Update([](mj::config::Settings& a_s) { a_s.global.serverUrl = "http://127.0.0.1:1"; });
        MJ::Pool::SettingsChanged();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        alive("closed port");
        // 5. Back to an invalid address once more (the reconfigure branch again).
        store->Update([](mj::config::Settings& a_s) { a_s.global.serverUrl = "http://[::1]:9"; });
        MJ::Pool::SettingsChanged();
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        alive("invalid address again");
    }

    void T_Download202Deadline() {
        const fs::path dir = TestDir();
        Server s([&](const Req&, Conn& c, int) {
            c.Send(Resp(202, "Accepted", R"({"status":"transcoding"})", "Retry-After: 1\r\n"));
        });
        HarmonyClient cl;
        Config cfg = MakeConfig(s.Url());
        cfg.downloadDeadlineS = 3;
        Configured(cl, cfg);
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dir / "c.wav", 100.0);
        CHECK_ERR(r, Error::kDeadline);
        CHECK(CountFiles(dir) == 0);
        CHECK(s.Seen().size() >= 2);
    }

    void T_DownloadTruncated() {
        const fs::path dir = TestDir();
        const std::string wav = MakeWav(8000, 1, 16, 90.0);
        Server s([&](const Req&, Conn& c, int) {
            c.Send(AudioHead(wav.size()) + wav.substr(0, wav.size() / 2));  // then the connection closes
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dir / "t.wav", 90.0);
        CHECK_ERR(r, Error::kTruncated);
        CHECK(CountFiles(dir) == 0);  // neither t.wav nor t.wav.part
    }

    void T_DownloadPreview() {
        const fs::path dir = TestDir();
        const std::string wav = MakeWav(44100, 2, 16, 30.0);  // Next said 224 s; Qobuz served the 30 s preview
        Server s([&](const Req&, Conn& c, int) { c.Send(AudioHead(wav.size()) + wav); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dir / "p.wav", 224.0);
        CHECK_ERR(r, Error::kPreviewOrTruncated);
        CHECK(r.audio.format == "wav" && r.audio.durationS > 29.99 && r.audio.durationS < 30.01);
        CHECK(CountFiles(dir) == 0);
        // The same file is fine for a short expected duration (rule only applies above 60 s).
        const DownloadResult ok = cl.DownloadAudio(kAudioUrl, dir / "p2.wav", 45.0);
        CHECK_ERR(ok, Error::kOk);
        CHECK(fs::exists(dir / "p2.wav"));
    }

    void T_DownloadFlac() {
        const fs::path dir = TestDir();
        const std::string good = MakeFlac(44100, 2, 16, 44100ull * 100);
        const std::string prev = MakeFlac(44100, 2, 16, 44100ull * 30);
        std::atomic<bool> usePrev{ false };
        Server s([&](const Req&, Conn& c, int) {
            const std::string& f = usePrev ? prev : good;
            c.Send(AudioHead(f.size(), "audio/flac") + f);
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const DownloadResult r = cl.DownloadAudio("/api/game/audio/local/x.flac?rate=44100", dir / "f.flac", 100.0);
        CHECK_ERR(r, Error::kOk);
        CHECK(r.audio.format == "flac" && r.audio.sampleRate == 44100 && r.audio.channels == 2 && r.audio.bitsPerSample == 16);
        CHECK(r.audio.totalFrames == 4410000 && r.audio.durationS > 99.99 && r.audio.durationS < 100.01);
        CHECK(fs::exists(dir / "f.flac"));
        usePrev = true;
        const DownloadResult p = cl.DownloadAudio("/api/game/audio/local/x.flac?rate=44100", dir / "g.flac", 224.0);
        CHECK_ERR(p, Error::kPreviewOrTruncated);
        CHECK(!fs::exists(dir / "g.flac") && CountFiles(dir) == 1);
    }

    void T_DownloadBadAudio() {
        const fs::path dir = TestDir();
        const std::string junk(5000, 'x');
        Server s([&](const Req&, Conn& c, int) { c.Send(AudioHead(junk.size()) + junk); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dir / "j.wav", 0.0);
        CHECK_ERR(r, Error::kBadAudio);
        CHECK(CountFiles(dir) == 0);
    }

    void T_Redirect() {
        const fs::path dir = TestDir();
        Server s([](const Req&, Conn& c, int) {
            c.Send(Resp(302, "Found", "", "Location: http://127.0.0.1:1/evil.wav\r\n"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dir / "r.wav", 0.0);
        CHECK_ERR(r, Error::kRedirect);
        CHECK(CountFiles(dir) == 0);
        const StatusResult st = cl.Status();
        CHECK_ERR(st, Error::kRedirect);
        CHECK(s.Seen().size() == 2);  // one request each, never followed
    }

    void RunCancel(bool a_useStopToken) {
        const fs::path dir = TestDir();
        std::atomic<bool> sentFirst{ false };
        Server s([&](const Req&, Conn& c, int) {
            c.Send(AudioHead(20'000'000));
            c.Send(std::string(128 * 1024, '\0'));
            sentFirst = true;
            c.Wait(15000);  // stalls until the server stops or the client goes away
        });
        HarmonyClient cl;
        Config cfg = MakeConfig(s.Url());
        cfg.receiveTimeoutMs = 20000;  // so only the cancel can end the stall in time
        Configured(cl, cfg);
        std::stop_source ss;
        DownloadResult   r;
        std::thread      t([&] { r = cl.DownloadAudio(kAudioUrl, dir / "x.wav", 0.0, a_useStopToken ? ss.get_token() : std::stop_token{}); });
        for (int i = 0; i < 200 && !sentFirst; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(sentFirst.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto t0 = std::chrono::steady_clock::now();
        if (a_useStopToken) ss.request_stop();
        else cl.Cancel();
        t.join();
        CHECK_ERR(r, Error::kCancelled);
        CHECK(MsSince(t0) < 3000);
        CHECK(CountFiles(dir) == 0);
        s.Stop();
    }
    void T_CancelMidDownload() { RunCancel(false); }
    void T_StopTokenMidDownload() { RunCancel(true); }

    void T_CancelFlag() {
        Server s([](const Req&, Conn& c, int) { c.Send(Resp(200, "OK", R"({"enabled":true})")); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        cl.Cancel();
        CHECK_ERR(cl.Status(), Error::kCancelled);
        CHECK(s.Seen().empty());
        cl.ClearCancel();
        CHECK_ERR(cl.Status(), Error::kOk);
        std::stop_source ss;
        ss.request_stop();
        CHECK_ERR(cl.Status(ss.get_token()), Error::kCancelled);
    }

    void T_KeyHeaderAndLogs() {
        const fs::path dir = TestDir();
        const std::string wav = MakeWav(8000, 1, 16, 70.0);
        Server s([&](const Req& r, Conn& c, int) {
            if (r.target.rfind("/api/game/status", 0) == 0) c.Send(Resp(200, "OK", R"({"enabled":true,"ffmpeg":true})"));
            else c.Send(AudioHead(wav.size()) + wav);
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url(), "SECRET-KEY-123"));
        ClearLog();
        CHECK_ERR(cl.Status(), Error::kOk);
        CHECK_ERR(cl.DownloadAudio("/api/game/audio/local/k.wav?rate=48000&key=SECRET-KEY-123", dir / "k.wav", 70.0), Error::kOk);
        const auto seen = s.Seen();
        CHECK(seen.size() == 2);
        for (const auto& r : seen) CHECK(r.headers.count("x-harmony-key") && r.headers.at("x-harmony-key") == "SECRET-KEY-123");
        // The query parameter was sent as given (we never invent it) ...
        CHECK(seen.size() == 2 && seen[1].target.find("key=SECRET-KEY-123") != std::string::npos);
        // ... but neither the secret nor the header name's value ever reach the log.
        const std::string log = LogText();
        CHECK(log.find("SECRET-KEY-123") == std::string::npos);
        CHECK(log.find("key=***") != std::string::npos);
        CHECK(log.find("[net] GET /api/game/status -> 200 (") != std::string::npos);
        // A rejected key is also not echoed.
        s.SetHandler([](const Req&, Conn& c, int) { c.Send(Resp(401, "Unauthorized", R"({"error":"personal key required"})")); });
        CHECK_ERR(cl.Status(), Error::kBadKey);
        CHECK(LogText().find("SECRET-KEY-123") == std::string::npos);
    }

    void T_UrlResolution() {
        const fs::path dir = TestDir();
        const std::string wav = MakeWav(8000, 1, 16, 70.0);
        Server s([&](const Req&, Conn& c, int) { c.Send(AudioHead(wav.size()) + wav); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const std::string port = std::to_string(s.Port());
        CHECK_ERR(cl.DownloadAudio("/api/game/audio/local/a.wav?rate=48000", dir / "1.wav", 70.0), Error::kOk);
        CHECK_ERR(cl.DownloadAudio("http://127.0.0.1:" + port + "/api/game/audio/local/b.wav", dir / "2.wav", 70.0), Error::kOk);
        CHECK_ERR(cl.DownloadAudio("api/game/audio/local/c.wav?wait=5", dir / "3.wav", 70.0), Error::kOk);
        auto seen = s.Seen();
        CHECK(seen.size() == 3);
        if (seen.size() == 3) {
            CHECK(seen[0].target == "/api/game/audio/local/a.wav?rate=48000&wait=30");
            CHECK(seen[1].target == "/api/game/audio/local/b.wav?wait=30");
            CHECK(seen[2].target == "/api/game/audio/local/c.wav?wait=5");  // caller's wait= kept
        }
        // Absolute URL on another host (the key must never go there) or scheme: rejected, no request made.
        CHECK_ERR(cl.DownloadAudio("http://127.0.0.2:" + port + "/x.wav", dir / "4.wav", 0.0), Error::kBadResponse);
        CHECK_ERR(cl.DownloadAudio("https://127.0.0.1:" + port + "/x.wav", dir / "5.wav", 0.0), Error::kBadResponse);
        CHECK_ERR(cl.DownloadAudio("//127.0.0.1/x.wav", dir / "6.wav", 0.0), Error::kBadResponse);
        CHECK_ERR(cl.DownloadAudio("", dir / "7.wav", 0.0), Error::kBadResponse);
        CHECK(s.Seen().size() == 3);

        // A path prefix is prepended to every request, including root-relative audio URLs.
        HarmonyClient cp;
        Configured(cp, MakeConfig(s.Url("/hm/")));
        CHECK_ERR(cp.DownloadAudio("/api/game/audio/local/d.wav", dir / "8.wav", 70.0), Error::kOk);
        s.SetHandler([](const Req&, Conn& c, int) { c.Send(Resp(200, "OK", R"({"enabled":true})")); });
        CHECK_ERR(cp.Status(), Error::kOk);
        seen = s.Seen();
        CHECK(seen.size() == 5);
        if (seen.size() == 5) {
            CHECK(seen[3].target == "/hm/api/game/audio/local/d.wav?wait=30");
            CHECK(seen[4].target == "/hm/api/game/status");
        }
    }

    void T_ConfigValidation() {
        HarmonyClient cl;
        const char* bad[] = { "", "ftp://host", "127.0.0.1:8080", "http://", "http:///x", "http://u:p@host", "http://host:0",
                              "http://host:99999", "http://host:abc", "http://[::1]:80", "http://host/?q=1", "http://ho st",
                              "file:///c:/x" };
        for (const char* u : bad) {
            const Result r = cl.Configure(MakeConfig(u));
            if (r.error != Error::kBadConfig) Fail(__LINE__, std::string("expected rejection of '") + u + "'");
        }
        const char* good[] = { "http://host", "HTTP://Host:8080/", "https://host:8443/prefix/", "http://192.168.1.5:8080" };
        for (const char* u : good) {
            const Result r = cl.Configure(MakeConfig(u));
            if (!r.Ok()) Fail(__LINE__, std::string("expected acceptance of '") + u + "'", r.message);
        }
        CHECK_ERR(cl.Configure(MakeConfig("http://host", "bad\r\nX-Evil: 1")), Error::kBadConfig);
        CHECK_ERR(cl.Configure(MakeConfig("http://host", "has space")), Error::kBadConfig);
        Config c = MakeConfig("http://host");
        c.receiveTimeoutMs = 0;
        CHECK_ERR(cl.Configure(c), Error::kBadConfig);
        c = MakeConfig("http://host");
        c.maxDownloadBytes = 0;
        CHECK_ERR(cl.Configure(c), Error::kBadConfig);
        // An unconfigured client fails cleanly instead of crashing.
        HarmonyClient fresh;
        CHECK_ERR(fresh.Status(), Error::kBadConfig);
        CHECK_ERR(fresh.DownloadAudio("/x.wav", g_root / "never.wav", 0.0), Error::kBadConfig);
    }

    void T_Probe() {
        const fs::path dir = TestDir();
        AudioInfo a;
        // WAV with an extra LIST chunk before data: chunks are walked.
        WriteBytes(dir / "list.wav", MakeWav(48000, 2, 24, 3.0, true));
        CHECK_ERR(ProbeAudioFile(dir / "list.wav", a), Error::kOk);
        CHECK(a.format == "wav" && a.sampleRate == 48000 && a.channels == 2 && a.bitsPerSample == 24 && a.durationS > 2.99 && a.durationS < 3.01);
        // RIFF size disagrees with the file size (short file).
        std::string w = MakeWav(8000, 1, 16, 5.0);
        WriteBytes(dir / "short.wav", w.substr(0, w.size() - 10));
        CHECK_ERR(ProbeAudioFile(dir / "short.wav", a), Error::kBadAudio);
        // Trailing junk after a consistent file also disagrees.
        WriteBytes(dir / "long.wav", w + "junk");
        CHECK_ERR(ProbeAudioFile(dir / "long.wav", a), Error::kBadAudio);
        // data size not a whole number of frames (stereo 16-bit with 2 extra bytes).
        std::string odd = MakeWav(8000, 2, 16, 1.0);
        odd.append(2, '\0');
        odd[4] = static_cast<char>(odd[4] + 2);
        odd[40] = static_cast<char>(odd[40] + 2);
        WriteBytes(dir / "odd.wav", odd);
        CHECK_ERR(ProbeAudioFile(dir / "odd.wav", a), Error::kBadAudio);
        WriteBytes(dir / "junk.bin", std::string(100, 'z'));
        CHECK_ERR(ProbeAudioFile(dir / "junk.bin", a), Error::kBadAudio);
        WriteBytes(dir / "tiny.bin", "RI");
        CHECK_ERR(ProbeAudioFile(dir / "tiny.bin", a), Error::kBadAudio);
        CHECK_ERR(ProbeAudioFile(dir / "missing.wav", a), Error::kIo);
        // FLAC: STREAMINFO read; unknown total sample count cannot be validated => rejected.
        WriteBytes(dir / "ok.flac", MakeFlac(48000, 1, 24, 48000ull * 10));
        CHECK_ERR(ProbeAudioFile(dir / "ok.flac", a), Error::kOk);
        CHECK(a.format == "flac" && a.sampleRate == 48000 && a.channels == 1 && a.bitsPerSample == 24 && a.durationS > 9.99 && a.durationS < 10.01);
        WriteBytes(dir / "nototal.flac", MakeFlac(48000, 1, 24, 0));
        CHECK_ERR(ProbeAudioFile(dir / "nototal.flac", a), Error::kBadAudio);
    }

    // ---- review fixes (net-client-fix) ----

    // Every 3xx to ANOTHER server (different port on loopback): never followed, so the key header
    // never reaches it, for GET (status, audio) and POST (next, 307/308 would re-send the body).
    void T_RedirectNeverLeaksKey() {
        const fs::path dir = TestDir();
        Server victim([](const Req&, Conn& c, int) { c.Send(Resp(200, "OK", R"({"enabled":true})")); });
        for (const int code : { 301, 302, 303, 307, 308 }) {
            Server s([&](const Req& r, Conn& c, int) {
                c.Send(Resp(code, "Redirect", "", "Location: " + victim.Url(r.target) + "\r\n"));
            });
            HarmonyClient cl;
            Configured(cl, MakeConfig(s.Url(), "SECRET-KEY-123"));
            CHECK_ERR(cl.Status(), Error::kRedirect);
            CHECK_ERR(cl.Next("skyrim", "explore", "", 1, false, ""), Error::kRedirect);
            CHECK_ERR(cl.DownloadAudio(kAudioUrl, dir / "r.wav", 0.0), Error::kRedirect);
            CHECK_ERR(cl.ReportPlaying("skyrim", "explore", "local", "abc"), Error::kRedirect);
            const auto seen = s.Seen();
            CHECK(seen.size() == 4);
            for (const auto& r : seen) CHECK(r.headers.count("x-harmony-key") == 1);  // the key went to the configured server only
            if (!victim.Seen().empty()) Fail(__LINE__, "redirect target was contacted", "HTTP " + std::to_string(code));
        }
        CHECK(victim.Seen().empty());
        CHECK(CountFiles(dir) == 0);
    }

    // Non-UTF-8 strings in JSON request fields are rejected locally (no throw, no request).
    void T_NonUtf8Rejected() {
        Server s([](const Req& r, Conn& c, int) {
            if (r.target == "/api/game/playing") c.Send(Resp(200, "OK", R"({"ok":true})"));
            else c.Send(Resp(200, "OK", R"({"type":"default","pool_size":1,"tracks":[]})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        CHECK_ERR(cl.Next("\xff\xfe", "explore", "", 1, false, ""), Error::kBadRequest);
        CHECK_ERR(cl.Next("skyrim", "expl\xc3", "", 1, false, ""), Error::kBadRequest);
        CHECK_ERR(cl.Next("skyrim", "explore", "lib\x80rary", 1, false, ""), Error::kBadRequest);
        CHECK_ERR(cl.Next("skyrim", "explore", "", 1, false, "wav\xed\xa0\x80"), Error::kBadRequest);  // UTF-16 surrogate
        CHECK_ERR(cl.ReportPlaying("skyrim", "explore", "local", "id\xff"), Error::kBadRequest);
        CHECK_ERR(cl.ReportPlaying("sky\xc0\xafrim", "explore", "local", "id"), Error::kBadRequest);  // overlong '/'
        CHECK(s.Seen().empty());
        // Valid non-ASCII UTF-8 goes through unchanged.
        const std::string type = "\xc3\x9cnic\xc3\xb8" "de";
        CHECK_ERR(cl.Next("skyrim", type, "", 1, false, ""), Error::kOk);
        const auto seen = s.Seen();
        CHECK(seen.size() == 1);
        if (seen.size() == 1) {
            const json b = json::parse(seen[0].body, nullptr, false);
            CHECK(b.is_object() && b.value("type", "") == type);
        }
    }

    // Size bound: Content-Length above maxDownloadBytes fails BEFORE the .part exists.
    void T_DownloadTooLarge() {
        const fs::path dir = TestDir();
        const std::string wav = MakeWav(8000, 1, 16, 1.0);  // 16 044 bytes
        Server s([&](const Req&, Conn& c, int) { c.Send(AudioHead(wav.size()) + wav); });
        HarmonyClient cl;
        Config cfg = MakeConfig(s.Url());
        cfg.maxDownloadBytes = 8192;
        Configured(cl, cfg);
        const DownloadResult r = cl.DownloadAudio(kAudioUrl, dir / "big.wav", 0.0);
        CHECK_ERR(r, Error::kTooLarge);
        CHECK(r.bytes == 0 && CountFiles(dir) == 0);
        cfg.maxDownloadBytes = 16044;  // exactly the size: allowed
        Configured(cl, cfg);
        CHECK_ERR(cl.DownloadAudio(kAudioUrl, dir / "ok.wav", 0.0), Error::kOk);
        CHECK(CountFiles(dir) == 1);

        // Default limit (256 MiB): a 300 MiB Content-Length is refused from the header alone; the
        // server sends no body at all, so anything but kTooLarge (e.g. kTruncated) means the body was read.
        const fs::path dir2 = dir / "default";
        fs::create_directories(dir2);
        s.SetHandler([](const Req&, Conn& c, int) { c.Send(AudioHead(300ull * 1024 * 1024)); });
        HarmonyClient cd;
        Configured(cd, MakeConfig(s.Url()));
        CHECK_ERR(cd.DownloadAudio(kAudioUrl, dir2 / "huge.wav", 0.0), Error::kTooLarge);
        CHECK(CountFiles(dir2) == 0);
    }

    // Bytes beyond Content-Length are never written. (a) plain trailing junk after the declared
    // body; (b) a chunked body longer than a contradicting Content-Length.
    void T_DownloadBodyLongerThanContentLength() {
        const fs::path dir = TestDir();
        const std::string wav = MakeWav(8000, 1, 16, 2.0);
        Server s([&](const Req&, Conn& c, int) { c.Send(AudioHead(wav.size()) + wav + std::string(200000, 'J')); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const DownloadResult a = cl.DownloadAudio(kAudioUrl, dir / "a.wav", 0.0);
        CHECK_ERR(a, Error::kOk);
        CHECK(fs::exists(dir / "a.wav") && fs::file_size(dir / "a.wav") == wav.size());

        s.SetHandler([&](const Req&, Conn& c, int) {
            std::string body = wav + std::string(100000, 'J');
            std::string out  = "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " + std::to_string(wav.size()) +
                              "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
            for (std::size_t off = 0; off < body.size(); off += 30000) {
                const std::size_t n = (std::min<std::size_t>)(30000, body.size() - off);
                char hex[32];
                std::snprintf(hex, sizeof hex, "%zx\r\n", n);
                out += hex;
                out += body.substr(off, n);
                out += "\r\n";
            }
            out += "0\r\n\r\n";
            c.Send(out);
        });
        const DownloadResult b = cl.DownloadAudio(kAudioUrl, dir / "b.wav", 0.0);
        // Whichever framing WinHTTP picks, no file larger than Content-Length may appear.
        if (b.Ok()) CHECK(fs::file_size(dir / "b.wav") == wav.size());
        else CHECK(!fs::exists(dir / "b.wav"));
        CHECK(!fs::exists(dir / "b.wav.part") && CountFiles(dir) <= 2);
        std::printf("    (chunked + contradicting Content-Length -> %s)\n", ErrorName(b.error));
    }

    void T_ReportPlaying() {
        Server s([](const Req&, Conn& c, int) {
            c.Send(Resp(200, "OK", R"({"ok":true,"now_playing":{"profile":"skyrim","type":"explore","service":"local","id":"87fbeaab3e0ef480","title":null,"age_s":0.0}})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url(), "SECRET-KEY-123"));
        CHECK_ERR(cl.ReportPlaying("skyrim", "explore", "local", "87fbeaab3e0ef480"), Error::kOk);
        auto seen = s.Seen();
        CHECK(seen.size() == 1);
        if (seen.size() == 1) {
            CHECK(seen[0].method == "POST" && seen[0].target == "/api/game/playing");
            CHECK(seen[0].headers.count("x-harmony-key") == 1 && seen[0].headers.at("content-type").find("application/json") == 0);
            const json b = json::parse(seen[0].body, nullptr, false);
            CHECK(b.is_object() && b.size() == 4);  // the server rejects unknown keys
            CHECK(b.value("profile", "") == "skyrim" && b.value("type", "") == "explore" && b.value("service", "") == "local" &&
                  b.value("id", "") == "87fbeaab3e0ef480");
        }
        // Missing fields are refused locally.
        CHECK_ERR(cl.ReportPlaying("skyrim", "explore", "local", ""), Error::kBadRequest);
        CHECK_ERR(cl.ReportPlaying("", "explore", "local", "x"), Error::kBadRequest);
        CHECK(s.Seen().size() == 1);
        // Server-side refusal is a returned error with the code.
        s.SetHandler([](const Req&, Conn& c, int) { c.Send(ErrResp(400, "Bad Request", "bad_request", "unknown key(s): x")); });
        const Result bad = cl.ReportPlaying("skyrim", "explore", "local", "x");
        CHECK_ERR(bad, Error::kBadRequest);
        CHECK(bad.code == "bad_request");
        s.SetHandler([](const Req&, Conn& c, int) { c.Send(ErrResp(403, "Forbidden", "disabled", "game source disabled")); });
        CHECK_ERR(cl.ReportPlaying("skyrim", "explore", "local", "x"), Error::kDisabled);
        // Harmony away: an error, not a throw.
        s.Stop();
        const Result away = cl.ReportPlaying("skyrim", "explore", "local", "x");
        CHECK(away.error == Error::kUnreachable || away.error == Error::kTimeout);
    }

    // Cancel while the server holds the request in its wait= phase (no status line yet: the client
    // is inside WinHttpReceiveResponse). Both Cancel() and the stop token; the client must stay usable.
    void RunCancelInWaitPhase(bool a_useStopToken) {
        const fs::path dir = TestDir();
        std::atomic<bool> gotRequest{ false };
        Server s(
            [&](const Req& r, Conn& c, int) {
                if (r.target == "/api/game/status") {
                    c.Send(Resp(200, "OK", R"({"enabled":true})"));
                    return;
                }
                gotRequest = true;
                c.Wait(20000);  // a server blocking in wait=: sends nothing
            },
            true);
        HarmonyClient cl;
        Config cfg = MakeConfig(s.Url());
        cfg.receiveTimeoutMs = 20000;
        cfg.waitS            = 30;  // => receive timeout 40 s: only the cancel can end this in time
        Configured(cl, cfg);
        std::stop_source ss;
        DownloadResult   r;
        std::thread      t([&] { r = cl.DownloadAudio(kAudioUrl, dir / "w.wav", 0.0, a_useStopToken ? ss.get_token() : std::stop_token{}); });
        for (int i = 0; i < 200 && !gotRequest; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(gotRequest.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto t0 = std::chrono::steady_clock::now();
        if (a_useStopToken) ss.request_stop();
        else cl.Cancel();
        const long long signalMs = MsSince(t0);
        t.join();
        const long long ms = MsSince(t0);
        CHECK_ERR(r, Error::kCancelled);
        CHECK(signalMs < 200);  // signalling only: never blocks the caller
        if (ms >= 1500) Fail(__LINE__, "cancel latency", std::to_string(ms) + " ms");
        CHECK(CountFiles(dir) == 0);
        // Reusable afterwards (the abandoned request must not poison the next one).
        cl.ClearCancel();
        CHECK_ERR(cl.Status(), Error::kOk);
        s.Stop();
    }
    void T_CancelInWaitPhase() { RunCancelInWaitPhase(false); }
    void T_StopTokenInWaitPhase() { RunCancelInWaitPhase(true); }

    // Cancel racing the start of a request: random delays from 0 to ~2 ms around the call start.
    // Every outcome must be kOk or kCancelled, promptly, with no crash and no hang.
    void T_CancelRacingStart() {
        Server s([](const Req&, Conn& c, int) { c.Send(Resp(200, "OK", R"({"enabled":true,"cache":{"files":7}})")); }, true);
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        std::uint32_t rng = 12345;
        int ok = 0, cancelled = 0;
        for (int i = 0; i < 300; ++i) {
            rng = rng * 1664525u + 1013904223u;
            const int  delayUs = static_cast<int>((rng >> 8) % 2000);
            const bool useStop = (i & 1) != 0;
            cl.ClearCancel();
            std::stop_source ss;
            StatusResult     r;
            std::thread      t([&] { r = cl.Status(useStop ? ss.get_token() : std::stop_token{}); });
            SpinUs(delayUs);
            const auto t0 = std::chrono::steady_clock::now();
            if (useStop) ss.request_stop();
            else cl.Cancel();
            t.join();
            const long long ms = MsSince(t0);
            if (r.error == Error::kOk) {
                ++ok;
                if (r.cache.files != 7) Fail(__LINE__, "wrong body after a race", std::to_string(i));
            } else if (r.error == Error::kCancelled) {
                ++cancelled;
            } else {
                Fail(__LINE__, "iteration " + std::to_string(i), std::string("got ") + ErrorName(r.error) + " (" + r.message + ")");
            }
            if (ms > 3000) Fail(__LINE__, "slow cancel in iteration " + std::to_string(i), std::to_string(ms) + " ms");
            if (g_curFailed) break;
        }
        std::printf("    (%d ok, %d cancelled)\n", ok, cancelled);
        CHECK(cancelled > 0);  // the window around the call start was actually hit
        cl.ClearCancel();
        CHECK_ERR(cl.Status(), Error::kOk);
    }

    // Two clients, each with its own server, cancel/start cycles in parallel (status and downloads
    // so cancels land in connect, wait, headers and mid-body). Any result must be kOk/kCancelled and
    // carry ITS server's data: catches handle reuse / cross-talk between clients.
    void T_CancelStressTwoClients() {
        const fs::path dir = TestDir();
        struct Side {
            int                     id;
            std::string             wav;
            std::unique_ptr<Server> srv;
            HarmonyClient           cl;
            fs::path                dir;
            int                     ok = 0, cancelled = 0;
            std::vector<std::string> errors;
        };
        Side sides[2];
        for (int k = 0; k < 2; ++k) {
            Side& sd = sides[k];
            sd.id    = k + 1;
            sd.wav   = MakeWav(8000, 2, 16, 4.0 + k);  // 128 KB / 160 KB: more than one read chunk
            sd.dir   = dir / std::to_string(sd.id);
            fs::create_directories(sd.dir);
            sd.srv = std::make_unique<Server>(
                [&sd](const Req& r, Conn& c, int) {
                    if (r.target == "/api/game/status") {
                        c.Send(Resp(200, "OK", "{\"enabled\":true,\"cache\":{\"files\":" + std::to_string(sd.id) + "}}"));
                        return;
                    }
                    // Head, a slice, a short stall, the rest: gives cancels a mid-body window.
                    c.Send(AudioHead(sd.wav.size()) + sd.wav.substr(0, 70000));
                    if (!c.Wait(2)) return;
                    c.Send(sd.wav.substr(70000));
                },
                true);
            Configured(sd.cl, MakeConfig(sd.srv->Url()));
        }
        auto drive = [](Side& sd, std::uint32_t seed) {
            std::uint32_t rng = seed;
            for (int i = 0; i < 150; ++i) {
                rng = rng * 1664525u + 1013904223u;
                const int  delayUs  = static_cast<int>((rng >> 8) % 4000);
                const bool useStop  = ((rng >> 4) & 1) != 0;
                const bool download = ((rng >> 5) & 1) != 0;
                sd.cl.ClearCancel();
                std::stop_source ss;
                Error            err = Error::kOk;
                std::string      msg;
                bool             dataOk = true;
                const fs::path   dest   = sd.dir / (std::to_string(i) + ".wav");
                std::thread      t([&] {
                    const std::stop_token tok = useStop ? ss.get_token() : std::stop_token{};
                    if (download) {
                        const DownloadResult d = sd.cl.DownloadAudio(kAudioUrl, dest, 0.0, tok);
                        err = d.error;
                        msg = d.message;
                        if (d.Ok()) dataOk = fs::file_size(dest) == sd.wav.size();
                    } else {
                        const StatusResult st = sd.cl.Status(tok);
                        err = st.error;
                        msg = st.message;
                        if (st.Ok()) dataOk = st.cache.files == sd.id;
                    }
                });
                SpinUs(delayUs);
                const auto t0 = std::chrono::steady_clock::now();
                if (useStop) ss.request_stop();
                else sd.cl.Cancel();
                t.join();
                const long long ms = MsSince(t0);
                if (err == Error::kOk) {
                    ++sd.ok;
                    if (!dataOk) sd.errors.push_back("iteration " + std::to_string(i) + ": OK result carries the other side's data");
                } else if (err == Error::kCancelled) {
                    ++sd.cancelled;
                } else {
                    sd.errors.push_back("iteration " + std::to_string(i) + ": " + ErrorName(err) + " (" + msg + ")");
                }
                if (ms > 3000) sd.errors.push_back("iteration " + std::to_string(i) + ": cancel took " + std::to_string(ms) + " ms");
            }
        };
        std::thread a([&] { drive(sides[0], 1u); });
        std::thread b([&] { drive(sides[1], 99u); });
        a.join();
        b.join();
        for (Side& sd : sides) {
            std::printf("    (client %d: %d ok, %d cancelled)\n", sd.id, sd.ok, sd.cancelled);
            for (std::size_t e = 0; e < sd.errors.size() && e < 10; ++e) Fail(__LINE__, "client " + std::to_string(sd.id), sd.errors[e]);
            std::error_code ec;
            for (auto it = fs::directory_iterator(sd.dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (it->path().extension() == ".part") Fail(__LINE__, "leftover .part", it->path().filename().string());
                else if (fs::file_size(it->path()) != sd.wav.size()) Fail(__LINE__, "wrong final file", it->path().filename().string());
            }
            // Still healthy after all of it.
            sd.cl.ClearCancel();
            const StatusResult st = sd.cl.Status();
            CHECK_ERR(st, Error::kOk);
            CHECK(st.cache.files == sd.id);
        }
        for (Side& sd : sides) sd.srv->Stop();
    }

    // Cancel()/ClearCancel() racing from two threads (released together by a spin barrier), then a
    // Status(): a kCancelled result must mean NOTHING was sent (pre-check), and an OK result must have
    // been sent exactly once. With a flag + event that could disagree, "sent, then cancelled" appeared.
    void T_CancelClearInterleave() {
        Server s([](const Req&, Conn& c, int) { c.Send(Resp(200, "OK", R"({"enabled":true})")); });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        std::atomic<int>  phase{ 0 };  // round number both racers wait for
        std::atomic<int>  done{ 0 };
        std::atomic<bool> quit{ false };
        auto racer = [&](bool a_cancel) {
            for (int round = 1;; ++round) {
                while (phase.load() < round) {
                    if (quit.load()) return;
                }
                if (a_cancel) cl.Cancel();
                else cl.ClearCancel();
                done.fetch_add(1);
            }
        };
        std::thread ra(racer, true);
        std::thread rb(racer, false);
        int cancelled = 0, ok = 0;
        for (int round = 1; round <= 2000 && !g_curFailed; ++round) {
            done  = 0;
            phase = round;
            while (done.load() < 2) {}
            const std::size_t  before = s.Seen().size();
            const StatusResult r      = cl.Status();
            const std::size_t  sent   = s.Seen().size() - before;
            if (r.error == Error::kCancelled) {
                ++cancelled;
                if (sent != 0) Fail(__LINE__, "round " + std::to_string(round), "cancelled after the request was sent");
            } else if (r.error == Error::kOk) {
                ++ok;
                if (sent != 1) Fail(__LINE__, "round " + std::to_string(round), "OK but sent " + std::to_string(sent));
            } else {
                Fail(__LINE__, "round " + std::to_string(round), std::string("got ") + ErrorName(r.error) + " (" + r.message + ")");
            }
        }
        quit = true;
        ra.join();
        rb.join();
        std::printf("    (%d ok, %d cancelled)\n", ok, cancelled);
        CHECK(ok > 0 && cancelled > 0);  // both orders were actually hit
        cl.ClearCancel();
        CHECK_ERR(cl.Status(), Error::kOk);
        cl.Cancel();
        const std::size_t before = s.Seen().size();
        CHECK_ERR(cl.Status(), Error::kCancelled);
        CHECK(s.Seen().size() == before);
    }

    struct TestEntry { const char* name; void (*fn)(); };

}  // namespace

int main() {
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup failed\n");
        return 2;
    }
    {
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log, true);
        auto lg   = std::make_shared<spdlog::logger>("mjtest", sink);
        lg->set_level(spdlog::level::info);
        spdlog::set_default_logger(lg);
    }
    g_root = fs::temp_directory_path() / ("mj_net_tests_" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    const TestEntry tests[] = {
        { "status_json", T_Status },
        { "status_no_containers_field", T_StatusNoContainers },
        { "next_json", T_Next },
        { "next_404_no_tracks", T_NextNoTracks404 },
        { "http_error_mapping", T_ErrorMapping },
        { "unreachable", T_Unreachable },
        { "receive_timeout", T_Timeout },
        { "put_profile", T_PutProfile },
        { "search", T_Search },
        { "status_services", T_StatusServices },
        { "browse", T_Browse },
        { "download_cold_transcode_loops", T_DownloadColdTranscodeLoops },
        { "download_cold_transcode_long_hold", T_DownloadColdTranscodeLongHold },
        { "classify_failure", T_ClassifyFailure },
        { "next_old_server_rejects_unknown_keys", T_NextOldServerRejectsUnknownKeys },
        { "download_wav_ok", T_DownloadWavOk },
        { "download_replace_and_preserve", T_DownloadReplacesAndPreserves },
        { "download_202_then_200", T_Download202Then200 },
        { "download_202_deadline", T_Download202Deadline },
        { "download_truncated_body", T_DownloadTruncated },
        { "download_preview_30s_vs_224s", T_DownloadPreview },
        { "download_flac_streaminfo", T_DownloadFlac },
        { "download_bad_audio", T_DownloadBadAudio },
        { "redirect_rejected", T_Redirect },
        { "cancel_mid_download", T_CancelMidDownload },
        { "stop_token_mid_download", T_StopTokenMidDownload },
        { "cancel_flag_sticky_and_clear", T_CancelFlag },
        { "key_header_sent_never_logged", T_KeyHeaderAndLogs },
        { "audio_url_resolution", T_UrlResolution },
        { "config_validation", T_ConfigValidation },
        { "probe_audio_headers", T_Probe },
        { "redirect_never_leaks_key", T_RedirectNeverLeaksKey },
        { "non_utf8_fields_rejected", T_NonUtf8Rejected },
        { "download_too_large", T_DownloadTooLarge },
        { "download_body_longer_than_content_length", T_DownloadBodyLongerThanContentLength },
        { "report_playing", T_ReportPlaying },
        { "cancel_in_wait_phase", T_CancelInWaitPhase },
        { "stop_token_in_wait_phase", T_StopTokenInWaitPhase },
        { "cancel_racing_start", T_CancelRacingStart },
        { "cancel_stress_two_clients", T_CancelStressTwoClients },
        { "cancel_clear_interleave", T_CancelClearInterleave },
        // LAST: it starts the process-lifetime pool threads (they end in an invalid-address state that sends nothing).
        { "pool_settings_never_deadlock", T_PoolSettingsNeverDeadlock },
    };
    for (const auto& t : tests) {
        g_curName   = t.name;
        g_curFailed = false;
        ClearLog();
        try {
            t.fn();
        } catch (const std::exception& e) {
            Fail(0, std::string("exception: ") + e.what());
        } catch (...) {
            Fail(0, "unknown exception");
        }
        // Every request context must be freed once WinHTTP delivered HANDLE_CLOSING (async; allow 10 s,
        // e.g. Wine finishing an abandoned send task after the test's server closed).
        {
            const auto t0 = std::chrono::steady_clock::now();
            while (MJ::Net::LiveRequestContextsForTests() != 0 && MsSince(t0) < 10000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const long live = MJ::Net::LiveRequestContextsForTests();
            if (live != 0) Fail(0, "request contexts still alive after the test", std::to_string(live));
        }
        std::printf("[ %s ] %s\n", g_curFailed ? "FAIL" : "PASS", t.name);
        std::fflush(stdout);
        (g_curFailed ? g_fail : g_pass)++;
    }
    std::printf("mj_net_tests: %d passed, %d failed\n", g_pass, g_fail);
    fs::remove_all(g_root, ec);
    ::WSACleanup();
    return g_fail == 0 ? 0 : 1;
}
