# pack.ps1 - build the installable Godot addon zip (bw_audio-godot-<version>.zip).
#
# ASCII ONLY, deliberately: Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a stray em-dash
# becomes mojibake and breaks the PARSER. CI runs pwsh, developers run 5.1. Keep it plain.
#
# This zip IS the distribution, the same way the Unity .tgz is. There is no registry: Godot's Asset
# Library wants a public repo laid out as an installable project, which this is not. A user unzips it
# into their project so they end up with addons/bw_audio/, and that is the whole install.
#
# Like tools/upm/pack.ps1 it stages into a CLEAN directory first, and for the SAME reason:
# addons/bw_audio/bin/ is gitignored (it is build output), so anything that packs the working tree
# naively produces an addon with no binaries. It installs fine and then fails to load.
#
# Two library flavours are built, because one is not enough to be useful:
#
#   editor           - what the Godot EDITOR loads. Without it the addon does nothing when you open
#                      the project, which reads as "the extension is broken".
#   template_release - what an EXPORTED game loads. Without it the addon works right up until someone
#                      exports, then fails there.
#
# GODOTCPP_TARGET is a build-WIDE choice in godot-cpp, so each flavour needs its own configure tree.
# That is why this script exists at all rather than being one cmake invocation.
#
# A THIRD library ships beside those two: android arm64-v8a template_release, what a standalone
# headset export loads. It is a cross-build, so it needs the NDK rather than MSVC. This script
# builds it when an NDK is on the machine, takes it from -AndroidFrom when one is handed over (that
# is how CI passes the android job's output into the Windows pack), and FAILS when it has neither -
# the manifest promises the file, and an addon that ships the promise without the file fails at
# load on a headset.
#
#   powershell -File tools/godot/pack.ps1 [-Version 0.3.0] [-OutDir dist/godot]
#                                         [-AndroidFrom <dir with the two android .so files>]
#
# The GIT TAG is the single source of truth for the version, as with the Unity package.
[CmdletBinding()]
param(
    [string] $Version,                 # optional: stamp this version (e.g. from a v0.3.0 tag)
    [string] $OutDir,                  # default: <repo>/dist/godot
    [string] $AndroidFrom,             # directory holding the PREBUILT Android arm64 pair (CI hands
                                       # this over from the android job); default: build it here
    [switch] $SkipBuild                # reuse existing build trees (local iteration)
)
$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$repo = (Resolve-Path (Join-Path $here '../..')).Path
$addon = Join-Path $repo 'bindings/godot/addons/bw_audio'
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist/godot' }

if (-not $Version) {
    # Same dev-version idea as the Unity pack: base tag + distance + hash, so a non-tag build is
    # honestly labeled rather than pretending to be a release.
    $Version = '0.0.0-dev'
    if (Get-Command git -ErrorAction SilentlyContinue) {
        try {
            $desc = & git -C $repo describe --tags --long --dirty --match 'v*' 2>$null
            if ($LASTEXITCODE -eq 0 -and $desc) {
                $desc = ($desc | Select-Object -First 1).Trim()
                if ($desc -match '^v?(.+?)-(\d+)-g([0-9a-fA-F]+)(-dirty)?$') {
                    $Version = if ([int]$Matches[2] -eq 0) { $Matches[1] }
                               else { "$($Matches[1])-dev.$($Matches[2]).g$($Matches[3])" }
                }
            }
        } catch { }
    }
}

# ---- build both library flavours ------------------------------------------------------------
if (-not $SkipBuild) {
    foreach ($target in @('editor', 'template_release')) {
        $tree = Join-Path $repo "build-godot-$target"
        Write-Host "==> configuring $target"
        # Deliberately NOT passing -DBWA_BUILD_TESTS=OFF. These tree names are shared with CI,
        # which caches them; turning tests off here would persist into the cache and leave the
        # next run's ctest selecting nothing - and an empty selection reads as a pass. Only the
        # bwa_gdextension target is built below, so leaving tests enabled costs nothing.
        & cmake -S $repo -B $tree -A x64 -DBWA_BUILD_GODOT=ON "-DGODOTCPP_TARGET=$target"
        if ($LASTEXITCODE -ne 0) { throw "configure failed for $target" }
        Write-Host "==> building $target"
        & cmake --build $tree --config RelWithDebInfo --target bwa_gdextension
        if ($LASTEXITCODE -ne 0) { throw "build failed for $target" }
    }
}

