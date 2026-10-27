# Review backlog (SEV-4/5 findings deferred, drained in one batch before a cut)

## M0a, raised against 61d98248bdbf216695c32481971daf9ebfdc329e (Opus 5.5 review)

- **SEV-5** `native/probe/Probe.cpp:290-300` (reviewer, verbatim): "The shutdown thread design is more complex
  than it needs to be." A static std::jthread guard joins in its destructor at DLL_PROCESS_DETACH. Acceptable:
  on normal exit Windows has already killed the thread and SKSE never unloads a plugin, but a detached/leaked
  read-only thread is simpler and loses nothing. Deferred; drain before the first release.

## core-settings, raised against 4a5f2c1d0e83de3d9cb7788a44e905dc04aef16d (Opus 5.5 review; deferred, not fixed)

- **SEV-4** `native/config/Settings.cpp` (Save/ReplaceFile; reviewer): mod-manager behaviour of the INI replace.
  MO2/Vortex virtual file systems, hardlinked or renamed INIs may make `MoveFileExW` replace break the link or
  write outside the intended overlay. Document in the README / handle when the menu author wires the INI path.
- **SEV-4** merge conflicts with branch `net-client` (reviewer): both branches edit `native/CMakeLists.txt`,
  `native/vcpkg.json` (nlohmann-json line) and `.github/workflows/native.yml` (test step) and define
  `MJ_BUILD_TESTS`. Resolve at integration; keep one `option()` and one vcpkg line.
- **SEV-5** (reviewer, round 1): minor items were not itemised in the hand-off message; nothing further to paste.
  (Authoring note: C4996 `getenv` warning in `tests/core/ProfileTests.cpp`.)

## core-settings, raised against 70979f19826ca7a905ec61e310ae141b859e9a84 (Opus 5.5 round-2 review; deferred)

- **SEV-4** (reviewer, verbatim): IsTableEdid over-blocking third-party MUSCombatBoss*/MUSExploreDLC*/DLC2MUS*
  (prefix rules are plugin-gated in Classify but IsTableEdid ignores the plugin), so the 'Other mods' UI must hide them.
- **SEV-4** (reviewer, verbatim): .bak rotating at each session's first Save when the PersonalKey line was lost.
- **SEV-5** (reviewer, verbatim): WriteFileDurable stale GetLastError on a 0-byte write.
- **SEV-5** (reviewer, verbatim): Ctx::Add logs raw other.<EDID>.
- **SEV-5** (reviewer, verbatim): leak test not running other.<key> through BuildProfile.
- **SEV-5** (reviewer, verbatim): comments-only INI flagged unusable and per-line warnings cleared.

## core-settings closing review (a862ccb) — Opus, verbatim, all deferrable

- SEV-4 `CopyDurable` no-overwrite is check-then-create (TOCTOU): uses `exists()` then `CREATE_ALWAYS`/`O_CREAT|O_TRUNC`. Fix: create-new flag in `WriteFileDurable` (`CREATE_NEW` / `O_CREAT|O_EXCL`), name loop moves to the next suffix on `ERROR_FILE_EXISTS`/`EEXIST`.
- SEV-4 a failed copy leaves a partial file: `.bak` `CREATE_ALWAYS` truncates the previous good backup before the write can fail (INI itself stays intact; Save refuses). Fix: write `.bak` as tmp then replace, or delete the partial on failure.
- SEV-5 misleading error text: copy failures say "temporary settings file".
- **CLOSED (core-playback, b3564bd + fix round)**: ENVIRONMENTS.md, NEXUS-COMPLIANCE.md and DESIGN 8.8 now all say `library` / `library,ytmusic`, Qobuz only with `AllowQobuz=1`. Original entry: SEV-4 docs disagree on Qobuz: docs/ENVIRONMENTS.md:277 ("`sources=all` (YouTube Music; never Qobuz)") vs ENVIRONMENTS.md:313 and DESIGN 8.8 (`all` includes Qobuz until Harmony accepts a source list) vs docs/NEXUS-COMPLIANCE.md:15 ("library ... plus Qobuz"). Fix in one docs commit; note Harmony game-pools now accepts a `sources` list and `game_excluded_services`, so streaming can be sent as `ytmusic` only.

