[CmdletBinding()]
param([ValidateSet('Debug', 'RelWithDebInfo', 'Release')][string]$Configuration = 'RelWithDebInfo')

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'Visual Studio C++ Build Tools were not found.' }
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
$command = '"{0}" && cmake -S "{1}" -B "{1}\build" -G Ninja -DCMAKE_BUILD_TYPE={2} && cmake --build "{1}\build" --clean-first' -f $vcvars, $root, $Configuration
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
