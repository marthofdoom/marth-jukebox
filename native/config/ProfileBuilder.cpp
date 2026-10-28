#include "config/ProfileBuilder.h"

#include "categories/Environments.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace mj::config
{
	namespace
	{
		using json = nlohmann::json;

		bool ValidUtf8(std::string_view s)
		{
			std::size_t i = 0;
			while (i < s.size()) {
				const auto c = static_cast<unsigned char>(s[i]);
				std::size_t extra = 0;
				if (c < 0x80) extra = 0;
				else if (c >= 0xC2 && c <= 0xDF) extra = 1;
				else if (c >= 0xE0 && c <= 0xEF) extra = 2;
				else if (c >= 0xF0 && c <= 0xF4) extra = 3;
				else return false;
				for (std::size_t k = 1; k <= extra; ++k) {
					if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
						return false;
					}
				}
				i += extra + 1;
			}
			return true;
		}

		std::size_t CodePoints(std::string_view s)
		{
			std::size_t n = 0;
			for (char c : s) {
				if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
					++n;
				}
			}
			return n;
		}

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
			return s;
		}

		// Returns an error text, or empty when `text` (already trimmed) is a valid Harmony selector text.
		std::string CheckText(std::string_view text, const char* what)
		{
			if (text.empty()) {
				return std::string(what) + " is empty";
			}
			for (char c : text) {
				const auto u = static_cast<unsigned char>(c);
				if (u < 0x20 || u == 0x7f) {
					return std::string(what) + " contains a control character";
				}
			}
			if (!ValidUtf8(text)) {
				return std::string(what) + " is not valid UTF-8";
			}
			const auto n = CodePoints(text);
			if (n > kMaxSelectorTextChars) {
				return std::string(what) + " is " + std::to_string(n) + " characters; Harmony allows at most " +
					   std::to_string(kMaxSelectorTextChars);
			}
			return {};
		}

		// Harmony ids: letters, digits and _ . : @ + = ~ - [ ], at most 200.
		std::string CheckServiceId(std::string_view id)
		{
			if (id.empty()) {
				return "id is empty (pick an item from the search list)";
			}
			if (id.size() > kMaxSelectorTextChars) {
				return "id is longer than " + std::to_string(kMaxSelectorTextChars) + " characters";
			}
			for (char c : id) {
				const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				                std::string_view("_.:@+=~-[]").find(c) != std::string_view::npos;
				if (!ok) {
					return "id contains a character Harmony does not accept";
				}
			}
			return {};
		}

		bool ValidTypeName(std::string_view n)
		{
			if (n.empty() || n.size() > 64) {
				return false;
			}
			auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
			return alnum(n.front()) && std::all_of(n.begin(), n.end(), [&](char c) { return alnum(c) || c == '_' || c == '.' || c == '-'; });
		}

		struct Ctx
		{
			std::vector<ProfileProblem>* out;
			bool                         log = true;
			void Add(const std::string& env, ProblemSeverity sev, const std::string& msg) const
			{
				if (out) {
					out->push_back({ env, sev, msg });
				}
				if (log) {
					Log(sev == ProblemSeverity::Error ? spdlog::level::warn : spdlog::level::info,
						"profile: " + (env.empty() ? std::string("profile") : env) + ": " + msg);
				}
			}
		};

		// Builds the selector for one environment; false = Error reported.
		bool BuildSelector(const std::string& id, const EnvironmentSetting& e, json& out, const Ctx& ctx)
		{
			switch (e.mode) {
			case Mode::Genre:
			case Mode::Query: {
				const std::string_view text = Trim(e.selector);
				const char* what = e.mode == Mode::Genre ? "genre" : "query";
				if (auto err = CheckText(text, what); !err.empty()) {
					ctx.Add(id, ProblemSeverity::Error, err + "; environment left out of the profile");
					return false;
				}
				out = json::object({ { what, std::string(text) } });
				return true;
			}
			case Mode::Artist:
			case Mode::Playlist: {
				const char* what = e.mode == Mode::Artist ? "artist" : "playlist";
				if (e.service != "local" && e.service != "ytmusic") {
					ctx.Add(id, ProblemSeverity::Error,
						std::string(what) + " service must be 'local' or 'ytmusic' (Qobuz is not supported); environment left out of the profile");
					return false;
				}
				if (auto err = CheckServiceId(e.id); !err.empty()) {
					ctx.Add(id, ProblemSeverity::Error, std::string(what) + ": " + err + "; environment left out of the profile");
					return false;
				}
				if (e.service == "ytmusic" && !e.allowStreaming) {
					ctx.Add(id, ProblemSeverity::Warning,
						std::string(what) + " is on YouTube Music but streaming is not allowed: with sources=library it will yield no tracks");
				}
				if (e.mode == Mode::Playlist && e.service == "local") {
					ctx.Add(id, ProblemSeverity::Warning, "the Library has no playlists: this selector will yield no tracks");
				}
				out = json::object({ { what, json::object({ { "service", e.service }, { "id", e.id } }) } });
				return true;
			}
			case Mode::Vanilla:
				break;
			}
			return false;
		}
	}

	std::string ProfilePath(const Settings& settings)
	{
		return "/api/game/profiles/" + settings.global.harmonyProfileName;
	}

	EffectiveEnvironment ResolveEnvironment(const Settings& settings, std::string_view groupId, std::string_view subId,
		const ProfileBuild& build)
	{
		EffectiveEnvironment r;
		if (!subId.empty()) {
			const auto subName = categories::EnvironmentId(groupId, subId);
			if (const auto* s = settings.Find(subName)) {
				r.typeName = subName;
				r.setting = s;
			}
		}
		if (!r.setting) {
			if (const auto* g = settings.Find(groupId)) {
				r.typeName = std::string(groupId);
				r.setting = g;
			}
		}
		// An environment the builder left out (Error) is not in the Harmony profile: it stays Vanilla.
		if (r.setting && r.setting->mode != Mode::Vanilla && !build.publishedTypes.contains(r.typeName)) {
			r.excluded = true;
		}
		return r;
	}

	ProfileBuild BuildProfile(const Settings& settings, bool logProblems)
	{
		ProfileBuild out;
		const Ctx ctx{ &out.problems, logProblems };
		json types = json::object();

		if (!ValidTypeName(settings.global.harmonyProfileName)) {
			ctx.Add("", ProblemSeverity::Error, "profile name is invalid; nothing to publish");
			return out;
		}

		for (const auto& [id, e] : settings.environments) {
			if (e.mode == Mode::Vanilla) {
				continue;
			}
			if (!ValidTypeName(id) || id == "default") {
				ctx.Add(id, ProblemSeverity::Error,
					id == "default" ? "'default' is reserved; left out of the profile"
									: "environment id is not a valid Harmony type name; left out of the profile");
				continue;
			}
			if (types.size() >= kMaxTypes) {
				ctx.Add(id, ProblemSeverity::Error, "too many environments for one Harmony profile (limit " + std::to_string(kMaxTypes) +
													   "); left out of the profile");
				continue;
			}
			json sel;
			if (!BuildSelector(id, e, sel, ctx)) {
				continue;
			}
			types[id] = std::move(sel);
			out.publishedTypes.insert(id);
		}

		if (types.empty()) {
			return out;
		}

		// No generated "default" type: the plugin only ever asks for published types, and a type that failed
		// validation must stay Vanilla rather than silently play some other environment's selector.
		json root = json::object();
		root["types"] = std::move(types);
		root["normalize"] = true;
		root["format"] = json::object({ { "rate", 44100 }, { "bits", 16 }, { "channels", 2 } });
		// Profile-level default source set is ALWAYS Library only (streaming is a per-environment opt-in, see
		// NEXUS-COMPLIANCE). Every `next` call passes `sources` explicitly from AllowStreaming, which overrides it;
		// omitting it here would make Harmony default to "all" for every type, including ones that never opted in.
		root["sources"] = "library";
		// Text was validated above; 'replace' only guards the odd overlong/surrogate sequence, it never throws.
		out.json = root.dump(2, ' ', false, json::error_handler_t::replace);
		return out;
	}

	std::string BuildHarmonyProfile(const Settings& settings, std::vector<ProfileProblem>* problems)
	{
		auto b = BuildProfile(settings);
		if (problems) {
			problems->insert(problems->end(), b.problems.begin(), b.problems.end());
		}
		return std::move(b.json);
	}
}
