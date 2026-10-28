#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <stop_token>
#include <string>

// Slot file protocol (docs/INTEGRATION-1.0.md A.3, docs/M0C-RESULTS.md). Worker threads only: every function
// here does file IO except the path helpers. Nothing here reads or writes engine memory.
//
// A slot file is Data\Music\Harmony\slot_NN.wav, the ANAM of wrapper MJ_SLOT_NN in MarthJukebox.esp. It is
// replaced in two steps:
//   PrepareSlot: decode/copy into slot_NN.wav.tmp (canonical PCM WAV) -> flush -> size check;
//   CommitSlot : record the size in the manifest (once per prepared file) -> MoveFileExW(REPLACE_EXISTING) over
//                the (possibly playing) slot file.
// M0c proved the rename over a PLAYING file on Proton + MO2/USVFS (9 of 9). On Windows it may fail with a sharing
// violation: the .tmp is then KEPT and only the rename is retried later (the caller decides when; nothing is
// forced, nothing is re-decoded).
//
// Ownership: the plugin only ever replaces a slot file it wrote itself. Data\SKSE\Plugins\MarthJukebox\slots.json
// records, per slot file, the sizes the plugin produced and which track (environment + key + cache file) it holds;
// an existing slot file whose size is not recorded is never overwritten (loud error: delete it by hand).
namespace MJ::Slots {

    // "<directory of the game exe>\Data". Absolute (MO2's USVFS maps it for the engine and for us alike).
    std::filesystem::path DataDir();
    std::filesystem::path HarmonyDir();   // Data\Music\Harmony
    std::filesystem::path PluginDir();    // Data\SKSE\Plugins

    // Data\SKSE\Plugins\MarthJukebox.noretarget exists (stat). Worker threads or load time.
    bool KillSwitchFilePresent();

    // What the manifest says a slot file holds, if its current size matches the record.
    struct SlotRecord {
        std::string   env, key, file;
        std::uint64_t bytes = 0;
    };

    // Reads the ownership manifest (once per process) and returns the slots whose file on disk still has the recorded size
    // and a recorded track (A-F7: they need no rewrite at start). Worker only.
    std::map<int, SlotRecord> LoadManifest();

    // Plugin load (game thread, BEFORE the engine initialises; the only file IO done there): creates every missing
    // slot_NN.wav as 1 s of silence and records it in the manifest, so the game knows all slot files from its start
    // (Deck field log: slot files created during the session were never played). Never overwrites an existing file.
    void EnsurePlaceholders();

    struct PrepResult {
        bool          ok = false;
        bool          sourceError = false;  // the cache file is bad (drop it); false = our side failed
        std::string   error;
        std::uint64_t bytes = 0;
        long long     ms = 0;
    };
    // Writes slot_NN.wav.tmp from `a_src` (WAV or FLAC cache file). Refuses when the existing slot file is not ours.
    PrepResult PrepareSlot(int a_slot, const std::filesystem::path& a_src, std::stop_token a_st);

    struct CommitResult {
        bool        ok = false;
        bool        retryable = false;     // sharing / lock violation: keep the .tmp, try the rename again later
        bool        accessDenied = false;  // ERROR_ACCESS_DENIED (retryable only a few times)
        std::string error;
    };
    // Renames the prepared .tmp over the slot file. `a_firstAttempt` records the new size in the manifest first (not
    // repeated on retries). On success the manifest records the track (env, key, cache file).
    CommitResult CommitSlot(int a_slot, std::uint64_t a_bytes, bool a_firstAttempt, const std::string& a_env, const std::string& a_key,
                            const std::string& a_file);

    // Deletes the slot's .tmp (abandoned preparation).
    void DiscardTmp(int a_slot);

    // Removes stale slot_NN.wav.tmp files left by a crash (they are ours by name and never played). Worker only.
    void CleanTempFiles();

    std::string Utf8(const std::filesystem::path& a_p);

    // Field #4 (Deck stutter): a worker thread runs below normal priority. Logged; failure is harmless.
    void LowerCurrentThreadPriority(const char* a_name);

    // Review R5-2: background (very low) IO priority ONLY around a lock-free, log-free section (the decode + write of a
    // slot file): a thread in background mode must never hold s.mx or the log sink, or the game thread could wait on it.
    struct BackgroundIo {
        BackgroundIo();
        ~BackgroundIo();
        BackgroundIo(const BackgroundIo&)            = delete;
        BackgroundIo& operator=(const BackgroundIo&) = delete;
        bool on = false;
    };

}
