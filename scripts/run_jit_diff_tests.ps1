# JIT Differential Test Runner (PowerShell, Windows equivalent of run_jit_diff_tests.sh)
#
# Runs tests with --jit-force (threshold=1) vs --no-jit (interpreter only),
# comparing output to detect JIT correctness issues.
#
# WHY --jit-force:
#   Default JIT threshold is 100 calls.  Regression tests call @test functions
#   only once, so default mode never actually triggers JIT compilation.
#   --jit-force sets threshold=1, ensuring every function gets JIT compiled.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\run_jit_diff_tests.ps1 [options]
#   -Binary <path>    Path to xray.exe (default: auto-detect)
#   -Filter <string>  Run only tests whose path contains this substring
#   -Verbose          Show output diff on mismatch
#   -Timeout <sec>    Timeout per test per mode (default: 15)
#   -TestDir <dir>    Test directory (default: tests\regression)
#   -Jobs <N>         Parallel jobs (default: CPU count)
#   -IncludeJitDir    Also run tests\jit directory
#   -Allowlist <path> Custom known-failures file
#   -SkipAllowlist    Ignore the allowlist; treat every failure as unexpected

param(
    [string]$Binary = "",
    [string]$TestDir = "",
    [string]$Filter = "",
    [int]$Timeout = 15,
    [int]$Jobs = 0,
    [switch]$Verbose,
    [switch]$IncludeJitDir,
    [string]$Allowlist = "",
    [switch]$SkipAllowlist
)

$ErrorActionPreference = "Continue"

# ── Paths ──────────────────────────────────────────────────────────────
$ProjectRoot = (Resolve-Path "$PSScriptRoot\..").Path

# Auto-detect build directory
if (-not $Binary) {
    if ($env:XRAY_BUILD_DIR -and (Test-Path "$env:XRAY_BUILD_DIR\xray.exe")) {
        $Binary = "$env:XRAY_BUILD_DIR\xray.exe"
    } elseif (Test-Path "$ProjectRoot\build-release\xray.exe") {
        $Binary = "$ProjectRoot\build-release\xray.exe"
    } else {
        $Binary = "$ProjectRoot\build\xray.exe"
    }
}
if (-not $TestDir)    { $TestDir   = "$ProjectRoot\tests\regression" }
if (-not $Allowlist)  { $Allowlist = "$ProjectRoot\tests\jit\known_failures.txt" }
if ($Jobs -le 0)      { $Jobs = [Environment]::ProcessorCount }

if (-not (Test-Path $Binary)) {
    Write-Host "Error: xray binary not found at $Binary" -ForegroundColor Red
    exit 1
}

# ── Known failures (allowlist) ─────────────────────────────────────────
$KnownFailures = @{}
if (-not $SkipAllowlist -and (Test-Path $Allowlist)) {
    Get-Content $Allowlist | ForEach-Object {
        $line = ($_ -replace '#.*', '').Trim()
        if ($line) { $KnownFailures[$line] = $true }
    }
}

# ── Collect test files ─────────────────────────────────────────────────
$testFiles = @(
    Get-ChildItem -Path $TestDir -Filter "*.xr" -Recurse |
        Where-Object { -not $_.Name.StartsWith("_") } |
        Sort-Object FullName
)
if ($IncludeJitDir) {
    $jitDir = "$ProjectRoot\tests\jit"
    if (Test-Path $jitDir) {
        $testFiles += @(
            Get-ChildItem -Path $jitDir -Filter "*.xr" -Recurse |
                Where-Object { -not $_.Name.StartsWith("_") } |
                Sort-Object FullName
        )
    }
}
if ($Filter) {
    $testFiles = @($testFiles | Where-Object { $_.FullName -like "*$Filter*" })
}
$total = $testFiles.Count

# ── Banner ─────────────────────────────────────────────────────────────
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "JIT Differential Test Runner (Windows)" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "Binary:    $Binary"
Write-Host "Test dir:  $TestDir"
Write-Host "Timeout:   ${Timeout}s per mode"
Write-Host "Parallel:  $Jobs jobs"
Write-Host "Tests:     $total"
Write-Host "Strategy:  --jit-force vs --no-jit"
Write-Host ""

$sw = [System.Diagnostics.Stopwatch]::StartNew()

