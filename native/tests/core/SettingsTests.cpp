#include "TestHarness.h"

#include "config/ProfileBuilder.h"
#include "config/Settings.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#	include <windows.h>
#endif

using namespace mj::config;
namespace fs = std::filesystem;

namespace
{
	std::string ReadAll(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::ostringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	void WriteAll(const fs::path& p, const std::string& text)
	{
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		out << text;
	}

	// Captures everything the module logs.
	struct LogCapture
	{
		std::mutex                m;
		std::vector<std::string>  lines;
		LogCapture()
		{
			SetLogSink([this](spdlog::level::level_enum, const std::string& s) {
				std::lock_guard lk(m);
				lines.push_back(s);
			});
		}
		~LogCapture() { SetLogSink([](spdlog::level::level_enum, const std::string&) {}); }
		bool Contains(const std::string& needle)
		{
			std::lock_guard lk(m);
			return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; });
		}
	};

	std::string LowerStr(std::string s)
	{
		for (auto& c : s) {
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		}
		return s;
	}

	// Leak = the secret, or its first 16 characters (a truncated echo), in any letter case.
	bool Leaks(const std::string& line, const std::string& secret)
	{
		const auto l = LowerStr(line);
		return l.find(LowerStr(secret)) != std::string::npos || l.find(LowerStr(secret.substr(0, 16))) != std::string::npos;
	}

	bool AnyContains(const Diagnostics& d, const std::string& needle)
	{
		return std::any_of(d.begin(), d.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; });
	}
}

MJ_TEST(Settings_Defaults)
{
	const Settings s = DefaultSettings();
	MJ_CHECK(s.version == kSettingsVersion);
	MJ_CHECK(s.global.serverUrl.empty() && s.global.personalKey.empty());
	MJ_CHECK(s.global.harmonyProfileName == "skyrim-jukebox");
	MJ_CHECK(s.global.cacheDir.empty() && s.global.cacheCapMB == 4096);
	MJ_CHECK(s.global.logLevel == LogLevel::Info);
	MJ_CHECK(s.global.features.retarget && s.global.features.network && s.global.features.menu && s.global.features.flac &&
			 s.global.features.rememberPlace);
	MJ_CHECK(s.environments.empty());
	MJ_CHECK(!NetworkActive(s));  // network off until a server URL is set
	EnvironmentSetting e;
	MJ_CHECK(e.mode == Mode::Vanilla && !e.allowStreaming && !e.rememberPlace && e.poolSize == 3);
	MJ_CHECK(SourcesFor(e) == "library");
	e.allowStreaming = true;
	MJ_CHECK(SourcesFor(e) == "all");
	MJ_CHECK(EffectiveCacheDir(s, "D:/def") == fs::path("D:/def"));
}

