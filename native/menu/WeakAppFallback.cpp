// TEMPORARY LINK FALLBACK -- delete this file when native/app/App.cpp (core-playback) is merged.
//
// The menu page calls the functions declared in app/AppApi.h. Until the playback core provides them (native/app/App.cpp),
// the DLL of the menu-page branch still has to link, so this file defines every one of them as an inert "core not built in"
// stub. native/menu/menu.cmake adds this file ONLY when native/app/App.cpp does not exist, so merging the core branch
// switches it off automatically; deleting the file afterwards is cosmetic. Never compiled into any test target
// (those use StubApp.cpp).
#include "app/AppApi.h"

namespace MJ::App
{
	void TestConnectionAsync() {}
	Connection GetConnection()
	{
		Connection c;
		c.message = "This build has no playback core yet.";
		return c;
	}
	void SearchAsync(const std::string&, const std::string&, bool) {}
	SearchState GetSearch() { return {}; }
	std::vector<EnvStatus> GetEnvStatus() { return {}; }
	void ApplySettingsChanged() {}
	void ResetEnvironmentToVanilla(const std::string&) {}
	void ClearCacheAsync() {}
	std::uint64_t CacheBytes() { return 0; }
	void PeekAsync(const std::string&) {}
	std::vector<std::string> GetPeek(const std::string&) { return {}; }
}
