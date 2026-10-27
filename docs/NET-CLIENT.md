# Harmony HTTP client (`native/net/HarmonyClient.{h,cpp}`, namespace `MJ::Net`)

A self-contained, **blocking** client for the Harmony game-source API
(`harmony/docs/api/game-source.md`). WinHTTP transport (system DLL; works under Wine/Proton),
nlohmann-json for replies, spdlog for logging. No engine or CommonLib calls.

## Threading rule

* Every call blocks (network + disk). **Worker threads only**, never the game thread or a UI callback.
* One instance is used by ONE worker at a time. Only `Cancel()` / `ClearCancel()` and the
  `std::stop_token` you pass are safe from another thread while a call runs.
* `Cancel()` is sticky (later calls return `kCancelled`) until `ClearCancel()`. The cancel state is one
  manual-reset event (no separate flag), so any interleaving of `Cancel()`/`ClearCancel()` ends in the state
  of whichever ran last, and a call whose pre-check sees "cancelled" sends nothing.
* **Job cancellation = a per-job `std::stop_source`.** Use `Cancel()` only for shutdown / "Harmony away":
  a `ClearCancel()` on the worker silently swallows a `Cancel()` issued just before it.
* Cancellation only **signals**: `Cancel()` / `ClearCancel()` set or reset an event, and the
  `stop_callback` registered for your token only sets a per-call event. No WinHTTP call ever runs on the
  cancelling thread, so the game thread may cancel. It must **not** then wait for the worker (no join or
  `future.get()` on the game thread); let the worker post its result.
* WinHTTP runs in **async mode** (`WINHTTP_FLAG_ASYNC` + a status callback). The worker is the only thread
  that touches the request/connect handles; every blocking point (send, the server's `wait=` phase inside
  `WinHttpReceiveResponse`, each body read, the 202 back-off) is a wait on {operation done, cancel, stop}.
  On cancel the worker closes its own handles and returns `kCancelled`.
