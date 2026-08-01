[CmdletBinding()]
param(
    [string]$DestinationRoot = ""
)

$ErrorActionPreference = "Stop"

$version = "0.13.1"
$expectedSha256 =
    "ee6db1a7e71a12deb5973a8dbfdf9f36d3635bec0e0b31b1cc74f28de7dac4c9"
$downloadUrl =
    "https://github.com/wolfpld/tracy/releases/download/v$version/windows-$version.zip"

if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
    $repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
    $DestinationRoot = Join-Path $repositoryRoot "external\tools\tracy"
}

$destination = Join-Path ([System.IO.Path]::GetFullPath($DestinationRoot)) $version
$profiler = Join-Path $destination "tracy-profiler.exe"
if (Test-Path -LiteralPath $profiler -PathType Leaf) {
    Write-Host "Tracy Profiler $version is already installed at $destination"
    exit 0
}

$temporaryRoot = Join-Path $env:TEMP ("VulkanLab-tracy-" + [guid]::NewGuid())
$archive = Join-Path $temporaryRoot "tracy.zip"
$expanded = Join-Path $temporaryRoot "expanded"
$staging = "$destination.staging-$PID"

try {
    New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
    Write-Host "Downloading Tracy Profiler $version..."
    Invoke-WebRequest -UseBasicParsing -Uri $downloadUrl -OutFile $archive
    $actualSha256 =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "Checksum mismatch: expected $expectedSha256, got $actualSha256"
    }

    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
    foreach ($required in @(
        "tracy-profiler.exe",
        "tracy-capture.exe",
        "tracy-csvexport.exe")) {
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
    Write-Host "Installed Tracy Profiler $version to $destination"
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}
