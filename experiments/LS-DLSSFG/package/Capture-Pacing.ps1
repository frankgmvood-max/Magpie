param(
    [ValidateSet('DLSSFG','LSFG','NoFG')][string]$Mode='DLSSFG',
    [ValidateRange(10,120)][int]$Seconds=60,
    [switch]$PrepareOnly
)
$ErrorActionPreference='Stop'

# A portable official console binary; no service, overlay, injection or install.
$version='2.6.0'
$expected='B2A706BC6AD475749E3B7E3409263AA1E6906D45BDCF993F6DBC0F660188F1AF'
$url='https://github.com/GameTechDev/PresentMon/releases/download/v2.6.0/PresentMon-2.6.0-x64.exe'
$toolDirectory=Join-Path $env:LOCALAPPDATA 'LS-DLSSFG\tools'
$presentMon=Join-Path $toolDirectory ('PresentMon-'+$version+'-x64.exe')

if (!$PrepareOnly) {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=New-Object Security.Principal.WindowsPrincipal($identity)
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host 'Windows requires administrator access for the ETW timing capture.'
        $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'" -Mode '+$Mode+' -Seconds '+$Seconds
        $child=Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        exit $child.ExitCode
    }
}

[IO.Directory]::CreateDirectory($toolDirectory) | Out-Null
if (!(Test-Path -LiteralPath $presentMon) -or (Get-FileHash -LiteralPath $presentMon -Algorithm SHA256).Hash -ne $expected) {
    $temporary=Join-Path $toolDirectory ([Guid]::NewGuid().ToString('N')+'.download')
    try {
        [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $url -OutFile $temporary -UseBasicParsing
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $expected) {throw 'Official PresentMon download hash mismatch.'}
        Move-Item -LiteralPath $temporary -Destination $presentMon -Force
    } finally {if (Test-Path -LiteralPath $temporary) {Remove-Item -LiteralPath $temporary}}
}
$help=(& $presentMon --help 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {throw 'PresentMon cannot start.'}
foreach ($flag in @('--process_id','--timed','--delay','--v2_metrics','--qpc_time','--write_display_metadata','--session_name','--terminate_after_timed','--no_console_stats','--no_track_input','--track_hybrid_present')) {
    if ($help -notmatch [regex]::Escape($flag)) {throw ('PresentMon option unavailable: '+$flag)}
}
if ($PrepareOnly) {Write-Host 'Pinned PresentMon hash and required command-line options verified.';exit 0}

$processes=@(Get-Process -Name LosslessScaling -ErrorAction SilentlyContinue)
if ($processes.Count -ne 1) {throw 'Run exactly one LosslessScaling.exe, start scaling, then repeat capture.'}
$lsProcess=$processes[0]
$lsDirectory=Split-Path -Parent $lsProcess.Path
$desktop=[Environment]::GetFolderPath('Desktop')
if (!$desktop) {$desktop=$env:USERPROFILE}
$capture=Join-Path $desktop ('LS-Pacing-'+$Mode+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6))
[IO.Directory]::CreateDirectory($capture) | Out-Null
$metadata=[ordered]@{
    mode_label=$Mode;mode_label_source='user-selected; not automatically verified'
    start_local=(Get-Date -Format o);pid=$lsProcess.Id;ls_path=$lsProcess.Path
    os=[Environment]::OSVersion.VersionString;presentmon_version=$version;presentmon_sha256=$expected
    capture_seconds=$Seconds;delay_seconds=5
    qpc_frequency=[Diagnostics.Stopwatch]::Frequency
    gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,PNPDeviceID)
    limit='ETW display timestamps and CPU submissions do not directly measure panel electronics.'
}
$metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $capture 'capture.json') -Encoding UTF8
foreach ($relative in @('addons\LS_DLSSFG\LS_DLSSFG.ini','addons\LS_DLSSFG\addon.json','addons\LS_DLSSFG\build-commit.txt','addons\LS_SmoothMotion\LS_SmoothMotion.ini')) {
    $source=Join-Path $lsDirectory $relative
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        try {Copy-Item -LiteralPath $source -Destination (Join-Path $capture ((Split-Path -Parent $relative).Replace('\','_')+'_'+(Split-Path -Leaf $relative)))}
        catch {('Could not copy '+$relative+': '+$_.Exception.Message) | Add-Content -LiteralPath (Join-Path $capture 'collection-warnings.txt')}
    }
}
$traceFolder=Join-Path $env:LOCALAPPDATA 'LS-DLSSFG\frame-traces'
$traceStarted=Get-Date
if ($Mode -eq 'DLSSFG') {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $traceFolder)) | Out-Null
    Set-Content -LiteralPath (Join-Path (Split-Path -Parent $traceFolder) 'capture-trace.request') -Value '60' -Encoding ASCII
}
Write-Host ('Capture '+$Mode+': return to the same moving game scene. Recording starts in 5 seconds and lasts '+$Seconds+' seconds.')
Write-Host 'Keep the manager window and measurement overlays off the game display. No LS or driver settings are changed.'
$csv=Join-Path $capture 'presentmon.csv'
$arguments=@('--process_id',[string]$lsProcess.Id,'--output_file',$csv,'--v2_metrics','--qpc_time','--write_display_metadata','--track_hybrid_present','--no_track_input','--no_console_stats','--session_name',('LS-Pacing-'+[Guid]::NewGuid().ToString('N')),'--delay','5','--timed',[string]$Seconds,'--terminate_after_timed')
& $presentMon @arguments 2>&1 | Out-File -LiteralPath (Join-Path $capture 'presentmon-console.txt') -Encoding UTF8
$exitCode=$LASTEXITCODE
if ($Mode -eq 'DLSSFG') {
    # The addon polls the request within two seconds and its worker finishes
    # after 60 seconds. Wait at most another five seconds for the final footer.
    foreach ($trace in @(Get-ChildItem -LiteralPath $traceFolder -Filter '*.csv' -ErrorAction SilentlyContinue | Where-Object {$_.CreationTime -ge $traceStarted.AddSeconds(-1)})) {
        for ($i=0;$i -lt 5;$i++) {
            try {$tail=Get-Content -LiteralPath $trace.FullName -Tail 1;if ($tail -match 'monitor_refresh_unmeasured') {break}} catch {}
            Start-Sleep -Seconds 1
        }
        Copy-Item -LiteralPath $trace.FullName -Destination $capture
    }
    $request=Join-Path (Split-Path -Parent $traceFolder) 'capture-trace.request'
    if (Test-Path -LiteralPath $request) {Remove-Item -LiteralPath $request; 'Addon did not accept trace request; check addon version and enabled state.' | Set-Content -LiteralPath (Join-Path $capture 'addon-trace-unavailable.txt')}
}
foreach ($folder in @($lsDirectory,(Join-Path $lsDirectory 'logs'))) {
    foreach ($log in @(Get-ChildItem -LiteralPath $folder -Filter 'LSAddonManager*.log' -ErrorAction SilentlyContinue)) {
        try {Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $capture $log.Name) -Force}
        catch {('Could not copy manager log: '+$_.Exception.Message) | Add-Content -LiteralPath (Join-Path $capture 'collection-warnings.txt')}
    }
}
$hasRows=$false
if (Test-Path -LiteralPath $csv) {$hasRows=@(Get-Content -LiteralPath $csv -TotalCount 2).Count -ge 2}
if ($exitCode -ne 0 -or !$hasRows) {
    ('Capture failed or contained no LS frame rows. PresentMon exit code: '+$exitCode) | Set-Content -LiteralPath (Join-Path $capture 'capture-error.txt')
}
$zip=$capture+'.zip'
Compress-Archive -LiteralPath $capture -DestinationPath $zip
Write-Host ('Saved: '+$zip)
Write-Host 'Send this ZIP for analysis. Keep monitor OSD observations separate from the CPU/ETW measurements.'
if ($exitCode -ne 0 -or !$hasRows) {throw 'Timing capture failed. Diagnostic ZIP was still saved.'}
