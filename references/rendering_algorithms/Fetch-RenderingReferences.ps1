[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$SkipCode,
    [switch]$SkipPapers,
    [switch]$SkipDocs
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifestPath = Join-Path $scriptRoot 'sources.json'
$downloadsRoot = Join-Path $scriptRoot 'downloads'
$codeRoot = Join-Path $downloadsRoot 'code'
$papersRoot = Join-Path $downloadsRoot 'papers'
$docsRoot = Join-Path $downloadsRoot 'docs'
$failures = @()

if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Reference manifest not found: $manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
    ConvertFrom-Json

if ($manifest.schemaVersion -ne 1) {
    throw "Unsupported reference manifest schema: $($manifest.schemaVersion)"
}

New-Item -ItemType Directory -Force -Path $downloadsRoot | Out-Null

function Assert-DownloadChildPath {
    param([Parameter(Mandatory)][string]$Path)

    $root = [IO.Path]::GetFullPath($downloadsRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $candidate = [IO.Path]::GetFullPath($Path)
    $prefix = $root + [IO.Path]::DirectorySeparatorChar
    if (-not $candidate.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside downloads/: $candidate"
    }
}

function Invoke-GitChecked {
    param([Parameter(Mandatory)][string[]]$Arguments)

    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git failed with exit code ${LASTEXITCODE}: git $($Arguments -join ' ')"
    }
}

function Get-RepositoryHead {
    param([Parameter(Mandatory)][string]$Path)

    $value = & git -C $Path rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    return $value.Trim()
}

function Fetch-CodeReference {
    param([Parameter(Mandatory)]$Entry)

    $destination = Join-Path $codeRoot $Entry.directory
    Assert-DownloadChildPath -Path $destination

    if (Test-Path -LiteralPath $destination) {
        $head = Get-RepositoryHead -Path $destination
        if (-not $Force -and $head -eq $Entry.commit) {
            Write-Host "[code] ready $($Entry.id) @ $head"
            return
        }
        if (-not $Force) {
            throw "Reference '$($Entry.id)' exists at '$head'; use -Force to restore $($Entry.commit)."
        }
        Remove-Item -LiteralPath $destination -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Write-Host "[code] fetching $($Entry.id) @ $($Entry.commit)"

    Invoke-GitChecked @('-C', $destination, 'init', '--quiet')
    Invoke-GitChecked @('-C', $destination, 'remote', 'add', 'origin',
        $Entry.url)
    Invoke-GitChecked @('-C', $destination, 'sparse-checkout', 'init',
        '--cone')
    Invoke-GitChecked (@('-C', $destination, 'sparse-checkout', 'set') +
        @($Entry.sparsePaths))
    Invoke-GitChecked @('-C', $destination, '-c', 'protocol.version=2',
        'fetch', '--quiet', '--depth', '1', '--filter=blob:none', 'origin',
        $Entry.commit)
    Invoke-GitChecked @('-C', $destination, 'checkout', '--quiet',
        '--detach', $Entry.commit)

    $head = Get-RepositoryHead -Path $destination
    if ($head -ne $Entry.commit) {
        throw "Reference '$($Entry.id)' checked out '$head', expected '$($Entry.commit)'."
    }
}

function Fetch-FileReference {
    param(
        [Parameter(Mandatory)]$Entry,
        [Parameter(Mandatory)][string]$Category,
        [Parameter(Mandatory)][string]$DestinationRoot
    )

    New-Item -ItemType Directory -Force -Path $DestinationRoot | Out-Null
    $destination = Join-Path $DestinationRoot $Entry.file
    Assert-DownloadChildPath -Path $destination

    if ((Test-Path -LiteralPath $destination) -and -not $Force) {
        Write-Host "[$Category] ready $($Entry.file)"
        return
    }

    $temporary = "$destination.part"
    Assert-DownloadChildPath -Path $temporary
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }

    Write-Host "[$Category] downloading $($Entry.file)"
    $downloadedWithBits = $false
    if (Get-Command Start-BitsTransfer -ErrorAction SilentlyContinue) {
        try {
            Start-BitsTransfer -Source $Entry.url -Destination $temporary `
                -DisplayName "VulkanLab reference: $($Entry.id)" `
                -ErrorAction Stop
            $downloadedWithBits = $true
        }
        catch {
            Write-Warning "BITS failed for $($Entry.id); falling back to curl: $($_.Exception.Message)"
        }
    }

    if (-not $downloadedWithBits) {
        & curl.exe --http1.1 --location --fail --retry 5 `
            --retry-all-errors --retry-delay 2 --connect-timeout 30 `
            --max-time 300 --silent --show-error `
            --user-agent 'VulkanLab-Reference-Fetch/1.0' `
            --output $temporary $Entry.url
    }

    if (-not $downloadedWithBits -and $LASTEXITCODE -ne 0) {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
        throw "Download failed for $($Entry.url)"
    }

    $downloaded = Get-Item -LiteralPath $temporary
    if ($downloaded.Length -eq 0) {
        Remove-Item -LiteralPath $temporary -Force
        throw "Downloaded an empty file from $($Entry.url)"
    }

    Move-Item -LiteralPath $temporary -Destination $destination -Force
}

if (-not $SkipCode) {
    New-Item -ItemType Directory -Force -Path $codeRoot | Out-Null
    foreach ($entry in $manifest.code) {
        try {
            Fetch-CodeReference -Entry $entry
        }
        catch {
            $failures += [ordered]@{
                category = 'code'
                id = $entry.id
                error = $_.Exception.Message
            }
            Write-Warning "[code] $($entry.id): $($_.Exception.Message)"
        }
    }
}

if (-not $SkipPapers) {
    foreach ($entry in $manifest.papers) {
        try {
            Fetch-FileReference -Entry $entry -Category 'paper' `
                -DestinationRoot $papersRoot
        }
        catch {
            $failures += [ordered]@{
                category = 'paper'
                id = $entry.id
                error = $_.Exception.Message
            }
            Write-Warning "[paper] $($entry.id): $($_.Exception.Message)"
        }
    }
}

if (-not $SkipDocs) {
    foreach ($entry in $manifest.docs) {
        try {
            Fetch-FileReference -Entry $entry -Category 'docs' `
                -DestinationRoot $docsRoot
        }
        catch {
            $failures += [ordered]@{
                category = 'docs'
                id = $entry.id
                error = $_.Exception.Message
            }
            Write-Warning "[docs] $($entry.id): $($_.Exception.Message)"
        }
    }
}

$fileRecords = @()
foreach ($group in @(
        [ordered]@{ directory = 'papers'; root = $papersRoot; entries = $manifest.papers },
        [ordered]@{ directory = 'docs'; root = $docsRoot; entries = $manifest.docs }
    )) {
    foreach ($entry in $group.entries) {
        $path = Join-Path $group.root $entry.file
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            continue
        }
        $file = Get-Item -LiteralPath $path
        $hash = Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256
        $fileRecords += [ordered]@{
            path = "$($group.directory)/$($entry.file)"
            bytes = $file.Length
            sha256 = $hash.Hash.ToLowerInvariant()
        }
    }
}

$codeRecords = @()
if (Test-Path -LiteralPath $codeRoot) {
    foreach ($entry in $manifest.code) {
        $destination = Join-Path $codeRoot $entry.directory
        if (-not (Test-Path -LiteralPath $destination)) {
            continue
        }
        $codeRecords += [ordered]@{
            id = $entry.id
            commit = Get-RepositoryHead -Path $destination
            path = "code/$($entry.directory)"
        }
    }
}

$sumLines = foreach ($record in $fileRecords) {
    "$($record.sha256)  $($record.path)"
}
Set-Content -LiteralPath (Join-Path $downloadsRoot 'SHA256SUMS.txt') `
    -Value $sumLines -Encoding UTF8

$report = [ordered]@{
    schemaVersion = 1
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    code = $codeRecords
    files = $fileRecords
    failures = $failures
}
$report | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $downloadsRoot 'fetch-report.json') `
        -Encoding UTF8

Write-Host "Reference fetch complete: $downloadsRoot"
Write-Host "Code repositories: $($codeRecords.Count)"
Write-Host "Downloaded files: $($fileRecords.Count)"
if ($failures.Count -ne 0) {
    Write-Warning "Reference fetch completed with $($failures.Count) failure(s); see fetch-report.json."
    exit 1
}
