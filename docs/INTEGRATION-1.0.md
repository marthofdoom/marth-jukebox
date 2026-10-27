# 1.0 integration spec (coordinator, 2026-10-27)

Inputs already on main: M0b/M0c engine results (docs/M0B-RESULTS.md, docs/M0C-RESULTS.md), settings + environment
map (docs/ENVIRONMENTS.md), Harmony client (docs/NET-CLIENT.md), DESIGN.md (sections 3, 8.1-8.8), menu research
(docs/MENU-FRAMEWORK.md), acceptance list (docs/ROADMAP-1.0.md). The seam between the two authors is
`native/app/AppApi.h` (do not change it without the coordinator).

## A. Playback core (Opus author, tier A: engine memory, file/rename protocol, threading)
Branch `core-playback`. Directories: `native/pool/` (pool, cache, FLAC decode, recent list, worker), generalise
`native/retarget/` and `native/slots/`, implement `native/app/App.cpp`, wire `plugin.cpp`. Replace the M0b/M0c
experiment code (hardcoded Tundra, tones, .orig experiment) by the real thing; keep the stall detector (log
autosave-sized gaps at debug, > 5 s at warn), play-event detection and trackHistory logging (debug level).

1. **Slots/ESP.** `tools/make_esp.py` generates K=24 slot wrappers (`MJ_SLOT_00..23`, ANAM `Data\Music\Harmony\slot_NN.wav`).
   An effective environment (Settings group or group.sub that is active) gets one slot, assigned deterministically
   at kDataLoaded. All replaceable (Single/Palette) entries of every MUSC type that Classify maps to that environment
   point at that slot's wrapper (wrapper A of M0c, in place, array size never changed, Silent entries kept, writes
   only on the main thread while the type is not current/queued and status is Inactive/Finished). Diegetic and unmapped
   types are never touched. More active environments than slots: the extras stay Vanilla, logged.
2. **Pool per active environment** (size from settings, default 3). Entries: a validated audio file in the cache dir
   (filename = hash of service+id, NEVER the raw id), recorded in a JSON index (service, id, title, artist, duration_s,
   bytes, sources string, fingerprint of the selector). Persist across sessions; revalidate at load (file present, size matches);
   drop a pool when its selector/sources fingerprint changed. Refill on the worker with `HarmonyClient::Next(count=1..)` +
   `Download`; accept only if the preview guard passes (decoded duration >= 90% of duration_s when > 60 s, DESIGN 8.8) and the
   file decodes. Only the PLAYED track is replaced; unplayed ones persist. Plugin-side recent-played list (last 50
   service:id per environment, persisted) rejects repeats that the server returned anyway (ask for count 3 and filter).
   Prefer the FLAC container when `status` says supported, else WAV. LRU cap `CacheCapMB` over the whole cache, never
   evicting pooled or slotted files. All network/file IO on the worker; back off exponentially after kTimeout/kUnreachable
   (docs/NET-CLIENT.md). `sources` string per call: `library` unless the environment's AllowStreaming, then `library,ytmusic`
   (+`,qobuz` only if `[General] AllowQobuz=1`: add that key + a helper in core-settings, with tests); Harmony older than
   the sources list: fall back to `library`/`all` exactly as the capability check allows, and never stream without consent.
3. **Slot fill = M0c protocol.** Play event = sound handle or index change of the slot wrapper while its type is current.
   After the event has been stable ~1.5 s, the worker writes the NEXT pool track into the slot: decode FLAC (dr_flac, header-only
   MIT/public-domain, vendored in `third_party/` with its licence) or copy WAV to `slot_NN.wav.tmp` in the canonical PCM WAV
   format the engine plays, flush, size-check, `MoveFileExW(REPLACE_EXISTING)` over the (possibly playing) file. The track
   heard is the one placed at the previous event; the event marks the played track as served, removes it from the pool
   and queues a replacement. Prefill every slot before the first play (kDataLoaded + worker). If the pool is empty or a
   slot is not ready when its type is about to start, restore that environment's original tracks (documented degrade,
   loud log) and re-target when ready. Never overwrite anything the plugin did not create; log every swap decision.
   Remember-place per environment: ON = engine default (resume of the same handle/index); OFF mechanism is an open
   research item: implement only if a safe read-only-driven approach exists, otherwise the setting is accepted, persisted
   and documented as "always on" in 1.0 (say so in the README).
