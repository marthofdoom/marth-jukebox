#include "TestHarness.h"

#include "menu/StubApp.h"
#include "menu/ViewModel.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace MJ::Menu;
using mj::config::Mode;

namespace
{
	MJ::App::SearchHit Hit(const char* kind, const char* service, const char* id, const char* title, const char* sub = "")
	{
		MJ::App::SearchHit h;
		h.kind = kind;
		h.service = service;
		h.id = id;
		h.title = title;
		h.subtitle = sub;
		return h;
	}
}

// ---- debounce state machine --------------------------------------------------------------------------------

MJ_TEST(Debounce_FiresOnceAfterQuiet)
{
	Debouncer d;
	MJ_CHECK(!d.Poll(1000, 400));  // nothing pending
	d.Touch(1000);
	MJ_CHECK(d.Pending());
	MJ_CHECK(!d.Poll(1399, 400));
	MJ_CHECK(d.Poll(1400, 400));
	MJ_CHECK(!d.Pending());
	MJ_CHECK(!d.Poll(9000, 400));  // only once per burst
}

MJ_TEST(Debounce_TypingRestartsTheWait)
{
	Debouncer d;
	d.Touch(0);
	d.Touch(300);
	d.Touch(600);
	MJ_CHECK(!d.Poll(900, 400));   // 300 ms after the last key
	MJ_CHECK(d.Poll(1000, 400));
}

MJ_TEST(Debounce_CancelAndClockGoingBackwards)
{
	Debouncer d;
	d.Touch(500);
	d.Cancel();
	MJ_CHECK(!d.Poll(5000, 400));
	d.Touch(500);
	MJ_CHECK(!d.Poll(100, 400));  // a clock that went backwards must not fire
}

MJ_TEST(ConfirmGate_TwoClicks)
{
	ConfirmGate g;
	MJ_CHECK(!g.Press(1000));
	MJ_CHECK(g.Armed(2000));
	MJ_CHECK(g.Press(2000));
	MJ_CHECK(!g.Armed(2000));
	MJ_CHECK(!g.Press(3000));            // armed again
	MJ_CHECK(!g.Press(3000 + kConfirmWindowMs + 1));  // window expired: re-arms, does not fire
	MJ_CHECK(g.Press(3000 + kConfirmWindowMs + 2));
}

MJ_TEST(TextField_CommitsOnlyWhenDirtyAndKeepsUserEdits)
{
	TextField f;
	f.SyncFromStore("a");
	MJ_CHECK(f.edit == "a" && !f.Dirty());
	MJ_CHECK(!f.Commit());
	f.edit = "ab";
	MJ_CHECK(f.Dirty());
	f.SyncFromStore("zzz");  // the user is typing: the stored value must not clobber the buffer
	MJ_CHECK(f.edit == "ab");
	MJ_CHECK(f.Commit());
	MJ_CHECK(!f.Commit());
	f.SyncFromStore("normalized");  // clean again: adopts the stored (sanitized) value
	MJ_CHECK(f.edit == "normalized" && f.committed == "normalized");
}

// ---- validation --------------------------------------------------------------------------------------------

MJ_TEST(Validate_ServerUrl)
{
	MJ_CHECK(ValidateServerUrl("").ok);  // network off is a valid state
	MJ_CHECK(ValidateServerUrl("http://192.168.0.5:8080").ok);
	MJ_CHECK(ValidateServerUrl("HTTPS://harmony.example").ok);
	MJ_CHECK(!ValidateServerUrl("harmony:8080").ok);
	MJ_CHECK(!ValidateServerUrl("ftp://x").ok);
	MJ_CHECK(!ValidateServerUrl("http://").ok);
	MJ_CHECK(!ValidateServerUrl("https://").ok);
	MJ_CHECK(!ValidateServerUrl("http://a b").ok);
	MJ_CHECK(!ValidateServerUrl("http://a\tb").ok);
	MJ_CHECK(!ValidateServerUrl("http://" + std::string(600, 'a')).ok);
}

