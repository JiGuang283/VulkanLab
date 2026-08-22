[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$IndexPath,
    [string]$Project = (Join-Path $PSScriptRoot '../..'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Get-FullPath([string]$Path, [string]$BasePath) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Assert-FileHash([string]$Path, [uint64]$Size, [string]$Sha256) {
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ([uint64]$file.Length -ne $Size) {
        throw "File size mismatch: $Path"
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ine $Sha256) {
        throw "File SHA-256 mismatch: $Path"
    }
}

function Resolve-InstallPath([string]$RelativePath) {
    if ([System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "Bundle path must be project-relative: $RelativePath"
    }
    $resolved = Get-FullPath $RelativePath $projectRoot
    if (-not $resolved.StartsWith(
            $projectPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Bundle path escapes the project root: $RelativePath"
    }
    return $resolved
}

$projectRoot = Get-FullPath $Project (Get-Location).Path
$projectPrefix = $projectRoot.TrimEnd('\') + '\'
$resolvedIndex = Get-FullPath $IndexPath (Get-Location).Path
$indexDirectory = Split-Path -Parent $resolvedIndex
$index = Get-Content -LiteralPath $resolvedIndex -Raw -Encoding UTF8 |
    ConvertFrom-Json
if ($index.schemaVersion -ne 1 -or
    $index.bundleId -ne 'vulkanlab-development-assets') {
    throw "Unsupported development asset index: $resolvedIndex"
}
$catalogPath = Join-Path $projectRoot 'assets/catalog.json'
$catalog = Get-Content -LiteralPath $catalogPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
if ([string]$catalog.projectId -ne [string]$index.projectId) {
    throw "Bundle project '$($index.projectId)' does not match '$($catalog.projectId)'."
}
$currentRevision = (& git.exe -C $projectRoot rev-parse HEAD 2>$null).Trim()
if ($LASTEXITCODE -eq 0 -and $currentRevision -ne [string]$index.sourceRevision) {
    Write-Warning "Bundle was generated from revision $($index.sourceRevision); current checkout is $currentRevision."
}

$parts = @($index.parts)
if ($parts.Count -eq 0) {
    throw 'Development asset index contains no archive parts.'
}
foreach ($part in $parts) {
    $partPath = Join-Path $indexDirectory ([string]$part.fileName)
    Assert-FileHash $partPath ([uint64]$part.size) ([string]$part.sha256)
}

$stagingRoot = Join-Path (Join-Path $projectRoot 'build') (
    '.development-assets-install-' + [Guid]::NewGuid().ToString('N'))
$archivePath = Join-Path $stagingRoot ([string]$index.archive.fileName)
$extractRoot = Join-Path $stagingRoot 'payload'
New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
try {
    $archiveOutput = [System.IO.File]::Create($archivePath)
    try {
        $buffer = New-Object byte[] (4MB)
        foreach ($part in $parts) {
            $partPath = Join-Path $indexDirectory ([string]$part.fileName)
            $partInput = [System.IO.File]::OpenRead($partPath)
            try {
                while (($read = $partInput.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $archiveOutput.Write($buffer, 0, $read)
                }
            } finally {
                $partInput.Dispose()
            }
        }
    } finally {
        $archiveOutput.Dispose()
    }
    Assert-FileHash $archivePath ([uint64]$index.archive.size) `
        ([string]$index.archive.sha256)

    $archiveEntries = @(& tar.exe -tf $archivePath)
    if ($LASTEXITCODE -ne 0) {
        throw "tar failed to list development assets (exit $LASTEXITCODE)."
    }
    foreach ($archiveEntry in $archiveEntries) {
        $normalized = ([string]$archiveEntry).Replace('\', '/').Trim()
        while ($normalized.StartsWith('./')) {
            $normalized = $normalized.Substring(2)
        }
        if ([string]::IsNullOrWhiteSpace($normalized)) {
            continue
        }
        $segments = @($normalized.Split('/') |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        if ($normalized.StartsWith('/') -or $normalized -match '^[A-Za-z]:' -or
            $segments -contains '..') {
            throw "Archive contains an unsafe path: $archiveEntry"
        }
    }

    & tar.exe -xf $archivePath -C $extractRoot
    if ($LASTEXITCODE -ne 0) {
        throw "tar failed to extract development assets (exit $LASTEXITCODE)."
    }
    $manifestPath = Join-Path $extractRoot 'development-assets-manifest.json'
    Assert-FileHash $manifestPath (Get-Item $manifestPath).Length `
        ([string]$index.manifestSha256)
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    if ($manifest.schemaVersion -ne 1 -or
        $manifest.bundleId -ne 'vulkanlab-development-assets' -or
        [string]$manifest.projectId -ne [string]$catalog.projectId) {
        throw 'Extracted development asset manifest is incompatible.'
    }

    $payloadFiles = @($manifest.files)
    $expectedPaths = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $payloadFiles) {
        $relative = ([string]$entry.path).Replace('\', '/')
        if (-not $expectedPaths.Add($relative)) {
            throw "Duplicate development asset path: $relative"
        }
        $sourcePath = Get-FullPath $relative $extractRoot
        $extractPrefix = $extractRoot.TrimEnd('\') + '\'
        if (-not $sourcePath.StartsWith(
                $extractPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Extracted path escapes staging: $relative"
        }
        Assert-FileHash $sourcePath ([uint64]$entry.size) ([string]$entry.sha256)
    }
    $actualFiles = @(Get-ChildItem -LiteralPath $extractRoot -Recurse -File |
            Where-Object { $_.FullName -ne $manifestPath })
    if ($actualFiles.Count -ne $expectedPaths.Count) {
        throw 'Archive contains files that are not declared by its manifest.'
    }
    foreach ($actual in $actualFiles) {
        $relative = $actual.FullName.Substring(
            $extractRoot.TrimEnd('\').Length + 1).Replace('\', '/')
        if (-not $expectedPaths.Contains($relative)) {
            throw "Archive contains an undeclared file: $relative"
        }
    }

    foreach ($entry in $payloadFiles) {
        $targetPath = Resolve-InstallPath ([string]$entry.path)
        if (Test-Path -LiteralPath $targetPath -PathType Leaf) {
            $target = Get-Item -LiteralPath $targetPath
            $sameSize = [uint64]$target.Length -eq [uint64]$entry.size
            $sameHash = $false
            if ($sameSize) {
                $sameHash = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ieq
                    [string]$entry.sha256
            }
            if ($sameHash) {
                continue
            }
            if (-not $Force) {
                throw "Refusing to overwrite a different local asset: $targetPath"
            }
        }
    }

    $installed = 0
    $unchanged = 0
    foreach ($entry in $payloadFiles) {
        $relative = ([string]$entry.path).Replace(
            '/', [System.IO.Path]::DirectorySeparatorChar)
        $sourcePath = Join-Path $extractRoot $relative
        $targetPath = Resolve-InstallPath ([string]$entry.path)
        if (Test-Path -LiteralPath $targetPath -PathType Leaf) {
            $target = Get-Item -LiteralPath $targetPath
            if ([uint64]$target.Length -eq [uint64]$entry.size -and
                (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ieq
                    [string]$entry.sha256) {
                $unchanged++
                continue
            }
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) `
            -Force | Out-Null
        $temporaryTarget = "$targetPath.vkl-install-$([Guid]::NewGuid().ToString('N'))"
        Copy-Item -LiteralPath $sourcePath -Destination $temporaryTarget
        Move-Item -LiteralPath $temporaryTarget -Destination $targetPath -Force
        $installed++
    }

    Write-Host "Development assets installed into: $projectRoot"
    Write-Host "Installed: $installed; already current: $unchanged"
} finally {
    $resolvedStaging = [System.IO.Path]::GetFullPath($stagingRoot)
    $buildPrefix = [System.IO.Path]::GetFullPath(
        (Join-Path $projectRoot 'build')).TrimEnd('\') + '\'
    if ($resolvedStaging.StartsWith(
            $buildPrefix, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedStaging)) {
        Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
    }
}
