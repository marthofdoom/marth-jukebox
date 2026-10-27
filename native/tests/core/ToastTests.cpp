// ToastPolicy tests (pool/ToastPolicy.h): first failure, repeat interval, recovery, disabled, auth once.
#include "TestHarness.h"

#include "pool/ToastPolicy.h"

using namespace mj::pool;

namespace
{
	ToastPolicy Make(bool enabled = true, long long intervalMs = 300'000)
	{
		ToastPolicy p;
		p.Configure({ enabled, true, intervalMs });
		p.SetActive(true);
		return p;
	}
}

MJ_TEST(Toast_FirstFailureIsABlipSecondAnnounces)
{
	auto p = Make();
	MJ_CHECK(!p.OnNetworkFailure(1'000));  // one blip: silent
	MJ_CHECK(p.Down());
	const auto t = p.OnNetworkFailure(3'000);
	MJ_CHECK(t && *t == kToastDown);
	MJ_CHECK(!p.OnNetworkFailure(5'000));  // no spam within the interval
	MJ_CHECK(!p.Tick(200'000));
}

MJ_TEST(Toast_TwentySecondsOfOneFailureAnnounces)
{
	auto p = Make();
	MJ_CHECK(!p.OnNetworkFailure(0));
	MJ_CHECK(!p.Tick(19'999));
	const auto t = p.Tick(20'000);
	MJ_CHECK(t && *t == kToastDown);
}

MJ_TEST(Toast_RepeatsEveryIntervalAndOneBackToast)
{
	auto p = Make(true, 60'000);
	(void)p.OnNetworkFailure(0);
	MJ_CHECK(p.OnNetworkFailure(1'000));
	MJ_CHECK(!p.Tick(60'999));
	MJ_CHECK(p.Tick(61'000));
	MJ_CHECK(!p.Tick(61'500));
	MJ_CHECK(p.Tick(121'000));
	const auto back = p.OnSuccess(130'000);
	MJ_CHECK(back && *back == kToastBack);
	MJ_CHECK(!p.OnSuccess(131'000));  // only one
	MJ_CHECK(!p.Tick(500'000));
	// A blip after recovery is silent again.
	MJ_CHECK(!p.OnNetworkFailure(600'000));
	MJ_CHECK(!p.OnSuccess(601'000));  // nothing was announced: no "back" either
}

MJ_TEST(Toast_DisabledInactiveOrUnconfiguredIsSilent)
{
	auto off = Make(false);
	MJ_CHECK(!off.OnNetworkFailure(0) && !off.OnNetworkFailure(1) && !off.Tick(100'000));
	MJ_CHECK(!off.OnAuthFailure(0) && !off.OnSuccess(1));

	auto idle = Make();
	idle.SetActive(false);  // nothing plays from Harmony: no reason to bother the player
	MJ_CHECK(!idle.OnNetworkFailure(0) && !idle.OnNetworkFailure(1));
	idle.SetActive(true);
	MJ_CHECK(idle.Tick(2));  // becomes active while still down: announced then

	ToastPolicy none;
	none.Configure({ true, false, 300'000 });  // no server URL
	none.SetActive(true);
	MJ_CHECK(!none.OnNetworkFailure(0) && !none.OnNetworkFailure(1) && !none.OnAuthFailure(2));
}

// Review R2-2: cancels (settings change, kill switch, a cancelled profile PUT) never count toward an outage.
MJ_TEST(Toast_CancelIsNotAFailure)
{
	auto p = Make();
	MJ_CHECK(!p.OnOutcome(ToastPolicy::Outcome::NetworkFailure, 0));
	MJ_CHECK(!p.OnOutcome(ToastPolicy::Outcome::Cancelled, 1'000));
	MJ_CHECK(!p.OnOutcome(ToastPolicy::Outcome::Cancelled, 2'000));
	MJ_CHECK(!p.OnOutcome(ToastPolicy::Outcome::Other, 3'000));
	MJ_CHECK(!p.Tick(19'000));  // still one failure: a blip
	MJ_CHECK(p.OnOutcome(ToastPolicy::Outcome::NetworkFailure, 4'000));  // the second REAL failure confirms
	auto q = Make();
	for (int i = 0; i < 10; ++i) MJ_CHECK(!q.OnOutcome(ToastPolicy::Outcome::Cancelled, i * 30'000));
	MJ_CHECK(!q.Down());
}

MJ_TEST(Toast_AuthOncePerSettingsChange)
{
	auto p = Make();
	const auto a = p.OnAuthFailure(0);
	MJ_CHECK(a && *a == kToastAuth);
	MJ_CHECK(!p.OnAuthFailure(1'000));
	MJ_CHECK(!p.OnAuthFailure(10'000'000));
	p.Configure({ true, true, 300'000 });  // the player edited the settings
	MJ_CHECK(p.OnAuthFailure(10'000'001));
	// Auth is not an outage: no down/back toasts from it.
	MJ_CHECK(!p.Down() && !p.OnSuccess(10'000'002));
}
