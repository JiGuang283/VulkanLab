[CmdletBinding(SupportsShouldProcess)]
param(
    [string[]]$KeepPreset = @(
        'dev',
        'ninja-dev',
        'full',
        'runtime',
        'tracy',
        'cacao'
    ),
    [switch]$IncludePackages
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../..')).TrimEnd('\')
$buildRoot = Join-Path $repositoryRoot 'build'

function Remove-RepositoryItem([string]$Path) {
    $resolved = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $prefix = $repositoryRoot + '\'
    if (-not $resolved.StartsWith(
            $prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an item outside the repository: $resolved"
    }
    if (-not (Test-Path -LiteralPath $resolved)) {
        return
    }
    if ($PSCmdlet.ShouldProcess($resolved, 'Remove generated output')) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

# Pre-workspace-layout outputs formerly written into the source root.
foreach ($relativePath in @(
        'logs',
        'artifacts',
        'derived_assets',
        'validation',
        'imgui.ini',
        'out/smoke')) {
    Remove-RepositoryItem (Join-Path $repositoryRoot $relativePath)
}

if ($IncludePackages) {
    Remove-RepositoryItem (Join-Path $repositoryRoot 'dist')
}

if (Test-Path -LiteralPath $buildRoot) {
    Get-ChildItem -LiteralPath $buildRoot -Force | ForEach-Object {
        if ($KeepPreset -notcontains $_.Name) {
            Remove-RepositoryItem $_.FullName
        }
    }
}

foreach ($presetName in $KeepPreset) {
    $presetRoot = Join-Path $buildRoot $presetName
    if (-not (Test-Path -LiteralPath $presetRoot)) {
        continue
    }

    foreach ($legacyPath in @(
            'Debug',
            'Release',
            'logs',
            'imgui.ini',
            'cluster-stage6-project',
            'cluster-stage6-project2',
            'cluster-stage6-metrics.json')) {
        Remove-RepositoryItem (Join-Path $presetRoot $legacyPath)
    }

    Get-ChildItem -LiteralPath $presetRoot -File -Force |
        Where-Object {
            $_.Name -match
                '^(stage|rendergraph|runtime-smoke).*(\.log|\.out|\.err|\.json)$'
        } |
        ForEach-Object { Remove-RepositoryItem $_.FullName }
}

if ($WhatIfPreference) {
    Write-Host 'Cleanup preview complete.'
} else {
    Write-Host 'Local generated outputs cleaned.'
}
