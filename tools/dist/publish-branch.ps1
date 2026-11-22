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
# -Tag also names the published commit, so a consumer can PIN a release. The v* release tags point
# at main, where neither manifest is at the root, so they cannot serve as an install ref; a tag on
# the distribution branch's own commit can:
#   https://github.com/aforren1/cave-audio.git#unity/v<version>
# A branch `unity` and a tag `unity/v1.2.3` do not collide: refs/heads/ and refs/tags/ are separate
# namespaces. The tag is checked BEFORE anything is pushed and travels in the same atomic push as
# the branch, so a rerun is a no-op and a rerun of an OLD release fails with the branch untouched,
# rather than committing old content on top of a newer release and only then tripping on the tag.
# "Rerun" means the SAME staged tree (re-run failed jobs, same pack inputs). A full rebuild of one
# release makes different binaries, so a different commit, and trips the guard too. That is
# intended: the first publish is the release, and a tag that moved would break every pin on it.
#
# Runs against a TEMPORARY CLONE, never the working tree, so a publish cannot disturb a build in
# progress or leave the repo on another branch.
#
#   powershell -File tools/dist/publish-branch.ps1 -Branch godot -Source build/godot/addon -Version 0.4.0
#   powershell -File tools/dist/publish-branch.ps1 -Branch unity -Source build/upm/package -Version 0.4.0 -Tag unity/v0.4.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Branch,   # orphan branch to publish to (godot | unity)
    [Parameter(Mandatory)] [string] $Source,   # directory whose CONTENTS become the branch root
    [string] $Version = 'dev',
    [string] $Tag,                             # optional tag for the published commit (unity/v0.4.0)
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
    $changed = ($LASTEXITCODE -ne 0)
    if (-not $changed -and -not $Tag) {
        Write-Host "    no changes; nothing to publish"
        return
    }

    if ($changed) {
        $msg = "$Branch $Version (built from $($sha.Substring(0,12)))"
        & git -C $work commit -q -m $msg
        if ($LASTEXITCODE -ne 0) { throw "commit failed" }
    } else {
        Write-Host "    no changes; '$Branch' already holds this build"
    }
    # With nothing to commit this is the branch head just fetched: the commit an earlier run of
    # the same release published. That is the one a missing tag belongs on.
    $newSha = (& git -C $work rev-parse HEAD).Trim()

    $pushTag = $false
    if ($Tag) {
        & git -C $work check-ref-format "refs/tags/$Tag"
        if ($LASTEXITCODE -ne 0) { throw "not a valid tag name: '$Tag'" }
        # Asked BEFORE the push, so a conflict leaves the branch as it was. A rejected tag push
        # would catch the same thing, but only after the branch had already moved. No --exit-code:
        # an absent tag is the normal first-publish case and must stay off stderr (see above).
        # The peeled line (^{}) is preferred so an annotated tag made by hand compares by commit.
        $have = $null
        foreach ($line in @(& git -C $work ls-remote --tags origin "refs/tags/$Tag" "refs/tags/$Tag^{}")) {
            $f = "$line" -split '\s+'
            if ($f.Count -lt 2) { continue }
            if ($f[1] -eq "refs/tags/$Tag^{}") { $have = $f[0] }
            elseif ($f[1] -eq "refs/tags/$Tag" -and -not $have) { $have = $f[0] }
        }
        if ($LASTEXITCODE -ne 0) { throw "could not list the remote's tags" }
        if (-not $have) {
            $pushTag = $true
        } elseif ($have -eq $newSha) {
            Write-Host "    tag '$Tag' already names $newSha"
        } else {
            throw ("tag '$Tag' already exists on $have, and this publish is $newSha. " +
                   "Nothing was pushed. A published tag is never moved: release a new version.")
        }
    }

    if ($DryRun) {
        Write-Host "    [dry run] would push $newSha to $Branch"
        if ($pushTag) { Write-Host "    [dry run] would tag it '$Tag'" }
        Write-Host "    tree:"
        & git -C $work ls-tree -r --name-only HEAD | ForEach-Object { Write-Host "      $_" }
        return
    }

    if (-not $changed -and -not $pushTag) {
        Write-Host "    nothing to publish"
        return
    }

    # One atomic push: the branch and its tag land together or not at all, so a failure cannot
    # leave a release published with no tag to pin it by. Never forced: the remote itself rejects
    # a tag that appeared between the check above and here.
    $refs = @()
    if ($changed) { $refs += "HEAD:refs/heads/$Branch" }
    if ($pushTag) {
        & git -C $work tag $Tag $newSha
        if ($LASTEXITCODE -ne 0) { throw "could not create tag '$Tag'" }
        $refs += "refs/tags/${Tag}:refs/tags/$Tag"
    }
    & git -C $work push -q --atomic origin @refs
    if ($LASTEXITCODE -ne 0) { throw "push to '$Branch' failed" }
    if ($changed) { Write-Host "    published $newSha -> $Branch" }
    if ($pushTag) { Write-Host "    tagged $newSha as $Tag" }
    # The Asset Library submission form asks for this exact hash.
    if ($env:GITHUB_OUTPUT) { "commit=$newSha" | Out-File -FilePath $env:GITHUB_OUTPUT -Append }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
