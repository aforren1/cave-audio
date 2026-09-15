<#
.SYNOPSIS
    Build (or take) an Android build of bw_audio, push it to a booted device or emulator, and run
    the ctest suite there.

.DESCRIPTION
    ctest cannot drive an Android target: the test executables are ELF binaries for the device and
    the host has nothing to run them with. This script is the bridge. It reads the test list out of
    the build directory's CTestTestfile.cmake, pushes libbw_audio.so plus the executables those
    tests name, and runs each one under adb with LD_LIBRARY_PATH set.

    Exit codes follow ctest's own convention so a CI wrapper can treat this like any other suite:
    0 when every test passed, 77 when every test that ran reported the ctest skip code (and none
    failed), and 1 otherwise. A test's own 77 is mapped through rather than swallowed, which is
    what makes "there is no audio device on this image" show as SKIPPED and never as a pass.

.PARAMETER Abi
    x86_64 (the emulator) or arm64-v8a (a headset). Decides the default build directory.

.PARAMETER BuildDir
    An existing Android build directory to use as-is. Without it the script configures and builds
    build-android-<Abi> from scratch.

.PARAMETER Tests
    Test names to run (the ctest names, such as smoke or audio_sink). Default: every test in the
    build directory. Accepts wildcards.

.PARAMETER Serial
    The adb device serial, for a host with more than one attached.

.PARAMETER Keep
    Leave the pushed files on the device afterwards, for a manual re-run or a debugger.

.EXAMPLE
    tools\android\run-tests.ps1
    Configure, build and run the whole suite on the one attached device.

.EXAMPLE
    tools\android\run-tests.ps1 -BuildDir build-android-x86_64 -Tests audio_sink,os
    Re-run two tests against a build that already exists.

.NOTES
    Needs ANDROID_HOME (or ANDROID_SDK_ROOT) pointing at an SDK with platform-tools, and
    ANDROID_NDK_HOME at an NDK r27 or later. See docs/build.md, "Android".
#>
[CmdletBinding()]
param(
    [ValidateSet('x86_64', 'arm64-v8a')]
    [string]   $Abi = 'x86_64',
    [string]   $BuildDir,
    [string[]] $Tests,
    [string]   $Serial,
    [int]      $ApiLevel = 26,
    [string]   $Config = 'RelWithDebInfo',
    [switch]   $Keep
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# ---- the SDK and the NDK -------------------------------------------------------------------
$sdk = $env:ANDROID_HOME
if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not (Test-Path $sdk)) { throw "No Android SDK: set ANDROID_HOME (looked at '$sdk')" }

$adb = Join-Path $sdk 'platform-tools\adb.exe'
if (-not (Test-Path $adb)) { throw "No adb at '$adb' (sdkmanager --install platform-tools)" }

$ndk = $env:ANDROID_NDK_HOME
if (-not $ndk) {
    $ndkRoot = Join-Path $sdk 'ndk'
    if (Test-Path $ndkRoot) {
        $newest = Get-ChildItem $ndkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($newest) { $ndk = $newest.FullName }
    }
}

$adbArgs = @()
if ($Serial) { $adbArgs = @('-s', $Serial) }

# adb writes its progress lines to stderr, and so does every test that prints a FAIL. PowerShell
# turns a native command's stderr into ErrorRecords, which an $ErrorActionPreference of Stop then
# makes terminating - on a perfectly successful push. Merging the streams and flattening each
# record to its text keeps the test's own diagnostics (they are the reason to run this at all)
# while stopping a progress line from ending the script. The exit code, not the stream, is what
# this script judges a run by, and it comes back through the BWA_RC line below.
function Invoke-Adb {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $adb @adbArgs @args 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.ToString() } else { $_ }
        }
    } finally { $ErrorActionPreference = $prev }
}

# ---- build (unless a build directory was handed in) -----------------------------------------
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build-android-$Abi" }
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $repo $BuildDir }

