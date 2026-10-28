// Slot file protocol. See SlotWriter.h. All file IO; worker threads only. No engine memory.
#include "PCH.h"
#include "slots/SlotWriter.h"

#include "pool/Audio.h"
#include "pool/PoolModel.h"
#include "retarget/SlotAssign.h"

// After the CommonLib PCH; WIN32_LEAN_AND_MEAN and NOMINMAX come from CMakePresets.json.
#include <windows.h>

#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

namespace MJ::Slots {

    namespace {

        namespace fs = std::filesystem;

        struct Entry {
            std::set<std::uint64_t> sizes;  // sizes the plugin wrote (two while a rename is pending)
            std::string             env, key, file;
        };

        std::mutex                   g_mx;  // guards g_manifest (the slot thread is the only writer; cheap anyway)
        std::map<std::string, Entry> g_manifest;  // slot file name -> record
        bool                         g_loaded{ false };

        fs::path ManifestPath() { return PluginDir() / L"MarthJukebox" / L"slots.json"; }

        fs::path SlotPath(int a_slot) { return HarmonyDir() / mj::retarget::SlotFileName(a_slot); }
        fs::path TmpPath(int a_slot) {
            fs::path t = SlotPath(a_slot);
            t += L".tmp";
            return t;
        }

        bool FileSize(const fs::path& a_p, std::uint64_t& a_size) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(a_p.c_str(), GetFileExInfoStandard, &fad)) return false;
            a_size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            return true;
        }

        // Caller holds g_mx. Written via .tmp + replace; a failure is logged and returned (the commit that needed it
        // is then refused, so an unrecorded slot file can never appear).
        bool SaveManifestLocked() {
            nlohmann::json j;
            j["version"] = 2;
            auto& slots  = j["slots"];
            slots        = nlohmann::json::object();
            for (const auto& [name, e] : g_manifest) {
                slots[name] = { { "sizes", std::vector<std::uint64_t>(e.sizes.begin(), e.sizes.end()) },
                                { "env", e.env },
                                { "key", e.key },
                                { "file", e.file } };
            }
            const std::string text = j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
            const fs::path    dst  = ManifestPath();
            std::error_code   ec;
            fs::create_directories(dst.parent_path(), ec);
            fs::path tmp = dst;
            tmp += L".tmp";
            HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                spdlog::error("[slots] cannot create {} (Win32 error {})", Utf8(tmp), ::GetLastError());
                return false;
            }
            DWORD wrote = 0;
            bool  ok    = ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size() &&
                      ::FlushFileBuffers(h);
            ::CloseHandle(h);
            if (!ok || !::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                spdlog::error("[slots] cannot write {} (Win32 error {})", Utf8(dst), ::GetLastError());
                ::DeleteFileW(tmp.c_str());
                return false;
            }
            return true;
        }

        long long MsSince(std::chrono::steady_clock::time_point a_t) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - a_t).count();
        }

    }  // namespace

    std::string Utf8(const fs::path& a_p) {
        const std::wstring& w = a_p.native();
        if (w.empty()) return {};
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
        if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    fs::path DataDir() {
        static const fs::path dir = [] {
            std::wstring buf(MAX_PATH, L'\0');
            for (;;) {
                const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
                if (n == 0) return fs::path(L"Data");  // relative to the CWD (the game folder)
                if (n < buf.size()) {
                    buf.resize(n);
                    break;
                }
                buf.resize(buf.size() * 2);
            }
            return fs::path(buf).parent_path() / L"Data";
        }();
        return dir;
    }

    fs::path HarmonyDir() { return DataDir() / L"Music" / L"Harmony"; }
    fs::path PluginDir() { return DataDir() / L"SKSE" / L"Plugins"; }

    bool KillSwitchFilePresent() {
        const DWORD a = ::GetFileAttributesW((PluginDir() / L"MarthJukebox.noretarget").c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    namespace {
        // Caller holds g_mx. Reads slots.json into g_manifest once per process.
        void ReadManifestOnceLocked() {
            if (g_loaded) return;
            g_loaded         = true;
            const fs::path p = ManifestPath();
            HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                spdlog::info("[slots] no slot manifest yet ({}): the plugin has not written a slot file", Utf8(p));
                return;
            }
            std::string   text;
            LARGE_INTEGER sz{};
            if (::GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 20)) {
                text.resize(static_cast<std::size_t>(sz.QuadPart));
                DWORD got = 0;
                if (!::ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
                text.resize(got);
            }
            ::CloseHandle(h);
            const auto j = nlohmann::json::parse(text, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("slots") || !j["slots"].is_object()) {
                spdlog::error("[slots] slot manifest {} is unreadable: every existing slot file is treated as NOT ours (delete the "
                              "slot_NN.wav files in Data\\Music\\Harmony to let the plugin use them again)",
                              Utf8(p));
                return;
            }
            auto str = [](const nlohmann::json& a_o, const char* a_k) {
                const auto it = a_o.find(a_k);
                return it != a_o.end() && it->is_string() ? it->get<std::string>() : std::string();
            };
            for (const auto& [name, rec] : j["slots"].items()) {
                Entry e;
                if (rec.is_array()) {  // version 1: sizes only
                    for (const auto& v : rec)
                        if (v.is_number_unsigned()) e.sizes.insert(v.get<std::uint64_t>());
                } else if (rec.is_object()) {
                    if (const auto it = rec.find("sizes"); it != rec.end() && it->is_array()) {
                        for (const auto& v : *it)
                            if (v.is_number_unsigned()) e.sizes.insert(v.get<std::uint64_t>());
                    }
                    e.env  = str(rec, "env");
                    e.key  = str(rec, "key");
                    e.file = str(rec, "file");
                    if (!mj::pool::IsCacheFileName(e.file)) e.file.clear();
                }
                g_manifest[name] = e;
            }
        }

        // 1 s of digital silence, canonical 44-byte header, 44.1 kHz 16-bit stereo (the only format the slot accepts).
        std::vector<std::uint8_t> SilentWav() {
            constexpr std::uint32_t kData = 44100u * 4u;
            const auto              hdr   = mj::pool::CanonicalWavHeader({ 44100, 2, 16 }, kData);
            std::vector<std::uint8_t> b(hdr.begin(), hdr.end());
            b.resize(b.size() + kData, 0);
            return b;
        }
    }

    std::map<int, SlotRecord> LoadManifest() {
        std::map<int, SlotRecord> out;
        std::scoped_lock          lk(g_mx);
        ReadManifestOnceLocked();
        // Slots whose file still has the single recorded size and a recorded track: the content is known.
        for (int s = 0; s < mj::retarget::kSlotCount; ++s) {
            const auto it = g_manifest.find(mj::retarget::SlotFileName(s));
            if (it == g_manifest.end() || it->second.sizes.size() != 1 || it->second.key.empty() || it->second.env.empty() ||
                it->second.file.empty())
                continue;
            std::uint64_t cur = 0;
            if (FileSize(SlotPath(s), cur) && cur == *it->second.sizes.begin())
                out[s] = { it->second.env, it->second.key, it->second.file, cur };
        }
        spdlog::info("[slots] slot manifest: {} slot file(s) recorded as written by the plugin, {} with a known track", g_manifest.size(),
                     out.size());
        return out;
    }

    void EnsurePlaceholders() {
        // Field log 2026-10-27 (Deck): every slot file the plugin CREATED during the session failed to play instantly (a new
        // sound handle every tick, the engine skipping to the next entry), while the M0b/M0c slot files, which existed when
        // the game started, played. So every slot file must exist before the engine starts: each missing one is created
        // here, at plugin load (before the game initialises its resources), as 1 s of silence; every real fill is then a
        // rename over an existing file, exactly the M0c case. Placeholders are never bound to a music type (only a slot
        // holding a pool track is), so they are never heard.
        const auto       t0 = std::chrono::steady_clock::now();
        std::scoped_lock lk(g_mx);
        ReadManifestOnceLocked();
        std::error_code ec;
        fs::create_directories(HarmonyDir(), ec);
        const auto silent  = SilentWav();
        int        existed = 0, created = 0, failed = 0;
        std::string missingAtStart;
        for (int s = 0; s < mj::retarget::kSlotCount; ++s) {
            const auto    dst = SlotPath(s);
            std::uint64_t cur = 0;
            if (FileSize(dst, cur)) {
                ++existed;
                continue;
            }
            missingAtStart += (missingAtStart.empty() ? "" : ",") + std::format("{:02}", s);
            fs::path tmp = dst;
            tmp += L".tmp";
            HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            bool   ok = h != INVALID_HANDLE_VALUE;
            if (ok) {
                DWORD wrote = 0;
                ok = ::WriteFile(h, silent.data(), static_cast<DWORD>(silent.size()), &wrote, nullptr) && wrote == silent.size() &&
                     ::FlushFileBuffers(h);
                ::CloseHandle(h);
            }
            // CREATE-only rename (no REPLACE_EXISTING): never overwrites a file that appeared meanwhile.
            ok = ok && ::MoveFileExW(tmp.c_str(), dst.c_str(), 0);
            if (!ok) {
                ++failed;
                ::DeleteFileW(tmp.c_str());
                continue;
            }
            auto& e = g_manifest[mj::retarget::SlotFileName(s)];
            e       = Entry{};
            e.sizes.insert(silent.size());
            ++created;
        }
        if (created > 0 && !SaveManifestLocked()) {
            spdlog::error("[slots] placeholder slot files created but slots.json could not be written");
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (created > 0 || failed > 0)
            spdlog::warn("[slots] {} slot file(s) did not exist at game start ({}): created {} silent placeholder(s), {} failed ({} ms). "
                         "Files created later in a session are not played by the game.",
                         created + failed, missingAtStart, created, failed, ms);
        else
            spdlog::info("[slots] all {} slot files existed at game start ({} ms)", existed, ms);
    }

    void CleanTempFiles() {
        for (int s = 0; s < mj::retarget::kSlotCount; ++s) {
            const fs::path tmp = TmpPath(s);
            if (::DeleteFileW(tmp.c_str())) spdlog::info("[slots] removed a stale {}", Utf8(tmp));
        }
    }

    void DiscardTmp(int a_slot) { ::DeleteFileW(TmpPath(a_slot).c_str()); }

    PrepResult PrepareSlot(int a_slot, const fs::path& a_src, std::stop_token a_st) {
        PrepResult r;
        const auto t0  = std::chrono::steady_clock::now();
        const auto dst = SlotPath(a_slot);
        const auto tmp = TmpPath(a_slot);
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);

        // 1. Ownership of an existing slot file (before any work).
        std::uint64_t cur = 0;
        if (FileSize(dst, cur)) {
            std::scoped_lock lk(g_mx);
            const auto       it = g_manifest.find(mj::retarget::SlotFileName(a_slot));
            if (it == g_manifest.end() || !it->second.sizes.contains(cur)) {
                r.error = std::format("{} exists ({} bytes) but was not written by this plugin; NOT overwritten (delete it by hand to let "
                                      "the plugin use slot {:02})",
                                      Utf8(dst), cur, a_slot);
                return r;
            }
        }
        // 2. Decode / copy into the .tmp as a canonical WAV, flushed to disk.
        const auto cr = mj::pool::ConvertToCanonicalWav(a_src, tmp, a_st);
        if (!cr.ok) {
            ::DeleteFileW(tmp.c_str());
            r.sourceError = cr.sourceError;
            r.error       = std::format("preparing {} failed: {}", Utf8(tmp), cr.error);
            return r;
        }
        // 3. Size check as the file system reports it.
        std::uint64_t got = 0;
        if (!FileSize(tmp, got) || got != cr.bytesWritten) {
            ::DeleteFileW(tmp.c_str());
            r.error = std::format("{} is {} bytes after the flush, expected {}", Utf8(tmp), got, cr.bytesWritten);
            return r;
        }
        r.ok    = true;
        r.bytes = got;
        r.ms    = MsSince(t0);
        return r;
    }

    CommitResult CommitSlot(int a_slot, std::uint64_t a_bytes, bool a_firstAttempt, const std::string& a_env, const std::string& a_key,
                            const std::string& a_file) {
        CommitResult r;
        const auto   name = mj::retarget::SlotFileName(a_slot);
        const auto   dst  = SlotPath(a_slot);
        const auto   tmp  = TmpPath(a_slot);
        // Record the new size BEFORE the first rename attempt (both sizes acceptable until it succeeded), so a crash
        // between the two never leaves a slot file the next session would refuse. Not repeated on retries.
        if (a_firstAttempt) {
            std::uint64_t    cur = 0;
            const bool       exists = FileSize(dst, cur);
            std::scoped_lock lk(g_mx);
            auto&            e = g_manifest[name];
            e.sizes.clear();
            if (exists) e.sizes.insert(cur);
            e.sizes.insert(a_bytes);
            e.key.clear();  // unknown until the rename succeeded
            if (!SaveManifestLocked()) {
                r.error = "the slot manifest could not be written; slot not replaced";
                return r;
            }
        }
        if (!::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            const DWORD err = ::GetLastError();
            r.retryable     = err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION || err == ERROR_ACCESS_DENIED;
            r.accessDenied  = err == ERROR_ACCESS_DENIED;
            r.error         = std::format("rename {} -> {} failed, Win32 error {}{}", Utf8(tmp), Utf8(dst), err,
                                          err == ERROR_SHARING_VIOLATION ? " (sharing violation: the engine holds the file)" : "");
            return r;
        }
        {
            std::scoped_lock lk(g_mx);
            auto&            e = g_manifest[name];
            e.sizes.clear();
            e.sizes.insert(a_bytes);
            e.env  = a_env;
            e.key  = a_key;
            e.file = a_file;
            (void)SaveManifestLocked();  // failure logged; the previous record still lists the size
        }
        r.ok = true;
        return r;
    }

}  // namespace MJ::Slots
