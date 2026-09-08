[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\SteamLibrary\steamapps\common\Divinity Original Sin 2'
)

$ErrorActionPreference = 'Stop'
$bin = Join-Path $GameRoot 'DefEd\bin'
$backup = Join-Path $bin 'DOS2DLSS-Backup'
$statePath = Join-Path $backup 'install-state.json'

if (Get-Process EoCApp -ErrorAction SilentlyContinue) {
    throw 'Close Divinity: Original Sin 2 before uninstalling DOS2DLSS.'
}

Remove-Item -LiteralPath (Join-Path $bin 'dos2-dlss.addon64') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $bin 'dos2-dlss.ini') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $bin 'dos2-dlss.log') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $bin 'NativeMods\DOS2DLSSNative.dll') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $bin 'NativeMods\DOS2DLSSNative.log') -Force -ErrorAction SilentlyContinue

$bink = Join-Path $bin 'bink2w64.dll'
$binkOriginal = Join-Path $bin 'bink2w64_original.dll'
if (Test-Path -LiteralPath $binkOriginal) {
    Copy-Item -LiteralPath $binkOriginal -Destination $bink -Force
    Remove-Item -LiteralPath $binkOriginal -Force
}

if (Test-Path -LiteralPath $statePath) {
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    foreach ($item in $state.Renamed) {
        if (Test-Path -LiteralPath $item.Disabled) {
            Move-Item -LiteralPath $item.Disabled -Destination $item.Active -Force
        }
    }
}

$bridgeBackup = Join-Path $backup 'dlss5-bridge.cfg'
if (Test-Path -LiteralPath $bridgeBackup) {
    Copy-Item -LiteralPath $bridgeBackup -Destination (Join-Path $bin 'dlss5-bridge.cfg') -Force
}
foreach ($leaf in @('settings.json', 'addon_deployments.json')) {
    $saved = Join-Path $backup $leaf
    if (Test-Path -LiteralPath $saved) {
        Copy-Item -LiteralPath $saved -Destination (Join-Path $env:LOCALAPPDATA "RHI\$leaf") -Force
    }
}

Write-Host 'DOS2DLSS removed and the previous game/RHI state restored.'

