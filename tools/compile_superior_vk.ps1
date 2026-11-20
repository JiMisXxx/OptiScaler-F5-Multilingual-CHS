# Rebuilds the Superior Scaling SPIR-V blobs for native Vulkan (OptiScaler/shaders/dlssnr/superior/precompile_vk/*_Shader_Vk.h)
# with the repository's own dxc (OptiScaler/shaders/shader_tools/dxc.exe). PowerShell only, no Python.
#
# The Vulkan Superior pass (shaders/dlssnr/DlssNrSuperior_Vk.cpp) runs the No DLSS path of the D3D12 one, from the same
# sources, built with VK_MODE:
#   frozen v59 shader      superior/dlssnr_vk.hlsl (VkAux.hlsli + precompile/dlssnr.hlsl) -> dlssnr_superior_area
#                          superior/RawLow_vk.hlsl (VkAux.hlsli + RawLow.hlsl)           -> host_low_raw
#   guide passes           superior/SpatialGuides.hlsl, JointGuides.hlsl                 -> host_uniform_*guides*
#   live NR shader         precompile/dlssnr.hlsl + superior/HostResolve.hlsli           -> host_resolve_guided_raw
# Every entry point is renamed CSMain in the module, so the host makes all pipelines the same way.
#
# The D3D12 blobs are not touched: the sources only gained code under VK_MODE, and tools/compile_nr_interpass.ps1 -Check
# still has to say that every D3D12 blob matches. A change to any source here must be followed by this script and the result
# committed. -Check builds into a temporary copy and compares instead of writing (exit 1 on any difference).
param([switch]$Check)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$shaders = Join-Path $root "OptiScaler/shaders/dlssnr"
$dxc = Join-Path $root "OptiScaler/shaders/shader_tools/dxc.exe"
$out = Join-Path $shaders "superior/precompile_vk"
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("superior-vk-blobs-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $scratch | Out-Null
New-Item -ItemType Directory -Force $out | Out-Null
$built = New-Object System.Collections.Generic.List[string]

# Byte-for-byte what shader_tools/create_header.py writes, with trailing spaces trimmed and CRLF lines.
function Write-Header([string]$spv, [string]$header, [string]$name) {
    $bytes = [IO.File]::ReadAllBytes($spv)
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("#pragma once`n`ninline static const unsigned char $name[] = {`n    ")
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        [void]$sb.Append(('0x{0:x2}' -f $bytes[$i]))
        if ($i -lt $bytes.Length - 1) { [void]$sb.Append(', ') }
        if ((($i + 1) % 12) -eq 0) { [void]$sb.Append("`n    ") }
    }
    [void]$sb.Append("`n};`n")
    $lines = $sb.ToString().TrimEnd("`n").Split("`n") | ForEach-Object { $_.TrimEnd() }
    [IO.File]::WriteAllText($header, (($lines -join "`r`n") + "`r`n"), [Text.UTF8Encoding]::new($false))
}

function Build([string]$source, [string]$entry, [string[]]$defines, [string]$stem) {
    $spv = Join-Path $scratch "${stem}_Shader_Vk.spv"
    $dxcArgs = @('-spirv', '-T', 'cs_6_0', '-E', $entry, '-fspv-entrypoint-name=CSMain', '-O3', '-Qstrip_debug', '-D', 'VK_MODE') +
        $defines + @((Split-Path $source -Leaf), '-Fo', $spv)
    Push-Location (Split-Path $source -Parent)
    try { & $dxc @dxcArgs | Out-Null } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw "DXC failed: $stem" }
    Write-Header $spv (Join-Path $scratch "${stem}_Shader_Vk.h") "${stem}_spv"
    $built.Add($stem)
}

try {
    $superior = Join-Path $shaders "superior"
    Build (Join-Path $superior "dlssnr_vk.hlsl") "CSMain" @("-D", "DLSSNR_SUPERIOR_SCALING=1", "-D", "DLSSNR_SUPERIOR_AREA=1") "dlssnr_superior_area"
    Build (Join-Path $superior "RawLow_vk.hlsl") "RawLowMain" @() "host_low_raw"

    Build (Join-Path $superior "SpatialGuides.hlsl") "SuperiorUniformSurfaceGuidesMain" @() "host_uniform_surface_guides"
    foreach ($minified in @($false, $true)) {
        $defines = @()
        if ($minified) { $defines += @("-D", "DLSSNR_JOINT_MINIFY=1") }
        $entry = if ($minified) { "SuperiorUniformJointMinifiedGuidesMain" } else { "SuperiorUniformJointGuidesMain" }
        $stem = "host_uniform_joint_" + $(if ($minified) { "minified_" } else { "" }) + "guides"
        Build (Join-Path $superior "JointGuides.hlsl") $entry $defines $stem
        Build (Join-Path $superior "JointGuides.hlsl") $entry ($defines + @("-D", "DLSSNR_QUANTIZED_DEPTH=1")) ($stem + "_quantized")
    }

    $live = Join-Path $shaders "precompile/dlssnr.hlsl"
    Build $live "CSMain" @("-D", "DLSSNR_SUPERIOR_HOST_RESOLVE=1", "-D", "DLSSNR_SUPERIOR_HOST_RAW=1", "-D", "DLSSNR_SUPERIOR_GUIDED_RAW=1") "host_resolve_guided_raw"

    # Every shipped blob must come from this script, and nothing else may sit beside them.
    $shipped = @(Get-ChildItem $out -Filter "*_Shader_Vk.h" | ForEach-Object { $_.Name -replace '_Shader_Vk\.h$', '' })
    $orphans = @($shipped | Where-Object { $built -notcontains $_ })
    $different = New-Object System.Collections.Generic.List[string]
    foreach ($stem in $built) {
        foreach ($ext in @("_Shader_Vk.h")) {
            $new = Join-Path $scratch "$stem$ext"
            $old = Join-Path $out "$stem$ext"
            if ($Check) {
                if (-not ((Test-Path $old) -and (Get-FileHash $new).Hash -eq (Get-FileHash $old).Hash)) { $different.Add("$stem$ext") }
            } else {
                Copy-Item $new $old -Force
            }
        }
        Write-Host "$stem SHA256=$((Get-FileHash (Join-Path $scratch "${stem}_Shader_Vk.spv")).Hash)"
    }
    Write-Host "$($built.Count) blobs built with $dxc"
    if ($orphans.Count) { Write-Host "Not built by this script: $($orphans -join ', ')" }
    if ($Check -and $different.Count) { Write-Host "Differs from the committed blob: $($different -join ', ')" }
    if ($Check -and ($orphans.Count -or $different.Count)) { exit 1 }
    if ($Check) { Write-Host "All $($built.Count) blobs and headers match byte for byte" }
} finally {
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
