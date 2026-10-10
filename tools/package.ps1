<#
.SYNOPSIS
    Release build + zip of Explorer Plus (used by the GitHub workflow; runs the same locally).

.EXAMPLE
    pwsh tools/package.ps1 -Version 1.2.0
    # -> dist/explorer-plus-1.2.0-win-x64.zip and dist/SHA256SUMS.txt

.NOTES
    Needs MinGW-w64 g++ (with windres) and CMake on PATH, plus Ninja or GNU Make.
    Set SIGN_PFX / SIGN_PFX_PASSWORD to sign the exe (Windows SDK signtool).
#>
param(
    [string]$Version = "",          # major.minor.patch; empty: the CMakeLists default
    [string]$Label = "",            # text in the file properties and the zip name (default: the version)
    [int]$Build = 0,                # fourth version number (CI run number)
    [string]$BuildDir = "build-release",
    [string]$OutDir = "dist",
    [string]$Generator = ""         # default: Ninja if available, else Unix Makefiles
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not $Generator) {
        $Generator = if (Get-Command ninja -ErrorAction SilentlyContinue) { "Ninja" } else { "Unix Makefiles" }
    }
    # every version setting is passed each time: CMake caches them across runs
    $cfg = @("-S", ".", "-B", $BuildDir, "-G", $Generator, "-DCMAKE_BUILD_TYPE=Release",
             "-DEXPLORERPLUS_BUILD=$Build", "-DEXPLORERPLUS_VERSION_LABEL=$Label")
    $cfg += if ($Version) { "-DEXPLORERPLUS_VERSION=$Version" } else { "-UEXPLORERPLUS_VERSION" }
    & cmake @cfg
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    & cmake --build $BuildDir --config Release -j 4
    if ($LASTEXITCODE) { throw "build failed" }

    $exe = Join-Path $BuildDir "explorer-plus.exe"
    if (-not (Test-Path $exe)) { throw "$exe was not produced" }

    # optional Authenticode signing: SIGN_PFX (path to a .pfx) + SIGN_PFX_PASSWORD
    if ($env:SIGN_PFX) {
        $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $signtool) { throw "signtool.exe not found (Windows SDK)" }
        & $signtool.FullName sign /fd SHA256 /f $env:SIGN_PFX /p $env:SIGN_PFX_PASSWORD `
            /tr http://timestamp.digicert.com /td SHA256 /d "Explorer Plus" $exe
        if ($LASTEXITCODE) { throw "signing failed" }
    }
    $ver = (Get-Item $exe).VersionInfo.ProductVersion
    $name = "explorer-plus-$ver-win-x64"

    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $stage = Join-Path $OutDir $name
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory $stage | Out-Null
    Copy-Item $exe $stage
    foreach ($f in "README.md", "LICENSE") { if (Test-Path $f) { Copy-Item $f $stage } }

    $zip = Join-Path $OutDir "$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
    Remove-Item -Recurse -Force $stage
    $zips = @($zip)

    # the split-off debug info: crash dumps from this build are resolved against it
    $dbg = Join-Path $BuildDir "explorer-plus.debug"
    if (Test-Path $dbg) {
        $symZip = Join-Path $OutDir "$name-symbols.zip"
        if (Test-Path $symZip) { Remove-Item -Force $symZip }
        Compress-Archive -Path $dbg -DestinationPath $symZip
        $zips += $symZip
    }

    $sums = foreach ($z in $zips) {
        "{0}  {1}" -f (Get-FileHash $z -Algorithm SHA256).Hash.ToLower(), (Split-Path -Leaf $z)
    }
    Set-Content -Path (Join-Path $OutDir "SHA256SUMS.txt") -Value $sums -Encoding ascii
    Write-Host "packaged $zip ($ver)"
    if ($env:GITHUB_OUTPUT) {
        "zip=$zip" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
        "version=$ver" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
    }
} finally {
    Pop-Location
}
