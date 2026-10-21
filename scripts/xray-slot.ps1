#Requires -Version 7.3
param(
    [Parameter(Mandatory=$true)][ValidateSet('build','heavy')][string]$Kind,
    [ValidateSet('normal','merge')][string]$Priority = 'normal',
    [string]$SlotRoot = 'D:\xray-build\slots',
    [int]$TimeoutSeconds = 7200,
    [Parameter(Mandatory=$true)][string]$Program,
    [string[]]$ArgumentList = @()
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandArgumentPassing = 'Standard'
$PSNativeCommandUseErrorActionPreference = $false
& python (Join-Path $PSScriptRoot 'xray_slot.py') --kind $Kind --priority $Priority --root $SlotRoot --timeout $TimeoutSeconds -- $Program @ArgumentList
exit $LASTEXITCODE