MJ_TEST(Settings_IniRoundTrip)
{
	Settings s = DefaultSettings();
	s.global.serverUrl = "https://harmony.lan:8080";
	s.global.personalKey = "k3y-with=equals;and semicolon";
	s.global.harmonyProfileName = "my.profile_1";
	s.global.cacheDir = "D:\\Games\\Cache dir";
	s.global.cacheCapMB = 8192;
	s.global.logLevel = LogLevel::Debug;
	s.global.features.flac = false;
	s.global.features.network = false;
	auto& ex = s.environments["explore"];
	ex.mode = Mode::Genre;
	ex.selector = "Ambient; chill = good";
	ex.poolSize = 5;
	ex.allowStreaming = true;
	auto& tu = s.environments["explore.tundra"];
	tu.mode = Mode::Artist;
	tu.selector = "Sigur R\xC3\xB3s";
	tu.service = "ytmusic";
	tu.id = "UC1234567890abcdefghij";
	tu.rememberPlace = true;
	tu.poolSize = 8;
	s.environments["combat"].mode = Mode::Query;
	s.environments["combat"].selector = "epic battle music";
	s.environments["boss"];  // Vanilla entry survives
	s.environments["other.WTMUSExplore"].mode = Mode::Genre;
	s.environments["other.WTMUSExplore"].selector = "folk";

	Diagnostics d0;
	Sanitize(s, d0);
	MJ_CHECK(d0.empty());

	const std::string text = SerializeIni(s);
	Diagnostics d;
	const Settings r = ParseIni(text, d);
	MJ_CHECK_MSG(d.empty(), d.empty() ? "" : d.front());
	MJ_CHECK(r.global.serverUrl == s.global.serverUrl);
	MJ_CHECK(r.global.personalKey == s.global.personalKey);
	MJ_CHECK(r.global.harmonyProfileName == s.global.harmonyProfileName);
	MJ_CHECK(r.global.cacheDir == s.global.cacheDir);
	MJ_CHECK(r.global.cacheCapMB == 8192 && r.global.logLevel == LogLevel::Debug);
	MJ_CHECK(!r.global.features.flac && !r.global.features.network && r.global.features.retarget);
	MJ_CHECK(r.environments.size() == s.environments.size());
	for (const auto& [id, e] : s.environments) {
		const auto* g = r.Find(id);
		MJ_CHECK_MSG(g != nullptr, id);
		if (!g) continue;
		MJ_CHECK(g->mode == e.mode && g->selector == e.selector && g->service == e.service && g->id == e.id &&
				 g->allowStreaming == e.allowStreaming && g->rememberPlace == e.rememberPlace && g->poolSize == e.poolSize);
	}
	// Stable output: serialising the parsed copy gives the same bytes.
	MJ_CHECK(SerializeIni(r) == text);
	// The warning comment sits right above the key.
	const auto w = text.find("PLAIN TEXT");
	const auto k = text.find("PersonalKey=");
	MJ_CHECK(w != std::string::npos && k != std::string::npos && w < k);
}

MJ_TEST(Settings_HandEditedGarbageIsSurvived)
{
	const std::string ini =
		"\xEF\xBB\xBF; comment\r\n"
		"# another\r\n"
		"this line has no equals sign\r\n"
		"orphan=1\r\n"                       // key outside any section
		"[General]\r\n"
		"   serverurl   =   http://h.lan:8080//   \r\n"  // case-insensitive key, spaces, trailing slashes
		"CacheCapMB = 12abc\r\n"             // not a number -> keeps default
		"LogLevel = LOUD\r\n"
		"Bogus=1\r\n"
		"Version=7\r\n"
		"[Features]\r\n"
		"Flac = nope\r\n"
		"Menu = off\r\n"
		"Retarget = TRUE\r\n"
		"[Env:explore]\r\n"
		"Mode = Genre\r\n"
		"Selector=Ambient\r\n"
		"Selector=Folk\r\n"                  // duplicate: last wins
		"PoolSize=4\r\n"
		"[Env:nonsense]\r\n"
		"Mode=Genre\r\n"
		"[Env:explore.nonsense]\r\n"
		"Mode=Genre\r\n"
		"[Surprise]\r\n"
		"Mode=Genre\r\n"
		"[broken\r\n"
		"Mode=Genre\r\n"
		"[Env:combat]\r\n"
		"Mode = Qobuz-radio\r\n"             // unknown mode keeps Vanilla
		"AllowStreaming = maybe\r\n"
		"[Env:town]\r\n"
		"Mode=Artist\r\n"
		"Service=Qobuz\r\n"                  // excluded service
		"Id=123\r\n";
	Diagnostics d;
	const Settings s = ParseIni(ini, d);
	MJ_CHECK(!d.empty());
	MJ_CHECK(s.global.serverUrl == "http://h.lan:8080");
	MJ_CHECK(!AnyContains(d, "Bogus") && !AnyContains(d, "bogus") && !AnyContains(d, "Surprise") && !AnyContains(d, "nonsense"));
	MJ_CHECK(s.global.cacheCapMB == kCacheCapDefaultMB);
	MJ_CHECK(s.global.logLevel == LogLevel::Info);
	MJ_CHECK(s.global.features.flac && !s.global.features.menu && s.global.features.retarget);
	MJ_CHECK(s.version == kSettingsVersion);
	MJ_CHECK(AnyContains(d, "newer") || AnyContains(d, "version"));
	const auto* ex = s.Find("explore");
	MJ_CHECK(ex && ex->mode == Mode::Genre && ex->selector == "Folk" && ex->poolSize == 5);
	MJ_CHECK(!s.Find("nonsense") && !s.Find("explore.nonsense"));
	const auto* co = s.Find("combat");
	MJ_CHECK(co && co->mode == Mode::Vanilla && !co->allowStreaming);
	const auto* tw = s.Find("town");
	MJ_CHECK(tw && tw->mode == Mode::Vanilla && tw->service.empty());
	MJ_CHECK(AnyContains(d, "Qobuz"));
	// Total garbage must not throw or produce nonsense.
	for (const char* g : { "", "\n\n\n", "[", "]", "=", "[]\n=\n[Env:]\n", "\x01\x02\x03\xff\xfe", "[General]\nCacheCapMB=99999999999999999999999\n" }) {
		Diagnostics dd;
		const Settings gs = ParseIni(g, dd);
		MJ_CHECK(gs.global.cacheCapMB >= kCacheCapMinMB && gs.global.cacheCapMB <= kCacheCapMaxMB);
	}
}

