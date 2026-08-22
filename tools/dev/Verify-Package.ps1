[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [switch]$LaunchSmoke,
    [ValidateRange(1, 120)]
    [int]$SmokeSeconds = 5,
    [switch]$SkipToolBuild
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'VulkanLabDev.ps1')

$packageRoot = Get-VulkanLabFullPath $Path
if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "Package directory was not found: $packageRoot"
}

if (-not $SkipToolBuild) {
    $null = Invoke-VulkanLabConfigure 'dev' 'Debug'
    $null = Invoke-VulkanLabBuildTarget 'dev' 'VulkanLabAssetTool' 'Debug'
}
$dev = Get-VulkanLabProfile 'dev' 'Debug'
if (-not (Test-Path -LiteralPath $dev.AssetTool -PathType Leaf)) {
    throw "VulkanLabAssetTool.exe was not found: $($dev.AssetTool)"
}

Invoke-VulkanLabCommand $dev.AssetTool @(
    'package', 'verify', '--path', $packageRoot)

if (-not $LaunchSmoke) {
    Write-Host 'Package verification complete.'
    return
}

$smokeBase = [System.IO.Path]::GetFullPath((Join-Path (
            [System.IO.Path]::GetTempPath()) 'VulkanLab/PackageSmoke')).TrimEnd('\')
$smokeRoot = Join-Path $smokeBase ([System.Guid]::NewGuid().ToString('N'))
$process = $null
try {
    New-Item -ItemType Directory -Path $smokeRoot -Force | Out-Null
    Get-ChildItem -LiteralPath $packageRoot -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $smokeRoot -Recurse -Force
    }

    Invoke-VulkanLabCommand $dev.AssetTool @(
        'package', 'verify', '--path', $smokeRoot)

    $runtime = Join-Path $smokeRoot 'VulkanLab.exe'
    $null = Assert-VulkanLabMinimalRuntime $runtime
    $stdout = Join-Path $smokeRoot 'package-smoke.stdout.log'
    $stderr = Join-Path $smokeRoot 'package-smoke.stderr.log'
    $process = Start-Process -FilePath $runtime -ArgumentList @(
        '--automation',
        '--window-size', '640x480',
        '--no-gui',
        '--validation', 'off',
        '--asset-mode', 'cooked-only') -WorkingDirectory $smokeRoot `
        -WindowStyle Hidden -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -PassThru
    Start-Sleep -Seconds $SmokeSeconds
    if ($process.HasExited) {
        $standardOutput = if (Test-Path -LiteralPath $stdout) {
            Get-Content -LiteralPath $stdout -Raw
        } else { '' }
        $standardError = if (Test-Path -LiteralPath $stderr) {
            Get-Content -LiteralPath $stderr -Raw
        } else { '' }
        throw ("Repository-external package smoke exited early with code " +
            "$($process.ExitCode).`n$standardOutput`n$standardError")
    }
    Write-Host "Repository-external package smoke passed (${SmokeSeconds}s)."
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    }
    $resolvedSmoke = [System.IO.Path]::GetFullPath($smokeRoot).TrimEnd('\')
    $smokePrefix = $smokeBase + '\'
    if (-not $resolvedSmoke.StartsWith(
            $smokePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an unexpected smoke directory: $resolvedSmoke"
    }
    if (Test-Path -LiteralPath $resolvedSmoke) {
        Remove-Item -LiteralPath $resolvedSmoke -Recurse -Force
    }
}

