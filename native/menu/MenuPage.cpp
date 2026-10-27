// The settings page: ImGui drawing + registration with the SKSE Menu Framework. All decisions live in ViewModel.cpp
// (unit tested); this file only draws and forwards. Rules (docs/MENU-FRAMEWORK.md section 4):
//  - Render() runs on the engine's UI thread once per frame while the panel is open: it never blocks, never touches the
//    disk or the network. It reads cached App state (refreshed at most 4x a second) and the settings snapshot.
//  - A change is: SettingsStore::Update (memory, instant) -> ApplyQueue (worker: Save, then App::ApplySettingsChanged).
//  - Text fields commit on Enter/blur (and when the panel closes), searches run 400 ms after typing stops.
//  - The personal key is masked (ImGui Password flag) unless the user ticks "Show"; it is never logged or formatted.
#include "PCH.h"

#include "menu/Menu.h"

#include "app/AppApi.h"
#include "config/Settings.h"
#include "menu/ViewModel.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>

// The vendored framework header (LGPL-2.1, see menu/third_party/README.md). It uses std::wstring_convert (deprecated).
#ifndef _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#	define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#endif
#pragma warning(push)
#pragma warning(disable : 4996 4100 4189 4244 4267 4458 4505)
#include "menu/third_party/SKSEMenuFramework.h"
#pragma warning(pop)

namespace MJ::Menu
{
	namespace
	{
		namespace UI = ImGuiMCP;
		using mj::config::SettingsStore;

		// ---- process state ------------------------------------------------------------------------------------
		std::atomic<SettingsStore*> g_store{ nullptr };
		std::atomic<ApplyQueue*>    g_queue{ nullptr };  // created in SetStore, intentionally never destroyed (no join at process exit)
		std::mutex                  g_pageMutex;         // Render vs the close-menu event

		std::uint64_t NowMs()
		{
			using namespace std::chrono;
			return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
		}

		struct PageState
		{
			std::vector<EnvRow> rows = BuildRows();

			TextField url;
			TextField key;
			bool      showKey = false;

			std::map<std::string, TextField>       queryText;   // Query mode text, per environment
			std::string                            searchEnv;   // which row owns the shared search results
			Debouncer                              searchDebounce;
			std::map<std::string, bool>            peekWanted;
			std::map<std::string, std::vector<std::string>> peekCache;

			ConfirmGate clearCacheGate;
			ConfirmGate vanillaGate;

			// library genre list (a browse search), used to tell which preset genres are missing
			std::vector<App::SearchHit> genreList;
			bool          genreWanted   = false;  // a genre browse was requested and its result is awaited
			bool          genreAsked    = false;  // asked once automatically after the first successful connection
			std::uint64_t genreSeq      = 0;

			// filtered suggestions, rebuilt only when the inputs change (not every frame)
			std::string                 hitsKey;
			std::vector<App::SearchHit> hits;
			std::string note;  // one-line feedback ("Preset applied: ...")

			// cached App state (refreshed every 250 ms)
			std::uint64_t          lastRefreshMs = 0;
			App::Connection        conn;
			std::vector<App::EnvStatus> status;
			App::SearchState       search;
			std::uint64_t          cacheBytes = 0;
		};

		PageState& S()
		{
			static PageState s;
			return s;
		}

		const UI::ImVec4 kGood{ 0.45f, 0.90f, 0.45f, 1.0f };
		const UI::ImVec4 kWarn{ 1.00f, 0.80f, 0.30f, 1.0f };
		const UI::ImVec4 kBad{ 1.00f, 0.45f, 0.45f, 1.0f };
		const UI::ImVec4 kDim{ 0.70f, 0.70f, 0.70f, 1.0f };

		void Line(const std::string& a_text) { UI::Text("%s", a_text.c_str()); }
		void Wrapped(const std::string& a_text) { UI::TextWrapped("%s", a_text.c_str()); }
		void Colored(const UI::ImVec4& a_col, const std::string& a_text) { UI::TextColored(a_col, "%s", a_text.c_str()); }
		void Tooltip(const char* a_text)
		{
			if (UI::IsItemHovered(UI::ImGuiHoveredFlags_ForTooltip)) {
				UI::SetItemTooltip("%s", a_text);
			}
		}

