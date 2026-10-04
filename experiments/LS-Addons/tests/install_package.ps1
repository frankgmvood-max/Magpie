#Requires -Version 5.1
param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$package = [IO.Path]::GetFullPath($PackageDirectory)
$target = Join-Path ([IO.Path]::GetTempPath()) ('LSInstallTest-' + [Guid]::NewGuid().ToString('N'))
function Assert([bool]$condition,[string]$message) { if (!$condition) { throw $message } }
function Put([string]$relative,[string]$text) {
    $path = Join-Path $target $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllText($path,$text)
}
try {
    New-Item -ItemType Directory -Path $target | Out-Null
    Put 'LosslessScaling.exe' 'test fixture; never executed'
    $preserved = @{
        'Lossless_original.dll' = 'original LS manager payload'
        'Lossless.dll' = 'installed manager proxy'
        'dxgi.dll' = 'user proxy untouched'
        'version.dll' = 'user loader untouched'
        'addons/LS_DLSSFG/LS_DLSSFG.ini' = "[FrameGeneration]`nNativeLSFGDisabled=1`nTargetFPS=131`n"
        'addons/LS_DLSSFG/runtime/nvngx_dlssg.dll' = 'working user RTX30 FG mod'
        'addons/LS_DLSSFG/runtime/nvngx_dlss.dll' = 'working companion runtime'
        'addons/LS_DLSSFG/runtime/README.txt' = 'user runtime note'
        'addons/LS_RTXHDR/LS_RTXHDR.ini' = "[RTXHDR]`nEnabled=1`nContrast=87`n"
    }
    foreach ($entry in $preserved.GetEnumerator()) { Put $entry.Key $entry.Value }
    Put 'LS_OutputBridge.dll' 'previous bridge'
    Put 'addons/LS_DLSSFG/LS_DLSSFG.dll' 'previous FG addon'
    & (Join-Path $package 'Install-Addons.ps1') -LSDirectory $target
    foreach ($entry in $preserved.GetEnumerator()) {
        Assert ([IO.File]::ReadAllText((Join-Path $target $entry.Key)) -ceq $entry.Value) ('Installer replaced ' + $entry.Key)
    }
    foreach ($relative in @('LS_OutputBridge.dll','addons/LS_DLSSFG/LS_DLSSFG.dll','addons/LS_RTXHDR/LS_RTXHDR.dll','addons/LS_RTXHDR/LS_RtxVideoRuntime.dll','addons/LS_RTXHDR/runtime/nvngx_truehdr.dll','addons/LS_SmoothMotion/LS_SmoothMotion.dll')) {
        Assert ((Get-FileHash -LiteralPath (Join-Path $target $relative)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $package $relative)).Hash) ('Missing or different installed binary: ' + $relative)
    }
    Assert (!(Test-Path -LiteralPath (Join-Path $target 'source'))) 'Installer copied source tree into LS root'
    Assert (!(Test-Path -LiteralPath (Join-Path $target 'addons/LS_DLSSFG/source'))) 'Installer copied legacy source tree into addons'
    $first = @(Get-ChildItem -LiteralPath $target -Directory -Filter 'LS-Addons-backup-*')
    Assert ($first.Count -eq 1) 'Replacement backup missing'
    Assert ([IO.File]::ReadAllText((Join-Path $first[0].FullName 'LS_OutputBridge.dll')) -ceq 'previous bridge') 'Old bridge not backed up'
    Assert ([IO.File]::ReadAllText((Join-Path $first[0].FullName 'addons/LS_DLSSFG/LS_DLSSFG.dll')) -ceq 'previous FG addon') 'Old FG addon not backed up'
    & (Join-Path $package 'Install-Addons.ps1') -LSDirectory $target
    $again = @(Get-ChildItem -LiteralPath $target -Directory -Filter 'LS-Addons-backup-*')
    Assert ($again.Count -eq 2) 'Repeated install overwrote the first backup'
    Assert ([IO.File]::ReadAllText((Join-Path $first[0].FullName 'addons/LS_DLSSFG/LS_DLSSFG.dll')) -ceq 'previous FG addon') 'Original recovery copy changed'
    foreach ($entry in $preserved.GetEnumerator()) {
        Assert ([IO.File]::ReadAllText((Join-Path $target $entry.Key)) -ceq $entry.Value) ('Repeated install replaced ' + $entry.Key)
    }
    'Package install passed: all real binaries copied, INI/runtime/proxies preserved, repeat install keeps separate backups.'
} finally {
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
}
