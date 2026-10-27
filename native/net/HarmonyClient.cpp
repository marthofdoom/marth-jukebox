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

        // Cancellable sleep in 50 ms slices.
        bool SleepOrCancel(int a_ms, const std::function<bool()>& a_cancelled) {
            auto left = a_ms;
            while (left > 0) {
                if (a_cancelled()) return false;
                const int step = (std::min)(left, 50);
                std::this_thread::sleep_for(std::chrono::milliseconds(step));
                left -= step;
            }
            return !a_cancelled();
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
        HINTERNET           session = nullptr;
        std::atomic<bool>   cancelled{ false };
        std::mutex          hmx;  // guards curConn/curReq
        HINTERNET           curConn = nullptr;
        HINTERNET           curReq  = nullptr;

        ~Impl() { CloseSession(); }

        void CloseSession() {
            AbortCurrent();
            if (session) {
                ::WinHttpCloseHandle(session);
                session = nullptr;
            }
        }

        void AbortCurrent() {
            std::lock_guard l(hmx);
            if (curReq) {
                ::WinHttpCloseHandle(curReq);
                curReq = nullptr;
            }
            if (curConn) {
                ::WinHttpCloseHandle(curConn);
                curConn = nullptr;
            }
        }

        bool Cancelled(const std::stop_token& a_st) const { return cancelled.load() || a_st.stop_requested(); }

        // One request/response. Handles live in curConn/curReq so Cancel() can close them from
        // another thread; the Exchange keeps local copies for its own calls.
        struct Exchange {
            Impl*        o;
            HINTERNET    conn = nullptr;
            HINTERNET    req  = nullptr;
            int          status = 0;
            std::int64_t contentLength = -1;
            int          retryAfterS = 0;
            explicit Exchange(Impl* a_o) : o(a_o) {}
            Exchange(const Exchange&) = delete;
            ~Exchange() { Close(); }
            void Close() {
                std::lock_guard l(o->hmx);
                if (req && o->curReq == req) {
                    ::WinHttpCloseHandle(req);
                    o->curReq = nullptr;
                }
                if (conn && o->curConn == conn) {
                    ::WinHttpCloseHandle(conn);
                    o->curConn = nullptr;
                }
                req = conn = nullptr;
            }
        };

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
            if (Cancelled(a_st)) {
                r.error   = Error::kCancelled;
                r.message = "cancelled";
                return r;
            }
            a_ex.conn = ::WinHttpConnect(session, wHost.c_str(), base.port, 0);
            if (!a_ex.conn) return fail(::GetLastError());
            {
                std::lock_guard l(hmx);
                curConn = a_ex.conn;
            }
            const std::wstring wTarget = Widen(a_target);
            a_ex.req = ::WinHttpOpenRequest(a_ex.conn, a_method, wTarget.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, base.scheme == "https" ? WINHTTP_FLAG_SECURE : 0);
            if (!a_ex.req) return fail(::GetLastError());
            {
                std::lock_guard l(hmx);
                curReq = a_ex.req;
            }
            if (Cancelled(a_st)) {  // Cancel() may have run before the handles were registered
                r.error   = Error::kCancelled;
                r.message = "cancelled";
                return r;
            }
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            ::WinHttpSetOption(a_ex.req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
            ::WinHttpSetTimeouts(a_ex.req, cfg.connectTimeoutMs, cfg.connectTimeoutMs, cfg.sendTimeoutMs, a_recvTimeoutMs);

            std::wstring headers = L"Accept: ";
            headers += a_accept;
            headers += L"\r\n";
            headers += wKeyHeader;
            if (a_body) headers += L"Content-Type: application/json\r\n";

            LPVOID      data = a_body ? const_cast<char*>(a_body->data()) : WINHTTP_NO_REQUEST_DATA;
            const DWORD dlen = a_body ? static_cast<DWORD>(a_body->size()) : 0;
            if (!::WinHttpSendRequest(a_ex.req, headers.c_str(), static_cast<DWORD>(-1), data, dlen, dlen, 0)) {
                return fail(::GetLastError());
            }
            if (!::WinHttpReceiveResponse(a_ex.req, nullptr)) return fail(::GetLastError());

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

        // Reads up to a_cap bytes of the body (stops at end of body or the cap).
        Result ReadBody(Exchange& a_ex, std::string& a_out, std::size_t a_cap, const std::stop_token& a_st) {
            Result r;
            std::vector<char> buf(kChunk);
            while (true) {
                if (Cancelled(a_st)) {
                    r.error   = Error::kCancelled;
                    r.message = "cancelled";
                    return r;
                }
                DWORD n = 0;
                if (!::WinHttpReadData(a_ex.req, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
                    r.error = TransportError(::GetLastError(), a_st, r.message);
                    return r;
                }
                if (n == 0) return r;
                if (a_out.size() + n > a_cap) {
                    a_out.append(buf.data(), a_cap - a_out.size());
                    return r;  // enough; remaining body is dropped with the connection
                }
                a_out.append(buf.data(), n);
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
            Exchange          ex(this);
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
            std::stop_callback cb(a_st, [this] { AbortCurrent(); });
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
            std::stop_callback cb(a_st, [this] { AbortCurrent(); });
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

        NextResult DoNext(std::string_view a_profile, std::string_view a_type, std::string_view a_sources, int a_count,
                          bool a_peek, std::string_view a_container, const std::stop_token& a_st) {
            std::stop_callback cb(a_st, [this] { AbortCurrent(); });
            NextResult         out;
            json req = { { "profile", std::string(a_profile) }, { "type", std::string(a_type) } };
            if (!a_sources.empty()) req["sources"] = std::string(a_sources);
            if (a_count > 0) req["count"] = (std::min)(a_count, 10);
            if (a_peek) req["peek"] = true;
            if (!a_container.empty()) req["container"] = std::string(a_container);
            const std::string reqBody = req.dump();
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
            std::stop_callback cb(a_st, [this] { AbortCurrent(); });
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
            std::stop_callback cb(a_st, [this] { AbortCurrent(); });
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
            const auto isCancelled = [&] { return Cancelled(a_st); };

            while (true) {
                if (Cancelled(a_st)) return fail(Error::kCancelled, "cancelled");
                const auto t0 = std::chrono::steady_clock::now();
                Exchange   ex(this);
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
                    if (!SleepOrCancel(waitS * 1000, isCancelled)) return fail(Error::kCancelled, "cancelled");
                    continue;
                }
                if (ex.status != 200) {
                    std::string body;
                    ReadBody(ex, body, kChunk, a_st);
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
                FileHandle fh;
                fh.h = ::CreateFileW(guard.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (fh.h == INVALID_HANDLE_VALUE) {
                    return fail(Error::kIo, std::format("cannot create .part file (error {})", static_cast<unsigned long>(::GetLastError())));
                }
                std::vector<char> buf(kChunk);
                std::int64_t      got = 0;
                while (true) {
                    if (Cancelled(a_st)) return fail(Error::kCancelled, "cancelled");
                    DWORD n = 0;
                    if (!::WinHttpReadData(ex.req, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
                        const DWORD le = ::GetLastError();
                        Log("GET", target, 0, "read failed", MsSince(t0));
                        std::string m;
                        const Error e = TransportError(le, a_st, m);
                        if (e == Error::kUnreachable) {
                            return fail(Error::kTruncated, std::format("connection lost after {} of {} bytes ({})", got, ex.contentLength, m));
                        }
                        return fail(e, std::format("{} after {} of {} bytes", m, got, ex.contentLength));
                    }
                    if (n == 0) break;
                    got += n;
                    if (got > ex.contentLength) return fail(Error::kBadResponse, "body longer than Content-Length");
                    DWORD written = 0;
                    if (!::WriteFile(fh.h, buf.data(), n, &written, nullptr) || written != n) {
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

        o.CloseSession();
        o.cfg        = a_cfg;
        o.base       = base;
        o.prefix     = prefix;
        o.wHost      = Widen(base.host);
        o.wUserAgent = Widen(a_cfg.userAgent);
        o.wKeyHeader = a_cfg.personalKey.empty() ? std::wstring() : L"X-Harmony-Key: " + Widen(a_cfg.personalKey) + L"\r\n";
        o.session    = ::WinHttpOpen(o.wUserAgent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
        if (!o.session) {
            o.configured = false;
            return bad(std::format("WinHttpOpen failed (error {})", static_cast<unsigned long>(::GetLastError())));
        }
        o.configured = true;
        spdlog::info("[net] configured: {}://{}:{}{} (key {})", base.scheme, base.host, base.port, prefix,
                     a_cfg.personalKey.empty() ? "not set" : "set");
        return r;
    }

    void HarmonyClient::Cancel() {
        impl_->cancelled.store(true);
        impl_->AbortCurrent();
    }
    void HarmonyClient::ClearCancel() { impl_->cancelled.store(false); }

    StatusResult HarmonyClient::Status(std::stop_token a_st) { return impl_->DoStatus(a_st); }

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
