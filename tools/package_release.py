#!/usr/bin/env python3
"""Builds the release zips of marth's Jukebox (docs/RELEASING.md).

Run:  python3 -I tools/package_release.py --dll PATH --pdb PATH [--esp PATH] [--out-dir DIR]

Writes into --out-dir (default: ./dist):
  MarthJukebox-<version>.zip          installable by MO2 / Vortex / by hand (extract into the game's Data folder):
      SKSE/Plugins/MarthJukebox.dll
      MarthJukebox.esp
      README.md  LICENSE  THIRD-PARTY-NOTICES.md  LICENSE-LGPL-2.1
  MarthJukebox-<version>-symbols.zip  SKSE/Plugins/MarthJukebox.pdb only (crash symbolication, not for players)

<version> is the first line of the file VERSION at the repository root (native/CMakeLists.txt reads the same file, so
it is also the plugin version the DLL reports). The zips are reproducible: fixed timestamps, sorted entries.
The mod ships no audio and no secrets; this script refuses audio, INI, JSON or PDB files in the main zip.
"""
import argparse
import hashlib
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXED_TIME = (2026, 1, 1, 0, 0, 0)  # any fixed value gives reproducible output


def read_version() -> str:
    lines = (ROOT / "VERSION").read_text(encoding="utf-8").strip().splitlines()
    ver = lines[0].strip() if lines else ""
    if not re.fullmatch(r"\d+\.\d+\.\d+", ver):
        sys.exit(f"VERSION must hold one MAJOR.MINOR.PATCH line, got {ver!r}")
    return ver


def need(path: Path, what: str) -> Path:
    if not path.is_file() or path.stat().st_size == 0:
        sys.exit(f"missing or empty {what}: {path}")
    return path


def write_zip(out: Path, files: dict) -> None:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arcname in sorted(files):
            info = zipfile.ZipInfo(arcname, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, files[arcname].read_bytes(), compresslevel=9)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True, type=Path)
    ap.add_argument("--pdb", required=True, type=Path)
    ap.add_argument("--esp", type=Path, default=ROOT / "assets" / "MarthJukebox.esp")
    ap.add_argument("--out-dir", type=Path, default=Path("dist"))
    a = ap.parse_args()

    ver = read_version()
    esp = need(a.esp, "ESP")
    if esp.read_bytes()[:4] != b"TES4":
        sys.exit(f"{esp} is not a plugin file (no TES4 header)")
    main_files = {
        "SKSE/Plugins/MarthJukebox.dll": need(a.dll, "DLL"),
        "MarthJukebox.esp": esp,
        "README.md": need(ROOT / "README.md", "README.md"),
        "LICENSE": need(ROOT / "LICENSE", "LICENSE"),
        "THIRD-PARTY-NOTICES.md": need(ROOT / "THIRD-PARTY-NOTICES.md", "THIRD-PARTY-NOTICES.md"),
        "LICENSE-LGPL-2.1": need(ROOT / "native" / "menu" / "third_party" / "LICENSE-LGPL-2.1", "LGPL licence text"),
    }
    sym_files = {"SKSE/Plugins/MarthJukebox.pdb": need(a.pdb, "PDB")}

    forbidden = (".wav", ".flac", ".mp3", ".ogg", ".xwm", ".ini", ".json", ".pdb")
    for name in main_files:
        if name.lower().endswith(forbidden):
            sys.exit(f"refusing to package {name}: not allowed in the main zip")

    a.out_dir.mkdir(parents=True, exist_ok=True)
    for path, files in ((a.out_dir / f"MarthJukebox-{ver}.zip", main_files),
                        (a.out_dir / f"MarthJukebox-{ver}-symbols.zip", sym_files)):
        write_zip(path, files)
        print(f"{path}  {path.stat().st_size} bytes  sha256 {hashlib.sha256(path.read_bytes()).hexdigest()}")
        with zipfile.ZipFile(path) as z:
            for i in z.infolist():
                print(f"    {i.filename}  {i.file_size}")


if __name__ == "__main__":
    main()
