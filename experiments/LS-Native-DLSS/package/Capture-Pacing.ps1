param([string]$LSFolder, [string]$GameName, [ValidateRange(10,600)][int]$Seconds = 120)
$ErrorActionPreference = 'Stop'
try {
    $presentMon = Join-Path $PSScriptRoot 'PresentMon-2.6.0-x64.exe'
    if (!(Test-Path -LiteralPath $presentMon)) { throw 'PresentMon is missing. Extract the complete package.' }
    if ((Get-FileHash -LiteralPath $presentMon -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af') { throw 'PresentMon hash mismatch.' }
    if (!$LSFolder -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt'))) {
        $LSFolder = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt') -Raw).Trim()
    }
    Add-Type -AssemblyName System.Windows.Forms
    if (!$LSFolder) {
        $picker = New-Object System.Windows.Forms.FolderBrowserDialog
        $picker.Description = 'Select your Lossless Scaling folder'
        if ($picker.ShowDialog() -ne 'OK') { exit 0 }; $LSFolder = $picker.SelectedPath
    }
    $LSFolder = [IO.Path]::GetFullPath($LSFolder)
    if (!(Test-Path -LiteralPath (Join-Path $LSFolder 'LosslessScaling.exe'))) { throw 'LosslessScaling.exe not found.' }
    if (!$GameName) {
        Add-Type -AssemblyName Microsoft.VisualBasic
        $GameName = [Microsoft.VisualBasic.Interaction]::InputBox('Game executable name, e.g. nightreign.exe. Empty = capture LS only.', 'Frame pacing capture', 'nightreign.exe').Trim()
    }
    if ($GameName -and ($GameName -notmatch '^[^\\/:*?"<>|]+\.exe$')) { throw 'Enter an executable name ending in .exe, not a path.' }
    $logs = Join-Path $LSFolder 'logs'; New-Item -ItemType Directory -Path $logs -Force | Out-Null
    $stem = 'pacing-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff')
    $csv = Join-Path $logs ($stem + '.csv')
    $captureArgs = @('--process_name','LosslessScaling.exe','--output_file',$csv,'--qpc_time',
        '--timed',$Seconds.ToString(),'--terminate_after_timed','--no_console_stats','--v2_metrics',
        '--write_display_metadata','--track_hybrid_present','--session_name',('NativeDLSS-' + [Guid]::NewGuid().ToString('N')))
    if ($GameName -and $GameName -ine 'LosslessScaling.exe') { $captureArgs += @('--process_name',$GameName) }
    $result = @{ Tool='PresentMon 2.6.0'; Sha256='b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af';
        Game=$GameName; Seconds=$Seconds; StartedUtc=[DateTime]::UtcNow.ToString('O'); QpcFrequency=[Diagnostics.Stopwatch]::Frequency;
        StartedQpc=[Diagnostics.Stopwatch]::GetTimestamp(); Arguments=$captureArgs }
    Write-Host "Capturing LS and $GameName for $Seconds seconds. Play and reproduce the stutter, then run Collect-Logs.cmd."
    & $presentMon @captureArgs 2>&1 | Tee-Object -FilePath (Join-Path $logs ($stem + '.txt')) | Out-Host
    $result.ExitCode = $LASTEXITCODE
    $result.FinishedUtc = [DateTime]::UtcNow.ToString('O'); $result.FinishedQpc = [Diagnostics.Stopwatch]::GetTimestamp()
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logs ($stem + '.json')) -Encoding UTF8
    if ($result.ExitCode -ne 0) { throw "PresentMon exited with $($result.ExitCode). Check $stem.txt. If ETW access was denied, run Capture-Pacing.cmd as administrator." }
    if (!(Test-Path -LiteralPath $csv)) { throw 'PresentMon did not produce the expected CSV. See its capture log.' }
    Write-Host "Capture saved: $csv"
} catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 1 }
