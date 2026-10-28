# Goal: a working 1.0 (marth 2026-10-27)

1.0 = the batched M1+M2+M3 build (DESIGN.md section 7) after it passes the acceptance list on LoreRim,
published as a GitHub release (tag, not draft) with a README that has full instructions. No Nexus upload.

## Acceptance (all on LoreRim, the primary target)
1. **Install** from the GitHub release zip into MO2 with no hand-edited files; the only typing is server URL and key in the menu.
2. **Menu** (SKSE Menu Framework): guided first run, Test connection with a plain-language result, one row per environment with source picker + search, preview, presets, reset to vanilla, per-environment toggles for streaming and remember-place, pool status ("2 of 3 ready"), pool-size setting.
3. **Playback**: Explore, Combat, Town, Dungeon, Boss and the other mapped environments play tracks from Harmony; bards/diegetic types and unknown mod types stay vanilla; silent-track cadence preserved.
4. **Never silent, never blocking**: Harmony unreachable or a pool empty -> the vanilla tracks play (documented degrade, loud log); no network or file IO on the game thread; no hitch at music events.
5. **Stability**: a 2-hour LoreRim session with combat, fast travel, save/load and menus without a crash attributable to the plugin; per-feature kill switches work.
6. **Variety**: a chosen genre almost never repeats a track (Harmony pools + persistent bag + plugin recent-played list).
7. **Compatibility**: works against an older Harmony without FLAC/pools (capability check via `status.containers`), and against the updated one.
8. **Hygiene**: logs useful, uninstall clean (restore + remove cache option), README complete, MIT, sources public, Nexus-compliance notes current.

## Critical path
M0b engine spike (built, in Opus review) -> its LoreRim log decides the slot/pick design ->
net client + pool -> menu -> FLAC decode + remember-place -> one LoreRim test cycle -> fixes -> 1.0.
Parallel, already running: Harmony `game-pools` (variety). Done: Harmony FLAC container (merged, NOT yet deployed to production).

## Decisions that block 1.0 and are marth's
- Updating the production Harmony server with the merged FLAC work and, later, `game-pools`.
- Which of FLAC / ADPCM ships (FLAC is built; ADPCM only if the engine accepts it and disk matters).

## Next release (was planned as 1.0.1; 1.0.1 shipped as the defaults/refill/diagnostics release, so this becomes 1.0.2)
- **Mixed mode (marth 2026-10-27): vanilla + the query.** Per environment, a "Mix" option: the environment keeps
  its vanilla songs AND plays Harmony tracks from the selector, interleaved. Design notes: the in-place retarget
  already leaves unreplaced entries untouched, so Mix = replace only a share of the replaceable (Single/Palette)
  entries with the slot wrapper (setting: share 25/50/75%, default 50%; Silent entries kept so the cadence is
  preserved) and leave the rest vanilla; pool, slot protocol and fallback are unchanged. Menu: a "Mix with vanilla"
  checkbox + share combo on each environment row. Needs: Settings key (`Mix`, `MixPercent`), SlotAssign/Retarget
  entry selection (deterministic pick of which entries to replace, stable across sessions), tests, README line.
  Tier A for the Retarget change (Opus), menu part Sonnet.

## 1.1.0 (larger; mostly Harmony-side)
- **Track vetting (marth 2026-10-27): catch tracks that do not match the theme.** A query like "epic battle music"
  or genre "ambient" returns whatever the streaming search ranks, including off-theme results (talks, reaction
  videos, 10-hour loops, remixes with vocals, live recordings with crowd noise, mislabelled tracks). Marth's view:
  likely better done on the Harmony side. Approach to design (nothing decided):
  - **Harmony (preferred home):** a vetting stage between candidate collection and the pool: metadata rules
    (title/channel/description keywords, duration window, category), audio checks with the ffmpeg Harmony
    already has (speech detection, silence ratio, loudness/dynamics, clipping, crowd noise), optional
    classifier or embedding score against the requested theme, a per-type reject list, and a persistent
    verdict cache so each track is judged once. Game-source API additions: vetting level per profile/type and a
    reject reason in `next`/status for diagnostics. This belongs in the real Harmony repo (owned by the Harmony
    agent/marth); the jukebox side must not push to Harmony main without that coordination.
  - **Plugin side (small):** a "this track does not fit" action (menu button / hotkey) that skips to the next
    track and reports the track id + environment to Harmony as negative feedback (new endpoint), and shows a
    vetting-level setting per environment (off / normal / strict). The plugin already has the preview guard
    (truncated or 30 s preview) and the plugin-side recent list; vetting is the semantic counterpart.
  - Needs: Harmony API design doc first (branch in the Harmony repo, reviewed by Opus), then the jukebox client
    endpoint + menu control, then a LoreRim test with deliberately off-theme queries.
