param(
    [string]$Binary = "",
    [string]$Filter = "",
    [int]$Timeout = 10,
    [string]$TestDir = "",
    [switch]$Verbose,
    [switch]$Diff,
    [string]$Allowlist = "",
    [switch]$SkipAllowlist
)

$ErrorActionPreference = "Continue"
$ProjectRoot = (Resolve-Path "$PSScriptRoot\..").Path

if (-not $Binary) {
    if ($env:XRAY_BUILD_DIR -and (Test-Path "$env:XRAY_BUILD_DIR\xray.exe")) {
        $Binary = "$env:XRAY_BUILD_DIR\xray.exe"
    } elseif (Test-Path "$ProjectRoot\build-release\xray.exe") {
        $Binary = "$ProjectRoot\build-release\xray.exe"
    } else {
        $Binary = "$ProjectRoot\build\xray.exe"
    }
}
if (-not $TestDir) {
    $TestDir = "$ProjectRoot\tests\jit"
}
if (-not $Allowlist) {
    $Allowlist = "$ProjectRoot\tests\jit\known_failures.txt"
}

if (-not (Test-Path $Binary)) {
    Write-Host "Error: xray binary not found at $Binary" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $TestDir)) {
    Write-Host "Error: JIT test directory not found at $TestDir" -ForegroundColor Red
    exit 1
}

$KnownFailures = @{}
if (-not $SkipAllowlist -and (Test-Path $Allowlist)) {
    Get-Content $Allowlist | ForEach-Object {
        $line = ($_ -replace '#.*', '').Trim()
        if ($line) { $KnownFailures[$line] = $true }
    }
}

function Quote-Arg([string]$Arg) {
    return '"' + ($Arg -replace '"', '\"') + '"'
}

function Join-Args([string[]]$XrayArgs) {
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($arg in $XrayArgs) {
        $parts.Add((Quote-Arg $arg))
    }
    return ($parts -join ' ')
}

function Invoke-XrayArgs([string]$Exe, [string[]]$XrayArgs, [int]$TimeoutSeconds) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = Join-Args $XrayArgs
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true

    try {
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        [void]$proc.Start()
        $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
        $stderrTask = $proc.StandardError.ReadToEndAsync()
        $exited = $proc.WaitForExit($TimeoutSeconds * 1000)
        if (-not $exited) {
            try { $proc.Kill() } catch {}
            $proc.WaitForExit()
            $stdoutTask.Wait()
            $stderrTask.Wait()
            return [pscustomobject]@{ ExitCode = 124; Stdout = $stdoutTask.Result; Stderr = $stderrTask.Result }
        }
        $stdoutTask.Wait()
        $stderrTask.Wait()
        return [pscustomobject]@{ ExitCode = $proc.ExitCode; Stdout = $stdoutTask.Result; Stderr = $stderrTask.Result }
    } catch {
        return [pscustomobject]@{ ExitCode = -999; Stdout = ""; Stderr = $_.Exception.Message }
    }
}

function Invoke-XrayFile([string]$Exe, [string]$File, [int]$TimeoutSeconds) {
    return Invoke-XrayArgs -Exe $Exe -XrayArgs @($File) -TimeoutSeconds $TimeoutSeconds
}

function Normalize-JitOutput([string]$Text) {
    $lines = $Text -split "`r?`n"
    $out = New-Object System.Collections.Generic.List[string]
    $inDump = $false
    foreach ($line in $lines) {
        if ($line -match '^===== end .* =====$') {
            $inDump = $false
            continue
        }
        if ($line -match '^===== .* =====$') {
            $inDump = $true
            continue
        }
        if ($inDump) {
            continue
        }
        $out.Add($line)
    }
    $joined = $out -join "`r`n"
    return ($joined -replace "(`r?`n)+$", "")
}

