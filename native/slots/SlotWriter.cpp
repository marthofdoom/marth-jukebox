// M0b slot protocol test. See SlotWriter.h. All file IO happens on ONE worker thread;
// the main thread only enqueues jobs and publishes slot statuses (atomics). Nothing
// here reads or writes engine memory.
#include "PCH.h"
#include "slots/SlotWriter.h"

// After the CommonLib PCH (as MFO's Board.cpp does via d3d11.h); WIN32_LEAN_AND_MEAN and
// NOMINMAX come from CMakePresets.json.
#include <windows.h>

#include <cmath>
#include <deque>
#include <numbers>

namespace MJ::Slots {

    namespace {

        constexpr const wchar_t* kSlotFile[2] = { L"explore_a.wav", L"explore_b.wav" };
        constexpr const char*    kSlotName[2] = { "A", "B" };

        // Test tone: 15 s, 44.1 kHz, 16-bit, stereo, canonical 44-byte header.
        constexpr std::uint32_t kRate       = 44100;
        constexpr std::uint16_t kChannels   = 2;
        constexpr std::uint16_t kBits       = 16;
        constexpr std::uint32_t kSeconds    = 15;
        constexpr std::uint16_t kBlockAlign = kChannels * (kBits / 8);
        constexpr std::uint32_t kFrames     = kRate * kSeconds;
        constexpr std::uint32_t kDataBytes  = kFrames * kBlockAlign;  // 2,646,000
        constexpr std::uint32_t kFileBytes  = 44 + kDataBytes;        // 2,646,044

        // Slot inner-track status as published by the main thread (MUSIC_STATUS values).
        std::atomic<std::uint32_t> g_status[2]{};
        std::atomic<bool>          g_kill{ false };
        std::atomic<bool>          g_refreshQueued[2]{};

        struct Job {
            enum class Kind { kRefresh, kLockProbe } kind;
            int         slot;
            std::string why;
        };

        std::mutex                  g_mx;  // guards g_jobs only
        std::condition_variable_any g_cv;
        std::deque<Job>             g_jobs;

        const char* StatusName(std::uint32_t a_v) {
            switch (a_v) {
            case 0: return "Inactive";
            case 1: return "Playing";
            case 2: return "Paused";
            case 3: return "Finishing";
            case 4: return "Finished";
            default: return "?";
            }
        }

        // Playing / Paused / Finishing: the engine may hold the file open.
        bool Busy(std::uint32_t a_st) { return a_st == 1 || a_st == 2 || a_st == 3; }

