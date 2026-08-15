param(
    [ValidateSet('core', 'sync', 'gpu')]
    [string]$Profile = 'core',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$BuildDirectory = 'build-debug',

    [string]$CaptureRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runtime = (Resolve-Path (Join-Path $repoRoot "$BuildDirectory\$Configuration")).Path
$appPath = Join-Path $runtime 'VulkanLab.exe'
$ctlPath = Join-Path $runtime 'VulkanLabCtl.exe'
if (-not (Test-Path -LiteralPath $appPath) -or
    -not (Test-Path -LiteralPath $ctlPath)) {
    throw "VulkanLab.exe and VulkanLabCtl.exe were not found under $runtime"
}

if ([string]::IsNullOrWhiteSpace($CaptureRoot)) {
    $CaptureRoot = Join-Path $repoRoot "artifacts\validation-smoke\$Profile-$PID"
}
$CaptureRoot = [System.IO.Path]::GetFullPath($CaptureRoot)
[System.IO.Directory]::CreateDirectory($CaptureRoot) | Out-Null

$suffix = "validation_${Profile}_${PID}_$([DateTime]::UtcNow.Ticks)"
$timeoutMs = if ($Profile -eq 'gpu') { 180000 } else { 60000 }
$app = $null

function Invoke-ControlJson {
    param([Parameter(Mandatory = $true)][string[]]$CommandArgs)

    $output = & $ctlPath --pipe $suffix --json @CommandArgs 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | Out-String).Trim()
    if ($exitCode -ne 0) {
        throw "VulkanLabCtl failed with exit code ${exitCode}: $text"
    }
    $response = $text | ConvertFrom-Json
    if (-not $response.ok) {
        throw "Runtime command failed: $text"
    }
    return $response
}

try {
    $arguments = @(
        '--runtime-control',
        '--runtime-control-pipe', $suffix,
        '--automation',
        '--window-size', '800x600',
        '--fixed-delta', '0.016666667',
        '--no-gui',
        '--capture-root', $CaptureRoot,
        '--validation', $Profile
    )
    $app = Start-Process -FilePath $appPath -WorkingDirectory $runtime `
        -ArgumentList $arguments -WindowStyle Hidden -PassThru

    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    while ($true) {
        if ($app.HasExited) {
            throw "VulkanLab exited before Runtime Control became available."
        }
        try {
            Invoke-ControlJson -CommandArgs @('ping') | Out-Null
            break
        } catch {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw
            }
            Start-Sleep -Milliseconds 250
        }
    }

    $info = (Invoke-ControlJson -CommandArgs @('info')).result
    $validation = $info.diagnostics.validation
    if ($validation.requested -ne $Profile -or
        $validation.actual -ne $Profile) {
        throw "Validation profile fallback: requested=$($validation.requested), actual=$($validation.actual), reason=$($validation.fallbackReason)"
    }
    if ($validation.errorCount -ne 0) {
        throw "Validation reported $($validation.errorCount) errors during startup."
    }

    Invoke-ControlJson -CommandArgs @('scene', 'load', 'Renderer Smoke Scene') |
        Out-Null
    Invoke-ControlJson -CommandArgs @(
        'shader', 'set', 'PBR-lite NormalMapped'
    ) | Out-Null
    Invoke-ControlJson -CommandArgs @(
        'window', 'resize', '1024', '720'
    ) | Out-Null
    Invoke-ControlJson -CommandArgs @(
        'render', 'wait', '--stable-frames', '8',
        '--timeout-ms', [string]$timeoutMs
    ) | Out-Null

    $capture = (Invoke-ControlJson -CommandArgs @(
        'capture', 'screenshot', "validation\$Profile.png", '--no-gui'
    )).result
    $taskId = [uint64]$capture.taskId
    do {
        Start-Sleep -Milliseconds 50
        $captureStatus =
            (Invoke-ControlJson -CommandArgs @(
                'capture', 'status', [string]$taskId
            )).result
    } until ($captureStatus.terminal)
    if ($captureStatus.state -ne 'Completed') {
        throw "Capture task ended in state $($captureStatus.state)."
    }

    $finalInfo = (Invoke-ControlJson -CommandArgs @('info')).result
    $finalValidation = $finalInfo.diagnostics.validation
    if ($finalValidation.actual -ne $Profile) {
        throw "Validation profile changed to $($finalValidation.actual)."
    }
    if ($finalValidation.errorCount -ne 0) {
        throw "Validation reported $($finalValidation.errorCount) errors."
    }

    Write-Host "Validation smoke passed: profile=$Profile"
    Write-Host "Warnings: $($finalValidation.warningCount)"
    Write-Host "Capture: $($captureStatus.result.outputPath)"
} finally {
    if ($app -and -not $app.HasExited) {
        try {
            Invoke-ControlJson -CommandArgs @('quit') | Out-Null
        } catch {
            Write-Warning "Could not request a clean shutdown: $_"
        }
        if (-not $app.WaitForExit(15000)) {
            Stop-Process -Id $app.Id -Force
        }
    }
}