MJ_TEST(Settings_ClampingAndValidation)
{
	Settings s;
	s.global.cacheCapMB = 10;
	s.global.serverUrl = "ftp://nope";
	s.global.harmonyProfileName = "bad name!";
	s.global.personalKey = std::string(300, 'x');
	s.global.cacheDir = std::string(600, 'd');
	auto& e = s.environments["dungeon"];
	e.mode = Mode::Genre;
	e.poolSize = 100;
	e.selector = std::string(kMaxTextLen + 1, 'a');
	s.environments["zzz-unknown"];
	Diagnostics d;
	Sanitize(s, d);
	MJ_CHECK(s.global.cacheCapMB == kCacheCapMinMB);
	MJ_CHECK(s.global.serverUrl.empty());
	MJ_CHECK(s.global.harmonyProfileName == "skyrim-jukebox");
	MJ_CHECK(s.global.personalKey.empty() && s.global.cacheDir.empty());
	MJ_CHECK(s.environments.size() == 1);
	MJ_CHECK(s.environments["dungeon"].poolSize == 8 && s.environments["dungeon"].selector.empty());
	MJ_CHECK(d.size() >= 6);

	s.global.cacheCapMB = 2'000'000'000;
	Diagnostics d2;
	Sanitize(s, d2);
	MJ_CHECK(s.global.cacheCapMB == kCacheCapMaxMB);

	for (auto [in, want] : { std::pair{ 0, 3 }, std::pair{ 1, 3 }, std::pair{ 3, 3 }, std::pair{ 4, 5 }, std::pair{ 5, 5 }, std::pair{ 6, 5 },
			 std::pair{ 7, 8 }, std::pair{ 8, 8 }, std::pair{ 9, 8 }, std::pair{ -4, 3 } }) {
		Settings p;
		p.environments["boss"].poolSize = in;
		Diagnostics dp;
		Sanitize(p, dp);
		MJ_CHECK_MSG(p.environments["boss"].poolSize == want, std::to_string(in));
	}

	// Qobuz never survives: artist/playlist -> Vanilla, other modes just lose the service.
	Settings q;
	q.environments["boss"] = { Mode::Playlist, "x", "qobuz", "1", true, false, 3 };
	q.environments["combat"] = { Mode::Genre, "metal", "qobuz", "", false, false, 3 };
	Diagnostics dq;
	Sanitize(q, dq);
	MJ_CHECK(q.environments["boss"].mode == Mode::Vanilla && q.environments["boss"].service.empty());
	MJ_CHECK(q.environments["combat"].mode == Mode::Genre && q.environments["combat"].service.empty());

	MJ_CHECK(ToString(Mode::Playlist) == "Playlist");
	Mode m = Mode::Vanilla;
	MJ_CHECK(ParseMode(" QUERY ", m) && m == Mode::Query && !ParseMode("radio", m));
}

