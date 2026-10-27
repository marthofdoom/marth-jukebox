// Pool / cache model tests (pool/PoolModel.h): file naming, index persistence and revalidation,
// fingerprint-driven pool drops, recent-played list, pick filtering, LRU eviction.
#include "TestHarness.h"

#include "pool/PoolModel.h"

#include <fstream>
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
	const auto rep2 = AcceptablePicks(idx, "explore", { { "local", "a" }, { "local", "b" } }, 2);
	MJ_CHECK(rep2.repeats && rep2.picks.size() == 2 && rep2.picks[0] == 1 && rep2.picks[1] == 0);
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
