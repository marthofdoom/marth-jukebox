// Pool / cache model tests (pool/PoolModel.h): file naming, index persistence and revalidation,
// fingerprint-driven pool drops, recent-played list, pick filtering, LRU eviction.
#include "TestHarness.h"

#include "pool/PoolModel.h"

#include <fstream>
#include <functional>
#include <map>

using namespace mj::pool;
namespace fs = std::filesystem;

namespace
{
	PoolEntry Entry(const std::string& svc, const std::string& id, std::uint64_t bytes, const std::string& container = "wav")
	{
		PoolEntry e;
		e.service   = svc;
		e.id        = id;
		e.title     = "Title " + id;
		e.artist    = "Artist";
		e.durationS = 200;
		e.bytes     = bytes;
		e.file      = CacheFileName(svc, id, container);
		e.sources   = "library";
		return e;
	}

	void WriteBytes(const fs::path& p, std::size_t n)
	{
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		const std::string s(n, 'x');
		f.write(s.data(), static_cast<std::streamsize>(s.size()));
	}
}

MJ_TEST(Pool_CacheFileNameNeverUsesTheRawId)
{
	const auto a = CacheFileName("ytmusic", "..\\..\\evil:stream/x", "flac");
	MJ_CHECK(IsCacheFileName(a));
	MJ_CHECK(a.size() == 21 && a.ends_with(".flac"));
	MJ_CHECK(a.find("evil") == std::string::npos && a.find(':') == std::string::npos && a.find('\\') == std::string::npos);
	MJ_CHECK(CacheFileName("local", "abc", "wav").ends_with(".wav"));
	MJ_CHECK(CacheFileName("local", "abc", "exe").ends_with(".wav"));  // unknown container -> wav
	// Deterministic, and sensitive to the service/id split.
	MJ_CHECK(CacheFileName("local", "abc", "wav") == CacheFileName("local", "abc", "wav"));
	MJ_CHECK(CacheFileName("a:b", "c", "wav") != CacheFileName("a", "b:c", "wav"));
	MJ_CHECK(CacheFileName("local", "abc", "wav").substr(0, 16) == CacheFileName("local", "abc", "flac").substr(0, 16));

	MJ_CHECK(!IsCacheFileName("index.json"));
	MJ_CHECK(!IsCacheFileName("0123456789abcdef.mp3"));
	MJ_CHECK(!IsCacheFileName("0123456789ABCDEF.wav"));  // upper case is not ours
	MJ_CHECK(!IsCacheFileName("../23456789abcdef.wav"));
	MJ_CHECK(IsCacheFileName("0123456789abcdef.wav"));
	MJ_CHECK(TrackKey("local", "x:y") == "local:x:y");
}

MJ_TEST(Pool_FingerprintTracksSelectorAndSources)
{
	mj::config::EnvironmentSetting e;
	e.mode     = mj::config::Mode::Genre;
	e.selector = "Ambient";
	const auto base = Fingerprint("explore", e, "library");
	MJ_CHECK(base.size() == 32);
	MJ_CHECK(base == Fingerprint("explore", e, "library"));
	MJ_CHECK(base != Fingerprint("explore", e, "library,ytmusic"));
	MJ_CHECK(base != Fingerprint("explore.tundra", e, "library"));
	auto e2 = e;
	e2.selector = "Ambient ";
	MJ_CHECK(base != Fingerprint("explore", e2, "library"));
	e2 = e;
	e2.mode = mj::config::Mode::Query;
	MJ_CHECK(base != Fingerprint("explore", e2, "library"));
	// Pool size, remember-place and the streaming flag itself (only its sources string counts) do not matter.
	e2 = e;
	e2.poolSize      = 8;
	e2.rememberPlace = true;
	MJ_CHECK(base == Fingerprint("explore", e2, "library"));
}

