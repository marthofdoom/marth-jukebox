#pragma once

// Settings -> Harmony profile JSON (the body of PUT /api/game/profiles/{name}).
// See Harmony docs/api/game-source.md. Pure C++ (nlohmann-json inside the .cpp only).
//
// Output shape:
//   { "types": { "<environment id>": <selector>, ... },     (no generated "default" type)
//     "normalize": true,
//     "format": { "rate": 44100, "bits": 16, "channels": 2 },
//     "sources": "library" }                                  (always; streaming is a per-environment opt-in)
// The plugin passes `sources` per `next` call from allowStreaming (it overrides the profile's).
// `format.container` is absent: the transport layer decides wav / flac.

#include "config/Settings.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mj::config
{
	// Harmony limits (game-source.md + the brief).
	inline constexpr std::size_t kMaxTypes             = 64;
	inline constexpr std::size_t kMaxSelectorTextChars = 200;  // code points

	enum class ProblemSeverity : std::uint8_t
	{
		Warning,  // the environment is still published
		Error     // the environment is left out of the profile
	};

	struct ProfileProblem
	{
		std::string     environment;  // environment id, "" = the profile as a whole
		ProblemSeverity severity = ProblemSeverity::Error;
		std::string     message;      // never contains the personal key or server URL
	};

	// Full result: the JSON, the set of Harmony type names that were published, and every problem.
	struct ProfileBuild
	{
		std::string                 json;            // empty when nothing is publishable
		std::set<std::string>       publishedTypes;  // environment ids present in `json`
		std::vector<ProfileProblem> problems;
	};
	[[nodiscard]] ProfileBuild BuildProfile(const Settings& settings);

	// Convenience wrapper: returns the JSON text, or an empty string when there is nothing to publish (every environment is
	// Vanilla or invalid; Harmony requires at least one type). Problems are appended to `problems` and
	// logged. Deterministic: same settings -> same bytes.
	[[nodiscard]] std::string BuildHarmonyProfile(const Settings& settings, std::vector<ProfileProblem>* problems = nullptr);

	// "/api/game/profiles/<name>" for the PUT.
	[[nodiscard]] std::string ProfilePath(const Settings& settings);

	// Which environment settings govern a played music type, and under which Harmony type name?
	// Resolution: the sub-environment entry if it exists (even when Vanilla), else the group's entry.
	struct EffectiveEnvironment
	{
		std::string                typeName;  // "explore.tundra" or "explore"; empty when nothing applies
		const EnvironmentSetting*  setting = nullptr;  // null when no entry exists (=> Vanilla)
		bool                       excluded = false;   // the builder left it out of the profile (Error): stays Vanilla

		// True when the played type should be retargeted at all.
		[[nodiscard]] bool Active() const { return setting && setting->mode != Mode::Vanilla && !excluded; }
	};
	// `build` is the ProfileBuild of the profile that was actually published (required): an environment rejected
	// with an Error is reported inactive (it must not fall back to a missing Harmony type).
	[[nodiscard]] EffectiveEnvironment ResolveEnvironment(const Settings& settings, std::string_view groupId, std::string_view subId,
		const ProfileBuild& build);
}