MJ_TEST(Settings_NetworkActiveNeedsUrlAndSwitch)
{
	Settings s;
	s.global.serverUrl = "http://h";
	MJ_CHECK(NetworkActive(s));
	s.global.features.network = false;
	MJ_CHECK(!NetworkActive(s));
	s.global.features.network = true;
	s.global.serverUrl.clear();
	MJ_CHECK(!NetworkActive(s));
}

MJ_TEST(Store_MissingFileGivesDefaultsAndWritesNothing)
{
	mjtest::ScratchDir dir;
	SettingsStore st(dir.Path() / "sub" / "MarthJukebox.ini", dir.Path() / "cache");
	MJ_CHECK(st.Load());
	MJ_CHECK(!fs::exists(st.IniPath()));
	MJ_CHECK(st.Snapshot()->environments.empty());
	MJ_CHECK(st.Revision() == 1);
	MJ_CHECK(st.EffectiveCacheDir() == dir.Path() / "cache");
}

MJ_TEST(Store_SaveLoadRoundTripAndNoTmpLeft)
{
	mjtest::ScratchDir dir;
	const auto ini = dir.Path() / "deep" / "er" / "MarthJukebox.ini";
	SettingsStore st(ini, dir.Path() / "cache");
	st.Update([](Settings& s) {
		s.global.serverUrl = "http://harmony:8080";
		s.environments["combat"].mode = Mode::Genre;
		s.environments["combat"].selector = "metal";
	});
	std::string err;
	MJ_CHECK_MSG(st.Save(&err), err);
	MJ_CHECK(fs::exists(ini));
	MJ_CHECK(!fs::exists(fs::path(ini.string() + ".tmp")));

	SettingsStore st2(ini, dir.Path() / "cache");
	MJ_CHECK(st2.Load());
	MJ_CHECK(st2.Snapshot()->global.serverUrl == "http://harmony:8080");
	MJ_CHECK(st2.Snapshot()->Find("combat") && st2.Snapshot()->Find("combat")->selector == "metal");

	// Custom cache dir wins over the injected default.
	st2.Update([](Settings& s) { s.global.cacheDir = "E:/mycache"; });
	MJ_CHECK(st2.EffectiveCacheDir() == fs::path("E:/mycache"));
}

MJ_TEST(Store_AtomicWriteSurvivesStaleTmp)
{
	mjtest::ScratchDir dir;
	const auto ini = dir.Path() / "MarthJukebox.ini";
	const auto tmp = fs::path(ini.string() + ".tmp");

	SettingsStore st(ini, dir.Path() / "cache");
	st.Update([](Settings& s) { s.global.cacheCapMB = 1000; });
	MJ_CHECK(st.Save());
	const std::string v1 = ReadAll(ini);

	// A crash after the tmp was written but before the replace leaves garbage next to a valid INI.
	WriteAll(tmp, "[General]\nCacheCapMB=77777\npartial wri");
	MJ_CHECK(ReadAll(ini) == v1);  // the real file is untouched by the stale tmp

	{
		LogCapture cap;
		SettingsStore fresh(ini, dir.Path() / "cache");
		MJ_CHECK(fresh.Load());
		MJ_CHECK(fresh.Snapshot()->global.cacheCapMB == 1000);  // read the INI, not the tmp
		MJ_CHECK(!fs::exists(tmp));                             // stale tmp removed
		MJ_CHECK(cap.Contains("stale"));
	}

	// Saving over a stale tmp works and replaces the INI wholesale.
	WriteAll(tmp, std::string(4096, 'Z'));
	st.Update([](Settings& s) { s.global.cacheCapMB = 2000; });
	MJ_CHECK(st.Save());
	MJ_CHECK(!fs::exists(tmp));
	SettingsStore again(ini, dir.Path() / "cache");
	MJ_CHECK(again.Load() && again.Snapshot()->global.cacheCapMB == 2000);
	MJ_CHECK(ReadAll(ini).find('Z') == std::string::npos);

	// First-ever save interrupted: only a tmp exists. Load must ignore it and give defaults.
	mjtest::ScratchDir dir2;
	const auto ini2 = dir2.Path() / "MarthJukebox.ini";
	WriteAll(fs::path(ini2.string() + ".tmp"), "[General]\nServerUrl=http://evil\n");
	SettingsStore s2(ini2, dir2.Path());
	MJ_CHECK(s2.Load());
	MJ_CHECK(s2.Snapshot()->global.serverUrl.empty());
}

