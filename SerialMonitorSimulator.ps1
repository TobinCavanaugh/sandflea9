param([switch]$InNewWindow)

# Check if the script is running with the 'InNewWindow' flag.
# If not, relaunch the script in a new process and exit the current one.
if (-not $InNewWindow) {
    Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InNewWindow"
    exit
}

# --- Your Original Script Below ---

# Optional: Set the window title so it's easier to find
$host.UI.RawUI.WindowTitle = "PCI Output Log Monitor"

Copy-Item "pci_serial_output.log" "pci_serial_output_old.log" -Force -ErrorAction SilentlyContinue

Get-Date -Format "yyyy-MM-dd HH:mm:ss" | Set-Content "pci_serial_output.log"

Get-Content "pci_serial_output.log" -Wait