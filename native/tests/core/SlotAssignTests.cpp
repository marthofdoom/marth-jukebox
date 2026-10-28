// Slot planning tests (retarget/SlotAssign.h): type -> environment, deterministic slot assignment.
#include "TestHarness.h"

#include "retarget/SlotAssign.h"

using namespace mj::retarget;
using namespace mj::config;

namespace
{
	Settings WithEnv(Settings s, const std::string& id, Mode m, const std::string& sel)
	{
		auto& e    = s.environments[id];
		e.mode     = m;
		e.selector = sel;
		return s;
	}
}

MJ_TEST(Slots_TypeToEnvironment)
{
	Settings s = DefaultSettings();
	s          = WithEnv(s, "explore", Mode::Genre, "Ambient");
	s          = WithEnv(s, "explore.tundra", Mode::Genre, "Folk");
	s          = WithEnv(s, "explore.snow", Mode::Vanilla, "");
	s          = WithEnv(s, "combat", Mode::Query, "epic battle");
	const auto build = BuildProfile(s);

	MJ_CHECK(EnvironmentForType({ "MUSExploreTundra", "Skyrim.esm", false, 107 }, s, build).envId == "explore.tundra");
	MJ_CHECK(EnvironmentForType({ "MUSExploreMarsh", "Skyrim.esm", false, 108 }, s, build).envId == "explore");
	// A sub that exists as Vanilla overrides its group: stays vanilla.
	auto snow = EnvironmentForType({ "MUSExploreSnow", "Skyrim.esm", false, 90 }, s, build);
	MJ_CHECK(snow.envId.empty() && !snow.reason.empty());
	MJ_CHECK(EnvironmentForType({ "MUSCombat", "Skyrim.esm", false, 30 }, s, build).envId == "combat");
	// combat.civil_war inherits the group.
	MJ_CHECK(EnvironmentForType({ "MUSCombatCivilWar", "Skyrim.esm", false, 3 }, s, build).envId == "combat");
	// Diegetic, unmapped, wrong origin plugin, all-silent: never.
	MJ_CHECK(EnvironmentForType({ "MUSTavern", "Skyrim.esm", false, 6 }, s, build).envId.empty());
	MJ_CHECK(EnvironmentForType({ "WTMUSExplore", "Wyrmstooth.esp", false, 10 }, s, build).envId.empty());
	MJ_CHECK(EnvironmentForType({ "MUSExploreTundra", "SomeMod.esp", false, 10 }, s, build).envId.empty());
	MJ_CHECK(EnvironmentForType({ "MUSExploreTundra", "Skyrim.esm", true, 3 }, s, build).envId.empty());
	// Not configured at all: vanilla.
	MJ_CHECK(EnvironmentForType({ "MUSDungeon", "Skyrim.esm", false, 8 }, s, build).envId.empty());
	// An environment the builder rejects (Error) stays vanilla, it does not fall back to the group.
	Settings bad = WithEnv(s, "explore.marsh", Mode::Genre, std::string(300, 'x'));
	MJ_CHECK(EnvironmentForType({ "MUSExploreMarsh", "Skyrim.esm", false, 108 }, bad, BuildProfile(bad)).envId.empty());
}

MJ_TEST(Slots_AssignmentIsDeterministicAndStable)
{
	auto p = AssignSlots({ "explore", "combat", "boss", "combat" }, {});
	MJ_CHECK(p.slotOf.size() == 3 && p.unassigned.empty());
	MJ_CHECK(p.slotOf["boss"] == 0 && p.slotOf["combat"] == 1 && p.slotOf["explore"] == 2);
	MJ_CHECK(AssignSlots({ "combat", "explore", "boss" }, {}).slotOf == p.slotOf);

	// Adding "dungeon" (sorts between) keeps the others where they were.
	auto q = AssignSlots({ "explore", "combat", "boss", "dungeon" }, p.slotOf);
	MJ_CHECK(q.slotOf["boss"] == 0 && q.slotOf["combat"] == 1 && q.slotOf["explore"] == 2 && q.slotOf["dungeon"] == 3);
	// Removing "combat" frees slot 1 for the next newcomer.
	auto r = AssignSlots({ "explore", "boss", "dungeon", "town" }, q.slotOf);
	MJ_CHECK(r.slotOf["town"] == 1 && r.slotOf["dungeon"] == 3);
	// A bad previous value (out of range, or a clash) is ignored.
	auto t = AssignSlots({ "a", "b" }, { { "a", 99 }, { "b", 0 } });
	MJ_CHECK(t.slotOf["b"] == 0 && t.slotOf["a"] == 1);
}

