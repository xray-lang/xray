<#
.SYNOPSIS
    One-shot Parallels Win11 VM provisioning for xray build/test work.

.DESCRIPTION
    Installs and configures everything a Windows runner needs to act as
    a remote build/test target for the xray codebase on macOS:

      - OpenSSH Server (autostart + firewall rule + key-only auth)
      - Public key installed into BOTH user authorized_keys and the
        administrators_authorized_keys file (Win11 sshd routes admin
        users through the latter -- this is the #1 reason key auth
        silently fails on Windows)
      - winget packages: VS 2022 Build Tools (x64 cross tools),
        CMake, Ninja, Git, Python, vcpkg
      - C:\workspace\xray and C:\workspace\xray-build directories
      - Windows Defender real-time scan exclusion for C:\workspace
      - Prints VM IPv4 address(es) for the macOS-side ssh_config

    Run this ONCE inside an elevated PowerShell session in the VM.

.PARAMETER PubKey
    Your macOS public key (~/.ssh/id_ed25519.pub contents).
    Pass it inline so the script stays user-agnostic.

.PARAMETER SkipBuildTools
    Skip MSVC Build Tools install (multi-GB, slow). Useful when
    iterating on the script itself.

.EXAMPLE
    # On macOS, copy your pubkey:
    #   pbcopy < ~/.ssh/id_ed25519.pub
    # In the VM (elevated PowerShell), paste the key when prompted:
    .\win_vm_provision.ps1 -PubKey 'ssh-ed25519 AAAA... user@host'

.NOTES
    Idempotent: re-running is safe. Components already installed are
    skipped.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$PubKey,

    [switch]$SkipBuildTools
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # winget progress bars break SSH/CI logs

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    [ok] $msg" -ForegroundColor Green }
function Write-Skip($msg) { Write-Host "    [skip] $msg" -ForegroundColor DarkGray }
function Write-Warn2($msg){ Write-Host "    [warn] $msg" -ForegroundColor Yellow }

# ---- Sanity: must run elevated ------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent() `
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error 'This script must be run from an elevated PowerShell.'
    exit 1
}

# ---- Sanity: validate pubkey format -------------------------------------
$PubKey = $PubKey.Trim()
if ($PubKey -notmatch '^(ssh-ed25519|ssh-rsa|ecdsa-sha2-\S+)\s+\S+') {
    Write-Error "PubKey does not look like an SSH public key: $PubKey"
    exit 1
}

# =========================================================================
# 1. OpenSSH Server
# =========================================================================
# Two install paths converge here:
#   a) Windows Capability (Add-WindowsCapability -Online ...)
#   b) Manual install of the Win32-OpenSSH zip from PowerShell/Win32-OpenSSH
# On ARM Win11 the capability path can hang for an hour on
# DISM/TrustedInstaller, so we treat "sshd service exists" as the
# definitive signal that OpenSSH is installed -- regardless of which
# path was used -- and skip the capability call in that case.
Write-Step 'Install OpenSSH Server'
$sshdSvc = Get-Service sshd -ErrorAction SilentlyContinue
if ($sshdSvc) {
    Write-Skip "sshd service already present (state=$($sshdSvc.Status)) -- skipping capability install"
} else {
    $sshFeature = Get-WindowsCapability -Online -Name 'OpenSSH.Server*'
    if ($sshFeature.State -ne 'Installed') {
        Write-Host '    invoking Add-WindowsCapability (this can take several minutes)...' -ForegroundColor DarkGray
        Add-WindowsCapability -Online -Name $sshFeature.Name | Out-Null
        Write-Ok "Installed: $($sshFeature.Name)"
    } else {
        Write-Skip 'OpenSSH.Server capability already installed'
    }
}

Write-Step 'Configure sshd service (autostart + running)'
Set-Service -Name sshd -StartupType Automatic
if ((Get-Service sshd).Status -ne 'Running') { Start-Service sshd }
Write-Ok 'sshd is running and set to autostart'

