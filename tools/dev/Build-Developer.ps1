[CmdletBinding()]
param(
    [ValidateSet('ninja-dev', 'dev', 'full', 'tracy', 'cacao')]
    [string]$Profile = 'ninja-dev',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VulkanLabDev.ps1')

$null = Ensure-VulkanLabConfigured $Profile $Configuration -Force:$Reconfigure
$profileInfo = Invoke-VulkanLabBuildPreset $Profile $Configuration

if (-not (Test-Path -LiteralPath $profileInfo.VulkanLab -PathType Leaf)) {
    throw "Developer build did not publish VulkanLab.exe: $($profileInfo.VulkanLab)"
}
Write-Host "Developer image ready: $($profileInfo.RuntimeDirectory)"
Write-Host "Run: $($profileInfo.VulkanLab) --project $(Get-VulkanLabRepositoryRoot)"