MJ_TEST(Validate_KeyMessagesNeverEchoTheKey)
{
	const std::string secret = "SECRET-KEY-VALUE";
	MJ_CHECK(ValidatePersonalKey("").ok);
	MJ_CHECK(ValidatePersonalKey(secret).ok);
	const auto bad = ValidatePersonalKey(secret + " with space");
	MJ_CHECK(!bad.ok);
	MJ_CHECK(bad.message.find("SECRET") == std::string::npos);
	MJ_CHECK(!ValidatePersonalKey(std::string(257, 'k')).ok);
	const auto url = ValidateServerUrl("http://secret-host/ has space");
	MJ_CHECK(url.message.find("secret-host") == std::string::npos);
}

MJ_TEST(Validate_SelectorText)
{
	MJ_CHECK(ValidateSelectorText("calm piano").ok);
	MJ_CHECK(ValidateSelectorText(std::string(200, 'a')).ok);
	MJ_CHECK(!ValidateSelectorText(std::string(201, 'a')).ok);
	MJ_CHECK(!ValidateSelectorText("bad\ntext").ok);
	// 200 two-byte characters are 400 bytes but 200 code points: allowed.
	std::string cyr;
	for (int i = 0; i < 200; ++i) cyr += "\xD0\xB1";
	MJ_CHECK(CountCodePoints(cyr) == 200);
	MJ_CHECK(ValidateSelectorText(cyr).ok);
	MJ_CHECK(!ValidateSelectorText(cyr + "\xD0\xB1").ok);
}

// ---- modes and rows ----------------------------------------------------------------------------------------

MJ_TEST(ModeChoices_GroupVsSub)
{
	const auto g = ModeChoices(false);
	MJ_CHECK(g.size() == 5);
	MJ_CHECK(!g.front().inherit && g.front().mode == Mode::Vanilla);
	const auto s = ModeChoices(true);
	MJ_CHECK(s.size() == 6);
	MJ_CHECK(s.front().inherit);

	MJ_CHECK(ModeChoiceIndex(g, nullptr) == 0);
	MJ_CHECK(ModeChoiceIndex(s, nullptr) == 0);  // no override = inherit
	mj::config::EnvironmentSetting e;
	e.mode = Mode::Artist;
	MJ_CHECK(g[static_cast<std::size_t>(ModeChoiceIndex(g, &e))].mode == Mode::Artist);
	MJ_CHECK(s[static_cast<std::size_t>(ModeChoiceIndex(s, &e))].mode == Mode::Artist);
	e.mode = Mode::Vanilla;  // explicit Vanilla override on a sub is NOT "inherit"
	MJ_CHECK(!s[static_cast<std::size_t>(ModeChoiceIndex(s, &e))].inherit);
}

MJ_TEST(Rows_PlayableGroupsWithSubsOnly)
{
	const auto rows = BuildRows();
	MJ_CHECK(!rows.empty());
	bool sawExplore = false, sawSnow = false;
	for (const auto& r : rows) {
		MJ_CHECK(mj::categories::IsKnownEnvironmentId(r.id));
		MJ_CHECK(r.id != "tavern" && r.id != "story" && r.id != "menu");
		MJ_CHECK(r.isSub == !r.parentId.empty());
		if (r.id == "explore") sawExplore = true;
		if (r.id == "explore.snow") { sawSnow = true; MJ_CHECK(r.parentId == "explore"); }
	}
	MJ_CHECK(sawExplore && sawSnow);
	MJ_CHECK(!rows.front().isSub);  // a group always precedes its subs
}

// ---- edits -------------------------------------------------------------------------------------------------

MJ_TEST(Edit_ModeChangeClearsSelectionAndKeepsFlags)
{
	auto s = mj::config::DefaultSettings();
	SetMode(s, "explore", Mode::Artist);
	auto& e = s.environments["explore"];
	e.selector = "Name"; e.service = "local"; e.id = "42"; e.allowStreaming = true; e.rememberPlace = true;
	SetMode(s, "explore", Mode::Genre);
	MJ_CHECK(e.mode == Mode::Genre);
	MJ_CHECK(e.selector.empty() && e.service.empty() && e.id.empty());
	MJ_CHECK(e.allowStreaming && e.rememberPlace);
	e.selector = "Ambient";
	SetMode(s, "explore", Mode::Genre);  // same mode: selection stays
	MJ_CHECK(e.selector == "Ambient");
}

