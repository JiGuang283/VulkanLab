param(
    [Parameter(Mandatory = $true)]
    [string]$Renderer,

    [Parameter(Mandatory = $true)]
    [string]$ControlTool,

    [string]$Project = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Scene = 'Main Sponza',
    [string]$Shader = 'PBR-lite NormalMapped',
    [ValidateSet('', 'auto', 'legacy', 'bindless')]
    [string]$MaterialBinding = '',
    [ValidateSet('Minimal', 'Default', 'Ssao', 'Ssr', 'Ssgi', 'Taa',
                 'Ddgi', 'SsgiDdgi')]
    [string]$Profile = 'Minimal',
    [int]$Width = 1280,
    [int]$Height = 720,
    [int]$WarmupSeconds = 3,
    [int]$SampleSeconds = 8,
    [int]$GpuSamples = 12,
    [switch]$Gui,
    [string]$Output
)

$ErrorActionPreference = 'Stop'

$rendererPath = (Resolve-Path $Renderer).Path
$controlPath = (Resolve-Path $ControlTool).Path
$projectPath = (Resolve-Path $Project).Path
$pipeSuffix = 'perf_' + [Guid]::NewGuid().ToString('N')
$stdoutPath = Join-Path $env:TEMP ($pipeSuffix + '.stdout.log')
$stderrPath = Join-Path $env:TEMP ($pipeSuffix + '.stderr.log')
$process = $null

function Invoke-Control {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)

    $text = & $controlPath --pipe $pipeSuffix --json @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "VulkanLabCtl failed ($LASTEXITCODE): $($Arguments -join ' ')`n$($text -join "`n")"
    }
    return (($text -join "`n") | ConvertFrom-Json)
}

function Wait-ForControl {
    for ($attempt = 0; $attempt -lt 120; ++$attempt) {
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $null = & $controlPath --pipe $pipeSuffix ping 2>$null
        $exitCode = $LASTEXITCODE
        $ErrorActionPreference = $previousPreference
        if ($exitCode -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 250
    }
    throw 'Runtime Control did not become ready within 30 seconds.'
}

function Median([double[]]$Values) {
    if ($Values.Count -eq 0) {
        return $null
    }
    $ordered = @($Values | Sort-Object)
    $middle = [int]($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) {
        return $ordered[$middle]
    }
    return ($ordered[$middle - 1] + $ordered[$middle]) * 0.5
}

try {
    Remove-Item $stdoutPath, $stderrPath -ErrorAction SilentlyContinue
    $rendererArguments = @(
        '--project', $projectPath,
        '--runtime-control',
        '--runtime-control-pipe', $pipeSuffix,
        '--automation',
        '--window-size', "${Width}x${Height}",
        '--validation', 'off'
    )
    if (-not $Gui) {
        $rendererArguments += '--no-gui'
    }
    if ($MaterialBinding) {
        $rendererArguments += @('--material-binding', $MaterialBinding)
    }

    $process = Start-Process -FilePath $rendererPath `
        -ArgumentList $rendererArguments `
        -WorkingDirectory $projectPath `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -PassThru

    Wait-ForControl
    $info = (Invoke-Control info).result
    $null = Invoke-Control shader set $Shader
    $null = Invoke-Control scene load $Scene

    if ($Profile -ne 'Default') {
        $null = Invoke-Control render-settings set `
            --shadows off `
            --ibl off `
            --skybox off `
            --bloom off `
            --occlusion off `
            --ao off `
            --taa off `
            --reflection ibl-only `
            --gi ambient-or-ibl

        switch ($Profile) {
            'Ssao' {
                $null = Invoke-Control render-settings set --ao ssao
            }
            'Ssr' {
                $null = Invoke-Control render-settings set --reflection ssr
            }
            'Ssgi' {
                $null = Invoke-Control render-settings set --gi ssgi
            }
            'Taa' {
                $null = Invoke-Control render-settings set --taa taa
            }
            'Ddgi' {
                $null = Invoke-Control render-settings set --gi ddgi
            }
            'SsgiDdgi' {
                $null = Invoke-Control render-settings set --gi ssgi-ddgi
            }
        }
    }

    $null = Invoke-Control render wait --stable-frames 120 --timeout-ms 60000
    Start-Sleep -Seconds $WarmupSeconds

    $before = (Invoke-Control render status).result
    $timer = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $SampleSeconds
    $after = (Invoke-Control render status).result
    $timer.Stop()

    $presentedFrames = [uint64]$after.presentedFrames -
        [uint64]$before.presentedFrames
    $fps = $presentedFrames / $timer.Elapsed.TotalSeconds

    $gpuTotals = [System.Collections.Generic.List[double]]::new()
    $passSamples = @{}
    $lastStatus = $after
    for ($sample = 0; $sample -lt $GpuSamples; ++$sample) {
        $lastStatus = (Invoke-Control render status).result
        if ($lastStatus.gpuTimings.available) {
            $gpuTotals.Add([double]$lastStatus.gpuTimings.totalMs)
            foreach ($property in $lastStatus.gpuTimings.passes.PSObject.Properties) {
                if (-not $passSamples.ContainsKey($property.Name)) {
                    $passSamples[$property.Name] =
                        [System.Collections.Generic.List[double]]::new()
                }
                $passSamples[$property.Name].Add([double]$property.Value)
            }
        }
        Start-Sleep -Milliseconds 50
    }

    $gpuPassMedians = [ordered]@{}
    foreach ($name in @($passSamples.Keys | Sort-Object)) {
        $gpuPassMedians[$name] = Median $passSamples[$name].ToArray()
    }

    $graph = $lastStatus.renderGraph
    $result = [ordered]@{
        revision = $info.build.revision
        configuration = $info.build.configuration
        gpu = $info.gpu.name
        scene = $Scene
        shader = $Shader
        materialBindingRequested = if ($MaterialBinding) { $MaterialBinding } else { 'unsupported-or-default' }
        materialBindingActive = $info.diagnostics.materialBinding.active
        renderPath = $lastStatus.renderPath
        profile = $Profile
        gui = [bool]$Gui
        extent = [ordered]@{ width = $Width; height = $Height }
        sampleSeconds = $timer.Elapsed.TotalSeconds
        presentedFrames = $presentedFrames
        framesPerSecond = $fps
        cpuFrameMsEstimated = if ($fps -gt 0) { 1000.0 / $fps } else { $null }
        gpuFrameMsMedian = Median $gpuTotals.ToArray()
        gpuPassMsMedian = $gpuPassMedians
        renderGraph = if ($null -ne $graph) {
            [ordered]@{
                activePasses = $graph.activePasses
                topologyHash = $graph.topologyHash
                dependencyEdges = $graph.dependencyEdges
                automaticBarriers = $graph.automaticBarriers
                layoutBarriers = $graph.layoutBarriers
                hazardBarriers = $graph.hazardBarriers
                executionOrder = $graph.executionOrder
            }
        } else { $null }
        screenSpace = $lastStatus.screenSpace
        validationErrors = $info.diagnostics.validation.errorCount
    }

    $json = $result | ConvertTo-Json -Depth 12
    if ($Output) {
        $outputPath = [IO.Path]::GetFullPath($Output)
        $outputDirectory = Split-Path -Parent $outputPath
        if ($outputDirectory) {
            New-Item -ItemType Directory -Force $outputDirectory | Out-Null
        }
        Set-Content -LiteralPath $outputPath -Value $json -Encoding UTF8
    }
    $json

    $null = Invoke-Control quit
    $process.WaitForExit(10000) | Out-Null
} finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}
