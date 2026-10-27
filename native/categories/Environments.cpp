#include "categories/Environments.h"

#include <algorithm>
#include <array>

namespace mj::categories
{
	namespace
	{
		constexpr auto Y = Retargetable::Yes;
		constexpr auto N = Retargetable::No;

		// ------------------------------------------------------------------------------------------
		// Sub-environments (children). Registered here so settings can validate "group.sub" ids.
		// ------------------------------------------------------------------------------------------
		constexpr std::array kExploreSubs{
			SubInfo{ "tundra", "Tundra" },
			SubInfo{ "mountain", "Mountains" },
			SubInfo{ "snow", "Snow" },
			SubInfo{ "marsh", "Marsh" },
			SubInfo{ "coast", "Coast" },
			SubInfo{ "reach", "The Reach" },
			SubInfo{ "forest_pine", "Pine forest" },
			SubInfo{ "forest_fall", "Autumn forest" },
			SubInfo{ "generic", "Generic / other regions" },
		};
		constexpr std::array kTownSubs{
			SubInfo{ "town", "Towns and cities" },
			SubInfo{ "castle", "Castles and keeps" },
		};
		constexpr std::array kDungeonSubs{
			SubInfo{ "cave", "Caves" },
			SubInfo{ "fort", "Forts" },
			SubInfo{ "ice", "Ice caverns" },
		};
		constexpr std::array kCombatSubs{
			SubInfo{ "civil_war", "Civil War battles" },
		};
		constexpr std::array kDiscoverySubs{
			SubInfo{ "city", "Discovering a city" },
			SubInfo{ "town", "Discovering a town" },
			SubInfo{ "dungeon", "Discovering a dungeon" },
			SubInfo{ "generic", "Discovering a place" },
			SubInfo{ "high_hrothgar", "High Hrothgar" },
			SubInfo{ "dread", "Dread (ominous places)" },
		};
		constexpr std::array kStingerSubs{
			SubInfo{ "reveal", "Reveal" },
			SubInfo{ "reward", "Reward" },
			SubInfo{ "success", "Success" },
			SubInfo{ "dungeon_cleared", "Dungeon cleared" },
		};
		constexpr std::array kSpecialSubs{
			SubInfo{ "death", "Player death" },
			SubInfo{ "alduin_death", "Alduin's defeat" },
			SubInfo{ "odahviing", "Riding Odahviing" },
			SubInfo{ "elder_scroll", "Elder Scroll reading" },
			SubInfo{ "eclipse", "Dawnguard eclipse" },
			SubInfo{ "boat_arrival", "Arriving on Solstheim" },
		};
		// Kept-vanilla groups list their members as subs for documentation only; the menu never offers them.
		constexpr std::array kStorySubs{
			SubInfo{ "sovngarde", "Sovngarde" },
			SubInfo{ "hall_of_valor", "Hall of Valor" },
			SubInfo{ "intro", "Intro cart ride" },
			SubInfo{ "chargen", "Character creation" },
			SubInfo{ "word_of_power", "Word of power" },
		};
		constexpr std::array<SubInfo, 0> kNoSubs{};

