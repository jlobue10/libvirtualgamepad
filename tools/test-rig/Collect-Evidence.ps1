<#
.SYNOPSIS
  Gathers the Steam Controller (2026) profile evidence from a test machine into one folder:
  the VHF driver state, the HID children Windows sees, Steam's controller logs and gamepad
  bindings, and Vibepollo's log lines about the virtual gamepad. Zips the folder.

  The zip is meant to be attached to issues and PRs, so it carries only what the gamepad
  investigation needs: from Steam's config.vdf only the gamepad binding keys (never the
  login-token hex), and from Vibepollo's log only the lines about the virtual gamepad
  (the full sunshine.log names clients, their addresses and pairing activity, and is not
  copied). Widen the line filter below if an investigation needs more of the host log.

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
    # Only the gamepad binding keys: an unanchored vendor/product id match also caught
    # encrypted login-token hex lines, which the evidence zip must never carry.
    Get-Content $vdf | Select-String -CaseSensitive -Pattern '^\s*"SDL_GamepadBind' | ForEach-Object { $_.Line.Trim() } | Set-Content (Join-Path $OutDir 'steam-config-gamepad-lines.txt')
}
if (Test-Path $VibepolloLog) {
    # Only the virtual-gamepad lines; the full host log is deliberately not copied (see the header).
    Get-Content $VibepolloLog | Select-String -Pattern 'gamepad|vhf|Steam Controller|virtual' | ForEach-Object { $_.Line } |
        Select-Object -Last 400 | Set-Content (Join-Path $OutDir 'vibepollo-gamepad-lines.txt')
}
$zip = "$OutDir.zip"
# Windows ships bsdtar; Compress-Archive's module does not load on every machine.
& (Join-Path $env:windir 'System32\tar.exe') -a -c -f $zip -C $OutDir .
"evidence: $zip"
Get-ChildItem $OutDir | Select-Object Name, Length | Format-Table -AutoSize
