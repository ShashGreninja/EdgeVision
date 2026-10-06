# Stop the processes started by run_demo.ps1: the firmware, Mosquitto, and
# only the Python processes running this project's scripts.

$ours = "cloud_stub.py|wifi_module.py|ring_gateway.py"

Get-Process edgevision, mosquitto -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "stopping $($_.ProcessName) ($($_.Id))"
    Stop-Process -Id $_.Id -Force
}

Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" |
    Where-Object { $_.CommandLine -match $ours } |
    ForEach-Object {
        Write-Host "stopping python $($_.ProcessId): $(($_.CommandLine -split ' ')[-1])"
        Stop-Process -Id $_.ProcessId -Force
    }
