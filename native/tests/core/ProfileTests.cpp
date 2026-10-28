#include "TestHarness.h"

#include "config/ProfileBuilder.h"
#include "config/Settings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace mj::config;
using json = nlohmann::json;

namespace
{
	// Git on Windows may check the golden files out with CRLF: ignore carriage returns.
	std::string Trimmed(std::string s)
	{
		s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
		return s;
	}

	// Compares against native/tests/core/golden/<name>. MJ_UPDATE_GOLDEN=1 rewrites the files (author use only).
	void CheckGolden(const std::string& name, const std::string& actual)
	{
		const auto path = mjtest::DataDir() / "golden" / name;
		if (std::getenv("MJ_UPDATE_GOLDEN")) {
			std::ofstream(path, std::ios::binary | std::ios::trunc) << actual << "\n";
			return;
		}
		std::ifstream in(path, std::ios::binary);
		std::ostringstream ss;
		ss << in.rdbuf();
		MJ_CHECK_MSG(in.good() || !ss.str().empty(), "missing golden file " + name);
		MJ_CHECK_MSG(Trimmed(ss.str()) == Trimmed(actual), "golden mismatch: " + name + "\n--- actual ---\n" + actual);
	}

	std::string ProblemsText(const std::vector<ProfileProblem>& v)
	{
		std::string t;
		for (const auto& p : v) {
			t += (p.severity == ProblemSeverity::Error ? "ERROR " : "WARN  ");
			t += (p.environment.empty() ? "(profile)" : p.environment);
			t += ": " + p.message + "\n";
		}
		return t;
	}

	EnvironmentSetting Env(Mode m, std::string selector, std::string service = "", std::string id = "", bool streaming = false)
	{
		EnvironmentSetting e;
		e.mode = m;
		e.selector = std::move(selector);
		e.service = std::move(service);
		e.id = std::move(id);
		e.allowStreaming = streaming;
		return e;
	}

	// The profile-level sources is ALWAYS "library" (streaming is a per-call opt-in).
	void CheckCommon(const json& j)
	{
		MJ_CHECK(j.at("normalize") == true);
		MJ_CHECK(j.at("format") == json::parse(R"({"rate":44100,"bits":16,"channels":2})"));
		MJ_CHECK(!j.at("format").contains("container"));
		MJ_CHECK(!j.at("types").contains("default"));  // no generated fallback type
		MJ_CHECK(j.at("types").size() <= kMaxTypes);
		MJ_CHECK(j.contains("sources") && j.at("sources") == "library");
	}
}

MJ_TEST(Profile_AllVanillaPublishesNothing)
{
	Settings s;
	std::vector<ProfileProblem> p;
	MJ_CHECK(BuildHarmonyProfile(s, &p).empty());
	s.environments["explore"];  // present but Vanilla
	MJ_CHECK(BuildHarmonyProfile(s, &p).empty());
	MJ_CHECK(p.empty());
}

MJ_TEST(Profile_GoldenBasicMix)
{
	Settings s;
	s.environments["explore"] = Env(Mode::Genre, "  Ambient  ");  // trimmed
	s.environments["combat"] = Env(Mode::Query, "epic battle music orchestral");
	s.environments["dungeon"] = Env(Mode::Artist, "Wardruna", "ytmusic", "UC1234567890abcdefghij", true);
	s.environments["boss"] = Env(Mode::Playlist, "Boss mix", "ytmusic", "PLabc_DEF-123", true);
	s.environments["town"] = Env(Mode::Vanilla, "ignored");
	s.environments["stinger"] = Env(Mode::Artist, "Local artist", "local", "9f2c1ab04d77e310");
	std::vector<ProfileProblem> p;
	const auto out = BuildHarmonyProfile(s, &p);
	MJ_CHECK_MSG(p.empty(), ProblemsText(p));
	const auto j = json::parse(out);
	CheckCommon(j);  // dungeon / boss allow streaming, the profile default is still library
	MJ_CHECK(j["types"].size() == 5);
	MJ_CHECK(!j["types"].contains("town"));
	MJ_CHECK(j["types"]["explore"] == json::parse(R"({"genre":"Ambient"})"));
	MJ_CHECK(j["types"]["dungeon"] == json::parse(R"({"artist":{"service":"ytmusic","id":"UC1234567890abcdefghij"}})"));
	CheckGolden("basic_mix.json", out);
	MJ_CHECK(BuildHarmonyProfile(s) == out);  // deterministic
}

