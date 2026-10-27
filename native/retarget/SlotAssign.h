#pragma once

// Pure slot planning (docs/INTEGRATION-1.0.md A.1). No engine: the retarget code passes plain facts about each
// music type and gets back which environment (and so which slot wrapper) it belongs to.
//
//  - MarthJukebox.esp carries K = 24 single-track wrappers MJ_SLOT_00..23 (local ids 0x800..0x817), whose ANAM
//    is Data\Music\Harmony\slot_NN.wav (tools/make_esp.py).
//  - Every ACTIVE effective environment (a Settings group or group.sub that resolves active) gets one slot.
//  - Diegetic, kept-vanilla, all-silent and unmapped types never get one.

#include "config/ProfileBuilder.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mj::retarget
{
	inline constexpr int           kSlotCount    = 24;
	inline constexpr std::uint32_t kFirstLocalId = 0x800;

	struct TypeFacts
	{
		std::string   editorId;
		std::string   plugin;  // origin file (TESForm::GetFile(0))
		bool          allSilent  = false;
		std::uint32_t trackCount = 0;
	};

	// The Settings environment id whose slot this type plays from, or "" (stays vanilla) with the reason.
	struct TypeTarget
	{
		std::string envId;
		std::string reason;  // why it stays vanilla (static text + ids only)
	};
	[[nodiscard]] TypeTarget EnvironmentForType(const TypeFacts& t, const config::Settings& s, const config::ProfileBuild& build);

	struct SlotPlan
	{
		std::map<std::string, int> slotOf;      // environment id -> slot 0..K-1
		std::vector<std::string>   unassigned;  // active environments beyond K (stay vanilla, logged)
	};
	// Deterministic: an environment that already had a slot in `previous` keeps it (so a settings change does
	// not reshuffle files under unaffected environments); the others take the lowest free slots in sorted id
	// order. Duplicates in `active` are ignored.
	[[nodiscard]] SlotPlan AssignSlots(std::vector<std::string> active, const std::map<std::string, int>& previous,
		int k = kSlotCount);

	[[nodiscard]] std::string SlotFileName(int slot);  // "slot_07.wav"
	[[nodiscard]] std::string SlotEditorId(int slot);  // "MJ_SLOT_07"
}
