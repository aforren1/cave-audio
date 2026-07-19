# release.ps1 - cut a release: roll the Unity CHANGELOG's [Unreleased] heading to a version, then tag.
#
# ASCII ONLY (see tools/upm/pack.ps1 for the full reasoning): Windows PowerShell 5.1 reads a BOM-less
# .ps1 as ANSI, so a stray non-ASCII char in THIS FILE breaks the parser. The CHANGELOG it edits does
# contain non-ASCII (em-dashes); that is handled by reading/writing it as UTF-8 below. Keep this script
# itself plain ASCII.
#
# The git tag is the single source of truth for the release version - pack.ps1 stamps it into the
# packaged manifest, so there is no version field to bump. The one manual step that remains is rolling
# the CHANGELOG's [Unreleased] section to the version being cut. This does that, commits it, and creates
# the matching annotated v-tag. It does NOT push by default: review the commit and tag, then
# `git push --follow-tags` (or pass -Push) to trigger the CI release.
#
#   powershell -File tools/release.ps1 0.3.0            # roll CHANGELOG, commit, tag v0.3.0 (no push)
#   powershell -File tools/release.ps1 v0.3.0 -Push     # ...and push, which triggers the CI release
#   powershell -File tools/release.ps1 0.3.0 -DryRun    # preview the CHANGELOG roll; change nothing
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
if (-not (Test-Path $changelog)) { throw "CHANGELOG not found at $changelog" }

# ---- version -------------------------------------------------------------------------------------
$ver = $Version -replace '^v', ''
if ($ver -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$') {
    throw "not a SemVer version: '$Version' (want e.g. 0.3.0 or v0.3.0)"
}
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

    if ($DryRun) {
        Write-Host "DRY RUN - would roll [Unreleased] -> [$ver] and tag $tag`n"
        $preview = ($rolled -split "\r?\n" | Select-Object -First 16) -join [Environment]::NewLine
        Write-Host $preview
        Write-Host "`n(no files changed, no commit, no tag)"
        return
    }

    [System.IO.File]::WriteAllText($changelog, $rolled)

    # ---- commit + tag ------------------------------------------------------------------------------
    git add -- 'bindings/unity/CHANGELOG.md'
    if ($LASTEXITCODE -ne 0) { throw "git add failed" }
    git commit -m "Release $tag"
    if ($LASTEXITCODE -ne 0) { throw "git commit failed" }
    git tag -a $tag -m "Release $tag"
    if ($LASTEXITCODE -ne 0) { throw "git tag failed" }
    Write-Host "rolled CHANGELOG, committed, and tagged $tag"

    if ($Push) {
        git push --follow-tags
        if ($LASTEXITCODE -ne 0) { throw "git push failed" }
        Write-Host "pushed - the CI release for $tag is now running"
    } else {
        Write-Host "not pushed. Review, then: git push --follow-tags   (this triggers the CI release)"
    }
}
finally { Pop-Location }
