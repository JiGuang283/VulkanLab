[CmdletBinding()]
param(
    [ValidateSet('ninja-dev', 'dev', 'full', 'runtime', 'tracy', 'cacao')]
    [string]$Profile = 'ninja-dev',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VulkanLabDev.ps1')

$profileInfo = Invoke-VulkanLabConfigure $Profile $Configuration
Write-Host "Configured '$Profile' at $($profileInfo.BinaryDirectory)"
