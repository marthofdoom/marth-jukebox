# Environments and settings reference

Contract for the menu and README authors. Code: `native/categories/Environments.{h,cpp}` (the table),
`native/config/Settings.{h,cpp}` (settings + INI), `native/config/ProfileBuilder.{h,cpp}` (Harmony profile).
Pure C++, no engine and no network; tested by `mj_core_tests` (CI step "Core tests").

## 1. How music types become environments

The plugin sees 215 `BGSMusicType` (MUSC) records on the primary LoreRim list (M0a log: 50 vanilla, the rest from
mods). The player never sees 215 rows: each music type is classified into one of **8 player-facing environment
groups** (plus 3 kept-vanilla groups that are only documented). A group is one Harmony `type`; a group may have
sub-environments (children) that can override it, whose Harmony type is `group.sub` (for example
`explore.tundra`).

`Classify(editorId, pluginName, hints) -> {groupId, subId, retargetable, diegetic, reason}`:

1. The table is keyed by **(plugin file, EDID)**, both compared case-insensitively. The plugin argument is the
   record's **origin file** (`TESForm::GetFile(0)`), not the plugin whose override wins. A vanilla EDID owned by some
   other plugin is *not* mapped: it is somebody else's music.
2. Exact rows are tried first; prefix rows (official plugins only) come last. First match wins.
3. `hints.allSilent` (the caller sets it when every track of the MUSC is a `BGSMusicSilenceTrack`) forces
   `retargetable = No` even for a mapped type.
4. **Unmapped** (anything not in the table): group `unmapped`, `retargetable = No`, `diegetic = false`.
   It stays vanilla, is never offered by default, and the advanced "Other mods" list may add it individually
   under the environment id `other.<EDID>` (`OtherModsEnvId()`; characters outside `[A-Za-z0-9_-]` become `_`).
   `other.<EDID>` applies only to records `Classify` returns as plain Unmapped: Settings refuses the id for any
   EDID the table knows (mapped, kept-vanilla, diegetic, known leftovers or an official prefix rule).
5. **Diegetic / kept vanilla** (`tavern`, `story`, `menu`): never retargetable, and `IsKnownEnvironmentId()`
   rejects them, so settings can never configure them.

Allow-list policy: only the official masters (`Skyrim.esm`, `Dawnguard.esm`, `Dragonborn.esm`, `Update.esm`)
and four explicit entries below are mapped from non-Skyrim plugins. Everything else defaults to Unmapped.

| Allow-list entry | Plugin | Why it is safe |
|---|---|---|
| `ccBGSSSE067_MUSCombatBoss` | ccbgssse067-daedinv.esm | Creation Club boss; flags 0x24, priority 2, fade 9 s, duck 10000, equal to the vanilla `MUSCombatBossDLC1` |
| `ccMUSDungeonOblivion` | Update.esm | Official file; priority 50 / fade 4 s equal `MUSDungeon` (flags are `None` instead of `CycleTracks`: the retarget path sets CycleTracks itself) |
| `EC_SS_MUSCombat` | Skyrim Extended Cut - Saints and Seducers.esp | flags 0x24, priority 2, duck 10000: the vanilla combat signature |
| `EC_SS_MUSDiscoveryTown`, `EC_SS_MUSDiscoverySpecial` | same | flags 0x25, priority 3, duck 3000: the vanilla `MUSDiscoveryDungeon` signature |

Deliberately NOT mapped although the name suggests a family: `EC_SS_ExploreDementia` / `EC_SS_ExploreMania`
(Sheogorath-realm themed music), `WTMUSExplore`/`WTMUSDungeon`/... (Wyrmstooth: bespoke land pools),
`000FCThrivingCity`/`MUSMorrowindCity` (explore-priority 49 "city" pools, not settlement music), every Vigilant
`zzzMUS*` boss/scene type (one scripted track each), `Eli_MUSVault` (priority 55, diegetic-looking).

## 2. Groups (menu order)

