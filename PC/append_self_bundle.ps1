param(
    [string]$ExePath = "d3d11_native_receiver_onefile.exe"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $ExePath)) {
    throw "Missing exe: $ExePath"
}

$files = @("adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll")
foreach ($f in $files) {
    if (!(Test-Path $f)) {
        throw "Missing payload file: $f"
    }
}

$magic = [System.Text.Encoding]::ASCII.GetBytes("HLADBPK1")

$fs = [System.IO.File]::Open($ExePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::Read)
try {
    $fs.Seek(0, [System.IO.SeekOrigin]::End) | Out-Null

    $entries = New-Object System.Collections.Generic.List[object]
    foreach ($name in $files) {
        $offset = [UInt64]$fs.Position
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path (Get-Location) $name))
        $fs.Write($bytes, 0, $bytes.Length)
        $entries.Add([PSCustomObject]@{
            Name   = $name
            Offset = $offset
            Size   = [UInt64]$bytes.Length
        }) | Out-Null
    }

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    try {
        $bw.Write([UInt32]1)
        $bw.Write([UInt32]$entries.Count)
        foreach ($e in $entries) {
            $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($e.Name)
            $bw.Write([UInt16]$nameBytes.Length)
            $bw.Write($nameBytes)
            $bw.Write([UInt64]$e.Offset)
            $bw.Write([UInt64]$e.Size)
        }
        $bw.Flush()
        $manifest = $ms.ToArray()
    }
    finally {
        $bw.Dispose()
        $ms.Dispose()
    }

    $manifestOffset = [UInt64]$fs.Position
    $fs.Write($manifest, 0, $manifest.Length)
    $fs.Write($magic, 0, $magic.Length)

    $bwFile = New-Object System.IO.BinaryWriter($fs)
    try {
        $bwFile.Write([UInt64]$manifestOffset)
        $bwFile.Write([UInt64]$manifest.Length)
        $bwFile.Flush()
    }
    finally {
        $bwFile.Dispose()
    }
}
finally {
    $fs.Dispose()
}

Write-Host "Bundled adb payload into $ExePath"
