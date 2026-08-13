# Third-party notices

Notice text accompanying bw_audio binary distributions. `https://github.com/aforren1/cave-audio/blob/fb85546ccff1/docs/build.md` has the
full dependency and licensing discussion; this file is what ships next to the
binaries.

## In `bw_audio.dll`

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
- **Steam Audio (`phonon.dll`)** — Apache License 2.0, Copyright Valve
  Corporation. Ships alongside `bw_audio.dll` in with-SDK builds, including the
  CI artifact; keep the two DLLs together.
  License: https://github.com/ValveSoftware/steam-audio/blob/master/LICENSE.md

## In the tools (`bwa_playground`, `bwa_layout_tool`, `bwa_calib_view`)

- **godot-cpp** — MIT (text below). Copyright (c) 2017-present Godot Engine
  contributors. Statically linked into the Godot extension
  (`bw_audio_gd.*.dll`) only — no other artifact carries it.
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

## MIT License (godot-cpp, Dear ImGui, ImPlot, ImPlot3D, cJSON)

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

## zlib/libpng License (raylib, rlImGui)

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
