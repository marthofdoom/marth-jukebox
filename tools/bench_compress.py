"""Compression benchmark on real game-audio WAVs (PCM16 stereo 44.1k, loudnorm'd by Harmony).
Measures size ratio, encode time, decode time for lossless codecs (lz4 frame, zstd, FLAC, WavPack, ALAC, TTA)
and SNR for the lossy ADPCM variants. Test material only."""
import io, json, os, struct, subprocess, sys, time
import numpy as np
import lz4.frame, zstandard

FF = sys.argv[1]
SAMPLES = sys.argv[2:]
TMP = os.environ.get('BENCH_TMP', '/tmp')


def wav_pcm(path):
    b = open(path, 'rb').read()
    n = struct.unpack('<I', b[40:44])[0]
    return b[:44], b[44:44 + n]


def best_time(fn, reps=3):
    t = []
    for _ in range(reps):
        a = time.perf_counter(); r = fn(); t.append(time.perf_counter() - a)
    return min(t), r


def prefilter(pcm):
    """Stereo PCM16 -> mid/side-free simple filter: per-channel first difference, then byte-plane split
    (low bytes then high bytes). The classic 'make LZ work on audio' trick; reversible."""
    a = np.frombuffer(pcm, dtype='<i2').reshape(-1, 2).astype(np.int32)
    d = np.empty_like(a); d[0] = a[0]; d[1:] = a[1:] - a[:-1]
    d = (d & 0xFFFF).astype('<u2')
    return np.ascontiguousarray(d.view(np.uint8).reshape(-1, 4).T).tobytes()  # 4 byte planes


def unfilter(buf, n):
    p = np.frombuffer(buf, dtype=np.uint8).reshape(4, n)
    d = np.ascontiguousarray(p.T).view('<i2').reshape(-1, 2).astype(np.int32)
    return (np.cumsum(d, axis=0) & 0xFFFF).astype('<u2').view('<i2').tobytes()


def run_ff(args, inp=None):
    return subprocess.run([FF, '-v', 'error', '-y'] + args, input=inp, capture_output=True, check=True).stdout


# baseline: ffmpeg process start + tiny decode, subtracted from FLAC decode times
tiny = io.BytesIO(); hdr = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 36 + 4000, b'WAVE', b'fmt ', 16, 1, 2, 44100, 176400, 4, 16, b'data', 4000)
open(f'{TMP}/tiny.wav', 'wb').write(hdr + b'\0' * 4000)
base, _ = best_time(lambda: run_ff(['-i', f'{TMP}/tiny.wav', '-f', 's16le', '-']), 5)

rows = []
for path in SAMPLES:
    name = os.path.basename(path).replace('.wav', '')
    hdr, pcm = wav_pcm(path)
    n = len(pcm) // 4
    secs = n / 44100
    mb = len(pcm) / 1048576

    def add(codec, size, enc, dec, note=''):
        rows.append(dict(sample=name, codec=codec, ratio=round(size / len(pcm), 3), mb=round(size / 1048576, 1),
                         enc_s=round(enc, 3), dec_s=round(dec, 3), dec_x_rt=round(secs / dec) if dec > 0 else 0, note=note))

    # --- lz4 frame, raw PCM
    for lvl in (0, 3, 9):
        e, c = best_time(lambda: lz4.frame.compress(pcm, compression_level=lvl, content_checksum=False), 1 if lvl >= 9 else 2)
        d, back = best_time(lambda: lz4.frame.decompress(c), 3)
        assert back == pcm
        add(f'lz4 frame L{lvl} raw', len(c), e, d)
    # --- lz4 frame with the delta + byte-plane prefilter
    pf = prefilter(pcm)
    assert unfilter(pf, n) == pcm
    for lvl in (0, 9):
        e, c = best_time(lambda: lz4.frame.compress(pf, compression_level=lvl, content_checksum=False), 1 if lvl >= 9 else 2)
        d, back = best_time(lambda: unfilter(lz4.frame.decompress(c), n), 3)
        assert back == pcm
        add(f'lz4 frame L{lvl} +delta/plane', len(c), e + 0, d)
    # --- zstd
    for lvl in (1, 3, 9):
        cctx = zstandard.ZstdCompressor(level=lvl); dctx = zstandard.ZstdDecompressor()
        e, c = best_time(lambda: cctx.compress(pf), 1)
        d, back = best_time(lambda: unfilter(dctx.decompress(c), n), 3)
        add(f'zstd L{lvl} +delta/plane', len(c), e, d)
    # --- FLAC / WavPack / ALAC / TTA via ffmpeg (lossless, verified bit-exact)
    wavp = path
    for label, args, ext in (
        ('FLAC L0', ['-c:a', 'flac', '-compression_level', '0'], 'flac'),
        ('FLAC L1', ['-c:a', 'flac', '-compression_level', '1'], 'flac'),
        ('FLAC L5', ['-c:a', 'flac', '-compression_level', '5'], 'flac'),
        ('FLAC L8', ['-c:a', 'flac', '-compression_level', '8'], 'flac'),
        ('WavPack fast', ['-c:a', 'wavpack', '-compression_level', '0'], 'wv'),
        ('ALAC', ['-c:a', 'alac'], 'm4a'),
        ('TTA', ['-c:a', 'tta'], 'tta'),
    ):
        out = f'{TMP}/b_{name}.{ext}'
        e, _ = best_time(lambda: run_ff(['-i', wavp] + args + [out]), 1)
        d, dec = best_time(lambda: run_ff(['-i', out, '-f', 's16le', '-']), 3)
        ok = dec == pcm
        add(label, os.path.getsize(out), e, max(d - base, 1e-4), '' if ok else 'NOT BIT-EXACT')
    # --- lossy ADPCM: ratio and SNR
    for label, codec in (('MS ADPCM (lossy)', 'adpcm_ms'), ('IMA ADPCM (lossy)', 'adpcm_ima_wav')):
        out = f'{TMP}/b_{name}_{codec}.wav'
        e, _ = best_time(lambda: run_ff(['-i', wavp, '-c:a', codec, out]), 1)
        d, dec = best_time(lambda: run_ff(['-i', out, '-f', 's16le', '-']), 3)
        a = np.frombuffer(pcm, dtype='<i2').astype(np.float64); m = min(len(a), len(dec) // 2)
        b = np.frombuffer(dec, dtype='<i2')[:m].astype(np.float64)
        snr = 10 * np.log10((a[:m] ** 2).sum() / max(((a[:m] - b) ** 2).sum(), 1))
        add(label, os.path.getsize(out), e, max(d - base, 1e-4), f'SNR {snr:.1f} dB')
    rows.append(dict(sample=name, codec='PCM WAV', ratio=1.0, mb=round(mb, 1), enc_s=0, dec_s=0, dec_x_rt=0, note=f'{secs:.0f}s'))

json.dump(dict(base_s=base, rows=rows), open(f'{TMP}/bench.json', 'w'))
print('ffmpeg process baseline (subtracted from ffmpeg decode times): %.3fs' % base)
