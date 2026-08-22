Set-StrictMode -Version 2.0

$script:VulkanLabRepositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../..')).TrimEnd('\')

function Get-VulkanLabRepositoryRoot {
    return $script:VulkanLabRepositoryRoot
}

function Get-VulkanLabFullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [string]$BasePath = $script:VulkanLabRepositoryRoot
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Format-VulkanLabCommandArgument {
    param([string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-VulkanLabCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $script:VulkanLabRepositoryRoot
    )

    $displayArguments = @($Arguments | ForEach-Object {
            Format-VulkanLabCommandArgument $_
        })
    Write-Host ('> ' + $FilePath +
        $(if ($displayArguments.Count -gt 0) {
                ' ' + ($displayArguments -join ' ')
            } else {
                ''
            }))

    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments | Out-Host
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($exitCode -ne 0) {
        throw "Command failed with exit code ${exitCode}: $FilePath"
    }
}

function Get-VulkanLabProfile {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    switch ($Profile) {
        'dev' {
            if ($Configuration -ne 'Debug') {
                throw 'The dev profile only defines a Debug build.'
            }
            $configurePreset = 'windows-msvc-dev-fast'
            $buildPreset = 'windows-msvc-dev-fast'
            $directory = 'dev'
        }
        'full' {
            $configurePreset = 'windows-msvc-full'
            $buildPreset = if ($Configuration -eq 'Release') {
                'windows-msvc-full-release'
            } else {
                'windows-msvc-full-debug'
            }
            $directory = 'full'
        }
        'runtime' {
            if ($Configuration -ne 'Release') {
                throw 'The runtime profile only defines a Release build.'
            }
            $configurePreset = 'windows-msvc-runtime'
            $buildPreset = 'windows-msvc-runtime'
            $directory = 'runtime'
        }
        'tracy' {
            if ($Configuration -ne 'Debug') {
                throw 'The tracy profile only defines a Debug build.'
            }
            $configurePreset = 'windows-msvc-tracy'
            $buildPreset = 'windows-msvc-tracy'
            $directory = 'tracy'
        }
        'cacao' {
            if ($Configuration -ne 'Debug') {
                throw 'The cacao profile only defines a Debug build.'
            }
            $configurePreset = 'windows-msvc-ao-compare'
            $buildPreset = 'windows-msvc-ao-compare'
            $directory = 'cacao'
        }
    }

    $binaryDirectory = Join-Path $script:VulkanLabRepositoryRoot (
        'build/' + $directory)
    $runtimeDirectory = Join-Path $binaryDirectory (
        'run/' + $Configuration)
    return [pscustomobject]@{
        Name = $Profile
        Configuration = $Configuration
        ConfigurePreset = $configurePreset
        BuildPreset = $buildPreset
        BinaryDirectory = $binaryDirectory
        RuntimeDirectory = $runtimeDirectory
        VulkanLab = Join-Path $runtimeDirectory 'VulkanLab.exe'
        AssetTool = Join-Path $runtimeDirectory 'VulkanLabAssetTool.exe'
    }
}

function Invoke-VulkanLabConfigure {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    $profileInfo = Get-VulkanLabProfile $Profile $Configuration
    Invoke-VulkanLabCommand 'cmake' @(
        '--preset', $profileInfo.ConfigurePreset)
    return $profileInfo
}

function Invoke-VulkanLabBuildPreset {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    $profileInfo = Get-VulkanLabProfile $Profile $Configuration
    Invoke-VulkanLabCommand 'cmake' @(
        '--build', '--preset', $profileInfo.BuildPreset)
    return $profileInfo
}

function Invoke-VulkanLabBuildTarget {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [Parameter(Mandatory = $true)]
        [string]$Target,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    $profileInfo = Get-VulkanLabProfile $Profile $Configuration
    Invoke-VulkanLabCommand 'cmake' @(
        '--build', $profileInfo.BinaryDirectory,
        '--config', $Configuration,
        '--target', $Target)
    return $profileInfo
}

function Get-VulkanLabBuildInfo {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable
    )

    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "VulkanLab executable was not found: $Executable"
    }
    $output = & $Executable --build-info-json 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "VulkanLab --build-info-json failed with exit code $exitCode"
    }
    try {
        return (($output | Out-String) | ConvertFrom-Json)
    } catch {
        throw "VulkanLab returned invalid build-info JSON: $($_.Exception.Message)"
    }
}

function Assert-VulkanLabMinimalRuntime {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable
    )

    $buildInfo = Get-VulkanLabBuildInfo $Executable
    if ($buildInfo.configuration -ne 'Release') {
        throw "Cook requires a Release runtime, got '$($buildInfo.configuration)'."
    }

    $forbiddenFeatures = @(
        'editorUi',
        'runtimeControl',
        'capture',
        'assetAuthoring',
        'validation',
        'gpuDebugUtils',
        'gpuProfiling',
        'tracy',
        'cacao')
    $enabled = @()
    foreach ($feature in $forbiddenFeatures) {
        $property = $buildInfo.features.PSObject.Properties[$feature]
        if ($null -eq $property) {
            throw "Build-info JSON is missing feature '$feature'."
        }
        if ([bool]$property.Value) {
            $enabled += $feature
        }
    }
    if ($enabled.Count -gt 0) {
        throw ('Cook requires the minimal runtime profile; enabled features: ' +
            ($enabled -join ', '))
    }
    return $buildInfo
}
