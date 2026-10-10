param([switch]$Restore, [string]$LSFolder)
$ErrorActionPreference = 'Stop'
$reference = '626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb'
$managedReference = 'e4ea2dbb1371ea1920d73c87202f19b6f095ef6f521a8a7a139d19f34f1fe866'
$names = @('Lossless.dll','LosslessScaling.dll','NativeDLSS.UI.dll','NativeDLSS.ini','Lossless_original.dll')
$stage = $null
function Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function RestoreFiles([string]$backup, $present, $files) {
    foreach ($name in $files) {
        $dest = Join-Path $LSFolder $name
        if ($present -contains $name) { Copy-Item -LiteralPath (Join-Path $backup $name) -Destination $dest -Force }
        elseif (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Force }
    }
}
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
    if (@(Get-Process -Name LosslessScaling -ErrorAction SilentlyContinue).Count) { throw 'Close Lossless Scaling completely before installing or restoring.' }
    $backupRoot = Join-Path $LSFolder 'native-dlss-backups'
    $backups = @()
    if (Test-Path -LiteralPath $backupRoot) {
        $backups = @(Get-ChildItem -LiteralPath $backupRoot -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'manifest.json') } | Sort-Object Name -Descending)
    }
    if ($Restore) {
        if (!$backups.Count) { throw 'No NativeDLSS backup found.' }
        $backup = $backups[0].FullName
        $manifest = Get-Content -LiteralPath (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
        $installed = Join-Path $LSFolder 'Lossless.dll'
        if (!(Test-Path -LiteralPath $installed) -or (Hash $installed) -ne $manifest.InstalledHash) { throw 'Lossless.dll changed since installation; no files restored.' }
        if ($manifest.ManagedInstalledHash) {
            foreach ($entry in @(@('LosslessScaling.dll',$manifest.ManagedInstalledHash),@('NativeDLSS.UI.dll',$manifest.HelperInstalledHash),@('Lossless_original.dll',$reference))) {
                $path = Join-Path $LSFolder $entry[0]
                if (!(Test-Path -LiteralPath $path) -or (Hash $path) -ne $entry[1]) { throw "$($entry[0]) changed since installation; no files restored." }
            }
            # The old enum cannot deserialize DLSS. Preserve all current user
            # settings, replacing just this FG value before restoring old UI.
            if ((Hash (Join-Path $backup 'LosslessScaling.dll')) -eq $managedReference -and $manifest.SettingsPath -and (Test-Path -LiteralPath $manifest.SettingsPath)) {
                $xml = New-Object Xml.XmlDocument
                $xml.PreserveWhitespace = $true; $xml.Load($manifest.SettingsPath)
                $changed = $false
                foreach ($node in @($xml.SelectNodes('//*[local-name()="FrameGeneration"]'))) {
                    if ($node.InnerText -eq 'DLSS') { $node.InnerText = 'LSFG3'; $changed = $true }
                }
                if ($changed) {
                    Copy-Item -LiteralPath $manifest.SettingsPath -Destination (Join-Path $backup 'LS-Settings-before-restore.xml') -Force
                    $settingsTemp = $manifest.SettingsPath + '.native-dlss-restore.tmp'
                    $xml.Save($settingsTemp); Move-Item -LiteralPath $settingsTemp -Destination $manifest.SettingsPath -Force
                }
            }
            RestoreFiles $backup @($manifest.Present) $names
        } else {
            # Compatibility with existing 0.2.x backup manifests.
            $present = @('Lossless.dll'); if (Test-Path -LiteralPath (Join-Path $backup 'NativeDLSS.ini')) { $present += 'NativeDLSS.ini' }
            RestoreFiles $backup $present @('Lossless.dll','NativeDLSS.ini')
        }
        Rename-Item -LiteralPath (Join-Path $backup 'manifest.json') -NewName 'restored-manifest.json'
        Write-Host 'Previous native proxy, LS interface and settings restored. SM86 files preserved.'
        exit 0
    }
    if ($LSFolder.TrimEnd('\') -eq $PSScriptRoot.TrimEnd('\')) { throw 'Extract the package to a separate folder, then run Install.cmd and select the LS folder.' }
    foreach ($name in @('Lossless.dll','NativeDLSS.ini','NativeDLSS.UI.dll','UI-Patcher.cs','Mono.Cecil.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot $name))) { throw "Incomplete package: $name is missing." }
    }
    $native = Join-Path $LSFolder 'Lossless_original.dll'
    $installed = Join-Path $LSFolder 'Lossless.dll'
    $nativeInput = $native
    if (!(Test-Path -LiteralPath $native)) { $nativeInput = $installed }
    if (!(Test-Path -LiteralPath $nativeInput) -or (Hash $nativeInput) -ne $reference) { throw 'Unsupported original native DLL. This build targets your LS 3.2.2 archive; nothing replaced.' }
    $managed = Join-Path $LSFolder 'LosslessScaling.dll'
    if (!(Test-Path -LiteralPath $managed)) { throw 'LosslessScaling.dll is missing.' }
    $managedInput = $managed
    if ((Hash $managedInput) -ne $managedReference) {
        # An upgrade may only reconstruct from an authenticated original backup.
        if (!$backups.Count) { throw 'Unsupported managed UI version; nothing replaced.' }
        $last = Get-Content -LiteralPath (Join-Path $backups[0].FullName 'manifest.json') -Raw | ConvertFrom-Json
        if (!$last.ManagedInstalledHash -or (Hash $managed) -ne $last.ManagedInstalledHash) { throw 'LS interface changed outside this installer; nothing replaced.' }
        $originals = @($backups | Where-Object {
            $p = Join-Path $_.FullName 'LosslessScaling.dll'
            (Test-Path -LiteralPath $p) -and (Hash $p) -eq $managedReference
        })
        if (!$originals.Count) { throw 'Original managed UI backup not found.' }
        $managedInput = Join-Path $originals[0].FullName 'LosslessScaling.dll'
    }
    # Explorer can propagate the downloaded ZIP's Mark of the Web to DLLs.
    # Windows PowerShell 5.1 then rejects Cecil LoadFrom with 0x80131515.
    # Only unblock the three shipped DLLs, after checking the LS version and
    # before loading Cecil or copying the UI helper. Leave LS/SM86 files alone.
    foreach ($name in @('Mono.Cecil.dll','NativeDLSS.UI.dll','Lossless.dll')) {
        $path = Join-Path $PSScriptRoot $name
        try { Unblock-File -LiteralPath $path -ErrorAction Stop }
        catch { throw "Cannot unblock package file ${name}. Open the downloaded ZIP's Properties, select Unblock, extract it again, and rerun Install.cmd. $($_.Exception.Message)" }
    }
    $stage = Join-Path $LSFolder ('native-dlss-stage-' + [Guid]::NewGuid())
    New-Item -ItemType Directory -Path $stage | Out-Null
    $cecil = Join-Path $PSScriptRoot 'Mono.Cecil.dll'
    [void][Reflection.Assembly]::LoadFrom($cecil)
    $patcher = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'UI-Patcher.cs'))
    Add-Type -TypeDefinition $patcher -ReferencedAssemblies @($cecil,'System.Core','System')
    $patched = Join-Path $stage 'LosslessScaling.dll'
    [NativeDLSS.Install.Patcher]::Patch($managedInput,$patched,(Join-Path $PSScriptRoot 'NativeDLSS.UI.dll'))
    $hash = Hash (Join-Path $PSScriptRoot 'Lossless.dll'); $managedHash = Hash $patched
    $helperHash = Hash (Join-Path $PSScriptRoot 'NativeDLSS.UI.dll')
    if ((Test-Path -LiteralPath $installed) -and (Hash $installed) -eq $hash -and (Hash $managed) -eq $managedHash -and
        (Test-Path -LiteralPath (Join-Path $LSFolder 'NativeDLSS.UI.dll')) -and (Hash (Join-Path $LSFolder 'NativeDLSS.UI.dll')) -eq $helperHash) {
        Write-Host 'This build is already installed. Select Frame Generation > Type > DLSS in LS.'; exit 0
    }
    $backup = Join-Path $backupRoot ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff'))
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    $present = @()
    foreach ($name in $names) {
        $existing = Join-Path $LSFolder $name
        if (Test-Path -LiteralPath $existing) { Copy-Item -LiteralPath $existing -Destination (Join-Path $backup $name); $present += $name }
    }
    $product = [Diagnostics.FileVersionInfo]::GetVersionInfo($managedInput).ProductName
    $settingsPath = $null
    if ($product) {
        $settingsPath = Join-Path (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) $product) 'Settings.xml'
        if (Test-Path -LiteralPath $settingsPath) { Copy-Item -LiteralPath $settingsPath -Destination (Join-Path $backup 'LS-Settings-before-install.xml') }
    }
    try {
        if (!(Test-Path -LiteralPath $native)) { Copy-Item -LiteralPath $nativeInput -Destination $native }
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Lossless.dll') -Destination $installed -Force
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NativeDLSS.UI.dll') -Destination (Join-Path $LSFolder 'NativeDLSS.UI.dll') -Force
        Copy-Item -LiteralPath $patched -Destination $managed -Force
        if (!(Test-Path -LiteralPath (Join-Path $LSFolder 'NativeDLSS.ini'))) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NativeDLSS.ini') -Destination (Join-Path $LSFolder 'NativeDLSS.ini') }
        if ((Hash $installed) -ne $hash -or (Hash $managed) -ne $managedHash -or (Hash (Join-Path $LSFolder 'NativeDLSS.UI.dll')) -ne $helperHash) { throw 'Installed file checksum mismatch.' }
        @{ Version = 3; InstalledHash = $hash; ManagedInstalledHash = $managedHash; HelperInstalledHash = $helperHash;
           NativeHash = $reference; OriginalManagedHash = $managedReference; Folder = $LSFolder; Present = $present; SettingsPath = $settingsPath } |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding UTF8
    } catch { RestoreFiles $backup $present $names; throw }
    $LSFolder | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'InstalledFolder.txt') -Encoding UTF8
    Write-Host 'NativeDLSS installed. In LS choose Frame Generation > Type > DLSS.'
    Write-Host 'Choose an existing type to use native LS frame generation. Optical Flow quality changes require a full LS restart.'
    Write-Host "Backup: $backup"
} catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 1 }
finally { if ($stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force } }
