# M0a in-game test (passive probe)

M0a changes nothing in the game. The plugin only reads: it lists every music type and track the engine
loaded, looks up our two test records, and logs every change of the music manager's state. In M0a our
two records are **not attached to any music type**, so you will not hear them. The test proves:

1. the DLL loads on your runtime (1.5.97 or 1.6.1170),
2. our ESL plugin loads and its two records get the expected FormIDs and hashed paths,
3. what the engine's music manager looks like while the game plays and switches music.

The log decides the design of M0b (the retarget).

## Install

1. DLL: copy `MarthJukebox.dll` (and `MarthJukebox.pdb`, optional) from the CI artifact `MarthJukebox-dll`
   to `Data/SKSE/Plugins/`.
2. ESP: copy `assets/MarthJukebox.esp` (from the repo or the branch) to `Data/`. Enable it in your load
   order. It is ESL-flagged (light), master Skyrim.esm; load order position does not matter.
3. Test audio: create `Data/Music/Harmony/` and copy any 44.1 kHz 16-bit stereo PCM WAV (about 2 minutes or
   longer; marth supplies it) there **twice**, as `explore_a.wav` and `explore_b.wav`. Do not commit audio; the
   repo ignores `*.wav`. The records only reference these files; the engine does not open them in M0a, but
   the hashes in the log are those of these paths.

## In game

Start the game via SKSE and load any save **outdoors in the Tundra** (Whiterun plains is fine). No new
game is needed. Then, in this order, leaving a few seconds between steps:

1. Stand still and wait for explore music to start (a minute or two; fast-wait/sleeping makes it more
   likely). Let it play for a bit, then let it stop.
2. Enter combat (a wolf, a bandit; anything that starts the combat music), win, wait for it to end.
3. Enter a dungeon or cave (a loading door interior).
4. Enter a tavern (e.g. the Bannered Mare).
5. Open the skills menu (character menu, skills tab) and close it again.
6. Quit to desktop normally (do not alt-F4; a clean exit lets the plugin's thread stop and flush).

Near-instant events count too: if a step plays nothing, note that and continue.

## What to send back

The log file is written by the plugin to the SKSE log directory, `MarthJukebox.log`:

- Windows: `Documents\My Games\Skyrim Special Edition\SKSE\MarthJukebox.log`
  (GOG: `Skyrim Special Edition GOG`; VR/other editions use their own folder name).
- Proton/Wine: inside the prefix, `.../drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/MarthJukebox.log`.

Send that file whole. Also send, if it exists, `skse64.log` from the same folder (it lists whether our DLL
loaded) and tell me: game runtime version, whether `MarthJukebox.esp` is enabled, and what you were doing
at roughly which time of day for any step that looked wrong.

Lines to expect: `[startup]`, then a block of `[probe] MUSC ...` lines (one per music type with its
tracks), `[probe] TOTALS ...`, `[probe] ---- our records ----`, then `[probe] t=+NNNNms music: ...` lines,
one per state change. If `MarthJukebox.esp` is missing, the log says so clearly and the rest still works.

## Reading the log

- The engine does **not** keep MUST editor IDs. `edid=""` on every MUST line and a null
  `LookupByEditorID` for `MJ_ExploreTundra_A/B` are EXPECTED and do not mean the ESP failed. The real
  checks are the FormID lookups (`ours A` / `ours B` found) and the `trackID` hashes.
- ANAM spelling in vanilla: of 258 MUST records in Skyrim.esm, 240 have an ANAM (all `.wav`); 210 use
  `\Data\...` (leading backslash), 30 use `Data\...`, and 18 have none. Ours uses the `Data\...` form; the
  log compares both vanilla spellings with ours.