		// Menu order: playable groups first, then the kept-vanilla ones.
		constexpr std::array kGroups{
			GroupInfo{ "explore", "Exploration", "Overland travel music; the longest-running track in the game.", GroupKind::Playable, kExploreSubs },
			GroupInfo{ "town", "Towns and cities", "Music inside settlements and keeps.", GroupKind::Playable, kTownSubs },
			GroupInfo{ "dungeon", "Dungeons", "Caves, forts, ice caverns and other delves.", GroupKind::Playable, kDungeonSubs },
			GroupInfo{ "combat", "Combat", "Normal fights.", GroupKind::Playable, kCombatSubs },
			GroupInfo{ "boss", "Boss fights", "Dragons and other boss encounters.", GroupKind::Playable, kNoSubs },
			GroupInfo{ "discovery", "Discovery", "Short one-shot music when you find a place.", GroupKind::Playable, kDiscoverySubs },
			GroupInfo{ "stinger", "Stingers and rewards", "Quest reveal / reward / success cues and the generic stinger.", GroupKind::Playable, kStingerSubs },
			GroupInfo{ "special", "Special moments", "Death, Alduin's defeat, Odahviing, Elder Scroll, DLC set pieces.", GroupKind::Playable, kSpecialSubs },
			GroupInfo{ "tavern", "Taverns and bards", "Diegetic: the bard plays in the room. Always vanilla.", GroupKind::KeptVanilla, kNoSubs },
			GroupInfo{ "story", "Story and ceremonial", "Sovngarde, Hall of Valor, intro cart, character creation, word-of-power cues. Always vanilla.", GroupKind::KeptVanilla, kStorySubs },
			GroupInfo{ "menu", "Menus and placeholders", "All-silent types (Skills menu, _NONE). Always vanilla.", GroupKind::KeptVanilla, kNoSubs },
		};

		// ------------------------------------------------------------------------------------------
		// The table. First match wins; exact rows before prefix rows. Every row names its plugin: a
		// record with a vanilla EDID in some other plugin is NOT mapped (it is unknown music).
		// The hints in the reasons come from the M0a log (priority / flags / track kinds).
		// ------------------------------------------------------------------------------------------
		constexpr std::string_view kSkyrim = "Skyrim.esm";
		constexpr std::string_view kDawnguard = "Dawnguard.esm";
		constexpr std::string_view kDragonborn = "Dragonborn.esm";
		constexpr std::string_view kUpdate = "Update.esm";
		constexpr std::string_view kCcDaedInv = "ccbgssse067-daedinv.esm";
		constexpr std::string_view kExtCutSS = "Skyrim Extended Cut - Saints and Seducers.esp";

