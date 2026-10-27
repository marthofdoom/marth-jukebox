# M0b in-game test (retarget spike + slot file test)

M0b is the first build that **changes** the game. It does three things, all hardcoded:

1. **Retarget.** The vanilla exploration music type `MUSExploreTundra` (FormID `000B9DCB`) gets its songs
   replaced, in memory, by our two test records from `MarthJukebox.esp` (`explore_a.wav`, `explore_b.wav`).
   Only the song entries are replaced (alternating A, B); the **Silent** entries (the quiet gaps between songs)
   and the number of entries stay exactly as vanilla, so the cadence is unchanged. Nothing is ever written
   while that music type is playing or queued.
2. **Slot file test.** When one of our two tracks starts playing, the plugin rewrites the OTHER file with a
   15-second pure sine tone (a higher pitch each time), using a `.tmp` file and a rename. This tests whether
   a slot file can be replaced while the game runs and whether the game then plays the new content.
3. **Probe.** Everything M0a logged, plus the playback position fields once per second while Tundra explore
   music plays, and which of A / B / vanilla is playing at each change.

## Install (LoreRim, profile with M0a already installed)

1. **Quit the game completely** (not just to the main menu).
2. Replace `MarthJukebox.dll` (and `MarthJukebox.pdb`) in the mod's `SKSE/Plugins/` with the ones from the CI
   artifact `MarthJukebox-dll` of this branch. `MarthJukebox.esp` and the two WAVs in `Music/Harmony/` stay as
   they are (your 133 s test music, as `explore_a.wav` and `explore_b.wav`).
3. Make sure `SKSE/Plugins/MarthJukebox.noretarget` does **not** exist (see Kill switch).

The first time a slot is rewritten, the plugin first copies your original to `explore_a.wav.orig` /
`explore_b.wav.orig` beside it. To get your test music back after the test, delete the `.wav` and rename the
`.orig` back. Under MO2 the new files (`.orig`, possibly `.tmp`) may appear in the **overwrite** folder
instead of the mod folder; that is expected and part of what the test shows.

## In game

1. Start via SKSE, load a save **outdoors in the Tundra** (Whiterun plains is fine).
2. Stand still and listen for exploration music (fast-wait or sleeping a few hours makes it more likely).
   **Expected:** your orchestral test track instead of the vanilla songs, with the vanilla quiet gaps between
   songs preserved.
3. Wait through **at least 3 song changes** (quiet gaps count as changes). Note roughly when each started.
4. Start a fight (a wolf, a bandit), win it, wait until the combat music ends and exploration music returns.
   Note whether the same track continues where it stopped, restarts from the beginning, or a new one starts.
5. Keep listening for a few more changes. At some point one of the picks may be a **pure sine tone**
   (15 seconds, then the track ends): that is the rewritten slot, and it is the **good** sign.
6. Quit to desktop normally (not alt-F4).

Note anything odd with the rough time: silence where music should be, a stutter or hitch when a song
changes, a freeze, a crash.

## Kill switch

Create an empty file `SKSE/Plugins/MarthJukebox.noretarget` (in the mod folder or the game's
`Data/SKSE/Plugins/`) to turn the retarget off. Checked at load; while the game runs it is checked every
2 s, and if anything was replaced the original songs are put back as soon as Tundra music is not playing.
Delete the file to turn the retarget on again (from the next game start).

## What to send back

`MarthJukebox.log`, whole, from the SKSE log directory (NOT the MO2 overwrite folder):

- Proton: `<prefix>/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/MarthJukebox.log`
- Windows: `Documents\My Games\Skyrim Special Edition\SKSE\MarthJukebox.log`

Also: the notes from "In game" (what you heard, at roughly which time), whether a crash happened (and the
crash log if so), and where the `.orig` / `.tmp` files ended up (mod folder or overwrite).

## Reading the log

All times are `t=+NNNNms` since the DLL loaded. Tags: `[retarget]` (the in-memory change), `[slots]` (file
test), `[probe]` (read-only observation).

**(a) When does the track array reach its final size?**
`[retarget] kDataLoaded: MUSExploreTundra tracks=N`, then `[retarget] kInputLoaded/kNewGame/kPostLoadGame: ...
tracks=N`, `[retarget] first watcher tick: ... tracks=N` and `[retarget] MUSExploreTundra tracks.size changed
X -> Y t=...`. The last `size changed` line (or the message after which no more change appears) is the answer.
Each growth is followed by a `[retarget] ... replaced=... (new this pass: K)` line when the new entries are
taken over (or `... entries to replace, deferred: <reason>` while the type is busy).

**(b) Does the engine accept a loose .wav at the hashed path and play it?**
`[probe] now-playing ... type=MUSExploreTundra(000B9DCB) ... wrapper=A inner=Single`, then
`[slots] ... slot A inner status Inactive -> Playing`, then `[probe] pos ...` lines whose
`lastKnownPlaybackPosition` increases and whose `trackHandle[soundID=...]` is not `0xFFFFFFFF`. Plus your ear.
If the status goes Playing -> Finished within a second with no position change, the file was rejected.

**(c) After combat, is explore resumed with the same index, and do the position fields restart or resume?**
The `[probe] pos` lines before combat (last `lastKnownPlaybackPosition`), then `now-playing ... type=MUSCombat...`,
then the first `now-playing ... type=MUSExploreTundra ... trackIndex=I` and the `pos` lines after it. Same
`trackIndex` and a position that continues from where it was = resume; a position back near 0 = restart.

**(d) Does it pick A,A or A,B in a row?**
The sequence of `[probe] now-playing ... type=MUSExploreTundra ... wrapper=A|B|vanilla(...)` lines (Silent entries
show as `vanilla(...) inner=Silent`).

**(e) Is a playing file locked for writing?**
`[slots] lock probe (plugin start): open ...explore_a.wav for write OK|FAILED, Win32 error E` and
`[slots] lock probe (A is Playing): ...`. OK at start and FAILED (error 32 = sharing violation) while playing
= the engine locks it; OK both times = it does not.

**(f) Does a rename-replaced slot play the NEW content when next picked?**
`[slots] refresh #n of B: ... renamed over ... -> slot now holds the F Hz test tone`, then the next
`now-playing ... wrapper=B` with `pos` lines: a 15 s tone gives a track that ends after about 15 s
(`lastKnownPlaybackPosition` stops near 15000 if it is in ms, and B's status goes to Finishing/Finished),
your 133 s music would run on. Your ear (sine tone) confirms it. Failures show as `rename ... FAILED, Win32
error E` (5 = access denied, 32 = sharing violation, 17 = not same device).

**(g) Crash / hang / stutter around the swap?**
Compare your noted times with `[retarget] ... replaced=...` and `[slots] refresh ...` lines (each step logs
its elapsed ms). A crash shows as a log that ends abruptly; the last lines say what was happening. A
`[retarget] RESTORED ...` line appears only if the kill switch was used.

Other lines: `[retarget] DISABLED: ...` (an ESP/record was not found: nothing is changed, by design), and
`[retarget] ... has NO tracks` (same).
