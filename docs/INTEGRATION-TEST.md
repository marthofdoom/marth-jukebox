# 1.0 integration test (LoreRim, one cycle with marth)

Covers docs/ROADMAP-1.0.md acceptance items 3-7 for the playback core (the menu has its own checks). About
60-90 minutes. Everything the plugin does is in `MarthJukebox.log` (SKSE log folder); keep it after each step.

## 0. Before
- Production Harmony on 1.0.13+ (`/api/game/status` shows `services` and `containers: ["wav","flac"]`).
- MO2 closed. Install the build into `mods/MarthJukebox`: `SKSE/Plugins/MarthJukebox.dll` (+ `.pdb`) and the
  NEW `MarthJukebox.esp` (24 slot records `MJ_SLOT_00..23`; the M0 ESP with `MJ_ExploreTundra_A/B` must be
  gone). Leftovers of M0b/M0c in `overwrite/Music/Harmony/` (`explore_a.wav`, `explore_a.wav.orig`, `explore_b.wav`)
  are no longer used and can be deleted.
- `overwrite/SKSE/Plugins/MarthJukebox.noretarget` must NOT exist.
- Record the DLL sha256 in the evidence file.

## 1. First run without a server (acceptance 1, 4)
1. No `MarthJukebox.ini` (or `ServerUrl=` empty). Start the game, load a save, walk 2 minutes.
2. Expect in the log: `[net] network inactive (no server URL): everything stays Vanilla`, `[plan] ... engine writes off`,
   no `[retarget] ... -> slot` line, no `[net] GET` line. Vanilla music plays.

## 2. Configure (menu or INI) (acceptance 3)
1. Set ServerUrl + PersonalKey, and: `explore` = Genre (a genre the library really has; the picker lists them),
   `combat` = Query `epic battle music orchestral` with streaming ON, `dungeon` = Artist from the library.
   PoolSize 3. Apply (menu) or restart (INI).
2. Expect: `[net] Connected. Sources: ...`, `[profile] published 3 environment type(s)`, `[plan] #n: engine writes on;
   3 environment(s), N music type(s) mapped to slots`, then `[pool] explore: + Artist - Title (... flac ...)` lines,
   combat filled first, then `[slots] slot 00 (boss?) <- ...` / `[slots] slot NN (explore) <- ...`, then
   `[retarget] MUSExploreTundra(...) -> slot NN (explore): tracks=125 replaced=...`.
3. The key must not appear anywhere in the log (search for it).

## 3. Exploration (acceptance 3, 6)
1. Walk outside for ~15 minutes across two regions (e.g. tundra -> pine forest).
2. Listen: Harmony tracks of the chosen genre, the vanilla Silent gaps still happen between tracks.
3. Log per song change: `[slots] slot NN (explore) PLAY handle=... now playing <Artist - Title>`, ~1.5 s later
   `[slots] slot NN (explore) <- <next title>`, and a `[pool] explore: + ...` refill. Note any
   `the previous track repeats` warning (the engine re-picked the slot before the refill landed).
4. Harmony web UI "now playing" shows the game track.

## 4. Combat and resume (acceptance 3, 4)
1. Start a fight during an exploration track; fight ~30 s; finish.
2. Expect combat music from the combat pool (`slot NN (combat) PLAY`), then exploration RESUMES the interrupted
   track mid-way (remember place = engine default; no new PLAY line for explore on the resume).

## 5. Dungeon, town, boss, diegetic (acceptance 3)
1. Enter a dungeon (artist tracks), a town with a bard (the bard and tavern music stay vanilla: no retarget line
   for `MUSTavern*`), and if possible a dragon/boss (stays vanilla unless configured).

## 6. Degrade: Harmony away (acceptance 4)
1. Stop the Harmony server (or block it) while in game. Keep playing 10 minutes, including at least 3 song changes
   in one environment.
2. Expect `[net] next failed: kUnreachable ...; next attempt in 2 s` with growing back-off (never a tight loop),
   tracks already pooled keep playing; when a pool runs dry: `[slots] slot NN (...): pool empty ...` and
   `[retarget] ... RESTORED to vanilla (slot not ready ...)` once the type is idle; vanilla music plays, no silence.
3. Start Harmony again: within the back-off the pool refills and the type is retargeted again (`-> slot NN`).

## 7. Settings changes in game (acceptance 2, 4)
1. Change `explore` to another genre, Apply. Expect `[pool] pool explore dropped`, a refill, the slot rewritten.
2. Set `combat` to Vanilla, Apply: `RESTORED to vanilla (environment not active)` for the combat types when safe.
3. A library-only genre with no tracks: the status line says "No matching tracks in your library ..." and the
   environment stays vanilla; the log shows a 10 min retry, not a loop.

## 8. Kill switches (acceptance 5)
1. Create `overwrite/SKSE/Plugins/MarthJukebox.noretarget` while in game (any content). Within ~2 s:
   `kill switch file APPEARED` and every retargeted type `RESTORED to vanilla (kill switch)` when idle. Delete it:
   `kill switch file removed`, retargets come back.
2. `[Features] Flac=0` (restart): downloads are `.wav`.

## 9. Save/load and fast travel (acceptance 5)
1. Save, load that save, fast travel twice, open/close the map and inventory. Expect `[retarget] kPostLoadGame: N music
   type(s) retargeted` and music continuing; no crash.
2. `[stall]` lines: gaps > 5 s are warnings (loading screens are expected); periodic ~3 s gaps are the autosaves
   (debug level).

## 10. Long session + restart (acceptance 5, 6)
1. Play on for the rest of the 2 hours. Quit. Restart and load: expect `[pool] index loaded: k pool(s), n validated
   track(s), 0 dropped` and immediate Harmony music (prefilled slots) before any new download.
2. Count repeats in the log for one environment (same title in two PLAY lines): expect none within 50 plays.

## Evidence to hand back
`MarthJukebox.log` of every session, `MarthJukebox.ini` (REMOVE the PersonalKey line first), DLL sha256, the list of
`Data/Music/Harmony/slot_*.wav` files with sizes, `Data/SKSE/Plugins/MarthJukebox/slots.json`, the cache folder size,
and notes of what was heard at steps 3-6.
