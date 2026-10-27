// Harmony reachability toasts. See ToastPolicy.h.
#include "pool/ToastPolicy.h"

namespace mj::pool
{
	namespace
	{
		constexpr int       kConfirmFailures = 2;
		constexpr long long kConfirmMs       = 20'000;
	}

	void ToastPolicy::Configure(const Config& c)
	{
		cfg_       = c;
		authShown_ = false;
		if (!c.configured) {  // no server: forget any outage, nothing to report
			failures_  = 0;
			announced_ = false;
		}
	}

	std::optional<std::string> ToastPolicy::OnOutcome(Outcome o, long long nowMs)
	{
		switch (o) {
		case Outcome::Success:        return OnSuccess(nowMs);
		case Outcome::NetworkFailure: return OnNetworkFailure(nowMs);
		case Outcome::AuthFailure:    return OnAuthFailure(nowMs);
		default:                      return std::nullopt;  // a cancel or a per-request problem is not an outage
		}
	}

	std::optional<std::string> ToastPolicy::OnSuccess(long long)
	{
		const bool wasAnnounced = announced_;
		failures_               = 0;
		announced_              = false;
		if (wasAnnounced && Allowed()) {
			return std::string(kToastBack);
		}
		return std::nullopt;
	}

	std::optional<std::string> ToastPolicy::OnNetworkFailure(long long nowMs)
	{
		if (failures_++ == 0) {
			failSinceMs_ = nowMs;
		}
		return Tick(nowMs);
	}

	std::optional<std::string> ToastPolicy::OnAuthFailure(long long)
	{
		// Not an outage: the server answered. Reset the down state silently.
		failures_  = 0;
		announced_ = false;
		if (authShown_ || !Allowed()) {
			return std::nullopt;
		}
		authShown_ = true;
		return std::string(kToastAuth);
	}

	std::optional<std::string> ToastPolicy::Tick(long long nowMs)
	{
		if (failures_ == 0 || !Allowed() || !active_) {
			return std::nullopt;
		}
		const bool confirmed = failures_ >= kConfirmFailures || nowMs - failSinceMs_ >= kConfirmMs;
		if (!confirmed) {
			return std::nullopt;
		}
		if (!announced_ || nowMs - lastToastMs_ >= cfg_.intervalMs) {
			announced_   = true;
			lastToastMs_ = nowMs;
			return std::string(kToastDown);
		}
		return std::nullopt;
	}
}
