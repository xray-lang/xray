param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDir '..'))
$buildName = if ($env:XR_ASAN_BUILD_DIR) { $env:XR_ASAN_BUILD_DIR } else { 'build-asan-windows' }
$buildDir = if ([System.IO.Path]::IsPathRooted($buildName)) {
    [System.IO.Path]::GetFullPath($buildName)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot $buildName))
}
# Default to all cores: this lane is RUN_SERIAL, so it owns the machine and the
# ASan rebuild is its dominant cost. (Was hard-coded to 8.)
$jobs = if ($env:XR_ASAN_JOBS) { [int]$env:XR_ASAN_JOBS } else { [Environment]::ProcessorCount }
$configuration = if ($env:XR_ASAN_CONFIGURATION) {
    $env:XR_ASAN_CONFIGURATION
} else {
    # /MDd archives require the MSVC debug CRT, which a managed Zig fallback
    # cannot resolve portably from a release SDK payload. RelWithDebInfo keeps
    # symbols and ASan instrumentation while using the redistributable CRT ABI.
    'RelWithDebInfo'
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [string[]]$Arguments = @()
    )
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "command failed ($LASTEXITCODE): $Command $($Arguments -join ' ')"
    }
}

Write-Host "== [asan_focused/windows] root=$repoRoot"
Write-Host "== [asan_focused/windows] build=$buildDir config=$configuration jobs=$jobs"

if ($env:XR_ASAN_SKIP_BUILD -ne '1') {
    Invoke-Checked -Command cmake -Arguments @(
        '-S', $repoRoot, '-B', $buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DENABLE_ASAN=ON', '-DENABLE_UBSAN=OFF', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
    )
    # Build only the executable and focused test binaries exercised below.
    # Their normal target dependencies still rebuild the compiler and runtime;
    # avoiding the unrelated all-tests solution keeps this gate practical.
    Invoke-Checked -Command cmake -Arguments @(
        '--build', $buildDir, '--config', $configuration,
        '--target', 'xray', 'test_xi_cgen', 'test_xaot_driver',
        'test_cgen_verify_output', 'test_cli_toolchain', 'test_xglobal_summary',
        '--parallel', [string]$jobs
    )
}

$xrayExe = Join-Path $buildDir "$configuration\xray.exe"
if (-not (Test-Path -LiteralPath $xrayExe -PathType Leaf)) {
    if ($env:XR_ASAN_SKIP_BUILD -eq '1') {
        throw "XR_ASAN_SKIP_BUILD=1 but no ASan binary at ${xrayExe}: build it first."
    }
    throw "ASan xray executable is missing: $xrayExe"
}

# When the build was skipped, refuse a binary older than the sources it claims
# to represent — a stale pass is worse than no run.
if ($env:XR_ASAN_SKIP_BUILD -eq '1') {
    $binTime = (Get-Item -LiteralPath $xrayExe).LastWriteTimeUtc
    $srcDirs = @('src', 'include', 'stdlib') | ForEach-Object { Join-Path $repoRoot $_ } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Container }
    $stale = Get-ChildItem -Path $srcDirs -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTimeUtc -gt $binTime } | Select-Object -First 1
    if ($stale) {
        throw "ASan binary is stale: $($stale.FullName) is newer than $xrayExe. Rebuild or unset XR_ASAN_SKIP_BUILD."
    }
    Write-Host "== [asan_focused/windows] XR_ASAN_SKIP_BUILD=1: reusing up-to-date ASan binary"
}

# The MSVC ASan runtime ships beside cl.exe.  Visual Studio normally stages it
# for debug runs; adding the compiler directory to PATH also makes direct CTest
# and workload invocations deterministic on Build Tools-only installations.
$cache = Join-Path $buildDir 'CMakeCache.txt'
$toolLine = Select-String -LiteralPath $cache `
    -Pattern '^(?:CMAKE_C_COMPILER|CMAKE_LINKER):FILEPATH=(.+)$' |
    Select-Object -First 1
if ($toolLine) {
    # Visual Studio generators do not persist CMAKE_C_COMPILER in the cache,
    # but CMAKE_LINKER lives beside cl.exe and both MSVC ASan runtime DLLs.
    $compilerDir = Split-Path -Parent $toolLine.Matches[0].Groups[1].Value
    if (Test-Path -LiteralPath $compilerDir -PathType Container) {
        $asanRuntimes = @(Get-ChildItem -LiteralPath $compilerDir -File `
            -Filter 'clang_rt.asan*_dynamic-x86_64.dll')
        if ($asanRuntimes.Count -eq 0) {
            throw "MSVC ASan runtime DLL is missing beside the compiler: $compilerDir"
        }
        $env:PATH = "$compilerDir;$env:PATH"

        # Keep the focused build directly runnable outside this PowerShell
        # process as well. Windows resolves a DLL beside an executable before
        # PATH, so stage both the release and debug ASan runtimes when the
        # active MSVC toolset provides them. This also prevents an interactive
        # missing-DLL dialog from blocking unattended validation.
        $configurationSuffix = "$([System.IO.Path]::DirectorySeparatorChar)$configuration"
        $asanOutputDirectories = @(Get-ChildItem -LiteralPath $buildDir -Recurse -File `
            -Filter '*.exe' |
            Where-Object {
                $_.DirectoryName.EndsWith(
                    $configurationSuffix,
                    [System.StringComparison]::OrdinalIgnoreCase
                )
            } |
            ForEach-Object { $_.Directory.FullName } |
            Sort-Object -Unique)
        if ($asanOutputDirectories.Count -eq 0) {
            throw "Windows ASan executable output directories are missing below: $buildDir"
        }
        foreach ($asanOutputDirectory in $asanOutputDirectories) {
            foreach ($asanRuntime in $asanRuntimes) {
                $stagedAsanRuntime = Join-Path $asanOutputDirectory $asanRuntime.Name
                Copy-Item -LiteralPath $asanRuntime.FullName `
                    -Destination $stagedAsanRuntime -Force
                if ((Get-FileHash -Algorithm SHA256 -LiteralPath $asanRuntime.FullName).Hash -ne
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedAsanRuntime).Hash) {
                    throw "Windows ASan runtime staging verification failed: $stagedAsanRuntime"
                }
                Write-Host "== [asan_focused/windows] staged runtime: $stagedAsanRuntime"
            }
        }
    }
} else {
    throw "cannot locate the MSVC tool directory in $cache"
}
$env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1:abort_on_error=1:symbolize=1:strict_string_checks=1'

