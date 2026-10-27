# Review backlog (SEV-4/5 findings deferred, drained in one batch before a cut)

## M0a, raised against 61d98248bdbf216695c32481971daf9ebfdc329e (Opus 5.5 review)

- **SEV-5** `native/probe/Probe.cpp:290-300` (reviewer, verbatim): "The shutdown thread design is more complex
  than it needs to be." A static std::jthread guard joins in its destructor at DLL_PROCESS_DETACH. Acceptable:
  on normal exit Windows has already killed the thread and SKSE never unloads a plugin, but a detached/leaked
  read-only thread is simpler and loses nothing. Deferred; drain before the first release.