MJ_TEST(Profile_GoldenSubEnvironmentsAndOtherMods)
{
	Settings s;
	s.global.personalKey = "SEKRET";
	s.global.serverUrl = "http://music.example.lan:8080";
	s.environments["explore"] = Env(Mode::Genre, "Ambient");
	s.environments["explore.tundra"] = Env(Mode::Genre, "Folk");
	s.environments["explore.snow"] = Env(Mode::Vanilla, "");  // explicit vanilla: no type, group not used for it
	s.environments["other.WTMUSExplore"] = Env(Mode::Query, "norse wilderness");
	std::vector<ProfileProblem> p;
	const auto out = BuildHarmonyProfile(s, &p);
	MJ_CHECK_MSG(p.empty(), ProblemsText(p));
	const auto j = json::parse(out);
	CheckCommon(j);
	MJ_CHECK(j["types"].contains("explore.tundra") && j["types"].contains("other.WTMUSExplore"));
	MJ_CHECK(!j["types"].contains("explore.snow"));
	MJ_CHECK(out.find("SEKRET") == std::string::npos && out.find("music.example.lan:8080") == std::string::npos);
	CheckGolden("sub_envs.json", out);
}

MJ_TEST(Profile_GoldenSingleEnvironment)
{
	Settings s;
	s.environments["combat"] = Env(Mode::Genre, "Metal");
	const auto out = BuildHarmonyProfile(s);
	const auto j = json::parse(out);
	CheckCommon(j);
	MJ_CHECK(j["types"].size() == 1);
	CheckGolden("single.json", out);
}

MJ_TEST(Profile_ValidationFailuresAreReportedPerEnvironment)
{
	Settings s;  // built directly: bypasses Sanitize on purpose
	s.environments["explore"] = Env(Mode::Genre, "Ambient");                                  // fine
	s.environments["combat"] = Env(Mode::Genre, std::string(201, 'x'));                       // too long
	s.environments["boss"] = Env(Mode::Query, "   ");                                         // empty after trim
	s.environments["dungeon"] = Env(Mode::Query, std::string("bad\x01text"));                 // control char
	s.environments["town"] = Env(Mode::Artist, "A", "qobuz", "123");                          // excluded service
	s.environments["discovery"] = Env(Mode::Artist, "A", "ytmusic", "");                      // missing id
	s.environments["stinger"] = Env(Mode::Playlist, "P", "ytmusic", "bad id with spaces");    // bad id chars
	s.environments["special"] = Env(Mode::Genre, std::string("caf\xC3\xA9 ok\xFF"));          // invalid UTF-8
	s.environments["bad name!"] = Env(Mode::Genre, "x");                                      // bad type name
	s.environments["default"] = Env(Mode::Genre, "x");                                        // reserved
	s.environments["tavern"] = Env(Mode::Artist, "A", "ytmusic", "UCabc");                    // warning: streaming off
	s.environments["story"] = Env(Mode::Playlist, "P", "local", "abc");                       // warning: no playlists locally
	std::vector<ProfileProblem> p;
	const auto out = BuildHarmonyProfile(s, &p);
	const auto j = json::parse(out);
	CheckCommon(j);

	auto has = [&](const std::string& env, ProblemSeverity sev) {
		return std::any_of(p.begin(), p.end(), [&](const ProfileProblem& x) { return x.environment == env && x.severity == sev; });
	};
	for (const char* e : { "combat", "boss", "dungeon", "town", "discovery", "stinger", "special", "bad name!", "default" }) {
		MJ_CHECK_MSG(has(e, ProblemSeverity::Error), e);
		MJ_CHECK_MSG(!j["types"].contains(e), e);
	}
	MJ_CHECK(has("tavern", ProblemSeverity::Warning) && j["types"].contains("tavern"));
	MJ_CHECK(has("story", ProblemSeverity::Warning) && j["types"].contains("story"));
	MJ_CHECK(!has("explore", ProblemSeverity::Error) && j["types"].contains("explore"));
	MJ_CHECK(p.size() == 11);
	CheckGolden("validation_failures.json", out);
	CheckGolden("validation_failures.problems.txt", ProblemsText(p));

	// Everything invalid: nothing is published.
	Settings none;
	none.environments["combat"] = Env(Mode::Genre, std::string(201, 'x'));
	std::vector<ProfileProblem> q;
	MJ_CHECK(BuildHarmonyProfile(none, &q).empty() && q.size() == 1);
}

