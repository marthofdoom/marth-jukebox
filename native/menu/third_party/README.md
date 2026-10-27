# Vendored: SKSEMenuFramework.h

- File: `SKSEMenuFramework.h`, an UNMODIFIED copy of `SKSEMenuFramework.h` from
  https://github.com/QTR-Modding/SKSE-Menu-Framework-3-API (header-only API repo), file last changed in commit
  `1dcb70179076aae4ab626f43c5baab2735ca5877` (2026-08-29). It resolves every function at run time from the installed
  framework DLL (`GetModuleHandleW(L"SKSEMenuFramework")`), so nothing is linked.
- Licence: **GNU LGPL 2.1** (copyright the framework authors). The licence text is in `LICENSE-LGPL-2.1` next to the
  header, copied verbatim from the same repository. Per the framework's README the header's licence does not require
  mods that use it as a library to be open source; our code (MIT) only includes it and calls it. Keep this notice and the
  licence file with the header; do not edit the header (if a change is ever needed, publish the changed header under the
  LGPL-2.1 as the licence requires).
- The framework DLL itself is "All Rights Reserved" and is **never bundled**: users install it from Nexus
  (docs/MENU-FRAMEWORK.md section 2). Do not copy the framework's GPL source into this repo.
- To update: download the header again from the API repo, diff, rebuild, re-test against the installed framework version
  (docs/MENU-FRAMEWORK.md section 7), and update the commit above.
