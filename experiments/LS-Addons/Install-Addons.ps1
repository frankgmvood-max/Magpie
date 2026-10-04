#Requires -Version 5.1
param([Parameter(Mandatory=$true)][string]$LSDirectory)
$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath($LSDirectory)
if (!(Test-Path -LiteralPath (Join-Path $target 'LosslessScaling.exe'))) { throw 'Choose the directory containing LosslessScaling.exe.' }
if (Get-Process LosslessScaling -ErrorAction SilentlyContinue) { throw 'Close Lossless Scaling before installing addons.' }
if (!(Test-Path -LiteralPath (Join-Path $target 'Lossless_original.dll'))) { throw 'Install the supported LS addon manager first. This package does not replace its proxy.' }
$backup = Join-Path $target ('LS-Addons-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$files = @((Get-Item -LiteralPath (Join-Path $PSScriptRoot 'LS_OutputBridge.dll')))
$files += @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'addons') -File -Recurse)
foreach ($file in $files) {
    $relative = $file.FullName.Substring($PSScriptRoot.Length).TrimStart('\','/')
    if ($relative -match '\\source\\|^source\\') { continue }
    $destination = Join-Path $target $relative
    if ((Test-Path -LiteralPath $destination) -and ($file.Extension -eq '.ini' -or $relative -match '^addons\\LS_DLSSFG\\runtime\\')) { continue }
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    if (Test-Path -LiteralPath $destination) {
        $previous = Join-Path $backup $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $previous) -Force | Out-Null
        Copy-Item -LiteralPath $destination -Destination $previous
    }
    Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
}
'Installed. Existing INI files and DLSS FG runtime were preserved. Smooth Motion and RTX HDR start disabled.'
