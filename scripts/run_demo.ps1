# Start the whole EdgeVision system locally, one window per component, and
# open the dashboard.
#
#   .\scripts\run_demo.ps1                                  synthetic scene
#   .\scripts\run_demo.ps1 -Clip eval\clips\testclip.mp4     real clip
#
# Components: Mosquitto (MQTT broker) -> cloud stub (events API + dashboard)
#             Wi-Fi module emulator -> firmware
#             Ring gateway, only if RING_HMAC_KEY is set in this shell.
# Stop everything with .\scripts\stop_demo.ps1.

param(
    [string]$Clip = "",
    [int]$AutoMotion = 0,
    [switch]$Aws   # use the AWS deployment from deploy_aws.ps1 instead of local Mosquitto
)

$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
$python = Join-Path $root ".venv\Scripts\python.exe"
$msys = "C:\msys64\ucrt64\bin"
$firmware = Join-Path $env:LOCALAPPDATA "edgevision\build\edgevision.exe"

$missing = @()
if (-not (Test-Path $python)) { $missing += "Python venv (python -m venv .venv; .venv\Scripts\pip install -r requirements.txt)" }
if (-not (Test-Path (Join-Path $msys "mosquitto.exe"))) { $missing += "Mosquitto (pacman -S mingw-w64-ucrt-x86_64-mosquitto)" }
if (-not (Test-Path $firmware)) { $missing += "firmware build (.\firmware\build.ps1)" }
if (-not (Test-Path (Join-Path $root "models\object_detection_nanodet_2022nov.onnx"))) { $missing += "detector model in models\ (see README)" }
if ($missing.Count -gt 0) {
    Write-Host "Missing:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" }
    exit 1
}

function Start-Component([string]$title, [string]$command) {
    $full = "`$host.UI.RawUI.WindowTitle = 'EdgeVision: $title'; `$env:PATH = '$msys;' + `$env:PATH; Set-Location '$root'; $command"
    Start-Process powershell -ArgumentList "-NoExit", "-Command", $full | Out-Null
    Write-Host "  started $title"
}

$dashboard = "http://127.0.0.1:8000/app.html?mode=live"
$moduleArgs = ""
if ($Aws) {
    $deploymentFile = Join-Path $root "certs\aws-deployment.json"
    if (-not (Test-Path $deploymentFile)) { Write-Host "No AWS deployment: run .\scripts\deploy_aws.ps1 first" -ForegroundColor Red; exit 1 }
    $d = Get-Content $deploymentFile -Raw | ConvertFrom-Json
    $moduleArgs = "--broker $($d.endpoint) --port 8883 --client-id $($d.thing) " +
        "--ca certs\AmazonRootCA1.pem --cert certs\device.pem.crt --key certs\private.pem.key"
    $dashboard = "http://127.0.0.1:8000/app.html?mode=live&api=$([uri]::EscapeDataString($d.events_api))"
}

Write-Host "Starting EdgeVision$(if ($Aws) { ' against AWS' })..." -ForegroundColor Cyan
if (-not $Aws) {
    Start-Component "MQTT broker" "mosquitto -p 1883 -v"
    Start-Sleep 1
}
# The stub always serves the dashboard page; with -Aws the events come from AWS.
Start-Component "cloud stub" "& '$python' -u cloud\local\cloud_stub.py"
Start-Component "Wi-Fi module" "& '$python' -u netmodule\wifi_module.py $moduleArgs"

if ($env:RING_HMAC_KEY) {
    Start-Component "Ring gateway" "& '$python' -u gateway\ring_gateway.py"
} else {
    Write-Host "  Ring gateway skipped: set RING_HMAC_KEY to receive Ring webhooks" -ForegroundColor Yellow
}

Start-Sleep 2
$fwArgs = @()
if ($Clip) { $fwArgs += "'$((Resolve-Path $Clip).Path)'" }
if ($AutoMotion -gt 0) { $fwArgs += "--auto-motion $AutoMotion" }
Start-Component "firmware" "& '$firmware' $($fwArgs -join ' ')"

Start-Sleep 3
Start-Process $dashboard
Write-Host "`nDashboard: $dashboard  (press 'Trigger motion', or m in the firmware window)" -ForegroundColor Green
