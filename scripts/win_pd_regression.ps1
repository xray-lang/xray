# Windows regression runner intended to be invoked from win_pd_test.sh
# via prlctl exec. Mirrors the discipline of scripts/run_regression_tests.sh:
# iterate every regression .xr, apply a per-test timeout, honour the
# --no-jit list, skip helper files (_*), and emit a single-line summary
# the host watchdog can parse.

param(
    [string]$XrayBin = 'C:\workspace\xray-build\xray.exe',
    [string]$TestRoot = 'C:\workspace\xray\tests\regression',
    [int]$PerTestTimeout = 30
)

$ErrorActionPreference = 'Continue'

$NoJit = @(
    '1148_scope_race_stress.xr',
    '1205_gc_incremental_pressure.xr',
    '1207_gc_stress.xr'
)

if (-not (Test-Path $XrayBin)) {
    Write-Host "[regression] xray.exe not found: $XrayBin"
    exit 2
}
if (-not (Test-Path $TestRoot)) {
    Write-Host "[regression] test root not found: $TestRoot"
    exit 2
}

# Mirror scripts/run_regression_tests.sh: skip helper files (_*) and
# fixture directories that exist only as imports for other tests.
$Files = Get-ChildItem -Path $TestRoot -Filter '*.xr' -Recurse |
    Where-Object {
        -not $_.Name.StartsWith('_') -and
        $_.FullName -notmatch '[\\/](fixtures|modules|reexport_test)[\\/]'
    } |
    Sort-Object FullName

function Quote-Arg([string]$a) {
    # Approximate the Windows command-line escaping rules. xray test paths
    # have no embedded quotes in practice, but this stays defensive.
    if ($a -match '[\s"]') {
        return '"' + ($a -replace '"', '\"') + '"'
    }
    return $a
}

function Run-XrayTest([string]$bin, [string[]]$argv, [int]$timeoutSec, [string]$stdoutPath, [string]$stderrPath) {
    # Use the .NET Process API directly. Start-Process -PassThru has a
    # known Windows PowerShell 5.x quirk where $proc.ExitCode reads back
    # as $null after WaitForExit, so we manage the process ourselves to
    # get a reliable exit code.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $bin
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $sb = New-Object System.Text.StringBuilder
    foreach ($a in $argv) {
        if ($sb.Length -gt 0) { [void]$sb.Append(' ') }
        [void]$sb.Append((Quote-Arg $a))
    }
    $psi.Arguments = $sb.ToString()

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()

    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()

    $exited = $proc.WaitForExit($timeoutSec * 1000)
    $rc = $null
    if (-not $exited) {
        try { $proc.Kill() } catch {}
        $proc.WaitForExit()
        $rc = -1
    } else {
        $rc = $proc.ExitCode
    }

    $stdoutTask.Wait()
    $stderrTask.Wait()
    [System.IO.File]::WriteAllText($stdoutPath, $stdoutTask.Result)
    [System.IO.File]::WriteAllText($stderrPath, $stderrTask.Result)

    return [pscustomobject]@{ Exited = $exited; ExitCode = $rc }
}

$pass = 0
$fail = 0
$timeout = 0
$failures = @()
$start = Get-Date

foreach ($f in $Files) {
    $argv = @('test')
    if ($NoJit -contains $f.Name) { $argv += '--no-jit' }
    $argv += $f.FullName

    $stdout = [System.IO.Path]::GetTempFileName()
    $stderr = [System.IO.Path]::GetTempFileName()
    $r = Run-XrayTest $XrayBin $argv $PerTestTimeout $stdout $stderr

    if (-not $r.Exited) {
        $timeout++
        $failures += [pscustomobject]@{ Kind = 'TIMEOUT'; Path = $f.FullName; Exit = $r.ExitCode }
        Write-Host "[TIMEOUT] $($f.FullName)"
        Get-Content $stdout -Tail 20 -ErrorAction SilentlyContinue
        Get-Content $stderr -Tail 20 -ErrorAction SilentlyContinue
    } elseif ($r.ExitCode -eq 0) {
        $pass++
    } else {
        $fail++
        $failures += [pscustomobject]@{ Kind = 'FAIL'; Path = $f.FullName; Exit = $r.ExitCode }
        Write-Host "[FAIL] $($f.FullName) (exit=$($r.ExitCode))"
        Get-Content $stdout -Tail 20 -ErrorAction SilentlyContinue
        Get-Content $stderr -Tail 20 -ErrorAction SilentlyContinue
    }

    Remove-Item $stdout -ErrorAction SilentlyContinue
    Remove-Item $stderr -ErrorAction SilentlyContinue
}

$duration = [int]((Get-Date) - $start).TotalSeconds
Write-Host ""
Write-Host ("regression: pass={0} fail={1} timeout={2} total={3} duration={4}s" -f `
    $pass, $fail, $timeout, $Files.Count, $duration)

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Failed tests:"
    foreach ($entry in $failures) {
        Write-Host ("  [{0}] {1}" -f $entry.Kind, $entry.Path)
    }
    exit 1
}
exit 0
