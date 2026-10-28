// Canonical WAV writer + FLAC decode (dr_flac). See Audio.h.
#include "pool/Audio.h"

#include "dr_libs/dr_flac.h"  // declarations only; the implementation is compiled in pool/DrFlac.cpp

#include <cstdio>
#include <cstring>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#if defined(_WIN32)
#	include <io.h>
#else
#	include <unistd.h>
#endif

namespace mj::pool
{
	namespace fs = std::filesystem;

	namespace
	{
		constexpr std::uint64_t kMaxDataBytes = 0xFFFFFFFFull - 36;  // RIFF size field is 32-bit
		constexpr std::size_t   kHeadBytes    = 64 * 1024;           // enough for any header Harmony writes
		constexpr double        kMaxSeconds   = 2.0 * 3600.0;        // Harmony cuts sources at 2 h (game-source.md)

		std::uint16_t Le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
		std::uint32_t Le32(const std::uint8_t* p)
		{
			return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
				   (static_cast<std::uint32_t>(p[3]) << 24);
		}

		struct FileCloser
		{
			void operator()(std::FILE* f) const
			{
				if (f) {
					std::fclose(f);
				}
			}
		};
		using File = std::unique_ptr<std::FILE, FileCloser>;

		File Open(const fs::path& p, bool write)
		{
#if defined(_WIN32)
			std::FILE* f = nullptr;
			if (_wfopen_s(&f, p.c_str(), write ? L"wb" : L"rb") != 0) {
				f = nullptr;
			}
			return File(f);
#else
			return File(std::fopen(p.c_str(), write ? "wb" : "rb"));
#endif
		}

		bool Seek(std::FILE* f, std::uint64_t off)
		{
#if defined(_WIN32)
			return _fseeki64(f, static_cast<long long>(off), SEEK_SET) == 0;
#else
			return fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
		}

		thread_local bool t_durable = true;  // set by ConvertToCanonicalWav for this call