if (-not (Test-Path (Join-Path $BuildDir 'CTestTestfile.cmake'))) {
    if (-not $ndk) { throw "No NDK: set ANDROID_NDK_HOME (or sdkmanager --install 'ndk;27.3.13750724')" }
    $toolchain = Join-Path $ndk 'build\cmake\android.toolchain.cmake'
    if (-not (Test-Path $toolchain)) { throw "No NDK toolchain file at '$toolchain'" }

    # The NDK ships no generator. Ninja comes with the SDK's own cmake package; a ninja on PATH
    # (Visual Studio ships one) does just as well.
    $ninja = $null
    $sdkCmake = Join-Path $sdk 'cmake'
    if (Test-Path $sdkCmake) {
        $cand = Get-ChildItem $sdkCmake -Directory | Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName 'bin\ninja.exe' } |
                Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($cand) { $ninja = $cand }
    }
    if (-not $ninja) {
        $onPath = Get-Command ninja -ErrorAction SilentlyContinue
        if ($onPath) { $ninja = $onPath.Source }
    }
    if (-not $ninja) { throw "No ninja: sdkmanager --install 'cmake;3.31.6', or put one on PATH" }

    # ANDROID_STL is pinned rather than left to default. A static phonon is C++, so the library
    # needs a libc++ from somewhere; c++_static puts it inside libbw_audio.so and keeps the promise
    # that nothing ships beside the engine. That is safe here because the ABI is C: no C++ type,
    # exception or allocation crosses the library boundary, which is the case where two static
    # libc++ copies in one process bite. The NDK's CMake toolchain already defaults this way, but
    # Gradle's does not, so say it.
    Write-Host "configuring $BuildDir ($Abi, android-$ApiLevel)" -ForegroundColor Cyan
    & cmake -S $repo -B $BuildDir -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DANDROID_ABI=$Abi" "-DANDROID_PLATFORM=android-$ApiLevel" `
        -DANDROID_STL=c++_static `
        "-DCMAKE_BUILD_TYPE=$Config" -DBWA_BUILD_TESTS=ON
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
}

Write-Host "building $BuildDir" -ForegroundColor Cyan
& cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }

# ---- the test list, read out of the build tree rather than duplicated here -------------------
# CTestTestfile.cmake carries one add_test line per test, so the script and ctest can never
# disagree about what the suite IS. Only the executable and its arguments are taken; a test that
# names something outside the build directory (there is none today) is reported and skipped.
$ctf = Join-Path $BuildDir 'CTestTestfile.cmake'
if (-not (Test-Path $ctf)) { throw "No CTestTestfile.cmake in '$BuildDir'" }

$all = @()
foreach ($line in Get-Content $ctf) {
    if ($line -notmatch '^\s*add_test\(') { continue }
    # add_test([=[name]=] "exe" "arg" ...) — cmake quotes each argument, and the name may carry
    # the bracket form when it holds characters that would need escaping.
    $m = [regex]::Match($line, '^\s*add_test\(\s*(?:\[=\[(?<n1>[^\]]*)\]=\]|"(?<n2>[^"]*)"|(?<n3>\S+))\s+(?<rest>.*)\)\s*$')
    if (-not $m.Success) { continue }
    $name = $m.Groups['n1'].Value + $m.Groups['n2'].Value + $m.Groups['n3'].Value
    # @() around the pipeline is load-bearing: a test with no arguments yields ONE string, which
    # PowerShell unrolls to a scalar, and [0] on a string is its first CHARACTER, not the path.
    $parts = @([regex]::Matches($m.Groups['rest'].Value, '"([^"]*)"') | ForEach-Object { $_.Groups[1].Value })
    if ($parts.Count -lt 1) { continue }
    $all += [pscustomobject]@{ Name = $name; Exe = $parts[0]; Args = @($parts | Select-Object -Skip 1) }
}
if ($all.Count -eq 0) { throw "No tests found in '$ctf'" }

$chosen = @($all)
if ($Tests) {
    $chosen = @($all | Where-Object { $n = $_.Name; @($Tests | Where-Object { $n -like $_ }).Count -gt 0 })
    if ($chosen.Count -eq 0) { throw "No test matched: $($Tests -join ', ')" }
}

# ---- the device ------------------------------------------------------------------------------
$devices = @((Invoke-Adb devices) | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice\s*$' })
if ($devices.Count -eq 0) { throw 'No booted device or emulator (adb devices is empty)' }
if ($devices.Count -gt 1 -and -not $Serial) {
    throw "More than one device attached; pass -Serial. Found:`n$($devices -join "`n")"
}
$null = Invoke-Adb wait-for-device