		// One text box over a std::string (the buffer is wiped after use: it may hold the key).
		bool EditText(const char* a_label, const char* a_hint, std::string& a_value, std::size_t a_maxBytes, int a_flags = 0)
		{
			char buf[1025];
			const std::size_t cap = std::min<std::size_t>(a_maxBytes, sizeof(buf) - 1);
			const std::size_t n = std::min(a_value.size(), cap);
			std::memcpy(buf, a_value.data(), n);
			buf[n] = '\0';
			const bool changed = UI::InputTextWithHint(a_label, a_hint, buf, cap + 1, a_flags);
			if (changed) {
				a_value.assign(buf);
			}
			SecureZeroMemory(buf, sizeof(buf));
			return changed;
		}

		// ---- store access -------------------------------------------------------------------------------------
		template <class F>
		void Change(F&& a_edit)
		{
			auto* store = g_store.load();
			if (!store) {
				return;
			}
			store->Update(std::function<void(Settings&)>(std::forward<F>(a_edit)));
			if (auto* q = g_queue.load()) {
				q->Request();
			}
		}

		// Commits every dirty text field (Enter, blur, panel closed, before Test connection).
		void CommitTextFields()
		{
			auto& s = S();
			const bool urlDirty = s.url.Dirty();
			const bool keyDirty = s.key.Dirty();
			if (urlDirty || keyDirty) {
				const std::string u = s.url.edit;
				const std::string k = s.key.edit;
				s.url.Commit();
				s.key.Commit();
				Change([&](Settings& st) {
					if (urlDirty) st.global.serverUrl = u;
					if (keyDirty) st.global.personalKey = k;
				});
			}
			for (auto& [envId, field] : s.queryText) {
				if (field.Commit()) {
					const std::string id = envId;
					const std::string text = field.edit;
					Change([&](Settings& st) { SetSelectorText(st, id, text); });
				}
			}
		}

		void StartSearch(const std::string& a_envId, Mode a_mode, const std::string& a_text, bool a_streaming)
		{
			const auto kind = SearchKindFor(a_mode);
			if (kind.empty()) {
				return;
			}
			auto& s = S();
			s.searchEnv = a_envId;
			s.searchDebounce.Cancel();
			App::SearchAsync(std::string(kind), a_text, a_streaming);
		}

		// Library genres for the preset check. Shares the core's single search slot, so it is only asked for when the user
		// presses the button or once after a successful connection test.
		void RequestGenreList()
		{
			auto& s = S();
			s.genreWanted = true;
			s.genreAsked = true;
			s.genreSeq = s.search.seq;
			s.searchEnv.clear();
			s.searchDebounce.Cancel();
			App::SearchAsync("genre", "", false);
		}

		void RefreshCaches(std::uint64_t a_now, bool a_force)
		{
			auto& s = S();
			if (!a_force && a_now - s.lastRefreshMs < 250) {
				return;
			}
			s.lastRefreshMs = a_now;
			s.conn = App::GetConnection();
			s.status = App::GetEnvStatus();
			s.search = App::GetSearch();
			if (s.genreWanted && !s.search.busy && s.search.seq != s.genreSeq) {
				s.genreWanted = false;
				s.genreList.clear();
				for (const auto& h : s.search.hits) {
					if (h.kind == "genre") {
						s.genreList.push_back(h);
					}
				}
			}
			s.cacheBytes = App::CacheBytes();
			for (auto& [id, wanted] : s.peekWanted) {
				if (wanted) {
					s.peekCache[id] = App::GetPeek(id);
				}
			}
		}

		// ---- drawing: connection / first run ----------------------------------------------------------------
		void DrawSteps(const FirstRun& a_fr)
		{
			const char* names[3] = { "1. Enter the server address and your key",
									 "2. Test the connection",
									 "3. Pick a preset or choose music per environment" };
			const bool done[3] = { a_fr.step1Done, a_fr.step2Done, a_fr.step3Done };
			for (int i = 0; i < 3; ++i) {
				const bool current = a_fr.current == i;
				Colored(done[i] ? kGood : (current ? kWarn : kDim),
					std::string(done[i] ? "[done] " : (current ? "[next] " : "[    ] ")) + names[i]);
			}
		}

