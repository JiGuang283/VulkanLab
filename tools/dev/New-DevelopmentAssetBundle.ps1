[CmdletBinding()]
param(
    [string]$Project = (Join-Path $PSScriptRoot '../..'),
    [string]$OutputDirectory = 'dist/development-assets/v1',
    [string]$Version = 'v1',
    [ValidateRange(0, 1900)]
    [int]$PartSizeMiB = 0,
    [switch]$SceneClosureOnly,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Get-FullPath([string]$Path, [string]$BasePath) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Resolve-ProjectPath([string]$RelativePath) {
    if ([System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "Asset path must be project-relative: $RelativePath"
    }
    $resolved = Get-FullPath $RelativePath $projectRoot
    if (-not $resolved.StartsWith(
            $projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Asset path escapes the project root: $RelativePath"
    }
    return $resolved
}

function Get-ProjectRelativePath([string]$FullPath) {
    $resolved = [System.IO.Path]::GetFullPath($FullPath)
    if (-not $resolved.StartsWith(
            $projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Asset path is outside the project root: $resolved"
    }
    return $resolved.Substring($projectPrefix.Length).Replace('\', '/')
}

function Add-ClosureFile(
    [System.Collections.Generic.Dictionary[string, object]]$Files,
    [string]$FullPath,
    [string]$ModelId) {
    $resolved = [System.IO.Path]::GetFullPath($FullPath)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "Required model dependency is missing: $resolved"
    }
    $relative = Get-ProjectRelativePath $resolved
    if ($trackedPaths.Contains($relative)) {
        return
    }
    if ($Files.ContainsKey($relative)) {
        $existing = $Files[$relative]
        if ($existing.ModelIds -notcontains $ModelId) {
            $existing.ModelIds += $ModelId
        }
        return
    }
    $Files.Add($relative, [pscustomobject]@{
            FullPath = $resolved
            ModelIds = @($ModelId)
        })
}

function Add-GltfClosure(
    [System.Collections.Generic.Dictionary[string, object]]$Files,
    [string]$SourcePath,
    [string]$ModelId) {
    Add-ClosureFile $Files $SourcePath $ModelId
    if ([System.IO.Path]::GetExtension($SourcePath) -ine '.gltf') {
        return
    }

    $document = Get-Content -LiteralPath $SourcePath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $sourceDirectory = Split-Path -Parent $SourcePath
    $uris = @()
    if ($null -ne $document.buffers) {
        $uris += @($document.buffers | ForEach-Object { $_.uri })
    }
    if ($null -ne $document.images) {
        $uris += @($document.images | ForEach-Object { $_.uri })
    }
    foreach ($uriValue in $uris) {
        $uri = [string]$uriValue
        if ([string]::IsNullOrWhiteSpace($uri) -or
            $uri.StartsWith('data:', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if ([Uri]::IsWellFormedUriString($uri, [UriKind]::Absolute)) {
            throw "Remote model dependencies are not supported: $uri"
        }
        $decoded = [Uri]::UnescapeDataString($uri).Replace(
            '/', [System.IO.Path]::DirectorySeparatorChar)
        $dependency = [System.IO.Path]::GetFullPath(
            (Join-Path $sourceDirectory $decoded))
        if (-not $dependency.StartsWith(
                $projectPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Model dependency escapes the project root: $uri"
        }
        Add-ClosureFile $Files $dependency $ModelId
    }

    Get-ChildItem -LiteralPath $sourceDirectory -File |
        Where-Object {
            $_.Name -match '^(credits?|license|copying|notice|readme)'
        } |
        ForEach-Object { Add-ClosureFile $Files $_.FullName $ModelId }
}

function Remove-BundleFile([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        Remove-Item -LiteralPath $Path -Force
    }
}

$projectRoot = Get-FullPath $Project (Get-Location).Path
$projectPrefix = $projectRoot.TrimEnd('\') + '\'
$catalogPath = Join-Path $projectRoot 'assets/catalog.json'
if (-not (Test-Path -LiteralPath $catalogPath -PathType Leaf)) {
    throw "Project Catalog was not found: $catalogPath"
}
$bundleConfigPath = Join-Path $projectRoot 'assets/development_assets.json'
if (-not (Test-Path -LiteralPath $bundleConfigPath -PathType Leaf)) {
    throw "Development asset configuration was not found: $bundleConfigPath"
}
$outputRoot = Get-FullPath $OutputDirectory $projectRoot
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$bundleBaseName = "VulkanLab-development-assets-$Version"
$indexPath = Join-Path $outputRoot "$bundleBaseName.json"
$archiveName = "$bundleBaseName.tar.zst"
$existingOutputs = @(Get-ChildItem -LiteralPath $outputRoot -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "$bundleBaseName*" })
if ($existingOutputs.Count -gt 0 -and -not $Force) {
    throw "Bundle outputs already exist in '$outputRoot'; pass -Force to replace them."
}

$trackedPaths = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$gitFiles = & git.exe -C $projectRoot ls-files
if ($LASTEXITCODE -ne 0) {
    throw 'git ls-files failed while resolving development asset ownership.'
}
foreach ($gitFile in $gitFiles) {
    [void]$trackedPaths.Add(([string]$gitFile).Replace('\', '/'))
}

$catalog = Get-Content -LiteralPath $catalogPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
$bundleConfig = Get-Content -LiteralPath $bundleConfigPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
if ($bundleConfig.schemaVersion -ne 1 -or
    $bundleConfig.bundleId -ne 'vulkanlab-development-assets') {
    throw "Unsupported development asset configuration: $bundleConfigPath"
}
$modelById = @{}
foreach ($model in $catalog.models) {
    $modelById[[string]$model.id] = $model
}

$sceneIds = @()
$referencedModelIds = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($sceneEntry in $catalog.scenes) {
    $scenePath = Resolve-ProjectPath ([string]$sceneEntry.source)
    if (-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) {
        throw "Catalog SceneDocument is missing: $scenePath"
    }
    $scene = Get-Content -LiteralPath $scenePath -Raw -Encoding UTF8 |
        ConvertFrom-Json
    $sceneIds += [string]$scene.id
    foreach ($entity in $scene.entities) {
        $instance = $entity.components.modelInstance
        if ($null -eq $instance) {
            continue
        }
        $modelId = [string]$instance.model
        if ($modelId.StartsWith(
                'vkl-primitive-', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        [void]$referencedModelIds.Add($modelId)
    }
}

$closureFiles = [System.Collections.Generic.Dictionary[string, object]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$selectedModelIds = [System.Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($modelId in $referencedModelIds) {
    [void]$selectedModelIds.Add($modelId)
}
$unavailableOptionalModelIds = @()
if (-not $SceneClosureOnly) {
    foreach ($model in $catalog.models) {
        if ([string]$model.type -ine 'gltf') {
            continue
        }
        $modelId = [string]$model.id
        $sourcePath = Resolve-ProjectPath ([string]$model.source)
        if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
            [void]$selectedModelIds.Add($modelId)
        } elseif ([bool]$model.optional) {
            $unavailableOptionalModelIds += $modelId
        } else {
            throw "Required Catalog model source is missing: $sourcePath"
        }
    }
}

foreach ($modelId in @($selectedModelIds | Sort-Object)) {
    if (-not $modelById.ContainsKey($modelId)) {
        throw "Scene references an unknown Catalog model: $modelId"
    }
    $model = $modelById[$modelId]
    $sourcePath = Resolve-ProjectPath ([string]$model.source)
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "Selected Catalog model source is missing: $sourcePath"
    }
    Add-GltfClosure $closureFiles $sourcePath $modelId
}
if ($closureFiles.Count -eq 0) {
    throw 'No untracked SceneDocument model dependencies were found.'
}

$revision = (& git.exe -C $projectRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'git rev-parse HEAD failed.'
}
$payloadEntries = @()
foreach ($relativePath in @($closureFiles.Keys | Sort-Object)) {
    $record = $closureFiles[$relativePath]
    $file = Get-Item -LiteralPath $record.FullPath
    $payloadEntries += [ordered]@{
        path = $relativePath
        size = [uint64]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        modelIds = @($record.ModelIds | Sort-Object)
    }
}
$packagedModelIds = @(
    $payloadEntries.modelIds | ForEach-Object { $_ } | Sort-Object -Unique)
$attributions = @($bundleConfig.catalogModelAttributions |
        Where-Object { $packagedModelIds -contains [string]$_.modelId })
$payloadBytes = [uint64]0
foreach ($entry in $payloadEntries) {
    $payloadBytes += [uint64]$entry.size
}

$manifest = [ordered]@{
    schemaVersion = 1
    bundleId = 'vulkanlab-development-assets'
    version = $Version
    projectId = [string]$catalog.projectId
    sourceRevision = $revision
    sceneIds = @($sceneIds)
    referencedModelIds = @($referencedModelIds | Sort-Object)
    selectedModelIds = @($selectedModelIds | Sort-Object)
    packagedModelIds = $packagedModelIds
    unavailableOptionalModelIds = @($unavailableOptionalModelIds | Sort-Object)
    payloadBytes = $payloadBytes
    catalogModelAttributions = $attributions
    files = $payloadEntries
}

$stagingRoot = Join-Path (Join-Path $projectRoot 'build') (
    '.development-assets-' + [Guid]::NewGuid().ToString('N'))
$workingOutputRoot = Join-Path $outputRoot (
    '.' + $bundleBaseName + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
New-Item -ItemType Directory -Path $workingOutputRoot -Force | Out-Null
try {
    $workingArchivePath = Join-Path $workingOutputRoot $archiveName
    $workingIndexPath = Join-Path $workingOutputRoot "$bundleBaseName.json"
    foreach ($entry in $payloadEntries) {
        $source = Resolve-ProjectPath ([string]$entry.path)
        $destination = Join-Path $stagingRoot (
            ([string]$entry.path).Replace(
                '/', [System.IO.Path]::DirectorySeparatorChar))
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) `
            -Force | Out-Null
        try {
            New-Item -ItemType HardLink -Path $destination -Target $source |
                Out-Null
        } catch {
            Copy-Item -LiteralPath $source -Destination $destination
        }
    }
    $manifestPath = Join-Path $stagingRoot 'development-assets-manifest.json'
    $manifest | ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $manifestPath -Encoding UTF8
    $manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()

    & tar.exe -a -cf $workingArchivePath -C $stagingRoot .
    if ($LASTEXITCODE -ne 0) {
        throw "tar failed to create development asset archive (exit $LASTEXITCODE)."
    }

    $archiveFile = Get-Item -LiteralPath $workingArchivePath
    $archiveHash = (Get-FileHash -LiteralPath $workingArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $partSize = [int64]$PartSizeMiB * 1MB
    $parts = @()
    if ($PartSizeMiB -eq 0 -or $archiveFile.Length -le $partSize) {
        $parts += [ordered]@{
            fileName = $archiveFile.Name
            size = [uint64]$archiveFile.Length
            sha256 = $archiveHash
        }
    } else {
        $input = [System.IO.File]::OpenRead($workingArchivePath)
        try {
            $buffer = New-Object byte[] (4MB)
            $partIndex = 1
            while ($input.Position -lt $input.Length) {
                $partName = '{0}.part{1:D3}' -f $archiveName, $partIndex
                $partPath = Join-Path $workingOutputRoot $partName
                $output = [System.IO.File]::Create($partPath)
                try {
                    $remaining = [Math]::Min($partSize, $input.Length - $input.Position)
                    while ($remaining -gt 0) {
                        $request = [int][Math]::Min($buffer.Length, $remaining)
                        $read = $input.Read($buffer, 0, $request)
                        if ($read -le 0) {
                            throw 'Unexpected end of archive while creating bundle parts.'
                        }
                        $output.Write($buffer, 0, $read)
                        $remaining -= $read
                    }
                } finally {
                    $output.Dispose()
                }
                $partFile = Get-Item -LiteralPath $partPath
                $parts += [ordered]@{
                    fileName = $partName
                    size = [uint64]$partFile.Length
                    sha256 = (Get-FileHash -LiteralPath $partPath -Algorithm SHA256).Hash.ToLowerInvariant()
                }
                $partIndex++
            }
        } finally {
            $input.Dispose()
        }
        Remove-BundleFile $workingArchivePath
    }

    $index = [ordered]@{
        schemaVersion = 1
        bundleId = 'vulkanlab-development-assets'
        version = $Version
        projectId = [string]$catalog.projectId
        sourceRevision = $revision
        sceneCount = [uint32]$sceneIds.Count
        packagedModelIds = $packagedModelIds
        fileCount = [uint32]$payloadEntries.Count
        payloadBytes = $payloadBytes
        archive = [ordered]@{
            fileName = $archiveName
            format = 'tar.zst'
            size = [uint64]$archiveFile.Length
            sha256 = $archiveHash
        }
        manifestSha256 = $manifestHash
        parts = $parts
    }
    $temporaryIndex = "$workingIndexPath.tmp"
    $index | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $temporaryIndex -Encoding UTF8
    Move-Item -LiteralPath $temporaryIndex -Destination $workingIndexPath -Force

    foreach ($existing in $existingOutputs) {
        Remove-BundleFile $existing.FullName
    }
    foreach ($outputFile in Get-ChildItem -LiteralPath $workingOutputRoot -File |
            Where-Object { $_.Name -ne "$bundleBaseName.json" }) {
        Move-Item -LiteralPath $outputFile.FullName -Destination $outputRoot -Force
    }
    Move-Item -LiteralPath $workingIndexPath -Destination $outputRoot -Force

    Write-Host "Development asset bundle ready: $outputRoot"
    Write-Host "Payload: $($payloadEntries.Count) files, $([math]::Round($archiveFile.Length / 1GB, 2)) GiB archive"
    Write-Host "Parts: $($parts.Count)"
    Write-Host "Index: $indexPath"
} finally {
    $resolvedStaging = [System.IO.Path]::GetFullPath($stagingRoot)
    $buildPrefix = [System.IO.Path]::GetFullPath(
        (Join-Path $projectRoot 'build')).TrimEnd('\') + '\'
    if ($resolvedStaging.StartsWith(
            $buildPrefix, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedStaging)) {
        Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
    }
    $resolvedWorkingOutput = [System.IO.Path]::GetFullPath($workingOutputRoot)
    $outputPrefix = $outputRoot.TrimEnd('\') + '\'
    if ($resolvedWorkingOutput.StartsWith(
            $outputPrefix, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedWorkingOutput)) {
        Remove-Item -LiteralPath $resolvedWorkingOutput -Recurse -Force
    }
}
