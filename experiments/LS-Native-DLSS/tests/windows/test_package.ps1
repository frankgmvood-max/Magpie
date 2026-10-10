param([Parameter(Mandatory=$true)][string]$ManagedBuild)
$ErrorActionPreference = 'Stop'
$ManagedBuild = [IO.Path]::GetFullPath($ManagedBuild)
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
    Copy-Item -LiteralPath (Join-Path $ManagedBuild 'fixture/NativeUIFixture.dll') -Destination (Join-Path $ls 'LosslessScaling.dll')
    $originalManagedHash = (Get-FileHash (Join-Path $ls 'LosslessScaling.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    $installer = $installer.Replace('e4ea2dbb1371ea1920d73c87202f19b6f095ef6f521a8a7a139d19f34f1fe866',$originalManagedHash).Replace('::Patch(','::PatchFixture(')
    foreach ($name in @('Mono.Cecil.dll','UI-Patcher.cs','NativeDLSS.UI.dll')) { Copy-Item -LiteralPath (Join-Path $ManagedBuild $name) -Destination $package }
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
    # Same patcher and installed managed payload, executed with actual WPF.
    $uiRun = Join-Path $root 'wpf-run'; New-Item -ItemType Directory -Path $uiRun | Out-Null
    Copy-Item (Join-Path $ManagedBuild 'fixture/*') $uiRun
    Copy-Item -LiteralPath (Join-Path $ls 'LosslessScaling.dll') -Destination (Join-Path $uiRun 'NativeUIFixture.dll') -Force
    Copy-Item -LiteralPath (Join-Path $ManagedBuild '../native-observer/Release/Lossless.dll') -Destination $uiRun
    Copy-Item -LiteralPath (Join-Path $project 'observer/NativeDLSS.ini') -Destination $uiRun
    & dotnet (Join-Path $uiRun 'NativeUIFixture.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Patched WPF fixture failed' }
    $product = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $ManagedBuild 'fixture/NativeUIFixture.dll')).ProductName
    $settingsFolder = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) $product
    New-Item -ItemType Directory -Path $settingsFolder -Force | Out-Null
    $settingsFile = Join-Path $settingsFolder 'Settings.xml'
    [IO.File]::WriteAllText($settingsFile,'<Settings><Profile><FrameGeneration>DLSS</FrameGeneration><ClipCursor>true</ClipCursor><NativeDLSSFlowPreset>3</NativeDLSSFlowPreset></Profile></Settings>')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls -Restore
    if ($LASTEXITCODE -ne 0) { throw 'Restore failed' }
    if ([IO.File]::ReadAllText((Join-Path $ls 'Lossless.dll')) -ne 'old manager proxy' -or
        [IO.File]::ReadAllText((Join-Path $ls 'NativeDLSS.ini')) -ne 'old ini' -or
        [IO.File]::ReadAllText((Join-Path $ls 'dxgi.dll')) -ne 'existing working SM86 proxy') { throw 'Backup or SM86 not preserved' }
    if ((Get-FileHash (Join-Path $ls 'LosslessScaling.dll') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $originalManagedHash -or (Test-Path (Join-Path $ls 'NativeDLSS.UI.dll'))) { throw 'Managed UI/helper rollback failed' }
    [xml]$restoredSettings = Get-Content $settingsFile -Raw
    if ($restoredSettings.Settings.Profile.FrameGeneration -ne 'LSFG3' -or $restoredSettings.Settings.Profile.ClipCursor -ne 'true') { throw 'Rollback lost settings or left an unsupported DLSS enum' }
    Remove-Item -LiteralPath $settingsFolder -Recurse -Force
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
    # Construct the actual WinForms controls and exercise profile load/save in
    # a separate Windows PowerShell process. Only the modal loop is replaced;
    # the distributed script keeps no hidden validation mode or test switch.
    $uiFolder = Join-Path $root 'ui'; New-Item -ItemType Directory -Path $uiFolder | Out-Null
    $uiIni = Join-Path $uiFolder 'NativeDLSS.ini'
    $uiConfig = @'
[NativeDLSS]
Enabled=1
Mode=2
Quality=5
HUD1=0,0,5000,10000
ActiveProfile=WoW.exe
[Profile:WoW.exe]
Mode=1
AnalysisPercent=75
HUD1=
'@
    [IO.File]::WriteAllText($uiIni,$uiConfig,[Text.UnicodeEncoding]::new($false,$true))
    $uiChecks = @'
$form.CreateControl()
if ($tabs.TabPages.Count -ne 4 -or $rects.Rows.Count -ne 8) { throw 'Settings controls missing' }
if ($mode.SelectedIndex -ne 1 -or $preset.SelectedIndex -ne 2 -or $grid.SelectedIndex -ne 1 -or $scale.Value -ne 75) { throw 'Profile inheritance or legacy quality migration failed' }
if ([string]$rects.Rows[0].Cells[0].Value) { throw 'Empty HUD profile override was not preserved' }
$profile.Text='NewGame.exe';$mode.SelectedIndex=2;$preset.SelectedIndex=0;$grid.SelectedIndex=0;$scale.Value=50
$rects.Rows[0].Cells[0].Value='10';$rects.Rows[0].Cells[1].Value='20';$rects.Rows[0].Cells[2].Value='30';$rects.Rows[0].Cells[3].Value='40'
SaveProfile
if ((ReadIni 'NativeDLSS' 'ActiveProfile' '') -ne 'NewGame.exe' -or (ReadIni 'Profile:NewGame.exe' 'HUD1' '') -ne '1000,2000,3000,4000') { throw 'UI profile/HUD save failed' }
LoadProfile
if ($mode.SelectedIndex -ne 2 -or $preset.SelectedIndex -ne 0 -or $grid.SelectedIndex -ne 0 -or $rects.Rows[0].Cells[0].Value -ne '10') { throw 'UI saved profile round trip failed' }
$rects.Rows[0].Cells[2].Value='5';$rejected=$false
try { SaveProfile } catch { $rejected=$true }
if (!$rejected -or (ReadIni 'Profile:NewGame.exe' 'HUD1' '') -ne '1000,2000,3000,4000') { throw 'Invalid HUD was saved' }
Write-Host 'Actual Settings controls, inherited profile, legacy migration, HUD round trip and invalid input rejection passed.'
'@
    $uiScript = [IO.File]::ReadAllText((Join-Path $project 'package/Settings.ps1'))
    if (!$uiScript.Contains('[void]$form.ShowDialog()')) { throw 'UI fixture cannot locate modal loop' }
    $uiScript = $uiScript.Replace('[void]$form.ShowDialog()',$uiChecks)
    $uiPath = Join-Path $root 'Settings-fixture.ps1'
    [IO.File]::WriteAllText($uiPath,$uiScript,[Text.UTF8Encoding]::new($true))
    & powershell.exe -STA -NoProfile -ExecutionPolicy Bypass -File $uiPath -LSFolder $uiFolder
    if ($LASTEXITCODE -ne 0) { throw 'Settings UI execution failed' }
    [IO.File]::WriteAllText($original,'unsupported original')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'Install.ps1') -LSFolder $ls
    if ($LASTEXITCODE -ne 1 -or [IO.File]::ReadAllText((Join-Path $ls 'Lossless.dll')) -ne 'old manager proxy') { throw 'Unsupported original was not rejected safely' }
    Write-Host 'Package scripts parse; install, repeated install, rollback, runtime preservation and version rejection passed.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
