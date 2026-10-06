# Build the firmware, then run every test:
#   - frame-pool unit tests on the RTOS (edgevision_tests.exe)
#   - functional tests that run the firmware through scenarios
#   - Ring gateway unit tests
#
#   .\test.ps1            all tests
#   .\test.ps1 -Quick     skip the slower functional tests

param([switch]$Quick)

# Exit codes are checked explicitly: in Windows PowerShell, "Stop" would turn
# unittest's normal stderr output into a fatal error.
$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
$python = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) { $python = "python" }

& (Join-Path $PSScriptRoot "build.ps1")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$failed = 0

Write-Host "`n== Ring gateway unit tests" -ForegroundColor Cyan
Push-Location (Join-Path $root "gateway")
& $python -m unittest -q test_ring_gateway
if ($LASTEXITCODE -ne 0) { $failed++ }
Pop-Location

Write-Host "`n== Firmware unit + functional tests" -ForegroundColor Cyan
Push-Location $root
if ($Quick) {
    & $python -m unittest discover -s firmware/tests -p "test_*.py" -v -k MemoryAndBoot
} else {
    & $python -m unittest discover -s firmware/tests -p "test_*.py" -v
}
if ($LASTEXITCODE -ne 0) { $failed++ }
Pop-Location

if ($failed -gt 0) { Write-Host "`n$failed test group(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nAll tests passed" -ForegroundColor Green