MJ_TEST(Pool_IndexRoundTripAndHostileInput)
{
	PoolIndex idx;
	idx.pools["explore"].fingerprint = "fp1";
	idx.pools["explore"].entries     = { Entry("local", "a1", 100), Entry("ytmusic", "b2", 200, "flac") };
	idx.pools["combat"].fingerprint  = "fp2";
	idx.pools["combat"].entries      = { Entry("local", "c3", 300) };
	idx.pools["combat"].entries[0].title = "Bad \xFF utf8";  // must not throw on serialise
	AddRecent(idx, "explore", "local:z9");
	AddRecent(idx, "explore", "local:z8");

	const auto text = SerializeIndex(idx);
	PoolIndex  back;
	std::vector<std::string> diag;
	MJ_CHECK(ParseIndex(text, back, diag));
	MJ_CHECK_MSG(diag.empty(), diag.empty() ? "" : diag.front());
	MJ_CHECK(back.pools.size() == 2);
	MJ_CHECK(back.pools["explore"].fingerprint == "fp1");
	MJ_CHECK(back.pools["explore"].entries.size() == 2);
	MJ_CHECK(back.pools["explore"].entries[1].file == idx.pools["explore"].entries[1].file);
	MJ_CHECK(back.pools["explore"].entries[1].bytes == 200 && back.pools["explore"].entries[1].durationS == 200);
	MJ_CHECK(back.pools["explore"].entries[0].Display() == "Artist - Title a1");
	MJ_CHECK(back.recent["explore"].size() == 2 && back.recent["explore"].back() == "local:z8");

	// Garbage and wrong versions give an empty index.
	diag.clear();
	MJ_CHECK(!ParseIndex("not json", back, diag) && back.pools.empty() && diag.size() == 1);
	diag.clear();
	MJ_CHECK(!ParseIndex(R"({"version": 99, "pools": {}})", back, diag) && diag.size() == 1);

	// A tampered entry pointing outside the cache dir (or with no size) is dropped; the rest survive.
	diag.clear();
	const std::string hostile = R"({"version":1,"pools":{"explore":{"fingerprint":"f","entries":[
		{"service":"local","id":"x","file":"..\\..\\Skyrim.esm","bytes":5,"duration_s":10},
		{"service":"local","id":"y","file":"0123456789abcdef.wav","bytes":0,"duration_s":10},
		{"service":"local","id":"z","file":"0123456789abcdef.wav","bytes":7,"duration_s":10},
		{"service":"local","id":"z","file":"0123456789abcdef.wav","bytes":7,"duration_s":10},
		42]}},"recent":{"explore":["a",1,"b"]}})";
	MJ_CHECK(ParseIndex(hostile, back, diag));
	MJ_CHECK(back.pools["explore"].entries.size() == 1 && back.pools["explore"].entries[0].id == "z");
	MJ_CHECK(diag.size() == 4);
	MJ_CHECK(back.recent["explore"].size() == 2);
	for (const auto& d : diag) {
		MJ_CHECK(d.find("Skyrim") == std::string::npos);  // diagnostics never echo server/index strings
	}
}

MJ_TEST(Pool_RevalidateDropsMissingAndResizedFiles)
{
	mjtest::ScratchDir dir;
	PoolIndex          idx;
	auto&              p = idx.pools["explore"];
	p.entries = { Entry("local", "ok", 10), Entry("local", "missing", 10), Entry("local", "resized", 10) };
	WriteBytes(dir.Path() / p.entries[0].file, 10);
	WriteBytes(dir.Path() / p.entries[2].file, 11);
	std::vector<std::string> diag;
	MJ_CHECK(Revalidate(idx, dir.Path(), diag) == 2);
	MJ_CHECK(p.entries.size() == 1 && p.entries[0].id == "ok");
	MJ_CHECK(diag.size() == 2);
}

MJ_TEST(Pool_DropChangedPoolsKeepsUnchangedAndRecent)
{
	PoolIndex idx;
	idx.pools["explore"].fingerprint = "same";
	idx.pools["combat"].fingerprint  = "old";
	idx.pools["town"].fingerprint    = "x";
	AddRecent(idx, "combat", "local:1");
	const auto dropped = DropChangedPools(idx, { { "explore", "same" }, { "combat", "new" } });
	MJ_CHECK(dropped.size() == 2);
	MJ_CHECK(idx.pools.contains("explore") && !idx.pools.contains("combat") && !idx.pools.contains("town"));
	MJ_CHECK(IsRecent(idx, "combat", "local:1"));  // the recent list outlives a selector change
}

