#include "TestHarness.h"

#include "categories/Environments.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace mj::categories;

namespace
{
	struct Entry
	{
		std::string   edid;
		std::string   plugin;
		std::uint32_t tracks = 0;
		std::uint32_t silent = 0;
	};

	std::vector<Entry> LoadFixture()
	{
		std::ifstream in(mjtest::DataDir() / "fixtures" / "music_types.json");
		auto j = nlohmann::json::parse(in);
		std::vector<Entry> out;
		for (const auto& e : j) {
			out.push_back({ e.at("edid").get<std::string>(), e.at("plugin").get<std::string>(), e.at("tracks").get<std::uint32_t>(),
				e.at("silent").get<std::uint32_t>() });
		}
		return out;
	}

	TypeHints HintsFor(const Entry& e)
	{
		return TypeHints{ e.tracks > 0 && e.silent == e.tracks, e.tracks };
	}

	bool Official(const std::string& plugin)
	{
		static const std::set<std::string> ok{ "skyrim.esm", "dawnguard.esm", "dragonborn.esm", "update.esm", "ccbgssse067-daedinv.esm" };
		std::string l = plugin;
		std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return ok.contains(l);
	}
}

MJ_TEST(Env_FixtureHasAllRealTypes)
{
	const auto fx = LoadFixture();
	MJ_CHECK(fx.size() == 215);
	int vanilla = 0;
	for (const auto& e : fx) {
		vanilla += e.plugin == "Skyrim.esm";
	}
	MJ_CHECK(vanilla == 50);
}

MJ_TEST(Env_ClassifiesEveryRealEdidAsExpected)
{
	const auto fx = LoadFixture();
	// Independent, hand-derived expectation (tools: see docs/ENVIRONMENTS.md).
	std::ifstream in(mjtest::DataDir() / "fixtures" / "expected_classification.tsv");
	std::map<std::pair<std::string, std::string>, std::vector<std::string>> expected;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF checkout
		if (line.empty() || line[0] == '#') continue;
		std::vector<std::string> f;
		std::stringstream ss(line);
		std::string cell;
		while (std::getline(ss, cell, '\t')) f.push_back(cell);
		while (f.size() < 6) f.emplace_back();
		expected[{ f[0], f[1] }] = f;
	}
	MJ_CHECK(expected.size() == fx.size());

	for (const auto& e : fx) {
		const auto it = expected.find({ e.edid, e.plugin });
		MJ_CHECK_MSG(it != expected.end(), "no expectation for " + e.edid);
		if (it == expected.end()) continue;
		const auto c = Classify(e.edid, e.plugin, HintsFor(e));
		const auto& f = it->second;
		const std::string got = std::string(c.groupId) + "|" + std::string(c.subId) + "|" + (c.retargetable == Retargetable::Yes ? "1" : "0") +
		                        "|" + (c.diegetic ? "1" : "0");
		const std::string want = f[2] + "|" + f[3] + "|" + f[4] + "|" + f[5];
		MJ_CHECK_MSG(got == want, e.edid + " [" + e.plugin + "] got " + got + " want " + want);
		MJ_CHECK_MSG(!c.reason.empty(), e.edid + " has no reason");
	}
}

MJ_TEST(Env_Invariants)
{
	for (const auto& e : LoadFixture()) {
		const auto c = Classify(e.edid, e.plugin, HintsFor(e));
		const std::string tag = e.edid + " [" + e.plugin + "]";
		// All-silent types are never retargetable, wherever they come from.
		if (e.tracks > 0 && e.silent == e.tracks) {
			MJ_CHECK_MSG(c.retargetable == Retargetable::No, tag + " all-silent but retargetable");
		}
		// Diegetic types are never retargetable.
		if (c.diegetic) {
			MJ_CHECK_MSG(c.retargetable == Retargetable::No, tag + " diegetic but retargetable");
		}
		// Unmapped: vanilla, not diegetic, group = "unmapped".
		if (c.groupId == kUnmappedGroup) {
			MJ_CHECK_MSG(c.retargetable == Retargetable::No && !c.diegetic && c.subId.empty(), tag + " unmapped flags");
		} else {
			MJ_CHECK_MSG(FindGroup(c.groupId) != nullptr, tag + " group not registered");
			// Only official plugins and the explicit allow-list ever map.
			const bool allow = Official(e.plugin) || e.plugin == "Skyrim Extended Cut - Saints and Seducers.esp";
			MJ_CHECK_MSG(allow, tag + " mapped from a non-allow-listed plugin");
		}
		// Priority-55 tavern / bard types (and anything else in the tavern group) stay vanilla.
		if (c.groupId == "tavern") {
			MJ_CHECK_MSG(c.retargetable == Retargetable::No, tag + " tavern retargetable");
		}
	}
}

