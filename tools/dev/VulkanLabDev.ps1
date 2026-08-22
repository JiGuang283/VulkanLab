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

function Initialize-VulkanLabMsvcEnvironment {
    if ($null -ne (Get-Command cl.exe -ErrorAction SilentlyContinue) -and
        $null -ne (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe was not found; install Visual Studio 2022 with the C++ workload.'
    }
    $installPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or
        [string]::IsNullOrWhiteSpace($installPath)) {
        throw 'Visual Studio 2022 with the x64 C++ toolchain was not found.'
    }

    if ($null -eq (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vsDevCmd = Join-Path $installPath 'Common7/Tools/VsDevCmd.bat'
        if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
            throw "VsDevCmd.bat was not found: $vsDevCmd"
        }
        $command = '"' + $vsDevCmd +
            '" -no_logo -arch=x64 -host_arch=x64 >nul && set'
        $environmentLines = & cmd.exe /d /s /c $command
        if ($LASTEXITCODE -ne 0) {
            throw 'VsDevCmd.bat failed to initialize the x64 MSVC environment.'
        }
        foreach ($line in $environmentLines) {
            $separator = $line.IndexOf('=')
            if ($separator -le 0) {
                continue
            }
            [Environment]::SetEnvironmentVariable(
                $line.Substring(0, $separator),
                $line.Substring($separator + 1),
                'Process')
        }
    }
    if ($null -eq (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'VsDevCmd completed but cl.exe is still unavailable.'
    }

    if ($null -eq (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
        $bundledNinja = Join-Path $installPath `
            'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
        if (Test-Path -LiteralPath $bundledNinja -PathType Leaf) {
            $ninjaDirectory = Split-Path -Parent $bundledNinja
            $env:Path = $ninjaDirectory + [IO.Path]::PathSeparator + $env:Path
        }
    }
    if ($null -eq (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
        throw 'ninja.exe was not found on PATH or in the Visual Studio CMake tools.'
    }
}

function Get-VulkanLabProfile {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('ninja-dev', 'dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    switch ($Profile) {
        'ninja-dev' {
            if ($Configuration -ne 'Debug') {
                throw 'The ninja-dev profile only defines a Debug build.'
            }
            $configurePreset = 'windows-ninja-dev'
            $buildPreset = 'windows-ninja-dev'
            $directory = 'ninja-dev'
        }
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
        [ValidateSet('ninja-dev', 'dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    $profileInfo = Get-VulkanLabProfile $Profile $Configuration
    if ($Profile -eq 'ninja-dev') {
        Initialize-VulkanLabMsvcEnvironment
    }
    Invoke-VulkanLabCommand 'cmake' @(
        '--preset', $profileInfo.ConfigurePreset)
    return $profileInfo
}

function Ensure-VulkanLabConfigured {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('ninja-dev', 'dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug',
        [switch]$Force
    )

    $profileInfo = Get-VulkanLabProfile $Profile $Configuration
    $cachePath = Join-Path $profileInfo.BinaryDirectory 'CMakeCache.txt'
    if ($Force -or -not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        return Invoke-VulkanLabConfigure $Profile $Configuration
    }
    return $profileInfo
}

function Invoke-VulkanLabBuildPreset {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('ninja-dev', 'dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    $profileInfo = Get-VulkanLabProfile $Profile $Configuration
    if ($Profile -eq 'ninja-dev') {
        Initialize-VulkanLabMsvcEnvironment
    }
    Invoke-VulkanLabCommand 'cmake' @(
        '--build', '--preset', $profileInfo.BuildPreset)
    return $profileInfo
}

function Invoke-VulkanLabBuildTarget {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('ninja-dev', 'dev', 'full', 'runtime', 'tracy', 'cacao')]
        [string]$Profile,
        [Parameter(Mandatory = $true)]
        [string]$Target,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Debug'
    )

    $profileInfo = Get-VulkanLabProfile $Profile $Configuration
    if ($Profile -eq 'ninja-dev') {
        Initialize-VulkanLabMsvcEnvironment
    }
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
