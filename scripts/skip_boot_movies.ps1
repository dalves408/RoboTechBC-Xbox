param(
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
$movieDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'game_files\Media\vcResources\xbox\vcMovie'
$stash = Join-Path $movieDir '.skipped_for_bringup'
$files = @(
    'robo-legal.bik',
    'TDK Logo.bik',
    'Harmony Logo.bik',
    'VC Logo.bik',
    'Intro.bik'
)
if ($Restore) {
    foreach ($f in $files) {
        $src = Join-Path $stash $f
        if (Test-Path -LiteralPath $src) {
            Move-Item -LiteralPath $src -Destination (Join-Path $movieDir $f) -Force
            Write-Host "restored $f"
        }
    }
    return
}
New-Item -ItemType Directory -Force $stash | Out-Null
foreach ($f in $files) {
    $src = Join-Path $movieDir $f
    if (Test-Path -LiteralPath $src) {
        Move-Item -LiteralPath $src -Destination (Join-Path $stash $f) -Force
        Write-Host "skipped $f"
    }
}
