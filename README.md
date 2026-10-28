# marth's Jukebox

An SKSE plugin for Skyrim that replaces the game's background music with tracks served by **your own
[Harmony](https://github.com/marthofdoom/harmony) music server**. For each kind of music in the game (exploring,
combat, dungeons, towns, bosses ...) you choose what plays: a genre, an artist, a playlist or a free-text search.

**This mod ships no music.** It does nothing useful without a Harmony server that you run yourself, with your own music
library (and, if you choose, your own streaming access). The plugin only talks to the server address you type in; it has
no telemetry and no auto-update.

Version 1.0.0. Licence: MIT. Source: https://github.com/marthofdoom/marth-jukebox

## Contents

1. [Requirements](#requirements)
2. [Install](#install)
3. [First-run setup](#first-run-setup)
4. [Environments](#environments)
5. [Per-environment settings](#per-environment-settings)
6. [Pool, cache and what is written where](#pool-cache-and-what-is-written-where)
7. [Performance and threading](#performance-and-threading)
8. [When Harmony is not reachable](#when-harmony-is-not-reachable)
9. [Kill switches](#kill-switches)
10. [Uninstall](#uninstall)
11. [Known limits](#known-limits)
12. [Troubleshooting](#troubleshooting)
13. [Security](#security)
14. [AI-assisted development](#ai-assisted-development)
15. [Licence and credits](#licence-and-credits)

## Requirements

- **Skyrim Special Edition / Anniversary Edition, runtime 1.6.x.** The only runtime this release was tested on is
  **1.6.1170**, on Linux/Proton with Mod Organizer 2: the 1.0 build was played on a Steam Deck (Tuxborn); the file-swap
  mechanism itself was first proven on a desktop LoreRim install. Skyrim 1.5.97 is **not supported** in this release.
- SKSE64 matching your game version.
- Address Library for SKSE Plugins (Nexus Mods).
- SKSE Menu Framework (Nexus Mods, mod 120352) for the settings page.
  It in turn needs "ImGui Icons" (Nexus mod 114790) for its controller button prompts. This mod does **not** include
  the framework. Without it the plugin still runs, but you can only configure it by editing `MarthJukebox.ini`.
- **A Harmony server, version 1.0.13 or newer**, reachable from the PC that runs Skyrim, plus its URL and your personal
  key (if the server asks for one). Harmony itself is not part of this mod.

Older Harmony servers (before 1.0.13) work in a reduced way: playing from the library works, but the "Allow streaming"
checkbox has no effect (the plugin stays on the library and says so), because those servers cannot be told which
streaming source to use. The only exception is `AllowQobuz=1` in the INI (see
[Per-environment settings](#per-environment-settings)), which lets such a server stream from all of its sources.
The Test connection result tells you when your server is older than 1.0.13.

## Install

The release zip `MarthJukebox-<version>.zip` contains:

```
SKSE/Plugins/MarthJukebox.dll
MarthJukebox.esp
docs/MarthJukebox/README.md  LICENSE  THIRD-PARTY-NOTICES.md  LICENSE-LGPL-2.1
```

The documents sit in their own folder (`Data/docs/MarthJukebox/` after install) so they cannot overwrite another mod's files.

**Mod Organizer 2:** install the zip with "Install a new mod from an archive" (or drag it into the mod list), enable it,
and make sure `MarthJukebox.esp` is ticked in the plugins list. It is a light (ESL-flagged) plugin and does not use a
normal plugin slot.

**Vortex:** install the zip through the Mods page, enable and deploy, and make sure `MarthJukebox.esp` is enabled.

**Manually:** extract the zip into your Skyrim `Data` folder, so that `Data/SKSE/Plugins/MarthJukebox.dll` and
`Data/MarthJukebox.esp` exist, and enable the plugin in your plugin list.

Install the requirements above first. The plugin does not need a patch for other mods: it only edits Skyrim's music
records in memory while the game runs (see [Environments](#environments)).

If you update from an earlier test build, remove the old files from `Data/Music/Harmony/` (`explore_a.wav`,
`explore_a.wav.orig`, `explore_b.wav` belonged to the pre-release experiments and are no longer used) and make sure the
old `MarthJukebox.esp` is replaced by the one in this zip.

## First-run setup

With no server address set, the plugin does nothing: all music stays vanilla and nothing touches the network.

1. Start the game and load into the world (or stay at the main menu).
2. Open the SKSE Menu Framework panel. The framework's default key is **F1** (configurable in its own INI; on a controller
   it is a double press of LB, per its documentation). See the framework's page if the key is different on your setup.
3. In the left list choose **marth's Jukebox > Settings**. At the top a "First-time setup" box shows the three steps.
4. Under **Harmony server**, type the **Harmony server address** (for example `http://192.168.1.20:8080`; the exact host
   and port are those of your own server) and paste your **Personal key** (it is masked; tick **Show** to check it). Text
   fields are applied when you press Enter or click out of the field.
5. Press **Test connection**. The result is written in plain language: for example "Connected. Sources ready: your library,
   YouTube Music.", or "Connected. This Harmony is older than 1.0.13: library only, update it for streaming.", or what is
   wrong (not reachable, key rejected ...). The key is never printed in that message
   or in the log.
6. Choose music. Either press a **preset** (under "Presets"), or set each environment yourself:
   - Pick a **Music source** for it: Vanilla, Genre, Artist, Playlist or Query.
   - Type the genre, artist, playlist or search text in the box. For artists and playlists, press **Show suggestions** and
     click one of the matches (this is also the way to choose without a keyboard). Genres are matched by name against the
     server's library.
   - Press **Show next up** to see which titles the server would play next (no audio is played or downloaded).
7. Play. The first tracks take a short while to download in the background; until a track is ready that environment
   keeps its vanilla music. Each environment row shows a status line such as "2 of 3 ready" and what is playing.

Presets only fill in the music source of some environments and you can change everything afterwards:
"Calm exploration + epic combat (YouTube Music)", "Dark dungeons (YouTube Music)", "Folk journey (library only)" and
"Back to vanilla". The first two turn **streaming on** for the environments they fill (applying one is your consent). They
assume your Harmony server has YouTube Music set up. The page warns when a preset's genre does not exist in your library.

On a **gamepad**, text boxes need a keyboard. Everything else (source, suggestions, checkboxes, presets) works with a
controller.

## Environments

Skyrim has about 50 music types, and mods add many more. The plugin groups the ones it knows into player-facing
**environments**; each environment can have its own source:

| Environment | What it covers |
|---|---|
| Exploration (`explore`) | overland music: tundra, mountain, snow, marsh, coast, the Reach, forests, Solstheim and other regions |
| Towns and cities (`town`) | town and castle music |
| Dungeons (`dungeon`) | dungeon, cave, fort, ice and the DLC dungeons |
| Combat (`combat`) | normal combat and civil-war combat |
| Boss fights (`boss`) | boss and dragon-boss combat music |
| Discovery (`discovery`) | the short "you found a place" cues |
| Stingers and rewards (`stinger`) | short reveal, reward and success cues |
| Special moments (`special`) | death, Alduin and Odahviing cues, Elder Scroll and similar one-offs |

A group has sub-environments (for example `explore.tundra`) shown under "Individual places in this group": by default they
follow their group, and you can give one its own source, or keep it vanilla.

What always stays **vanilla**:

- **Bards and tavern music** (they are in the game world, played by NPCs), and story or ceremonial music (Sovngarde, the
  Hall of Valor, the intro and character-creation scenes, the Word of Power cue). The plugin never touches these.
- **Music types added by other mods that the plugin does not know.** They keep playing as they are. (A small allow-list of
  a few well-known mod and Creation Club types is mapped to the matching environment.)
- Any environment you leave on **Vanilla**.

The plugin changes nothing on disk in your other mods; it only points the music records in memory at its own track
slots while an environment is active, and puts the originals back when it is switched off.

## Per-environment settings

For each environment row in the menu:

- **Music source**: Vanilla / Genre / Artist / Playlist / Query, plus the text or selection.
- **Allow streaming (YouTube Music)**: **off by default.** Off means Harmony is asked for tracks from your own library
  only. On means it may also pick tracks from YouTube Music, which needs YouTube Music to be set up on your Harmony
  server. Streamed tracks are downloaded through your server, which is your decision and your responsibility; check that
  doing so is allowed for you under the service's terms. Streaming tracks take longer to appear the first time. Turning
  the box on or off drops that environment's ready tracks and refills them.
- **Remember track place**: stored, but in 1.0 an interrupted track **always resumes** where it stopped (that is how the
  game's own music works). Switching the box off has no effect yet. The same applies to "Resume tracks where they
  stopped" under Advanced.
- **Show next up** and **Reset to vanilla**.

**Qobuz** is never used unless you add `AllowQobuz=1` under `[General]` in `MarthJukebox.ini`. It is deliberately not in the
menu: without a Qobuz subscription the service only serves 30-second previews. If you do enable it, the streaming
checkbox then also lets Harmony use Qobuz.

Global settings (page section **Pool and cache** and **Advanced**):

- **Tracks kept ready per environment** (pool size): 3, 5 or 8. Default 3.
- **Cache size limit**: default 4096 MB (4 GB), minimum 256 MB (`CacheCapMB` in the INI). The limit applies to the
  downloaded tracks; the slot files (below) come on top.
- **Cache on disk** (the number shown in the page): everything this mod keeps on disk for music, i.e. the downloaded
  tracks in the cache folder PLUS the slot files under `Music/Harmony`. It is re-measured after every download and every
  few seconds. "2 of 3 ready" counts the tracks waiting in that environment's pool, including the one already placed in
  its slot file for the next play; a track still downloading is not counted.
- **Clear cache**: deletes downloaded tracks that are not in use (they are fetched again when needed). Asks you to click twice.
- **Log level** and the kill switches (see below).

Track variety: the plugin remembers the last 50 tracks it played per environment and skips repeats. With a **small
selection** (for example a library genre with a few tracks) repeats are unavoidable; the page then says "Small selection:
tracks will repeat". Broad streaming selections should rarely repeat.

## Pool, cache and what is written where

Paths are relative to your Skyrim `Data` folder. Under **MO2** the game writes new files into MO2's **Overwrite** folder
(or a mod folder you redirect it to), so look there for them, for example `overwrite/SKSE/Plugins/`.

| What | Where |
|---|---|
| Settings (server URL, **personal key in plain text**, per-environment choices) | `SKSE/Plugins/MarthJukebox.ini` (a backup `MarthJukebox.ini.bak` is kept; an unusable file is preserved as `.unreadable`) |
| Downloaded tracks (the cache) and its index `index.json` (recently played list, pool state) | `SKSE/Plugins/MarthJukebox/cache/` by default; change with `CacheDir=` under `[General]` |
| Which slot files the plugin wrote | `SKSE/Plugins/MarthJukebox/slots.json` |
| The slot files the game plays (up to 24, one per active environment; each holds one full track as an uncompressed WAV) | `Music/Harmony/slot_00.wav` ... `slot_23.wav` |
| Log | `MarthJukebox.log` in the SKSE log folder (see [Troubleshooting](#troubleshooting)) |

The cache keeps tracks as FLAC when your Harmony offers it (smaller downloads, decoded locally) and as WAV otherwise
(`Flac=0` under `[Features]` forces WAV). Only tracks that pass a check are kept: a track must decode fully, and a
streamed track that comes out far shorter than the server said (for example a 30-second preview or a cut-off transfer) is
discarded and replaced. The cache never deletes tracks that are in a ready pool or in a slot; when it exceeds the cap it
removes the least recently used others. File names in the cache are hashes, not track names.

The slot files are what Skyrim actually plays, so their disk use is real: roughly 10 MB per minute of music for each
active environment (44.1 kHz 16-bit stereo), on top of the cache.

The plugin never overwrites a slot file that it did not create itself.

The game only plays slot files that already existed when it started. So the first time the plugin loads it creates all
24 slot files as one second of silence (about 4 MB together); they are replaced by real tracks while you play. If a slot
file cannot be played (the game skips it again and again), that environment goes back to vanilla music, the page says
why, and the log has the details; restart the game or change that environment's settings to try again.

## Performance and threading

- **No network or disk access happens on the game thread.** Downloads, decoding, file writes, the connection test and
  searches all run on background threads of their own. The game thread only does a small, periodic check (about every
  250 ms) of the music state and, when safe, points music records at the slot files that are already prepared; it also
  shows the notification described below.
- A track is placed into its slot file about 1.5 seconds after the previous one started playing, off the game thread.
- The settings page is drawn by the SKSE Menu Framework and only reads small copies of the state.
- Honest limits: the plugin uses CPU and disk while it decodes and writes tracks, and network bandwidth while it
  downloads. The plugin logs gaps in its own game-thread check; the
  periodic ~3 second gaps seen in test logs lined up with Skyrim's timed autosaves, not with the plugin's work. That is
  an observation from one tested setup (Linux/Proton), not a guarantee for yours. If you see hitches, turn features off one at a time (see
  [Kill switches](#kill-switches)) and send the log.

## When Harmony is not reachable

- The plugin never leaves you without music. If an environment's slot has no fresh track, its original vanilla tracks are
  restored (and the log says so loudly).
- While a track pool runs dry during play and Harmony stays away, you hear **earlier Harmony tracks of that environment
  again** (least recently played first, if they are still in the cache); when that music stops, vanilla music takes over.
  The plugin retries with growing waits (2 seconds, doubling up to 5 minutes).
- A notification appears at the top left: **"Jukebox: Harmony server not reachable - playing saved/vanilla music"**, again
  every 5 minutes while it stays down, and **"Jukebox: Harmony is back"** once when it recovers. A refused key shows
  **"Jukebox: Harmony rejected the key"** once per change of settings. They never appear over the main menu or a loading
  screen.
- INI keys under `[General]`: `ToastEnabled=0` switches the notifications off; `ToastIntervalMinutes=` (1 to 60, default 5)
  sets the repeat interval.
- A server address that is not even well-formed (for example missing `http://`) gives no notification; the settings
  page says what is wrong instead.

## Kill switches

If something goes wrong, you can switch parts off without uninstalling.

In `MarthJukebox.ini` (game closed, or use the toggles under **Advanced** in the page, which apply right away):

```
[Features]
Retarget=1       ; 0 = never replace Skyrim's music (everything stays vanilla)
Network=1        ; 0 = never use the network
Menu=1           ; 0 = the settings page is listed but disabled (set 1 and restart to use it)
Flac=1           ; 0 = download WAV instead of FLAC
RememberPlace=1  ; stored; resume is always on in 1.0
```

And an **emergency file**: create an empty file `Data/SKSE/Plugins/MarthJukebox.noretarget` (under MO2 that is
`overwrite/SKSE/Plugins/MarthJukebox.noretarget`). Within about 2 seconds the plugin stops, cancels any download and
restores the original music as soon as it is safe (a song that is currently playing finishes first). Delete the file and
it resumes.

## Uninstall

1. If you want the downloaded music gone, press **Clear cache** in the page while the game is still running (or delete
   the cache folder yourself afterwards; by default `Data/SKSE/Plugins/MarthJukebox/cache/`).
2. Quit the game.
3. Disable or remove the mod (MO2/Vortex), or delete `Data/SKSE/Plugins/MarthJukebox.dll` and `Data/MarthJukebox.esp`.
4. Optionally delete what the plugin created: `Data/SKSE/Plugins/MarthJukebox.ini` and its copies `MarthJukebox.ini.bak` and `MarthJukebox.ini.unreadable*` (they all hold your
   key in plain text), the folder
   `Data/SKSE/Plugins/MarthJukebox/`, the files `Data/Music/Harmony/slot_*.wav`, and the log `MarthJukebox.log`.

The plugin makes no change to your save, to other mods' files or to Skyrim's data files: the music records are only
edited in memory. With the plugin gone, Skyrim plays its normal music again. The ESP only adds 24 music records that nothing else
refers to, but removing it from an existing save has not been tested; make a backup of your saves first.

## Known limits

- **Tested on Linux/Proton with Mod Organizer 2 only (Skyrim 1.6.1170); the 1.0 build was played mainly on a Steam Deck.**
  A full play-through on a desktop LoreRim install was not part of this release's testing. The plugin replaces a music file with the next
  track while it is still playing (the game re-opens the file for every song). That worked every time on Proton. On
  **native Windows this is unverified**: if Windows refuses to replace a file the game has open, the plugin keeps the
  prepared file and retries when that slot is quiet (and logs it). It never forces it.
- **Long pauses seen on the Steam Deck.** In one Steam Deck session the log showed main-thread pauses of 15-26 seconds
  (loading screens, autosaves, and a few while a track was downloading; one with nothing of the plugin running). The
  log now says what the plugin was doing at each long pause (`[stall] ... | ours: ...`). If you see long hitches, turn
  features off one at a time (see [Kill switches](#kill-switches)) and send the log. This is under investigation.
- **The first start after install creates 24 short silent placeholder files** (`Music/Harmony/slot_NN.wav`, about 4 MB
  in total). The game only plays slot files that existed when it started, so the very first session after a fresh
  install can play Harmony music only in the environments whose slot already existed; restart the game once after
  installing for everything to work.
- **Skyrim 1.5.97 is unsupported.** Skyrim VR is untested.
- **Gamepad has no text entry** in the settings page (a limit of the SKSE Menu Framework): use a keyboard to type the
  server address and key.
- **Remember track place is always on** in 1.0.
- **Music from other mods** that the plugin does not know stays vanilla; the plugin cannot target an individual quest or
  scripted scene.
- A freshly chosen environment needs a moment: the first track has to be downloaded, so the vanilla music plays until it
  is ready. A streamed track can take several seconds to appear the first time.
- Planned, not in 1.0: a **mixed mode** (vanilla songs and Harmony tracks interleaved) is planned for 1.0.1; **track
  vetting** (filtering off-theme tracks, mostly on the Harmony side) is planned for 1.1.0.
- Very many active environments: there are 24 slots; extra environments beyond that stay vanilla (logged).

## Troubleshooting

**Where is the log?** `MarthJukebox.log` is written to the SKSE log folder, the same folder as SKSE's own `skse64.log`
(normally `Documents\My Games\Skyrim Special Edition\SKSE\`). On Linux/Proton that is inside the game's Wine prefix, in the
corresponding `Documents` folder. On the default info level it is short; set **Log level** (under Advanced, or `LogLevel=`
under `[General]`) to `debug` for details. The log never contains your key. When you send a log to someone, remove the
`PersonalKey` line from any INI you also send.

**The settings page is not in the menu.** The SKSE Menu Framework is missing or too old (the log says
`[menu] SKSE Menu Framework is not loaded` or names the missing export); the page is then not listed at all. With `[Features] Menu=0` the page is still listed but only says it is switched
off in the INI: set `Menu=1` and restart the game. Without the page, edit `MarthJukebox.ini` by hand (game closed).

**Nothing changes, music stays vanilla.** Look in the log: `network inactive (no server URL)` = no server address set;
`engine writes off` = `Retarget=0` or the `.noretarget` file exists; `DISABLED: MarthJukebox.esp is not in the load order`
(or a slot wrapper problem) = the ESP is not enabled or an old one is installed.

**Common log lines**

| Line (shortened) | Meaning |
|---|---|
| `[net] Connected. Sources: ...` | the server was reached and the key accepted |
| `[net] ... failed: ... next attempt in N s` | the server was not reachable or answered with an error; it retries with growing waits |
| `[profile] published N environment type(s)` | your choices were sent to the Harmony profile `skyrim-jukebox` that the plugin owns (it overwrites that profile) |
| `[plan] #n: engine writes on; ...` | which environments are active and how many music types were mapped |
| `[retarget] <type> -> slot NN (<env>)` | a Skyrim music type now plays from that environment's slot |
| `[retarget] <type> RESTORED to vanilla (...)` | the original tracks were put back (and why) |
| `[pool] <env>: + Artist - Title (...)` | a track was downloaded and checked and is ready |
| `[pool] <env>: Harmony has no tracks for this selection` | the selection is empty on the server; the environment stays vanilla, retry in 10 minutes |
| `[net] preview or truncated stream` / `pick rejected` | a downloaded track was too short or broken and was discarded |
| `[slots] slot NN (<env>) PLAY ... now playing ...` | the game started a track from that slot |
| `[slots] slot NN (<env>) <- Artist - Title` | the next track was written into the slot file |
| `[slots] ... rename retried` | Windows would not replace the file while it plays; retried when quiet |
| `[retarget] KILL SWITCH file present` | the `.noretarget` file exists |

If the game crashes, the log's last lines are the best clue; a crash dump from SKSE or a crash logger helps too. Please
open an issue at https://github.com/marthofdoom/marth-jukebox/issues with the log (key removed).

## Security

- The **personal key is stored in plain text** in `MarthJukebox.ini` (it has to be sent as a header with every request),
  and also in its copies `MarthJukebox.ini.bak` and `MarthJukebox.ini.unreadable*`. Do not share or upload any of them.
- The key (and your requests) go **only to the server address you entered**. The plugin has no other network
  destination, no telemetry and no auto-update.
- Use an **`https://`** server address whenever Harmony is reached from outside your own LAN or VPN. With plain `http://`
  the key travels unencrypted.
- The plugin owns one Harmony profile (named `skyrim-jukebox` unless you change `HarmonyProfileName`) and overwrites it
  with your current choices.

## AI-assisted development

This plugin was written largely by AI coding agents (Anthropic's Claude), working from specifications by the
maintainer. Each build was tested in the game by the maintainer, and the code was reviewed in a separate AI review pass. The source is public so
you can read it.

## Licence and credits

MIT, see `LICENSE`. Third-party components and their licences are listed in `THIRD-PARTY-NOTICES.md`:

- CommonLibSSE-NG (the MIT-licensed fork from `marthofdoom/vcpkg-registry`), [spdlog](https://github.com/gabime/spdlog)
  (MIT), [nlohmann/json](https://github.com/nlohmann/json) (MIT).
- [dr_flac](https://github.com/mackron/dr_libs) by David Reid (public domain / MIT-0), used to decode FLAC.
- The SKSE Menu Framework API header (GNU LGPL 2.1; text in `LICENSE-LGPL-2.1`). The framework itself is a separate mod
  by its own author and is not included here.

Design and developer documentation (not needed to use the mod) is in `DESIGN.md` and the `docs/` folder of the source.
