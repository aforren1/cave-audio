# pack.ps1 - build the installable UPM tarball (com.brainworks.bw_audio-<version>.tgz).
#
# ASCII ONLY, deliberately: Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a stray em-dash
# or arrow becomes mojibake and breaks the PARSER. CI runs pwsh, developers run 5.1. Keep it plain.
#
# This tarball IS the distribution. CI attaches it to a GitHub Release on a v* tag, and Unity installs
# it directly: Package Manager > "+" > "Install package from tarball...". No registry involved.
#
# It stages into a CLEAN directory before packing, which is not fussiness:
#   npm falls back to .gitignore when a package has no .npmignore - and bw_audio.dll IS gitignored (it
#   is build output). Packing bindings/unity in place therefore yields a tarball with no native plugin:
#   it installs fine, then throws DllNotFoundException on the first engine call.
#   Staging first means npm only ever sees what we put there.
#
# Only the engine build is a prereq (CMake stages the engine DLL into Runtime/Plugins/x86_64). The
# tarball is written with `tar`, which ships with Windows 10+ - no Node/npm anywhere in the pipeline.
#
#   cmake --build build --config RelWithDebInfo      # produces the engine DLL
#   powershell -File tools/upm/pack.ps1 [-Version 0.3.0] [-OutDir dist]
#                                       [-AndroidFrom <dir>] [-LinuxFrom <dir>] [-MacFrom <dir>]
#
# The package carries the engine for FOUR platforms, one folder each, because Unity keys a native
# plugin by its folder as well as by the import settings in its .meta:
#
#   Runtime/Plugins/x86_64/bw_audio.dll                  the Windows desktop (and its editor)
#   Runtime/Plugins/Android/arm64-v8a/libbw_audio.so     a standalone headset
#   Runtime/Plugins/Linux/x86_64/libbw_audio.so          the Linux desktop (and its editor)
#   Runtime/Plugins/macOS/libbw_audio.dylib              macOS, universal (and its editor)
#
# The last three are cross-builds a Windows pack cannot produce. A native build of this repo on
# each of those platforms stages its own, and -AndroidFrom / -LinuxFrom / -MacFrom hand one over
# (which is how CI passes the android, linux and macos jobs' artifacts into the Windows pack).
# Missing, any of them FAILS the pack - a tarball whose plugin for a platform is absent installs,
# builds, and throws DllNotFoundException on that platform, which is the latest possible place to
# find out.
#
# The GIT TAG is the single source of truth for the release version. -Version (CI passes the tag)
# STAMPS the staged package.json, so the committed manifest is a placeholder (0.0.0-dev) that never
# needs bumping - you cut a release by pushing a `v*` tag, nothing else. Without -Version (a local
# dev pack) the placeholder rides through, which is the honest version for an unreleased checkout.
[CmdletBinding()]
param(
    [string] $Version,                 # optional: stamp this version into the tarball (e.g. from a v0.3.0 tag)
    [string] $OutDir,                  # default: <repo>/dist
    [string] $PluginsFrom,             # optional: build dir to take bw_audio.dll FROM
    [string] $AndroidFrom,             # optional: dir to take the Android arm64 libbw_audio.so FROM
    [string] $LinuxFrom,               # optional: dir to take the Linux x86_64 libbw_audio.so FROM
    [string] $MacFrom                  # optional: dir to take the macOS universal libbw_audio.dylib FROM
)
$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$repo = (Resolve-Path (Join-Path $here '../..')).Path
$pkg  = Join-Path $repo 'bindings/unity'
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }

if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
    throw "tar not found on PATH (it ships with Windows 10+ as bsdtar)."
}

