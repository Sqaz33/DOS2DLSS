[CmdletBinding()]
param([string]$Version = '0.6.1-preview.1')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^[0-9A-Za-z.-]+$') { throw 'Invalid version.' }
$root = Split-Path $PSScriptRoot -Parent
$package = Join-Path $root "dist\DOS2DLSS-$Version-win64"
if (Test-Path $package) { throw 'Package directory already exists; choose a new version or inspect it first.' }
foreach ($dir in @('build', 'scripts', 'config', 'licenses', 'third_party\native_mod_loader')) {
    New-Item -ItemType Directory -Path (Join-Path $package $dir) -Force | Out-Null
}
foreach ($name in @('DOS2DLSSNative.dll', 'dos2-dlss.addon64', 'dos2-nr-bridge.addon64', 'dos2dlss_status.exe')) {
    Copy-Item (Join-Path $root "build\$name") (Join-Path $package 'build')
}
Copy-Item (Join-Path $root 'third_party\nvidia_dlss\lib\Windows_x86_64\rel\nvngx_dlss.dll') (Join-Path $package 'build')
Copy-Item (Join-Path $root 'third_party\native_mod_loader\bink2w64.dll') (Join-Path $package 'third_party\native_mod_loader')
Copy-Item (Join-Path $root 'config\dos2-dlss.ini') (Join-Path $package 'config')
foreach ($name in @('install.ps1', 'uninstall.ps1')) {
    Copy-Item (Join-Path $PSScriptRoot $name) (Join-Path $package 'scripts')
}
Copy-Item (Join-Path $root 'docs\release-install-ru.md') (Join-Path $package 'INSTALL-RU.md')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $package 'LICENSE')
Copy-Item (Join-Path $root 'third_party\nvidia_dlss\LICENSE.txt') (Join-Path $package 'licenses\NVIDIA-DLSS.txt')
Copy-Item (Join-Path $root 'third_party\dos2wasd\LICENSE') (Join-Path $package 'licenses\NativeModLoader-MIT.txt')
Copy-Item (Join-Path $root 'third_party\dlss5_bridge_src\LICENSE') (Join-Path $package 'licenses\Bridge-MIT.txt')
Copy-Item (Join-Path $root 'third_party\reshade\LICENSE.md') (Join-Path $package 'licenses\ReShade-SDK.md')
@'
@echo off
where pwsh.exe >nul 2>&1
if errorlevel 1 (
  echo PowerShell 7 is required. See INSTALL-RU.md.
  pause
  exit /b 1
)
if "%~1"=="" (
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1"
) else (
  pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1" -GameRoot "%~1"
)
pause
'@ | Set-Content (Join-Path $package 'Install.cmd') -Encoding ascii
$revision = & git -C $root rev-parse HEAD
"Version=$Version`nSourceCommit=$revision" | Set-Content (Join-Path $package 'BUILD.txt') -Encoding ascii
Get-ChildItem $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName).Hash, $_.FullName.Substring($package.Length + 1)
} | Set-Content (Join-Path $package 'SHA256SUMS.txt') -Encoding ascii
$archive = "$package.zip"
Compress-Archive -Path "$package\*" -DestinationPath $archive
Write-Output $archive