## net-client, raised against 0e98fbb (Opus 5.5 review; deferred by the fix round, wording from the review brief)

Note: the `core-settings` branch also edits this file; merge both sections.

- **SEV-3** Cancel/stop on Wine unverified - upload mj_net_tests.exe as CI artifact and run under Proton.
  (The fix round moved WinHTTP to async mode so a cancel no longer depends on Wine unblocking a socket,
  and CI now uploads `mj_net_tests.exe` beside the DLL. A local Wine 10 run passes all cancel tests and
  showed Wine's async receive timeout never fires, now enforced by the client. Proton itself is still open.)
- **SEV-4** Test gaps: the tests only run on Windows (CI); no Wine/Proton run of `mj_net_tests` yet.
  (Update after the fix round: Wine 10 ran locally, 34/34 x3 at 28668cb; only the Proton run is open.)
- **SEV-5** `target.find("wait=")` also matches `await=` (a caller URL with `await=` would get no `wait=`).
- **SEV-5** `JInt` casts a JSON double to int64 with no range check (UB for huge or NaN-like values).
- **SEV-5** A download request can exceed `downloadDeadlineS` by about `recvMs + connect` (the deadline is
  only checked between 202 retries).
- **SEV-5** A 4 MiB JSON body can build a ~60 MB nlohmann DOM.
- **SEV-5** An `http://` base sends the key in clear text (documented: README / NEXUS-COMPLIANCE recommend
  `https://` off-LAN).

## core-playback, raised against b3564bd and e5bdb99 (Opus 5.5 reviews A and B, two rounds)

Reviewer A's wording is pasted from its log (`core-review-a.md`); that log names most findings in one line, so the text
below is exactly that line's wording, nothing added. Status: FIXED = in e5bdb99 (fix round 1) or in the round-2 commit.

Reviewer A, round 1 (b3564bd), log line: "F1 SEV-2 Windows retry thrash; F2 SEV-3 dry pool loops last track while type
current (test doc wrong); F3 SEV-3 kill switch latency blocked by downloads; F4 SEV-3 bad pooled head wedges env; F5/F6
SEV-4 event attribution; F7 startup rewrite; F8 48k claim; F9 plan/bound race; F10-12 SEV-5; F13 harmony host name in DESIGN
(pre-existing); F14 no post-load entry check." (the host name itself is not repeated here)
- **A-F1 SEV-2** "Windows retry thrash": FIXED e5bdb99.
- **A-F2 SEV-3** "dry pool loops last track while type current (test doc wrong)": FIXED e5bdb99.
- **A-F3 SEV-3** "kill switch latency blocked by downloads": FIXED e5bdb99.
- **A-F4 SEV-3** "bad pooled head wedges env": FIXED e5bdb99.
- **A-F5 SEV-4** "event attribution" (with F6): BACKLOG.
- **A-F6 SEV-4** "event attribution" (with F5): BACKLOG.
- **A-F7** "startup rewrite": FIXED e5bdb99 (round 2 log: "F7 fixed (manifest v2 known slots; v1 read; 2 sizes -> rewrite)").
- **A-F8** "48k claim": FIXED e5bdb99 ("F8 fixed (44.1k stereo only)").
- **A-F9** "plan/bound race": BACKLOG.
- **A-F10 SEV-5** (one of "F10-12 SEV-5"): FIXED e5bdb99 ("F10 fixed (LogState count; appliedSize=0 -> reassert at safe tick)").
- **A-F11 SEV-5** (one of "F10-12 SEV-5"): FIXED e5bdb99 ("F11 fixed").
- **A-F12 SEV-5** (one of "F10-12 SEV-5"; not itemised in the log): BACKLOG.
- **A-F13** "the server host name in DESIGN (pre-existing)": FIXED e5bdb99 (DESIGN, ROADMAP and tests no longer name the server).
- **A-F14** "no post-load entry check": FIXED e5bdb99 (the post-load entry count, reviewer A's round-2 "F10 fixed" line).

Reviewer A, round 2 (e5bdb99), verbatim:
- **R2-1 SEV-3** "DebugNotification is a new engine call, unproven in field (ToastEnabled=0 is its kill switch)":
  documented in DESIGN 3 item 4 as the one new engine call of 1.0; proven only when the toast shows in game.
- **R2-2 SEV-4** "cancelled profile PUT -> NetFailure (backoff + toast failure count) PoolWorker.cpp:461": FIXED in the
  round-2 commit (early return on kCancelled; `ClassifyFailure` / `ToastPolicy::OnOutcome` tests).
- **R2-3 SEV-5** "replay never prepared if refresh fires while not current (one repeat)": BACKLOG.
- **R2-4 SEV-5** "permanent ACCESS_DENIED re-decodes every ~2 min": BACKLOG.

Reviewer B, round 1 (b3564bd), deferred:
- **SEV-4 B** cache overshoot and pooledBytes double count: the cap check counts pooled bytes per pool, a file pooled in
  two environments counts twice, and a download can push the cache above `CacheCapMB` until the next eviction.
- **SEV-4 B** genre mode needs a library: genres come from the live library only, so a streaming-only user cannot pick
  a genre in the menu (Query works).
- **SEV-5 B** artist browse is cut at 200 hits with no notice in the menu.
- **SEV-5 B** "Show next up" peeks the server bag, not the pool: `EnvStatus.poolTitles` is what actually plays next.
- **SEV-5 B** count-3-and-filter wastes picks: `next` consumes 3 from the server bag while the pool may take fewer
  (visible in the small-selection test as an uneven spread).
- **SEV-5 B** bad key vs bad address message (App.cpp:67 / PoolWorker.cpp:323): an invalid address and a refused key can
  read alike in the menu.

Reviewer B, round 2 (e5bdb99), from its log:
- **SEV-4 B** "REVIEW-BACKLOG.md:69 re-adds the host name": FIXED in the round-2 commit.
- **SEV-4 B** "N<=poolSize -> all picks pooled -> eligible empty -> 20 s next poll forever, no notice": FIXED in the
  round-2 commit (the environment waits for its next play event or a settings change; "Small selection" notice; a
  plain-language problem after repeated rejected picks).
- **SEV-4 B** "fallback can re-pool the just-played track (back-to-back repeat) when it is the only eligible pick while
  pool still non-empty": FIXED in the round-2 commit (test in `Pool_AcceptablePicksFiltersRepeats`).
- **SEV-5 B** "backlog A items are guesses/misnumbered": FIXED (this section). "INTEGRATION-TEST 10.2 vs repeats", "ToastPolicy.h
  comment (disabled/invalid address)", "replay nowPlaying shows raw key": FIXED in the round-2 commit.

Reviewer A's in-game checklist (round 2, 9 items) is docs/INTEGRATION-TEST.md "Reviewer checklist".

## menu-page (settings page), Opus review of 3a17c22 + da76f61

- **SEV-5** A literal `##` in a server-supplied title truncates the label of a suggestion row (ImGui id syntax).
- **SEV-5** `SettingsStore::Update` may log (sanitize corrections) on the render thread.
- **SEV-5** `s.queryText.erase` inside the suggestions loop is fragile (it relies on the loop ending right after the pick).
- **SEV-5** The empty-text browse `SearchAsync` ("Show suggestions" with no text, genre list for presets) is a contract with
  the core: it must return the library's genres/artists for an empty query.
