#pragma once

// Settings page for the SKSE Menu Framework (docs/INTEGRATION-1.0.md section B, docs/MENU-FRAMEWORK.md).
// Public surface of native/menu/: two functions.

namespace mj::config
{
	class SettingsStore;
}

namespace MJ::Menu
{
	// Registers the page with the framework (call once, from the kDataLoaded handler: every plugin DLL is mapped by then,
	// so the header's cached function pointers cannot be null because of load order). Logs the framework version. If the
	// framework is missing the page is simply not registered and the plugin runs headless on its INI.
	void Register();

	// The settings store the page edits. Call once after SettingsStore::Load() (the playback core owns the store).
	// Until then the page shows "settings not loaded yet". The store must outlive the process; pass nullptr to detach.
	void SetStore(mj::config::SettingsStore* a_store);
}
