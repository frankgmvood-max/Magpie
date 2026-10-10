param([string]$LSFolder)
$ErrorActionPreference = 'Stop'
try {
    if (!$LSFolder -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt'))) {
        $LSFolder = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt') -Raw).Trim()
    }
    if (!$LSFolder) {
        Add-Type -AssemblyName System.Windows.Forms
        $picker = New-Object System.Windows.Forms.FolderBrowserDialog
        $picker.Description = 'Select your Lossless Scaling folder'
        if ($picker.ShowDialog() -ne 'OK') { exit 0 }
        $LSFolder = $picker.SelectedPath
    }
    $LSFolder = [IO.Path]::GetFullPath($LSFolder)
    $exe = Join-Path $LSFolder 'LosslessScaling.exe'
    if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'LosslessScaling.exe not found.' }
    if (@(Get-Process -Name LosslessScaling -ErrorAction SilentlyContinue).Count) { throw 'Close Lossless Scaling completely before the diagnostic launch.' }
    $folder = Join-Path $LSFolder ('logs/startup-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff'))
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $exe; $start.WorkingDirectory = $LSFolder
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    # .NET 9 apphost trace applies only to this child process, not the system.
    $start.EnvironmentVariables['COREHOST_TRACE'] = '1'
    $start.EnvironmentVariables['COREHOST_TRACEFILE'] = Join-Path $folder 'native-startup-host.log'
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    Write-Host 'Launching LS with startup diagnostics. Close LS when finished; this console will save the result.'
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [IO.File]::WriteAllText((Join-Path $folder 'native-startup-stdout.log'),$stdout.Result)
    [IO.File]::WriteAllText((Join-Path $folder 'native-startup-stderr.log'),$stderr.Result)
    @{ ExitCode = $process.ExitCode; Executable = $exe; WorkingDirectory = $LSFolder; TimeUtc = [DateTime]::UtcNow.ToString('O') } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $folder 'native-startup-result.json') -Encoding UTF8
    Write-Host "LS exit code: $($process.ExitCode). Diagnostics: $folder"
    Write-Host 'Run Collect-Logs.cmd and attach the resulting ZIP.'
    $process.Dispose()
} catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 1 }