MJ_TEST(Edit_SubInheritErasesOverrideAndNewSubCopiesParentFlags)
{
	auto s = mj::config::DefaultSettings();
	SetMode(s, "explore", Mode::Genre);
	SetStreaming(s, "explore", true);
	SetRememberPlace(s, "explore", true);
	SetPoolSizeAll(s, 5);
	SetMode(s, "explore.snow", Mode::Artist);  // creates the override
	const auto* sub = s.Find("explore.snow");
	MJ_CHECK(sub != nullptr);
	MJ_CHECK(sub->allowStreaming && sub->rememberPlace && sub->poolSize == 5);
	SetMode(s, "explore.snow", Mode::Vanilla, true);  // inherit
	MJ_CHECK(s.Find("explore.snow") == nullptr);
}

MJ_TEST(Edit_StreamingDefaultsOffAndIsPerEnvironment)
{
	auto s = mj::config::DefaultSettings();
	SetMode(s, "combat", Mode::Genre);
	MJ_CHECK(!s.Find("combat")->allowStreaming);
	SetStreaming(s, "explore", true);
	MJ_CHECK(s.Find("explore")->allowStreaming);
	MJ_CHECK(!s.Find("combat")->allowStreaming);
}

MJ_TEST(Edit_SelectorTextOnlyForGenreAndQuery)
{
	auto s = mj::config::DefaultSettings();
	MJ_CHECK(!SetSelectorText(s, "explore", "x"));  // no entry
	SetMode(s, "explore", Mode::Artist);
	MJ_CHECK(!SetSelectorText(s, "explore", "x"));
	SetMode(s, "explore", Mode::Query);
	MJ_CHECK(SetSelectorText(s, "explore", "calm piano"));
	MJ_CHECK(s.Find("explore")->selector == "calm piano");
	MJ_CHECK(!SetSelectorText(s, "explore", std::string(201, 'a')));
	MJ_CHECK(s.Find("explore")->selector == "calm piano");
	MJ_CHECK(!SetSelectorText(s, "explore", "bad\nline"));
}

MJ_TEST(Edit_PoolSizeAllAndCurrent)
{
	auto s = mj::config::DefaultSettings();
	MJ_CHECK(CurrentPoolSize(s) == 3);
	SetMode(s, "explore", Mode::Genre);
	SetMode(s, "combat", Mode::Genre);
	SetPoolSizeAll(s, 8);
	MJ_CHECK(s.Find("explore")->poolSize == 8 && s.Find("combat")->poolSize == 8);
	MJ_CHECK(CurrentPoolSize(s) == 8);
	SetPoolSizeAll(s, 4);  // not an allowed size
	MJ_CHECK(s.Find("explore")->poolSize == 8);
	SetMode(s, "town", Mode::Genre);  // new entries use the current size
	MJ_CHECK(s.Find("town")->poolSize == 8);
}

MJ_TEST(Edit_ResetToVanilla)
{
	auto s = mj::config::DefaultSettings();
	SetMode(s, "explore", Mode::Genre);
	s.environments["explore"].selector = "Ambient";
	SetMode(s, "explore.snow", Mode::Artist);
	SetMode(s, "explore.tundra", Mode::Playlist);
	ResetToVanilla(s, "explore.snow");  // parent active -> explicit Vanilla override
	MJ_CHECK(s.Find("explore.snow") && s.Find("explore.snow")->mode == Mode::Vanilla);
	ResetToVanilla(s, "explore");  // group: group vanilla AND sub overrides gone
	MJ_CHECK(s.Find("explore")->mode == Mode::Vanilla && s.Find("explore")->selector.empty());
	MJ_CHECK(s.Find("explore.snow") == nullptr && s.Find("explore.tundra") == nullptr);

	SetMode(s, "combat", Mode::Vanilla);  // parent vanilla: a sub reset just removes its override
	SetMode(s, "combat.civil_war", Mode::Genre);
	ResetToVanilla(s, "combat.civil_war");
	MJ_CHECK(s.Find("combat.civil_war") == nullptr);
}

