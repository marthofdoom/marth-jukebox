#include "menu/ViewModel.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace MJ::Menu
{
	namespace
	{
		constexpr std::size_t kMaxUrlChars      = 512;  // Settings.cpp Sanitize
		constexpr std::size_t kMaxKeyChars      = 256;  // Settings.cpp Sanitize
		constexpr std::size_t kMaxSelectorChars = 200;  // Harmony (ProfileBuilder.h kMaxSelectorTextChars)

		bool HasControl(std::string_view s)
		{
			return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
		}

		bool HasSpace(std::string_view s)
		{
			return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c == ' ' || c == '\t'; });
		}

		bool StartsWithNoCase(std::string_view s, std::string_view prefix)
		{
			return s.size() >= prefix.size() && mj::categories::EqualsNoCase(s.substr(0, prefix.size()), prefix);
		}

		const char* ModeLabel(Mode m)
		{
			switch (m) {
			case Mode::Vanilla:  return "Vanilla (leave alone)";
			case Mode::Genre:    return "Genre";
			case Mode::Artist:   return "Artist";
			case Mode::Playlist: return "Playlist";
			case Mode::Query:    return "Search text";
			}
			return "Vanilla (leave alone)";
		}

		std::string ParentOf(const std::string& a_envId)
		{
			const auto dot = a_envId.find('.');
			return dot == std::string::npos ? std::string{} : a_envId.substr(0, dot);
		}

		void ClearSelection(EnvSetting& e)
		{
			e.selector.clear();
			e.service.clear();
			e.id.clear();
		}

		std::string GroupLabel(std::string_view a_groupId)
		{
			if (const auto* g = mj::categories::FindGroup(a_groupId)) {
				return std::string(g->displayName);
			}
			return std::string(a_groupId);
		}

		std::string DescribeEntry(const EnvSetting& e)
		{
			switch (e.mode) {
			case Mode::Vanilla:
				return "Vanilla";
			case Mode::Genre:
				return "Genre: " + (e.selector.empty() ? std::string("(none picked)") : e.selector);
			case Mode::Artist:
			case Mode::Playlist: {
				std::string s = e.mode == Mode::Artist ? "Artist: " : "Playlist: ";
				s += e.selector.empty() ? std::string("(none picked)") : e.selector;
				if (!e.service.empty()) {
					s += " (" + e.service + ")";
				}
				return s;
			}
			case Mode::Query:
				return "Search text: " + (e.selector.empty() ? std::string("(empty)") : e.selector);
			}
			return "Vanilla";
		}
	}

	// ---- validation ---------------------------------------------------------------------------------------

	std::size_t CountCodePoints(std::string_view a_utf8)
	{
		std::size_t n = 0;
		for (unsigned char c : a_utf8) {
			if ((c & 0xC0) != 0x80) {
				++n;
			}
		}
		return n;
	}

	FieldCheck ValidateServerUrl(std::string_view a_url)
	{
		if (a_url.empty()) {
			return { true, "No server set: the mod stays vanilla and does not touch the network." };
		}
		if (a_url.size() > kMaxUrlChars) {
			return { false, "The server address is too long (512 characters at most)." };
		}
		if (HasControl(a_url) || HasSpace(a_url)) {
			return { false, "The server address must not contain spaces." };
		}
		const bool https = StartsWithNoCase(a_url, "https://");
		if (!https && !StartsWithNoCase(a_url, "http://")) {
			return { false, "The server address must start with http:// or https://" };
		}
		if (a_url.size() <= (https ? 8u : 7u)) {
			return { false, "The server address needs a host name after http:// or https://" };
		}
		return {};
	}

	FieldCheck ValidatePersonalKey(std::string_view a_key)
	{
		if (a_key.empty()) {
			return { true, "No key set." };
		}
		if (a_key.size() > kMaxKeyChars) {
			return { false, "The key is too long (256 characters at most)." };
		}
		if (HasControl(a_key) || HasSpace(a_key)) {
			return { false, "The key must not contain spaces. Paste it again." };
		}
		return {};
	}

	FieldCheck ValidateSelectorText(std::string_view a_text)
	{
		if (HasControl(a_text)) {
			return { false, "The text must not contain control characters." };
		}
		if (CountCodePoints(a_text) > kMaxSelectorChars) {
			return { false, "The text is too long (200 characters at most)." };
		}
		return {};
	}

	// ---- modes --------------------------------------------------------------------------------------------

	std::vector<ModeChoice> ModeChoices(bool a_isSub)
	{
		std::vector<ModeChoice> v;
		if (a_isSub) {
			v.push_back({ true, Mode::Vanilla, "Same as the parent" });
		}
		for (Mode m : { Mode::Vanilla, Mode::Genre, Mode::Artist, Mode::Playlist, Mode::Query }) {
			v.push_back({ false, m, ModeLabel(m) });
		}
		return v;
	}

	int ModeChoiceIndex(const std::vector<ModeChoice>& a_choices, const EnvSetting* a_setting)
	{
		for (std::size_t i = 0; i < a_choices.size(); ++i) {
			if (a_setting == nullptr) {
				// No entry: a sub inherits (the first choice if it is the inherit one), a group is Vanilla.
				if (a_choices[i].inherit || (!a_choices.front().inherit && a_choices[i].mode == Mode::Vanilla)) {
					return static_cast<int>(i);
				}
			} else if (!a_choices[i].inherit && a_choices[i].mode == a_setting->mode) {
				return static_cast<int>(i);
			}
		}
		return 0;
	}

	std::string_view SearchKindFor(Mode a_mode)
	{
		switch (a_mode) {
		case Mode::Genre:    return "genre";
		case Mode::Artist:   return "artist";
		case Mode::Playlist: return "playlist";
		default:             return {};
		}
	}

	bool ModeNeedsSearch(Mode a_mode)
	{
		return !SearchKindFor(a_mode).empty();
	}

	// ---- rows ---------------------------------------------------------------------------------------------

	std::vector<EnvRow> BuildRows()
	{
		std::vector<EnvRow> rows;
		for (const auto& g : mj::categories::ListGroups()) {
			if (g.kind != mj::categories::GroupKind::Playable) {
				continue;
			}
			EnvRow group;
			group.id = mj::categories::EnvironmentId(g.id, "");
			group.label = std::string(g.displayName);
			group.summary = std::string(g.summary);
			rows.push_back(group);
			for (const auto& s : g.subs) {
				EnvRow sub;
				sub.id = mj::categories::EnvironmentId(g.id, s.id);
				sub.label = std::string(s.displayName);
				sub.parentId = group.id;
				sub.isSub = true;
				rows.push_back(std::move(sub));
			}
		}
		return rows;
	}

	std::string DescribeSelection(const Settings& a_settings, const EnvRow& a_row)
	{
		if (const auto* e = a_settings.Find(a_row.id)) {
			return DescribeEntry(*e);
		}
		if (a_row.isSub) {
			const auto* p = a_settings.Find(a_row.parentId);
			if (p == nullptr || p->mode == Mode::Vanilla) {
				return "Vanilla (same as " + GroupLabel(a_row.parentId) + ")";
			}
			return "Same as " + GroupLabel(a_row.parentId) + ": " + DescribeEntry(*p);
		}
		return "Vanilla";
	}

	// ---- edits --------------------------------------------------------------------------------------------

	int CurrentPoolSize(const Settings& a_settings)
	{
		for (const auto& [id, e] : a_settings.environments) {
			if (e.mode != Mode::Vanilla) {
				return e.poolSize;
			}
		}
		return a_settings.environments.empty() ? 3 : a_settings.environments.begin()->second.poolSize;
	}

	EnvSetting& EnsureEntry(Settings& a_settings, const std::string& a_envId)
	{
		auto it = a_settings.environments.find(a_envId);
		if (it != a_settings.environments.end()) {
			return it->second;
		}
		EnvSetting fresh;
		fresh.poolSize = CurrentPoolSize(a_settings);
		if (const auto parent = ParentOf(a_envId); !parent.empty()) {
			if (const auto* p = a_settings.Find(parent)) {
				fresh = *p;  // an INHERITING sub must keep resolving to the parent's music (mode and selection included)
			}
		}
		return a_settings.environments.emplace(a_envId, std::move(fresh)).first->second;
	}

	void SetMode(Settings& a_settings, const std::string& a_envId, Mode a_mode, bool a_inherit)
	{
		if (a_inherit) {
			a_settings.environments.erase(a_envId);
			return;
		}
		auto& e = EnsureEntry(a_settings, a_envId);
		if (e.mode != a_mode) {
			ClearSelection(e);
		}
		e.mode = a_mode;
	}

	void SetStreaming(Settings& a_settings, const std::string& a_envId, bool a_value)
	{
		EnsureEntry(a_settings, a_envId).allowStreaming = a_value;
	}

	void SetRememberPlace(Settings& a_settings, const std::string& a_envId, bool a_value)
	{
		EnsureEntry(a_settings, a_envId).rememberPlace = a_value;
	}

	bool SetSelectorText(Settings& a_settings, const std::string& a_envId, const std::string& a_text)
	{
		const auto it = a_settings.environments.find(a_envId);
		if (it == a_settings.environments.end() || it->second.mode == Mode::Vanilla) {
			return false;
		}
		if (!ValidateSelectorText(a_text).ok) {
			return false;
		}
		auto& e = it->second;
		const bool linked = e.mode == Mode::Artist || e.mode == Mode::Playlist;
		if (linked && a_text == e.selector) {
			return true;  // same name as the picked suggestion: keep its service + id
		}
		e.selector = a_text;
		e.service.clear();  // typed text no longer refers to the picked artist/playlist
		e.id.clear();
		return true;
	}

	bool NeedsLink(const EnvSetting& a_setting)
	{
		return (a_setting.mode == Mode::Artist || a_setting.mode == Mode::Playlist) && a_setting.id.empty();
	}

	void SetPoolSizeAll(Settings& a_settings, int a_size)
	{
		if (std::find(std::begin(mj::config::kPoolSizes), std::end(mj::config::kPoolSizes), a_size) == std::end(mj::config::kPoolSizes)) {
			return;
		}
		if (a_settings.environments.empty()) {
			// Fresh setup: no entry carries the size yet. A Vanilla stub (equivalent to no entry for the profile and the
			// retargeter) persists it in the INI, and CurrentPoolSize / EnsureEntry hand it to every later entry.
			EnsureEntry(a_settings, "explore");
		}
		for (auto& [id, e] : a_settings.environments) {
			e.poolSize = a_size;
		}
	}

	void ResetToVanilla(Settings& a_settings, const std::string& a_envId)
	{
		if (const auto parent = ParentOf(a_envId); !parent.empty()) {
			const auto* p = a_settings.Find(parent);
			if (p == nullptr || p->mode == Mode::Vanilla) {
				a_settings.environments.erase(a_envId);  // inheriting a Vanilla parent is already vanilla
			} else {
				auto& e = EnsureEntry(a_settings, a_envId);  // explicit "keep this sub vanilla"
				e.mode = Mode::Vanilla;
				ClearSelection(e);
			}
			return;
		}
		if (auto it = a_settings.environments.find(a_envId); it != a_settings.environments.end()) {
			it->second.mode = Mode::Vanilla;
			ClearSelection(it->second);
		}
		const std::string prefix = a_envId + ".";
		for (auto it = a_settings.environments.begin(); it != a_settings.environments.end();) {
			if (it->first.rfind(prefix, 0) == 0) {
				it = a_settings.environments.erase(it);
			} else {
				++it;
			}
		}
	}

	bool HitUsable(const App::SearchHit& a_hit, Mode a_mode, bool a_allowStreaming)
	{
		const auto kind = SearchKindFor(a_mode);
		if (kind.empty() || a_hit.kind != kind) {
			return false;
		}
		if (mj::categories::EqualsNoCase(a_hit.service, "qobuz")) {
			return false;  // never stored (DESIGN 8.8)
		}
		const bool local = a_hit.service.empty() || a_hit.service == "local";
		if (!local && !a_allowStreaming) {
			return false;
		}
		if (a_mode == Mode::Genre) {
			return !a_hit.id.empty() || !a_hit.title.empty();
		}
		return !mj::config::IsPlaceholderId(a_hit.id);  // "None" from Harmony is not an id
	}

	std::vector<App::SearchHit> FilterHits(const std::vector<App::SearchHit>& a_hits, Mode a_mode, bool a_allowStreaming,
		std::size_t a_max)
	{
		std::vector<App::SearchHit> out;
		for (const auto& h : a_hits) {
			if (out.size() >= a_max) {
				break;
			}
			if (HitUsable(h, a_mode, a_allowStreaming)) {
				out.push_back(h);
			}
		}
		return out;
	}

	bool ApplyHit(Settings& a_settings, const std::string& a_envId, const App::SearchHit& a_hit)
	{
		const auto it = a_settings.environments.find(a_envId);
		if (it == a_settings.environments.end()) {
			return false;
		}
		auto& e = it->second;
		if (!HitUsable(a_hit, e.mode, e.allowStreaming)) {
			return false;
		}
		if (e.mode == Mode::Genre) {
			e.selector = a_hit.id.empty() ? a_hit.title : a_hit.id;
			e.service.clear();
			e.id.clear();
		} else {
			e.service = a_hit.service.empty() ? std::string("local") : a_hit.service;
			e.id = a_hit.id;
			e.selector = a_hit.title;
		}
		return true;
	}

	std::string HitLabel(const App::SearchHit& a_hit)
	{
		std::string s = a_hit.title;
		if (!a_hit.subtitle.empty()) {
			s += " - " + a_hit.subtitle;
		}
		if (!a_hit.service.empty() && a_hit.service != "local") {
			s += " [" + a_hit.service + "]";
		}
		return s;
	}

	// ---- presets ------------------------------------------------------------------------------------------

	const std::vector<Preset>& Presets()
	{
		// marth's own setup (2026-10-30). Non-ASCII text is written as UTF-8 byte escapes so it does not depend on the
		// compiler's source character set ("Benoit" with the i-circumflex U+00EE = C3 AE).
		static const std::vector<Preset> kPresets = {
			{ "author", "Author's Recommendation (YouTube Music)",
			  "Streams from YouTube Music: turns streaming ON for the 16 environments it fills (bosses, combat, dungeons, "
			  "exploration regions, towns) and sets their music to the author's artists and playlist. Needs a Harmony server "
			  "with YouTube Music. Other environments are left as they are; you can change anything afterwards.",
			  {
				{ "boss", Mode::Query, "epic battle music orchestral", "", "" },
				{ "combat", Mode::Artist, "Michael Ghelfi", "ytmusic", "UCek0QuSdJ9sW6-L3K0VmnPw" },
				{ "combat.civil_war", Mode::Artist, "Adrian von Ziegler", "ytmusic", "UCJU_n-HbwmQntgeMeIe3nXg" },
				{ "dungeon", Mode::Artist, "Atrium Carceri", "ytmusic", "UCGyKzvCBAw1m04s7VIG5w2g" },
				{ "dungeon.fort", Mode::Artist, "Old Tower", "ytmusic", "UC4ik8p6RUYzvFskVU4TADsw" },
				{ "dungeon.ice", Mode::Artist, "Stephan Micus", "ytmusic", "UCprn6h7zUdy_oO4_iG4vUCQ" },
				{ "explore", Mode::Artist, "Hawktail", "ytmusic", "UCVCj1g2wXBVOXozmTnEG4FA" },
				{ "explore.coast", Mode::Artist, "Cheryl E. Leonard", "ytmusic", "UCVaLftJFR7QWXLgevl-lQxA" },
				{ "explore.forest_fall", Mode::Artist, "Tim Janis", "ytmusic", "UCnDvEXlB3dRT0W1jTdJf_Wg" },
				{ "explore.forest_pine", Mode::Artist, "Deuter", "ytmusic", "UCmKx7PP_bBsNpNsHAhx_a3A" },
				{ "explore.marsh", Mode::Artist, "Beno\xC3\xAE" "t Pioulard", "ytmusic", "UCDfF-6JZ4eOz8HyjQMjW8IA" },
				{ "explore.mountain", Mode::Artist, "Kaki King", "ytmusic", "UCdtSCu86Vy66yeeA4TmB2AA" },
				{ "explore.reach", Mode::Playlist, "Goat Rodeo Sessions", "ytmusic", "PL030536720850468C" },
				{ "explore.snow", Mode::Artist, "Henning Schmiedt", "ytmusic", "UC_g4ULXwWKixpmEWWqof5dQ" },
				{ "town", Mode::Artist, "Per-Olov Kindgren", "ytmusic", "UCL5frAZoI4rtafCi2BqPDAw" },
				{ "town.castle", Mode::Artist, "Fief", "ytmusic", "UCcY_pc8WPZ58k33Cp3ye70A" },
			  } },
		};
		return kPresets;
	}

	const Preset* FindPreset(std::string_view a_id)
	{
		for (const auto& p : Presets()) {
			if (a_id == p.id) {
				return &p;
			}
		}
		return nullptr;
	}

	bool ApplyPreset(Settings& a_settings, std::string_view a_presetId)
	{
		const auto* preset = FindPreset(a_presetId);
		if (!preset) {
			return false;
		}
		for (const auto& entry : preset->entries) {
			// Replace the whole entry (no inherited leftovers); unlisted sub-environments are not touched.
			EnvSetting fresh;
			fresh.poolSize = CurrentPoolSize(a_settings);
			fresh.mode = entry.mode;
			fresh.selector = entry.selector;
			fresh.service = entry.service;
			fresh.id = entry.id;
			fresh.allowStreaming = true;  // the click is the user's consent
			a_settings.environments[entry.envId] = std::move(fresh);
		}
		return true;
	}

	// ---- first run ----------------------------------------------------------------------------------------

	FirstRun ComputeFirstRun(const Settings& a_settings, const App::Connection& a_conn, const std::vector<App::EnvStatus>* a_status)
	{
		FirstRun r;
		r.step1Done = !a_settings.global.serverUrl.empty() && !a_settings.global.personalKey.empty();
		r.step2Done = a_conn.state == App::ConnState::Ok;
		// Step 3 needs a REAL selection: not Vanilla, not an empty selector, not a typed artist/playlist without its link.
		r.step3Done = std::any_of(a_settings.environments.begin(), a_settings.environments.end(), [](const auto& kv) {
			return kv.second.mode != Mode::Vanilla && !kv.second.selector.empty() && !NeedsLink(kv.second);
		});
		// ...and when the core reports on the active environments, not all of them may be known-empty.
		if (r.step3Done && a_status) {
			bool anyActive = false;
			bool anyReady = false;
			for (const auto& st : *a_status) {
				if (st.active) {
					anyActive = true;
					anyReady = anyReady || st.ready > 0;
				}
			}
			if (anyActive && !anyReady) {
				r.step3Done = false;
			}
		}
		r.current = !r.step1Done ? 0 : !r.step2Done ? 1 : !r.step3Done ? 2 : 3;
		return r;
	}

	// ---- status text --------------------------------------------------------------------------------------

	const App::EnvStatus* FindStatus(const std::vector<App::EnvStatus>& a_all, std::string_view a_envId)
	{
		for (const auto& s : a_all) {
			if (s.id == a_envId) {
				return &s;
			}
		}
		return nullptr;
	}

	std::string FormatPoolLine(const App::EnvStatus* a_status)
	{
		if (!a_status) {
			return "Not reported yet";
		}
		std::string s = std::to_string(a_status->ready) + " of " + std::to_string(a_status->target) + " ready";
		if (!a_status->state.empty()) {
			s += " - " + a_status->state;
		}
		if (!a_status->nowPlaying.empty()) {
			s += " - now: " + a_status->nowPlaying;
		}
		return s;
	}

	std::string FormatBytes(std::uint64_t a_bytes)
	{
		static constexpr const char* kUnits[] = { "B", "KB", "MB", "GB", "TB" };
		double v = static_cast<double>(a_bytes);
		std::size_t u = 0;
		while (v >= 1024.0 && u + 1 < std::size(kUnits)) {
			v /= 1024.0;
			++u;
		}
		char buf[48];
		if (u == 0) {
			std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(a_bytes));
		} else {
			std::snprintf(buf, sizeof(buf), "%.1f %s", v, kUnits[u]);
		}
		return buf;
	}

	const char* ConnStateLabel(App::ConnState a_state)
	{
		switch (a_state) {
		case App::ConnState::NotConfigured: return "Not set up";
		case App::ConnState::Testing:       return "Testing...";
		case App::ConnState::Ok:            return "Connected";
		case App::ConnState::Failed:        return "Failed";
		}
		return "Unknown";
	}

	const std::vector<int>& CacheCapChoicesMB()
	{
		static const std::vector<int> kChoices = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
		return kChoices;
	}

	std::string FormatCacheCap(int a_mb)
	{
		if (a_mb >= 1024 && a_mb % 1024 == 0) {
			return std::to_string(a_mb / 1024) + " GB";
		}
		return std::to_string(a_mb) + " MB";
	}

	const std::vector<mj::config::LogLevel>& LogLevelChoices()
	{
		static const std::vector<mj::config::LogLevel> kLevels = {
			mj::config::LogLevel::Error, mj::config::LogLevel::Warn, mj::config::LogLevel::Info,
			mj::config::LogLevel::Debug, mj::config::LogLevel::Trace, mj::config::LogLevel::Off
		};
		return kLevels;
	}

	// ---- apply worker -------------------------------------------------------------------------------------

	ApplyQueue::ApplyQueue(std::function<std::string()> a_job) :
		job_(std::move(a_job))
	{
		thread_ = std::thread([this] { Run(); });
	}

	ApplyQueue::~ApplyQueue()
	{
		{
			std::lock_guard lk(mutex_);
			stop_ = true;
		}
		cv_.notify_all();
		if (thread_.joinable()) {
			thread_.join();
		}
	}

	void ApplyQueue::Request()
	{
		{
			std::lock_guard lk(mutex_);
			pending_ = true;
		}
		cv_.notify_all();
	}

	std::string ApplyQueue::LastError() const
	{
		std::lock_guard lk(mutex_);
		return lastError_;
	}

	std::uint64_t ApplyQueue::Completed() const
	{
		std::lock_guard lk(mutex_);
		return completed_;
	}

	void ApplyQueue::WaitIdle()
	{
		std::unique_lock lk(mutex_);
		cv_.wait(lk, [this] { return !pending_ && !running_; });
	}

	void ApplyQueue::Run()
	{
		std::unique_lock lk(mutex_);
		for (;;) {
			cv_.wait(lk, [this] { return pending_ || stop_; });
			if (!pending_) {
				return;  // stop requested and nothing left to do
			}
			pending_ = false;
			running_ = true;
			lk.unlock();
			std::string err;
			try {
				err = job_();
			} catch (...) {
				err = "Saving the settings failed unexpectedly.";
			}
			lk.lock();
			lastError_ = std::move(err);
			++completed_;
			running_ = false;
			cv_.notify_all();
		}
	}
}
