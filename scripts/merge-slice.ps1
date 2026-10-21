#Requires -Version 7.3
param(
    [Parameter(Mandatory=$true)][string]$Branch,
    [Parameter(Mandatory=$true)][string]$Review,
    [string]$AsanUnavailable = ''
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandArgumentPassing = 'Standard'
$PSNativeCommandUseErrorActionPreference = $false
$queueArguments = @('--branch', $Branch, '--review', $Review)
if ($AsanUnavailable) { $queueArguments += @('--asan-unavailable', $AsanUnavailable) }
& python (Join-Path $PSScriptRoot 'merge_slice.py') @queueArguments
exit $LASTEXITCODE
