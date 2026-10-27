// marth's Jukebox: plays Skyrim music from a Harmony server (DESIGN.md, docs/INTEGRATION-1.0.md).
// Game thread: probe + retarget (retarget/ is the only engine-memory writer). Workers: pool/ (Harmony, cache,
// slot files) and app/ (connection test, search, peek). No hooks.
#include "PCH.h"
#include "app/AppHost.h"
#include "menu/Menu.h"

namespace {

    void SetupLog() {
        auto dir = SKSE::log::log_directory();
        if (!dir) return;   // no log is survivable; a crash here is not

        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        try {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "MarthJukebox.log").string(), true);
        } catch (const spdlog::spdlog_ex&) {
            return;
        }

        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);  // the pool thread applies [General] LogLevel once the INI is loaded
        // Flush every line: a CTD must not eat the last second of the log.
        log->flush_on(spdlog::level::trace);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) return;
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            spdlog::info("[startup] kDataLoaded");
            MJ::App::OnDataLoaded();
            MJ::Menu::Register();           // settings page; the store is attached in SKSEPluginLoad, the page waits for the INI load
            break;
        case SKSE::MessagingInterface::kNewGame:
            MJ::App::OnGameLoaded("kNewGame");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            MJ::App::OnGameLoaded("kPostLoadGame");
            break;
        default:
            break;
        }
    }

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto  ver    = plugin->GetVersion();
    spdlog::info("=== MarthJukebox {}.{}.{} loading -- game {} ===", ver.major(), ver.minor(), ver.patch(),
                 REL::Module::get().version().string());

    MJ::App::Init();  // settings store object only; the INI is read by the pool thread after kDataLoaded
    MJ::Menu::SetStore(MJ::App::Store());  // the page treats Revision()==0 (INI not loaded yet) as 'not loaded', so it cannot save defaults over the user's INI
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    spdlog::info("=== MarthJukebox loaded ===");
    return true;
}
