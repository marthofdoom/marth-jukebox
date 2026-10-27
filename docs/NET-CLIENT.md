# Harmony HTTP client (`native/net/HarmonyClient.{h,cpp}`, namespace `MJ::Net`)

A self-contained, **blocking** client for the Harmony game-source API
(`harmony/docs/api/game-source.md`). WinHTTP transport (system DLL; works under Wine/Proton),
nlohmann-json for replies, spdlog for logging. No engine or CommonLib calls.

## Threading rule

* Every call blocks (network + disk). **Worker threads only**, never the game thread or a UI callback.
* One instance is used by ONE worker at a time. Only `Cancel()` / `ClearCancel()` and the
  `std::stop_token` you pass are safe from another thread while a call runs.
* `Cancel()` is sticky (later calls return `kCancelled`) until `ClearCancel()`. It closes the in-flight
  WinHTTP request handle, so a blocked read returns at once instead of waiting for a timeout.
* Nothing throws across the API; every call returns a struct derived from `Result`
  (`error`, `httpStatus`, `code`, `message`, `retryAfterS`).

## API

| Call | Request | Returns |
|------|---------|---------|
| `Configure(Config)` | validates base URL (`http`/`https`, host, optional port and path prefix; no userinfo, query, IPv6 literal), key (printable ASCII), timeouts | `Result` |
| `Status()` | `GET /api/game/status` | `StatusResult` {reachable, enabled, ffmpeg, containers (missing => `["wav"]`), cache{bytes,files,inProgressBytes,cap}} |
| `PutProfile(name, json)` | `PUT /api/game/profiles/{name}` (name validated locally) | `Result` |
| `Next(profile, type, sources, count, peek, container)` | `POST /api/game/next` (empty `sources`/`container` omitted) | `NextResult` {tracks[], poolSize, type, requestedType, sources}; tracks without id or audio_url are dropped |
| `Search(query, kinds, limit)` | `GET /api/search?q=..&kinds=..&limit=..` | `SearchResult` {tracks[], artists[], albums[]} (tolerant parse; the endpoint is not in game-source.md) |
| `DownloadAudio(audio_url, destFile, expectedDurationS, stop_token)` | `GET` the audio URL + `&wait=<n>` | `DownloadResult` {audio info, bytes} |
| `ProbeAudioFile(path, AudioInfo&)` | none (header parse only) | `Result` |

`Config`: `baseUrl`, `personalKey` (header `X-Harmony-Key`, never logged), `userAgent`
(caller passes `MarthJukebox/<version>`), `connect/send/receiveTimeoutMs`, `waitS` (default 30),
`downloadDeadlineS` (default 180). Proxies are not used (`WINHTTP_ACCESS_TYPE_NO_PROXY`; Harmony is a
LAN/Tailnet server). Redirects are never followed (3xx => `kRedirect`).

### Download protocol

1. Resolve `audio_url`: root-relative or relative URLs are appended to the base prefix; an absolute URL
   is accepted only for the **same scheme, host and port** as the base (the key must not leave the
   configured server). Query is kept as given; `&wait=<waitS>` is added unless already present.
2. `202` (`{"status":"transcoding"}`): honour `Retry-After` (1..30 s, default 2) and retry until
   `downloadDeadlineS` (then `kDeadline`) or cancel. The deadline covers the waiting phase; streaming is
   bounded by the per-read receive timeout and cancel.
3. `200`: needs a `Content-Length`; stream in 64 KiB chunks to `<dest>.part`; the byte count must equal
   it (`kTruncated` otherwise); `FlushFileBuffers`, close.
4. Validate (below). Only then `MoveFileExW(.part -> dest, REPLACE_EXISTING)`. The `.part` is deleted on
   every failure; an existing `dest` is never touched by a failed download.

### Audio validation (DESIGN 8.8)

* WAV: `RIFF`/`WAVE`; `RIFF size + 8` must equal the file size; chunks are walked (extra chunks such as
  `LIST` are tolerated) until `data`; `fmt` must come first (PCM or extensible tag); `data` must fit in
  the file and be a whole number of frames; duration = dataBytes / (rate * channels * bits/8).
* FLAC: `fLaC`, first block STREAMINFO (34 bytes); rate, channels, bits, total samples. A total sample
  count of 0 (unknown) is rejected because the duration cannot be verified.
* If `expectedDurationS > 60` and measured duration < 90% of it: `kPreviewOrTruncated` (Qobuz 30 s
  previews), file deleted; the measured format/duration are still returned in `DownloadResult::audio`.
* Anything else unparsable: `kBadAudio`.

## Error table

| `Error` | Cause |
|---------|-------|
| `kBadConfig` | `Configure` rejected the input, or the client was never configured |
| `kUnreachable` | DNS / connect / TLS / other WinHTTP failure |
| `kTimeout` | WinHTTP resolve/connect/send/receive timeout |
| `kCancelled` | `Cancel()` or the caller's stop token |
| `kRedirect` | any 3xx |
| `kBadKey` | 401 |
| `kDisabled` | 403 |
| `kNoTracks` / `kUnknownType` / `kUnknownProfile` | 404 with that `code` |
| `kNotFound` | any other 404 |
| `kBadRequest` | 400 and other 4xx (405, 409, 411, 413, 416 ...), also local name/arg validation |
| `kSourceUnavailable` | 502 (`source_unavailable`, `transcode_failed`, `bad_source`, `too_long`) |
| `kBusy` | 503 `busy` / `cache_busy` (see `retryAfterS`) |
| `kCacheFull` | 503 `cache_full` |
| `kFfmpegMissing` | 503 `ffmpeg_missing` |
| `kServerError` | any other 5xx |
| `kGatewayTimeout` | 504 (`transcode_timeout`) |
| `kBadResponse` | unparsable JSON, 200 without `Content-Length`, unexpected status, foreign `audio_url` |
| `kDeadline` | still `202` at `downloadDeadlineS` |
| `kIo` | local file create/write/rename failure |
| `kTruncated` | body shorter than `Content-Length` (connection lost) |
| `kBadAudio` | downloaded file is not a consistent WAV/FLAC |
| `kPreviewOrTruncated` | duration < 90% of the expected duration (expected > 60 s) |

## Logging

One line per request: `[net] GET /api/game/next -> 200 (123 ms)`; transport failures log the error name
instead of a status. The key is never logged (it is only a header), and any `key=` query value in a
logged URL is replaced with `***`.

## Tests (`mj_net_tests`, CMake option `MJ_BUILD_TESTS=ON`, run by CI)

`native/tests/net_tests.cpp` (+ `native/tests/shim/PCH.h`, which stands in for the CommonLib PCH so the
test exe does not link CommonLibSSE). A scripted 127.0.0.1 HTTP/1.1 server (Winsock, test-only) answers
each test; WAV/FLAC fixtures are synthesised in memory. Never contacts a real Harmony.

status_json, status_no_containers_field, next_json, next_404_no_tracks, http_error_mapping (401, 403,
404 x4, 400, 413, 502, 503 x4 incl. Retry-After, 504, 500), unreachable, receive_timeout, put_profile,
search, download_wav_ok, download_replace_and_preserve, download_202_then_200, download_202_deadline,
download_truncated_body, download_preview_30s_vs_224s, download_flac_streaminfo, download_bad_audio,
redirect_rejected, cancel_mid_download, stop_token_mid_download, cancel_flag_sticky_and_clear,
key_header_sent_never_logged, audio_url_resolution (relative, absolute, prefix, foreign host),
config_validation, probe_audio_headers.
