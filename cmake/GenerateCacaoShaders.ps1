param(
    [Parameter(Mandatory = $true)][string]$Dxc,
    [Parameter(Mandatory = $true)][string]$SpirvVal,
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [Parameter(Mandatory = $true)][string]$Stamp
)

$ErrorActionPreference = 'Stop'

$batchPath = Join-Path $SourceRoot 'build_shaders_spirv.bat'
$patchedSourceRoot = Join-Path $OutputRoot 'source'
$hlslPath = Join-Path $patchedSourceRoot 'ffx_cacao.hlsl'
$headerRoot = Join-Path $OutputRoot 'PrecompiledShadersSPIRV'
$spirvRoot = Join-Path $OutputRoot 'spirv'

New-Item -ItemType Directory -Force -Path $headerRoot, $spirvRoot, $patchedSourceRoot | Out-Null

Copy-Item -LiteralPath (Join-Path $SourceRoot 'ffx_cacao.hlsl') -Destination $hlslPath -Force
Copy-Item -LiteralPath (Join-Path $SourceRoot 'ffx_cacao_defines.h') -Destination $patchedSourceRoot -Force
$bindingsPath = Join-Path $patchedSourceRoot 'ffx_cacao_bindings.hlsl'
$bindings = Get-Content -LiteralPath (Join-Path $SourceRoot 'ffx_cacao_bindings.hlsl') -Raw

# DXC shipped with recent Vulkan SDKs correctly rejects non-immediate texture
# offsets. CACAO v1.2 passes constants through helper parameters, so preserve
# the same texel locations by folding the offset into coordinates in the build
# copy. The pinned upstream submodule remains untouched.
$bindings = $bindings.Replace(
    'return g_DepthIn.Load(int3(coord, 0), offset);',
    'return g_DepthIn.Load(int3(coord + offset, 0));')
$bindings = $bindings.Replace(
    'return g_DepthIn.GatherRed(g_PointClampSampler, uv, offset);',
    'return g_DepthIn.GatherRed(g_PointClampSampler, uv + float2(offset) * g_FFX_CACAO_Consts.DepthBufferInverseDimensions);')
$bindings = $bindings.Replace(
    'return g_ViewspaceDepthSource.GatherRed(g_PointMirrorSampler, float3(uv, 0.0f), offset);',
    'return g_ViewspaceDepthSource.GatherRed(g_PointMirrorSampler, float3(uv + float2(offset) * g_FFX_CACAO_Consts.DeinterleavedDepthBufferInverseDimensions, 0.0f));')
$bindings = $bindings.Replace(
    'return g_BilateralUpscaleDepth.Load(int3(coord, 0), offset);',
    'return g_BilateralUpscaleDepth.Load(int3(coord + offset, 0));')
Set-Content -LiteralPath $bindingsPath -Value $bindings -Encoding UTF8

$pattern = '^%cauldron_dxc_(16|32)%\s+-Fh\s+PrecompiledShadersSPIRV/([^\s]+)\s+-Vn\s+([^\s]+)\s+-E\s+([^\s]+)\s+ffx_cacao\.hlsl\s*$'
$entries = foreach ($line in Get-Content -LiteralPath $batchPath) {
    if ($line -match $pattern) {
        [PSCustomObject]@{
            Precision = $Matches[1]
            Header = $Matches[2]
            Symbol = $Matches[3]
            EntryPoint = $Matches[4]
        }
    }
}

if ($entries.Count -lt 60) {
    throw "Expected at least 60 CACAO shader entries, found $($entries.Count)."
}

$baseArguments = @(
    '-Wno-conversion',
    '-spirv',
    '-T', 'cs_6_2',
    '-fspv-target-env=vulkan1.0',
    '-fvk-s-shift', '0', '0',
    '-fvk-b-shift', '10', '0',
    '-fvk-t-shift', '20', '0',
    '-fvk-u-shift', '30', '0'
)

foreach ($entry in $entries) {
    $headerPath = Join-Path $headerRoot $entry.Header
    $spirvPath = Join-Path $spirvRoot ($entry.Header -replace '\.h$', '.spv')
    $arguments = @($baseArguments)
    if ($entry.Precision -eq '16') {
        $arguments += '-enable-16bit-types'
    }
    $arguments += @(
        '-Fo', $spirvPath,
        '-Fh', $headerPath,
        '-Vn', $entry.Symbol,
        '-E', $entry.EntryPoint,
        $hlslPath
    )

    & $Dxc @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "DXC failed for CACAO entry point $($entry.EntryPoint) ($($entry.Precision)-bit)."
    }

    & $SpirvVal '--target-env' 'vulkan1.0' $spirvPath
    if ($LASTEXITCODE -ne 0) {
        throw "spirv-val failed for CACAO entry point $($entry.EntryPoint) ($($entry.Precision)-bit)."
    }
}

Set-Content -LiteralPath $Stamp -Value "CACAO SPIR-V generated: $($entries.Count)" -Encoding Ascii