MJ_TEST(Env_ModTypesAreUnmappedByDefault)
{
	// A few real mod types from the log that must NOT map (unsure => Unmapped).
	for (const char* edid : { "WTMUSExplore", "000FCThrivingCity", "MUSMorrowindCity", "EC_SS_ExploreDementia", "zzzMUSColosseum",
			 "Eli_MUSVault", "AK69MUSHideout", "HD1CampsiteSounds", "Haiku_Silence" }) {
		const auto c = Classify(edid, "SomeMod.esp");
		MJ_CHECK_MSG(c.groupId == kUnmappedGroup && c.retargetable == Retargetable::No, edid);
	}
	// A vanilla EDID owned by another plugin is unknown music.
	MJ_CHECK(Classify("MUSExploreTundra", "SomeMod.esp").groupId == kUnmappedGroup);
	MJ_CHECK(Classify("MUSExploreTundra", "").groupId == kUnmappedGroup);
	MJ_CHECK(Classify("", "Skyrim.esm").groupId == kUnmappedGroup);
	// Prefix rules apply to the official plugin only.
	MJ_CHECK(Classify("DLC2MUSExploreFutureThing", "Dragonborn.esm").groupId == "explore");
	MJ_CHECK(Classify("DLC2MUSExploreFutureThing", "Skyrim.esm").groupId == kUnmappedGroup);
	MJ_CHECK(Classify("DLC2MUSExploreFutureThing", "ModX.esp").groupId == kUnmappedGroup);
	MJ_CHECK(Classify("MUSCombatBossFuture", "Dawnguard.esm").groupId == "boss");
	MJ_CHECK(Classify("MUSCombatBossFuture", "Skyrim.esm").groupId == kUnmappedGroup);
}

MJ_TEST(Env_CaseInsensitiveAndHints)
{
	MJ_CHECK(Classify("musexploretundra", "SKYRIM.ESM").groupId == "explore");
	MJ_CHECK(Classify("musexploretundra", "SKYRIM.ESM").subId == "tundra");
	// Hints: a retargetable table type whose live tracks are all silent is refused.
	const auto c = Classify("MUSExploreTundra", "Skyrim.esm", TypeHints{ true, 3 });
	MJ_CHECK(c.groupId == "explore" && c.retargetable == Retargetable::No);
	// A mod type that is all-silent is unmapped and explains why.
	const auto m = Classify("Foo", "Mod.esp", TypeHints{ true, 2 });
	MJ_CHECK(m.groupId == kUnmappedGroup && m.retargetable == Retargetable::No && !m.reason.empty());
	// Tavern specifics.
	MJ_CHECK(Classify("MUSTavern", "Skyrim.esm").diegetic);
	MJ_CHECK(Classify("MUSTavernB", "Skyrim.esm").diegetic);
	MJ_CHECK(Classify("MUSTavernSILENCE", "Skyrim.esm").retargetable == Retargetable::No);
	MJ_CHECK(Classify("MUSSpecialWordOfPower", "Skyrim.esm").diegetic);
	MJ_CHECK(Classify("MUSSovngardeHallofValor", "Skyrim.esm").diegetic);
	MJ_CHECK(Classify("MUSSpecialCart", "Skyrim.esm").diegetic);
	MJ_CHECK(Classify("MUSDungeonChargen", "Skyrim.esm").diegetic);
}