| Id | Display name | Kind | Sub-environments |
|---|---|---|---|
| `explore` | Exploration | playable | tundra, mountain, snow, marsh, coast, reach, forest_pine, forest_fall, generic |
| `town` | Towns and cities | playable | town, castle |
| `dungeon` | Dungeons | playable | cave, fort, ice (base = generic dungeon, plus the DLC dungeons) |
| `combat` | Combat | playable | civil_war |
| `boss` | Boss fights | playable | (none) |
| `discovery` | Discovery | playable | city, town, dungeon, generic, high_hrothgar, dread |
| `stinger` | Stingers and rewards | playable | reveal, reward, success, dungeon_cleared |
| `special` | Special moments | playable | death, alduin_death, odahviing, elder_scroll, eclipse, boat_arrival |
| `tavern` | Taverns and bards | kept vanilla (diegetic) | |
| `story` | Story and ceremonial | kept vanilla (diegetic) | sovngarde, hall_of_valor, intro, chargen, word_of_power (documentation only) |
| `menu` | Menus and placeholders | kept vanilla (all-silent) | |

Reasoning for the judgement calls:
- `MUSExploreTestb` (60 tracks, priority 50, CycleTracks) and `MUSTownTest` (45 tracks, priority 48): the "Test"
  names are Bethesda leftovers, but the flags and track counts are those of live exploration / town music.
  Mapped as `explore.generic` and `town.town`.
- `MUSDread` (PlaysOnce, priority 3) behaves like a discovery stinger, so it is `discovery.dread`, not a dungeon.
- `MUSE3DemoDungeon` / `MUSE3DemoExplore` (priority 100, UseTrackOrder): E3-demo leftovers, Unmapped.
- `MUSTavernSILENCE` has 3 Silent tracks only (it just ducks other music while a bard plays): never retargetable.
- `MUSSkillsMenu` and `_NONE` have only Silent tracks.
- `story` holds the scripted moments: Sovngarde chants (priority 1-3, AbruptTransition), Hall of Valor, intro
  cart, the Helgen / character-creation dungeon and dragon types, and the word-of-power cue.
- The `special` death / Alduin / Odahviing / Elder Scroll cues are scripted one-shots but not diegetic; they are
  offered (default Vanilla like everything else).

## 3. The classification table (215 music types from the real list)

Generated from the fixture `native/tests/core/fixtures/music_types.json` (names, plugins, flags, priority and
track counts only) and checked by the unit test against `expected_classification.tsv`.

Counts: explore 14, town 2, dungeon 7, combat 3, boss 6, discovery 8, stinger 5, special 6, tavern 3, story 10,
menu 2 (= 66 mapped or kept-vanilla-grouped) and 149 Unmapped (2 vanilla E3 leftovers, 147 mod types).

#### explore (14)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| coast | `MUSExploreCoast` | Skyrim.esm | 119 (19 silent) | yes | no |
| forest_fall | `MUSExploreForestFall` | Skyrim.esm | 130 (17 silent) | yes | no |
| forest_pine | `MUSExploreForestPine` | Skyrim.esm | 129 (22 silent) | yes | no |
| generic | `DLC2MUSExploreSolstheimGeneral` | Dragonborn.esm | 21 (2 silent) | yes | no |
| generic | `MUSExploreDLCFalmerValley` | Dawnguard.esm | 12 (7 silent) | yes | no |
| generic | `MUSExploreDLCSoulCairn` | Dawnguard.esm | 8 (4 silent) | yes | no |
| generic | `MUSExploreTestb` | Skyrim.esm | 60 (13 silent) | yes | no |
| marsh | `MUSExploreMarsh` | Skyrim.esm | 108 (20 silent) | yes | no |
| mountain | `DLC2MUSExploreMountain` | Dragonborn.esm | 49 (15 silent) | yes | no |
| mountain | `MUSExploreMountain` | Skyrim.esm | 124 (28 silent) | yes | no |
| reach | `MUSExploreReach` | Skyrim.esm | 90 (18 silent) | yes | no |
| snow | `DLC2MUSExploreSnow` | Dragonborn.esm | 46 (14 silent) | yes | no |
| snow | `MUSExploreSnow` | Skyrim.esm | 90 (17 silent) | yes | no |
| tundra | `MUSExploreTundra` | Skyrim.esm | 107 (16 silent) | yes | no |

