#pragma once

// Plugin settings model + persistence (DESIGN 3 item 6, 8.4, 8.8).
//
// Pure C++ (std + spdlog). No engine calls: the INI path and the default cache directory are injected by
// the caller. Everything here is safe to call from any thread; the game thread never needs more than
// Snapshot(), which is a shared_ptr copy under a short mutex.
//
// Safe defaults: every environment Vanilla, network effectively off until a server URL is set.
// The personal key is stored in plain text (it has to be sent as a header) and NEVER appears in any
// message this module logs or returns: diagnostics name keys, never values of ServerUrl/PersonalKey.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

namespace mj::config
{
	inline constexpr int kSettingsVersion = 1;

	inline constexpr int kCacheCapMinMB     = 256;
	inline constexpr int kCacheCapDefaultMB = 4096;
	inline constexpr int kCacheCapMaxMB     = 1'048'576;  // 1 TiB: a typo guard, not a policy
	inline constexpr int kPoolSizes[]       = { 3, 5, 8 };
	inline constexpr int kToastIntervalMinMin     = 1;
	inline constexpr int kToastIntervalDefaultMin = 5;
	inline constexpr int kToastIntervalMaxMin     = 60;
	inline constexpr std::size_t kMaxTextLen       = 1024;  // hard cap when loading; ProfileBuilder enforces Harmony's 200
	inline constexpr std::size_t kMaxEnvironments  = 128;
	inline constexpr const char* kDefaultProfileName = "skyrim-jukebox";

	enum class Mode : std::uint8_t
	{
		Vanilla,   // the environment is left alone
		Genre,     // selector = genre name
		Artist,    // service + id
		Playlist,  // service + id
		Query      // selector = free-text search
	};

	enum class LogLevel : std::uint8_t
	{
		Trace,
		Debug,
		Info,
		Warn,
		Error,
		Off
	};

	struct Features
	{
		bool retarget      = true;
		bool network       = true;
		bool menu          = true;
		bool flac          = true;
		bool rememberPlace = true;
	};

	struct Global
	{
		std::string serverUrl;                                 // empty = network off
		std::string personalKey;                               // plain text; see file header
		std::string harmonyProfileName = kDefaultProfileName;  // Harmony profile the plugin owns
		std::string cacheDir;                                  // empty = the caller's default directory
		int         cacheCapMB = kCacheCapDefaultMB;
		LogLevel    logLevel   = LogLevel::Info;
		bool        allowQobuz = false;  // [General] AllowQobuz: INI only, never in the menu (DESIGN 8.8)
		bool        toastEnabled = true;                            // in-game "Harmony not reachable" notifications
		int         toastIntervalMinutes = kToastIntervalDefaultMin;  // repeat interval while it stays down (1..60)
		Features    features;
	};

	// One music environment ("explore") or sub-environment override ("explore.tundra").
	// A sub-environment entry that EXISTS overrides its group (Vanilla included = "keep this sub vanilla");
	// no entry = inherit from the group.
	struct EnvironmentSetting
	{
		Mode        mode = Mode::Vanilla;
		std::string selector;   // genre / query text, or the display label of a picked artist/playlist
		std::string service;    // artist / playlist: "local" | "ytmusic" (never qobuz, DESIGN 8.8)
		std::string id;         // artist / playlist id from /api/search
		bool        allowStreaming = false;  // false => Harmony sources=library; true => YouTube Music too
		bool        rememberPlace  = false;
		int         poolSize       = 3;      // 3 | 5 | 8
	};

	struct Settings
	{
		int           version = kSettingsVersion;
		Global        global;
		std::map<std::string, EnvironmentSetting, std::less<>> environments;  // key = environment id
		std::uint64_t revision = 0;  // set by SettingsStore on every published change

		[[nodiscard]] const EnvironmentSetting* Find(std::string_view envId) const;
	};

	using Diagnostics = std::vector<std::string>;

	// ---- string conversions ------------------------------------------------------------------------
	[[nodiscard]] std::string_view ToString(Mode m);
	[[nodiscard]] bool             ParseMode(std::string_view s, Mode& out);
	[[nodiscard]] std::string_view ToString(LogLevel l);
	[[nodiscard]] bool             ParseLogLevel(std::string_view s, LogLevel& out);
	[[nodiscard]] spdlog::level::level_enum ToSpdlog(LogLevel l);

	// ---- pure functions ----------------------------------------------------------------------------
	[[nodiscard]] Settings DefaultSettings();

	// 1.0.1 (marth): the settings of a FRESH install, used only when no settings file exists yet. Two example
	// environments, both with streaming OFF (liability decision): exploration = Query "Brass Fantasy Adventure", town =
	// Artist "Per-Olov Kindgren" (YouTube Music id from the menu's suggestions). They play nothing until the player ticks
	// "Allow streaming" (or has matching library tracks), and nothing touches the network before ServerUrl is set.
	// An existing INI is never changed: an environment without a section stays Vanilla.
	[[nodiscard]] Settings FirstRunSettings();

	// Harmony bug: some search results carry the id "None"/"null" instead of a real one. True for
	// such placeholders and for an empty id: never offered, never stored, never sent to Harmony.
	[[nodiscard]] bool IsPlaceholderId(std::string_view id);

	// Search hit for an Artist/Playlist/Album pick: usable, or why not (the menu shows the reason).
	enum class HitVerdict : std::uint8_t { Ok, NoId, Qobuz, NeedsStreaming, OtherService };
	[[nodiscard]] HitVerdict CheckSearchHit(std::string_view service, std::string_view id, bool streaming);

