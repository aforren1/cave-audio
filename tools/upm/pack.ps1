# pack.ps1 - build the installable UPM tarball (com.brainworks.bw_audio-<version>.tgz).
#
# ASCII ONLY, deliberately: Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a stray em-dash
# or arrow becomes mojibake and breaks the PARSER. CI runs pwsh, developers run 5.1. Keep it plain.
#
# This tarball IS the distribution. CI attaches it to a GitHub Release on a v* tag, and Unity installs
# it directly: Package Manager > "+" > "Install package from tarball...". No registry involved.
#
# It stages into a CLEAN directory before packing, which is not fussiness:
#   npm falls back to .gitignore when a package has no .npmignore - and bw_audio.dll / phonon.dll ARE
#   gitignored (they are build output). Packing bindings/unity in place therefore yields a tarball with
#   no native plugin: it installs fine, then throws DllNotFoundException on the first engine call.
#   Staging first means npm only ever sees what we put there.
#
# Only the engine build is a prereq (CMake stages the two DLLs into Runtime/Plugins/x86_64). The
# tarball is written with `tar`, which ships with Windows 10+ - no Node/npm anywhere in the pipeline.
#
#   cmake --build build --config RelWithDebInfo      # produces the DLLs
#   powershell -File tools/upm/pack.ps1 [-Version 0.3.0] [-OutDir dist]
#
# The GIT TAG is the single source of truth for the release version. -Version (CI passes the tag)
# STAMPS the staged package.json, so the committed manifest is a placeholder (0.0.0-dev) that never
# needs bumping - you cut a release by pushing a `v*` tag, nothing else. Without -Version (a local
# dev pack) the placeholder rides through, which is the honest version for an unreleased checkout.
[CmdletBinding()]
param(
    [string] $Version,                 # optional: stamp this version into the tarball (e.g. from a v0.3.0 tag)
    [string] $OutDir,                  # default: <repo>/dist
    [string] $PluginsFrom              # optional: build dir to take bw_audio.dll + phonon.dll FROM
)
$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$repo = (Resolve-Path (Join-Path $here '../..')).Path
$pkg  = Join-Path $repo 'bindings/unity'
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }

if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
    throw "tar not found on PATH (it ships with Windows 10+ as bsdtar)."
}

# ---- version -------------------------------------------------------------------------------------
# The tag (via -Version) wins; the committed manifest is only the fallback for a local dev pack. The
# staged package.json is stamped to $pkgVersion below (after it is copied), so the tarball name, the
# manifest inside it, and this line all agree without the committed file ever being edited.
$manifest   = Get-Content (Join-Path $pkg 'package.json') -Raw | ConvertFrom-Json
if ($Version) { $pkgVersion = $Version -replace '^v', '' }   # accept a raw git tag (v0.3.0) or a bare version
else          { $pkgVersion = $manifest.version }            # dev pack: the placeholder rides through
Write-Host "packing $($manifest.name) $pkgVersion"

# ---- the native plugins must be present ----------------------------------------------------------
# CMake POST_BUILD stages the DLLs here on EVERY config it builds, so whichever config ran LAST wins.
# Building Debug after RelWithDebInfo (as CI does) therefore leaves the DEBUG engine sitting in the
# package folder. -PluginsFrom pins the config explicitly, so a release cannot ship a Debug DLL.
$plugins = Join-Path $pkg 'Runtime/Plugins/x86_64'
if ($PluginsFrom) {
    $src = (Resolve-Path $PluginsFrom).Path
    Write-Host "taking the native plugins from $src"
    foreach ($dll in 'bw_audio.dll', 'phonon.dll') {
        $from = Join-Path $src $dll
        $to   = Join-Path $plugins $dll
        if (-not (Test-Path $from)) { throw "$dll not found in $src" }

        # Skip the copy when it is already the same binary. Not just an optimisation: an OPEN UNITY
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
foreach ($dll in 'bw_audio.dll', 'phonon.dll') {
    if (-not (Test-Path (Join-Path $plugins $dll))) {
        throw "$dll is missing from Runtime/Plugins/x86_64. Build the engine first: cmake --build build --config RelWithDebInfo (CMake stages both DLLs there), or pass -PluginsFrom build/RelWithDebInfo. A tarball without them installs, then fails at runtime."
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
foreach ($need in 'package/Runtime/Plugins/x86_64/bw_audio.dll', 'package/Runtime/Plugins/x86_64/phonon.dll') {
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
