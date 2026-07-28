# doc-pointers.ps1 - rewrite repo-doc references in a STAGED distribution, then verify none dangle.
#
# ASCII ONLY, like the pack scripts that call it: Windows PowerShell 5.1 reads a BOM-less .ps1 as
# ANSI, so a stray em-dash becomes mojibake and breaks the PARSER.
#
# The problem this solves: every artifact this repo ships (the Godot addon zip, the Unity tarball)
# carries CODE, not the repo's docs/ tree. Any reference to a repo doc that survives into the stage
# therefore points at a file the installing user does not have. A dogfooding report found exactly
# that in bw_audio-godot-0.4.0.zip: playground.gd and scenes.gd cited api.md, THIRD_PARTY-NOTICES.md
# cited docs/build.md, and none of the three was in the zip. Their link checker caught it on install,
# which is late. This runs at pack time.
#
# Two passes, deliberately separate:
#
#   REWRITE  every reference to a docs/*.md this repo actually has becomes a permalink at $Ref
#            (a commit, so the doc a reader lands on keeps matching the code they installed).
#   VERIFY   ANY relative .md reference the stage cannot satisfy fails the pack - including ones no
#            rewrite rule knows about, so something new citing a repo path breaks here instead of
#            shipping a dangling pointer.
#
# Only names docs/ actually has get linkified: a typo must stay broken and visible, not become a
# confident URL to a 404.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]   $Stage,        # the staged tree to fix up, in place
    [Parameter(Mandatory)] [string]   $Repo,         # repo root (source of docs/ and its file list)
    [Parameter(Mandatory)] [string]   $Ref,          # commit-ish the links pin to
    [string[]] $Extensions = @('.md', '.txt', '.gd', '.tscn', '.gdextension', '.cfg', '.cs', '.json'),
    [string]   $UrlBase = 'https://github.com/aforren1/cave-audio/blob',
    # Repo files a pack SHIPS UNDER A DIFFERENT NAME, as @{ 'REPO-NAME.md' = 'Shipped Name.md' }.
    # The Unity tarball renames THIRD_PARTY-NOTICES.md to "Third Party Notices.md" for the Package
    # Manager UI, so prose citing the repo spelling is satisfied, not dangling — but only the pack
    # knows that. Without this the check fails the release over a file that is right there.
    [hashtable] $Aliases = @{}
)
$ErrorActionPreference = 'Stop'

# A short hash keeps the substituted URLs readable inside prose and code comments; GitHub resolves
# an abbreviated commit the same as the full one.
$short = if ($Ref -match '^[0-9a-f]{40}$') { $Ref.Substring(0, 12) } else { $Ref }
$docBase = "$UrlBase/$short"

$docNames = @(Get-ChildItem (Join-Path $Repo 'docs') -Filter '*.md' | ForEach-Object { $_.Name })
if ($docNames.Count -eq 0) { throw "no docs/*.md found under $Repo - is the repo intact?" }

# A markdown link's TEXT must survive as text: rewriting [docs/api.md](../../docs/api.md) on both
# halves yields a URL displayed as a URL. So the exclusions cover an opening bracket, and the
# bracket-plus-backtick that a code-formatted link text ([`docs/api.md`](...)) starts with. The link
# TARGET has its own rule below, which keeps the text and replaces only what is inside the parens.
$skip = '(?<![\w:/.\-\[]|\[`)'

$files = Get-ChildItem $Stage -Recurse -File | Where-Object { $Extensions -contains $_.Extension.ToLower() }
$rewrote = 0
foreach ($f in $files) {
    $t = [IO.File]::ReadAllText($f.FullName)
    $orig = $t
    foreach ($name in $docNames) {
        $esc = [regex]::Escape($name)
        $url = "$docBase/docs/$name"
        # 1. a markdown link: keep whatever the author wrote as the text (code-formatted or not,
        #    with or without a #anchor), replace only the target. Runs first so the later rules
        #    see a target that is already a URL.
        $t = $t -replace "\[([^\]]*)\]\((?:\.\./)*docs/$esc(#[^)]*)?\)", ('[$1](' + $url + '$2)')
        # 2. a bare docs/<name> in prose or a code comment, however many ../ deep
        $t = $t -replace "$skip(?:\.\./)*docs/$esc\b", $url
        # 3. the name alone, without its directory ("the api.md recipe")
        $t = $t -replace "$skip$esc\b", $url
    }
    if ($t -ne $orig) {
        [IO.File]::WriteAllText($f.FullName, $t)
        $rewrote++
    }
}
Write-Host "doc pointers: rewrote $rewrote staged file(s) -> $docBase"

$dangling = [System.Collections.Generic.List[string]]::new()
foreach ($f in Get-ChildItem $Stage -Recurse -File | Where-Object { $Extensions -contains $_.Extension.ToLower() }) {
    $t = [IO.File]::ReadAllText($f.FullName)
    foreach ($m in [regex]::Matches($t, "$skip((?:[\w.-]+/)*[\w.-]+\.md)\b")) {
        $ref = $m.Groups[1].Value
        $leaf = Split-Path $ref -Leaf
        if ($Aliases.ContainsKey($leaf)) { $leaf = $Aliases[$leaf] }   # shipped under a different name
        # Resolvable from the citing file, from the stage root, or (for a nested addon layout) from
        # wherever it lands - a shipped sibling doc is a valid pointer, only a missing one is not.
        $ok = (Test-Path (Join-Path $f.DirectoryName $ref)) -or (Test-Path (Join-Path $Stage $ref)) -or
              ((Get-ChildItem $Stage -Recurse -File | Where-Object { $_.Name -eq $leaf } | Measure-Object).Count -gt 0)
        if (-not $ok) {
            $rel = $f.FullName.Substring($Stage.Length).TrimStart('\', '/')
            $dangling.Add("$rel -> $ref")
        }
    }
}
if ($dangling.Count -gt 0) {
    throw ("the staged distribution cites $($dangling.Count) doc(s) it does not ship:`n  " +
        (($dangling | Select-Object -Unique) -join "`n  ") +
        "`nLink them at $docBase/... or ship the file.")
}
Write-Host "doc pointers: verified - nothing in the stage cites a doc it does not ship"
