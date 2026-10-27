// Harmony HTTP client (game-source API). See docs/NET-CLIENT.md.
//
// THREADING RULE (hard): every call here BLOCKS (network + disk). Call it ONLY from a worker
// thread, never from the game thread, never from a render/UI callback. Nothing in this module
// touches the engine or CommonLib; the only outside dependency is spdlog logging.
//
// One client instance is used by ONE worker at a time (calls are not re-entrant). The only
// members safe to call from another thread while a call is in flight are Cancel() and the
// stop_token you pass in. API calls never throw; failures come back as Result::error.
//
// Secrets: the personal key is sent as the X-Harmony-Key header and is never logged. Any
// "key=" query value in a logged URL is replaced by "***".
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace MJ::Net {

    enum class Error {
        kOk = 0,
        kBadConfig,          // base URL / key / timeouts rejected by Configure()
        kUnreachable,        // DNS, connect, TLS or connection failure
        kTimeout,            // a WinHTTP resolve/connect/send/receive timeout fired
        kCancelled,          // Cancel() or the caller's stop_token
        kRedirect,           // 3xx (redirects are never followed)
        kBadKey,             // 401
        kDisabled,           // 403 (game_source_enabled is false on the server)
        kBadRequest,         // 400 and other 4xx not listed here (405, 409, 411, 413, 416 ...)
        kNoTracks,           // 404 no_tracks
        kUnknownType,        // 404 unknown_type
        kUnknownProfile,     // 404 unknown_profile
        kNotFound,           // any other 404
        kSourceUnavailable,  // 502 (source_unavailable, transcode_failed, bad_source, too_long)
        kBusy,               // 503 busy / cache_busy (retry after retryAfterS)
        kCacheFull,          // 503 cache_full
        kFfmpegMissing,      // 503 ffmpeg_missing
        kServerError,        // any other 5xx
        kGatewayTimeout,     // 504 transcode_timeout
        kBadResponse,        // unparsable JSON, missing Content-Length, unexpected status
        kDeadline,           // DownloadAudio: still 202 transcoding at the overall deadline
        kIo,                 // local file create/write/rename failure
        kTruncated,          // body shorter than Content-Length (connection lost)
        kBadAudio,           // downloaded file is not a consistent WAV/FLAC
        kPreviewOrTruncated  // duration < 90% of the expected duration (Qobuz 30 s preview)
    };

    const char* ErrorName(Error a_err);

    struct Config {
        std::string baseUrl;     // http(s)://host[:port][/prefix]; nothing else is accepted
        std::string personalKey; // optional; sent as X-Harmony-Key, never logged
        std::string userAgent = "MarthJukebox/0.1.0";  // caller sets "MarthJukebox/<version>"
        int         connectTimeoutMs = 5000;   // also the DNS-resolve timeout
        int         sendTimeoutMs    = 15000;
        int         receiveTimeoutMs = 30000;  // per read; a download's first response also gets waitS + 10 s
        int         waitS            = 30;     // `wait=` appended to audio URLs (server blocks up to this long)
        int         downloadDeadlineS = 180;   // overall budget incl. 202 retries
    };

    struct Result {
        Error       error = Error::kOk;
        int         httpStatus = 0;     // 0 when no HTTP answer was received
        std::string code;               // Harmony "code" field when the body carried one
        std::string message;            // human text, safe to log (no secrets)
        int         retryAfterS = 0;    // Retry-After header when present
        bool Ok() const { return error == Error::kOk; }
    };

    struct CacheInfo {
        std::int64_t bytes = 0;
        std::int64_t files = 0;
        std::int64_t inProgressBytes = 0;
        std::int64_t cap = 0;
    };

    struct StatusResult : Result {
        bool                     reachable = false;  // an HTTP answer was received (true also for 401/403)
        bool                     enabled = false;
        bool                     ffmpeg = false;
        std::vector<std::string> containers;         // missing field => {"wav"}
        CacheInfo                cache;
    };

    struct Track {
        std::string  service, id, title, artist, album;
        std::int64_t durationS = 0;
        std::string  audioUrl;       // as returned (relative to the server)
        std::string  qualityLabel;   // quality.label, empty when quality is null
        std::string  sourceService, sourceLabel;
        bool         fromLibrary = false;
    };

    struct NextResult : Result {
        std::vector<Track> tracks;
        std::int64_t       poolSize = 0;
        std::string        type;           // the type actually used ("default" after a fallback)
        std::string        requestedType;
        std::string        sources;
    };

    // One row of /api/search. Fields that a kind does not have stay empty/zero.
    struct SearchItem {
        std::string  service, id, title, artist, album;
        std::int64_t durationS = 0;
    };

    struct SearchResult : Result {
        std::vector<SearchItem> tracks, artists, albums;
    };

    struct AudioInfo {
        std::string   format;          // "wav" or "flac"
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::uint16_t bitsPerSample = 0;
        std::uint64_t totalFrames = 0;
        double        durationS = 0.0;
        std::uint64_t fileBytes = 0;
    };

    struct DownloadResult : Result {
        AudioInfo   audio;      // filled whenever the header could be parsed (also on kPreviewOrTruncated)
        std::int64_t bytes = 0; // bytes received
    };

    // Parses a WAV/FLAC header (no decoding). WAV: RIFF/data sizes must agree with the file size.
    // Returns kOk, kBadAudio or kIo. Pure file IO; usable without a client.
    Result ProbeAudioFile(const std::filesystem::path& a_file, AudioInfo& a_out);

    class HarmonyClient {
    public:
        HarmonyClient();
        ~HarmonyClient();
        HarmonyClient(const HarmonyClient&) = delete;
        HarmonyClient& operator=(const HarmonyClient&) = delete;

        // Validates and stores the config. Not safe while a call is running on another thread.
        Result Configure(const Config& a_cfg);

        // Aborts the call in flight (if any) and makes later calls return kCancelled until
        // ClearCancel(). Cheap, thread-safe, callable from any thread.
        void Cancel();
        void ClearCancel();

        StatusResult Status(std::stop_token a_st = {});
        Result       PutProfile(std::string_view a_name, std::string_view a_json, std::stop_token a_st = {});
        // a_sources / a_container: empty string = field omitted (server default).
        NextResult   Next(std::string_view a_profile, std::string_view a_type, std::string_view a_sources,
                          int a_count, bool a_peek, std::string_view a_container, std::stop_token a_st = {});
        SearchResult Search(std::string_view a_query, std::string_view a_kinds, int a_limit,
                            std::stop_token a_st = {});
        // Downloads a_audioUrl (relative to the base URL, or absolute on the SAME host:port) to a_destFile.
        // The final file only ever appears after the byte count and the audio header validated.
        // a_expectedDurationS <= 0 (or <= 60) skips the preview/truncation check.
        DownloadResult DownloadAudio(std::string_view a_audioUrl, const std::filesystem::path& a_destFile,
                                     double a_expectedDurationS, std::stop_token a_st = {});

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace MJ::Net
