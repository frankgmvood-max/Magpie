#Requires -Version 7.0
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$output=Join-Path $env:TEMP 'Magpie-VRR-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
foreach($test in @('VrrPolicyTests','PresentationPipelineTests','VrrWarpTests','PresentationWaitTests')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 "/I$repo/src/Magpie.Core" "$PSScriptRoot/$test.cpp" "/Fe:$output/$test.exe" "/Fo:$output/$test.obj" d3d11.lib dxgi.lib d3dcompiler.lib dcomp.lib user32.lib
    if($LASTEXITCODE) { throw "Compile failed: $test" }
    & "$output/$test.exe"
    if($LASTEXITCODE) { throw "Test failed: $test" }
}
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "$PSScriptRoot/CrashReporterTests.cpp" "/Fe:$output/CrashReporterTests.exe" "/Fo:$output/CrashReporterTests.obj" shell32.lib dbghelp.lib
if ($LASTEXITCODE) { throw 'Compile failed: native crash reporter' }
& "$output/CrashReporterTests.exe"
if ($LASTEXITCODE) { throw 'Test failed: native crash reporter' }
