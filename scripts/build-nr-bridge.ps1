[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sourceDir = Join-Path $root 'third_party\dlss5_bridge_src\src'
$source = Join-Path $sourceDir 'dlss5-bridge.cpp'
Copy-Item (Join-Path $root 'patches\bridge\dos2_nr_gate.inc') $sourceDir -Force
$text = Get-Content $source -Raw
if (-not $text.Contains('#include "dos2_nr_gate.inc"')) {
    $anchor = 'static NVSDK_NGX_Result ForwardEvaluate(Hook &h,'
    if (-not $text.Contains($anchor)) { throw 'Unsupported bridge source: ForwardEvaluate missing.' }
    $text = $text.Replace($anchor, '#include "dos2_nr_gate.inc"' + "`n" + $anchor)
    $text = $text.Replace('if (g_cfg.source == CFG_SRC_SYNTH)', 'if (g_cfg.source == CFG_SRC_SYNTH || !Dos2NrEnabled())')
    $text = $text.Replace('g_reshade_module = self;', 'g_reshade_module = self;' + "`n" + '                g_dos2_get_config = reinterpret_cast<Dos2GetConfig>(GetProcAddress(mods[i], "ReShadeGetConfigValue"));')
    $text = $text.Replace('[bridge] source=synth, so the game', '[bridge] mirror bypass requested, so the game')
    [IO.File]::WriteAllText($source, $text)
}
if (-not $text.Contains('if (g_dos2_reset_native)')) {
    $branch = 'if (g_cfg.source == CFG_SRC_SYNTH || !Dos2NrEnabled())'
    $start = $text.IndexOf($branch)
    if ($start -lt 0) { throw 'NR bypass branch missing.' }
    $brace = $text.IndexOf('{', $start)
    $text = $text.Insert($brace + 1, "`n        if (g_dos2_reset_native) { const_cast<NVSDK_NGX_Parameter *>(p)->Set(`"Reset`", 1); g_dos2_reset_native = false; }")
    [IO.File]::WriteAllText($source, $text)
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools missing.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $root 'build\dos2-nr-bridge.addon64'
Push-Location $sourceDir
try {
    & cmd.exe /d /s /c ('"{0}" && cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /Ireshade /LD dlss5-bridge.cpp /Fe:"{1}" /link /DLL user32.lib advapi32.lib bcrypt.lib' -f $vcvars, $output)
    if ($LASTEXITCODE -ne 0) { throw 'NR bridge build failed.' }
} finally { Pop-Location }
