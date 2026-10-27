#pragma once

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>

// Slot file protocol (docs/INTEGRATION-1.0.md A.3, docs/M0C-RESULTS.md). Worker threads only: every function
// here does file IO except the path helpers. Nothing here reads or writes engine memory.
//
// A slot file is Data\Music\Harmony\slot_NN.wav, the ANAM of wrapper MJ_SLOT_NN in MarthJukebox.esp. It is
// replaced by: decode/copy into slot_NN.wav.tmp (canonical PCM WAV) -> flush -> size check -> ownership check ->
// MoveFileExW(REPLACE_EXISTING) over the (possibly playing) file. M0c proved the rename over a PLAYING file on
// Proton + MO2/USVFS (9 of 9); on Windows it may fail with a sharing violation, which is returned as an error
// (the caller retries later; nothing is forced).
//
// Ownership: the plugin only ever replaces a slot file it wrote itself. Data\SKSE\Plugins\MarthJukebox\slots.json
// records the size of every slot file the plugin produced; an existing slot file whose size is not recorded
// is never overwritten (loud error: delete the file by hand to let the plugin use that slot).
namespace MJ::Slots {

    // "<directory of the game exe>\Data". Absolute (MO2's USVFS maps it for the engine and for us alike).
    std::filesystem::path DataDir();
    std::filesystem::path HarmonyDir();   // Data\Music\Harmony
    std::filesystem::path PluginDir();    // Data\SKSE\Plugins

    // Data\SKSE\Plugins\MarthJukebox.noretarget exists (stat). Worker threads or load time.
    bool KillSwitchFilePresent();

    struct WriteResult {
        bool          ok = false;
        bool          retryable = false;  // e.g. a sharing violation on the rename: try again later
        std::string   error;              // plain text
        std::uint64_t bytes = 0;
        long long     ms = 0;
    };

    // Reads the ownership manifest (once, before the first WriteSlot). Worker only.
    void LoadManifest();

    // Writes `a_src` (a validated cache file, WAV or FLAC) into slot `a_slot` by the protocol above. Worker only;
    // one thread at a time (the slot filler).
    WriteResult WriteSlot(int a_slot, const std::filesystem::path& a_src, std::stop_token a_st);

    // Removes stale slot_NN.wav.tmp files left by a crash (they are ours by name and never played). Worker only.
    void CleanTempFiles();

    std::string Utf8(const std::filesystem::path& a_p);

}