# Derive a dev version from `git describe` for a non-tag pack - the same idea setuptools_scm/hatch-vcs
# use, adapted to SemVer (UPM's parser, not PEP 440): base tag + commit distance + short hash. So a
# local or non-tag CI tarball reads e.g. 0.2.0-dev.4.g1a2b3c instead of a flat placeholder. The hash and
# distance live in the PRERELEASE field (no `+` build metadata, which older UPM parsers dislike). Falls
# back to $Fallback (the committed placeholder) whenever git can't answer: no matching tag, shallow
# clone, or git not on PATH.
function Get-DevVersion {
    param([string] $Repo, [string] $Fallback)

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return $Fallback }
    # --long: uniform "<tag>-<n>-g<hash>" form even on an exact tag (n=0). --match v*: only release tags.
    try   { $desc = & git -C $Repo describe --tags --long --dirty --match 'v*' 2>$null }
    catch { return $Fallback }
    if ($LASTEXITCODE -ne 0 -or -not $desc) { return $Fallback }
    $desc = ($desc | Select-Object -First 1).Trim()

    # v0.2.0-4-g1a2b3c[-dirty] -> base (may itself carry a prerelease, e.g. 0.2.0-rc.1), distance, hash.
    if ($desc -notmatch '^v?(.+?)-(\d+)-g([0-9a-fA-F]+)(-dirty)?$') { return $Fallback }
    $base = $Matches[1]; $n = [int] $Matches[2]; $hash = $Matches[3]; $dirty = [bool] $Matches[4]

    if ($n -eq 0 -and -not $dirty) { return $base }              # sitting on a clean tag: the release version
    # Append into the prerelease: '-' if base has none yet, '.' to extend an existing one (rc.1 -> rc.1.dev...).
    $sep = if ($base -match '-') { '.' } else { '-' }
    $ver = "$base$sep" + "dev.$n.g$hash"
    if ($dirty) { $ver += '.dirty' }
    return $ver
}

# ---- version -------------------------------------------------------------------------------------
# The tag (via -Version) wins; otherwise a dev version is derived from git (see Get-DevVersion). The
# staged package.json is stamped to $pkgVersion below (after it is copied), so the tarball name, the
# manifest inside it, and this line all agree without the committed file ever being edited.
$manifest   = Get-Content (Join-Path $pkg 'package.json') -Raw | ConvertFrom-Json
if ($Version) { $pkgVersion = $Version -replace '^v', '' }   # accept a raw git tag (v0.3.0) or a bare version
else          { $pkgVersion = Get-DevVersion -Repo $repo -Fallback $manifest.version }
Write-Host "packing $($manifest.name) $pkgVersion"

# ---- the native plugins must be present ----------------------------------------------------------
# CMake POST_BUILD stages the DLLs here on EVERY config it builds, so whichever config ran LAST wins.
# Building Debug after RelWithDebInfo (as CI does) therefore leaves the DEBUG engine sitting in the
# package folder. -PluginsFrom pins the config explicitly, so a release cannot ship a Debug DLL.
$plugins = Join-Path $pkg 'Runtime/Plugins/x86_64'
$winLibs = @('bw_audio.dll')
if ($PluginsFrom) {
    $src = (Resolve-Path $PluginsFrom).Path
    Write-Host "taking the native plugins from $src"
    foreach ($dll in $winLibs) {
        $from = Join-Path $src $dll
        $to   = Join-Path $plugins $dll
        if (-not (Test-Path $from)) { throw "$dll not found in $src" }

        # Skip the copy when it is already the same binary. Not just an optimization: an OPEN UNITY
        # EDITOR holds these DLLs loaded (a local/embedded package loads them straight out of this
        # folder), which locks them against writing. Hashing still works, so the common case - pack
        # right after a build, Unity open - goes through untouched instead of dying on a lock.
        if ((Test-Path $to) -and (Get-FileHash $from).Hash -eq (Get-FileHash $to).Hash) { continue }
        try { Copy-Item $from $to -Force }
        catch [System.IO.IOException] {
            throw ("$dll is LOCKED - cannot stage it into the package.`n" +
                   "The Unity Editor loads the native plugin out of this folder and holds it open, so it " +
                   "cannot be overwritten while Unity is running. Close the editor and re-run. (A CMake " +
                   "rebuild fails its POST_BUILD copy for the same reason.)")
        }
    }
}
foreach ($dll in $winLibs) {
    if (-not (Test-Path (Join-Path $plugins $dll))) {
        throw "$dll is missing from Runtime/Plugins/x86_64. Build the engine first: cmake --build build --config RelWithDebInfo (CMake stages it there), or pass -PluginsFrom build/RelWithDebInfo. A tarball without it installs, then fails at runtime."
    }
}

# ---- and so must the Android one -------------------------------------------------------------
# One library per ABI, whatever the SDK state: phonon has been linked STATICALLY into the engine
# library since 2026-09-15, so there is no second file to stage on any platform.
$androidPlugins = Join-Path $pkg 'Runtime/Plugins/Android/arm64-v8a'
$androidLibs = @('libbw_audio.so')
if ($AndroidFrom) {
    $src = (Resolve-Path $AndroidFrom).Path
    Write-Host "taking the android arm64 plugin from $src"
    New-Item -ItemType Directory -Force -Path $androidPlugins | Out-Null
    foreach ($so in $androidLibs) {
        $from = Join-Path $src $so
        if (-not (Test-Path $from)) { throw "$so not found in $src" }
        Copy-Item $from (Join-Path $androidPlugins $so) -Force
    }
}
foreach ($so in $androidLibs) {
    if (-not (Test-Path (Join-Path $androidPlugins $so))) {
        throw "$so is missing from Runtime/Plugins/Android/arm64-v8a. Cross-build it (docs/build.md, 'Android': an arm64-v8a build stages it there itself) or pass -AndroidFrom <dir with $so>. A tarball without it installs, exports an APK, and throws DllNotFoundException on the headset."
    }
}

