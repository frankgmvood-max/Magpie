param([string]$LSFolder, [string]$Output)
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
    if (!(Test-Path -LiteralPath (Join-Path $LSFolder 'LosslessScaling.exe'))) { throw 'LosslessScaling.exe not found.' }
    if (!$Output) { $Output = Join-Path $LSFolder ('NativeDLSS-logs-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '.zip') }
    if (Test-Path -LiteralPath $Output) { throw 'Output ZIP already exists.' }
    $stage = Join-Path ([IO.Path]::GetTempPath()) ('native-logs-' + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $stage | Out-Null
    $inventory = @()
    foreach ($name in @('Lossless.dll','Lossless_original.dll','version.dll','dxgi.dll','native-runtime/nvngx_dlssg.dll','addons/LS_DLSSFG/runtime/nvngx_dlssg.dll','nvngx_dlssg.dll')) {
        $path = Join-Path $LSFolder $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $file = Get-Item -LiteralPath $path
            $inventory += @{ file=$name; size=$file.Length; version=$file.VersionInfo.FileVersion; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
        } else { $inventory += @{ file=$name; exists=$false } }
    }
    $inventory | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'runtime-inventory.json') -Encoding UTF8
    foreach ($name in @('NativeDLSS.ini','dlssg_sm86.ini')) {
        $path = Join-Path $LSFolder $name
        if (Test-Path -LiteralPath $path -PathType Leaf) { Copy-Item -LiteralPath $path -Destination $stage }
    }
    $copied = @(); $skipped = @(); $total = 0L
    foreach ($dir in @('logs','native-dlss-cache','dlssg_sm86/logs')) {
        $base = Join-Path $LSFolder $dir
        if (!(Test-Path -LiteralPath $base -PathType Container)) { continue }
        $files = @(Get-ChildItem -LiteralPath $base -File -Recurse | Where-Object {
            $_.Extension -in @('.log','.jsonl','.txt') -and ($dir -ne 'logs' -or $_.Name -like 'native-dlss-*.log' -or $_.Name -eq 'native-ui.log')
        } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 40)
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($LSFolder.TrimEnd('\').Length + 1)
            if ($file.Length -gt 25MB -or $total + $file.Length -gt 100MB) { $skipped += $relative; continue }
            $target = Join-Path $stage $relative
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
            # Allow the logger to keep writing while taking a bounded snapshot.
            $reader = [IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
            try {
                $sink = [IO.File]::Create($target)
                try {
                    $remaining = $file.Length; $buffer = New-Object byte[] 65536
                    while ($remaining -gt 0) {
                        $n = $reader.Read($buffer,0,[int][Math]::Min($remaining,$buffer.Length))
                        if (!$n) { break }
                        $sink.Write($buffer,0,$n); $remaining -= $n
                    }
                } finally { $sink.Dispose() }
            } finally { $reader.Dispose() }
            $total += (Get-Item -LiteralPath $target).Length; $copied += $relative
        }
    }
    @{ copied=$copied; skipped=$skipped; bytes=$total } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'collection.json') -Encoding UTF8
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $Output
    Write-Host "Логи собраны: $Output"
} catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 1 }
finally { if ($stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force } }
