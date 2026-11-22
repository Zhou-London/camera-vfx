param(
    [string]$Version = 'latest',
    [string]$Destination = (Join-Path $PSScriptRoot '..\third_party\nvvfx_runtime')
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$index = 'https://pypi.nvidia.com/nvidia-vfx/'
$requiredDlls = 'NVVideoEffects.dll', 'NVCVImage.dll', 'nvVFXVideoFrameGeneration.dll', 'nvVFXVideoSuperRes.dll', 'nvVFXTrueHDR.dll'
$expectedRuntime = [version]'1.3'

$wheels = [regex]::Matches((Invoke-WebRequest $index -UseBasicParsing).Content,
        'href="(?<file>nvidia_vfx-(?<ver>[\d.]+)-cp312-abi3-win_amd64\.whl)#sha256=(?<sha>[0-9a-f]{64})"') |
    ForEach-Object { [pscustomobject]@{ File = $_.Groups['file'].Value; Version = [version]$_.Groups['ver'].Value; Sha = $_.Groups['sha'].Value } } |
    Sort-Object Version
$wheel = if ($Version -eq 'latest') { $wheels | Select-Object -Last 1 } else { $wheels | Where-Object { $_.Version -eq [version]$Version } | Select-Object -Last 1 }
if (-not $wheel) { throw "No win_amd64 wheel for nvidia-vfx $Version at $index" }

$download = Join-Path ([IO.Path]::GetTempPath()) $wheel.File
Write-Host "Downloading $($wheel.File) (about 500 MB)"
Invoke-WebRequest ($index + $wheel.File) -OutFile $download -UseBasicParsing
if ((Get-FileHash $download -Algorithm SHA256).Hash -ne $wheel.Sha) {
    Remove-Item $download
    throw "SHA256 mismatch for $($wheel.File)"
}

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force $Destination | Out-Null
$zip = [IO.Compression.ZipFile]::OpenRead($download)
try {
    foreach ($entry in $zip.Entries) {
        if (-not $entry.Name) { continue }
        $target = if ($entry.FullName -like 'nvvfx/libs/*') { Join-Path $Destination $entry.FullName.Substring('nvvfx/libs/'.Length) }
                  elseif ($entry.Name -like '*.pdf') { Join-Path $Destination "licenses\$($entry.Name)" }
        if (-not $target) { continue }
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    }
} finally { $zip.Dispose() }
Remove-Item $download

$missing = $requiredDlls | Where-Object { -not (Test-Path (Join-Path $Destination $_)) }
if ($missing) { throw "Wheel $($wheel.File) lacks: $($missing -join ', ')" }
$runtime = (Get-Item (Join-Path $Destination 'NVVideoEffects.dll')).VersionInfo
Write-Host "VFX runtime $($runtime.FileVersion) installed to $((Resolve-Path $Destination).Path)"
if ($runtime.FileMajorPart -ne $expectedRuntime.Major -or $runtime.FileMinorPart -ne $expectedRuntime.Minor) {
    Write-Warning "CameraVFX is built against the VFX SDK $expectedRuntime headers; pick a wheel with that runtime via -Version"
}
