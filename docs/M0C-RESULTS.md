# M0c results: slot protocol (LoreRim, Skyrim SE 1.6.1170, Proton + MO2/USVFS, 2026-10-27)

Build: `m0c-slot-protocol` 561e224 (Opus-reviewed, CI green). Test: 9 song changes of `MUSExploreTundra`
over ~25 min, one slot file (`explore_a.wav`, wrapper A for every replaceable entry), original = 43 MB folk
track, replaced alternately with a 15 s tone (rising pitch) and the original. **Platform: Proton/Wine + USVFS
only. Do not treat these as Windows facts** (a Windows engine open without FILE_SHARE_DELETE would fail the
rename with error 32); Windows needs its own verification before the Windows result is claimed.

## Answers

| Question | Result |
|---|---|
| (e) Can a PLAYING slot file be opened for write / renamed over? | **Yes**, 9 of 9 plays: write-open OK (0 ms), `MoveFileExW` over the playing file OK (6-34 ms; total step 27-49 ms). |
| (f) Does the replaced content play at the next song change? | **Yes** (marth, by ear): tones of rising pitch (660, 1100, 1540 ... Hz) alternated with the folk track as declared; the engine re-opens the file on every play. |
| Can the next pick be predicted? | **Yes**: `trackHistory[0]` after a play equals the `currentTrackIndex` of the NEXT play, 4 of 4 checked at the time (30 -> 48 -> 116 -> 88 -> 101), later plays consistent (81, 11, 50, 96). `tracks[history[0]]` also tells whether the next entry is ours (A) or vanilla/Silent (play #5 predicted a Silent gap). |
| Main-thread cost per play event | 47-73 us (play detection + trackHistory read + logging). |
| Stutter | marth heard none. `[stall]` lines (>400 ms tick gaps): ~2.7-2.9 s gaps at 14:42:19, 14:47:36, 14:53:08 (every ~5 min 17 s), plus a loading-screen gap. They do not align with our file operations (14:36, 14:40, 14:44, 14:49, 14:53:25) and the earlier M0b run had a similar 3.4 s gap: a periodic main-thread stall, not our worker. **marth confirmed they are the game's timed autosaves** (~every 5 min), so the stall detector's periodic lines are expected and benign. |

## Consequence for the 1.0 slot protocol

One wrapper (A) per replaceable entry, one slot file per environment, refilled from the pool by
`.tmp` -> flush -> `MoveFileExW(REPLACE_EXISTING)` **after the play event has been stable ~1.5 s**
(the file is re-opened per play, and the rename-over of the playing file is accepted on this platform).
Using `trackHistory[0]` the plugin can know before the next change whether the next entry is ours, so the
slot can be filled with the pool track in good time (and a Silent gap predicted); a play event is still
detected by sound-handle/index change. Remember-place ON is the engine default (it resumes the same handle and index).
Open: Windows verification of the same rename (fallback: write only while the type is not current, as in M0b);
the ~2.8 s autosave stalls are the game's own (not ours).
