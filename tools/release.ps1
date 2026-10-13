# release.ps1 - cut a release: roll the Unity CHANGELOG's [Unreleased] heading to a version, then tag.
#
# ASCII ONLY (see tools/upm/pack.ps1 for the full reasoning): Windows PowerShell 5.1 reads a BOM-less
# .ps1 as ANSI, so a stray non-ASCII char in THIS FILE breaks the parser. The CHANGELOG it edits does
# contain non-ASCII (em-dashes); that is handled by reading/writing it as UTF-8 below. Keep this script
# itself plain ASCII.
#
# The git tag is the single source of truth for the release version - pack.ps1 stamps it into the
# packaged manifest, so there is no version field to bump there. ONE number rides in the tree and
# has to agree with the tag: BWA_VERSION_* in include/bw_audio.h, which is what bwa_get_version()
# returns and what the Python wheels, the MEX gateways and the CMake package report. A wheel that
# says 0.15.0 out of a release called v0.7.0 is what this used to produce. The Unity binding carries
# a SECOND copy, Bwa.BoundVersion, because C# cannot read the header; Engine.Awake refuses a DLL
# whose major.minor differs from it, and a pin left at 0.13.0 shipped three packages that could never
# start. So this script rewrites the three defines AND BoundVersion to the version being cut, rolls
# the CHANGELOG's [Unreleased] section, commits all three files, and creates the matching annotated
# v-tag. CI refuses a tag whose header disagrees, and any push whose BoundVersion does. It does
# NOT push by default: review the commit and tag, then `git push --follow-tags` (or pass -Push) to
# trigger the CI release.
#
#   powershell -File tools/release.ps1 0.3.0            # set header, roll CHANGELOG, commit, tag v0.3.0 (no push)
#   powershell -File tools/release.ps1 v0.3.0 -Push     # ...and push, which triggers the CI release
#   powershell -File tools/release.ps1 0.3.0 -DryRun    # preview both edits; change nothing
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Version,   # 0.3.0 or v0.3.0
    [switch] $Push,                              # also push the branch + tag (fires the CI release)
    [switch] $DryRun                             # show the roll, change nothing
)
$ErrorActionPreference = 'Stop'

$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$repo = (Resolve-Path (Join-Path $here '..')).Path
$changelog = Join-Path $repo 'bindings/unity/CHANGELOG.md'
$header = Join-Path $repo 'include/bw_audio.h'
$bwacs = Join-Path $repo 'bindings/unity/Runtime/Bwa.cs'
if (-not (Test-Path $changelog)) { throw "CHANGELOG not found at $changelog" }
if (-not (Test-Path $header)) { throw "header not found at $header" }
if (-not (Test-Path $bwacs)) { throw "Unity binding not found at $bwacs" }

# ---- version -------------------------------------------------------------------------------------
# Three plain integers, no prerelease suffix: the header's defines are integers, and every consumer
# of the tag (pack.ps1, the wheel filename, the CMake package version file) reads x.y.z.
$ver = $Version -replace '^v', ''
if ($ver -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw "not a plain x.y.z version: '$Version' (want e.g. 0.3.0 or v0.3.0)"
}
$major = [int]$Matches[1]; $minor = [int]$Matches[2]; $patch = [int]$Matches[3]
$tag = "v$ver"