		void DrawConnection(const Settings& a_set)
		{
			auto& s = S();
			s.url.SyncFromStore(a_set.global.serverUrl);
			s.key.SyncFromStore(a_set.global.personalKey);

			UI::SetNextItemWidth(420.0f);
			EditText("Harmony server address##url", "http://host:8080", s.url.edit, 512);  // committed on Enter/blur
			if (UI::IsItemDeactivatedAfterEdit()) {
				CommitTextFields();
			}
			const auto urlCheck = ValidateServerUrl(s.url.edit);
			if (!urlCheck.message.empty()) {
				Colored(urlCheck.ok ? kDim : kBad, urlCheck.message);
			}

			UI::SetNextItemWidth(420.0f);
			EditText("Personal key##key", "paste your key", s.key.edit, 256, s.showKey ? 0 : UI::ImGuiInputTextFlags_Password);
			if (UI::IsItemDeactivatedAfterEdit()) {
				CommitTextFields();
			}
			UI::SameLine();
			UI::Checkbox("Show##showkey", &s.showKey);
			const auto keyCheck = ValidatePersonalKey(s.key.edit);
			if (!keyCheck.message.empty()) {
				Colored(keyCheck.ok ? kDim : kBad, keyCheck.message);
			}

			const bool canTest = urlCheck.ok && !s.url.edit.empty() && keyCheck.ok && s.conn.state != App::ConnState::Testing;
			UI::BeginDisabled(!canTest);
			if (UI::Button("Test connection")) {
				CommitTextFields();
				App::TestConnectionAsync();
				RefreshCaches(NowMs(), true);
			}
			UI::EndDisabled();
			UI::SameLine();
			const auto& c = s.conn;
			const UI::ImVec4& col = c.state == App::ConnState::Ok ? kGood : c.state == App::ConnState::Failed ? kBad
				: c.state == App::ConnState::Testing ? kWarn : kDim;
			Colored(col, ConnStateLabel(c.state));
			if (!c.message.empty()) {
				Wrapped(c.message);
			}
		}

		void DrawPresets()
		{
			auto& s = S();
			UI::SeparatorText("Presets");
			Wrapped("A preset only sets the music source of some environments. You can change anything afterwards.");
			if (s.conn.state == App::ConnState::Ok && !s.genreAsked) {
				RequestGenreList();
			}
			for (const auto& p : Presets()) {
				const bool isVanilla = std::string_view(p.id) == "vanilla";
				const bool armed = isVanilla && s.vanillaGate.Armed(NowMs());
				const std::string label = armed ? std::string("Click again to put every environment back to vanilla") : std::string(p.label);
				bool apply = false;
				if (UI::Button((label + "##preset_" + p.id).c_str())) {
					apply = isVanilla ? s.vanillaGate.Press(NowMs()) : true;
				}
				Tooltip(p.description);
				if (apply) {
					const std::string id = p.id;
					Change([&](Settings& st) { ApplyPreset(st, id); });
					s.searchEnv.clear();
					s.queryText.clear();
					s.note = std::string("Applied preset: ") + p.label;
				}
				if (const auto missing = MissingPresetGenres(p, s.genreList); !missing.empty()) {
					std::string m = "Not in your library: ";
					for (std::size_t k = 0; k < missing.size(); ++k) {
						m += (k ? ", " : "") + missing[k];
					}
					Colored(kWarn, m + ". Those environments will have no tracks until you pick another genre.");
				}
			}
			if (UI::Button("Check preset genres against the server")) {
				RequestGenreList();
			}
			Tooltip("Asks the server for its genre list so the presets can tell you which genres your library lacks.");
			if (!s.note.empty()) {
				Colored(kGood, s.note);
			}
		}