# ---- and the two other desktops ---------------------------------------------------------------
# Same rule, one folder per platform. Neither is buildable on a Windows machine at all - there is
# no cross-toolchain here the way there is for Android - so each arrives prebuilt or the pack
# fails. A native Linux or macOS build of this repo stages its own copy (CMakeLists.txt does it
# POST_BUILD), which is what makes a pack run on those machines need no switch.
$crossDesktops = @(
    @{ Name = 'linux'; Dir = 'Runtime/Plugins/Linux/x86_64'; File = 'libbw_audio.so'
       From = $LinuxFrom; Switch = '-LinuxFrom'; Input = 'linux-pack-input/unity' },
    @{ Name = 'macos'; Dir = 'Runtime/Plugins/macOS';        File = 'libbw_audio.dylib'
       From = $MacFrom;   Switch = '-MacFrom';   Input = 'macos-pack-input/unity' }
)
foreach ($p in $crossDesktops) {
    $dir = Join-Path $pkg $p.Dir
    if ($p.From) {
        $src = (Resolve-Path $p.From).Path
        Write-Host "taking the $($p.Name) plugin from $src"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $from = Join-Path $src $p.File
        if (-not (Test-Path $from)) { throw "$($p.File) not found in $src" }
        Copy-Item $from (Join-Path $dir $p.File) -Force
    }
    if (-not (Test-Path (Join-Path $dir $p.File))) {
        throw ("$($p.File) is missing from $($p.Dir). Build this repo on $($p.Name) (it stages the " +
               "library there itself) or pass $($p.Switch) <dir with $($p.File)> - CI hands over the " +
               "$($p.Name) job's artifact, $($p.Input). A tarball without it installs, then throws " +
               "DllNotFoundException the first time anyone runs it on $($p.Name).")
    }
}

# ---- stage ---------------------------------------------------------------------------------------
$stage = Join-Path $repo 'build/upm/package'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage -Force | Out-Null

Copy-Item (Join-Path $pkg 'package.json')      $stage
# Stamp the release version into the STAGED manifest (never the committed one). A targeted regex on the
# raw text keeps the file's formatting - and the description's embedded \n - byte-for-byte; a
# ConvertTo-Json round-trip would reflow and re-escape the whole thing. Only the one top-level "version".
$staged  = Join-Path $stage 'package.json'
$content = (Get-Content $staged -Raw) -replace '("version"\s*:\s*")[^"]*"', ('${1}' + $pkgVersion + '"')
[IO.File]::WriteAllText($staged, $content)
Copy-Item (Join-Path $pkg 'README.md')         $stage
Copy-Item (Join-Path $pkg 'CHANGELOG.md')      $stage
Copy-Item (Join-Path $pkg 'package.json.meta') $stage
Copy-Item (Join-Path $pkg 'README.md.meta')    $stage
Copy-Item (Join-Path $pkg 'CHANGELOG.md.meta') $stage
foreach ($dir in 'Runtime', 'Editor') {
    Copy-Item (Join-Path $pkg $dir) $stage -Recurse
    Copy-Item (Join-Path $pkg "$dir.meta") $stage
}
# git bookkeeping never ships (and a staged .gitignore would make npm re-apply its rules to the tree)
Get-ChildItem $stage -Recurse -Force -Include '.gitignore', '.gitkeep' | Remove-Item -Force

# The GPLv3 text and the third-party notices RIDE ALONG: this tarball is a binary distribution of a
# GPLv3 work, so the license travels with it. UPM shows both in the Package Manager UI.
Copy-Item (Join-Path $repo 'LICENSE')                (Join-Path $stage 'LICENSE.md')
Copy-Item (Join-Path $repo 'THIRD_PARTY-NOTICES.md') (Join-Path $stage 'Third Party Notices.md')

