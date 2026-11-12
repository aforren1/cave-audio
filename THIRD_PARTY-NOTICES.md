# Third-party notices

Notice text accompanying bw_audio binary distributions. `docs/build.md` has the
full dependency and licensing discussion; this file is what ships next to the
binaries.

## In the engine library (`bw_audio.dll`, `libbw_audio.so`, `libbw_audio.dylib`)

The ASIO SDK below is in the Windows DLL only, because ASIO is a Windows driver model.
Everything else is in every platform's build of the library.

- **Steinberg ASIO SDK** — used under its GPLv3 option. This repository and the
  distributed DLL are GPLv3 (see `LICENSE`). (c) Steinberg Media Technologies GmbH.
  The SDK is statically linked into `bw_audio.dll`, so its source is part of this
  DLL's GPLv3 corresponding source. It ships as `asio-sdk-src.zip` alongside the
  binaries (and as a `bw_audio-asio-sdk-src-<tag>.zip` asset on each GitHub Release),
  since the repo itself fetches the SDK at build time rather than vendoring it.
- **dr_libs** (dr_wav / dr_flac / dr_mp3) by David Reid — your choice of public
  domain (Unlicense) or MIT No Attribution.
- **cJSON** — MIT (text below). Copyright (c) 2009-2017 Dave Gamble and cJSON
  contributors.
- **Steam Audio (phonon)** — Apache License 2.0, Copyright Valve Corporation.
  Statically linked into `bw_audio.dll` in with-SDK builds, including the CI
  artifact, together with the mysofa, zlib and pffft archives it links. Its
  source, at the pinned commit and with the one patch this repo applies, ships as
  `bw_audio-steam-audio-src-<tag>.zip` on each GitHub Release.
  License: https://github.com/ValveSoftware/steam-audio/blob/master/LICENSE.md
  phonon links these archives, so they are in the library too:
  - **libmysofa**: BSD-3-Clause (text below). Copyright 2016 Christian Hoene, Symonics GmbH.
  - **zlib** 1.3.1: zlib License (text below). Copyright (C) 1995-2024 Jean-loup Gailly and
    Mark Adler.
  - **PFFFT**: the FFTPACK license (text below). Copyright (c) 2004 the University
    Corporation for Atmospheric Research ("UCAR"); Copyright (c) 2013 Julien Pommier.
  - **FlatBuffers** 1.12 (header code compiled into phonon): Apache License 2.0, Copyright
    2014 Google Inc.

## In the Python wheels (`bw_audio-*.whl`)

The wheel carries the engine library above, plus the `_bwa` extension module:

- **nanobind**: BSD-3-Clause (text below). Copyright (c) 2022 Wenzel Jakob. Statically
  linked into `_bwa` (`NB_STATIC`).
- **robin_map** (bundled with nanobind): MIT (text below). Copyright (c) 2017 Thibaut
  Goetghebuer-Planchon.

## In the tools (`bwa_playground`, `bwa_layout_tool`, `bwa_calib_view`)

- **godot-cpp** — MIT (text below). Copyright (c) 2017-present Godot Engine
  contributors. Statically linked into the Godot extension
  (`bw_audio_gd.*`, one per platform and flavor) only — no other artifact carries it.
- **Dear ImGui** — MIT (text below). Copyright (c) 2014-2026 Omar Cornut.
- **ImPlot** — MIT (text below). Copyright (c) 2020 Evan Pezent.
- **ImPlot3D** — MIT (text below). Copyright (c) 2024-2026 Breno Cunha Queiroz.
- **Dear ImGui Test Engine** — "Dear ImGui Test Engine License" v1.04, used under
  its free tier for open-source software (this project is GPLv3 open source).
  **Not MIT** — read the license before reusing these binaries inside closed
  software:
  https://github.com/ocornut/imgui_test_engine/blob/main/imgui_test_engine/LICENSE.txt
- **raylib** — zlib/libpng (text below). Copyright (c) 2013-2024 Ramon
  Santamaria (@raysan5).
- **rlImGui** — zlib/libpng (text below). Copyright (c) 2020-2021 Jeffery Myers.
- **Roboto** (embedded font) — Apache License 2.0, Copyright Google.
  https://www.apache.org/licenses/LICENSE-2.0
- **Tracy Profiler** — BSD-3-Clause, Copyright (c) Bartosz Taudul. Only in
  builds made with `BWA_TRACY=ON` (not in CI artifacts).

`bwa_calibrate` and `bwa_zylia_probe` are console tools; they add nothing beyond
the `bw_audio.dll` list.

## In the web demo (GitHub Pages artifact)

- **coi-serviceworker** — MIT (text below). Copyright (c) 2021 Guido Zuidhof. Vendored at
  `bindings/web/deploy/coi-serviceworker.js`; it adds the COOP/COEP headers GitHub Pages cannot.
  https://github.com/gzuidhof/coi-serviceworker

- **three.js** — MIT (text below). Copyright (c) 2010-2026 three.js authors. Version **0.186.0**,
  pinned. NOT committed: `tools/wasm/fetch-web-vendor.sh` fetches `build/three.module.js`,
  `build/three.core.js` and `LICENSE` into `bindings/web/dist/vendor/three/` at build time, checks
  each against a sha256 recorded in that script, and `bindings/web/deploy/stage.sh` copies them
  into the Pages artifact. The upstream `LICENSE` text travels with the files. The playground page
  imports it from there because the site is served under COEP require-corp, where a CDN script is
  blocked. https://github.com/mrdoob/three.js

## MIT License (godot-cpp, Dear ImGui, ImPlot, ImPlot3D, cJSON, coi-serviceworker, three.js, robin_map)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## zlib/libpng License (raylib, rlImGui, zlib)

This software is provided "as-is", without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use
of this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to
the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a product,
   an acknowledgment in the product documentation would be appreciated but is
   not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

## BSD 3-Clause License (libmysofa, nanobind, Tracy)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## FFTPACK License (PFFFT)

Copyright (c) 2004 the University Corporation for Atmospheric Research ("UCAR").
All rights reserved. Developed by NCAR's Computational and Information Systems
Laboratory, UCAR, www.cisl.ucar.edu.

Redistribution and use of the Software in source and binary forms, with or
without modification, is permitted provided that the following conditions are
met:

- Neither the names of NCAR's Computational and Information Systems Laboratory,
  the University Corporation for Atmospheric Research, nor the names of its
  sponsors or contributors may be used to endorse or promote products derived
  from this Software without specific prior written permission.
- Redistributions of source code must retain the above copyright notices, this
  list of conditions, and the disclaimer below.
- Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions, and the disclaimer below in the documentation and/or other
  materials provided with the distribution.

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING, BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE SOFTWARE.
