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
#   powershell -File tools/godot/pack.ps1 [-Version 0.3.0] [-OutDir dist/godot]
#
# The GIT TAG is the single source of truth for the version, as with the Unity package.
[CmdletBinding()]
param(
    [string] $Version,                 # optional: stamp this version (e.g. from a v0.3.0 tag)
    [string] $OutDir,                  # default: <repo>/dist/godot
    [switch] $SkipBuild                # reuse existing build trees (local iteration)
)
$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$repo = (Resolve-Path (Join-Path $here '../..')).Path
$addon = Join-Path $repo 'bindings/godot/addons/bw_audio'
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist/godot' }

if (-not $Version) {
    # Same dev-version idea as the Unity pack: base tag + distance + hash, so a non-tag build is
    # honestly labelled rather than pretending to be a release.
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
$want.Add('bw_audio.dll')      # not in [libraries]: the OS resolves it as an import of the extension

foreach ($f in $want) {
    $p = Join-Path $binsrc $f
    if (-not (Test-Path $p)) { throw "missing $f - build every flavour first (drop -SkipBuild)" }
    Copy-Item $p (Join-Path $dest 'bin')
}
Write-Host "verified $($want.Count) binaries against the manifest"
# phonon.dll only exists in a Steam Audio build. Its ABSENCE is legitimate (the no-SDK build is
# supported: ISM + FDN + manual occlusion), so warn rather than fail - but say which addon this is,
# because "no HRTF monitor" is otherwise a confusing thing to discover later.
$phonon = Join-Path $binsrc 'phonon.dll'
if (Test-Path $phonon) {
    Copy-Item $phonon (Join-Path $dest 'bin')
} else {
    Write-Warning "phonon.dll not present: packing a NO-SDK addon (no HRTF monitor, no ray-traced occlusion/reflections/pathing)."
    # The manifest's [dependencies] promises phonon.dll unconditionally (the SDK build is the
    # normal one). A no-SDK pack must not ship that promise: the store requires listed files
    # to be present, and Godot's exporter would fail on the missing entry. Strip the phonon
    # lines from the STAGED copy - comma first, so the remaining dictionary stays parseable.
    $mf = Join-Path $dest 'bw_audio.gdextension'
    $text = [IO.File]::ReadAllText($mf)
    $text = [regex]::Replace($text, ',\s*"res://addons/bw_audio/bin/phonon\.dll":\s*""', '')
    # Guard on FUNCTIONAL references (res:// paths) only - the manifest's comments also say
    # "phonon.dll" while explaining why it is listed, and prose is not a promise to load.
    if ($text -match '"res://[^"]*phonon\.dll"') { throw "failed to strip phonon.dll from the staged manifest" }
    [IO.File]::WriteAllText($mf, $text)
    Write-Host "stripped phonon.dll from the staged manifest's [dependencies]"
}

# GPLv3 travels with the binaries, same as every other artifact this repo ships.
Copy-Item (Join-Path $repo 'LICENSE') $dest
Copy-Item (Join-Path $repo 'THIRD_PARTY-NOTICES.md') $dest -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repo 'bindings/godot/README.md') $dest

$commit = try { (& git -C $repo rev-parse HEAD).Trim() } catch { 'unknown' }
@"
bw_audio for Godot $Version
commit $commit

Install: copy addons/bw_audio/ into your Godot project, then enable nothing - a GDExtension
loads on project open. Restart the editor after copying.

Requires Godot 4.4 or newer (compatibility_minimum in the manifest); built and tested against
4.7. The extension is Windows x64 only, because the engine's device path is ASIO.

Licensed GPLv3 (see LICENSE). Complete corresponding source: this repo at the commit above.
"@ | Set-Content -Path (Join-Path $dest 'DIST.txt') -Encoding ascii

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