MJ_TEST(Env_TableIsConsistent)
{
	const auto rules = AllRules();
	std::set<std::string> exactKeys;
	bool seenPrefix = false;
	for (const auto& r : rules) {
		const std::string tag = std::string(r.pattern);
		const auto* g = FindGroup(r.groupId);
		MJ_CHECK_MSG(g != nullptr, tag + " group unknown");
		if (!g) continue;
		MJ_CHECK_MSG(!r.reason.empty() && !r.plugin.empty() && !r.pattern.empty(), tag + " incomplete row");
		if (!r.subId.empty()) {
			const bool reg = std::any_of(g->subs.begin(), g->subs.end(), [&](const SubInfo& s) { return s.id == r.subId; });
			MJ_CHECK_MSG(reg, tag + " sub not registered");
		}
		if (r.prefix) {
			seenPrefix = true;
		} else {
			MJ_CHECK_MSG(!seenPrefix, tag + " exact row after a prefix row (prefix rows must come last)");
			const auto key = std::string(r.plugin) + "|" + std::string(r.pattern);
			MJ_CHECK_MSG(exactKeys.insert(key).second, tag + " duplicate exact row");
		}
		if (g->kind == GroupKind::KeptVanilla) {
			MJ_CHECK_MSG(r.retargetable == Retargetable::No, tag + " kept-vanilla group with a retargetable row");
		}
		if (r.diegetic) {
			MJ_CHECK_MSG(r.retargetable == Retargetable::No, tag + " diegetic row retargetable");
		}
	}
	for (const auto& g : ListGroups()) {
		const auto members = GroupMembers(g.id);
		MJ_CHECK_MSG(!members.empty(), std::string(g.id) + " has no members");
		if (g.kind == GroupKind::Playable) {
			MJ_CHECK_MSG(std::any_of(members.begin(), members.end(), [](const Rule& r) { return r.retargetable == Retargetable::Yes; }),
				std::string(g.id) + " playable without retargetable member");
		}
		for (const auto& s : g.subs) {
			const bool used = std::any_of(members.begin(), members.end(), [&](const Rule& r) { return r.subId == s.id; });
			MJ_CHECK_MSG(used, std::string(g.id) + "." + std::string(s.id) + " registered but has no member");
		}
	}
	MJ_CHECK(ListGroups().size() >= 10 && ListGroups().size() <= 14);
	MJ_CHECK(GroupMembers("nope").empty());
}

MJ_TEST(Env_IdHelpers)
{
	MJ_CHECK(EnvironmentId("explore", "") == "explore");
	MJ_CHECK(EnvironmentId("explore", "tundra") == "explore.tundra");
	std::string g, s;
	MJ_CHECK(SplitEnvironmentId("explore.tundra", g, s) && g == "explore" && s == "tundra");
	MJ_CHECK(SplitEnvironmentId("combat", g, s) && g == "combat" && s.empty());
	MJ_CHECK(!SplitEnvironmentId("explore.nope", g, s));
	MJ_CHECK(!SplitEnvironmentId("nope", g, s));
	MJ_CHECK(IsKnownEnvironmentId("boss") && IsKnownEnvironmentId("dungeon.cave") && IsKnownEnvironmentId("other.Foo"));
	MJ_CHECK(!IsKnownEnvironmentId("tavern") && !IsKnownEnvironmentId("story.chargen") && !IsKnownEnvironmentId("menu"));
	MJ_CHECK(!IsKnownEnvironmentId("default") && !IsKnownEnvironmentId("") && !IsKnownEnvironmentId("other."));
	MJ_CHECK(OtherModsEnvId("WTMUSExplore") == "other.WTMUSExplore");
	MJ_CHECK(OtherModsEnvId("a b:c") == "other.a_b_c");
	MJ_CHECK(OtherModsEnvId("   ").empty());
	MJ_CHECK(OtherModsEnvId(std::string(200, 'x')).size() == 64);
}