MJ_TEST(Store_FailedSaveLeavesOldFileIntact)
{
	mjtest::ScratchDir dir;
	const auto ini = dir.Path() / "MarthJukebox.ini";
	SettingsStore st(ini, dir.Path());
	st.Update([](Settings& s) { s.global.cacheCapMB = 1234; });
	MJ_CHECK(st.Save());
	const std::string before = ReadAll(ini);

	// Block the tmp path with a directory: opening it for writing must fail, the INI stays as it was.
	fs::create_directory(fs::path(ini.string() + ".tmp"));
	st.Update([](Settings& s) { s.global.cacheCapMB = 4321; });
	std::string err;
	MJ_CHECK(!st.Save(&err));
	MJ_CHECK(!err.empty());
	MJ_CHECK(ReadAll(ini) == before);
	MJ_CHECK(st.Snapshot()->global.cacheCapMB == 4321);  // memory keeps the edit; caller may retry

	// A parent that is a file cannot be created.
	WriteAll(dir.Path() / "afile", "x");
	SettingsStore bad(dir.Path() / "afile" / "MarthJukebox.ini", dir.Path());
	MJ_CHECK(!bad.Save(&err));
}

MJ_TEST(Store_SnapshotIsImmutableAndRevisionMonotonic)
{
	mjtest::ScratchDir dir;
	SettingsStore st(dir.Path() / "a.ini", dir.Path());
	const auto s0 = st.Snapshot();
	const auto r0 = st.Revision();
	const auto r1 = st.Update([](Settings& s) { s.global.cacheCapMB = 5000; });
	const auto s1 = st.Snapshot();
	MJ_CHECK(r1 > r0 && st.Revision() == r1 && s1->revision == r1);
	MJ_CHECK(s0->global.cacheCapMB == kCacheCapDefaultMB);  // the old view never changes
	MJ_CHECK(s1->global.cacheCapMB == 5000);
	MJ_CHECK(s0.get() != s1.get());
	const auto r2 = st.ResetToDefaults();
	MJ_CHECK(r2 > r1 && st.Snapshot()->global.cacheCapMB == kCacheCapDefaultMB);
	MJ_CHECK(s1->global.cacheCapMB == 5000);
}

MJ_TEST(Store_ConcurrentWritersAndReaders)
{
	mjtest::ScratchDir dir;
	SettingsStore st(dir.Path() / "a.ini", dir.Path());
	constexpr int kWriters = 4;
	constexpr int kPerWriter = 300;
	std::atomic<bool> stop{ false };
	std::atomic<int> torn{ 0 };
	std::atomic<int> regress{ 0 };

	std::vector<std::thread> readers;
	for (int r = 0; r < 3; ++r) {
		readers.emplace_back([&] {
			std::uint64_t last = 0;
			while (!stop.load()) {
				const auto snap = st.Snapshot();
				// Writers always set serverUrl and the explore selector as a pair.
				const auto* e = snap->Find("explore");
				const std::string url = snap->global.serverUrl;
				const std::string sel = e ? e->selector : std::string();
				if (!url.empty() && url != "http://h" + sel) {
					++torn;
				}
				if (snap->revision < last) {
					++regress;
				}
				last = snap->revision;
				// Reading the same snapshot again must give the same values (immutability).
				if (snap->global.serverUrl != url) {
					++torn;
				}
			}
		});
	}
	std::vector<std::thread> writers;
	for (int w = 0; w < kWriters; ++w) {
		writers.emplace_back([&, w] {
			for (int i = 0; i < kPerWriter; ++i) {
				st.Update([&](Settings& s) {
					const std::string tag = std::to_string(w) + "_" + std::to_string(i);
					s.global.serverUrl = "http://h" + tag;
					auto& e = s.environments["explore"];
					e.mode = Mode::Genre;
					e.selector = tag;
					s.global.cacheCapMB += 1;  // lost updates would show here
				});
			}
		});
	}
	for (auto& t : writers) t.join();
	stop = true;
	for (auto& t : readers) t.join();

	MJ_CHECK(torn.load() == 0);
	MJ_CHECK(regress.load() == 0);
	MJ_CHECK(st.Snapshot()->global.cacheCapMB == kCacheCapDefaultMB + kWriters * kPerWriter);
	MJ_CHECK(st.Revision() == static_cast<std::uint64_t>(kWriters * kPerWriter));
}

