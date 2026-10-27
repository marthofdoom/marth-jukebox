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
- **SEV-5** minor items (reviewer): remaining SEV-5 notes from the 4a5f2c1 review are not itemised in the hand-off
  message; the coordinator should paste them verbatim here when draining. (Known from authoring: C4996 `getenv`
  warning in `tests/core/ProfileTests.cpp`; `EnvLabel`/`SafeName` still echo known key names only.)
