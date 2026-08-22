[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$PackageName,
    [string]$OutputDirectory,
    [string[]]$SceneId = @(),
    [string]$StartupScene,
    [string]$Project,
    [string]$CacheRoot,
    [switch]$BuildMissing,
    [ValidateRange(1, 256)]
    [int]$Workers,
    [ValidateRange(128, 1048576)]
    [int]$MemoryBudgetMiB = 2048,
    [string]$KtxTool,
    [string]$GltfValidator,
    [switch]$LaunchSmoke
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VulkanLabDev.ps1')

$repositoryRoot = Get-VulkanLabRepositoryRoot
if ([string]::IsNullOrWhiteSpace($Project)) {
    $Project = $repositoryRoot
}
$projectRoot = Get-VulkanLabFullPath $Project
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot ('dist/' + $PackageName)
}
$packageRoot = Get-VulkanLabFullPath $OutputDirectory

$null = Invoke-VulkanLabConfigure 'dev' 'Debug'
$null = Invoke-VulkanLabBuildTarget 'dev' 'VulkanLabAssetTool' 'Debug'
$null = Invoke-VulkanLabConfigure 'runtime' 'Release'
$runtimeProfile = Invoke-VulkanLabBuildPreset 'runtime' 'Release'
$null = Assert-VulkanLabMinimalRuntime $runtimeProfile.VulkanLab

$dev = Get-VulkanLabProfile 'dev' 'Debug'
if (-not (Test-Path -LiteralPath $dev.AssetTool -PathType Leaf)) {
    throw "VulkanLabAssetTool.exe was not found: $($dev.AssetTool)"
}

$cookArguments = @(
    'cook',
    '--platform', 'windows-x64',
    '--project', $projectRoot,
    '--runtime-dir', $runtimeProfile.RuntimeDirectory,
    '--output', $packageRoot,
    '--memory-budget-mib', $MemoryBudgetMiB.ToString())
foreach ($id in $SceneId) {
    if (-not [string]::IsNullOrWhiteSpace($id)) {
        $cookArguments += @('--scene-id', $id)
    }
}
if (-not [string]::IsNullOrWhiteSpace($StartupScene)) {
    $cookArguments += @('--startup-scene', $StartupScene)
}
if (-not [string]::IsNullOrWhiteSpace($CacheRoot)) {
    $cookArguments += @('--cache-root', (Get-VulkanLabFullPath $CacheRoot))
}
if ($BuildMissing) {
    $cookArguments += '--build-missing'
}
if ($PSBoundParameters.ContainsKey('Workers')) {
    $cookArguments += @('--workers', $Workers.ToString())
}
if (-not [string]::IsNullOrWhiteSpace($KtxTool)) {
    $cookArguments += @('--ktx-tool', (Get-VulkanLabFullPath $KtxTool))
}
if (-not [string]::IsNullOrWhiteSpace($GltfValidator)) {
    $cookArguments += @(
        '--gltf-validator', (Get-VulkanLabFullPath $GltfValidator))
}

Invoke-VulkanLabCommand $dev.AssetTool $cookArguments

$verifyScript = Join-Path $PSScriptRoot 'Verify-Package.ps1'
$verifyArguments = @{
    Path = $packageRoot
    SkipToolBuild = $true
}
if ($LaunchSmoke) {
    $verifyArguments.LaunchSmoke = $true
}
& $verifyScript @verifyArguments

Write-Host "Cooked package ready: $packageRoot"