        long long MsSince(std::chrono::steady_clock::time_point a_t) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - a_t).count();
        }

        std::string Utf8(const std::filesystem::path& a_p) {
            const std::wstring& w = a_p.native();
            if (w.empty()) return {};
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
            if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        std::filesystem::path HarmonyDir() { return DataDir() / L"Music" / L"Harmony"; }
        std::filesystem::path KillSwitchPath() { return DataDir() / L"SKSE" / L"Plugins" / L"MarthJukebox.noretarget"; }

        bool FileExists(const std::filesystem::path& a_p) {
            const DWORD a = ::GetFileAttributesW(a_p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }

        // Open for write without writing or truncating, close at once: tells whether the
        // engine holds the file with a share mode that excludes writers.
        void LockProbe(int a_slot, const std::string& a_why) {
            const auto p  = HarmonyDir() / kSlotFile[a_slot];
            const auto t0 = std::chrono::steady_clock::now();
            HANDLE     h  = ::CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                const DWORD err = ::GetLastError();
                spdlog::warn("[slots] lock probe ({}): open {} for write FAILED, Win32 error {} (slot {} status {}) {}ms", a_why, Utf8(p), err,
                             kSlotName[a_slot], StatusName(g_status[a_slot].load()), MsSince(t0));
                return;
            }
            ::CloseHandle(h);
            spdlog::info("[slots] lock probe ({}): open {} for write OK, closed without writing (slot {} status {}) {}ms", a_why, Utf8(p),
                         kSlotName[a_slot], StatusName(g_status[a_slot].load()), MsSince(t0));
        }

        std::vector<std::uint8_t> MakeSineWav(double a_hz) {
            std::vector<std::uint8_t> b(kFileBytes);
            auto put32 = [&](std::size_t a_off, std::uint32_t a_v) { std::memcpy(b.data() + a_off, &a_v, 4); };  // x64: little-endian
            auto put16 = [&](std::size_t a_off, std::uint16_t a_v) { std::memcpy(b.data() + a_off, &a_v, 2); };
            std::memcpy(b.data() + 0, "RIFF", 4);
            put32(4, 36 + kDataBytes);
            std::memcpy(b.data() + 8, "WAVE", 4);
            std::memcpy(b.data() + 12, "fmt ", 4);
            put32(16, 16);
            put16(20, 1);  // PCM
            put16(22, kChannels);
            put32(24, kRate);
            put32(28, kRate * kBlockAlign);
            put16(32, kBlockAlign);
            put16(34, kBits);
            std::memcpy(b.data() + 36, "data", 4);
            put32(40, kDataBytes);

            // -12 dBFS sine with 20 ms linear fade in/out (no clicks).
            constexpr double kAmp  = 0.25 * 32767.0;
            constexpr double kFade = 0.020 * kRate;
            for (std::uint32_t i = 0; i < kFrames; ++i) {
                double env = 1.0;
                if (i < kFade) env = i / kFade;
                if (kFrames - 1 - i < kFade) env = std::min(env, (kFrames - 1 - i) / kFade);
                const auto v = static_cast<std::int16_t>(std::lround(kAmp * env * std::sin(2.0 * std::numbers::pi * a_hz * i / kRate)));
                put16(44 + static_cast<std::size_t>(i) * kBlockAlign, static_cast<std::uint16_t>(v));
                put16(44 + static_cast<std::size_t>(i) * kBlockAlign + 2, static_cast<std::uint16_t>(v));
            }
            return b;
        }

        // n-th refresh of a slot file: backup once, .tmp, flush/close, verify size, rename.
        void Refresh(int a_slot, std::uint32_t a_n) {
            const auto dst  = HarmonyDir() / kSlotFile[a_slot];
            auto       tmp  = dst;
            tmp += L".tmp";
            auto orig = dst;
            orig += L".orig";
            const double hz = 220.0 * (2 + (a_n % 10));  // 440, 660, ... 2420 Hz, then wraps
            const auto   t0 = std::chrono::steady_clock::now();

            if (g_kill.load()) {
                spdlog::info("[slots] refresh #{} of {}: skipped, kill switch", a_n, kSlotName[a_slot]);
                return;
            }
            if (const auto st = g_status[a_slot].load(); Busy(st)) {
                spdlog::warn("[slots] refresh #{} of {}: skipped, slot is {} (never write a busy slot)", a_n, kSlotName[a_slot], StatusName(st));
                return;
            }

            // 1. The slot files are the user's own test music: keep one copy before the first
            //    overwrite (never destroy a user file). No backup -> no overwrite.
            if (!FileExists(orig)) {
                if (!FileExists(dst)) {
                    spdlog::info("[slots] refresh #{} of {}: {} does not exist, nothing to back up", a_n, kSlotName[a_slot], Utf8(dst));
                } else if (!::CopyFileW(dst.c_str(), orig.c_str(), TRUE)) {
                    const DWORD err = ::GetLastError();
                    spdlog::error("[slots] refresh #{} of {}: backup {} -> {} FAILED, Win32 error {}; slot left untouched", a_n,
                                  kSlotName[a_slot], Utf8(dst), Utf8(orig), err);
                    return;
                } else {
                    spdlog::info("[slots] refresh #{} of {}: backed up the original to {} (+{}ms)", a_n, kSlotName[a_slot], Utf8(orig),
                                 MsSince(t0));
                }
            }

            // 2. Generate.
            const auto wav = MakeSineWav(hz);
            spdlog::info("[slots] refresh #{} of {}: generated {:.0f} Hz sine, {} bytes (+{}ms)", a_n, kSlotName[a_slot], hz, wav.size(),
                         MsSince(t0));

            // 3. Write the .tmp beside the slot, flush, close.
            HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                spdlog::error("[slots] refresh #{} of {}: create {} FAILED, Win32 error {}", a_n, kSlotName[a_slot], Utf8(tmp), ::GetLastError());
                return;
            }
            std::size_t off = 0;
            bool        ok  = true;
            while (off < wav.size()) {
                DWORD wrote = 0;
                if (!::WriteFile(h, wav.data() + off, static_cast<DWORD>(wav.size() - off), &wrote, nullptr) || wrote == 0) {
                    spdlog::error("[slots] refresh #{} of {}: write {} FAILED at {} bytes, Win32 error {}", a_n, kSlotName[a_slot], Utf8(tmp), off,
                                  ::GetLastError());
                    ok = false;
                    break;
                }
                off += wrote;
            }
            if (ok && !::FlushFileBuffers(h)) {
                spdlog::error("[slots] refresh #{} of {}: flush {} FAILED, Win32 error {}", a_n, kSlotName[a_slot], Utf8(tmp), ::GetLastError());
                ok = false;
            }
            if (!::CloseHandle(h)) {
                spdlog::error("[slots] refresh #{} of {}: close {} FAILED, Win32 error {}", a_n, kSlotName[a_slot], Utf8(tmp), ::GetLastError());
                ok = false;
            }
            if (!ok) return;
            spdlog::info("[slots] refresh #{} of {}: wrote+flushed+closed {} ({} bytes) (+{}ms)", a_n, kSlotName[a_slot], Utf8(tmp), off,
                         MsSince(t0));

            // 4. Verify the byte count as the file system reports it.
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(tmp.c_str(), GetFileExInfoStandard, &fad)) {
                spdlog::error("[slots] refresh #{} of {}: stat {} FAILED, Win32 error {}", a_n, kSlotName[a_slot], Utf8(tmp), ::GetLastError());
                return;
            }
            const std::uint64_t size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            if (size != kFileBytes) {
                spdlog::error("[slots] refresh #{} of {}: {} is {} bytes, expected {}; not renamed", a_n, kSlotName[a_slot], Utf8(tmp), size,
                              kFileBytes);
                return;
            }
            spdlog::info("[slots] refresh #{} of {}: verified {} bytes (+{}ms)", a_n, kSlotName[a_slot], size, MsSince(t0));

            // 5. Rename over the slot, re-checking the slot is still idle right before.
            if (const auto st = g_status[a_slot].load(); Busy(st)) {
                spdlog::warn("[slots] refresh #{} of {}: slot became {} before the rename; not renamed", a_n, kSlotName[a_slot], StatusName(st));
                return;
            }
            if (!::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                spdlog::error("[slots] refresh #{} of {}: rename {} -> {} FAILED, Win32 error {} (+{}ms)", a_n, kSlotName[a_slot], Utf8(tmp),
                              Utf8(dst), ::GetLastError(), MsSince(t0));
                return;
            }
            spdlog::info("[slots] refresh #{} of {}: renamed over {} -> slot now holds the {:.0f} Hz test tone (total {}ms)", a_n,
                         kSlotName[a_slot], Utf8(dst), hz, MsSince(t0));
        }

        void Enqueue(Job a_job) {
            {
                std::scoped_lock lk(g_mx);
                g_jobs.push_back(std::move(a_job));
            }
            g_cv.notify_one();
        }

        void WorkerLoop(std::stop_token a_st) {
            std::uint32_t    refreshN     = 0;
            auto             lastKillPoll = std::chrono::steady_clock::now();
            std::unique_lock lk(g_mx);
            while (!a_st.stop_requested()) {
                (void)g_cv.wait_for(lk, a_st, 500ms, [] { return !g_jobs.empty(); });
                if (a_st.stop_requested()) break;

                if (std::chrono::steady_clock::now() - lastKillPoll >= 2s) {
                    lastKillPoll = std::chrono::steady_clock::now();
                    lk.unlock();
                    if (FileExists(KillSwitchPath()) && !g_kill.exchange(true))
                        spdlog::warn("[slots] kill switch file appeared: {}", Utf8(KillSwitchPath()));
                    lk.lock();
                }

                while (!g_jobs.empty() && !a_st.stop_requested()) {
                    Job j = std::move(g_jobs.front());
                    g_jobs.pop_front();
                    lk.unlock();
                    if (j.kind == Job::Kind::kLockProbe) {
                        LockProbe(j.slot, j.why);
                    } else {
                        g_refreshQueued[j.slot] = false;
                        Refresh(j.slot, refreshN++);
                    }
                    lk.lock();
                }
            }
        }

        // Same ownership pattern as the probe watcher (Probe.cpp): a static guard that
        // requests stop and joins at DLL_PROCESS_DETACH.
        struct Worker {
            std::jthread th;
            ~Worker() {
                th.request_stop();
                if (th.joinable()) th.join();
            }
        } g_worker;

    }  // namespace

    std::filesystem::path DataDir() {
        static const std::filesystem::path dir = [] {
            std::wstring buf(MAX_PATH, L'\0');
            for (;;) {
                const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
                if (n == 0) return std::filesystem::path(L"Data");  // relative to the CWD (the game folder)
                if (n < buf.size()) {
                    buf.resize(n);
                    break;
                }
                buf.resize(buf.size() * 2);
            }
            return std::filesystem::path(buf).parent_path() / L"Data";
        }();
        return dir;
    }

    bool KillSwitchPresentNow() {
        const bool k = FileExists(KillSwitchPath());
        if (k) g_kill = true;
        spdlog::info("[slots] kill switch {} : {}", Utf8(KillSwitchPath()), k ? "PRESENT" : "absent");
        return k;
    }

    bool KillSwitchSeen() { return g_kill.load(); }

    void Start() {
        if (g_worker.th.joinable()) return;
        spdlog::info("[slots] data dir {} ; slot dir {} ; test tone {} s {} Hz {}-bit {} ch = {} bytes", Utf8(DataDir()), Utf8(HarmonyDir()),
                     kSeconds, kRate, kBits, kChannels, kFileBytes);
        g_worker.th = std::jthread(WorkerLoop);
        Enqueue({ Job::Kind::kLockProbe, 0, "plugin start" });
    }

    void PublishStatus(int a_slot, std::uint32_t a_status) { g_status[a_slot].store(a_status); }

    void RequestRefresh(int a_slot) {
        if (g_refreshQueued[a_slot].exchange(true)) {
            spdlog::info("[slots] refresh of {} already queued", kSlotName[a_slot]);
            return;
        }
        spdlog::info("[slots] refresh of {} queued", kSlotName[a_slot]);
        Enqueue({ Job::Kind::kRefresh, a_slot, {} });
    }

    void RequestLockProbe(int a_slot, const char* a_why) { Enqueue({ Job::Kind::kLockProbe, a_slot, a_why }); }

}  // namespace MJ::Slots
