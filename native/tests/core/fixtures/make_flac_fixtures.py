#!/usr/bin/env python3
"""Regenerates the FLAC decode fixtures used by AudioTests.cpp (mj_core_tests).

    python3 -m venv /tmp/v && /tmp/v/bin/pip install soundfile numpy
    /tmp/v/bin/python -I native/tests/core/fixtures/make_flac_fixtures.py

Writes, next to this script:
  golden_s16.flac  0.3 s, 44.1 kHz, stereo, 16-bit, encoded by libFLAC (via libsndfile), so the
                   decoder sees real LPC/fixed subframes, not a hand-made stream.
  golden_s16.wav   the SAME samples as a canonical 44-byte-header PCM WAV, written here with struct
                   (independent of any decoder). FLAC is lossless, so decoding the .flac must give
                   exactly these bytes.
  mono_s24.flac    0.05 s, 48 kHz, mono, 24-bit: valid FLAC that the engine path must refuse.
The signal is synthetic (sines, a deterministic LCG noise, full-scale extremes, silence): no audio
from anywhere else is in the repository.
"""
import os
import struct

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))


def canonical_wav(path, pcm, rate):
    frames, ch = pcm.shape
    data = pcm.astype("<i2").tobytes()
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + b"fmt " +
           struct.pack("<IHHIIHH", 16, 1, ch, rate, rate * ch * 2, ch * 2, 16) +
           b"data" + struct.pack("<I", len(data)))
    with open(path, "wb") as f:
        f.write(hdr + data)


def main():
    rate, n = 44100, 13230
    t = np.arange(n) / rate
    left = 0.5 * np.sin(2 * np.pi * 440 * t) * np.minimum(1.0, t * 20)
    x, noise = 12345, np.empty(n)
    for i in range(n):  # deterministic LCG noise, identical on every machine
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        noise[i] = (x / 0x7FFFFFFF) * 2 - 1
    right = 0.3 * np.sin(2 * np.pi * 1000 * t) + 0.2 * noise
    pcm = np.stack([np.round(left * 32767), np.round(right * 32767)], axis=1).astype(np.int32)
    pcm[1000:1010, 0] = 32767     # full-scale extremes
    pcm[1010:1020, 0] = -32768
    pcm[5000:6000, :] = 0         # digital silence
    pcm = pcm.astype(np.int16)
    sf.write(os.path.join(HERE, "golden_s16.flac"), pcm, rate, format="FLAC", subtype="PCM_16")
    canonical_wav(os.path.join(HERE, "golden_s16.wav"), pcm, rate)
    back, r = sf.read(os.path.join(HERE, "golden_s16.flac"), dtype="int16")
    assert r == rate and np.array_equal(back, pcm), "libsndfile round trip mismatch"

    m = (np.sin(2 * np.pi * 300 * np.arange(2400) / 48000) * 4000000).astype(np.int32)
    sf.write(os.path.join(HERE, "mono_s24.flac"), m, 48000, format="FLAC", subtype="PCM_24")
    print("fixtures written")


if __name__ == "__main__":
    main()