	// The menu's note for a playlist search/list (empty = none): `shown` usable hits, `noId` hidden for a missing id,
	// `qobuz` hidden because they are Qobuz playlists.
	[[nodiscard]] std::string PlaylistSearchNote(int shown, int noId, int qobuz, bool streaming);

	// Clamp / validate in place. Every correction appends one line to `diag` (never the key or URL value).
	void Sanitize(Settings& s, Diagnostics& diag);

	// Tolerant parser: unknown sections/keys, garbage lines, BOM, CRLF, duplicate keys (last wins) and
	// bad values are survived; each problem becomes a diagnostic line without echoing line content.
	// `recognisedKeys` (optional) receives how many known keys were found: 0 for a non-empty file means the
	// content is not one of ours (UTF-16, zero-filled, wrong file...).
	[[nodiscard]] Settings ParseIni(std::string_view text, Diagnostics& diag, int* recognisedKeys = nullptr);

	// Stable, human-editable INI. Environments sorted by id. Carries the plain-text key warning.
	[[nodiscard]] std::string SerializeIni(const Settings& s);

	// True when the plugin may talk to the network at all: kill switch on AND a server URL set.
	[[nodiscard]] bool NetworkActive(const Settings& s);

	// The cache directory in effect: Settings.cacheDir if set, else `defaultDir`.
	[[nodiscard]] std::filesystem::path EffectiveCacheDir(const Settings& s, const std::filesystem::path& defaultDir);

	// Streaming flag -> Harmony `sources` request field, assuming a server that accepts a service list and
	// AllowQobuz off: "library" or "library,ytmusic". Use ChooseSources() for the real decision.
	[[nodiscard]] std::string_view SourcesFor(const EnvironmentSetting& e);

	// The `sources` value for one environment's `next` call (DESIGN 8.8, docs/NEXUS-COMPLIANCE.md).
	//   streaming off                      -> "library"
	//   streaming on, server takes a list  -> "library,ytmusic"  (+ ",qobuz" only with [General] AllowQobuz=1)
	//   streaming on, server too old for a list (no `services` in /api/game/status): its only streaming value is
	//     "all", which includes Qobuz when the server is signed in, so it is used only with AllowQobuz=1;
	//     otherwise "library" and streamingDropped = true (never stream what the player did not consent to).
	struct SourcesChoice
	{
		std::string sources;
		bool        streamingDropped = false;  // the player asked for streaming but this server cannot honour it safely
	};
	[[nodiscard]] SourcesChoice ChooseSources(const Settings& s, const EnvironmentSetting& e, bool serverAcceptsList);

	// ---- logging hook ------------------------------------------------------------------------------
	// Everything this module logs goes through here. Default: spdlog's default logger. Tests install a
	// capturing sink; production code leaves it alone.
	using LogSink = std::function<void(spdlog::level::level_enum, const std::string&)>;
	void SetLogSink(LogSink sink);
	void Log(spdlog::level::level_enum level, const std::string& message);

	// ---- store -------------------------------------------------------------------------------------
	class SettingsStore
	{
	public:
		// iniPath: Data/SKSE/Plugins/MarthJukebox.ini (injected). defaultCacheDir: injected too.
		SettingsStore(std::filesystem::path iniPath, std::filesystem::path defaultCacheDir);

		// Reads the INI (missing file = defaults, nothing is created). Publishes a new snapshot.
		// A stale "<ini>.tmp" from a crashed save is ignored and removed. Returns false when the file exists
		// but could not be used: unreadable, or content with no recognised key (empty, zero-filled, UTF-16...).
		// Defaults are published anyway, and the NEXT Save first preserves the original bytes in
		// "<ini>.unreadable[.N]" (it refuses to save if that copy fails), so a bad read can never destroy
		// the user's INI or personal key.
		bool Load();

		// Atomically writes the current snapshot: "<ini>.tmp" (flushed to disk), then replace. False + message
		// on failure (the old file is left intact). The first Save of a session also keeps the previous good
		// file as "<ini>.bak".
		bool Save(std::string* error = nullptr);

		// Immutable view; cheap. A worker keeps its pointer as long as it likes.
		[[nodiscard]] std::shared_ptr<const Settings> Snapshot() const;

		// Increases on every published change (Load, Update, Reset). Workers poll this.
		[[nodiscard]] std::uint64_t Revision() const noexcept { return revision_.load(std::memory_order_acquire); }

		// Copy-on-write edit: copies the current snapshot, runs `edit`, sanitizes (logging corrections),
		// publishes, returns the new revision. Concurrent Update calls serialize; none is lost.
		// `edit` runs under the store's lock: it must NOT call back into this store (Snapshot/Update/...).
		std::uint64_t Update(const std::function<void(Settings&)>& edit);

		std::uint64_t ResetToDefaults();

		[[nodiscard]] const std::filesystem::path& IniPath() const noexcept { return iniPath_; }
		[[nodiscard]] const std::filesystem::path& DefaultCacheDir() const noexcept { return defaultCacheDir_; }
		[[nodiscard]] std::filesystem::path EffectiveCacheDir() const;

	private:
		std::uint64_t Publish(Settings&& next);  // caller holds mutex_

		std::filesystem::path iniPath_;
		std::filesystem::path defaultCacheDir_;

		mutable std::mutex                 mutex_;  // guards current_ and revision generation
		std::shared_ptr<const Settings>    current_;
		std::atomic<std::uint64_t>         revision_{ 0 };
		std::mutex                         saveMutex_;  // one file operation at a time (Load and Save); locked before mutex_
		bool                               loadUnusable_ = false;  // guarded by saveMutex_: last Load found an unusable existing file
		bool                               bakDone_      = false;  // guarded by saveMutex_: previous good file already kept this session
	};
}
