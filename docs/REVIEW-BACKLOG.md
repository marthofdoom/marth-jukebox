# Review backlog (SEV-4/5 findings deferred, drained in one batch before a cut)

## M0a, raised against 61d98248bdbf216695c32481971daf9ebfdc329e (Opus 5.5 review)

- **SEV-5** `native/probe/Probe.cpp:290-300` (reviewer, verbatim): "The shutdown thread design is more complex
  than it needs to be." A static std::jthread guard joins in its destructor at DLL_PROCESS_DETACH. Acceptable:
  on normal exit Windows has already killed the thread and SKSE never unloads a plugin, but a detached/leaked
  read-only thread is simpler and loses nothing. Deferred; drain before the first release.

## net-client, raised against 0e98fbb (Opus 5.5 review; deferred by the fix round, wording from the review brief)

Note: the `core-settings` branch also edits this file; merge both sections.

- **SEV-3** Cancel/stop on Wine unverified - upload mj_net_tests.exe as CI artifact and run under Proton.
  (The fix round moved WinHTTP to async mode so a cancel no longer depends on Wine unblocking a socket,
  and CI now uploads `mj_net_tests.exe` beside the DLL. A local Wine 10 run passes all cancel tests and
  showed Wine's async receive timeout never fires, now enforced by the client. Proton itself is still open.)
- **SEV-4** Test gaps: the tests only run on Windows (CI); no Wine/Proton run of `mj_net_tests` yet.
- **SEV-5** `target.find("wait=")` also matches `await=` (a caller URL with `await=` would get no `wait=`).
- **SEV-5** `JInt` casts a JSON double to int64 with no range check (UB for huge or NaN-like values).
- **SEV-5** A download request can exceed `downloadDeadlineS` by about `recvMs + connect` (the deadline is
  only checked between 202 retries).
- **SEV-5** A 4 MiB JSON body can build a ~60 MB nlohmann DOM.
- **SEV-5** An `http://` base sends the key in clear text (documented: README / NEXUS-COMPLIANCE recommend
  `https://` off-LAN).
