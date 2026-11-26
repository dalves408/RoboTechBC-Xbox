param(
    [switch]$SkipPython,
    # Optional folder holding shared checkouts (xboxrecomp, Xbox-Debug-Kit) to
    # junction instead of cloning. Defaults to $env:RBX_SHARED_TOOLS.
    [string]$SharedTools = $env:RBX_SHARED_TOOLS
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$dependencies = Get-Content (Join-Path $projectRoot 'dependencies.json') -Raw | ConvertFrom-Json

New-Item -ItemType Directory -Force (Join-Path $projectRoot 'tools') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $projectRoot 'analysis') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $projectRoot 'src\recomp\gen') | Out-Null

# Prefer a shared toolkit checkout via a junction rather than cloning another copy.
$sharedXboxrecomp = if ($SharedTools) { Join-Path $SharedTools 'xboxrecomp' } else { '' }
$localXboxrecomp = Join-Path $projectRoot 'tools\xboxrecomp'
if (-not (Test-Path -LiteralPath $localXboxrecomp)) {
    if ($sharedXboxrecomp -and (Test-Path -LiteralPath $sharedXboxrecomp)) {
        cmd /c mklink /J "$localXboxrecomp" "$sharedXboxrecomp" | Out-Null
    } else {
        & git clone $dependencies.xboxrecomp.url $localXboxrecomp
        if ($LASTEXITCODE -ne 0) { throw 'Could not clone xboxrecomp.' }
        & git -C $localXboxrecomp checkout --detach $dependencies.xboxrecomp.commit
        if ($LASTEXITCODE -ne 0) { throw 'Could not check out pinned xboxrecomp revision.' }
    }
}

$sharedDebugKit = if ($SharedTools) { Join-Path $SharedTools 'Xbox-Debug-Kit' } else { '' }
$localDebugKit = Join-Path $projectRoot 'tools\xbox-debug-kit'
if ($sharedDebugKit -and (Test-Path -LiteralPath $sharedDebugKit) -and -not (Test-Path -LiteralPath $localDebugKit)) {
    cmd /c mklink /J "$localDebugKit" "$sharedDebugKit" | Out-Null
}

if (-not $SkipPython) {
    $pythonExe = Join-Path $projectRoot '.venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $pythonExe)) {
        & python -m venv (Join-Path $projectRoot '.venv')
        if ($LASTEXITCODE -ne 0) { throw 'Install Python 3.12+ and make python available on PATH.' }
    }
    & $pythonExe -m pip install --target (Join-Path $projectRoot '.deps') "capstone==$($dependencies.capstone)"
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the pinned Capstone dependency.' }
    & $pythonExe -m pip install "psutil==$($dependencies.psutil)"
    if ($LASTEXITCODE -ne 0) { throw 'Could not install psutil.' }
}

Write-Host 'Robotech Battlecry recomp deps ready.'
Write-Host 'Next: .\scripts\extract_disc.ps1'
Write-Host 'Then: .\.venv\Scripts\python.exe scripts\pipeline.py --lift'
