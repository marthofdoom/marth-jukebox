# Compression test on the game-audio output formats (2026-10-27)

Question (marth): is lz4 (frame format) a good fit for shipping the WAVs? Method: four real tracks
(`tools/bench_compress.py`; PCM16 stereo 44.1 kHz, loudness-normalised by Harmony), each codec round-tripped
and verified; ffmpeg 7.0.2 static for the audio codecs, `lz4` 4.4.5 and `zstandard` 0.25 in-process.
Tracks: orchestral YouTube rip (132 s), folk (Library FLAC, 244 s), metal (Library, 203 s), dark ambient
(YouTube, 285 s). A fifth sample (Qobuz) was a 30 s preview and is excluded (see ROADMAP/DESIGN 8.8).
Averages over the four tracks; "MB/4min" = size of a 4-minute track (PCM = 40.4 MB).

| Codec | Ratio | MB/4min | Encode (s/track) | Decode (s/track) | Decode speed | Notes |
|---|---|---|---|---|---|---|
| PCM WAV | 1.000 | 40.4 | - | - | - | what the engine plays |
| lz4 frame L0, raw PCM | 0.990 | 40.0 | 0.05 | 0.04 | 8900x realtime | **saves 1%** |
| lz4 frame L9, raw PCM | 0.986 | 39.8 | 0.81 | 0.05 | 7700x | saves 1.4% |
| lz4 L0 + delta/byte-plane filter | 0.821 | 33.1 | 0.06 | 0.22* | 1000x* | saves 18% |
| lz4 L9 + delta/byte-plane filter | 0.726 | 29.3 | 2.96 | 0.22* | 1070x* | saves 27% |
| zstd L3 + delta/byte-plane | 0.682 | 27.5 | 0.14 | 0.20* | 1100x* | saves 32% |
| **FLAC L1** (lossless) | **0.608** | **24.6** | 0.42 | 0.10 | 2200x | saves 39% |
| FLAC L5 (lossless) | 0.560 | 22.6 | 0.33 | 0.07 | 3000x | saves 44% |
| WavPack fast / ALAC / TTA | 0.60 / 0.58 / 0.56 | 24.3 / 23.5 / 22.5 | 0.4-0.7 | ~0.1 | ~2000x | all bit-exact |
| MS ADPCM (lossy) | 0.253 | 10.2 | 0.20 | 0.10 | 2300x | SNR 28-33 dB |
| IMA ADPCM (lossy) | 0.252 | 10.2 | 0.24 | 0.12 | 1800x | SNR 29-34 dB |

\* decode time of the filtered variants is dominated by the numpy un-filter in this script; in C++ the
un-filter is a few tens of ms, so lz4's own decode (about 40-50 ms per track) is the floor.
Per-track FLAC L1 ratios: ambient 0.46, orchestral 0.58, metal 0.68, folk 0.70. lz4 raw: 0.97-1.00 on all four.
All lossless rows are bit-exact (asserted); the lossy rows report SNR.

## Reading
- **lz4 on raw PCM is useless** (1% smaller). Music PCM has almost no repeated byte sequences; LZ matching
  has nothing to find. lz4 only helps after a predictor (the delta filter): then 18-27%, still well short of FLAC.
- FLAC is itself a very cheap codec: 0.4 s to encode a 4-minute track, 0.1 s to decode (2200x realtime),
  and 39-44% smaller. Everything lz4 is good at (speed) is already irrelevant here, because decode runs once per
  track on the worker thread, never on the game thread.
- Transfer + decode time for a 4-minute track by link speed (decode counted at FLAC 0.1 s, lz4+filter 0.03 s in C++):
  | Link | PCM | lz4 + filter L0 | FLAC L1 |
  |---|---|---|---|
  | 1 Gbit LAN | 0.32 s | 0.30 s | 0.20 + 0.10 = 0.30 s |
  | 100 Mbit | 3.2 s | 2.7 s | 2.0 + 0.1 = 2.1 s |
  | 20 Mbit (WAN/Tailnet) | 16 s | 13.2 s | 9.8 + 0.1 = 9.9 s |
  The server work (resolve + loudnorm: 7-65 s cold) dwarfs all of these. Nothing here changes cold latency;
  compression matters for the on-disk pool and for remote servers.
- ADPCM is 4:1 but lossy at 28-33 dB SNR (audible hiss on quiet passages) on top of lossy YouTube sources, and
  the engine has not been shown to play it. Dropped unless disk becomes the problem.

## Decision
**FLAC level 1 for transfer and the pool (already merged in Harmony as `container=flac`); the plugin decodes
into the canonical PCM WAV slot.** lz4 stays out of the audio path. Where lz4 *would* earn its place: if
the pool ever stores PCM and the disk write is the bottleneck (it is not), or for JSON/state blobs (it is not
needed). A possible middle path, if a decoder dependency is unwanted: lz4+delta (0.82) costs 18% less than FLAC's
saving and needs our own filter code; dr_flac is a single public-domain header, so the dependency is smaller
than the code we would write. Revisit only with a measured need.
