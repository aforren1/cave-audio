# publish-branch.ps1 - push a staged directory to an orphan distribution branch.
#
# ASCII ONLY, deliberately: Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a stray em-dash
# becomes mojibake and breaks the PARSER. CI runs pwsh, developers run 5.1. Keep it plain.
#
# The gh-pages idea applied to the bindings: `main` stays source-only, and each binding also gets a
# branch whose ROOT is the installable thing. That is what makes the standard installers work, because
# both of them install from a git ref and both expect their manifest at the top level:
#
#   godot   ->  addons/bw_audio/...        the Asset Library downloads a repo ARCHIVE at a commit, so
#                                          the binaries have to be IN the tree - a release asset will
#                                          not do.
#   unity   ->  package.json at the root   UPM installs from a git URL only when package.json is at
#                                          the ref's root:
#                                            https://github.com/aforren1/cave-audio.git#unity
#
# Each publish is a NORMAL COMMIT on the branch, not a force-push. The Asset Library pins a specific
# commit hash, and rewriting history would eventually leave a published entry pointing at a commit
# that no longer resolves. The cost is that the branch accumulates roughly the addon's size per
# release; that is the price of in-repo distribution and there is no way around it.
#
# Runs against a TEMPORARY CLONE, never the working tree, so a publish cannot disturb a build in
# progress or leave the repo on another branch.
#
#   powershell -File tools/dist/publish-branch.ps1 -Branch godot -Source build/godot/addon -Version 0.4.0
#   powershell -File tools/dist/publish-branch.ps1 -Branch unity -Source build/upm/package -Version 0.4.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Branch,   # orphan branch to publish to (godot | unity)
    [Parameter(Mandatory)] [string] $Source,   # directory whose CONTENTS become the branch root
    [string] $Version = 'dev',
    [string] $RemoteUrl,                       # default: this repo's origin
    [switch] $DryRun                           # build the commit, skip the push
)
$ErrorActionPreference = 'Stop'
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$repo = (Resolve-Path (Join-Path $here '../..')).Path
$src  = (Resolve-Path $Source).Path

if (-not (Test-Path $src)) { throw "source directory not found: $Source" }
if (-not (Get-ChildItem $src)) { throw "source directory is empty: $Source" }
if (-not $RemoteUrl) { $RemoteUrl = (& git -C $repo remote get-url origin).Trim() }

$sha = (& git -C $repo rev-parse HEAD).Trim()
$work = Join-Path ([IO.Path]::GetTempPath()) ("bwa-publish-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null

try {
    Write-Host "==> preparing '$Branch' from $src"
    & git -C $work init -q
    & git -C $work remote add origin $RemoteUrl
    # Publish bytes verbatim. Line-ending translation would rewrite the staged text files and, more
    # to the point, git announces it on STDERR - which Windows PowerShell 5.1 promotes to an
    # ErrorRecord, so a cosmetic warning can terminate the script under 'Stop'.
    & git -C $work config core.autocrlf false
    & git -C $work config core.safecrlf false

    # Does the branch exist yet? Asked with ls-remote rather than by letting a fetch fail, because
    # a failing git writes to stderr and Windows PowerShell 5.1 turns native stderr into an
    # ErrorRecord - which under $ErrorActionPreference = 'Stop' is TERMINATING. The first publish
    # of a branch is a normal case, not an error, so it must not be expressed as one.
    & git -C $work ls-remote --exit-code --heads origin $Branch | Out-Null
    $exists = ($LASTEXITCODE -eq 0)

    if ($exists) {
        & git -C $work fetch -q --depth 1 origin $Branch
        & git -C $work checkout -q -B $Branch FETCH_HEAD
        # Clear the tree so a file dropped from the addon actually disappears downstream, rather
        # than lingering because nothing overwrote it.
        Get-ChildItem $work -Force | Where-Object { $_.Name -ne '.git' } | Remove-Item -Recurse -Force
    } else {
        Write-Host "    (branch '$Branch' does not exist yet - starting it)"
        & git -C $work checkout -q --orphan $Branch
    }

    Copy-Item (Join-Path $src '*') $work -Recurse -Force

    # An identity is required to commit and CI runners have none configured. This must be the
    # ACTIONS BOT identity, not an invented one: GitHub resolves USERNAME@users.noreply.github.com
    # to that account, so a made-up local part attributes the commits to whoever owns that
    # username - 'ci' is a real user, who briefly starred in this repo's history. The bot's
    # address is id-prefixed and reserved, so it can never collide with a person.
    & git -C $work config user.name  'github-actions[bot]'
    & git -C $work config user.email '41898282+github-actions[bot]@users.noreply.github.com'
    & git -C $work add -A

    # `git diff --cached --quiet` exits 0 when nothing is staged. Re-running a publish for an
    # unchanged build should be a no-op, not an empty commit.
    & git -C $work diff --cached --quiet
    if ($LASTEXITCODE -eq 0) {
        Write-Host "    no changes; nothing to publish"
        return
    }

    $msg = "$Branch $Version (built from $($sha.Substring(0,12)))"
    & git -C $work commit -q -m $msg
    if ($LASTEXITCODE -ne 0) { throw "commit failed" }
    $newSha = (& git -C $work rev-parse HEAD).Trim()

    if ($DryRun) {
        Write-Host "    [dry run] would push $newSha to $Branch"
        Write-Host "    tree:"
        & git -C $work ls-tree -r --name-only HEAD | ForEach-Object { Write-Host "      $_" }
        return
    }

    & git -C $work push -q origin "HEAD:refs/heads/$Branch"
    if ($LASTEXITCODE -ne 0) { throw "push to '$Branch' failed" }
    Write-Host "    published $newSha -> $Branch"
    # The Asset Library submission form asks for this exact hash.
    if ($env:GITHUB_OUTPUT) { "commit=$newSha" | Out-File -FilePath $env:GITHUB_OUTPUT -Append }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