MJ_TEST(Settings_PersonalKeyNeverInMessages)
{
	const std::string key = "SEKRET-KEY-0123456789abcdef";
	const std::string url = "http://harmony.lan:8080/?key=" + key;
	LogCapture cap;
	Diagnostics all;
	std::vector<std::string> produced;

	// 1. Garbage that contains the key in places a careless message might echo.
	const std::string ini =
		"PersonalKey " + key + "\n"                       // no '='
		"[General]\n"
		+ key + "=1\n"                                      // unknown key whose NAME is the secret
		+ key.substr(0, 10) + "=1\n" +                      // ...and a truncated one
		"[" + key + "]\n"                                   // unknown section named after the secret
		"x=1\n"
		"[Env:" + key + "]\n"                               // unknown environment named after the secret
		"Mode=Genre\n"
		"[Env:other." + key + "]\n"                         // valid advanced id built from the secret
		"Mode=Genre\n"
		"Selector=x\n"
		+ key + "=1\n" +
		"[Features]\n"
		+ key + "=1\n" +
		"[General]\n"
		"PersonalKey=" + key + "\n"
		"ServerUrl=ftp://" + key + "\n"                    // rejected URL
		"CacheCapMB=" + key + "\n"                         // bad number
		"LogLevel=" + key + "\n"
		"HarmonyProfileName=" + key + " " + key + "\n"     // invalid name (space)
		"[Features]\n"
		"Network=" + key + "\n"
		"[Env:explore]\n"
		"Mode=" + key + "\n"
		"PoolSize=" + key + "\n"
		"AllowStreaming=" + key + "\n";
	Diagnostics d;
	Settings s = ParseIni(ini, d);
	all.insert(all.end(), d.begin(), d.end());
	MJ_CHECK(s.global.personalKey == key);  // it IS stored (plain text, by design)...
	MJ_CHECK(s.global.serverUrl.empty());

	// 2. Store operations with a key + a server URL carrying ?key=.
	mjtest::ScratchDir dir;
	SettingsStore st(dir.Path() / "MarthJukebox.ini", dir.Path());
	st.Update([&](Settings& x) {
		x.global.personalKey = key;
		x.global.serverUrl = url;
		x.global.cacheCapMB = 1;           // clamped -> logged
		x.global.harmonyProfileName = "bad name";
		auto& e = x.environments["explore"];
		e.mode = Mode::Artist;
		e.service = "qobuz";
		e.id = key;
	});
	std::string err;
	MJ_CHECK(st.Save(&err));
	produced.push_back(err);
	MJ_CHECK(st.Load());
	fs::create_directory(fs::path(st.IniPath().string() + ".tmp"));
	MJ_CHECK(!st.Save(&err));
	produced.push_back(err);
	fs::remove(fs::path(st.IniPath().string() + ".tmp"));

	// 3. Profile builder: problems + JSON.
	Settings p = *st.Snapshot();
	p.environments["combat"] = { Mode::Playlist, "x", "ytmusic", key + "!bad", false, false, 3 };
	p.environments["boss"] = { Mode::Genre, std::string(250, 'g'), "", "", false, false, 3 };
	std::vector<ProfileProblem> probs;
	const std::string json = BuildHarmonyProfile(p, &probs);
	for (const auto& pr : probs) produced.push_back(pr.message);
	produced.push_back(json);
	MJ_CHECK(!probs.empty());

	// Nothing the module produced contains the key (or the URL's embedded copy of it).
	MJ_CHECK(!all.empty());
	for (const auto& line : all) MJ_CHECK_MSG(!Leaks(line, key), line);
	for (const auto& line : produced) MJ_CHECK_MSG(!Leaks(line, key), line);
	{
		std::lock_guard lk(cap.m);
		MJ_CHECK(!cap.lines.empty());
		for (const auto& line : cap.lines) MJ_CHECK_MSG(!Leaks(line, key), line);
	}
	// The INI file itself holds the key in plain text, with the warning above it.
	MJ_CHECK(ReadAll(st.IniPath()).find("PLAIN TEXT") != std::string::npos);
}

