// Slot file protocol. See SlotWriter.h. All file IO; worker threads only. No engine memory.
#include "PCH.h"
#include "slots/SlotWriter.h"

#include "pool/Audio.h"
#include "retarget/SlotAssign.h"

// After the CommonLib PCH; WIN32_LEAN_AND_MEAN and NOMINMAX come from CMakePresets.json.
#include <windows.h>

#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

namespace MJ::Slots {

    namespace {

        namespace fs = std::filesystem;

        std::mutex                                    g_mx;        // guards g_manifest (one writer thread, but cheap)
        std::map<std::string, std::set<std::uint64_t>> g_manifest;  // slot file name -> sizes we wrote
        bool                                          g_loaded{ false };

        fs::path ManifestPath() { return PluginDir() / L"MarthJukebox" / L"slots.json"; }

        bool FileSize(const fs::path& a_p, std::uint64_t& a_size) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(a_p.c_str(), GetFileExInfoStandard, &fad)) return false;
            a_size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            return true;
        }

        // Caller holds g_mx. Written via .tmp + replace; a failure is logged and returned (the slot write that
        // needed it is then refused, so an unrecorded file can never appear).
        bool SaveManifestLocked() {
            nlohmann::json j;
            j["version"] = 1;
            auto& slots  = j["slots"];
            slots        = nlohmann::json::object();
            for (const auto& [name, sizes] : g_manifest) slots[name] = std::vector<std::uint64_t>(sizes.begin(), sizes.end());
            const std::string text = j.dump(1);
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

    void LoadManifest() {
        std::scoped_lock lk(g_mx);
        if (g_loaded) return;
        g_loaded = true;
        const fs::path p = ManifestPath();
        HANDLE         h = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
        for (const auto& [name, sizes] : j["slots"].items()) {
            if (!sizes.is_array()) continue;
            for (const auto& s : sizes) {
                if (s.is_number_unsigned()) g_manifest[name].insert(s.get<std::uint64_t>());
            }
        }
        spdlog::info("[slots] slot manifest: {} slot file(s) recorded as written by the plugin", g_manifest.size());
    }

    void CleanTempFiles() {
        for (int s = 0; s < mj::retarget::kSlotCount; ++s) {
            fs::path tmp = HarmonyDir() / mj::retarget::SlotFileName(s);
            tmp += L".tmp";
            if (::DeleteFileW(tmp.c_str())) spdlog::info("[slots] removed a stale {}", Utf8(tmp));
        }
    }

    WriteResult WriteSlot(int a_slot, const fs::path& a_src, std::stop_token a_st) {
        WriteResult r;
        const auto  t0   = std::chrono::steady_clock::now();
        const auto  name = mj::retarget::SlotFileName(a_slot);
        const auto  dst  = HarmonyDir() / name;
        fs::path    tmp  = dst;
        tmp += L".tmp";
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);

        // 1. Ownership of an existing slot file (before any work).
        std::uint64_t cur = 0;
        const bool    exists = FileSize(dst, cur);
        {
            std::scoped_lock lk(g_mx);
            if (exists) {
                const auto it = g_manifest.find(name);
                if (it == g_manifest.end() || !it->second.contains(cur)) {
                    r.error = std::format("{} exists ({} bytes) but was not written by this plugin; NOT overwritten (delete it by "
                                          "hand to let the plugin use slot {:02})",
                                          Utf8(dst), cur, a_slot);
                    return r;
                }
            }
        }

        // 2. Decode / copy into the .tmp as a canonical WAV, flushed to disk.
        const auto cr = mj::pool::ConvertToCanonicalWav(a_src, tmp, a_st);
        if (!cr.ok) {
            ::DeleteFileW(tmp.c_str());
            r.error     = std::format("preparing {} failed: {}", Utf8(tmp), cr.error);
            r.retryable = cr.error == "cancelled";
            return r;
        }
        // 3. Size check as the file system reports it.
        std::uint64_t got = 0;
        if (!FileSize(tmp, got) || got != cr.bytesWritten) {
            ::DeleteFileW(tmp.c_str());
            r.error = std::format("{} is {} bytes after the flush, expected {}", Utf8(tmp), got, cr.bytesWritten);
            return r;
        }
        // 4. Record the new size BEFORE the rename (both sizes stay acceptable until the rename succeeded), so a
        //    crash between the two never leaves a slot file the next session would refuse.
        {
            std::scoped_lock lk(g_mx);
            auto&            sizes = g_manifest[name];
            sizes.clear();
            if (exists) sizes.insert(cur);
            sizes.insert(got);
            if (!SaveManifestLocked()) {
                ::DeleteFileW(tmp.c_str());
                r.error = "the slot manifest could not be written; slot not replaced";
                return r;
            }
        }
        // 5. Rename over the slot (it may be playing; M0c).
        if (!::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            const DWORD err = ::GetLastError();
            ::DeleteFileW(tmp.c_str());
            r.retryable = err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION;
            r.error     = std::format("rename {} -> {} failed, Win32 error {}{}", Utf8(tmp), Utf8(dst), err,
                                      err == ERROR_SHARING_VIOLATION ? " (sharing violation: the engine holds the file)" : "");
            return r;
        }
        {
            std::scoped_lock lk(g_mx);
            auto&            sizes = g_manifest[name];
            sizes.clear();
            sizes.insert(got);
            (void)SaveManifestLocked();  // failure logged; the previous record still lists `got`
        }
        r.ok    = true;
        r.bytes = got;
        r.ms    = MsSince(t0);
        return r;
    }

}  // namespace MJ::Slots
