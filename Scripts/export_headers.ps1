# Auto-Connecting Power Rails — zip every .h under the SML project's Source tree.
#
# WHY. Work on a vanilla class needs its header (FGBeamHologram.h for the preview length,
# FGColoredInstanceMeshProxy.h / FGBuildable.h / FGFactoryColoringTypes.h for how customization
# data reaches a material): a name or a function is checked against the header, not guessed, and
# one zip is cheaper than pasting them one at a time.
#
# WHAT. Mirrors only the *.h files (relative paths kept) into a temp folder and zips that, so the
# archive is headers and nothing else — no .cpp, no binaries, no Mods.
#
# USAGE, from anywhere:
#     powershell -ExecutionPolicy Bypass -File export_headers.ps1
#     powershell -ExecutionPolicy Bypass -File export_headers.ps1 -Root G:\Other\SML -Out C:\tmp\h.zip
#
# Defaults assume this script lives in <SML>\Mods\GameFeatures\AutoConnectingPowerRails\Scripts.

param(
    [string] $Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path,
    [string] $Out  = ""
)

$ErrorActionPreference = "Stop"

$source = Join-Path $Root "Source"
if( -not (Test-Path $source) ) {
    throw "No Source folder under '$Root'. Pass -Root <path to the SML project>."
}
if( $Out -eq "" ) {
    $Out = Join-Path $Root ("SML-headers-" + (Get-Date -Format "yyyyMMdd-HHmm") + ".zip")
}

$headers = Get-ChildItem -Path $source -Recurse -Filter *.h -File
if( $headers.Count -eq 0 ) {
    throw "No .h files found under '$source'."
}

$staging = Join-Path ([IO.Path]::GetTempPath()) ("sml-headers-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $staging | Out-Null
try {
    $sourceFull = (Resolve-Path $source).Path.TrimEnd('\') + '\'
    foreach( $h in $headers ) {
        $relative = $h.FullName.Substring($sourceFull.Length)
        $target = Join-Path $staging $relative
        $targetDir = Split-Path $target -Parent
        if( -not (Test-Path $targetDir) ) { New-Item -ItemType Directory -Path $targetDir | Out-Null }
        Copy-Item $h.FullName $target
    }

    if( Test-Path $Out ) { Remove-Item $Out -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($staging, $Out, [IO.Compression.CompressionLevel]::Optimal, $false)

    $size = [math]::Round((Get-Item $Out).Length / 1MB, 1)
    Write-Host ("{0} header(s) from {1} -> {2} ({3} MB)" -f $headers.Count, $source, $Out, $size)
}
finally {
    Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
}