#### town (2)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| castle | `MUSCastle` | Skyrim.esm | 12 (4 silent) | yes | no |
| town | `MUSTownTest` | Skyrim.esm | 45 (7 silent) | yes | no |

#### dungeon (7)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| (base) | `DLC2MUSDungeonApocrypha` | Dragonborn.esm | 2 | yes | no |
| (base) | `MUSDungeon` | Skyrim.esm | 8 | yes | no |
| (base) | `MUSDungeonDLCVampireCastle` | Dawnguard.esm | 10 (4 silent) | yes | no |
| (base) | `ccMUSDungeonOblivion` | Update.esm | 5 | yes | no |
| cave | `MUSDungeonCave` | Skyrim.esm | 7 | yes | no |
| fort | `MUSDungeonFort` | Skyrim.esm | 10 | yes | no |
| ice | `MUSDungeonIce` | Skyrim.esm | 7 | yes | no |

#### combat (3)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| (base) | `EC_SS_MUSCombat` | Skyrim Extended Cut - Saints and Seducers.esp | 4 | yes | no |
| (base) | `MUSCombat` | Skyrim.esm | 30 | yes | no |
| civil_war | `MUSCombatCivilWar` | Skyrim.esm | 3 | yes | no |

#### boss (6)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| (base) | `DLC2MUSCombatBoss` | Dragonborn.esm | 3 | yes | no |
| (base) | `DLC2MUSCombatKarstaag` | Dragonborn.esm | 3 | yes | no |
| (base) | `MUSCombatBoss` | Skyrim.esm | 8 | yes | no |
| (base) | `MUSCombatBossDLC1` | Dawnguard.esm | 4 | yes | no |
| (base) | `MUSCombatBossUmbra` | Update.esm | 1 | yes | no |
| (base) | `ccBGSSSE067_MUSCombatBoss` | ccbgssse067-daedinv.esm | 3 | yes | no |

#### discovery (8)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| city | `MUSDiscoveryCity` | Skyrim.esm | 3 | yes | no |
| dread | `MUSDread` | Skyrim.esm | 4 | yes | no |
| dungeon | `MUSDiscoveryDungeon` | Skyrim.esm | 4 | yes | no |
| generic | `EC_SS_MUSDiscoverySpecial` | Skyrim Extended Cut - Saints and Seducers.esp | 1 | yes | no |
| generic | `MUSDiscoveryGeneric` | Skyrim.esm | 3 | yes | no |
| high_hrothgar | `MUSDiscoveryHighHrothgar` | Skyrim.esm | 1 | yes | no |
| town | `EC_SS_MUSDiscoveryTown` | Skyrim Extended Cut - Saints and Seducers.esp | 1 | yes | no |
| town | `MUSDiscoveryTown` | Skyrim.esm | 3 | yes | no |

#### stinger (5)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| (base) | `MUSStinger` | Skyrim.esm | 5 | yes | no |
| dungeon_cleared | `MUSDungeonCleared` | Skyrim.esm | 3 | yes | no |
| reveal | `MUSReveal` | Skyrim.esm | 6 | yes | no |
| reward | `MUSReward` | Skyrim.esm | 3 | yes | no |
| success | `MUSSpecialSuccess` | Skyrim.esm | 3 | yes | no |

#### special (6)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| alduin_death | `MUSAlduinDeath` | Skyrim.esm | 1 | yes | no |
| boat_arrival | `MUSDLC2BoatArrival` | Dragonborn.esm | 1 | yes | no |
| death | `MUSSpecialDeath` | Skyrim.esm | 6 | yes | no |
| eclipse | `MUSEclipse` | Dawnguard.esm | 1 | yes | no |
| elder_scroll | `MUSSpecialElderScrollSquence` | Skyrim.esm | 1 | yes | no |
| odahviing | `MUSOdahviingRiding` | Skyrim.esm | 1 | yes | no |

#### tavern (3)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| (base) | `MUSTavern` | Skyrim.esm | 6 | no | yes |
| (base) | `MUSTavernB` | Skyrim.esm | 9 | no | yes |
| (base) | `MUSTavernSILENCE` | Skyrim.esm | 3 (3 silent) | no | no |

