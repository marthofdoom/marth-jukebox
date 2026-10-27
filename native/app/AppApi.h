#pragma once
// The seam between the playback core (native/pool, native/retarget, native/slots: the "core author")
// and the settings UI (native/menu: the "menu author"). The menu only includes THIS header plus
// config/Settings.h; the core implements it in native/app/App.cpp. All functions are thread-safe,
// never block (heavy work runs on the core's worker) and never touch engine memory.
// Spec: docs/INTEGRATION-1.0.md.
#include <cstdint>
#include <string>
#include <vector>

namespace MJ::App
{
	enum class ConnState : std::uint8_t { NotConfigured, Testing, Ok, Failed };

	struct Connection
	{
		ConnState   state{ ConnState::NotConfigured };
		std::string message;                 // plain language, shown verbatim ("Connected. Harmony 1.0.13, 3 sources ready.")
		bool        supportsFlac{ false };   // from /api/game/status capabilities
		bool        supportsPools{ false };
		bool        youtubeUsable{ false };  // streaming checkbox is meaningful only if true
	};

	// Starts a /api/game/status check with the CURRENT settings (server URL + key). Result via GetConnection().
	void        TestConnectionAsync();
	Connection  GetConnection();

	struct SearchHit
	{
		std::string kind;     // "genre" | "artist" | "playlist" | "album" | "track"
		std::string service;  // "local" | "ytmusic"  (never "qobuz" unless AllowQobuz)
		std::string id;       // selector value to store in Settings (EnvironmentSetting::Id / Selector)
		std::string title;
		std::string subtitle; // artist / owner / track count, may be empty
	};

	struct SearchState
	{
		std::uint64_t        seq{ 0 };     // increments when results change
		bool                 busy{ false };
		std::string          error;        // empty = none; plain language
		std::vector<SearchHit> hits;
	};

	// kind: "genre" | "artist" | "playlist" | "query". Debounce is the menu's job. Replaces the previous search.
	void        SearchAsync(const std::string& a_kind, const std::string& a_text, bool a_includeStreaming);
	SearchState GetSearch();

	struct EnvStatus
	{
		std::string id;           // Settings environment id ("explore", "explore.snow", "combat", ...)
		std::string label;        // "Exploration (snow)"
		bool        active{ false };   // a non-Vanilla mode that was published to Harmony
		int         ready{ 0 };        // tracks in the pool, downloaded and validated
		int         target{ 0 };       // pool size setting
		std::string state;             // plain language: "Playing from Harmony", "Filling pool (1 of 3)", "Vanilla (Harmony unreachable)", ...
		std::string nowPlaying;        // "Artist - Title" or empty
		std::vector<std::string> poolTitles;   // "Artist - Title" per ready track (this is the 1.0 'preview': no audio)
	};

	std::vector<EnvStatus> GetEnvStatus();

	// Call after SettingsStore::Update/Save from the menu. The core re-resolves environments, re-PUTs the Harmony
	// profile, drops pools whose selector/sources changed (keeping the others) and re-targets on the next tick.
	void        ApplySettingsChanged();

	// Convenience for the menu's "reset to vanilla" / "clear cache" buttons (they call ApplySettingsChanged internally).
	void        ResetEnvironmentToVanilla(const std::string& a_envId);
	void        ClearCacheAsync();
	std::uint64_t CacheBytes();        // current cache size on disk

	// Menu preview of what the selector would give: titles from /api/game/next peek=true (no audio, no pool change).
	void        PeekAsync(const std::string& a_envId);
	std::vector<std::string> GetPeek(const std::string& a_envId);
}
