param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'Debug'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeExe = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmakeExe) {
    $vswhereExe = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhereExe) {
        $vsRoot = & $vswhereExe -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsRoot) {
            $bundledCMake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $bundledCMake) { $cmakeExe = $bundledCMake }
        }
    }
}
if (-not $cmakeExe) {
    throw 'Install CMake 3.20+ and Visual Studio 2022 Build Tools with Desktop C++.'
}

$genTypes = Join-Path $projectRoot 'src\recomp\gen\recomp_types.h'
$genAny = Get-ChildItem (Join-Path $projectRoot 'src\recomp\gen') -Filter 'recomp_*.c' -ErrorAction SilentlyContinue
if (-not (Test-Path -LiteralPath $genTypes) -and -not $genAny) {
    throw 'Generate C sources first: .\.venv\Scripts\python.exe scripts\pipeline.py --lift'
}

# Interactive keyboard-as-pad needs bak OHCI. Shared toolkit ohci.c is often
# overwritten by the XAPI_TD WIP; pin/reapply before every build.
$ohci = Join-Path $projectRoot 'tools\xboxrecomp\src\usb\ohci.c'
$ohciBak = Join-Path $projectRoot 'patches\ohci.c.dx11bak'
$ohciToolkitBak = Join-Path $projectRoot 'tools\xboxrecomp\src\usb\ohci.c.dx11bak'
if (-not (Test-Path -LiteralPath $ohciBak) -and (Test-Path -LiteralPath $ohciToolkitBak)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $ohciBak) | Out-Null
    Copy-Item -LiteralPath $ohciToolkitBak -Destination $ohciBak -Force
}
if ((Test-Path -LiteralPath $ohciBak) -and (Test-Path -LiteralPath $ohci)) {
    $hLive = (Get-FileHash -LiteralPath $ohci).Hash
    $hBak = (Get-FileHash -LiteralPath $ohciBak).Hash
    if ($hLive -ne $hBak) {
        Write-Host "Reapplying patches\ohci.c.dx11bak over tools/.../ohci.c (WIP drift)."
        Copy-Item -LiteralPath $ohciBak -Destination $ohci -Force
        (Get-Item -LiteralPath $ohci).LastWriteTime = Get-Date
    }
}

$buildDir = Join-Path $projectRoot 'build'
$cmakeArgs = @(
    '-S', $projectRoot,
    '-B', $buildDir,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64'
)
& $cmakeExe @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& $cmakeExe --build $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
Write-Host "Built $Configuration under $buildDir"
