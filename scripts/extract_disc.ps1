param(
    [string]$Iso = (Join-Path (Split-Path $PSScriptRoot -Parent) 'Robotech - Battlecry (USA).xiso.iso'),
    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'game_files'),
    # extract-xiso.exe: -ExtractXiso, else $env:RBX_EXTRACT_XISO, else tools\extract-xiso.exe, else PATH.
    [string]$ExtractXiso = $env:RBX_EXTRACT_XISO
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$extract = $ExtractXiso
if (-not $extract) {
    $extract = Join-Path $projectRoot 'tools\extract-xiso.exe'
    if (-not (Test-Path -LiteralPath $extract)) {
        $cmd = Get-Command extract-xiso.exe -ErrorAction SilentlyContinue
        if ($cmd) { $extract = $cmd.Source }
    }
}
if (-not (Test-Path -LiteralPath $extract)) {
    throw "extract-xiso not found. Pass -ExtractXiso <path>, set RBX_EXTRACT_XISO, put it in tools\, or add it to PATH."
}
if (-not (Test-Path -LiteralPath $Iso)) {
    throw "ISO not found: $Iso"
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
Write-Host "Extracting:`n  $Iso`n-> $Destination"
& $extract -x -d $Destination $Iso
if ($LASTEXITCODE -ne 0) { throw "extract-xiso failed with exit $LASTEXITCODE" }

$defaultXbe = Join-Path $Destination 'default.xbe'
if (-not (Test-Path -LiteralPath $defaultXbe)) {
    # Some titles use a differently named boot XBE; list candidates.
    $xbes = Get-ChildItem -LiteralPath $Destination -Filter '*.xbe' -Recurse -ErrorAction SilentlyContinue
    Write-Host 'default.xbe missing. Found XBEs:'
    $xbes | ForEach-Object { Write-Host "  $($_.FullName) ($($_.Length) bytes)" }
    throw "Extraction finished but default.xbe is missing under $Destination"
}

$py = Join-Path $projectRoot '.venv\Scripts\python.exe'
$extractPy = Join-Path $PSScriptRoot 'extract_disc.py'
if ((Test-Path -LiteralPath $py) -and (Test-Path -LiteralPath $extractPy)) {
    & $py $extractPy $Iso $Destination --manifest (Join-Path $projectRoot 'analysis\disc_manifest.json')
}

Write-Host "OK: $defaultXbe"
Get-FileHash -LiteralPath $defaultXbe -Algorithm SHA256 | Format-List