#### story (10)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| chargen | `MUSCombatBossChargen` | Skyrim.esm | 4 | no | yes |
| chargen | `MUSDungeonChargen` | Skyrim.esm | 7 | no | yes |
| hall_of_valor | `MUSSovngardeHallofValor` | Skyrim.esm | 2 | no | yes |
| hall_of_valor | `MUSSpecialHallofValor` | Skyrim.esm | 3 | no | yes |
| intro | `MUSSpecialCart` | Skyrim.esm | 1 | no | yes |
| intro | `MUSSpecialIntro` | Skyrim.esm | 1 | no | yes |
| sovngarde | `MUSExploreSovngarde` | Skyrim.esm | 2 | no | yes |
| sovngarde | `MUSExploreSovngardeChant` | Skyrim.esm | 2 | no | yes |
| sovngarde | `MUSExploreSovngardeChantExterior` | Skyrim.esm | 2 | no | yes |
| word_of_power | `MUSSpecialWordOfPower` | Skyrim.esm | 3 | no | yes |

#### menu (2)

| Sub | EDID | Plugin | Tracks | Retargetable | Diegetic |
|---|---|---|---|---|---|
| (base) | `MUSSkillsMenu` | Skyrim.esm | 1 (1 silent) | no | no |
| (base) | `_NONE` | Skyrim.esm | 1 (1 silent) | no | no |

#### Unmapped types seen in the LoreRim log (149)