		// ---- drawing: one environment ---------------------------------------------------------------------
		void DrawEnvironment(const EnvRow& a_row, const Settings& a_set)
		{
			auto& s = S();
			UI::PushID(a_row.id.c_str());

			const EnvSetting* own = a_set.Find(a_row.id);
			const EnvSetting* shown = own;
			if (!shown && a_row.isSub) {
				shown = a_set.Find(a_row.parentId);  // what the sub inherits (flags are displayed from it)
			}
			const Mode curMode = own ? own->mode : Mode::Vanilla;
			const bool streaming = shown ? shown->allowStreaming : false;

			// Music source combo.
			const auto choices = ModeChoices(a_row.isSub);
			const int cur = ModeChoiceIndex(choices, own);
			UI::SetNextItemWidth(260.0f);
			if (UI::BeginCombo("Music source", choices[static_cast<std::size_t>(cur)].label)) {
				for (std::size_t i = 0; i < choices.size(); ++i) {
					if (UI::Selectable(choices[i].label, static_cast<int>(i) == cur) && static_cast<int>(i) != cur) {
						const ModeChoice ch = choices[i];
						const std::string id = a_row.id;
						Change([&](Settings& st) { SetMode(st, id, ch.mode, ch.inherit); });
						s.queryText.erase(id);
						s.searchEnv.clear();
						if (ModeNeedsSearch(ch.mode) && !ch.inherit) {
							StartSearch(id, ch.mode, "", streaming);  // browse list: reachable without a keyboard
						}
					}
				}
				UI::EndCombo();
			}

			Line("Selected: " + DescribeSelection(a_set, a_row));

			if (curMode != Mode::Vanilla) {
				// The selector is ALWAYS a plain typed text field (marth 2026-10-27): genre, artist or playlist name, or search words.
				auto& field = s.queryText[a_row.id];
				field.SyncFromStore(own ? own->selector : std::string{});
				const char* hint = curMode == Mode::Genre ? "type a genre, e.g. ambient"
					: curMode == Mode::Artist ? "type an artist name, e.g. Hans Zimmer"
					: curMode == Mode::Playlist ? "type a playlist name" : "e.g. epic battle music";
				UI::SetNextItemWidth(420.0f);
				if (EditText("Text##selector", hint, field.edit, 800) && ModeNeedsSearch(curMode)) {
					s.searchEnv = a_row.id;
					s.searchDebounce.Touch(NowMs());  // suggestions follow the typing (400 ms after it stops)
				}
				if (UI::IsItemDeactivatedAfterEdit()) {
					CommitTextFields();
				}
				const auto chk = ValidateSelectorText(field.edit);
				if (!chk.ok) {
					Colored(kBad, chk.message);
				}
				if (own && NeedsLink(*own)) {
					Colored(kWarn, "Not linked to a specific " + std::string(SearchKindFor(curMode)) +
						" yet: choose one of the suggestions below so the server knows which one you mean.");
				}

				if (ModeNeedsSearch(curMode)) {
					if (UI::Button("Show suggestions")) {
						StartSearch(a_row.id, curMode, field.edit, streaming);
					}
					Tooltip("Lists matches from the server (also the way to pick without a keyboard).");
					if (s.searchEnv == a_row.id) {
						if (s.search.busy) {
							Colored(kWarn, "Searching...");
						}
						if (!s.search.error.empty()) {
							Colored(kBad, s.search.error);
						}
						const std::string key = std::to_string(s.search.seq) + "|" + a_row.id + "|" +
							std::to_string(static_cast<int>(curMode)) + (streaming ? "|s" : "|l");
						if (key != s.hitsKey) {
							s.hitsKey = key;
							s.hits = FilterHits(s.search.hits, curMode, streaming);  // capped at kMaxSuggestions
						}
						const auto& hits = s.hits;
						UI::TextColored(kDim, "%s", "Suggestions (optional)");
						if (UI::BeginListBox("##results", UI::ImVec2{ 0.0f, 170.0f })) {
							for (std::size_t i = 0; i < hits.size(); ++i) {
								const bool picked = own && !hits[i].id.empty() && own->id == hits[i].id && own->service == hits[i].service;
								const std::string label = HitLabel(hits[i]) + "##hit" + std::to_string(i);
								if (UI::Selectable(label.c_str(), picked)) {
									const std::string id = a_row.id;
									const App::SearchHit hit = hits[i];
									Change([&](Settings& st) { ApplyHit(st, id, hit); });
									s.queryText.erase(id);  // the text field re-reads the stored value
								}
							}
							if (hits.empty() && !s.search.busy) {
								UI::TextColored(kDim, "%s", "No suggestions yet.");
							}
							UI::EndListBox();
						}
					}
				}
			}

			// Flags.
			bool stream = streaming;
			UI::BeginDisabled(!s.conn.youtubeUsable && !stream);
			if (UI::Checkbox("Allow streaming (YouTube Music)##stream", &stream)) {
				const std::string id = a_row.id;
				const bool v = stream;
				Change([&](Settings& st) { SetStreaming(st, id, v); });
				if (s.searchEnv == a_row.id) {
					s.searchDebounce.Touch(NowMs());
				}
			}
			UI::EndDisabled();
			if (!s.conn.youtubeUsable && !stream) {
				Tooltip("Needs a Harmony server with YouTube Music set up. Off by default.");
			}
			bool remember = shown ? shown->rememberPlace : false;
			if (UI::Checkbox("Remember track place##remember", &remember)) {
				const std::string id = a_row.id;
				const bool v = remember;
				Change([&](Settings& st) { SetRememberPlace(st, id, v); });
			}

			// Status + preview.
			const auto* st = FindStatus(s.status, a_row.id);
			Colored(kDim, "Pool: " + FormatPoolLine(st));
			if (UI::Button("Show next up")) {
				s.peekWanted[a_row.id] = true;
				App::PeekAsync(a_row.id);
			}
			Tooltip("Titles the server would play next. No audio is played or downloaded.");
			UI::SameLine();
			if (UI::Button("Reset to vanilla")) {
				const std::string id = a_row.id;
				Change([&](Settings& stt) { ResetToVanilla(stt, id); });
				s.queryText.erase(id);
				if (s.searchEnv == id) {
					s.searchEnv.clear();
				}
			}
			if (const auto it = s.peekCache.find(a_row.id); it != s.peekCache.end() && s.peekWanted[a_row.id]) {
				if (it->second.empty()) {
					Colored(kDim, "No preview yet.");
				}
				for (const auto& t : it->second) {
					Line("  next: " + t);
				}
			}

			UI::PopID();
		}

