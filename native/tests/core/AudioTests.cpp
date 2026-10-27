// Slot audio tests (pool/Audio.h): FLAC -> canonical WAV against a golden file, WAV re-wrap, refusals.
// Fixtures: tests/core/fixtures/make_flac_fixtures.py (synthetic signal, libFLAC-encoded).
#include "TestHarness.h"

#include "pool/Audio.h"

#include <cstring>
#include <fstream>
#include <iterator>

using namespace mj::pool;
namespace fs = std::filesystem;

namespace
{
	std::vector<std::uint8_t> ReadAll(const fs::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
	}

	void WriteAll(const fs::path& p, const std::vector<std::uint8_t>& b)
	{
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
	}

	fs::path Fixture(const char* name) { return mjtest::DataDir() / "fixtures" / name; }

	void Put32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v)
	{
		for (int i = 0; i < 4; ++i) {
			b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
		}
	}
}

MJ_TEST(Audio_FlacDecodesToTheGoldenWav)
{
	mjtest::ScratchDir dir;
	const auto         golden = ReadAll(Fixture("golden_s16.wav"));
	MJ_CHECK(golden.size() == 44 + 13230 * 4);
	const auto out = dir.Path() / "slot.wav.tmp";
	const auto r   = ConvertToCanonicalWav(Fixture("golden_s16.flac"), out);
	MJ_CHECK_MSG(r.ok, r.error);
	MJ_CHECK(r.container == "flac" && r.frames == 13230 && r.fmt.rate == 44100 && r.fmt.channels == 2 && r.fmt.bits == 16);
	MJ_CHECK(r.bytesWritten == golden.size());
	MJ_CHECK(r.durationS > 0.29 && r.durationS < 0.31);
	const auto got = ReadAll(out);
	MJ_CHECK(got.size() == golden.size());
	MJ_CHECK(got == golden);  // bit-exact: FLAC is lossless and the header is canonical

	// Validate-only mode decodes everything and writes nothing.
	const auto v = ConvertToCanonicalWav(Fixture("golden_s16.flac"), {});
	MJ_CHECK(v.ok && v.bytesWritten == 0 && v.frames == 13230);
}

MJ_TEST(Audio_CanonicalHeaderMatchesGolden)
{
	const auto golden = ReadAll(Fixture("golden_s16.wav"));
	const auto h      = CanonicalWavHeader({ 44100, 2, 16 }, 13230 * 4);
	MJ_CHECK(golden.size() >= 44 && std::memcmp(golden.data(), h.data(), 44) == 0);
	WavLayout   L;
	std::string err;
	MJ_CHECK(ParseWav(golden, golden.size(), L, err));
	MJ_CHECK(L.dataOffset == 44 && L.dataBytes == 13230 * 4 && L.fmt.rate == 44100);
}

MJ_TEST(Audio_WavIsRewrappedCanonically)
{
	mjtest::ScratchDir dir;
	const auto         golden = ReadAll(Fixture("golden_s16.wav"));
	// Same samples behind an EXTENSIBLE fmt chunk (40 bytes) and a LIST chunk with an odd size (pad byte).
	std::vector<std::uint8_t> w;
	auto app = [&w](const void* p, std::size_t n) { w.insert(w.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n); };
	app("RIFF\0\0\0\0WAVEfmt ", 16);
	std::vector<std::uint8_t> fmt(4 + 40, 0);
	Put32(fmt, 0, 40);
	std::memcpy(fmt.data() + 4, golden.data() + 20, 16);  // tag..bits from the canonical header
	fmt[4] = 0xFE;
	fmt[5] = 0xFF;  // WAVE_FORMAT_EXTENSIBLE
	app(fmt.data(), fmt.size());
	app("LIST\x03\0\0\0abc\0", 12);
	app(golden.data() + 36, golden.size() - 36);  // "data" + size + samples
	Put32(w, 4, static_cast<std::uint32_t>(w.size() - 8));
	const auto src = dir.Path() / "in.wav";
	WriteAll(src, w);
	const auto out = dir.Path() / "out.wav";
	const auto r   = ConvertToCanonicalWav(src, out);
	MJ_CHECK_MSG(r.ok, r.error);
	MJ_CHECK(r.container == "wav" && r.frames == 13230);
	MJ_CHECK(ReadAll(out) == golden);
	// Validate-only WAV.
	MJ_CHECK(ConvertToCanonicalWav(src, {}).ok);
}