		// Paced writing for non-durable outputs: 2 ms pause per 4 MiB written.
		thread_local std::uint64_t t_sincePause = 0;
		void Pace(std::size_t n)
		{
			if (t_durable) {
				return;
			}
			t_sincePause += n;
			if (t_sincePause >= (4u << 20)) {
				t_sincePause = 0;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}

		// fflush + (durable only) flush the OS buffers to the device, then close. False on any failure.
		bool FlushAndClose(File& f)
		{
			bool ok = std::fflush(f.get()) == 0;
			if (t_durable) {
#if defined(_WIN32)
				ok = ok && _commit(_fileno(f.get())) == 0;
#else
				ok = ok && fsync(fileno(f.get())) == 0;
#endif
			}
			std::FILE* raw = f.release();
			ok             = (std::fclose(raw) == 0) && ok;
			return ok;
		}

		bool WriteAll(std::FILE* f, const void* p, std::size_t n)
		{
			const bool ok = n == 0 || std::fwrite(p, 1, n, f) == n;
			Pace(n);
			return ok;
		}

		// A failure of the SOURCE file (unreadable, not decodable, wrong format): the caller drops that track.
		ConvertResult Fail(ConvertResult r, std::string why)
		{
			r.ok          = false;
			r.sourceError = true;
			r.error       = std::move(why);
			return r;
		}

		// A failure on the OUTPUT side (create/write/flush, disk full) or a cancel: the source is fine.
		ConvertResult FailDst(ConvertResult r, std::string why)
		{
			r.ok          = false;
			r.sourceError = false;
			r.error       = std::move(why);
			return r;
		}

		struct DrFlacCloser
		{
			void operator()(drflac* p) const
			{
				if (p) {
					drflac_close(p);
				}
			}
		};

		ConvertResult FromWav(std::FILE* in, std::uint64_t fileSize, const fs::path& dst, const std::stop_token& st)
		{
			ConvertResult r;
			r.container = "wav";
			std::vector<std::uint8_t> head(static_cast<std::size_t>(std::min<std::uint64_t>(fileSize, kHeadBytes)));
			if (std::fread(head.data(), 1, head.size(), in) != head.size()) {
				return Fail(r, "cannot read the WAV header");
			}
			WavLayout   L;
			std::string err;
			if (!ParseWav(head, fileSize, L, err)) {
				return Fail(r, "bad WAV: " + err);
			}
			r.fmt       = L.fmt;
			r.frames    = L.dataBytes / (static_cast<std::uint64_t>(L.fmt.channels) * (L.fmt.bits / 8));
			r.durationS = static_cast<double>(r.frames) / L.fmt.rate;
			if (!EnginePlayable(L.fmt, err)) {
				return Fail(r, err);
			}
			if (L.dataBytes > kMaxDataBytes) {
				return Fail(r, "WAV data is larger than a RIFF file can hold");
			}
			if (r.durationS > kMaxSeconds) {
				return Fail(r, "audio is longer than 2 hours");
			}
			if (dst.empty()) {
				r.ok = true;
				return r;
			}
			File out = Open(dst, true);
			if (!out) {
				return FailDst(r, "cannot create the output file");
			}
			const auto hdr = CanonicalWavHeader(L.fmt, static_cast<std::uint32_t>(L.dataBytes));
			if (!WriteAll(out.get(), hdr.data(), hdr.size()) || !Seek(in, L.dataOffset)) {
				return FailDst(r, "cannot write the WAV header");
			}
			std::vector<std::uint8_t> buf(1u << 20);
			std::uint64_t             left = L.dataBytes;
			while (left > 0) {
				if (st.stop_requested()) {
					return FailDst(r, "cancelled");
				}
				const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
				if (std::fread(buf.data(), 1, n, in) != n) {
					return Fail(r, "WAV source ended early");
				}
				if (!WriteAll(out.get(), buf.data(), n)) {
					return FailDst(r, "write failed (disk full?)");
				}
				left -= n;
			}
			if (!FlushAndClose(out)) {
				return FailDst(r, "flush/close of the output failed");
			}
			r.bytesWritten = 44 + L.dataBytes;
			r.ok           = true;
			return r;
		}

		ConvertResult FromFlac(const fs::path& src, const fs::path& dst, const std::stop_token& st)
		{
			ConvertResult r;
			r.container = "flac";
#if defined(_WIN32)
			std::unique_ptr<drflac, DrFlacCloser> fl(drflac_open_file_w(src.c_str(), nullptr));
#else
			std::unique_ptr<drflac, DrFlacCloser> fl(drflac_open_file(src.c_str(), nullptr));
#endif
			if (!fl) {
				return Fail(r, "not a decodable FLAC stream");
			}
			r.fmt.rate     = fl->sampleRate;
			r.fmt.channels = fl->channels;
			r.fmt.bits     = fl->bitsPerSample;
			const std::uint64_t total = fl->totalPCMFrameCount;
			std::string         err;
			if (!EnginePlayable(r.fmt, err)) {
				return Fail(r, err);
			}
			if (total == 0) {
				return Fail(r, "FLAC STREAMINFO has no total sample count (duration unknown)");
			}
			if (static_cast<double>(total) / r.fmt.rate > kMaxSeconds) {
				return Fail(r, "audio is longer than 2 hours");
			}
			const std::uint64_t dataBytes = total * r.fmt.channels * 2;
			if (dataBytes > kMaxDataBytes) {
				return Fail(r, "decoded FLAC is larger than a RIFF file can hold");
			}
			File out;
			if (!dst.empty()) {
				out = Open(dst, true);
				if (!out) {
					return FailDst(r, "cannot create the output file");
				}
				const auto hdr = CanonicalWavHeader(r.fmt, static_cast<std::uint32_t>(dataBytes));
				if (!WriteAll(out.get(), hdr.data(), hdr.size())) {
					return FailDst(r, "write failed (disk full?)");
				}
			}
			constexpr std::uint64_t   kChunk = 16384;
			std::vector<std::int16_t> pcm(static_cast<std::size_t>(kChunk * r.fmt.channels));
			std::uint64_t             got = 0;
			while (got < total) {
				if (st.stop_requested()) {
					return FailDst(r, "cancelled");
				}
				const std::uint64_t want = std::min<std::uint64_t>(kChunk, total - got);
				const std::uint64_t n    = drflac_read_pcm_frames_s16(fl.get(), want, pcm.data());
				if (n == 0) {
					break;  // corrupt frame or truncated stream: caught by the count check below
				}
				if (out && !WriteAll(out.get(), pcm.data(), static_cast<std::size_t>(n * r.fmt.channels * 2))) {
					return FailDst(r, "write failed (disk full?)");
				}
				got += n;
			}
			r.frames    = got;
			r.durationS = static_cast<double>(got) / r.fmt.rate;
			if (got != total) {
				return Fail(r, "FLAC decoded " + std::to_string(got) + " of " + std::to_string(total) +
								   " frames (truncated or corrupt stream)");
			}
			if (out) {
				if (!FlushAndClose(out)) {
					return FailDst(r, "flush/close of the output failed");
				}
				r.bytesWritten = 44 + dataBytes;
			}
			r.ok = true;
			return r;
		}
	}

