// Pure slot planning. See SlotAssign.h.
#include "retarget/SlotAssign.h"

#include "categories/Environments.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace mj::retarget
{
	TypeTarget EnvironmentForType(const TypeFacts& t, const config::Settings& s, const config::ProfileBuild& build)
	{
		TypeTarget  out;
		const auto c = categories::Classify(t.editorId, t.plugin, { t.allSilent, t.trackCount });
		if (c.groupId == categories::kUnmappedGroup) {
			out.reason = "unmapped";
			return out;
		}
		if (c.diegetic) {
			out.reason = "diegetic";
			return out;
		}
		if (c.retargetable != categories::Retargetable::Yes) {
			out.reason = "not retargetable";
			return out;
		}
		const auto eff = config::ResolveEnvironment(s, c.groupId, c.subId, build);
		if (!eff.Active()) {
			out.reason = eff.excluded ? "environment rejected by the profile builder" : "environment is Vanilla";
			return out;
		}
		out.envId = eff.typeName;
		return out;
	}

	SlotPlan AssignSlots(std::vector<std::string> active, const std::map<std::string, int>& previous, int k)
	{
		std::sort(active.begin(), active.end());
		active.erase(std::unique(active.begin(), active.end()), active.end());
		SlotPlan      plan;
		std::set<int> used;
		for (const auto& env : active) {
			const auto it = previous.find(env);
			if (it != previous.end() && it->second >= 0 && it->second < k && !used.contains(it->second)) {
				plan.slotOf[env] = it->second;
				used.insert(it->second);
			}
		}
		int next = 0;
		for (const auto& env : active) {
			if (plan.slotOf.contains(env)) {
				continue;
			}
			while (next < k && used.contains(next)) {
				++next;
			}
			if (next >= k) {
				plan.unassigned.push_back(env);
				continue;
			}
			plan.slotOf[env] = next;
			used.insert(next);
		}
		return plan;
	}

	bool SlotReady(std::string_view owner, std::string_view env, bool fresh, bool replay, bool failing, long long servedAtMs,
		long long nowMs, long long graceMs)
	{
		if (owner.empty() || owner != env || failing) {
			return false;
		}
		return fresh || replay || (servedAtMs >= 0 && nowMs - servedAtMs < graceMs);
	}

	std::string SlotFileName(int slot)
	{
		char b[32];
		std::snprintf(b, sizeof(b), "slot_%02d.wav", slot);
		return b;
	}

	std::string SlotEditorId(int slot)
	{
		char b[32];
		std::snprintf(b, sizeof(b), "MJ_SLOT_%02d", slot);
		return b;
	}
}