MJ_TEST(Describe_SelectionText)
{
	auto s = mj::config::DefaultSettings();
	const auto rows = BuildRows();
	auto find = [&](const std::string& id) { for (const auto& r : rows) if (r.id == id) return r; return EnvRow{}; };
	MJ_CHECK(DescribeSelection(s, find("explore")) == "Vanilla");
	SetMode(s, "explore", Mode::Genre);
	s.environments["explore"].selector = "Ambient";
	MJ_CHECK(DescribeSelection(s, find("explore")) == "Genre: Ambient");
	MJ_CHECK(DescribeSelection(s, find("explore.snow")).find("Same as") == 0);
	SetMode(s, "explore.snow", Mode::Artist);
	auto& e = s.environments["explore.snow"];
	e.selector = "Sigur"; e.service = "local"; e.id = "9";
	MJ_CHECK(DescribeSelection(s, find("explore.snow")) == "Artist: Sigur (local)");
}

// ---- search hits -------------------------------------------------------------------------------------------

MJ_TEST(Hits_FilterByKindServiceAndQobuz)
{
	std::vector<MJ::App::SearchHit> hits = {
		Hit("artist", "local", "1", "A"), Hit("artist", "ytmusic", "2", "B"), Hit("artist", "qobuz", "3", "C"),
		Hit("genre", "local", "Ambient", "Ambient"), Hit("track", "local", "t", "T"), Hit("artist", "local", "", "NoId"),
	};
	auto lib = FilterHits(hits, Mode::Artist, false);
	MJ_CHECK(lib.size() == 1 && lib[0].title == "A");
	auto stream = FilterHits(hits, Mode::Artist, true);
	MJ_CHECK(stream.size() == 2);  // qobuz is never offered, even with streaming
	MJ_CHECK(FilterHits(hits, Mode::Genre, false).size() == 1);
	MJ_CHECK(FilterHits(hits, Mode::Query, true).empty());
	MJ_CHECK(FilterHits(hits, Mode::Vanilla, true).empty());
}

MJ_TEST(Hits_ApplyStoresSelector)
{
	auto s = mj::config::DefaultSettings();
	SetMode(s, "explore", Mode::Artist);
	MJ_CHECK(ApplyHit(s, "explore", Hit("artist", "local", "77", "Name")));
	auto* e = s.Find("explore");
	MJ_CHECK(e->service == "local" && e->id == "77" && e->selector == "Name");
	MJ_CHECK(!ApplyHit(s, "explore", Hit("artist", "ytmusic", "5", "Stream")));  // streaming off
	MJ_CHECK(e->id == "77");
	SetStreaming(s, "explore", true);
	MJ_CHECK(ApplyHit(s, "explore", Hit("artist", "ytmusic", "5", "Stream")));
	MJ_CHECK(e->service == "ytmusic" && e->id == "5");
	MJ_CHECK(!ApplyHit(s, "explore", Hit("artist", "qobuz", "5", "Q")));
	MJ_CHECK(!ApplyHit(s, "explore", Hit("playlist", "local", "5", "P")));  // wrong kind for the mode

	SetMode(s, "combat", Mode::Genre);
	MJ_CHECK(ApplyHit(s, "combat", Hit("genre", "local", "Epic", "Epic Music")));
	MJ_CHECK(s.Find("combat")->selector == "Epic" && s.Find("combat")->id.empty() && s.Find("combat")->service.empty());
	MJ_CHECK(!ApplyHit(s, "boss", Hit("genre", "local", "x", "x")));  // no entry
	MJ_CHECK(HitLabel(Hit("artist", "ytmusic", "1", "T", "sub")) == "T - sub [ytmusic]");
}

// ---- presets -----------------------------------------------------------------------------------------------

MJ_TEST(Presets_AllEntriesAreValidGenreEnvironments)
{
	MJ_CHECK(FindPreset("calm_epic") != nullptr);
	MJ_CHECK(FindPreset("nope") == nullptr);
	for (const auto& p : Presets()) {
		MJ_CHECK(p.label && *p.label);
		for (const auto& e : p.entries) {
			MJ_CHECK(mj::categories::IsKnownEnvironmentId(e.envId));
			MJ_CHECK(e.mode == Mode::Genre);
			MJ_CHECK(ValidateSelectorText(e.selector).ok);
		}
	}
}

