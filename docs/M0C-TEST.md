# M0c in-game test (replace the file while it plays)

M0b showed that the engine plays our WAV and opens the file again for every pick, but it never picked A, so
two questions stayed open:

- **(e)** can a file that is **playing right now** be opened for writing or replaced by a rename?
- **(f)** when the file is replaced, does the **next** play use the new content?

M0c answers both on purpose:

- `MUSExploreTundra`'s songs now all point to **one** record, A (`explore_a.wav`). B is unused.
- Every time A starts a new play, the plugin waits 1.5 s, then:
  - tries to open the playing file for writing (and closes it without writing);
  - replaces the playing file by a rename. Odd plays (1st, 3rd, ...) put a **15 s sine tone** in its
    place; even plays (2nd, 4th, ...) put **your original music** back.

The play that is already running should not change. The NEXT play should be whatever was put in the file.
The silent gaps between songs are unchanged.

## Install

1. **Quit the game completely.**
2. **First restore your test music in slot A** (M0b left a 440 Hz tone in it):
   - move `overwrite/Music/Harmony/explore_a.wav.orig` over `mods/<mod>/Music/Harmony/explore_a.wav`
     (replace it, drop the `.orig` suffix);
   - delete any `*.tmp` and any other `*.orig` left in `overwrite/Music/Harmony/`.

   If a `.orig` is missing when M0c needs one, it creates it from the current `explore_a.wav`. It refuses
   if that file has exactly the tone's size (2,646,044 bytes) and then changes nothing. So restore first.
3. Replace `MarthJukebox.dll` and `MarthJukebox.pdb` in the mod's `SKSE/Plugins/` with the ones from the
   CI artifact `MarthJukebox-dll` of branch `m0c-slot-protocol`. Leave the ESP alone.
4. Make sure `SKSE/Plugins/MarthJukebox.noretarget` does **not** exist.

## In game

1. Start via SKSE and load a save **outdoors in the Tundra**.
2. Wait for exploration music, then wait through **at least 6 song changes** (about 15 minutes; quiet gaps
   count as changes, and fast-wait or sleeping helps).
3. What you should hear, play by play:
   1. your music (play 1; during it the file is swapped to a tone);
   2. a **sine tone** for 15 s (play 2; during it the file is swapped back to your music);
   3. your music (play 3), then a higher tone (play 4), and so on.

   If you only ever hear your music, the swap did not reach the next play. If you hear a tone, the swap
   worked. Either result is an answer.
4. Optional: one fight in the middle, to see that an interrupted play is not counted as a new one.
5. Quit to desktop normally.

Note roughly when each song started and what it was (music / tone), plus any stutter, silence or crash.

## After the test

Your music is in `explore_a.wav` after an even number of swaps. If the last swap was a tone, restore
`explore_a.wav` from `overwrite/Music/Harmony/explore_a.wav.orig` as in step 2 of Install.

## Kill switch

Create an empty `SKSE/Plugins/MarthJukebox.noretarget` to stop it:

- It is checked at load, and every 2 s while the game runs.
- File swaps stop at once.
- The original songs are put back at the next 250 ms tick where `MUSExploreTundra` is neither the current
  music type nor queued (status Inactive or Finished).
- It stays off until the next game start.

## What to send back

`MarthJukebox.log`, whole, from `<prefix>/drive_c/users/steamuser/Documents/My Games/Skyrim Special
Edition/SKSE/MarthJukebox.log` (Windows: `Documents\My Games\Skyrim Special Edition\SKSE\`). Also send your
notes (music/tone per song, with rough times) and where `explore_a.wav.orig` / `.tmp` are afterwards.

## Reading the log

**Play events.** One line per new play of A:

```
[slots] play #n handle=0x... idx=... trackStatus=... t=+...ms
```

A play event means `MUSExploreTundra` is the current type, `tracks[currentTrackIndex]` is A, and either A's
`trackHandle.soundID` or `currentTrackIndex` differs from the last seen one. Resuming after combat (same
handle, same index) is not a new play.

After 1.5 s the worker logs one of:

- `[slots] play #n (...): stable for N ms (A still current: true|false); running (e) and (f)`;
- `... dropped, the play changed within 1500 ms`. This happens when the index and the handle changed one
  tick apart. In that case `n` advances twice, so the odd/even tone pattern shifts. Always go by the
  "declared to hold" line, not by `n`.

**(e) Is a playing file locked for writing?**

```
[slots] lock probe (play #n ...): open ...explore_a.wav for write OK|FAILED, Win32 error E
```

- OK means the engine does not lock the file it is playing.
- FAILED with error 32 (sharing violation) means it does.

**(f) Does the replaced content play on the next pick?** First, the swap itself:

- `[slots] play #n (...): rename over the PLAYING file OK (...); slot A now declared to hold: tone F Hz | original`
- or `RENAME OVER THE PLAYING FILE FAILED, Win32 error E (sharing violation | access denied | ...)`. This is
  not retried; the next play event tries again.

Then compare with the next `[slots] play #n+1` and your ear:

- the declared content is heard: (f) is yes;
- the old content is heard although the rename succeeded: the engine caches something per file/path.

The `[probe] pos` lines show whether a tone play ends after about 15 s (its status goes to
Finishing/Finished, or the next pick comes sooner).

**trackHistory (does CycleTracks pick the first playable history entry?)** On each play event:

```
[probe] trackHistory (play #n): type=... currentTrackIndex=I tracks=125 history size=H first=[h0,h1,h2] head: tracks[h0]=A|vanilla (...)
```

Compare `h0` (and its wrapper) from play #n with the `idx` of play #n+1 and with the Silent picks seen in
`[probe] now-playing` lines in between. Whether this log is taken right after the engine rotated the history,
or before, is part of what the data must show.

**Crash / stutter:** compare your noted times with the `play #n` and `rename` lines; each step logs its
elapsed ms. Two lines help:

- `[slots] play #n handled on main thread in <us> us`: the time the plugin spent on the game's main thread
  for that play event (detecting it, logging, and queueing the file work). The file work itself runs on
  the plugin's own thread.
- `[stall] main-thread tick gap <N> ms (previous tick at +<t>ms, this at +<t>ms) current=... idx=...
  status=...`: written only when the plugin's 250 ms check was held up for more than 1 s. That means the
  game's main thread was blocked for that long; nothing is logged when it was not. Loading screens also
  produce these lines, which is normal.

**If you notice a stutter:**

- A `[stall]` line at the same time means the main thread was blocked. The lines around it show what else
  was happening.
- No `[stall]` line means the main thread was not blocked, so it was not our thread. The other mods' logs
  for that moment are the place to look.