# /data/local/tmp is the one directory an adb shell can both write and execute in on a stock
# image. An app's own lib directory would need an installed package, which the suite has no use for.
$remote = '/data/local/tmp/bwa'
Invoke-Adb shell "rm -rf $remote; mkdir -p $remote" | Out-Null

$push = @(Join-Path $BuildDir 'libbw_audio.so')
foreach ($t in $chosen) {
    $exe = $t.Exe
    if (-not [System.IO.Path]::IsPathRooted($exe)) { $exe = Join-Path $BuildDir $exe }
    if (Test-Path $exe) { $push += $exe }
}
foreach ($f in ($push | Sort-Object -Unique)) {
    if (-not (Test-Path $f)) { throw "Missing build artifact: $f" }
    Invoke-Adb push $f $remote | Out-Null
}
Invoke-Adb shell "chmod 755 $remote/*" | Out-Null

# Some tests write beside their input (the calibration and UTF-8 path ones), so run from a
# writable working directory rather than from /.
$env_prefix = "cd $remote && LD_LIBRARY_PATH=$remote"

# ---- run --------------------------------------------------------------------------------------
$SKIP = 77
$pass = 0; $fail = 0; $skip = 0
$failed = @()
Write-Host ''
Write-Host "running $($chosen.Count) test(s) on $((Invoke-Adb shell getprop ro.product.model) -join '') (API $((Invoke-Adb shell getprop ro.build.version.sdk) -join ''))" -ForegroundColor Cyan

foreach ($t in $chosen) {
    $exeName = Split-Path -Leaf $t.Exe
    $argline = ($t.Args | ForEach-Object { "'" + $_ + "'" }) -join ' '
    # The exit code has to come back over a shell whose own status adb does not forward, so the
    # test prints it on a line of its own and the host parses that. Anything else would report
    # every test as passing, which is the failure mode this whole script exists to avoid.
    $cmd = "$env_prefix ./$exeName $argline; echo BWA_RC=`$?"
    $out = Invoke-Adb shell $cmd
    $rcLine = ($out | Where-Object { $_ -match 'BWA_RC=(\d+)' } | Select-Object -Last 1)
    $rc = if ($rcLine -match 'BWA_RC=(\d+)') { [int]$Matches[1] } else { 1 }
    $body = $out | Where-Object { $_ -notmatch 'BWA_RC=' }

    if ($rc -eq 0) {
        $pass++
        Write-Host ("  PASS  " + $t.Name) -ForegroundColor Green
    } elseif ($rc -eq $SKIP) {
        $skip++
        Write-Host ("  SKIP  " + $t.Name) -ForegroundColor Yellow
    } else {
        $fail++; $failed += $t.Name
        Write-Host ("  FAIL  " + $t.Name + " (exit $rc)") -ForegroundColor Red
    }
    if ($VerbosePreference -eq 'Continue' -or $rc -ne 0) { $body | ForEach-Object { Write-Host "        $_" } }
}

if (-not $Keep) { Invoke-Adb shell "rm -rf $remote" | Out-Null }

Write-Host ''
Write-Host "$pass passed, $skip skipped, $fail failed of $($chosen.Count)" -ForegroundColor Cyan
if ($fail -gt 0) { Write-Host "failed: $($failed -join ', ')" -ForegroundColor Red; exit 1 }
if ($pass -eq 0 -and $skip -gt 0) { exit $SKIP }   # everything that ran skipped: ctest's own rule
exit 0
