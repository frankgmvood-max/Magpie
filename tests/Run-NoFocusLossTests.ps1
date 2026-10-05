#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$noFocusRepo = Split-Path $PSScriptRoot -Parent
$noFocusOutput = Join-Path $env:TEMP 'Magpie-NoFocusLoss-tests'
New-Item -ItemType Directory -Path $noFocusOutput -Force | Out-Null
$noFocusVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$noFocusVs = & $noFocusVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $noFocusVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $noFocusVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$noFocusRepo/src/Magpie.Core" `
    "$PSScriptRoot/NoFocusLossPolicyTests.cpp" "/Fe:$noFocusOutput/policy.exe" "/Fo:$noFocusOutput/policy.obj"
if ($LASTEXITCODE) { throw 'NoFocusLoss policy compilation failed' }
& "$noFocusOutput/policy.exe"
if ($LASTEXITCODE) { throw 'NoFocusLoss policy tests failed' }
$noFocusObjects = @()
foreach ($noFocusSource in @('buffer.c', 'hook.c', 'trampoline.c', 'hde/hde64.c')) {
    $noFocusObject = Join-Path $noFocusOutput ([IO.Path]::GetFileNameWithoutExtension($noFocusSource) + '.obj')
    & cl.exe /nologo /TC /MT /O2 /W3 /c "$noFocusRepo/src/Magpie.Core/third_party/minhook/src/$noFocusSource" "/Fo:$noFocusObject"
    if ($LASTEXITCODE) { throw "MinHook test compilation failed: $noFocusSource" }
    $noFocusObjects += $noFocusObject
}
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$noFocusRepo/src/Magpie.Core" `
    "$PSScriptRoot/NoFocusLossNativeTests.cpp" "$noFocusRepo/src/Magpie.Core/NoFocusLossController.cpp" `
    $noFocusObjects "/Fe:$noFocusOutput/native.exe" "/Fo:$noFocusOutput/" /link user32.lib comctl32.lib
if ($LASTEXITCODE) { throw 'NoFocusLoss native test compilation failed' }
& "$noFocusOutput/native.exe"
if ($LASTEXITCODE) { throw 'NoFocusLoss native tests failed' }
