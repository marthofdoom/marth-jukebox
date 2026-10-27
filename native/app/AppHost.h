#pragma once

// Plugin-side entry points of the core (plugin.cpp only). The menu uses app/AppApi.h, never this header.
#include "config/Settings.h"

namespace MJ::App {

    // At plugin load: creates the settings store (no file IO yet) so the menu can be handed Store() early.
    void Init();

    // kDataLoaded (game thread): probe + retarget setup, starts the pool/slot/UI workers (the pool thread
    // loads the INI). No network or file IO on the game thread.
    void OnDataLoaded();

    // SKSE messages after load (kPostLoadGame, kNewGame): state log only.
    void OnGameLoaded(const char* a_why);

    // The process-lifetime settings store (never null after Init). For MJ::Menu::SetStore at integration.
    mj::config::SettingsStore* Store();

}
