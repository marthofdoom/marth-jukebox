#pragma once

// Audio file helpers for the slot protocol (docs/INTEGRATION-1.0.md A.3). Pure C++ + dr_flac; no engine.
//
// The engine plays a loose PCM WAV at the slot path (M0b: a canonical 44-byte header, 16-bit stereo,
// 44.1 kHz, heard by marth). Only that format is accepted for now: 48 kHz and mono are NOT field-proven
// (DESIGN 5), and the Harmony profile asks for 44.1 kHz stereo 16-bit anyway. Every slot file this plugin writes is therefore a CANONICAL WAV:
// RIFF/WAVE, one 16-byte `fmt ` chunk (tag 1 = PCM), one `data` chunk at offset 44, nothing else.
// ConvertToCanonicalWav() produces it from either a Harmony WAV (any chunk layout ProbeAudioFile accepts)
// or a Harmony FLAC (decoded with dr_flac), and refuses anything the engine path was not proven with.

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>

namespace mj::pool
{
	struct PcmFormat
	{
		std::uint32_t rate     = 0;
		std::uint16_t channels = 0;
		std::uint16_t bits     = 0;
	};

	struct WavLayout
	{
		PcmFormat     fmt;
		std::uint64_t dataOffset = 0;  // file offset of the first sample byte
		std::uint64_t dataBytes  = 0;
	};

	// Parses the RIFF header from the first bytes of a file. `head` must hold everything up to the start of
	// the `data` payload (a few KiB is plenty for Harmony files); `fileSize` is the real file size.
	// Accepts PCM (tag 1) or WAVE_FORMAT_EXTENSIBLE; the data chunk must fit in the file and hold whole frames.
	bool ParseWav(std::span<const std::uint8_t> head, std::uint64_t fileSize, WavLayout& out, std::string& err);

	// The 44-byte canonical header for `dataBytes` of PCM in format `f`.
	[[nodiscard]] std::array<std::uint8_t, 44> CanonicalWavHeader(const PcmFormat& f, std::uint32_t dataBytes);

	// What the slot path was proven with in the field: 16-bit PCM, stereo, 44100 Hz. Nothing else.
	bool EnginePlayable(const PcmFormat& f, std::string& why);

	struct ConvertResult
	{
		bool          ok = false;
		bool          sourceError = false;  // true: the SOURCE is bad (drop the track); false: output side / cancel
		std::string   error;          // plain text, no server strings
		std::string   container;      // "wav" | "flac" (detected from the magic, not the extension)
		PcmFormat     fmt;
		std::uint64_t frames       = 0;
		std::uint64_t bytesWritten = 0;  // size of `dst` (0 in validate-only mode)
		double        durationS    = 0.0;
	};

	// Decodes / copies `src` (WAV or FLAC by magic) into `dst` as a canonical WAV, flushed to disk before it
	// returns (FlushFileBuffers / fsync). `dst` is created or truncated; on failure it may be left partial (the
	// caller writes to a .tmp and removes it). An empty `dst` = validate only: everything is decoded (FLAC frame
	// CRCs, the frame count against STREAMINFO) and nothing is written.
	// FLAC: the decoded frame count must equal STREAMINFO's total (a truncated or corrupt stream fails).
	// Anything longer than 2 hours is refused (Harmony cuts its sources there).
	// The stop token is checked between blocks; a stop gives ok=false, error "cancelled".
	ConvertResult ConvertToCanonicalWav(const std::filesystem::path& src, const std::filesystem::path& dst,
		std::stop_token st = {});
}
