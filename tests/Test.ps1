param(
    [Parameter(Mandatory)][string]$Loader,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [switch]$CheckActiveRuntime,
    [switch]$Disable64BitLayer
)

$ErrorActionPreference = 'Stop'
$loader = (Resolve-Path -LiteralPath $Loader).Path
$runtime = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'Release\StrideXRTestRuntime.dll')).Path
$test = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'Release\StrideXRTest.exe')).Path
$scratch = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('test-settings-' + [guid]::NewGuid().ToString('N'))
$data = New-Item -ItemType Directory -Force -Path (Join-Path $scratch 'StrideXR')
$runtimeManifest = Join-Path $scratch 'runtime.json'
$ini = Join-Path $data.FullName 'StrideXR.ini'

function Invoke-Check([string]$Mode, [string]$RuntimeDll) {
    Write-Output "Testing $Mode"
    & $test $loader $RuntimeDll $Mode
    if ($LASTEXITCODE -ne 0) { throw "$Mode failed ($LASTEXITCODE)." }
}

$savedLocalAppData = $env:LOCALAPPDATA
$savedRuntimeManifest = $env:XR_RUNTIME_JSON
$savedDisable64 = $env:DISABLE_STRIDEXR
$debug = $null
try {
    # TTWVR switches off the incompatible x64 layer before opening OpenXR.
    if ($Disable64BitLayer) { $env:DISABLE_STRIDEXR = '1' }
    Invoke-Check probe '-'
    $env:LOCALAPPDATA = $scratch
    $debugId = [guid]::NewGuid().ToString('N')
    $debugDirectory = New-Item -ItemType Directory -Force (Join-Path $data.FullName "Diagnostics/$debugId")
    $debug = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, "Local\StrideXR-Debug-$debugId")
    @('[Capture]', "Id=$debugId") | Set-Content -Encoding Unicode -LiteralPath (Join-Path $data.FullName 'DebugCapture.ini')
    if ($CheckActiveRuntime) {
        Remove-Item Env:\XR_RUNTIME_JSON -ErrorAction SilentlyContinue
        "[StrideXR]`r`nGames=StrideXRTest.exe" | Set-Content -Encoding Unicode -LiteralPath $ini
        Invoke-Check vdxr '-'
        Get-Content -LiteralPath (Join-Path $data.FullName 'StrideXRTest.exe.status')
    }
    @{ file_format_version = '1.0.0'; runtime = @{
        library_path = $runtime; functions = @{
            xrNegotiateLoaderRuntimeInterface = 'xrNegotiateLoaderRuntimeInterface'
        }
    }} | ConvertTo-Json -Depth 5 | Set-Content -Encoding Unicode -LiteralPath $runtimeManifest
    # OpenXR manifests are UTF-8.
    $json = Get-Content -LiteralPath $runtimeManifest -Raw
    [IO.File]::WriteAllText($runtimeManifest, $json, [Text.UTF8Encoding]::new($false))
    $env:XR_RUNTIME_JSON = $runtimeManifest
    if ($Disable64BitLayer) {
        $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew('Local\StrideXR-Audiobook-v3', 4160)
        $gate = [Threading.Mutex]::new($false, 'Local\StrideXR-Audiobook-v3-Lock')
        $view = $mapping.CreateViewAccessor()
        try {
            $fallout = Join-Path $scratch 'FalloutNV.exe'
            Copy-Item -LiteralPath $test -Destination $fallout
            '[StrideXR]', 'Games=OtherGame.exe' | Set-Content -Encoding Unicode -LiteralPath $ini
            $caption = 'The subtitle is visible in the headset.'
            $view.Write(0, [int]1)
            $view.Write(4, [int]1)
            $view.Write(20, [Environment]::TickCount)
            $view.Write(24, [int]$caption.Length)
            $view.Write(4128, [int]25)
            $view.Write(4132, [int]45)
            $view.Write(4136, [int]50)
            $view.Write(4140, [int]3)
            for ($i = 0; $i -lt $caption.Length; $i++) { $view.Write(32 + 2 * $i, [uint16][char]$caption[$i]) }
            & $fallout $loader $runtime audiobook
            if ($LASTEXITCODE -ne 0) { throw "Audiobook OpenXR test failed ($LASTEXITCODE)." }
        } finally {
            $view.Dispose()
            $gate.Dispose()
            $mapping.Dispose()
        }
    }
    foreach ($mode in 'default', 'shipwright', 'shipwright-tuned', 'either', 'reverse', 'disabled', 'reject-bindings', 'reject-attach') {
        $game = if ($mode -eq 'disabled') { 'Dishonored2.exe' } else { '"OtherGame.exe;StrideXRTest.exe"' }
        $gate = if ($mode -eq 'either') { 'Either' } else { 'Both' }
        $settings = @('[StrideXR]', "Games=$game", "Thumbrests=$gate")
        if ($mode -eq 'shipwright-tuned') {
            $settings += @('StartSpeed=0.20', 'FullSpeed=3.00', 'MinimumStick=0.151', 'MaximumStick=0.53')
        }
        $settings | Set-Content -Encoding Unicode -LiteralPath $ini
        Invoke-Check $mode $runtime
    }
    $profiles = Join-Path $data.FullName 'Profiles.ini'
    foreach ($mode in 'profile-assigned', 'profile-global', 'profile-canceled', 'profile-capture') {
        $useGlobal = [int]($mode -eq 'profile-global')
        $target = if ($mode -eq 'profile-assigned') { $test } else { 'DifferentGame.exe' }
        $basename = if ($mode -eq 'profile-assigned') { 'StrideXRTest.exe' } else { 'OtherGame.exe' }
        @"
[Profiles]
Ids="fallback;pending;basename;exact"
Default=fallback
Global=$useGlobal
Capture=pending
[Profile:fallback]
Name=Default
FullSpeed=0.50
MinimumStick=0.20
MaximumStick=0.62
StopDelay=0.10
StopFade=0.10
[Profile:pending]
Name=Pending
FullSpeed=0.50
MinimumStick=0.20
MaximumStick=0.51
StopDelay=0.10
StopFade=0.10
[Profile:basename]
Name=Filename Match
TargetCount=1
Target0="$basename"
FullSpeed=0.50
MinimumStick=0.20
MaximumStick=0.35
StopDelay=0.10
StopFade=0.10
[Profile:exact]
Name=Exact Path
TargetCount=1
Target0="$target"
FullSpeed=0.50
MinimumStick=0.20
MaximumStick=0.44
StopDelay=0.10
StopFade=0.10
"@ | Set-Content -Encoding Unicode -LiteralPath $profiles
        $capture = if ($mode -eq 'profile-capture') {
            [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, 'Local\StrideXR-pending')
        }
        try { Invoke-Check $mode $runtime }
        finally { if ($capture) { $capture.Dispose() } }
    }
    $trace = (Get-ChildItem -LiteralPath $debugDirectory.FullName -Filter 'Layer-*.log' | Get-Content -Raw) -join "`n"
    if ($trace -notmatch 'xrAttachSessionActionSets with StrideXR returned 0' -or
        $trace -notmatch 'Peak signed stick value returned to game') { throw 'The diagnostic capture is missing the input stages.' }
    if ($Disable64BitLayer -and $trace -notmatch 'x86 layer entered .*x64 disable switch present=yes') {
        throw 'The x86 layer was not traced under the TTWVR startup condition.'
    }
} finally {
    if ($debug) { $debug.Dispose() }
    $env:LOCALAPPDATA = $savedLocalAppData
    $env:XR_RUNTIME_JSON = $savedRuntimeManifest
    $env:DISABLE_STRIDEXR = $savedDisable64
}
