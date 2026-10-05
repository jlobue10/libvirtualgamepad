<#
.SYNOPSIS
  Gathers the Steam Controller (2026) profile evidence from a test machine into one folder:
  the VHF driver state, the HID children Windows sees, Steam's controller logs and gamepad
  bindings, and Vibepollo's log lines about the virtual gamepad. Zips the folder.

.EXAMPLE
  .\Collect-Evidence.ps1                      # -> .\evidence-<machine>-<timestamp>.zip
#>
[CmdletBinding()]
param(
    [string] $OutDir = (Join-Path $PSScriptRoot "evidence-$env:COMPUTERNAME-$(Get-Date -Format yyyyMMdd-HHmmss)"),
    [string] $SteamDir = 'C:\Program Files (x86)\Steam',
    [string] $VibepolloLog = "$env:ProgramFiles\Vibepollo\config\sunshine.log"
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$sys = Join-Path $OutDir 'system.txt'
"machine: $env:COMPUTERNAME  $((Get-CimInstance Win32_OperatingSystem).Caption) build $([Environment]::OSVersion.Version.Build)" | Set-Content $sys
"Secure Boot: $(try { Confirm-SecureBootUEFI } catch { 'unknown' })" | Add-Content $sys
"test signing: $((bcdedit /enum '{current}' 2>&1 | Select-String testsigning) -join '')" | Add-Content $sys
"driver: " + ((Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -like 'ROOT\VIBESHINE*' } | ForEach-Object { "$($_.InfName) $($_.DriverVersion) $($_.DriverDate)" }) -join '; ') | Add-Content $sys
Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_28DE|VIBESHINE|VHF' } |
    Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String -Width 220 |
    Set-Content (Join-Path $OutDir 'hid-children.txt')
foreach ($name in 'controller.txt', 'controller_ui.txt', 'console_log.txt', 'bootstrap_log.txt') {
    $f = Join-Path $SteamDir "logs\$name"
    if (Test-Path $f) { Copy-Item $f (Join-Path $OutDir "steam-$name") }
}
$vdf = Join-Path $SteamDir 'config\config.vdf'
if (Test-Path $vdf) {
    Get-Content $vdf | Select-String 'SDL_GamepadBind|28de|1302' | ForEach-Object { $_.Line.Trim() } | Set-Content (Join-Path $OutDir 'steam-config-gamepad-lines.txt')
}
if (Test-Path $VibepolloLog) {
    Get-Content $VibepolloLog | Select-String -Pattern 'gamepad|vhf|Steam Controller|virtual' | ForEach-Object { $_.Line } |
        Select-Object -Last 400 | Set-Content (Join-Path $OutDir 'vibepollo-gamepad-lines.txt')
    Copy-Item $VibepolloLog (Join-Path $OutDir 'sunshine.log')
}
$zip = "$OutDir.zip"
Compress-Archive -Path "$OutDir\*" -DestinationPath $zip -Force
"evidence: $zip"
Get-ChildItem $OutDir | Select-Object Name, Length | Format-Table -AutoSize