MJ_TEST(Profile_TextLimitsAreInCodePointsNotBytes)
{
	Settings s;
	std::string two200;
	for (int i = 0; i < 200; ++i) two200 += "\xC3\xA9";  // 200 x e-acute = 400 bytes
	s.environments["explore"] = Env(Mode::Genre, two200);
	std::vector<ProfileProblem> p;
	const auto out = BuildHarmonyProfile(s, &p);
	MJ_CHECK_MSG(p.empty(), ProblemsText(p));
	MJ_CHECK(json::parse(out)["types"]["explore"]["genre"].get<std::string>() == two200);

	s.environments["explore"] = Env(Mode::Genre, two200 + "x");
	p.clear();
	MJ_CHECK(BuildHarmonyProfile(s, &p).empty() && p.size() == 1);

	// Exactly 200 ASCII characters pass.
	s.environments["explore"] = Env(Mode::Query, std::string(200, 'a'));
	p.clear();
	MJ_CHECK(!BuildHarmonyProfile(s, &p).empty() && p.empty());
}

MJ_TEST(Profile_LimitOnTypes)
{
	// 70 environments: Harmony allows 64 types => 64 kept.
	Settings s;
	for (int i = 0; i < 70; ++i) {
		char name[16];
		std::snprintf(name, sizeof name, "e%03d", i);
		s.environments[name] = Env(Mode::Genre, "genre" + std::to_string(i));
	}
	std::vector<ProfileProblem> p;
	const auto out = BuildHarmonyProfile(s, &p);
	const auto j = json::parse(out);
	MJ_CHECK(j["types"].size() == kMaxTypes);
	MJ_CHECK(std::count_if(p.begin(), p.end(), [](const ProfileProblem& x) { return x.severity == ProblemSeverity::Error; }) == 6);
}

MJ_TEST(Profile_SourcesIsAlwaysLibrary)
{
	Settings s;
	s.environments["explore"] = Env(Mode::Genre, "Ambient");
	MJ_CHECK(json::parse(BuildHarmonyProfile(s)).at("sources") == "library");
	s.environments["combat"] = Env(Mode::Genre, "Metal", "", "", true);  // one environment opts in to streaming
	MJ_CHECK(json::parse(BuildHarmonyProfile(s)).at("sources") == "library");  // ...the others must not inherit "all"
}