		void DrawEnvironments(const Settings& a_set)
		{
			auto& s = S();
			UI::SeparatorText("Music per environment");
			Wrapped("Choose where each kind of music comes from. Anything left on Vanilla keeps the original Skyrim music.");
			for (std::size_t i = 0; i < s.rows.size(); ++i) {
				const auto& group = s.rows[i];
				if (group.isSub) {
					continue;
				}
				const std::string header = group.label + "  -  " + DescribeSelection(a_set, group) + "###grp_" + group.id;
				if (UI::CollapsingHeader(header.c_str())) {
					UI::PushID(("grp_" + group.id).c_str());
					Colored(kDim, group.summary);
					DrawEnvironment(group, a_set);
					bool hasSubs = i + 1 < s.rows.size() && s.rows[i + 1].isSub;
					if (hasSubs && UI::TreeNode("Individual places in this group")) {
						for (std::size_t j = i + 1; j < s.rows.size() && s.rows[j].isSub; ++j) {
							const auto& sub = s.rows[j];
							const std::string subHeader = sub.label + "  -  " + DescribeSelection(a_set, sub) + "###sub_" + sub.id;
							if (UI::TreeNode(subHeader.c_str())) {
								DrawEnvironment(sub, a_set);
								UI::TreePop();
							}
						}
						UI::TreePop();
					}
					UI::PopID();
				}
			}
		}