# Those two are DERIVED, so they carry no committed .meta. Mint them here with the same path-stable
# GUID scheme gen-meta.ps1 uses (MD5 of the package-relative path: same file, same id, every release).
function New-TextMeta([string] $StagedFile, [string] $Relative) {
    $md5  = [System.Security.Cryptography.MD5]::Create()
    $guid = (($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($Relative)) | ForEach-Object { $_.ToString('x2') }) -join '')
    $body = "fileFormatVersion: 2`nguid: $guid`nTextScriptImporter:`n  externalObjects: {}`n  userData:`n  assetBundleName:`n  assetBundleVariant:`n"
    [IO.File]::WriteAllText("$StagedFile.meta", $body)
}
New-TextMeta (Join-Path $stage 'LICENSE.md')             'LICENSE.md'
New-TextMeta (Join-Path $stage 'Third Party Notices.md') 'Third Party Notices.md'

# ---- doc pointers ---------------------------------------------------------------------------------
# The tarball ships Runtime/Editor sources and three markdown files, not the repo's docs/ tree - so a
# reference to docs/anything in a staged comment or README points at a file the installing user does
# not have. Rewrite those to permalinks at the packed commit, and refuse to pack if any survive.
# Shared with the Godot addon pack, which had exactly this bug found in the wild (a 0.4.0 zip citing
# api.md and docs/build.md, neither shipped). Runs BEFORE the .meta check, because it only edits file
# CONTENT and never adds or removes a staged file.
$commit = try { (& git -C $repo rev-parse HEAD).Trim() } catch { 'main' }
& (Join-Path $repo 'tools/dist/doc-pointers.ps1') -Stage $stage -Repo $repo -Ref $commit `
    -Extensions @('.md', '.txt', '.cs', '.json') `
    -Aliases @{ 'THIRD_PARTY-NOTICES.md' = 'Third Party Notices.md'; 'LICENSE' = 'LICENSE.md' }

# ---- every asset must carry a .meta ---------------------------------------------------------------
# A file arriving without one gets a fresh random GUID in EACH project, so a scene that references the
# script breaks across machines ("Missing (Mono Script)"). Catch that here, not in a user's project.
$missing = @()
$missing += Get-ChildItem $stage -Recurse -File |
    Where-Object { $_.Extension -ne '.meta' -and -not (Test-Path ($_.FullName + '.meta')) } |
    ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
$missing += Get-ChildItem $stage -Recurse -Directory |
    Where-Object { -not (Test-Path ($_.FullName + '.meta')) } |
    ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
if ($missing.Count) {
    throw ("no .meta for: " + ($missing -join ', ') + "`nRun: powershell -File tools/upm/gen-meta.ps1")
}

# ---- pack ----------------------------------------------------------------------------------------
# UPM (like npm) requires every entry to sit under a single "package/" root - which is exactly what
# the staging directory is named, so we tar it by name from its parent. That is also the layout
# `npm pack` emits, so the file would stay valid if this is ever served from a UPM registry (they all
# speak the npm protocol) - but nothing here depends on npm being installed.
New-Item -ItemType Directory $OutDir -Force | Out-Null
$tgz = Join-Path $OutDir ("{0}-{1}.tgz" -f $manifest.name, $pkgVersion)
if (Test-Path $tgz) { Remove-Item $tgz -Force }
& tar -czf $tgz -C (Split-Path -Parent $stage) 'package'
if ($LASTEXITCODE -ne 0) { throw "tar failed ($LASTEXITCODE)" }
if (-not (Test-Path $tgz)) { throw "tar did not produce $tgz" }

# Prove the plugins actually made it in. This is the whole point of staging (see the header).
$listing = & tar -tzf $tgz
foreach ($need in 'package/Runtime/Plugins/x86_64/bw_audio.dll',
                  'package/Runtime/Plugins/Android/arm64-v8a/libbw_audio.so',
                  'package/Runtime/Plugins/Linux/x86_64/libbw_audio.so',
                  'package/Runtime/Plugins/macOS/libbw_audio.dylib') {
    if ($listing -notcontains $need) {
        throw "$need is MISSING from the tarball. It would install and then fail at runtime."
    }
}
Write-Host ""
Write-Host "$($listing.Count) entries:"
$listing | ForEach-Object { Write-Host "  $_" }
Write-Host ""
Write-Host "OK -> $tgz"
Write-Host 'install: Unity, Package Manager, "+", "Install package from tarball..."'

# Expose the packed version to the workflow (for the artifact name). No-op off CI, where the env var is
# unset. AppendAllText is UTF-8 without a BOM - GITHUB_OUTPUT parsing chokes on the UTF-16 that 5.1's
# `>>` would write, and on a mid-file BOM; the value is a single ASCII line, so no here-doc is needed.
if ($env:GITHUB_OUTPUT) {
    [System.IO.File]::AppendAllText($env:GITHUB_OUTPUT, "version=$pkgVersion`n")
}
