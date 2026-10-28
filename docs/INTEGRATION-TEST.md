# 1.0 integration test (LoreRim, one test cycle)

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

## 1b. Slot files at start (Deck field log 2026-10-27)
1. First start after installing: the log says `[slots] 24 slot file(s) did not exist at game start (...): created 24 silent
   placeholder(s)`; every later start: `[slots] all 24 slot files existed at game start`.
2. At the first Harmony track of each environment, `[retarget] slot NN play check +500 ms: slot still current=true handle
   now=<same as the event> inner trackStatus=Playing`. A line ending `<- NOT playing` or a `[slots] slot NN ... cannot play`
   error means the game could not play the file: keep the log.

3. Every change of the music the game is playing gets one `[music] current: <type> ...` line (what type, whether it is
   retargeted to a slot, its status, which entry is selected and whether that entry is our slot with a sound handle).
   A retargeted type that is current for 2 s without a play event gets one `[music] ... gave no play event for 2 s` warning.
   For a fight: note the clock time it starts and what you hear (silence / vanilla combat music / a Harmony track).

4. Field #4 checks. During and after fights: combat music starts with the fight (note the clock time of any delay);
   the combat row shows "N of 5 ready"; no `RESTORED to vanilla` for combat types while `[slots] ... <- REPLAY` lines show
   earlier tracks being placed; every `[stall] ... gap ... ms` warning ends with `ours: ...` (whether a slot write or a
   download was running): a gap while ours says "idle" / "not downloading" is the game's own (autosave, loading).
   Each slot write line shows `decode+write N ms, manifest+rename N ms`.
   Field #5: once a minute a `[perf] game thread in our watcher task` line (avg/max microseconds per tick, waits > 20 ms
   for the pool lock). Stalls are ours only if that line shows long waits or a high max; `[stall]` lines also say whether
   a loading screen was open.

## 2. Configure (menu or INI) (acceptance 3)
1. Set ServerUrl + PersonalKey, and: `explore` = Genre (a genre the library really has; the picker lists them),
   `combat` = Query `epic battle music orchestral` with streaming ON, `dungeon` = Artist from the library.
   Note: the presets and these examples assume YouTube Music streaming for most environments (a small library
   cannot fill them); a library-only selection with few tracks shows "Small selection: tracks will repeat".
   PoolSize 3. Apply (menu) or restart (INI).
2. Expect: `[net] Connected. Sources: ...`, `[profile] published 3 environment type(s)`, `[plan] #n: engine writes on;
   3 environment(s), N music type(s) mapped to slots`, then `[pool] explore: + Artist - Title (... flac ...)` lines,
   combat filled first, then `[slots] slot NN (combat) <- ...` / `[slots] slot NN (explore) <- ...`, then
   `[retarget] MUSExploreTundra(...) -> slot NN (explore): tracks=125 replaced=...`.
3. The key must not appear anywhere in the log (search for it).

## 3. Exploration (acceptance 3, 6)
1. Walk outside for ~15 minutes across two regions (e.g. tundra -> pine forest).
2. Listen: Harmony tracks of the chosen genre, the vanilla Silent gaps still happen between tracks.
3. Log per song change: `[slots] slot NN (explore) PLAY handle=... now playing <Artist - Title>`, ~1.5 s later
   `[slots] slot NN (explore) <- <next title>`, and a `[pool] explore: + ...` refill. Note any
   `the previous track repeats` warning (the engine re-picked the slot before the refill landed); with a small
   selection the status line says "Small selection: tracks will repeat" and the pool keeps playing.
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
2. Expect `[net] next failed: unreachable (...); next attempt in 2 s` (or `status failed: unreachable`, `timeout`)
   with a growing back-off (never a tight loop). What you HEAR: tracks already pooled keep playing; when a pool runs
   dry WHILE its music plays, earlier tracks of that environment rotate (`[slots] slot NN (...) <- REPLAY ...` and
   `PLAY ... pool was dry, replaying recently played`), not the same song in a loop; when that music type stops,
   `[retarget] ... RESTORED to vanilla (slot not ready ...)` and the next time vanilla music plays. Never silence.
   An in-game notification "Jukebox: Harmony server not reachable - playing saved/vanilla music" appears after
   about 20 s of failure (or two failed requests) and again every 5 minutes (`ToastIntervalMinutes`); none at the
   main menu or a loading screen.
3. Start Harmony again: within the back-off the pool refills and the type is retargeted again (`-> slot NN`); one
   notification "Jukebox: Harmony is back".
4. Notification triggers to check once: a wrong ServerUrl in the menu (unreachable: the same notification), a
   wrong PersonalKey ("Jukebox: Harmony rejected the key", once per settings change), `ToastEnabled=0` (none).

