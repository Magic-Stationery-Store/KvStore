param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [switch]$Demo,
    [string]$Screenshot = ''
)

$ErrorActionPreference = 'Stop'

$msys = Join-Path $Root '.toolchains\msys64\mingw64'
$exe = Join-Path $Root 'build-win\qt-demo\kvstore_qt_demo.exe'

if (-not (Test-Path $exe)) {
    throw "Demo binary not found. Run scripts/build-qt-demo.ps1 first."
}

$env:PATH = (Join-Path $msys 'bin') + ';' + $env:PATH
$env:QT_PLUGIN_PATH = Join-Path $msys 'share\qt6\plugins'

$arguments = @()
if ($Demo) { $arguments += '--demo' }
if ($Screenshot -ne '') { $arguments += @('--screenshot', $Screenshot) }

if ($Screenshot -ne '') {
    $env:QT_QPA_PLATFORM = 'offscreen'
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

& $exe @arguments
exit $LASTEXITCODE
