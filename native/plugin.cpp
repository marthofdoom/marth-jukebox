// marth-jukebox M0a: passive probe. No hooks, no engine writes. See DESIGN.md.
#include "PCH.h"
#include "probe/Probe.h"

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
        log->set_level(spdlog::level::info);
        // Flush every line: a CTD must not eat the last second of the log.
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) return;
        if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
            spdlog::info("[startup] kDataLoaded");
            MJ::Probe::OnDataLoaded();
        }
    }

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto  ver    = plugin->GetVersion();
    spdlog::info("=== MarthJukebox {}.{}.{} (M0a passive probe) loading -- game {} ===",
                 ver.major(), ver.minor(), ver.patch(),
                 REL::Module::get().version().string());

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    spdlog::info("=== MarthJukebox loaded ===");
    return true;
}
