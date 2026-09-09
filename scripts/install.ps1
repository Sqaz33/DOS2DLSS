[CmdletBinding()]
param(
    [string]$GameRoot,
    [string]$BuildDirectory,
    [switch]$PauseNeuralRendering
)

$ErrorActionPreference = 'Stop'
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot '..\build' }

$expectedExeHash = '1D14A07CB7F22EBA64559789F826C9FC3A6C54C420FE6F1C8532316B5E33A33D'
$expectedBinkHash = 'FCA6CEC726FF3E9FB9444B96C5AF38ED7C70C5D03F5F80557D5C4ABD099E8A5E'
$loaderHash = '6C2932D54E56DCB1A6E9E0D9CD13C2DBACB9BDF9D4175B0BCBB0E0CAB0FC20FC'
. (Join-Path $PSScriptRoot 'installer-common.ps1')
$bin = Resolve-GameBin $GameRoot
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
$dlssRuntime = Join-Path $BuildDirectory 'nvngx_dlss.dll'
if ((Test-Path -LiteralPath $dlssRuntime) -and
    -not (Test-Path -LiteralPath (Join-Path $bin 'nvngx_dlss.dll'))) {
    Copy-Item -LiteralPath $dlssRuntime -Destination (Join-Path $bin 'nvngx_dlss.dll')
}
if (-not (Test-Path -LiteralPath (Join-Path $bin 'dos2-dlss.ini'))) {
    Copy-Item -LiteralPath $configSource -Destination (Join-Path $bin 'dos2-dlss.ini')
}

$renamed = @()
foreach ($leaf in @('dlss5-feed.addon64', 'dlss5-bridge.addon64')) {
    $active = Join-Path $bin $leaf
    $disabled = "$active.dos2dlss-disabled"
    if (Test-Path -LiteralPath $active) {
        if (Test-Path -LiteralPath $disabled) { Remove-Item -LiteralPath $disabled -Force }
        Move-Item -LiteralPath $active -Destination $disabled
    }
    if (Test-Path -LiteralPath $disabled) {
        $renamed += [pscustomobject]@{ Active = $active; Disabled = $disabled }
    }
}

# Keep NR available unless an explicit diagnostic install requests a pause.
foreach ($leaf in @('DLSS5 DX11 Bridge.addon64', 'renodx-dlss5.addon64')) {
    $active = Join-Path $bin $leaf
    $disabled = "$active.dos2dlss-paused"
    if ($PauseNeuralRendering) {
        if (Test-Path -LiteralPath $active) {
            Move-Item -LiteralPath $active -Destination $disabled -Force
        }
        if (Test-Path -LiteralPath $disabled) {
            $renamed += [pscustomobject]@{ Active = $active; Disabled = $disabled }
        }
    } elseif ((Test-Path -LiteralPath $disabled) -and -not (Test-Path -LiteralPath $active)) {
        Move-Item -LiteralPath $disabled -Destination $active
    }
}
$nrBridge = Join-Path $BuildDirectory 'dos2-nr-bridge.addon64'
if (-not $PauseNeuralRendering -and (Test-Path -LiteralPath $nrBridge)) {
    Copy-Item -LiteralPath $nrBridge -Destination (Join-Path $bin 'DLSS5 DX11 Bridge.addon64') -Force
}

# DOS2 ships the Windows 8.1 SDK build of D3DCompiler. RenoDX's DLSS5 proxy
# compiles a Shader Model 5.1 compute shader at runtime, which that old DLL
# rejects with X3506. Let Windows load its current system copy instead, while
# retaining the game-local file for a completely reversible uninstall.
$compiler = Join-Path $bin 'd3dcompiler_47.dll'
$compilerDisabled = "$compiler.dos2dlss-disabled"
if (Test-Path -LiteralPath $compiler) {
    $compilerVersion = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
    if ($compilerVersion -like '6.3.9600.*') {
        if (Test-Path -LiteralPath $compilerDisabled) {
            throw 'Both active and DOS2DLSS-disabled copies of d3dcompiler_47.dll exist.'
        }
        Move-Item -LiteralPath $compiler -Destination $compilerDisabled
    }
}
if (Test-Path -LiteralPath $compilerDisabled) {
    $renamed += [pscustomobject]@{ Active = $compiler; Disabled = $compilerDisabled }
}

$bridgeConfig = Join-Path $bin 'dlss5-bridge.cfg'
if (Test-Path -LiteralPath $bridgeConfig) {
    Copy-Item -LiteralPath $bridgeConfig -Destination (Join-Path $backup 'dlss5-bridge.cfg') -Force
    $text = Get-Content -LiteralPath $bridgeConfig -Raw
    $text = $text -replace '(?m)^synth\s*=.*$', 'synth=0'
    $text = $text -replace '(?m)^source\s*=.*$', 'source=auto'
    $text = $text -replace '(?m)^skip_game\s*=.*$', 'skip_game=1'
    if (-not $PauseNeuralRendering -and (Test-Path -LiteralPath $nrBridge)) {
        $text = $text -replace '(?m)^stage\s*=.*$', 'stage=3'
    }
    Write-Utf8File -LiteralPath $bridgeConfig -Value $text
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
    $settings = Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $selection = $settings.PerGameAddonSelection | ConvertFrom-JsonMap
    $key = 'Divinity: Original Sin 2|Steam'
    if ($selection.ContainsKey($key)) {
        $pausedAddons = @('DLSS5 Feeder')
        if ($PauseNeuralRendering) { $pausedAddons += @('DLSS5 Tool', 'DLSS5 DX11 Bridge', 'RenoDX Upgrade') }
        $selection[$key] = @($selection[$key] | Where-Object { $_ -notin $pausedAddons })
        $settings.PerGameAddonSelection = $selection | ConvertTo-Json -Compress
        $settings | ConvertTo-Json -Compress -Depth 20 | Write-Utf8File -LiteralPath $settingsPath
    }
}

$deploymentsPath = $rhiFiles[1]
if (Test-Path -LiteralPath $deploymentsPath) {
    $deployments = Get-Content -LiteralPath $deploymentsPath -Raw -Encoding UTF8 | ConvertFrom-JsonMap
    if ($deployments.ContainsKey($bin)) {
        $pausedFiles = @('dlss5-feed.addon64', 'dlss5-bridge.addon64')
        if ($PauseNeuralRendering) { $pausedFiles += @('DLSS5 DX11 Bridge.addon64', 'renodx-dlss5.addon64') }
        $deployments[$bin] = @($deployments[$bin] | Where-Object { $_ -notin $pausedFiles })
        $deployments | ConvertTo-Json -Depth 10 | Write-Utf8File -LiteralPath $deploymentsPath
    }
}

[pscustomobject]@{
    InstalledAt = (Get-Date).ToString('o')
    GameRoot = $GameRoot
    Renamed = $renamed
    RhiFiles = @($rhiFiles | Where-Object { Test-Path -LiteralPath $_ })
} | ConvertTo-Json -Depth 8 | Write-Utf8File -LiteralPath $statePath

Write-Host 'DOS2DLSS installed.'
Write-Host 'Launch the game through RHI or Steam and open ReShade with F11.'
