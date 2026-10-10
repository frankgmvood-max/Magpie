$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
foreach ($name in @('Install.ps1','Settings.ps1','Collect-Logs.ps1')) {
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
    if ([IO.File]::ReadAllText((Join-Path $ls 'NativeDLSS.ini')) -ne 'old ini') { throw 'Upgrade overwrote existing settings and profiles' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls
    if ($LASTEXITCODE -ne 0) { throw 'Repeated install failed' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls -Restore
    if ($LASTEXITCODE -ne 0) { throw 'Restore failed' }
    if ([IO.File]::ReadAllText((Join-Path $ls 'Lossless.dll')) -ne 'old manager proxy' -or
        [IO.File]::ReadAllText((Join-Path $ls 'NativeDLSS.ini')) -ne 'old ini' -or
        [IO.File]::ReadAllText((Join-Path $ls 'dxgi.dll')) -ne 'existing working SM86 proxy') { throw 'Backup or SM86 not preserved' }
    New-Item -ItemType Directory -Path (Join-Path $ls 'logs'),(Join-Path $ls 'native-dlss-cache/nested'),(Join-Path $ls 'dlssg_sm86/logs') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $ls 'logs/native-dlss-fixture.log'),'native log')
    [IO.File]::WriteAllText((Join-Path $ls 'native-dlss-cache/nested/ngx.log'),'NGX log')
    [IO.File]::WriteAllText((Join-Path $ls 'dlssg_sm86/logs/backend_fixture.jsonl'),'SM86 log')
    [IO.File]::WriteAllText((Join-Path $ls 'logs/unrelated.txt'),'must not collect')
    $collector = [IO.File]::ReadAllText((Join-Path $project 'package/Collect-Logs.ps1'))
    [IO.File]::WriteAllText((Join-Path $package 'Collect-Logs.ps1'),$collector,[Text.UTF8Encoding]::new($true))
    $zip = Join-Path $root 'logs.zip'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Collect-Logs.ps1') -LSFolder $ls -Output $zip
    if ($LASTEXITCODE -ne 0) { throw 'Collector failed' }
    $unzip = Join-Path $root 'unpacked'; Expand-Archive -LiteralPath $zip -DestinationPath $unzip
    foreach ($name in @('logs/native-dlss-fixture.log','native-dlss-cache/nested/ngx.log','dlssg_sm86/logs/backend_fixture.jsonl','runtime-inventory.json','NativeDLSS.ini')) {
        if (!(Test-Path -LiteralPath (Join-Path $unzip $name))) { throw "Missing collected file: $name" }
    }
    if (@(Get-ChildItem $unzip -File -Recurse -Filter '*.dll').Count -or (Test-Path (Join-Path $unzip 'logs/unrelated.txt'))) { throw 'Unrequested binaries/files collected' }
    if ([IO.File]::ReadAllText((Join-Path $ls 'NativeDLSS.ini')) -ne 'old ini') { throw 'Collector changed settings' }
    [IO.File]::WriteAllText($original,'unsupported original')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls
    if ($LASTEXITCODE -ne 1 -or [IO.File]::ReadAllText((Join-Path $ls 'Lossless.dll')) -ne 'old manager proxy') { throw 'Unsupported original was not rejected safely' }
    Write-Host 'Package scripts parse; install, repeated install, rollback, runtime preservation and version rejection passed.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
