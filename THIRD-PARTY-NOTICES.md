# Third-party notices

marth's Jukebox is MIT-licensed (see `LICENSE`). It uses the following third-party software. None of it is modified.

| Component | Used for | Licence | Source |
|---|---|---|---|
| CommonLibSSE-NG (an MIT-licensed fork, from the `marthofdoom/vcpkg-registry` port) | Skyrim engine bindings and SKSE plumbing, compiled into the DLL | MIT | the fork named above; upstream project CommonLibSSE-NG |
| spdlog | Logging, compiled into the DLL | MIT | https://github.com/gabime/spdlog |
| nlohmann/json | Reading Harmony server replies and the cache index, compiled into the DLL | MIT | https://github.com/nlohmann/json |
| dr_flac (`dr_libs`) by David Reid | Decoding FLAC downloads to the WAV slot files; vendored in `third_party/dr_libs/` of the source | Public domain (Unlicense) or MIT No Attribution (MIT-0), at your choice; this project uses MIT-0. Full text at the end of `dr_flac.h` | https://github.com/mackron/dr_libs |
| `SKSEMenuFramework.h` (SKSE Menu Framework API header) | Registers the settings page with the SKSE Menu Framework; vendored unmodified in `native/menu/third_party/` of the source | **GNU LGPL 2.1** (licence text: `LICENSE-LGPL-2.1` in this package) | https://github.com/QTR-Modding/SKSE-Menu-Framework-3-API |

Notes:

- The LGPL-2.1 applies only to that header file. The header resolves every function at run time from the installed
  framework DLL; it does not link the framework. The full source of this project, including the unmodified header, is
  at https://github.com/marthofdoom/marth-jukebox, so the header can be inspected or replaced by anyone.
- The **SKSE Menu Framework itself (its DLL)** is a separate mod by its own author, under its own licence. It is NOT
  included in this package; install it from its Nexus Mods page.
- Skyrim, SKSE, Address Library and Harmony are not part of this package.
