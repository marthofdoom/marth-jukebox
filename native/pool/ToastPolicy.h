#pragma once

// When to show an in-game notification about Harmony's reachability (marth 2026-10-27: "a Harmony is down
// toast every few minutes"). Pure state machine with an injected clock (ms); no engine, no threads: the pool
// thread feeds it and queues the returned text, the game thread only shows it (app/App.cpp).
//
//  - Down: after 2 consecutive network failures, or failures for >= 20 s (a single blip stays silent), while at
//    least one environment is active: one toast, then again every `intervalMs` while it stays down.
//  - Back: exactly one toast when a request succeeds after a down toast was shown.
//  - A refused key (401): "Harmony rejected the key", once per settings change.
//  - The game source switched off on the server (403 / status enabled=false) counts as NOT REACHABLE (the repeating
//    toast). A malformed server address gives no toast (the menu shows it; nothing is ever sent).
//  - Our own cancel (settings change, kill switch, shutdown) is not a failure: OnOutcome(kCancelled) does nothing.
//  - ToastEnabled=0 or no server configured: never a toast.

#include <optional>
#include <string>

namespace mj::pool
{
	inline constexpr const char* kToastDown = "Jukebox: Harmony server not reachable - playing saved/vanilla music";
	inline constexpr const char* kToastBack = "Jukebox: Harmony is back";
	inline constexpr const char* kToastAuth = "Jukebox: Harmony rejected the key";

	class ToastPolicy
	{
	public:
		struct Config
		{
			bool      enabled    = true;   // [General] ToastEnabled
			bool      configured = false;  // a server URL is set and the network switch is on
			long long intervalMs = 5 * 60'000;
		};

		// Settings changed: new config, and the auth toast may be shown again. Down/back state is kept.
		void Configure(const Config& c);

		// Whether at least one environment is active (bound or planned to play from Harmony).
		void SetActive(bool active) { active_ = active; }

		enum class Outcome { Success, NetworkFailure, AuthFailure, Cancelled, Other };
		// One entry point for any request result: Cancelled and Other change nothing.
		std::optional<std::string> OnOutcome(Outcome o, long long nowMs);

		std::optional<std::string> OnSuccess(long long nowMs);
		std::optional<std::string> OnNetworkFailure(long long nowMs);  // unreachable, timeout, refused, bad status
		std::optional<std::string> OnAuthFailure(long long nowMs);     // 401 / 403 / rejected configuration

		// Call regularly (the pool thread's loop): repeat toasts, and a deferred first toast once 20 s passed.
		std::optional<std::string> Tick(long long nowMs);

		[[nodiscard]] bool Down() const { return failures_ > 0; }

	private:
		[[nodiscard]] bool Allowed() const { return cfg_.enabled && cfg_.configured; }

		Config    cfg_;
		bool      active_      = false;
		int       failures_    = 0;      // consecutive network failures
		long long failSinceMs_ = 0;
		bool      announced_   = false;  // a down toast was shown for this outage
		long long lastToastMs_ = 0;
		bool      authShown_   = false;
	};
}
