param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build'),
    [string]$OpenXRLoader = (Join-Path $PSScriptRoot 'tools\openxr\x64\openxr_loader.dll'),
    [switch]$WithoutTests
)

$ErrorActionPreference = 'Stop'
$cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmake)) { throw 'CMake was not found.' }
$OpenXRLoader = (Resolve-Path -LiteralPath $OpenXRLoader).Path
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$testing = if ($WithoutTests) { 'OFF' } else { 'ON' }
& $cmake -S $PSScriptRoot -B $BuildDirectory -A x64 "-DBUILD_TESTING=$testing"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $BuildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
$localLoader = Join-Path $BuildDirectory 'Release\openxr_loader.dll'
if ($OpenXRLoader -ne $localLoader) { Copy-Item -LiteralPath $OpenXRLoader -Destination $localLoader -Force }

$build32 = Join-Path $BuildDirectory 'Win32'
& $cmake -S $PSScriptRoot -B $build32 -A Win32 "-DBUILD_TESTING=$testing"
if ($LASTEXITCODE -ne 0) { throw 'Win32 CMake configuration failed.' }
& $cmake --build $build32 --config Release
if ($LASTEXITCODE -ne 0) { throw 'Win32 native build failed.' }

$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $csc /nologo /target:winexe /platform:x64 /optimize+ "/out:$BuildDirectory\StrideXR.exe" `
    "/resource:$BuildDirectory\Release\StrideXRAudio.dll,StrideXRAudio.dll" `
    "/resource:$BuildDirectory\Release\StrideXR.dll,StrideXR.dll" `
    "/resource:$BuildDirectory\Release\StrideXRCalibrate.exe,StrideXRCalibrate.exe" `
    "/resource:$OpenXRLoader,openxr_loader.dll" `
    "/resource:$PSScriptRoot\third_party\OpenXR-MIT.txt,OpenXR-MIT.txt" `
    "/resource:$PSScriptRoot\third_party\OpenXR-loader-LICENSE.txt,OpenXR-loader-LICENSE.txt" `
    "/resource:$PSScriptRoot\third_party\JsonCpp-LICENSE.txt,JsonCpp-LICENSE.txt" `
    "/resource:$build32\Release\StrideXR.dll,StrideXR-x86.dll" `
    "/resource:$PSScriptRoot\package\StrideXR.json,StrideXR.json" `
    "/resource:$PSScriptRoot\package\StrideXR-x86.json,StrideXR-x86.json" `
    "/resource:$PSScriptRoot\package\StrideXR.ini,StrideXR.ini" `
    "$PSScriptRoot\src\ControlPanel.cs" "$PSScriptRoot\src\Profiles.cs" `
    "$PSScriptRoot\src\LaunchMonitor.cs" "$PSScriptRoot\src\ProfileWindow.cs" "$PSScriptRoot\src\Diagnostics.cs" `
    "$PSScriptRoot\src\CalibrationRun.cs" "$PSScriptRoot\src\AudiobookTab.cs" `
    "$PSScriptRoot\src\AudiobookLink.cs" "$PSScriptRoot\src\AudiobookSubtitles.cs" `
    "$PSScriptRoot\src\BookAudio.cs" "$PSScriptRoot\src\DarkTheme.cs" "$PSScriptRoot\src\SubtitleStyle.cs"
if ($LASTEXITCODE -ne 0) { throw 'Control panel build failed.' }
Get-Item -LiteralPath (Join-Path $BuildDirectory 'StrideXR.exe')
