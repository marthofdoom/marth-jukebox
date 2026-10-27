# Vendored: SKSEMenuFramework.h

- File: `SKSEMenuFramework.h`, unmodified copy of `SKSEMenuFramework.h` from
  https://github.com/QTR-Modding/SKSE-Menu-Framework-3-API (the header-only API repo, licence **LGPL-2.1**;
  repo last pushed 2026-08-29). It resolves every function at run time from the installed framework DLL
  (`GetModuleHandleW(L"SKSEMenuFramework")`), so nothing is linked.
- Our code (MIT) only includes it and calls it; per the framework's README the header's licence does not require
  mods using it as a library to be open source. Keep this notice and the LGPL text reference next to the file.
- The framework DLL itself is "All Rights Reserved" and is **never bundled**: users install it from Nexus
  (docs/MENU-FRAMEWORK.md section 2). Do not copy the framework's GPL source into this repo.
- To update: download the header again from the API repo, diff, rebuild, and re-test against the installed framework
  version (docs/MENU-FRAMEWORK.md section 7).
