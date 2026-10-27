# M0a in-game results

## LoreRim (ULLR, Proton, game 1.6.1170, profile Ultra), 2026-10-27, build 79769c3

Log: `<prefix>/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/MarthJukebox.log`
(NOT the MO2 overwrite folder: the plugin logs to the SKSE log directory).

| Question | Result |
|---|---|
| Plugin loads on 1.6.1170 under Proton | yes, no crash through a session of explore/combat/town/tavern/discovery |
| Enumeration/casts | 215 MUSC, 1014 MUST, 1776 track slots, 0 non-wrapper, 0 null inner, **0 cast mismatches** (identity map vs RTDynamicCast agree) |
| Our ESL loads | yes: `FECE9800` / `FECE9801`, both inner tracks `Single` |
| Path hashed at record load | yes: `trackID` non-zero, ext `wav`, dir hash `0x78E04DC4` (ours) |
| Leading `\` in ANAM | normalised away: vanilla `Data\Music\Explore\...` and `\Data\Music\Explore\...` share dir hash `0x0DE44A40` |
| Review SEV-3 (wrapper status never updated) | **confirmed**: `wrapperStatus` stayed `Inactive` for every sample while the inner `trackStatus` showed `Playing`/`Finishing` |
| MUST editor IDs | not kept (`edid=""`), as the test doc says |
| An engine `BSIMusicType` that is not a form | present at the head of the queue (`<unknown BSIMusicType ...>`, 0 tracks); the watcher correctly reads nothing from it |

## Findings that change the design

1. **Track arrays grow AFTER kDataLoaded.** `MUSExploreTundra` had 107 tracks when enumerated at our kDataLoaded
   and `trackIndex=33/125` once playing; `MUSCombat` 30 -> 38. Another plugin/mod appends tracks later (SKSE
   dispatches kDataLoaded in plugin order and we run early, or a mod adds at game load). **The retarget must run
   after those mods are done** (first main-thread task after kDataLoaded / on kPostLoadGame / kNewGame) and must be
   re-asserted, never done once at our own kDataLoaded.
2. **The engine keeps `currentTrackIndex` per type and resumes it.** After combat ended, Explore resumed at the
   same index (48/125). If the array is shrunk to 2 while the index is 48, the engine indexes out of bounds.
   **M0b must reset `currentTrackIndex` (and clear `trackHistory`) whenever it changes `tracks`.** Tier A.
3. **Track choice within a type is not sequential.** Explore moved 33 -> 48 while "Playing"; Tavern silence went
   1,0,2,1. The "[A, B] alternates" assumption in DESIGN.md is unproven: the engine may pick the same one twice.
   M0b must make the choice safe for either order (the idle-slot rule must hold whichever track is picked).
4. **LoreRim has 215 music types** (48 Skyrim.esm, 48 Vigilant, 19 evgSIRENROOT, ...), so the EDID -> category
   map needs a fallback for unknown types and per-mod grouping. Flags seen: CycleTracks, PlaysOnce, PlaysOver,
   AbruptTransition. Combat is `CycleTracks|PlaysOver`.
5. Event order seen: Combat `Playing -> Finishing`, then the previous type (Explore) is re-queued and resumes.

## Tuxborn (deck)
Not collected yet: the deck was asleep (ssh timed out) when checked.