MJ_TEST(Presets_ApplySetsFieldsOnlyAndDropsSubOverrides)
{
	auto s = mj::config::DefaultSettings();
	SetMode(s, "explore.snow", Mode::Artist);  // an override that would beat the group
	SetMode(s, "town", Mode::Genre);           // unrelated: must stay
	s.environments["town"].selector = "Jazz";
	SetStreaming(s, "explore", true);
	SetPoolSizeAll(s, 5);
	s.global.serverUrl = "http://h:1";
	s.global.personalKey = "k";

	MJ_CHECK(ApplyPreset(s, "calm_epic"));
	MJ_CHECK(s.Find("explore")->mode == Mode::Genre && s.Find("explore")->selector == "Ambient");
	MJ_CHECK(s.Find("combat")->mode == Mode::Genre && s.Find("combat")->selector == "Epic");
	MJ_CHECK(s.Find("boss")->selector == "Epic");
	MJ_CHECK(s.Find("explore.snow") == nullptr);
	MJ_CHECK(s.Find("explore")->allowStreaming);           // flags untouched
	MJ_CHECK(s.Find("combat")->poolSize == 5);             // new entries follow the pool size
	MJ_CHECK(s.Find("town")->selector == "Jazz");          // not in the preset
	MJ_CHECK(s.global.serverUrl == "http://h:1" && s.global.personalKey == "k");  // only environments change
	MJ_CHECK(!ApplyPreset(s, "unknown"));
}

MJ_TEST(Presets_VanillaResetsEverything)
{
	auto s = mj::config::DefaultSettings();
	ApplyPreset(s, "calm_epic");
	SetMode(s, "explore.snow", Mode::Artist);
	MJ_CHECK(ApplyPreset(s, "vanilla"));
	for (const auto& [id, e] : s.environments) {
		MJ_CHECK_MSG(e.mode == Mode::Vanilla, id);
	}
}

MJ_TEST(Presets_SurviveSanitize)
{
	// The store sanitizes every Update: a preset must not be mangled or dropped by it.
	auto s = mj::config::DefaultSettings();
	ApplyPreset(s, "folk_journey");
	mj::config::Diagnostics diag;
	mj::config::Sanitize(s, diag);
	MJ_CHECK(s.Find("explore") && s.Find("explore")->selector == "Folk");
	MJ_CHECK(s.Find("dungeon") && s.Find("dungeon")->selector == "Dark Ambient");
}

// ---- first run / status text -------------------------------------------------------------------------------

MJ_TEST(FirstRun_StepsInOrder)
{
	auto s = mj::config::DefaultSettings();
	MJ::App::Connection conn;
	auto fr = ComputeFirstRun(s, conn);
	MJ_CHECK(fr.current == 0 && !fr.step1Done);
	s.global.serverUrl = "http://h:1";
	fr = ComputeFirstRun(s, conn);
	MJ_CHECK(fr.current == 0);  // key missing
	s.global.personalKey = "k";
	fr = ComputeFirstRun(s, conn);
	MJ_CHECK(fr.step1Done && fr.current == 1);
	conn.state = MJ::App::ConnState::Failed;
	MJ_CHECK(ComputeFirstRun(s, conn).current == 1);
	conn.state = MJ::App::ConnState::Ok;
	fr = ComputeFirstRun(s, conn);
	MJ_CHECK(fr.step2Done && fr.current == 2);
	SetMode(s, "explore", Mode::Genre);
	fr = ComputeFirstRun(s, conn);
	MJ_CHECK(fr.step3Done && fr.current == 3);
	SetMode(s, "explore", Mode::Vanilla);
	MJ_CHECK(ComputeFirstRun(s, conn).current == 2);
}

MJ_TEST(Status_Formatting)
{
	MJ_CHECK(FormatPoolLine(nullptr) == "Not reported yet");
	MJ::App::EnvStatus st;
	st.id = "explore"; st.ready = 2; st.target = 3; st.state = "Filling pool (2 of 3)";
	MJ_CHECK(FormatPoolLine(&st) == "2 of 3 ready - Filling pool (2 of 3)");
	st.nowPlaying = "A - B";
	MJ_CHECK(FormatPoolLine(&st) == "2 of 3 ready - Filling pool (2 of 3) - now: A - B");
	std::vector<MJ::App::EnvStatus> all = { st };
	MJ_CHECK(FindStatus(all, "explore") == &all[0]);
	MJ_CHECK(FindStatus(all, "combat") == nullptr);

	MJ_CHECK(FormatBytes(0) == "0 B");
	MJ_CHECK(FormatBytes(1023) == "1023 B");
	MJ_CHECK(FormatBytes(1536) == "1.5 KB");
	MJ_CHECK(FormatBytes(5ull * 1024 * 1024 * 1024) == "5.0 GB");
	MJ_CHECK(FormatCacheCap(4096) == "4 GB");
	MJ_CHECK(FormatCacheCap(1500) == "1500 MB");
	MJ_CHECK(std::string(ConnStateLabel(MJ::App::ConnState::Ok)) == "Connected");
}

