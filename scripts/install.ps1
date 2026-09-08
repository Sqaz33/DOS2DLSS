[CmdletBinding()]
param(
    [string]$GameRoot = 'D:\SteamLibrary\steamapps\common\Divinity Original Sin 2',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build')
)

$ErrorActionPreference = 'Stop'

$expectedExeHash = '1D14A07CB7F22EBA64559789F826C9FC3A6C54C420FE6F1C8532316B5E33A33D'
$expectedBinkHash = 'FCA6CEC726FF3E9FB9444B96C5AF38ED7C70C5D03F5F80557D5C4ABD099E8A5E'
$loaderHash = '6C2932D54E56DCB1A6E9E0D9CD13C2DBACB9BDF9D4175B0BCBB0E0CAB0FC20FC'
$bin = Join-Path $GameRoot 'DefEd\bin'
$exe = Join-Path $bin 'EoCApp.exe'
$nativeSource = Join-Path $BuildDirectory 'DOS2DLSSNative.dll'
$addonSource = Join-Path $BuildDirectory 'dos2-dlss.addon64'
$configSource = Join-Path $PSScriptRoot '..\config\dos2-dlss.ini'
$loaderSource = Join-Path $PSScriptRoot '..\third_party\native_mod_loader\bink2w64.dll'
$backup = Join-Path $bin 'DOS2DLSS-Backup'
$statePath = Join-Path $backup 'install-state.json'

if (Get-Process EoCApp -ErrorAction SilentlyContinue) {
    throw 'Close Divinity: Original Sin 2 before installing DOS2DLSS.'
}
foreach ($required in @($exe, $nativeSource, $addonSource, $configSource, $loaderSource)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required file is missing: $required" }
}
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $expectedExeHash) {
    throw 'EoCApp.exe does not match supported build 3.6.117.3735.'
}
if ((Get-FileHash -LiteralPath $loaderSource -Algorithm SHA256).Hash -ne $loaderHash) {
    throw 'NativeModLoader proxy hash does not match the reviewed dependency.'
}

New-Item -ItemType Directory -Path $backup -Force | Out-Null
$nativeMods = Join-Path $bin 'NativeMods'
New-Item -ItemType Directory -Path $nativeMods -Force | Out-Null

$bink = Join-Path $bin 'bink2w64.dll'
$binkOriginal = Join-Path $bin 'bink2w64_original.dll'
$currentBinkHash = (Get-FileHash -LiteralPath $bink -Algorithm SHA256).Hash
if ($currentBinkHash -eq $expectedBinkHash) {
    if (-not (Test-Path -LiteralPath $binkOriginal)) {
        Copy-Item -LiteralPath $bink -Destination $binkOriginal
    }
} elseif ($currentBinkHash -ne $loaderHash) {
    throw 'bink2w64.dll is already modified by an unknown loader.'
}
if ((Get-FileHash -LiteralPath $binkOriginal -Algorithm SHA256).Hash -ne $expectedBinkHash) {
    throw 'bink2w64_original.dll is not the original DOS2 library.'
}
Copy-Item -LiteralPath $loaderSource -Destination $bink -Force

Copy-Item -LiteralPath $nativeSource -Destination (Join-Path $nativeMods 'DOS2DLSSNative.dll') -Force
Copy-Item -LiteralPath $addonSource -Destination (Join-Path $bin 'dos2-dlss.addon64') -Force
if (-not (Test-Path -LiteralPath (Join-Path $bin 'dos2-dlss.ini'))) {
    Copy-Item -LiteralPath $configSource -Destination (Join-Path $bin 'dos2-dlss.ini')
}

$renamed = @()
foreach ($leaf in @('dlss5-feed.addon64', 'dlss5-bridge.addon64')) {
    $active = Join-Path $bin $leaf
    if (Test-Path -LiteralPath $active) {
        $disabled = "$active.dos2dlss-disabled"
        if (Test-Path -LiteralPath $disabled) { Remove-Item -LiteralPath $disabled -Force }
        Move-Item -LiteralPath $active -Destination $disabled
        $renamed += [pscustomobject]@{ Active = $active; Disabled = $disabled }
    }
}

$bridgeConfig = Join-Path $bin 'dlss5-bridge.cfg'
if (Test-Path -LiteralPath $bridgeConfig) {
    Copy-Item -LiteralPath $bridgeConfig -Destination (Join-Path $backup 'dlss5-bridge.cfg') -Force
    $text = Get-Content -LiteralPath $bridgeConfig -Raw
    $text = $text -replace '(?m)^synth\s*=.*$', 'synth=0'
    $text = $text -replace '(?m)^source\s*=.*$', 'source=auto'
    Set-Content -LiteralPath $bridgeConfig -Value $text -Encoding utf8NoBOM
}

$rhiFiles = @(
    (Join-Path $env:LOCALAPPDATA 'RHI\settings.json'),
    (Join-Path $env:LOCALAPPDATA 'RHI\addon_deployments.json')
)
foreach ($rhiFile in $rhiFiles) {
    if (Test-Path -LiteralPath $rhiFile) {
        Copy-Item -LiteralPath $rhiFile -Destination (Join-Path $backup ([IO.Path]::GetFileName($rhiFile))) -Force
    }
}

$settingsPath = $rhiFiles[0]
if (Test-Path -LiteralPath $settingsPath) {
    $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
    $selection = $settings.PerGameAddonSelection | ConvertFrom-Json -AsHashtable
    $key = 'Divinity: Original Sin 2|Steam'
    if ($selection.ContainsKey($key)) {
        $selection[$key] = @($selection[$key] | Where-Object { $_ -ne 'DLSS5 Feeder' })
        $settings.PerGameAddonSelection = $selection | ConvertTo-Json -Compress
        $settings | ConvertTo-Json -Compress -Depth 20 | Set-Content -LiteralPath $settingsPath -Encoding utf8NoBOM
    }
}

$deploymentsPath = $rhiFiles[1]
if (Test-Path -LiteralPath $deploymentsPath) {
    $deployments = Get-Content -LiteralPath $deploymentsPath -Raw | ConvertFrom-Json -AsHashtable
    if ($deployments.ContainsKey($bin)) {
        $deployments[$bin] = @($deployments[$bin] | Where-Object { $_ -ne 'dlss5-feed.addon64' })
        $deployments | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $deploymentsPath -Encoding utf8NoBOM
    }
}

[pscustomobject]@{
    InstalledAt = (Get-Date).ToString('o')
    GameRoot = $GameRoot
    Renamed = $renamed
    RhiFiles = @($rhiFiles | Where-Object { Test-Path -LiteralPath $_ })
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $statePath -Encoding utf8NoBOM

Write-Host 'DOS2DLSS installed.'
Write-Host 'Launch the game through RHI or Steam and open ReShade with F11.'