## 7. Settings changes in game (acceptance 2, 4)
1. Change `explore` to another genre, Apply. Expect `[pool] pool explore dropped`, a refill, the slot rewritten.
2. Set `combat` to Vanilla, Apply: `RESTORED to vanilla (environment not active)` for the combat types when safe.
3. A library-only genre with no tracks: the status line says "No matching tracks in your library ..." and the
   environment stays vanilla; the log shows a 10 min retry, not a loop. (Editing `MarthJukebox.ini` by hand instead of
   the menu: do it with the game closed, or the menu's next save overwrites the edit; the plugin re-reads the INI at
   start.)

## 8. Kill switches (acceptance 5)
1. Create `overwrite/SKSE/Plugins/MarthJukebox.noretarget` while in game (any content). Within ~2 s (the slot thread
   polls it; a download in flight is cancelled): `kill switch file APPEARED` and every retargeted type
   `RESTORED to vanilla (kill switch)` as soon as it is idle (a type that is playing keeps its current track until it
   stops). Delete it: `kill switch file removed`, retargets come back.
2. `[Features] Flac=0` (restart): downloads are `.wav`.

## 9. Save/load and fast travel (acceptance 5)
1. Save, load that save, fast travel twice, open/close the map and inventory. Expect `[retarget] kPostLoadGame: N music
   type(s) retargeted`, NO `has ... entries on slot ... re-asserting` error (that would mean something rebuilt the
   arrays), and music continuing; no crash.
2. `[stall]` lines: gaps > 5 s are warnings (loading screens are expected); periodic ~3 s gaps are the autosaves
   (debug level).

## 10. Long session + restart (acceptance 5, 6)
1. Play on for the rest of the 2 hours. Quit. Restart and load: expect `[pool] index loaded: k pool(s), n validated
   track(s), 0 dropped`, `[slots] slot manifest: ... with a known track` and NO `slot NN (...) <-` rewrite for slots that
   still hold an unplayed pool track; immediate Harmony music before any new download.
2. Count repeats in the log for one environment with a BROAD streaming selection, outside the Harmony-down test of §6
   (same title in two PLAY lines): expect none within 50 plays. A small selection repeats by design (its menu line says so).

## Reviewer checklist (in this order; each one is a yes/no with the log kept)
1. **Resume after a fight, with a new song already waiting.** Let an exploration song play for a minute (the plugin
   has already swapped the NEXT song into the file by then), start a fight, finish it. Does the exploration song you
   were hearing continue where it stopped, and NOT jump to the new song or start over?
2. **One slot shared by several regions.** With `explore` set (no region-specific settings), walk from tundra into a pine
   forest and a marsh. Every region plays Harmony music from the same exploration list, and the log shows one slot for
   all of them.
3. **The "Harmony not reachable" notification.** (a) Set a server address that is well-formed but wrong (e.g. the right
   host with a wrong port): after about 20 s a notification top-left, again every 5 minutes. (b) A wrong key: "Harmony
   rejected the key", once. (c) `ToastEnabled=0` in the INI: no notification at all. (d) None appears over the main
   menu or a loading screen (it waits until they are gone).
4. **Kill switch during a download.** While the log shows a download in progress (`[pool] ... +` lines still coming),
   create the `MarthJukebox.noretarget` file: within about 2 s the log says the kill switch appeared, the download stops,
   and the music goes back to vanilla when the current song ends.
5. **Harmony stops while music plays.** Stop the server during exploration and keep playing through several song
   changes: you hear earlier Harmony songs of that environment again (not the same song over and over); when that
   music stops, vanilla music takes over. Start the server again: one "Harmony is back" notification.
6. **Save, load, new game.** Save, load the save, and start a new game from the main menu: music keeps working each
   time and the log shows no "re-asserting" error.
7. **Restart without rewriting.** Quit and start the game again: the log shows the slot files were recognised and NOT
   written again at start; Harmony music plays right away.
8. **Quit cleanly.** Quit to desktop (from the main menu and from a save): the game closes without hanging.
9. **Windows note.** This whole test runs under Proton. On native Windows the file swap may be refused while a song
   plays; the log then says the rename is retried when that slot is quiet. Report it if you test on Windows.

## Evidence to hand back
`MarthJukebox.log` of every session, `MarthJukebox.ini` (REMOVE the PersonalKey line first), DLL sha256, the list of
`Data/Music/Harmony/slot_*.wav` files with sizes, `Data/SKSE/Plugins/MarthJukebox/slots.json`, the cache folder size,
and notes of what was heard at steps 3-6.
