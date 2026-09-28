# Build (and optionally run) the EdgeVision firmware.
#
#   .\build.ps1                      build only
#   .\build.ps1 -Run                 build, then run with the synthetic scene
#   .\build.ps1 -Run -- clip.mp4     extra arguments go to the firmware
#
# Build output goes to %LOCALAPPDATA%\edgevision\build, outside OneDrive.
param(
    [switch]$Run,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$FirmwareArgs
)

$ErrorActionPreference = "Stop"
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
$build = Join-Path $env:LOCALAPPDATA "edgevision\build"

cmake -S $PSScriptRoot -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Run) {
    & (Join-Path $build "edgevision.exe") @FirmwareArgs
}
