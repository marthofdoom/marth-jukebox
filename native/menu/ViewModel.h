#pragma once

// Pure view-model logic of the settings page (docs/INTEGRATION-1.0.md section B).
//
// No ImGui, no engine, no CommonLib, no network: std + config/Settings.h + categories/Environments.h +
// app/AppApi.h (types only). Everything the page DECIDES lives here so the console test target
// (mj_menu_tests) can exercise it: preset application, field validation, the debounce state machine,
// hit -> setting mapping, row building, status formatting, the coalescing apply worker.
//
// The personal key is handled as an opaque string: nothing in this file formats, logs or returns it.

#include "app/AppApi.h"
#include "categories/Environments.h"
#include "config/Settings.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace MJ::Menu
{
	using EnvSetting = mj::config::EnvironmentSetting;
	using Mode = mj::config::Mode;
	using Settings = mj::config::Settings;

	// ---- time-driven state machines (the caller feeds a monotonic millisecond clock) --------------------
	inline constexpr std::uint64_t kSearchDebounceMs = 400;   // spec: searches 400 ms after typing stops
	inline constexpr std::uint64_t kConfirmWindowMs  = 4000;  // "click again to confirm" window

	// Fires ONCE, `a_delayMs` after the last Touch. Touch again restarts the wait; Cancel drops it.
	class Debouncer
	{
	public:
		void Touch(std::uint64_t a_nowMs) noexcept { pending_ = true; last_ = a_nowMs; }
		void Cancel() noexcept { pending_ = false; }
		[[nodiscard]] bool Pending() const noexcept { return pending_; }
		// True exactly once per burst of Touch calls, when the quiet time has elapsed.
		[[nodiscard]] bool Poll(std::uint64_t a_nowMs, std::uint64_t a_delayMs) noexcept
		{
			if (!pending_ || a_nowMs < last_ || a_nowMs - last_ < a_delayMs) {
				return false;
			}
			pending_ = false;
			return true;
		}
	private:
		bool          pending_ = false;
		std::uint64_t last_    = 0;
	};

	// Two-click confirmation for destructive buttons: first Press arms, a second Press inside the window fires.
	class ConfirmGate
	{
	public:
		// Returns true when the action should run now.
		[[nodiscard]] bool Press(std::uint64_t a_nowMs, std::uint64_t a_windowMs = kConfirmWindowMs) noexcept
		{
			if (armed_ && a_nowMs >= armedAt_ && a_nowMs - armedAt_ <= a_windowMs) {
				armed_ = false;
				return true;
			}
			armed_ = true;
			armedAt_ = a_nowMs;
			return false;
		}
		[[nodiscard]] bool Armed(std::uint64_t a_nowMs, std::uint64_t a_windowMs = kConfirmWindowMs) const noexcept
		{
			return armed_ && a_nowMs >= armedAt_ && a_nowMs - armedAt_ <= a_windowMs;
		}
	private:
		bool          armed_   = false;
		std::uint64_t armedAt_ = 0;
	};

	// ---- editable text field: buffer vs last committed value --------------------------------------------
	// Text is committed on Enter/blur only (never per keystroke). SyncFromStore adopts a changed stored value
	// unless the user has uncommitted edits in the buffer.
	struct TextField
	{
		std::string edit;
		std::string committed;

		[[nodiscard]] bool Dirty() const { return edit != committed; }
		// Returns true (and marks committed) when there is something to commit.
		bool Commit()
		{
			if (!Dirty()) {
				return false;
			}
			committed = edit;
			return true;
		}
		void SyncFromStore(const std::string& a_stored)
		{
			if (!Dirty()) {
				edit = committed = a_stored;
			}
		}
	};

	// ---- field validation (advisory: the store sanitizes again; these messages go on screen) --------------
	struct FieldCheck
	{
		bool        ok = true;
		std::string message;  // plain language, empty when there is nothing to say; never contains the value
	};

	[[nodiscard]] FieldCheck ValidateServerUrl(std::string_view a_url);
	[[nodiscard]] FieldCheck ValidatePersonalKey(std::string_view a_key);
	// Harmony limits a selector text to 200 characters without control characters.
	[[nodiscard]] FieldCheck ValidateSelectorText(std::string_view a_text);
	[[nodiscard]] std::size_t CountCodePoints(std::string_view a_utf8);

	// ---- modes ------------------------------------------------------------------------------------------
	struct ModeChoice
	{
		bool        inherit = false;  // sub-environments only: "Same as the parent" (erases the override)
		Mode        mode    = Mode::Vanilla;
		const char* label   = "";
	};
	// Combo contents: groups get Vanilla/Genre/Artist/Playlist/Query, sub-environments get "inherit" first.
	[[nodiscard]] std::vector<ModeChoice> ModeChoices(bool a_isSub);
	[[nodiscard]] int ModeChoiceIndex(const std::vector<ModeChoice>& a_choices, const EnvSetting* a_setting);
	// "genre" | "artist" | "playlist" for list-backed modes; empty for Vanilla and Query (typed text).
	[[nodiscard]] std::string_view SearchKindFor(Mode a_mode);
	[[nodiscard]] bool ModeNeedsSearch(Mode a_mode);

	// ---- rows -------------------------------------------------------------------------------------------
	struct EnvRow
	{
		std::string id;          // settings key: "explore" or "explore.snow"
		std::string label;       // "Exploration" / "Snow"
		std::string summary;     // group one-liner (groups only)
		std::string parentId;    // groups: empty
		bool        isSub = false;
	};
	// One row per PLAYABLE group followed by its sub-environments. Kept-vanilla groups are never listed.
	[[nodiscard]] std::vector<EnvRow> BuildRows();

	// "Vanilla" / "Genre: Ambient" / "Artist: Name (local)" / "Same as Exploration: Genre: Ambient".
	[[nodiscard]] std::string DescribeSelection(const Settings& a_settings, const EnvRow& a_row);

	// ---- edits (all run inside SettingsStore::Update; they only touch fields the user asked about) --------
	// Pool size applied to environments created from now on: the size of the first configured entry, else 3.
	[[nodiscard]] int CurrentPoolSize(const Settings& a_settings);

	// Gets the entry for `a_envId`, creating it (copied from the parent group's entry when one exists, so the pool
	// size and flags carry over; mode reset to Vanilla) when missing.
	EnvSetting& EnsureEntry(Settings& a_settings, const std::string& a_envId);

	// Changes the mode. A mode change clears selector/service/id (they belong to the previous mode).
	// `a_inherit` (sub rows) erases the override instead.
	void SetMode(Settings& a_settings, const std::string& a_envId, Mode a_mode, bool a_inherit = false);
	void SetStreaming(Settings& a_settings, const std::string& a_envId, bool a_value);
	void SetRememberPlace(Settings& a_settings, const std::string& a_envId, bool a_value);
	// The typed text field (every mode except Vanilla). Genre/Query: the text is the selector. Artist/Playlist: typing a
	// different name than the picked suggestion drops the stored service + id (see NeedsLink); the same name keeps them.
	// False (no change) for Vanilla, a missing entry or invalid text.
	bool SetSelectorText(Settings& a_settings, const std::string& a_envId, const std::string& a_text);
	// Artist/Playlist typed but not linked to a service + id yet: ProfileBuilder cannot publish it until a suggestion is chosen.
	[[nodiscard]] bool NeedsLink(const EnvSetting& a_setting);
	// Pool size for every configured entry (3 | 5 | 8; anything else is ignored).
	void SetPoolSizeAll(Settings& a_settings, int a_size);

	// A group row resets the group AND erases its sub-environment overrides; a sub row becomes an explicit Vanilla override.
	void ResetToVanilla(Settings& a_settings, const std::string& a_envId);

	// Search hits usable for a mode: right kind, never qobuz, streaming services only when streaming is allowed.
	[[nodiscard]] bool HitUsable(const App::SearchHit& a_hit, Mode a_mode, bool a_allowStreaming);
	inline constexpr std::size_t kMaxSuggestions = 200;
	[[nodiscard]] std::vector<App::SearchHit> FilterHits(const std::vector<App::SearchHit>& a_hits, Mode a_mode, bool a_allowStreaming,
		std::size_t a_max = kMaxSuggestions);
	// Stores the pick: genre -> selector (id, else title); artist/playlist -> service + id + title as the label.
	// False (no change) when the hit is unusable for the current mode.
	bool ApplyHit(Settings& a_settings, const std::string& a_envId, const App::SearchHit& a_hit);
	// "Title - subtitle [service]" for the result list.
	[[nodiscard]] std::string HitLabel(const App::SearchHit& a_hit);

	// ---- presets (they only set Settings fields; flags and pool size are left as the user has them) --------
	struct PresetEntry
	{
		const char* envId;
		Mode        mode;
		const char* selector;
	};
	struct Preset
	{
		const char*              id;
		const char*              label;
		const char*              description;
		std::vector<PresetEntry> entries;
	};
	[[nodiscard]] const std::vector<Preset>& Presets();
	[[nodiscard]] const Preset* FindPreset(std::string_view a_id);
	// Applies every entry (and drops sub-environment overrides of the groups it names, which would otherwise win).
	// "vanilla" preset: every entry in the settings goes back to Vanilla. False when the id is unknown.
	bool ApplyPreset(Settings& a_settings, std::string_view a_presetId);
	// Genre selectors of the preset that are NOT in `a_genres` (a genre-browse result from the server). Empty list given
	// = nothing known = nothing reported.
	[[nodiscard]] std::vector<std::string> MissingPresetGenres(const Preset& a_preset, const std::vector<App::SearchHit>& a_genres);

	// ---- first run --------------------------------------------------------------------------------------
	struct FirstRun
	{
		bool step1Done = false;  // server URL and key present
		bool step2Done = false;  // connection test passed
		bool step3Done = false;  // at least one environment is not Vanilla
		int  current   = 0;      // 0..2 = first unfinished step, 3 = all done
	};
	// Step 3 = a real selection exists (not Vanilla, selector filled, artist/playlist linked) and, when `a_status` is given,
	// the core does not report every active environment as empty.
	[[nodiscard]] FirstRun ComputeFirstRun(const Settings& a_settings, const App::Connection& a_conn,
		const std::vector<App::EnvStatus>* a_status = nullptr);

	// ---- status text ------------------------------------------------------------------------------------
	[[nodiscard]] const App::EnvStatus* FindStatus(const std::vector<App::EnvStatus>& a_all, std::string_view a_envId);
	// "2 of 3 ready - Filling pool (2 of 3)"; with nowPlaying: "... - now: Artist - Title". No status: "not reported yet".
	[[nodiscard]] std::string FormatPoolLine(const App::EnvStatus* a_status);
	[[nodiscard]] std::string FormatBytes(std::uint64_t a_bytes);
	[[nodiscard]] const char* ConnStateLabel(App::ConnState a_state);

	// Cache cap choices (MB) offered by the combo; the current value is shown even when it is not in the list.
	[[nodiscard]] const std::vector<int>& CacheCapChoicesMB();
	[[nodiscard]] std::string FormatCacheCap(int a_mb);

	// ---- log level / features ---------------------------------------------------------------------------
	[[nodiscard]] const std::vector<mj::config::LogLevel>& LogLevelChoices();

	// ---- coalescing apply worker ------------------------------------------------------------------------
	// Runs `job` (Save + ApplySettingsChanged) on its own thread so the render thread never touches the disk.
	// Request() is cheap and non-blocking; requests made while the job runs collapse into ONE more run.
	// The job returns an error message ("" = fine) which LastError() hands back to the page.
	class ApplyQueue
	{
	public:
		explicit ApplyQueue(std::function<std::string()> a_job);
		~ApplyQueue();
		ApplyQueue(const ApplyQueue&) = delete;
		ApplyQueue& operator=(const ApplyQueue&) = delete;

		void Request();
		[[nodiscard]] std::string LastError() const;
		[[nodiscard]] std::uint64_t Completed() const;  // finished job runs (tests)
		// Blocks until nothing is pending or running (tests only; the page never calls this).
		void WaitIdle();

	private:
		void Run();

		std::function<std::string()> job_;
		mutable std::mutex           mutex_;
		std::condition_variable      cv_;
		bool                         pending_ = false;
		bool                         running_ = false;
		bool                         stop_    = false;
		std::string                  lastError_;
		std::uint64_t                completed_ = 0;
		std::thread                  thread_;
	};
}