Write-Step 'Set active networks to Private (Public profile blocks inbound)'
# Win11 defaults Parallels Shared and most "unidentified" networks to
# the Public profile. Public profile inbound is effectively closed
# unless explicitly overridden, even with a matching Allow rule. For
# a build/test runner sitting behind a hypervisor NAT, treating those
# networks as Private is the safe and standard fix.
Get-NetConnectionProfile | Where-Object NetworkCategory -ne 'Private' | ForEach-Object {
    Set-NetConnectionProfile -InterfaceIndex $_.InterfaceIndex -NetworkCategory Private
    Write-Ok "Network '$($_.Name)' (iface $($_.InterfaceIndex)) -> Private"
}

Write-Step 'Firewall rule for port 22'
# Two rules can exist here: our own "OpenSSH-Server-In-TCP" plus a
# stock one OpenSSH capability installs as DisplayName "OpenSSH SSH
# Server (sshd)". The stock rule is pinned to Profile=Private,
# which silently blocks SSH on networks Win11 categorizes as Public
# (Parallels Shared often is). Force every matching rule to
# Profile=Any to be robust against either install path.
$existingByName = Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue
if (-not $existingByName) {
    New-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' `
        -DisplayName 'OpenSSH Server (sshd)' `
        -Enabled True -Direction Inbound -Protocol TCP `
        -Action Allow -LocalPort 22 -Profile Any | Out-Null
    Write-Ok 'Created firewall rule for tcp/22 (Profile=Any)'
} else {
    Write-Skip 'OpenSSH-Server-In-TCP rule already exists'
}
# Also widen any stock sshd rule installed by the OpenSSH capability.
Get-NetFirewallRule -DisplayName '*OpenSSH*' -ErrorAction SilentlyContinue |
    Where-Object { $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' -and $_.Profile -ne 'Any' } |
    ForEach-Object {
        Set-NetFirewallRule -Name $_.Name -Profile Any -Enabled True
        Write-Ok "Widened firewall rule to Profile=Any: $($_.DisplayName)"
    }

# =========================================================================
# 2. Authorized keys (BOTH locations)
# =========================================================================
Write-Step 'Install pubkey into user authorized_keys'
$userSshDir = Join-Path $env:USERPROFILE '.ssh'
if (-not (Test-Path $userSshDir)) {
    New-Item -ItemType Directory -Path $userSshDir | Out-Null
}
$userAuth = Join-Path $userSshDir 'authorized_keys'
if (Test-Path $userAuth) {
    $existing = Get-Content $userAuth -Raw -ErrorAction SilentlyContinue
    if ($existing -and $existing.Contains($PubKey)) {
        Write-Skip 'pubkey already present in user authorized_keys'
    } else {
        Add-Content -Path $userAuth -Value $PubKey -Encoding ascii
        Write-Ok "Appended pubkey to $userAuth"
    }
} else {
    Set-Content -Path $userAuth -Value $PubKey -Encoding ascii
    Write-Ok "Created $userAuth with pubkey"
}
# Tight ACL on the user .ssh dir (sshd refuses keys if too permissive)
icacls $userSshDir /inheritance:r | Out-Null
icacls $userSshDir /grant:r "$($env:USERNAME):(F)" | Out-Null
icacls $userAuth /inheritance:r | Out-Null
icacls $userAuth /grant:r "$($env:USERNAME):(F)" | Out-Null

Write-Step 'Install pubkey into administrators_authorized_keys (Win11 admin path)'
# IMPORTANT: Win11 sshd's default Match Group administrators rule
# directs admin users to this single shared file, NOT their user
# authorized_keys. Skipping this is the #1 reason "I added my key
# and key auth still asks for a password" happens.
$adminAuth = 'C:\ProgramData\ssh\administrators_authorized_keys'
if (Test-Path $adminAuth) {
    $existing = Get-Content $adminAuth -Raw -ErrorAction SilentlyContinue
    if ($existing -and $existing.Contains($PubKey)) {
        Write-Skip 'pubkey already present in administrators_authorized_keys'
    } else {
        Add-Content -Path $adminAuth -Value $PubKey -Encoding ascii
        Write-Ok "Appended pubkey to $adminAuth"
    }
} else {
    Set-Content -Path $adminAuth -Value $PubKey -Encoding ascii
    Write-Ok "Created $adminAuth with pubkey"
}
icacls $adminAuth /inheritance:r | Out-Null
icacls $adminAuth /grant 'Administrators:F' 'SYSTEM:F' | Out-Null
Write-Ok 'administrators_authorized_keys ACL: Administrators+SYSTEM only'

Restart-Service sshd
Write-Ok 'sshd restarted (picks up new authorized_keys)'

# =========================================================================
# 3. winget bootstrap
# =========================================================================
Write-Step 'Verify winget is available'
$winget = Get-Command winget -ErrorAction SilentlyContinue
if (-not $winget) {
    Write-Error 'winget not found. Open Microsoft Store, search "App Installer", install it, then re-run this script.'
    exit 1
}
Write-Ok "winget: $(winget --version)"

function Install-WingetPkg($id, $extra = @()) {
    $installed = winget list --id $id --exact 2>$null | Select-String -Pattern $id -Quiet
    if ($installed) {
        Write-Skip "$id already installed"
        return
    }
    Write-Host "    installing $id ..." -ForegroundColor DarkGray
    $args = @('install','--id',$id,'--exact','--accept-source-agreements','--accept-package-agreements','--silent') + $extra
    & winget @args | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warn2 "winget install $id returned $LASTEXITCODE (continuing)" }
    else { Write-Ok "$id installed" }
}