# Release/install layouts expose their managed Zig through the normal discovery
# paths.  A source checkout keeps the pinned developer toolchain in the sibling
# .tools directory, so make that candidate visible when neither XRAY_ZIG nor a
# PATH Zig was supplied.  The compiler still capability-probes MSVC first and
# falls back only when the generated-C contract is unsupported.
if (-not $env:XRAY_ZIG -and -not (Get-Command zig -ErrorAction SilentlyContinue)) {
    $preferredLine = Select-String -LiteralPath $cache `
        -Pattern '^XRAY_PREFERRED_ZIG_VERSION:STRING=(.+)$' |
        Select-Object -First 1
    $preferredZig = if ($preferredLine) {
        $preferredLine.Matches[0].Groups[1].Value
    } else {
        '0.16.0'
    }
    $workspaceZig = [System.IO.Path]::GetFullPath((Join-Path $repoRoot `
        "..\.tools\zig-x86_64-windows-$preferredZig\zig.exe"))
    if (Test-Path -LiteralPath $workspaceZig -PathType Leaf) {
        $env:XRAY_ZIG = $workspaceZig
        Write-Host "== [asan_focused/windows] managed Zig fallback: $workspaceZig"
    }
}

$focusedRegex = if ($env:XR_ASAN_CTEST_REGEX) {
    $env:XR_ASAN_CTEST_REGEX
} else {
    '^(meta_ownership_inventory|test_xi_cgen|test_xaot_driver|test_cgen_verify_output|test_cli_toolchain|test_xglobal_summary)$'
}
Write-Host "== [asan_focused/windows] focused CTest regex: $focusedRegex"
Invoke-Checked -Command ctest -Arguments @(
    '--test-dir', $buildDir, '-C', $configuration, '--output-on-failure',
    '-j', [string]$jobs, '-R', $focusedRegex, '--timeout', '600'
)

$diffManifest = if ($env:XR_ASAN_DIFF_CASES_FILE) {
    [System.IO.Path]::GetFullPath($env:XR_ASAN_DIFF_CASES_FILE)
} else {
    Join-Path $repoRoot 'tests\diff\task190_mem_cases.txt'
}
$diffRunner = Join-Path $repoRoot 'tests\diff\run_backend_diff_fast.py'
if (-not (Test-Path -LiteralPath $diffManifest -PathType Leaf)) {
    throw "backend-diff manifest is missing: $diffManifest"
}
$pythonCommand = if ($env:XR_ASAN_PYTHON) {
    $env:XR_ASAN_PYTHON
} else {
    (Get-Command python -ErrorAction Stop).Source
}
$env:XRAY_DIFF_CASES_FILE = $diffManifest
$env:XRAY_DIFF_EXTRA_CASES_FILE = ''
$env:XRAY_DIFF_MAX_AUTO_JOBS = [string]$jobs
$env:XRAY_TEST_CACHE_ROOT = Join-Path $buildDir 'asan-focused-diff-cache'
Write-Host "== [asan_focused/windows] backend diff manifest: $diffManifest"
Invoke-Checked -Command $pythonCommand -Arguments @($diffRunner, $xrayExe)

$workDir = Join-Path $buildDir 'asan-focused-workloads'
New-Item -ItemType Directory -Force -Path $workDir | Out-Null
$xxhashMain = if ($env:XR_ASAN_XXHASH_MAIN) {
    [System.IO.Path]::GetFullPath($env:XR_ASAN_XXHASH_MAIN)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot '..\xray-ports\ports\xxhash\src\main.xr'))
}
if (Test-Path -LiteralPath $xxhashMain -PathType Leaf) {
    Write-Host "== [asan_focused/windows] AOT emit: $xxhashMain"
    Push-Location (Split-Path -Parent (Split-Path -Parent $xxhashMain))
    try {
        Invoke-Checked -Command $xrayExe -Arguments @(
            'build', '--native', '--c-only', '-o', (Join-Path $workDir 'xxhash.c'), $xxhashMain
        )
    } finally {
        Pop-Location
    }
} else {
    Write-Host "== [asan_focused/windows] xxhash workload not present; skipping"
}

$biliMain = if ($env:XR_ASAN_BILI_MAIN) {
    [System.IO.Path]::GetFullPath($env:XR_ASAN_BILI_MAIN)
} else {
    Join-Path $repoRoot 'tests\meta\fixtures\bili-analysis-server\src\main.xr'
}
if (-not (Test-Path -LiteralPath $biliMain -PathType Leaf)) {
    throw "required bili workload is missing: $biliMain"
}
Write-Host "== [asan_focused/windows] AOT emit: $biliMain"
Push-Location (Split-Path -Parent (Split-Path -Parent $biliMain))
try {
    Invoke-Checked -Command $xrayExe -Arguments @(
        'build', '--native', '--c-only', '-o', (Join-Path $workDir 'bili.c'), $biliMain
    )
} finally {
    Pop-Location
}

Write-Host '== [asan_focused/windows] PASS'
