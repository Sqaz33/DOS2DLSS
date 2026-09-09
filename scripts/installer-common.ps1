# Compatible with built-in Windows PowerShell 5.1 and PowerShell 7.
function Resolve-GameBin {
    param([string]$Location)
    if (-not $Location) { $Location = Join-Path $PSScriptRoot '..' }
    $locationPath = (Resolve-Path -LiteralPath $Location).Path
    if (Test-Path -LiteralPath (Join-Path $locationPath 'DefEd\bin\EoCApp.exe')) {
        return Join-Path $locationPath 'DefEd\bin'
    }
    if (Test-Path -LiteralPath (Join-Path $locationPath 'EoCApp.exe')) {
        return $locationPath
    }
    throw 'Extract the complete archive into the game folder or DefEd\bin, then run Install.cmd there.'
}

function Write-Utf8File {
    param([string]$LiteralPath, [Parameter(ValueFromPipeline=$true)][string]$Value)
    process { [IO.File]::WriteAllText($LiteralPath, $Value, (New-Object Text.UTF8Encoding($false))) }
}

function ConvertFrom-JsonMap {
    param([Parameter(ValueFromPipeline=$true)][string]$InputObject)
    process {
        $result = @{}
        $object = ConvertFrom-Json -InputObject $InputObject
        foreach ($property in $object.PSObject.Properties) { $result[$property.Name] = $property.Value }
        return $result
    }
}