		constexpr std::array kRules{
			// ---- explore: priority 49, CycleTracks, 90-130 tracks per region ----
			Rule{ "explore", "tundra", kSkyrim, "MUSExploreTundra", false, Y, false, "Vanilla region exploration music" },
			Rule{ "explore", "mountain", kSkyrim, "MUSExploreMountain", false, Y, false, "Vanilla region exploration music" },
			Rule{ "explore", "snow", kSkyrim, "MUSExploreSnow", false, Y, false, "Vanilla region exploration music" },
			Rule{ "explore", "marsh", kSkyrim, "MUSExploreMarsh", false, Y, false, "Vanilla region exploration music" },
			Rule{ "explore", "coast", kSkyrim, "MUSExploreCoast", false, Y, false, "Vanilla region exploration music" },
			Rule{ "explore", "reach", kSkyrim, "MUSExploreReach", false, Y, false, "Vanilla region exploration music" },
			Rule{ "explore", "forest_pine", kSkyrim, "MUSExploreForestPine", false, Y, false, "Vanilla region exploration music" },
			Rule{ "explore", "forest_fall", kSkyrim, "MUSExploreForestFall", false, Y, false, "Vanilla region exploration music" },
			Rule{ "explore", "generic", kSkyrim, "MUSExploreTestb", false, Y, false, "Vanilla 60-track CycleTracks explore pool (priority 50) used as the generic fallback; ambiguous name, treated as exploration" },
			// Official DLC exploration: same family, own pools.
			Rule{ "explore", "mountain", kDragonborn, "DLC2MUSExploreMountain", false, Y, false, "Dragonborn (official) Solstheim mountains; same signature as the vanilla region type" },
			Rule{ "explore", "snow", kDragonborn, "DLC2MUSExploreSnow", false, Y, false, "Dragonborn (official) Solstheim snow; same signature as the vanilla region type" },
			Rule{ "explore", "generic", kDragonborn, "DLC2MUSExploreSolstheimGeneral", false, Y, false, "Dragonborn (official) Solstheim general exploration" },
			Rule{ "explore", "generic", kDawnguard, "MUSExploreDLCFalmerValley", false, Y, false, "Dawnguard (official) exterior exploration pool" },
			Rule{ "explore", "generic", kDawnguard, "MUSExploreDLCSoulCairn", false, Y, false, "Dawnguard (official) Soul Cairn exploration pool" },

			// ---- town: priority 47/48, CycleTracks ----
			Rule{ "town", "town", kSkyrim, "MUSTownTest", false, Y, false, "Vanilla 45-track town pool (priority 48); the 'Test' name is a Bethesda leftover but it is the live town music" },
			Rule{ "town", "castle", kSkyrim, "MUSCastle", false, Y, false, "Vanilla castle / keep music (priority 47, 12 tracks)" },

			// ---- dungeon: priority 50, CycleTracks ----
			Rule{ "dungeon", "", kSkyrim, "MUSDungeon", false, Y, false, "Vanilla generic dungeon music (priority 50)" },
			Rule{ "dungeon", "cave", kSkyrim, "MUSDungeonCave", false, Y, false, "Vanilla cave music" },
			Rule{ "dungeon", "fort", kSkyrim, "MUSDungeonFort", false, Y, false, "Vanilla fort music" },
			Rule{ "dungeon", "ice", kSkyrim, "MUSDungeonIce", false, Y, false, "Vanilla ice-cavern music" },
			Rule{ "dungeon", "", kDawnguard, "MUSDungeonDLCVampireCastle", false, Y, false, "Dawnguard (official) vampire castle dungeon music" },
			Rule{ "dungeon", "", kDragonborn, "DLC2MUSDungeonApocrypha", false, Y, false, "Dragonborn (official) Apocrypha dungeon music" },
			// ccMUSDungeonOblivion sits in Update.esm (Creation Club content merged into the official update); flags
			// are 'None' but priority 50 and fade 4 s equal MUSDungeon exactly.
			Rule{ "dungeon", "", kUpdate, "ccMUSDungeonOblivion", false, Y, false, "Creation Club Oblivion-gate dungeon music (official Update.esm); priority 50 / fade 4 s equal MUSDungeon" },

			// ---- combat: priority 1-2, CycleTracks|PlaysOver, ducks other music by 10000 ----
			Rule{ "combat", "", kSkyrim, "MUSCombat", false, Y, false, "Vanilla combat music (30 tracks)" },
			Rule{ "combat", "civil_war", kSkyrim, "MUSCombatCivilWar", false, Y, false, "Vanilla Civil War battle music" },
			// Saints and Seducers combat: identical flags/priority/duck to the vanilla boss/combat types.
			Rule{ "combat", "", kExtCutSS, "EC_SS_MUSCombat", false, Y, false, "Allow-list: Skyrim Extended Cut S&S combat; flags 0x24, priority 2, duck 10000 equal the vanilla combat family" },

			// ---- boss ----
			Rule{ "boss", "", kSkyrim, "MUSCombatBoss", false, Y, false, "Vanilla boss music (8 tracks)" },
			Rule{ "boss", "", kDawnguard, "MUSCombatBossDLC1", false, Y, false, "Dawnguard (official) boss music" },
			Rule{ "boss", "", kUpdate, "MUSCombatBossUmbra", false, Y, false, "Official Update.esm Umbra boss music (1 track)" },
			Rule{ "boss", "", kDragonborn, "DLC2MUSCombatBoss", false, Y, false, "Dragonborn (official) boss music" },
			Rule{ "boss", "", kDragonborn, "DLC2MUSCombatKarstaag", false, Y, false, "Dragonborn (official) Karstaag boss music" },
			Rule{ "boss", "", kCcDaedInv, "ccBGSSSE067_MUSCombatBoss", false, Y, false, "Allow-list: Creation Club (Daedric Invasion) boss; flags 0x24, priority 2, fade 9 equal MUSCombatBossDLC1" },

			// ---- discovery: PlaysOnce one-shots, priority 2-3 (100 for city/town) ----
			Rule{ "discovery", "city", kSkyrim, "MUSDiscoveryCity", false, Y, false, "Vanilla discovery stinger" },
			Rule{ "discovery", "town", kSkyrim, "MUSDiscoveryTown", false, Y, false, "Vanilla discovery stinger" },
			Rule{ "discovery", "dungeon", kSkyrim, "MUSDiscoveryDungeon", false, Y, false, "Vanilla discovery stinger" },
			Rule{ "discovery", "generic", kSkyrim, "MUSDiscoveryGeneric", false, Y, false, "Vanilla discovery stinger" },
			Rule{ "discovery", "high_hrothgar", kSkyrim, "MUSDiscoveryHighHrothgar", false, Y, false, "Vanilla discovery stinger (1 track)" },
			Rule{ "discovery", "dread", kSkyrim, "MUSDread", false, Y, false, "Vanilla PlaysOnce, priority 3 ominous-place cue; behaves like a discovery stinger" },
			Rule{ "discovery", "town", kExtCutSS, "EC_SS_MUSDiscoveryTown", false, Y, false, "Allow-list: Skyrim Extended Cut S&S discovery; flags 0x25 / priority 3 / duck 3000 equal MUSDiscoveryDungeon" },
			Rule{ "discovery", "generic", kExtCutSS, "EC_SS_MUSDiscoverySpecial", false, Y, false, "Allow-list: Skyrim Extended Cut S&S discovery; same signature as EC_SS_MUSDiscoveryTown" },

			// ---- stinger ----
			Rule{ "stinger", "", kSkyrim, "MUSStinger", false, Y, false, "Vanilla generic stinger (5 tracks, priority 2, duck 1200)" },
			Rule{ "stinger", "reveal", kSkyrim, "MUSReveal", false, Y, false, "Vanilla quest reveal cue (6 tracks)" },
			Rule{ "stinger", "reward", kSkyrim, "MUSReward", false, Y, false, "Vanilla reward cue" },
			Rule{ "stinger", "success", kSkyrim, "MUSSpecialSuccess", false, Y, false, "Vanilla success cue (PlaysOnce, abrupt)" },
			Rule{ "stinger", "dungeon_cleared", kSkyrim, "MUSDungeonCleared", false, Y, false, "Vanilla dungeon-cleared cue" },

			// ---- special: scripted one-shots, priority 1-2, not diegetic ----
			Rule{ "special", "death", kSkyrim, "MUSSpecialDeath", false, Y, false, "Vanilla player-death cue (6 tracks)" },
			Rule{ "special", "alduin_death", kSkyrim, "MUSAlduinDeath", false, Y, false, "Vanilla Alduin defeat cue (1 track)" },
			Rule{ "special", "odahviing", kSkyrim, "MUSOdahviingRiding", false, Y, false, "Vanilla Odahviing-riding cue (1 track)" },
			Rule{ "special", "elder_scroll", kSkyrim, "MUSSpecialElderScrollSquence", false, Y, false, "Vanilla Elder Scroll sequence cue (EDID typo is Bethesda's)" },
			Rule{ "special", "eclipse", kDawnguard, "MUSEclipse", false, Y, false, "Dawnguard (official) eclipse cue (1 track)" },
			Rule{ "special", "boat_arrival", kDragonborn, "MUSDLC2BoatArrival", false, Y, false, "Dragonborn (official) Solstheim boat arrival cue" },

			// ---- tavern (diegetic: the bard is a scene actor; priority 55 beats everything) ----
			Rule{ "tavern", "", kSkyrim, "MUSTavern", false, N, true, "Diegetic: tavern bard music (priority 55)" },
			Rule{ "tavern", "", kSkyrim, "MUSTavernB", false, N, true, "Diegetic: tavern bard music variant (priority 55)" },
			Rule{ "tavern", "", kSkyrim, "MUSTavernSILENCE", false, N, false, "All three tracks are Silent: it only mutes background music while a bard plays" },

			// ---- story / ceremonial: kept vanilla ----
			Rule{ "story", "sovngarde", kSkyrim, "MUSExploreSovngarde", false, N, true, "Sovngarde ambience (scripted afterlife sequence)" },
			Rule{ "story", "sovngarde", kSkyrim, "MUSExploreSovngardeChant", false, N, true, "Diegetic: Sovngarde chant (priority 1, abrupt)" },
			Rule{ "story", "sovngarde", kSkyrim, "MUSExploreSovngardeChantExterior", false, N, true, "Diegetic: Sovngarde chant, exterior" },
			Rule{ "story", "hall_of_valor", kSkyrim, "MUSSovngardeHallofValor", false, N, true, "Hall of Valor (scripted)" },
			Rule{ "story", "hall_of_valor", kSkyrim, "MUSSpecialHallofValor", false, N, true, "Hall of Valor (scripted)" },
			Rule{ "story", "intro", kSkyrim, "MUSSpecialCart", false, N, true, "Intro cart ride (scripted, 1 track)" },
			Rule{ "story", "intro", kSkyrim, "MUSSpecialIntro", false, N, true, "Intro sequence (scripted, 1 track)" },
			Rule{ "story", "chargen", kSkyrim, "MUSDungeonChargen", false, N, true, "Character-creation / Helgen dungeon music (priority 2)" },
			Rule{ "story", "chargen", kSkyrim, "MUSCombatBossChargen", false, N, true, "Helgen dragon fight (character creation sequence)" },
			Rule{ "story", "word_of_power", kSkyrim, "MUSSpecialWordOfPower", false, N, true, "Word-of-power cue (PlaysOnce|DoesntQueue, 3 tracks); part of the shout learning moment" },

			// ---- menu / placeholder: all Silent ----
			Rule{ "menu", "", kSkyrim, "MUSSkillsMenu", false, N, false, "Single Silent track: nothing to replace" },
			Rule{ "menu", "", kSkyrim, "_NONE", false, N, false, "Placeholder with one Silent track: nothing to replace" },

			// ---- prefix rules: ONLY for official plugins, AFTER every exact row (first match wins) ----
			Rule{ "explore", "generic", kDragonborn, "DLC2MUSExplore", true, Y, false, "Dragonborn (official) exploration pool not listed above" },
			Rule{ "explore", "generic", kDawnguard, "MUSExploreDLC", true, Y, false, "Dawnguard (official) exploration pool not listed above" },
			Rule{ "boss", "", kDragonborn, "DLC2MUSCombatBoss", true, Y, false, "Dragonborn (official) boss pool not listed above" },
			Rule{ "boss", "", kDawnguard, "MUSCombatBoss", true, Y, false, "Dawnguard (official) boss pool not listed above" },
			Rule{ "boss", "", kUpdate, "MUSCombatBoss", true, Y, false, "Official Update.esm boss pool not listed above" },
		};

