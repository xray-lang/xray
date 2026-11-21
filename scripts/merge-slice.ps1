#Requires -Version 7.3
param(
    [Parameter(Mandatory=$true)][string]$Branch,
    [Parameter(Mandatory=$true)][string]$Review,
    [Parameter(Mandatory=$true)][string]$Docs
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandArgumentPassing = 'Standard'
$PSNativeCommandUseErrorActionPreference = $false
# The review points at frozen Task339 evidence. The Python queue verifies it before publication.
$queueArguments = @('--branch', $Branch, '--review', $Review, '--docs', $Docs)
& python (Join-Path $PSScriptRoot 'merge_slice.py') @queueArguments
exit $LASTEXITCODE