	bool ParseWav(std::span<const std::uint8_t> h, std::uint64_t fileSize, WavLayout& out, std::string& err)
	{
		if (h.size() < 12 || std::memcmp(h.data(), "RIFF", 4) != 0 || std::memcmp(h.data() + 8, "WAVE", 4) != 0) {
			err = "not a RIFF/WAVE file";
			return false;
		}
		if (static_cast<std::uint64_t>(Le32(h.data() + 4)) + 8 != fileSize) {
			err = "RIFF size does not match the file size";
			return false;
		}
		bool          haveFmt = false;
		std::uint64_t off     = 12;
		while (off + 8 <= h.size()) {
			const std::uint8_t* c  = h.data() + off;
			const std::uint64_t sz = Le32(c + 4);
			if (std::memcmp(c, "fmt ", 4) == 0) {
				if (sz < 16 || off + 8 + 16 > h.size()) {
					err = "short fmt chunk";
					return false;
				}
				const std::uint16_t tag = Le16(c + 8);
				if (tag != 1 && tag != 0xFFFE) {
					err = "not PCM";
					return false;
				}
				out.fmt.channels = Le16(c + 10);
				out.fmt.rate     = Le32(c + 12);
				out.fmt.bits     = Le16(c + 22);
				if (out.fmt.channels == 0 || out.fmt.rate == 0 || out.fmt.bits == 0 || out.fmt.bits % 8 != 0) {
					err = "bad fmt values";
					return false;
				}
				haveFmt = true;
			} else if (std::memcmp(c, "data", 4) == 0) {
				if (!haveFmt) {
					err = "data chunk before fmt";
					return false;
				}
				const std::uint64_t frame = static_cast<std::uint64_t>(out.fmt.channels) * (out.fmt.bits / 8);
				if (off + 8 + sz > fileSize || sz == 0 || sz % frame != 0) {
					err = "data chunk does not fit the file or holds a partial frame";
					return false;
				}
				out.dataOffset = off + 8;
				out.dataBytes  = sz;
				return true;
			}
			off += 8 + sz + (sz & 1);  // chunks are word-aligned
		}
		err = "no data chunk in the header area";
		return false;
	}

	std::array<std::uint8_t, 44> CanonicalWavHeader(const PcmFormat& f, std::uint32_t dataBytes)
	{
		std::array<std::uint8_t, 44> b{};
		auto put32 = [&](std::size_t o, std::uint32_t v) {
			for (int i = 0; i < 4; ++i) {
				b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
			}
		};
		auto put16 = [&](std::size_t o, std::uint16_t v) {
			b[o]     = static_cast<std::uint8_t>(v);
			b[o + 1] = static_cast<std::uint8_t>(v >> 8);
		};
		const std::uint16_t block = static_cast<std::uint16_t>(f.channels * (f.bits / 8));
		std::memcpy(b.data(), "RIFF", 4);
		put32(4, 36 + dataBytes);
		std::memcpy(b.data() + 8, "WAVEfmt ", 8);
		put32(16, 16);
		put16(20, 1);
		put16(22, f.channels);
		put32(24, f.rate);
		put32(28, f.rate * block);
		put16(32, block);
		put16(34, f.bits);
		std::memcpy(b.data() + 36, "data", 4);
		put32(40, dataBytes);
		return b;
	}

	bool IsCanonicalSlotHeader(std::span<const std::uint8_t> head, std::uint64_t fileSize)
	{
		if (head.size() < 44 || fileSize < 44) {
			return false;
		}
		WavLayout   L;
		std::string err;
		if (!ParseWav(head.first(44), fileSize, L, err) || L.dataOffset != 44 || L.dataBytes != fileSize - 44) {
			return false;
		}
		return EnginePlayable(L.fmt, err);
	}

	bool EnginePlayable(const PcmFormat& f, std::string& why)
	{
		if (f.bits != 16) {
			why = std::to_string(f.bits) + "-bit audio (the slot needs 16-bit PCM)";
			return false;
		}
		if (f.channels != 2) {
			why = std::to_string(f.channels) + " channel(s) (the slot needs stereo)";
			return false;
		}
		if (f.rate != 44100) {
			why = std::to_string(f.rate) + " Hz (the slot needs 44100 Hz)";
			return false;
		}
		return true;
	}

	ConvertResult ConvertToCanonicalWav(const fs::path& src, const fs::path& dst, std::stop_token st, bool durable)
	{
		t_durable    = durable;
		t_sincePause = 0;
		ConvertResult r;
		std::error_code ec;
		const auto      size = fs::file_size(src, ec);
		if (ec) {
			return Fail(r, "source file missing");
		}
		File in = Open(src, false);
		if (!in) {
			return Fail(r, "cannot open the source file");
		}
		char magic[4]{};
		if (size < 4 || std::fread(magic, 1, 4, in.get()) != 4 || !Seek(in.get(), 0)) {
			return Fail(r, "source file too short");
		}
		if (std::memcmp(magic, "RIFF", 4) == 0) {
			return FromWav(in.get(), size, dst, st);
		}
		if (std::memcmp(magic, "fLaC", 4) == 0) {
			in.reset();  // dr_flac opens the file itself
			return FromFlac(src, dst, st);
		}
		return Fail(r, "neither WAV nor FLAC");
	}
}