		// Skyrim.esm EDIDs that exist but must stay untouched and are not part of any environment.
		// Reported as Unmapped with a specific reason.
		struct KnownUnmapped
		{
			std::string_view plugin;
			std::string_view edid;
			std::string_view reason;
		};
		constexpr std::array kKnownUnmapped{
			KnownUnmapped{ kSkyrim, "MUSE3DemoDungeon", "Vanilla E3-demo leftover (priority 100, UseTrackOrder); not used in normal play" },
			KnownUnmapped{ kSkyrim, "MUSE3DemoExplore", "Vanilla E3-demo leftover (priority 100, UseTrackOrder); not used in normal play" },
		};

		bool IsAlnum(char c)
		{
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		}

		bool PrefixNoCase(std::string_view s, std::string_view prefix)
		{
			return s.size() >= prefix.size() && EqualsNoCase(s.substr(0, prefix.size()), prefix);
		}
	}

	bool EqualsNoCase(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i) {
			auto lo = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
			if (lo(a[i]) != lo(b[i])) {
				return false;
			}
		}
		return true;
	}

	std::span<const GroupInfo> ListGroups()
	{
		return kGroups;
	}

	const GroupInfo* FindGroup(std::string_view groupId)
	{
		for (const auto& g : kGroups) {
			if (g.id == groupId) {
				return &g;
			}
		}
		return nullptr;
	}

	std::span<const Rule> AllRules()
	{
		return kRules;
	}

	std::vector<Rule> GroupMembers(std::string_view groupId)
	{
		std::vector<Rule> out;
		for (const auto& r : kRules) {
			if (r.groupId == groupId) {
				out.push_back(r);
			}
		}
		return out;
	}

	Classification Classify(std::string_view editorId, std::string_view pluginName, const TypeHints& hints)
	{
		for (const auto& r : kRules) {
			if (!EqualsNoCase(r.plugin, pluginName)) {
				continue;
			}
			const bool hit = r.prefix ? PrefixNoCase(editorId, r.pattern) : EqualsNoCase(editorId, r.pattern);
			if (!hit) {
				continue;
			}
			Classification c{ r.groupId, r.subId, r.retargetable, r.diegetic, r.reason };
			if (c.retargetable == Retargetable::Yes && hints.allSilent) {
				c.retargetable = Retargetable::No;
				c.reason = "All tracks are Silent: nothing to replace";
			}
			return c;
		}
		for (const auto& k : kKnownUnmapped) {
			if (EqualsNoCase(k.plugin, pluginName) && EqualsNoCase(k.edid, editorId)) {
				return Classification{ kUnmappedGroup, "", Retargetable::No, false, k.reason };
			}
		}
		if (hints.allSilent) {
			return Classification{ kUnmappedGroup, "", Retargetable::No, false,
				"Unmapped mod music type with only Silent tracks; kept vanilla" };
		}
		return Classification{ kUnmappedGroup, "", Retargetable::No, false,
			"Unmapped: not a known vanilla / official music type, so it stays vanilla (the advanced list may offer it individually)" };
	}

	std::string EnvironmentId(std::string_view groupId, std::string_view subId)
	{
		std::string s(groupId);
		if (!subId.empty()) {
			s += '.';
			s += subId;
		}
		return s;
	}

	bool SplitEnvironmentId(std::string_view id, std::string& groupId, std::string& subId)
	{
		const auto dot = id.find('.');
		const auto g = id.substr(0, dot);
		const auto* info = FindGroup(g);
		if (!info) {
			return false;
		}
		std::string_view sub;
		if (dot != std::string_view::npos) {
			sub = id.substr(dot + 1);
			const bool ok = std::any_of(info->subs.begin(), info->subs.end(), [&](const SubInfo& s) { return s.id == sub; });
			if (!ok) {
				return false;
			}
		}
		groupId = std::string(g);
		subId = std::string(sub);
		return true;
	}

	bool IsKnownEnvironmentId(std::string_view id)
	{
		if (id.size() > kOtherModsPrefix.size() && id.substr(0, kOtherModsPrefix.size()) == kOtherModsPrefix) {
			return true;
		}
		std::string g, s;
		// Kept-vanilla groups (tavern, story, menu) are documented but can never be configured.
		return SplitEnvironmentId(id, g, s) && FindGroup(g)->kind == GroupKind::Playable;
	}

	std::string OtherModsEnvId(std::string_view editorId)
	{
		std::string out(kOtherModsPrefix);
		std::size_t used = 0;
		for (char c : editorId) {
			if (out.size() >= 64) {
				break;
			}
			if (IsAlnum(c) || c == '_' || c == '-') {
				++used;
				out += c;
			} else {
				out += '_';
			}
		}
		return used == 0 ? std::string{} : out;
	}
}
