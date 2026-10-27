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
    // scripted HTTP/1.1 server (one connection at a time, "Connection: close")
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

        explicit Server(Handler a_h) : handler_(std::move(a_h)) {
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
                Serve(c);
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
              "playlists":[],"unknown":true})"));
        });
        HarmonyClient cl;
        Configured(cl, MakeConfig(s.Url()));
        const SearchResult r = cl.Search("dark ambient&x", "tracks,artists", 5);
        CHECK_ERR(r, Error::kOk);
        CHECK(r.tracks.size() == 1 && r.tracks[0].id == "abc" && r.tracks[0].durationS == 120 && r.tracks[0].album == "B");
        CHECK(r.artists.size() == 1 && r.artists[0].title == "Artist One" && r.artists[0].service == "local");
        CHECK(r.albums.size() == 1 && r.albums[0].id == "12345" && r.albums[0].artist == "X");
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
        std::printf("[ %s ] %s\n", g_curFailed ? "FAIL" : "PASS", t.name);
        std::fflush(stdout);
        (g_curFailed ? g_fail : g_pass)++;
    }
    std::printf("mj_net_tests: %d passed, %d failed\n", g_pass, g_fail);
    fs::remove_all(g_root, ec);
    ::WSACleanup();
    return g_fail == 0 ? 0 : 1;
}
