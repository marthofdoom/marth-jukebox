#include "config/Settings.h"

#include "categories/Environments.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>

#if defined(_WIN32)
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#endif

namespace mj::config
{
	namespace
	{
		std::mutex gSinkMutex;
		LogSink    gSink;

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
				s.remove_prefix(1);
			}
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
				s.remove_suffix(1);
			}
			return s;
		}

		std::string Lower(std::string_view s)
		{
			std::string r(s);
			for (auto& c : r) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return r;
		}

		// Removes control characters (incl. CR/LF/TAB) and trims. Keeps UTF-8 bytes (>= 0x80).
		std::string Clean(std::string_view s)
		{
			std::string r;
			r.reserve(s.size());
			for (char c : s) {
				const auto u = static_cast<unsigned char>(c);
				if (u >= 0x20 && u != 0x7f) {
					r += c;
				}
			}
			return std::string(Trim(r));
		}

		// A name that is safe to echo in a message: [A-Za-z0-9_.:-], else '?', capped.
		std::string SafeName(std::string_view s)
		{
			std::string r;
			for (char c : s) {
				if (r.size() >= 48) {
					r += "...";
					break;
				}
				const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				                c == '_' || c == '.' || c == ':' || c == '-';
				r += ok ? c : '?';
			}
			return r;
		}

		bool ParseBool(std::string_view v, bool& out)
		{
			const auto s = Lower(Trim(v));
			if (s == "1" || s == "true" || s == "yes" || s == "on" || s == "enabled") {
				out = true;
				return true;
			}
			if (s == "0" || s == "false" || s == "no" || s == "off" || s == "disabled") {
				out = false;
				return true;
			}
			return false;
		}

		bool ParseInt(std::string_view v, long long& out)
		{
			v = Trim(v);
			if (v.empty()) {
				return false;
			}
			const char* b = v.data();
			const char* e = v.data() + v.size();
			if (*b == '+') {
				++b;
			}
			auto [p, ec] = std::from_chars(b, e, out);
			return ec == std::errc{} && p == e;
		}

		bool ValidTypeName(std::string_view n)
		{
			if (n.empty() || n.size() > 64) {
				return false;
			}
			auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
			if (!alnum(n.front())) {
				return false;
			}
			return std::all_of(n.begin(), n.end(), [&](char c) { return alnum(c) || c == '_' || c == '.' || c == '-'; });
		}

		bool ValidService(std::string_view s)
		{
			return s == "local" || s == "ytmusic";
		}

		int SnapPoolSize(long long v)
		{
			int best = kPoolSizes[0];
			long long bestDist = -1;
			for (int p : kPoolSizes) {
				const long long d = v > p ? v - p : p - v;
				if (bestDist < 0 || d < bestDist || (d == bestDist && p > best)) {
					best = p;
					bestDist = d;
				}
			}
			return best;
		}

		// Environment-id normalisation for INI section names: group/sub lower-case, other.<edid> keeps case.
		std::string NormalizeEnvId(std::string_view raw)
		{
			raw = Trim(raw);
			if (raw.size() > categories::kOtherModsPrefix.size() &&
				categories::EqualsNoCase(raw.substr(0, categories::kOtherModsPrefix.size()), categories::kOtherModsPrefix)) {
				return std::string(categories::kOtherModsPrefix) + std::string(raw.substr(categories::kOtherModsPrefix.size()));
			}
			return Lower(raw);
		}

		bool ValidEnvId(const std::string& id)
		{
			if (!ValidTypeName(id) || id == "default") {
				return false;
			}
			return categories::IsKnownEnvironmentId(id);
		}

		void ApplyGeneral(Settings& s, const std::string& key, std::string_view value, Diagnostics& diag)
		{
			auto& g = s.global;
			if (key == "version") {
				long long v = 0;
				if (!ParseInt(value, v) || v < 1) {
					diag.push_back("[General] Version is not a positive number; assuming " + std::to_string(kSettingsVersion));
				} else {
					s.version = static_cast<int>(std::min<long long>(v, 1'000'000));
					if (s.version > kSettingsVersion) {
						diag.push_back("[General] settings file is version " + std::to_string(s.version) + " but this build understands " +
									   std::to_string(kSettingsVersion) + "; unknown keys are ignored and will be dropped on the next save");
					}
				}
			} else if (key == "serverurl") {
				g.serverUrl = Clean(value);
			} else if (key == "personalkey") {
				g.personalKey = Clean(value);
			} else if (key == "harmonyprofilename") {
				g.harmonyProfileName = Clean(value);
			} else if (key == "cachedir") {
				g.cacheDir = Clean(value);
			} else if (key == "cachecapmb") {
				long long v = 0;
				if (ParseInt(value, v)) {
					g.cacheCapMB = static_cast<int>(std::clamp<long long>(v, INT32_MIN / 2, INT32_MAX / 2));
				} else {
					diag.push_back("[General] CacheCapMB is not a number; keeping " + std::to_string(g.cacheCapMB));
				}
			} else if (key == "loglevel") {
				if (!ParseLogLevel(value, g.logLevel)) {
					diag.push_back("[General] LogLevel not recognised (trace|debug|info|warn|error|off); keeping " + std::string(ToString(g.logLevel)));
				}
			} else {
				diag.push_back("[General] unknown key ignored: " + SafeName(key));
			}
		}

		void ApplyFeature(Features& f, const std::string& key, std::string_view value, Diagnostics& diag)
		{
			bool* target = nullptr;
			if (key == "retarget") target = &f.retarget;
			else if (key == "network") target = &f.network;
			else if (key == "menu") target = &f.menu;
			else if (key == "flac") target = &f.flac;
			else if (key == "rememberplace") target = &f.rememberPlace;
			if (!target) {
				diag.push_back("[Features] unknown key ignored: " + SafeName(key));
				return;
			}
			bool b = false;
			if (ParseBool(value, b)) {
				*target = b;
			} else {
				diag.push_back("[Features] " + SafeName(key) + " is not a boolean (0/1/true/false); keeping the current value");
			}
		}

		void ApplyEnv(EnvironmentSetting& e, const std::string& envId, const std::string& key, std::string_view value, Diagnostics& diag)
		{
			const std::string where = "[Env:" + SafeName(envId) + "] ";
			if (key == "mode") {
				if (!ParseMode(value, e.mode)) {
					diag.push_back(where + "Mode not recognised (vanilla|genre|artist|playlist|query); keeping " + std::string(ToString(e.mode)));
				}
			} else if (key == "selector") {
				e.selector = Clean(value);
			} else if (key == "service") {
				e.service = Lower(Clean(value));
			} else if (key == "id") {
				e.id = Clean(value);
			} else if (key == "allowstreaming" || key == "rememberplace") {
				bool b = false;
				if (ParseBool(value, b)) {
					(key == "allowstreaming" ? e.allowStreaming : e.rememberPlace) = b;
				} else {
					diag.push_back(where + SafeName(key) + " is not a boolean; keeping the current value");
				}
			} else if (key == "poolsize") {
				long long v = 0;
				if (ParseInt(value, v)) {
					e.poolSize = static_cast<int>(std::clamp<long long>(v, 0, 1'000'000));
				} else {
					diag.push_back(where + "PoolSize is not a number; keeping " + std::to_string(e.poolSize));
				}
			} else {
				diag.push_back(where + "unknown key ignored: " + SafeName(key));
			}
		}

		bool ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::string& err)
		{
#if defined(_WIN32)
			if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				err = "MoveFileExW failed, error " + std::to_string(GetLastError());
				return false;
			}
			return true;
