param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

$msys = Join-Path $Root '.toolchains\msys64\mingw64'
$gxx = Join-Path $msys 'bin\g++.exe'
$gcc = Join-Path $msys 'bin\gcc.exe'
$cmake = Join-Path $msys 'bin\cmake.exe'

if (-not (Test-Path $gxx)) {
    throw "Qt/MinGW toolchain not found at $msys. See qt-demo/README.md."
}

$env:PATH = (Join-Path $msys 'bin') + ';' + $env:PATH

& $cmake -S $Root -B (Join-Path $Root 'build-win') -G Ninja `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_C_COMPILER=$gcc" `
    "-DCMAKE_CXX_COMPILER=$gxx" `
    "-DKVSTORE_BUILD_QT_DEMO=ON" `
    "-DCMAKE_PREFIX_PATH=$msys"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build (Join-Path $Root 'build-win')
exit $LASTEXITCODE
