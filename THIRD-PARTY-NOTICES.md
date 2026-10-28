# Third-party notices

marth's Jukebox is MIT-licensed (see `LICENSE`). It uses the following third-party software. The vendored files
(dr_flac, the menu API header) are unmodified; the other components are built from their upstream sources via vcpkg.
CommonLibSSE-NG is a personal fork (`marthofdoom/vcpkg-registry`).

| Component | Used for | Licence | Source |
|---|---|---|---|
| CommonLibSSE-NG, a personal fork (`marthofdoom/vcpkg-registry`) | Skyrim engine bindings and SKSE plumbing, compiled into the DLL | MIT | the fork named above |
| spdlog | Logging, compiled into the DLL | MIT | https://github.com/gabime/spdlog |
| fmt (pulled in by spdlog and CommonLibSSE-NG) | String formatting, compiled into the DLL | MIT with an exception for embedding in binaries | https://github.com/fmtlib/fmt |
| rapidcsv (pulled in by CommonLibSSE-NG) | CSV parsing in CommonLibSSE-NG's Address Library loader, compiled into the DLL | BSD-3-Clause | https://github.com/d99kris/rapidcsv |
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

---

# Licence texts

## CommonLibSSE-NG (MIT)

MIT License

Copyright (c) 2018 Ryan-rsm-McKenzie

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


## spdlog (MIT)

The MIT License (MIT)

Copyright (c) 2015 Gabi Melman.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


## nlohmann/json (MIT)

MIT License. The copyright line is the one in the LICENSE.MIT file of https://github.com/nlohmann/json (not reproduced here
because it could not be read from a local copy while preparing this release):

Copyright (c) <holders named in the upstream LICENSE.MIT>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## fmt (MIT with exception)

Copyright (c) <holders named in the upstream LICENSE.rst of https://github.com/fmtlib/fmt>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

--- Optional exception to the license ---

As an exception, if, as a result of your compiling your source code, portions of this Software are embedded into a
machine-executable object form of such source code, you may redistribute such embedded portions in such object form
without including the above copyright and permission notices.

(Standard fmt licence text; the copyright line is in the upstream repository.)

## rapidcsv (BSD-3-Clause)

Copyright (c) <holder named in the upstream LICENSE of https://github.com/d99kris/rapidcsv>
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following
conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.

(Standard BSD-3-Clause text; the copyright line is in the upstream repository.)

## dr_flac and the menu API header

The full dr_flac licence choice (Unlicense / MIT-0) is at the end of `dr_flac.h` in the source; the LGPL-2.1 text for the
menu API header is in `LICENSE-LGPL-2.1` in this package.
