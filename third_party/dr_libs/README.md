# dr_flac (vendored)

`dr_flac.h` from [mackron/dr_libs](https://github.com/mackron/dr_libs), commit
`d422edf14ec2b81ac40a7f22b5a7f64276b5fc34` (2026-10-23, header version "v0.13.4 - TBD"), unmodified.
Chosen over the 0.13.3 release because the commits since then add metadata bounds checks and fix an
undefined shift, a NULL dereference and an overflow; the plugin decodes files downloaded from the user's server.

Author: David Reid. Licence: your choice of **public domain (Unlicense)** or **MIT No Attribution (MIT-0)**;
the full text of both is at the end of `dr_flac.h`. marth's Jukebox uses it under MIT-0.

Used by `native/pool/Audio.cpp` (FLAC -> canonical PCM WAV for the slot files); the implementation is compiled
once in `native/pool/DrFlac.cpp` (`DR_FLAC_IMPLEMENTATION`, `DR_FLAC_NO_OGG`).