- **Skyrim.esm** (2): `MUSE3DemoDungeon`, `MUSE3DemoExplore`
- **018Auri.esp** (1): `018MUSAuriHome`
- **AncientBloodII.esl** (1): `_AII_MUSCombatBoss`
- **Ascend - Hidden Peaks of Skyrim.esp** (2): `Haiku_MusicType`, `Haiku_Silence`
- **BetalillesHammerfellQuestBundle.esp** (1): `DDB_manny_GF_MusicAlShedim`
- **Eli_Dovah Vault.esp** (1): `Eli_MUSVault`
- **evgSIRENROOT.esm** (29): `evgSRMUS06onlyDARK`, `evgSRMUShonrichtemple`, `evgSRMUShonrichtemple07DARK`, `evgSRMUShonrichtempleDARK`, `evgSRMUShonrichtempleDARK05`, `evgSRMUShonrichtempleLIGHT04`, `evgSRMUShonrichtempleLIGHT08`, `evgSRMUShonrichtempleMIX`, `evgSRMUShonrichtempleMIX09`, `evgSRMUSmainhall2`, `evgSRMUSmainhall3`, `evgSRMUSmainhallonly`, `evgSRMUSRcayricedoors`, `evgSRMUSReveal2`, `evgSRMUSRpeletiusreveal`, `evgSRMUSRpeletiusrpancake`, `evgSRMUStype06collapse`, `evgSRMUStypechimesecrets`, `evgSRMUStypeFINALBOSS`, `evgSRMUStypefirstcutscene`, `evgSRMUStypeHorrorHeavy`, `evgSRMUStypeHorrorLight`, `evgSRMUStypeHorrorMedium`, `evgSRMUStypeLarelleisAppear`, `evgSRMUStypetemplefirstsight`, `evgSRMUStypetemplereveal`, `evgSRMUStypetimer40panic`, `evgSRMUStypetrance`, `evgSRMUSyineellost`
- **FalkreathShades.esp** (3): `GRVEMUSCombatGolemBoss`, `GRVEMUSCombatWraithmotherBoss`, `GRVEMUSExploreUndercity`
- **ForgottenCity.esp** (9): `000FCMUSArbiter`, `000FCMUSArbiterMotif`, `000FCMUSCombat`, `000FCMUSDiscovery`, `000FCMUSDread`, `000FCMUSDreadHighPriority`, `000FCMUSExplore`, `000FCMUSJudgment`, `000FCThrivingCity`
- **GORE.esp** (9): `HD1CampsiteSounds`, `HD1GoreCombatMusic`, `HD1InterludeMusic`, `MUSGoreCombat`, `MusGoreCombatLyrics`, `MUSGoreInterlude`, `MUSGoreNighttime`, `MUSGoreSad1`, `MUSGoreSad2`
- **Gray Fox Cowl.esm** (15): `manny_GF_MusicAkavir`, `manny_GF_MusicAlikrDesert`, `manny_GF_MusicAlShedim`, `manny_GF_MusicBenErai`, `manny_GF_MusicCaveOfMoraSul`, `manny_GF_MusicColdharbour`, `manny_GF_MusicCombatCursedCity`, `manny_GF_MusicCombatThePit`, `manny_GF_MusicDread`, `manny_GF_MusicEndTitles`, `manny_GF_MusicGrayCowlFound`, `manny_GF_MusicHallOfTheWest`, `manny_GF_MusicMausoleumChampion`, `manny_GF_MusicMausoleumSheogorath`, `manny_GF_MusicOblivionTheme`
- **Journey to Baan Malur.esp** (1): `MUSMorrowindCity`
- **Katana.esp** (2): `AK69CozyMusic`, `AK69MUSHideout`
- **SeranaDialogueExpansion - Romance.esp** (1): `SDE_RomanceTheme`
- **Skyrim Extended Cut - Saints and Seducers.esp** (2): `EC_SS_ExploreDementia`, `EC_SS_ExploreMania`
- **SkyrimsGotTalent-Bards.esp** (1): `_Talent_Silence`
- **Tools of Kagrenac.esp** (2): `TOK_MUSC_DungeonAyleid`, `TOK_MUSC_RevealAyleid`
- **Undeath.esp** (8): `NecroMUSDiscoveryDragontailMountains`, `NecroMUSDiscoveryScourgBarrow`, `NecroMUSDungeonApocrypha`, `NecroMUSDungeonBrokerShack`, `NecroMUSDungeonRavenscornSpire`, `NecroMUSDungeonScourgBarrow`, `NecroMUSDungeonSewers`, `NecroMUSExploreDragontailMountains`
- **Vigilant.esm** (48): `zCHMusColdharbourRealm`, `zCHMusDungeonCave`, `zCHMUSDungeonDarkSewer`, `zCHMusDungeonFort`, `zCHMusDungeonMadKing`, `zCHMusDungeonOrder`, `zCHMusDungeonPrison`, `zCHMusDungeonTemple`, `zCHMusInterior`, `zCHMusIntPrayRoom`, `zCHMUSSardCoffin`, `zCHMusWaterfront`, `zCOMusCursedMansion`, `zCOMUSMansion`, `zzzMUSAbbot`, `zzzMUSAredhel`, `zzzMUSBloodSuckerKing`, `zzzMUSBloodWork`, `zzzMUSColosseum`, `zzzMUSDyingPelinal`, `zzzMUSEnding`, `zzzMUSEndingBitter`, `zzzMUSEndingTrue`, `zzzMUSEndingWorst`, `zzzMUSGorieus`, `zzzMUSJuncan`, `zzzMUSJyggalag`, `zzzMUSKeeper`, `zzzMUSLamae`, `zzzMUSLaza`, `zzzMUSMaraKnight`, `zzzMUSMaram`, `zzzMUSMary`, `zzzMUSMinos`, `zzzMUSMolag`, `zzzMUSPelinal`, `zzzMUSPiper`, `zzzMUSRequiem`, `zzzMUSReyda`, `zzzMUSRitho`, `zzzMUSSelector`, `zzzMUSStillbornKing`, `zzzMUSTorolf`, `zzzMUSTower`, `zzzMUSUmaril`, `zzzMUSVarla`, `zzzMUSWarChant`, `zzzMUSWasteland`
- **Wyrmstooth.esp** (11): `WTMUSBarrow`, `WTMUSBoatIntro`, `WTMUSDimfrost`, `WTMUSDragonKilled`, `WTMUSDragonSpeech`, `WTMUSDungeon`, `WTMUSExplore`, `WTMUSReveal`, `WTMUSRevealBarrow01`, `WTMUSRevealBarrow02`, `WTMUSRevealBarrow03`

## 4. Settings reference (`Data/SKSE/Plugins/MarthJukebox.ini`)

