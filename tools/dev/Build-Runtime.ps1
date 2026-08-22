[CmdletBinding()]
param(
    [switch]$SkipConfigure
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VulkanLabDev.ps1')

if (-not $SkipConfigure) {
    $null = Invoke-VulkanLabConfigure 'runtime' 'Release'
}
$profileInfo = Invoke-VulkanLabBuildPreset 'runtime' 'Release'
$buildInfo = Assert-VulkanLabMinimalRuntime $profileInfo.VulkanLab

Write-Host "Minimal runtime image ready: $($profileInfo.RuntimeDirectory)"
Write-Host "Revision: $($buildInfo.revision)"

