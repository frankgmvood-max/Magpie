$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
foreach ($name in @('Install.ps1','Settings.ps1')) {
    $errors = $null; $tokens = $null
    # Windows PowerShell defaults BOM-less files to ANSI. Read source as UTF-8;
    # the distributed scripts receive a UTF-8 BOM during packaging.
    $script = [IO.File]::ReadAllText((Join-Path $project "package/$name"))
    [Management.Automation.Language.Parser]::ParseInput($script, [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count) { throw "Parse errors in ${name}: $errors" }
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('native-package-test-' + [Guid]::NewGuid())
$package = Join-Path $root 'package'; $ls = Join-Path $root 'LS'
New-Item -ItemType Directory -Path $package,$ls -Force | Out-Null
try {
    $original = Join-Path $ls 'Lossless_original.dll'
    [IO.File]::WriteAllText($original, 'fixture original - NOT LS')
    $hash = (Get-FileHash $original -Algorithm SHA256).Hash.ToLowerInvariant()
    $installer = [IO.File]::ReadAllText((Join-Path $project 'package/Install.ps1'))
    # Only this generated test fixture changes the reference; production keeps
    # the exact commercial-original hash and contains no test switch.
    $installer = $installer.Replace('626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb',$hash)
    [IO.File]::WriteAllText((Join-Path $package 'Install.ps1'),$installer,[Text.UTF8Encoding]::new($true))
    [IO.File]::WriteAllText((Join-Path $ls 'LosslessScaling.exe'),'fixture app')
    [IO.File]::WriteAllText((Join-Path $ls 'Lossless.dll'),'old manager proxy')
    [IO.File]::WriteAllText((Join-Path $ls 'NativeDLSS.ini'),'old ini')
    [IO.File]::WriteAllText((Join-Path $ls 'dxgi.dll'),'existing working SM86 proxy')
    [IO.File]::WriteAllText((Join-Path $package 'Lossless.dll'),'new native proxy')
    [IO.File]::WriteAllText((Join-Path $package 'NativeDLSS.ini'),'new ini')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls
    if ($LASTEXITCODE -ne 0) { throw 'Install failed' }
    if ([IO.File]::ReadAllText((Join-Path $ls 'Lossless.dll')) -ne 'new native proxy') { throw 'Wrong installed DLL' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls
    if ($LASTEXITCODE -ne 0) { throw 'Repeated install failed' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls -Restore
    if ($LASTEXITCODE -ne 0) { throw 'Restore failed' }
    if ([IO.File]::ReadAllText((Join-Path $ls 'Lossless.dll')) -ne 'old manager proxy' -or
        [IO.File]::ReadAllText((Join-Path $ls 'NativeDLSS.ini')) -ne 'old ini' -or
        [IO.File]::ReadAllText((Join-Path $ls 'dxgi.dll')) -ne 'existing working SM86 proxy') { throw 'Backup or SM86 not preserved' }
    [IO.File]::WriteAllText($original,'unsupported original')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls
    if ($LASTEXITCODE -ne 1 -or [IO.File]::ReadAllText((Join-Path $ls 'Lossless.dll')) -ne 'old manager proxy') { throw 'Unsupported original was not rejected safely' }
    Write-Host 'Package scripts parse; install, repeated install, rollback, runtime preservation and version rejection passed.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
