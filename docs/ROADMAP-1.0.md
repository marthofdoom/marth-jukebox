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
