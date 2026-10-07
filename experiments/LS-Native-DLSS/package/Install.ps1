param([switch]$Restore, [string]$LSFolder)
$ErrorActionPreference = 'Stop'
$reference = '626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb'
function Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
try {
    if (!$LSFolder) {
        if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'LosslessScaling.exe')) { $LSFolder = $PSScriptRoot }
        else {
            Add-Type -AssemblyName System.Windows.Forms
            $picker = New-Object System.Windows.Forms.FolderBrowserDialog
            $picker.Description = 'Select your Lossless Scaling folder (contains LosslessScaling.exe)'
            if ($picker.ShowDialog() -ne 'OK') { exit 0 }
            $LSFolder = $picker.SelectedPath
        }
    }
    $LSFolder = [IO.Path]::GetFullPath($LSFolder)
    if (!(Test-Path -LiteralPath (Join-Path $LSFolder 'LosslessScaling.exe'))) { throw 'LosslessScaling.exe not found in selected folder.' }
    $running = @(Get-Process -Name LosslessScaling -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Close Lossless Scaling completely before installing or restoring.' }
    $backupRoot = Join-Path $LSFolder 'native-dlss-backups'
    if ($Restore) {
        $backups = @(Get-ChildItem -LiteralPath $backupRoot -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'manifest.json') } | Sort-Object Name -Descending)
        if (!$backups.Count) { throw 'No NativeDLSS backup found.' }
        $backup = $backups[0].FullName
        $manifest = Get-Content -LiteralPath (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
        $installed = Join-Path $LSFolder 'Lossless.dll'
        if ((Hash $installed) -ne $manifest.InstalledHash) { throw 'Lossless.dll changed since installation. No files restored; select the backup manually.' }
        foreach ($name in @('Lossless.dll', 'NativeDLSS.ini')) {
            $saved = Join-Path $backup $name
            if (Test-Path -LiteralPath $saved) { Copy-Item -LiteralPath $saved -Destination (Join-Path $LSFolder $name) -Force }
            elseif ($name -eq 'NativeDLSS.ini') { Remove-Item -LiteralPath (Join-Path $LSFolder $name) -ErrorAction SilentlyContinue }
        }
        Rename-Item -LiteralPath (Join-Path $backup 'manifest.json') -NewName 'restored-manifest.json'
        Write-Host 'Previous Lossless.dll and settings restored. Your SM86 files were preserved.'
        exit 0
    }
    if ($LSFolder.TrimEnd('\') -eq $PSScriptRoot.TrimEnd('\')) { throw 'Extract this package to a separate folder, then run Install.cmd and select the LS folder.' }
    $native = Join-Path $LSFolder 'Lossless_original.dll'
    $installed = Join-Path $LSFolder 'Lossless.dll'
    if (Test-Path -LiteralPath $native) {
        if ((Hash $native) -ne $reference) { throw 'Unsupported Lossless_original.dll. This build targets the DLL from your LS 3.2.2 archive.' }
    } elseif ((Test-Path -LiteralPath $installed) -and (Hash $installed) -eq $reference) {
        Copy-Item -LiteralPath $installed -Destination $native
    } else { throw 'Matching original LS DLL not found. Installation aborted before replacing any files.' }
    $sourceDLL = Join-Path $PSScriptRoot 'Lossless.dll'
    if (!(Test-Path -LiteralPath $sourceDLL) -or !(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'NativeDLSS.ini'))) { throw 'Incomplete package.' }
    $hash = Hash $sourceDLL
    if ((Test-Path -LiteralPath $installed) -and (Hash $installed) -eq $hash) { Write-Host 'This build is already installed. Use Settings.cmd to configure it.'; exit 0 }
    $backup = Join-Path $backupRoot ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff'))
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    foreach ($name in @('Lossless.dll', 'NativeDLSS.ini')) {
        $existing = Join-Path $LSFolder $name
        if (Test-Path -LiteralPath $existing) { Copy-Item -LiteralPath $existing -Destination (Join-Path $backup $name) }
    }
    @{ InstalledHash = $hash; NativeHash = $reference; Folder = $LSFolder } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding UTF8
    try {
        Copy-Item -LiteralPath $sourceDLL -Destination $installed -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NativeDLSS.ini') -Destination (Join-Path $LSFolder 'NativeDLSS.ini') -Force
        if ((Hash $installed) -ne $hash) { throw 'Installed DLL checksum mismatch.' }
    } catch {
        foreach ($name in @('Lossless.dll', 'NativeDLSS.ini')) {
            $saved = Join-Path $backup $name
            if (Test-Path -LiteralPath $saved) { Copy-Item -LiteralPath $saved -Destination (Join-Path $LSFolder $name) -Force }
        }
        throw
    }
    $LSFolder | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt') -Encoding UTF8
    Write-Host 'NativeDLSS installed. Native LSFG3 must remain ON: Fixed x2, HDR OFF.'
    Write-Host 'Keep your working SM86 dxgi.dll and runtime files. The addon manager is not loaded by this proxy.'
    Write-Host "Backup: $backup"
} catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 1 }
