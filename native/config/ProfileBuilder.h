#pragma once

// Settings -> Harmony profile JSON (the body of PUT /api/game/profiles/{name}).
// See Harmony docs/api/game-source.md. Pure C++ (nlohmann-json inside the .cpp only).
//
// Output shape:
//   { "types": { "<environment id>": <selector>, ..., "default": <selector or list> },
//     "normalize": true,
//     "format": { "rate": 44100, "bits": 16, "channels": 2 } }
// `sources` is deliberately absent: the plugin passes it per `next` call from allowStreaming.
// `format.container` is absent: the transport layer decides wav / flac.

#include "config/Settings.h"

#include <string>
#include <string_view>
#include <vector>

namespace mj::config
{
	// Harmony limits (game-source.md + the brief).
	inline constexpr std::size_t kMaxTypes             = 64;   // including "default"
	inline constexpr std::size_t kMaxSelectorsPerType  = 16;
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

	// Returns the JSON text, or an empty string when there is nothing to publish (every environment is
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

		// True when the played type should be retargeted at all.
		[[nodiscard]] bool Active() const { return setting && setting->mode != Mode::Vanilla; }
	};
	[[nodiscard]] EffectiveEnvironment ResolveEnvironment(const Settings& settings, std::string_view groupId, std::string_view subId);
}
