// Test-only implementation of app/AppApi.h for mj_menu_tests. NEVER linked into the DLL (the real App.cpp comes from the
// core-playback branch; the DLL uses it or WeakAppFallback.cpp). Records calls so tests can assert what the menu asked for.
#include "menu/StubApp.h"

#include <mutex>

namespace MJ::App
{
	namespace
	{
		std::mutex g_mutex;
	}

	namespace Stub
	{
		Recorder& Rec()
		{
			static Recorder r;
			return r;
		}
		void Reset()
		{
			std::lock_guard lk(g_mutex);
			Rec() = Recorder{};
		}
	}

	void TestConnectionAsync() { std::lock_guard lk(g_mutex); ++Stub::Rec().testConnection; }
	Connection GetConnection() { std::lock_guard lk(g_mutex); return Stub::Rec().connection; }
	void SearchAsync(const std::string& k, const std::string& t, bool s)
	{
		std::lock_guard lk(g_mutex);
		Stub::Rec().searches.push_back({ k, t, s });
	}
	SearchState GetSearch() { std::lock_guard lk(g_mutex); return Stub::Rec().search; }
	std::vector<EnvStatus> GetEnvStatus() { std::lock_guard lk(g_mutex); return Stub::Rec().status; }
	void ApplySettingsChanged() { std::lock_guard lk(g_mutex); ++Stub::Rec().applied; }
	void ResetEnvironmentToVanilla(const std::string& id) { std::lock_guard lk(g_mutex); Stub::Rec().resets.push_back(id); }
	void ClearCacheAsync() { std::lock_guard lk(g_mutex); ++Stub::Rec().clearCache; }
	std::uint64_t CacheBytes() { std::lock_guard lk(g_mutex); return Stub::Rec().cacheBytes; }
	void PeekAsync(const std::string& id) { std::lock_guard lk(g_mutex); Stub::Rec().peeks.push_back(id); }
	std::vector<std::string> GetPeek(const std::string&) { std::lock_guard lk(g_mutex); return Stub::Rec().peek; }
}