* What is guaranteed after a cancel: the **call** returns `kCancelled` as soon as the worker thread is
  scheduled (no network round trip, no WinHTTP timeout), from any phase. What is NOT guaranteed: when the
  socket underneath is actually torn down; that is up to WinHTTP (or Wine's WinHTTP). Anything an
  abandoned operation may still touch (read buffer, request body, header block, completion event) lives in a
  reference-counted per-request context that is freed only on `WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING`, so
  a late completion never writes into freed memory, and the worker never waits for it.
* Timeouts are enforced by the worker too: each wait has a budget of WinHTTP's own timeout + 0.5 s, after
  which the worker abandons the request with `kTimeout` and a `[net]` warning. On Windows WinHTTP's timeout
  fires first. Under Wine 10 (traced with `WINEDEBUG=+winhttp`) the async send task waits for the reply
  itself and **no** receive timeout fires, so there a server that never answers is bounded by the worker's
  send-phase budget: 2 x connect + send + receive timeout + 0.5 s (defaults: 5+5+15+30+0.5 s; a download
  uses its longer receive timeout).
* **Consumer contract: back off after `kTimeout` / `kUnreachable`** (exponential, e.g. 2 s doubling to a few
  minutes). Under Wine an abandoned request's send task never times out against a server that accepts but
  never answers: every such `kTimeout` or cancel leaves a WinHTTP thread + socket + 64 KiB context alive until
  that server answers or closes. A tight retry loop against a hung server would grow without bound.
* Wine 10 (local run of the CI exe, 2026-10-27): all cancel tests pass (wait phase, mid-body, racing start,
  two-client stress). Proton itself is still to be run.
* Nothing throws across the API; every call returns a struct derived from `Result`
  (`error`, `httpStatus`, `code`, `message`, `retryAfterS`).

## API

| Call | Request | Returns |
|------|---------|---------|
| `Configure(Config)` | validates base URL (`http`/`https`, host, optional port and path prefix; no userinfo, query, IPv6 literal), key (printable ASCII), timeouts | `Result` |
| `Status()` | `GET /api/game/status` | `StatusResult` {reachable, enabled, ffmpeg, containers (missing => `["wav"]`), cache{bytes,files,inProgressBytes,cap}, servicesReported + library/ytmusic/qobuzUsable (`services`; its presence = the server takes a `sources` list), excludedServices} |
| `PutProfile(name, json)` | `PUT /api/game/profiles/{name}` (name validated locally) | `Result` |
| `Next(profile, type, sources, count, peek, container)` | `POST /api/game/next` (empty `sources`/`container` omitted) | `NextResult` {tracks[], poolSize, type, requestedType, sources}; tracks without id or audio_url are dropped |
| `Search(query, kinds, limit)` | `GET /api/search?q=..&kinds=..&limit=..` | `SearchResult` {tracks[], artists[], albums[], playlists[]} (tolerant parse; the endpoint is not in game-source.md; the server ignores `limit`) |
| `LibraryView(view, limit)` | `GET /api/library/view?view=genres\|artists&limit=..` | `BrowseResult` {enabled, items[]}: genres (id = title = name, service `local`), artists (id, name) of the live, federated Library |
| `Playlists()` | `GET /api/playlists` | `BrowseResult` {items[]}: the signed-in services' own playlists |
| `ReportPlaying(profile, type, service, id)` | `POST /api/game/playing` `{profile, type, service, id}` (all four required; nothing else, the server rejects unknown keys) | `Result`; best effort: a failure is only a returned error |
| `DownloadAudio(audio_url, destFile, expectedDurationS, stop_token)` | `GET` the audio URL + `&wait=<n>` | `DownloadResult` {audio info, bytes} |
| `ProbeAudioFile(path, AudioInfo&)` | none (header parse only) | `Result` |

`Config`: `baseUrl`, `personalKey` (header `X-Harmony-Key`, never logged), `userAgent`
(caller passes `MarthJukebox/<version>`), `connect/send/receiveTimeoutMs`, `waitS` (default 30),
`downloadDeadlineS` (default 180), `maxDownloadBytes` (default 256 MiB, 4096 .. 64 GiB). Proxies are not used (`WINHTTP_ACCESS_TYPE_NO_PROXY`; Harmony is a
LAN/Tailnet server). Redirects are never followed (3xx => `kRedirect`): every request sets both
`WINHTTP_OPTION_REDIRECT_POLICY_NEVER` and `WINHTTP_DISABLE_REDIRECTS`, and if WinHTTP refuses either
(or the timeouts) the request is not sent (`kBadConfig`), so the key can never follow a `Location` to
another host.

Request bodies are built with nlohmann's strict UTF-8 check: a `profile` / `type` / `sources` /
`container` / playing field that is not valid UTF-8 is rejected locally (`kBadRequest`, nothing sent).

`Track` strings (notably `service` and `id`) are **untrusted** server data: an id may contain `:` (an NTFS
alternate data stream), `/`, `\`, `..`. The cache layer must hash `(service, id)` into file names and never
use them raw.

The base URL may be `http://`; then the key travels in clear text. Fine on a LAN or inside a Tailnet
(already encrypted); anywhere else use `https://`.

### Download protocol

1. Resolve `audio_url`: root-relative or relative URLs are appended to the base prefix; an absolute URL
   is accepted only for the **same scheme, host and port** as the base (the key must not leave the
   configured server). Query is kept as given; `&wait=<waitS>` is added unless already present.
2. `202` (`{"status":"transcoding"}`): honour `Retry-After` (1..30 s, default 2) and retry until
   `downloadDeadlineS` (then `kDeadline`) or cancel. The deadline covers the waiting phase; streaming is
   bounded by the per-read receive timeout and cancel.
3. `200`: needs a `Content-Length`, which must be <= `maxDownloadBytes` (else `kTooLarge`, checked before
   the `.part` is created; the byte count read is bounded by it as well); stream in 64 KiB chunks to `<dest>.part`; the byte count must equal
   it (`kTruncated` otherwise); `FlushFileBuffers`, close.
4. Validate (below). Only then `MoveFileExW(.part -> dest, REPLACE_EXISTING)`. The `.part` is deleted on
   every failure; an existing `dest` is never touched by a failed download.

`Range` is intentionally unused: a download is always the whole file (the server supports ranges, but a
resumed partial file would need its own validation story; a failed download simply starts over).

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
| `kBadConfig` | `Configure` rejected the input, the client was never configured, or WinHTTP refused a safety option (redirect policy, timeouts) so the request was not sent |
| `kUnreachable` | DNS / connect / TLS / other WinHTTP failure |
| `kTimeout` | WinHTTP resolve/connect/send/receive timeout, or the client-enforced timeout (WinHTTP timeout + 0.5 s; the bound under Wine) |
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
| `kTooLarge` | download `Content-Length` (or bytes read) above `maxDownloadBytes` |

## Logging

One line per request: `[net] GET /api/game/next -> 200 (123 ms)`; transport failures log the error name
instead of a status. The key is never logged (it is only a header), and any `key=` query value in a
logged URL is replaced with `***`.

## Tests (`mj_net_tests`, CMake option `MJ_BUILD_TESTS=ON`, run by CI)

`native/tests/net_tests.cpp` (+ `native/tests/shim/PCH.h`, which stands in for the CommonLib PCH so the
test exe does not link CommonLibSSE). A scripted 127.0.0.1 HTTP/1.1 server (Winsock, test-only) answers
each test; WAV/FLAC fixtures are synthesised in memory. Never contacts a real Harmony.

status_json, status_no_containers_field, status_services, browse, next_json, next_404_no_tracks, http_error_mapping (401, 403,
404 x4, 400, 413, 502, 503 x4 incl. Retry-After, 504, 500), unreachable, receive_timeout, put_profile,
search, download_wav_ok, download_replace_and_preserve, download_202_then_200, download_202_deadline,
download_truncated_body, download_preview_30s_vs_224s, download_flac_streaminfo, download_bad_audio,
redirect_rejected, cancel_mid_download, stop_token_mid_download, cancel_flag_sticky_and_clear,
key_header_sent_never_logged, audio_url_resolution (relative, absolute, prefix, foreign host),
config_validation, probe_audio_headers, redirect_never_leaks_key (301/302/303/307/308 to a second local
server: never contacted), non_utf8_fields_rejected, download_too_large (limit and a 300 MiB header with no
body), download_body_longer_than_content_length (trailing bytes; chunked body contradicting
Content-Length), report_playing, cancel_in_wait_phase / stop_token_in_wait_phase (server holds the request,
client inside `WinHttpReceiveResponse`; client reusable afterwards), cancel_racing_start (300 cancels at
0..2 ms around the call start), cancel_stress_two_clients (two clients, own servers, 150 cancel/start
cycles each over status and downloads; results must carry their own server's data).

cancel_clear_interleave (Cancel/ClearCancel from two threads at once, 2000 rounds: a `kCancelled` call never
sent a request). After EVERY test the harness waits for the live request-context count to return to 0
(proves `HANDLE_CLOSING` delivery, no context leak), on Windows and under Wine.

CI uploads `mj_net_tests.exe` with the DLL artifact so the same binary can be run under Wine/Proton. CI also
builds an `/fsanitize=address` copy of `mj_net_tests` and runs it as a separate, non-blocking step.
