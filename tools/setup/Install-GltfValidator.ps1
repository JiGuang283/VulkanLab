[CmdletBinding()]
param(
    [string]$DestinationRoot = ""
)

$ErrorActionPreference = "Stop"

$version = "2.0.0-dev.3.10"
$expectedSha256 =
    "c5068f51205deedc28acc3529ee7e11ee60e853454f673093398eba80142202c"
$downloadUrl =
    "https://github.com/KhronosGroup/glTF-Validator/releases/download/$version/gltf_validator-$version-win64.zip"

if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
    $repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
    $DestinationRoot = Join-Path $repositoryRoot "external\tools\gltf-validator"
}

$destination = Join-Path ([System.IO.Path]::GetFullPath($DestinationRoot)) $version
$executable = Join-Path $destination "gltf_validator.exe"
if (Test-Path -LiteralPath $executable -PathType Leaf) {
    Write-Host "glTF Validator $version is already installed at $destination"
    exit 0
}

$temporaryRoot = Join-Path $env:TEMP ("VulkanLab-gltf-validator-" + [guid]::NewGuid())
$archive = Join-Path $temporaryRoot "validator.zip"
$expanded = Join-Path $temporaryRoot "expanded"
$staging = "$destination.staging-$PID"

try {
    New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
    Write-Host "Downloading glTF Validator $version..."
    Invoke-WebRequest -UseBasicParsing -Uri $downloadUrl -OutFile $archive
    $actualSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "Checksum mismatch: expected $expectedSha256, got $actualSha256"
    }

    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
    foreach ($required in @("gltf_validator.exe", "LICENSE", "NOTICES")) {
        if (-not (Test-Path -LiteralPath (Join-Path $expanded $required) -PathType Leaf)) {
            throw "Downloaded package is missing $required"
        }
    }

    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Move-Item -LiteralPath $expanded -Destination $staging
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    Move-Item -LiteralPath $staging -Destination $destination
    Write-Host "Installed glTF Validator $version to $destination"
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}