// ---- apply worker + store flow (real SettingsStore, stub App) ------------------------------------------------

MJ_TEST(ApplyQueue_CoalescesAndReportsErrors)
{
	std::atomic<int> runs{ 0 };
	std::atomic<bool> release{ false };
	std::atomic<bool> inJob{ false };
	std::string nextError;
	{
		ApplyQueue q([&]() -> std::string {
			inJob = true;
			while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			++runs;
			return nextError;
		});
		q.Request();
		while (!inJob) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		q.Request(); q.Request(); q.Request();  // arrive while the job runs: collapse into ONE more run
		release = true;
		q.WaitIdle();
		MJ_CHECK(runs == 2);
		MJ_CHECK(q.Completed() == 2);
		MJ_CHECK(q.LastError().empty());
	}
	{
		ApplyQueue q([&]() -> std::string { return "disk full"; });
		q.Request();
		q.WaitIdle();
		MJ_CHECK(q.LastError() == "disk full");
	}
	{
		ApplyQueue q([&]() -> std::string { throw 1; });
		q.Request();
		q.WaitIdle();
		MJ_CHECK(!q.LastError().empty());  // an exception in the job must not kill the worker
		q.Request();
		q.WaitIdle();
		MJ_CHECK(q.Completed() == 2);
	}
	{
		ApplyQueue q([&]() -> std::string { ++runs; return {}; });  // destructor with nothing pending joins cleanly
	}
}

MJ_TEST(StoreFlow_EditSaveApply)
{
	mjtest::ScratchDir dir;
	const auto ini = dir.Path() / "MarthJukebox.ini";
	mj::config::SettingsStore store(ini, dir.Path() / "cache");
	MJ_CHECK(store.Load());
	MJ::App::Stub::Reset();

	ApplyQueue q([&]() -> std::string {
		std::string err;
		const bool ok = store.Save(&err);
		MJ::App::ApplySettingsChanged();
		return ok ? std::string{} : err;
	});

	const std::string secret = "TOPSECRETKEY123";
	store.Update([&](mj::config::Settings& s) {
		s.global.serverUrl = "http://127.0.0.1:8080";
		s.global.personalKey = secret;
	});
	q.Request();
	store.Update([&](mj::config::Settings& s) { ApplyPreset(s, "calm_epic"); });
	q.Request();
	q.WaitIdle();

	MJ_CHECK(q.LastError().empty());
	MJ_CHECK(MJ::App::Stub::Rec().applied >= 1);

	mj::config::SettingsStore reloaded(ini, dir.Path() / "cache");
	MJ_CHECK(reloaded.Load());
	const auto snap = reloaded.Snapshot();
	MJ_CHECK(snap->Find("explore") && snap->Find("explore")->selector == "Ambient");
	MJ_CHECK(snap->global.serverUrl == "http://127.0.0.1:8080");
	MJ_CHECK(snap->global.personalKey == secret);
	MJ_CHECK(q.LastError().find(secret) == std::string::npos);
}

MJ_TEST(StubApp_RecordsTheSeamCalls)
{
	MJ::App::Stub::Reset();
	MJ::App::SearchAsync("genre", "amb", false);
	MJ::App::TestConnectionAsync();
	MJ::App::PeekAsync("explore");
	MJ::App::ClearCacheAsync();
	const auto& r = MJ::App::Stub::Rec();
	MJ_CHECK(r.searches.size() == 1 && r.searches[0].kind == "genre" && r.searches[0].text == "amb" && !r.searches[0].streaming);
	MJ_CHECK(r.testConnection == 1 && r.peeks.size() == 1 && r.clearCache == 1);
}