// ---- review round: a bad Load must never cost the user's INI (and key) ---------------------------------------

namespace
{
	const char* kGoodIni = "[General]\nServerUrl=http://harmony:8080\nPersonalKey=SEKRET-VALUE\nCacheCapMB=2048\n";

	std::string Utf16Le(const std::string& ascii)
	{
		std::string r = "\xFF\xFE";
		for (char c : ascii) {
			r += c;
			r += '\0';
		}
		return r;
	}

	void ExpectPreservedOnNextSave(const std::string& original, const char* what)
	{
		mjtest::ScratchDir dir;
		const auto ini = dir.Path() / "MarthJukebox.ini";
		WriteAll(ini, original);
		SettingsStore st(ini, dir.Path());
		MJ_CHECK_MSG(!st.Load(), what);  // reported as unusable
		MJ_CHECK(st.Snapshot()->global.serverUrl.empty());  // defaults in memory
		MJ_CHECK_MSG(ReadAll(ini) == original, what);       // Load itself never touches the file
		st.Update([](Settings& s) { s.global.cacheCapMB = 999; });
		std::string err;
		MJ_CHECK_MSG(st.Save(&err), err);
		const fs::path side = fs::path(ini.string() + ".unreadable");
		MJ_CHECK_MSG(fs::exists(side), what);
		MJ_CHECK_MSG(ReadAll(side) == original, what);      // the original bytes are intact
		MJ_CHECK_MSG(ReadAll(ini).find("CacheCapMB=999") != std::string::npos, what);

		// A second unusable file in a later session never overwrites the first preserved copy.
		WriteAll(ini, original + "second");
		SettingsStore st2(ini, dir.Path());
		MJ_CHECK(!st2.Load());
		MJ_CHECK(st2.Save());
		MJ_CHECK(ReadAll(side) == original);
		MJ_CHECK(ReadAll(fs::path(ini.string() + ".unreadable.1")) == original + "second");
	}
}

MJ_TEST(Store_GarbledFileIsPreservedNotOverwritten)
{
	ExpectPreservedOnNextSave("", "zero-byte file");
	ExpectPreservedOnNextSave(std::string(512, '\0'), "zero-filled file");
	ExpectPreservedOnNextSave(Utf16Le(kGoodIni), "UTF-16 file");
	ExpectPreservedOnNextSave("; just a comment\nnothing = here\n", "no recognised key");
	ExpectPreservedOnNextSave("\x01\x02\x03 binary \xff\xfe", "binary");
}

#if !defined(_WIN32)
MJ_TEST(Store_UnreadableFileRefusesSaveAndKeepsOriginal)
{
	mjtest::ScratchDir dir;
	const auto ini = dir.Path() / "MarthJukebox.ini";
	WriteAll(ini, kGoodIni);
	fs::permissions(ini, fs::perms::none);
	SettingsStore st(ini, dir.Path());
	const bool canRead = std::ifstream(ini).good();
	if (!canRead) {  // (skipped when running as root, which can read anything)
		MJ_CHECK(!st.Load());
		std::string err;
		MJ_CHECK(!st.Save(&err));  // cannot preserve it => refuses
		MJ_CHECK(!err.empty());
		fs::permissions(ini, fs::perms::owner_all);
		MJ_CHECK(ReadAll(ini) == kGoodIni);
	} else {
		fs::permissions(ini, fs::perms::owner_all);
	}
}
#endif