# ── Worker scriptblock (runs inside RunspacePool) ──────────────────────
# Returns a hashtable with all info needed for the summary.
$WorkerScript = {
    param($testFile, $xray, $timeoutMs, $projectRoot)

    function Invoke-Xray($xray, $args, $timeoutMs) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName  = $xray
        $psi.Arguments = $args
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError  = $true
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow  = $true
        try {
            $p = [System.Diagnostics.Process]::Start($psi)
            $stdout = $p.StandardOutput.ReadToEnd()
            if (-not $p.WaitForExit($timeoutMs)) {
                try { $p.Kill() } catch {}
                return @{ Rc = 124; Out = $stdout }
            }
            return @{ Rc = $p.ExitCode; Out = $stdout }
        } catch {
            return @{ Rc = -999; Out = "" }
        }
    }

    $relPath = $testFile.Replace($projectRoot, '').TrimStart('\', '/') -replace '\\', '/'

    # Run interp and jit (sequential inside one worker; workers run in parallel)
    $interp = Invoke-Xray $xray "test --quiet --no-jit `"$testFile`"" $timeoutMs
    $jit    = Invoke-Xray $xray "test --quiet --jit-force `"$testFile`"" $timeoutMs

    $iRc = $interp.Rc; $jRc = $jit.Rc

    # Windows crash codes: ACCESS_VIOLATION = -1073741819, STACK_OVERFLOW = -1073741571, etc.
    $IsCrash = { param($rc) $rc -lt -1 -and $rc -ne -999 -and $rc -ne 124 }

    $status = "UNKNOWN"
    if ($iRc -eq 124 -and $jRc -eq 124)                           { $status = "TIMEOUT_BOTH" }
    elseif ($jRc -eq 124)                                          { $status = "TIMEOUT_JIT" }
    elseif ((& $IsCrash $jRc) -and -not (& $IsCrash $iRc))        { $status = "CRASH" }
    elseif ((& $IsCrash $jRc) -and (& $IsCrash $iRc))             { $status = "BOTH_FAIL" }
    elseif ($iRc -ne 0 -and $jRc -ne 0)                           { $status = "BOTH_FAIL" }
    elseif ($iRc -ne $jRc)                                         { $status = "EXIT_DIFF" }
    else {
        $normI = $interp.Out -replace '\(\d+ms\)', '(_ms)' -replace '\d+ms', '_ms'
        $normJ = $jit.Out    -replace '\(\d+ms\)', '(_ms)' -replace '\d+ms', '_ms'
        if ($normI -eq $normJ) { $status = "PASS" } else { $status = "OUTPUT_DIFF" }
    }

    return @{
        Status   = $status
        InterpRc = $iRc
        JitRc    = $jRc
        RelPath  = $relPath
        InterpOut= $interp.Out
        JitOut   = $jit.Out
    }
}

# ── Parallel execution via RunspacePool ────────────────────────────────
$pool = [RunspaceFactory]::CreateRunspacePool(1, $Jobs)
$pool.Open()

$pipelines = [System.Collections.ArrayList]::new()
$timeoutMs = $Timeout * 1000

for ($i = 0; $i -lt $total; $i++) {
    $ps = [PowerShell]::Create().AddScript($WorkerScript).
        AddArgument($testFiles[$i].FullName).
        AddArgument($Binary).
        AddArgument($timeoutMs).
        AddArgument($ProjectRoot)
    $ps.RunspacePool = $pool
    [void]$pipelines.Add(@{ Index = $i; PS = $ps; Handle = $ps.BeginInvoke() })
}

# ── Collect results (in submission order for stable output) ────────────
$pass = 0; $diffFail = 0; $bothFail = 0; $jitCrash = 0; $timeoutCount = 0
$knownCount = 0; $unexpectedCount = 0; $failures = @()

