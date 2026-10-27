#pragma once

// Music-type -> player-facing ENVIRONMENT map (DESIGN 4 / 8.4).
//
// Pure C++: no engine, no network, no CommonLib. The caller (retarget/menu code) passes the MUSC editor
// id and the name of the plugin that owns the record; this module answers which environment group it
// belongs to, whether it may be retargeted and whether it is diegetic (kept vanilla by default).
//
// Doctrine:
//  - Only types listed in the table (Environments.cpp) are ever mapped. Anything else is UNMAPPED and
//    stays vanilla; the advanced menu may list unmapped types individually as "Other mods".
//  - A type whose tracks are all Silent is never retargetable.
//  - Diegetic types (tavern/bard, Sovngarde chants, intro cart, word-of-power...) are never offered.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mj::categories
{
	enum class Retargetable : std::uint8_t
	{
		No,
		Yes
	};

	// Group id returned for music types that are not in the table.
	inline constexpr std::string_view kUnmappedGroup = "unmapped";

	// Prefix of the environment id of one individually listed unmapped type ("other.<EDID>").
	inline constexpr std::string_view kOtherModsPrefix = "other.";

	// Facts the caller can read from the live record. Optional: the table alone is enough for every
	// vanilla type; the hints add the "all tracks Silent" rule for anything else (and double-check the table).
	struct TypeHints
	{
		bool          allSilent  = false;  // every track of the MUSC is a BGSMusicSilenceTrack
		std::uint32_t trackCount = 0;
	};

	struct Classification
	{
		std::string_view groupId;  // a ListGroups() id, or kUnmappedGroup. Static storage.
		std::string_view subId;    // "" = the group's base type
		Retargetable     retargetable = Retargetable::No;
		bool             diegetic     = false;
		std::string_view reason;  // human-readable, static storage
	};

	struct SubInfo
	{
		std::string_view id;
		std::string_view displayName;
	};

	enum class GroupKind : std::uint8_t
	{
		Playable,    // offered in the menu
		KeptVanilla  // listed for information only: diegetic, scripted or silent
	};

	struct GroupInfo
	{
		std::string_view      id;           // stable; also the Harmony type name
		std::string_view      displayName;  // menu label
		std::string_view      summary;      // one line for the menu / docs
		GroupKind             kind = GroupKind::Playable;
		std::span<const SubInfo> subs;      // sub-environments (children), may be empty
	};

	// One row of the table: which record(s) of which plugin land where.
	struct Rule
	{
		std::string_view groupId;
		std::string_view subId;       // "" = group base
		std::string_view plugin;      // exact plugin file name, case-insensitive
		std::string_view pattern;     // EDID, case-insensitive
		bool             prefix = false;  // true: pattern is a prefix of the EDID
		Retargetable     retargetable = Retargetable::Yes;
		bool             diegetic     = false;
		std::string_view reason;
	};

	// Classify one music type. Thread-safe (reads constant data).
	// `pluginName` is the ORIGIN file of the record (TESForm::GetFile(0)), not the plugin whose override wins.
	// "other.<EDID>" environment ids (advanced list) apply only to records this returns as plain Unmapped.
	[[nodiscard]] Classification Classify(std::string_view editorId, std::string_view pluginName,
		const TypeHints& hints = {});

	// All environment groups in menu order (playable first, kept-vanilla last).
	[[nodiscard]] std::span<const GroupInfo> ListGroups();
	[[nodiscard]] const GroupInfo*           FindGroup(std::string_view groupId);

	// Every rule row that maps into the group (copy; the string_views point at static data).
	[[nodiscard]] std::vector<Rule> GroupMembers(std::string_view groupId);

	// The whole table, for documentation generation and tests.
	[[nodiscard]] std::span<const Rule> AllRules();

	// Settings / profile type name for a (group, sub): "explore" or "explore.tundra".
	[[nodiscard]] std::string EnvironmentId(std::string_view groupId, std::string_view subId);

	// True for a PLAYABLE group id, a "group.sub" of its registered sub-environment, or "other.<name>".
	// Kept-vanilla groups (tavern, story, menu) are never configurable.
	[[nodiscard]] bool IsKnownEnvironmentId(std::string_view id);

	// Splits "explore.tundra" into group + sub. False when the group is unknown (or sub unregistered).
	[[nodiscard]] bool SplitEnvironmentId(std::string_view id, std::string& groupId, std::string& subId);

	// "other.<EDID>" for the advanced list of unmapped types. Empty when the EDID has no usable characters.
	// Characters outside [A-Za-z0-9_-] become '_'; the result fits Harmony's type-name rules (<= 64).
	[[nodiscard]] std::string OtherModsEnvId(std::string_view editorId);

	// True when the table (any row, exact or prefix) or the known-unmapped list names this EDID, regardless of
	// plugin. Settings refuses "other.<EDID>" for such EDIDs.
	[[nodiscard]] bool IsTableEdid(std::string_view editorId);

	// ASCII case-insensitive equality (EDIDs and plugin names compare this way).
	[[nodiscard]] bool EqualsNoCase(std::string_view a, std::string_view b);
}