#else
			std::error_code ec;
			std::filesystem::rename(from, to, ec);
			if (ec) {
				err = "rename failed: " + ec.message();
				return false;
			}
			return true;
#endif
		}
	}

	// ------------------------------------------------------------------------------------------------
	// conversions
	// ------------------------------------------------------------------------------------------------
	const EnvironmentSetting* Settings::Find(std::string_view envId) const
	{
		auto it = environments.find(envId);
		return it == environments.end() ? nullptr : &it->second;
	}

	std::string_view ToString(Mode m)
	{
		switch (m) {
		case Mode::Vanilla: return "Vanilla";
		case Mode::Genre: return "Genre";
		case Mode::Artist: return "Artist";
		case Mode::Playlist: return "Playlist";
		case Mode::Query: return "Query";
		}
		return "Vanilla";
	}

	bool ParseMode(std::string_view s, Mode& out)
	{
		const auto l = Lower(Trim(s));
		if (l == "vanilla") out = Mode::Vanilla;
		else if (l == "genre") out = Mode::Genre;
		else if (l == "artist") out = Mode::Artist;
		else if (l == "playlist") out = Mode::Playlist;
		else if (l == "query") out = Mode::Query;
		else return false;
		return true;
	}

	std::string_view ToString(LogLevel l)
	{
		switch (l) {
		case LogLevel::Trace: return "trace";
		case LogLevel::Debug: return "debug";
		case LogLevel::Info: return "info";
		case LogLevel::Warn: return "warn";
		case LogLevel::Error: return "error";
		case LogLevel::Off: return "off";
		}
		return "info";
	}

	bool ParseLogLevel(std::string_view s, LogLevel& out)
	{
		const auto l = Lower(Trim(s));
		if (l == "trace") out = LogLevel::Trace;
		else if (l == "debug") out = LogLevel::Debug;
		else if (l == "info") out = LogLevel::Info;
		else if (l == "warn" || l == "warning") out = LogLevel::Warn;
		else if (l == "error" || l == "err") out = LogLevel::Error;
		else if (l == "off" || l == "none") out = LogLevel::Off;
		else return false;
		return true;
	}

	spdlog::level::level_enum ToSpdlog(LogLevel l)
	{
		switch (l) {
		case LogLevel::Trace: return spdlog::level::trace;
		case LogLevel::Debug: return spdlog::level::debug;
		case LogLevel::Info: return spdlog::level::info;
		case LogLevel::Warn: return spdlog::level::warn;
		case LogLevel::Error: return spdlog::level::err;
		case LogLevel::Off: return spdlog::level::off;
		}
		return spdlog::level::info;
	}

	void SetLogSink(LogSink sink)
	{
		std::lock_guard lk(gSinkMutex);
		gSink = std::move(sink);
	}

	void Log(spdlog::level::level_enum level, const std::string& message)
	{
		LogSink sink;
		{
			std::lock_guard lk(gSinkMutex);
			sink = gSink;
		}
		if (sink) {
			sink(level, message);
		} else {
			spdlog::log(level, "[settings] {}", message);
		}
	}

	// ------------------------------------------------------------------------------------------------
	// pure functions
	// ------------------------------------------------------------------------------------------------
	Settings DefaultSettings()
	{
		return Settings{};
	}

	bool NetworkActive(const Settings& s)
	{
		return s.global.features.network && !s.global.serverUrl.empty();
	}

	std::filesystem::path EffectiveCacheDir(const Settings& s, const std::filesystem::path& defaultDir)
	{
		return s.global.cacheDir.empty() ? defaultDir : std::filesystem::path(s.global.cacheDir);
	}

	std::string_view SourcesFor(const EnvironmentSetting& e)
	{
		return e.allowStreaming ? "all" : "library";
	}

	void Sanitize(Settings& s, Diagnostics& diag)
	{
		s.version = kSettingsVersion;
		auto& g = s.global;

		// ServerUrl: http(s) only, no spaces, no trailing slash. The value is never echoed (may carry ?key=).
		g.serverUrl = Clean(g.serverUrl);
		if (!g.serverUrl.empty()) {
			const auto l = Lower(g.serverUrl);
			const bool schemeOk = l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
			const bool hostOk = g.serverUrl.size() > (l.rfind("https://", 0) == 0 ? 8u : 7u);
			const bool spaces = g.serverUrl.find(' ') != std::string::npos;
			if (!schemeOk || !hostOk || spaces || g.serverUrl.size() > 512) {
				diag.push_back("ServerUrl rejected (needs http:// or https://, a host, no spaces, at most 512 chars); network stays off");
				g.serverUrl.clear();
			} else {
				while (g.serverUrl.size() > 8 && g.serverUrl.back() == '/') {
					g.serverUrl.pop_back();
				}
			}
		}

		g.personalKey = Clean(g.personalKey);
		if (g.personalKey.size() > 256) {
			diag.push_back("PersonalKey longer than 256 characters; cleared");
			g.personalKey.clear();
		}

		g.harmonyProfileName = Clean(g.harmonyProfileName);
		if (!ValidTypeName(g.harmonyProfileName)) {
			diag.push_back("HarmonyProfileName invalid (1-64 chars of letters, digits, _ . -, starting alphanumeric); using '" +
						   std::string(kDefaultProfileName) + "'");
			g.harmonyProfileName = kDefaultProfileName;
		}

		g.cacheDir = Clean(g.cacheDir);
		if (g.cacheDir.size() > 520) {
			diag.push_back("CacheDir longer than 520 characters; using the default directory");
			g.cacheDir.clear();
		}

		if (g.cacheCapMB < kCacheCapMinMB) {
			diag.push_back("CacheCapMB " + std::to_string(g.cacheCapMB) + " below the minimum; using " + std::to_string(kCacheCapMinMB));
			g.cacheCapMB = kCacheCapMinMB;
		} else if (g.cacheCapMB > kCacheCapMaxMB) {
			diag.push_back("CacheCapMB " + std::to_string(g.cacheCapMB) + " above the maximum; using " + std::to_string(kCacheCapMaxMB));
			g.cacheCapMB = kCacheCapMaxMB;
		}

		// Environments.
		for (auto it = s.environments.begin(); it != s.environments.end();) {
			if (!ValidEnvId(it->first)) {
				diag.push_back("environment '" + SafeName(it->first) + "' is not a known environment id; dropped");
				it = s.environments.erase(it);
			} else {
				++it;
			}
		}
		while (s.environments.size() > kMaxEnvironments) {
			diag.push_back("more than " + std::to_string(kMaxEnvironments) + " environments; dropped '" + SafeName(s.environments.rbegin()->first) + "'");
			s.environments.erase(std::prev(s.environments.end()));
		}

		for (auto& [id, e] : s.environments) {
			const std::string where = "environment '" + SafeName(id) + "': ";
			e.selector = Clean(e.selector);
			if (e.selector.size() > kMaxTextLen) {
				diag.push_back(where + "selector longer than " + std::to_string(kMaxTextLen) + " characters; cleared");
				e.selector.clear();
			}
			e.id = Clean(e.id);
			if (e.id.size() > 256) {
				diag.push_back(where + "id longer than 256 characters; cleared");
				e.id.clear();
			}
			e.service = Lower(Clean(e.service));
			if (!e.service.empty() && !ValidService(e.service)) {
				const bool usesService = e.mode == Mode::Artist || e.mode == Mode::Playlist;
				diag.push_back(where + "service not supported (only local and ytmusic; Qobuz is excluded)" +
							   (usesService ? "; environment reset to Vanilla" : "; cleared"));
				e.service.clear();
				if (usesService) {
					e.mode = Mode::Vanilla;
				}
			}
			const int snapped = SnapPoolSize(e.poolSize);
			if (snapped != e.poolSize) {
				diag.push_back(where + "poolSize " + std::to_string(e.poolSize) + " not one of 3/5/8; using " + std::to_string(snapped));
				e.poolSize = snapped;
			}
		}
	}

	Settings ParseIni(std::string_view text, Diagnostics& diag)
	{
		Settings s = DefaultSettings();
		if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
			static_cast<unsigned char>(text[2]) == 0xBF) {
			text.remove_prefix(3);
		}

		enum class Sec { None, General, Features, Env, Unknown };
		Sec         sec = Sec::None;
		std::string envId;
		bool        envOk = false;
		std::size_t lineNo = 0;

		while (!text.empty()) {
			const auto nl = text.find('\n');
			std::string_view line = text.substr(0, nl);
			text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
			++lineNo;
			line = Trim(line);
			if (line.empty() || line.front() == ';' || line.front() == '#') {
				continue;
			}
			if (line.front() == '[') {
				if (line.back() != ']') {
					diag.push_back("line " + std::to_string(lineNo) + ": malformed section header ignored");
					sec = Sec::Unknown;
					continue;
				}
				const auto name = Trim(line.substr(1, line.size() - 2));
				const auto ln = Lower(name);
				if (ln == "general") {
					sec = Sec::General;
				} else if (ln == "features") {
					sec = Sec::Features;
				} else if (ln.rfind("env:", 0) == 0) {
					sec = Sec::Env;
					envId = NormalizeEnvId(name.substr(4));
					envOk = ValidEnvId(envId);
					if (!envOk) {
						diag.push_back("line " + std::to_string(lineNo) + ": section [Env:" + SafeName(envId) + "] is not a known environment; ignored");
					} else if (!s.environments.contains(envId)) {
						s.environments.emplace(envId, EnvironmentSetting{});
					}
				} else {
					sec = Sec::Unknown;
					diag.push_back("line " + std::to_string(lineNo) + ": unknown section [" + SafeName(name) + "] ignored");
				}
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string_view::npos) {
				diag.push_back("line " + std::to_string(lineNo) + ": not a key=value line; ignored");
				continue;
			}
			const std::string key = Lower(Trim(line.substr(0, eq)));
			const std::string_view value = Trim(line.substr(eq + 1));
			switch (sec) {
			case Sec::General: ApplyGeneral(s, key, value, diag); break;
			case Sec::Features: ApplyFeature(s.global.features, key, value, diag); break;
			case Sec::Env:
				if (envOk) {
					ApplyEnv(s.environments[envId], envId, key, value, diag);
				}
				break;
			case Sec::None:
				diag.push_back("line " + std::to_string(lineNo) + ": key outside any section; ignored");
				break;
			case Sec::Unknown: break;
			}
		}
		Sanitize(s, diag);
		return s;
	}

	std::string SerializeIni(const Settings& s)
	{
		auto safe = [](const std::string& v) { return Clean(v); };
		std::ostringstream o;
		const auto& g = s.global;
		o << "; marth's Jukebox settings. Edited by the in-game menu; hand edits are fine while the game is closed.\n"
		  << "; Unknown keys are ignored. Invalid values are corrected and logged to MarthJukebox.log.\n"
		  << "\n[General]\n"
		  << "Version=" << kSettingsVersion << "\n"
		  << "; Harmony server base URL, e.g. http://harmony.lan:8080. Empty = the plugin never touches the network.\n"
		  << "ServerUrl=" << safe(g.serverUrl) << "\n"
		  << "; WARNING: the personal key is stored in PLAIN TEXT here. Do not share or upload this file.\n"
		  << "PersonalKey=" << safe(g.personalKey) << "\n"
		  << "; Name of the Harmony profile this plugin owns and overwrites.\n"
		  << "HarmonyProfileName=" << safe(g.harmonyProfileName) << "\n"
		  << "; Empty = Data\\SKSE\\Plugins\\MarthJukebox\\cache\n"
		  << "CacheDir=" << safe(g.cacheDir) << "\n"
		  << "; Cache size cap in MB (minimum " << kCacheCapMinMB << ").\n"
		  << "CacheCapMB=" << g.cacheCapMB << "\n"
		  << "; trace | debug | info | warn | error | off\n"
		  << "LogLevel=" << ToString(g.logLevel) << "\n"
		  << "\n[Features]\n"
		  << "; Kill switches (1 = on). Switch one off to isolate a problem.\n"
		  << "Retarget=" << g.features.retarget << "\n"
		  << "Network=" << g.features.network << "\n"
		  << "Menu=" << g.features.menu << "\n"
		  << "Flac=" << g.features.flac << "\n"
		  << "RememberPlace=" << g.features.rememberPlace << "\n";

		for (const auto& [id, e] : s.environments) {
			o << "\n[Env:" << id << "]\n"
			  << "Mode=" << ToString(e.mode) << "\n"
			  << "Selector=" << safe(e.selector) << "\n"
			  << "Service=" << safe(e.service) << "\n"
			  << "Id=" << safe(e.id) << "\n"
			  << "AllowStreaming=" << e.allowStreaming << "\n"
			  << "RememberPlace=" << e.rememberPlace << "\n"
			  << "PoolSize=" << e.poolSize << "\n";
		}
		return o.str();
	}

	// ------------------------------------------------------------------------------------------------
	// store
	// ------------------------------------------------------------------------------------------------
	SettingsStore::SettingsStore(std::filesystem::path iniPath, std::filesystem::path defaultCacheDir) :
		iniPath_(std::move(iniPath)),
		defaultCacheDir_(std::move(defaultCacheDir)),
		current_(std::make_shared<const Settings>(DefaultSettings()))
	{}

	std::shared_ptr<const Settings> SettingsStore::Snapshot() const
	{
		std::lock_guard lk(mutex_);
		return current_;
	}

	std::filesystem::path SettingsStore::EffectiveCacheDir() const
	{
		return config::EffectiveCacheDir(*Snapshot(), defaultCacheDir_);
	}

	std::uint64_t SettingsStore::Publish(Settings&& next)
	{
		const std::uint64_t rev = revision_.load(std::memory_order_relaxed) + 1;
		next.revision = rev;
		current_ = std::make_shared<const Settings>(std::move(next));
		revision_.store(rev, std::memory_order_release);
		return rev;
	}

	std::uint64_t SettingsStore::Update(const std::function<void(Settings&)>& edit)
	{
		Diagnostics diag;
		std::uint64_t rev = 0;
		{
			std::lock_guard lk(mutex_);
			Settings next = *current_;
			edit(next);
			Sanitize(next, diag);
			rev = Publish(std::move(next));
		}
		for (const auto& d : diag) {
			Log(spdlog::level::warn, d);
		}
		return rev;
	}

	std::uint64_t SettingsStore::ResetToDefaults()
	{
		std::uint64_t rev = 0;
		{
			std::lock_guard lk(mutex_);
			rev = Publish(DefaultSettings());
		}
		Log(spdlog::level::info, "settings reset to defaults");
		return rev;
	}

	bool SettingsStore::Load()
	{
		std::error_code ec;
		std::filesystem::path tmp = iniPath_;
		tmp += ".tmp";
		if (std::filesystem::exists(tmp, ec)) {
			Log(spdlog::level::warn, "stale settings temp file from an interrupted save found; ignoring and removing it");
			std::filesystem::remove(tmp, ec);
		}

		Diagnostics diag;
		Settings loaded = DefaultSettings();
		bool readOk = true;

		if (!std::filesystem::exists(iniPath_, ec)) {
			Log(spdlog::level::info, "no settings file yet; using defaults (nothing is written until a setting changes)");
		} else {
			std::ifstream in(iniPath_, std::ios::binary);
			if (!in) {
				Log(spdlog::level::err, "settings file exists but could not be opened; using defaults");
				readOk = false;
			} else {
				std::ostringstream ss;
				ss << in.rdbuf();
				if (in.bad()) {
					Log(spdlog::level::err, "settings file could not be read completely; using defaults");
					readOk = false;
				} else {
					loaded = ParseIni(ss.str(), diag);
				}
			}
		}
		for (const auto& d : diag) {
			Log(spdlog::level::warn, d);
		}
		{
			std::lock_guard lk(mutex_);
			Publish(std::move(loaded));
		}
		return readOk;
	}

	bool SettingsStore::Save(std::string* error)
	{
		std::lock_guard saveLk(saveMutex_);
		const auto snap = Snapshot();
		const std::string text = SerializeIni(*snap);

		auto fail = [&](const std::string& why) {
			Log(spdlog::level::err, "saving settings failed: " + why);
			if (error) {
				*error = why;
			}
			return false;
		};

		std::error_code ec;
		if (iniPath_.has_parent_path()) {
			std::filesystem::create_directories(iniPath_.parent_path(), ec);
			if (ec) {
				return fail("cannot create the settings folder: " + ec.message());
			}
		}
		std::filesystem::path tmp = iniPath_;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out) {
				return fail("cannot create the temporary settings file");
			}
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			out.flush();
			if (!out) {
				out.close();
				std::filesystem::remove(tmp, ec);
				return fail("writing the temporary settings file failed (disk full?)");
			}
		}
		std::string why;
		if (!ReplaceFile(tmp, iniPath_, why)) {
			std::filesystem::remove(tmp, ec);
			return fail(why);
		}
		return true;
	}
}
