[CmdletBinding()]
param(
    [ValidateSet('dev', 'full', 'runtime', 'tracy', 'cacao')]
    [string]$Profile = 'dev',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VulkanLabDev.ps1')

$profileInfo = Invoke-VulkanLabConfigure $Profile $Configuration
Write-Host "Configured '$Profile' at $($profileInfo.BinaryDirectory)"

