@echo off
setlocal
set "ROOT=%~dp0"
set "MSYS=%ROOT%.toolchains\msys64\mingw64"

if not exist "%ROOT%build-win\qt-demo\kvstore_qt_demo.exe" (
    echo Qt demo not built. Run:
    echo   powershell -ExecutionPolicy Bypass -File "%ROOT%scripts\build-qt-demo.ps1"
    pause
    exit /b 1
)

set "PATH=%MSYS%\bin;%PATH%"
set "QT_PLUGIN_PATH=%MSYS%\share\qt6\plugins"
start "" "%ROOT%build-win\qt-demo\kvstore_qt_demo.exe" --demo
endlocal
