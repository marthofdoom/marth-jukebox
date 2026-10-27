// Harmony HTTP client. See HarmonyClient.h for the threading rule: BLOCKING, worker threads only.
#include "PCH.h"
#include "net/HarmonyClient.h"

// After the CommonLib PCH (house style, see SlotWriter.cpp); WIN32_LEAN_AND_MEAN and NOMINMAX
// come from CMakePresets.json.
#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace MJ::Net {

    namespace fs = std::filesystem;
    using json   = nlohmann::json;

    namespace {

        constexpr std::size_t kChunk       = 64 * 1024;
        constexpr std::size_t kMaxJsonBody = 4 * 1024 * 1024;

        std::string ToLower(std::string a_s) {
            for (auto& c : a_s) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return a_s;
        }

        bool IEquals(std::string_view a, std::string_view b) {
            return a.size() == b.size() && ToLower(std::string(a)) == ToLower(std::string(b));
        }

        std::wstring Widen(std::string_view a_s) {
            if (a_s.empty()) return {};
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, a_s.data(), static_cast<int>(a_s.size()), nullptr, 0);
            if (n <= 0) return {};
            std::wstring w(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, a_s.data(), static_cast<int>(a_s.size()), w.data(), n);
            return w;
        }

        std::string UrlEncode(std::string_view a_s) {
            static const char* hex = "0123456789ABCDEF";
            std::string        out;
            for (const unsigned char c : a_s) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~') {
                    out.push_back(static_cast<char>(c));
                } else {
                    out.push_back('%');
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                }
            }
            return out;
        }

        // Replaces the value of any "key=" query parameter with "***" (log hygiene only).
        std::string Redact(std::string_view a_target) {
            const auto q = a_target.find('?');
            if (q == std::string_view::npos) return std::string(a_target);
            std::string out(a_target.substr(0, q + 1));
            std::string_view rest = a_target.substr(q + 1);
            bool first = true;
            while (!rest.empty()) {
                const auto amp = rest.find('&');
                std::string_view part = rest.substr(0, amp);
                if (!first) out.push_back('&');
                first = false;
                if (part.size() >= 4 && IEquals(part.substr(0, 4), "key=")) {
                    out += part.substr(0, 4);
                    out += "***";
                } else {
                    out += part;
                }
                if (amp == std::string_view::npos) break;
                rest.remove_prefix(amp + 1);
            }
            return out;
        }

        struct ParsedUrl {
            std::string   scheme;  // lower case "http" / "https"
            std::string   host;    // lower case
            std::uint16_t port = 0;
            std::string   rest;    // everything from the first '/' or '?' (may be empty)
        };

        // scheme://host[:port][rest]. Rejects userinfo, IPv6 literals, empty host, bad port.
        bool SplitUrl(std::string_view a_url, ParsedUrl& a_out, std::string& a_err) {
            std::size_t schemeLen = 0;
            if (a_url.size() > 7 && IEquals(a_url.substr(0, 7), "http://")) {
                a_out.scheme = "http";
                schemeLen    = 7;
            } else if (a_url.size() > 8 && IEquals(a_url.substr(0, 8), "https://")) {
                a_out.scheme = "https";
                schemeLen    = 8;
            } else {
                a_err = "URL must start with http:// or https://";
                return false;
            }
            for (const unsigned char c : a_url) {
                if (c <= 0x20 || c == 0x7F || c == '\\') {
                    a_err = "URL contains whitespace, control or backslash characters";
                    return false;
                }
            }
            std::string_view after = a_url.substr(schemeLen);
            const auto       slash = after.find_first_of("/?#");
            std::string_view auth  = after.substr(0, slash);
            a_out.rest             = slash == std::string_view::npos ? std::string() : std::string(after.substr(slash));
            if (const auto hash = a_out.rest.find('#'); hash != std::string::npos) a_out.rest.resize(hash);
            if (auth.empty()) {
                a_err = "URL has no host";
                return false;
            }
            if (auth.find('@') != std::string_view::npos) {
                a_err = "URL must not contain credentials (user@host)";
                return false;
            }
            if (auth.front() == '[') {
                a_err = "IPv6 literal hosts are not supported";
                return false;
            }
            std::string_view host = auth;
            std::uint32_t    port = a_out.scheme == "https" ? 443 : 80;
            if (const auto colon = auth.find(':'); colon != std::string_view::npos) {
                host = auth.substr(0, colon);
                std::string_view p = auth.substr(colon + 1);
                if (p.empty() || p.size() > 5) {
                    a_err = "bad port";
                    return false;
                }
                port = 0;
                for (const char c : p) {
                    if (c < '0' || c > '9') {
                        a_err = "bad port";
                        return false;
                    }
                    port = port * 10 + static_cast<std::uint32_t>(c - '0');
                }
                if (port < 1 || port > 65535) {
                    a_err = "port out of range";
                    return false;
                }
            }
            if (host.empty()) {
                a_err = "URL has no host";
                return false;
            }
            a_out.host = ToLower(std::string(host));
            a_out.port = static_cast<std::uint16_t>(port);
            return true;
        }

        Error MapHttp(int a_status, const std::string& a_code) {
            if (a_status >= 300 && a_status < 400) return Error::kRedirect;
            switch (a_status) {
            case 401: return Error::kBadKey;
            case 403: return Error::kDisabled;
            case 404:
                if (a_code == "no_tracks") return Error::kNoTracks;
                if (a_code == "unknown_type") return Error::kUnknownType;
                if (a_code == "unknown_profile") return Error::kUnknownProfile;
                return Error::kNotFound;
            case 502: return Error::kSourceUnavailable;
            case 503:
                if (a_code == "busy" || a_code == "cache_busy") return Error::kBusy;
                if (a_code == "cache_full") return Error::kCacheFull;
                if (a_code == "ffmpeg_missing") return Error::kFfmpegMissing;
                return Error::kServerError;
            case 504: return Error::kGatewayTimeout;
            default: break;
            }
            if (a_status >= 400 && a_status < 500) return Error::kBadRequest;
            if (a_status >= 500) return Error::kServerError;
            return Error::kBadResponse;
        }

        // ---- tolerant JSON accessors (unknown fields ignored, wrong types => default) ----
        std::string JStr(const json& a_j, const char* a_key) {
            const auto it = a_j.find(a_key);
            if (it == a_j.end()) return {};
            if (it->is_string()) return it->get<std::string>();
            if (it->is_number_integer()) return std::to_string(it->get<long long>());
            if (it->is_object()) {
                for (const char* k : { "name", "title", "label" }) {
                    const auto n = it->find(k);
                    if (n != it->end() && n->is_string()) return n->get<std::string>();
                }
            }
            return {};
        }
        std::int64_t JInt(const json& a_j, const char* a_key) {
            const auto it = a_j.find(a_key);
            if (it == a_j.end() || !it->is_number()) return 0;
            return static_cast<std::int64_t>(it->get<double>());
        }
        bool JBool(const json& a_j, const char* a_key) {
            const auto it = a_j.find(a_key);
            return it != a_j.end() && it->is_boolean() && it->get<bool>();
        }

        // ---- audio header probe ----
        std::uint32_t Le32(const std::uint8_t* p) {
            return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                   (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        }
        std::uint16_t Le16(const std::uint8_t* p) {
            return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
        }

        Result BadAudio(std::string a_msg) {
            Result r;
            r.error   = Error::kBadAudio;
            r.message = std::move(a_msg);
            return r;
        }

        bool ReadAt(std::ifstream& a_in, std::uint64_t a_off, void* a_buf, std::size_t a_n) {
            a_in.clear();
            a_in.seekg(static_cast<std::streamoff>(a_off), std::ios::beg);
            a_in.read(static_cast<char*>(a_buf), static_cast<std::streamsize>(a_n));
            return static_cast<std::size_t>(a_in.gcount()) == a_n;
        }

        // The worker enforces the configured timeouts itself: it waits for an async op at most
        // WinHTTP's own timeout + this grace, then abandons the request (kTimeout, logged). On Windows
        // WinHTTP's timeout fires first. Under Wine 10 (traced 2026-10-27) the async send task reads
        // the reply itself and no receive timeout fires, so there a stalled server is bounded by the
        // send-phase budget: 2*connect + send + receive + grace.
        constexpr DWORD kWatchdogGraceMs = 500;

        // Per-request state shared with the WinHTTP async callback (WinHTTP runs in async mode).
        // Heap-allocated, reference counted: one reference for the worker (dropped when it closes
        // the handles) and one for WinHTTP (dropped on WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, the
        // last callback WinHTTP makes for a handle). Everything an in-flight async operation may
        // still touch (read buffer, request body, header block, completion event) lives here, so a
        // worker that abandons a request after a cancel never leaves WinHTTP writing into freed
        // memory, and never has to wait for WinHTTP (or Wine) to actually unblock a socket.
        // Live ReqCtx count (test hook: must return to 0 once WinHTTP delivered every HANDLE_CLOSING).
        std::atomic<long> g_liveReqCtx{ 0 };

        struct ReqCtx {
            std::atomic<long> refs{ 1 };
            HANDLE            done = nullptr;  // auto-reset; set once per completed async op
            // Written by the callback BEFORE SetEvent(done); read by the worker only after its wait
            // returned on `done` (one op in flight at a time, so no concurrent write).
            bool              ok    = false;
            DWORD             err   = 0;
            DWORD             bytes = 0;
            std::wstring      headers;
            std::string       body;
            std::vector<char> buf;

            ReqCtx() : done(::CreateEventW(nullptr, FALSE, FALSE, nullptr)), buf(kChunk) { g_liveReqCtx.fetch_add(1); }
            ~ReqCtx() {
                if (done) ::CloseHandle(done);
                g_liveReqCtx.fetch_sub(1);
            }
            ReqCtx(const ReqCtx&) = delete;
            ReqCtx& operator=(const ReqCtx&) = delete;
            void AddRef() { refs.fetch_add(1); }
            void Release() {
                if (refs.fetch_sub(1) == 1) delete this;
            }
            void Complete(bool a_ok, DWORD a_err, DWORD a_bytes) {
                ok    = a_ok;
                err   = a_err;
                bytes = a_bytes;
                ::SetEvent(done);
            }
        };

        // Session-wide status callback (inherited by connect/request handles). Runs on a WinHTTP
        // thread or inline inside a WinHTTP call; it only records the result and signals. Handles
        // without a context (the session, connect handles, a request before its context is set)
        // arrive with a_ctx == 0 and are ignored.
        void CALLBACK OnWinHttpStatus(HINTERNET, DWORD_PTR a_ctx, DWORD a_status, LPVOID a_info, DWORD a_len) {
            auto* const c = reinterpret_cast<ReqCtx*>(a_ctx);
            if (!c) return;
            switch (a_status) {
            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                c->Complete(true, 0, 0);
                break;
            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                c->Complete(true, 0, a_len);
                break;
            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
                const auto* ar = static_cast<const WINHTTP_ASYNC_RESULT*>(a_info);
                c->Complete(false, ar ? ar->dwError : static_cast<DWORD>(ERROR_WINHTTP_INTERNAL_ERROR), 0);
                break;
            }
            case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
                c->Release();  // WinHTTP's reference; nothing touches c after this
                break;
            default:
                break;
            }
        }

        long long MsSince(std::chrono::steady_clock::time_point a_t) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - a_t).count();
        }

        // Owns a Win32 file handle for the .part file; closing before removal matters on Windows.
        struct FileHandle {
            HANDLE h = INVALID_HANDLE_VALUE;
            ~FileHandle() { Close(); }
            void Close() {
                if (h != INVALID_HANDLE_VALUE) {
                    ::CloseHandle(h);
                    h = INVALID_HANDLE_VALUE;
                }
            }
        };

        // Deletes the .part unless released. Declared BEFORE the FileHandle so the handle closes first.
        struct PartGuard {
            fs::path path;
            bool     keep = false;
            ~PartGuard() {
                if (!keep) {
                    std::error_code ec;
                    fs::remove(path, ec);
                }
            }
        };

    }  // namespace

    const char* ErrorName(Error a_err) {
        switch (a_err) {
        case Error::kOk: return "ok";
        case Error::kBadConfig: return "bad_config";
        case Error::kUnreachable: return "unreachable";
        case Error::kTimeout: return "timeout";
        case Error::kCancelled: return "cancelled";
        case Error::kRedirect: return "redirect_rejected";
        case Error::kBadKey: return "bad_key";
        case Error::kDisabled: return "disabled";
        case Error::kBadRequest: return "bad_request";
        case Error::kNoTracks: return "no_tracks";
        case Error::kUnknownType: return "unknown_type";
        case Error::kUnknownProfile: return "unknown_profile";
        case Error::kNotFound: return "not_found";
        case Error::kSourceUnavailable: return "source_unavailable";
        case Error::kBusy: return "busy";
        case Error::kCacheFull: return "cache_full";
        case Error::kFfmpegMissing: return "ffmpeg_missing";
        case Error::kServerError: return "server_error";
        case Error::kGatewayTimeout: return "gateway_timeout";
        case Error::kBadResponse: return "bad_response";
        case Error::kDeadline: return "deadline";
        case Error::kIo: return "io";
        case Error::kTruncated: return "truncated";
        case Error::kBadAudio: return "bad_audio";
        case Error::kPreviewOrTruncated: return "preview_or_truncated";
        case Error::kTooLarge: return "too_large";
        }
        return "?";
    }

    Result ProbeAudioFile(const fs::path& a_file, AudioInfo& a_out) {
        a_out = {};
        Result          r;
        std::error_code ec;
        const std::uint64_t size = fs::file_size(a_file, ec);
        if (ec) {
            r.error   = Error::kIo;
            r.message = "cannot stat audio file: " + ec.message();
            return r;
        }
        std::ifstream in(a_file, std::ios::binary);
        if (!in) {
            r.error   = Error::kIo;
            r.message = "cannot open audio file";
            return r;
        }
        a_out.fileBytes = size;

        std::uint8_t head[12]{};
        if (size < 12 || !ReadAt(in, 0, head, sizeof head)) return BadAudio("file too short for an audio header");

        if (std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "WAVE", 4) == 0) {
            const std::uint64_t riffSize = Le32(head + 4);
            if (riffSize + 8 != size) {
                return BadAudio(std::format("RIFF size {} does not match file size {}", riffSize + 8, size));
            }
            bool          haveFmt = false;
            std::uint16_t tag = 0, ch = 0, bits = 0;
            std::uint32_t rate = 0;
            std::uint64_t pos  = 12;
            while (pos + 8 <= size) {
                std::uint8_t ck[8]{};
                if (!ReadAt(in, pos, ck, sizeof ck)) break;
                const std::uint64_t csize = Le32(ck + 4);
                if (std::memcmp(ck, "fmt ", 4) == 0) {
                    if (csize < 16 || pos + 8 + csize > size) return BadAudio("bad fmt chunk");
                    std::uint8_t f[16]{};
                    if (!ReadAt(in, pos + 8, f, sizeof f)) return BadAudio("short fmt chunk");
                    tag     = Le16(f);
                    ch      = Le16(f + 2);
                    rate    = Le32(f + 4);
                    bits    = Le16(f + 14);
                    haveFmt = true;
                } else if (std::memcmp(ck, "data", 4) == 0) {
                    if (!haveFmt) return BadAudio("data chunk before fmt chunk");
                    if (pos + 8 + csize > size) {
                        return BadAudio(std::format("data chunk size {} runs past the end of the file", csize));
                    }
                    if (tag != 1 && tag != 0xFFFE) return BadAudio(std::format("unsupported WAV format tag {}", tag));
                    if (ch == 0 || rate == 0 || bits == 0 || (bits % 8) != 0) return BadAudio("invalid fmt fields");
                    const std::uint64_t block = static_cast<std::uint64_t>(ch) * (bits / 8);
                    if (csize % block != 0) return BadAudio("data size is not a whole number of frames");
                    a_out.format        = "wav";
                    a_out.sampleRate    = rate;
                    a_out.channels      = ch;
                    a_out.bitsPerSample = bits;
                    a_out.totalFrames   = csize / block;
                    a_out.durationS     = static_cast<double>(a_out.totalFrames) / static_cast<double>(rate);
                    return r;
                }
                pos += 8 + csize + (csize & 1);
            }
            return BadAudio("no data chunk");
        }

        if (std::memcmp(head, "fLaC", 4) == 0) {
            // First metadata block must be STREAMINFO (type 0, 34 bytes).
            const std::uint8_t type = head[4] & 0x7F;
            const std::uint32_t len = (static_cast<std::uint32_t>(head[5]) << 16) | (static_cast<std::uint32_t>(head[6]) << 8) | head[7];
            if (type != 0 || len != 34) return BadAudio("FLAC: first block is not STREAMINFO");
            std::uint8_t si[34]{};
            if (!ReadAt(in, 8, si, sizeof si)) return BadAudio("FLAC: short STREAMINFO");
            const std::uint32_t rate = (static_cast<std::uint32_t>(si[10]) << 12) | (static_cast<std::uint32_t>(si[11]) << 4) | (si[12] >> 4);
            const std::uint16_t ch   = static_cast<std::uint16_t>(((si[12] >> 1) & 7) + 1);
            const std::uint16_t bps  = static_cast<std::uint16_t>((((si[12] & 1) << 4) | (si[13] >> 4)) + 1);
            const std::uint64_t total = (static_cast<std::uint64_t>(si[13] & 0x0F) << 32) | (static_cast<std::uint64_t>(si[14]) << 24) |
                                        (static_cast<std::uint64_t>(si[15]) << 16) | (static_cast<std::uint64_t>(si[16]) << 8) | si[17];
            if (rate == 0) return BadAudio("FLAC: sample rate 0");
            if (total == 0) return BadAudio("FLAC: STREAMINFO has no total sample count (duration unverifiable)");
            a_out.format        = "flac";
            a_out.sampleRate    = rate;
            a_out.channels      = ch;
            a_out.bitsPerSample = bps;
            a_out.totalFrames   = total;
            a_out.durationS     = static_cast<double>(total) / static_cast<double>(rate);
            return r;
        }

        return BadAudio("not a RIFF/WAVE or FLAC file");
    }

    // ------------------------------------------------------------------------------------------
    struct HarmonyClient::Impl {
        Config              cfg;
        bool                configured = false;
        ParsedUrl           base;
        std::string         prefix;  // base path without trailing '/'
        std::wstring        wHost, wUserAgent, wKeyHeader;
        HINTERNET           session = nullptr;  // WINHTTP_FLAG_ASYNC, status callback OnWinHttpStatus
        // Cancellation is signalling only: Cancel()/ClearCancel() set/reset cancelEvt and the
        // per-call stop_callback sets stopEvt. No WinHTTP call ever runs on the cancelling thread;
        // the worker (the only thread that touches WinHTTP handles) wakes up and closes its own handles.
        // cancelEvt is the ONLY cancel state (no separate flag that could disagree with it).
        HANDLE              cancelEvt = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset, sticky
        HANDLE              stopEvt   = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset, per call

        Impl() = default;
        ~Impl() {
            CloseSession();
            if (cancelEvt) ::CloseHandle(cancelEvt);
            if (stopEvt) ::CloseHandle(stopEvt);
        }

        void CloseSession() {
            if (session) {
                ::WinHttpCloseHandle(session);
                session = nullptr;
            }
        }

        bool Cancelled(const std::stop_token& a_st) const {
            return ::WaitForSingleObject(cancelEvt, 0) == WAIT_OBJECT_0 || a_st.stop_requested();
        }

        // Start of every API call: arm the per-call stop event. The stop_callback the caller then
        // registers only does SetEvent(stopEvt) (safe on any thread, including the game thread).
        void ArmStop() { ::ResetEvent(stopEvt); }

        // One request/response, owned by the worker thread for its whole life (every WinHTTP call
        // on conn/req happens on this thread; nobody else ever sees the handles).
        struct Exchange {
            HINTERNET    conn = nullptr;
            HINTERNET    req  = nullptr;
            ReqCtx*      ctx  = nullptr;  // the worker's reference
            DWORD        recvMs = 0;
            int          status = 0;
            std::int64_t contentLength = -1;
            int          retryAfterS = 0;
            Exchange() = default;
            Exchange(const Exchange&) = delete;
            Exchange& operator=(const Exchange&) = delete;
            ~Exchange() { Close(); }
            // Closing a request with an async op still pending cancels that op; WinHTTP then
            // delivers HANDLE_CLOSING (possibly later, on its own thread) and drops its ctx ref.
            void Close() {
                if (req) {
                    ::WinHttpCloseHandle(req);
                    req = nullptr;
                }
                if (conn) {
                    ::WinHttpCloseHandle(conn);
                    conn = nullptr;
                }
                if (ctx) {
                    ctx->Release();
                    ctx = nullptr;
                }
            }
        };

        // Waits for the async op in flight on a_ctx, for cancel/stop, or for the watchdog budget.
        Error WaitOp(ReqCtx& a_ctx, DWORD a_budgetMs, const std::stop_token& a_st, std::string& a_msg) const {
            const HANDLE hs[3] = { a_ctx.done, cancelEvt, stopEvt };
            const DWORD  w     = ::WaitForMultipleObjects(3, hs, FALSE, a_budgetMs);
            if (w == WAIT_OBJECT_0) {
                if (a_ctx.ok) return Error::kOk;
                return TransportError(a_ctx.err, a_st, a_msg);
            }
            if (w == WAIT_OBJECT_0 + 1 || w == WAIT_OBJECT_0 + 2) {
                a_msg = "cancelled";
                return Error::kCancelled;
            }
            if (w == WAIT_TIMEOUT) {
                spdlog::warn("[net] WinHTTP did not complete an operation within {} ms and its own timeout did not fire "
                             "(expected under Wine); client enforced the timeout, abandoning the request",
                             static_cast<unsigned long>(a_budgetMs));
                a_msg = "timed out (enforced by the client; WinHTTP's timeout did not fire)";
                return Error::kTimeout;
            }
            a_msg = std::format("wait failed (error {})", static_cast<unsigned long>(::GetLastError()));
            return Error::kUnreachable;
        }

        // Cancellable sleep: kOk after the full sleep, kCancelled, or kUnreachable if the wait itself failed.
        Error SleepOrCancel(int a_ms, const std::stop_token& a_st, std::string& a_msg) const {
            if (Cancelled(a_st)) {
                a_msg = "cancelled";
                return Error::kCancelled;
            }
            const HANDLE hs[2] = { cancelEvt, stopEvt };
            const DWORD  w     = ::WaitForMultipleObjects(2, hs, FALSE, static_cast<DWORD>(a_ms));
            if (w == WAIT_FAILED) {
                a_msg = std::format("wait failed (error {})", static_cast<unsigned long>(::GetLastError()));
                return Error::kUnreachable;
            }
            if (w != WAIT_TIMEOUT || Cancelled(a_st)) {
                a_msg = "cancelled";
                return Error::kCancelled;
            }
            return Error::kOk;
        }

        Error TransportError(DWORD a_err, const std::stop_token& a_st, std::string& a_msg) const {
            if (Cancelled(a_st)) {
                a_msg = "cancelled";
                return Error::kCancelled;
            }
            if (a_err == ERROR_WINHTTP_TIMEOUT) {
                a_msg = "timed out";
                return Error::kTimeout;
            }
            a_msg = std::format("WinHTTP error {}", static_cast<unsigned long>(a_err));
            return Error::kUnreachable;
        }

        // Sends the request and reads the status line + headers. On success a_ex.status etc. are set.
        Result Open(const wchar_t* a_method, const std::string& a_target, const std::string* a_body,
                    const wchar_t* a_accept, int a_recvTimeoutMs, const std::stop_token& a_st, Exchange& a_ex) {
            Result r;
            auto   fail = [&](DWORD a_e) {
                r.error = TransportError(a_e, a_st, r.message);
                return r;
            };
            auto local = [&](Error a_e, const char* a_what) {  // refused before any byte was sent
                r.error   = a_e;
                r.message = std::format("{} (error {})", a_what, static_cast<unsigned long>(::GetLastError()));
                return r;
            };
            auto waitOp = [&](DWORD a_budgetMs) {
                r.error = WaitOp(*a_ex.ctx, a_budgetMs, a_st, r.message);
                return r.Ok();
            };
            if (Cancelled(a_st)) {
                r.error   = Error::kCancelled;
                r.message = "cancelled";
                return r;
            }
            a_ex.recvMs = static_cast<DWORD>(a_recvTimeoutMs);
            try {
                a_ex.ctx = new ReqCtx();
            } catch (const std::exception&) {
                r.error   = Error::kUnreachable;
                r.message = "out of memory for the request context";
                return r;
            }
            if (!a_ex.ctx->done) return local(Error::kUnreachable, "CreateEvent failed");
            a_ex.conn = ::WinHttpConnect(session, wHost.c_str(), base.port, 0);
            if (!a_ex.conn) return fail(::GetLastError());
            const std::wstring wTarget = Widen(a_target);
            a_ex.req = ::WinHttpOpenRequest(a_ex.conn, a_method, wTarget.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, base.scheme == "https" ? WINHTTP_FLAG_SECURE : 0);
            if (!a_ex.req) return fail(::GetLastError());
            {
                // WinHTTP's reference on the context; released by HANDLE_CLOSING once the value is set.
                a_ex.ctx->AddRef();
                DWORD_PTR cv = reinterpret_cast<DWORD_PTR>(a_ex.ctx);
                if (!::WinHttpSetOption(a_ex.req, WINHTTP_OPTION_CONTEXT_VALUE, &cv, sizeof cv)) {
                    a_ex.ctx->Release();  // never attached, so no HANDLE_CLOSING will carry it
                    return local(Error::kUnreachable, "WinHTTP refused the request context");
                }
            }
            // Redirects are never followed (the key must not travel to another host). Both knobs,
            // and a failure of either refuses the request instead of failing open.
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            if (!::WinHttpSetOption(a_ex.req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy)) {
                return local(Error::kBadConfig, "WinHTTP refused REDIRECT_POLICY_NEVER; request not sent");
            }
            DWORD noRedirects = WINHTTP_DISABLE_REDIRECTS;
            if (!::WinHttpSetOption(a_ex.req, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirects, sizeof noRedirects)) {
                return local(Error::kBadConfig, "WinHTTP refused DISABLE_REDIRECTS; request not sent");
            }
            if (!::WinHttpSetTimeouts(a_ex.req, cfg.connectTimeoutMs, cfg.connectTimeoutMs, cfg.sendTimeoutMs, a_recvTimeoutMs)) {
                return local(Error::kBadConfig, "WinHTTP refused the timeouts; request not sent");
            }

            // Header block and body live in the context: an async send may still read them after
            // this worker has abandoned the request.
            ReqCtx& c = *a_ex.ctx;
            c.headers = L"Accept: ";
            c.headers += a_accept;
            c.headers += L"\r\n";
            c.headers += wKeyHeader;
            if (a_body) {
                c.headers += L"Content-Type: application/json\r\n";
                c.body = *a_body;
            }
            LPVOID      data = a_body ? static_cast<LPVOID>(c.body.data()) : WINHTTP_NO_REQUEST_DATA;
            const DWORD dlen = a_body ? static_cast<DWORD>(c.body.size()) : 0;
            if (Cancelled(a_st)) {
                r.error   = Error::kCancelled;
                r.message = "cancelled";
                return r;
            }
            if (!::WinHttpSendRequest(a_ex.req, c.headers.c_str(), static_cast<DWORD>(-1), data, dlen, dlen,
                                      reinterpret_cast<DWORD_PTR>(a_ex.ctx))) {
                return fail(::GetLastError());
            }
            // Resolve + connect + TLS + send, then the server's answer (which includes a wait= phase).
            const DWORD sendBudget = static_cast<DWORD>(2 * cfg.connectTimeoutMs + cfg.sendTimeoutMs + a_recvTimeoutMs) + kWatchdogGraceMs;
            if (!waitOp(sendBudget)) return r;
            if (!::WinHttpReceiveResponse(a_ex.req, nullptr)) return fail(::GetLastError());
            if (!waitOp(a_ex.recvMs + kWatchdogGraceMs)) return r;

            DWORD status = 0;
            DWORD len    = sizeof status;
            if (!::WinHttpQueryHeaders(a_ex.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                       WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
                return fail(::GetLastError());
            }
            a_ex.status = static_cast<int>(status);

            wchar_t buf[64]{};
            len = sizeof buf - sizeof(wchar_t);
            if (::WinHttpQueryHeaders(a_ex.req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buf, &len,
                                      WINHTTP_NO_HEADER_INDEX)) {
                a_ex.contentLength = static_cast<std::int64_t>(std::wcstoull(buf, nullptr, 10));
            }
            wchar_t ra[64]{};
            len = sizeof ra - sizeof(wchar_t);
            if (::WinHttpQueryHeaders(a_ex.req, WINHTTP_QUERY_CUSTOM, L"Retry-After", ra, &len, WINHTTP_NO_HEADER_INDEX)) {
                a_ex.retryAfterS = static_cast<int>(std::wcstol(ra, nullptr, 10));
                if (a_ex.retryAfterS < 0) a_ex.retryAfterS = 0;
            }
            r.httpStatus  = a_ex.status;
            r.retryAfterS = a_ex.retryAfterS;
            return r;
        }

        // One async WinHttpReadData into a_ex.ctx->buf; a_n = bytes read (0 = end of body).
        Error ReadChunk(Exchange& a_ex, DWORD& a_n, const std::stop_token& a_st, std::string& a_msg) {
            a_n = 0;
            if (Cancelled(a_st)) {
                a_msg = "cancelled";
                return Error::kCancelled;
            }
            // Async mode: the byte count arrives in READ_COMPLETE, never through the out pointer.
            if (!::WinHttpReadData(a_ex.req, a_ex.ctx->buf.data(), static_cast<DWORD>(a_ex.ctx->buf.size()), nullptr)) {
                return TransportError(::GetLastError(), a_st, a_msg);
            }
            const Error e = WaitOp(*a_ex.ctx, a_ex.recvMs + kWatchdogGraceMs, a_st, a_msg);
            if (e == Error::kOk) a_n = (std::min)(a_ex.ctx->bytes, static_cast<DWORD>(a_ex.ctx->buf.size()));
            return e;
        }

        // Reads up to a_cap bytes of the body (stops at end of body or the cap).
        Result ReadBody(Exchange& a_ex, std::string& a_out, std::size_t a_cap, const std::stop_token& a_st) {
            Result r;
            while (true) {
                DWORD n = 0;
                r.error = ReadChunk(a_ex, n, a_st, r.message);
                if (!r.Ok() || n == 0) return r;
                if (a_out.size() + n > a_cap) {
                    a_out.append(a_ex.ctx->buf.data(), a_cap - a_out.size());
                    return r;  // enough; remaining body is dropped with the connection
                }
                a_out.append(a_ex.ctx->buf.data(), n);
            }
        }

        void Log(const char* a_method, const std::string& a_target, int a_status, const char* a_errName, long long a_ms) {
            const std::string t = Redact(a_target);
            if (a_status > 0) {
                spdlog::info("[net] {} {} -> {} ({} ms)", a_method, t, a_status, a_ms);
            } else {
                spdlog::warn("[net] {} {} -> {} ({} ms)", a_method, t, a_errName, a_ms);
            }
        }

        // Fills a_r from an error response body: {"error": "...", "code": "..."}.
        void ClassifyHttp(Result& a_r, int a_status, const std::string& a_body, int a_retryAfter) {
            a_r.httpStatus  = a_status;
            a_r.retryAfterS = a_retryAfter;
            std::string msg;
            try {
                const json j = json::parse(a_body, nullptr, false);
                if (j.is_object()) {
                    a_r.code = JStr(j, "code");
                    msg      = JStr(j, "error");
                    if (msg.empty()) msg = JStr(j, "message");
                }
            } catch (...) {
            }
            a_r.error   = MapHttp(a_status, a_r.code);
            a_r.message = std::format("HTTP {}{}{}", a_status, msg.empty() ? "" : ": ", msg);
        }

        // One non-streaming JSON round trip. a_okBody receives the body on 2xx.
        Result Roundtrip(const wchar_t* a_method, const char* a_methodName, const std::string& a_path,
                         const std::string* a_body, const std::stop_token& a_st, std::string& a_okBody) {
            Result r;
            if (!configured) {
                r.error   = Error::kBadConfig;
                r.message = "client not configured";
                return r;
            }
            const std::string target = prefix + a_path;
            const auto        t0     = std::chrono::steady_clock::now();
            Exchange          ex;
            r = Open(a_method, target, a_body, L"application/json", cfg.receiveTimeoutMs, a_st, ex);
            if (!r.Ok()) {
                Log(a_methodName, target, 0, ErrorName(r.error), MsSince(t0));
                return r;
            }
            std::string body;
            const Result rb = ReadBody(ex, body, kMaxJsonBody, a_st);
            Log(a_methodName, target, ex.status, "", MsSince(t0));
            if (!rb.Ok()) return rb;
            if (ex.status >= 200 && ex.status < 300) {
                a_okBody = std::move(body);
                r.httpStatus = ex.status;
                return r;
            }
            ClassifyHttp(r, ex.status, body, ex.retryAfterS);
            return r;
        }

        // ---- API implementations ----
        StatusResult DoStatus(const std::stop_token& a_st) {
            ArmStop();  // the stop callback below only signals; it never calls WinHTTP
            std::stop_callback cb(a_st, [h = stopEvt] { ::SetEvent(h); });
            StatusResult       out;
            std::string        body;
            static_cast<Result&>(out) = Roundtrip(L"GET", "GET", "/api/game/status", nullptr, a_st, body);
            out.reachable = out.httpStatus > 0;
            if (!out.Ok()) return out;
            try {
                const json j = json::parse(body, nullptr, false);
                if (!j.is_object()) throw std::runtime_error("status body is not a JSON object");
                out.enabled = JBool(j, "enabled");
                out.ffmpeg  = JBool(j, "ffmpeg");
                if (const auto it = j.find("containers"); it != j.end() && it->is_array()) {
                    for (const auto& c : *it) {
                        if (c.is_string()) out.containers.push_back(c.get<std::string>());
                    }
                } else {
                    out.containers.push_back("wav");  // a server without the field only does WAV
                }
                if (const auto it = j.find("cache"); it != j.end() && it->is_object()) {
                    out.cache.bytes           = JInt(*it, "bytes");
                    out.cache.files           = JInt(*it, "files");
                    out.cache.inProgressBytes = JInt(*it, "in_progress_bytes");
                    out.cache.cap             = JInt(*it, "cap");
                }
            } catch (const std::exception& e) {
                out.error   = Error::kBadResponse;
                out.message = std::string("bad status response: ") + e.what();
            }
            return out;
        }

        Result DoPutProfile(std::string_view a_name, std::string_view a_json, const std::stop_token& a_st) {
            ArmStop();  // the stop callback below only signals; it never calls WinHTTP
            std::stop_callback cb(a_st, [h = stopEvt] { ::SetEvent(h); });
            Result             r;
            bool               nameOk = !a_name.empty() && a_name.size() <= 64;
            for (std::size_t i = 0; i < a_name.size() && nameOk; ++i) {
                const char c = a_name[i];
                const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                if (!(alnum || (i > 0 && (c == '_' || c == '-' || c == '.')))) nameOk = false;
            }
            if (!nameOk) {
                r.error   = Error::kBadRequest;
                r.message = "invalid profile name";
                return r;
            }
            if (a_json.empty()) {
                r.error   = Error::kBadRequest;
                r.message = "empty profile body";
                return r;
            }
            const std::string body(a_json);
            std::string       ignored;
            return Roundtrip(L"PUT", "PUT", "/api/game/profiles/" + std::string(a_name), &body, a_st, ignored);
        }

        static Track ParseTrack(const json& a_t) {
            Track t;
            t.service    = JStr(a_t, "service");
            t.id         = JStr(a_t, "id");
            t.title      = JStr(a_t, "title");
            t.artist     = JStr(a_t, "artist");
            t.album      = JStr(a_t, "album");
            t.durationS  = JInt(a_t, "duration_s");
            t.audioUrl   = JStr(a_t, "audio_url");
            if (const auto q = a_t.find("quality"); q != a_t.end() && q->is_object()) t.qualityLabel = JStr(*q, "label");
            if (const auto s = a_t.find("source"); s != a_t.end() && s->is_object()) {
                t.sourceService = JStr(*s, "service");
                t.sourceLabel   = JStr(*s, "label");
                t.fromLibrary   = JBool(*s, "from_library");
            }
            return t;
        }

        // Serialises a request body. Strings that are not valid UTF-8 are REJECTED (kBadRequest, no
        // request sent) rather than silently altered; nlohmann's strict dump throws on them.
        static bool DumpRequest(const json& a_req, std::string& a_out, Result& a_r) {
            try {
                a_out = a_req.dump(-1, ' ', false, json::error_handler_t::strict);
                return true;
            } catch (const std::exception&) {
                a_r.error   = Error::kBadRequest;
                a_r.message = "request field is not valid UTF-8";
                return false;
            }
        }

        NextResult DoNext(std::string_view a_profile, std::string_view a_type, std::string_view a_sources, int a_count,
                          bool a_peek, std::string_view a_container, const std::stop_token& a_st) {
            ArmStop();  // the stop callback below only signals; it never calls WinHTTP
            std::stop_callback cb(a_st, [h = stopEvt] { ::SetEvent(h); });
            NextResult         out;
            std::string        reqBody;
            try {
                json req = { { "profile", std::string(a_profile) }, { "type", std::string(a_type) } };
                if (!a_sources.empty()) req["sources"] = std::string(a_sources);
                if (a_count > 0) req["count"] = (std::min)(a_count, 10);
                if (a_peek) req["peek"] = true;
                if (!a_container.empty()) req["container"] = std::string(a_container);
                if (!DumpRequest(req, reqBody, out)) return out;
            } catch (const std::exception& e) {
                out.error   = Error::kBadRequest;
                out.message = std::string("cannot build next request: ") + e.what();
                return out;
            }
            std::string       body;
            static_cast<Result&>(out) = Roundtrip(L"POST", "POST", "/api/game/next", &reqBody, a_st, body);
            if (!out.Ok()) return out;
            try {
                const json j = json::parse(body, nullptr, false);
                if (!j.is_object()) throw std::runtime_error("next body is not a JSON object");
                out.type          = JStr(j, "type");
                out.requestedType = JStr(j, "requested_type");
                out.sources       = JStr(j, "sources");
                out.poolSize      = JInt(j, "pool_size");
                const auto it     = j.find("tracks");
                if (it == j.end() || !it->is_array()) throw std::runtime_error("no tracks array");
                for (const auto& t : *it) {
                    if (!t.is_object()) continue;
                    Track tr = ParseTrack(t);
                    if (tr.id.empty() || tr.audioUrl.empty()) continue;  // unusable without an id and a URL
                    out.tracks.push_back(std::move(tr));
                }
            } catch (const std::exception& e) {
                out.error   = Error::kBadResponse;
                out.message = std::string("bad next response: ") + e.what();
                out.tracks.clear();
            }
            return out;
        }

        SearchResult DoSearch(std::string_view a_query, std::string_view a_kinds, int a_limit, const std::stop_token& a_st) {
            ArmStop();  // the stop callback below only signals; it never calls WinHTTP
            std::stop_callback cb(a_st, [h = stopEvt] { ::SetEvent(h); });
            SearchResult       out;
            std::string        path = "/api/search?q=" + UrlEncode(a_query);
            if (!a_kinds.empty()) path += "&kinds=" + UrlEncode(a_kinds);
            if (a_limit > 0) path += "&limit=" + std::to_string(a_limit);
            std::string body;
            static_cast<Result&>(out) = Roundtrip(L"GET", "GET", path, nullptr, a_st, body);
            if (!out.Ok()) return out;
            try {
                const json j = json::parse(body, nullptr, false);
                if (!j.is_object()) throw std::runtime_error("search body is not a JSON object");
                auto fill = [&](const char* a_key, std::vector<SearchItem>& a_vec) {
                    const auto it = j.find(a_key);
                    if (it == j.end() || !it->is_array()) return;
                    for (const auto& e : *it) {
                        if (!e.is_object()) continue;
                        SearchItem s;
                        s.service   = JStr(e, "service");
                        s.id        = JStr(e, "id");
                        s.title     = JStr(e, "title");
                        if (s.title.empty()) s.title = JStr(e, "name");
                        s.artist    = JStr(e, "artist");
                        s.album     = JStr(e, "album");
                        s.durationS = JInt(e, "duration_s");
                        a_vec.push_back(std::move(s));
                    }
                };
                fill("tracks", out.tracks);
                fill("artists", out.artists);
                fill("albums", out.albums);
            } catch (const std::exception& e) {
                out.error   = Error::kBadResponse;
                out.message = std::string("bad search response: ") + e.what();
            }
            return out;
        }

        // POST /api/game/playing {profile, type, service, id} (all four required by the server).
        Result DoPlaying(std::string_view a_profile, std::string_view a_type, std::string_view a_service, std::string_view a_id,
                         const std::stop_token& a_st) {
            ArmStop();  // the stop callback below only signals; it never calls WinHTTP
            std::stop_callback cb(a_st, [h = stopEvt] { ::SetEvent(h); });
            Result             r;
            if (a_profile.empty() || a_type.empty() || a_service.empty() || a_id.empty()) {
                r.error   = Error::kBadRequest;
                r.message = "playing report needs profile, type, service and id";
                return r;
            }
            std::string reqBody;
            try {
                const json req = { { "profile", std::string(a_profile) }, { "type", std::string(a_type) },
                                   { "service", std::string(a_service) }, { "id", std::string(a_id) } };
                if (!DumpRequest(req, reqBody, r)) return r;
            } catch (const std::exception& e) {
                r.error   = Error::kBadRequest;
                r.message = std::string("cannot build playing report: ") + e.what();
                return r;
            }
            std::string ignored;
            return Roundtrip(L"POST", "POST", "/api/game/playing", &reqBody, a_st, ignored);
        }

        // Relative URLs are rooted at the base prefix; absolute ones must name the same host:port.
        bool ResolveAudioUrl(std::string_view a_url, std::string& a_target, std::string& a_err) const {
            if (a_url.empty()) {
                a_err = "empty audio_url";
                return false;
            }
            for (const unsigned char c : a_url) {
                if (c <= 0x20 || c == 0x7F || c == '\\') {
                    a_err = "audio_url contains whitespace or control characters";
                    return false;
                }
            }
            std::string_view u = a_url;
            if (const auto hash = u.find('#'); hash != std::string_view::npos) u = u.substr(0, hash);
            if (u.empty()) {
                a_err = "empty audio_url";
                return false;
            }
            if (u.size() > 2 && u.substr(0, 2) == "//") {
                a_err = "scheme-relative audio_url rejected";
                return false;
            }
            if (u.find("://") != std::string_view::npos && u.find("://") < 6) {
                ParsedUrl p;
                if (!SplitUrl(u, p, a_err)) return false;
                if (p.scheme != base.scheme || p.host != base.host || p.port != base.port) {
                    a_err = "audio_url points to a different host than the configured server";
                    return false;
                }
                a_target = p.rest.empty() ? "/" : (p.rest[0] == '/' ? p.rest : "/" + p.rest);
                return true;
            }
            a_target = prefix + (u.front() == '/' ? "" : "/") + std::string(u);
            return true;
        }

        DownloadResult DoDownload(std::string_view a_audioUrl, const fs::path& a_dest, double a_expectedS,
                                  const std::stop_token& a_st) {
            ArmStop();  // the stop callback below only signals; it never calls WinHTTP
            std::stop_callback cb(a_st, [h = stopEvt] { ::SetEvent(h); });
            DownloadResult     out;
            auto fail = [&](Error a_e, std::string a_msg) -> DownloadResult {
                out.error   = a_e;
                out.message = std::move(a_msg);
                return out;
            };
            if (!configured) return fail(Error::kBadConfig, "client not configured");

            std::string target, err;
            if (!ResolveAudioUrl(a_audioUrl, target, err)) return fail(Error::kBadResponse, err);
            if (target.find("wait=") == std::string::npos) {
                target += (target.find('?') == std::string::npos ? "?wait=" : "&wait=") + std::to_string(cfg.waitS);
            }

            PartGuard guard;
            guard.path = a_dest;
            guard.path += ".part";
            {
                std::error_code ec;
                fs::remove(guard.path, ec);
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.downloadDeadlineS);
            const int  recvMs   = (std::max)(cfg.receiveTimeoutMs, (cfg.waitS + 10) * 1000);

            while (true) {
                if (Cancelled(a_st)) return fail(Error::kCancelled, "cancelled");
                const auto t0 = std::chrono::steady_clock::now();
                Exchange   ex;
                Result     r = Open(L"GET", target, nullptr, L"*/*", recvMs, a_st, ex);
                if (!r.Ok()) {
                    Log("GET", target, 0, ErrorName(r.error), MsSince(t0));
                    out.error = r.error;
                    out.message = r.message;
                    return out;
                }
                if (ex.status == 202) {
                    std::string drain;
                    ReadBody(ex, drain, kChunk, a_st);
                    Log("GET", target, 202, "", MsSince(t0));
                    ex.Close();
                    int waitS = ex.retryAfterS > 0 ? ex.retryAfterS : 2;
                    waitS     = (std::min)((std::max)(waitS, 1), 30);
                    const auto now = std::chrono::steady_clock::now();
                    if (now + std::chrono::seconds(waitS) > deadline) {
                        return fail(Error::kDeadline, std::format("still transcoding after {} s", cfg.downloadDeadlineS));
                    }
                    std::string sm;
                    if (const Error se = SleepOrCancel(waitS * 1000, a_st, sm); se != Error::kOk) return fail(se, sm);
                    continue;
                }
                if (ex.status != 200) {
                    std::string body;
                    if (ReadBody(ex, body, kChunk, a_st).error == Error::kCancelled) {
                        Log("GET", target, 0, ErrorName(Error::kCancelled), MsSince(t0));
                        return fail(Error::kCancelled, "cancelled");
                    }
                    Log("GET", target, ex.status, "", MsSince(t0));
                    Result rr;
                    ClassifyHttp(rr, ex.status, body, ex.retryAfterS);
                    if (rr.error == Error::kBadResponse) rr.message = std::format("unexpected HTTP status {}", ex.status);
                    out.error       = rr.error;
                    out.message     = rr.message;
                    out.code        = rr.code;
                    out.httpStatus  = rr.httpStatus;
                    out.retryAfterS = rr.retryAfterS;
                    return out;
                }

                // ---- 200: stream to .part ----
                out.httpStatus = 200;
                if (ex.contentLength <= 0) {
                    Log("GET", target, 200, "", MsSince(t0));
                    return fail(Error::kBadResponse, "200 without a usable Content-Length");
                }
                // Size bound, checked BEFORE anything is created on disk.
                if (ex.contentLength > cfg.maxDownloadBytes) {
                    Log("GET", target, 200, "", MsSince(t0));
                    return fail(Error::kTooLarge, std::format("Content-Length {} exceeds the {} byte download limit", ex.contentLength,
                                                              cfg.maxDownloadBytes));
                }
                FileHandle fh;
                fh.h = ::CreateFileW(guard.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (fh.h == INVALID_HANDLE_VALUE) {
                    return fail(Error::kIo, std::format("cannot create .part file (error {})", static_cast<unsigned long>(::GetLastError())));
                }
                std::int64_t got = 0;
                while (true) {
                    DWORD       n = 0;
                    std::string m;
                    const Error e = ReadChunk(ex, n, a_st, m);
                    if (e != Error::kOk) {
                        Log("GET", target, 0, "read failed", MsSince(t0));
                        if (e == Error::kUnreachable) {
                            return fail(Error::kTruncated, std::format("connection lost after {} of {} bytes ({})", got, ex.contentLength, m));
                        }
                        return fail(e, std::format("{} after {} of {} bytes", m, got, ex.contentLength));
                    }
                    if (n == 0) break;
                    got += n;
                    if (got > ex.contentLength) return fail(Error::kBadResponse, "body longer than Content-Length");
                    if (got > cfg.maxDownloadBytes) {
                        return fail(Error::kTooLarge, std::format("body exceeds the {} byte download limit", cfg.maxDownloadBytes));
                    }
                    const char* const buf     = ex.ctx->buf.data();
                    DWORD             written = 0;
                    if (!::WriteFile(fh.h, buf, n, &written, nullptr) || written != n) {
                        return fail(Error::kIo, std::format("write failed (error {})", static_cast<unsigned long>(::GetLastError())));
                    }
                }
                Log("GET", target, 200, "", MsSince(t0));
                out.bytes = got;
                if (got != ex.contentLength) {
                    return fail(Error::kTruncated, std::format("received {} of {} bytes", got, ex.contentLength));
                }
                if (!::FlushFileBuffers(fh.h)) {
                    return fail(Error::kIo, std::format("flush failed (error {})", static_cast<unsigned long>(::GetLastError())));
                }
                fh.Close();
                ex.Close();

                // ---- validate before the file can appear under its final name ----
                const Result pr = ProbeAudioFile(guard.path, out.audio);
                if (!pr.Ok()) {
                    spdlog::warn("[net] downloaded audio rejected: {}", pr.message);
                    return fail(pr.error, pr.message);
                }
                if (a_expectedS > 60.0 && out.audio.durationS < 0.9 * a_expectedS) {
                    spdlog::warn("[net] preview or truncated stream: {:.1f} s of expected {:.1f} s", out.audio.durationS, a_expectedS);
                    return fail(Error::kPreviewOrTruncated,
                                std::format("preview or truncated stream: measured {:.1f} s, expected {:.1f} s", out.audio.durationS, a_expectedS));
                }
                if (!::MoveFileExW(guard.path.c_str(), a_dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    return fail(Error::kIo, std::format("rename failed (error {})", static_cast<unsigned long>(::GetLastError())));
                }
                guard.keep = true;
                return out;
            }
        }
    };

    // ------------------------------------------------------------------------------------------
    HarmonyClient::HarmonyClient() : impl_(std::make_unique<Impl>()) {}
    HarmonyClient::~HarmonyClient() = default;

    Result HarmonyClient::Configure(const Config& a_cfg) {
        Result r;
        auto   bad = [&](std::string a_msg) {
            r.error   = Error::kBadConfig;
            r.message = std::move(a_msg);
            return r;
        };
        Impl&     o = *impl_;
        ParsedUrl base;
        std::string err;
        if (!SplitUrl(a_cfg.baseUrl, base, err)) return bad(err);
        if (base.rest.find('?') != std::string::npos) return bad("base URL must not contain a query");
        std::string prefix = base.rest;
        while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
        for (const unsigned char c : a_cfg.personalKey) {
            if (c <= 0x20 || c >= 0x7F) return bad("personal key must be printable ASCII without spaces");
        }
        for (const unsigned char c : a_cfg.userAgent) {
            if (c < 0x20 || c >= 0x7F) return bad("user agent must be printable ASCII");
        }
        auto okMs = [](int v) { return v >= 100 && v <= 600000; };
        if (!okMs(a_cfg.connectTimeoutMs) || !okMs(a_cfg.sendTimeoutMs) || !okMs(a_cfg.receiveTimeoutMs)) {
            return bad("timeouts must be 100..600000 ms");
        }
        if (a_cfg.waitS < 0 || a_cfg.waitS > 120) return bad("waitS must be 0..120");
        if (a_cfg.downloadDeadlineS < 1 || a_cfg.downloadDeadlineS > 3600) return bad("downloadDeadlineS must be 1..3600");
        if (a_cfg.maxDownloadBytes < 4096 || a_cfg.maxDownloadBytes > (std::int64_t{ 64 } << 30)) {
            return bad("maxDownloadBytes must be 4096 .. 64 GiB");
        }
        if (!o.cancelEvt || !o.stopEvt) return bad("CreateEvent failed");

        o.CloseSession();
        o.configured = false;
        o.cfg        = a_cfg;
        o.base       = base;
        o.prefix     = prefix;
        o.wHost      = Widen(base.host);
        o.wUserAgent = Widen(a_cfg.userAgent);
        o.wKeyHeader = a_cfg.personalKey.empty() ? std::wstring() : L"X-Harmony-Key: " + Widen(a_cfg.personalKey) + L"\r\n";
        // Async session: every blocking point becomes an event the worker waits on together with
        // the cancel/stop events, so a cancel never depends on WinHTTP (or Wine) unblocking a socket.
        o.session    = ::WinHttpOpen(o.wUserAgent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (!o.session) {
            return bad(std::format("WinHttpOpen failed (error {})", static_cast<unsigned long>(::GetLastError())));
        }
        if (::WinHttpSetStatusCallback(o.session, OnWinHttpStatus,
                                       WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                       0) == WINHTTP_INVALID_STATUS_CALLBACK) {
            const DWORD le = ::GetLastError();
            o.CloseSession();
            return bad(std::format("WinHttpSetStatusCallback failed (error {})", static_cast<unsigned long>(le)));
        }
        o.configured = true;
        spdlog::info("[net] configured: {}://{}:{}{} (key {})", base.scheme, base.host, base.port, prefix,
                     a_cfg.personalKey.empty() ? "not set" : "set");
        return r;
    }

    // Signalling only (no WinHTTP call, no lock, no wait): safe from the game thread.
    // The event is the single source of truth, so any interleaving of Cancel/ClearCancel leaves one
    // consistent state (whichever ran last), never "flag says go, event says stop" or the reverse.
    void HarmonyClient::Cancel() { ::SetEvent(impl_->cancelEvt); }
    void HarmonyClient::ClearCancel() { ::ResetEvent(impl_->cancelEvt); }

    long LiveRequestContextsForTests() { return g_liveReqCtx.load(); }

    StatusResult HarmonyClient::Status(std::stop_token a_st) { return impl_->DoStatus(a_st); }

    Result HarmonyClient::ReportPlaying(std::string_view a_profile, std::string_view a_type, std::string_view a_service,
                                        std::string_view a_id, std::stop_token a_st) {
        return impl_->DoPlaying(a_profile, a_type, a_service, a_id, a_st);
    }

    Result HarmonyClient::PutProfile(std::string_view a_name, std::string_view a_json, std::stop_token a_st) {
        return impl_->DoPutProfile(a_name, a_json, a_st);
    }

    NextResult HarmonyClient::Next(std::string_view a_profile, std::string_view a_type, std::string_view a_sources, int a_count,
                                   bool a_peek, std::string_view a_container, std::stop_token a_st) {
        return impl_->DoNext(a_profile, a_type, a_sources, a_count, a_peek, a_container, a_st);
    }

    SearchResult HarmonyClient::Search(std::string_view a_query, std::string_view a_kinds, int a_limit, std::stop_token a_st) {
        return impl_->DoSearch(a_query, a_kinds, a_limit, a_st);
    }

    DownloadResult HarmonyClient::DownloadAudio(std::string_view a_audioUrl, const fs::path& a_destFile,
                                                double a_expectedDurationS, std::stop_token a_st) {
        return impl_->DoDownload(a_audioUrl, a_destFile, a_expectedDurationS, a_st);
    }

}  // namespace MJ::Net
