#pragma once

// Pure pool / cache model (docs/INTEGRATION-1.0.md A.2, DESIGN 3 item 3, 8.7, 8.8).
//
// No engine, no network, no threads: the pool worker (pool/PoolWorker.cpp) owns one PoolIndex under its
// mutex and calls these functions; mj_core_tests exercises them directly. File IO here is limited to
// stat/remove of cache files and the index text (callers run it on a worker thread only).
//
// Safety rules encoded here:
//  - A cache file name is ALWAYS "<16 lowercase hex>.<wav|flac>", the FNV-1a 64 hash of (service, id).
//    The raw server id (untrusted: may contain ':', '/', '\\', "..") never becomes part of a path.
//  - Only files whose name has that exact shape are ever evicted or cleared; anything else in the cache
//    directory (the index itself, a user's file) is never touched.
//  - A parsed index is untrusted too: an entry whose file name is not of that shape is dropped.

#include "config/Settings.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mj::pool
{
	inline constexpr int         kIndexVersion   = 1;
	inline constexpr std::size_t kRecentPerEnv   = 50;   // plugin-side recent-played list (DESIGN 8.7)
	inline constexpr std::size_t kMaxPoolEntries = 16;   // hard cap per environment when parsing (largest setting is 8)
	inline constexpr const char* kIndexFileName  = "index.json";

	// One validated track in an environment's pool.
	struct PoolEntry
	{
		std::string   service;    // untrusted server strings: display/report only, never a path
		std::string   id;
		std::string   title;
		std::string   artist;
		double        durationS = 0.0;  // duration_s reported by `next`
		std::uint64_t bytes     = 0;    // size of the cache file when it was accepted
		std::string   file;             // cache file name (CacheFileName), relative to the cache dir
		std::string   sources;          // the `sources` value it was picked with

		[[nodiscard]] std::string Key() const;      // "service:id"
		[[nodiscard]] std::string Display() const;  // "Artist - Title" (or the title / the key)
	};

	struct EnvPool
	{
		std::string            fingerprint;  // Fingerprint() of the selector + sources that filled it
		std::vector<PoolEntry> entries;      // FIFO: entries[0] is the next track for the slot
	};

	struct PoolIndex
	{
		std::map<std::string, EnvPool, std::less<>>                 pools;   // environment id -> pool
		std::map<std::string, std::deque<std::string>, std::less<>> recent;  // environment id -> keys, oldest first
	};

	// ---- names and fingerprints ----------------------------------------------------------------
	[[nodiscard]] std::string   TrackKey(std::string_view service, std::string_view id);
	[[nodiscard]] std::uint64_t Fnv1a64(std::string_view data, std::uint64_t seed = 14695981039346656037ull);
	// "<16 hex>.wav" / "<16 hex>.flac". `container` must be "wav" or "flac" (anything else -> "wav").
	[[nodiscard]] std::string CacheFileName(std::string_view service, std::string_view id, std::string_view container);
	[[nodiscard]] bool        IsCacheFileName(std::string_view name);
	// Hex digest of everything that decides WHICH tracks a pool holds: Harmony type name, mode, selector
	// text, service, id, and the `sources` string. Pool size, remember-place and the container are not part
	// of it (they do not change which tracks are acceptable).
	[[nodiscard]] std::string Fingerprint(std::string_view typeName, const config::EnvironmentSetting& e, std::string_view sources);

	// ---- persistence -----------------------------------------------------------------------------
	[[nodiscard]] std::string SerializeIndex(const PoolIndex& index);
	// Tolerant: a broken file gives an empty index (false); bad entries are dropped with one diagnostic each
	// (never echoing server strings). Unknown fields are ignored.
	bool ParseIndex(std::string_view text, PoolIndex& out, std::vector<std::string>& diag);

	// Drops every entry whose file is missing in `cacheDir` or whose size differs from `bytes`.
	// Returns the number of dropped entries (one diagnostic each).
	std::size_t Revalidate(PoolIndex& index, const std::filesystem::path& cacheDir, std::vector<std::string>& diag);

	// `wanted`: environment id -> current fingerprint, for every environment that is configured (non-Vanilla).
	// A pool whose environment is absent from `wanted` or whose fingerprint differs is dropped (its files
	// become ordinary LRU cache files). Returns the dropped environment ids. Recent lists are kept.
	std::vector<std::string> DropChangedPools(PoolIndex& index, const std::map<std::string, std::string>& wanted);

	// The fingerprint an environment has under `s` (selector + `sources` from its streaming consent and the server's
	// capabilities), or "" when it is not configured or Vanilla. Settings that do not decide WHICH tracks are valid
	// (pool size, remember-place, toasts, log level, other environments) never change it.
	[[nodiscard]] std::string EnvFingerprint(const config::Settings& s, std::string_view envId, bool serverAcceptsList);

	// marth 2026-10-27: "downloads should only be dumped if the changed setting invalidates them". True when a pick that
	// is being downloaded for `envId` with `fingerprint` is still wanted under the new settings `s`: same server URL and
	// key as the running client (`url`, `key`), network on, and the environment's fingerprint unchanged.
	[[nodiscard]] bool DownloadStillValid(const config::Settings& s, std::string_view url, std::string_view key, std::string_view envId,
		std::string_view fingerprint, bool serverAcceptsList);

	// Bytes on disk of our cache files in `cacheDir` (IsCacheFileName only) plus the given extra files (slot files);
	// missing files count 0. Pure file stats (worker thread).
	struct DiskUsage
	{
		std::uint64_t cacheBytes = 0;
		std::size_t   cacheFiles = 0;
		std::uint64_t extraBytes = 0;
	};
	[[nodiscard]] DiskUsage MeasureDisk(const std::filesystem::path& cacheDir, const std::vector<std::filesystem::path>& extra);

	// ---- refill policy (field #4: a fight must never wait on the network) ---------------------------
	// Combat and boss drain fastest (a fight consumes a track and gives no warning): they keep at least 5 ready.
	inline constexpr int kHotPoolMin = 5;
	[[nodiscard]] bool IsHotEnvironment(std::string_view envId);  // "combat", "boss" and their sub-environments
	[[nodiscard]] int  EffectivePoolSize(std::string_view envId, int poolSize);
	// Urgent = the pool is empty, or a hot environment has fewer than 2 ready. An urgent refill may cancel a running
	// non-urgent download and gets the full download deadline; a non-urgent one gets a short one.
	[[nodiscard]] bool UrgentRefill(std::string_view envId, int have);
	// Which environment the pool thread refills next: larger first. Urgent before anything; then hot; then the gap.
	[[nodiscard]] int RefillScore(std::string_view envId, int have, int target);
	// Recently played keys to rotate through a slot while its pool is dry: least recently played first, without
	// `exclude` (the track the slot holds now). The caller keeps the ones whose cache file still exists.
	[[nodiscard]] std::vector<std::string> ReplayOrder(const PoolIndex& index, std::string_view envId, std::string_view exclude);

	// ---- recent-played list (per environment, bounded) -------------------------------------------
	[[nodiscard]] bool IsRecent(const PoolIndex& index, std::string_view envId, std::string_view key);
	void               AddRecent(PoolIndex& index, std::string_view envId, std::string_view key);

	// Which of `picks` (service, id pairs, in server order) may enter `envId`'s pool, at most `want`.
	// Never: an empty key, a key already in the pool, a duplicate within `picks`, a key in `exclude` (the
	// worker's short-lived set of downloads that just failed). Preferred: picks NOT in the recent-played list.
	// If every eligible pick was played recently (a small selection played through), the least recently played
	// ones are taken instead and `repeats` is set: a small selection repeats rather than starving the pool.
	struct PickChoice
	{
		std::vector<std::size_t> picks;
		bool                     repeats = false;
	};
	[[nodiscard]] PickChoice AcceptablePicks(const PoolIndex& index, std::string_view envId,
		const std::vector<std::pair<std::string, std::string>>& picks, std::size_t want, const std::set<std::string>& exclude = {});

	// Removes the entry with this key from the environment's pool. True if it was there.
	bool RemoveEntry(PoolIndex& index, std::string_view envId, std::string_view key);

	// Every cache file name referenced by any pool.
	[[nodiscard]] std::set<std::string> PooledFiles(const PoolIndex& index);

	// ---- LRU cache cap -----------------------------------------------------------------------------
	struct CacheFile
	{
		std::string   name;
		std::uint64_t bytes   = 0;
		std::int64_t  lastUse = 0;  // any monotonic clock (file last-write time); older = smaller
	};
	// Files to delete, least recently used first, until the total is <= capBytes. Never names a file in
	// `keep` or a name that is not IsCacheFileName(). The total may stay above the cap when only kept
	// files are left (the caller then stops downloading).
	[[nodiscard]] std::vector<std::string> PlanEviction(std::vector<CacheFile> files, const std::set<std::string>& keep,
		std::uint64_t capBytes);
}
