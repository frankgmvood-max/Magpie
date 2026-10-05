#Requires -Version 7.0
param(
    [string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-DuplicateFrameTests-' + [guid]::NewGuid())),
    [string]$ShaderHeader = '',
    [switch]$ShaderOnly
)
$ErrorActionPreference = 'Stop'
$duplicateRepo = Split-Path $PSScriptRoot -Parent
$duplicateOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $duplicateOutput -Force | Out-Null
$duplicateVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$duplicateVs = & $duplicateVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $duplicateVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $duplicateVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

if ($ShaderHeader) {
    Copy-Item -LiteralPath $ShaderHeader -Destination "$duplicateOutput/DuplicateFrameCS.h"
    Write-Host 'Testing the duplicate shader header produced by the actual application build.'
} else {
    [xml]$duplicateProject = Get-Content -LiteralPath "$duplicateRepo/src/Magpie.Core/Magpie.Core.vcxproj"
    $duplicateSdk = $duplicateProject.SelectSingleNode("//*[local-name()='WindowsTargetPlatformVersion']").InnerText
    $duplicateFxc = Join-Path $env:WindowsSdkDir "bin/$duplicateSdk/x64/fxc.exe"
    if (-not (Test-Path -LiteralPath $duplicateFxc)) { throw "Pinned SDK FXC unavailable: $duplicateSdk" }
    # Same compiler and flags as Magpie.Core's FxCompile target.
    & $duplicateFxc /nologo /E main /WX /T cs_5_0 /all_resources_bound `
        /Fh "$duplicateOutput/DuplicateFrameCS.h" /Vn DuplicateFrameCS "$duplicateRepo/src/Magpie.Core/shaders/DuplicateFrameCS.hlsl"
    if ($LASTEXITCODE) { throw 'Production duplicate shader SDK compilation failed' }
}
if (-not $ShaderOnly) {
    & python "$duplicateRepo/scripts/tests/test_fg_duplicate_filter.py" $duplicateOutput
    if ($LASTEXITCODE) { throw 'Capture duplicate-filter extraction failed' }
    & python "$PSScriptRoot/prepare_duplicate_capture_pipeline_test.py" $duplicateOutput
    if ($LASTEXITCODE) { throw 'Capture pipeline extraction failed' }
}
$duplicateTests = @(
    @{ Source = "$PSScriptRoot/EffectFrameStateTests.cpp"; Name = 'effect-frame-state'; Libs = @() },
    @{ Source = "$duplicateOutput/fg_duplicate.cpp"; Name = 'capture-filter'; Libs = @() },
    @{ Source = "$duplicateOutput/duplicate_capture_pipeline.cpp"; Name = 'capture-pipeline'; Libs = @() },
    @{ Source = "$PSScriptRoot/DuplicateFrameShaderTests.cpp"; Name = 'duplicate-shader'; Libs = @('d3d11.lib','d3dcompiler.lib') }
)
if ($ShaderOnly) { $duplicateTests = @($duplicateTests[-1]) }
foreach ($duplicateTest in $duplicateTests) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$duplicateRepo/src/Magpie.Core/include" `
        "/I$duplicateOutput" $duplicateTest.Source "/Fe:$duplicateOutput/$($duplicateTest.Name).exe" "/Fo:$duplicateOutput/$($duplicateTest.Name).obj" @($duplicateTest.Libs)
    if ($LASTEXITCODE) { throw "Isolated test compilation failed: $($duplicateTest.Name)" }
    if ($duplicateTest.Name -eq 'duplicate-shader') {
        & "$duplicateOutput/$($duplicateTest.Name).exe" "$duplicateRepo/src/Magpie.Core/shaders/DuplicateFrameCS.hlsl"
    } else {
        & "$duplicateOutput/$($duplicateTest.Name).exe"
    }
    if ($LASTEXITCODE) { throw "Test failed: $($duplicateTest.Name)" }
}
Write-Host "Isolated CPU/WARP tests complete. Output: $duplicateOutput"