MJ_TEST(Store_KeepsBackupOfPreviousGoodFileOnFirstSaveOnly)
{
	mjtest::ScratchDir dir;
	const auto ini = dir.Path() / "MarthJukebox.ini";
	WriteAll(ini, kGoodIni);
	const auto bak = fs::path(ini.string() + ".bak");
	SettingsStore st(ini, dir.Path());
	MJ_CHECK(st.Load());
	MJ_CHECK(st.Snapshot()->global.personalKey == "SEKRET-VALUE");
	st.Update([](Settings& s) { s.global.cacheCapMB = 3000; });
	MJ_CHECK(st.Save());
	MJ_CHECK(ReadAll(bak) == kGoodIni);  // the previous good file
	st.Update([](Settings& s) { s.global.cacheCapMB = 3500; });
	MJ_CHECK(st.Save());
	MJ_CHECK(ReadAll(bak) == kGoodIni);  // not replaced by a later (session) state
	MJ_CHECK(!fs::exists(fs::path(ini.string() + ".unreadable")));
}

MJ_TEST(Store_ReplaceFailureLeavesOriginalAndCleansTmp)
{
	// Destination is a non-empty directory: the replace step fails on every platform.
	mjtest::ScratchDir dir;
	const auto ini = dir.Path() / "MarthJukebox.ini";
	fs::create_directory(ini);
	WriteAll(ini / "keep.txt", "x");
	SettingsStore st(ini, dir.Path());
	st.Update([](Settings& s) { s.global.cacheCapMB = 1111; });
	std::string err;
	MJ_CHECK(!st.Save(&err));
	MJ_CHECK(!err.empty());
	MJ_CHECK(fs::is_directory(ini) && ReadAll(ini / "keep.txt") == "x");
	MJ_CHECK(!fs::exists(fs::path(ini.string() + ".tmp")));  // no litter

#if defined(_WIN32)
	// Locked destination (open without sharing): MoveFileExW fails, the file is untouched, the error is clean.
	mjtest::ScratchDir dir2;
	const auto ini2 = dir2.Path() / "MarthJukebox.ini";
	WriteAll(ini2, kGoodIni);
	SettingsStore st2(ini2, dir2.Path());
	MJ_CHECK(st2.Load());
	st2.Update([](Settings& s) { s.global.cacheCapMB = 2222; });
	MJ_CHECK(st2.Save());  // first save of the session: makes the .bak, so the next one goes straight to the replace
	const std::string afterFirst = ReadAll(ini2);
	st2.Update([](Settings& s) { s.global.cacheCapMB = 3333; });
	HANDLE h = CreateFileW(ini2.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	MJ_CHECK(h != INVALID_HANDLE_VALUE);
	std::string err2;
	const bool saved = st2.Save(&err2);
	CloseHandle(h);
	MJ_CHECK(!saved && !err2.empty());
	MJ_CHECK(ReadAll(ini2) == afterFirst);
	MJ_CHECK(!fs::exists(fs::path(ini2.string() + ".tmp")));
#endif
}

MJ_TEST(Settings_OtherModsIdRefusesTableEdids)
{
	Settings s;
	s.environments["other.WTMUSExplore"].mode = Mode::Genre;       // plain unmapped mod EDID: kept
	s.environments["other.MUSTavern"].mode = Mode::Genre;          // diegetic table EDID: refused
	s.environments["other.musexploretundra"].mode = Mode::Genre;   // table EDID, any case: refused
	s.environments["other.DLC2MUSExploreNew"].mode = Mode::Genre;  // matches an official prefix rule: refused
	s.environments["other.MUSE3DemoDungeon"].mode = Mode::Genre;   // known leftover: refused
	Diagnostics d;
	Sanitize(s, d);
	MJ_CHECK(s.environments.size() == 1 && s.Find("other.WTMUSExplore"));
	MJ_CHECK(!d.empty());
}