Push-Location $repo
try {
    # ---- git preconditions -----------------------------------------------------------------------
    git rev-parse --is-inside-work-tree 1>$null 2>$null
    if ($LASTEXITCODE -ne 0) { throw "not a git repository: $repo" }

    if (git tag --list $tag) { throw "tag $tag already exists" }

    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -ne 'main') { Write-Warning "on branch '$branch', not 'main'" }

    # A clean tree keeps the release commit to just the CHANGELOG roll. Skipped for a dry run (preview).
    if (-not $DryRun) {
        $dirty = git status --porcelain
        if ($dirty) { throw "working tree is dirty; commit or stash first so the release commit is only the CHANGELOG roll:`n$dirty" }
    }

    # ---- roll the CHANGELOG ------------------------------------------------------------------------
    # Read/write as UTF-8 (the file has non-ASCII): .NET ReadAllText/WriteAllText default to UTF-8 and
    # WriteAllText emits no BOM, so the round-trip is byte-stable apart from the edit.
    $raw = [System.IO.File]::ReadAllText($changelog)
    $nl  = if ($raw -match "`r`n") { "`r`n" } else { "`n" }

    # The [Unreleased] body is everything up to the next version heading; refuse to cut an empty one.
    $body = [regex]::Match($raw, '(?ms)^## \[Unreleased\][ \t]*\r?\n(.*?)(?=^## \[)')
    if (-not $body.Success) { throw "no [Unreleased] section (followed by a prior version) found in the CHANGELOG" }
    if ($body.Groups[1].Value -notmatch '\S') { throw "[Unreleased] is empty - nothing to release" }

    # Insert a fresh empty [Unreleased] above, renaming the old heading to this version. One occurrence.
    $rx     = [regex]::new('^(## \[Unreleased\][ \t]*\r?\n)', [System.Text.RegularExpressions.RegexOptions]::Multiline)
    $rolled = $rx.Replace($raw, ('${1}' + $nl + "## [$ver]" + $nl), 1)

    # ---- set the header version ----------------------------------------------------------------
    # The header is plain ASCII and the three defines sit on their own lines, so a per-line regex
    # is exact. Read/write as UTF-8 for the same byte-stable round trip as the CHANGELOG.
    $hraw = [System.IO.File]::ReadAllText($header)
    $hnew = $hraw
    foreach ($pair in @(@('MAJOR', $major), @('MINOR', $minor), @('PATCH', $patch))) {
        $name = $pair[0]; $val = $pair[1]
        $hrx = [regex]::new("^(#define BWA_VERSION_$name )\d+[ \t]*$", [System.Text.RegularExpressions.RegexOptions]::Multiline)
        if ($hrx.Matches($hnew).Count -ne 1) { throw "expected exactly one '#define BWA_VERSION_$name' line in $header" }
        $hnew = $hrx.Replace($hnew, ('${1}' + $val), 1)
    }
    $old = [regex]::Match($hraw, '(?m)^#define BWA_VERSION_MAJOR (\d+)').Groups[1].Value + '.' +
           [regex]::Match($hraw, '(?m)^#define BWA_VERSION_MINOR (\d+)').Groups[1].Value + '.' +
           [regex]::Match($hraw, '(?m)^#define BWA_VERSION_PATCH (\d+)').Groups[1].Value
    $headerChanges = ($hnew -ne $hraw)

    # ---- set the Unity binding's pin -------------------------------------------------------------
    # One line, rewritten whole so the trailing "// x.y.z" comment cannot drift from the value. Bwa.cs
    # carries non-ASCII, so the same UTF-8 round trip as above.
    $craw = [System.IO.File]::ReadAllText($bwacs)
    $crx  = [regex]::new('^([ \t]*public const uint BoundVersion = )[^\r\n]*', [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($crx.Matches($craw).Count -ne 1) { throw "expected exactly one 'public const uint BoundVersion' line in $bwacs" }
    $cnew = $crx.Replace($craw, ('${1}' + "(${major}u << 16) | (${minor}u << 8) | ${patch}u;   // $ver"), 1)
    $pinChanges = ($cnew -ne $craw)

    if ($DryRun) {
        Write-Host "DRY RUN - would roll [Unreleased] -> [$ver], set BWA_VERSION $old -> $ver, set Bwa.BoundVersion to $ver, and tag $tag`n"
        $preview = ($rolled -split "\r?\n" | Select-Object -First 16) -join [Environment]::NewLine
        Write-Host $preview
        Write-Host "`n(no files changed, no commit, no tag)"
        return
    }

    [System.IO.File]::WriteAllText($changelog, $rolled)
    if ($headerChanges) { [System.IO.File]::WriteAllText($header, $hnew) }
    if ($pinChanges)    { [System.IO.File]::WriteAllText($bwacs, $cnew) }

    # ---- commit + tag ------------------------------------------------------------------------------
    git add -- 'bindings/unity/CHANGELOG.md' 'include/bw_audio.h' 'bindings/unity/Runtime/Bwa.cs'
    if ($LASTEXITCODE -ne 0) { throw "git add failed" }
    git commit -m "Release $tag"
    if ($LASTEXITCODE -ne 0) { throw "git commit failed" }
    git tag -a $tag -m "Release $tag"
    if ($LASTEXITCODE -ne 0) { throw "git tag failed" }
    if ($headerChanges) { Write-Host "set BWA_VERSION $old -> $ver, rolled CHANGELOG, committed, and tagged $tag" }
    else                { Write-Host "BWA_VERSION already $ver; rolled CHANGELOG, committed, and tagged $tag" }

    if ($Push) {
        git push --follow-tags
        if ($LASTEXITCODE -ne 0) { throw "git push failed" }
        Write-Host "pushed - the CI release for $tag is now running"
    } else {
        Write-Host "not pushed. Review, then: git push --follow-tags   (this triggers the CI release)"
    }
}
finally { Pop-Location }