4. **Settings/profile.** Load `SettingsStore` at kDataLoaded; on connect and on every ApplySettingsChanged: ResolveEnvironment
   for each, BuildProfile (published types only, environments that failed to publish stay Vanilla), `PutProfile`, report
   problems in `EnvStatus.state`. Kill switches: INI `[Features]` (Retarget, Network, Flac, RememberPlace) plus the
   file `Data/SKSE/Plugins/MarthJukebox.noretarget`; first-run with no ServerUrl = everything Vanilla, nothing on the network.
   `ReportPlaying` best-effort on each play event.
5. **AppApi.** Implement every function in `native/app/AppApi.h`; Connection.message is plain language and never contains
   the key; search maps kinds to `/api/search` and the library views, filters Qobuz unless allowed.
6. **Tests.** Pure logic (pool index persistence/revalidation, recent list, cache eviction, sources-string helper,
   slot assignment, FLAC->WAV decode against a golden file) in `mj_core_tests`; engine-facing code stays thin.
7. **Docs.** Update DESIGN.md (replace the 'proposed' language with what is built), docs/NEXUS-COMPLIANCE.md (network surface now
   includes `/api/game/playing`, search), add docs/INTEGRATION-TEST.md (the LoreRim test script for marth).

## B. Menu (Sonnet author) -- branch `menu-page`, directory `native/menu/`
Implements the SKSE Menu Framework page per docs/MENU-FRAMEWORK.md using ONLY `native/app/AppApi.h` and
`config/Settings.h` (SettingsStore) + `categories/Environments.h` (ListGroups for the rows). Compile-linkable without
the core: a `native/menu/StubApp.cpp` built only for the menu test target is NOT shipped; the real App.cpp comes from
the core branch at integration. Requirements: guided first run (3 steps: server URL + key, Test connection, pick a
preset), key field masked with a Show toggle, Test button with the plain-language `Connection.message`, one row per active-able
environment (label, mode combo Vanilla/Genre/Artist/Playlist/Query, search box + result list from SearchAsync, selected
value shown, "Allow streaming (YouTube Music)" checkbox default off, "Remember track place" checkbox, "Reset to vanilla"),
presets (e.g. "Calm exploration + epic combat" = genre selectors; presets only set Settings fields), pool status line
per environment from GetEnvStatus ("2 of 3 ready" + state + nowPlaying), "Show next up" (PeekAsync titles; this is the 1.0 preview, no audio),
pool-size combo (3/5/8), cache size + "Clear cache", feature kill-switch toggles under an Advanced header, log level.
Every change: SettingsStore::Update -> Save -> App::ApplySettingsChanged (debounce text fields: apply on Enter/blur,
searches 400 ms after typing stops). Gamepad has no text entry: keep every setting reachable by list/combos; note it in the
page. No game-thread blocking; the page only reads cached state. Log `GetMenuFrameworkVersion()` at registration and
handle the framework being absent (plugin works headless with the INI). Registration-order risk: our DLL name sorts
before "SKSE...", verify the registration approach in MENU-FRAMEWORK.md. Tests: pure view-model logic (preset
application, field validation, debounce state machine) in a console test target; no ImGui in tests.

## C. Coordinator (me)
Merge order: `menu-page` and `core-playback` branches (separate worktrees), Opus review of each, merge, one CI build,
README.md (full instructions, release checklist in CLAUDE.md), deploy to LoreRim, ONE test cycle with marth
(docs/INTEGRATION-TEST.md), fixes, tag v1.0.0 + GitHub release (published). Production Harmony must be on 1.0.13+
(marth updates it) before the live test; plugin must also degrade cleanly against an older Harmony.
