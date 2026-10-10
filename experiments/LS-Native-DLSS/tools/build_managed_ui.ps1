param([string]$OutputRoot = 'build/managed-ui')
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
# Official pinned Cecil package; installer uses its .NET Framework variant.
$cecilZip = Join-Path $OutputRoot 'mono.cecil.0.11.6.nupkg'
Invoke-WebRequest -Uri 'https://api.nuget.org/v3-flatcontainer/mono.cecil/0.11.6/mono.cecil.0.11.6.nupkg' -OutFile $cecilZip
if ((Get-FileHash $cecilZip -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'd2a23832aaa948ba9a01acc42b5726e34c5f995958f1b30d45c0e7c70b3a72d5') { throw 'Cecil package hash mismatch.' }
$cecil = Join-Path $OutputRoot 'cecil'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($cecilZip,$cecil)
Copy-Item -LiteralPath (Join-Path $cecil 'lib/net40/Mono.Cecil.dll') -Destination $OutputRoot
Copy-Item -LiteralPath (Join-Path $project 'managed-ui/Patcher.cs') -Destination (Join-Path $OutputRoot 'UI-Patcher.cs')
dotnet build (Join-Path $project 'managed-ui/NativeDLSS.UI.csproj') -c Release -o (Join-Path $OutputRoot 'ui')
if ($LASTEXITCODE -ne 0) { throw 'Managed UI build failed.' }
dotnet build (Join-Path $project 'tests/windows/managed-ui/NativeUIFixture.csproj') -c Release -o (Join-Path $OutputRoot 'fixture')
if ($LASTEXITCODE -ne 0) { throw 'Managed UI fixture build failed.' }
Copy-Item -LiteralPath (Join-Path $OutputRoot 'ui/NativeDLSS.UI.dll') -Destination $OutputRoot