MJ_TEST(Audio_TruncatedOrCorruptFlacIsRefused)
{
	mjtest::ScratchDir dir;
	const auto         flac = ReadAll(Fixture("golden_s16.flac"));
	auto               cut  = flac;
	cut.resize(flac.size() * 6 / 10);
	WriteAll(dir.Path() / "cut.flac", cut);
	auto r = ConvertToCanonicalWav(dir.Path() / "cut.flac", dir.Path() / "o1.wav");
	MJ_CHECK(!r.ok);
	MJ_CHECK_MSG(r.error.find("frames") != std::string::npos, r.error);

	auto bad = flac;
	for (std::size_t i = flac.size() / 2; i < flac.size() / 2 + 64; ++i) {
		bad[i] ^= 0x5A;
	}
	WriteAll(dir.Path() / "bad.flac", bad);
	r = ConvertToCanonicalWav(dir.Path() / "bad.flac", {});
	MJ_CHECK_MSG(!r.ok, "a corrupt FLAC frame must not pass validation");

	// Only the 4-byte magic.
	WriteAll(dir.Path() / "magic.flac", { 'f', 'L', 'a', 'C' });
	MJ_CHECK(!ConvertToCanonicalWav(dir.Path() / "magic.flac", {}).ok);
	WriteAll(dir.Path() / "junk.bin", { 'O', 'g', 'g', 'S', 0, 0 });
	r = ConvertToCanonicalWav(dir.Path() / "junk.bin", {});
	MJ_CHECK(!r.ok && r.error == "neither WAV nor FLAC");
	MJ_CHECK(!ConvertToCanonicalWav(dir.Path() / "absent.flac", {}).ok);
}

MJ_TEST(Audio_FormatsTheSlotWasNotProvenWithAreRefused)
{
	auto r = ConvertToCanonicalWav(Fixture("mono_s24.flac"), {});
	MJ_CHECK(!r.ok && r.fmt.bits == 24);
	MJ_CHECK_MSG(r.error.find("24-bit") != std::string::npos, r.error);
	std::string why;
	MJ_CHECK(EnginePlayable({ 48000, 1, 16 }, why));
	MJ_CHECK(EnginePlayable({ 44100, 2, 16 }, why));
	MJ_CHECK(!EnginePlayable({ 22050, 2, 16 }, why));
	MJ_CHECK(!EnginePlayable({ 44100, 6, 16 }, why));
	MJ_CHECK(!EnginePlayable({ 44100, 2, 8 }, why));
}

MJ_TEST(Audio_ParseWavRejectsInconsistentHeaders)
{
	const auto  golden = ReadAll(Fixture("golden_s16.wav"));
	WavLayout   L;
	std::string err;
	MJ_CHECK(!ParseWav(golden, golden.size() + 1, L, err));  // RIFF size vs file size
	auto odd = golden;
	Put32(odd, 40, 13230 * 4 - 2);  // partial frame
	MJ_CHECK(!ParseWav(odd, odd.size(), L, err));
	auto big = golden;
	Put32(big, 40, 13230 * 4 + 4);  // data past the end
	MJ_CHECK(!ParseWav(big, big.size(), L, err));
	auto pcm = golden;
	pcm[20] = 3;  // float
	MJ_CHECK(!ParseWav(pcm, pcm.size(), L, err) && err == "not PCM");
	MJ_CHECK(!ParseWav(std::span<const std::uint8_t>(golden.data(), 8), golden.size(), L, err));
}

MJ_TEST(Audio_StopTokenCancels)
{
	mjtest::ScratchDir dir;
	std::stop_source   ss;
	ss.request_stop();
	const auto r = ConvertToCanonicalWav(Fixture("golden_s16.flac"), dir.Path() / "x.wav", ss.get_token());
	MJ_CHECK(!r.ok && r.error == "cancelled");
}