		// ---- drawing: global / advanced ---------------------------------------------------------------------
		void DrawGlobal(const Settings& a_set)
		{
			auto& s = S();
			UI::SeparatorText("Pool and cache");

			const int poolNow = CurrentPoolSize(a_set);
			UI::SetNextItemWidth(120.0f);
			if (UI::BeginCombo("Tracks kept ready per environment", std::to_string(poolNow).c_str())) {
				for (int size : mj::config::kPoolSizes) {
					if (UI::Selectable(std::to_string(size).c_str(), size == poolNow) && size != poolNow) {
						Change([&](Settings& st) { SetPoolSizeAll(st, size); });
					}
				}
				UI::EndCombo();
			}

			Line("Cache on disk: " + FormatBytes(s.cacheBytes));
			const int capNow = a_set.global.cacheCapMB;
			UI::SetNextItemWidth(160.0f);
			if (UI::BeginCombo("Cache size limit", FormatCacheCap(capNow).c_str())) {
				for (int mb : CacheCapChoicesMB()) {
					if (UI::Selectable(FormatCacheCap(mb).c_str(), mb == capNow) && mb != capNow) {
						Change([&](Settings& st) { st.global.cacheCapMB = mb; });
					}
				}
				UI::EndCombo();
			}
			const bool armed = s.clearCacheGate.Armed(NowMs());
			if (UI::Button(armed ? "Click again to delete the cached music" : "Clear cache")) {
				if (s.clearCacheGate.Press(NowMs())) {
					App::ClearCacheAsync();
				}
			}
			Tooltip("Deletes downloaded tracks that are not in use. They are fetched again when needed.");
		}

		void DrawAdvanced(const Settings& a_set)
		{
			UI::SeparatorText("Advanced");
			Wrapped("Kill switches. Turn a part off to find out what causes a problem. Changes apply right away.");
			auto toggle = [&](const char* a_label, bool a_value, bool mj::config::Features::*a_member) {
				bool v = a_value;
				if (UI::Checkbox(a_label, &v)) {
					Change([&](Settings& st) { st.global.features.*a_member = v; });
				}
			};
			const auto& f = a_set.global.features;
			toggle("Replace Skyrim's music (Retarget)", f.retarget, &mj::config::Features::retarget);
			toggle("Use the network (Harmony)", f.network, &mj::config::Features::network);
			toggle("Decode FLAC (smaller downloads)", f.flac, &mj::config::Features::flac);
			toggle("Resume tracks where they stopped", f.rememberPlace, &mj::config::Features::rememberPlace);

			const auto level = a_set.global.logLevel;
			UI::SetNextItemWidth(160.0f);
			if (UI::BeginCombo("Log level", std::string(mj::config::ToString(level)).c_str())) {
				for (auto l : LogLevelChoices()) {
					if (UI::Selectable(std::string(mj::config::ToString(l)).c_str(), l == level) && l != level) {
						Change([&](Settings& st) { st.global.logLevel = l; });
					}
				}
				UI::EndCombo();
			}
			Colored(kDim, "The log is MarthJukebox.log next to the plugin. The page itself is switched off with [Features] Menu=0 in the INI.");
		}

		// ---- the registered callbacks ---------------------------------------------------------------------------
		void __stdcall Render()
		{
			std::lock_guard lk(g_pageMutex);
			auto* store = g_store.load();
			if (!store) {
				UI::TextWrapped("%s", "Settings are not loaded yet. Wait until the game has finished starting.");
				return;
			}
			auto& s = S();
			const auto now = NowMs();

			const auto snap = store->Snapshot();
			const Settings& set = *snap;
			if (!set.global.features.menu) {
				// [Features] Menu=0: the INI switch turns the page off (it stays registered, it just draws this).
				UI::TextWrapped("%s", "The settings page is switched off in MarthJukebox.ini ([Features] Menu=0). Set it to 1 and restart to use it.");
				return;
			}
			RefreshCaches(now, s.lastRefreshMs == 0);

			// Search debounce: fires once, 400 ms after the last keystroke.
			if (s.searchDebounce.Poll(now, kSearchDebounceMs) && !s.searchEnv.empty()) {
				const auto* e = set.Find(s.searchEnv);
				if (e && ModeNeedsSearch(e->mode)) {
					StartSearch(s.searchEnv, e->mode, s.queryText[s.searchEnv].edit, e->allowStreaming);
				}
			}

			Wrapped("Controller: text boxes need a keyboard. Everything else works with a controller: pick a source, press "
					"Show suggestions, and choose from the list.");

			if (const auto* q = g_queue.load()) {
				if (const auto err = q->LastError(); !err.empty()) {
					Colored(kBad, "Could not save the settings: " + err);
				}
			}

			const auto fr = ComputeFirstRun(set, s.conn, &s.status);
			if (fr.current < 3) {
				UI::SeparatorText("First-time setup");
				DrawSteps(fr);
			}
			UI::SeparatorText("Harmony server");
			DrawConnection(set);
			DrawPresets();
			DrawEnvironments(set);
			DrawGlobal(set);
			DrawAdvanced(set);
		}

