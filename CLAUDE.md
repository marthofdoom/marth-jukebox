# CLAUDE.md — marth's Jukebox, SKSE C++ plugin

Plays Skyrim music from a Harmony server (see `DESIGN.md`). Same family as MFO/MEO/MAO: read this file
and `DESIGN.md` first. Process rules are inherited from MFO's CLAUDE.md; the parts that apply are below.
Global model roles are in `~/.claude/CLAUDE.md` and win on any conflict.

## Hard facts

- **CI is the only compiler** (Windows/MSVC on GitHub Actions). No local build. Never report success
  without a green run whose `headSha` is YOUR commit (`cancelled/pending/queued` = keep waiting).
- **Verify every CommonLib symbol** in `/mnt/gaming/modlists/Projects/_commonlib/mit-3.7-fork/`
  (the `mit-3.7` tip). NOT in `live-alandtse-ng/` (GPL, reference only, never copy code) and not from
  memory. Anything ABI-shaped (vfunc signatures, offsets, ids) is checked against the disassembled
  binary, because CommonLib declarations are not ABI-trustworthy. If it is not in the fork, it does
  not exist for our build.
- **Licence MIT.** Never copy GPL code (alandtse 7.x). Credit CommonLibSSE-NG (MIT fork), ImGui, etc.
- **The mod ships no audio and no secrets.** The Harmony server URL and personal key are user settings.
  Nothing about the user's server, key or key's location goes into the public repo.
- **Nexus compliance is a design input, not a release chore:** `docs/NEXUS-COMPLIANCE.md`. Any change
  that adds network destinations, telemetry, auto-update, bundled media, or changes what the mod page
  must say needs that doc updated in the same commit.

## Engineering principles (from MFO, apply here)

1. **Disassembly proves a path exists, not that it runs.** Observe it executing (log line, probe, field
   capture) before building on it. The milestone 0 spike exists for exactly this.
2. **Never mask a failure.** No retry/watchdog that makes a broken mechanism look fine. Log loudly. A
   documented degrade (Harmony away -> restore the vanilla tracks) is a contract, not a mask.
3. **Proper solutions, not workarounds.** If a compromise is unavoidable, flag it and record why.
4. **Per-runtime paths:** what is proven on 1.6.1170 ships there; 1.5.97 gets its own verified path.
5. **Lowest crash surface first:** read-only polling and data edits before vfunc hooks; one hook class
   per build so a CTD bisects.
6. **Network never touches the game thread.** All HTTP and file IO on a worker; the game thread only
   swaps already-validated pointers/files.

## Scope discipline

DO EXACTLY WHAT THE BRIEF ASKS. No refactors, splits, renames, new files or "while I was in there"
cleanups unless the brief names them; if the work seems to need one, STOP and report. Before merging,
run `git diff --stat` and `git diff --name-status -M` and read them **for what you did NOT expect**
(any `A`/`D`/`R` is a STOP until explained). A summary is not a review.

Layout: `native/<subsystem>/` with one small public header, one `_internal.h`, cohesive `.cpp` files;
new mechanism = new file in its subsystem. ~1500 lines = plan a split (its own brief); 2500 = hard stop.

## Agents (inherited from MFO; models per the global file)

- **Authors:** `sonnet-coder` writes tier B/C code from a precise spec. Tier A (engine seats/hooks,
  ABI, threading, track-array mutation, file-slot/rename protocol if it races the engine) is authored
  by Opus. `sonnet-chore` / `haiku-coder` for the menial/trivial ones. **Opus reviews EVERY diff.**
  Fable only as an escalation, with the reason stated.
- **Review depth by shape:** doc/comment-only = comment-strip diff (identical code); localized change in
  an existing mechanism = one focused Opus pass; new mechanism / threading / hooks / engine memory
  edits = full adversarial Opus. CLOSING rounds (strings, wording) get no Opus pass. SEV-4/5 findings go
  to `docs/REVIEW-BACKLOG.md` (verbatim, severity, SHA) and are drained in one batch before a cut;
  co-save/threading/ABI findings and anything the next field cycle will exercise are never deferred.
- **Brief once, completely.** One brief per agent, run to completion; no drip-fed refinements. Resume only
  to fix a real error, via SendMessage to the SAME author (review findings go back to the author, never
  a fresh agent), restating the hard rules. Small greps/reads/edits are done inline.
- **At most 3 agents at once (reviews count); never two agents on the same build/working tree**
  (use `isolation: worktree` or serialize).
- **Titles + registry:** every Agent `description` is `[JUKEBOX] Role · Task` (Author/Review/Diagnose/
  Chore); keep `<scratchpad>/agents.md` (Title | Model | Status | Branch | Log) updated at every spawn,
  finish, resume and stop.
- **Disk logs:** every agent brief says "create `<scratchpad>/agentlogs/<task>.md` at the start and append
  after each meaningful step (values verified, decisions + why, file:line written, what remains,
  branch/commit state); commit+push WIP at coherent checkpoints; if resumed, read the log first."
- **Agent waiters outlive the agent:** a background CI waiter can still act after the agent "finished".
  Briefs say: no merge, tag, release or force-push; push your own branch only; do not arm a waiter
  that performs any action after CI.
- **Review the BRANCH, not the main checkout:** `git show <branch>:<path>` / `git diff main..<branch>`.
- **Field surprise -> the coordinator GATHERS** (logs, deployed DLL sha, INI state, an evidence file) and
  **Opus CONCLUDES**; do not arrive with a root-cause theory.
- **Cloud isolation (`remote`)** only for work that needs just the GitHub repo; self-contained briefs, no
  local paths. Anything needing the local `_commonlib`, binaries or Harmony needs local.

## Release (family pattern)

**Distribution is GitHub-only (marth 2026-10-27)**; Nexus is held until Harmony has Spotify. A GitHub
release (tag + published, not draft) ships with a README that has FULL instructions: requirements
(Harmony server, SKSE, Address Library, the menu framework), install, first-run setup (server URL, key),
the per-environment settings incl. the streaming checkbox, the cache folder and its size cap, known
limits, uninstall, and the log file location. Update the README in the same release that adds or changes
a user-facing feature; a release without current instructions is not done. Pre-release milestones (M0..)
stay documented in docs/ only. GitHub has no Nexus-style rules, but docs/NEXUS-COMPLIANCE.md stays
current so a later Nexus upload is a small step.

One milestone per build, in-game tested before the next is written. A release request means tag + publish
(not draft) + the Nexus text; the docs and the Nexus kit (`assets/nexus-description.bbcode`, banner,
packaged README) are standing scope. The Nexus upload itself stays a manual browser step for marth.
Dead agent docs are recovered/recreated from the log, not re-briefed from zero.
