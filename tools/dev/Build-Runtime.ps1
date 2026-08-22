[CmdletBinding()]
param([switch]$Reconfigure)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VulkanLabDev.ps1')

$null = Ensure-VulkanLabConfigured 'runtime' 'Release' -Force:$Reconfigure
$profileInfo = Invoke-VulkanLabBuildPreset 'runtime' 'Release'
$buildInfo = Assert-VulkanLabMinimalRuntime $profileInfo.VulkanLab

Write-Host "Minimal runtime image ready: $($profileInfo.RuntimeDirectory)"
Write-Host "Revision: $($buildInfo.revision)"