MJ_TEST(Slots_MoreEnvironmentsThanSlotsStayVanilla)
{
	std::vector<std::string> envs;
	for (int i = 0; i < 30; ++i) {
		envs.push_back("env" + std::to_string(100 + i));
	}
	const auto p = AssignSlots(envs, {});
	MJ_CHECK(p.slotOf.size() == static_cast<std::size_t>(kSlotCount));
	MJ_CHECK(p.unassigned.size() == 6 && p.unassigned.front() == "env124");
	MJ_CHECK(SlotFileName(7) == "slot_07.wav" && SlotEditorId(23) == "MJ_SLOT_23");
}

// Field #4: a type never falls back to vanilla while its slot can play something of its own environment.
MJ_TEST(Slots_ReadyDecision)
{
	MJ_CHECK(SlotReady("combat", "combat", true, false, false, -1, 0));       // unplayed pool track
	MJ_CHECK(SlotReady("combat", "combat", false, true, false, -1, 0));       // pool dry: an earlier track from the cache
	MJ_CHECK(SlotReady("combat", "combat", false, false, false, 1000, 5000)); // just played, refill due
	MJ_CHECK(!SlotReady("combat", "combat", false, false, false, 1000, 20000)); // played long ago, nothing placed since
	MJ_CHECK(!SlotReady("combat", "combat", true, true, true, -1, 0));       // failing (flood guard): never
	MJ_CHECK(!SlotReady("explore", "combat", true, false, false, -1, 0));     // the file belongs to another environment
	MJ_CHECK(!SlotReady("", "combat", true, false, false, -1, 0));            // unknown content (placeholder)
}

// Review R5-1: one cached track, pool dry, several plays: the slot stays ready the whole time (no restore/re-bind flap,
// so no engine-write storm), and it is only "failing" when the slot holds nothing of the environment.
MJ_TEST(Slots_DryPoolNoFlap)
{
	MJ_CHECK(ChooseDryPoolAction(true, true) == DryPoolAction::PlaceReplay);
	MJ_CHECK(ChooseDryPoolAction(true, false) == DryPoolAction::PlaceReplay);
	MJ_CHECK(ChooseDryPoolAction(false, true) == DryPoolAction::RepeatCurrent);
	MJ_CHECK(ChooseDryPoolAction(false, false) == DryPoolAction::Fail);

	// The slot thread's state for one slot, driven tick by tick like SlotFiller + the game thread's SlotReady.
	struct State
	{
		std::string owner = "combat";
		bool        fresh = true, replay = false, failing = false;
		long long   servedAt = -1;
	} st;
	int       bindingChanges = 0;
	bool      bound          = SlotReady(st.owner, "combat", st.fresh, st.replay, st.failing, st.servedAt, 0);
	long long now            = 0;
	for (int play = 0; play < 6; ++play) {
		now += 60'000;  // a fight every minute
		st.fresh    = false;  // play event: the content is served
		st.servedAt = now;
		for (int tick = 1; tick <= 400; ++tick) {  // the next 100 s, 250 ms ticks
			const long long t = now + tick * 250;
			if (tick == 6) {  // 1.5 s later: the refill; the pool is dry and nothing else is cached
				switch (ChooseDryPoolAction(false, st.owner == "combat")) {
				case DryPoolAction::RepeatCurrent:
					st.replay  = true;
					st.failing = false;
					break;
				case DryPoolAction::Fail:
					st.failing = true;
					break;
				case DryPoolAction::PlaceReplay:
					break;
				}
			}
			const bool ready = SlotReady(st.owner, "combat", st.fresh, st.replay, st.failing, st.servedAt, t);
			if (ready != bound) {
				++bindingChanges;
				bound = ready;
			}
		}
	}
	MJ_CHECK_MSG(bindingChanges == 0, std::to_string(bindingChanges) + " restore/re-bind changes");
	MJ_CHECK(bound);
}
