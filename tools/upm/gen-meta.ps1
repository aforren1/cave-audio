# gen-meta.ps1 - create the Unity .meta files for the UPM package (bindings/unity).
#
# WHY these are committed, and not left to Unity:
#   A package installed from a registry/tarball is IMMUTABLE - Unity imports it read-only into
#   Library/PackageCache. Any file arriving without a .meta gets a FRESH RANDOM GUID in each project,
#   so a scene that references Emitter on machine A points at a GUID machine B never generated:
#   the component deserializes as "Missing (Mono Script)". Shipping the .meta pins the GUID for
#   everyone. The same applies to the native plugins' import settings (which platforms, Editor on/off):
#   in an immutable package the user CANNOT fix them in the Inspector, so they must ship correct.
#
# GUIDs are the MD5 of the package-relative path - deterministic, so a fresh clone regenerates the
# SAME ids. Existing .meta files are never overwritten (a GUID, once published, is forever: changing
# it breaks every scene referencing that script). Run after adding a file to the package; pack.ps1
# fails the build if anything is missing a .meta.
#
#   pwsh tools/upm/gen-meta.ps1 [-WhatIf]
[CmdletBinding(SupportsShouldProcess)]
param(
    [string] $PackageDir   # default: bindings/unity, resolved relative to this script
)
$ErrorActionPreference = 'Stop'
# $PSScriptRoot is not reliably bound inside param() defaults under Windows PowerShell 5.1 - resolve here.
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $PackageDir) { $PackageDir = Join-Path $here '../../bindings/unity' }
$PackageDir = (Resolve-Path $PackageDir).Path