The path is injected by the caller; the module does no engine calls. Writes are atomic (`MarthJukebox.ini.tmp`,
then replace); a stale `.tmp` from a crashed save is ignored and deleted on load. The file is versioned
(`Version=1`), tolerant of hand edits (BOM, CRLF, comments `;` and `#`, any key case, spaces around `=`,
duplicate keys: last wins, unknown keys / sections / environments ignored and logged, bad values corrected
and logged). Messages never echo user-supplied text: no values of `ServerUrl` / `PersonalKey`, and no unknown
key, section or environment names (only "line N: unknown key ignored"). Comments and unknown keys are not
preserved when the plugin rewrites the file.

Protecting the user's file (and key): the temp file is flushed to disk before the replace. The first `Save` of a
session keeps the previous good file as `MarthJukebox.ini.bak` (copies are written through the same flushed
path as the INI, so they are on disk before the original is replaced). If `Load` finds an existing file it cannot use
(unreadable, empty, zero-filled, UTF-16, no recognised key) it publishes defaults and returns false; the next
`Save` first copies the original bytes to `MarthJukebox.ini.unreadable` (`.unreadable.1`, ... never overwriting
an earlier copy) and refuses to save if that copy fails. A failed replace leaves the original untouched and
removes the temp file.

First-run defaults (1.0.1, fresh install only: used when no INI exists; an existing INI keeps its meaning, an environment
without a section stays Vanilla), both with streaming off, so they yield nothing until the player ticks streaming or has
matching library tracks:

| Environment | Mode | Selector | Service / Id | AllowStreaming | PoolSize |
|---|---|---|---|---|---|
| `explore` | Query | `Brass Fantasy Adventure` | - | 0 | 3 |
| `town` | Artist | `Per-Olov Kindgren` | `ytmusic` / `UCL5frAZoI4rtafCi2BqPDAw` | 0 | 3 |

An Artist or Playlist with a typed name but no picked suggestion (no service/id) is left out of the profile with the
message "pick the artist from the suggestions": Harmony's selectors need the service id.

Defaults are safe: every other environment Vanilla, and the network is off until a server URL is set
(`NetworkActive()` = `Network` switch on AND `ServerUrl` non-empty). Values are clamped, with the reason logged.

### `[General]`

| Key | Default | Range / rule |
|---|---|---|
| `Version` | 1 | positive integer; newer than the build understands is logged |
| `ServerUrl` | empty | `http://` or `https://` + host, no spaces, <= 512 chars, trailing `/` removed; invalid = cleared (network stays off) |
| `PersonalKey` | empty | <= 256 chars, **stored in plain text** (the file carries a warning comment line) |
| `HarmonyProfileName` | `skyrim-jukebox` | Harmony profile name: 1-64 of `[A-Za-z0-9_.-]`, starting alphanumeric; invalid = default |
| `CacheDir` | empty | empty = the caller's default (`Data\SKSE\Plugins\MarthJukebox\cache`); <= 520 chars |
| `CacheCapMB` | 4096 | 256 .. 1048576 (below 256 is raised to 256) |
| `LogLevel` | `info` | `trace` `debug` `info` `warn` `error` `off` |
| `ToastEnabled` | 1 | 1 = in-game notification when Harmony is not reachable while music plays from it (after ~20 s or two failed requests), repeated every `ToastIntervalMinutes`, one "Harmony is back" on recovery, and "Harmony rejected the key" once per settings change. Never without a server URL |
| `ToastIntervalMinutes` | 5 | 1 .. 60 (out of range is clamped, logged) |
| `AllowQobuz` | 0 | INI only (not in the menu). 1 = a streaming environment may also use Qobuz (`sources=library,ytmusic,qobuz`). Off by default: without a subscription Qobuz serves 30 s previews (DESIGN 8.8) |

### `[Features]` (kill switches, 1 = on, default all on)

`Retarget`, `Network`, `Menu`, `Flac`, `RememberPlace`. Booleans accept `1/0 true/false yes/no on/off`.

### `[Env:<id>]` (one section per configured environment)

`<id>` is a playable group id (`explore`), a sub-environment (`explore.tundra`) or `other.<EDID>` (advanced
list of Unmapped types). Unknown ids are ignored and logged. A sub-environment section that exists **overrides
its group, Vanilla included** ("keep this sub vanilla"); no section = the sub inherits the group
(`ResolveEnvironment()`).