# ---- the Android arm64 library ---------------------------------------------------------------
# Two files, both named by the manifest: the extension itself and the engine library it imports.
# They end up in addons/bw_audio/bin/ exactly like the Windows pair, so the manifest-driven staging
# below needs no special case for them.
$addonBin = Join-Path $addon 'bin'
New-Item -ItemType Directory -Force -Path $addonBin | Out-Null
$androidPair = @('libbw_audio_gd.android.template_release.arm64.so', 'libbw_audio.so')

if ($AndroidFrom) {
    $src = (Resolve-Path $AndroidFrom).Path
    Write-Host "==> taking the android arm64 library from $src"
    foreach ($f in $androidPair) {
        $p = Join-Path $src $f
        if (-not (Test-Path $p)) { throw "-AndroidFrom '$src' has no $f" }
        Copy-Item $p $addonBin -Force
    }
} elseif (-not $SkipBuild) {
    # The NDK, looked up the same way tools/android/run-tests.ps1 does it.
    $sdk = $env:ANDROID_HOME
    if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
    if (-not $sdk -and $env:LOCALAPPDATA) { $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
    $ndk = $env:ANDROID_NDK_HOME
    if (-not $ndk) { $ndk = $env:ANDROID_NDK_ROOT }
    if (-not $ndk -and $sdk -and (Test-Path (Join-Path $sdk 'ndk'))) {
        $newest = Get-ChildItem (Join-Path $sdk 'ndk') -Directory |
                  Sort-Object Name -Descending | Select-Object -First 1
        if ($newest) { $ndk = $newest.FullName }
    }
    if (-not $ndk -or -not (Test-Path (Join-Path $ndk 'build/cmake/android.toolchain.cmake'))) {
        throw ("No Android NDK, and no -AndroidFrom. The addon's manifest names an android arm64 " +
               "library, so packing without one would ship an addon that fails to load on a headset. " +
               "Either set ANDROID_NDK_HOME (see docs/build.md, 'Android') or pass -AndroidFrom " +
               "<dir holding $($androidPair -join ' + ')>, which is what CI does with the android job's artifact.")
    }
    # The NDK ships no generator; the SDK's cmake package brings a ninja, and one on PATH does too.
    $ninja = $null
    if ($sdk -and (Test-Path (Join-Path $sdk 'cmake'))) {
        $ninja = Get-ChildItem (Join-Path $sdk 'cmake') -Directory | Sort-Object Name -Descending |
                 ForEach-Object { Join-Path $_.FullName 'bin/ninja.exe' } |
                 Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $ninja) {
        $onPath = Get-Command ninja -ErrorAction SilentlyContinue
        if ($onPath) { $ninja = $onPath.Source }
    }
    if (-not $ninja) { throw "No ninja for the android build: sdkmanager --install 'cmake;3.31.6', or put one on PATH" }

    $tree = Join-Path $repo 'build-android-godot'
    Write-Host "==> configuring android arm64-v8a (template_release)"
    & cmake -S $repo -B $tree -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBWA_BUILD_GODOT=ON -DGODOTCPP_TARGET=template_release
    if ($LASTEXITCODE -ne 0) { throw 'configure failed for android arm64-v8a' }
    Write-Host "==> building android arm64-v8a"
    & cmake --build $tree --target bwa_gdextension      # its POST_BUILD copies libbw_audio.so along
    if ($LASTEXITCODE -ne 0) { throw 'build failed for android arm64-v8a' }

    # Strip the debug info out of the two files this addon SHIPS. The Windows pair already ships
    # without its (a .pdb the addon does not carry), and an APK pays for every byte it holds: this
    # is 11 MB down to 1.9 for the extension and 20.7 down to 7.0 for the engine, which carries a
    # static phonon (it was 2.5 down to 0.4 before Android had one). A missing
    # llvm-strip is a warning, not a failure - a fat library still loads. CI hands over binaries it
    # has already stripped, so the -AndroidFrom path above needs none of this.
    $strip = Get-ChildItem (Join-Path $ndk 'toolchains/llvm/prebuilt') -Directory -ErrorAction SilentlyContinue |
             ForEach-Object { Join-Path $_.FullName 'bin/llvm-strip.exe' } |
             Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($strip) {
        foreach ($f in $androidPair) { & $strip --strip-debug (Join-Path $addonBin $f) }
        if ($LASTEXITCODE -ne 0) { throw 'llvm-strip failed on the android libraries' }
    } else {
        Write-Warning "no llvm-strip in the NDK: shipping the android libraries with their debug info (large, but valid)."
    }
}
foreach ($f in $androidPair) {
    if (-not (Test-Path (Join-Path $addonBin $f))) {
        throw "missing $f in addons/bw_audio/bin - build the android flavour (drop -SkipBuild) or pass -AndroidFrom"
    }
}

# ---- stage ----------------------------------------------------------------------------------
# Kept after packing, not cleaned up: tools/dist/publish-branch.ps1 pushes this exact tree to the
# `godot` branch, where its root becomes addons/bw_audio/ - which is what the Asset Library needs,
# since it downloads a repo ARCHIVE at a commit rather than a release asset. Mirrors the Unity
# pack's build/upm/package stage, which the `unity` branch is published from the same way.
$stage = Join-Path $repo 'build/godot/addon'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$dest = Join-Path $stage 'addons/bw_audio'
New-Item -ItemType Directory -Force -Path (Join-Path $dest 'bin') | Out-Null

Copy-Item (Join-Path $addon 'bw_audio.gdextension') $dest

# The binaries the manifest promises - read OUT of the manifest rather than restated here, so the
# two can never drift. The Asset Store makes this a hard rule ("any shared library files listed in
# a .gdextension file must be present"), and a missing one is a runtime load failure anyway.
$binsrc = Join-Path $addon 'bin'
$manifest = Get-Content (Join-Path $addon 'bw_audio.gdextension')
$section = ''
$want = [System.Collections.Generic.List[string]]::new()
foreach ($line in $manifest) {
    $t = $line.Trim()
    if ($t -match '^\[(.+)\]$') { $section = $Matches[1]; continue }
    if ($t.StartsWith(';') -or -not $t) { continue }
    if ($section -eq 'libraries' -and $t -match '=\s*"res://addons/bw_audio/bin/([^"]+)"') {
        if (-not $want.Contains($Matches[1])) { $want.Add($Matches[1]) }   # flavours may share a file
    }
}
if ($want.Count -eq 0) { throw "no libraries found in bw_audio.gdextension - is the manifest intact?" }
# Not in [libraries]: the loader resolves the engine library as an import of the extension. Both
# spellings, because the addon carries both platforms' binaries.
$want.Add('bw_audio.dll')
$want.Add('libbw_audio.so')

foreach ($f in $want) {
    $p = Join-Path $binsrc $f
    if (-not (Test-Path $p)) { throw "missing $f - build every flavour first (drop -SkipBuild)" }
    Copy-Item $p (Join-Path $dest 'bin')
}
Write-Host "verified $($want.Count) binaries against the manifest"
# Phonon is linked statically into the engine library since 2026-09-15, so a Steam Audio build and a
# no-SDK build stage exactly the same files and the manifest needs no [dependencies] surgery.

# The playground ships WITH the addon: it lives inside addons/bw_audio/ precisely so it can
# (its res:// paths work unchanged in any project), and it is the consumer demo - open
# playground/playground.tscn and press play. Exclude the editor's .uid sidecars: they are
# per-checkout cache identity, and a user's editor mints its own.
Copy-Item (Join-Path $addon 'playground') $dest -Recurse
Get-ChildItem (Join-Path $dest 'playground') -Filter '*.uid' | Remove-Item

# GPLv3 travels with the binaries, same as every other artifact this repo ships.
Copy-Item (Join-Path $repo 'LICENSE') $dest
Copy-Item (Join-Path $repo 'THIRD_PARTY-NOTICES.md') $dest -ErrorAction SilentlyContinue
# The binding README serves two readers: the addon CONSUMER (install, the coordinate seam,
# the node reference, the traps) and the engine DEVELOPER (CMake trees, ctest, distribution,
# the demo project you can "open and press play"). None of the developer half exists in the
# shipped addon, so staging the file verbatim tells a paying-attention reader to open a demo
# that is not there. The source README fences its dev-only regions in <!-- dev -->/<!-- /dev -->
# markers; the staged copy drops them. Unbalanced markers fail the pack rather than shipping
# a half-stripped document.
$rl = Get-Content (Join-Path $repo 'bindings/godot/README.md')
$out = [System.Collections.Generic.List[string]]::new()
$depth = 0
foreach ($line in $rl) {
    if ($line -match '^\s*<!--\s*dev\s*-->')  { $depth++; continue }
    if ($line -match '^\s*<!--\s*/dev\s*-->') { $depth--; if ($depth -lt 0) { throw "README: '<!-- /dev -->' without opener" }; continue }
    if ($depth -eq 0) { $out.Add($line) }
}
if ($depth -ne 0) { throw "README: unbalanced <!-- dev --> markers (depth $depth at EOF)" }
# Collapse the blank-line runs the removals leave behind, so the result reads as written.
$textReadme = ($out -join "`n") -replace "(`n){3,}", "`n`n"
Set-Content -Path (Join-Path $dest 'README.md') -Value $textReadme -Encoding utf8

$commit = try { (& git -C $repo rev-parse HEAD).Trim() } catch { 'unknown' }
@"
bw_audio for Godot $Version
commit $commit

Install: copy addons/bw_audio/ into your Godot project, then enable nothing - a GDExtension
loads on project open. Restart the editor after copying. To hear it work, open
addons/bw_audio/playground/playground.tscn and press play (eight by-ear scenes; falls back
to silent visual-only mode without an ASIO device).

Requires Godot 4.7 or newer: compatibility_minimum in the manifest now matches the 4.7
extension API the binary is compiled against (earlier packs understated it as 4.4). The
extension ships for Windows x64 (the desktop, where the engine's device path is ASIO) and
for Android arm64-v8a (a standalone headset, stereo out through AAudio). The Android
libraries are a NO-SDK build: no Steam Audio there yet, so binaural is the fallback pan.

Licensed GPLv3 (see LICENSE). Complete corresponding source: this repo at the commit above.
"@ | Set-Content -Path (Join-Path $dest 'DIST.txt') -Encoding ascii

# A README at the STAGE ROOT - sibling of addons/, not inside it. This lands on the `godot`
# distribution branch (publish-branch.ps1 pushes the whole stage), where it is the only thing
# GitHub will render: without it the branch page is one bare addons/ folder that reads as a
# broken checkout to anyone arriving from a store listing. It does NOT enter the release zip
# (the tar below packs only addons/), so a manual unzip stays exactly the addon. The one cost:
# an Asset Library install merges the repo archive, so this file can land in a user's project
# root - Godot's install dialog lets them deselect it, and the file says it is safe to delete.
@"
# bw_audio - Godot addon (distribution branch)

This branch is machine-published by CI on every release tag. It exists so the Godot Asset
Library / Asset Store, which download a repository archive at a pinned commit, can serve the
addon WITH its binaries - which are deliberately not committed to ``main``.

The addon is ``addons/bw_audio/``: a GDExtension control client for the bw_audio spatial
audio engine (26-speaker CAVE array over ASIO, binaural monitor). Windows x64 only.

- Install: copy ``addons/bw_audio/`` into your project, restart the editor. Nothing to enable.
- Try it: open ``addons/bw_audio/playground/playground.tscn`` and press play.
- Docs, license, and the exact source commit: inside ``addons/bw_audio/``.
- Source, issues, releases: https://github.com/aforren1/cave-audio (branch ``main``).

If this file ended up in your project root via an Asset Library install, it is safe to delete
- only ``addons/bw_audio/`` matters.
"@ | Set-Content -Path (Join-Path $stage 'README.md') -Encoding ascii

# ---- doc pointers --------------------------------------------------------------------------
# The zip carries the ADDON, not the repo's docs/ tree, so a reference to a repo doc that survives
# into the stage points at a file the installing user does not have. Shared with the Unity pack,
# which ships the same kind of prose from the same repo; see tools/dist/doc-pointers.ps1 for what
# it rewrites and what it refuses to let out.
& (Join-Path $repo 'tools/dist/doc-pointers.ps1') -Stage $stage -Repo $repo -Ref $commit `
    -Extensions @('.md', '.txt', '.gd', '.tscn', '.gdextension', '.cfg')

# ---- zip ------------------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "bw_audio-godot-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# tar (bsdtar, ships with Windows 10+), NOT Compress-Archive: under Windows PowerShell 5.1 the
# latter writes BACKSLASH entry paths, which the zip spec forbids and non-Windows extractors
# (including whatever a store backend runs) turn into literal 'addons\bw_audio\...' filenames.
# bsdtar always writes forward slashes, on both 5.1 and pwsh. -C so the archive roots at
# addons/bw_audio/... and unzipping into a project lands the addon where Godot looks for it.
if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
    throw "tar not found on PATH (it ships with Windows 10+ as bsdtar)."
}
& tar -a -cf $zip -C $stage addons
if ($LASTEXITCODE -ne 0) { throw "tar failed" }

Write-Host "packed $zip"
Write-Host "stage kept at $stage (publish-branch.ps1 pushes this to the 'godot' branch)"
if ($env:GITHUB_OUTPUT) { "version=$Version" | Out-File -FilePath $env:GITHUB_OUTPUT -Append }