MJ_TEST(Pool_RecentListIsBoundedAndPerEnvironment)
{
	PoolIndex idx;
	for (int i = 0; i < 60; ++i) {
		AddRecent(idx, "explore", "local:" + std::to_string(i));
	}
	MJ_CHECK(idx.recent["explore"].size() == kRecentPerEnv);
	MJ_CHECK(!IsRecent(idx, "explore", "local:9"));
	MJ_CHECK(IsRecent(idx, "explore", "local:10") && IsRecent(idx, "explore", "local:59"));
	MJ_CHECK(!IsRecent(idx, "combat", "local:59"));
	AddRecent(idx, "explore", "local:10");  // re-adding moves it to the newest end, no duplicate
	MJ_CHECK(idx.recent["explore"].size() == kRecentPerEnv && idx.recent["explore"].back() == "local:10");
	AddRecent(idx, "explore", "local:60");
	MJ_CHECK(IsRecent(idx, "explore", "local:10") && !IsRecent(idx, "explore", "local:11"));
}

MJ_TEST(Pool_AcceptablePicksFiltersRepeats)
{
	PoolIndex idx;
	idx.pools["explore"].entries = { Entry("local", "inpool", 1) };
	AddRecent(idx, "explore", "local:played");
	const std::vector<std::pair<std::string, std::string>> picks = {
		{ "local", "played" }, { "local", "inpool" }, { "local", "new1" }, { "local", "new1" }, { "", "x" }, { "ytmusic", "new2" },
		{ "local", "new3" }
	};
	const auto ok = AcceptablePicks(idx, "explore", picks, 2);
	MJ_CHECK(ok.picks.size() == 2 && ok.picks[0] == 2 && ok.picks[1] == 5 && !ok.repeats);
	MJ_CHECK(AcceptablePicks(idx, "explore", picks, 10).picks.size() == 3);
	MJ_CHECK(AcceptablePicks(idx, "combat", picks, 10).picks.size() == 5);  // other env: only dup/empty filtered
	// A failed download (short-lived exclude set) is skipped but does NOT count as played.
	const auto ex = AcceptablePicks(idx, "explore", picks, 10, { "local:new1" });
	MJ_CHECK(ex.picks.size() == 2 && ex.picks[0] == 5 && !IsRecent(idx, "explore", "local:new1"));
	// Only repeats: the least recently played one is taken (and flagged) instead of nothing.
	AddRecent(idx, "explore", "local:b");
	AddRecent(idx, "explore", "local:a");
	const auto rep = AcceptablePicks(idx, "explore", { { "local", "a" }, { "local", "b" }, { "local", "played" } }, 1);
	MJ_CHECK(rep.repeats && rep.picks.size() == 1 && rep.picks[0] == 2);  // "played" is the oldest in the list
	// A non-empty pool (one track waiting):
	idx.pools["explore"].entries = { Entry("local", "head", 1) };
	// "a" is the track that just played (newest): with a non-empty pool it is never queued again right away.
	const auto rep2 = AcceptablePicks(idx, "explore", { { "local", "a" }, { "local", "b" } }, 2);
	MJ_CHECK(rep2.repeats && rep2.picks.size() == 1 && rep2.picks[0] == 1);
	const auto only = AcceptablePicks(idx, "explore", { { "local", "a" } }, 1);
	MJ_CHECK(only.picks.empty() && !only.repeats);
	// With an empty pool the just-played track may come back (the pool would otherwise stay empty).
	idx.pools["explore"].entries.clear();
	const auto empty = AcceptablePicks(idx, "explore", { { "local", "a" } }, 1);
	MJ_CHECK(empty.repeats && empty.picks.size() == 1);
	idx.pools["explore"].entries = { Entry("local", "inpool", 1) };
	MJ_CHECK(RemoveEntry(idx, "explore", "local:inpool") && idx.pools["explore"].entries.empty());
	MJ_CHECK(!RemoveEntry(idx, "explore", "local:inpool") && !RemoveEntry(idx, "nope", "local:x"));
}

