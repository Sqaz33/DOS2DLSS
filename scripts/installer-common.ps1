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

function Install-ModFile {
    param([string]$Source, [string]$Destination, [string]$BackupDirectory)
    try { Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop }
    catch {
        if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) { throw }
        # A debugger/scanner can retain a read handle after the game exits.
        # Rename preserves that file and its handle while allowing an update.
        $saved = Join-Path $BackupDirectory ((Split-Path $Destination -Leaf) + '.' + [DateTime]::UtcNow.Ticks + '.previous')
        Move-Item -LiteralPath $Destination -Destination $saved -ErrorAction Stop
        try { Copy-Item -LiteralPath $Source -Destination $Destination -ErrorAction Stop }
        catch {
            # Copy can fail after creating a partial replacement.
            if (Test-Path -LiteralPath $Destination -PathType Leaf) {
                Remove-Item -LiteralPath $Destination -Force -ErrorAction Stop
            }
            Move-Item -LiteralPath $saved -Destination $Destination -ErrorAction Stop
            throw
        }
    }
}