foreach ($job in $pipelines) {
    $result = $job.PS.EndInvoke($job.Handle)
    $job.PS.Dispose()

    if (-not $result -or $result.Count -eq 0) {
        $idx = $job.Index + 1
        Write-Host ("  [{0,3}] {1,-55} " -f $idx, $testFiles[$job.Index].Name) -NoNewline
        Write-Host "ERROR (no result)" -ForegroundColor Red
        continue
    }
    $r = $result[0]
    $idx     = $job.Index + 1
    $status  = $r.Status
    $relPath = $r.RelPath
    $label   = "[{0,3}] {1,-55}" -f $idx, $relPath

    $isKnown = $KnownFailures.ContainsKey($relPath)

    switch ($status) {
        "PASS" {
            $pass++
            Write-Host "  $label " -NoNewline; Write-Host "PASS" -ForegroundColor Green
        }
        "TIMEOUT_BOTH" {
            $timeoutCount++
            Write-Host "  $label " -NoNewline; Write-Host "TIMEOUT (both)" -ForegroundColor Yellow
        }
        "TIMEOUT_JIT" {
            $timeoutCount++
            Write-Host "  $label " -NoNewline; Write-Host "TIMEOUT (jit)" -ForegroundColor Yellow
            $failures += "TIMEOUT(jit): $relPath"
        }
        "CRASH" {
            $jitCrash++
            if ($isKnown) {
                $knownCount++
                Write-Host "  $label " -NoNewline; Write-Host "CRASH (known)" -ForegroundColor Yellow
            } else {
                $unexpectedCount++
                Write-Host "  $label " -NoNewline
                Write-Host "CRASH (jit=0x$("{0:X}" -f [uint32]$r.JitRc), interp=$($r.InterpRc))" -ForegroundColor Red
                $failures += "CRASH(jit=0x$("{0:X}" -f [uint32]$r.JitRc)): $relPath"
            }
        }
        "BOTH_FAIL" {
            $bothFail++
            Write-Host "  $label " -NoNewline
            Write-Host "BOTH_FAIL (interp=$($r.InterpRc), jit=$($r.JitRc))" -ForegroundColor Yellow
        }
        "EXIT_DIFF" {
            $diffFail++
            if ($isKnown) {
                $knownCount++
                Write-Host "  $label " -NoNewline; Write-Host "EXIT_DIFF (known)" -ForegroundColor Yellow
            } else {
                $unexpectedCount++
                Write-Host "  $label " -NoNewline
                Write-Host "EXIT_DIFF (interp=$($r.InterpRc), jit=$($r.JitRc))" -ForegroundColor Red
                $failures += "EXIT_DIFF(interp=$($r.InterpRc),jit=$($r.JitRc)): $relPath"
            }
        }
        "OUTPUT_DIFF" {
            $diffFail++
            if ($isKnown) {
                $knownCount++
                Write-Host "  $label " -NoNewline; Write-Host "OUTPUT_DIFF (known)" -ForegroundColor Yellow
            } else {
                $unexpectedCount++
                Write-Host "  $label " -NoNewline; Write-Host "OUTPUT_DIFF" -ForegroundColor Red
                $failures += "OUTPUT_DIFF: $relPath"
            }
            if ($Verbose) {
                $iLines = ($r.InterpOut -split "`n") | Select-Object -First 5
                $jLines = ($r.JitOut   -split "`n") | Select-Object -First 5
                Write-Host "    --- interpreter ---" -ForegroundColor DarkGray
                $iLines | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
                Write-Host "    --- jit ---" -ForegroundColor DarkGray
                $jLines | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
            }
        }
        default {
            Write-Host "  $label " -NoNewline; Write-Host "UNKNOWN($status)" -ForegroundColor Red
        }
    }
}

$pool.Close()
$pool.Dispose()

# ── Summary ────────────────────────────────────────────────────────────
$sw.Stop()
Write-Host ""
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "JIT Differential Test Summary" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "Total:      $total"
Write-Host "Elapsed:    $([int]$sw.Elapsed.TotalSeconds)s"
Write-Host "Pass:       $pass" -ForegroundColor Green
Write-Host "Diff fail:  $diffFail" -ForegroundColor Red
Write-Host "JIT crash:  $jitCrash" -ForegroundColor Red
Write-Host "Both fail:  $bothFail  (pre-existing, not JIT bugs)" -ForegroundColor Yellow
Write-Host "Timeout:    $timeoutCount" -ForegroundColor Yellow
if (-not $SkipAllowlist) {
    Write-Host "Known:      $knownCount  (in allowlist)" -ForegroundColor Yellow
    Write-Host "Unexpected: $unexpectedCount  (NEW regressions)" -ForegroundColor Red
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Unexpected JIT failures (NOT in allowlist):" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
}

Write-Host ""
if ($unexpectedCount -eq 0) {
    if ($knownCount -gt 0) {
        Write-Host "All JIT differential tests passed ($knownCount known failures in allowlist)." -ForegroundColor Green
    } else {
        Write-Host "All JIT differential tests passed!" -ForegroundColor Green
    }
    exit 0
} else {
    Write-Host "$unexpectedCount unexpected JIT failure(s) detected!" -ForegroundColor Red
    exit 1
}