MJ_TEST(Pool_EvictionIsLruAndNeverTouchesKeptOrForeignFiles)
{
	const std::string a = CacheFileName("local", "a", "wav"), b = CacheFileName("local", "b", "wav"),
					  c = CacheFileName("local", "c", "flac"), d = CacheFileName("local", "d", "wav");
	std::vector<CacheFile> files = { { a, 100, 4 }, { b, 100, 1 }, { c, 100, 2 }, { d, 100, 3 }, { "index.json", 500, 0 } };
	// Under the cap: nothing.
	MJ_CHECK(PlanEviction(files, {}, 10'000).empty());
	// Over by 250 counting the foreign file: oldest first (b, c, d), never index.json.
	auto ev = PlanEviction(files, {}, 650);
	MJ_CHECK(ev.size() == 3 && ev[0] == b && ev[1] == c && ev[2] == d);
	// c is pooled: skipped, the next oldest goes instead.
	ev = PlanEviction(files, { c }, 750);
	MJ_CHECK(ev.size() == 2 && ev[0] == b && ev[1] == d);
	// Only kept/foreign files left: the plan stops short of the cap rather than touching them.
	ev = PlanEviction(files, { a, b, c, d }, 0);
	MJ_CHECK(ev.empty());
	MJ_CHECK(PooledFiles(PoolIndex{}).empty());
}

// Review B1: a 10-track selection played through twice must never starve the pool (the recent list holds 50).
MJ_TEST(Pool_SmallSelectionPlaysThroughTwice)
{
	PoolIndex         idx;
	const std::string env = "explore";
	std::vector<std::string> bag;
	for (int i = 0; i < 10; ++i) {
		bag.push_back("t" + std::to_string(i));
	}
	std::size_t cursor = 0;  // the server's bag: `next` consumes `count` per call, wrapping around
	auto next = [&](int count) {
		std::vector<std::pair<std::string, std::string>> v;
		for (int k = 0; k < count; ++k) {
			v.emplace_back("local", bag[cursor++ % bag.size()]);
		}
		return v;
	};
	const std::size_t          poolSize = 3;
	std::map<std::string, int> plays;
	bool                       sawRepeatFlag = false;
	std::string                last;
	for (int play = 0; play < 20; ++play) {
		for (int attempt = 0; attempt < 3 && idx.pools[env].entries.size() < poolSize; ++attempt) {
			const auto picks = next(3);
			const auto c     = AcceptablePicks(idx, env, picks, poolSize - idx.pools[env].entries.size());
			sawRepeatFlag |= c.repeats;
			for (const auto i : c.picks) {
				idx.pools[env].entries.push_back(Entry(picks[i].first, picks[i].second, 1));
			}
		}
		MJ_CHECK_MSG(!idx.pools[env].entries.empty(), "pool starved at play " + std::to_string(play));
		if (idx.pools[env].entries.empty()) {
			return;
		}
		const auto head = idx.pools[env].entries.front().Key();
		MJ_CHECK_MSG(head != last, "back-to-back repeat of " + head);
		last = head;
		++plays[head];
		MJ_CHECK(RemoveEntry(idx, env, head));
		AddRecent(idx, env, head);
	}
	// Every track is heard; the server's bag consumes 3 per call while the pool takes fewer, so the spread is
	// uneven (backlog: count-3-and-filter wastes picks), but nothing starves and nothing dominates.
	MJ_CHECK(plays.size() == 10);
	for (const auto& [k, n] : plays) {
		MJ_CHECK_MSG(n >= 1 && n <= 3, k + " played " + std::to_string(n) + " times");
	}
	MJ_CHECK(sawRepeatFlag);
}

// marth 2026-10-27: "downloads should only be dumped if the changed setting invalidates them".
namespace
{
	mj::config::Settings Base()
	{
		mj::config::Settings s;
		s.global.serverUrl   = "http://music.example.lan:8080";
		s.global.personalKey = "k";
		auto& ex             = s.environments["explore"];
		ex.mode              = mj::config::Mode::Genre;
		ex.selector          = "ambient";
		ex.allowStreaming    = true;
		auto& co             = s.environments["combat"];
		co.mode              = mj::config::Mode::Query;
		co.selector          = "epic battle";
		return s;
	}

	std::map<std::string, std::string> Wanted(const mj::config::Settings& s)
	{
		std::map<std::string, std::string> w;
		for (const auto& [id, e] : s.environments) {
			if (const auto f = EnvFingerprint(s, id, true); !f.empty()) {
				w[id] = f;
			}
		}
		return w;
	}

	using Edit = std::pair<const char*, std::function<void(mj::config::Settings&)>>;
}

MJ_TEST(Pool_OnlyInvalidatingChangesTouchAnEnvironment)
{
	using mj::config::Mode;
	const auto s0  = Base();
	const auto fpE = EnvFingerprint(s0, "explore", true);
	const auto fpC = EnvFingerprint(s0, "combat", true);
	const std::string url = "http://music.example.lan:8080";
	MJ_CHECK(!fpE.empty() && !fpC.empty() && EnvFingerprint(s0, "boss", true).empty());

	// Changes that must NOT touch explore's pool, slot or running download.
	const std::vector<Edit> keep = {
		{ "other environment toggled", [](mj::config::Settings& s) { s.environments["combat"].mode = Mode::Vanilla; } },
		{ "preset fills other environments",
			[](mj::config::Settings& s) {
				s.environments["dungeon"].mode     = Mode::Genre;
				s.environments["dungeon"].selector = "dark";
				s.environments["boss"].mode        = Mode::Query;
				s.environments["boss"].selector    = "boss fight";
			} },
		{ "pool size grows", [](mj::config::Settings& s) { s.environments["explore"].poolSize = 8; } },
		{ "pool size shrinks", [](mj::config::Settings& s) { s.environments["explore"].poolSize = 3; } },
		{ "remember place", [](mj::config::Settings& s) { s.environments["explore"].rememberPlace = true; } },
		{ "toasts and log level",
			[](mj::config::Settings& s) {
				s.global.toastEnabled         = false;
				s.global.toastIntervalMinutes = 30;
				s.global.logLevel             = mj::config::LogLevel::Debug;
			} },
		{ "cache cap", [](mj::config::Settings& s) { s.global.cacheCapMB = 8192; } },
	};
	for (const auto& [what, fn] : keep) {
		auto s = s0;
		fn(s);
		MJ_CHECK_MSG(EnvFingerprint(s, "explore", true) == fpE, what);
		MJ_CHECK_MSG(DownloadStillValid(s, url, "k", "explore", fpE, true), what);
		PoolIndex idx;
		idx.pools["explore"].fingerprint = fpE;
		idx.pools["explore"].entries     = { Entry("local", "a", 1), Entry("local", "b", 1), Entry("local", "c", 1) };
		(void)DropChangedPools(idx, Wanted(s));
		MJ_CHECK_MSG(idx.pools.contains("explore") && idx.pools["explore"].entries.size() == 3, what);  // nothing dumped
	}

	// Changes that DO invalidate explore, and only explore.
	const std::vector<Edit> drop = {
		{ "selector", [](mj::config::Settings& s) { s.environments["explore"].selector = "folk"; } },
		{ "mode", [](mj::config::Settings& s) { s.environments["explore"].mode = Mode::Query; } },
		{ "streaming consent", [](mj::config::Settings& s) { s.environments["explore"].allowStreaming = false; } },
		{ "AllowQobuz while streaming", [](mj::config::Settings& s) { s.global.allowQobuz = true; } },
		{ "set to Vanilla", [](mj::config::Settings& s) { s.environments["explore"].mode = Mode::Vanilla; } },
	};
	for (const auto& [what, fn] : drop) {
		auto s = s0;
		fn(s);
		MJ_CHECK_MSG(!DownloadStillValid(s, url, "k", "explore", fpE, true), what);
		MJ_CHECK_MSG(DownloadStillValid(s, url, "k", "combat", fpC, true), what);  // the other environment is untouched
		PoolIndex idx;
		idx.pools["explore"].fingerprint = fpE;
		idx.pools["combat"].fingerprint  = fpC;
		(void)DropChangedPools(idx, Wanted(s));
		MJ_CHECK_MSG(!idx.pools.contains("explore") && idx.pools.contains("combat"), what);
	}
	// AllowQobuz does not matter for an environment without streaming.
	auto q = s0;
	q.global.allowQobuz = true;
	MJ_CHECK(EnvFingerprint(q, "combat", true) == fpC);

	// Server / key / network: every download is invalid (the client is re-configured).
	auto u = s0;
	u.global.serverUrl = "http://other.lan:8080";
	MJ_CHECK(!DownloadStillValid(u, url, "k", "combat", fpC, true));
	MJ_CHECK(!DownloadStillValid(s0, url, "other-key", "combat", fpC, true));
	auto n = s0;
	n.global.features.network = false;
	MJ_CHECK(!DownloadStillValid(n, url, "k", "combat", fpC, true));
}

MJ_TEST(Pool_MeasureDiskCountsCacheAndSlotFiles)
{
	mjtest::ScratchDir dir;
	const auto         cache = dir.Path() / "cache";
	fs::create_directories(cache);
	WriteBytes(cache / CacheFileName("local", "a", "flac"), 100);
	WriteBytes(cache / CacheFileName("local", "b", "wav"), 50);
	WriteBytes(cache / "index.json", 7);                                       // not a cache file
	WriteBytes(cache / (CacheFileName("local", "c", "wav") + ".part"), 999);  // a partial download: not counted
	WriteBytes(dir.Path() / "slot_00.wav", 30);
	const auto d = MeasureDisk(cache, { dir.Path() / "slot_00.wav", dir.Path() / "slot_01.wav" });
	MJ_CHECK(d.cacheBytes == 150 && d.cacheFiles == 2 && d.extraBytes == 30);
}

// Field #4: refill policy (combat/boss first and deeper; urgent before anything) and the replay rotation order.
MJ_TEST(Pool_RefillPolicy)
{
	MJ_CHECK(IsHotEnvironment("combat") && IsHotEnvironment("combat.civil_war") && IsHotEnvironment("boss"));
	MJ_CHECK(!IsHotEnvironment("explore") && !IsHotEnvironment("dungeon.cave") && !IsHotEnvironment("combatx"));
	MJ_CHECK(EffectivePoolSize("combat", 3) == 5 && EffectivePoolSize("combat", 8) == 8 && EffectivePoolSize("explore", 3) == 3);
	MJ_CHECK(UrgentRefill("explore", 0) && !UrgentRefill("explore", 1));
	MJ_CHECK(UrgentRefill("combat", 1) && !UrgentRefill("combat", 2));
	// An empty dungeon pool still beats a combat pool at 2 of 5; a combat pool at 1 beats a dungeon at 1 of 3.
	MJ_CHECK(RefillScore("dungeon", 0, 3) > RefillScore("combat", 2, 5));
	MJ_CHECK(RefillScore("combat", 1, 5) > RefillScore("dungeon", 1, 3));
	// Never a non-urgent dungeon refill ahead of an urgent combat one.
	MJ_CHECK(RefillScore("combat", 0, 5) > RefillScore("dungeon", 2, 3));
	MJ_CHECK(RefillScore("explore", 3, 3) < RefillScore("explore", 2, 3));

	PoolIndex idx;
	AddRecent(idx, "combat", "local:a");
	AddRecent(idx, "combat", "local:b");
	AddRecent(idx, "combat", "local:c");
	const auto order = ReplayOrder(idx, "combat", "local:c");
	MJ_CHECK(order.size() == 2 && order[0] == "local:a" && order[1] == "local:b");
	MJ_CHECK(ReplayOrder(idx, "explore", "").empty());
}

// Review R5-4: the preview guard with and without trim_silence.
MJ_TEST(Pool_PreviewGuardWithTrim)
{
	MJ_CHECK(PreviewOrTruncated(30.0, 224.0, false));   // Qobuz preview
	MJ_CHECK(PreviewOrTruncated(30.0, 224.0, true));    // still caught with trim on
	MJ_CHECK(PreviewOrTruncated(190.0, 224.0, false));  // 85%: too short without trim
	MJ_CHECK(PreviewOrTruncated(190.0, 224.0, true));   // 85% and 34 s short: still rejected with trim
	MJ_CHECK(!PreviewOrTruncated(200.0, 224.0, true));  // 89%, 24 s of trimmed silence: accepted with trim
	MJ_CHECK(!PreviewOrTruncated(210.0, 224.0, false)); // 94%: fine either way
	MJ_CHECK(!PreviewOrTruncated(40.0, 59.0, false));   // tracks up to 60 s are not checked
}

// Review R6-1: replays and repeats follow the CURRENT selection and streaming consent.
MJ_TEST(Pool_ReplayRespectsConsentAndSelection)
{
	MJ_CHECK(ServiceAllowedBySources("local", "library") && !ServiceAllowedBySources("ytmusic", "library"));
	MJ_CHECK(ServiceAllowedBySources("ytmusic", "library,ytmusic") && ServiceAllowedBySources("local", "library,ytmusic"));
	MJ_CHECK(!ServiceAllowedBySources("qobuz", "library,ytmusic"));  // Qobuz never without AllowQobuz
	MJ_CHECK(ServiceAllowedBySources("qobuz", "library,ytmusic,qobuz"));
	MJ_CHECK(ServiceAllowedBySources("ytmusic", "all") && !ServiceAllowedBySources("ytmusic", ""));

	// Unchanged selection: the replay behaviour stays.
	PoolIndex idx;
	idx.pools["combat"].fingerprint = "fp-stream";
	AddRecent(idx, "combat", "ytmusic:old1");
	AddRecent(idx, "combat", "local:lib1");
	AddRecent(idx, "combat", "ytmusic:now");
	MJ_CHECK(ReplayOrder(idx, "combat", "ytmusic:now", "library,ytmusic").size() == 2);
	MJ_CHECK(RepeatAllowed(idx, "combat", "combat", "ytmusic:now", "library,ytmusic"));

	// The player UNTICKS streaming: same environment, sources become "library" (and the fingerprint changes).
	// Even before the pool is dropped, no YouTube track is replayed or repeated.
	const auto lib = ReplayOrder(idx, "combat", "ytmusic:now", "library");
	MJ_CHECK(lib.size() == 1 && lib[0] == "local:lib1");
	MJ_CHECK(!RepeatAllowed(idx, "combat", "combat", "ytmusic:now", "library"));
	// After the drop the recent list is gone too: nothing of the old selection is left -> Fail (vanilla).
	const auto dropped = DropChangedSelections(idx, { { "combat", "fp-library" } });
	MJ_CHECK(dropped.size() == 1 && !idx.recent.contains("combat"));
	MJ_CHECK(ReplayOrder(idx, "combat", "", "library").empty());
	MJ_CHECK(!RepeatAllowed(idx, "combat", "combat", "local:lib1", "library"));

	// A selector change with a dry pool: same outcome (nothing of the old selection replays).
	PoolIndex sel;
	sel.pools["explore"].fingerprint = "ambient";
	AddRecent(sel, "explore", "local:a");
	(void)DropChangedSelections(sel, { { "explore", "folk" } });
	MJ_CHECK(ReplayOrder(sel, "explore", "", "library").empty() && !RepeatAllowed(sel, "explore", "explore", "local:a", "library"));
	// Unchanged environments keep their recent lists.
	PoolIndex keep;
	keep.pools["explore"].fingerprint = "ambient";
	AddRecent(keep, "explore", "local:a");
	MJ_CHECK(DropChangedSelections(keep, { { "explore", "ambient" } }).empty() && IsRecent(keep, "explore", "local:a"));
	// A slot owned by another environment, or with unknown content, never repeats.
	MJ_CHECK(!RepeatAllowed(keep, "explore", "combat", "local:a", "library") && !RepeatAllowed(keep, "explore", "explore", "", "library"));
}
