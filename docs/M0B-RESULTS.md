# M0b in-game results (LoreRim, 1.6.1170, Proton/MO2), 2026-10-27, build cdbe904

Log: 8089 lines, 12:24-12:40, one ~15 min session. No warning, error, crash or cast mismatch lines.

| Q | Answer from the log | Confidence |
|---|---|---|
| (a) when is the array final | 107 entries at our kDataLoaded; **125 at the first watcher tick** (main menu, +432 s). Retarget applied there: `replaced=109 (A=55 B=54) kept-silent=16 kept-other=0`. Nothing was appended later (kPostLoadGame still 125). | high |
| (b) does the engine accept a loose PCM .wav at the hashed path and play it | **Yes.** Slot B (`FECE9801`) reached `trackStatus=Playing` with a sound handle and stayed there ~130 s per pick, 4 picks, no errors. | log: high; audio content: marth to confirm by ear |
| (c) resume after combat | Explore -> Combat (15 s) -> Explore: **same index (15), same sound handle (0x51), no new handle**, track ended ~114 s of playing time after the first 40 s + 75 s. Consistent with **resume mid-track**, not restart (`lastKnownPlaybackPosition` stays 0 and is useless as a position). | medium: marth to confirm by ear |
| (d) A,A or A,B | **B,B,B,B**: four picks (idx 15, 57, 65, 109), all B, never A, although 55 of 109 entries are A (chance about 6%). The engine re-picked the same wrapper back to back. No alternation guarantee. | medium |
| (e) is a playing file locked | **Not answered**: A never played, so the "A is playing" lock probe never ran; the start-of-session probe (A idle) opened for write fine. | open |
| (f) does a rename-replaced slot play the new content | **Not answered**: A (rewritten with the 440 Hz tone at 12:33:26) was never picked. The rewrite itself worked: write+flush+verify+MoveFileExW in 42 ms under USVFS; the file physically replaced `mods/MarthJukebox/Music/Harmony/explore_a.wav` (now 2,646,044 B) and the `.orig` went to `overwrite/Music/Harmony/`, exactly as predicted. | open |
| (g) crash/hitch | none in the log | marth to confirm by ear |

## Facts learned that the 1.0 design must use
1. **Each pick builds a NEW sound handle** (ids 0x51, 0x787, 0xE0B, 0x14EE): the engine re-opens the file on every play. A file replaced on disk BEFORE the next DoPlay is what plays next. That makes the "refill a file between plays" model viable.
2. **Slot status does not show re-plays**: B stayed `Playing` from 12:33:26 to the end across four picks (the same inner track object is re-played back to back, status never visibly left Playing). Edge detection on `trackStatus` therefore misses plays; the plugin must key on the **sound handle id** changing (or `currentTrackIndex` changing), not on status transitions. This is why the refresh of A was only ever triggered once.
3. **The engine can re-pick the same wrapper immediately.** With two slots A/B we cannot assume alternation. Options for v1: (i) ONE wrapper per type and rewrite it for the next play while it is still playing (needs rename-over-open-file to work: question e), or (ii) predict the next pick from `trackHistory[0]` (the review's binary analysis: CycleTracks takes the first playable entry of the shuffled history and rotates it) and make sure that entry's wrapper holds the next track.
4. `lastKnownPlaybackPosition` stays 0 during play: it cannot be used to read position. "Remember place" may be free (the engine resumes the interrupted track itself, point (c)): to be verified by ear before building anything.
5. Appended tracks arrive before the first main-menu tick on this list, so a retarget at the first watcher tick (re-asserted each safe tick) is enough here; keep the re-assert for other lists.

## Next (M0c, small): answer (e) and (f) on purpose
Single wrapper A for every replaceable entry; when A's sound handle changes (a new play), log it, try a write-open and a rename-over of the PLAYING file (a 15 s tone with a different pitch each time) and log the Win32 result; the next play then proves whether the engine picks up replaced content. One more LoreRim run, about 10 minutes of listening.