function Get-Expected([string]$Path) {
    $expected = New-Object System.Collections.Generic.List[string]
    $skip = $false
    foreach ($line in Get-Content $Path) {
        if ($line -match '^// EXPECTED: (.*)$') {
            $expected.Add($Matches[1])
            continue
        }
        if ($line -match '^// SKIP') {
            $skip = $true
            break
        }
        if ($line -notmatch '^//') {
            break
        }
    }
    return [pscustomobject]@{ Skip = $skip; Text = ($expected -join "`r`n"); Count = $expected.Count }
}

$files = @(Get-ChildItem -Path $TestDir -Filter "*.xr" -File | Sort-Object Name)
if ($Filter) {
    $files = @($files | Where-Object { $_.BaseName -like "*$Filter*" })
}

$total = 0
$pass = 0
$fail = 0
$known = 0
$skip = 0
$timeoutCount = 0
$crashCount = 0
$failures = New-Object System.Collections.Generic.List[string]

foreach ($file in $files) {
    $total++
    $name = $file.BaseName
    $relPath = $file.FullName.Replace($ProjectRoot, "").TrimStart('\', '/') -replace '\\', '/'
    $isKnown = (-not $SkipAllowlist) -and $KnownFailures.ContainsKey($relPath)

    if ($Diff) {
        $interp = Invoke-XrayArgs -Exe $Binary -XrayArgs @("run", "--no-jit", $file.FullName) -TimeoutSeconds $Timeout
        $jit = Invoke-XrayArgs -Exe $Binary -XrayArgs @("run", "--jit-force", $file.FullName) -TimeoutSeconds $Timeout

        if ($interp.ExitCode -eq 124 -or $jit.ExitCode -eq 124) {
            $timeoutCount++
            Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
            if ($isKnown) {
                $known++
                Write-Host "TIMEOUT (known)" -ForegroundColor Yellow
            } else {
                $fail++
                Write-Host "TIMEOUT" -ForegroundColor Yellow
                $failures.Add("TIMEOUT: $relPath")
            }
            continue
        }
        if ($jit.ExitCode -lt -1 -and $jit.ExitCode -ne -999) {
            $crashCount++
            Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
            if ($isKnown) {
                $known++
                Write-Host "CRASH (known)" -ForegroundColor Yellow
            } else {
                $fail++
                Write-Host ("CRASH (jit=0x{0:X})" -f [uint32]$jit.ExitCode) -ForegroundColor Red
                $failures.Add(("CRASH(0x{0:X}): {1}" -f [uint32]$jit.ExitCode, $relPath))
            }
            continue
        }
        if ($interp.ExitCode -ne $jit.ExitCode) {
            Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
            if ($isKnown) {
                $known++
                Write-Host "EXIT_DIFF (known)" -ForegroundColor Yellow
            } else {
                $fail++
                Write-Host "EXIT_DIFF (interp=$($interp.ExitCode), jit=$($jit.ExitCode))" -ForegroundColor Red
                $failures.Add("EXIT_DIFF(interp=$($interp.ExitCode),jit=$($jit.ExitCode)): $relPath")
            }
            continue
        }
        if ($interp.ExitCode -ne 0) {
            $skip++
            Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
            Write-Host "BOTH_FAIL (exit=$($interp.ExitCode))" -ForegroundColor Yellow
            continue
        }

        $interpOut = Normalize-JitOutput $interp.Stdout
        $jitOut = Normalize-JitOutput $jit.Stdout
        if ($interpOut -eq $jitOut) {
            $pass++
            Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
            Write-Host "PASS" -ForegroundColor Green
        } else {
            Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
            if ($isKnown) {
                $known++
                Write-Host "OUTPUT_DIFF (known)" -ForegroundColor Yellow
            } else {
                $fail++
                Write-Host "OUTPUT_DIFF" -ForegroundColor Red
                $failures.Add("OUTPUT_DIFF: $relPath")
            }
            if ($Verbose) {
                Write-Host "    Interpreter:"
                $interpOut -split "`r?`n" | Select-Object -First 8 | ForEach-Object { Write-Host "      $_" }
                Write-Host "    JIT:"
                $jitOut -split "`r?`n" | Select-Object -First 8 | ForEach-Object { Write-Host "      $_" }
            }
        }
        continue
    }

    $meta = Get-Expected $file.FullName

    if ($meta.Skip) {
        $skip++
        Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
        Write-Host "SKIP" -ForegroundColor Yellow
        continue
    }
    if ($meta.Count -eq 0) {
        $skip++
        Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
        Write-Host "SKIP (no EXPECTED)" -ForegroundColor Yellow
        continue
    }

    $result = Invoke-XrayFile $Binary $file.FullName $Timeout
    if ($result.ExitCode -eq 124) {
        $timeoutCount++
        Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
        if ($isKnown) {
            $known++
            Write-Host "TIMEOUT (known)" -ForegroundColor Yellow
        } else {
            $fail++
            Write-Host "TIMEOUT" -ForegroundColor Yellow
            $failures.Add("TIMEOUT: $relPath")
        }
        continue
    }
    if ($result.ExitCode -lt -1 -and $result.ExitCode -ne -999) {
        $crashCount++
        Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
        if ($isKnown) {
            $known++
            Write-Host "CRASH (known)" -ForegroundColor Yellow
        } else {
            $fail++
            Write-Host ("CRASH (exit=0x{0:X})" -f [uint32]$result.ExitCode) -ForegroundColor Red
            $failures.Add(("CRASH(0x{0:X}): {1}" -f [uint32]$result.ExitCode, $relPath))
        }
        continue
    }
    if ($result.ExitCode -ne 0) {
        Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
        if ($isKnown) {
            $known++
            Write-Host "FAIL (known, exit=$($result.ExitCode))" -ForegroundColor Yellow
        } else {
            $fail++
            Write-Host "FAIL (exit=$($result.ExitCode))" -ForegroundColor Red
            $failures.Add("FAIL($($result.ExitCode)): $relPath")
        }
        if ($Verbose) {
            Write-Host "    Stdout: $($result.Stdout.TrimEnd())"
            Write-Host "    Stderr: $($result.Stderr.TrimEnd())"
        }
        continue
    }

    $actual = ($result.Stdout -replace "`r?`n$", "")
    if ($actual -eq $meta.Text) {
        $pass++
        Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
        Write-Host "PASS" -ForegroundColor Green
    } else {
        Write-Host ("  [{0,3}] {1,-45} ... " -f $total, $name) -NoNewline
        if ($isKnown) {
            $known++
            Write-Host "FAIL (known)" -ForegroundColor Yellow
        } else {
            $fail++
            Write-Host "FAIL" -ForegroundColor Red
            $failures.Add("MISMATCH: $relPath")
        }
        if ($Verbose) {
            Write-Host "    Expected:"
            $meta.Text -split "`r?`n" | ForEach-Object { Write-Host "      $_" }
            Write-Host "    Actual:"
            $actual -split "`r?`n" | ForEach-Object { Write-Host "      $_" }
            if ($result.Stderr) {
                Write-Host "    Stderr: $($result.Stderr.TrimEnd())"
            }
        }
    }
}

Write-Host ""
Write-Host "======================================"
Write-Host "JIT Test Summary"
Write-Host "======================================"
Write-Host "Total:   $total"
Write-Host "Pass:    $pass" -ForegroundColor Green
Write-Host "Fail:    $fail" -ForegroundColor Red
Write-Host "Crash:   $crashCount" -ForegroundColor Red
Write-Host "Timeout: $timeoutCount" -ForegroundColor Yellow
Write-Host "Skip:    $skip" -ForegroundColor Yellow
if (-not $SkipAllowlist) {
    Write-Host "Known:   $known" -ForegroundColor Yellow
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Unexpected failures:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
}

if ($fail -eq 0) {
    Write-Host ""
    if ($known -gt 0) {
        Write-Host "All JIT tests passed ($known known failures in allowlist)." -ForegroundColor Green
    } else {
        Write-Host "All JIT tests passed!" -ForegroundColor Green
    }
    exit 0
}
exit 1