| Key | Default | Range / rule |
|---|---|---|
| `Mode` | `Vanilla` | `Vanilla` `Genre` `Artist` `Playlist` `Query` |
| `Selector` | empty | genre or query text; for Artist/Playlist the display label. Loaded up to 1024 chars; Harmony limit (200 characters, no control characters, valid UTF-8) is enforced by the profile builder |
| `Service` | empty | `local` or `ytmusic` for Artist/Playlist. **`qobuz` and anything else is refused** (Artist/Playlist then fall back to Vanilla; DESIGN 8.8) |
| `Id` | empty | service id from `/api/search`; <= 256 loaded, <= 200 of `[A-Za-z0-9_.:@+=~\-\[\]]` for Harmony |
| `AllowStreaming` | 0 | 0 = Harmony `sources=library`; 1 = `sources=library,ytmusic` (+`,qobuz` only with `[General] AllowQobuz=1`). A Harmony older than 1.0.13 has no source list: streaming then needs AllowQobuz=1 (`all`), else the environment stays on the library. Passed per `next` call (`ChooseSources()`), not part of the profile |
| `RememberPlace` | 0 | stored and shown; in 1.0 the engine always resumes an interrupted track (its own default), see DESIGN 3 item 4 |
| `PoolSize` | 3 | 3, 5 or 8; other values snap to the nearest (ties up) |

Limits: at most 128 environment sections are kept.

### API summary (what the other components call)

- `SettingsStore(iniPath, defaultCacheDir)`: `Load()`, `Save(&err)`, `Snapshot()` (immutable `shared_ptr`, safe on
  any thread), `Revision()` (monotonic, bumped by every `Load` / `Update` / `ResetToDefaults`, also stored in
  `Settings::revision`), `Update(fn)` (copy-on-write; concurrent updates serialize, none is lost).
- `SetLogSink()` redirects the module's log lines (default: spdlog default logger, prefix `[settings]`).

## 5. Harmony profile (`BuildHarmonyProfile`)

`PUT /api/game/profiles/{HarmonyProfileName}` body (see `ProfilePath()`):

```json
{
  "types": {
    "explore":        {"genre": "Ambient"},
    "explore.tundra": {"genre": "Folk"},
    "combat":         {"query": "epic battle music orchestral"}
  },
  "normalize": true,
  "format": {"rate": 44100, "bits": 16, "channels": 2},
  "sources": "library"
}
```

- `normalize` is always `true`; `format.container` is omitted (transport layer decides).
- Selectors: Genre -> `genre`, Query -> `query`, Artist -> `artist{service,id}`, Playlist -> `playlist{service,id}`.
- There is **no generated `default` type**. The plugin only asks for published types, and an environment that
  failed validation stays Vanilla instead of silently playing another environment's selector. An environment
  literally named `default` is rejected as reserved.
- Profile-level `sources` is **always `"library"`** (streaming is a per-environment opt-in, NEXUS-COMPLIANCE).
  Omitting it would make Harmony default to `all` (which includes Qobuz) for every type, including environments
  that never opted in. Every `next` call passes `sources` explicitly from `AllowStreaming`, which overrides it.
- Validation (per environment, reported as `ProfileProblem` and logged; **Error** = environment left out,
  **Warning** = published): type name 1-64 of `[A-Za-z0-9_.-]` starting alphanumeric; at most 64 types including
  all environments; text trimmed, 1-200 code points, no control characters, valid UTF-8; Artist/Playlist
  service `local`/`ytmusic` and a valid id. Warnings: YouTube Music selector while streaming is off; a local
  playlist.
- Returns an empty string when nothing is publishable (all Vanilla, or all invalid): do not PUT then.
- `BuildProfile()` returns the JSON, the set of published type names and the problems. Pass it (required) to
  `ResolveEnvironment(settings, group, sub, build)`: it gives the type name to pass to `next` (sub if it has a
  section, else the group) and `Active()`, which is false for Vanilla AND for an environment the builder left
  out with an Error (it stays Vanilla; it does not fall back to the group).
