param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [switch]$CheckActiveRuntime
)

$ErrorActionPreference = 'Stop'
$app = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'Release\StrideXRCalibrate.exe')).Path
$runtime = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory 'Release\StrideXRTestRuntime.dll')).Path
$scratch = New-Item -ItemType Directory -Force (Join-Path $BuildDirectory ('calibration-test-' + [Guid]::NewGuid().ToString('N')))
$manifest = Join-Path $scratch.FullName 'runtime.json'
$json = @{ file_format_version = '1.0.0'; runtime = @{ library_path = $runtime } } | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText($manifest, $json, [Text.UTF8Encoding]::new($false))

function Invoke-Calibration([switch]$Probe, [switch]$Scene) {
    $arguments = '"' + $scratch.FullName + '"'
    if ($Probe) { $arguments += ' --probe' }
    if ($Scene) { $arguments += ' --scene' }
    $process = Start-Process -FilePath $app -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(45000)) {
        Stop-Process -InputObject $process
        throw 'The calibration test timed out.'
    }
    if ($process.ExitCode -ne 0) {
        $message = Get-Content -LiteralPath (Join-Path $scratch.FullName 'Error.txt') -ErrorAction SilentlyContinue
        throw "Calibration failed ($($process.ExitCode)). $message"
    }
}

$savedRuntime = $env:XR_RUNTIME_JSON
$saved64 = $env:DISABLE_STRIDEXR
$saved32 = $env:DISABLE_STRIDEXR_X86
try {
    $env:DISABLE_STRIDEXR = '1'
    $env:DISABLE_STRIDEXR_X86 = '1'
    if ($CheckActiveRuntime) {
        Remove-Item Env:\XR_RUNTIME_JSON -ErrorAction SilentlyContinue
        Invoke-Calibration -Probe
        Get-Content -LiteralPath (Join-Path $scratch.FullName 'Probe.txt')
    }
    $env:XR_RUNTIME_JSON = $manifest
    Invoke-Calibration
    $values = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $scratch.FullName 'Result.ini')) {
        $key, $value = $line -split '=', 2
        $values[$key] = [double]::Parse($value, [Globalization.CultureInfo]::InvariantCulture)
    }
    if ($values.SlowSpeed -lt .25 -or $values.SlowSpeed -gt .47 -or
        $values.MidSpeed -lt .70 -or $values.MidSpeed -gt .95 -or
        $values.FullSpeed -lt 1.10 -or $values.FullSpeed -gt 1.40 -or
        $values.StartSpeed -ge $values.SlowSpeed) {
        throw 'Calibration did not recover three ordered simulated swing speeds.'
    }
    Get-Content -LiteralPath (Join-Path $scratch.FullName 'Result.ini')
    '[Subtitles]', 'Opacity=76' | Set-Content -Encoding Unicode -LiteralPath (Join-Path $scratch.FullName 'Input.ini')
    $map = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew('Local\StrideXR-Audiobook-v3-Lab', 4160)
    $gate = [Threading.Mutex]::new($false, 'Local\StrideXR-Audiobook-v3-Lab-Lock')
    $view = $map.CreateViewAccessor()
    try {
        $caption = "A live preview line."
        $view.Write(0, [int]1)
        $view.Write(4, [int]1)
        $view.Write(20, [Environment]::TickCount)
        $view.Write(24, [int]$caption.Length)
        for ($i = 0; $i -lt $caption.Length; $i++) { $view.Write(32 + 2 * $i, [uint16][char]$caption[$i]) }
        Invoke-Calibration -Scene
        if ($view.ReadInt32(16) -ne 1 -or $view.ReadInt32(4148) -le 4 -or $view.ReadInt32(4152) -le 0) {
            throw "VR Lab command/layout mismatch. Presses=$($view.ReadInt32(16)) lines=$($view.ReadInt32(4148)) layout=$($view.ReadInt32(4152)) flags=$($view.ReadInt32(12))."
        }
    } finally {
        $view.Dispose()
        $gate.Dispose()
        $map.Dispose()
    }
    $style = (Get-Content -LiteralPath (Join-Path $scratch.FullName 'SubtitleStyle.txt')).Split(',') | ForEach-Object { [int]$_ }
    if ($style.Count -ne 5 -or $style[0] -le 0 -or $style[1] -le 40 -or $style[2] -le 76 -or $style[3] -le 4 -or $style[4] -le 0) {
        throw 'The stereo scene did not save all four edited subtitle settings.'
    }
    "Stereo scene settings: $($style -join ', ')"
} finally {
    $env:XR_RUNTIME_JSON = $savedRuntime
    $env:DISABLE_STRIDEXR = $saved64
    $env:DISABLE_STRIDEXR_X86 = $saved32
}
