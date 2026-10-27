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

## core-playback, raised against b3564bd (two Opus 5.5 reviews, A and B; deferred by the coordinator, not fixed)

Note: the reviewers' full reports were not on disk for the author; the wording below is taken from reviewer A's log
and the coordinator's fix-round message, and marked where it is a summary.

- **SEV-4 A-F5** (review A log: "F5/F6 SEV-4 event attribution"): play-event attribution, part 1. A play event is
  attributed to the slot's current owner environment; an event that races a plan change can be credited to the wrong
  environment (summary).
- **SEV-4 A-F6** (same log line): play-event attribution, part 2 (summary; see reviewer A's report).
- **SEV-4 A-F9** (review A log: "F9 plan/bound race"): the slot thread reads `bound` and the plan under the lock, but the
  game thread publishes `bound` after its writes of the same tick, so for up to one tick a slot write can be decided on a
  stale binding (summary).
- **SEV-5 A-F12** (review A log: "F10-12 SEV-5"): minor, see reviewer A's report (not itemised in the log).
- **SEV-5 A-F13** (review A log: "F13 harmony:8080 in DESIGN (pre-existing)"): the host name. FIXED in this round as
  coordinator item A-F11 (DESIGN, ROADMAP and the tests no longer name the server); kept here for the record.
- **SEV-4 A-F14** (review A log: "F14 no post-load entry check"): FIXED in this round as coordinator item A-F10
  (OnGameLoaded counts our entries per bound type and re-asserts on a mismatch); kept here for the record.
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

### In-game checklist from review A (10 items) -> docs/INTEGRATION-TEST.md

The checklist itself was not in reviewer A's log; the integration test covers, after this round: first run without a
server (§1), the profile/plan/prefill log lines (§2), play events and refills (§3), resume after combat (§4),
diegetic types untouched (§5), Harmony away incl. the replay rotation and the notifications (§6), settings changes
incl. pool drops and no_tracks (§7), the kill switch timing (§8), save/load with the post-load entry check (§9),
restart with known slot contents and no rewrite (§10). Coordinator: map reviewer A's 10 items onto these before the
test cycle.
