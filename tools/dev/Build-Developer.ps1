[CmdletBinding()]
param(
    [ValidateSet('dev', 'full', 'tracy', 'cacao')]
    [string]$Profile = 'dev',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$SkipConfigure
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VulkanLabDev.ps1')

if (-not $SkipConfigure) {
    $null = Invoke-VulkanLabConfigure $Profile $Configuration
}
$profileInfo = Invoke-VulkanLabBuildPreset $Profile $Configuration

if (-not (Test-Path -LiteralPath $profileInfo.VulkanLab -PathType Leaf)) {
    throw "Developer build did not publish VulkanLab.exe: $($profileInfo.VulkanLab)"
}
Write-Host "Developer image ready: $($profileInfo.RuntimeDirectory)"
Write-Host "Run: $($profileInfo.VulkanLab) --project $(Get-VulkanLabRepositoryRoot)"