MJ_TEST(Profile_ResolveEnvironmentFollowsBuilderErrors)
{
	Settings s;
	s.environments["explore"] = Env(Mode::Genre, "Ambient");
	s.environments["explore.tundra"] = Env(Mode::Genre, std::string(201, 'x'));  // Error: left out
	s.environments["combat"] = Env(Mode::Artist, "A", "qobuz", "1");              // Error: left out
	s.environments["boss"] = Env(Mode::Artist, "A", "ytmusic", "UCabc");          // Warning only: published
	const auto build = BuildProfile(s);
	MJ_CHECK(build.publishedTypes.contains("explore") && build.publishedTypes.contains("boss"));
	MJ_CHECK(!build.publishedTypes.contains("explore.tundra") && !build.publishedTypes.contains("combat"));

	// The rejected sub-environment stays Vanilla: it does NOT fall back to the group, nor to a missing type.
	auto r = ResolveEnvironment(s, "explore", "tundra", build);
	MJ_CHECK(r.typeName == "explore.tundra" && r.excluded && !r.Active());
	r = ResolveEnvironment(s, "explore", "coast", build);  // inherits the published group
	MJ_CHECK(r.typeName == "explore" && r.Active());
	r = ResolveEnvironment(s, "combat", "", build);
	MJ_CHECK(!r.Active() && r.excluded);
	r = ResolveEnvironment(s, "boss", "", build);
	MJ_CHECK(r.Active());
	// Cross-check: every environment with an Error is inactive; every other non-Vanilla one is active.
	for (const auto& [id, e] : s.environments) {
		const bool errored = std::any_of(build.problems.begin(), build.problems.end(),
			[&](const ProfileProblem& x) { return x.environment == id && x.severity == ProblemSeverity::Error; });
		std::string g = id, sub;
		if (const auto dot = id.find('.'); dot != std::string::npos) {
			g = id.substr(0, dot);
			sub = id.substr(dot + 1);
		}
		MJ_CHECK_MSG(ResolveEnvironment(s, g, sub, build).Active() == !errored, id);
	}
}

MJ_TEST(Profile_PathAndResolve)
{
	Settings s;
	MJ_CHECK(ProfilePath(s) == "/api/game/profiles/skyrim-jukebox");
	s.global.harmonyProfileName = "mine";
	MJ_CHECK(ProfilePath(s) == "/api/game/profiles/mine");

	s.environments["explore"] = Env(Mode::Genre, "Ambient");
	s.environments["explore.tundra"] = Env(Mode::Genre, "Folk");
	s.environments["explore.snow"] = Env(Mode::Vanilla, "");
	s.environments["combat"] = Env(Mode::Vanilla, "");

	const auto b = BuildProfile(s);
	auto r = ResolveEnvironment(s, "explore", "tundra", b);
	MJ_CHECK(r.typeName == "explore.tundra" && r.Active() && r.setting->selector == "Folk");
	r = ResolveEnvironment(s, "explore", "coast", b);  // no sub entry: inherits the group
	MJ_CHECK(r.typeName == "explore" && r.Active() && r.setting->selector == "Ambient");
	r = ResolveEnvironment(s, "explore", "snow", b);   // explicit vanilla override
	MJ_CHECK(r.typeName == "explore.snow" && !r.Active());
	r = ResolveEnvironment(s, "explore", "", b);
	MJ_CHECK(r.typeName == "explore" && r.Active());
	r = ResolveEnvironment(s, "combat", "", b);
	MJ_CHECK(r.typeName == "combat" && !r.Active());
	r = ResolveEnvironment(s, "boss", "", b);
	MJ_CHECK(r.typeName.empty() && r.setting == nullptr && !r.Active());
}

// Field #4: `trim_silence` only when asked (a server new enough to know it); never in the default build.
MJ_TEST(Profile_TrimSilenceOnlyWhenAsked)
{
	Settings s = DefaultSettings();
	s.environments["combat"].mode     = Mode::Query;
	s.environments["combat"].selector = "epic battle";
	const auto plain = BuildProfile(s, false);
	const auto trim  = BuildProfile(s, false, true);
	MJ_CHECK(!plain.json.empty() && plain.json.find("trim_silence") == std::string::npos);
	MJ_CHECK(trim.json.find("\"trim_silence\": true") != std::string::npos);
	MJ_CHECK(trim.publishedTypes == plain.publishedTypes);
}
