// Pure pool / cache model. See PoolModel.h.
#include "pool/PoolModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <system_error>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace mj::pool
{
	namespace fs = std::filesystem;
	using json   = nlohmann::json;

	namespace
	{
		std::string Hex16(std::uint64_t v)
		{
			char buf[17];
			std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
			return std::string(buf, 16);
		}

		std::string Str(const json& j, const char* key)
		{
			const auto it = j.find(key);
			return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
		}

		// Server strings are stored as given but bounded, so a hostile index or reply cannot bloat memory.
		std::string Bounded(std::string s, std::size_t max = 512)
		{
			if (s.size() > max) {
				s.resize(max);
			}
			return s;
		}
	}

	std::string PoolEntry::Key() const
	{
		return TrackKey(service, id);
	}

	std::string PoolEntry::Display() const
	{
		if (!artist.empty() && !title.empty()) {
			return artist + " - " + title;
		}
		if (!title.empty()) {
			return title;
		}
		return Key();
	}

	std::string TrackKey(std::string_view service, std::string_view id)
	{
		std::string k;
		k.reserve(service.size() + 1 + id.size());
		k.append(service).push_back(':');
		k.append(id);
		return k;
	}

	std::uint64_t Fnv1a64(std::string_view data, std::uint64_t seed)
	{
		std::uint64_t h = seed;
		for (const unsigned char c : data) {
			h ^= c;
			h *= 1099511628211ull;
		}
		return h;
	}

	std::string CacheFileName(std::string_view service, std::string_view id, std::string_view container)
	{
		// Length-prefixed so ("a:b", "c") and ("a", "b:c") never hash the same input.
		std::string in = std::to_string(service.size());
		in.push_back('|');
		in.append(service).push_back('|');
		in.append(id);
		return Hex16(Fnv1a64(in)) + (container == "flac" ? ".flac" : ".wav");
	}

	bool IsCacheFileName(std::string_view name)
	{
		std::string_view ext;
		if (name.size() == 16 + 4 && name.substr(16) == ".wav") {
			ext = ".wav";
		} else if (name.size() == 16 + 5 && name.substr(16) == ".flac") {
			ext = ".flac";
		} else {
			return false;
		}
		for (std::size_t i = 0; i < 16; ++i) {
			const char c = name[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
				return false;
			}
		}
		return true;
	}

	std::string Fingerprint(std::string_view typeName, const config::EnvironmentSetting& e, std::string_view sources)
	{
		// Every field length-prefixed (no separator ambiguity), then two independent 64-bit hashes.
		std::string in = "v1";
		auto add = [&in](std::string_view f) {
			in += '|';
			in += std::to_string(f.size());
			in += ':';
			in.append(f);
		};
		add(typeName);
		add(config::ToString(e.mode));
		add(e.selector);
		add(e.service);
		add(e.id);
		add(sources);
		return Hex16(Fnv1a64(in)) + Hex16(Fnv1a64(in, 0x84222325cbf29ce4ull));
	}

	std::string SerializeIndex(const PoolIndex& index)
	{
		json j;
		j["version"] = kIndexVersion;
		json pools   = json::object();
		for (const auto& [env, p] : index.pools) {
			json entries = json::array();
			for (const auto& e : p.entries) {
				entries.push_back({ { "service", e.service }, { "id", e.id }, { "title", e.title }, { "artist", e.artist },
					{ "duration_s", e.durationS }, { "bytes", e.bytes }, { "file", e.file }, { "sources", e.sources } });
			}
			pools[env] = { { "fingerprint", p.fingerprint }, { "entries", std::move(entries) } };
		}
		j["pools"]  = std::move(pools);
		json recent = json::object();
		for (const auto& [env, keys] : index.recent) {
			recent[env] = json(std::vector<std::string>(keys.begin(), keys.end()));
		}
		j["recent"] = std::move(recent);
		json replay = json::object();
		for (const auto& [env, keys] : index.replayable) {
			replay[env] = json(std::vector<std::string>(keys.begin(), keys.end()));
		}
		j["replayable"] = std::move(replay);
		// Invalid UTF-8 from a server string must not throw: replace it.
		return j.dump(1, '\t', false, json::error_handler_t::replace);
	}

	bool ParseIndex(std::string_view text, PoolIndex& out, std::vector<std::string>& diag)
	{
		out = PoolIndex{};
		const json j = json::parse(text, nullptr, false);
		if (j.is_discarded() || !j.is_object()) {
			diag.push_back("pool index is not valid JSON; starting with empty pools");
			return false;
		}
		const auto ver = j.find("version");
		if (ver == j.end() || !ver->is_number_integer() || ver->get<long long>() != kIndexVersion) {
			diag.push_back("pool index has an unknown version; starting with empty pools");
			return false;
		}
		if (const auto pools = j.find("pools"); pools != j.end() && pools->is_object()) {
			for (const auto& [env, pj] : pools->items()) {
				if (!pj.is_object() || env.empty() || env.size() > 64) {
					diag.push_back("pool index: a malformed pool was dropped");
					continue;
				}
				EnvPool p;
				p.fingerprint = Bounded(Str(pj, "fingerprint"), 64);
				std::unordered_set<std::string> seen;
				if (const auto ents = pj.find("entries"); ents != pj.end() && ents->is_array()) {
					for (const auto& ej : *ents) {
						if (!ej.is_object()) {
							diag.push_back("pool index: a malformed entry was dropped");
							continue;
						}
						PoolEntry e;
						e.service = Bounded(Str(ej, "service"), 64);
						e.id      = Bounded(Str(ej, "id"), 256);
						e.title   = Bounded(Str(ej, "title"));
						e.artist  = Bounded(Str(ej, "artist"));
						e.file    = Str(ej, "file");
						e.sources = Bounded(Str(ej, "sources"), 64);
						if (const auto d = ej.find("duration_s"); d != ej.end() && d->is_number()) {
							e.durationS = d->get<double>();
						}
						if (const auto b = ej.find("bytes"); b != ej.end() && b->is_number_unsigned()) {
							e.bytes = b->get<std::uint64_t>();
						}
						if (e.service.empty() || e.id.empty() || !IsCacheFileName(e.file) || e.bytes == 0 ||
							!(e.durationS >= 0.0 && e.durationS < 1e7)) {
							diag.push_back("pool index: an entry with a bad file name, size or duration was dropped");
							continue;
						}
						if (!seen.insert(e.Key()).second || p.entries.size() >= kMaxPoolEntries) {
							diag.push_back("pool index: a duplicate or surplus entry was dropped");
							continue;
						}
						p.entries.push_back(std::move(e));
					}
				}
				out.pools.emplace(env, std::move(p));
			}
		}
		// "recent" and (since R7-2, optional: an older index has none, so nothing is replayable until played) "replayable".
		auto lists = [&](const char* a_key, std::map<std::string, std::deque<std::string>, std::less<>>& a_out) {
			const auto rec = j.find(a_key);
			if (rec == j.end() || !rec->is_object()) {
				return;
			}
			for (const auto& [env, rj] : rec->items()) {
				if (!rj.is_array() || env.empty() || env.size() > 64) {
					continue;
				}
				auto& dq = a_out[env];
				for (const auto& k : rj) {
					if (k.is_string()) {
						dq.push_back(Bounded(k.get<std::string>(), 330));
					}
				}
				while (dq.size() > kRecentPerEnv) {
					dq.pop_front();
				}
			}
		};
		lists("recent", out.recent);
		lists("replayable", out.replayable);
		return true;
	}

	std::size_t Revalidate(PoolIndex& index, const fs::path& cacheDir, std::vector<std::string>& diag)
	{
		std::size_t dropped = 0;
		for (auto& [env, p] : index.pools) {
			auto& v = p.entries;
			for (auto it = v.begin(); it != v.end();) {
				std::error_code ec;
				const auto      sz = fs::file_size(cacheDir / it->file, ec);
				if (ec) {
					diag.push_back("pool " + env + ": cache file " + it->file + " is missing; entry dropped");
				} else if (sz != it->bytes) {
					diag.push_back("pool " + env + ": cache file " + it->file + " is " + std::to_string(sz) + " bytes, expected " +
								   std::to_string(it->bytes) + "; entry dropped");
				} else {
					++it;
					continue;
				}
				it = v.erase(it);
				++dropped;
			}
		}
		return dropped;
	}

	std::vector<std::string> DropChangedPools(PoolIndex& index, const std::map<std::string, std::string>& wanted)
	{
		std::vector<std::string> dropped;
		for (auto it = index.pools.begin(); it != index.pools.end();) {
			const auto w = wanted.find(it->first);
			if (w == wanted.end() || w->second != it->second.fingerprint) {
				dropped.push_back(it->first);
				it = index.pools.erase(it);
			} else {
				++it;
			}
		}
		return dropped;
	}

	std::string EnvFingerprint(const config::Settings& s, std::string_view envId, bool serverAcceptsList)
	{
		const auto* e = s.Find(envId);
		if (!e || e->mode == config::Mode::Vanilla) {
			return {};
		}
		return Fingerprint(envId, *e, config::ChooseSources(s, *e, serverAcceptsList).sources);
	}

	bool DownloadStillValid(const config::Settings& s, std::string_view url, std::string_view key, std::string_view envId,
		std::string_view fingerprint, bool serverAcceptsList)
	{
		if (!config::NetworkActive(s) || s.global.serverUrl != url || s.global.personalKey != key) {
			return false;
		}
		const auto fp = EnvFingerprint(s, envId, serverAcceptsList);
		return !fp.empty() && fp == fingerprint;
	}

	DiskUsage MeasureDisk(const fs::path& cacheDir, const std::vector<fs::path>& extra)
	{
		DiskUsage       d;
		std::error_code ec;
		for (fs::directory_iterator it(cacheDir, ec), end; !ec && it != end; it.increment(ec)) {
			std::error_code e2;
			if (!IsCacheFileName(it->path().filename().string()) || !it->is_regular_file(e2)) {
				continue;
			}
			const auto sz = it->file_size(e2);
			if (!e2) {
				d.cacheBytes += sz;
				++d.cacheFiles;
			}
		}
		for (const auto& p : extra) {
			std::error_code e2;
			const auto      sz = fs::file_size(p, e2);
			if (!e2) {
				d.extraBytes += sz;
			}
		}
		return d;
	}

	bool PreviewOrTruncated(double decodedS, double expectedS, bool trimSilence)
	{
		if (!(expectedS > 60.0) || decodedS >= 0.9 * expectedS) {
			return false;
		}
		return !trimSilence || expectedS - decodedS > 30.0;
	}

	int CapAwareTarget(int base, int adaptive, std::uint64_t pooledBytes, std::uint64_t capBytes)
	{
		return pooledBytes < capBytes / 4 * 3 ? std::max(base, adaptive) : base;
	}

	bool RefillAllowedUnderCap(std::uint64_t pooledBytes, std::uint64_t capBytes, bool urgent, std::uint64_t oneTrackBytes)
	{
		return pooledBytes < capBytes || (urgent && pooledBytes < capBytes + oneTrackBytes);
	}

	bool ShouldPreempt(std::string_view jobEnv, bool jobUrgent, std::string_view env, bool envUrgent)
	{
		return envUrgent && !jobEnv.empty() && jobEnv != env && !jobUrgent;
	}

	bool IsHotEnvironment(std::string_view envId)
	{
		auto group = envId.substr(0, envId.find('.'));
		return group == "combat" || group == "boss";
	}

	int EffectivePoolSize(std::string_view envId, int poolSize)
	{
		return IsHotEnvironment(envId) ? std::max(poolSize, kHotPoolMin) : poolSize;
	}

	bool UrgentRefill(std::string_view, int have)
	{
		return have <= 1;
	}

	int AdaptivePoolSize(int base, double avgDownloadS, double avgTrackS)
	{
		if (!(avgDownloadS > 0.0) || !(avgTrackS > 1.0)) {
			return base;
		}
		const int want = static_cast<int>(std::ceil(avgDownloadS / avgTrackS)) + 2;
		return std::clamp(std::max(base, want), base, std::max(base, 8));
	}

	int RefillScore(std::string_view envId, int have, int target)
	{
		return (UrgentRefill(envId, have) ? 1000 : 0) + (IsHotEnvironment(envId) ? 100 : 0) + std::max(0, target - have);
	}

	bool ServiceAllowedBySources(std::string_view service, std::string_view sources)
	{
		if (sources == "all") {
			return true;
		}
		if (sources.empty()) {
			sources = "library";
		}
		const std::string_view want = service == "local" ? std::string_view("library") : service;
		while (!sources.empty()) {
			const auto comma = sources.find(',');
			const auto part  = sources.substr(0, comma);
			if (part == want) {
				return true;
			}
			if (comma == std::string_view::npos) {
				break;
			}
			sources.remove_prefix(comma + 1);
		}
		return false;
	}

	namespace
	{
		std::string_view ServiceOf(std::string_view key)
		{
			const auto c = key.find(':');
			return c == std::string_view::npos ? std::string_view() : key.substr(0, c);
		}
	}

	std::vector<std::string> ReplayOrder(const PoolIndex& index, std::string_view envId, std::string_view exclude, std::string_view sources)
	{
		std::vector<std::string> out;
		if (const auto r = index.replayable.find(envId); r != index.replayable.end()) {
			for (const auto& k : r->second) {  // oldest first
				if (k != exclude && ServiceAllowedBySources(ServiceOf(k), sources)) {
					out.push_back(k);
				}
			}
		}
		return out;
	}

	bool RepeatAllowed(const PoolIndex& index, std::string_view envId, std::string_view owner, std::string_view contentKey,
		std::string_view sources)
	{
		if (owner != envId || contentKey.empty() || !ServiceAllowedBySources(ServiceOf(contentKey), sources)) {
			return false;
		}
		if (const auto r = index.replayable.find(envId);
			r != index.replayable.end() && std::find(r->second.begin(), r->second.end(), contentKey) != r->second.end()) {
			return true;
		}
		if (const auto p = index.pools.find(envId); p != index.pools.end()) {
			for (const auto& e : p->second.entries) {
				if (e.Key() == contentKey) {
					return true;
				}
			}
		}
		return false;
	}

	std::vector<std::string> DropChangedSelections(PoolIndex& index, const std::map<std::string, std::string>& wanted)
	{
		auto dropped = DropChangedPools(index, wanted);
		for (const auto& env : dropped) {
			index.replayable.erase(env);  // the recent list stays (repeat avoidance, review R7-2)
		}
		return dropped;
	}

	std::string ConsentSources(const config::Settings& s, const config::EnvironmentSetting& e)
	{
		if (!e.allowStreaming) {
			return "library";
		}
		return s.global.allowQobuz ? "library,ytmusic,qobuz" : "library,ytmusic";
	}

	std::vector<std::string> ApplyConsent(PoolIndex& index, const std::map<std::string, std::string>& consentByEnv)
	{
		std::vector<std::string> changed;
		for (const auto& [env, consent] : consentByEnv) {
			bool lost = false;
			if (const auto p = index.pools.find(env); p != index.pools.end()) {
				lost |= std::erase_if(p->second.entries, [&](const PoolEntry& e) { return !ServiceAllowedBySources(e.service, consent); }) > 0;
			}
			if (const auto r = index.replayable.find(env); r != index.replayable.end()) {
				lost |= std::erase_if(r->second, [&](const std::string& k) { return !ServiceAllowedBySources(ServiceOf(k), consent); }) > 0;
			}
			if (lost) {
				changed.push_back(env);
			}
		}
		return changed;
	}

	const PoolEntry* FirstAllowedHead(const PoolIndex& index, std::string_view envId, std::string_view consent)
	{
		const auto p = index.pools.find(envId);
		if (p == index.pools.end()) {
			return nullptr;
		}
		for (const auto& e : p->second.entries) {
			if (ServiceAllowedBySources(e.service, consent)) {
				return &e;
			}
		}
		return nullptr;
	}

	void AddReplayable(PoolIndex& index, std::string_view envId, std::string_view key)
	{
		auto it = index.replayable.find(envId);
		if (it == index.replayable.end()) {
			it = index.replayable.emplace(std::string(envId), std::deque<std::string>{}).first;
		}
		auto& dq = it->second;
		if (const auto f = std::find(dq.begin(), dq.end(), key); f != dq.end()) {
			dq.erase(f);
		}
		dq.emplace_back(key);
		while (dq.size() > kRecentPerEnv) {
			dq.pop_front();
		}
	}

	bool IsRecent(const PoolIndex& index, std::string_view envId, std::string_view key)
	{
		const auto it = index.recent.find(envId);
		return it != index.recent.end() && std::find(it->second.begin(), it->second.end(), key) != it->second.end();
	}

	void AddRecent(PoolIndex& index, std::string_view envId, std::string_view key)
	{
		auto it = index.recent.find(envId);
		if (it == index.recent.end()) {
			it = index.recent.emplace(std::string(envId), std::deque<std::string>{}).first;
		}
		auto& dq = it->second;
		if (const auto f = std::find(dq.begin(), dq.end(), key); f != dq.end()) {
			dq.erase(f);  // move to the newest position
		}
		dq.emplace_back(key);
		while (dq.size() > kRecentPerEnv) {
			dq.pop_front();
		}
	}

	PickChoice AcceptablePicks(const PoolIndex& index, std::string_view envId,
		const std::vector<std::pair<std::string, std::string>>& picks, std::size_t want, const std::set<std::string>& exclude)
	{
		PickChoice                      out;
		std::unordered_set<std::string> taken;
		if (const auto p = index.pools.find(envId); p != index.pools.end()) {
			for (const auto& e : p->second.entries) {
				taken.insert(e.Key());
			}
		}
		const std::deque<std::string>* recent = nullptr;
		if (const auto r = index.recent.find(envId); r != index.recent.end()) {
			recent = &r->second;
		}
		// Eligible picks with their recent-list position (-1 = not played recently; larger = played later).
		std::vector<std::pair<std::size_t, long long>> eligible;
		for (std::size_t i = 0; i < picks.size(); ++i) {
			const auto key = TrackKey(picks[i].first, picks[i].second);
			if (picks[i].first.empty() || picks[i].second.empty() || exclude.contains(key) || !taken.insert(key).second) {
				continue;
			}
			long long pos = -1;
			if (recent) {
				if (const auto f = std::find(recent->begin(), recent->end(), key); f != recent->end()) {
					pos = f - recent->begin();
				}
			}
			eligible.emplace_back(i, pos);
		}
		for (const auto& [i, pos] : eligible) {
			if (pos < 0 && out.picks.size() < want) {
				out.picks.push_back(i);
			}
		}
		if (!out.picks.empty() || eligible.empty()) {
			return out;
		}
		// Only repeats came back: least recently played first. The track that JUST played (newest in the list) is never
		// queued behind a non-empty pool: it would come straight back (review B r2). With an empty pool it may.
		std::stable_sort(eligible.begin(), eligible.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
		if (!taken.empty() && recent && !recent->empty()) {
			const long long newest = static_cast<long long>(recent->size()) - 1;
			const std::size_t poolCount = index.pools.contains(envId) ? index.pools.find(envId)->second.entries.size() : 0;
			if (poolCount > 0) {
				std::erase_if(eligible, [newest](const auto& e) { return e.second == newest; });
			}
		}
		for (const auto& [i, pos] : eligible) {
			if (out.picks.size() >= want) {
				break;
			}
			out.picks.push_back(i);
		}
		out.repeats = !out.picks.empty();
		return out;
	}

	bool RemoveEntry(PoolIndex& index, std::string_view envId, std::string_view key)
	{
		const auto p = index.pools.find(envId);
		if (p == index.pools.end()) {
			return false;
		}
		auto& v = p->second.entries;
		const auto it = std::find_if(v.begin(), v.end(), [&](const PoolEntry& e) { return e.Key() == key; });
		if (it == v.end()) {
			return false;
		}
		v.erase(it);
		return true;
	}

	std::set<std::string> PooledFiles(const PoolIndex& index)
	{
		std::set<std::string> s;
		for (const auto& [env, p] : index.pools) {
			for (const auto& e : p.entries) {
				s.insert(e.file);
			}
		}
		return s;
	}

	std::vector<std::string> PlanEviction(std::vector<CacheFile> files, const std::set<std::string>& keep, std::uint64_t capBytes)
	{
		std::uint64_t total = 0;
		for (const auto& f : files) {
			total += f.bytes;
		}
		std::vector<std::string> out;
		if (total <= capBytes) {
			return out;
		}
		std::stable_sort(files.begin(), files.end(), [](const CacheFile& a, const CacheFile& b) {
			return a.lastUse != b.lastUse ? a.lastUse < b.lastUse : a.name < b.name;
		});
		for (const auto& f : files) {
			if (total <= capBytes) {
				break;
			}
			if (keep.contains(f.name) || !IsCacheFileName(f.name)) {
				continue;
			}
			out.push_back(f.name);
			total -= f.bytes;
		}
		return out;
	}
}