		void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType a_type)
		{
			if (a_type == SKSEMenuFramework::Model::kCloseMenu) {
				std::lock_guard lk(g_pageMutex);
				S().showKey = false;  // the key is masked again the next time the panel opens
				if (g_store.load()) {
					CommitTextFields();
				}
			}
		}
	}

	void SetStore(SettingsStore* a_store)
	{
		// The queue first, THEN the store: once the page can see a store, edits may Request() immediately.
		if (a_store && !g_queue.load()) {
			// Save + apply on a worker. Deliberately leaked: joining a thread during process teardown can deadlock.
			g_queue.store(new ApplyQueue([]() -> std::string {
				auto* st = g_store.load();
				if (!st) {
					return {};
				}
				std::string err;
				const bool saved = st->Save(&err);
				if (!saved) {
					spdlog::error("[menu] could not save MarthJukebox.ini: {}", err);
				}
				App::ApplySettingsChanged();
				if (saved) {
					return {};
				}
				return err.empty() ? std::string("the settings file could not be written") : err;
			}));
		}
		g_store.store(a_store);
	}

	void Register()
	{
		// Module check, not the header's IsInstalled(): that one tests a path relative to the working directory.
		if (!GetMenuFrameworkModule()) {
			spdlog::warn("[menu] SKSE Menu Framework is not loaded (module SKSEMenuFramework not found): settings page not registered, "
						 "the mod runs on MarthJukebox.ini");
			return;
		}
		// Export gate. Every header wrapper calls GetProcAddress and then calls the result WITHOUT a null check, so a
		// framework version that lacks one of the exports we draw with would crash the game the first time the page opens.
		// Refuse to register, loudly, naming the export.
		static constexpr const char* kRequiredExports[] = {
			"GetMenuFrameworkVersion", "AddSectionItem", "RegisterEventPriority",
			"igButton", "igCheckbox", "igInputTextWithHint", "igBeginCombo", "igEndCombo", "igSelectable_Bool",
			"igBeginListBox", "igEndListBox", "igTextV", "igTextColoredV", "igTextWrappedV", "igSeparatorText",
			"igCollapsingHeader_TreeNodeFlags", "igTreeNode_Str", "igTreePop", "igSameLine", "igPushID_Str", "igPopID",
			"igSetNextItemWidth", "igBeginDisabled", "igEndDisabled", "igIsItemDeactivatedAfterEdit", "igIsItemHovered",
			"igSetItemTooltipV",
		};
		for (const char* name : kRequiredExports) {
			if (!GetMenuFrameworkFunction<void*>(name)) {
				spdlog::error("[menu] SKSE Menu Framework lacks the export '{}' that the settings page needs (framework too old or "
							  "changed): settings page NOT registered; the mod runs on MarthJukebox.ini", name);
				return;
			}
		}
		const auto versionFn = GetMenuFrameworkFunction<float (*)()>("GetMenuFrameworkVersion");
		spdlog::info("[menu] SKSE Menu Framework version {}", versionFn());
		SKSEMenuFramework::SetSection("marth's Jukebox");
		SKSEMenuFramework::AddSectionItem("Settings", Render);
		SKSEMenuFramework::AddEvent(OnMenuEvent, 0.0f);
		spdlog::info("[menu] settings page registered (marth's Jukebox / Settings)");
	}
}