# =========================================================================
# 4. Toolchain packages
# =========================================================================
Write-Step 'CMake, Ninja, Git, Python, vcpkg'
Install-WingetPkg 'Kitware.CMake'
Install-WingetPkg 'Ninja-build.Ninja'
Install-WingetPkg 'Git.Git'
Install-WingetPkg 'Python.Python.3.12'
Install-WingetPkg 'Microsoft.Vcpkg'

if ($SkipBuildTools) {
    Write-Skip 'MSVC Build Tools (per -SkipBuildTools) -- install separately via vs_BuildTools.exe GUI for visible progress'
} else {
    Write-Step 'MSVC Build Tools 2022 (this can take 10-15 min)'
    # We pass the workload + components via --override so winget hands
    # them straight to the VS installer. ARM64 host needs x86.x64 cross
    # tools to emit real x64 binaries. installPath is left at the
    # default ("C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools")
    # so win_build.sh's hard-coded vcvars path stays valid.
    $vsArgs = @(
        '--override',
        '--quiet --wait --norestart --nocache ' +
        '--add Microsoft.VisualStudio.Workload.VCTools ' +
        '--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ' +
        '--add Microsoft.VisualStudio.Component.VC.Tools.ARM64 ' +
        '--add Microsoft.VisualStudio.Component.Windows11SDK.22621'
    )
    Install-WingetPkg 'Microsoft.VisualStudio.2022.BuildTools' $vsArgs
}

# =========================================================================
# 5. Workspace dirs + Defender exclusion
# =========================================================================
Write-Step 'Workspace directories'
foreach ($dir in @('C:\workspace', 'C:\workspace\xray', 'C:\workspace\xray-build')) {
    if (Test-Path $dir) { Write-Skip "$dir exists" }
    else { New-Item -ItemType Directory -Path $dir | Out-Null; Write-Ok "Created $dir" }
}

Write-Step 'Defender real-time exclusion for C:\workspace'
try {
    Add-MpPreference -ExclusionPath 'C:\workspace' -ErrorAction Stop
    Write-Ok 'C:\workspace excluded from Defender real-time scanning'
} catch {
    Write-Warn2 "Could not add Defender exclusion: $($_.Exception.Message)"
}

# =========================================================================
# 6. Report VM IPs for ~/.ssh/config on macOS
# =========================================================================
Write-Step 'VM IPv4 addresses (use one of these in ~/.ssh/config HostName)'
$ips = Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' } |
    Select-Object IPAddress, InterfaceAlias
$ips | Format-Table -AutoSize
Write-Host ''
Write-Host '------------------------------------------------------------' -ForegroundColor Yellow
Write-Host 'Provisioning complete. Next steps on macOS:' -ForegroundColor Yellow
Write-Host '------------------------------------------------------------' -ForegroundColor Yellow
Write-Host @"
1. Add to ~/.ssh/config (replace <ip> with one of the IPs above):

       Host xray-win
           HostName <ip>
           User $($env:USERNAME)
           IdentityFile ~/.ssh/id_ed25519
           ServerAliveInterval 30

2. Test from macOS:

       ssh xray-win 'cmd /c ver'

3. First sync + build:

       ./scripts/win_build.sh
"@
