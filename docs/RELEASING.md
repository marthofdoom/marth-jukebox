# Releasing

Distribution is GitHub-only (CLAUDE.md "Release"). A release is a tag plus a **published** (not draft) GitHub release.

## How the zips are built

CI (`.github/workflows/native.yml`) runs `tools/package_release.py` after the Release build and uploads the result as the
artifact `MarthJukebox-<version>-zips`. The version is the one line in the root file `VERSION`; `native/CMakeLists.txt`
reads the same file, so the DLL's reported plugin version and the zip names cannot drift.

```
python3 -I tools/package_release.py --dll native/build/release/MarthJukebox.dll \
                                    --pdb native/build/release/MarthJukebox.pdb [--esp assets/MarthJukebox.esp] [--out-dir dist]
```

Output (both zips are reproducible: fixed timestamps, sorted entries):

- `MarthJukebox-<version>.zip` (players): `SKSE/Plugins/MarthJukebox.dll`, `MarthJukebox.esp` (from `assets/`),
  and at the zip root `README.md`, `LICENSE`, `THIRD-PARTY-NOTICES.md`, `LICENSE-LGPL-2.1`. No PDB, no audio, no INI.
  Installable as-is by MO2 and Vortex, or extracted into `Data/`.
- `MarthJukebox-<version>-symbols.zip`: `SKSE/Plugins/MarthJukebox.pdb` only, for symbolicating crash reports.

The script exits non-zero if an input is missing or empty, if the ESP lacks a `TES4` header, or if a forbidden file type
(audio, INI, JSON, PDB) would enter the main zip. The CI step then fails, so a green CI run on the release commit means
the zips exist. GitHub wraps an uploaded artifact in one more zip when you download it; unwrap once.

## Checklist

1. `VERSION` holds the release number (`MAJOR.MINOR.PATCH`). Nothing else carries a version by hand
   (`native/vcpkg.json` keeps `0.1.0`; it is only a manifest name and changing it would invalidate the vcpkg cache).
2. README.md is current: every user-facing change since the last release is described, "Known limits" is honest, the
   version line near the top matches. A release without current instructions is not done (CLAUDE.md).
3. `docs/NEXUS-COMPLIANCE.md` is current if network destinations, bundled content or the mod-page text changed.
4. `THIRD-PARTY-NOTICES.md` lists every third-party component compiled into the DLL or vendored (dr_flac, the SKSE Menu
   Framework header, CommonLibSSE-NG, spdlog, nlohmann/json). New dependency = new row.
5. `assets/MarthJukebox.esp` matches `tools/make_esp.py`: `python3 -I tools/make_esp.py /tmp/check.esp` and compare it with
   the committed file (only needed when the generator changed).
6. Changes are merged to `main`; the CI run on that exact commit (`headSha` equals the commit) is **green**, including the
   "Package release zips" step. Ignore `cancelled/pending/queued`; wait.
7. Download the artifact from that run, unwrap, and check: the file list above, the DLL's sha256, and that the PDB is only
   in the symbols zip. Install the main zip into a clean MO2 profile and run the in-game test (docs/INTEGRATION-TEST.md).
8. Tag `v<version>` on that commit and create the GitHub release (published, not draft). Attach both zips and the DLL
   sha256 in the notes; the notes name the verified runtime (1.6.1170 on Proton) and the known limits.
9. The Nexus kit (`assets/nexus-description.bbcode`, banner) stays current, but Nexus upload is on hold and is a manual
   step for the maintainer.

Never put a server address, a personal key or any audio into a release, the README, an issue or a log you attach.
