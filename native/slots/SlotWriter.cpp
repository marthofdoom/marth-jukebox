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
        // Settle rule (review F2), published by the main thread with the status: SteadyMs()
        // when the slot's current Inactive/Finished run began (-1 while busy), and the
        // required settle time (the type's fadeTime + 1 s). Three separate atomics: a read
        // may mix two consecutive ticks, which only matters at a busy/idle edge.
        std::atomic<long long>     g_idleSince[2]{ -1, -1 };
        std::atomic<long long>     g_settleMs[2]{ 0, 0 };
        std::atomic<bool>          g_kill{ false };
        std::atomic<bool>          g_refreshQueued[2]{};
        // M0c: what the main thread last saw for wrapper A as the current track.
        std::atomic<bool>          g_playACurrent{ false };
        std::atomic<std::uint32_t> g_playHandle{ 0 };
        std::atomic<std::uint32_t> g_playIdx{ 0 };

        constexpr long long kPlayStableMs = 1500;  // let the engine open the file first

        struct Job {
            enum class Kind { kRefresh, kLockProbe, kPlayExperiment } kind;
            int           slot;
            std::string   why;
            std::uint32_t n{ 0 };        // kPlayExperiment: play event number
            std::uint32_t handle{ 0 };   //   soundID at the event
            std::uint32_t idx{ 0 };      //   currentTrackIndex at the event
            long long     eventMs{ 0 };  //   SteadyMs() at the event
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

        // Idle (not Playing/Paused/Finishing) and idle for at least the settle time, per the
        // last values the main thread published (up to one 250 ms tick old, plus the time
        // this job waited in the queue).
        bool Settled(int a_slot, std::uint32_t& a_st, long long& a_idleFor, long long& a_settle) {
            a_st                  = g_status[a_slot].load();
            const long long since = g_idleSince[a_slot].load();
            a_settle              = g_settleMs[a_slot].load();
            a_idleFor             = since < 0 ? -1 : SteadyMs() - since;
            return !Busy(a_st) && since >= 0 && a_idleFor >= a_settle;
        }

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
            std::uint32_t st      = 0;
            long long     idleFor = 0, settle = 0;
            if (!Settled(a_slot, st, idleFor, settle)) {
                spdlog::warn("[slots] refresh #{} of {}: skipped, slot is {} idle {} ms of {} ms settle (never write a busy or unsettled slot)",
                             a_n, kSlotName[a_slot], StatusName(st), idleFor, settle);
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

            // 5. Rename over the slot. Re-check, from the published atomics, that the slot is
            //    still idle and settled. Those values are up to one watcher tick (250 ms) old
            //    plus queue latency, so this is not a guarantee that the engine has not opened
            //    the file since; what keeps a race harmless is that the rename is atomic (a
            //    reader sees the whole old file or the whole new one).
            if (!Settled(a_slot, st, idleFor, settle)) {
                spdlog::warn("[slots] refresh #{} of {}: at the pre-rename check (as last published) the slot was {} idle {} ms of {} ms "
                             "settle; not renamed",
                             a_n, kSlotName[a_slot], StatusName(st), idleFor, settle);
                return;
            }
            if (!::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                spdlog::error("[slots] refresh #{} of {}: rename {} -> {} FAILED, Win32 error {} (+{}ms)", a_n, kSlotName[a_slot], Utf8(tmp),
                              Utf8(dst), ::GetLastError(), MsSince(t0));
                return;
            }
            spdlog::info("[slots] refresh #{} of {}: renamed over {} -> slot now holds the {:.0f} Hz test tone (total {}ms; slot {} "
                         "for {} ms at the pre-rename check, settle {} ms)",
                         a_n, kSlotName[a_slot], Utf8(dst), hz, MsSince(t0), StatusName(st), idleFor, settle);
        }

        void Enqueue(Job a_job) {
            {
                std::scoped_lock lk(g_mx);
                g_jobs.push_back(std::move(a_job));
            }
            g_cv.notify_one();
        }

        // ---- M0c play experiment (slot A only) ----

        std::string g_declared = "as found";  // what explore_a.wav is declared to hold (worker only)

        bool FileSize(const std::filesystem::path& a_p, std::uint64_t& a_size) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(a_p.c_str(), GetFileExInfoStandard, &fad)) return false;
            a_size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            return true;
        }

        // Kill switch, checked on the file itself (not only the 2 s poll). Latches g_kill.
        bool KillNow(const std::string& a_tag, const char* a_where) {
            if (!g_kill.load() && FileExists(KillSwitchPath())) {
                g_kill = true;
                spdlog::warn("[slots] kill switch file present: {}", Utf8(KillSwitchPath()));
            }
            if (g_kill.load()) {
                spdlog::info("[slots] {}: rename experiment stopped {} (kill switch)", a_tag, a_where);
                return true;
            }
            return false;
        }

        // .orig is created once from the slot file and never overwritten: copied to .orig.part,
        // verified, then renamed to .orig WITHOUT replace. Refuses to back up a file that has
        // exactly the test-tone size (a slot left as a tone by an earlier run), and refuses an
        // existing .orig that is empty or tone-sized. Any refusal leaves the slot untouched.
        bool EnsureBackup(const std::filesystem::path& a_dst, const std::filesystem::path& a_orig, const std::string& a_tag) {
            if (FileExists(a_orig)) {
                std::uint64_t osz = 0;
                if (!FileSize(a_orig, osz)) {
                    spdlog::error("[slots] {}: cannot stat {} (Win32 error {}); slot NOT touched", a_tag, Utf8(a_orig), ::GetLastError());
                    return false;
                }
                if (osz == 0 || osz == kFileBytes) {
                    spdlog::error("[slots] {}: BAD BACKUP {} is {} bytes ({}); slot NOT touched (restore the original by hand, see "
                                  "docs/M0C-TEST.md)",
                                  a_tag, Utf8(a_orig), osz, osz == 0 ? "empty" : "a test tone");
                    return false;
                }
                return true;
            }
            std::uint64_t sz = 0;
            if (!FileSize(a_dst, sz)) {
                spdlog::error("[slots] {}: no {} and cannot stat {} (Win32 error {}); slot NOT touched", a_tag, Utf8(a_orig), Utf8(a_dst),
                              ::GetLastError());
                return false;
            }
            if (sz == kFileBytes) {
                spdlog::error("[slots] {}: no {} and {} is {} bytes = a test tone, not the original; slot NOT touched "
                              "(restore the original first, see docs/M0C-TEST.md)",
                              a_tag, Utf8(a_orig), Utf8(a_dst), sz);
                return false;
            }
            const auto t0   = std::chrono::steady_clock::now();
            auto       part = a_orig;
            part += L".part";
            if (!::CopyFileW(a_dst.c_str(), part.c_str(), FALSE)) {
                spdlog::error("[slots] {}: backup copy {} -> {} FAILED, Win32 error {}; slot NOT touched", a_tag, Utf8(a_dst), Utf8(part),
                              ::GetLastError());
                return false;
            }
            std::uint64_t psz = 0;
            if (!FileSize(part, psz) || psz != sz) {
                spdlog::error("[slots] {}: backup {} verify FAILED ({} bytes, expected {}); slot NOT touched", a_tag, Utf8(part), psz, sz);
                return false;
            }
            if (!::MoveFileExW(part.c_str(), a_orig.c_str(), 0)) {
                spdlog::error("[slots] {}: backup rename {} -> {} FAILED, Win32 error {}; slot NOT touched", a_tag, Utf8(part), Utf8(a_orig),
                              ::GetLastError());
                return false;
            }
            spdlog::info("[slots] {}: backed up the original ({} bytes) to {} via .part ({}ms)", a_tag, sz, Utf8(a_orig), MsSince(t0));
            return true;
        }

        // Write a buffer to a fresh file, flush, close, verify the size. No rename.
        bool WriteVerified(const std::filesystem::path& a_p, const std::vector<std::uint8_t>& a_data, const std::string& a_tag) {
            const auto t0 = std::chrono::steady_clock::now();
            HANDLE     h  = ::CreateFileW(a_p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                spdlog::error("[slots] {}: create {} FAILED, Win32 error {}", a_tag, Utf8(a_p), ::GetLastError());
                return false;
            }
            std::size_t off = 0;
            bool        ok  = true;
            while (off < a_data.size()) {
                DWORD wrote = 0;
                if (!::WriteFile(h, a_data.data() + off, static_cast<DWORD>(a_data.size() - off), &wrote, nullptr) || wrote == 0) {
                    spdlog::error("[slots] {}: write {} FAILED at {} bytes, Win32 error {}", a_tag, Utf8(a_p), off, ::GetLastError());
                    ok = false;
                    break;
                }
                off += wrote;
            }
            if (ok && !::FlushFileBuffers(h)) {
                spdlog::error("[slots] {}: flush {} FAILED, Win32 error {}", a_tag, Utf8(a_p), ::GetLastError());
                ok = false;
            }
            if (!::CloseHandle(h)) {
                spdlog::error("[slots] {}: close {} FAILED, Win32 error {}", a_tag, Utf8(a_p), ::GetLastError());
                ok = false;
            }
            if (!ok) return false;
            std::uint64_t sz = 0;
            if (!FileSize(a_p, sz) || sz != a_data.size()) {
                spdlog::error("[slots] {}: {} verify FAILED ({} bytes, expected {})", a_tag, Utf8(a_p), sz, a_data.size());
                return false;
            }
            spdlog::info("[slots] {}: wrote+flushed+closed+verified {} ({} bytes, {}ms)", a_tag, Utf8(a_p), sz, MsSince(t0));
            return true;
        }

        // Runs when the play event has been stable for kPlayStableMs. (e) write-open probe of
        // the PLAYING file; (f) rename-over of the PLAYING file: odd n = tone, even n = original.
        void PlayExperiment(const Job& a_j) {
            const auto tag = std::format("play #{} (handle=0x{:08X} idx={})", a_j.n, a_j.handle, a_j.idx);
            const bool          aCur = g_playACurrent.load();
            const std::uint32_t hNow = g_playHandle.load();
            const std::uint32_t iNow = g_playIdx.load();
            if (hNow != a_j.handle || iNow != a_j.idx) {
                spdlog::info("[slots] {}: dropped, the play changed within {} ms (now handle=0x{:08X} idx={})", tag, kPlayStableMs, hNow, iNow);
                return;
            }
            spdlog::info("[slots] {}: stable for {} ms (A still current: {}); running (e) and (f) on the PLAYING file", tag,
                         SteadyMs() - a_j.eventMs, aCur);

            // (e)
            LockProbe(0, tag);

            // (f)
            const auto dst = HarmonyDir() / kSlotFile[0];
            auto       tmp = dst;
            tmp += L".tmp";
            auto orig = dst;
            orig += L".orig";
            if (KillNow(tag, "after the 1.5 s wait, before the backup")) return;
            if (!EnsureBackup(dst, orig, tag)) return;

            const bool   tone = (a_j.n % 2) == 1;
            const double hz   = 220.0 * (2 + (a_j.n % 10));
            const auto   t0   = std::chrono::steady_clock::now();
            if (tone) {
                if (!WriteVerified(tmp, MakeSineWav(hz), tag)) return;
            } else {
                std::uint64_t osz = 0, tsz = 0;
                if (!::CopyFileW(orig.c_str(), tmp.c_str(), FALSE)) {
                    spdlog::error("[slots] {}: copy {} -> {} FAILED, Win32 error {}", tag, Utf8(orig), Utf8(tmp), ::GetLastError());
                    return;
                }
                if (!FileSize(orig, osz) || !FileSize(tmp, tsz) || osz != tsz) {
                    spdlog::error("[slots] {}: copy verify FAILED ({} vs {} bytes)", tag, tsz, osz);
                    return;
                }
                spdlog::info("[slots] {}: copied the original to {} ({} bytes, {}ms)", tag, Utf8(tmp), tsz, MsSince(t0));
            }
            if (KillNow(tag, "before the rename")) return;
            const auto tr = std::chrono::steady_clock::now();
            if (!::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                const DWORD err = ::GetLastError();
                spdlog::error("[slots] {}: RENAME OVER THE PLAYING FILE FAILED, Win32 error {} ({}) after {}ms; not retried (the next play "
                              "event tries again); slot still declared to hold: {}",
                              tag, err, err == 32 ? "sharing violation" : err == 5 ? "access denied" : err == 17 ? "not same device" : "other",
                              MsSince(tr), g_declared);
                return;
            }
            g_declared = tone ? std::format("tone {:.0f} Hz", hz) : std::string("original");
            spdlog::info("[slots] {}: rename over the PLAYING file OK ({}ms; step total {}ms); slot A now declared to hold: {}", tag,
                         MsSince(tr), MsSince(t0), g_declared);
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
                    if (j.kind == Job::Kind::kPlayExperiment) {
                        // Wait (stop-aware) until the event is kPlayStableMs old.
                        while (!a_st.stop_requested()) {
                            const long long left = j.eventMs + kPlayStableMs - SteadyMs();
                            if (left <= 0) break;
                            std::mutex                 wmx;
                            std::unique_lock           wlk(wmx);
                            std::condition_variable_any wcv;
                            (void)wcv.wait_for(wlk, a_st, std::chrono::milliseconds(left), [] { return false; });
                        }
                        if (!a_st.stop_requested()) PlayExperiment(j);
                    } else if (j.kind == Job::Kind::kLockProbe) {
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

    long long SteadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void PublishStatus(int a_slot, std::uint32_t a_status, long long a_idleSinceMs, long long a_settleMs) {
        g_idleSince[a_slot].store(a_idleSinceMs);
        g_settleMs[a_slot].store(a_settleMs);
        g_status[a_slot].store(a_status);
    }

    void RequestRefresh(int a_slot) {
        if (g_refreshQueued[a_slot].exchange(true)) {
            spdlog::info("[slots] refresh of {} already queued", kSlotName[a_slot]);
            return;
        }
        spdlog::info("[slots] refresh of {} queued", kSlotName[a_slot]);
        Enqueue({ Job::Kind::kRefresh, a_slot, {} });
    }

    void RequestLockProbe(int a_slot, const char* a_why) { Enqueue({ Job::Kind::kLockProbe, a_slot, a_why }); }

    void PublishPlay(bool a_aCurrent, std::uint32_t a_handle, std::uint32_t a_idx) {
        g_playHandle.store(a_handle);
        g_playIdx.store(a_idx);
        g_playACurrent.store(a_aCurrent);
    }

    void RequestPlayExperiment(std::uint32_t a_n, std::uint32_t a_handle, std::uint32_t a_idx) {
        Job j{ Job::Kind::kPlayExperiment, 0, {} };
        j.n       = a_n;
        j.handle  = a_handle;
        j.idx     = a_idx;
        j.eventMs = SteadyMs();
        Enqueue(std::move(j));
    }

}  // namespace MJ::Slots
