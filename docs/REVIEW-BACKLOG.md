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
- SEV-4 docs disagree on Qobuz: docs/ENVIRONMENTS.md:277 ("`sources=all` (YouTube Music; never Qobuz)") vs ENVIRONMENTS.md:313 and DESIGN 8.8 (`all` includes Qobuz until Harmony accepts a source list) vs docs/NEXUS-COMPLIANCE.md:15 ("library ... plus Qobuz"). Fix in one docs commit; note Harmony game-pools now accepts a `sources` list and `game_excluded_services`, so streaming can be sent as `ytmusic` only.
