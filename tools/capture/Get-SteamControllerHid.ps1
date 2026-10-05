<#
.SYNOPSIS
  Inventories the HID collections Windows created for a connected Steam Controller (2026).

.DESCRIPTION
  Steam opens the wired controller's vendor collection through HIDAPI. Windows splits the
  device's top-level collections into separate HID devices whose compatible IDs carry the
  usage page and usage (HID_DEVICE_UP:FFFF_U:0001 and friends). This script prints, for every
  Valve HID device (VID 28DE), its instance ID, hardware and compatible IDs, status and the
  bound driver, and summarises the usage page/usage per collection. It changes nothing.

  Run while the controller is plugged in over USB (not the puck, not Bluetooth), then save the
  output next to the capture:  .\Get-SteamControllerHid.ps1 | Tee-Object steam-controller-hid.txt

.NOTES
  The HID *report descriptor* itself is not reachable through PnP; capture it with USBPcap at
  plug-in time (see docs/STEAM_CONTROLLER_PROFILE.md, "Continuing on Windows").
#>
[CmdletBinding()]
param(
    [string] $VendorId = '28DE'
)

$ErrorActionPreference = 'Stop'

$devices = @(Get-CimInstance Win32_PnPEntity | Where-Object {
    $_.PNPDeviceID -match "VID_$VendorId" -or $_.HardwareID -match "VID_$VendorId"
})
if ($devices.Count -eq 0) {
    Write-Output "No PnP devices with VID_$VendorId found. Is the controller plugged in over USB?"
    exit 1
}

foreach ($device in $devices | Sort-Object PNPDeviceID) {
    Write-Output ('=' * 78)
    Write-Output "Name        : $($device.Name)"
    Write-Output "Instance ID : $($device.PNPDeviceID)"
    Write-Output "Class       : $($device.PNPClass)  Status: $($device.Status)  Service: $($device.Service)"
    Write-Output "Hardware IDs:"
    foreach ($id in @($device.HardwareID)) { Write-Output "  $id" }
    Write-Output "Compatible IDs:"
    foreach ($id in @($device.CompatibleID)) { Write-Output "  $id" }

    $usage = @($device.HardwareID + $device.CompatibleID) |
        Where-Object { $_ -match 'UP:([0-9A-F]{4})_U:([0-9A-F]{4})' } |
        ForEach-Object { if ($_ -match 'UP:([0-9A-F]{4})_U:([0-9A-F]{4})') { "usage page 0x$($Matches[1]) usage 0x$($Matches[2])" } } |
        Select-Object -Unique
    if ($usage) { Write-Output "Collection  : $($usage -join ', ')" }

    if ($device.PNPDeviceID -match '^USB\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})') {
        Write-Output "USB identity: VID 0x$($Matches[1]) PID 0x$($Matches[2])"
    }
    if ($device.PNPDeviceID -match 'REV_([0-9A-F]{4})') {
        Write-Output "bcdDevice   : 0x$($Matches[1])  (this is sc26_usb::version)"
    }
}

Write-Output ('=' * 78)
Write-Output "Collections Steam can open are the HID children above; the one with a vendor usage page"
Write-Output "(0xFF00..0xFFFF) and a feature report is the controller interface."