# Stable GUID from the package-relative path. Unity wants 32 lowercase hex chars.
function Get-BwGuid([string] $Relative) {
    $md5   = [System.Security.Cryptography.MD5]::Create()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Relative.Replace('\', '/'))
    return (($md5.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '')
}

# The importer block Unity writes for each asset kind. A native plugin is the interesting one: it must
# name the platforms explicitly, because "Any Platform" would try to load a Windows x64 DLL on every
# target. The Windows DLL keeps the Editor enabled, so the plugin works in Play mode and not only in
# a build; the Android library does not, because the editor is not an Android player.
function Get-BwImporter([string] $Path, [bool] $IsFolder) {
    if ($IsFolder) { return "folderAsset: yes`nDefaultImporter:`n  externalObjects: {}`n  userData:`n  assetBundleName:`n  assetBundleVariant:" }
    switch ([IO.Path]::GetExtension($Path).ToLowerInvariant()) {
        '.cs' {
            return "MonoImporter:`n  externalObjects: {}`n  serializedVersion: 2`n  defaultReferences: []`n  executionOrder: 0`n  icon: {instanceID: 0}`n  userData:`n  assetBundleName:`n  assetBundleVariant:"
        }
        '.asmdef' {
            return "AssemblyDefinitionImporter:`n  externalObjects: {}`n  userData:`n  assetBundleName:`n  assetBundleVariant:"
        }
        '.dll' {
            return @'
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: x86_64
        DefaultValueInitialized: true
        OS: Windows
  - first:
      Standalone: Win64
    second:
      enabled: 1
      settings:
        CPU: x86_64
  userData:
  assetBundleName:
  assetBundleVariant:
'@.TrimEnd()
        }
        # A native ELF library. Two folders take one, and they mean different things:
        #
        #   Plugins/Android/<abi>  a headset build. Unity picks the ABI folder up as a hint, but the
        #                          CPU setting is what the importer actually serializes, so it has to
        #                          say ARM64 too. Enabled for Android and for NOTHING else: not Any
        #                          (which would try to load an ARM ELF on every target), and not the
        #                          Editor, which runs on the desktop.
        #   Plugins/Linux/x86_64   a Linux desktop. Enabled for the Linux EDITOR as well as the
        #                          player, exactly as the Windows DLL above is: a Linux collaborator
        #                          has to be able to press Play. The Editor OS and CPU spellings are
        #                          Unity's own (EditorPluginOSArchitecture: AnyOS/OSX/Windows/Linux).
        #
        # An installed package is immutable, so neither can be fixed in the Inspector afterwards.
        '.so' {
            if ($Path -match 'Plugins/Linux/') {
                return @'
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: x86_64
        DefaultValueInitialized: true
        OS: Linux
  - first:
      Standalone: Linux64
    second:
      enabled: 1
      settings:
        CPU: x86_64
  userData:
  assetBundleName:
  assetBundleVariant:
'@.TrimEnd()
            }
            if ($Path -notmatch 'Plugins/Android/') {
                throw "no importer rule for a .so outside Plugins/Android or Plugins/Linux: $Path"
            }
            return @'
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Android: Android
    second:
      enabled: 1
      settings:
        CPU: ARM64
  - first:
      Editor: Editor
    second:
      enabled: 0
      settings:
        CPU: AnyCPU
        DefaultValueInitialized: true
        OS: AnyOS
  userData:
  assetBundleName:
  assetBundleVariant:
'@.TrimEnd()
        }
        # A macOS native library. Unity accepts a plain .dylib as well as a .bundle
        # (DesktopPluginImporterExtension.IsUsableOnOSX tests for both), and one UNIVERSAL file
        # covers Intel and Apple silicon - which is why both CPU fields say AnyCPU rather than
        # naming an architecture. Enabled for the macOS editor as well as the player, for the same
        # reason the Linux .so is: a Mac collaborator has to be able to press Play.
        '.dylib' {
            if ($Path -notmatch 'Plugins/macOS/') {
                throw "no importer rule for a .dylib outside Plugins/macOS: $Path"
            }
            return @'
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: AnyCPU
        DefaultValueInitialized: true
        OS: OSX
  - first:
      Standalone: OSXUniversal
    second:
      enabled: 1
      settings:
        CPU: AnyCPU
  userData:
  assetBundleName:
  assetBundleVariant:
'@.TrimEnd()
        }
        { $_ -in '.md', '.json', '.txt' } {
            return "TextScriptImporter:`n  externalObjects: {}`n  userData:`n  assetBundleName:`n  assetBundleVariant:"
        }
        default {
            return "DefaultImporter:`n  externalObjects: {}`n  userData:`n  assetBundleName:`n  assetBundleVariant:"
        }
    }
}

# Every asset that ships. The native libraries are BUILD OUTPUT (gitignored) - but their .meta is
# not, so it is generated here whether or not the binary is currently staged on this machine. Four
# platforms, four folders, because Unity keys a native plugin by its folder as well as by the import
# settings in its .meta: x86_64 (the Windows desktop), Android/arm64-v8a (the ABI every current
# standalone headset runs), Linux/x86_64 and macOS (one universal file for both Apple
# architectures). One library per platform: phonon has been linked STATICALLY into the engine
# library since 2026-09-15, so no Steam Audio file ships beside it.
$assets = @(
    'package.json', 'README.md', 'CHANGELOG.md',
    'Runtime', 'Runtime/Plugins', 'Runtime/Plugins/x86_64', 'Editor',
    'Runtime/Plugins/x86_64/bw_audio.dll',
    'Runtime/Plugins/Android', 'Runtime/Plugins/Android/arm64-v8a',
    'Runtime/Plugins/Android/arm64-v8a/libbw_audio.so',
    'Runtime/Plugins/Linux', 'Runtime/Plugins/Linux/x86_64',
    'Runtime/Plugins/Linux/x86_64/libbw_audio.so',
    'Runtime/Plugins/macOS', 'Runtime/Plugins/macOS/libbw_audio.dylib'
)
# RECURSE. An .asmdef governs its own folder and everything under it, so splitting the package into
# assemblies (BwAudio, BwAudio.RigDay) necessarily means subfolders. A flat scan silently skips them,
# and the assets ship with no .meta at all - which is the exact failure this script exists to prevent.
# Folders need a .meta as much as files do: without one Unity regenerates the folder GUID per project
# and every reference into it breaks. Anything with a '~' segment is Unity-invisible (Samples~), so it
# neither ships as an asset nor wants a .meta.
foreach ($dir in 'Runtime', 'Editor') {
    $root = Join-Path $PackageDir $dir
    $rel  = { $args[0].Substring($PackageDir.Length + 1).Replace([char]92, '/') }   # 92 = backslash
    Get-ChildItem $root -Recurse -Directory |
        ForEach-Object { & $rel $_.FullName } |
        Where-Object { $_ -notmatch '~' } |
        ForEach-Object { $assets += $_ }
    Get-ChildItem $root -Recurse -File |
        Where-Object { $_.Extension -in '.cs', '.asmdef' } |
        ForEach-Object { & $rel $_.FullName } |
        Where-Object { $_ -notmatch '~' } |
        ForEach-Object { $assets += $_ }
}

$made = 0
foreach ($rel in ($assets | Sort-Object -Unique)) {
    $meta = Join-Path $PackageDir "$rel.meta"
    if (Test-Path $meta) { continue }        # NEVER regenerate: a published GUID is permanent
    $isFolder = -not [IO.Path]::GetExtension($rel)
    $body = "fileFormatVersion: 2`nguid: $(Get-BwGuid $rel)`n$(Get-BwImporter $rel $isFolder)`n"
    if ($PSCmdlet.ShouldProcess($meta, 'create')) {
        [IO.File]::WriteAllText($meta, $body.Replace("`r`n", "`n"))   # LF: Unity's own format
    }
    Write-Host "  + $rel.meta"
    $made++
}
Write-Host "$made .meta file(s) created; $($assets.Count) asset(s) tracked."
